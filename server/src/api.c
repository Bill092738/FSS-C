#include "api.h"

#include "db.h"

#include <string.h>

/* The single source of truth for REST endpoints (roadmap 9.1). Paths are
 * relative to the "/api/v1" prefix. */
static const fss_route_s FSS_ROUTES[] = {
    {"GET", "/health", api_health},
    {"GET", "/campuses", api_campuses_index},
    {"GET", "/campuses/:slug", api_campus_show},
    {"POST", "/auth/register", api_auth_register},
    {"POST", "/auth/login", api_auth_login},
    {"POST", "/auth/logout", api_auth_logout},
    {"GET", "/me", api_me_show},
    {"PATCH", "/me", api_me_update},
    {"GET", "/spots", api_spots_index},
    {"POST", "/spots", api_spots_create},
    {"GET", "/spots/#id", api_spot_show},
    {"POST", "/spots/#id/claims", api_spot_claims_create},
    {"POST", "/claims/#id/vote", api_claim_vote},
};

static int method_is(fio_str_info_s m, const char *name) {
  size_t n = strlen(name);
  return m.len == n && !memcmp(m.buf, name, n);
}

void fss_api_dispatch(fio_http_s *h) {
  if (!fss_db()) {
    fss_send_error(h, 503, "unavailable", "database connection unavailable");
    return;
  }
  fio_str_info_s method = fio_http_method(h);
  fio_str_info_s path = fio_http_path(h);
  int path_known = 0;
  for (size_t i = 0; i < sizeof(FSS_ROUTES) / sizeof(FSS_ROUTES[0]); ++i) {
    fss_params_s params;
    if (!fss_route_match(FSS_ROUTES[i].pattern, path.buf, path.len, &params))
      continue;
    path_known = 1;
    if (!method_is(method, FSS_ROUTES[i].method))
      continue;
    FSS_ROUTES[i].fn(h, &params);
    return;
  }
  if (path_known)
    fss_send_error(h, 405, "method_not_allowed", "method not allowed");
  else
    fss_send_error(h, 404, "not_found", "no such endpoint");
}
