/**
 * @file proc_threads_syscall.c
 * @brief Kernel Thread System Call Dispatcher
 * 
 * Routes pthread-related system calls to appropriate kernel subsystems.
 * Handles synchronization, signaling, scheduling, and cleanup operations.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"

#include "proc_threads_policy.h"
#include "proc_threads_signal.h"
#include "proc_threads_sync.h"
#include "proc_threads_scheduler.h"
#include "proc_threads_signal.h"
#include "proc_threads_sleep_yield.h"
#include "proc_threads_helper.h"
#include "proc_threads_tsd.h"
#include "proc_threads_cleanup.h"
#include "proc_threads_atomic.h"
#include "proc_threads_queue.h"

#ifndef __SIZE_T
#define __SIZE_T
typedef unsigned long size_t;
#endif

/* Memory access helper for single-address-space systems */
#ifndef copyout
#define copyout(src, dst, len) \
    (memcpy((void*)(dst), (const void*)(src), (size_t)(len)), 0)
#endif

#ifndef copyin
#define copyin(src, dst, len) \
    (memcpy((void*)(dst), (const void*)(src), (size_t)(len)), 0)
#endif

long _cdecl sys_p_thread_ctrl(long mode, long arg1, long arg2) {
    switch (mode) {
        case THREAD_CTRL_EXIT: // Exit thread
            TRACE_THREAD("EXIT: sys_p_thread_ctrl called with exit mode");
            proc_thread_exit((void*)arg1, NULL);  // Use arg1 as the return value
            return 0;  // Should never reach here

        case THREAD_CTRL_SETCANCELSTATE: {
            struct thread *t = CURTHREAD;
            if (!t) return EINVAL;
            
            int new_state = (int)arg1;
            if (new_state != PTHREAD_CANCEL_ENABLE && 
                new_state != PTHREAD_CANCEL_DISABLE) {
                return EINVAL;
            }
            
            register unsigned short sr = splhigh();
            int old_state = t->cancel_state;
            t->cancel_state = new_state;
            
            // If oldstate pointer provided, store previous state
            if (arg2) {
                if (copyout(&old_state, (void*)arg2, sizeof(int))) {
                    spl(sr);
                    return EFAULT;
                }
            }
            
            spl(sr);
            return 0;
        }
        
        case THREAD_CTRL_SETCANCELTYPE: {
            struct thread *t = CURTHREAD;
            if (!t) return EINVAL;
            
            int new_type = (int)arg1;
            if (new_type != PTHREAD_CANCEL_DEFERRED && 
                new_type != PTHREAD_CANCEL_ASYNCHRONOUS) {
                return EINVAL;
            }
            
            register unsigned short sr = splhigh();
            int old_type = t->cancel_type;
            t->cancel_type = new_type;
            
            if (arg2) {
                if (copyout(&old_type, (void*)arg2, sizeof(int))) {
                    spl(sr);
                    return EFAULT;
                }
            }
            
            spl(sr);
            return 0;
        }
        
        case THREAD_CTRL_TESTCANCEL: {
            struct thread *t = CURTHREAD;
            if (!t) return EINVAL;
            
            register unsigned short sr = splhigh();
            int should_cancel = (t->cancel_state == PTHREAD_CANCEL_ENABLE && 
                                t->cancel_pending);
            spl(sr);
            
            if (should_cancel) {
                TRACE_THREAD("TESTCANCEL: Thread %d cancelling itself", t->tid);
                // Exit current thread (t = NULL means current thread exits itself)
                proc_thread_exit(PTHREAD_CANCELED, NULL);
            }
            return 0;
        }
        
        case THREAD_CTRL_CANCEL: {
            struct thread *target = get_thread_by_id(curproc, (short)arg2);
            if (!target) return ESRCH;
            
            register unsigned short sr = splhigh();
            target->cancel_pending = 1;
            
            // For ASYNCHRONOUS mode, send a signal to interrupt the target thread
            if (target->cancel_state == PTHREAD_CANCEL_ENABLE && 
                target->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) {
                
                // If target is sleeping/waiting, wake it up
                if (target->state == THREAD_STATE_BLOCKED) {
                    atomic_thread_state_change(target, THREAD_STATE_READY);
                    add_to_ready_queue(target);
                }
                
                // Set a flag that the scheduler will check
                target->cancel_requested = 1;
            }
            
            spl(sr);
            return 0;
        }

        case THREAD_CTRL_STATUS:
            // TRACE_THREAD("STATUS: proc_thread_status called for tid=%ld", arg1);
            // Get thread status
            return proc_thread_status(arg1);

        case THREAD_CTRL_GETID:
            return sys_p_thread_getid();

        case THREAD_CTRL_SETNAME: {
            short tid = (short)arg1;
            char *user_name = (char *)arg2;
            struct thread *target = get_thread_by_id(curproc, tid);
            if (!target) return ESRCH;

            char kname[16];
            if (copyin(user_name, kname, 16)) return EFAULT;
            kname[15] = '\0'; // Ensure null termination
            
            strcpy(target->name, kname);
            return 0;
        }
        
        case THREAD_CTRL_GETNAME: {
            short tid = (short)arg1;
            char *user_buf = (char *)arg2;
            struct thread *target = get_thread_by_id(curproc, tid);
            if (!target) return ESRCH;
            
            if (copyout(target->name, user_buf, 16)) return EFAULT;
            return 0;
        }

        case THREAD_CTRL_IS_INITIAL: {
            struct thread *t = CURTHREAD;
            if (!t) return 0; // Not a thread? Then not initial
            return (t->tid == 0) ? 1 : 0;
        }

        case THREAD_CTRL_IS_MULTITHREADED: {
            struct proc *p = curproc;
            if (!p || !p->threads) return 0;
            return (p->num_threads > 1) ? 1 : 0;
        }
        
        case THREAD_CTRL_SWITCH_TO_MAIN: {
            struct proc *p = curproc;
            if (!p) {
                return EINVAL;
            }

            struct thread *main_thread = get_main_thread(p);
            if (!main_thread || main_thread->magic != CTXT_MAGIC) {
                return ESRCH;
            }

            struct thread *current = p->current_thread;
            if (!current) {
                return EINVAL;
            }

            // Already on main thread
            if (current->tid == 0) {
                return 0;
            }

            // Check if main thread is runnable
            if (main_thread->state != THREAD_STATE_READY) {
                return EAGAIN;
            }

            register unsigned short sr = splhigh();

            // Prepare current thread for rescheduling
            atomic_thread_state_change(current, THREAD_STATE_READY);
            add_to_ready_queue(current);

            // Prepare main thread for execution
            atomic_thread_state_change(main_thread, THREAD_STATE_RUNNING);
            p->current_thread = main_thread;

            // Remove main thread from queues if present
            if (is_in_ready_queue(main_thread)) {
                remove_from_ready_queue(main_thread);
            }
            remove_thread_from_wait_queues(main_thread);

            // Perform context switch
            thread_switch(current, main_thread);

            spl(sr);
            return 0;
        }

        case THREAD_CTRL_SWITCH_TO_THREAD: {
            short target_tid = (short)arg1;
            struct proc *p = curproc;
            struct thread *current = p->current_thread;
            struct thread *target = NULL;

            // Find target thread
            register unsigned short sr = splhigh();
            for (struct thread *t = p->threads; t != NULL; t = t->next) {
                if (t->tid == target_tid && t->magic == CTXT_MAGIC && 
                    !(t->state & THREAD_STATE_EXITED)) {
                    target = t;
                    break;
                }
            }

            if (!target) {
                spl(sr);
                TRACE_THREAD("SWITCH_TO_THREAD: Thread %d not found", target_tid);
                return ESRCH;
            }

            // Validate thread state
            if (target->state != THREAD_STATE_READY || target == current) {
                spl(sr);
                TRACE_THREAD("SWITCH_TO_THREAD: Thread %d not ready or is current", target_tid);
                return EAGAIN;
            }

            // Remove from ready queue
            remove_from_ready_queue(target);

            // Update states
            if (current->wait_type == WAIT_NONE) {
                atomic_thread_state_change(current, THREAD_STATE_READY);
                add_to_ready_queue(current);
            }
            
            atomic_thread_state_change(target, THREAD_STATE_RUNNING);
            p->current_thread = target;
            target->last_scheduled = get_system_ticks();

            // Perform context switch
            TRACE_THREAD("SWITCH_TO_THREAD: Switching %d -> %d", 
                        current->tid, target->tid);
            thread_switch(current, target);
            
            spl(sr);
            return 0;
        }
        
        default:
            TRACE_THREAD("ERROR: sys_p_thread_ctrl called with invalid mode %d", mode);
            return EINVAL;
    }
}

long _cdecl sys_p_thread_signal(long func, long arg1, long arg2) {
    
    TRACE_THREAD("sys_p_thread_signal: func=%ld, arg1=%ld, arg2=%ld", func, arg1, arg2);
    
    switch (func) {
        case PTSIG_MODE:
            TRACE_THREAD("proc_thread_signal_mode: %s thread signals", 
                        arg1 ? "enabling" : "disabling");
            return proc_thread_signal_mode((int)arg1);
        case PTSIG_KILL:
            {        
            TRACE_THREAD("PTSIG_KILL: arg1=%ld, arg2=%ld", arg1, arg2);
            /* Send signal to specific thread */
            struct thread *target = NULL;
            struct proc *p = curproc;
            
            /* Find thread by ID */
            register unsigned short sr = splhigh();
            struct thread *t;
            for (t = p->threads; t != NULL; t = t->next) {
                if (t->tid == arg1) {
                    target = t;
                    break;
                }
            }
            spl(sr);
            
            if (!target) {
                TRACE_THREAD("PTSIG_KILL: Thread with ID %ld not found", arg1);
                return ESRCH;
            }
            
            /* Send signal to target thread */
            TRACE_THREAD("PTSIG_KILL: Sending signal %ld to thread %d", arg2, target->tid);
            return proc_thread_signal_kill(target, (int)arg2);
            }
        case PTSIG_GETMASK:
            return proc_thread_signal_sigmask(0);
            
        case PTSIG_SETMASK:
            return proc_thread_signal_sigmask(arg1);
            
        case PTSIG_BLOCK:
            return proc_thread_signal_sigblock((ulong)arg1);
            
        case PTSIG_UNBLOCK:
            {
                struct thread *t = CURTHREAD;
                if (!t) return EINVAL;
                ulong old_mask = t->t_sigmask;
                t->t_sigmask &= ~(arg1 & ~UNMASKABLE);
                return old_mask;
            }
            
        case PTSIG_WAIT:
            return proc_thread_signal_sigwait(arg1, arg2);
            
        case PTSIG_HANDLER:
            TRACE_THREAD("sys_p_thread_signal -> proc_thread_signal_sighandler: PROC ID %d, THREAD ID %d, SIG %ld, HANDLER %lx, ARG %p", 
                        curproc ? curproc->pid : -1, 
                        CURTHREAD ? CURTHREAD->tid : -1,
                        arg1, arg2, NULL);
            return proc_thread_signal_sighandler((int)arg1, (void (*)(int, void*))arg2, NULL);
            
        case PTSIG_HANDLER_ARG:
            TRACE_THREAD("sys_p_thread_signal -> proc_thread_signal_sighandler_arg: PROC ID %d, THREAD ID %d, SIG %ld, ARG %ld", 
                        curproc ? curproc->pid : -1, 
                        CURTHREAD ? CURTHREAD->tid : -1,
                        arg1, arg2);
            return proc_thread_signal_sighandler_arg((int)arg1, (void*)arg2);

        case PTSIG_PENDING:
           {
               struct thread *t = CURTHREAD;
               if (!t) return EINVAL;
               return t->t_sigpending;
           }            
        case PTSIG_ALARM:
            return proc_thread_signal_sigalrm(CURTHREAD, arg1);

        case PTSIG_BROADCAST:
            return proc_thread_signal_broadcast(arg1);

        default:
            if (func > 0 && func < NSIG) {
                /* Direct signal to thread */
                TRACE_THREAD("Sending signal %ld to thread ID %ld", func, arg1);
                struct thread *target = NULL;
                struct proc *p = curproc;
                
                if (arg1 == 0) {
                    /* Signal current thread */
                    target = CURTHREAD;
                    TRACE_THREAD("Using current thread (ID %d)", target ? target->tid : -1);
                } else {
                    /* Find thread by ID */
                    register unsigned short sr = splhigh();
                    struct thread *t;
                    for (t = p->threads; t != NULL; t = t->next) {
                        if (t->tid == arg1) {
                            target = t;
                            TRACE_THREAD("Found thread with ID %ld", arg1);
                            break;
                        }
                    }
                    spl(sr);
                    
                    if (!target) {
                        TRACE_THREAD("Thread with ID %ld not found", arg1);
                        return ESRCH;
                    }
                }
                
                /* Now deliver the signal to the target thread */
                if (target) {
                    TRACE_THREAD("Sending signal %ld to thread ID %ld", func, target->tid);
                    return proc_thread_signal_kill(target, (int)func);
                }
            }
            
            TRACE_THREAD("Invalid function code: %ld", func);
            return EINVAL;
    }
}

long _cdecl sys_p_thread_sync(long operator, long arg1, long arg2) {
    
    // TRACE_THREAD("sys_p_thread_sync(OP = %ld, arg1 = %ld, arg2= %ld)", operator, arg1, arg2);
    
    switch (operator) {
        case THREAD_SYNC_SEM_WAIT:
            TRACE_THREAD("THREAD_SYNC_SEM_WAIT");
            return thread_semaphore_down((struct semaphore *)arg1);
            
        case THREAD_SYNC_SEM_POST:
            TRACE_THREAD("THREAD_SYNC_SEM_POST");
            return thread_semaphore_up((struct semaphore *)arg1);
            
        case THREAD_SYNC_MUTEX_LOCK:
            TRACE_THREAD("THREAD_SYNC_MUTEX_LOCK");
            return thread_mutex_lock((struct mutex *)arg1);

        case THREAD_SYNC_MUTEX_TRYLOCK:
            TRACE_THREAD("THREAD_SYNC_MUTEX_TRYLOCK");
            return thread_mutex_trylock((struct mutex *)arg1);

        case THREAD_SYNC_MUTEX_UNLOCK:
            TRACE_THREAD("THREAD_SYNC_MUTEX_UNLOCK");
            return thread_mutex_unlock((struct mutex *)arg1);
            
        case THREAD_SYNC_MUTEX_INIT:
            TRACE_THREAD("THREAD_SYNC_MUTEX_INIT");
            return thread_mutex_init((struct mutex *)arg1, (const struct mutex_attr *)arg2);

            // Mutex attribute functions
        case THREAD_SYNC_MUTEX_DESTROY:
            TRACE_THREAD("THREAD_SYNC_MUTEX_DESTROY");
            return thread_mutex_destroy((struct mutex *)arg1);

        case THREAD_SYNC_MUTEX_ATTR_INIT:
            TRACE_THREAD("THREAD_SYNC_MUTEXATTR_INIT");
            return thread_mutexattr_init((struct mutex_attr *)arg1);

        case THREAD_SYNC_MUTEX_ATTR_DESTROY:
            TRACE_THREAD("THREAD_SYNC_MUTEXATTR_DESTROY");
            return thread_mutexattr_destroy((struct mutex_attr *)arg1);
            
        case THREAD_SYNC_MUTEXATTR_SETTYPE:
            {
                struct mutex_attr *attr = (struct mutex_attr *)arg1;
                int type = (int)arg2;

                if (!attr) {
                    TRACE_THREAD("SETTYPE: attr is NULL");
                    return EINVAL;
                }
                
                if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
                    TRACE_THREAD("SETTYPE: invalid type %d", type);
                    return EINVAL;
                }
                
                TRACE_THREAD("SETTYPE: attr=%p, setting type to %d", attr, type);
                
                attr->type = type;
                
                TRACE_THREAD("SETTYPE: attr->type is now %d", attr->type);
                
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_SETPROTOCOL:
            {
                struct mutex_attr *attr = (struct mutex_attr *)arg1;
                int protocol = (int)arg2;

                if (!attr) 
                    return EINVAL;

                if (protocol < PTHREAD_PRIO_NONE || protocol > PTHREAD_PRIO_PROTECT)
                    return EINVAL;
                TRACE_THREAD("THREAD_SYNC_MUTEXATTR_SETPROTOCOL: attr=%p, protocol=%d", attr, protocol);
                attr->protocol = protocol;
                TRACE_THREAD("THREAD_SYNC_MUTEXATTR_SETPROTOCOL: attr->protocol is now %d", attr->protocol);
                return THREAD_SUCCESS;
            }
            
        case THREAD_SYNC_MUTEXATTR_SETPRIOCEILING:
            {
                struct mutex_attr *attr = (struct mutex_attr *)arg1;
                int prioceiling = (int)arg2;
                if (!attr) 
                    return EINVAL;
                
                if (prioceiling < 0 || prioceiling > MAX_POSIX_THREAD_PRIORITY)
                    return EINVAL;
                
                attr->prioceiling = prioceiling;
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_GETTYPE:
            {
                struct mutex_attr *user_attr = (struct mutex_attr *)arg1;
                long *type = (long *)arg2;

                if (!user_attr || !type) return EINVAL;
                *type = user_attr->type;
                TRACE_THREAD("THREAD_SYNC_MUTEXATTR_GETTYPE: type=%d, type addr is %p", *type, type);
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_GETPRIOCEILING:
            {
                struct mutex_attr *user_attr = (struct mutex_attr *)arg1;
                long *prioceiling = (long *)arg2;

                if (!user_attr || !prioceiling) return EINVAL;
                *prioceiling = user_attr->prioceiling;
                TRACE_THREAD("THREAD_SYNC_MUTEXATTR_GETPRIOCEILING: prioceiling=%d, prioceiling addr is %p", *prioceiling, prioceiling);
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_MUTEXATTR_GETPROTOCOL:
            {
                struct mutex_attr *user_attr = (struct mutex_attr *)arg1;
                long *protocol = (long *)arg2;

                if (!user_attr || !protocol) return EINVAL;
                *protocol = user_attr->protocol;
                TRACE_THREAD("THREAD_SYNC_MUTEXATTR_GETPROTOCOL: protocol=%d, protocol addr is %p", *protocol, protocol);
                return THREAD_SUCCESS;
            }

        case THREAD_SYNC_SEM_INIT: {
            struct semaphore *sem = (struct semaphore *)arg1;
            TRACE_THREAD("THREAD_SYNC_SEM_INIT: count=%d", sem->count);
            return thread_semaphore_init(sem, sem->count);
        }            
        case THREAD_SYNC_JOIN: // Join thread
            TRACE_THREAD("JOIN: proc_thread_join called for tid=%ld", arg1);
            return proc_thread_join(arg1, (void**)arg2);
            
        case THREAD_SYNC_DETACH: // Detach thread
            TRACE_THREAD("DETACH: proc_thread_detach called for tid=%ld", arg1);
            return proc_thread_detach(arg1);

        case THREAD_SYNC_TRYJOIN:
            TRACE_THREAD("TRYJOIN: proc_thread_tryjoin called for tid=%ld", arg1);
            // New non-blocking join
            return proc_thread_tryjoin(arg1, (void **)arg2);

        case THREAD_SYNC_SLEEP:
            return proc_thread_sleep((long)arg1);

        case THREAD_SYNC_YIELD:
            return proc_thread_yield();

        case THREAD_SYNC_COND_INIT:
            TRACE_THREAD("THREAD_SYNC_COND_INIT");
            return proc_thread_condvar_init((struct condvar *)arg1);
            
        case THREAD_SYNC_COND_DESTROY:
            TRACE_THREAD("THREAD_SYNC_COND_DESTROY");
            return proc_thread_condvar_destroy((struct condvar *)arg1);
            
        case THREAD_SYNC_COND_WAIT:
            TRACE_THREAD("THREAD_SYNC_COND_WAIT");
            return proc_thread_condvar_wait((struct condvar *)arg1, (struct mutex *)arg2);
            
        case THREAD_SYNC_COND_TIMEDWAIT:
            TRACE_THREAD("THREAD_SYNC_COND_TIMEDWAIT");
            return proc_thread_condvar_timedwait((struct condvar *)arg1, (struct mutex *)arg2, 
                                          ((struct condvar *)arg1)->timeout_ms);
            
        case THREAD_SYNC_COND_SIGNAL:
            TRACE_THREAD("THREAD_SYNC_COND_SIGNAL");
            return proc_thread_condvar_signal((struct condvar *)arg1);
            
        case THREAD_SYNC_COND_BROADCAST:
            TRACE_THREAD("THREAD_SYNC_COND_BROADCAST");
            return proc_thread_condvar_broadcast((struct condvar *)arg1);

        case THREAD_SYNC_CLEANUP_PUSH:
            TRACE_THREAD("THREAD_SYNC_CLEANUP_PUSH: routine=%p, arg=%p", (void*)arg1, (void*)arg2);
            return thread_cleanup_push((void (*)(void*))arg1, (void*)arg2);
            
        case THREAD_SYNC_CLEANUP_POP:
            TRACE_THREAD("THREAD_SYNC_CLEANUP_POP: routine_ptr=%p, arg_ptr=%p", (void*)arg1, (void*)arg2);
            return thread_cleanup_pop((void (**)(void*))arg1, (void**)arg2);

        case THREAD_SYNC_CLEANUP_GET:
            TRACE_THREAD("THREAD_SYNC_CLEANUP_GET: handlers=%p, max_handlers=%ld", (void*)arg1, arg2);
            return get_cleanup_handlers(CURTHREAD, (struct cleanup_info*)arg1, (int)arg2);
        
        case THREAD_TSD_CREATE_KEY:
            return thread_key_create((void (*)(void*))arg1);
            
        case THREAD_TSD_DELETE_KEY:
            return thread_key_delete(arg1);
            
        case THREAD_TSD_GET_SPECIFIC:
            return (long)thread_getspecific(arg1);
            
        case THREAD_TSD_SET_SPECIFIC:
            return thread_setspecific(arg1, (void*)arg2);

        case THREAD_SYNC_RWLOCK_INIT:
            return thread_rwlock_init();

        case THREAD_SYNC_RWLOCK_DESTROY:
            return thread_rwlock_destroy(arg1);
            
        case THREAD_SYNC_RWLOCK_RDLOCK:
            return thread_rwlock_rdlock(arg1);
            
        case THREAD_SYNC_RWLOCK_TRYRDLOCK:
            return thread_rwlock_tryrdlock(arg1);
            
        case THREAD_SYNC_RWLOCK_WRLOCK:
            return thread_rwlock_wrlock(arg1);
            
        case THREAD_SYNC_RWLOCK_TRYWRLOCK:
            return thread_rwlock_trywrlock(arg1);
            
        case THREAD_SYNC_RWLOCK_UNLOCK:
            return thread_rwlock_unlock(arg1);
        
        default:
            TRACE_THREAD("THREAD_SYNC_UNKNOWN: %d", operator);
            return EINVAL;
    }
}

long _cdecl sys_p_thread_sched_policy(long func, long arg1, long arg2, long arg3) {
    
    TRACE_THREAD("IN KERNEL: sys_p_thread_sched_policy: func=%ld, arg1=%ld, arg2=%ld, arg3=%ld", 
                func, arg1, arg2, arg3);
    
    switch (func) {
        case PSCHED_SETPARAM:
            return proc_thread_set_schedparam(arg1, arg2, arg3);
            
        case PSCHED_GETPARAM:
            return proc_thread_get_schedparam(arg1, (long*)arg2, (long*)arg3);
            
        case PSCHED_GETRRINTERVAL:
            return proc_thread_get_rrinterval(arg1, (long*)arg2);
            
        case PSCHED_SET_TIMESLICE:
            return proc_thread_set_timeslice(arg1, arg2);
            
        case PSCHED_GET_TIMESLICE:
            return proc_thread_get_timeslice(arg1, (long*)arg2, (long*)arg3);
            
        default:
            return EINVAL;
    }
}

/**
 * Atomic operations system call dispatcher
 * 
 * @param operation - The atomic operation to perform
 * @param ptr - Pointer to the value to operate on
 * @param arg1 - First argument (value for most operations, oldval for CAS)
 * @param arg2 - Second argument (newval for CAS, unused for others)
 * @return Result of the atomic operation
 */
long _cdecl sys_p_thread_atomic(long operation, long ptr, long arg1, long arg2) {
    volatile int *target = (volatile int *)ptr;
    
    /* Validate pointer */
    if (!target) {
        return EINVAL;
    }
    
    switch (operation) {
        case THREAD_ATOMIC_INCREMENT:
            return atomic_increment(target);
            
        case THREAD_ATOMIC_DECREMENT:
            return atomic_decrement(target);
            
        case THREAD_ATOMIC_CAS:
            return atomic_cas(target, (int)arg1, (int)arg2);
            
        case THREAD_ATOMIC_EXCHANGE:
            return atomic_exchange(target, (int)arg1);
            
        case THREAD_ATOMIC_ADD:
            return atomic_add(target, (int)arg1);
            
        case THREAD_ATOMIC_SUB:
            return atomic_sub(target, (int)arg1);
            
        case THREAD_ATOMIC_OR:
            return atomic_or(target, (int)arg1);
            
        case THREAD_ATOMIC_AND:
            return atomic_and(target, (int)arg1);
            
        case THREAD_ATOMIC_XOR:
            return atomic_xor(target, (int)arg1);
            
        default:
            return EINVAL;
    }
}