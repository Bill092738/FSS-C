#include "rules.h"

#include <math.h>

fss_rules_s FSS_RULES = {
    .claim_prior_votes = 5.0,
    .claim_min_p = 0.55,
    .claim_user_prior = 0.5,
    .vote_checkin_boost = 1.5,
    .search_default_limit = 20,
    .search_max_limit = 50,
    .search_w_live = 0.5,
    .search_w_dist = 0.3,
    .search_w_quality = 0.5,
    .session_ttl_ms = 30LL * 24 * 3600 * 1000,
    .fence_acc_inside = 50,
    .fence_acc_near = 100,
    .fence_g_inside = 1.0,
    .fence_g_near = 0.6,
    .fence_g_outside = 0.2,
    .live_half_life_ms = 20LL * 60 * 1000,
    .live_window_ms = 90LL * 60 * 1000,
    .live_prior_weight = 0.5,
    .live_basis_min_w = 0.5,
    .live_default_forecast = 1.0,
    .event_ttl_ms = 4LL * 3600 * 1000,
    .event_min_users = 2,
    .event_trusted_rep = 1.5,
    .report_dedupe_ms = 10LL * 60 * 1000,
    .report_max_buildings = 3,
    .ws_max_subs = 200,
    .job_live_decay_ms = 60LL * 1000,
    .job_wal_checkpoint_ms = 10LL * 60 * 1000,
};

double fss_claim_confidence(double prior, double up, double down,
                            double prior_votes) {
  double denom = prior_votes + up + down;
  if (denom <= 0)
    return prior;
  return (prior_votes * prior + up) / denom;
}

int fss_quiet_params(int quiet, int *max_noise, double *w_quiet) {
  static const int noise_cap[4] = {-1, -1, 2, 1};
  static const double weight[4] = {0.0, 0.5, 1.0, 2.0};
  if (quiet < 0 || quiet > 3)
    return -1;
  *max_noise = noise_cap[quiet];
  *w_quiet = weight[quiet];
  return 0;
}

const char *fss_live_color(double est) {
  if (est < 0.67)
    return "green";
  if (est < 1.33)
    return "yellow";
  return "red";
}

/* The roadmap table gives 1.0 for "inside and accuracy <= 50" and 0.6 for
 * "outside but within accuracy of the fence, accuracy <= 100". A point inside
 * with 50 < accuracy <= 100 has distance 0 to the building, so it takes the
 * 0.6 row rather than falling through to 0.2. */
double fss_fence_factor(int has_pos, int inside, double dist_m,
                        double accuracy_m) {
  if (!has_pos || !(accuracy_m >= 0))
    return FSS_RULES.fence_g_outside;
  if (inside && accuracy_m <= FSS_RULES.fence_acc_inside)
    return FSS_RULES.fence_g_inside;
  if ((inside || dist_m <= accuracy_m) && accuracy_m <= FSS_RULES.fence_acc_near)
    return FSS_RULES.fence_g_near;
  return FSS_RULES.fence_g_outside;
}

fss_live_est_s fss_live_estimate(const fss_live_obs_s *obs, size_t n,
                                 double forecast, int64_t now) {
  double w0 = FSS_RULES.live_prior_weight;
  double wsum = 0, lsum = 0;
  for (size_t i = 0; i < n; ++i) {
    int64_t age = now - obs[i].at;
    if (age < 0 || age > FSS_RULES.live_window_ms || !(obs[i].weight > 0))
      continue;
    double w = obs[i].weight *
               exp2(-(double)age / (double)FSS_RULES.live_half_life_ms);
    wsum += w;
    lsum += w * obs[i].level;
  }
  fss_live_est_s r = {
      .est = (w0 * forecast + lsum) / (w0 + wsum),
      .conf = 1.0 - exp(-wsum),
      .wsum = wsum,
      .basis_reports = wsum >= FSS_RULES.live_basis_min_w,
  };
  if (w0 + wsum <= 0) /* w0 = 0 and no reports */
    r.est = forecast;
  return r;
}
