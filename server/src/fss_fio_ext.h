/* Exported wrappers for cstl functions that application units cannot call
 * directly (FIO_IFUNC bodies exist only in src/fio_impl.c). */
#ifndef FSS_FIO_EXT_H
#define FSS_FIO_EXT_H

#include "fss_fio.h"

void *fss_http_udata2(fio_http_s *h);
void *fss_http_udata2_set(fio_http_s *h, void *p);

#endif /* FSS_FIO_EXT_H */
