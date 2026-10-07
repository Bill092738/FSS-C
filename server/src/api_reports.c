/* Crowding and event reports: POST /spots/:id/reports (roadmap 3, 7.2, 7.3).
 *
 *   {"level": 0 | 1 | 2}            green / yellow / red
 *   {"event": "outlet_broken"}      or wifi_down, closed_event
 *   + optional "lat", "lon", "accuracy_m" (all three or none)
 *
 * The fence factor and weight are computed before the write transaction; the
 * transaction inserts the report, recomputes the materialized state, credits
 * karma (a crowding report with g >= 0.6; for an event, the other users whose
 * report it confirms) and publishes changes after the commit.
 */
#include "api.h"

#include "auth.h"
#include "community.h"
#include "db.h"
#include "fence.h"
#include "live.h"
#include "rules.h"

#include <string.h>

static const char SQL_REPORT_CONTEXT[] =
    "SELECT s.building_id, s.status, u.reputation FROM spot s, user u"
    " WHERE s.id = ?1 AND u.id = ?2";
/* Distinct other buildings this user reported in during the last hour. */
static const char SQL_USER_BUILDINGS[] =
    "SELECT count(DISTINCT s.building_id) FROM report r"
    " JOIN spot s ON s.id = r.spot_id"
    " WHERE r.user_id = ?1 AND r.at > ?2 - 3600000 AND r.at <= ?2"
    "   AND s.building_id != ?3";
/* A karma-earning crowding report by the same user at the same spot within
 * the dedupe window: only one of them earns karma (roadmap 7.6). */
static const char SQL_REPORT_RECENT[] =
    "SELECT 1 FROM report WHERE user_id = ?1 AND spot_id = ?2"
    " AND level IS NOT NULL AND fence >= ?3 AND at > ?4 - ?5 AND at <= ?4"
    " AND id != ?6 LIMIT 1";
/* Other users with a live report of the same event: each one's first report
 * in the TTL window identifies the episode their confirmation pays for. */
static const char SQL_EVENT_PEERS[] =
    "SELECT user_id, min(id) FROM report WHERE spot_id = ?1 AND event = ?2"
    " AND at > ?3 - ?4 AND at <= ?3 AND user_id != ?5 GROUP BY user_id";
static const char SQL_REPORT_INSERT[] =
    "INSERT INTO report (spot_id, user_id, level, event, in_fence, accuracy_m,"
    " weight, fence, at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";

#define FSS_EVENT_PEERS_MAX 64

/* Roadmap 7.5: an event report confirmed by another user earns its author
 * karma_event once; the confirmation also counts as an accepted
 * contribution for reputation. */
static int confirm_event(int64_t spot, const char *kind, int64_t uid,
                         int64_t now, fss_outbox_s *box) {
  int64_t users[FSS_EVENT_PEERS_MAX], reports[FSS_EVENT_PEERS_MAX];
  size_t n = 0;
  sqlite3_stmt *st = fss_stmt(SQL_EVENT_PEERS);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 3, now);
  sqlite3_bind_int64(st, 4, FSS_RULES.event_ttl_ms);
  sqlite3_bind_int64(st, 5, uid);
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < FSS_EVENT_PEERS_MAX) {
    users[n] = sqlite3_column_int64(st, 0);
    reports[n++] = sqlite3_column_int64(st, 1);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return -1;
  for (size_t i = 0; i < n; ++i)
    if (fss_karma_award(users[i], FSS_RULES.karma_event, 0, "event_confirmed",
                        "report", reports[i], now, box) < 0 ||
        fss_rep_adjust(users[i], FSS_RULES.rep_accept, "event_confirmed",
                       "report", reports[i], now, box) < 0)
      return -1;
  return 0;
}

/* Roadmap 7.5: +karma_report for a crowding report with g >= karma_report_min_g,
 * at most once per spot and dedupe window, karma_report_daily per day.
 * Returns the points credited or -1. */
static int report_karma(int64_t uid, int64_t spot, int64_t report, double g,
                        int64_t now, fss_outbox_s *box) {
  if (g < FSS_RULES.karma_report_min_g)
    return 0;
  sqlite3_stmt *st = fss_stmt(SQL_REPORT_RECENT);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int64(st, 2, spot);
  sqlite3_bind_double(st, 3, FSS_RULES.karma_report_min_g);
  sqlite3_bind_int64(st, 4, now);
  sqlite3_bind_int64(st, 5, FSS_RULES.report_dedupe_ms);
  sqlite3_bind_int64(st, 6, report);
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc == SQLITE_ROW)
    return 0;
  if (rc != SQLITE_DONE)
    return -1;
  return fss_karma_award(uid, FSS_RULES.karma_report,
                         (int64_t)FSS_RULES.karma_report_daily *
                             FSS_RULES.karma_report,
                         "report", "report", report, now, box);
}

void api_spot_reports_create(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  int64_t spot = p->num[0];
  FIOBJ lv = fiobj_hash_get2(body, "level", 5);
  FIOBJ ev = fiobj_hash_get2(body, "event", 5);
  int64_t level = FIOBJ_TYPE_IS(lv, FIOBJ_T_NUMBER) ? fiobj2i(lv) : -1;
  fio_str_info_s ev_name = FIOBJ_TYPE_IS(ev, FIOBJ_T_STRING)
                               ? fiobj2cstr(ev)
                               : (fio_str_info_s){0};
  const char *kind = ev_name.len ? fss_event_kind(ev_name.buf, ev_name.len) : NULL;
  fss_position_s pos;
  if (!lv == !ev || (lv && (level < 0 || level > 2)) || (ev && !kind)) {
    fss_send_error(h, 422, "invalid_report",
                   "send either level (0, 1, 2) or event (outlet_broken,"
                   " wifi_down, closed_event)");
    goto done;
  }
  if (fss_position_parse(h, body, &pos))
    goto done;

  /* context and weight: pure computation, outside the write transaction */
  sqlite3_stmt *st = fss_stmt(SQL_REPORT_CONTEXT);
  if (!st) {
    fss_send_db_error(h);
    goto done;
  }
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_int64(st, 2, uid);
  int rc = sqlite3_step(st);
  int64_t building = 0;
  double reputation = 0;
  char status[16] = {0};
  if (rc == SQLITE_ROW) {
    building = sqlite3_column_int64(st, 0);
    const char *s = (const char *)sqlite3_column_text(st, 1);
    if (s)
      strncpy(status, s, sizeof(status) - 1);
    reputation = sqlite3_column_double(st, 2);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    fss_send_db_error(h);
    goto done;
  }
  if (rc == SQLITE_DONE || !strcmp(status, "merged")) {
    fss_send_error(h, 404, "not_found", "unknown spot");
    goto done;
  }
  if (strcmp(status, "active")) {
    fss_send_error(h, 409, "spot_not_active",
                   "reports are accepted for active spots only");
    goto done;
  }
  int inside;
  double g = fss_fence_factor_at(building, &pos, &inside);
  double weight = reputation * g;
  int64_t now = fss_now_ms();

  fss_outbox_s box = {0};
  fss_live_state_s live;
  fss_event_state_s event;
  int64_t report_id = 0;
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    goto done;
  }
  /* roadmap 7.6: at most N distinct buildings per user per hour */
  if (!(st = fss_stmt(SQL_USER_BUILDINGS)))
    goto db_error;
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int64(st, 2, now);
  sqlite3_bind_int64(st, 3, building);
  rc = sqlite3_step(st);
  int64_t others = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW)
    goto db_error;
  if (others >= FSS_RULES.report_max_buildings) {
    fss_tx_rollback();
    fss_send_error(h, 429, "rate_limited",
                   "too many different buildings reported in the last hour");
    goto done;
  }
  if (!(st = fss_stmt(SQL_REPORT_INSERT)))
    goto db_error;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_int64(st, 2, uid);
  if (lv)
    sqlite3_bind_int64(st, 3, level);
  if (kind)
    sqlite3_bind_text(st, 4, kind, -1, SQLITE_STATIC);
  sqlite3_bind_int(st, 5, inside);
  if (pos.has_pos)
    sqlite3_bind_double(st, 6, pos.accuracy);
  sqlite3_bind_double(st, 7, weight);
  sqlite3_bind_double(st, 8, g);
  sqlite3_bind_int64(st, 9, now);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto db_error;
  report_id = sqlite3_last_insert_rowid(fss_db());
  rc = kind ? fss_event_update(spot, kind, now, &event, &box)
            : fss_live_update(spot, now, &live, &box);
  if (rc != SQLITE_OK)
    goto db_error;
  int karma = kind ? confirm_event(spot, kind, uid, now, &box)
                   : report_karma(uid, spot, report_id, g, now, &box);
  /* badges count every report, also those that earn no karma */
  if (karma < 0 || (!karma && fss_badges_check(uid, now, &box) < 0) ||
      fss_live_commit(&box) != SQLITE_OK)
    goto db_error;

  char *out = fio_bstr_printf(NULL, "{\"report\":{\"id\":%lld,\"spot\":%lld,",
                              (long long)report_id, (long long)spot);
  if (kind)
    out = fio_bstr_printf(out, "\"event\":\"%s\",", kind);
  else
    out = fio_bstr_printf(out, "\"level\":%d,", (int)level);
  out = fio_bstr_printf(out, "\"in_fence\":%s,\"fence\":%.2f,\"weight\":%.3f,"
                             "\"at\":%lld},\"karma\":%d,",
                        inside ? "true" : "false", g, weight, (long long)now,
                        kind ? 0 : karma);
  if (kind) {
    out = fio_bstr_write(out, "\"event\":", 8);
    out = fss_event_json(out, &event);
  } else {
    out = fio_bstr_write(out, "\"live\":", 7);
    out = fss_live_json(out, &live);
  }
  out = fio_bstr_write(out, "}", 1);
  fss_send_json(h, 201, out);
  goto done;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
done:
  fiobj_free(body);
}
