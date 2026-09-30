#include "auth.h"

#include "db.h"
#include "http.h"
#include "rules.h"

#include <stdio.h>
#include <string.h>

/* Argon2id: 19 MiB, 4 passes, 1 lane (OWASP minimum is 19 MiB / 2 passes;
 * passes were doubled to reach ~100 ms here while keeping memory per
 * concurrent login low). Re-tune with
 * `fss --bench-password` so a hash takes roughly 100 ms (roadmap 10.5). */
fss_pw_params_s FSS_PW_PARAMS = {.m_cost = 19456, .t_cost = 4, .parallelism = 1};

static int pw_compute(const char *pw, size_t len, const uint8_t *salt,
                      size_t salt_len, uint8_t out[FSS_PW_HASH_LEN],
                      fss_pw_params_s p) {
  return fio_argon2_hash(out, .password = FIO_BUF_INFO2((char *)pw, len),
                         .salt = FIO_BUF_INFO2((char *)salt, salt_len),
                         .t_cost = p.t_cost, .m_cost = p.m_cost,
                         .parallelism = p.parallelism,
                         .outlen = FSS_PW_HASH_LEN, .type = FIO_ARGON2ID);
}

int fss_pw_hash(const char *pw, size_t len, uint8_t salt[FSS_PW_SALT_LEN],
                uint8_t hash[FSS_PW_HASH_LEN], fss_pw_params_s params) {
  if (fio_rand_bytes_secure(salt, FSS_PW_SALT_LEN))
    return -1;
  return pw_compute(pw, len, salt, FSS_PW_SALT_LEN, hash, params);
}

int fss_pw_verify(const char *pw, size_t len, const uint8_t *salt,
                  size_t salt_len, const uint8_t *hash, size_t hash_len,
                  fss_pw_params_s params) {
  uint8_t out[FSS_PW_HASH_LEN];
  if (hash_len != FSS_PW_HASH_LEN ||
      pw_compute(pw, len, salt, salt_len, out, params))
    return 0;
  return fio_ct_is_eq(out, hash, FSS_PW_HASH_LEN) ? 1 : 0;
}

void fss_pw_params_str(fss_pw_params_s p, char *out, size_t cap) {
  snprintf(out, cap, "argon2id:m=%u,t=%u,p=%u", p.m_cost, p.t_cost,
           p.parallelism);
}

int fss_pw_params_parse(const char *s, fss_pw_params_s *p) {
  unsigned m, t, par;
  if (!s || sscanf(s, "argon2id:m=%u,t=%u,p=%u", &m, &t, &par) != 3 || !t ||
      !par || m < 8 * par)
    return -1;
  *p = (fss_pw_params_s){.m_cost = m, .t_cost = t, .parallelism = par};
  return 0;
}

/* *****************************************************************************
Sessions: the cookie carries 32 random bytes (hex); the database only stores
blake2b-256 of the cookie value.
***************************************************************************** */

static void token_hash(const char *token, size_t len, uint8_t out[32]) {
  fio_blake2b_hash(out, 32, token, len, NULL, 0);
}

static void set_cookie(fio_http_s *h, const char *value, size_t len,
                       int max_age) {
  fio_http_cookie_set(h, .name = FIO_STR_INFO1((char *)FSS_SESSION_COOKIE),
                      .value = FIO_STR_INFO2((char *)value, len),
                      .path = FIO_STR_INFO1((char *)"/"), .max_age = max_age,
                      .same_site = FIO_HTTP_COOKIE_SAME_SITE_LAX,
                      .http_only = 1);
}

static const char SQL_SESSION_INSERT[] =
    "INSERT INTO auth_session (token_hash, user_id, created_at, expires_at)"
    " VALUES (?1, ?2, ?3, ?4)";

int fss_session_create(fio_http_s *h, int64_t user_id) {
  static const char hex[] = "0123456789abcdef";
  uint8_t raw[32];
  char token[FSS_TOKEN_HEX_LEN];
  if (fio_rand_bytes_secure(raw, sizeof(raw)))
    return -1;
  for (size_t i = 0; i < sizeof(raw); ++i) {
    token[i * 2] = hex[raw[i] >> 4];
    token[i * 2 + 1] = hex[raw[i] & 15];
  }
  uint8_t th[32];
  token_hash(token, sizeof(token), th);
  int64_t now = fss_now_ms();
  sqlite3_stmt *st = fss_stmt(SQL_SESSION_INSERT);
  if (!st)
    return -1;
  sqlite3_bind_blob(st, 1, th, sizeof(th), SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, user_id);
  sqlite3_bind_int64(st, 3, now);
  sqlite3_bind_int64(st, 4, now + FSS_RULES.session_ttl_ms);
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return -1;
  set_cookie(h, token, sizeof(token), (int)(FSS_RULES.session_ttl_ms / 1000));
  return 0;
}

static const char SQL_SESSION_DELETE[] =
    "DELETE FROM auth_session WHERE token_hash = ?1";

void fss_session_destroy(fio_http_s *h) {
  fio_str_info_s c = fio_http_cookie(h, FSS_SESSION_COOKIE,
                                     sizeof(FSS_SESSION_COOKIE) - 1);
  if (c.len == FSS_TOKEN_HEX_LEN) {
    uint8_t th[32];
    token_hash(c.buf, c.len, th);
    sqlite3_stmt *st = fss_stmt(SQL_SESSION_DELETE);
    if (st) {
      sqlite3_bind_blob(st, 1, th, sizeof(th), SQLITE_TRANSIENT);
      sqlite3_step(st);
      fss_stmt_release(st);
    }
  }
  set_cookie(h, NULL, 0, 0); /* an empty value deletes the cookie */
}

static const char SQL_SESSION_LOOKUP[] =
    "SELECT user_id FROM auth_session WHERE token_hash = ?1 AND expires_at > ?2";

int64_t fss_auth_user(fio_http_s *h) {
  fio_str_info_s c = fio_http_cookie(h, FSS_SESSION_COOKIE,
                                     sizeof(FSS_SESSION_COOKIE) - 1);
  if (c.len != FSS_TOKEN_HEX_LEN)
    return 0;
  uint8_t th[32];
  token_hash(c.buf, c.len, th);
  sqlite3_stmt *st = fss_stmt(SQL_SESSION_LOOKUP);
  if (!st)
    return 0;
  sqlite3_bind_blob(st, 1, th, sizeof(th), SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, fss_now_ms());
  int64_t uid = 0;
  if (sqlite3_step(st) == SQLITE_ROW)
    uid = sqlite3_column_int64(st, 0);
  fss_stmt_release(st);
  return uid;
}

int64_t fss_require_user(fio_http_s *h) {
  int64_t uid = fss_auth_user(h);
  if (!uid)
    fss_send_error(h, 401, "unauthorized", "login required");
  return uid;
}

int fss_check_origin(fio_http_s *h) {
  fio_str_info_s origin =
      fio_http_request_header(h, FIO_STR_INFO1("origin"), 0);
  if (!origin.len)
    return 0; /* non-browser clients do not send Origin */
  fio_str_info_s host = fio_http_request_header(h, FIO_STR_INFO1("host"), 0);
  const char *sep = memmem(origin.buf, origin.len, "://", 3);
  if (sep && host.len) {
    const char *o_host = sep + 3;
    size_t o_len = origin.len - (size_t)(o_host - origin.buf);
    if (o_len == host.len && !memcmp(o_host, host.buf, host.len))
      return 0;
  }
  fss_send_error(h, 403, "bad_origin", "cross-origin request rejected");
  return -1;
}
