/* Minimal in-prefix path matcher (pure, unit-testable).
 *
 * facil.io routes are best-prefix matches, so the whole REST API is mounted
 * on a single prefix ("/api/v1") and this matcher resolves the remaining path
 * against patterns such as "/spots/#id/reports".
 *
 * Pattern segments:
 *   literal  - must match exactly
 *   :name    - any non-empty segment (captured as a string)
 *   #name    - a segment that must parse as int64 (captured as string + num)
 * A single trailing '/' on the request path is ignored.
 */
#ifndef FSS_ROUTER_H
#define FSS_ROUTER_H

#include <stddef.h>
#include <stdint.h>

#define FSS_ROUTE_MAX_PARAMS 4

typedef struct {
  const char *buf;
  size_t len;
} fss_slice_s;

typedef struct {
  size_t count;
  fss_slice_s str[FSS_ROUTE_MAX_PARAMS];
  int64_t num[FSS_ROUTE_MAX_PARAMS]; /* valid for '#' captures only */
} fss_params_s;

/* Returns 1 on match (filling `p` if not NULL), 0 otherwise. */
int fss_route_match(const char *pattern, const char *path, size_t path_len,
                    fss_params_s *p);

#endif /* FSS_ROUTER_H */
