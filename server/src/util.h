/* Pure helpers with no facil.io / SQLite dependency (unit-testable). */
#ifndef FSS_UTIL_H
#define FSS_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Percent-decodes `src` into `dst` ("+" becomes a space).
 * Returns the decoded length, or (size_t)-1 on malformed input or overflow.
 * `dst` is always NUL terminated when `cap > 0` and decoding succeeds. */
size_t fss_pct_decode(char *dst, size_t cap, const char *src, size_t len);

/* Looks up `name` in a raw query string (without the leading '?') and writes
 * its decoded value to `out`.
 * Returns the decoded length, -1 if the key is missing, -2 on malformed input
 * or when the value does not fit into `cap` bytes (including the NUL). */
long fss_query_get(const char *query, size_t qlen, const char *name, char *out,
                   size_t cap);

/* Strict integer / float parsing of an entire buffer. Return 0 on success. */
int fss_parse_i64(const char *s, size_t len, int64_t *out);
int fss_parse_double(const char *s, size_t len, double *out);

/* Splits "a,b,c" style lists. Calls `fn` for each non-empty trimmed item and
 * stops early (returning -1) if `fn` returns non-zero. Returns item count. */
int fss_split_csv(const char *s, size_t len,
                  int (*fn)(const char *item, size_t len, void *udata),
                  void *udata);

/* Builds a safe FTS5 query from free text: every whitespace-separated token
 * becomes a quoted phrase (embedded quotes doubled), tokens are ANDed.
 * Returns the query length, 0 if there are no tokens, or (size_t)-1 when the
 * result does not fit into `cap` bytes (including the NUL). */
size_t fss_fts_query(char *dst, size_t cap, const char *text, size_t len,
                     size_t max_tokens);

#endif /* FSS_UTIL_H */
