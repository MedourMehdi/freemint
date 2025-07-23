#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Test result tracking */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT_EQ(actual, expected, message) \
    do { \
        if ((actual) == (expected)) { \
            printf("PASS: %s (got %d)\n", message, actual); \
            tests_passed++; \
        } else { \
            printf("FAIL: %s (expected %d, got %d)\n", message, expected, actual); \
            tests_failed++; \
        } \
    } while(0)

/* Test only the failed mutex type attribute getters */
void test_failed_mutex_type_getters(void) {
    printf("\n=== Failed Test: Mutex Type Attribute Getters ===\n");
    
    pthread_mutexattr_t attr;
    int type, result;
    
    printf("Test Side: type address=%p\n", &type);

    pthread_mutexattr_init(&attr);
    
    /* Test PTHREAD_MUTEX_NORMAL getter */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    result = pthread_mutexattr_gettype(&attr, &type);
    printf("DEBUG: gettype result=%d, type value=%d (0x%x)\n", result, type, type);
    TEST_ASSERT_EQ(type, PTHREAD_MUTEX_NORMAL, "Type should be PTHREAD_MUTEX_NORMAL");
    
    /* Test PTHREAD_MUTEX_RECURSIVE getter */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    result = pthread_mutexattr_gettype(&attr, &type);
    printf("DEBUG: gettype result=%d, type value=%d (0x%x)\n", result, type, type);
    TEST_ASSERT_EQ(type, PTHREAD_MUTEX_RECURSIVE, "Type should be PTHREAD_MUTEX_RECURSIVE");
    
    /* Test PTHREAD_MUTEX_ERRORCHECK getter */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    result = pthread_mutexattr_gettype(&attr, &type);
    printf("DEBUG: gettype result=%d, type value=%d (0x%x)\n", result, type, type);
    TEST_ASSERT_EQ(type, PTHREAD_MUTEX_ERRORCHECK, "Type should be PTHREAD_MUTEX_ERRORCHECK");
    
    pthread_mutexattr_destroy(&attr);
}

/* Test only the failed priority protocol attribute getters */
void test_failed_protocol_getters(void) {
    printf("\n=== Failed Test: Priority Protocol Attribute Getters ===\n");
    
    pthread_mutexattr_t attr;
    int protocol, result;
    
    printf("Test Side: protocol address=%p\n", &protocol);

    pthread_mutexattr_init(&attr);
    
    /* Test PTHREAD_PRIO_NONE getter */
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_NONE);
    result = pthread_mutexattr_getprotocol(&attr, &protocol);
    printf("DEBUG: getprotocol result=%d, protocol value=%d (0x%x)\n", result, protocol, protocol);
    TEST_ASSERT_EQ(protocol, PTHREAD_PRIO_NONE, "Protocol should be PTHREAD_PRIO_NONE");
    
    /* Test PTHREAD_PRIO_INHERIT getter */
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    result = pthread_mutexattr_getprotocol(&attr, &protocol);
    printf("DEBUG: getprotocol result=%d, protocol value=%d (0x%x)\n", result, protocol, protocol);
    TEST_ASSERT_EQ(protocol, PTHREAD_PRIO_INHERIT, "Protocol should be PTHREAD_PRIO_INHERIT");
    
    /* Test PTHREAD_PRIO_PROTECT getter */
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT);
    result = pthread_mutexattr_getprotocol(&attr, &protocol);
    printf("DEBUG: getprotocol result=%d, protocol value=%d (0x%x)\n", result, protocol, protocol);
    TEST_ASSERT_EQ(protocol, PTHREAD_PRIO_PROTECT, "Protocol should be PTHREAD_PRIO_PROTECT");
    
    pthread_mutexattr_destroy(&attr);
}

/* Test only the failed priority ceiling attribute getters */
void test_failed_ceiling_getters(void) {
    printf("\n=== Failed Test: Priority Ceiling Attribute Getters ===\n");
    
    pthread_mutexattr_t attr;
    int ceiling, result;
    ceiling = 0xdeadbeef;
    printf("Test Side: ceiling address=%p\n", &ceiling);
    pthread_mutexattr_init(&attr);
    
    /* Test ceiling value 10 getter */
    pthread_mutexattr_setprioceiling(&attr, 10);
    result = pthread_mutexattr_getprioceiling(&attr, &ceiling);
    printf("DEBUG: getprioceiling result=%d, ceiling value=%d (0x%x)\n", result, ceiling, ceiling);
    TEST_ASSERT_EQ(ceiling, 10, "Priority ceiling should be 10");
    
    /* Test ceiling value 0 getter */
    pthread_mutexattr_setprioceiling(&attr, 0);
    result = pthread_mutexattr_getprioceiling(&attr, &ceiling);
    printf("DEBUG: getprioceiling result=%d, ceiling value=%d (0x%x)\n", result, ceiling, ceiling);
    TEST_ASSERT_EQ(ceiling, 0, "Priority ceiling should be 0");
    
    pthread_mutexattr_destroy(&attr);
}

/* Additional debugging test to examine attribute structure */
void test_attribute_structure_debug(void) {
    printf("\n=== Debug: Attribute Structure Analysis ===\n");
    
    pthread_mutexattr_t attr;
    unsigned char *ptr;
    int i;
    
    printf("sizeof(pthread_mutexattr_t) = %zu\n", sizeof(pthread_mutexattr_t));
    
    pthread_mutexattr_init(&attr);
    
    /* Print raw memory contents */
    ptr = (unsigned char*)&attr;
    printf("Raw attribute memory after init: ");
    for (i = 0; i < sizeof(pthread_mutexattr_t); i++) {
        printf("%02x ", ptr[i]);
    }
    printf("\n");
    
    /* Set type and examine memory */
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    printf("Raw attribute memory after settype(RECURSIVE): ");
    for (i = 0; i < sizeof(pthread_mutexattr_t); i++) {
        printf("%02x ", ptr[i]);
    }
    printf("\n");
    
    /* Set protocol and examine memory */
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    printf("Raw attribute memory after setprotocol(INHERIT): ");
    for (i = 0; i < sizeof(pthread_mutexattr_t); i++) {
        printf("%02x ", ptr[i]);
    }
    printf("\n");
    
    /* Set ceiling and examine memory */
    pthread_mutexattr_setprioceiling(&attr, 42);
    printf("Raw attribute memory after setprioceiling(42): ");
    for (i = 0; i < sizeof(pthread_mutexattr_t); i++) {
        printf("%02x ", ptr[i]);
    }
    printf("\n");
    
    pthread_mutexattr_destroy(&attr);
}

/* Test to check if constants are defined correctly */
void test_constants_debug(void) {
    printf("\n=== Debug: Pthread Constants ===\n");
    
    printf("PTHREAD_MUTEX_NORMAL = %d (0x%x)\n", PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_NORMAL);
    printf("PTHREAD_MUTEX_RECURSIVE = %d (0x%x)\n", PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_RECURSIVE);
    printf("PTHREAD_MUTEX_ERRORCHECK = %d (0x%x)\n", PTHREAD_MUTEX_ERRORCHECK, PTHREAD_MUTEX_ERRORCHECK);
    
    printf("PTHREAD_PRIO_NONE = %d (0x%x)\n", PTHREAD_PRIO_NONE, PTHREAD_PRIO_NONE);
    printf("PTHREAD_PRIO_INHERIT = %d (0x%x)\n", PTHREAD_PRIO_INHERIT, PTHREAD_PRIO_INHERIT);
    printf("PTHREAD_PRIO_PROTECT = %d (0x%x)\n", PTHREAD_PRIO_PROTECT, PTHREAD_PRIO_PROTECT);
    
    printf("EINVAL = %d (0x%x)\n", EINVAL, EINVAL);
}

/* Main test runner for failed tests only */
int main(int argc, char* argv[]) {
    printf("FreeMiNT pthread_mutexattr_t Failed Tests Only\n");
    printf("==============================================\n");
    
    /* Debug info first */
    test_constants_debug();
    test_attribute_structure_debug();
    
    /* Run only the failed tests */
    test_failed_mutex_type_getters();
    test_failed_protocol_getters();
    test_failed_ceiling_getters();
    
    /* Print summary */
    printf("\n=== Test Summary ===\n");
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    
    if (tests_failed == 0) {
        printf("All failed tests now PASSED!\n");
        return 0;
    } else {
        printf("Tests still FAILING!\n");
        return 1;
    }
}
