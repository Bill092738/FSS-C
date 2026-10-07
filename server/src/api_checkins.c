/* Check-ins (roadmap 7.4):
 *
 *   POST /checkins                {"spot_id", lat?, lon?, accuracy_m?}
 *   POST /checkins/:id/heartbeat  {lat?, lon?, accuracy_m?}   every ~10 min
 *   POST /checkins/:id/end        {}
 *
 * A position with fence factor >= checkin_fence_g counts as inside. A session
 * is verified once a position was inside; heartbeats credit the time between
 * two inside positions (capped). Two consecutive outside heartbeats, no
 * heartbeat for checkin_stale_ms or checkin_max_ms after the start end it.
 * Open verified check-ins feed the spot's crowding estimate.
 */
#include "api.h"

#include "auth.h"
#include "checkin.h"
#include "db.h"
#include "fence.h"
#include "live.h"
#include "rules.h"

#include <string.h>

static const char SQL_CHECKIN_SPOT[] =
    "SELECT building_id, status FROM spot WHERE id = ?1";
static const char SQL_CHECKIN_OPEN[] =
    "SELECT id, start_at, COALESCE(last_beat_at, start_at) FROM checkin"
    " WHERE user_id = ?1 AND end_at IS NULL";
static const char SQL_CHECKIN_INSERT[] =
    "INSERT INTO checkin (spot_id, user_id, start_at, last_beat_at, verified,"
    " last_in) VALUES (?1, ?2, ?3, ?3, ?4, ?4)";
static const char SQL_CHECKIN_GET[] =
    "SELECT k.user_id, k.spot_id, s.building_id, k.start_at,"
    " COALESCE(k.last_beat_at, k.start_at), k.last_in, k.outside_beats,"
    " k.end_at IS NOT NULL FROM checkin k JOIN spot s ON s.id = k.spot_id"
    " WHERE k.id = ?1";
static const char SQL_CHECKIN_BEAT[] =
    "UPDATE checkin SET last_beat_at = ?2, last_in = ?3,"
    " verified = verified OR ?3, verified_ms = verified_ms + ?4,"
    " outside_beats = CASE WHEN ?3 THEN 0 ELSE outside_beats + 1 END"
    " WHERE id = ?1 RETURNING outside_beats";

/* Sends {"checkin":{...}[,"live":{...}][,"karma":n]} with `status`. */
static void send_checkin(fio_http_s *h, size_t status, int64_t id,
                         const fss_live_state_s *live, int karma) {
  char *out = fio_bstr_write(NULL, "{\"checkin\":", 11);
  char *json = fss_checkin_json(out, id);
  if (!json) {
    fio_bstr_free(out);
    fss_send_db_error(h);
    return;
  }
  out = json;
  if (live) {
    out = fio_bstr_write(out, ",\"live\":", 8);
    out = fss_live_json(out, live);
  }
  if (karma >= 0)
    out = fio_bstr_printf(out, ",\"karma\":%d", karma);
  fss_send_json(h, status, fio_bstr_write(out, "}", 1));
}

static void send_ended(fio_http_s *h) {
  fss_send_error(h, 409, "checkin_ended", "this check-in has ended");
}

/* *****************************************************************************
POST /checkins
***************************************************************************** */

void api_checkins_create(fio_http_s *h, fss_params_s *p) {
  (void)p;
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  fss_outbox_s box = {0};
  FIOBJ sp = fiobj_hash_get2(body, "spot_id", 7);
  fss_position_s pos;
  if (!FIOBJ_TYPE_IS(sp, FIOBJ_T_NUMBER) || fiobj2i(sp) <= 0) {
    fss_send_error(h, 422, "invalid_checkin", "spot_id (number) is required");
    goto done;
  }
  if (fss_position_parse(h, body, &pos))
    goto done;
  int64_t spot = fiobj2i(sp);
  sqlite3_stmt *st = fss_stmt(SQL_CHECKIN_SPOT);
  if (!st) {
    fss_send_db_error(h);
    goto done;
  }
  sqlite3_bind_int64(st, 1, spot);
  int rc = sqlite3_step(st);
  int64_t building = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  const char *status = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 1) : NULL;
  int active = status && !strcmp(status, "active");
  int merged = status && !strcmp(status, "merged");
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    fss_send_db_error(h);
    goto done;
  }
  if (rc == SQLITE_DONE || merged) {
    fss_send_error(h, 404, "not_found", "unknown spot");
    goto done;
  }
  if (!active) {
    fss_send_error(h, 409, "spot_not_active",
                   "check-ins are accepted for active spots only");
    goto done;
  }
  int inside;
  double g = fss_fence_factor_at(building, &pos, &inside);
  int in = g >= FSS_RULES.checkin_fence_g;
  int64_t now = fss_now_ms();

  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    goto done;
  }
  /* one open session per user; an expired one the job has not reached yet
   * is ended here */
  if (!(st = fss_stmt(SQL_CHECKIN_OPEN)))
    goto db_error;
  sqlite3_bind_int64(st, 1, uid);
  rc = sqlite3_step(st);
  int64_t open_id = 0, end_at = 0;
  int why = 0;
  if (rc == SQLITE_ROW) {
    open_id = sqlite3_column_int64(st, 0);
    why = fss_checkin_expired(sqlite3_column_int64(st, 1),
                              sqlite3_column_int64(st, 2), now, &end_at);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    goto db_error;
  if (open_id && !why) {
    fss_outbox_clear(&box);
    fss_tx_rollback();
    char *out = fio_bstr_printf(
        NULL,
        "{\"error\":{\"code\":\"checkin_open\",\"message\":\"end the open"
        " check-in first\",\"checkin_id\":%lld}}",
        (long long)open_id);
    fss_send_json(h, 409, out);
    goto done;
  }
  if (open_id &&
      fss_checkin_close(open_id, end_at,
                        why == FSS_CHECKIN_STALE ? "stale" : "timeout", now,
                        NULL, &box) != SQLITE_OK)
    goto db_error;
  if (!(st = fss_stmt(SQL_CHECKIN_INSERT)))
    goto db_error;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_int64(st, 2, uid);
  sqlite3_bind_int64(st, 3, now);
  sqlite3_bind_int(st, 4, in);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto db_error;
  int64_t id = sqlite3_last_insert_rowid(fss_db());
  fss_live_state_s live;
  if (fss_live_update(spot, now, &live, &box) != SQLITE_OK ||
      fss_live_commit(&box) != SQLITE_OK)
    goto db_error;
  send_checkin(h, 201, id, &live, -1);
  goto done;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
done:
  fiobj_free(body);
}

/* *****************************************************************************
Heartbeat and end
***************************************************************************** */

typedef struct {
  int64_t user, spot, building, start, last_beat;
  int last_in, outside, ended;
} checkin_row_s;

/* Loads a check-in inside the transaction. Returns 1 when found and owned by
 * `uid`, 0 after sending 404, -1 on database error. */
static int load_own(fio_http_s *h, int64_t id, int64_t uid, checkin_row_s *k) {
  sqlite3_stmt *st = fss_stmt(SQL_CHECKIN_GET);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, id);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)
    *k = (checkin_row_s){.user = sqlite3_column_int64(st, 0),
                         .spot = sqlite3_column_int64(st, 1),
                         .building = sqlite3_column_int64(st, 2),
                         .start = sqlite3_column_int64(st, 3),
                         .last_beat = sqlite3_column_int64(st, 4),
                         .last_in = sqlite3_column_int(st, 5),
                         .outside = sqlite3_column_int(st, 6),
                         .ended = sqlite3_column_int(st, 7)};
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return -1;
  if (rc == SQLITE_DONE || k->user != uid) { /* other users' ids stay hidden */
    fss_send_error(h, 404, "not_found", "unknown check-in");
    return 0;
  }
  return 1;
}

void api_checkin_heartbeat(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  fss_position_s pos;
  fss_outbox_s box = {0};
  int64_t id = p->num[0];
  if (fss_position_parse(h, body, &pos))
    goto done;
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    goto done;
  }
  checkin_row_s k;
  int found = load_own(h, id, uid, &k);
  if (found <= 0) {
    fss_tx_rollback();
    if (found < 0)
      fss_send_db_error(h);
    goto done;
  }
  if (k.ended) {
    fss_tx_rollback();
    send_ended(h);
    goto done;
  }
  int64_t now = fss_now_ms(), end_at;
  int why = fss_checkin_expired(k.start, k.last_beat, now, &end_at);
  if (why) { /* too late: the session already ended at end_at */
    if (fss_checkin_close(id, end_at,
                          why == FSS_CHECKIN_STALE ? "stale" : "timeout", now,
                          NULL, &box) != SQLITE_OK ||
        fss_live_commit(&box) != SQLITE_OK)
      goto db_error;
    send_checkin(h, 200, id, NULL, -1);
    goto done;
  }
  /* the building is known only after the ownership check, so the fence is
   * read inside the transaction (a short read) */
  int inside;
  int in = fss_fence_factor_at(k.building, &pos, &inside) >=
           FSS_RULES.checkin_fence_g;
  sqlite3_stmt *st = fss_stmt(SQL_CHECKIN_BEAT);
  if (!st)
    goto db_error;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, now);
  sqlite3_bind_int(st, 3, in);
  sqlite3_bind_int64(st, 4, fss_checkin_credit(k.last_beat, k.last_in, now, in));
  int rc = sqlite3_step(st);
  int outside = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
  if (rc == SQLITE_ROW)
    rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto db_error;
  int karma = -1;
  fss_live_state_s live;
  if (outside >= FSS_RULES.checkin_max_outside)
    rc = fss_checkin_close(id, now, "outside", now, &karma, &box);
  else /* verification may have changed the implicit report */
    rc = fss_live_update(k.spot, now, &live, &box);
  if (rc != SQLITE_OK || fss_live_commit(&box) != SQLITE_OK)
    goto db_error;
  send_checkin(h, 200, id, karma < 0 ? &live : NULL, karma);
  goto done;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
done:
  fiobj_free(body);
}

void api_checkin_end(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h); /* Content-Type check (CSRF baseline) */
  if (!body)
    return;
  fiobj_free(body);
  fss_outbox_s box = {0};
  int64_t id = p->num[0];
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    return;
  }
  checkin_row_s k;
  int found = load_own(h, id, uid, &k);
  if (found <= 0) {
    fss_tx_rollback();
    if (found < 0)
      fss_send_db_error(h);
    return;
  }
  if (k.ended) {
    fss_tx_rollback();
    send_ended(h);
    return;
  }
  int64_t now = fss_now_ms(), end_at = now;
  int why = fss_checkin_expired(k.start, k.last_beat, now, &end_at);
  int karma = 0;
  if (fss_checkin_close(id, end_at,
                        why == FSS_CHECKIN_STALE   ? "stale"
                        : why == FSS_CHECKIN_TIMEOUT ? "timeout"
                                                     : "user",
                        now, &karma, &box) != SQLITE_OK ||
      fss_live_commit(&box) != SQLITE_OK) {
    fss_outbox_clear(&box);
    fss_tx_rollback();
    fss_send_db_error(h);
    return;
  }
  send_checkin(h, 200, id, NULL, karma);
}
