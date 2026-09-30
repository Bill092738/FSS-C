/* Service metadata: health check and campus configuration. */
#include "api.h"

#include "db.h"

static const char SQL_USER_VERSION[] = "PRAGMA user_version";

void api_health(fio_http_s *h, fss_params_s *p) {
  (void)p;
  sqlite3_stmt *st = fss_stmt(SQL_USER_VERSION);
  if (!st || sqlite3_step(st) != SQLITE_ROW) {
    fss_stmt_release(st);
    fss_send_db_error(h);
    return;
  }
  int schema = sqlite3_column_int(st, 0);
  fss_stmt_release(st);
  char *out = fio_bstr_write2(NULL, FIO_STRING_WRITE_STR1("{\"ok\":true,\"schema\":"),
                              FIO_STRING_WRITE_NUM(schema),
                              FIO_STRING_WRITE_STR1(",\"now\":"),
                              FIO_STRING_WRITE_NUM(fss_now_ms()),
                              FIO_STRING_WRITE_STR1("}"));
  fss_send_json(h, 200, out);
}

static const char SQL_CAMPUSES[] =
    "SELECT json_object('slug', slug, 'name', name, 'tz', tz,"
    " 'bbox', json(bbox_json)) FROM campus ORDER BY name";

void api_campuses_index(fio_http_s *h, fss_params_s *p) {
  (void)p;
  sqlite3_stmt *st = fss_stmt(SQL_CAMPUSES);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  char *out = fio_bstr_write(NULL, "{\"campuses\":", 12);
  int rc = fss_json_rows(&out, st, 0);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE) {
    fio_bstr_free(out);
    fss_send_db_error(h);
    return;
  }
  out = fio_bstr_write(out, "}", 1);
  fss_send_json(h, 200, out);
}

/* The attribute registry is returned with the campus so that the front-end
 * can generate its filters without hard-coding attributes (roadmap 10.2). */
static const char SQL_CAMPUS_SHOW[] =
    "SELECT json_object("
    " 'slug', c.slug, 'name', c.name, 'tz', c.tz,"
    " 'bbox', json(c.bbox_json), 'calendar', json(c.calendar_json),"
    " 'buildings', (SELECT count(*) FROM building b WHERE b.campus_id = c.id),"
    " 'spots', (SELECT count(*) FROM spot s JOIN building b ON b.id = s.building_id"
    "           WHERE b.campus_id = c.id AND s.status = 'active'),"
    " 'attrs', (SELECT json_group_array(json_object("
    "     'key', a.key, 'kind', a.kind, 'group', a.grp, 'bit', a.bit,"
    "     'domain', json(a.domain_json), 'label_en', a.label_en,"
    "     'label_zh', a.label_zh, 'sort', a.sort))"
    "   FROM (SELECT * FROM attr_def ORDER BY sort) a))"
    " FROM campus c WHERE c.slug = ?1";

void api_campus_show(fio_http_s *h, fss_params_s *p) {
  sqlite3_stmt *st = fss_stmt(SQL_CAMPUS_SHOW);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_text(st, 1, p->str[0].buf, (int)p->str[0].len, SQLITE_STATIC);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    fss_send_json_buf(h, 200, (const char *)sqlite3_column_text(st, 0),
                      (size_t)sqlite3_column_bytes(st, 0));
  } else if (rc == SQLITE_DONE) {
    fss_send_error(h, 404, "not_found", "unknown campus");
  } else {
    fss_send_db_error(h);
  }
  fss_stmt_release(st);
}
