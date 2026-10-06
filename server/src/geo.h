/* Campus-scale geometry (pure, unit-testable; roadmap 7.2).
 *
 * Coordinates are degrees. Distances use an equirectangular projection
 * centered on the query point, which is accurate to well under a meter over
 * the few hundred meters that matter for a building fence.
 */
#ifndef FSS_GEO_H
#define FSS_GEO_H

#include <stddef.h>

typedef struct {
  double lon, lat;
} fss_point_s;

/* Ray casting point-in-polygon. The polygon may or may not repeat its first
 * vertex at the end. Returns 1 inside, 0 outside (or fewer than 3 vertices). */
int fss_geo_inside(fss_point_s p, const fss_point_s *poly, size_t n);

/* Shortest distance in meters from `p` to the polygon's boundary (also when
 * `p` is inside). Returns INFINITY for an empty polygon. */
double fss_geo_boundary_dist_m(fss_point_s p, const fss_point_s *poly,
                               size_t n);

#endif /* FSS_GEO_H */
