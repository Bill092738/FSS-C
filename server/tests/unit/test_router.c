#include "harness.h"

#include "router.h"

static int m(const char *pattern, const char *path, fss_params_s *p) {
  return fss_route_match(pattern, path, strlen(path), p);
}

void test_router(void) {
  fss_params_s p;

  CHECK(m("/health", "/health", &p));
  CHECK(m("/health", "/health/", &p));
  CHECK(!m("/health", "/healthz", &p));
  CHECK(!m("/health", "/health/x", &p));
  CHECK(!m("/health", "/", &p));
  CHECK(m("/", "/", &p));
  CHECK(m("/", "", &p));
  CHECK(!m("/", "/x", &p));

  CHECK(m("/spots/#id", "/spots/42", &p));
  CHECK_EQ_INT(p.count, 1);
  CHECK_EQ_INT(p.num[0], 42);
  CHECK(!m("/spots/#id", "/spots/abc", &p));
  CHECK(!m("/spots/#id", "/spots/", &p));
  CHECK(!m("/spots/#id", "/spots", &p));

  CHECK(m("/spots/#id/reports", "/spots/7/reports", &p));
  CHECK_EQ_INT(p.num[0], 7);
  CHECK(!m("/spots/#id/reports", "/spots/7/claims", &p));
  CHECK(!m("/spots/#id/reports", "/spots/7", &p));

  CHECK(m("/campuses/:slug", "/campuses/osu", &p));
  CHECK_STR(p.str[0].buf, p.str[0].len, "osu");
  CHECK(m("/campuses/:slug/heatmap", "/campuses/osu/heatmap", &p));
  CHECK_EQ_INT(p.count, 1);

  CHECK(m("/a/#x/b/#y", "/a/1/b/2", &p));
  CHECK_EQ_INT(p.count, 2);
  CHECK_EQ_INT(p.num[0], 1);
  CHECK_EQ_INT(p.num[1], 2);

  /* NULL params pointer is allowed */
  CHECK(fss_route_match("/x", "/x", 2, NULL));
}
