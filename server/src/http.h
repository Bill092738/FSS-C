/* HTTP response / request helpers shared by all API handlers. */
#ifndef FSS_HTTP_H
#define FSS_HTTP_H

#include "fss_fio.h"
#include "router.h"

#include <stdint.h>

/* Handler signature used by the API route table. */
typedef void (*fss_handler_f)(fio_http_s *h, fss_params_s *p);

typedef struct {
  const char *method;  /* "GET", "POST", ... */
  const char *pattern; /* see router.h */
  fss_handler_f fn;
} fss_route_s;

/* Sends `json` (a fio_bstr, ownership is taken) with the given status. */
void fss_send_json(fio_http_s *h, size_t status, char *json);
/* Sends a copy of `json`. */
void fss_send_json_buf(fio_http_s *h, size_t status, const char *json,
                       size_t len);
/* Sends {"error":{"code":...,"message":...}} (roadmap 9.1). */
void fss_send_error(fio_http_s *h, size_t status, const char *code,
                    const char *message);
/* Convenience: 500 with a generic database error, logs the SQLite message. */
void fss_send_db_error(fio_http_s *h);

/* Appends a JSON string literal (quoted and escaped) to a fio_bstr. */
char *fss_bstr_json_str(char *dest, const char *s, size_t len);

/* Reads a query parameter into `out`. Returns length, -1 missing, -2 error. */
long fss_query(fio_http_s *h, const char *name, char *out, size_t cap);

/* Parses the request body as a JSON object. Returns FIOBJ_INVALID (and sends
 * a 400/415 response) when the body is not a JSON object. */
FIOBJ fss_body_json(fio_http_s *h);

/* Current time in milliseconds since the epoch. On a server started with
 * --test-clock, an API request's `X-FSS-Now` header (epoch ms) replaces it for
 * the duration of that request (roadmap 10.4). */
int64_t fss_now_ms(void);
/* Real wall-clock time; never overridden. */
int64_t fss_wall_ms(void);

/* Set once at startup (--test-clock); off by default. */
extern int FSS_TEST_CLOCK;
/* Applies the request's X-FSS-Now header to the calling thread (if enabled).
 * Returns -1 (after sending 400) when the header is malformed. */
int fss_clock_from_request(fio_http_s *h);
/* Clears the calling thread's override. */
void fss_clock_reset(void);

#endif /* FSS_HTTP_H */
