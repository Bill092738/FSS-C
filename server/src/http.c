#include "http.h"

#include "db.h"
#include "util.h"

#include <string.h>
#include <time.h>

static void bstr_dealloc(void *p) { fio_bstr_free((char *)p); }

static void set_json_type(fio_http_s *h) {
  fio_http_response_header_set(h, FIO_STR_INFO1("content-type"),
                               FIO_STR_INFO1("application/json; charset=utf-8"));
  fio_http_response_header_set(h, FIO_STR_INFO1("cache-control"),
                               FIO_STR_INFO1("no-store"));
}

void fss_send_json(fio_http_s *h, size_t status, char *json) {
  fio_http_status_set(h, status);
  set_json_type(h);
  fio_http_write(h, .buf = json, .len = fio_bstr_len(json),
                 .dealloc = bstr_dealloc, .finish = 1);
}

void fss_send_json_buf(fio_http_s *h, size_t status, const char *json,
                       size_t len) {
  fio_http_status_set(h, status);
  set_json_type(h);
  fio_http_write(h, .buf = json, .len = len, .copy = 1, .finish = 1);
}

char *fss_bstr_json_str(char *dest, const char *s, size_t len) {
  dest = fio_bstr_write(dest, "\"", 1);
  dest = fio_bstr_write_escape(dest, s, len);
  return fio_bstr_write(dest, "\"", 1);
}

void fss_send_error(fio_http_s *h, size_t status, const char *code,
                    const char *message) {
  char *out = fio_bstr_write(NULL, "{\"error\":{\"code\":", 17);
  out = fss_bstr_json_str(out, code, strlen(code));
  out = fio_bstr_write(out, ",\"message\":", 11);
  out = fss_bstr_json_str(out, message, strlen(message));
  out = fio_bstr_write(out, "}}", 2);
  fss_send_json(h, status, out);
}

void fss_send_db_error(fio_http_s *h) {
  sqlite3 *db = fss_db();
  FIO_LOG_ERROR("database error: %s", db ? sqlite3_errmsg(db) : "no connection");
  fss_send_error(h, 500, "db_error", "database error");
}

long fss_query(fio_http_s *h, const char *name, char *out, size_t cap) {
  fio_str_info_s q = fio_http_query(h);
  if (!q.buf || !q.len)
    return -1;
  return fss_query_get(q.buf, q.len, name, out, cap);
}

FIOBJ fss_body_json(fio_http_s *h) {
  fio_str_info_s ct =
      fio_http_request_header(h, FIO_STR_INFO1("content-type"), 0);
  if (!ct.buf || ct.len < 16 || strncasecmp(ct.buf, "application/json", 16)) {
    fss_send_error(h, 415, "unsupported_media_type",
                   "Content-Type must be application/json");
    return FIOBJ_INVALID;
  }
  size_t len = fio_http_body_length(h);
  fio_http_body_seek(h, 0);
  fio_str_info_s body = fio_http_body_read(h, len);
  size_t consumed = 0;
  FIOBJ o = body.len ? fiobj_json_parse(body, &consumed) : FIOBJ_INVALID;
  if (!FIOBJ_TYPE_IS(o, FIOBJ_T_HASH)) {
    fiobj_free(o);
    fss_send_error(h, 400, "bad_json", "request body must be a JSON object");
    return FIOBJ_INVALID;
  }
  return o;
}

int FSS_TEST_CLOCK = 0;
static __thread int64_t tl_now_override;

int64_t fss_wall_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t fss_now_ms(void) {
  return tl_now_override ? tl_now_override : fss_wall_ms();
}

int fss_clock_from_request(fio_http_s *h) {
  if (!FSS_TEST_CLOCK)
    return 0;
  fio_str_info_s v = fio_http_request_header(h, FIO_STR_INFO1("x-fss-now"), 0);
  if (!v.len)
    return 0;
  int64_t ms;
  if (fss_parse_i64(v.buf, v.len, &ms) || ms <= 0) {
    fss_send_error(h, 400, "bad_clock", "X-FSS-Now must be epoch milliseconds");
    return -1;
  }
  tl_now_override = ms;
  return 0;
}

void fss_clock_reset(void) { tl_now_override = 0; }
