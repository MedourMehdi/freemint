#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

void* thread_func(void* arg) {
    printf("Thread started\n");
    msleep(1000);
    return NULL;
}

void test_name_operations() {
    pthread_t thread;
    char name[16];
    
    pthread_create(&thread, NULL, thread_func, NULL);
    
    // Test valid name
    pthread_setname_np(thread, "WorkerThread");
    pthread_getname_np(thread, name, sizeof(name));
    printf("Normal name: %s\n", name);
    
    // Test NULL name (unnamed)
    pthread_setname_np(thread, NULL);
    pthread_getname_np(thread, name, sizeof(name));
    printf("After NULL set: '%s'\n", name);
    
    // Test long name (should truncate)
    pthread_setname_np(thread, "ThisNameIsWayTooLongButWeTruncateIt");
    pthread_getname_np(thread, name, sizeof(name));
    printf("Truncated name: %s\n", name);
    
    // Test empty name
    pthread_setname_np(thread, "");
    pthread_getname_np(thread, name, sizeof(name));
    printf("Empty name: '%s'\n", name);
    
    pthread_join(thread, NULL);
}

void test_error_handling() {
    char name[16];
    pthread_t self = pthread_self();
    
    // Invalid buffer size
    int rc = pthread_getname_np(self, name, 10);
    printf("Small buffer test: %s\n", rc == ERANGE ? "PASS" : "FAIL");
    
    // NULL buffer
    rc = pthread_getname_np(self, NULL, 16);
    printf("NULL buffer test: %s\n", rc == EINVAL ? "PASS" : "FAIL");
    
    // Invalid thread
    rc = pthread_setname_np(9999, "Invalid");
    printf("Invalid thread (set): %s\n", rc == ESRCH ? "PASS" : "FAIL");
    
    rc = pthread_getname_np(9999, name, sizeof(name));
    printf("Invalid thread (get): %s\n", rc == ESRCH ? "PASS" : "FAIL");
}

int main() {
    printf("=== Name Operation Tests ===\n");
    test_name_operations();
    
    printf("\n=== Error Handling Tests ===\n");
    test_error_handling();
    
    printf("\n=== Main Thread Name Test ===\n");
    char name[16];
    pthread_setname_np(pthread_self(), "MainThread");
    pthread_getname_np(pthread_self(), name, sizeof(name));
    printf("Main thread name: %s\n", name);
    
    return 0;
}