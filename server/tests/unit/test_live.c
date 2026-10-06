#include "harness.h"

#include "geo.h"
#include "rules.h"

/* Engineering Hall of the demo fixture: a ~68 m x 67 m square. */
static const fss_point_s HALL[] = {
    {-83.0124, 40.0027}, {-83.0116, 40.0027}, {-83.0116, 40.0033},
    {-83.0124, 40.0033}, {-83.0124, 40.0027},
};
#define HALL_N (sizeof(HALL) / sizeof(HALL[0]))

void test_geo(void) {
  fss_point_s center = {-83.0120, 40.0030};
  CHECK(fss_geo_inside(center, HALL, HALL_N));
  CHECK(fss_geo_inside(center, HALL, HALL_N - 1)); /* open polygon */
  CHECK(!fss_geo_inside((fss_point_s){-83.0110, 40.0030}, HALL, HALL_N));
  CHECK(!fss_geo_inside((fss_point_s){-83.0120, 40.0040}, HALL, HALL_N));
  CHECK(!fss_geo_inside(center, HALL, 2));

  /* concave "L": the notch is outside */
  static const fss_point_s L[] = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
  CHECK(fss_geo_inside((fss_point_s){0.5, 1.5}, L, 6));
  CHECK(!fss_geo_inside((fss_point_s){1.5, 1.5}, L, 6));

  /* 0.0004 deg longitude at 40 N is about 34 m; center to edge */
  CHECK_NEAR(fss_geo_boundary_dist_m(center, HALL, HALL_N), 33.2, 1.0);
  /* 0.0001 deg east of the east edge: about 8.5 m */
  CHECK_NEAR(fss_geo_boundary_dist_m((fss_point_s){-83.0115, 40.0030}, HALL,
                                     HALL_N),
             8.53, 0.2);
  /* nearest point is a corner */
  double d = fss_geo_boundary_dist_m((fss_point_s){-83.0115, 40.0034}, HALL,
                                     HALL_N);
  CHECK_NEAR(d, sqrt(8.53 * 8.53 + 11.05 * 11.05), 0.3);
  CHECK(isinf(fss_geo_boundary_dist_m(center, HALL, 0)));
}

void test_fence(void) {
  /* roadmap 7.2 table */
  CHECK_NEAR(fss_fence_factor(1, 1, 30, 20), 1.0, 1e-12);
  CHECK_NEAR(fss_fence_factor(1, 1, 30, 50), 1.0, 1e-12);
  CHECK_NEAR(fss_fence_factor(1, 0, 40, 60), 0.6, 1e-12);
  CHECK_NEAR(fss_fence_factor(1, 0, 40, 100), 0.6, 1e-12);
  CHECK_NEAR(fss_fence_factor(1, 0, 40, 30), 0.2, 1e-12);  /* d > accuracy */
  CHECK_NEAR(fss_fence_factor(1, 0, 40, 150), 0.2, 1e-12); /* too vague */
  CHECK_NEAR(fss_fence_factor(0, 0, 0, 0), 0.2, 1e-12);    /* no position */
  /* inside but 50 < accuracy <= 100: the near row applies */
  CHECK_NEAR(fss_fence_factor(1, 1, 10, 80), 0.6, 1e-12);
  CHECK_NEAR(fss_fence_factor(1, 1, 10, 120), 0.2, 1e-12);
  CHECK_NEAR(fss_fence_factor(1, 1, 10, NAN), 0.2, 1e-12);
}

void test_estimate(void) {
  const int64_t now = 1790000000000LL, min = 60000;
  /* no reports: the forecast, basis forecast, zero confidence */
  fss_live_est_s e = fss_live_estimate(NULL, 0, 1.0, now);
  CHECK_NEAR(e.est, 1.0, 1e-12);
  CHECK_NEAR(e.conf, 0.0, 1e-12);
  CHECK(!e.basis_reports);

  /* one fresh in-fence red report from reputation 1.0 */
  fss_live_obs_s one = {.level = 2, .weight = 1.0, .at = now};
  e = fss_live_estimate(&one, 1, 1.0, now);
  CHECK_NEAR(e.est, (0.5 * 1.0 + 2.0) / 1.5, 1e-12);
  CHECK_NEAR(e.conf, 1 - exp(-1.0), 1e-12);
  CHECK(e.basis_reports);
  CHECK(!strcmp(fss_live_color(e.est), "red"));

  /* 20 minutes later the weight halved */
  e = fss_live_estimate(&one, 1, 1.0, now + 20 * min);
  CHECK_NEAR(e.wsum, 0.5, 1e-12);
  CHECK_NEAR(e.est, (0.5 + 1.0) / 1.0, 1e-12);
  CHECK(e.basis_reports); /* sum w >= 0.5 */
  e = fss_live_estimate(&one, 1, 1.0, now + 21 * min);
  CHECK(!e.basis_reports);

  /* outside the 90-minute window (or in the future) it is ignored */
  e = fss_live_estimate(&one, 1, 0.0, now + 91 * min);
  CHECK_NEAR(e.est, 0.0, 1e-12);
  CHECK_NEAR(e.conf, 0.0, 1e-12);
  e = fss_live_estimate(&one, 1, 0.0, now - 1);
  CHECK_NEAR(e.wsum, 0.0, 1e-12);
  e = fss_live_estimate(&one, 1, 0.0, now + 90 * min);
  CHECK_NEAR(e.wsum, exp2(-4.5), 1e-12); /* window is inclusive */

  /* an out-of-fence report (g = 0.2) alone stays "forecast" */
  fss_live_obs_s far = {.level = 2, .weight = 0.2, .at = now};
  e = fss_live_estimate(&far, 1, 1.0, now);
  CHECK(!e.basis_reports);
  CHECK(!strcmp(fss_live_color(e.est), "yellow"));

  /* weighted mix */
  fss_live_obs_s mix[] = {{.level = 0, .weight = 1.0, .at = now},
                          {.level = 2, .weight = 3.0, .at = now - 20 * min}};
  e = fss_live_estimate(mix, 2, 1.0, now);
  CHECK_NEAR(e.wsum, 2.5, 1e-12);
  CHECK_NEAR(e.est, (0.5 + 0 + 1.5 * 2) / 3.0, 1e-12);
}
