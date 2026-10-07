#include "harness.h"

int fss_test_failures = 0;
int fss_test_checks = 0;

/* Each test file exposes one entry point. */
void test_router(void);
void test_util(void);
void test_rules(void);
void test_fts(void);
void test_geo(void);
void test_fence(void);
void test_estimate(void);
void test_karma_rules(void);
void test_reputation_rules(void);
void test_checkin_rules(void);
void test_rate_limit(void);

static const struct {
  const char *name;
  void (*fn)(void);
} TESTS[] = {
    {"router", test_router},
    {"util", test_util},
    {"rules", test_rules},
    {"fts", test_fts},
    {"geo", test_geo},
    {"fence", test_fence},
    {"estimate", test_estimate},
    {"karma", test_karma_rules},
    {"reputation", test_reputation_rules},
    {"checkin", test_checkin_rules},
    {"rate_limit", test_rate_limit},
};

int main(void) {
  for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); ++i) {
    int before = fss_test_failures;
    TESTS[i].fn();
    fprintf(stderr, "%-12s %s\n", TESTS[i].name,
            fss_test_failures == before ? "ok" : "FAILED");
  }
  fprintf(stderr, "%d checks, %d failures\n", fss_test_checks,
          fss_test_failures);
  return fss_test_failures ? 1 : 0;
}
