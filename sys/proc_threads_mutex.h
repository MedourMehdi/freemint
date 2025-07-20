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

#ifndef PROC_THREADS_MUTEX_H
#define PROC_THREADS_MUTEX_H

/* Adaptive spinning configuration */
#define DEFAULT_SPIN_COUNT 50
#define MAX_SPIN_COUNT 1000
#define MIN_SPIN_COUNT 10
#define SPIN_BACKOFF_FACTOR 2
#define CONTENTION_THRESHOLD 5

/* Mutex attribute structure */
struct mutex_attr {
    int type;                 /* Mutex type: NORMAL, RECURSIVE, ERRORCHECK */
    int pshared;              /* Process-shared flag */
    int protocol;             /* Priority protocol */
    int prioceiling;          /* Priority ceiling value */
};

/* Mutex types */
#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_ERRORCHECK  2

/* Priority protocols */
#define PTHREAD_PRIO_NONE         0
#define PTHREAD_PRIO_INHERIT      1
#define PTHREAD_PRIO_PROTECT      2

struct mutex {
    struct thread *owner;
    struct thread *wait_queue;    
    int locked;
    int protocol;              /* Priority protocol */
    int type;                  /* Mutex type */
    int prioceiling;           /* Priority ceiling */
    int saved_priority;        /* Saved priority for ceiling protocol */
    int lock_count;            /* Lock count for recursive mutexes */
};

int thread_mutex_trylock(struct mutex *mutex);
// Function to unlock a mutex
int thread_mutex_unlock(struct mutex *mutex);
// Function to lock a mutex
int thread_mutex_lock(struct mutex *mutex);
// Function to initialize a mutex
int thread_mutex_init(struct mutex *mutex, const struct mutex_attr *attr);
// Function to destroy a mutex
int thread_mutex_destroy(struct mutex *mutex);
// Function to initialize mutex attributes
int thread_mutexattr_init(struct mutex_attr *attr);
// Function to destroy mutex attributes
int thread_mutexattr_destroy(struct mutex_attr *attr);

#endif /* PROC_THREADS_MUTEX_H */