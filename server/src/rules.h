/* Business rules as pure functions plus their tunable parameters.
 *
 * Every formula of roadmap sections 7 and 8 lives here so it can be unit
 * tested without IO. Defaults match the roadmap; config/rules.json may
 * override them at startup (see rules_load.c).
 */
#ifndef FSS_RULES_H
#define FSS_RULES_H

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

#endif /* FSS_RULES_H */
