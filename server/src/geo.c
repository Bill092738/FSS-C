#include "geo.h"

#include <math.h>

#define FSS_M_PER_DEG_LAT 110540.0
#define FSS_M_PER_DEG_LON 111320.0

int fss_geo_inside(fss_point_s p, const fss_point_s *poly, size_t n) {
  if (n < 3)
    return 0;
  int inside = 0;
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const fss_point_s a = poly[i], b = poly[j];
    if ((a.lat > p.lat) != (b.lat > p.lat) &&
        p.lon < (b.lon - a.lon) * (p.lat - a.lat) / (b.lat - a.lat) + a.lon)
      inside = !inside;
  }
  return inside;
}

/* Distance from the origin to segment ab (planar meters). */
static double seg_dist(double ax, double ay, double bx, double by) {
  double dx = bx - ax, dy = by - ay;
  double len2 = dx * dx + dy * dy;
  double t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0;
  if (t < 0)
    t = 0;
  else if (t > 1)
    t = 1;
  double x = ax + t * dx, y = ay + t * dy;
  return sqrt(x * x + y * y);
}

double fss_geo_boundary_dist_m(fss_point_s p, const fss_point_s *poly,
                               size_t n) {
  if (!n)
    return INFINITY;
  double kx = FSS_M_PER_DEG_LON * cos(p.lat * M_PI / 180.0);
  double best = INFINITY;
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    double d = seg_dist((poly[j].lon - p.lon) * kx,
                        (poly[j].lat - p.lat) * FSS_M_PER_DEG_LAT,
                        (poly[i].lon - p.lon) * kx,
                        (poly[i].lat - p.lat) * FSS_M_PER_DEG_LAT);
    if (d < best)
      best = d;
  }
  return best;
}
