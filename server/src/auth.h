/* Passwords, sessions and request authentication (roadmap 10.5). */
#ifndef FSS_AUTH_H
#define FSS_AUTH_H

#include "fss_fio.h"

#include <stddef.h>
#include <stdint.h>

#define FSS_PW_SALT_LEN 16
#define FSS_PW_HASH_LEN 32
#define FSS_TOKEN_HEX_LEN 64 /* 32 random bytes, hex encoded */
#define FSS_SESSION_COOKIE "fss_sid"

typedef struct {
  uint32_t m_cost; /* KiB */
  uint32_t t_cost;
  uint32_t parallelism;
} fss_pw_params_s;

/* Current parameters for new hashes. */
extern fss_pw_params_s FSS_PW_PARAMS;

/* Hashes a password with a fresh random salt. Returns 0 on success. */
int fss_pw_hash(const char *pw, size_t len, uint8_t salt[FSS_PW_SALT_LEN],
                uint8_t hash[FSS_PW_HASH_LEN], fss_pw_params_s params);
/* Verifies a password in constant time. Returns 1 on match. */
int fss_pw_verify(const char *pw, size_t len, const uint8_t *salt,
                  size_t salt_len, const uint8_t *hash, size_t hash_len,
                  fss_pw_params_s params);
/* Serializes / parses the pw_params column ("argon2id:m=..,t=..,p=.."). */
void fss_pw_params_str(fss_pw_params_s p, char *out, size_t cap);
int fss_pw_params_parse(const char *s, fss_pw_params_s *p);

/* Creates a session for `user_id` (inside the caller's transaction, if any)
 * and sets the session cookie. Returns 0 on success. */
int fss_session_create(fio_http_s *h, int64_t user_id);
/* Deletes the current session (if any) and clears the cookie. */
void fss_session_destroy(fio_http_s *h);

/* Returns the authenticated user id or 0 for anonymous requests. */
int64_t fss_auth_user(fio_http_s *h);
/* Like fss_auth_user but sends 401 and returns 0 when not logged in. */
int64_t fss_require_user(fio_http_s *h);

/* CSRF baseline: if an Origin header is present, its host must equal the Host
 * header. Sends 403 and returns -1 on mismatch. */
int fss_check_origin(fio_http_s *h);

#endif /* FSS_AUTH_H */
