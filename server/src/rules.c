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
    .checkin_fence_g = 0.6,
    .checkin_credit_ms = 20LL * 60 * 1000,
    .checkin_max_outside = 2,
    .checkin_max_ms = 6LL * 3600 * 1000,
    .checkin_stale_ms = 30LL * 60 * 1000,
    .checkin_signal_weight = 0.3,
    .karma_report = 2,
    .karma_report_daily = 20,
    .karma_report_min_g = 0.6,
    .karma_event = 5,
    .karma_photo = 3,
    .karma_photo_daily = 5,
    .karma_claim = 3,
    .karma_claim_daily = 10,
    .karma_claim_min_up = 2,
    .karma_spot = 20,
    .karma_spot_daily = 2,
    .karma_checkin_hour = 1,
    .karma_checkin_daily = 4,
    .spot_confirm_min = 2,
    .photo_daily_max = 20,
    .photo_max_bytes = 5LL << 20,
    .rep_accept = 0.05,
    .rep_reject = 0.1,
    .rep_min = 0.1,
    .rep_max = 3.0,
    .rep_report_diff = 1.5,
    .rep_report_agree = 0.5,
    .rep_report_conf = 0.8,
    .rep_window_ms = 30LL * 60 * 1000,
    .reject_p = 0.3,
    .reject_down = 3,
    .auth_ip_per_min = 10,
    .job_live_decay_ms = 60LL * 1000,
    .job_wal_checkpoint_ms = 10LL * 60 * 1000,
    .job_checkin_timeout_ms = 5LL * 60 * 1000,
    .job_hourly_rollup_ms = 3600LL * 1000,
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

/* *****************************************************************************
Community
***************************************************************************** */

int fss_karma_allowed(int delta, int64_t used, int64_t cap) {
  if (delta <= 0 || cap <= 0)
    return delta;
  if (used >= cap)
    return 0;
  return cap - used < delta ? (int)(cap - used) : delta;
}

double fss_rep_clamp(double r) {
  if (!(r >= FSS_RULES.rep_min)) /* also maps NaN to the minimum */
    return FSS_RULES.rep_min;
  return r > FSS_RULES.rep_max ? FSS_RULES.rep_max : r;
}

int fss_report_verdict(double level, double consensus, double conf) {
  if (!(conf >= FSS_RULES.rep_report_conf))
    return 0;
  double diff = fabs(level - consensus);
  if (diff >= FSS_RULES.rep_report_diff)
    return -1;
  return diff <= FSS_RULES.rep_report_agree ? 1 : 0;
}

double fss_checkin_level(int64_t present, double capacity) {
  if (present <= 0 || !(capacity > 0))
    return -1;
  double level = 2.0 * (double)present / capacity;
  return level > 2.0 ? 2.0 : level;
}

int64_t fss_checkin_credit(int64_t prev_at, int prev_in, int64_t now,
                           int now_in) {
  if (!prev_in || !now_in || now <= prev_at)
    return 0;
  int64_t gap = now - prev_at;
  return gap > FSS_RULES.checkin_credit_ms ? FSS_RULES.checkin_credit_ms : gap;
}

int fss_checkin_expired(int64_t start, int64_t last_beat, int64_t now,
                        int64_t *end_at) {
  int64_t limit = start + FSS_RULES.checkin_max_ms;
  if (last_beat < start)
    last_beat = start;
  if (now - last_beat >= FSS_RULES.checkin_stale_ms && last_beat < limit) {
    *end_at = last_beat;
    return FSS_CHECKIN_STALE;
  }
  if (now >= limit) {
    *end_at = limit;
    return FSS_CHECKIN_TIMEOUT;
  }
  return 0;
}

int fss_checkin_karma(int64_t verified_ms) {
  if (verified_ms <= 0)
    return 0;
  return (int)(verified_ms / 3600000) * FSS_RULES.karma_checkin_hour;
}

int fss_rejected_by_votes(double prior, double up, double down) {
  return down >= FSS_RULES.reject_down &&
         fss_claim_confidence(prior, up, down, FSS_RULES.claim_prior_votes) <
             FSS_RULES.reject_p;
}

#define FSS_RL_PROBE 16

int64_t fss_rl_hit(fss_rl_slot_s *slots, size_t n_slots, uint64_t key,
                   int64_t now, int64_t window_ms, uint32_t limit) {
  if (!n_slots)
    return 0;
  if (!key) /* 0 marks an empty slot; keys are hashes, so 0 == 1 is harmless */
    key = 1;
  size_t start = (size_t)((key * 0x9E3779B97F4A7C15ULL) % n_slots);
  fss_rl_slot_s *free_slot = NULL, *oldest = NULL;
  for (size_t i = 0; i < FSS_RL_PROBE && i < n_slots; ++i) {
    fss_rl_slot_s *s = &slots[(start + i) % n_slots];
    if (s->key == key) {
      if (now - s->start >= window_ms || now < s->start) {
        s->start = now;
        s->count = 0;
      }
      if (s->count >= limit)
        return s->start + window_ms - now;
      ++s->count;
      return 0;
    }
    if (!free_slot && (!s->key || now - s->start >= window_ms))
      free_slot = s;
    if (!oldest || s->start < oldest->start)
      oldest = s;
  }
  /* new key: reuse an empty or expired slot, else evict the oldest window */
  fss_rl_slot_s *s = free_slot ? free_slot : oldest;
  *s = (fss_rl_slot_s){.key = key, .start = now, .count = 0};
  if (!limit)
    return window_ms;
  s->count = 1;
  return 0;
}
