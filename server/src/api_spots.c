/* Spots: multi-criteria search (7.1), detail, submission, claims and votes (7.6). */
#include "api.h"

#include "attrs.h"
#include "auth.h"
#include "community.h"
#include "db.h"
#include "live.h"
#include "rules.h"
#include "spots.h"
#include "util.h"

#include <math.h>
#include <string.h>

/* *****************************************************************************
Search: GET /spots
  campus=slug  bbox=minLon,minLat,maxLon,maxLat  must=a,b  not=c
  quiet=0..3   vibe=deep_work  near=lat,lon  q=text  limit=N  cursor=offset
***************************************************************************** */

static const char SQL_SEARCH[] =
    "WITH cand AS ("
    "  SELECT s.id, s.noise, s.quality,"
    "    CASE WHEN :near THEN sqrt("
    "      ((s.lat - :lat) * 110540.0) * ((s.lat - :lat) * 110540.0) +"
    "      ((s.lon - :lon) * :lon_m) * ((s.lon - :lon) * :lon_m)) END AS dist"
    "  FROM spot s JOIN building b ON b.id = s.building_id"
    "  WHERE s.status = 'active'"
    "    AND (:campus IS NULL OR b.campus_id = :campus)"
    "    AND (:bbox = 0 OR s.building_id IN (SELECT id FROM building_rtree"
    "         WHERE min_lon <= :max_lon AND max_lon >= :min_lon"
    "           AND min_lat <= :max_lat AND max_lat >= :min_lat))"
    "    AND (s.features & :must) = :must"
    "    AND (s.features & :not) = 0"
    "    AND (:max_noise IS NULL OR s.noise <= :max_noise)"
    "    AND (:min_outlets IS NULL OR s.outlets >= :min_outlets)"
    "    AND (:vibe IS NULL OR s.vibe = :vibe)"
    "    AND (:q IS NULL OR s.id IN"
    "         (SELECT rowid FROM spot_fts WHERE spot_fts MATCH :q))"
    "), scored AS ("
    "  SELECT c.id, c.dist, o.est, o.conf, o.basis,"
    "    :w_quiet * COALESCE(c.noise, 2)"
    "    + :w_dist * COALESCE(c.dist, 0) / 100.0"
    "    + :w_live * COALESCE(o.est, 1.0)"
    "    - :w_quality * c.quality AS score"
    "  FROM cand c LEFT JOIN occupancy_live o ON o.spot_id = c.id"
    ")"
    "SELECT json_object("
    "  'id', s.id, 'name', s.name, 'floor', s.floor, 'lat', s.lat, 'lon', s.lon,"
    "  'building', json_object('id', b.id, 'name', b.name),"
    "  'features', s.features, 'noise', s.noise, 'outlets', s.outlets,"
    "  'temp', s.temp, 'capacity', s.capacity, 'vibe', s.vibe,"
    "  'quality', round(s.quality, 3),"
    "  'dist_m', CAST(round(sc.dist) AS INTEGER),"
    "  'live', CASE WHEN sc.est IS NOT NULL THEN json_object("
    "     'est', round(sc.est, 2), 'conf', round(sc.conf, 2),"
    "     'basis', sc.basis, 'color', fss_color(sc.est)) END,"
    "  'score', round(sc.score, 4))"
    " FROM scored sc JOIN spot s ON s.id = sc.id"
    " JOIN building b ON b.id = s.building_id"
    " ORDER BY sc.score, sc.id LIMIT :limit OFFSET :offset";

typedef struct {
  int64_t mask;
  int min_outlets;
  int is_must;
  const char *bad_key;
  size_t bad_len;
  const char *error;
} filter_ctx_s;

static int filter_item(const char *item, size_t len, void *udata) {
  filter_ctx_s *f = udata;
  const fss_attr_s *a = fss_attr_find(item, len);
  if (!a) {
    f->error = "unknown_attr";
  } else if (a->kind == FSS_ATTR_FLAG) {
    f->mask |= (int64_t)1 << a->bit;
    return 0;
  } else if (f->is_must && !strcmp(a->key, "outlets")) {
    f->min_outlets = 1; /* "must have outlets" == outlets >= 1 */
    return 0;
  } else {
    f->error = "attr_not_filterable";
  }
  f->bad_key = item;
  f->bad_len = len;
  return -1;
}

static void bind_named_int64(sqlite3_stmt *st, const char *name, int64_t v) {
  sqlite3_bind_int64(st, sqlite3_bind_parameter_index(st, name), v);
}
static void bind_named_double(sqlite3_stmt *st, const char *name, double v) {
  sqlite3_bind_double(st, sqlite3_bind_parameter_index(st, name), v);
}

/* Parses "a,b[,c,d]" into doubles; returns the number parsed or -1. */
static int parse_doubles(const char *s, size_t len, double *out, int max) {
  int n = 0;
  size_t pos = 0;
  while (pos <= len && n < max) {
    size_t end = pos;
    while (end < len && s[end] != ',')
      ++end;
    if (fss_parse_double(s + pos, end - pos, &out[n]))
      return -1;
    ++n;
    pos = end + 1;
    if (end == len)
      return n;
  }
  return -1;
}

static const char SQL_CAMPUS_BY_SLUG[] = "SELECT id FROM campus WHERE slug = ?1";

void api_spots_index(fio_http_s *h, fss_params_s *p) {
  (void)p;
  char buf[512], msg[160];
  long n;
  int64_t campus = 0;
  double bbox[4], near[2];
  int has_bbox = 0, has_near = 0;
  filter_ctx_s must = {.is_must = 1}, not_ = {0};
  int quiet = 0, max_noise = -1;
  double w_quiet = 0;
  char vibe[32] = {0};
  char fts[512] = {0};
  int64_t limit = FSS_RULES.search_default_limit, offset = 0;

  if ((n = fss_query(h, "campus", buf, sizeof(buf))) >= 0) {
    sqlite3_stmt *st = fss_stmt(SQL_CAMPUS_BY_SLUG);
    if (!st) {
      fss_send_db_error(h);
      return;
    }
    sqlite3_bind_text(st, 1, buf, (int)n, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
      campus = sqlite3_column_int64(st, 0);
    fss_stmt_release(st);
    if (!campus) {
      fss_send_error(h, 404, "not_found", "unknown campus");
      return;
    }
  }
  if ((n = fss_query(h, "bbox", buf, sizeof(buf))) >= 0) {
    if (parse_doubles(buf, (size_t)n, bbox, 4) != 4 || bbox[0] > bbox[2] ||
        bbox[1] > bbox[3]) {
      fss_send_error(h, 400, "bad_bbox", "bbox must be minLon,minLat,maxLon,maxLat");
      return;
    }
    has_bbox = 1;
  }
  if ((n = fss_query(h, "near", buf, sizeof(buf))) >= 0) {
    if (parse_doubles(buf, (size_t)n, near, 2) != 2 || fabs(near[0]) > 90 ||
        fabs(near[1]) > 180) {
      fss_send_error(h, 400, "bad_near", "near must be lat,lon");
      return;
    }
    has_near = 1;
  }
  filter_ctx_s *filters[2] = {&must, &not_};
  const char *names[2] = {"must", "not"};
  for (int i = 0; i < 2; ++i) {
    if ((n = fss_query(h, names[i], buf, sizeof(buf))) < 0)
      continue;
    if (fss_split_csv(buf, (size_t)n, filter_item, filters[i]) < 0) {
      snprintf(msg, sizeof(msg), "%s: attribute '%.*s' cannot be used here",
               names[i], (int)(filters[i]->bad_len > 64 ? 64 : filters[i]->bad_len),
               filters[i]->bad_key);
      fss_send_error(h, 422, filters[i]->error, msg);
      return;
    }
  }
  if (must.mask & not_.mask) {
    fss_send_error(h, 422, "conflicting_filters",
                   "an attribute cannot be in both must and not");
    return;
  }
  if ((n = fss_query(h, "quiet", buf, sizeof(buf))) >= 0) {
    int64_t q;
    if (fss_parse_i64(buf, (size_t)n, &q) ||
        fss_quiet_params((int)q, &max_noise, &w_quiet)) {
      fss_send_error(h, 400, "bad_quiet", "quiet must be 0..3");
      return;
    }
    quiet = (int)q;
  }
  if ((n = fss_query(h, "vibe", buf, sizeof(buf))) >= 0) {
    const fss_attr_s *a = fss_attr_find("vibe", 4);
    int ok = 0;
    for (size_t i = 0; a && i < a->n_enum; ++i)
      ok |= strlen(a->enums[i]) == (size_t)n && !memcmp(a->enums[i], buf, n);
    if (!ok) {
      fss_send_error(h, 400, "bad_vibe", "unknown vibe");
      return;
    }
    memcpy(vibe, buf, (size_t)n + 1);
  }
  if ((n = fss_query(h, "q", buf, sizeof(buf))) > 0 &&
      fss_fts_query(fts, sizeof(fts), buf, (size_t)n, 8) == (size_t)-1) {
    fss_send_error(h, 400, "bad_query", "search text is too long");
    return;
  }
  if ((n = fss_query(h, "limit", buf, sizeof(buf))) >= 0 &&
      (fss_parse_i64(buf, (size_t)n, &limit) || limit < 1 ||
       limit > FSS_RULES.search_max_limit)) {
    fss_send_error(h, 400, "bad_limit", "limit out of range");
    return;
  }
  if ((n = fss_query(h, "cursor", buf, sizeof(buf))) >= 0 &&
      (fss_parse_i64(buf, (size_t)n, &offset) || offset < 0 || offset > 10000)) {
    fss_send_error(h, 400, "bad_cursor", "invalid cursor");
    return;
  }
  (void)quiet;

  sqlite3_stmt *st = fss_stmt(SQL_SEARCH);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  bind_named_int64(st, ":near", has_near);
  if (has_near) {
    bind_named_double(st, ":lat", near[0]);
    bind_named_double(st, ":lon", near[1]);
    bind_named_double(st, ":lon_m", 111320.0 * cos(near[0] * M_PI / 180.0));
  }
  if (campus)
    bind_named_int64(st, ":campus", campus);
  bind_named_int64(st, ":bbox", has_bbox);
  if (has_bbox) {
    bind_named_double(st, ":min_lon", bbox[0]);
    bind_named_double(st, ":min_lat", bbox[1]);
    bind_named_double(st, ":max_lon", bbox[2]);
    bind_named_double(st, ":max_lat", bbox[3]);
  }
  bind_named_int64(st, ":must", must.mask);
  bind_named_int64(st, ":not", not_.mask);
  if (max_noise >= 0)
    bind_named_int64(st, ":max_noise", max_noise);
  if (must.min_outlets)
    bind_named_int64(st, ":min_outlets", must.min_outlets);
  if (vibe[0])
    sqlite3_bind_text(st, sqlite3_bind_parameter_index(st, ":vibe"), vibe, -1,
                      SQLITE_STATIC);
  if (fts[0])
    sqlite3_bind_text(st, sqlite3_bind_parameter_index(st, ":q"), fts, -1,
                      SQLITE_STATIC);
  bind_named_double(st, ":w_quiet", w_quiet);
  bind_named_double(st, ":w_dist", has_near ? FSS_RULES.search_w_dist : 0.0);
  bind_named_double(st, ":w_live", FSS_RULES.search_w_live);
  bind_named_double(st, ":w_quality", FSS_RULES.search_w_quality);
  bind_named_int64(st, ":limit", limit + 1); /* one extra row: has next page? */
  bind_named_int64(st, ":offset", offset);

  char *out = fio_bstr_write(NULL, "{\"spots\":[", 10);
  int rc, rows = 0;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (rows == limit) {
      ++rows;
      break;
    }
    if (rows++)
      out = fio_bstr_write(out, ",", 1);
    out = fio_bstr_write(out, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    fio_bstr_free(out);
    fss_send_db_error(h);
    return;
  }
  out = fio_bstr_write(out, "],\"next\":", 9);
  if (rows > limit)
    out = fio_bstr_write2(out, FIO_STRING_WRITE_STR1("\""),
                          FIO_STRING_WRITE_NUM(offset + limit),
                          FIO_STRING_WRITE_STR1("\""));
  else
    out = fio_bstr_write(out, "null", 4);
  out = fio_bstr_write(out, "}", 1);
  fss_send_json(h, 200, out);
}

/* *****************************************************************************
Detail: GET /spots/:id
***************************************************************************** */

static const char SQL_SPOT_SHOW[] =
    "SELECT json_object("
    " 'id', s.id, 'name', s.name, 'floor', s.floor, 'lat', s.lat, 'lon', s.lon,"
    " 'status', s.status, 'campus', c.slug,"
    " 'building', json_object('id', b.id, 'name', b.name, 'lat', b.lat,"
    "    'lon', b.lon, 'hours', json(b.hours_json)),"
    " 'features', s.features, 'noise', s.noise, 'outlets', s.outlets,"
    " 'temp', s.temp, 'capacity', s.capacity, 'vibe', s.vibe,"
    " 'attrs', json(COALESCE(s.attrs_json, '{}')),"
    " 'summary', s.summary, 'pros', json(COALESCE(s.pros_json, '[]')),"
    " 'cons', json(COALESCE(s.cons_json, '[]')), 'quality', round(s.quality, 3),"
    " 'live', (SELECT json_object('est', round(o.est, 2), 'conf', round(o.conf, 2),"
    "    'basis', o.basis, 'color', fss_color(o.est), 'at', o.updated_at)"
    "    FROM occupancy_live o WHERE o.spot_id = s.id),"
    /* visible event reports (roadmap 7.3); same rule as fss_event_update */
    " 'events', (SELECT json_group_array(json_object('kind', e.kind,"
    "    'reports', e.users, 'until', e.last + ?6)) FROM ("
    "    SELECT r.event AS kind, count(DISTINCT r.user_id) AS users,"
    "      max(r.at) AS last,"
    "      max(r.fence >= ?3 AND r.weight >= ?4 * r.fence) AS trusted"
    "    FROM report r WHERE r.spot_id = s.id AND r.event IS NOT NULL"
    "      AND r.at > ?2 - ?6 AND r.at <= ?2 GROUP BY r.event) e"
    "    WHERE e.users >= ?5 OR e.trusted),"
    /* roadmap 7.4 / 7.5: people checked in, confirmations of a submitted
     * spot, visible photos (best first) */
    " 'present', (SELECT count(*) FROM checkin k WHERE k.spot_id = s.id"
    "    AND k.end_at IS NULL AND k.verified = 1),"
    " 'confirmations', (SELECT count(*) FROM spot_confirm sc"
    "    WHERE sc.spot_id = s.id),"
    " 'photos', (SELECT json_group_array(json_object('id', ph.id,"
    "    'url', '/uploads/' || ph.path, 'up', round(ph.up, 2),"
    "    'down', round(ph.down, 2), 'at', ph.at)) FROM (SELECT * FROM photo"
    "    WHERE spot_id = s.id AND status = 'visible'"
    "    ORDER BY up - down DESC, at DESC LIMIT 20) ph),"
    " 'claims', (SELECT json_group_array(json_object("
    "    'id', cl.id, 'attr', cl.attr, 'value', json(cl.value),"
    "    'source', cl.source, 'evidence', cl.evidence, 'url', sd.url,"
    "    'p', round(fss_conf(cl.prior, cl.up, cl.down), 3),"
    "    'up', round(cl.up, 2), 'down', round(cl.down, 2), 'at', cl.created_at))"
    "    FROM claim cl LEFT JOIN source_doc sd ON sd.id = cl.source_doc_id"
    "    WHERE cl.spot_id = s.id),"
    " 'updated_at', s.updated_at)"
    " FROM spot s JOIN building b ON b.id = s.building_id"
    " JOIN campus c ON c.id = b.campus_id"
    " WHERE s.id = ?1 AND s.status != 'merged'";

static void send_spot(fio_http_s *h, size_t status, int64_t id) {
  sqlite3_stmt *st = fss_stmt(SQL_SPOT_SHOW);
  if (!st) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_int64(st, 1, id);
  sqlite3_bind_int64(st, 2, fss_now_ms());
  sqlite3_bind_double(st, 3, FSS_RULES.fence_g_inside);
  sqlite3_bind_double(st, 4, FSS_RULES.event_trusted_rep);
  sqlite3_bind_int(st, 5, FSS_RULES.event_min_users);
  sqlite3_bind_int64(st, 6, FSS_RULES.event_ttl_ms);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    char *out = fio_bstr_write(NULL, "{\"spot\":", 8);
    out = fio_bstr_write(out, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
    out = fio_bstr_write(out, "}", 1);
    fss_send_json(h, status, out);
  } else if (rc == SQLITE_DONE) {
    fss_send_error(h, 404, "not_found", "unknown spot");
  } else {
    fss_send_db_error(h);
  }
  fss_stmt_release(st);
}

void api_spot_show(fio_http_s *h, fss_params_s *p) { send_spot(h, 200, p->num[0]); }

/* *****************************************************************************
Claims
***************************************************************************** */

static const char SQL_USER_CLAIMS_TODAY[] =
    "SELECT count(*) FROM claim WHERE user_id = ?1 AND created_at > ?2";
static const char SQL_SPOT_EXISTS[] =
    "SELECT 1 FROM spot WHERE id = ?1 AND status != 'merged'";
static const char SQL_CLAIM_SAME_VALUE[] =
    "SELECT id FROM claim WHERE spot_id = ?1 AND attr = ?2 AND value = ?3";
static const char SQL_CLAIM_INSERT[] =
    "INSERT INTO claim (spot_id, attr, value, source, user_id, evidence, prior,"
    " created_at) VALUES (?1, ?2, ?3, 'user', ?4, ?5, ?6, ?7)";

#define FSS_CLAIMS_PER_DAY 10 /* roadmap 7.5 daily cap for attribute fixes */
#define FSS_EVIDENCE_MAX 300

static int64_t scalar_i64(const char *sql, int64_t a, int64_t b, int *err) {
  sqlite3_stmt *st = fss_stmt(sql);
  int64_t v = 0;
  if (!st) {
    *err = 1;
    return 0;
  }
  sqlite3_bind_int64(st, 1, a);
  if (sqlite3_bind_parameter_count(st) > 1)
    sqlite3_bind_int64(st, 2, b);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW)
    v = sqlite3_column_int64(st, 0);
  else if (rc != SQLITE_DONE)
    *err = 1;
  fss_stmt_release(st);
  return v;
}

/* Inserts one user claim; the caller owns the transaction. Returns the new
 * claim id, 0 when an identical claim already exists (`*existing` is set), or
 * -1 on database error. */
static int64_t insert_user_claim(int64_t spot, const fss_attr_s *a,
                                 const char *value, fio_str_info_s evidence,
                                 int64_t uid, int64_t *existing) {
  sqlite3_stmt *st = fss_stmt(SQL_CLAIM_SAME_VALUE);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_text(st, 2, a->key, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, value, (int)fio_bstr_len((char *)value), SQLITE_STATIC);
  int rc = sqlite3_step(st);
  *existing = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return -1;
  if (*existing)
    return 0;
  if (!(st = fss_stmt(SQL_CLAIM_INSERT)))
    return -1;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_text(st, 2, a->key, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 3, value, (int)fio_bstr_len((char *)value), SQLITE_STATIC);
  sqlite3_bind_int64(st, 4, uid);
  if (evidence.len)
    sqlite3_bind_text(st, 5, evidence.buf, (int)evidence.len, SQLITE_STATIC);
  sqlite3_bind_double(st, 6, FSS_RULES.claim_user_prior);
  sqlite3_bind_int64(st, 7, fss_now_ms());
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(fss_db()) : -1;
}

/* Parses {"attr": key, "value": v}; sends 422 and returns NULL on error. */
static const fss_attr_s *claim_from_json(fio_http_s *h, FIOBJ o, char **value) {
  FIOBJ k = fiobj_hash_get2(o, "attr", 4);
  fio_str_info_s key = FIOBJ_TYPE_IS(k, FIOBJ_T_STRING) ? fiobj2cstr(k)
                                                         : (fio_str_info_s){0};
  const fss_attr_s *a = key.len ? fss_attr_find(key.buf, key.len) : NULL;
  if (!a) {
    fss_send_error(h, 422, "unknown_attr", "unknown attribute");
    return NULL;
  }
  if (fss_attr_canonical(a, fiobj_hash_get2(o, "value", 5), value)) {
    fss_send_error(h, 422, "invalid_value", "value outside the attribute's domain");
    return NULL;
  }
  return a;
}

void api_spot_claims_create(fio_http_s *h, fss_params_s *p) {
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  char *value = NULL;
  const fss_attr_s *a = claim_from_json(h, body, &value);
  if (!a)
    goto done;
  FIOBJ ev = fiobj_hash_get2(body, "evidence", 8);
  fio_str_info_s evidence = FIOBJ_TYPE_IS(ev, FIOBJ_T_STRING)
                                ? fiobj2cstr(ev)
                                : (fio_str_info_s){0};
  if (evidence.len > FSS_EVIDENCE_MAX) {
    fss_send_error(h, 422, "invalid_evidence", "evidence is too long");
    goto done;
  }

  int err = 0;
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    goto done;
  }
  if (!scalar_i64(SQL_SPOT_EXISTS, p->num[0], 0, &err) || err) {
    fss_tx_rollback();
    if (err)
      fss_send_db_error(h);
    else
      fss_send_error(h, 404, "not_found", "unknown spot");
    goto done;
  }
  int64_t today =
      scalar_i64(SQL_USER_CLAIMS_TODAY, uid, fss_now_ms() - 24LL * 3600 * 1000, &err);
  if (!err && today >= FSS_CLAIMS_PER_DAY) {
    fss_tx_rollback();
    fss_send_error(h, 429, "rate_limited", "daily claim limit reached");
    goto done;
  }
  int64_t existing = 0;
  int64_t cid = err ? -1
                    : insert_user_claim(p->num[0], a, value, evidence, uid,
                                        &existing);
  if (cid == 0) {
    fss_tx_rollback();
    char *out = fio_bstr_write2(
        NULL, FIO_STRING_WRITE_STR1("{\"error\":{\"code\":\"duplicate_claim\","
                                    "\"message\":\"an identical claim exists;"
                                    " vote for it instead\",\"claim_id\":"),
        FIO_STRING_WRITE_NUM(existing), FIO_STRING_WRITE_STR1("}}"));
    fss_send_json(h, 409, out);
    goto done;
  }
  if (cid < 0 || fss_spot_materialize(p->num[0]) != SQLITE_OK ||
      fss_tx_commit() != SQLITE_OK) {
    fss_tx_rollback();
    fss_send_db_error(h);
    goto done;
  }
  send_spot(h, 201, p->num[0]);
done:
  fio_bstr_free(value);
  fiobj_free(body);
}

/* *****************************************************************************
Votes: POST /claims/:id/vote {"v": 1 | -1 | 0}
***************************************************************************** */

static const char SQL_CLAIM_INFO[] =
    "SELECT c.spot_id, c.user_id, s.building_id FROM claim c"
    " JOIN spot s ON s.id = c.spot_id WHERE c.id = ?1";
static const char SQL_VOTE_UPSERT[] =
    "INSERT INTO claim_vote (claim_id, user_id, v, weight, at)"
    " VALUES (?1, ?2, ?3, ?4, ?5)"
    " ON CONFLICT (claim_id, user_id) DO UPDATE SET"
    " v = excluded.v, weight = excluded.weight, at = excluded.at";
static const char SQL_VOTE_DELETE[] =
    "DELETE FROM claim_vote WHERE claim_id = ?1 AND user_id = ?2";
static const char SQL_CLAIM_TALLY[] =
    "UPDATE claim SET"
    " up = COALESCE((SELECT sum(weight) FROM claim_vote"
    "   WHERE claim_id = ?1 AND v = 1), 0),"
    " down = COALESCE((SELECT sum(weight) FROM claim_vote"
    "   WHERE claim_id = ?1 AND v = -1), 0)"
    " WHERE id = ?1";
/* The author's standing after a vote (roadmap 7.5 / 7.6). */
static const char SQL_CLAIM_STATE[] =
    "SELECT c.user_id, c.prior, c.up, c.down, (SELECT count(*) FROM claim_vote"
    " WHERE claim_id = c.id AND v = 1) FROM claim c WHERE c.id = ?1";
static const char SQL_CLAIM_JSON[] =
    "SELECT json_object('id', id, 'attr', attr, 'value', json(value),"
    " 'p', round(fss_conf(prior, up, down), 3), 'up', round(up, 2),"
    " 'down', round(down, 2)) FROM claim WHERE id = ?1";

/* A user claim with karma_claim_min_up up votes is accepted: karma and
 * reputation for its author; one that the votes reject costs reputation.
 * Both are idempotent, so later votes do not repeat them. */
static int claim_author_effects(int64_t claim, int64_t now, fss_outbox_s *box) {
  sqlite3_stmt *st = fss_stmt(SQL_CLAIM_STATE);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, claim);
  int rc = sqlite3_step(st);
  int64_t author = 0, ups = 0;
  double prior = 0, up = 0, down = 0;
  if (rc == SQLITE_ROW) {
    author = sqlite3_column_int64(st, 0);
    prior = sqlite3_column_double(st, 1);
    up = sqlite3_column_double(st, 2);
    down = sqlite3_column_double(st, 3);
    ups = sqlite3_column_int64(st, 4);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW)
    return -1;
  if (!author) /* pipeline and official claims */
    return 0;
  if (ups >= FSS_RULES.karma_claim_min_up &&
      (fss_karma_award(author, FSS_RULES.karma_claim,
                       (int64_t)FSS_RULES.karma_claim_daily * FSS_RULES.karma_claim,
                       "claim_accepted", "claim", claim, now, box) < 0 ||
       fss_rep_adjust(author, FSS_RULES.rep_accept, "claim_accepted", "claim",
                      claim, now, box) < 0))
    return -1;
  if (fss_rejected_by_votes(prior, up, down) &&
      fss_rep_adjust(author, -FSS_RULES.rep_reject, "claim_rejected", "claim",
                     claim, now, box) < 0)
    return -1;
  return 0;
}

static int step_done(sqlite3_stmt *st) {
  int rc = st ? sqlite3_step(st) : SQLITE_ERROR;
  fss_stmt_release(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

void api_claim_vote(fio_http_s *h, fss_params_s *p) {
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
  int64_t claim = p->num[0];
  fss_outbox_s box = {0};
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_stmt *st = fss_stmt(SQL_CLAIM_INFO);
  if (!st)
    goto db_error;
  sqlite3_bind_int64(st, 1, claim);
  int rc = sqlite3_step(st);
  int64_t spot = 0, author = 0, building = 0;
  if (rc == SQLITE_ROW) {
    spot = sqlite3_column_int64(st, 0);
    author = sqlite3_column_int64(st, 1);
    building = sqlite3_column_int64(st, 2);
  }
  fss_stmt_release(st);
  if (rc == SQLITE_DONE) {
    fss_tx_rollback();
    fss_send_error(h, 404, "not_found", "unknown claim");
    return;
  }
  if (rc != SQLITE_ROW)
    goto db_error;
  if (author == uid) {
    fss_tx_rollback();
    fss_send_error(h, 403, "own_claim", "cannot vote on your own claim");
    return;
  }

  if (v) {
    double weight = fss_voter_weight(uid, building);
    if (weight < 0 || !(st = fss_stmt(SQL_VOTE_UPSERT)))
      goto db_error;
    sqlite3_bind_int64(st, 1, claim);
    sqlite3_bind_int64(st, 2, uid);
    sqlite3_bind_int64(st, 3, v);
    sqlite3_bind_double(st, 4, weight);
    sqlite3_bind_int64(st, 5, fss_now_ms());
  } else {
    if (!(st = fss_stmt(SQL_VOTE_DELETE)))
      goto db_error;
    sqlite3_bind_int64(st, 1, claim);
    sqlite3_bind_int64(st, 2, uid);
  }
  if (step_done(st))
    goto db_error;
  st = fss_stmt(SQL_CLAIM_TALLY);
  if (st)
    sqlite3_bind_int64(st, 1, claim);
  if (step_done(st) || fss_spot_materialize(spot) != SQLITE_OK ||
      claim_author_effects(claim, fss_now_ms(), &box) ||
      fss_live_commit(&box) != SQLITE_OK)
    goto db_error;

  if (!(st = fss_stmt(SQL_CLAIM_JSON))) {
    fss_send_db_error(h);
    return;
  }
  sqlite3_bind_int64(st, 1, claim);
  if (sqlite3_step(st) == SQLITE_ROW) {
    char *out = fio_bstr_write(NULL, "{\"claim\":", 9);
    out = fio_bstr_write(out, sqlite3_column_text(st, 0),
                         (size_t)sqlite3_column_bytes(st, 0));
    out = fio_bstr_write(out, "}", 1);
    fss_send_json(h, 200, out);
  } else {
    fss_send_db_error(h);
  }
  fss_stmt_release(st);
  return;

db_error:
  fss_outbox_clear(&box);
  fss_tx_rollback();
  fss_send_db_error(h);
}

/* *****************************************************************************
Submission: POST /spots {"building_id", "name", "floor"?, "lat"?, "lon"?,
                         "claims"?: [{"attr", "value"}]}
New spots start hidden until spot_confirm_min other users confirm them through
POST /spots/:id/confirm (roadmap 7.5).
***************************************************************************** */

static const char SQL_USER_SPOTS_TODAY[] =
    "SELECT count(*) FROM spot WHERE created_by = ?1 AND created_at > ?2";
static const char SQL_BUILDING_POS[] = "SELECT lat, lon FROM building WHERE id = ?1";
static const char SQL_SPOT_INSERT[] =
    "INSERT INTO spot (building_id, name, floor, lat, lon, status, created_by,"
    " created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, 'hidden', ?6, ?7, ?7)";

#define FSS_SPOTS_PER_DAY 5
#define FSS_SPOT_CLAIMS_MAX 12

void api_spots_create(fio_http_s *h, fss_params_s *p) {
  (void)p;
  if (fss_check_origin(h))
    return;
  int64_t uid = fss_require_user(h);
  if (!uid)
    return;
  FIOBJ body = fss_body_json(h);
  if (!body)
    return;
  char *values[FSS_SPOT_CLAIMS_MAX] = {0};
  const fss_attr_s *attrs[FSS_SPOT_CLAIMS_MAX] = {0};
  size_t n_claims = 0;

  FIOBJ b = fiobj_hash_get2(body, "building_id", 11);
  FIOBJ nm = fiobj_hash_get2(body, "name", 4);
  FIOBJ fl = fiobj_hash_get2(body, "floor", 5);
  FIOBJ la = fiobj_hash_get2(body, "lat", 3), lo = fiobj_hash_get2(body, "lon", 3);
  FIOBJ cl = fiobj_hash_get2(body, "claims", 6);
  fio_str_info_s name = FIOBJ_TYPE_IS(nm, FIOBJ_T_STRING) ? fiobj2cstr(nm)
                                                          : (fio_str_info_s){0};
  fio_str_info_s floor_ = FIOBJ_TYPE_IS(fl, FIOBJ_T_STRING) ? fiobj2cstr(fl)
                                                             : (fio_str_info_s){0};
  if (!FIOBJ_TYPE_IS(b, FIOBJ_T_NUMBER) || !name.len || name.len > 80 ||
      floor_.len > 16 || (fl && !FIOBJ_TYPE_IS(fl, FIOBJ_T_STRING))) {
    fss_send_error(h, 422, "invalid_spot",
                   "building_id (number) and name (<= 80 bytes) are required");
    goto done;
  }
  int has_pos = la || lo;
  double lat = has_pos ? fiobj2f(la) : 0, lon = has_pos ? fiobj2f(lo) : 0;
  if (has_pos && (!(FIOBJ_TYPE_IS(la, FIOBJ_T_NUMBER) || FIOBJ_TYPE_IS(la, FIOBJ_T_FLOAT)) ||
                  !(FIOBJ_TYPE_IS(lo, FIOBJ_T_NUMBER) || FIOBJ_TYPE_IS(lo, FIOBJ_T_FLOAT)) ||
                  fabs(lat) > 90 || fabs(lon) > 180)) {
    fss_send_error(h, 422, "invalid_position", "lat and lon must be numbers");
    goto done;
  }
  if (cl) {
    if (!FIOBJ_TYPE_IS(cl, FIOBJ_T_ARRAY) ||
        fiobj_array_count(cl) > FSS_SPOT_CLAIMS_MAX) {
      fss_send_error(h, 422, "invalid_claims", "claims must be a short array");
      goto done;
    }
    for (uint32_t i = 0; i < fiobj_array_count(cl); ++i) {
      FIOBJ c = fiobj_array_get(cl, i);
      if (!FIOBJ_TYPE_IS(c, FIOBJ_T_HASH)) {
        fss_send_error(h, 422, "invalid_claims", "claims must be objects");
        goto done;
      }
      if (!(attrs[n_claims] = claim_from_json(h, c, &values[n_claims])))
        goto done;
      ++n_claims;
    }
  }

  int err = 0;
  int64_t now = fss_now_ms();
  if (fss_tx_begin() != SQLITE_OK) {
    fss_send_db_error(h);
    goto done;
  }
  if (scalar_i64(SQL_USER_SPOTS_TODAY, uid, now - 24LL * 3600 * 1000, &err) >=
          FSS_SPOTS_PER_DAY &&
      !err) {
    fss_tx_rollback();
    fss_send_error(h, 429, "rate_limited", "daily spot submission limit reached");
    goto done;
  }
  sqlite3_stmt *st = fss_stmt(SQL_BUILDING_POS);
  if (!st || err)
    goto db_error;
  sqlite3_bind_int64(st, 1, fiobj2i(b));
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW && !has_pos) {
    lat = sqlite3_column_double(st, 0);
    lon = sqlite3_column_double(st, 1);
  }
  fss_stmt_release(st);
  if (rc == SQLITE_DONE) {
    fss_tx_rollback();
    fss_send_error(h, 422, "unknown_building", "unknown building");
    goto done;
  }
  if (rc != SQLITE_ROW || !(st = fss_stmt(SQL_SPOT_INSERT)))
    goto db_error;
  sqlite3_bind_int64(st, 1, fiobj2i(b));
  sqlite3_bind_text(st, 2, name.buf, (int)name.len, SQLITE_STATIC);
  if (floor_.len)
    sqlite3_bind_text(st, 3, floor_.buf, (int)floor_.len, SQLITE_STATIC);
  sqlite3_bind_double(st, 4, lat);
  sqlite3_bind_double(st, 5, lon);
  sqlite3_bind_int64(st, 6, uid);
  sqlite3_bind_int64(st, 7, now);
  if (step_done(st))
    goto db_error;
  int64_t spot = sqlite3_last_insert_rowid(fss_db());
  for (size_t i = 0; i < n_claims; ++i) {
    int64_t existing;
    if (insert_user_claim(spot, attrs[i], values[i], (fio_str_info_s){0}, uid,
                          &existing) < 0)
      goto db_error;
  }
  if (fss_spot_materialize(spot) != SQLITE_OK || fss_tx_commit() != SQLITE_OK)
    goto db_error;
  send_spot(h, 201, spot);
  goto done;

db_error:
  fss_tx_rollback();
  fss_send_db_error(h);
done:
  for (size_t i = 0; i < FSS_SPOT_CLAIMS_MAX; ++i)
    fio_bstr_free(values[i]);
  fiobj_free(body);
}
