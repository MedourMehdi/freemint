/**
 * @file proc_threads_scheduler.c
 * @brief Kernel Thread Scheduler Core
 * 
 * Implements preemptive thread scheduling with POSIX policies inside the kernel.
 * Handles context switching, priority inheritance, and thread exit resource reclamation.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

 /**
 * Thread Scheduler Core
 * 
 * Implements preemptive thread scheduling with POSIX-compliant policies 
 * (FIFO, RR, OTHER). Features timeslice management, priority inheritance, 
 * and robust thread exit handling with resource cleanup.
 */

#include "proc_threads_scheduler.h"

#include "proc_threads_helper.h"
#include "proc_threads_queue.h"
#include "proc_threads_sleep_yield.h"
#include "proc_threads_policy.h"
#include "proc_threads_sync.h"
#include "proc_threads_signal.h"
#include "proc_threads_tsd.h"
#include "proc_threads_cleanup.h"
#include "proc_threads_cancel.h"

void reset_thread_switch_state(void);
void thread_switch_timeout_handler(PROC *p, long arg);

/* Thread scheduling helper functions */
static int should_schedule_thread(struct thread *current, struct thread *next);
// static void thread_switch(struct thread *from, struct thread *to);

/* Thread exit helper functions */
static void cancel_thread_timeouts(struct proc *p, struct thread *t);
static struct thread *find_next_thread_to_run(struct proc *p);

/* Mutex for timer operations */
static int timer_operation_locked = 0;
static int thread_switch_in_progress = 0;
static TIMEOUT *thread_switch_timeout = NULL;

/* Structure to encapsulate thread switch context and reduce parameter passing */
struct thread_switch_context {
    struct thread *from;
    struct thread *to;
    struct proc *process;
    unsigned long switch_time;
    CONTEXT *to_ctx;
    int should_reset_boost;
};

/* Structure to prepare scheduling decisions outside critical sections */
struct scheduling_decision {
    struct thread *current_thread;
    struct thread *next_thread;
    int should_switch;
    unsigned long decision_time;
};

/* Forward declarations for new functions */
static void prepare_thread_switch(struct thread_switch_context *ctx);
static void execute_thread_switch(struct thread_switch_context *ctx);
static int prepare_scheduling_decision(struct proc *p, struct scheduling_decision *decision);
static void execute_scheduling_decision(struct proc *p, struct scheduling_decision *decision);

/**
 * Thread preemption handler
 * 
 * This function is called periodically to implement preemptive multitasking.
 * It checks if the current thread should be preempted and schedules another thread if needed.
 */
void thread_preempt_handler(PROC *p, long arg) {
    register unsigned short sr;
    struct thread *thread_arg = (struct thread *)arg;
    
    if (!p) {
        TRACE_THREAD("PREEMPT: Invalid process pointer");
        return;
    }

    // If not current process, reschedule the timeout
    if (p != curproc) {
        /* Boost the timer for the current process */
        /* Should be disabled for non threaded mintlib's functions like sleep() */
        // make_process_eligible(p);

        reschedule_preemption_timer(p, (long)p->current_thread);
        return;
    }
    // Check for sleeping threads first
    if (p->sleep_queue) {
        TRACE_THREAD("PREEMPT: Checking sleep queue for process %d", p->pid);
        check_and_wake_sleeping_threads(p);
    }

    TRACE_THREAD("PREEMPT: Timer fired for process %d", p->pid);
    // Validate thread argument once
    if (thread_arg && (thread_arg->magic != CTXT_MAGIC || thread_arg->proc != p)) {
        TRACE_THREAD("PREEMPT: Invalid thread argument, using current thread");
        thread_arg = p->current_thread;
    }
    
    // Protection against reentrance
    if (p->p_thread_timer.in_handler) {
        if (!p->p_thread_timer.enabled) {
            TRACE_THREAD("PREEMPT: Timer disabled, not rescheduling");
            return;
        }
        TRACE_THREAD("PREEMPT: Already in handler, rescheduling");
        reschedule_preemption_timer(p, (long)p->current_thread);
        return;
    }
    
    p->p_thread_timer.in_handler = 1;
    p->p_thread_timer.timeout = NULL;
    sr = splhigh();
    
    struct thread *curr_thread = p->current_thread;
    
    // Update timeslice accounting (skip for FIFO threads)
    if (curr_thread->policy != SCHED_FIFO) {
        unsigned long elapsed = get_system_ticks() - curr_thread->last_scheduled;
        
        if (curr_thread->remaining_timeslice <= elapsed) {
            // Timeslice expired, reset and mark for potential preemption
            curr_thread->remaining_timeslice = curr_thread->timeslice;
            
            // For RR and OTHER, move to end of ready queue if preempted
            if (curr_thread->policy == SCHED_RR || curr_thread->policy == SCHED_OTHER) {
                TRACE_THREAD("PREEMPT: Thread %d timeslice expired", curr_thread->tid);
                atomic_thread_state_change(curr_thread, THREAD_STATE_READY);
                add_to_ready_queue(curr_thread);
            }
        } else {
            curr_thread->remaining_timeslice -= elapsed;
        }
    }

    struct thread *next = get_highest_priority_thread(p);
    
    if (!next) {
        TRACE_THREAD("PREEMPT: No other threads to run, continuing with current thread %d", curr_thread->tid);
        
        // If there are sleeping threads, create an idle thread
        if (p->sleep_queue) {
            next = get_idle_thread(p);
            if (next) {
                TRACE_THREAD("PREEMPT: Created idle thread to wait for sleeping threads");
                remove_from_ready_queue(next);
                atomic_thread_state_change(next, THREAD_STATE_RUNNING);
                p->current_thread = next;
                // next->last_scheduled = get_system_ticks();
                
                // Rearm timer before switch
                reschedule_preemption_timer(p, (long)next);
                
                TRACE_THREAD("PREEMPT: Switching from thread %d to idle thread", curr_thread->tid);
                thread_switch(curr_thread, next);
                
                spl(sr);
                return;
            }
        }
        
        reschedule_preemption_timer(p, (long)curr_thread);
        spl(sr);
        return;
    }
    
    // Check if we should preempt current thread
    if (should_schedule_thread(curr_thread, next)) {
        // Don't preempt if thread switch already in progress
        if (thread_switch_in_progress) {
            TRACE_THREAD("PREEMPT: Thread switch already in progress");
            reschedule_preemption_timer(p, (long)curr_thread);
            spl(sr);
            return;
        }
        
        // Perform the switch
        remove_from_ready_queue(next);
        atomic_thread_state_change(next, THREAD_STATE_RUNNING);
        p->current_thread = next;
        // next->last_scheduled = get_system_ticks();
        
        // Rearm timer before switch
        reschedule_preemption_timer(p, (long)next);
        
        TRACE_THREAD("PREEMPT: Switching from thread %d to thread %d", curr_thread->tid, next->tid);
        thread_switch(curr_thread, next);
        
        spl(sr);
        return;
    }
    
    // No switch needed, reschedule timer
    reschedule_preemption_timer(p, (long)curr_thread);
    spl(sr);
}

/**
 * Schedule a new thread to run
 * 
 * This function implements the core scheduling algorithm for threads.
 * It selects the next thread to run based on priority and scheduling policy,
 * and performs the context switch if needed.
 */
void proc_thread_schedule(void) {
    struct proc *p = get_curproc();
    register unsigned short sr;
    
    if (!p) {
        TRACE_THREAD("SCHED: Invalid current process");
        return;
    }

    TRACE_THREAD("SCHED: Entered scheduler");
    
    // Create a scheduling decision structure
    struct scheduling_decision decision = {0};
    
    // Prepare the scheduling decision outside critical section
    if (!prepare_scheduling_decision(p, &decision)) {
        // No switch needed
        return;
    }
    
    // Enter critical section for the actual switch
    sr = splhigh();
    
    // Execute the scheduling decision in minimal critical section
    execute_scheduling_decision(p, &decision);
    
    spl(sr);
}

/**
 * Handle thread joining during thread exit
 * 
 * @param current The exiting thread
 * @param retval The return value of the exiting thread
 */
void handle_thread_joining(struct thread *current, void *retval) {
    if (!current || !current->joiner || current->joiner->magic != CTXT_MAGIC) {
        return;
    }
    
    struct thread *joiner = current->joiner;
    TRACE_THREAD("EXIT: Thread %d is being joined by thread %d", 
                current->tid, joiner->tid);
    
    // If joiner is waiting for this thread
    if ((joiner->wait_type & WAIT_JOIN) && joiner->join_wait_obj == current) {
        // Wake up the joining thread
        joiner->wait_type &= ~WAIT_JOIN;
        joiner->join_wait_obj = NULL;
        
        // Store return value directly in joiner's requested location
        if (joiner->join_retval) {
            *(joiner->join_retval) = retval;
        }
        
        // Mark as joined
        current->joined = 1;
        
        // Wake up joiner
        atomic_thread_state_change(joiner, THREAD_STATE_READY);
        add_to_ready_queue(joiner);
        TRACE_THREAD("EXIT: Woke up joining thread %d", joiner->tid);
    }
}

/**
 * Cancel all timeouts associated with a thread
 * 
 * @param p The process containing the thread
 * @param t The thread whose timeouts should be cancelled
 */
static void cancel_thread_timeouts(struct proc *p, struct thread *t) {
    if (!p || !t) {
        return;
    }
    
    TIMEOUT *timelist, *next_timelist;
    for (timelist = tlist; timelist; timelist = next_timelist) {
        next_timelist = timelist->next;
        if (timelist->proc == p && timelist->arg == (long)t) {
            TRACE_THREAD("Cancelling timeout with thread %d as argument", t->tid);
            canceltimeout(timelist);
        }
    }
}

/**
 * Find the next thread to run after a thread exits
 * 
 * @param p The process containing the threads
 * @return The next thread to run, or NULL if none found
 */
static struct thread *find_next_thread_to_run(struct proc *p) {
    struct thread *next_thread = NULL;
    
    if (!p) {
        return NULL;
    }
    
    // STEP 1: First check the sleep queue for threads that should wake up
    if (p->sleep_queue) {
        unsigned long current_time = get_system_ticks();
        int woke_threads;
        
        TRACE_THREAD("EXIT: Checking sleep queue at time %lu", current_time);
        
        // Wake threads that have reached their wakeup time
        woke_threads = wake_threads_by_time(p, current_time);
        
        if (woke_threads > 0) {
            TRACE_THREAD("EXIT: Woke up %d threads from sleep queue", woke_threads);
        }
    }
    
    // STEP 2: Check the ready queue for the next thread to run
    next_thread = p->ready_queue;
    
    // Make sure the next thread is valid
    while (next_thread && (next_thread->magic != CTXT_MAGIC || 
                          (next_thread->state & THREAD_STATE_EXITED))) {
        TRACE_THREAD("EXIT: Skipping invalid thread %d in ready queue", next_thread->tid);
        remove_from_ready_queue(next_thread);
        next_thread = p->ready_queue;
    }
    
    if (next_thread) {
        TRACE_THREAD("EXIT: Found next thread %d in ready queue", next_thread->tid);
        remove_from_ready_queue(next_thread);
        return next_thread;
    }
    
    // STEP 3: If no thread in ready queue, try to find thread0
    // (but only if we're not already thread0)
    int current_tid = p->current_thread ? p->current_thread->tid : -1;
    if (current_tid != 0) {
        TRACE_THREAD("EXIT: No ready threads, looking for thread0");
        struct thread *t;
        int count = 0;
        for (t = p->threads; t != NULL && count < p->num_threads; t = t->next, count++) {
            if (t->tid == 0 && 
                t->magic == CTXT_MAGIC && 
                !(t->state & THREAD_STATE_EXITED) &&
                !(t->wait_type & WAIT_JOIN)) {
                next_thread = t;
                TRACE_THREAD("EXIT: Found thread0 at %p, state=%d, wait_type=%d", 
                            next_thread, next_thread->state, next_thread->wait_type);
                break;
            }
        }
    }
    
    return next_thread;
}

/**
 * Clean up thread resources during thread exit
 * 
 * @param p The process containing the thread
 * @param t The thread to clean up
 * @param tid The thread ID (for logging)
 */
void cleanup_thread_resources(struct proc *p, struct thread *t, int tid) {
    if (!p || !t || t->magic != CTXT_MAGIC) {
        TRACE_THREAD("EXIT: Cleaning up resources: Invalid thread %d", tid);
        return;
    }
    TRACE_THREAD("EXIT: Cleaning up resources for thread %d", tid);

    /* Clean up signal stack */
    cleanup_signal_stack(p, (long)t);

    /* Clean up thread signal resources */
    cleanup_thread_signals(t);

    /* Clean up thread cleanup handlers */
    cleanup_thread_handlers(t);

    /* Clean up cancellation state */
    cleanup_thread_cancellation(t);

    /* Clean up thread-specific data */
    cleanup_thread_tsd(t);

    if (t->alarm_timeout) {
        canceltimeout(t->alarm_timeout);
        t->alarm_timeout = NULL;
        TRACE_THREAD("EXIT: Cancelled alarm timeout for thread %d", tid);
    }
    
    // Clear thread signal state
    t->t_sigpending = 0;
    THREAD_SIGMASK_SET(t, 0);
    t->t_sig_in_progress = 0;
    
    int should_free = (t->detached || t->joined) && tid != 0 && !(t->joiner != NULL && t->joiner->magic == CTXT_MAGIC);
    TRACE_THREAD("EXIT: Thread %d detached=%d, joined=%d, has_joiner=%d, should_free=%d", 
                tid, t->detached, t->joined, (t->joiner != NULL), should_free);
    
    // Remove from thread list if detached or joined
    if (should_free) {
        struct thread **tp;
        for (tp = &p->threads; *tp; tp = &(*tp)->next) {
            if (*tp == t) {
                *tp = t->next;
                break;
            }
        }
    }
    TRACE_THREAD("EXIT: Removed thread %d from thread list", tid);
    
    // Clear current_thread pointer to prevent use after free
    if (p->current_thread == t) {
        p->current_thread = NULL;
    }
    TRACE_THREAD("EXIT: Cleared current_thread pointer for thread %d", tid);
    
    // Free resources if detached or joined and no joiner
    if (should_free) {
        if (t->stack && tid != 0) {
            TRACE_THREAD("EXIT: Freeing stack for thread %d", tid);
            kfree(t->stack);
            t->stack = NULL;
        }
        
        // Clear magic BEFORE freeing to prevent use after free
        t->magic = 0;
        
        kfree(t);
        TRACE_THREAD("EXIT: KFREE thread %d", tid);
    } else {
        TRACE_THREAD("EXIT: Thread %d not detached or joined or has joiner, keeping resources", tid);
    }
}

/**
 * Thread exit function
 * 
 * This function handles the termination of a thread, including:
 * - Handling thread joining
 * - Special handling for thread0
 * - Cancelling timeouts
 * - Removing from queues
 * - Finding the next thread to run
 * - Cleaning up resources
 * - Context switching
 * 
 * @param retval The return value of the exiting thread
 */
void proc_thread_exit(void *retval, void *arg) {
    struct proc *p = curproc;
    if (!p) {
        TRACE_THREAD("EXIT ERROR: No current process");
        return;
    }
    struct thread *current = NULL;
    if(!arg) {
        current = p->current_thread;
    } else {
        current = (struct thread *)arg;
        TRACE_THREAD("EXIT: Thread %d is exiting (CANCEL THREAD)", current->tid);
    }

    if (!current) {
        TRACE_THREAD("EXIT ERROR: No current thread");
        return;
    }

    // Check for pending signals before exiting
    if (current->t_sigpending) {
        int sig = check_thread_signals(current);
        if (sig && current->sig_handlers[sig].handler) {
            TRACE_THREAD("EXIT: Thread %d has pending signal %d, handling before exit", 
                        current->tid, sig);
            handle_thread_signal(current, sig);
        }
    }
    if (current->magic == CTXT_MAGIC) {
        TRACE_THREAD("EXIT: Running cleanup handlers for thread %d", current->tid);
        run_cleanup_handlers(current);  // Execute all cleanup handlers automatically
        TRACE_THREAD("EXIT: Running tsd destructors for thread %d", current->tid);
        run_tsd_destructors(current);  // User-space destructor handler
    }

    static int thread_exit_in_progress = 0;
    static int exit_owner_tid = -1;
    register unsigned short sr = splhigh();
    
    // Protection against reentrance
    if (thread_exit_in_progress && exit_owner_tid != current->tid) {
        spl(sr);
        TRACE_THREAD("EXIT: Thread exit already in progress by thread %d, waiting", exit_owner_tid);
        proc_thread_exit(retval, NULL); // Pass retval to recursive call
        return;
    }
    
    int tid = current->tid;
    thread_exit_in_progress = 1;
    exit_owner_tid = tid;
    
    if (retval == PTHREAD_CANCELED) {
        TRACE_THREAD("EXIT: Thread %d canceled", current->tid);
    }
    // Store the return value in the thread structure
    current->retval = retval;
    
    TRACE_THREAD("EXIT: Thread %d beginning exit process", tid);
    
    // Handle thread joining
    handle_thread_joining(current, retval);
    
    // Special handling for thread0 - wait for other threads to complete
    if (current->tid == 0 && p->num_threads > 1) {
        TRACE_THREAD("thread0 waiting for other threads to exit before exiting");
        // Wait until all other threads have exited
        while (p->num_threads > 1) {
            // Yield to other threads
            atomic_thread_state_change(current, THREAD_STATE_READY);
            add_to_ready_queue(current);
            proc_thread_schedule();
            spl(sr);
        }
        TRACE_THREAD("All other threads have exited, thread0 can now exit");
    }
    
    // Check if the thread is already exited or freed
    if (current->magic != CTXT_MAGIC || (current->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("WARNING: proc_thread_exit: Thread %d already exited or freed (magic=%lx, state=%d)",
                    tid, current->magic, current->state);
        thread_exit_in_progress = 0;
        exit_owner_tid = -1;
        spl(sr);
        return;
    }
    
    // Cancel timeouts associated with this thread
    cancel_thread_timeouts(p, current);

    // Remove from all queues
    remove_thread_from_wait_queues(current);
    remove_from_ready_queue(current);
    
    // Mark thread as exited
    atomic_thread_state_change(current, THREAD_STATE_EXITED);

    if(tid > 0) {
        p->num_threads--;
    }

    TRACE_THREAD("EXIT: Thread %d exited, num_threads=%d", tid, p->num_threads);
    
    // Handle timers
    if (p->num_threads == 1) {
        TRACE_THREAD("Only one thread remaining, stopping all timers");
        if (thread_switch_timeout) {
            canceltimeout(thread_switch_timeout);
            thread_switch_timeout = NULL;
        }
        if (p->p_thread_timer.enabled) {
            thread_timer_stop(p);
        }
    }
    
    // Find next thread to run
    struct thread *next_thread = find_next_thread_to_run(p);
    CONTEXT *target_ctx = NULL;
    if (next_thread)check_thread_cancellation(next_thread);
    // If we found a valid next thread, prepare to switch to it
    if (next_thread && next_thread->magic == CTXT_MAGIC && !(next_thread->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("EXIT: Will switch to thread %d", next_thread->tid);
        atomic_thread_state_change(next_thread, THREAD_STATE_RUNNING);
        p->current_thread = next_thread;
        target_ctx = get_thread_context(next_thread);
        
        if (!target_ctx) {
            TRACE_THREAD("EXIT ERROR: Could not get context for thread %d", next_thread->tid);
            next_thread = NULL;
            p->current_thread = NULL;
        }
    }
    
    // If no valid thread found, use process context
    if (!target_ctx) {
        TRACE_THREAD("EXIT: No next thread, returning to process context");
        target_ctx = &p->ctxt[CURRENT];
        p->current_thread = NULL;
    }
    
    // Clean up thread resources
    cleanup_thread_resources(p, current, tid);
    
    TRACE_THREAD("Thread %d exited", tid);
    
    thread_exit_in_progress = 0;
    exit_owner_tid = -1;
    
    // Switch to target context
    TRACE_THREAD("EXIT: Switching to target context, PC=%lx", target_ctx->pc);
    
    // CRITICAL FIX: Do NOT save context when exiting!
    // The exiting thread should never resume - just switch directly
    spl(sr);
    change_context(target_ctx);
    
    // Should NEVER reach here - if we do, it's a critical error
    TRACE_THREAD("CRITICAL ERROR: Returned from change_context after thread exit!");
    
    // Try to recover by scheduling another thread
    proc_thread_schedule();
}

/**
 * Helper function to determine if a thread should be scheduled
 * 
 * This function implements POSIX-compliant scheduling policies:
 * - SCHED_FIFO: First-in, first-out scheduling without time slicing
 * - SCHED_RR: Round-robin scheduling with time slicing
 * - SCHED_OTHER: Default time-sharing scheduling
 * 
 * @param current The currently running thread
 * @param next The candidate thread to be scheduled next
 * @return 1 if next should preempt current, 0 otherwise
 */
static int should_schedule_thread(struct thread *current, struct thread *next) {
    if (!next) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Invalid thread");
        return 0;
    }
    
    // If no current thread, always schedule next thread
    if (!current) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): No current thread, scheduling next thread %d", 
                    next->tid);
        return 1;
    }

    /* Special case: thread0 is always preemptible by other threads */
    if (current->tid == 0 && next->tid != 0) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): thread0 is current, allowing switch to thread %d", next->tid);
        return 1;
    }

    /* If current thread is not running, always schedule next thread */
    if ((current->state != THREAD_STATE_RUNNING) || (current->state & THREAD_STATE_BLOCKED)) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread %d is not running, scheduling next thread %d",
                    current->tid, next->tid);
        return 1;
    }

    /* Calculate elapsed time once */
    unsigned long elapsed = get_system_ticks() - current->last_scheduled;

    /* PRIORITY CHECK FIRST - Higher priority always preempts */
    if (next->priority > current->priority) {
        /* But respect minimum timeslice for non-boosted threads */
        if (!next->priority_boost && current->tid >= 0 && elapsed < next->proc->thread_min_timeslice && (current->state & THREAD_STATE_RUNNING)) {
            TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Higher priority thread %d waiting for min timeslice",
                        next->tid);
            return 0;
        }
        
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Higher priority thread %d (pri %d%s) preempting thread %d (pri %d)",
                    next->tid, next->priority, next->priority_boost ? " boosted" : "",
                    current->tid, current->priority);
        return 1;
    }

    /* Check minimum timeslice for equal/lower priority */
    if (elapsed < current->proc->thread_min_timeslice) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Current thread %d hasn't used minimum timeslice (%lu < %d)",
                    current->tid, elapsed, current->proc->thread_min_timeslice);
        return 0;
    }

    /* RT threads always preempt SCHED_OTHER threads (regardless of priority) */
    if ((next->policy == SCHED_FIFO || next->policy == SCHED_RR) &&
        next->priority > 0 && current->policy == SCHED_OTHER) {
        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): RT thread %d (pri %d) preempting SCHED_OTHER thread %d",
                    next->tid, next->priority, current->tid);
        return 1;
    }

    /* Equal priority handling */
    if (next->priority == current->priority) {
        /* SCHED_FIFO threads continue running until preempted by higher priority */
        if (current->policy == SCHED_FIFO) {
            TRACE_THREAD("THREAD_SCHED (should_schedule_thread): SCHED_FIFO thread %d continues (equal priority)",
                        current->tid);
            return 0;
        }

        /* SCHED_RR and SCHED_OTHER use timeslice */
        if (elapsed >= current->timeslice) {
            TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Thread %d timeslice expired (%lu >= %d), switching to %d",
                        current->tid, elapsed, current->timeslice, next->tid);
            return 1;
        }

        TRACE_THREAD("THREAD_SCHED (should_schedule_thread): Thread %d timeslice not expired (%lu < %d)",
                    current->tid, elapsed, current->timeslice);
        return 0;
    }

    /* Lower priority threads don't preempt higher priority ones */
    return 0;
}

void thread_switch(struct thread *from, struct thread *to) {
    struct thread_switch_context ctx = {0};
    
    // Fast validation first
    if (!to) {
        TRACE_THREAD("SWITCH: Invalid destination thread pointer");
        return;
    }
    
    // Special case: if from is NULL, just switch to the destination thread
    if (!from) {
        to->last_scheduled = get_system_ticks();
        
        TRACE_THREAD("SWITCH: Switching to thread %d (no source thread)", to->tid);
        register unsigned short sr = splhigh();
        change_context(get_thread_context(to));
        spl(sr);
        return;
    }
    
    if (from == to) {
        TRACE_THREAD("SWITCH: Source and destination threads are the same");
        return;
    }
    
    // Initialize context structure
    ctx.from = from;
    ctx.to = to;
    ctx.process = from->proc;
    ctx.switch_time = get_system_ticks();
    
    // PREPARATION PHASE - Outside critical section
    prepare_thread_switch(&ctx);
    
    // Execute the switch if preparation was successful
    if (ctx.to && ctx.to_ctx) {
        register unsigned short sr = splhigh();
        execute_thread_switch(&ctx);
        spl(sr);
    }
}

/* Add this new function to prepare thread switch outside critical section */
static void prepare_thread_switch(struct thread_switch_context *ctx) {
    TRACE_THREAD("SWITCH: Preparing switch from %d to %d", 
                ctx->from->tid, ctx->to->tid);
    
    // Check magic numbers and states in one go
    if (ctx->from->magic != CTXT_MAGIC || ctx->to->magic != CTXT_MAGIC ||
        (ctx->from->state & THREAD_STATE_EXITED) || (ctx->to->state & THREAD_STATE_EXITED)) {
        TRACE_THREAD("SWITCH: Invalid thread magic or exited state: from=%d, to=%d", 
                    ctx->from->tid, ctx->to->tid);
        ctx->to = NULL;
        return;
    }
    
    // Special handling for thread0
    if (ctx->from->tid == 0 && (ctx->from->state & THREAD_STATE_EXITED) && 
        ctx->from->proc->num_threads > 1) {
        TRACE_THREAD("SWITCH: Preventing thread0 exit while other threads running");
        atomic_thread_state_change(ctx->from, THREAD_STATE_READY);
        ctx->to = NULL;
        return;
    }
    
    // Verify stack integrity
    if (ctx->from->stack_magic != STACK_MAGIC || ctx->to->stack_magic != STACK_MAGIC) {
        TRACE_THREAD("SWITCH ERROR: Stack corruption detected!");
        ctx->to = NULL;
        return;
    }
    
    // Get destination context once
    ctx->to_ctx = get_thread_context(ctx->to);
    if (!ctx->to_ctx) {
        TRACE_THREAD("SWITCH: Failed to get context for thread %d", ctx->to->tid);
        ctx->to = NULL;
        return;
    }
    
    // Calculate if priority boost should be reset
    if (ctx->from->priority_boost && ctx->from->tid != 0) {
        unsigned long elapsed = ctx->switch_time - ctx->from->last_scheduled;
        ctx->should_reset_boost = (elapsed > ctx->from->proc->thread_min_timeslice || 
                                  ctx->from->wait_type != WAIT_NONE);
    }
}

/* Add this new function to execute thread switch in minimal critical section */
static void execute_thread_switch(struct thread_switch_context *ctx) {
    unsigned long now;
    // Check if another switch is in progress
    if (thread_switch_in_progress) {
        TRACE_THREAD("SWITCH: Another switch in progress, aborting");
        return;
    }
    
    // Set switch in progress and setup deadlock detection
    thread_switch_in_progress = 1;
    
    if (thread_switch_timeout) {
        canceltimeout(thread_switch_timeout);
    }
    thread_switch_timeout = addtimeout(ctx->from->proc, 
                                     ((ctx->from->proc->thread_default_timeslice * MS_PER_TICK) / 20), 
                                     thread_switch_timeout_handler);
    
    TRACE_THREAD("SWITCH: Switching threads: %d -> %d", ctx->from->tid, ctx->to->tid);
    
    // Reset priority boost if needed
    if (ctx->should_reset_boost) {
        TRACE_THREAD("SWITCH: Resetting priority boost for thread %d (current pri: %d, original: %d)",
                    ctx->from->tid, ctx->from->priority, ctx->from->original_priority);
        reset_thread_priority(ctx->from);
    } else if (ctx->from->priority_boost && ctx->from->tid != 0) {
        TRACE_THREAD("SWITCH: Keeping priority boost for thread %d",
                    ctx->from->tid);
    }
    // Update CPU time for outgoing thread
    now = get_system_ticks();

    if (ctx->from->last_scheduled == 0) {
        /*
         * First time this thread is switched out - it was never properly scheduled.
         * This happens for initial threads. Set last_scheduled to now to prevent
         * accounting the entire uptime, but don't add to CPU time.
         */
        ctx->from->last_scheduled = now;
        TRACE_THREAD("SWITCH: Initializing last_scheduled for thread %d", ctx->from->tid);
    }

    // Set new schedule time for incoming thread
    ctx->to->last_scheduled = now;
    TRACE_THREAD("SWITCH: Thread %d scheduled at %lu", ctx->to->tid, now);

    /* Check for pending signals in the thread we're switching to */
    if (ctx->to->proc->p_sigacts && ctx->to->proc->p_sigacts->thread_signals) {
        /* Skip thread0 - it handles process signals */
        /* Also skip threads that haven't run yet (last_scheduled == 0) */
        if (ctx->to->tid > 0 && ctx->to->last_scheduled > 0) {
            dispatch_thread_signals(ctx->to);
        }
    }
    
    // Handle context switch based on thread state
    if ((ctx->from->wait_type & WAIT_SLEEP) || (ctx->from->wait_type & WAIT_JOIN)) {
        TRACE_THREAD("SWITCH: Thread %d is sleeping, joining or waiting on semaphore, skipping switch", ctx->from->tid);
        // Sleeping thread path - direct context switch
        atomic_thread_state_change(ctx->to, THREAD_STATE_RUNNING);
        ctx->from->proc->current_thread = ctx->to;
        
        reset_thread_switch_state();
        TRACE_THREAD("SWITCH: Switched to context for thread %d", ctx->to->tid);
        change_context(ctx->to_ctx);
        
        TRACE_THREAD("SWITCH ERROR: Returned from change_context!");
    } 
    else if (save_context(get_thread_context(ctx->from)) == 0) {
        TRACE_THREAD("SWITCH: Saved context successfully for thread %d", ctx->from->tid);
        // Only change state if not blocked on mutex/semaphore
        if (ctx->from->wait_type == WAIT_NONE) {
            atomic_thread_state_change(ctx->from, THREAD_STATE_READY);
        }
        atomic_thread_state_change(ctx->to, THREAD_STATE_RUNNING);
        ctx->from->proc->current_thread = ctx->to;
        
        reset_thread_switch_state();
        TRACE_THREAD("SWITCH: Switched to context for thread %d", ctx->to->tid);
        change_context(ctx->to_ctx);
        
        TRACE_THREAD("SWITCH ERROR: Returned from change_context!");
    }
    TRACE_THREAD("SWITCH: Return path after being switched back");
    // Return path after being switched back
    reset_thread_switch_state();
}

/* Add these functions to optimize proc_thread_schedule */
static int prepare_scheduling_decision(struct proc *p, struct scheduling_decision *decision) {
    decision->current_thread = p->current_thread;
    decision->decision_time = get_system_ticks();
    
    if (!decision->current_thread || 
        decision->current_thread->last_scheduled + time_slice < decision->decision_time) {
        // Check and wake any sleeping threads
        check_and_wake_sleeping_threads(p);
    }

    // Get highest priority thread from ready queue
    decision->next_thread = get_highest_priority_thread(p);
    
    // Validate next thread
    if (decision->next_thread && (decision->next_thread->magic != CTXT_MAGIC || 
                                 (decision->next_thread->state & THREAD_STATE_EXITED))) {
        TRACE_THREAD("SCHED: Next thread %d is invalid or exited, removing from ready queue", 
                    decision->next_thread->tid);
        remove_from_ready_queue(decision->next_thread);
        decision->next_thread = NULL;
    }
    
    // Handle case where no next thread is found
    if (!decision->next_thread) {
        // Try to find thread0 or create idle thread
        struct thread *thread0 = get_main_thread(p);
        
        if (thread0 && thread0->state == THREAD_STATE_READY) {
            decision->next_thread = thread0;
            TRACE_THREAD("SCHED: Falling back to thread0");
        } else if (decision->current_thread && 
                  !(decision->current_thread->state & THREAD_STATE_BLOCKED)) {
            TRACE_THREAD("SCHED: Continuing with current thread %d", 
                        decision->current_thread->tid);
                        decision->current_thread->last_scheduled = decision->decision_time;
            return 0; // No switch needed
        } else if (p->sleep_queue) {
            // If there are sleeping threads, create an idle thread
            decision->next_thread = get_idle_thread(p);
            TRACE_THREAD("SCHED: Created idle thread to wait for sleeping threads");
        }
    }
    
    // If no next thread found, nothing to do
    if (!decision->next_thread) {
        TRACE_THREAD("SCHED: No threads available");
        return 0;
    }
    
    // If next is current, look for another thread
    if (decision->next_thread == decision->current_thread) {
        struct thread *alt_next = decision->next_thread->next_ready;
        while (alt_next && alt_next->state != THREAD_STATE_READY) {
            alt_next = alt_next->next_ready;
        }
        
        if (!alt_next) {
            TRACE_THREAD("SCHED: No other ready threads available");
            return 0;
        }
        
        decision->next_thread = alt_next;
    }
    
    // Check if we should schedule next thread
    decision->should_switch = should_schedule_thread(decision->current_thread, 
                                                   decision->next_thread);
    TRACE_THREAD("SCHED: Should switch: %d", decision->should_switch);
    return decision->should_switch;
}

static void execute_scheduling_decision(struct proc *p, struct scheduling_decision *decision) {
    if (!decision->should_switch || !decision->next_thread) {
        return;
    }
    
    TRACE_THREAD("SCHED: Executing switch from %d to %d", 
                decision->current_thread ? decision->current_thread->tid : -1, 
                decision->next_thread->tid);
    
    // Remove next from ready queue if it's there
    if (is_in_ready_queue(decision->next_thread)) {
        remove_from_ready_queue(decision->next_thread);
    }
    
    // Update thread states and prepare for switch
    if (decision->current_thread) {
        // Update timeslice accounting for current thread
        update_thread_timeslice(decision->current_thread);
        
        // Handle current thread based on its state
        if (decision->current_thread->state == THREAD_STATE_RUNNING) {
            if (decision->current_thread->wait_type != WAIT_NONE) {
                atomic_thread_state_change(decision->current_thread, THREAD_STATE_BLOCKED);
                
                // Priority inheritance for mutexes
                if ((decision->current_thread->wait_type & WAIT_MUTEX) && 
                    decision->current_thread->mutex_wait_obj) {
                    struct mutex *m = (struct mutex*)decision->current_thread->mutex_wait_obj;
                    if (m->owner && m->owner->priority < decision->current_thread->priority) {
                        boost_thread_priority(m->owner, 
                                            decision->current_thread->priority - m->owner->priority);
                        
                        // Reinsert owner in ready queue if needed
                        if (m->owner->state == THREAD_STATE_READY) {
                            remove_from_ready_queue(m->owner);
                            add_to_ready_queue(m->owner);
                        }
                    }
                }
            } else {
                // Thread is runnable but being preempted
                atomic_thread_state_change(decision->current_thread, THREAD_STATE_READY);
                add_to_ready_queue(decision->current_thread);
            }
        }
    }
    
    // Update next thread state
    atomic_thread_state_change(decision->next_thread, THREAD_STATE_RUNNING);
    p->current_thread = decision->next_thread;
    
    // Record scheduling time for timeslice accounting
    // decision->next_thread->last_scheduled = get_system_ticks();
    
    // Use the original thread_switch function for now
    // This ensures compatibility until optimized_thread_switch is fully tested
    thread_switch(decision->current_thread, decision->next_thread);
}


/**
 * Helper function to reschedule the preemption timer
 */
void reschedule_preemption_timer(PROC *p, long arg) {
    if (!p){
        TRACE_THREAD("SCHED_TIMER: Invalid process reference");
        return;
    }

    // Cancel existing timeout first
    if (p->p_thread_timer.timeout) {
        canceltimeout(p->p_thread_timer.timeout);
        p->p_thread_timer.timeout = NULL;
    }

    struct thread *t = (struct thread *)arg;
    // TRACE_THREAD("SCHED_TIMER: Rescheduling preemption timer for process %d, arg tid %d", p->pid, t->tid);
    p->p_thread_timer.timeout = addtimeout(p, p->thread_preempt_interval, thread_preempt_handler);
    p->p_thread_timer.in_handler = 0;
    if (p->p_thread_timer.timeout) {
        p->p_thread_timer.timeout->arg = (long)t;
    } else {
        TRACE_THREAD("SCHED_TIMER: Failed to reschedule preemption timer for process %d", p->pid);
    }
}

/*
 * Reset the thread switch state
 * Called when a thread switch completes or times out
 */
void reset_thread_switch_state(void) {

    register unsigned short sr = splhigh();
    
    thread_switch_in_progress = 0;
    
    if (thread_switch_timeout) {
        canceltimeout(thread_switch_timeout);
        thread_switch_timeout = NULL;
    }
    
    spl(sr);
}

/*
 * Thread switch timeout handler
 * Called when a thread switch takes too long
 */
void thread_switch_timeout_handler(PROC *p, long arg) {

    static int recovery_attempts = 0;
    
    TRACE_THREAD("TIMEOUT: Thread switch timed out after %dms", ((p->thread_default_timeslice * 5) / 20));
    
    if (++recovery_attempts > MAX_SWITCH_RETRIES) {
        TRACE_THREAD("CRITICAL: Max recovery attempts reached, system may be unstable");
        
        // More aggressive recovery - try to restore a known good state
        if (p && p->current_thread && p->current_thread->magic == CTXT_MAGIC) {
            TRACE_THREAD("TIMEOUT: Attempting to restore current thread %d", 
                        p->current_thread->tid);

            change_context(get_thread_context(p->current_thread));

        }
        
        recovery_attempts = 0;
    }
    
    /* Force reset of thread switch state */
    reset_thread_switch_state();
    
    /* Schedule next thread to try to recover */
    proc_thread_schedule();
}

/*
 * Start timing a specific process/thread
 */
void thread_timer_start(struct proc *p, int thread_id) {
    register unsigned short sr, retry_count = 0;
    
    TRACE_THREAD("TIMER: thread_timer_start called for process %d", p->pid);
    if (!p)
        return;

    /* Try to acquire the timer operation lock with a timeout */
    while (1) {
        sr = splhigh();
        if (!timer_operation_locked) {
            TRACE_THREAD("TIMER: Acquired timer operation lock");
            timer_operation_locked = 1;
            break;
        }
        spl(sr);
        
        /* If we've tried too many times, give up */
        if (++retry_count > 10) {
            TRACE_THREAD("TIMER WARNING: Failed to acquire timer operation lock after 10 retries");
            return;
        }
    }

    /* CRITICAL SECTION - We now have the timer operation lock */
    TRACE_THREAD("TIMER: Starting thread timer for process %d", p->pid);
    
    sr = splhigh();
    
    /* If timer is already enabled, don't add another timeout */
    if (p->p_thread_timer.enabled && p->p_thread_timer.timeout) {
        TRACE_THREAD("TIMER: Timer already enabled, not adding another timeout");
        spl(sr);
        goto cleanup;
    }
    
    /* Create the timeout before modifying any state */
    p->p_thread_timer.timeout = addtimeout(p, p->thread_preempt_interval, thread_preempt_handler);
    if (!(p->p_thread_timer.timeout)) {
        TRACE_THREAD("TIMER ERROR: Failed to create timeout");
        spl(sr);
        goto cleanup;
    }
    p->p_thread_timer.timeout->arg = (long)p->current_thread;

    /* Now that we have a valid timeout, update the timer state */
    p->p_thread_timer.thread_id = p->current_thread->tid;

    /* Set enabled flag last to ensure everything is set up */
    p->p_thread_timer.enabled = 1;
    p->p_thread_timer.in_handler = 0;
    
    TRACE_THREAD("TIMER: Thread timer started for process %d", p->pid);
    spl(sr);
    
cleanup:
    /* Always release the timer operation lock */
    sr = splhigh();
    timer_operation_locked = 0;
    spl(sr);
    
    TRACE_THREAD("TIMER: Timer timer operation lock released");
}

/*
 * Stop the thread timer
 */
void thread_timer_stop(PROC *p)
{
    register unsigned short sr;
    TIMEOUT *timeout_to_cancel = NULL;
    int retry_count = 0;

    if (!p) {
        return;
    }

    /* Try to acquire the timer operation lock with a timeout */
    while (1) {
        sr = splhigh();
        if (!timer_operation_locked) {
            timer_operation_locked = 1;
            spl(sr);
            break;
        }
        spl(sr);
        
        /* If we've tried too many times, give up */
        if (++retry_count > 10) {
            TRACE_THREAD("WARNING: Failed to acquire timer operation lock after 10 retries");
            return;
        }

    }

    /* CRITICAL SECTION - We now have the operation lock */
    TRACE_THREAD("Stopping thread timer for process %d", p->pid);
    
    sr = splhigh();
    
    /* Check if timer is already disabled */
    if (p->num_threads <= 1) {
        TRACE_THREAD("TIMER: Disabling timer for process %d (num_threads=%d)", p->pid, p->num_threads);
        p->p_thread_timer.enabled = 0;
    }
    
    /* Save the timeout pointer locally before clearing it */
    timeout_to_cancel = p->p_thread_timer.timeout;
    p->p_thread_timer.timeout = NULL;
    TRACE_THREAD("TIMER: Cleared timeout pointer");
    
    spl(sr);
    
    /* Cancel the timeout outside the critical section */
    if (timeout_to_cancel) {
        canceltimeout(timeout_to_cancel);
        TRACE_THREAD("TIMER: Cancelled timeout");
    }
    
    /* Always release the timer operation lock */
    sr = splhigh();
    timer_operation_locked = 0;
    spl(sr);
    
    TRACE_THREAD("TIMER: Timer operation lock released");
}