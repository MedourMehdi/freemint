#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>

/* Structure for shared spinlock - in shared memory */
typedef struct {
    volatile int lock;
    volatile int refcount;
    volatile int initialized;
} pthread_spin_shm_t;

/* Local structure to track shared memory handles */
typedef struct {
    pthread_spin_shm_t *shm_ptr;  /* Fixed: was pthread_spin_local_t */
    long file_handle;
    char *shm_path;
    int is_creator;
} pthread_spin_local_t;

/* Structure for private spinlock */
typedef struct {
    volatile int lock;
    int magic;  // Magic number to identify private locks
} pthread_spin_private_t;

/* Test data structure for shared counter */
typedef struct {
    pthread_spinlock_t lock;
    volatile int counter;
    int iterations;
} test_data_t;

/* Global test data */
test_data_t test_data;

/* Thread function for testing spinlock */
void *thread_test_function(void *arg) {
    test_data_t *data = (test_data_t *)arg;
    int i;
    
    printf("Thread %ld starting, will do %d iterations\n", 
           (long)pthread_self(), data->iterations);
    
    for (i = 0; i < data->iterations; i++) {
        // Lock the spinlock
        if (pthread_spin_lock(&data->lock) != 0) {
            printf("Thread %ld: Failed to lock spinlock\n", (long)pthread_self());
            return NULL;
        }
        
        // Critical section - increment counter
        int old_val = data->counter;
        // Simulate some work
        usleep(1);  // 1 millisecond delay
        data->counter = old_val + 1;
        
        // Unlock the spinlock
        if (pthread_spin_unlock(&data->lock) != 0) {
            printf("Thread %ld: Failed to unlock spinlock\n", (long)pthread_self());
            return NULL;
        }
        
        // Give other threads a chance
        if (i % 100 == 0) {
            pthread_yield();
        }
    }
    
    printf("Thread %ld completed %d iterations\n", 
           (long)pthread_self(), data->iterations);
    return NULL;
}

/* Test basic spinlock operations */
int test_basic_operations(void) {
    pthread_spinlock_t lock;
    int result;
    
    printf("\n=== Testing Basic Spinlock Operations ===\n");
    
    // Test initialization
    result = pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
    if (result != 0) {
        printf("FAIL: pthread_spin_init returned %d (%s)\n", result, strerror(result));
        return -1;
    }
    printf("PASS: pthread_spin_init\n");
    
    // Test lock
    result = pthread_spin_lock(&lock);
    if (result != 0) {
        printf("FAIL: pthread_spin_lock returned %d (%s)\n", result, strerror(result));
        return -1;
    }
    printf("PASS: pthread_spin_lock\n");
    
    // Test trylock (should fail since already locked)
    result = pthread_spin_trylock(&lock);
    if (result != EBUSY) {
        printf("FAIL: pthread_spin_trylock should return EBUSY, got %d\n", result);
        return -1;
    }
    printf("PASS: pthread_spin_trylock (correctly returned EBUSY)\n");
    
    // Test unlock
    result = pthread_spin_unlock(&lock);
    if (result != 0) {
        printf("FAIL: pthread_spin_unlock returned %d (%s)\n", result, strerror(result));
        return -1;
    }
    printf("PASS: pthread_spin_unlock\n");
    
    // Test trylock after unlock (should succeed)
    result = pthread_spin_trylock(&lock);
    if (result != 0) {
        printf("FAIL: pthread_spin_trylock after unlock returned %d (%s)\n", result, strerror(result));
        return -1;
    }
    printf("PASS: pthread_spin_trylock (after unlock)\n");
    
    // Unlock again
    pthread_spin_unlock(&lock);
    
    // Test destroy
    result = pthread_spin_destroy(&lock);
    if (result != 0) {
        printf("FAIL: pthread_spin_destroy returned %d (%s)\n", result, strerror(result));
        return -1;
    }
    printf("PASS: pthread_spin_destroy\n");
    
    return 0;
}

/* Test multithreaded spinlock */
int test_multithreaded(void) {
    pthread_t threads[4];
    int i, result;
    const int iterations_per_thread = 1000;
    
    printf("\n=== Testing Multithreaded Spinlock ===\n");
    
    // Initialize test data
    test_data.counter = 0;
    test_data.iterations = iterations_per_thread;
    
    result = pthread_spin_init(&test_data.lock, PTHREAD_PROCESS_PRIVATE);
    if (result != 0) {
        printf("FAIL: pthread_spin_init returned %d\n", result);
        return -1;
    }
    
    printf("Creating 4 threads, each doing %d iterations\n", iterations_per_thread);
    
    // Create threads
    for (i = 0; i < 4; i++) {
        result = pthread_create(&threads[i], NULL, thread_test_function, &test_data);
        if (result != 0) {
            printf("FAIL: pthread_create for thread %d returned %d\n", i, result);
            return -1;
        }
    }
    
    // Wait for all threads to complete
    for (i = 0; i < 4; i++) {
        result = pthread_join(threads[i], NULL);
        if (result != 0) {
            printf("FAIL: pthread_join for thread %d returned %d\n", i, result);
            return -1;
        }
    }
    
    printf("All threads completed\n");
    printf("Expected counter value: %d\n", 4 * iterations_per_thread);
    printf("Actual counter value: %d\n", test_data.counter);
    
    if (test_data.counter == 4 * iterations_per_thread) {
        printf("PASS: Counter value is correct (no race conditions)\n");
    } else {
        printf("FAIL: Counter value is incorrect (race condition detected)\n");
        return -1;
    }
    
    pthread_spin_destroy(&test_data.lock);
    return 0;
}

/* Test shared spinlock between processes */
/* Test shared spinlock between processes */
int test_shared_spinlock(void) {
    pthread_spinlock_t shared_lock;
    pid_t pid;
    int result;
    int status;
    
    printf("\n=== Testing Shared Spinlock (Process-Shared) ===\n");
    
    // Initialize shared spinlock
    result = pthread_spin_init(&shared_lock, PTHREAD_PROCESS_SHARED);
    if (result != 0) {
        printf("FAIL: pthread_spin_init (shared) returned %d (%s)\n", result, strerror(result));
        return -1;
    }
    printf("PASS: pthread_spin_init (shared)\n");
    
    // Test basic lock/unlock in single process
    result = pthread_spin_lock(&shared_lock);
    if (result != 0) {
        printf("FAIL: pthread_spin_lock (shared) returned %d\n", result);
        return -1;
    }
    printf("PASS: pthread_spin_lock (shared)\n");
    
    result = pthread_spin_unlock(&shared_lock);
    if (result != 0) {
        printf("FAIL: pthread_spin_unlock (shared) returned %d\n", result);
        return -1;
    }
    printf("PASS: pthread_spin_unlock (shared)\n");
    
    // Get the shared memory path from the parent's lock structure
    pthread_spin_local_t *parent_local = (pthread_spin_local_t *)shared_lock;
    if (!parent_local || !parent_local->shm_path) {
        printf("FAIL: Could not get shared memory path from parent lock\n");
        return -1;
    }
    
    // Fork a child process to test inter-process synchronization
    pid = fork();
    if (pid == 0) {
        // Child process
        pthread_spinlock_t child_lock;
        printf("Child process started, attempting to attach to shared spinlock...\n");
        // Attach to the shared spinlock using the path
        result = pthread_spin_attach(&child_lock, parent_local->shm_path);
        if (result != 0) {
            printf("CHILD FAIL: pthread_spin_attach returned %d\n", result);
            exit(1);
        }
        printf("CHILD PASS: pthread_spin_attach\n");
        
        // Try to lock (parent should be holding it)
        printf("Child attempting to lock...\n");
        result = pthread_spin_lock(&child_lock);
        if (result != 0) {
            printf("CHILD FAIL: pthread_spin_lock returned %d\n", result);
            exit(1);
        }
        printf("CHILD PASS: Got lock\n");
        
        // Hold lock briefly
        sleep(1);
        
        // Unlock
        result = pthread_spin_unlock(&child_lock);
        if (result != 0) {
            printf("CHILD FAIL: pthread_spin_unlock returned %d\n", result);
            exit(1);
        }
        printf("CHILD PASS: Released lock\n");
        
        // Destroy our reference
        pthread_spin_destroy(&child_lock);
        exit(0);
    } else if (pid > 0) {
        // Parent process
        printf("Parent locking shared spinlock...\n");
        pthread_spin_lock(&shared_lock);
        
        // Hold lock for a short time
        sleep(2);
        
        printf("Parent releasing shared spinlock...\n");
        pthread_spin_unlock(&shared_lock);
        
        // Wait for child
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("PASS: Child process completed successfully\n");
        } else {
            printf("FAIL: Child process failed\n");
            return -1;
        }
    } else {
        printf("FAIL: fork() failed\n");
        return -1;
    }
    
    // Clean up
    pthread_spin_destroy(&shared_lock);
    printf("PASS: pthread_spin_destroy (shared)\n");
    
    return 0;
}

/* Test error conditions */
int test_error_conditions(void) {
    pthread_spinlock_t lock;
    int result;
    
    printf("\n=== Testing Error Conditions ===\n");
    
    // Test with NULL pointer
    result = pthread_spin_init(NULL, PTHREAD_PROCESS_PRIVATE);
    if (result != EINVAL) {
        printf("FAIL: pthread_spin_init(NULL) should return EINVAL, got %d\n", result);
        return -1;
    }
    printf("PASS: pthread_spin_init(NULL) returns EINVAL\n");
    
    // Test with invalid pshared value
    result = pthread_spin_init(&lock, 999);
    if (result != EINVAL) {
        printf("FAIL: pthread_spin_init with invalid pshared should return EINVAL, got %d\n", result);
        return -1;
    }
    printf("PASS: pthread_spin_init with invalid pshared returns EINVAL\n");
    
    // Test operations on uninitialized lock
    result = pthread_spin_lock(NULL);
    if (result != EINVAL) {
        printf("FAIL: pthread_spin_lock(NULL) should return EINVAL, got %d\n", result);
        return -1;
    }
    printf("PASS: pthread_spin_lock(NULL) returns EINVAL\n");
    
    return 0;
}

/* Main test function */
int main(void) {
    int failed_tests = 0;
    
    printf("=== Spinlock Implementation Test Suite ===\n");
    printf("Testing spinlock implementation...\n");
    if (test_shared_spinlock() != 0) {
        failed_tests++;
    }    
    // Run all tests
    if (test_basic_operations() != 0) {
        failed_tests++;
    }
    
    if (test_multithreaded() != 0) {
        failed_tests++;
    }
    
    if (test_error_conditions() != 0) {
        failed_tests++;
    }
    
    // Summary
    printf("\n=== Test Summary ===\n");
    if (failed_tests == 0) {
        printf("All tests PASSED! ✓\n");
        return 0;
    } else {
        printf("%d test(s) FAILED! ✗\n", failed_tests);
        return 1;
    }
}
