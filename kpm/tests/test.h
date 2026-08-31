#ifndef STACKPLZ_TEST_H
#define STACKPLZ_TEST_H

#include <stdio.h>

extern int spz_test_failures;

#define SPZ_EXPECT(condition)                                                                  \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, __LINE__, #condition); \
            spz_test_failures++;                                                               \
        }                                                                                      \
    } while (0)

#define SPZ_EXPECT_EQ(actual, expected)                                                        \
    do {                                                                                       \
        unsigned long long spz_actual = (unsigned long long)(actual);                          \
        unsigned long long spz_expected = (unsigned long long)(expected);                      \
        if (spz_actual != spz_expected) {                                                       \
            fprintf(stderr, "%s:%d: got %llu, want %llu: %s\n", __FILE__, __LINE__,          \
                    spz_actual, spz_expected, #actual);                                        \
            spz_test_failures++;                                                               \
        }                                                                                      \
    } while (0)

#endif
