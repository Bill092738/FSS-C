/* The single translation unit that emits the facil.io cstl implementation. */
#define FIO_EXTERN_COMPLETE
#include "fss_fio.h"

#include "fss_fio_ext.h"

/* fio_http_udata2 / fio_http_udata2_set are FIO_IFUNC declarations whose
 * bodies are only emitted here (`432 http types.h` is guarded by
 * FIO_EXTERN_COMPLETE), so other units reach them through these wrappers. */
void *fss_http_udata2(fio_http_s *h) { return fio_http_udata2(h); }
void *fss_http_udata2_set(fio_http_s *h, void *p) {
  return fio_http_udata2_set(h, p);
}
