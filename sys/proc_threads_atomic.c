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
#include "mint/arch/asm_spl.h"

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

static inline int atomic_increment(volatile int *value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = ++(*value);
    restore_interrupts(sr);
    return result;
}

static inline int atomic_decrement(volatile int *value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = --(*value);
    restore_interrupts(sr);
    return result;
}

static inline int atomic_cas(volatile int *ptr, int oldval, int newval) {
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
// static inline int atomic_cas_with_old_value(volatile int *ptr, int oldval, int newval) {
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

static inline int atomic_exchange(volatile int *ptr, int newval) {
    unsigned short sr;
    int oldval;
    disable_interrupts(&sr);
    oldval = *ptr;
    *ptr = newval;
    restore_interrupts(sr);
    return oldval;
}

static inline int atomic_add(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) + value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

static inline int atomic_sub(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) - value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

static inline int atomic_or(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) | value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

static inline int atomic_and(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) & value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
}

static inline int atomic_xor(volatile int *ptr, int value) {
    unsigned short sr;
    int result;
    disable_interrupts(&sr);
    result = (*ptr) ^ value;
    *ptr = result;
    restore_interrupts(sr);
    return result;
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
    
    TRACE_THREAD("sys_p_thread_atomic: op=%ld, ptr=%p, arg1=%ld, arg2=%ld", 
                operation, target, arg1, arg2);
    
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

/* Convenience functions for internal kernel use */
int thread_atomic_increment(volatile int *value) {
    return atomic_increment(value);
}

int thread_atomic_decrement(volatile int *value) {
    return atomic_decrement(value);
}

int thread_atomic_cas(volatile int *ptr, int oldval, int newval) {
    return atomic_cas(ptr, oldval, newval);
}

int thread_atomic_exchange(volatile int *ptr, int newval) {
    return atomic_exchange(ptr, newval);
}

/* Atomic operations for reference counting */
void thread_refcount_inc(volatile int *refcount) {
    atomic_increment(refcount);
}

int thread_refcount_dec(volatile int *refcount) {
    return atomic_decrement(refcount);
}

/* Atomic flag operations */
int thread_atomic_test_and_set(volatile int *flag) {
    return atomic_exchange(flag, 1);
}

void thread_atomic_clear(volatile int *flag) {
    atomic_exchange(flag, 0);
}

// /* Atomic state change for threads */
// void atomic_thread_state_change(struct thread *t, int new_state) {
//     if (!t || t->magic != CTXT_MAGIC) {
//         return;
//     }
    
//     unsigned short sr = splhigh();
//     // int old_state = t->state;
//     t->state = new_state;
//     spl(sr);
    
//     TRACE_THREAD("ATOMIC STATE: Thread %d changed from %d to %d", 
//                 t->tid, old_state, new_state);
// }

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