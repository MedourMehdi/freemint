/**
 * @file proc_threads_sync.h
 * @brief Thread Synchronization Primitives Interface
 * 
 * Declares POSIX synchronization mechanisms and thread lifecycle operations.
 * Defines structures and APIs for mutexes, condition variables, semaphores,
 * and thread joining/detaching operations.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"

#ifndef PROC_THREADS_SYNC_H
#define PROC_THREADS_SYNC_H

struct semaphore {
    volatile unsigned short count;
    struct thread *wait_queue;
};

struct mutex {
    volatile short locked;
    struct thread *owner;
    struct thread *wait_queue;
};

#define CONDVAR_MAGIC 0xC0DEC0DE

struct condvar {
    struct thread *wait_queue;      /* Queue of threads waiting on this condvar */
    struct mutex *associated_mutex; /* Mutex associated with this condvar */
    unsigned long magic;            /* Magic number for validation */
    int destroyed;                  /* Flag indicating if condvar is destroyed */
    long timeout_ms;                /* Timeout value in milliseconds */
};

/* Read-Write Lock Structure */
struct rwlock {
    struct mutex lock;          // Mutex protecting internal state
    struct condvar readers_ok;  // Readers condition variable
    struct condvar writers_ok;  // Writers condition variable
    int readers;                // Active readers count
    int writers;                // Active writers (0 or 1)
    int waiting_writers;        // Writers waiting for access
    int waiting_readers;        // Readers waiting for access
};

long proc_thread_join(long tid, void **retval);
long proc_thread_tryjoin(long tid, void **retval);
long proc_thread_detach(long tid);

/* Function to clean up thread synchronization states */
void cleanup_thread_sync_states(struct proc *p);

// Function to unlock a mutex
int thread_mutex_unlock(struct mutex *mutex);
// Function to lock a mutex
int thread_mutex_lock(struct mutex *mutex);
// Function to initialize a mutex
int thread_mutex_init(struct mutex *mutex);
// Function to destroy a mutex
int thread_mutex_destroy(struct mutex *mutex);

// Function to up a semaphore
int thread_semaphore_up(struct semaphore *sem);
// Function to down a semaphore
int thread_semaphore_down(struct semaphore *sem);
// Function to initialize a semaphore
int thread_semaphore_init(struct semaphore *sem, short count);

long thread_rwlock_init(void);
long thread_rwlock_destroy(long handle);
long thread_rwlock_rdlock(long handle);
long thread_rwlock_tryrdlock(long handle);
long thread_rwlock_wrlock(long handle);
long thread_rwlock_trywrlock(long handle);
long thread_rwlock_unlock(long handle);

/* Condition variable functions */
int proc_thread_condvar_init(struct condvar *cond);
int proc_thread_condvar_destroy(struct condvar *cond);
int proc_thread_condvar_wait(struct condvar *cond, struct mutex *mutex);
int proc_thread_condvar_timedwait(struct condvar *cond, struct mutex *mutex, long timeout_ms);
int proc_thread_condvar_signal(struct condvar *cond);
int proc_thread_condvar_broadcast(struct condvar *cond);

#endif /* PROC_THREADS_SYNC_H */