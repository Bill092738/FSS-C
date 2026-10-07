/* Community endpoints (roadmap 7.5, 7.6, 10.5):
 *
 *   POST /spots/:id/confirm   {}            confirm a submitted (hidden) spot
 *   POST /spots/:id/photos    image bytes   Content-Type image/jpeg|png|webp
 *   POST /photos/:id/vote     {"v": 1 | -1 | 0}
 *   GET  /me/karma            karma ledger, newest first (limit, cursor)
 *   POST /debug/jobs/:name    runs a job now (only with --test-clock)
 */
#include "api.h"

#include "auth.h"
#include "checkin.h"
#include "community.h"
#include "db.h"
#include "live.h"
#include "rules.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const char *FSS_UPLOADS_DIR = "data/uploads";

static int step_done(sqlite3_stmt *st) {
  int rc = st ? sqlite3_step(st) : SQLITE_ERROR;
  fss_stmt_release(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

/* *****************************************************************************
Spot discovery: POST /spots/:id/confirm (roadmap 7.5)
***************************************************************************** */

static const char SQL_CONFIRM_SPOT[] =
    "SELECT status, created_by FROM spot WHERE id = ?1";
static const char SQL_CONFIRM_INSERT[] =
    "INSERT INTO spot_confirm (spot_id, user_id, at) VALUES (?1, ?2, ?3)"
    " ON CONFLICT DO NOTHING";
static const char SQL_CONFIRM_COUNT[] =
    "SELECT count(*) FROM spot_confirm WHERE spot_id = ?1";
static const char SQL_SPOT_ACTIVATE[] =
    "UPDATE spot SET status = 'active', updated_at = ?2"
    " WHERE id = ?1 AND status = 'hidden'";

void api_spot_confirm(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  fiobj_free(body);
  int64_t spot = p->num[0], now = fss_now_ms();
  fss_outbox_s box = {0};
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_stmt *st = fss_stmt(SQL_CONFIRM_SPOT);
  if (!st)
    goto db_error;
  sqlite3_bind_int64(st, 1, spot);
  int rc = sqlite3_step(st);
  char status[16] = {0};
  int64_t creator = 0;
  if (rc == SQLITE_ROW) {
    const char *s = (const char *)sqlite3_column_text(st, 0);
    snprintf(status, sizeof(status), "%s", s ? s : "");
    creator = sqlite3_column_int64(st, 1);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    goto db_error;
  const char *code = NULL, *msg = NULL;
  size_t http = 0;
  if (rc == SQLITE_DONE || !strcmp(status, "merged"))
    http = 404, code = "not_found", msg = "unknown spot";
  else if (strcmp(status, "hidden"))
    http = 409, code = "already_active", msg = "the spot is already active";
  else if (creator == uid)
    http = 403, code = "own_spot", msg = "other users must confirm your spot";
  if (code) {
    fss_tx_rollback();
    fss_send_error(h, http, code, msg);
    return;
  }
  if (!(st = fss_stmt(SQL_CONFIRM_INSERT)))
    goto db_error;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_int64(st, 2, uid);
  sqlite3_bind_int64(st, 3, now);
  if (step_done(st) || !(st = fss_stmt(SQL_CONFIRM_COUNT)))
    goto db_error;
  sqlite3_bind_int64(st, 1, spot);
  rc = sqlite3_step(st);
  int64_t confirmations = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW)
    goto db_error;
  int active = confirmations >= FSS_RULES.spot_confirm_min;
  if (active) {
    if (!(st = fss_stmt(SQL_SPOT_ACTIVATE)))
      goto db_error;
    sqlite3_bind_int64(st, 1, spot);
    sqlite3_bind_int64(st, 2, now);
    if (step_done(st))
      goto db_error;
    /* +20 for the discoverer, 2 per day; the activation is also an
     * accepted contribution (roadmap 7.5 / 7.6) */
    if (creator) {
      int k = fss_karma_award(creator, FSS_RULES.karma_spot,
                              (int64_t)FSS_RULES.karma_spot_daily *
                                  FSS_RULES.karma_spot,
                              "spot_discovered", "spot", spot, now, &box);
      if (k < 0 || (!k && fss_badges_check(creator, now, &box) < 0) ||
          fss_rep_adjust(creator, FSS_RULES.rep_accept, "spot_discovered",
                         "spot", spot, now, &box) < 0)
        goto db_error;
    }
  }
  if (fss_live_commit(&box) != SQLITE_OK)
    goto db_error;
  char *out = fio_bstr_printf(
      NULL,
      "{\"spot_id\":%lld,\"status\":\"%s\",\"confirmations\":%lld,"
      "\"needed\":%d}",
      (long long)spot, active ? "active" : "hidden", (long long)confirmations,
      FSS_RULES.spot_confirm_min);
  fss_send_json(h, 200, out);
  return;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
}

/* *****************************************************************************
Photos (roadmap 7.5 / 10.5): the raw image is the request body. Only JPEG,
PNG and WebP are accepted, recognized by their magic bytes; the file is
stored as <sha256>.<ext>. No image processing happens in C.
***************************************************************************** */

static const char SQL_PHOTO_SPOT[] =
    "SELECT 1 FROM spot WHERE id = ?1 AND status != 'merged'";
static const char SQL_PHOTO_TODAY[] =
    "SELECT count(*) FROM photo WHERE user_id = ?1 AND at > ?2";
static const char SQL_PHOTO_BY_SHA[] = "SELECT id FROM photo WHERE sha256 = ?1";
static const char SQL_PHOTO_INSERT[] =
    "INSERT INTO photo (spot_id, user_id, path, sha256, at)"
    " VALUES (?1, ?2, ?3, ?4, ?5)";
static const char SQL_PHOTO_JSON[] =
    "SELECT json_object('id', id, 'spot', spot_id, 'url', '/uploads/' || path,"
    " 'up', round(up, 2), 'down', round(down, 2),"
    " 'p', round(fss_conf(?2, up, down), 3), 'status', status, 'at', at)"
    " FROM photo WHERE id = ?1";

/* Returns the file extension for a supported image, else NULL. */
static const char *image_ext(const uint8_t *b, size_t len) {
  if (len >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
    return "jpg";
  if (len >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8))
    return "png";
  if (len >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4))
    return "webp";
  return NULL;
}

/* Writes the file atomically (temporary name, then rename). */
static int store_file(const char *name, const char *data, size_t len) {
  char path[1024], tmp[1100];
  snprintf(path, sizeof(path), "%s/%s", FSS_UPLOADS_DIR, name);
  snprintf(tmp, sizeof(tmp), "%s/.%s.%d.tmp", FSS_UPLOADS_DIR, name,
           (int)getpid());
  if (!access(path, F_OK)) /* content-addressed: same name, same bytes */
    return 0;
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    FIO_LOG_ERROR("upload: cannot create %s: %s", tmp, strerror(errno));
    return -1;
  }
  int ok = fwrite(data, 1, len, f) == len;
  ok = !fclose(f) && ok;
  if (!ok || rename(tmp, path)) {
    FIO_LOG_ERROR("upload: cannot store %s: %s", path, strerror(errno));
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void send_photo(fio_http_s *h, size_t status, int64_t id, int karma) {
  sqlite3_stmt *st = fss_stmt(SQL_PHOTO_JSON);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_double(st, 2, FSS_RULES.claim_user_prior);
  if (sqlite3_step(st) == SQLITE_ROW) {
    char *out = fio_bstr_write(NULL, "{\"photo\":", 9);
    out = fio_bstr_write(out, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
    if (karma >= 0)
      out = fio_bstr_printf(out, ",\"karma\":%d", karma);
    fss_send_json(h, status, fio_bstr_write(out, "}", 1));
  } else {
    fss_send_db_error(h);
  }
  fss_stmt_release(st);
}

static void send_duplicate_photo(fio_http_s *h, int64_t id) {
  fss_send_json(h, 409,
                fio_bstr_printf(NULL,
                                "{\"error\":{\"code\":\"duplicate_photo\","
                                "\"message\":\"this photo was already "
                                "uploaded\",\"photo_id\":%lld}}",
                                (long long)id));
}

static int64_t scalar_i64(const char *sql, int64_t a, int64_t b, int *err) {
  sqlite3_stmt *st = fss_stmt(sql);
  if (!st) {
    *err = 1;
    return 0;
  }
  sqlite3_bind_int64(st, 1, a);
  if (sqlite3_bind_parameter_count(st) > 1)
    sqlite3_bind_int64(st, 2, b);
  int rc = sqlite3_step(st);
  int64_t v = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    *err = 1;
  fss_stmt_release(st);
  return v;
}

void api_spot_photos_create(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  /* image types cannot be sent cross-site by an HTML form, which keeps the
   * CSRF baseline of the JSON endpoints (roadmap 10.5) */
  fio_str_info_s ct =
      fio_http_request_header(h, FIO_STR_INFO1("content-type"), 0);
  if (!ct.buf || ct.len < 6 || strncasecmp(ct.buf, "image/", 6)) {
    fss_send_error(h, 415, "unsupported_media_type",
                   "send the image itself as image/jpeg, image/png or"
                   " image/webp");
    return;
  }
  size_t len = fio_http_body_length(h);
  if (!len) {
    fss_send_error(h, 422, "empty_photo", "the request body is empty");
    return;
  }
  if ((int64_t)len > FSS_RULES.photo_max_bytes) {
    fss_send_error(h, 413, "photo_too_large", "photos are limited to 5 MB");
    return;
  }
  int64_t spot = p->num[0], now = fss_now_ms();
  int err = 0;
  if (!scalar_i64(SQL_PHOTO_SPOT, spot, 0, &err) || err) {
    if (err)
      fss_send_db_error(h);
    else
      fss_send_error(h, 404, "not_found", "unknown spot");
    return;
  }
  fio_http_body_seek(h, 0);
  fio_str_info_s data = fio_http_body_read(h, len);
  const char *ext = image_ext((const uint8_t *)data.buf, data.len);
  if (data.len != len || !ext) {
    fss_send_error(h, 415, "unsupported_image",
                   "only JPEG, PNG and WebP images are accepted");
    return;
  }
  fio_u256 digest = fio_sha256(data.buf, data.len);
  static const char hex[] = "0123456789abcdef";
  char name[80];
  for (size_t i = 0; i < 32; ++i) {
    name[i * 2] = hex[digest.u8[i] >> 4];
    name[i * 2 + 1] = hex[digest.u8[i] & 15];
  }
  snprintf(name + 64, sizeof(name) - 64, ".%s", ext);

  int64_t existing = 0;
  sqlite3_stmt *st = fss_stmt(SQL_PHOTO_BY_SHA);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_blob(st, 1, digest.u8, 32, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  existing = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    fss_send_db_error(h);
    return;
  }
  if (existing) {
    send_duplicate_photo(h, existing);
    return;
  }
  if (scalar_i64(SQL_PHOTO_TODAY, uid, now - 24LL * 3600 * 1000, &err) >=
          FSS_RULES.photo_daily_max ||
      err) {
    if (err)
      fss_send_db_error(h);
    else
      fss_send_error(h, 429, "rate_limited", "daily photo upload limit reached");
    return;
  }
  /* file IO stays outside the write transaction */
  if (store_file(name, data.buf, data.len)) {
    fss_send_error(h, 500, "storage_error", "the photo could not be stored");
    return;
  }

  fss_outbox_s box = {0};
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    return;
  }
  if (!(st = fss_stmt(SQL_PHOTO_INSERT)))
    goto db_error;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_int64(st, 2, uid);
  sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
  sqlite3_bind_blob(st, 4, digest.u8, 32, SQLITE_STATIC);
  sqlite3_bind_int64(st, 5, now);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc == SQLITE_CONSTRAINT) { /* a concurrent upload of the same bytes */
    fss_tx_rollback();
    if (!(st = fss_stmt(SQL_PHOTO_BY_SHA))) {
      fss_send_db_error(h);
      return;
    }
    sqlite3_bind_blob(st, 1, digest.u8, 32, SQLITE_STATIC);
    existing = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
    fss_stmt_release(st);
    send_duplicate_photo(h, existing);
    return;
  }
  if (rc != SQLITE_DONE)
    goto db_error;
  int64_t id = sqlite3_last_insert_rowid(fss_db());
  int karma = fss_karma_award(
      uid, FSS_RULES.karma_photo,
      (int64_t)FSS_RULES.karma_photo_daily * FSS_RULES.karma_photo, "photo",
      "photo", id, now, &box);
  if (karma < 0 || (!karma && fss_badges_check(uid, now, &box) < 0) ||
      fss_live_commit(&box) != SQLITE_OK)
    goto db_error;
  send_photo(h, 201, id, karma);
  return;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
}

/* *****************************************************************************
Photo votes: POST /photos/:id/vote {"v": 1 | -1 | 0} (roadmap 7.6)
***************************************************************************** */

static const char SQL_PHOTO_INFO[] =
    "SELECT p.user_id, s.building_id FROM photo p"
    " JOIN spot s ON s.id = p.spot_id WHERE p.id = ?1";
static const char SQL_PHOTO_VOTE_UPSERT[] =
    "INSERT INTO photo_vote (photo_id, user_id, v, weight, at)"
    " VALUES (?1, ?2, ?3, ?4, ?5)"
    " ON CONFLICT (photo_id, user_id) DO UPDATE SET"
    " v = excluded.v, weight = excluded.weight, at = excluded.at";
static const char SQL_PHOTO_VOTE_DELETE[] =
    "DELETE FROM photo_vote WHERE photo_id = ?1 AND user_id = ?2";
static const char SQL_PHOTO_TALLY[] =
    "UPDATE photo SET"
    " up = COALESCE((SELECT sum(weight) FROM photo_vote"
    "   WHERE photo_id = ?1 AND v = 1), 0),"
    " down = COALESCE((SELECT sum(weight) FROM photo_vote"
    "   WHERE photo_id = ?1 AND v = -1), 0)"
    " WHERE id = ?1"
    " RETURNING up, down, status,"
    "   (SELECT count(*) FROM photo_vote WHERE photo_id = ?1 AND v = 1)";
static const char SQL_PHOTO_HIDE[] =
    "UPDATE photo SET status = 'hidden' WHERE id = ?1";

void api_photo_vote(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  FIOBJ vv = fiobj_hash_get2(body, "v", 1);
  int64_t v = FIOBJ_TYPE_IS(vv, FIOBJ_T_NUMBER) ? fiobj2i(vv) : 2;
  fiobj_free(body);
  if (v < -1 || v > 1) {
    fss_send_error(h, 422, "invalid_vote", "v must be 1, -1 or 0");
    return;
  }
  int64_t photo = p->num[0], now = fss_now_ms();
  fss_outbox_s box = {0};
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_stmt *st = fss_stmt(SQL_PHOTO_INFO);
  if (!st)
    goto db_error;
  sqlite3_bind_int64(st, 1, photo);
  int rc = sqlite3_step(st);
  int64_t author = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  int64_t building = rc == SQLITE_ROW ? sqlite3_column_int64(st, 1) : 0;
  fss_stmt_release(st);
  if (rc == SQLITE_DONE) {
    fss_tx_rollback();
    fss_send_error(h, 404, "not_found", "unknown photo");
    return;
  }
  if (rc != SQLITE_ROW)
    goto db_error;
  if (author == uid) {
    fss_tx_rollback();
    fss_send_error(h, 403, "own_photo", "cannot vote on your own photo");
    return;
  }
  if (v) {
    double weight = fss_voter_weight(uid, building);
    if (weight < 0 || !(st = fss_stmt(SQL_PHOTO_VOTE_UPSERT)))
      goto db_error;
    sqlite3_bind_int64(st, 1, photo);
    sqlite3_bind_int64(st, 2, uid);
    sqlite3_bind_int64(st, 3, v);
    sqlite3_bind_double(st, 4, weight);
    sqlite3_bind_int64(st, 5, now);
  } else {
    if (!(st = fss_stmt(SQL_PHOTO_VOTE_DELETE)))
      goto db_error;
    sqlite3_bind_int64(st, 1, photo);
    sqlite3_bind_int64(st, 2, uid);
  }
  if (step_done(st) || !(st = fss_stmt(SQL_PHOTO_TALLY)))
    goto db_error;
  sqlite3_bind_int64(st, 1, photo);
  rc = sqlite3_step(st);
  double up = 0, down = 0;
  int visible = 0;
  int64_t ups = 0;
  if (rc == SQLITE_ROW) {
    up = sqlite3_column_double(st, 0);
    down = sqlite3_column_double(st, 1);
    const char *status = (const char *)sqlite3_column_text(st, 2);
    visible = status && !strcmp(status, "visible");
    ups = sqlite3_column_int64(st, 3);
    rc = sqlite3_step(st);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto db_error;
  /* roadmap 7.6: hidden at p < 0.3 with D >= 3; the upload karma is taken
   * back and the author's reputation drops. Hiding is final. */
  if (visible &&
      fss_rejected_by_votes(FSS_RULES.claim_user_prior, up, down)) {
    if (!(st = fss_stmt(SQL_PHOTO_HIDE)))
      goto db_error;
    sqlite3_bind_int64(st, 1, photo);
    if (step_done(st) ||
        fss_karma_revoke(author, "photo", "photo_hidden", "photo", photo, now,
                         &box) < 0 ||
        fss_rep_adjust(author, -FSS_RULES.rep_reject, "photo_hidden", "photo",
                       photo, now, &box) < 0)
      goto db_error;
  } else if (visible && ups >= FSS_RULES.karma_claim_min_up &&
             fss_rep_adjust(author, FSS_RULES.rep_accept, "photo_accepted",
                            "photo", photo, now, &box) < 0) {
    goto db_error;
  }
  if (fss_live_commit(&box) != SQLITE_OK)
    goto db_error;
  send_photo(h, 200, photo, -1);
  return;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
}

/* *****************************************************************************
GET /me/karma?limit=N&cursor=<entry id>
***************************************************************************** */

static const char SQL_KARMA_PAGE[] =
    "SELECT json_object('id', id, 'delta', delta, 'reason', reason,"
    " 'ref_type', ref_type, 'ref_id', ref_id, 'at', at), id"
    " FROM karma_ledger WHERE user_id = ?1 AND (?2 = 0 OR id < ?2)"
    " ORDER BY id DESC LIMIT ?3";
static const char SQL_KARMA_TOTAL[] = "SELECT karma FROM user WHERE id = ?1";

void api_me_karma(fio_http_s *h, fss_params_s *p) {
  (void)p;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  char buf[32];
  long n;
  int64_t limit = 50, cursor = 0;
  if ((n = fss_query(h, "limit", buf, sizeof(buf))) >= 0 &&
      (fss_parse_i64(buf, (size_t)n, &limit) || limit < 1 || limit > 200)) {
    fss_send_error(h, 400, "bad_limit", "limit must be 1..200");
    return;
  }
  if ((n = fss_query(h, "cursor", buf, sizeof(buf))) >= 0 &&
      (fss_parse_i64(buf, (size_t)n, &cursor) || cursor < 1)) {
    fss_send_error(h, 400, "bad_cursor", "invalid cursor");
    return;
  }
  int err = 0;
  int64_t total = scalar_i64(SQL_KARMA_TOTAL, uid, 0, &err);
  sqlite3_stmt *st = err ? NULL : fss_stmt(SQL_KARMA_PAGE);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int64(st, 2, cursor);
  sqlite3_bind_int64(st, 3, limit + 1); /* one extra row: has next page? */
  char *out = fio_bstr_printf(NULL, "{\"karma\":%lld,\"entries\":[",
                              (long long)total);
  int rc, rows = 0;
  int64_t last = 0, next = 0;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (rows == limit) {
      next = last;
      break;
    }
    if (rows++)
      out = fio_bstr_write(out, ",", 1);
    out = fio_bstr_write(out, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
    last = sqlite3_column_int64(st, 1);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    fio_bstr_free(out);
    fss_send_db_error(h);
    return;
  }
  out = next ? fio_bstr_printf(out, "],\"next\":\"%lld\"}", (long long)next)
             : fio_bstr_write(out, "],\"next\":null}", 14);
  fss_send_json(h, 200, out);
}

/* *****************************************************************************
POST /debug/jobs/:name: runs a periodic job synchronously with the request's
clock, so tests and the simulator can drive time (roadmap 10.4). Exists only
on servers started with --test-clock.
***************************************************************************** */

void api_debug_job(fio_http_s *h, fss_params_s *p) {
  if (!FSS_TEST_CLOCK) {
    fss_send_error(h, 404, "not_found", "no such endpoint");
    return;
  }
  fio_str_info_s name = {.buf = (char *)p->str[0].buf, .len = p->str[0].len};
  int64_t now = fss_now_ms();
  int result;
#define FSS_IS(lit) (name.len == sizeof(lit) - 1 && !memcmp(name.buf, lit, name.len))
  if (FSS_IS("live_decay"))
    result = fss_live_decay_all(now);
  else if (FSS_IS("checkin_timeout"))
    result = fss_checkin_timeout_all(now);
  else if (FSS_IS("hourly_rollup"))
    result = fss_reputation_rollup(now);
  else {
    fss_send_error(h, 404, "not_found", "unknown job");
    return;
  }
#undef FSS_IS
  if (result < 0) {
    fss_send_db_error(h);
    return;
  }
  fss_send_json(h, 200, fio_bstr_printf(NULL, "{\"result\":%d}", result));
}
