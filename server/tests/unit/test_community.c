#include "harness.h"

#include "rules.h"

void test_karma_rules(void) {
  /* daily caps (roadmap 7.5) */
  CHECK_EQ_INT(fss_karma_allowed(2, 0, 40), 2);
  CHECK_EQ_INT(fss_karma_allowed(2, 38, 40), 2);
  CHECK_EQ_INT(fss_karma_allowed(2, 39, 40), 1);
  CHECK_EQ_INT(fss_karma_allowed(2, 40, 40), 0);
  CHECK_EQ_INT(fss_karma_allowed(2, 99, 40), 0);
  CHECK_EQ_INT(fss_karma_allowed(5, 1000, 0), 5);  /* no cap */
  CHECK_EQ_INT(fss_karma_allowed(-3, 1000, 15), -3); /* revocations pass */
  CHECK_EQ_INT(fss_karma_allowed(0, 0, 4), 0);

  /* verified check-in hours */
  CHECK_EQ_INT(fss_checkin_karma(0), 0);
  CHECK_EQ_INT(fss_checkin_karma(3599999), 0);
  CHECK_EQ_INT(fss_checkin_karma(3600000), 1);
  CHECK_EQ_INT(fss_checkin_karma(3 * 3600000 + 5), 3);
  CHECK_EQ_INT(fss_checkin_karma(-1), 0);
}

void test_reputation_rules(void) {
  CHECK_NEAR(fss_rep_clamp(1.0), 1.0, 1e-12);
  CHECK_NEAR(fss_rep_clamp(0.05), 0.1, 1e-12);
  CHECK_NEAR(fss_rep_clamp(3.2), 3.0, 1e-12);
  CHECK_NEAR(fss_rep_clamp(NAN), 0.1, 1e-12);

  /* consensus verdicts (roadmap 7.6) */
  CHECK_EQ_INT(fss_report_verdict(2, 0.4, 0.9), -1); /* off by 1.6 */
  CHECK_EQ_INT(fss_report_verdict(0, 1.5, 0.8), -1); /* exactly 1.5 */
  CHECK_EQ_INT(fss_report_verdict(2, 0.4, 0.79), 0); /* weak consensus */
  CHECK_EQ_INT(fss_report_verdict(1, 1.4, 0.95), 1);
  CHECK_EQ_INT(fss_report_verdict(0, 0.5, 0.95), 1);
  CHECK_EQ_INT(fss_report_verdict(0, 1.0, 0.95), 0); /* neither */
  CHECK_EQ_INT(fss_report_verdict(1, 1, NAN), 0);

  /* moderation: p < 0.3 and D >= 3; prior 0.5 = 2.5 of 5 votes */
  CHECK(!fss_rejected_by_votes(0.5, 0, 2.9));
  CHECK(!fss_rejected_by_votes(0.5, 0, 3.0)); /* p = 2.5/8 = 0.3125 */
  CHECK(fss_rejected_by_votes(0.5, 0, 4.0));  /* p = 2.5/9 = 0.278 */
  CHECK(!fss_rejected_by_votes(0.5, 2, 4.0)); /* p = 4.5/11 = 0.41 */
}

void test_checkin_rules(void) {
  const int64_t min = 60000, t = 1790000000000LL;
  CHECK_NEAR(fss_checkin_level(0, 120), -1, 1e-12);
  CHECK_NEAR(fss_checkin_level(3, 0), -1, 1e-12); /* unknown capacity */
  CHECK_NEAR(fss_checkin_level(30, 120), 0.5, 1e-12);
  CHECK_NEAR(fss_checkin_level(60, 120), 1.0, 1e-12);
  CHECK_NEAR(fss_checkin_level(500, 120), 2.0, 1e-12);

  CHECK_EQ_INT(fss_checkin_credit(t, 1, t + 10 * min, 1), 10 * min);
  CHECK_EQ_INT(fss_checkin_credit(t, 1, t + 50 * min, 1), 20 * min); /* capped */
  CHECK_EQ_INT(fss_checkin_credit(t, 0, t + 10 * min, 1), 0);
  CHECK_EQ_INT(fss_checkin_credit(t, 1, t + 10 * min, 0), 0);
  CHECK_EQ_INT(fss_checkin_credit(t, 1, t - 1, 1), 0);

  /* expiry: 30 min without a heartbeat, 6 h at most */
  const int64_t h = 60 * min;
  int64_t end = 0;
  CHECK_EQ_INT(fss_checkin_expired(t, t + 10 * min, t + 39 * min, &end), 0);
  CHECK_EQ_INT(fss_checkin_expired(t, t + 10 * min, t + 40 * min, &end),
               FSS_CHECKIN_STALE);
  CHECK_EQ_INT(end, t + 10 * min);
  CHECK_EQ_INT(fss_checkin_expired(t, t, t + 30 * min, &end), FSS_CHECKIN_STALE);
  CHECK_EQ_INT(end, t);
  CHECK_EQ_INT(fss_checkin_expired(t, t + 6 * h - min, t + 6 * h, &end),
               FSS_CHECKIN_TIMEOUT);
  CHECK_EQ_INT(end, t + 6 * h);
  /* stale past the limit: still ends at the limit */
  CHECK_EQ_INT(fss_checkin_expired(t, t + 6 * h + min, t + 7 * h, &end),
               FSS_CHECKIN_TIMEOUT);
  CHECK_EQ_INT(end, t + 6 * h);
}

void test_rate_limit(void) {
  fss_rl_slot_s slots[8] = {{0}};
  const int64_t t = 1000000, w = 60000;
  for (int i = 0; i < 10; ++i)
    CHECK_EQ_INT(fss_rl_hit(slots, 8, 42, t + i, w, 10), 0);
  CHECK_EQ_INT(fss_rl_hit(slots, 8, 42, t + 30000, w, 10), 30000);
  CHECK_EQ_INT(fss_rl_hit(slots, 8, 7, t + 30000, w, 10), 0); /* other key */
  CHECK_EQ_INT(fss_rl_hit(slots, 8, 42, t + w, w, 10), 0);    /* new window */
  /* a full table evicts the oldest window instead of failing */
  for (uint64_t k = 100; k < 120; ++k)
    CHECK_EQ_INT(fss_rl_hit(slots, 8, k, t + w + (int64_t)k, w, 1), 0);
  CHECK(fss_rl_hit(slots, 8, 119, t + w + 200, w, 1) > 0);
}
