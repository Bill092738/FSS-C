#include "harness.h"

#include "rules.h"
#include "util.h"

void test_rules(void) {
  /* 7.6: prior 0.8 with no votes stays 0.8; votes move it toward the tally */
  CHECK_NEAR(fss_claim_confidence(0.8, 0, 0, 5), 0.8, 1e-12);
  CHECK_NEAR(fss_claim_confidence(0.5, 2, 0, 5), 4.5 / 7.0, 1e-12);
  CHECK_NEAR(fss_claim_confidence(0.6, 0, 3, 5), 3.0 / 8.0, 1e-12);
  CHECK(fss_claim_confidence(0.45, 0, 0, 5) < 0.55); /* weak llm claim hidden */
  CHECK(fss_claim_confidence(0.45, 1, 0, 5) >= 0.54);

  int max_noise;
  double w;
  CHECK(!fss_quiet_params(0, &max_noise, &w) && max_noise == -1 && w == 0);
  CHECK(!fss_quiet_params(2, &max_noise, &w) && max_noise == 2 && w == 1.0);
  CHECK(!fss_quiet_params(3, &max_noise, &w) && max_noise == 1);
  CHECK(fss_quiet_params(4, &max_noise, &w));
  CHECK(fss_quiet_params(-1, &max_noise, &w));

  CHECK(!strcmp(fss_live_color(0.0), "green"));
  CHECK(!strcmp(fss_live_color(0.66), "green"));
  CHECK(!strcmp(fss_live_color(0.67), "yellow"));
  CHECK(!strcmp(fss_live_color(1.32), "yellow"));
  CHECK(!strcmp(fss_live_color(1.33), "red"));
}

void test_fts(void) {
  char buf[64];
  size_t n = fss_fts_query(buf, sizeof(buf), " basement  outlets ", 19, 8);
  CHECK_STR(buf, n, "\"basement\" \"outlets\"");
  n = fss_fts_query(buf, sizeof(buf), "a\"b OR", 6, 8);
  CHECK_STR(buf, n, "\"a\"\"b\" \"OR\"");
  n = fss_fts_query(buf, sizeof(buf), "   ", 3, 8);
  CHECK_EQ_INT(n, 0);
  n = fss_fts_query(buf, sizeof(buf), "a b c", 5, 2);
  CHECK_STR(buf, n, "\"a\" \"b\"");
  CHECK(fss_fts_query(buf, 4, "abcdef", 6, 8) == (size_t)-1);
}
