#include "attrs.h"

#include "db.h"

#include <string.h>

static fss_attr_s ATTRS[FSS_ATTR_MAX];
static size_t ATTR_COUNT;

static void copy_text(char *dst, size_t cap, const unsigned char *src) {
  if (!src) {
    dst[0] = 0;
    return;
  }
  size_t n = strlen((const char *)src);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static int parse_domain(fss_attr_s *a, const unsigned char *json) {
  if (a->kind != FSS_ATTR_ORDINAL && a->kind != FSS_ATTR_ENUM)
    return 0;
  if (!json)
    return -1;
  FIOBJ d = fiobj_json_parse2((char *)json, strlen((const char *)json), NULL);
  int rc = -1;
  if (a->kind == FSS_ATTR_ORDINAL && FIOBJ_TYPE_IS(d, FIOBJ_T_HASH)) {
    FIOBJ mn = fiobj_hash_get2(d, "min", 3), mx = fiobj_hash_get2(d, "max", 3);
    if (FIOBJ_TYPE_IS(mn, FIOBJ_T_NUMBER) && FIOBJ_TYPE_IS(mx, FIOBJ_T_NUMBER)) {
      a->min = fiobj2i(mn);
      a->max = fiobj2i(mx);
      rc = a->min <= a->max ? 0 : -1;
    }
  } else if (a->kind == FSS_ATTR_ENUM && FIOBJ_TYPE_IS(d, FIOBJ_T_ARRAY)) {
    uint32_t n = fiobj_array_count(d);
    rc = n && n <= FSS_ATTR_ENUM_MAX ? 0 : -1;
    for (uint32_t i = 0; !rc && i < n; ++i) {
      fio_str_info_s s = fiobj2cstr(fiobj_array_get(d, i));
      if (!s.len || s.len >= sizeof(a->enums[0])) {
        rc = -1;
        break;
      }
      memcpy(a->enums[i], s.buf, s.len);
      a->enums[i][s.len] = 0;
    }
    if (!rc)
      a->n_enum = n;
  }
  fiobj_free(d);
  return rc;
}

int fss_attrs_load(const char *db_path) {
  sqlite3 *db = fss_db_open(db_path);
  if (!db)
    return -1;
  sqlite3_stmt *st = NULL;
  int rc = sqlite3_prepare_v2(
      db,
      "SELECT key, kind, bit, domain_json, label_en, label_zh FROM attr_def"
      " ORDER BY sort",
      -1, &st, NULL);
  ATTR_COUNT = 0;
  while (rc == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
    if (ATTR_COUNT >= FSS_ATTR_MAX) {
      FIO_LOG_ERROR("attr_def has more than %d rows", FSS_ATTR_MAX);
      rc = SQLITE_ERROR;
      break;
    }
    fss_attr_s *a = &ATTRS[ATTR_COUNT];
    *a = (fss_attr_s){.bit = -1};
    copy_text(a->key, sizeof(a->key), sqlite3_column_text(st, 0));
    const char *kind = (const char *)sqlite3_column_text(st, 1);
    if (!strcmp(kind, "flag"))
      a->kind = FSS_ATTR_FLAG;
    else if (!strcmp(kind, "ordinal"))
      a->kind = FSS_ATTR_ORDINAL;
    else if (!strcmp(kind, "enum"))
      a->kind = FSS_ATTR_ENUM;
    else
      a->kind = FSS_ATTR_TEXT;
    if (sqlite3_column_type(st, 2) != SQLITE_NULL)
      a->bit = sqlite3_column_int(st, 2);
    copy_text(a->label_en, sizeof(a->label_en), sqlite3_column_text(st, 4));
    copy_text(a->label_zh, sizeof(a->label_zh), sqlite3_column_text(st, 5));
    if (parse_domain(a, sqlite3_column_text(st, 3))) {
      FIO_LOG_ERROR("attr_def %s has an invalid domain_json", a->key);
      rc = SQLITE_ERROR;
      break;
    }
    ++ATTR_COUNT;
  }
  sqlite3_finalize(st);
  sqlite3_close_v2(db);
  return rc == SQLITE_OK ? (int)ATTR_COUNT : -1;
}

const fss_attr_s *fss_attr_find(const char *key, size_t len) {
  for (size_t i = 0; i < ATTR_COUNT; ++i)
    if (strlen(ATTRS[i].key) == len && !memcmp(ATTRS[i].key, key, len))
      return &ATTRS[i];
  return NULL;
}

size_t fss_attr_count(void) { return ATTR_COUNT; }
const fss_attr_s *fss_attr_at(size_t i) {
  return i < ATTR_COUNT ? &ATTRS[i] : NULL;
}

#define FSS_ATTR_TEXT_MAX 200

int fss_attr_canonical(const fss_attr_s *a, FIOBJ v, char **out) {
  *out = NULL;
  switch (a->kind) {
  case FSS_ATTR_FLAG:
    if (FIOBJ_TYPE_IS(v, FIOBJ_T_TRUE))
      *out = fio_bstr_write(NULL, "true", 4);
    else if (FIOBJ_TYPE_IS(v, FIOBJ_T_FALSE))
      *out = fio_bstr_write(NULL, "false", 5);
    break;
  case FSS_ATTR_ORDINAL:
    if (FIOBJ_TYPE_IS(v, FIOBJ_T_NUMBER)) {
      intptr_t n = fiobj2i(v);
      if (n >= a->min && n <= a->max)
        *out = fio_bstr_write_i(NULL, n);
    }
    break;
  case FSS_ATTR_ENUM:
    if (FIOBJ_TYPE_IS(v, FIOBJ_T_STRING)) {
      fio_str_info_s s = fiobj2cstr(v);
      for (size_t i = 0; i < a->n_enum; ++i) {
        if (strlen(a->enums[i]) == s.len && !memcmp(a->enums[i], s.buf, s.len)) {
          *out = fio_bstr_write(NULL, "\"", 1);
          *out = fio_bstr_write(*out, s.buf, s.len);
          *out = fio_bstr_write(*out, "\"", 1);
          break;
        }
      }
    }
    break;
  case FSS_ATTR_TEXT:
    if (FIOBJ_TYPE_IS(v, FIOBJ_T_STRING)) {
      fio_str_info_s s = fiobj2cstr(v);
      if (s.len && s.len <= FSS_ATTR_TEXT_MAX) {
        *out = fio_bstr_write(NULL, "\"", 1);
        *out = fio_bstr_write_escape(*out, s.buf, s.len);
        *out = fio_bstr_write(*out, "\"", 1);
      }
    }
    break;
  }
  return *out ? 0 : -1;
}
