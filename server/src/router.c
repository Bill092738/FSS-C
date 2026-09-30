#include "router.h"

#include "util.h"

#include <string.h>

/* Advances past one '/'-delimited segment; returns its length. */
static size_t seg_len(const char *s, size_t len) {
  size_t i = 0;
  while (i < len && s[i] != '/')
    ++i;
  return i;
}

int fss_route_match(const char *pattern, const char *path, size_t path_len,
                    fss_params_s *p) {
  fss_params_s tmp;
  if (!p)
    p = &tmp;
  p->count = 0;

  /* ignore one trailing slash, treat "" as "/" */
  if (path_len > 1 && path[path_len - 1] == '/')
    --path_len;
  if (!path_len) {
    path = "/";
    path_len = 1;
  }

  size_t plen = strlen(pattern);
  size_t pi = 0, si = 0;
  for (;;) {
    /* both sides are positioned on a '/' or at the end */
    int pend = pi >= plen, send = si >= path_len;
    if (pend || send)
      return pend && send;
    if (pattern[pi] != '/' || path[si] != '/')
      return 0;
    ++pi;
    ++si;
    size_t pl = seg_len(pattern + pi, plen - pi);
    size_t sl = seg_len(path + si, path_len - si);
    const char *ps = pattern + pi, *ss = path + si;

    if (pl && (ps[0] == ':' || ps[0] == '#')) {
      if (!sl || p->count >= FSS_ROUTE_MAX_PARAMS)
        return 0;
      p->str[p->count] = (fss_slice_s){.buf = ss, .len = sl};
      p->num[p->count] = 0;
      if (ps[0] == '#' && fss_parse_i64(ss, sl, &p->num[p->count]))
        return 0;
      ++p->count;
    } else if (pl != sl || memcmp(ps, ss, pl)) {
      return 0;
    }
    pi += pl;
    si += sl;
  }
}
