#include "proc_threads.h"
#include "proc_threads_mutex.h"

#ifndef PROC_THREADS_COND_H
#define PROC_THREADS_COND_H

#define CONDVAR_MAGIC 0xC0DEC0DE

struct condvar {
    struct thread *wait_queue;      /* Queue of threads waiting on this condvar */
    struct mutex *associated_mutex; /* Mutex associated with this condvar */
    unsigned long magic;            /* Magic number for validation */
    int destroyed;                  /* Flag indicating if condvar is destroyed */
    long timeout_ms;                /* Timeout value in milliseconds */
};

/* Condition variable functions */
int proc_thread_condvar_init(struct condvar *cond);
int proc_thread_condvar_destroy(struct condvar *cond);
int proc_thread_condvar_wait(struct condvar *cond, struct mutex *mutex);
int proc_thread_condvar_timedwait(struct condvar *cond, struct mutex *mutex, long timeout_ms);
int proc_thread_condvar_signal(struct condvar *cond);
int proc_thread_condvar_broadcast(struct condvar *cond);

#endif /* PROC_THREADS_COND_H */