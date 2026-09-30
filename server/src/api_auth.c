/* /auth/register, /auth/login, /auth/logout, GET /me, PATCH /me. */
#include "api.h"

#include "auth.h"
#include "db.h"

#include <string.h>

/* *****************************************************************************
Helpers
***************************************************************************** */

/* Returns a string field or an empty info when missing / not a string. */
static fio_str_info_s str_field(FIOBJ o, const char *key) {
  FIOBJ v = fiobj_hash_get2(o, key, strlen(key));
  if (!FIOBJ_TYPE_IS(v, FIOBJ_T_STRING))
    return (fio_str_info_s){0};
  return fiobj2cstr(v);
}

static int email_valid(fio_str_info_s e) {
  if (e.len < 3 || e.len > 254)
    return 0;
  const char *at = memchr(e.buf, '@', e.len);
  if (!at || at == e.buf || memchr(at + 1, '@', e.len - (at + 1 - e.buf)))
    return 0;
  const char *dot = memchr(at + 1, '.', e.len - (at + 1 - e.buf));
  if (!dot || dot == at + 1 || dot == e.buf + e.len - 1)
    return 0;
  for (size_t i = 0; i < e.len; ++i)
    if ((unsigned char)e.buf[i] <= ' ')
      return 0;
  return 1;
}

static const char SQL_USER_JSON[] =
    "SELECT json_object('id', u.id, 'email', u.email,"
    " 'display_name', u.display_name,"
    " 'campus', (SELECT slug FROM campus WHERE id = u.campus_id),"
    " 'major', u.major,"
    " 'langs', json(COALESCE(u.langs_json, '[]')),"
    " 'courses', json(COALESCE(u.courses_json, '[]')),"
    " 'role', u.role, 'reputation', u.reputation, 'karma', u.karma,"
    " 'badges', (SELECT json_group_array(json_object('key', b.key,"
    "     'name', b.name, 'at', ub.at))"
    "   FROM user_badge ub JOIN badge b ON b.key = ub.badge_key"
    "   WHERE ub.user_id = u.id))"
    " FROM user u WHERE u.id = ?1";

/* Sends {"user":{...}} for `uid`. */
static void send_user(fio_http_s *h, size_t status, int64_t uid) {
  sqlite3_stmt *st = fss_stmt(SQL_USER_JSON);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_int64(st, 1, uid);
  if (sqlite3_step(st) == SQLITE_ROW) {
    char *out = fio_bstr_write(NULL, "{\"user\":", 8);
    out = fio_bstr_write(out, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
    out = fio_bstr_write(out, "}", 1);
    fss_send_json(h, status, out);
  } else {
    fss_send_error(h, 404, "not_found", "user not found");
  }
  fss_stmt_release(st);
}

static const char SQL_CAMPUS_ID[] = "SELECT id FROM campus WHERE slug = ?1";

/* Resolves a campus slug; returns 0 if unknown. */
static int64_t campus_id(fio_str_info_s slug) {
  sqlite3_stmt *st = fss_stmt(SQL_CAMPUS_ID);
  if (!st)
    return 0;
  sqlite3_bind_text(st, 1, slug.buf, (int)slug.len, SQLITE_STATIC);
  int64_t id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  return id;
}

/* *****************************************************************************
Register / login / logout
***************************************************************************** */

static const char SQL_USER_INSERT[] =
    "INSERT INTO user (email, pw_hash, pw_salt, pw_params, display_name,"
    " campus_id, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)";

void api_auth_register(fio_http_s *h, fss_params_s *p) {
  (void)p;
  if (fss_check_origin(h))
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  fio_str_info_s email = str_field(body, "email");
  fio_str_info_s pw = str_field(body, "password");
  fio_str_info_s name = str_field(body, "display_name");
  fio_str_info_s campus = str_field(body, "campus");
  if (!email_valid(email)) {
    fss_send_error(h, 422, "invalid_email", "a valid email is required");
    goto done;
  }
  if (pw.len < 8 || pw.len > 256) {
    fss_send_error(h, 422, "invalid_password",
                   "password must be 8 to 256 bytes long");
    goto done;
  }
  if (name.len > 60) {
    fss_send_error(h, 422, "invalid_display_name",
                   "display_name must be at most 60 bytes");
    goto done;
  }

  /* hash outside of the write transaction (roadmap 5.1 / 10.5) */
  uint8_t salt[FSS_PW_SALT_LEN], hash[FSS_PW_HASH_LEN];
  char params[64];
  fss_pw_params_str(FSS_PW_PARAMS, params, sizeof(params));
  if (fss_pw_hash(pw.buf, pw.len, salt, hash, FSS_PW_PARAMS)) {
    fss_send_error(h, 500, "internal", "password hashing failed");
    goto done;
  }

  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    goto done;
  }
  int64_t cid = campus.len ? campus_id(campus) : 0;
  if (campus.len && !cid) {
    fss_tx_rollback();
    fss_send_error(h, 422, "unknown_campus", "unknown campus");
    goto done;
  }
  sqlite3_stmt *st = fss_stmt(SQL_USER_INSERT);
  int rc = SQLITE_ERROR;
  if (st) {
    sqlite3_bind_text(st, 1, email.buf, (int)email.len, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, hash, sizeof(hash), SQLITE_STATIC);
    sqlite3_bind_blob(st, 3, salt, sizeof(salt), SQLITE_STATIC);
    sqlite3_bind_text(st, 4, params, -1, SQLITE_STATIC);
    if (name.len)
      sqlite3_bind_text(st, 5, name.buf, (int)name.len, SQLITE_STATIC);
    if (cid)
      sqlite3_bind_int64(st, 6, cid);
    sqlite3_bind_int64(st, 7, fss_now_ms());
    rc = sqlite3_step(st);
    fss_stmt_release(st);
  }
  if (rc == SQLITE_CONSTRAINT) {
    fss_tx_rollback();
    fss_send_error(h, 409, "email_taken", "email already registered");
    goto done;
  }
  int64_t uid = sqlite3_last_insert_rowid(fss_db());
  if (rc != SQLITE_DONE || fss_session_create(h, uid) ||
      fss_tx_commit() != SQLITE_OK) {
    fss_tx_rollback();
    fss_send_db_error(h);
    goto done;
  }
  send_user(h, 201, uid);
done:
  fiobj_free(body);
}

static const char SQL_LOGIN[] =
    "SELECT id, pw_hash, pw_salt, pw_params FROM user WHERE email = ?1";

void api_auth_login(fio_http_s *h, fss_params_s *p) {
  (void)p;
  if (fss_check_origin(h))
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  fio_str_info_s email = str_field(body, "email");
  fio_str_info_s pw = str_field(body, "password");
  int64_t uid = 0;
  int ok = 0;
  if (email.len && pw.len && pw.len <= 256) {
    sqlite3_stmt *st = fss_stmt(SQL_LOGIN);
    if (!st) {
      fss_send_db_error(h);
      goto done;
    }
    sqlite3_bind_text(st, 1, email.buf, (int)email.len, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
      fss_pw_params_s params;
      uid = sqlite3_column_int64(st, 0);
      if (!fss_pw_params_parse((const char *)sqlite3_column_text(st, 3),
                               &params))
        ok = fss_pw_verify(pw.buf, pw.len, sqlite3_column_blob(st, 2),
                           (size_t)sqlite3_column_bytes(st, 2),
                           sqlite3_column_blob(st, 1),
                           (size_t)sqlite3_column_bytes(st, 1), params);
    } else {
      /* spend the same time for unknown accounts */
      uint8_t salt[FSS_PW_SALT_LEN], hash[FSS_PW_HASH_LEN];
      fss_pw_hash(pw.buf, pw.len, salt, hash, FSS_PW_PARAMS);
    }
    fss_stmt_release(st);
  }
  if (!ok) {
    fss_send_error(h, 401, "invalid_credentials", "wrong email or password");
    goto done;
  }
  if (fss_session_create(h, uid)) {
    fss_send_db_error(h);
    goto done;
  }
  send_user(h, 200, uid);
done:
  fiobj_free(body);
}

void api_auth_logout(fio_http_s *h, fss_params_s *p) {
  (void)p;
  if (fss_check_origin(h))
    return;
  fss_session_destroy(h);
  fss_send_json_buf(h, 200, "{\"ok\":true}", 11);
}

/* *****************************************************************************
Profile
***************************************************************************** */

void api_me_show(fio_http_s *h, fss_params_s *p) {
  (void)p;
  int64_t uid = fss_require_user(h);
  if (uid)
    send_user(h, 200, uid);
}

/* Validates an array of short strings; returns a JSON bstr or NULL. */
static char *string_list_json(FIOBJ v, size_t max_items, size_t max_len) {
  if (!FIOBJ_TYPE_IS(v, FIOBJ_T_ARRAY) || fiobj_array_count(v) > max_items)
    return NULL;
  char *out = fio_bstr_write(NULL, "[", 1);
  for (uint32_t i = 0; i < fiobj_array_count(v); ++i) {
    FIOBJ item = fiobj_array_get(v, i);
    fio_str_info_s s = fiobj2cstr(item);
    if (!FIOBJ_TYPE_IS(item, FIOBJ_T_STRING) || !s.len || s.len > max_len) {
      fio_bstr_free(out);
      return NULL;
    }
    if (i)
      out = fio_bstr_write(out, ",", 1);
    out = fss_bstr_json_str(out, s.buf, s.len);
  }
  return fio_bstr_write(out, "]", 1);
}

static const char SQL_ME_UPDATE[] =
    "UPDATE user SET"
    " display_name = CASE WHEN ?2 THEN ?3 ELSE display_name END,"
    " major = CASE WHEN ?4 THEN ?5 ELSE major END,"
    " langs_json = CASE WHEN ?6 THEN ?7 ELSE langs_json END,"
    " courses_json = CASE WHEN ?8 THEN ?9 ELSE courses_json END,"
    " campus_id = CASE WHEN ?10 THEN ?11 ELSE campus_id END"
    " WHERE id = ?1";

void api_me_update(fio_http_s *h, fss_params_s *p) {
  (void)p;
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  char *langs = NULL, *courses = NULL;
  FIOBJ v_name = fiobj_hash_get2(body, "display_name", 12);
  FIOBJ v_major = fiobj_hash_get2(body, "major", 5);
  FIOBJ v_langs = fiobj_hash_get2(body, "langs", 5);
  FIOBJ v_courses = fiobj_hash_get2(body, "courses", 7);
  FIOBJ v_campus = fiobj_hash_get2(body, "campus", 6);
  fio_str_info_s name = str_field(body, "display_name");
  fio_str_info_s major = str_field(body, "major");
  if ((v_name && (!FIOBJ_TYPE_IS(v_name, FIOBJ_T_STRING) || name.len > 60)) ||
      (v_major && (!FIOBJ_TYPE_IS(v_major, FIOBJ_T_STRING) || major.len > 60))) {
    fss_send_error(h, 422, "invalid_field",
                   "display_name and major must be strings of at most 60 bytes");
    goto done;
  }
  if ((v_langs && !(langs = string_list_json(v_langs, 8, 16))) ||
      (v_courses && !(courses = string_list_json(v_courses, 20, 32)))) {
    fss_send_error(h, 422, "invalid_field",
                   "langs (<= 8) and courses (<= 20) must be arrays of short strings");
    goto done;
  }
  int64_t cid = 0;
  if (v_campus) {
    fio_str_info_s slug = str_field(body, "campus");
    if (!slug.len || !(cid = campus_id(slug))) {
      fss_send_error(h, 422, "unknown_campus", "unknown campus");
      goto done;
    }
  }
  sqlite3_stmt *st = fss_stmt(SQL_ME_UPDATE);
  if (!st) {
    fss_send_db_error(h);
    goto done;
  }
  sqlite3_bind_int64(st, 1, uid);
  sqlite3_bind_int(st, 2, v_name != FIOBJ_INVALID);
  sqlite3_bind_text(st, 3, name.buf, (int)name.len, SQLITE_STATIC);
  sqlite3_bind_int(st, 4, v_major != FIOBJ_INVALID);
  sqlite3_bind_text(st, 5, major.buf, (int)major.len, SQLITE_STATIC);
  sqlite3_bind_int(st, 6, langs != NULL);
  if (langs)
    sqlite3_bind_text(st, 7, langs, (int)fio_bstr_len(langs), SQLITE_STATIC);
  sqlite3_bind_int(st, 8, courses != NULL);
  if (courses)
    sqlite3_bind_text(st, 9, courses, (int)fio_bstr_len(courses), SQLITE_STATIC);
  sqlite3_bind_int(st, 10, cid != 0);
  sqlite3_bind_int64(st, 11, cid);
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    fss_send_db_error(h);
  else
    send_user(h, 200, uid);
done:
  fio_bstr_free(langs);
  fio_bstr_free(courses);
  fiobj_free(body);
}
