#include <stdio.h>
#include "mint_pthread.h"
#include <unistd.h> // for sleep()

void* thread_func(void* arg) {
    int is_initial = pthread_is_initialthread_np();
    int is_mt = pthread_is_multithreaded_np();
    
    printf("[Thread] Is initial: %s\n", is_initial ? "YES" : "NO");
    printf("[Thread] Is multithreaded: %s\n", is_mt ? "YES" : "NO");
    printf("[Thread] Total threads: %d\n", is_mt + 1);
    
    return NULL;
}

int main() {
    printf("=== Starting in single-thread mode ===\n");
    printf("[Main] Is initial: %s\n", pthread_is_initialthread_np() ? "YES" : "NO");
    printf("[Main] Is multithreaded: %s\n", pthread_is_multithreaded_np() ? "YES" : "NO");
    printf("[Main] Total threads: %d\n\n", pthread_is_multithreaded_np() + 1);
    
    pthread_t tid;
    printf("=== Creating new thread ===\n");
    pthread_create(&tid, NULL, thread_func, NULL);
    
    // // Give time for thread to start
    // sleep(1);
    pthread_yield();
    
    printf("\n=== After thread creation ===\n");
    printf("[Main] Is initial: %s\n", pthread_is_initialthread_np() ? "YES" : "NO");
    printf("[Main] Is multithreaded: %s\n", pthread_is_multithreaded_np() ? "YES" : "NO");
    printf("[Main] Total threads: %d\n", pthread_is_multithreaded_np() + 1);
    
    pthread_join(tid, NULL);
    
    printf("\n=== After thread exit ===\n");
    printf("[Main] Is multithreaded: %s\n", pthread_is_multithreaded_np() ? "YES" : "NO");
    printf("[Main] Total threads: %d\n", pthread_is_multithreaded_np() + 1);
    
    return 0;
}

/*
=== Starting in single-thread mode ===
[Main] Is initial: YES
[Main] Is multithreaded: NO
[Main] Total threads: 1

=== Creating new thread ===
[Thread] Is initial: NO
[Thread] Is multithreaded: YES
[Thread] Total threads: 2

=== After thread creation ===
[Main] Is initial: YES
[Main] Is multithreaded: YES
[Main] Total threads: 2

=== After thread exit ===
[Main] Is multithreaded: NO
[Main] Total threads: 1
*/