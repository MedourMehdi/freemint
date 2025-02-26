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
#define THREAD_STACK_SIZE  (8 * 1024)    // 8 KB default stack
#define GUARD_PAGE_SIZE    0             // Disabled for now
// #define STACK_MAGIC        0xDEADBEEF    // Sentinel value for overflow detection

/*
 * We initialize proc_clock to a very large value so that we don't have
 * to worry about unexpected process switches while starting up
 */
unsigned short proc_clock = 0x7fff;

struct proc_queue sysq[NUM_QUEUES] = { { NULL } };

struct proc *ready_queue = NULL;

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

// Assembly snippet to initialize a new thread's stack
void init_thread_stack(struct thread *t, void (*entry)(void*), void *arg) {
    unsigned long *sp = (unsigned long*)((char*)t->stack + THREAD_STACK_SIZE);
    sp = (unsigned long*)((unsigned long)sp & ~1);  // Align to even address
    *--sp = (unsigned long)arg;     // Argument
    *--sp = (unsigned long)entry;   // PC
    t->sp = sp;
}

/* Context switch assembly (m68k) */
void switch_to_thread(struct proc *from, struct proc *to)
{
	#ifdef __mcoldfire__
    asm volatile (
        "lea -48(sp),sp\n\t"
        "movem.l d2-d7/a2-a6,(sp)\n\t"
        "move.l sp,%0\n\t"
        "move.l %1,sp\n\t"
        "movem.l (sp),d2-d7/a2-a6\n\t"
        "lea 48(sp),sp"
        : "=m" (from->p_save_sp)
        : "m" (to->p_save_sp)
        : "memory"
    );
#else
	asm volatile (
		 "movem.l  %%d2-%%d7/%%a2-%%a6, -(%%sp)\n\t"
		 "move.l   %%sp, %0\n\t"
		 "move.l   %1, %%sp\n\t"
		 "movem.l  (%%sp)+, %%d2-%%d7/%%a2-%%a6\n\t"
		 : "=m" (from->p_save_sp)
		 : "m" (to->p_save_sp)
		 : "memory"
	);
#endif
}

/* Process iteration macro */
#define for_each_proc(p) for (p = proclist; p != NULL; p = p->p_next)

void schedule(void)
{
	struct proc *p, *next = NULL;
	int highest_pri = -1;

	// Select highest-priority ready thread
	for (p = ready_queue; p != NULL; p = p->p_next) {
		if (p->p_priority > highest_pri) {
			highest_pri = p->p_priority;
			next = p;
		}
	}

	if (next) {
		curproc->p_time_quantum = 10; // Reset quantum (10 ms)
		switch_to_thread(curproc, next);
		curproc = next; // Update current process
	}
}

// Add a thread to a mutex's wait queue (sorted by priority)
void add_to_wait_queue(struct proc **queue, struct proc *p)
{
	struct proc **curr;
	for (curr = queue; *curr; curr = &(*curr)->p_wnext) {
		if (p->p_priority > (*curr)->p_priority) break;
	}
	p->p_wnext = *curr;
	*curr = p;
}

/**
 * Removes a thread from a wait queue.
 */
void remove_from_wait_queue(struct proc **queue, struct proc *p)
{
	if (!queue || !p) return;

	struct proc **curr;
	for (curr = queue; *curr != NULL; curr = &(*curr)->p_wnext) {
		if (*curr == p) {
			*curr = p->p_wnext;
			p->p_wnext = NULL;
			break;
		}
	}
}

void add_to_ready_queue(struct proc *p)
{
	// Insert into ready_queue sorted by priority
	struct proc **curr;
	for (curr = &ready_queue; *curr; curr = &(*curr)->p_next) {
		if (p->p_priority > (*curr)->p_priority) break;
	}
	p->p_next = *curr;
	*curr = p;
}

void remove_from_ready_queue(struct proc *p)
{
	struct proc **curr;
	for (curr = &ready_queue; *curr; curr = &(*curr)->p_next) {
		if (*curr == p) {
			*curr = p->p_next;
			p->p_next = NULL;
			break;
		}
	}
}

// Remove the highest-priority thread from a wait queue
struct proc* remove_highest_priority(struct proc **queue)
{
	struct proc *highest = *queue;
	if (highest) *queue = highest->p_wnext;
	return highest;
}

void th_sleep(void)
{
	curproc->p_state = STATE_BLOCKED;
	curproc->p_block_time = timer_ticks;
	remove_from_ready_queue(curproc);
	schedule();
}

void wakeup(struct proc *p)
{
	remove_from_wait_queue((struct proc **)&p->p_blocked_on, p);
	add_to_ready_queue(p);
	p->p_state = STATE_RUNNING;
}

/* Timer ISR: called every millisecond */
void timer_interrupt_handler(void)
{
	timer_ticks++;

	if (!curproc || !curproc->threads) return;

	struct proc *p = curproc;

	if (p->p_time_quantum > 0) p->p_time_quantum--;
	if (p->p_time_quantum == 0 || (p->p_flags & PF_YIELD)) {
		p->p_flags &= ~PF_YIELD;
		schedule();
	}
}

/* Mutex functions */
void mutex_lock(struct mutex *m)
{
	while (tas(&m->locked)) {
		add_to_wait_queue(&m->wait_queue, curproc);
		th_sleep();
	}
	m->owner = curproc;
}

void mutex_unlock(struct mutex *m)
{
	m->locked = 0;
	m->owner = NULL;
	struct proc *next = remove_highest_priority(&m->wait_queue);
	if (next) wakeup(next);
}

/* Thread stack management */
void* allocate_thread_stack(void)
{
	void *stack = kmalloc(THREAD_STACK_SIZE + GUARD_PAGE_SIZE);
	if (!stack) return NULL;

	unsigned long *stack_top = (unsigned long *)((char*)stack + GUARD_PAGE_SIZE + THREAD_STACK_SIZE - 4);
	*stack_top = STACK_MAGIC; // Sentinel
	return (char*)stack + GUARD_PAGE_SIZE;
}

void free_thread_stack(void *stack)
{
	if (stack) {
		void *base = (char*)stack - GUARD_PAGE_SIZE;
		kfree(base);
	}
}

void mutex_init(struct mutex *m) {
    m->locked = 0;
    m->owner = NULL;
    m->wait_queue = NULL;
}

void semaphore_init(struct semaphore *s, int count) {
    s->count = count;
    s->wait_queue = NULL;
}

long sys_tls_create(void) {
    if (tls_next_key >= THREAD_TLS_KEYS) return -ENOMEM;
    return tls_next_key++;
}

long sys_tls_set(long key, void *value) {
    if (key < 0 || key >= THREAD_TLS_KEYS) return -EINVAL;
    curproc->threads->tls[key] = value;
    return 0;
}

long sys_tls_get(long key) {
    if (key < 0 || key >= THREAD_TLS_KEYS) return -EINVAL;
    return (long)curproc->threads->tls[key];
}

long sys_create_thread(void (*func)(void*), void *arg, void *stack) 
{
    struct proc *p = curproc;

	Debug("sys_create_thread: func=%p arg=%p stack=%p", func, arg, stack);

    // First thread for this process
    if (!p->threads) {
        // Initialize thread subsystem
        if (!timer_initialized) {
            Jdisint(VBL);
            old_timer = (void *)Setexc(VBL, (long)timer_interrupt_handler);
            Jenabint(VBL);
            timer_initialized = 1;
        }
        
        // Create main thread
        p->threads = kmalloc(sizeof(struct thread));
        p->threads->tid = 0;
        p->threads->stack = p->stack;
        p->threads->next = NULL;
        p->num_threads = 1;
        p->p_state = STATE_RUNNING;
        p->p_priority = 20;
        ready_queue = p;
        add_to_ready_queue(p);
    }
    
    // Create new thread
    struct thread *t = kmalloc(sizeof(struct thread));
    t->stack = stack ? stack : allocate_thread_stack();
    t->tid = p->num_threads++;
    t->proc = p;
    t->priority = p->p_priority;
    t->next = p->threads;
    p->threads = t;
    
    init_thread_stack(t, func, arg);
    return t->tid;
}

// long sys_create_thread(void (*func)(void*), void *arg, void *stack) {
//     struct proc *p = curproc;
//     struct thread *t = kmalloc(sizeof(struct thread));

// 	if (!func || !stack) return -EINVAL;

//     if (!t) return -ENOMEM;

//     t->stack = stack ? stack : allocate_thread_stack();
//     if (!t->stack) {
//         kfree(t);
//         return -ENOMEM;
//     }

//     // Initialize thread context
//     t->tid = p->num_threads++;
//     t->proc = p;
//     t->priority = p->p_priority;
//     t->next = p->threads;
//     p->threads = t;

//     // // Set up initial stack frame (m68k-specific)
//     // unsigned long *sp = (unsigned long*)((char*)t->stack + THREAD_STACK_SIZE - 4);
//     // *--sp = (unsigned long)arg;       // Argument
//     // *--sp = (unsigned long)func;      // PC (start execution here)
//     // t->sp = sp;                  // Saved SP

//     // Initialize thread stack
//     init_thread_stack(t, func, arg);

//     add_to_ready_queue(p);
//     return t->tid;
// }

/* Syscalls */
long sys_setpriority(long priority)
{
	struct proc *p = curproc;
	if (priority < 0 || priority > 31) return -EINVAL;
	p->p_priority = priority;
	return 0;
}

long sys_yield(void)
{
	curproc->p_flags |= PF_YIELD;
	schedule();
	return 0;
}

long sys_exit(void) {
    struct proc *p = curproc;
    struct thread *t = p->threads;

    // Remove all threads
    while (t) {
        struct thread *next = t->next;
        free_thread_stack(t->stack);
        kfree(t);
        t = next;
    }

    // If last thread, terminate process
    if (p->num_threads == 0) {
        remove_from_ready_queue(p);
        kfree(p);
    }

    schedule();
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
    add_to_wait_queue(&t->waiting_procs, curproc);
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
    if (!t || !t->proc->threads) return;
    
    // Wake up all waiting processes
    struct proc *waiting;
    while ((waiting = remove_highest_priority(&t->waiting_procs))) {
        waiting->p_blocked_on = NULL;
        wakeup(waiting);
    }
    
    free_thread_stack(t->stack);
    kfree(t);
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

    rootproc->threads = NULL;
    rootproc->num_threads = 0;
    rootproc->p_state = 0;
    rootproc->p_priority = 0;
    rootproc->p_time_quantum = 0;
    rootproc->p_flags = 0;

    // rootproc->threads = kmalloc(sizeof(struct thread));
    // rootproc->threads->tid = 0;
    // rootproc->threads->stack = rootproc->stack; // Use main stack
    // rootproc->num_threads = 1;
    // rootproc->p_state = STATE_RUNNING;
    // rootproc->p_priority = 20; // Default priority
    
    // /* Initialize ready queue */
    // ready_queue = rootproc;

    // add_to_ready_queue(rootproc);

    // Set up timer for thread scheduling
    // proc_clock = time_slice;
    // old_timer = (void *)Setexc(0x100, (long)timer_interrupt_handler);
    // *((volatile unsigned short *)0x468L) = 20; // 50Hz
//         if (!timer_active) {
//             Jdisint(VBL);
//             old_timer = (void *)Setexc(VBL, (long)timer_interrupt_handler);
//             Jenabint(VBL);
//             timer_active = 1;
//         }	
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
