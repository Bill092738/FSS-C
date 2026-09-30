#include "rules.h"

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
