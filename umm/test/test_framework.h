#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int _tests_run = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;
static const char *_current_test = NULL;

#define TEST(name) static void test_##name(void)

#define RUN_TEST(name) do { \
    _current_test = #name; \
    _tests_run++; \
    printf("  [%d] Testing %s... ", _tests_run, #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASSED\n"); \
    _tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED\n    %s:%d: Assertion failed: %s\n", \
               __FILE__, __LINE__, #cond); \
        _tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b)   ASSERT((a) == (b))
#define ASSERT_NE(a, b)   ASSERT((a) != (b))
#define ASSERT_NULL(p)    ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_TRUE(cond)  ASSERT(cond)
#define ASSERT_FALSE(cond) ASSERT(!(cond))

#define TEST_SUITE(name) int main(void) { \
    printf("\n=== Test Suite: %s ===\n", name); \
    do {

#define END_TEST_SUITE() \
    } while(0); \
    printf("\nResults: %d run, %d passed, %d failed\n", \
           _tests_run, _tests_passed, _tests_failed); \
    return _tests_failed > 0 ? 1 : 0; \
}

#endif
