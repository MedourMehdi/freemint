#include "mint_pthread.h"
#include <stdio.h>
#include <unistd.h>

#define NUM_READERS 3
#define NUM_WRITERS 2
#define NUM_LOOPS 5

pthread_rwlock_t rwlock;
int shared_data = 0;

void *reader(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < NUM_LOOPS; i++) {
        pthread_rwlock_rdlock(&rwlock);
        printf("Reader %d: read shared_data = %d\n", id, shared_data);
        pthread_rwlock_unlock(&rwlock);
        // usleep(100000); // 100ms
        pthread_sleep_ms(100);
    }
    return NULL;
}

void *writer(void *arg) {
    int id = (int)(long)arg;
    for (int i = 0; i < NUM_LOOPS; i++) {
        pthread_rwlock_wrlock(&rwlock);
        int new_val = ++shared_data;
        printf("Writer %d: updated shared_data = %d\n", id, new_val);
        pthread_rwlock_unlock(&rwlock);
        // usleep(200000); // 200ms
        pthread_sleep_ms(200);
    }
    return NULL;
}

int main(void){
    pthread_t readers[NUM_READERS], writers[NUM_WRITERS];
    
    // Initialize RWLock
    if (pthread_rwlock_init(&rwlock, NULL)) {
        perror("rwlock_init failed");
        return 1;
    }

    // Create reader threads
    for (int i = 0; i < NUM_READERS; i++) {
        if (pthread_create(&readers[i], NULL, reader, (void*)(long)i)) {
            perror("reader create failed");
            return 1;
        }
    }

    // Create writer threads
    for (int i = 0; i < NUM_WRITERS; i++) {
        if (pthread_create(&writers[i], NULL, writer, (void*)(long)i)) {
            perror("writer create failed");
            return 1;
        }
    }

    // Wait for readers
    for (int i = 0; i < NUM_READERS; i++) {
        pthread_join(readers[i], NULL);
    }

    // Wait for writers
    for (int i = 0; i < NUM_WRITERS; i++) {
        pthread_join(writers[i], NULL);
    }

    // Clean up
    pthread_rwlock_destroy(&rwlock);
    printf("Final shared_data value: %d\n", shared_data);
    
    return 0;
}