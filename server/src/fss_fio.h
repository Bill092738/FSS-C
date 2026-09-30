/* Project-wide facil.io cstl include.
 *
 * Every translation unit includes this header. Only src/fio_impl.c defines
 * FIO_EXTERN_COMPLETE before including it, so the (large) implementation is
 * compiled exactly once. See vendor/VERSIONS.md for the pinned cstl commit.
 */
#ifndef FSS_FIO_H
#define FSS_FIO_H

#define FIO_EXTERN
#define FIO_LOG
#define FIO_RAND   /* fio_rand_bytes_secure */
#define FIO_FIOBJ  /* soft types + JSON */
#define FIO_HTTP   /* pulls in IO / IPC / PUBSUB / JSON / MULTIPART */
#define FIO_ARGON2 /* password hashing */
#define FIO_BLAKE2 /* token hashing / HMAC */
#include "fio-stl.h"

#endif /* FSS_FIO_H */
