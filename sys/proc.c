/*
 * This file has been modified as part of the FreeMiNT project. See
 * the file Changes.MH for details and dates.
 *
 *
 * Copyright 1990,1991,1992 Eric R. Smith.
 * Copyright 1992,1993,1994 Atari Corporation.
 * All rights reserved.
 *
 *
 * routines for handling processes
 *
 */

# include "proc.h"
# include "global.h"

# include "libkern/libkern.h"

# include "mint/asm.h"
# include "mint/credentials.h"
# include "mint/filedesc.h"
# include "mint/basepage.h"
# include "mint/resource.h"
# include "mint/signal.h"

# include "arch/context.h"	/* save_context, change_context */
# include "arch/kernel.h"
# include "arch/mprot.h"
# include "arch/tosbind.h"
# include "arch/user_things.h"	/* trampoline */

# include "bios.h"
# include "cookie.h"
# include "dosfile.h"
# include "filesys.h"
# include "k_exit.h"
# include "kmemory.h"
# include "memory.h"
# include "proc_help.h"
# include "proc_wakeup.h"
# include "random.h"
# include "signal.h"
# include "time.h"
# include "timeout.h"
# include "random.h"
# include "xbios.h"

#define VBL 28
// #define STACK_MAGIC        0xDEADBEEF    // Sentinel value for overflow detection
/* Stack boundaries and configuration */
#define STACK_SIZE       (16 * 1024)     // 16KB default stack size
#define STACK_MIN_SIZE   (4 * 1024)      // 4KB minimum stack size
#define STACK_ALIGN      16              // Stack alignment requirement

/* Stack boundary definitions */
#define STACK_MIN_ADDR   0x00001000      // Lowest valid stack address
#define STACK_MAX_ADDR   0x7FFFFFFF      // Highest valid stack address

/* Stack guard page size */
#define GUARD_PAGE_SIZE  4096            // 4KB guard page
#ifdef __mcoldfire__
#define SR_IPL_MASK    0x2700
#define SR_SUPER_MASK  0x2000
#define SET_IPL(sr)    "move.w #0x2700,%%sr"
#else
#define SR_IPL_MASK    0x0700
#define SR_SUPER_MASK  0x2000
#define SET_IPL(sr)    "ori.w #0x0700,%%sr"
#endif

/* Process iteration macro */
#define for_each_proc(p) for (p = proclist; p != NULL; p = p->p_next)

/*
 * We initialize proc_clock to a very large value so that we don't have
 * to worry about unexpected process switches while starting up
 */
unsigned short proc_clock = 0x7fff;

struct proc_queue sysq[NUM_QUEUES] = { { NULL } };

struct thread *ready_queue = NULL;

static long tls_next_key = 0;

/* global process variables */
struct proc *proclist = NULL;		/* list of all active processes */
struct proc *curproc  = NULL;	/* current process		*/
struct proc *rootproc = NULL;		/* pid 0 -- MiNT itself		*/

/* Timer handling */
static void (*old_timer)(void);          /* Original timer handler */
volatile unsigned long timer_ticks = 0;  // Global timer

/* default; actual value comes from mint.cnf */
short time_slice = 2;

struct proc *_cdecl get_curproc(void) { return curproc; }

static int timer_initialized = 0;

void exit_thread(void);
void debug_ready_queue(void);
void verify_ready_queue(void);
bool verify_user_access(void *addr, size_t size);

// void thread_safe_switch(struct proc *from, struct proc *to);
void thread_safe_switch(struct thread *from, struct thread *to);
bool verify_context(struct context *ctx, void *stack_base, void *stack_top);
bool verify_context_switch(struct thread *from, struct thread *to);

struct thread *get_thread_from_stack_pointer(void *stack_ptr);

// Define debug_to_file function
void debug_to_file(const char *filename, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buffer[1024];
    kvsprintf(buffer, sizeof(buffer), fmt, args);

    int fd = Fopen(filename, O_RDWR | O_CREAT | O_APPEND);
    if (fd < 0) {
        return; // Handle error if needed
    }

    // Write the formatted message to the file
    Fwrite(fd, strlen(buffer), buffer);

    // Optionally write a newline
    Fwrite(fd, 1, "\n");

    // Close the file
    Fclose(fd);

    va_end(args);
}
int tas(volatile long *lock) {
    int old_value;
    asm volatile (
        "tas.b %1\n\t"
        "sne %0"
        : "=d" (old_value), "+m" (*lock)
        : 
        : "cc"
    );
    return old_value;
}

// Add exit_thread function to handle thread termination
void exit_thread(void) {
    sys_p_exit();
}

void add_to_ready_queue(struct thread *t) 
{
    if (!t) return;

    DEBUG_TO_FILE("ADD_TO_READY_Q: Thread %d (pri %d, state %d)", 
                 t->tid, t->priority, t->state);
    DEBUG_TO_FILE("Current ready_queue ptr: %p", ready_queue);

    // Check if already in queue
    struct thread *iter = ready_queue;
    while (iter) {
        if (iter == t) {
            DEBUG_TO_FILE("Thread %d already in queue", t->tid);
            return;
        }
        iter = iter->next_ready;
    }

    // Add to end of same priority group
    struct thread **pp = &ready_queue;
    while (*pp && (*pp)->priority >= t->priority) {
        DEBUG_TO_FILE("Checking thread %d (pri %d)", 
                     (*pp)->tid, (*pp)->priority);
        pp = &(*pp)->next_ready;
    }
    
    t->next_ready = *pp;
    *pp = t;

    DEBUG_TO_FILE("Added thread %d to ready queue (pri %d)", t->tid, t->priority);
    DEBUG_TO_FILE("New ready_queue ptr: %p", ready_queue);
    debug_ready_queue();
}

void schedule(void)
{
    DEBUG_TO_FILE("----- SCHEDULE START -----");
    debug_ready_queue();

    if (!curproc || !curproc->current_thread) {
        DEBUG_TO_FILE("No current process/thread!");
        return;
    }

    struct thread *current = curproc->current_thread;
    
    // Only switch if we have threads in ready queue
    if (!ready_queue) {
        DEBUG_TO_FILE("No threads in ready queue");
        return;
    }

    // Get next thread to run - make sure it's actually READY
    struct thread *selected = ready_queue;
    while (selected && (selected->state != STATE_READY || selected == current)) {
        selected = selected->next_ready;
    }

    if (!selected) {
        DEBUG_TO_FILE("----- SCHEDULE END (no eligible thread) -----");
        return;
    }

    DEBUG_TO_FILE("[SCHED] Switching from thread %d to thread %d", 
                 current->tid, selected->tid);

    // Remove selected thread from ready queue
    remove_from_ready_queue(selected);
    
    // Do the switch
    thread_safe_switch(current, selected);

    DEBUG_TO_FILE("----- SCHEDULE END -----");
}

void thread_safe_switch(struct thread *from, struct thread *to)
{
    if (!to || from == to) return;

    DEBUG_TO_FILE("Thread switch: %d -> %d", from->tid, to->tid);
    
    // Save current context
    if (from) {
        save_context(&from->ctxt);
        from->state = STATE_READY;
    }

    // Prepare new thread
    to->state = STATE_RUNNING;
    curproc->current_thread = to;

    // Set up proper kernel exit frame if going to user mode
    if (!(to->ctxt.sr & SR_SUPER_MASK)) {
        // Ensure even alignment and space for frame
        unsigned short *sp = (unsigned short *)((to->ctxt.ssp - 6) & ~1L);
        
        // Set up frame in CORRECT ORDER for 68000 RTE instruction:
        *(sp + 0) = 0x0000;                              // SR first
        *(sp + 1) = (unsigned short)(to->ctxt.pc >> 16);  // PC high
        *(sp + 2) = (unsigned short)(to->ctxt.pc);        // PC low
        
        to->ctxt.ssp = (long)sp;  // Point to start of frame
        
        DEBUG_TO_FILE("Stack frame at 0x%lx:", (long)sp);
        DEBUG_TO_FILE("  SR=0x0000");
        DEBUG_TO_FILE("  PC=0x%lx", to->ctxt.pc);
        DEBUG_TO_FILE("  Frame size=6 bytes");
        DEBUG_TO_FILE("  SSP=0x%lx", to->ctxt.ssp);
        
        // Ensure proper domain
        curproc->domain = DOM_TOS;
        
        leave_kernel();
    }

    change_context(&to->ctxt);
}


// void thread_safe_switch(struct thread *from, struct thread *to)
// {
//     if (!to || from == to) return;

//     DEBUG_TO_FILE("Thread switch: %d -> %d", from->tid, to->tid);
//     DEBUG_TO_FILE("FROM thread state: PC=0x%lx USP=0x%lx SR=0x%04x SSP=0x%lx", 
//                  from->ctxt.pc, from->ctxt.usp, from->ctxt.sr, from->ctxt.ssp);
//     DEBUG_TO_FILE("TO thread state: PC=0x%lx USP=0x%lx SR=0x%04x SSP=0x%lx", 
//                  to->ctxt.pc, to->ctxt.usp, to->ctxt.sr, to->ctxt.ssp);
    
//     unsigned short sr = splhigh();

//     // Save current context
//     if (from) {
//         DEBUG_TO_FILE("Saving context for thread %d", from->tid);
//         save_context(&from->ctxt);
//         from->state = STATE_READY;
//         DEBUG_TO_FILE("After save: PC=0x%lx USP=0x%lx SR=0x%04x SSP=0x%lx", 
//                      from->ctxt.pc, from->ctxt.usp, from->ctxt.sr, from->ctxt.ssp);
//     }

//     // Prepare new thread
//     to->state = STATE_RUNNING;
//     curproc->current_thread = to;

//     // Set up proper kernel exit frame if going to user mode
//     if (!(to->ctxt.sr & SR_SUPER_MASK)) {
//         DEBUG_TO_FILE("Setting up user mode transition for thread %d", to->tid);
        
//         // Ensure stack alignment
//         to->ctxt.ssp = (to->ctxt.ssp - 4) & ~3L;
//         long *sp = (long *)to->ctxt.ssp;
        
//         // Verify stack space
//         if ((char *)sp - (char *)to->stack < 256) {
//             DEBUG_TO_FILE("Stack overflow risk detected!");
//             return;
//         }

//         // Set up minimal frame
//         *(--sp) = to->ctxt.pc;    // Return PC
//         *(--sp) = 0x0000;         // Initial SR (user mode)
        
//         // Save new stack pointer
//         to->ctxt.ssp = (long)sp;
        
//         DEBUG_TO_FILE("Stack frame setup:");
//         DEBUG_TO_FILE("  PC=0x%lx", to->ctxt.pc);
//         DEBUG_TO_FILE("  SR=0x%04x", to->ctxt.sr);
//         DEBUG_TO_FILE("  SSP=0x%lx", to->ctxt.ssp);
//         DEBUG_TO_FILE("  USP=0x%lx", to->ctxt.usp);
        
//         // Ensure proper domain
//         curproc->domain = DOM_TOS;
        
//         DEBUG_TO_FILE("Before leave_kernel");
//         leave_kernel();
//         DEBUG_TO_FILE("After leave_kernel");  // Should not see this
//     }

//     DEBUG_TO_FILE("Before change_context");
//     // Verify context before switch
//     if (!verify_context(&to->ctxt, to->stack_base, to->stack_top)) {
//         DEBUG_TO_FILE("Invalid context detected!");
//         return;
//     }
    
//     change_context(&to->ctxt);
//     DEBUG_TO_FILE("After change_context");  // Should not see this

//     spl(sr);
// }


bool verify_context(struct context *ctx, void *stack_base, void *stack_top) {
    if (!ctx) {
        DEBUG_TO_FILE("NULL context pointer");
        return false;
    }

    // Basic register validation
    if (ctx->pc < 0x1000) {
        DEBUG_TO_FILE("Invalid PC: 0x%08lx", ctx->pc);
        return false;
    }

    // On 68000: stack grows DOWN
    // stack_base is LOW address
    // stack_top is HIGH address
    // USP should be between them
    unsigned long base = (unsigned long)stack_base;  // Low address
    unsigned long top = (unsigned long)stack_top;    // High address
    unsigned long usp = ctx->usp;

    if (usp < base || usp > top) {
        DEBUG_TO_FILE("Stack pointer out of bounds:");
        DEBUG_TO_FILE("  Stack base (low): 0x%08lx", base);
        DEBUG_TO_FILE("  Stack top (high): 0x%08lx", top);
        DEBUG_TO_FILE("  USP: 0x%08lx", usp);
        return false;
    }

    return true;
}


bool verify_context_switch(struct thread *from, struct thread *to)
{
    DEBUG_TO_FILE("Verifying context switch:");
    
    if (from) {
        DEBUG_TO_FILE("  From thread %d:", from->tid);
        if (!verify_context(&from->ctxt, from->stack_base, from->stack_top)) {
            DEBUG_TO_FILE("  Source context invalid");
            return false;
        }
    }
    
    DEBUG_TO_FILE("  To thread %d:", to->tid);
    if (!verify_context(&to->ctxt, to->stack_base, to->stack_top)) {
        DEBUG_TO_FILE("  Target context invalid");
        return false;
    }
    
    return true;
}

void init_thread_stack(struct thread *t, void (*func)(void*), void *arg)
{
    DEBUG_TO_FILE("Init thread %d stack", t->tid);
    
    // Clear context
    memset(&t->ctxt, 0, sizeof(struct context));

    // Ensure even alignment for 68000
    unsigned long stack_top = ((unsigned long)t->stack + STACK_SIZE) & ~1L;
    
    // Set up user stack - ensure even alignment and leave space
    unsigned short *usp = (unsigned short *)(stack_top - 64);
    
    // Push initial values in correct order for 68000
    *(--usp) = (unsigned short)((unsigned long)arg);        // Argument low
    *(--usp) = (unsigned short)((unsigned long)arg >> 16);  // Argument high
    *(--usp) = (unsigned short)((unsigned long)exit_thread);        // Return low
    *(--usp) = (unsigned short)((unsigned long)exit_thread >> 16);  // Return high
    
    // Set up supervisor stack with space for frame
    unsigned short *ssp = (unsigned short *)(stack_top - 256);
    
    // Set up context
    t->ctxt.pc = (long)func;
    t->ctxt.usp = (long)usp;
    t->ctxt.sr = 0x0000;        // User mode
    t->ctxt.ssp = (long)ssp;
    
    DEBUG_TO_FILE("Thread %d context:", t->tid);
    DEBUG_TO_FILE("  PC=0x%lx", t->ctxt.pc);
    DEBUG_TO_FILE("  USP=0x%lx", t->ctxt.usp);
    DEBUG_TO_FILE("  SR=0x%04x", t->ctxt.sr);
    DEBUG_TO_FILE("  SSP=0x%lx", t->ctxt.ssp);
    DEBUG_TO_FILE("  Stack check:");
    DEBUG_TO_FILE("    Stack base: 0x%lx", (unsigned long)t->stack);
    DEBUG_TO_FILE("    Stack top: 0x%lx", stack_top);
    DEBUG_TO_FILE("    USP aligned: %s", (t->ctxt.usp & 1) ? "no" : "yes");
    DEBUG_TO_FILE("    SSP aligned: %s", (t->ctxt.ssp & 1) ? "no" : "yes");
    
    t->state = STATE_READY;
}


// void init_thread_stack(struct thread *t, void (*func)(void*), void *arg)
// {
//     DEBUG_TO_FILE("Init thread %d stack", t->tid);
    
//     // Clear context
//     memset(&t->ctxt, 0, sizeof(struct context));

//     char *stack_base = (char *)t->stack;
    
//     // Calculate stack positions - ensure proper alignment
//     unsigned long ssp = (unsigned long)(stack_base + STACK_SIZE - 16) & ~3UL;
//     unsigned long usp = (unsigned long)(t->stack_top) & ~3UL;

//     // Set up initial register state
//     t->ctxt.regs[15] = (long)arg;        // D7 = argument
//     t->ctxt.regs[14] = 0;                // D6
//     t->ctxt.regs[13] = 0;                // D5
//     t->ctxt.regs[8] = (long)exit_thread; // A0 = return address

//     // Set up supervisor stack frame
//     long *ssp_ptr = (long *)ssp;
//     *(--ssp_ptr) = (long)func;           // Initial PC
//     *(--ssp_ptr) = 0x0000;              // Initial SR (user mode)
//     *(--ssp_ptr) = usp;                 // Initial USP

//     // Set up user stack frame
//     long *usp_ptr = (long *)usp;
//     *(--usp_ptr) = (long)arg;           // Push argument
//     *(--usp_ptr) = (long)exit_thread;   // Return address
//     *(--usp_ptr) = 0;                   // Frame pointer

//     // Initialize context structure
//     t->ctxt.pc = (long)func;
//     t->ctxt.usp = (long)usp_ptr;
//     t->ctxt.sr = 0x0000;               // User mode
//     t->ctxt.ssp = (long)ssp_ptr;
//     t->ctxt.term_vec = 0x102;          // GEMDOS terminate vector
//     t->ctxt.sfmt = 0x0000;             // Normal frame format

//     t->state = STATE_READY;

//     DEBUG_TO_FILE("Thread %d context initialized:", t->tid);
//     DEBUG_TO_FILE("  PC=0x%lx", t->ctxt.pc);
//     DEBUG_TO_FILE("  USP=0x%lx", t->ctxt.usp);
//     DEBUG_TO_FILE("  SR=0x%04x", t->ctxt.sr);
//     DEBUG_TO_FILE("  SSP=0x%lx", t->ctxt.ssp);
// }

bool verify_user_access(void *addr, size_t size) {
    unsigned long start = (unsigned long)addr;
    unsigned long end = start + size;
    
    // Check alignment
    if (start & 1) return false;
    
    // Check address range
    if (start < 0x1000 || end > 0x1000000) return false;
    
    return true;
}

void switch_to_thread(struct proc *from, struct proc *to) {
    // Validate input parameters
    if (!to) {
        DEBUG_TO_FILE("Invalid switch_to_thread: to proc is NULL");
        return;
    }
    if (!to->current_thread) {
        DEBUG_TO_FILE("Invalid switch_to_thread: to proc %p has no current thread", to);
        return;
    }

    if (from == to) {
        DEBUG_TO_FILE("Not switching to same thread");
        return;
    }

    // Validate thread states
    struct thread *from_thread = from ? from->current_thread : NULL;
    struct thread *to_thread = to->current_thread;
    
    if (!to_thread) {
        DEBUG_TO_FILE("Invalid switch_to_thread: to_thread is NULL");
        return;
    }

    // Log the switch attempt
    DEBUG_TO_FILE("Switch request:");
    if (from_thread) {
        DEBUG_TO_FILE("  From: Thread %d (PC=0x%08lx, USP=0x%08lx)", 
                     from_thread->tid,
                     from_thread->ctxt.pc,
                     from_thread->ctxt.usp);
    } else {
        DEBUG_TO_FILE("  From: NULL");
    }
    
    DEBUG_TO_FILE("  To: Thread %d (PC=0x%08lx, USP=0x%08lx)",
                 to_thread->tid,
                 to_thread->ctxt.pc,
                 to_thread->ctxt.usp);

    if (from_thread == to_thread) {
        DEBUG_TO_FILE("Skipping switch to same thread");
        return;
    }

    // Verify thread contexts
    if (!verify_context_switch(from_thread, to_thread)) {
        DEBUG_TO_FILE("Context switch verification failed");
        return;
    }

    // Save current context if there is one
    if (from_thread) {
        unsigned short old_sr;
        asm volatile ("move.w %%sr,%0" : "=d" (old_sr));
        #ifdef __mcoldfire__
        asm volatile ("move.w #0x2700,%%sr" : : : "cc");
        #else
        asm volatile ("ori.w #0x0700,%%sr" : : : "cc");
        #endif
        
        if (save_context(&from_thread->ctxt)) {
            DEBUG_TO_FILE("Returned from context save in thread %d", from_thread->tid);
            asm volatile ("move.w %0,%%sr" : : "d" (old_sr));
            return;
        }
        
        from_thread->state = STATE_READY;
    }

    // Switch to new thread
    to_thread->state = STATE_RUNNING;
    curproc = to;

    // Verify context before switch
    verify_context(&to_thread->ctxt, to_thread->stack_base, to_thread->stack_top);
    
    change_context(&to_thread->ctxt);
}

void debug_ready_queue(void) {
    DEBUG_TO_FILE("Ready queue contents:");
    if (!ready_queue) {
        DEBUG_TO_FILE("  <empty>");
        return;
    }

    struct thread *t = ready_queue;
    while (t) {
        DEBUG_TO_FILE("  Thread %d (pri=%d, state=%d)", 
                     t->tid, t->priority, t->state);
        t = t->next_ready;  // Use next_ready
    }
}

void verify_ready_queue(void) {
    DEBUG_TO_FILE("Verifying ready queue integrity:");
    
    if (!ready_queue) {
        DEBUG_TO_FILE("  Queue is empty");
        return;
    }

    struct thread *curr = ready_queue;
    int count = 0;
    int last_priority = 31;
    struct thread *last = NULL;
    
    while (curr && count < 32) {
        // Check state
        if (curr->state != STATE_READY) {
            DEBUG_TO_FILE("  ERROR: Thread %d has invalid state %d",
                         curr->tid, curr->state);
        }
        
        // Check priority ordering
        if (curr->priority > last_priority) {
            DEBUG_TO_FILE("  ERROR: Priority inversion: thread %d (pri=%d) > %d",
                         curr->tid, curr->priority, last_priority);
        }
        
        // Check for loops
        if (curr->next_ready == curr) {
            DEBUG_TO_FILE("  ERROR: Self-loop detected at thread %d",
                         curr->tid);
            break;
        }
        
        last_priority = curr->priority;
        last = curr;
        curr = curr->next_ready;
        count++;
    }
    
    if (count >= 32) {
        DEBUG_TO_FILE("  ERROR: Possible circular reference detected");
    }
    
    // Verify last node
    if (last && last->next_ready != NULL) {
        DEBUG_TO_FILE("  ERROR: Last node has non-NULL next pointer");
    }
}

void timer_interrupt_handler(void)
{
    // Check if interrupts are disabled
    unsigned short sr;
    asm volatile ("move.w %%sr,%0" : "=d" (sr));
    if (sr & 0x0700) {  // Check interrupt mask
        return;
    }

    if (!curproc || !curproc->current_thread) return;
    
    struct thread *current = curproc->current_thread;
    
    DEBUG_TO_FILE("[TIMER] Tick for thread %d (Q=%d)", 
                 current->tid, current->time_quantum);

    if (current->time_quantum > 0) {
        current->time_quantum--;
        if (current->time_quantum <= 0) {
            DEBUG_TO_FILE("Thread %d quantum expired", current->tid);
            
            sr = splhigh();
            
            // Force a schedule when quantum expires
            current->state = STATE_READY;
            add_to_ready_queue(current);
            current->time_quantum = time_slice; // Reset quantum
            
            schedule();
            
            spl(sr);
        }
    }
    timer_ticks++;
}

void add_to_wait_queue(struct thread **queue, struct thread *t)
{
	struct thread **curr;
	for (curr = queue; *curr; curr = &(*curr)->next) {
		if (t->priority > (*curr)->priority) break;
	}
	t->next = *curr;
	*curr = t;
}

void remove_from_wait_queue(struct thread **queue, struct thread *t) {
    struct thread **curr;
    for (curr = queue; *curr != NULL; curr = &(*curr)->next) { // Use 'next'
        if (*curr == t) {
            *curr = t->next;
            break;
        }
    }
}

void remove_from_ready_queue(struct thread *t) {
    if (!t) return;

    DEBUG_TO_FILE("REMOVE_FROM_READY_Q: Thread %d", t->tid);
    debug_ready_queue();

    struct thread **pp = &ready_queue;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next_ready;  // Remove from list
            t->next_ready = NULL; // Clear next pointer
            DEBUG_TO_FILE("Removed thread %d from ready queue", t->tid);
            debug_ready_queue();
            return;
        }
        pp = &(*pp)->next_ready;
    }
}

struct thread* remove_highest_priority(struct thread **queue)
{
	struct thread *highest = *queue;
	if (highest) 
		*queue = highest->next;
	return highest;
}

void th_sleep(void)
{
	curproc->p_state = STATE_BLOCKED;
	curproc->p_block_time = timer_ticks;
	// Pass the current thread instead of the process
	if (curproc->current_thread)
		remove_from_ready_queue(curproc->current_thread);
	schedule();
}

void wakeup(struct proc *p) {
    if (p && p->current_thread) {
        remove_from_wait_queue(&p->current_thread->wait_queue, p->current_thread);
        add_to_ready_queue(p->current_thread);
    }
}

/* Mutex functions */

long _cdecl sys_p_mutex_init(void) {
    struct kernel_mutex *m = kmalloc(sizeof(struct kernel_mutex));
    if (!m) return -ENOMEM;

    m->locked = 0;
    m->owner = NULL;
    m->wait_queue = NULL;

    // Return the kernel address as a handle (cast to long)
    return (long)m;
}

long _cdecl sys_p_mutex_lock(long mutex_ptr)
{
    struct kernel_mutex *m = (struct kernel_mutex *)mutex_ptr;
    struct thread *current = curproc->current_thread;
    
    DEBUG_TO_FILE("Thread %d attempting to lock mutex %p", 
                 current->tid, m);

    while (tas(&m->locked)) {
        DEBUG_TO_FILE("Thread %d blocking on mutex %p (owner=%d)", 
                     current->tid, m,
                     m->owner ? m->owner->tid : -1);
        
        current->state = STATE_BLOCKED;
        add_to_wait_queue(&m->wait_queue, current);
        schedule();
    }

    m->owner = current;
    DEBUG_TO_FILE("Mutex %p acquired by thread %d", m, current->tid);
    return 0;
}

long _cdecl sys_p_mutex_unlock(long mutex_ptr)
{
    struct kernel_mutex *m = (struct kernel_mutex *)mutex_ptr;
    struct thread *current = curproc->current_thread;

    DEBUG_TO_FILE("mutex_unlock: thread %d releasing %p", 
                 current->tid, m);

    if (m->owner != current) {
        DEBUG_TO_FILE("mutex_unlock: thread %d doesn't own mutex!", current->tid);
        return -EPERM;
    }

    unsigned short sr = splhigh();
    m->locked = 0;
    m->owner = NULL;

    // Wake highest priority waiter
    struct thread *waiter = remove_highest_priority(&m->wait_queue);
    if (waiter) {
        DEBUG_TO_FILE("mutex_unlock: waking thread %d", waiter->tid);
        waiter->state = STATE_READY;
        add_to_ready_queue(waiter);
        
        if (waiter->priority > current->priority) {
            DEBUG_TO_FILE("mutex_unlock: priority boost needed");
            schedule();
        }
    }

    spl(sr);
    return 0;
}


/* Thread stack management */
void* allocate_thread_stack(void) {
    DEBUG_TO_FILE("allocate_thread_stack occurred\n");
    void *base = kmalloc(STACK_SIZE + GUARD_PAGE_SIZE);
    if (!base) return NULL;

    // Set stack_base to start of allocated block
    void *usable_stack = (char*)base + GUARD_PAGE_SIZE;

    // Set sentinel at the end of the usable stack
    unsigned long *stack_top = (unsigned long *)((char*)usable_stack + STACK_SIZE - 4);
    *stack_top = STACK_MAGIC; 

    return usable_stack; // This becomes thread->stack
}

struct thread *get_thread_from_stack_pointer(void *stack_ptr) {
    if (stack_ptr == NULL) {
        // Handle error: invalid stack pointer
        return NULL;
    }

    // Get current stack pointer if none provided
    if (stack_ptr == NULL) {
        asm volatile("move.l %%a7,%0" : "=r" (stack_ptr));
    }

    // Search through all threads to find which stack range contains this pointer
    struct proc *p = curproc;
    if (!p) return NULL;

    struct thread *t = p->threads;
    while (t) {
        // Check if stack_ptr is within this thread's stack bounds
        // Remember stack grows DOWN, so stack_top < stack_ptr < stack_base
        if (stack_ptr >= t->stack_top && stack_ptr <= t->stack_base) {
            return t;
        }
        t = t->next;
    }

    return NULL;
}

void free_thread_stack(void *stack)
{
    if (stack) {
        DEBUG_TO_FILE("free_thread_stack occurred\n");
        struct thread *t = get_thread_from_stack_pointer(stack);
        if (t->stack_base) {
            kfree(t->stack_base); // Free the entire block (base includes guard)
        }
    }
}

void semaphore_init(struct semaphore *s, int count) {
    s->count = (long)count;
    s->wait_queue = NULL;
}

void semaphore_wait(struct semaphore *s) {
    while (tas(&s->count) <= 0) {
        add_to_wait_queue(&s->wait_queue, curproc->current_thread);
        th_sleep();
    }
    s->count--;
}

void semaphore_post(struct semaphore *s)
{
    struct thread *current = curproc->current_thread;
    
    DEBUG_TO_FILE("sem_post: thread %d incrementing sem %p", 
                 current->tid, s);

    unsigned short sr = splhigh();
    s->count++;
    
    struct thread *waiter = remove_highest_priority(&s->wait_queue);
    if (waiter) {
        DEBUG_TO_FILE("sem_post: waking thread %d", waiter->tid);
        waiter->state = STATE_READY;
        add_to_ready_queue(waiter);
        
        if (waiter->priority > current->priority) {
            DEBUG_TO_FILE("sem_post: priority inversion prevented");
            schedule();
        }
    }
    
    spl(sr);
}

long _cdecl sys_p_tlscreate(void) {
    if (tls_next_key >= THREAD_TLS_KEYS) return -ENOMEM;
    return tls_next_key++;
}

long _cdecl sys_p_tlsset(long key, void *value) {
    if (key < 0 || key >= THREAD_TLS_KEYS) return -EINVAL;
    curproc->threads->tls[key] = value;
    return 0;
}

long _cdecl sys_p_tlsget(long key) {
    if (key < 0 || key >= THREAD_TLS_KEYS) return -EINVAL;
    return (long)curproc->threads->tls[key];
}

/* Thread creation with detailed logging */
long create_new_thread(struct proc *p, const struct thread_params *params) {
    DEBUG_TO_FILE("Creating thread in process %p (has %d threads)", 
                 p, p->num_threads);

    // Initialize thread system if first thread
    if (!p->threads) {
        DEBUG_TO_FILE("Initializing main thread for process %p", p);
        if (!timer_initialized) {
            Jdisint(VBL);
            old_timer = (void *)Setexc(VBL, (long)timer_interrupt_handler);
            Jenabint(VBL);
            timer_initialized = 1;
            DEBUG_TO_FILE("Timer subsystem initialized");
        }
        
        p->threads = kmalloc(sizeof(struct thread));
        p->threads->tid = 0;
        p->threads->stack = p->stack;
        p->num_threads = 1;
        p->threads->state = STATE_READY; // Set state
        p->threads->proc = p;
        p->threads->priority = p->p_priority;
        
		// Set the current thread to the main thread
		p->current_thread = p->threads;

        DEBUG_TO_FILE("Main thread created with TID 0");
        add_to_ready_queue(p->threads); // <-- Add main thread to ready queue
    }
    
    struct thread *t = kmalloc(sizeof(struct thread));
    if (!t) {
        DEBUG_TO_FILE("Failed to allocate thread struct!");
        return -ENOMEM;
    }

    // Assign stack (use allocated stack if provided)
    t->stack = params->stack ? params->stack : allocate_thread_stack();
    if (!t->stack) {
        kfree(t);
        return -ENOMEM;
    }
    t->tid = p->num_threads++;
    t->proc = p;
    t->priority = p->p_priority;  // Inherit parent's priority
    DEBUG_TO_FILE("Thread %d created with priority %d (parent proc %d)", 
                 t->tid, t->priority, p->pid);
                 t->time_quantum = time_slice + (t->priority / 4); // Set based on priority
                 DEBUG_TO_FILE("New thread %d created with quantum %d", t->tid, t->time_quantum);                 
	t->state = STATE_READY; 
    t->next = p->threads;

    p->threads = t;

    DEBUG_TO_FILE("New thread %d created. Stack: %p, Entry: %p", 
                 t->tid, t->stack, params->func);
    
    init_thread_stack(t, params->func, params->arg);
    
    DEBUG_TO_FILE("Initialized stack for thread %d. SP: %p", 
                 t->tid, t->ctxt.usp);
    
    add_to_ready_queue(t);
    return t->tid;
}

/* Syscalls */
long sys_p_createthread(void (*func)(void*), void *arg, void *stack)
{
    if (!timer_initialized) {
        DEBUG_TO_FILE("Initializing timer subsystem");
        Jdisint(VBL);
        old_timer = (void *)Setexc(VBL, (long)timer_interrupt_handler);
        Jenabint(VBL);
        timer_initialized = 1;
    }

    DEBUG_TO_FILE("sys_p_createthread: func=%p arg=%p stack=%p", func, arg, stack);
    
    if (!func) return -EINVAL;

    struct proc *p = curproc;
    if (!p) return -EINVAL;

    struct thread *t = kmalloc(sizeof(struct thread));
    if (!t) return -ENOMEM;

    // Basic initialization
    memset(t, 0, sizeof(struct thread));
    t->tid = p->num_threads++;
    t->proc = p;
    t->priority = p->current_thread->priority;
    t->time_quantum = time_slice;
    t->state = STATE_INIT;
    
    // Initialize queue links
    t->next_ready = NULL;  // For ready queue
    t->next = p->threads;  // Link into process thread list
    p->threads = t;        // Add to front of process thread list

    // Stack setup
    if (!stack) {
        t->stack = allocate_thread_stack();
        if (!t->stack) {
            kfree(t);
            return -ENOMEM;
        }
        t->stack_base = t->stack;
        t->stack_top = (char*)t->stack + STACK_SIZE;
    } else {
        t->stack = stack;
        t->stack_base = stack;
        t->stack_top = (char*)stack + STACK_SIZE;
    }

    // Initialize thread context and stack
    init_thread_stack(t, func, arg);

    // Add to ready queue
    add_to_ready_queue(t);

    return t->tid;
}

long _cdecl sys_p_threadsetpriority(long priority) {
    struct thread *current = curproc->current_thread;
    if (!current || priority < 0 || priority > 31) {
        return -EINVAL;
    }

    DEBUG_TO_FILE("SET_PRIORITY: Thread %d: %d -> %ld", 
                 current->tid, current->priority, priority);

    // Save old priority
    int old_priority = current->priority;

    DEBUG_TO_FILE("[PRI] Thread %d: Old=%d, New=%d", 
        current->tid, current->priority, priority);

    // Update priority
    current->priority = priority;
    curproc->p_priority = priority;

    DEBUG_TO_FILE("Thread %d priority updated: %d -> %d", 
                 current->tid, old_priority, current->priority);

    // // Force reschedule if running
    // if (current->state == STATE_RUNNING) {
    //     current->state = STATE_READY;
    //     add_to_ready_queue(current);
    //     schedule();
    // }
    
    return 0;
}

long _cdecl sys_p_yield(void)
{
    struct proc *p = curproc;
    if (!p || !p->current_thread) {
        DEBUG_TO_FILE("sys_p_yield: invalid current process/thread");
        return -EINVAL;
    }

    DEBUG_TO_FILE("sys_p_yield: thread %d (pri %d) yielding", 
                 p->current_thread->tid, p->current_thread->priority);

    unsigned short sr = splhigh();
    
    struct thread *current = p->current_thread;
    
    // Save user context if in user mode
    if (!(current->ctxt.sr & SR_SUPER_MASK)) {
        save_context(&current->ctxt);  // Use build_context for user mode
    }

    // Remove from ready queue if present
    remove_from_ready_queue(current);
    
    // Add to end of ready queue
    current->state = STATE_READY;
    add_to_ready_queue(current);
    
    // Force schedule
    schedule();
    
    spl(sr);
    return 0;
}

long _cdecl sys_p_exit(void) {
    struct proc *p = curproc;
    struct thread *current = p->current_thread;
    struct thread **t;

	current->state = STATE_EXITED;
	current->thread_flags |= THREAD_EXITING;
	DEBUG_TO_FILE("Exiting thread %d from process %p", current->tid, p);

    // Remove current thread from ready queue
    remove_from_ready_queue(current);

    // Detach thread from process's thread list
    for (t = &p->threads; *t; t = &(*t)->next) {
        if (*t == current) {
            *t = current->next;
            break;
        }
    }

    // Free thread resources
	DEBUG_TO_FILE("In sys_p_exit -> Before Calling free_thread_stack\n", current->tid);
    free_thread_stack(current->stack);
    kfree(current);
    p->num_threads--;

    DEBUG_TO_FILE("Thread %d exited. Remaining threads: %d", current->tid, p->num_threads);

    // Terminate process if no threads remain
    if (p->num_threads == 0) {
        DEBUG_TO_FILE("Process %p terminating", p);
        rm_q(p->wait_q, p);
        kfree(p);
    }

    schedule();
	DEBUG_TO_FILE("Thread %d exited successfully.", current->tid);
    return 0;
}

long sys_thread_join(int tid, void **retval) {
    struct proc *p = curproc;
    struct thread *t = p->threads;
    
    // Find thread by tid
    while (t && t->tid != tid) {
        t = t->next;
    }
    
    if (!t || t->thread_flags & THREAD_DETACHED) {
        return -EINVAL;
    }
    
    // Add current process to thread's wait queue
    add_to_wait_queue(&t->join_queue, curproc->current_thread);
    curproc->p_blocked_on = t;
    th_sleep();
    
    if (retval) {
        *retval = (void*)(long)t->exit_code;
    }
    
    thread_cleanup(t);
    return 0;
}

long sys_thread_detach(int tid) {
    struct proc *p = curproc;
    struct thread *t = p->threads;
    
    while (t && t->tid != tid) {
        t = t->next;
    }
    
    if (!t) {
        return -EINVAL;
    }
    
    t->thread_flags |= THREAD_DETACHED;
    return 0;
}

long sys_thread_cancel(int tid) {
    struct proc *p = curproc;
    struct thread *t = p->threads;
    
    while (t && t->tid != tid) {
        t = t->next;
    }
    
    if (!t) {
        return -EINVAL;
    }
    
    t->thread_flags |= THREAD_CANCELLED;
    wakeup(t->proc);
    return 0;
}

void thread_cleanup(struct thread *t) {
	if (t->state == STATE_EXITED) {
		struct proc *waiting;
		// Cast to handle proc list with thread function (temporary fix)
		while ((waiting = (struct proc *)remove_highest_priority((struct thread **)&t->waiting_procs))) {
			wakeup(waiting);
		}
		DEBUG_TO_FILE("In thread_cleanup -> Before Calling free_thread_stack for thread %d\n", t->tid);
		free_thread_stack(t->stack);
		kfree(t);
	}
}


/*
 * initialize the process table
 */
void
init_proc(void)
{
	static DTABUF dta;

	static struct proc	rootproc0;
	static struct memspace	mem0;
	static struct ucred	ucred0;
	static struct pcred	pcred0;
	static struct filedesc	fd0;
	static struct cwd	cwd0;
	static struct sigacts	sigacts0;
	static struct plimit	limits0;

	mint_bzero(&sysq, sizeof(sysq));

	/* XXX */
	mint_bzero(&rootproc0, sizeof(rootproc0));
	mint_bzero(&mem0, sizeof(mem0));
	mint_bzero(&ucred0, sizeof(ucred0));
	mint_bzero(&pcred0, sizeof(pcred0));
	mint_bzero(&fd0, sizeof(fd0));
	mint_bzero(&cwd0, sizeof(cwd0));
	mint_bzero(&sigacts0, sizeof(sigacts0));
	mint_bzero(&limits0, sizeof(limits0)); 

	pcred0.ucr = &ucred0;			ucred0.links = 1;

	rootproc0.p_mem		= &mem0;	mem0.links = 1;
	rootproc0.p_cred	= &pcred0;	pcred0.links = 1;
	rootproc0.p_fd		= &fd0;		fd0.links = 1;
	rootproc0.p_cwd		= &cwd0;	cwd0.links = 1;
	rootproc0.p_sigacts	= &sigacts0;	sigacts0.links = 1;
//	rootproc0.p_limits	= &limits0;	limits0.links = 1;

	fd0.ofiles = fd0.dfiles;
	fd0.ofileflags = (unsigned char *)fd0.dfileflags;
	fd0.nfiles = NDFILE;

	DEBUG(("init_proc() inf : %p, %p, %p, %p, %p, %p, %p",
		&rootproc0, &mem0, &pcred0, &ucred0, &fd0, &cwd0, &sigacts0));

	curproc = rootproc = &rootproc0;
	rootproc0.links = 1;
	
	
	/* set the stack barrier */
	rootproc->stack_magic = STACK_MAGIC;

	rootproc->ppid = -1;		/* no parent */
	rootproc->p_flag = P_FLAG_SYS;
	rootproc->domain = DOM_TOS;	/* TOS domain */
	rootproc->sysstack = (long)(rootproc->stack + STKSIZE - 12);
	rootproc->magic = CTXT_MAGIC;

	((long *) rootproc->sysstack)[1] = FRAME_MAGIC;
	((long *) rootproc->sysstack)[2] = 0;
	((long *) rootproc->sysstack)[3] = 0;

	rootproc->p_fd->dta = &dta;	/* looks ugly */
	strcpy(rootproc->name, "MiNT");
	strcpy(rootproc->fname, "MiNT");
	strcpy(rootproc->cmdlin, "MiNT");

	/* get some memory */
	rootproc->p_mem->memflags = F_PROT_S; /* default prot mode: super-only */
	rootproc->p_mem->num_reg = NUM_REGIONS;
	{
		union { char *c; void *v; } ptr;
		unsigned long size = rootproc->p_mem->num_reg * sizeof(void *);
		ptr.v = kmalloc(size * 2);
		/* make sure kmalloc was successful */
		assert(ptr.v);
		rootproc->p_mem->mem = ptr.v;
		rootproc->p_mem->addr = (void *)(ptr.c + size);
		/* make sure it's filled with zeros */
		mint_bzero(ptr.c, size * 2L); 
	}
	rootproc->p_mem->base = _base;
	
	/* init trampoline things */
	rootproc->p_mem->tp_ptr = &kernel_things;
	rootproc->p_mem->tp_reg = NULL;

	/* init page table for curproc */
	init_page_table_ptr(rootproc->p_mem);
	init_page_table(rootproc, rootproc->p_mem);

	/* get root and current directories for all drives */
	{
		FILESYS *fs;
		int i;

		for (i = 0; i < NUM_DRIVES; i++)
		{
			fcookie dir;

			fs = drives[i];
			if (fs && xfs_root(fs, i, &dir) == E_OK)
			{
				rootproc->p_cwd->root[i] = dir;
				dup_cookie(&rootproc->p_cwd->curdir[i], &dir);
			}
			else
			{
				rootproc->p_cwd->root[i].fs = rootproc->p_cwd->curdir[i].fs = 0;
				rootproc->p_cwd->root[i].dev = rootproc->p_cwd->curdir[i].dev = i;
			}
		}
	}

	/* Set the correct drive. The current directory we
	 * set later, after all file systems have been loaded.
	 */
	rootproc->p_cwd->curdrv = TRAP_Dgetdrv();
	proclist = rootproc;

	rootproc->p_cwd->cmask = 0;

	/* some more protection against job control; unless these signals are
	 * re-activated by a shell that knows about job control, they'll have
	 * no effect
	 */
	SIGACTION(rootproc, SIGTTIN).sa_handler = SIG_IGN;
	SIGACTION(rootproc, SIGTTOU).sa_handler = SIG_IGN;
	SIGACTION(rootproc, SIGTSTP).sa_handler = SIG_IGN;

	/* set up some more per-process variables */
	rootproc->started = xtime;

	if (has_bconmap)
		/* init_xbios not happened yet */
		rootproc->p_fd->bconmap = (int) TRAP_Bconmap(-1);
	else
		rootproc->p_fd->bconmap = 1;
	rootproc->logbase = (void *) TRAP_Logbase();
	rootproc->criticerr = *((long _cdecl (**)(long)) 0x404L);

    // Initialize main thread for MiNT (rootproc)
    rootproc->threads = kmalloc(sizeof(struct thread));
    if (!rootproc->threads) {
        DEBUG_TO_FILE("Cannot allocate main thread");
        return;
    }

    struct thread *main_thread = rootproc->threads;
    main_thread->tid = 0;
    main_thread->stack = rootproc->stack;
    // For 68000: stack grows DOWN, so stack_base is higher than stack_top
    // CORRECTED: For stack growing DOWN
    main_thread->stack_base = rootproc->stack;                    // Low address
    main_thread->stack_top = (char *)rootproc->stack + STKSIZE; 
    main_thread->state = STATE_READY;
    main_thread->proc = rootproc;
    main_thread->priority = rootproc->pri;  // Inherit process priority
    main_thread->time_quantum = 5 + (main_thread->priority / 4); // Initial quantum
    rootproc->num_threads = 1;
    rootproc->current_thread = main_thread;

    // Initialize context structure
    struct context *ctx = &main_thread->ctxt;
    memset(ctx, 0, sizeof(struct context));

    // Set up initial context for root thread
    ctx->pc = (unsigned long)rootproc->sysstack;  // Current execution point
    ctx->usp = rootproc->sysstack;               // Current stack pointer
    ctx->sr = 0x2000;                            // Supervisor mode
    ctx->sfmt = 0x0000;                          // Normal frame format
    ctx->term_vec = *(unsigned long *)0x408;     // GEMDOS terminate vector
    
    // Store current registers
    asm volatile (
        "movem.l %%d0-%%d7/%%a0-%%a6,%0"
        : "=m" (ctx->regs[0])
        :
        : "memory"
    );

    DEBUG_TO_FILE("Root thread context initialized:");
    DEBUG_TO_FILE("  PC=0x%lx", ctx->pc);
    DEBUG_TO_FILE("  USP=0x%lx", ctx->usp);
    DEBUG_TO_FILE("  SR=0x%x", ctx->sr);
    DEBUG_TO_FILE("  Stack range: 0x%lx - 0x%lx",
                 (unsigned long)main_thread->stack,
                 (unsigned long)main_thread->stack + STKSIZE);

    rootproc->p_time_quantum = time_slice;

    // Add to ready queue
    add_to_ready_queue(main_thread);
}

/* remaining_proc_time():
 *
 * this function returns the numer of milliseconds remaining to
 * the normal preemption. It may be useful for drivers of devices,
 * which do not generate interrupts (Falcon IDE for example).
 * Such a device must give the CPU up from time to time, while
 * looping.
 *
 * Actually reading the proc_clock directly would be much simpler,
 * but doing it so we retain compatibility if we ever resize the
 * proc_clock variable to long or increase its granularity
 * (its actually 50 Hz).
 *
 */
unsigned long _cdecl
remaining_proc_time(void)
{
	unsigned long proc_ms = proc_clock;

	proc_ms *= 20; /* one tick is 20 ms */

	return proc_ms;
}

/* reset_priorities():
 *
 * reset all process priorities to their base level
 * called once per second, so that cpu hogs can get _some_ time
 * slices :-).
 */
void
reset_priorities(void)
{
	struct proc *p;

	for (p = proclist; p; p = p->gl_next)
	{
		if (p->slices >= 0)
		{
			p->curpri = p->pri;
			p->slices = SLICES(p->curpri);
		}
	}
}

/* run_next(p, slices):
 *
 * schedule process "p" to run next, with "slices" initial time slices;
 * "p" does not actually start running until the next context switch
 */
void
run_next(struct proc *p, int slices)
{
	register unsigned short sr = splhigh();

	p->slices = -slices;
	p->curpri = MAX_NICE;
	p->wait_q = READY_Q;
	p->q_next = sysq[READY_Q].head;
	sysq[READY_Q].head = p;
	if (!p->q_next)
		sysq[READY_Q].tail = p;
	else
		p->q_next->q_prev = p;
	p->q_prev = NULL;

	spl(sr);
}

/* fresh_slices(slices):
 *
 * give the current process "slices" more slices in which to run
 */
void
fresh_slices(int slices)
{
	reset_priorities();
	curproc->slices = 0;
	curproc->curpri = MAX_NICE + 1;
	proc_clock = time_slice + slices;
}

/*
 * add a process to a wait (or ready) queue.
 *
 * processes go onto a queue in first in-first out order
 */
void
add_q(int que, struct proc *proc)
{
	/* "proc" should not already be on a list */
	assert(proc->wait_q == 0);
	assert(proc->q_next == 0);

	if (sysq[que].tail) {
		proc->q_prev = sysq[que].tail;
		sysq[que].tail->q_next = proc;
	} else {
		proc->q_prev = NULL;
		sysq[que].head = proc;
	}
	sysq[que].tail = proc;
	proc->wait_q = que;
	if (que != READY_Q && proc->slices >= 0) {
		proc->curpri = proc->pri;	/* reward the process */
		proc->slices = SLICES(proc->curpri);
	}
}

/*
 * remove a process from a queue
 */
void
rm_q(int que, struct proc *proc)
{
	assert(proc->wait_q == que);

	if (proc->q_prev)
		proc->q_prev->q_next = proc->q_next;
	else
		sysq[que].head = proc->q_next;

	if (proc->q_next)
		proc->q_next->q_prev = proc->q_prev;
	else {
		if ((sysq[que].tail = proc->q_prev))
			proc->q_prev->q_next = NULL;
	}
	proc->wait_q = 0;
	proc->q_next = proc->q_prev = NULL;
}

/*
 * preempt(): called by the vbl routine and/or the trap handlers when
 * they detect that a process has exceeded its time slice and hasn't
 * yielded gracefully. For now, it just does sleep(READY_Q); later,
 * we might want to keep track of statistics or something.
 */

void _cdecl
preempt(void)
{
	assert(!(curproc->p_flag & P_FLAG_SYS));

	if (bconbsiz)
	{
		bflush();
	}
	else
	{
		/* punish the pre-empted process */
		if (curproc->curpri >= MIN_NICE)
			curproc->curpri -= 1;
	}

	sleep(READY_Q, curproc->wait_cond);
}

/*
 * swap_in_curproc(): for all memory regions of the current process swaps
 * in the contents of those regions that have been saved in a shadow region
 */

static void
swap_in_curproc(void)
{
	struct memspace *mem = curproc->p_mem;
	long txtsize = curproc->p_mem->txtsize;
	MEMREGION *m, *shdw, *save;
	int i;

	assert(mem && mem->mem);

	for (i = 0; i < mem->num_reg; i++)
	{
		m = mem->mem[i];
		if (m && m->save)
		{
			save = m->save;
			for (shdw = m->shadow; shdw->save; shdw = shdw->shadow)
				assert (shdw != m);

			assert (m->loc == shdw->loc);

			shdw->save = save;
			m->save = 0;
			if (i != 1 || txtsize == 0)
			{
				quickswap((char *)m->loc, (char *)save->loc, m->len);
			}
			else
			{
				quickswap((char *)m->loc, (char *)save->loc, 256);
				quickswap((char *)m->loc + (txtsize+256), (char *)save->loc + 256, m->len - (txtsize+256));
			}
		}
	}
}

/*
 * sleep(que, cond): put the current process on the given queue, then switch
 * contexts. Before a new process runs, give it a fresh time slice. "cond"
 * is the condition for which the process is waiting, and is placed in
 * curproc->wait_cond
 */

static void
do_wakeup_things(short sr, int newslice, long cond)
{
	/*
	 * check for stack underflow, just in case
	 */
	auto int foo;
	struct proc *p;

	p = curproc;

	if ((sr & 0x700) < 0x500)
	{
		/* skip all this if int level is too high */

		if (p->pid && ((long) &foo) < (long) p->stack + ISTKSIZE + 512)
		{
			ALERT("stack underflow");
			handle_sig(SIGBUS);
		}

		/* see if process' time limit has been exceeded */
		if (p->maxcpu)
		{
			if (p->maxcpu <= p->systime + p->usrtime)
			{
				DEBUG(("cpu limit exceeded"));
				raise(SIGXCPU);
			}
		}

		/* check for alarms and similar time out stuff */
		checkalarms();

		if (p->sigpending && cond != (long) sys_pwaitpid)
			/* check for signals */
			check_sigs();

		/* check for proc specific wakeup things */
		checkprocwakeup(p);
	}

	/* Kludge: restore the cookie jar pointer. If this to be restored,
	 * this means that the process has changed it directly, not through
	 * Setexc(). We don't like that.
	 */
# ifdef JAR_PRIVATE
	*CJAR = p->p_mem->tp_ptr->user_jar_p;
# endif

	if (newslice)
	{
		if (p->slices >= 0)
		{
			/* get a fresh time slice */
			proc_clock = time_slice;
		}
		else
		{
			/* slices set by run_next */
			proc_clock = time_slice - p->slices;
			p->curpri = p->pri;
		}

		p->slices = SLICES(p->curpri);
	}
}

static long sleepcond, iwakecond;

/*
 * sleep: returns 1 if no signals have happened since our last sleep, 0
 * if some have
 */

int _cdecl
sleep(int _que, long cond)
{
	struct proc *p;
	unsigned short sr;
	short que = _que & 0xff;
	unsigned long onsigs = curproc->nsigs;
	int newslice = 1;

	/* save condition, checkbttys may just wake() it right away ...
	 * note this assumes the condition will never be waked from interrupts
	 * or other than thru wake() before we really went to sleep, otherwise
	 * use the 0x100 bit like select
	 */
	sleepcond = cond;

	/* if there have been keyboard interrupts since our last sleep,
	 * check for special keys like CTRL-ALT-Fx
	 */
	sr = splhigh();
	if ((sr & 0x700) < 0x500)
	{
		/* can't call checkkeys if sleep was called
		 * with interrupts off  -nox
		 */
		spl(sr);
		checkbttys();
		if (kintr)
		{
			checkkeys();
			kintr = 0;
		}

# ifdef DEV_RANDOM
		/* Wake processes waiting for random bytes */
		checkrandom();
# endif

		sr = splhigh();
		if ((curproc->sigpending & ~(curproc->p_sigmask))
			&& curproc->pid && que != ZOMBIE_Q && que != TSR_Q)
		{
			spl(sr);
			check_sigs();
			sleepcond = 0;	/* possibly handled a signal, return */
			sr = splhigh();
		}
	}

	/* kay: If _que & 0x100 != 0 then take curproc->wait_cond != cond as
	 * an indicatation that the wakeup has already happend before we
	 * actually go to sleep and return immediatly.
	 */
	if ((que == READY_Q && !sysq[READY_Q].head)
	    || ((sleepcond != cond || (iwakecond == cond && cond) || (_que & 0x100 && curproc->wait_cond != cond))
		&& (!sysq[READY_Q].head || (newslice = 0, proc_clock))))
	{
		/* we're just going to wake up again right away! */
		iwakecond = 0;

		spl(sr);
		do_wakeup_things(sr, newslice, cond);

		return (onsigs != curproc->nsigs);
	}

	/* unless our time slice has expired (proc_clock == 0) and other
	 * processes are ready...
	 */
	iwakecond = 0;
	if (!newslice)
		que = READY_Q;
	else
		curproc->wait_cond = cond;

	add_q(que, curproc);

	/* alright curproc is on que now... maybe there's an
	 * interrupt pending that will wakeselect or signal someone
	 */
	spl(sr);

	if (!sysq[READY_Q].head)
	{
		/* hmm, no-one is ready to run. might be a deadlock, might not.
		 * first, try waking up any napping processes;
		 * if that doesn't work, run the root process,
		 * just so we have someone to charge time to.
		 */
		wake(SELECT_Q, (long) nap);

		if (!sysq[READY_Q].head)
		{
			sr = splhigh();
			p = rootproc;		/* pid 0 */
			rm_q(p->wait_q, p);
			add_q(READY_Q, p);
			spl(sr);
		}
	}

	/*
	 * Walk through the ready list, to find what process should run next.
	 * Lower priority processes don't get to run every time through this
	 * loop; if "p->slices" is positive, it's the number of times that
	 * they will have to miss a turn before getting to run again
	 *
	 * Loop structure:
	 *	while (we haven't picked anybody)
	 *	{
	 *		for (each process)
	 *		{
	 *			if (sleeping off a penalty)
	 *			{
	 *				decrement penalty counter
	 *			}
	 *			else
	 *			{
	 *				pick this one and break out of
	 *				both loops
	 *			}
	 *		}
	 *	}
	 */

	sr = splhigh();
	p = 0;
	while (!p)
	{
		for (p = sysq[READY_Q].head; p; p = p->q_next)
		{
			if (p->slices > 0)
				p->slices--;
			else
				break;
		}
	}
	/* p is our victim */
	rm_q(READY_Q, p);
	spl(sr);

	if (save_context(&(curproc->ctxt[CURRENT])))
	{
		/*
		 * restore per-process variables here
		 */
		swap_in_curproc();
		do_wakeup_things(sr, 1, cond);

		return (onsigs != curproc->nsigs);
	}

	/*
	 * save per-process variables here
	 */
	curproc->ctxt[CURRENT].regs[0] = 1;
	curproc = p;

	proc_clock = time_slice;			/* fresh time */

	if ((p->ctxt[CURRENT].sr & 0x2000) == 0)	/* user mode? */
		leave_kernel();

	assert(p->magic == CTXT_MAGIC);
	change_context(&(p->ctxt[CURRENT]));

	/* not reached */
	return 0;
}

/*
 * wake(que, cond): wake up all processes on the given queue that are waiting
 * for the indicated condition
 */

INLINE void
do_wake(int que, long cond)
{
	struct proc *p;

top:
	p = sysq[que].head;

	while (p)
	{
		register unsigned short s = splhigh();

		/* check if p is still on the right queue,
		 * maybe an interrupt just woke it...
		 */
		if (p->wait_q != que)
		{
			spl(s);
			goto top;
		}

		/* move to ready queue */
		{
			struct proc *q = p;

			p = p->q_next;

			if (q->wait_cond == cond)
			{
				rm_q(que, q);
				add_q(READY_Q, q);
			}
		}

		spl(s);
	}
}

void _cdecl
wake(int que, long cond)
{
	if (que == READY_Q)
	{
		ALERT("wake: why wake up ready processes??");
		return;
	}

	if (sleepcond == cond)
		sleepcond = 0;

	do_wake(que, cond);
}

/*
 * iwake(que, cond, pid): special version of wake() for IO interrupt
 * handlers and such.  the normal wake() would lose when its
 * interrupt goes off just before a process is calling sleep() on the
 * same condition (similar problem like with wakeselect...)
 *
 * use like this:
 *	static ipid = -1;
 *	static volatile sleepers = 0;	(optional, to save useless calls)
 *	...
 *	device_read(...)
 *	{
 *		ipid = curproc->pid;	(p_getpid() for device drivers...)
 *		while (++sleepers, (not ready for IO...)) {
 *			sleep(IO_Q, cond);
 *			if (--sleepers < 0)
 *				sleepers = 0;
 *		}
 *		if (--sleepers < 0)
 *			sleepers = 0;
 *		ipid = -1;
 *		...
 *	}
 *
 * and in the interrupt handler:
 *	if (sleepers > 0)
 *	{
 *		sleepers = 0;
 *		iwake (IO_Q, cond, ipid);
 *	}
 *
 * caller is responsible for not trying to wake READY_Q or other nonsense :)
 * and making sure the passed pid is always -1 when curproc is calling
 * sleep() for another than the waked que/condition.
 */

void _cdecl
iwake(int que, long cond, short pid)
{
	if (pid >= 0)
	{
		register unsigned short s = splhigh();

		if (iwakecond == cond)
		{
			spl(s);
			return;
		}

		if (curproc->pid == pid && !curproc->wait_q)
			iwakecond = cond;

		spl(s);
	}

	do_wake(que, cond);
}

/*
 * wakeselect(p): wake process p from a select() system call
 * may be called by an interrupt handler or whatever
 */

void _cdecl
wakeselect(struct proc *p)
{
	unsigned short s = splhigh();

	if (p->wait_cond == (long) wakeselect
		|| p->wait_cond == (long) &select_coll)
	{
		p->wait_cond = 0;
	}

	if (p->wait_q == SELECT_Q)
	{
		rm_q(SELECT_Q, p);
		add_q(READY_Q, p);
	}

	spl(s);
}

/*
 * dump out information about processes
 */

/*
 * kludge alert! In order to get the right pid printed by FORCE, we use
 * curproc as the loop variable.
 *
 * I have changed this function so it is more useful to a user, less to
 * somebody debugging MiNT.  I haven't had any stack problems in MiNT
 * at all, so I consider all that stack info wasted space.  -- AKP
 */

# ifdef DEBUG_INFO
static const char *qstring[] =
{
	"run", "ready", "wait", "iowait", "zombie", "tsr", "stop", "select"
};

/* UNSAFE macro for qname, evaluates x 1, 2, or 3 times */
# define qname(x) ((x >= 0 && x < NUM_QUEUES) ? qstring[x] : "unkn")
# endif

unsigned long uptime = 0;
unsigned long avenrun[3] = { 0, 0, 0 };
unsigned short uptimetick = 200;

static unsigned short number_running;

void
DUMPPROC(void)
{
#ifdef DEBUG_INFO
	struct proc *p = curproc;

	FORCE("Uptime: %ld seconds Loads: %ld %ld %ld Processes running: %d",
		uptime,
		(avenrun[0] * 100) / 2048 , (avenrun[1] * 100) / 2048, (avenrun[2] * 100 / 2048),
 		number_running);

	for (curproc = proclist; curproc; curproc = curproc->gl_next)
	{
		FORCE("state %s sys %s, PC: %lx/%lx BP: %p (pgrp %i)",
			qname(curproc->wait_q),
			curproc->p_flag & P_FLAG_SYS ? "yes":" no",
			curproc->ctxt[CURRENT].pc, curproc->ctxt[SYSCALL].pc,
			curproc->p_mem ? curproc->p_mem->base : NULL,
			curproc->pgrp);
	}
	curproc = p;	/* restore the real curproc */
# endif
}

INLINE unsigned long
gen_average(unsigned long *sum, unsigned char *load_ptr, unsigned long max_size)
{
	register long old_load = (long) *load_ptr;
	register long new_load = number_running;

	*load_ptr = (unsigned char) new_load;

	*sum += (new_load - old_load) * LOAD_SCALE;

	return (*sum / max_size);
}

void
calc_load_average(void)
{
	static unsigned char one_min [SAMPS_PER_MIN];
	static unsigned char five_min [SAMPS_PER_5MIN];
	static unsigned char fifteen_min [SAMPS_PER_15MIN];

	static unsigned short one_min_ptr = 0;
	static unsigned short five_min_ptr = 0;
	static unsigned short fifteen_min_ptr = 0;

	static unsigned long sum1 = 0;
	static unsigned long sum5 = 0;
	static unsigned long sum15 = 0;

	register struct proc *p;

# if 0	/* moved to intr.spp */
	uptime++;
	uptimetick += 200;

	if (uptime % 5) return;
# endif

	number_running = 0;

	for (p = proclist; p; p = p->gl_next)
	{
		if (p != rootproc)
		{
			if ((p->wait_q == CURPROC_Q) || (p->wait_q == READY_Q))
				number_running++;
		}

		/* Check the stack magic here, to ensure the system/interrupt
		 * stack hasn't grown too much. Most noticeably, NVDI 5's new
		 * bitmap conversion (vr_transfer_bits()) seems to eat _a lot_
		 * of supervisor stack, that's why the values in proc.h have
		 * been increased.
		 */
		if (p->stack_magic != STACK_MAGIC)
			FATAL("proc %lx has invalid stack_magic %lx", (long) p, p->stack_magic);
	}

	if (one_min_ptr == SAMPS_PER_MIN)
		one_min_ptr = 0;

	avenrun [0] = gen_average(&sum1, &one_min [one_min_ptr++], SAMPS_PER_MIN);

	if (five_min_ptr == SAMPS_PER_5MIN)
		five_min_ptr = 0;

	avenrun [1] = gen_average(&sum5, &five_min [five_min_ptr++], SAMPS_PER_5MIN);

	if (fifteen_min_ptr == SAMPS_PER_15MIN)
		fifteen_min_ptr = 0;

	avenrun [2] = gen_average(&sum15, &fifteen_min [fifteen_min_ptr++], SAMPS_PER_15MIN);
}
