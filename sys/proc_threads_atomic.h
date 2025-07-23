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

/* Thread-safe linked list operations */
int thread_atomic_list_add(struct thread **head, struct thread *new_thread);
int thread_atomic_list_remove(struct thread **head, struct thread *thread_to_remove);

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

inline void spinlock_init(spinlock_t *lock);
inline void spinlock_lock(spinlock_t *lock);
inline void spinlock_unlock(spinlock_t *lock);
inline int spinlock_trylock(spinlock_t *lock);

/* Atomic counter for generating unique IDs */
typedef struct {
    volatile int counter;
} atomic_counter_t;


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

/* Atomic counter operations */
inline void atomic_counter_init(atomic_counter_t *counter, int initial_value);
inline int atomic_counter_inc(atomic_counter_t *counter);
inline int atomic_counter_dec(atomic_counter_t *counter);
inline int atomic_counter_get(atomic_counter_t *counter);
/* Atomic operations */
inline int atomic_xor(volatile int *ptr, int value);
inline int atomic_and(volatile int *ptr, int value);
inline int atomic_or(volatile int *ptr, int value);
inline int atomic_sub(volatile int *ptr, int value);
inline int atomic_add(volatile int *ptr, int value);
inline int atomic_exchange(volatile int *ptr, int newval);
inline int atomic_cas(volatile int *ptr, int oldval, int newval);
inline int atomic_decrement(volatile int *value);
inline int atomic_increment(volatile int *value);

/* Convenience functions for internal kernel use */
/* Kernel-internal atomic operations */
#define thread_atomic_increment(x) atomic_increment(x)
#define thread_atomic_decrement(x) atomic_decrement(x)
#define thread_atomic_cas(x, y, z) atomic_cas(x, y, z)
#define thread_atomic_exchange(x, y) atomic_exchange(x, y)
/* Reference counting operations */
#define thread_refcount_inc(x) atomic_increment(x)
#define thread_refcount_dec(x) atomic_decrement(x)
/* Flag operations */
#define thread_atomic_test_and_set(x)   atomic_exchange(x, 1)
#define thread_atomic_clear(x)          atomic_exchange(x, 0)

/* Macros for common atomic operations */
#define ATOMIC_INC(var) thread_atomic_increment(&(var))
#define ATOMIC_DEC(var) thread_atomic_decrement(&(var))
#define ATOMIC_CAS(ptr, old, new) thread_atomic_cas(&(ptr), (old), (new))
#define ATOMIC_SET(var, val) thread_atomic_exchange(&(var), (val))
#define ATOMIC_GET(var) (*(volatile int*)&(var))

#endif /* PROC_THREADS_ATOMIC_H */