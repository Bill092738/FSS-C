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
  /* 7.4 check-ins */
  double checkin_fence_g;    /* fence factor that counts as "inside" (0.6) */
  int64_t checkin_credit_ms; /* max time credited per heartbeat (20 min) */
  int checkin_max_outside;   /* consecutive outside heartbeats that end it (2) */
  int64_t checkin_max_ms;    /* hard session limit (6 h) */
  int64_t checkin_stale_ms;  /* no heartbeat for this long ends it (30 min) */
  double checkin_signal_weight; /* weight of the implicit crowding report (0.3) */
  /* 7.5 karma: points per contribution and daily caps (counted in contributions) */
  int karma_report;          /* crowding report with g >= karma_report_min_g (2) */
  int karma_report_daily;    /* 20 */
  double karma_report_min_g; /* 0.6 */
  int karma_event;           /* event report confirmed by another user (5) */
  int karma_photo;           /* 3; revoked when the photo is hidden */
  int karma_photo_daily;     /* 5 */
  int karma_claim;           /* user claim with karma_claim_min_up up votes (3) */
  int karma_claim_daily;     /* 10 */
  int karma_claim_min_up;    /* 2 */
  int karma_spot;            /* discovered spot became active (20) */
  int karma_spot_daily;      /* 2 */
  int karma_checkin_hour;    /* per verified check-in hour, at the end (1) */
  int karma_checkin_daily;   /* 4 */
  int spot_confirm_min;      /* other users that activate a submitted spot (2) */
  int photo_daily_max;       /* uploads per user per day (20) */
  int64_t photo_max_bytes;   /* 5 MiB */
  /* 7.6 reputation */
  double rep_accept;         /* contribution accepted (+0.05) */
  double rep_reject;         /* contribution rejected or hidden (-0.1, stored positive) */
  double rep_min, rep_max;   /* 0.1 .. 3.0 */
  double rep_report_diff;    /* |level - consensus| that rejects a report (1.5) */
  double rep_report_agree;   /* |level - consensus| that accepts a report (0.5) */
  double rep_report_conf;    /* consensus confidence needed for either (0.8) */
  int64_t rep_window_ms;     /* consensus window around a report (30 min) */
  double reject_p;           /* photos hide / user claims count as rejected below this p (0.3) */
  double reject_down;        /* ... with at least this many weighted down votes (3) */
  /* 10.5 per-IP limit on the /auth endpoints */
  int auth_ip_per_min;       /* 10 */
  /* 4.6 jobs */
  int64_t job_live_decay_ms;     /* 60 s */
  int64_t job_wal_checkpoint_ms; /* 10 min */
  int64_t job_checkin_timeout_ms; /* 5 min */
  int64_t job_hourly_rollup_ms;  /* 1 h */
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

/* *****************************************************************************
Community (roadmap 7.4 - 7.6)
***************************************************************************** */

/* Karma that may still be credited: `delta` limited so that the points
 * credited for one reason within 24 h stay <= cap (cap <= 0: no cap). */
int fss_karma_allowed(int delta, int64_t used, int64_t cap);

/* Clamps a reputation to [rep_min, rep_max]. */
double fss_rep_clamp(double r);

/* A crowding report against the consensus of other users at the same time
 * (roadmap 7.6): -1 rejected (|diff| >= rep_report_diff), +1 accepted
 * (|diff| <= rep_report_agree), 0 otherwise or when the consensus confidence
 * is below rep_report_conf. */
int fss_report_verdict(double level, double consensus, double conf);

/* Implicit crowding level of `present` verified check-ins at a spot with
 * `capacity` seats: 0 (empty) .. 2 (full). Negative when there is no signal. */
double fss_checkin_level(int64_t present, double capacity);

/* Time a heartbeat credits to verified_ms: the gap since the previous
 * position, capped, and only when both positions were inside. */
int64_t fss_checkin_credit(int64_t prev_at, int prev_in, int64_t now, int now_in);

/* Whether an open check-in must end (roadmap 7.4): FSS_CHECKIN_STALE when no
 * heartbeat arrived for checkin_stale_ms (it ends at the last heartbeat),
 * FSS_CHECKIN_TIMEOUT at checkin_max_ms after the start (it ends there), else
 * 0. `last_beat` is the start when no heartbeat was sent. */
#define FSS_CHECKIN_TIMEOUT 1
#define FSS_CHECKIN_STALE 2
int fss_checkin_expired(int64_t start, int64_t last_beat, int64_t now,
                        int64_t *end_at);

/* Karma for a finished check-in: whole verified hours x karma_checkin_hour. */
int fss_checkin_karma(int64_t verified_ms);

/* Down-vote moderation for photos and user claims (roadmap 7.6). */
int fss_rejected_by_votes(double prior, double up, double down);

/* Fixed-window rate limiter over a small open-addressed table (no locking;
 * the caller serializes). Returns 0 when the hit is allowed, otherwise the
 * milliseconds until the key's window resets. */
typedef struct {
  uint64_t key;
  int64_t start;
  uint32_t count;
} fss_rl_slot_s;
int64_t fss_rl_hit(fss_rl_slot_s *slots, size_t n_slots, uint64_t key,
                   int64_t now, int64_t window_ms, uint32_t limit);

#endif /* FSS_RULES_H */
