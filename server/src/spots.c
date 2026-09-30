#include "spots.h"

#include "attrs.h"
#include "db.h"
#include "fss_fio.h"
#include "http.h"
#include "rules.h"

#include <stdlib.h>
#include <string.h>

/* Winning claim per attribute: highest confidence, ties broken by recency. */
static const char SQL_BEST_CLAIMS[] =
    "SELECT attr, value, value ->> '$', p FROM ("
    "  SELECT c.attr, c.value, c.created_at, c.id,"
    "    fss_conf(c.prior, c.up, c.down) AS p,"
    "    row_number() OVER (PARTITION BY c.attr"
    "      ORDER BY fss_conf(c.prior, c.up, c.down) DESC, c.created_at DESC,"
    "               c.id DESC) AS rn"
    "  FROM claim c WHERE c.spot_id = ?1)"
    " WHERE rn = 1 AND p >= ?2";

static const char SQL_SPOT_UPDATE[] =
    "UPDATE spot SET features = ?2, noise = ?3, outlets = ?4, temp = ?5,"
    " capacity = ?6, vibe = ?7, attrs_json = ?8, quality = ?9, updated_at = ?10"
    " WHERE id = ?1";

static const char SQL_FTS_DELETE[] = "DELETE FROM spot_fts WHERE rowid = ?1";

static const char SQL_FTS_INSERT[] =
    "INSERT INTO spot_fts (rowid, name, summary, tags)"
    " SELECT s.id, s.name || ' ' || b.name || COALESCE(' ' || s.floor, ''),"
    "   s.summary, ?2"
    " FROM spot s JOIN building b ON b.id = s.building_id WHERE s.id = ?1";

/* Column slots for ordinal / enum attributes that have a dedicated column. */
typedef struct {
  int has;
  int64_t v;
} opt_int_s;

int fss_spot_materialize(int64_t spot_id) {
  sqlite3_stmt *st = fss_stmt(SQL_BEST_CLAIMS);
  if (!st)
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, spot_id);
  sqlite3_bind_double(st, 2, FSS_RULES.claim_min_p);

  int64_t features = 0;
  opt_int_s noise = {0}, outlets = {0}, temp = {0}, capacity = {0};
  char vibe[32] = {0};
  double p_sum = 0;
  int n = 0, rc;
  char *attrs = fio_bstr_write(NULL, "{", 1);
  char *tags = NULL;

  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const char *key = (const char *)sqlite3_column_text(st, 0);
    const fss_attr_s *a = key ? fss_attr_find(key, strlen(key)) : NULL;
    if (!a)
      continue; /* attribute removed from the registry */
    const char *json = (const char *)sqlite3_column_text(st, 1);
    double p = sqlite3_column_double(st, 3);

    if (n)
      attrs = fio_bstr_write(attrs, ",", 1);
    attrs = fss_bstr_json_str(attrs, a->key, strlen(a->key));
    attrs = fio_bstr_write(attrs, ":{\"v\":", 6);
    attrs = fio_bstr_write(attrs, json, strlen(json));
    attrs = fio_bstr_write2(attrs, FIO_STRING_WRITE_STR1(",\"p\":"),
                            FIO_STRING_WRITE_FLOAT(p),
                            FIO_STRING_WRITE_STR1("}"));
    p_sum += p;
    ++n;

    switch (a->kind) {
    case FSS_ATTR_FLAG:
      if (sqlite3_column_int(st, 2)) {
        features |= (int64_t)1 << a->bit;
        tags = fio_bstr_printf(tags, "%s%s %s %s", tags ? " " : "", a->key,
                               a->label_en, a->label_zh);
      }
      break;
    case FSS_ATTR_ORDINAL: {
      int64_t v = sqlite3_column_int64(st, 2);
      opt_int_s *slot = !strcmp(a->key, "noise")      ? &noise
                        : !strcmp(a->key, "outlets")  ? &outlets
                        : !strcmp(a->key, "temp")     ? &temp
                        : !strcmp(a->key, "capacity") ? &capacity
                                                      : NULL;
      if (slot)
        *slot = (opt_int_s){.has = 1, .v = v};
      break;
    }
    case FSS_ATTR_ENUM:
      if (!strcmp(a->key, "vibe")) {
        const char *v = (const char *)sqlite3_column_text(st, 2);
        size_t len = v ? strlen(v) : 0;
        if (len < sizeof(vibe)) {
          memcpy(vibe, v, len);
          vibe[len] = 0;
          tags = fio_bstr_printf(tags, "%s%s", tags ? " " : "", vibe);
        }
      }
      break;
    case FSS_ATTR_TEXT:
      break;
    }
  }
  fss_stmt_release(st);
  attrs = fio_bstr_write(attrs, "}", 1);
  if (rc != SQLITE_DONE)
    goto done;

  rc = SQLITE_ERROR;
  st = fss_stmt(SQL_SPOT_UPDATE);
  if (!st)
    goto done;
  sqlite3_bind_int64(st, 1, spot_id);
  sqlite3_bind_int64(st, 2, features);
  opt_int_s *cols[] = {&noise, &outlets, &temp, &capacity};
  for (int i = 0; i < 4; ++i)
    if (cols[i]->has)
      sqlite3_bind_int64(st, 3 + i, cols[i]->v);
  if (vibe[0])
    sqlite3_bind_text(st, 7, vibe, -1, SQLITE_STATIC);
  sqlite3_bind_text(st, 8, attrs, (int)fio_bstr_len(attrs), SQLITE_STATIC);
  sqlite3_bind_double(st, 9, n ? p_sum / n : 0.0);
  sqlite3_bind_int64(st, 10, fss_now_ms());
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto done;

  rc = SQLITE_ERROR;
  if (!(st = fss_stmt(SQL_FTS_DELETE)))
    goto done;
  sqlite3_bind_int64(st, 1, spot_id);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    goto done;

  rc = SQLITE_ERROR;
  if (!(st = fss_stmt(SQL_FTS_INSERT)))
    goto done;
  sqlite3_bind_int64(st, 1, spot_id);
  sqlite3_bind_text(st, 2, tags ? tags : "", tags ? (int)fio_bstr_len(tags) : 0,
                    SQLITE_STATIC);
  rc = sqlite3_step(st);
  fss_stmt_release(st);

done:
  fio_bstr_free(attrs);
  fio_bstr_free(tags);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static const char SQL_ALL_SPOTS[] = "SELECT id FROM spot WHERE status != 'merged'";

int fss_spots_materialize_all(const char *db_path) {
  fss_db_thread_open((void *)db_path);
  if (!fss_db())
    return -1;
  int count = 0;
  if (fss_tx_begin() != SQLITE_OK) {
    count = -1;
    goto out;
  }
  /* collect ids first: materialization writes to the spot table */
  sqlite3_stmt *st = fss_stmt(SQL_ALL_SPOTS);
  int64_t *ids = NULL;
  size_t len = 0, cap = 0;
  while (st && sqlite3_step(st) == SQLITE_ROW) {
    if (len == cap) {
      cap = cap ? cap * 2 : 256;
      int64_t *tmp = realloc(ids, cap * sizeof(*ids));
      if (!tmp)
        break;
      ids = tmp;
    }
    ids[len++] = sqlite3_column_int64(st, 0);
  }
  fss_stmt_release(st);
  for (size_t i = 0; i < len && count >= 0; ++i) {
    if (fss_spot_materialize(ids[i]) != SQLITE_OK) {
      FIO_LOG_ERROR("materialize spot %lld: %s", (long long)ids[i],
                    sqlite3_errmsg(fss_db()));
      count = -1;
    } else {
      ++count;
    }
  }
  free(ids);
  if (count >= 0 && fss_tx_commit() != SQLITE_OK)
    count = -1;
  if (count < 0)
    fss_tx_rollback();
out:
  fss_db_thread_close(NULL);
  return count;
}
