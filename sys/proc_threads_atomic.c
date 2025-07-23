/**
 * @file proc_threads_atomic.c
 * @brief Atomic Operations for FreeMiNT Threading System
 * 
 * Provides atomic operations for the threading subsystem.
 * These operations are implemented as kernel functions since they require
 * supervisor mode access to manipulate the status register.
 * 
 * @author Medour Mehdi
 * @date June 2025
 * @version 1.0
 */

#include "proc_threads.h"
#include "proc_threads_atomic.h"
#include "proc_threads_sleep_yield.h"
#include "mint/arch/asm_spl.h"

/* Atomic operations implementation */
#ifdef __mcoldfire__
/* ColdFire version - uses different instructions */
static inline void disable_interrupts(unsigned short *sr) {
    asm volatile ("move.w %%sr,%0" : "=d" (*sr));
    asm volatile ("move.w #0x2700,%%sr" : : : "memory");
}

static inline void restore_interrupts(unsigned short sr) {
    asm volatile ("move.w %0,%%sr" : : "d" (sr) : "memory");
}
#else
/* Original 68k version */
static inline void disable_interrupts(unsigned short *sr) {
    asm volatile ("move.w %%sr,%0" : "=d" (*sr));
    asm volatile ("ori.w #0x0700,%%sr" : : : "memory");
}

static inline void restore_interrupts(unsigned short sr) {
    asm volatile ("move.w %0,%%sr" : : "d" (sr) : "memory");
}
#endif

inline int atomic_increment(volatile int *value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = ++(*value);
    restore_interrupts(sr);
    return result;
}

inline int atomic_decrement(volatile int *value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = --(*value);
    restore_interrupts(sr);
    return result;
}

inline int atomic_cas(volatile int *ptr, int oldval, int newval) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    if (*ptr == oldval) {
        *ptr = newval;
        result = 1;  /* Success - old value matched */
    } else {
        result = 0;  /* Failure - old value didn't match */
    }
    restore_interrupts(sr);
    return result;
}

// /* Alternative implementation that returns the actual old value */
// int atomic_cas_with_old_value(volatile int *ptr, int oldval, int newval) {
//     unsigned short sr;
//     int old_value;
//     disable_interrupts(&sr);
//     old_value = *ptr;
//     if (old_value == oldval) {
//         *ptr = newval;
//     }
//     restore_interrupts(sr);
//     return old_value;  /* Return actual old value */
// }

inline int atomic_exchange(volatile int *ptr, int newval) {
    unsigned short sr;
    int oldval;
    disable_interrupts(&sr);
    oldval = *ptr;
    *ptr = newval;
    restore_interrupts(sr);
    return oldval;
}

inline int atomic_add(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) + value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

inline int atomic_sub(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) - value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

inline int atomic_or(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) | value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

inline int atomic_and(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) & value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

inline int atomic_xor(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) ^ value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

inline void atomic_counter_init(atomic_counter_t *counter, int initial_value) {
    counter->counter = initial_value;
}

inline int atomic_counter_inc(atomic_counter_t *counter) {
    return thread_atomic_increment(&counter->counter);
}

inline int atomic_counter_dec(atomic_counter_t *counter) {
    return thread_atomic_decrement(&counter->counter);
}

inline int atomic_counter_get(atomic_counter_t *counter) {
    return ATOMIC_GET(counter->counter);
}

/* Thread-safe linked list operations */
int thread_atomic_list_add(struct thread **head, struct thread *new_thread) {
    if (!head || !new_thread || new_thread->magic != CTXT_MAGIC) {
        return EINVAL;
    }
    
    unsigned short sr = splhigh();
    new_thread->next = *head;
    *head = new_thread;
    spl(sr);
    
    return 0;
}

int thread_atomic_list_remove(struct thread **head, struct thread *thread_to_remove) {
    if (!head || !thread_to_remove || thread_to_remove->magic != CTXT_MAGIC) {
        return EINVAL;
    }
    
    unsigned short sr = splhigh();
    struct thread *current = *head;
    struct thread *prev = NULL;
    
    while (current) {
        if (current == thread_to_remove) {
            if (prev) {
                prev->next = current->next;
            } else {
                *head = current->next;
            }
            current->next = NULL;
            spl(sr);
            return 0;
        }
        prev = current;
        current = current->next;
    }
    
    spl(sr);
    return ESRCH;  /* Thread not found in list */
}

inline void spinlock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->owner_tid = -1;
}

inline void spinlock_lock(spinlock_t *lock) {
    struct thread *t = CURTHREAD;
    int tid = t ? t->tid : -1;
    
    while (thread_atomic_test_and_set(&lock->locked)) {
        proc_thread_yield();
    }
    
    lock->owner_tid = tid;
    MEMORY_BARRIER();
}

inline int spinlock_trylock(spinlock_t *lock) {
    struct thread *t = CURTHREAD;
    int tid = t ? t->tid : -1;
    
    if (thread_atomic_test_and_set(&lock->locked) == 0) {
        lock->owner_tid = tid;
        MEMORY_BARRIER();
        return 1;  /* Success */
    }
    return 0;  /* Failed */
}

inline void spinlock_unlock(spinlock_t *lock) {
    MEMORY_BARRIER();
    lock->owner_tid = -1;
    thread_atomic_clear(&lock->locked);
}