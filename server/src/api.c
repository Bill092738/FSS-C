#include "api.h"

#include "db.h"
#include "fss_fio_ext.h"
#include "rules.h"

#include <pthread.h>
#include <string.h>
#include <sys/socket.h>

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
    {"POST", "/spots/#id/reports", api_spot_reports_create},
    {"POST", "/spots/#id/confirm", api_spot_confirm},
    {"POST", "/spots/#id/photos", api_spot_photos_create},
    {"POST", "/claims/#id/vote", api_claim_vote},
    {"POST", "/photos/#id/vote", api_photo_vote},
    {"POST", "/checkins", api_checkins_create},
    {"POST", "/checkins/#id/heartbeat", api_checkin_heartbeat},
    {"POST", "/checkins/#id/end", api_checkin_end},
    {"GET", "/me/karma", api_me_karma},
    {"POST", "/debug/jobs/:slug", api_debug_job},
};

/* *****************************************************************************
Per-IP limit on the /auth endpoints (roadmap 10.5): auth_ip_per_min requests per minute
and peer address. The address comes from the socket, never from Forwarded /
X-Forwarded-For headers, which clients control.
***************************************************************************** */

#define FSS_AUTH_RL_SLOTS 4096
static fss_rl_slot_s AUTH_RL[FSS_AUTH_RL_SLOTS];
static pthread_mutex_t AUTH_RL_LOCK = PTHREAD_MUTEX_INITIALIZER;

/* FNV-1a over the peer address bytes (port excluded); 0 when unknown. */
static uint64_t peer_key(fio_http_s *h) {
  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  int fd = fss_http_fd(h);
  if (fd < 0 || getpeername(fd, (struct sockaddr *)&addr, &len))
    return 0;
  const uint8_t *b;
  size_t n;
  if (addr.ss_family == AF_INET) {
    b = (const uint8_t *)&((struct sockaddr_in *)&addr)->sin_addr;
    n = 4;
  } else if (addr.ss_family == AF_INET6) {
    b = (const uint8_t *)&((struct sockaddr_in6 *)&addr)->sin6_addr;
    n = 16;
  } else {
    return 0;
  }
  uint64_t k = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; ++i)
    k = (k ^ b[i]) * 0x100000001b3ULL;
  return k;
}

/* Returns 0 when the request may proceed, else sends 429 and returns -1. */
static int auth_rate_limit(fio_http_s *h) {
  uint64_t key = peer_key(h);
  if (!key)
    return 0; /* unix sockets and the like: nothing to key on */
  pthread_mutex_lock(&AUTH_RL_LOCK);
  int64_t wait = fss_rl_hit(AUTH_RL, FSS_AUTH_RL_SLOTS, key, fss_now_ms(),
                            60000, (uint32_t)FSS_RULES.auth_ip_per_min);
  pthread_mutex_unlock(&AUTH_RL_LOCK);
  if (!wait)
    return 0;
  char secs[24];
  int n = snprintf(secs, sizeof(secs), "%lld", (long long)(wait + 999) / 1000);
  fio_http_response_header_set(h, FIO_STR_INFO1("retry-after"),
                               FIO_STR_INFO2(secs, (size_t)n));
  fss_send_error(h, 429, "rate_limited",
                 "too many authentication requests from this address");
  return -1;
}

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
    if (fss_clock_from_request(h))
      return;
    if (path.len > 6 && !memcmp(path.buf, "/auth/", 6) && auth_rate_limit(h)) {
      fss_clock_reset();
      return;
    }
    FSS_ROUTES[i].fn(h, &params);
    fss_clock_reset();
    return;
  }
  if (path_known)
    fss_send_error(h, 405, "method_not_allowed", "method not allowed");
  else
    fss_send_error(h, 404, "not_found", "no such endpoint");
}
