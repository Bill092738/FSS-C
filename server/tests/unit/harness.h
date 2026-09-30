/* Minimal unit test harness (roadmap 10.4): no dependencies, one binary. */
#ifndef FSS_TEST_HARNESS_H
#define FSS_TEST_HARNESS_H

#include <math.h>
#include <stdio.h>
#include <string.h>

extern int fss_test_failures;
extern int fss_test_checks;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++fss_test_checks;                                                         \
    if (!(cond)) {                                                             \
      ++fss_test_failures;                                                     \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
    }                                                                          \
  } while (0)

#define CHECK_EQ_INT(a, b)                                                     \
  do {                                                                         \
    long long a_ = (long long)(a), b_ = (long long)(b);                        \
    ++fss_test_checks;                                                         \
    if (a_ != b_) {                                                            \
      ++fss_test_failures;                                                     \
      fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,     \
              __LINE__, #a, #b, a_, b_);                                       \
    }                                                                          \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                  \
  do {                                                                         \
    double a_ = (double)(a), b_ = (double)(b);                                 \
    ++fss_test_checks;                                                         \
    if (!(fabs(a_ - b_) <= (eps))) {                                           \
      ++fss_test_failures;                                                     \
      fprintf(stderr, "  FAIL %s:%d: %s ~= %s (%g vs %g)\n", __FILE__,         \
              __LINE__, #a, #b, a_, b_);                                       \
    }                                                                          \
  } while (0)

#define CHECK_STR(buf, len, lit)                                               \
  CHECK((len) == sizeof(lit) - 1 && !memcmp((buf), (lit), sizeof(lit) - 1))

#endif /* FSS_TEST_HARNESS_H */
