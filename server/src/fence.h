/* Client positions and the geofence factor (roadmap 7.2), shared by reports,
 * check-ins and heartbeats. */
#ifndef FSS_FENCE_H
#define FSS_FENCE_H

#include "fss_fio.h"

#include <stdint.h>

typedef struct {
  int has_pos;
  double lat, lon, accuracy;
} fss_position_s;

/* Parses the optional "lat", "lon", "accuracy_m" fields (all three or none).
 * Sends 422 invalid_position and returns -1 when invalid. */
int fss_position_parse(fio_http_s *h, FIOBJ body, fss_position_s *pos);

/* Fence factor g of `pos` for `building`; sets *inside. A missing position
 * gives the outside factor. */
double fss_fence_factor_at(int64_t building, const fss_position_s *pos,
                           int *inside);

#endif /* FSS_FENCE_H */
