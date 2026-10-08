#include "community.h"

#include "db.h"
#include "fss_fio.h"
#include "http.h"
#include "rules.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAY_MS (24LL * 3600 * 1000)

/* Runs a one-row statement binding ?1 = a (and ?2 = b when the statement has
 * a second parameter). Returns 0 and sets *out, or -1 on error. A query
 * without a row yields 0. */
static int scalar_f(const char *sql, int64_t a, double b, double *out) {
  sqlite3_stmt *st = fss_stmt(sql);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, a);
  if (sqlite3_bind_parameter_count(st) > 1)
    sqlite3_bind_double(st, 2, b);
  int rc = sqlite3_step(st);
  *out = rc == SQLITE_ROW ? sqlite3_column_double(st, 0) : 0;
  fss_stmt_release(st);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

/* *****************************************************************************
Karma (roadmap 7.5)
***************************************************************************** */

static const char SQL_KARMA_USED[] =
    "SELECT COALESCE(sum(delta), 0) FROM karma_ledger"
    " WHERE user_id = ?1 AND reason = ?2 AND at > ?3 AND delta > 0";
static const char SQL_KARMA_INSERT[] =
    "INSERT INTO karma_ledger (user_id, delta, reason, ref_type, ref_id, at)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT DO NOTHING";
static const char SQL_KARMA_APPLY[] =
    "UPDATE user SET karma = karma + ?2 WHERE id = ?1 RETURNING karma";
static const char SQL_KARMA_CREDITED[] =
    "SELECT delta FROM karma_ledger WHERE user_id = ?1 AND reason = ?2"
    " AND ref_type = ?3 AND ref_id = ?4";

/* Inserts a ledger row and updates the cached balance. Returns 1 when
 * written, 0 when the idempotency key already exists, -1 on error. */
static int karma_write(int64_t uid, int delta, const char *reason,
                       const char *ref_type, int64_t ref_id, int64_t now,
                       fss_outbox_s *box) {
  sqlite3_stmt *st = fss_stmt(SQL_KARMA_INSERT);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int(st, 2, delta);
  sqlite3_bind_text(st, 3, reason, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, ref_type, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 5, ref_id);
  sqlite3_bind_int64(st, 6, now);
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return -1;
  if (!sqlite3_changes(fss_db()))
    return 0;
  if (!(st = fss_stmt(SQL_KARMA_APPLY)))
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int(st, 2, delta);
  rc = sqlite3_step(st);
  int64_t total = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  if (rc == SQLITE_ROW)
    rc = sqlite3_step(st); /* finish the RETURNING statement */
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return -1;
  char *m = fio_bstr_printf(NULL,
                            "{\"t\":\"karma\",\"delta\":%d,\"total\":%lld,"
                            "\"reason\":\"%s\"",
                            delta, (long long)total, reason);
  fss_outbox_user(box, uid, m);
  return 1;
}

int fss_karma_award(int64_t uid, int delta, int64_t daily, const char *reason,
                    const char *ref_type, int64_t ref_id, int64_t now,
                    fss_outbox_s *box) {
  if (delta <= 0)
    return 0;
  double used = 0;
  if (daily > 0) {
    sqlite3_stmt *st = fss_stmt(SQL_KARMA_USED);
    if (!st)
      return -1;
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, reason, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now - DAY_MS);
    int rc = sqlite3_step(st);
    used = rc == SQLITE_ROW ? sqlite3_column_double(st, 0) : 0;
    fss_stmt_release(st);
    if (rc != SQLITE_ROW)
      return -1;
  }
  int allowed = fss_karma_allowed(delta, (int64_t)used, daily);
  if (allowed <= 0)
    return 0;
  int rc = karma_write(uid, allowed, reason, ref_type, ref_id, now, box);
  if (rc <= 0)
    return rc;
  return fss_badges_check(uid, now, box) < 0 ? -1 : allowed;
}

int fss_karma_revoke(int64_t uid, const char *award_reason, const char *reason,
                     const char *ref_type, int64_t ref_id, int64_t now,
                     fss_outbox_s *box) {
  sqlite3_stmt *st = fss_stmt(SQL_KARMA_CREDITED);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_text(st, 2, award_reason, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, ref_type, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 4, ref_id);
  int rc = sqlite3_step(st);
  int credited = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return -1;
  if (credited <= 0)
    return 0;
  rc = karma_write(uid, -credited, reason, ref_type, ref_id, now, box);
  return rc <= 0 ? rc : credited;
}

/* *****************************************************************************
Reputation (roadmap 7.6)
***************************************************************************** */

static const char SQL_REP_INSERT[] =
    "INSERT INTO rep_ledger (user_id, delta, reason, ref_type, ref_id, at)"
    " VALUES (?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT DO NOTHING";
static const char SQL_REP_APPLY[] =
    "UPDATE user SET reputation = max(?3, min(?4, reputation + ?2))"
    " WHERE id = ?1";

int fss_rep_adjust(int64_t uid, double delta, const char *reason,
                   const char *ref_type, int64_t ref_id, int64_t now,
                   fss_outbox_s *box) {
  sqlite3_stmt *st = fss_stmt(SQL_REP_INSERT);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_double(st, 2, delta);
  sqlite3_bind_text(st, 3, reason, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 4, ref_type, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 5, ref_id);
  sqlite3_bind_int64(st, 6, now);
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return -1;
  if (!sqlite3_changes(fss_db()))
    return 0;
  if (!(st = fss_stmt(SQL_REP_APPLY)))
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_double(st, 2, delta);
  sqlite3_bind_double(st, 3, FSS_RULES.rep_min);
  sqlite3_bind_double(st, 4, FSS_RULES.rep_max);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return -1;
  return delta > 0 && fss_badges_check(uid, now, box) < 0 ? -1 : 1;
}

/* Voter weight: reputation x boost when the voter has a verified check-in in
 * the building (roadmap 7.6). */
static const char SQL_VOTER_WEIGHT[] =
    "SELECT u.reputation * CASE WHEN EXISTS ("
    "  SELECT 1 FROM checkin k JOIN spot s2 ON s2.id = k.spot_id"
    "  WHERE k.user_id = u.id AND k.verified = 1 AND s2.building_id = ?2)"
    " THEN ?3 ELSE 1.0 END FROM user u WHERE u.id = ?1";

double fss_voter_weight(int64_t uid, int64_t building) {
  sqlite3_stmt *st = fss_stmt(SQL_VOTER_WEIGHT);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int64(st, 2, building);
  sqlite3_bind_double(st, 3, FSS_RULES.vote_checkin_boost);
  int rc = sqlite3_step(st);
  double w = rc == SQLITE_ROW ? sqlite3_column_double(st, 0) : -1;
  fss_stmt_release(st);
  return w;
}

/* *****************************************************************************
Badges (roadmap 7.5): badge.rule_json = {"count": <counter>, "gte": <n>}
***************************************************************************** */

static const char SQL_CNT_REPORTS[] =
    "SELECT count(*) FROM report WHERE user_id = ?1";
static const char SQL_CNT_SPOTS[] =
    "SELECT count(*) FROM spot WHERE created_by = ?1 AND status = 'active'";
static const char SQL_CNT_CLAIMS[] =
    "SELECT count(*) FROM claim c WHERE c.user_id = ?1 AND (SELECT count(*)"
    " FROM claim_vote v WHERE v.claim_id = c.id AND v.v = 1) >= ?2";
static const char SQL_CNT_PHOTOS[] =
    "SELECT count(*) FROM photo WHERE user_id = ?1 AND status = 'visible'";
static const char SQL_CNT_EVENTS[] =
    "SELECT count(*) FROM rep_ledger WHERE user_id = ?1"
    " AND reason = 'event_confirmed'";
static const char SQL_CNT_HOURS[] =
    "SELECT COALESCE(sum(verified_ms), 0) / 3600000.0 FROM checkin"
    " WHERE user_id = ?1 AND end_at IS NOT NULL";
static const char SQL_CNT_REP[] = "SELECT reputation FROM user WHERE id = ?1";
static const char SQL_CNT_KARMA[] = "SELECT karma FROM user WHERE id = ?1";

static const struct {
  const char *name;
  const char *sql;
} COUNTERS[] = {
    {"reports", SQL_CNT_REPORTS},          {"spots_discovered", SQL_CNT_SPOTS},
    {"claims_accepted", SQL_CNT_CLAIMS},   {"photos", SQL_CNT_PHOTOS},
    {"events_confirmed", SQL_CNT_EVENTS},  {"checkin_hours", SQL_CNT_HOURS},
    {"reputation", SQL_CNT_REP},           {"karma", SQL_CNT_KARMA},
};
#define N_COUNTERS (sizeof(COUNTERS) / sizeof(COUNTERS[0]))

static const char SQL_BADGES_OPEN[] =
    "SELECT b.key, b.name, b.rule_json ->> '$.count', b.rule_json ->> '$.gte'"
    " FROM badge b WHERE NOT EXISTS (SELECT 1 FROM user_badge ub"
    "   WHERE ub.user_id = ?1 AND ub.badge_key = b.key) ORDER BY b.key";
static const char SQL_BADGE_GRANT[] =
    "INSERT INTO user_badge (user_id, badge_key, at) VALUES (?1, ?2, ?3)"
    " ON CONFLICT DO NOTHING";

#define MAX_OPEN_BADGES 64

int fss_badges_check(int64_t uid, int64_t now, fss_outbox_s *box) {
  struct {
    char key[32], name[64];
    int counter;
    double gte;
  } open[MAX_OPEN_BADGES];
  size_t n = 0;
  sqlite3_stmt *st = fss_stmt(SQL_BADGES_OPEN);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < MAX_OPEN_BADGES) {
    const char *key = (const char *)sqlite3_column_text(st, 0);
    const char *name = (const char *)sqlite3_column_text(st, 1);
    const char *counter = (const char *)sqlite3_column_text(st, 2);
    int c = -1;
    for (size_t i = 0; counter && i < N_COUNTERS; ++i)
      if (!strcmp(counter, COUNTERS[i].name))
        c = (int)i;
    if (c < 0 || !key || strlen(key) >= sizeof(open[0].key)) {
      FIO_LOG_WARNING("badge %s has an unknown counter", key ? key : "?");
      continue;
    }
    snprintf(open[n].key, sizeof(open[n].key), "%s", key);
    snprintf(open[n].name, sizeof(open[n].name), "%s", name ? name : key);
    open[n].counter = c;
    open[n].gte = sqlite3_column_double(st, 3);
    ++n;
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return -1;

  double value[N_COUNTERS];
  int have[N_COUNTERS] = {0};
  int granted = 0;
  for (size_t i = 0; i < n; ++i) {
    int c = open[i].counter;
    if (!have[c]) {
      if (scalar_f(COUNTERS[c].sql, uid, FSS_RULES.karma_claim_min_up,
                   &value[c]))
        return -1;
      have[c] = 1;
    }
    if (!(value[c] >= open[i].gte))
      continue;
    if (!(st = fss_stmt(SQL_BADGE_GRANT)))
      return -1;
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, open[i].key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, now);
    rc = sqlite3_step(st);
    fss_stmt_release(st);
    if (rc != SQLITE_DONE)
      return -1;
    char *m = fio_bstr_write(NULL, "{\"t\":\"badge\",\"key\":", 19);
    m = fss_bstr_json_str(m, open[i].key, strlen(open[i].key));
    m = fio_bstr_write(m, ",\"name\":", 8);
    m = fss_bstr_json_str(m, open[i].name, strlen(open[i].name));
    fss_outbox_user(box, uid, m);
    ++granted;
  }
  return granted;
}

/* *****************************************************************************
Reputation rollup (roadmap 7.6): a crowding report is compared with what other
users reported for the same spot within rep_window_ms before and after it.
Each other user counts once (their mean level, their strongest weight decayed
by the time distance), so one prolific user cannot form a consensus alone.
***************************************************************************** */

static const char SQL_CURSOR_GET[] =
    "SELECT at FROM job_cursor WHERE name = ?1";
static const char SQL_CURSOR_SET[] =
    "INSERT INTO job_cursor (name, at) VALUES (?1, ?2)"
    " ON CONFLICT (name) DO UPDATE SET at = excluded.at";
static const char SQL_REP_CONSENSUS[] =
    "WITH todo AS ("
    "  SELECT id, spot_id, user_id, level, at FROM report"
    "  WHERE level IS NOT NULL AND at >= ?1 AND at < ?2),"
    " peer AS ("
    "  SELECT t.id AS rid, avg(o.level) AS lvl,"
    "    max(o.weight * pow(2.0, -abs(o.at - t.at) / ?4)) AS w"
    "  FROM todo t JOIN report o ON o.spot_id = t.spot_id"
    "    AND o.level IS NOT NULL AND o.user_id != t.user_id"
    "    AND o.at >= t.at - ?3 AND o.at <= t.at + ?3"
    "  GROUP BY t.id, o.user_id)"
    " SELECT t.id, t.user_id, t.level, sum(p.w * p.lvl) / sum(p.w), sum(p.w)"
    " FROM todo t JOIN peer p ON p.rid = t.id"
    " GROUP BY t.id ORDER BY t.at, t.id";

static const char CURSOR_REPUTATION[] = "reputation";

int fss_reputation_rollup(int64_t now) {
  int64_t until = now - FSS_RULES.rep_window_ms; /* complete windows only */
  if (fss_tx_begin() != SQLITE_OK)
    return -1;
  fss_outbox_s box = {0};
  int judged = 0;
  sqlite3_stmt *st = fss_stmt(SQL_CURSOR_GET);
  if (!st)
    goto fail;
  sqlite3_bind_text(st, 1, CURSOR_REPUTATION, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  int64_t from = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    goto fail;
  if (until <= from) {
    fss_tx_rollback();
    return 0;
  }

  /* collect verdicts first: adjusting reputation writes to user while the
   * consensus query is still reading */
  typedef struct {
    int64_t report, user;
    int verdict;
  } verdict_s;
  verdict_s *v = NULL;
  size_t n = 0, cap = 0;
  if (!(st = fss_stmt(SQL_REP_CONSENSUS)))
    goto fail;
  sqlite3_bind_int64(st, 1, from);
  sqlite3_bind_int64(st, 2, until);
  sqlite3_bind_int64(st, 3, FSS_RULES.rep_window_ms);
  sqlite3_bind_double(st, 4, (double)FSS_RULES.live_half_life_ms);
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    double wsum = sqlite3_column_double(st, 4);
    int verdict = fss_report_verdict(sqlite3_column_double(st, 2),
                                     sqlite3_column_double(st, 3),
                                     1.0 - exp(-wsum));
    if (!verdict)
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 256;
      verdict_s *p = realloc(v, cap * sizeof(*v));
      if (!p) {
        rc = SQLITE_NOMEM;
        break;
      }
      v = p;
    }
    v[n++] = (verdict_s){.report = sqlite3_column_int64(st, 0),
                         .user = sqlite3_column_int64(st, 1),
                         .verdict = verdict};
  }
  fss_stmt_release(st);
  if (rc != SQLITE_DONE) {
    free(v);
    goto fail;
  }
  for (size_t i = 0; i < n; ++i) {
    int ok = v[i].verdict > 0
                 ? fss_rep_adjust(v[i].user, FSS_RULES.rep_accept,
                                  "report_agreed", "report", v[i].report, now,
                                  &box)
                 : fss_rep_adjust(v[i].user, -FSS_RULES.rep_reject,
                                  "report_rejected", "report", v[i].report, now,
                                  &box);
    if (ok < 0) {
      free(v);
      goto fail;
    }
  }
  judged = (int)n;
  free(v);
  if (!(st = fss_stmt(SQL_CURSOR_SET)))
    goto fail;
  sqlite3_bind_text(st, 1, CURSOR_REPUTATION, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 2, until);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto fail;
  return fss_live_commit(&box) == SQLITE_OK ? judged : -1;

fail:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  return -1;
}
