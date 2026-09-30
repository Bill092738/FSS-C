/* In-memory copy of the attr_def registry (roadmap 6.4).
 *
 * Loaded once on the main thread before the reactor starts and read-only
 * afterwards, so worker threads may read it without locking.
 */
#ifndef FSS_ATTRS_H
#define FSS_ATTRS_H

#include "fss_fio.h"

#include <stddef.h>
#include <stdint.h>

#define FSS_ATTR_MAX 64
#define FSS_ATTR_ENUM_MAX 16

typedef enum {
  FSS_ATTR_FLAG,
  FSS_ATTR_ORDINAL,
  FSS_ATTR_ENUM,
  FSS_ATTR_TEXT,
} fss_attr_kind_e;

typedef struct {
  char key[32];
  char label_en[64];
  char label_zh[64];
  fss_attr_kind_e kind;
  int bit;            /* flags only, -1 otherwise */
  int64_t min, max;   /* ordinals only */
  size_t n_enum;
  char enums[FSS_ATTR_ENUM_MAX][32];
} fss_attr_s;

/* Loads attr_def from the database. Returns the number of attributes or -1. */
int fss_attrs_load(const char *db_path);

const fss_attr_s *fss_attr_find(const char *key, size_t len);
size_t fss_attr_count(void);
const fss_attr_s *fss_attr_at(size_t i);

/* Validates a JSON value against the attribute's domain and writes its
 * canonical JSON scalar text (e.g. `true`, `3`, `"deep_work"`) as a new
 * fio_bstr into `*out`. Returns 0 on success, -1 when the value is invalid. */
int fss_attr_canonical(const fss_attr_s *a, FIOBJ value, char **out);

#endif /* FSS_ATTRS_H */
