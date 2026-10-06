/* Crowding and event reports: POST /spots/:id/reports (roadmap 3, 7.2, 7.3).
 *
 *   {"level": 0 | 1 | 2}            green / yellow / red
 *   {"event": "outlet_broken"}      or wifi_down, closed_event
 *   + optional "lat", "lon", "accuracy_m" (all three or none)
 *
 * The fence factor and weight are computed before the write transaction; the
 * transaction inserts the report, recomputes the materialized state and
 * publishes changes after the commit.
 */
#include "api.h"

#include "auth.h"
#include "db.h"
#include "geo.h"
#include "live.h"
#include "rules.h"

#include <math.h>
#include <string.h>

#define FSS_FENCE_MAX_VERTICES 256
#define FSS_ACCURACY_MAX_M 100000.0

static const char SQL_REPORT_CONTEXT[] =
    "SELECT s.building_id, s.status, u.reputation FROM spot s, user u"
    " WHERE s.id = ?1 AND u.id = ?2";
static const char SQL_FENCE_POINTS[] =
    "SELECT j.value ->> 0, j.value ->> 1"
    " FROM building b, json_each(b.fence_json) j"
    " WHERE b.id = ?1 ORDER BY j.key";
/* Distinct other buildings this user reported in during the last hour. */
static const char SQL_USER_BUILDINGS[] =
    "SELECT count(DISTINCT s.building_id) FROM report r"
    " JOIN spot s ON s.id = r.spot_id"
    " WHERE r.user_id = ?1 AND r.at > ?2 - 3600000 AND r.at <= ?2"
    "   AND s.building_id != ?3";
static const char SQL_REPORT_INSERT[] =
    "INSERT INTO report (spot_id, user_id, level, event, in_fence, accuracy_m,"
    " weight, fence, at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";

static int is_number(FIOBJ o) {
  return FIOBJ_TYPE_IS(o, FIOBJ_T_NUMBER) || FIOBJ_TYPE_IS(o, FIOBJ_T_FLOAT);
}

typedef struct {
  int has_pos;
  double lat, lon, accuracy;
} position_s;

/* Parses the optional position; sends 422 and returns -1 when invalid. */
static int parse_position(fio_http_s *h, FIOBJ body, position_s *pos) {
  FIOBJ la = fiobj_hash_get2(body, "lat", 3);
  FIOBJ lo = fiobj_hash_get2(body, "lon", 3);
  FIOBJ ac = fiobj_hash_get2(body, "accuracy_m", 10);
  *pos = (position_s){0};
  if (!la && !lo && !ac)
    return 0;
  if (is_number(la) && is_number(lo) && is_number(ac)) {
    *pos = (position_s){.has_pos = 1, .lat = fiobj2f(la), .lon = fiobj2f(lo),
                        .accuracy = fiobj2f(ac)};
    if (fabs(pos->lat) <= 90 && fabs(pos->lon) <= 180 && pos->accuracy > 0 &&
        pos->accuracy <= FSS_ACCURACY_MAX_M)
      return 0;
  }
  fss_send_error(h, 422, "invalid_position",
                 "lat, lon and accuracy_m must be sent together as numbers"
                 " (accuracy_m > 0)");
  return -1;
}

/* Geofence (roadmap 7.2): sets `inside` and returns the fence factor. */
static double fence_factor(int64_t building, const position_s *pos,
                           int *inside) {
  *inside = 0;
  if (!pos->has_pos)
    return fss_fence_factor(0, 0, INFINITY, 0);
  fss_point_s poly[FSS_FENCE_MAX_VERTICES];
  size_t n = 0;
  sqlite3_stmt *st = fss_stmt(SQL_FENCE_POINTS);
  if (st) {
    sqlite3_bind_int64(st, 1, building);
    while (sqlite3_step(st) == SQLITE_ROW && n < FSS_FENCE_MAX_VERTICES)
      poly[n++] = (fss_point_s){.lon = sqlite3_column_double(st, 0),
                                .lat = sqlite3_column_double(st, 1)};
    fss_stmt_release(st);
  }
  fss_point_s p = {.lon = pos->lon, .lat = pos->lat};
  *inside = fss_geo_inside(p, poly, n);
  return fss_fence_factor(1, *inside, fss_geo_boundary_dist_m(p, poly, n),
                          pos->accuracy);
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
  position_s pos;
  if (!lv == !ev || (lv && (level < 0 || level > 2)) || (ev && !kind)) {
    fss_send_error(h, 422, "invalid_report",
                   "send either level (0, 1, 2) or event (outlet_broken,"
                   " wifi_down, closed_event)");
    goto done;
  }
  if (parse_position(h, body, &pos))
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
  double g = fence_factor(building, &pos, &inside);
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
  if (rc != SQLITE_OK || fss_live_commit(&box) != SQLITE_OK)
    goto db_error;

  char *out = fio_bstr_printf(NULL, "{\"report\":{\"id\":%lld,\"spot\":%lld,",
                              (long long)report_id, (long long)spot);
  if (kind)
    out = fio_bstr_printf(out, "\"event\":\"%s\",", kind);
  else
    out = fio_bstr_printf(out, "\"level\":%d,", (int)level);
  out = fio_bstr_printf(out, "\"in_fence\":%s,\"fence\":%.2f,\"weight\":%.3f,"
                             "\"at\":%lld},",
                        inside ? "true" : "false", g, weight, (long long)now);
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
