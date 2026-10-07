#include "fence.h"

#include "db.h"
#include "geo.h"
#include "http.h"
#include "rules.h"

#include <math.h>

#define FSS_FENCE_MAX_VERTICES 256
#define FSS_ACCURACY_MAX_M 100000.0

static const char SQL_FENCE_POINTS[] =
    "SELECT j.value ->> 0, j.value ->> 1"
    " FROM building b, json_each(b.fence_json) j"
    " WHERE b.id = ?1 ORDER BY j.key";

static int is_number(FIOBJ o) {
  return FIOBJ_TYPE_IS(o, FIOBJ_T_NUMBER) || FIOBJ_TYPE_IS(o, FIOBJ_T_FLOAT);
}

int fss_position_parse(fio_http_s *h, FIOBJ body, fss_position_s *pos) {
  FIOBJ la = fiobj_hash_get2(body, "lat", 3);
  FIOBJ lo = fiobj_hash_get2(body, "lon", 3);
  FIOBJ ac = fiobj_hash_get2(body, "accuracy_m", 10);
  *pos = (fss_position_s){0};
  if (!la && !lo && !ac)
    return 0;
  if (is_number(la) && is_number(lo) && is_number(ac)) {
    *pos = (fss_position_s){.has_pos = 1, .lat = fiobj2f(la),
                            .lon = fiobj2f(lo), .accuracy = fiobj2f(ac)};
    if (fabs(pos->lat) <= 90 && fabs(pos->lon) <= 180 && pos->accuracy > 0 &&
        pos->accuracy <= FSS_ACCURACY_MAX_M)
      return 0;
  }
  fss_send_error(h, 422, "invalid_position",
                 "lat, lon and accuracy_m must be sent together as numbers"
                 " (accuracy_m > 0)");
  return -1;
}

double fss_fence_factor_at(int64_t building, const fss_position_s *pos,
                           int *inside) {
  *inside = 0;
  if (!pos->has_pos)
    return fss_fence_factor(0, 0, INFINITY, 0);
  fss_point_s poly[FSS_FENCE_MAX_VERTICES];
  size_t n = 0;
  sqlite3_stmt *st = fss_stmt(SQL_FENCE_POINTS);
  if (st) {
    sqlite3_bind_int64(st, 1, building);
    while (sqlite3_step(st) == SQLITE_ROW && n < FSS_FENCE_MAX_VERTICES)
      poly[n++] = (fss_point_s){.lon = sqlite3_column_double(st, 0),
                                .lat = sqlite3_column_double(st, 1)};
    fss_stmt_release(st);
  }
  fss_point_s p = {.lon = pos->lon, .lat = pos->lat};
  *inside = fss_geo_inside(p, poly, n);
  return fss_fence_factor(1, *inside, fss_geo_boundary_dist_m(p, poly, n),
                          pos->accuracy);
}
