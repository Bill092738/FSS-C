/* Business rules as pure functions plus their tunable parameters.
 *
 * Every formula of roadmap sections 7 and 8 lives here so it can be unit
 * tested without IO. Defaults match the roadmap; config/rules.json may
 * override them at startup (see rules_load.c).
 */
#ifndef FSS_RULES_H
#define FSS_RULES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  /* 7.6 claims */
  double claim_prior_votes;  /* prior expressed as N equivalent votes (5) */
  double claim_min_p;        /* minimum confidence to materialize (0.55) */
  double claim_user_prior;   /* prior for user-submitted claims (0.5) */
  double vote_checkin_boost; /* voter weight multiplier with a verified check-in in the building (1.5) */
  /* 7.1 search */
  int search_default_limit;  /* 20 */
  int search_max_limit;      /* 50 */
  double search_w_live;      /* weight of live crowding in ranking (0.5) */
  double search_w_dist;      /* ranking penalty per 100 m when `near` is given (0.3) */
  double search_w_quality;   /* ranking bonus per unit of spot quality (0.5) */
  /* 10.5 auth */
  int64_t session_ttl_ms;    /* 30 days */
  /* 7.2 geofence */
  double fence_acc_inside;   /* max accuracy (m) for the inside factor (50) */
  double fence_acc_near;     /* max accuracy (m) for the near factor (100) */
  double fence_g_inside;     /* 1.0 */
  double fence_g_near;       /* 0.6 */
  double fence_g_outside;    /* 0.2, also used without a position */
  /* 7.3 live estimate */
  int64_t live_half_life_ms; /* report weight half-life (20 min) */
  int64_t live_window_ms;    /* reports older than this are ignored (90 min) */
  double live_prior_weight;  /* w0 of the forecast pseudo-observation (0.5) */
  double live_basis_min_w;   /* sum of weights for basis "reports" (0.5) */
  double live_default_forecast; /* forecast when nothing better is known (1.0) */
  int64_t event_ttl_ms;      /* lifetime of an event report (4 h) */
  int event_min_users;       /* distinct reporters that make an event visible (2) */
  double event_trusted_rep;  /* reputation that makes one in-fence report enough (1.5) */
  /* 7.6 report limits */
  int64_t report_dedupe_ms;  /* only a user's last report per spot in this window counts (10 min) */
  int report_max_buildings;  /* distinct buildings per user per hour (3) */
  /* 4.5 / 9.2 WebSocket */
  int ws_max_subs;           /* subscriptions per connection (200) */
  /* 4.6 jobs */
  int64_t job_live_decay_ms;     /* 60 s */
  int64_t job_wal_checkpoint_ms; /* 10 min */
} fss_rules_s;

extern fss_rules_s FSS_RULES;

/* Beta-mean claim confidence: p = (N*prior + U) / (N + U + D). */
double fss_claim_confidence(double prior, double up, double down,
                            double prior_votes);

/* Maps quiet intensity 0..3 to a hard noise ceiling (-1 = none) and a ranking
 * weight (roadmap 7.1). Returns -1 for out-of-range input. */
int fss_quiet_params(int quiet, int *max_noise, double *w_quiet);

/* Display color of a crowding estimate 0..2 (roadmap 7.3). */
const char *fss_live_color(double est);

/* Geofence factor g (roadmap 7.2). `dist_m` is the distance to the fence
 * boundary; pass has_pos = 0 when the client sent no position. */
double fss_fence_factor(int has_pos, int inside, double dist_m,
                        double accuracy_m);

/* One crowding report: level 0..2, static weight (reputation x fence factor)
 * and time in ms. */
typedef struct {
  double level;
  double weight;
  int64_t at;
} fss_live_obs_s;

typedef struct {
  double est;        /* 0..2 */
  double conf;       /* 1 - exp(-sum w) */
  double wsum;       /* sum of decayed weights */
  int basis_reports; /* 1: "reports", 0: "forecast" */
} fss_live_est_s;

/* Live crowding estimate (roadmap 7.3): reports decay with the half-life and
 * are mixed with the forecast as a pseudo-observation of weight w0. Reports
 * outside [now - window, now] are ignored. */
fss_live_est_s fss_live_estimate(const fss_live_obs_s *obs, size_t n,
                                 double forecast, int64_t now);

#endif /* FSS_RULES_H */
