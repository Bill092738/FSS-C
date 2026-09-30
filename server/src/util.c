#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

size_t fss_pct_decode(char *dst, size_t cap, const char *src, size_t len) {
  size_t o = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = src[i];
    if (c == '%') {
      if (i + 2 >= len)
        return (size_t)-1;
      int hi = hexval(src[i + 1]), lo = hexval(src[i + 2]);
      if (hi < 0 || lo < 0)
        return (size_t)-1;
      c = (char)((hi << 4) | lo);
      i += 2;
    } else if (c == '+') {
      c = ' ';
    }
    if (o + 1 >= cap)
      return (size_t)-1;
    dst[o++] = c;
  }
  if (cap)
    dst[o] = 0;
  return o;
}

long fss_query_get(const char *query, size_t qlen, const char *name, char *out,
                   size_t cap) {
  size_t nlen = strlen(name);
  size_t pos = 0;
  while (pos < qlen) {
    size_t end = pos;
    while (end < qlen && query[end] != '&')
      ++end;
    const char *eq = memchr(query + pos, '=', end - pos);
    size_t klen = eq ? (size_t)(eq - (query + pos)) : end - pos;
    if (klen == nlen && !memcmp(query + pos, name, nlen)) {
      const char *v = eq ? eq + 1 : query + end;
      size_t vlen = (size_t)((query + end) - v);
      size_t r = fss_pct_decode(out, cap, v, vlen);
      return r == (size_t)-1 ? -2 : (long)r;
    }
    pos = end + 1;
  }
  return -1;
}

int fss_parse_i64(const char *s, size_t len, int64_t *out) {
  if (!len || len > 20)
    return -1;
  char buf[24];
  memcpy(buf, s, len);
  buf[len] = 0;
  char *end = NULL;
  errno = 0;
  long long v = strtoll(buf, &end, 10);
  if (errno || end != buf + len)
    return -1;
  *out = (int64_t)v;
  return 0;
}

int fss_parse_double(const char *s, size_t len, double *out) {
  if (!len || len > 63)
    return -1;
  char buf[64];
  memcpy(buf, s, len);
  buf[len] = 0;
  char *end = NULL;
  errno = 0;
  double v = strtod(buf, &end);
  if (errno || end != buf + len || !isfinite(v))
    return -1;
  *out = v;
  return 0;
}

int fss_split_csv(const char *s, size_t len,
                  int (*fn)(const char *item, size_t len, void *udata),
                  void *udata) {
  int count = 0;
  size_t pos = 0;
  while (pos <= len) {
    size_t end = pos;
    while (end < len && s[end] != ',')
      ++end;
    size_t a = pos, b = end;
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
      ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
      --b;
    if (b > a) {
      if (fn(s + a, b - a, udata))
        return -1;
      ++count;
    }
    pos = end + 1;
  }
  return count;
}

size_t fss_fts_query(char *dst, size_t cap, const char *text, size_t len,
                     size_t max_tokens) {
  size_t o = 0, tokens = 0, i = 0;
#define FSS_PUT(c)                                                             \
  do {                                                                         \
    if (o + 1 >= cap)                                                          \
      return (size_t)-1;                                                       \
    dst[o++] = (c);                                                            \
  } while (0)
  while (i < len && tokens < max_tokens) {
    while (i < len && (unsigned char)text[i] <= ' ')
      ++i;
    if (i >= len)
      break;
    if (tokens)
      FSS_PUT(' ');
    FSS_PUT('"');
    while (i < len && (unsigned char)text[i] > ' ') {
      if (text[i] == '"')
        FSS_PUT('"');
      FSS_PUT(text[i]);
      ++i;
    }
    FSS_PUT('"');
    ++tokens;
  }
#undef FSS_PUT
  if (cap)
    dst[o] = 0;
  return o;
}
