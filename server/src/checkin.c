#include "checkin.h"

#include "community.h"
#include "db.h"
#include "fss_fio.h"
#include "rules.h"

#include <stdlib.h>

static const char SQL_CHECKIN_END[] =
    "UPDATE checkin SET end_at = max(?2, start_at), end_reason = ?3"
    " WHERE id = ?1 AND end_at IS NULL"
    " RETURNING user_id, spot_id, verified_ms";

int fss_checkin_close(int64_t id, int64_t end_at, const char *reason,
                      int64_t now, int *karma, fss_outbox_s *box) {
  if (karma)
    *karma = 0;
  sqlite3_stmt *st = fss_stmt(SQL_CHECKIN_END);
  if (!st)
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, end_at);
  sqlite3_bind_text(st, 3, reason, -1, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  int64_t uid = 0, spot = 0, verified_ms = 0;
  if (rc == SQLITE_ROW) {
    uid = sqlite3_column_int64(st, 0);
    spot = sqlite3_column_int64(st, 1);
    verified_ms = sqlite3_column_int64(st, 2);
    rc = sqlite3_step(st);
  } else if (rc == SQLITE_DONE) {
    rc = SQLITE_NOTFOUND;
  }
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return rc;

  /* roadmap 7.5: +1 per verified hour when the session ends, 4 per day */
  int points = fss_karma_award(
      uid, fss_checkin_karma(verified_ms),
      (int64_t)FSS_RULES.karma_checkin_daily * FSS_RULES.karma_checkin_hour,
      "checkin", "checkin", id, now, box);
  if (points < 0 || (!points && fss_badges_check(uid, now, box) < 0))
    return SQLITE_ERROR;
  if (karma)
    *karma = points;
  fss_live_state_s live;
  return fss_live_update(spot, now, &live, box);
}

static const char SQL_CHECKIN_JSON[] =
    "SELECT json_object('id', id, 'spot', spot_id, 'start_at', start_at,"
    " 'end_at', end_at, 'end_reason', end_reason,"
    " 'last_beat_at', last_beat_at,"
    " 'in_fence', json(CASE WHEN last_in THEN 'true' ELSE 'false' END),"
    " 'verified', json(CASE WHEN verified THEN 'true' ELSE 'false' END),"
    " 'verified_ms', verified_ms, 'outside_beats', outside_beats)"
    " FROM checkin WHERE id = ?1";

char *fss_checkin_json(char *dest, int64_t id) {
  sqlite3_stmt *st = fss_stmt(SQL_CHECKIN_JSON);
  if (!st)
    return NULL;
  sqlite3_bind_int64(st, 1, id);
  char *out = NULL;
  if (sqlite3_step(st) == SQLITE_ROW)
    out = fio_bstr_write(dest, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
  fss_stmt_release(st);
  return out;
}

/* Candidates: past the time limit or without a heartbeat for too long. */
static const char SQL_CHECKIN_EXPIRED[] =
    "SELECT id, start_at, COALESCE(last_beat_at, start_at) FROM checkin"
    " WHERE end_at IS NULL AND (start_at <= ?1 - ?2"
    "   OR COALESCE(last_beat_at, start_at) <= ?1 - ?3)";

int fss_checkin_timeout_all(int64_t now) {
  if (fss_tx_begin() != SQLITE_OK)
    return -1;
  fss_outbox_s box = {0};
  struct {
    int64_t id, end_at;
    int why;
  } *rows = NULL;
  size_t n = 0, cap = 0;
  sqlite3_stmt *st = fss_stmt(SQL_CHECKIN_EXPIRED);
  int rc = SQLITE_ERROR;
  if (st) {
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int64(st, 2, FSS_RULES.checkin_max_ms);
    sqlite3_bind_int64(st, 3, FSS_RULES.checkin_stale_ms);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      int64_t end_at;
      int why = fss_checkin_expired(sqlite3_column_int64(st, 1),
                                    sqlite3_column_int64(st, 2), now, &end_at);
      if (!why)
        continue;
      if (n == cap) {
        cap = cap ? cap * 2 : 32;
        void *p = realloc(rows, cap * sizeof(*rows));
        if (!p) {
          rc = SQLITE_NOMEM;
          break;
        }
        rows = p;
      }
      rows[n].id = sqlite3_column_int64(st, 0);
      rows[n].end_at = end_at;
      rows[n++].why = why;
    }
    fss_stmt_release(st);
  }
  if (rc == SQLITE_DONE)
    rc = SQLITE_OK;
  for (size_t i = 0; i < n && rc == SQLITE_OK; ++i) {
    rc = fss_checkin_close(rows[i].id, rows[i].end_at,
                           rows[i].why == FSS_CHECKIN_STALE ? "stale" : "timeout",
                           now, NULL, &box);
    if (rc == SQLITE_NOTFOUND)
      rc = SQLITE_OK;
  }
  free(rows);
  if (rc != SQLITE_OK) {
    fss_outbox_clear(&box);
    fss_tx_rollback();
    return -1;
  }
  return fss_live_commit(&box) == SQLITE_OK ? (int)n : -1;
}
