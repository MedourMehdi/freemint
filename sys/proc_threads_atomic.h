/**
 * @file proc_threads_atomic.h
 * @brief Atomic Operations Header for FreeMiNT Threading System
 * 
 * Provides atomic operations for the threading subsystem.
 * These operations are implemented as kernel functions since they require
 * supervisor mode access to manipulate the status register.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#ifndef PROC_THREADS_ATOMIC_H
#define PROC_THREADS_ATOMIC_H

#include "proc_threads.h"

/* Atomic operations syscall constants */
#define THREAD_ATOMIC_INCREMENT     1
#define THREAD_ATOMIC_DECREMENT     2
#define THREAD_ATOMIC_CAS           3   /* Compare and swap */
#define THREAD_ATOMIC_EXCHANGE      4   /* Atomic exchange */
#define THREAD_ATOMIC_ADD           5   /* Atomic add */
#define THREAD_ATOMIC_SUB           6   /* Atomic subtract */
#define THREAD_ATOMIC_OR            7   /* Atomic bitwise OR */
#define THREAD_ATOMIC_AND           8   /* Atomic bitwise AND */
#define THREAD_ATOMIC_XOR           9   /* Atomic bitwise XOR */

/* Kernel-internal atomic operations */
int thread_atomic_increment(volatile int *value);
int thread_atomic_decrement(volatile int *value);
int thread_atomic_cas(volatile int *ptr, int oldval, int newval);
int thread_atomic_exchange(volatile int *ptr, int newval);

/* Reference counting operations */
void thread_refcount_inc(volatile int *refcount);
int thread_refcount_dec(volatile int *refcount);

/* Flag operations */
int thread_atomic_test_and_set(volatile int *flag);
void thread_atomic_clear(volatile int *flag);

// /* Thread state management */
// void atomic_thread_state_change(struct thread *t, int new_state);

/* Thread-safe linked list operations */
int thread_atomic_list_add(struct thread **head, struct thread *new_thread);
int thread_atomic_list_remove(struct thread **head, struct thread *thread_to_remove);

/* Macros for common atomic operations */
#define ATOMIC_INC(var) thread_atomic_increment(&(var))
#define ATOMIC_DEC(var) thread_atomic_decrement(&(var))
#define ATOMIC_CAS(ptr, old, new) thread_atomic_cas(&(ptr), (old), (new))
#define ATOMIC_SET(var, val) thread_atomic_exchange(&(var), (val))
#define ATOMIC_GET(var) (*(volatile int*)&(var))

/* Memory barriers for 68k */
#ifdef __mcoldfire__
#define MEMORY_BARRIER() asm volatile("" : : : "memory")
#else
#define MEMORY_BARRIER() asm volatile("" : : : "memory")
#endif

/* Spinlock implementation using atomic operations */
typedef struct {
    volatile int locked;
    int owner_tid;  /* For debugging */
} spinlock_t;

static inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->owner_tid = -1;
}

static inline void spinlock_lock(spinlock_t *lock) {
    struct thread *t = CURTHREAD;
    int tid = t ? t->tid : -1;
    
    while (thread_atomic_test_and_set(&lock->locked)) {
        /* Spin - could add yield here for better performance */
        asm volatile("nop");
    }
    
    lock->owner_tid = tid;
    MEMORY_BARRIER();
}

static inline int spinlock_trylock(spinlock_t *lock) {
    struct thread *t = CURTHREAD;
    int tid = t ? t->tid : -1;
    
    if (thread_atomic_test_and_set(&lock->locked) == 0) {
        lock->owner_tid = tid;
        MEMORY_BARRIER();
        return 1;  /* Success */
    }
    return 0;  /* Failed */
}

static inline void spinlock_unlock(spinlock_t *lock) {
    MEMORY_BARRIER();
    lock->owner_tid = -1;
    thread_atomic_clear(&lock->locked);
}

/* Atomic counter for generating unique IDs */
typedef struct {
    volatile int counter;
} atomic_counter_t;

static inline void atomic_counter_init(atomic_counter_t *counter, int initial_value) {
    counter->counter = initial_value;
}

static inline int atomic_counter_inc(atomic_counter_t *counter) {
    return thread_atomic_increment(&counter->counter);
}

static inline int atomic_counter_dec(atomic_counter_t *counter) {
    return thread_atomic_decrement(&counter->counter);
}

static inline int atomic_counter_get(atomic_counter_t *counter) {
    return ATOMIC_GET(counter->counter);
}

/* Wait-free queue operations for single producer/consumer */
typedef struct queue_node {
    volatile struct queue_node *next;
    void *data;
} queue_node_t;

typedef struct {
    volatile queue_node_t *head;
    volatile queue_node_t *tail;
    spinlock_t lock;  /* For multi-producer/consumer scenarios */
} atomic_queue_t;

/* Function declarations for atomic queue operations */
void atomic_queue_init(atomic_queue_t *queue);
int atomic_queue_enqueue(atomic_queue_t *queue, void *data);
void* atomic_queue_dequeue(atomic_queue_t *queue);
int atomic_queue_is_empty(atomic_queue_t *queue);

#endif /* PROC_THREADS_ATOMIC_H */