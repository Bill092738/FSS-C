/* Loads config/rules.json overrides into FSS_RULES (roadmap 7: "defaults are
 * collected in config/rules.json and read at startup"). Unknown keys are
 * rejected so that typos do not silently fall back to defaults. */
#include "rules_load.h"

#include "fss_fio.h"
#include "rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { R_DOUBLE, R_INT, R_I64 } rule_type_e;

static const struct {
  const char *key;
  rule_type_e type;
  void *ptr;
} RULE_FIELDS[] = {
    {"claim_prior_votes", R_DOUBLE, &FSS_RULES.claim_prior_votes},
    {"claim_min_p", R_DOUBLE, &FSS_RULES.claim_min_p},
    {"claim_user_prior", R_DOUBLE, &FSS_RULES.claim_user_prior},
    {"vote_checkin_boost", R_DOUBLE, &FSS_RULES.vote_checkin_boost},
    {"search_default_limit", R_INT, &FSS_RULES.search_default_limit},
    {"search_max_limit", R_INT, &FSS_RULES.search_max_limit},
    {"search_w_live", R_DOUBLE, &FSS_RULES.search_w_live},
    {"search_w_dist", R_DOUBLE, &FSS_RULES.search_w_dist},
    {"search_w_quality", R_DOUBLE, &FSS_RULES.search_w_quality},
    {"session_ttl_ms", R_I64, &FSS_RULES.session_ttl_ms},
    {"fence_acc_inside", R_DOUBLE, &FSS_RULES.fence_acc_inside},
    {"fence_acc_near", R_DOUBLE, &FSS_RULES.fence_acc_near},
    {"fence_g_inside", R_DOUBLE, &FSS_RULES.fence_g_inside},
    {"fence_g_near", R_DOUBLE, &FSS_RULES.fence_g_near},
    {"fence_g_outside", R_DOUBLE, &FSS_RULES.fence_g_outside},
    {"live_half_life_ms", R_I64, &FSS_RULES.live_half_life_ms},
    {"live_window_ms", R_I64, &FSS_RULES.live_window_ms},
    {"live_prior_weight", R_DOUBLE, &FSS_RULES.live_prior_weight},
    {"live_basis_min_w", R_DOUBLE, &FSS_RULES.live_basis_min_w},
    {"live_default_forecast", R_DOUBLE, &FSS_RULES.live_default_forecast},
    {"event_ttl_ms", R_I64, &FSS_RULES.event_ttl_ms},
    {"event_min_users", R_INT, &FSS_RULES.event_min_users},
    {"event_trusted_rep", R_DOUBLE, &FSS_RULES.event_trusted_rep},
    {"report_dedupe_ms", R_I64, &FSS_RULES.report_dedupe_ms},
    {"report_max_buildings", R_INT, &FSS_RULES.report_max_buildings},
    {"ws_max_subs", R_INT, &FSS_RULES.ws_max_subs},
    {"job_live_decay_ms", R_I64, &FSS_RULES.job_live_decay_ms},
    {"job_wal_checkpoint_ms", R_I64, &FSS_RULES.job_wal_checkpoint_ms},
};

static int apply_field(fiobj_each_s *e) {
  int *errors = e->udata;
  fio_str_info_s k = fiobj2cstr(e->key);
  if (k.len && k.buf[0] == '_') /* "_comment" style keys are ignored */
    return 0;
  for (size_t i = 0; i < sizeof(RULE_FIELDS) / sizeof(RULE_FIELDS[0]); ++i) {
    if (strlen(RULE_FIELDS[i].key) != k.len ||
        memcmp(RULE_FIELDS[i].key, k.buf, k.len))
      continue;
    if (!FIOBJ_TYPE_IS(e->value, FIOBJ_T_NUMBER) &&
        !FIOBJ_TYPE_IS(e->value, FIOBJ_T_FLOAT))
      break;
    switch (RULE_FIELDS[i].type) {
    case R_DOUBLE:
      *(double *)RULE_FIELDS[i].ptr = fiobj2f(e->value);
      break;
    case R_INT:
      *(int *)RULE_FIELDS[i].ptr = (int)fiobj2i(e->value);
      break;
    case R_I64:
      *(int64_t *)RULE_FIELDS[i].ptr = (int64_t)fiobj2i(e->value);
      break;
    }
    return 0;
  }
  FIO_LOG_ERROR("rules: unknown or non-numeric key \"%.*s\"", (int)k.len,
                k.buf);
  ++*errors;
  return 0;
}

/* Rejects values that would divide by zero or break timers. */
static int rules_validate(void) {
  const fss_rules_s *r = &FSS_RULES;
  const char *bad = NULL;
  if (r->live_half_life_ms <= 0)
    bad = "live_half_life_ms must be > 0";
  else if (r->live_window_ms <= 0)
    bad = "live_window_ms must be > 0";
  else if (r->live_prior_weight < 0)
    bad = "live_prior_weight must be >= 0";
  else if (r->event_ttl_ms <= 0 || r->event_min_users < 1)
    bad = "event_ttl_ms and event_min_users must be positive";
  else if (r->ws_max_subs < 1)
    bad = "ws_max_subs must be >= 1";
  else if (r->job_live_decay_ms < 100 || r->job_live_decay_ms > 86400000 ||
           r->job_wal_checkpoint_ms < 100 || r->job_wal_checkpoint_ms > 86400000)
    bad = "job intervals must be between 100 ms and 24 h";
  if (bad)
    FIO_LOG_ERROR("rules: %s", bad);
  return bad ? -1 : 0;
}

int fss_rules_load(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    FIO_LOG_INFO("no rules file at %s, using built-in defaults", path);
    return rules_validate();
  }
  char buf[1 << 16];
  size_t len = fread(buf, 1, sizeof(buf), f);
  int too_big = !feof(f);
  fclose(f);
  if (too_big) {
    FIO_LOG_ERROR("rules file %s is too large", path);
    return -1;
  }
  FIOBJ o = fiobj_json_parse2(buf, len, NULL);
  if (!FIOBJ_TYPE_IS(o, FIOBJ_T_HASH)) {
    fiobj_free(o);
    FIO_LOG_ERROR("rules file %s is not a JSON object", path);
    return -1;
  }
  int errors = 0;
  fiobj_each1(o, apply_field, &errors, 0);
  fiobj_free(o);
  return errors ? -1 : rules_validate();
}
