/* Periodic jobs (roadmap 4.6).
 *
 * Jobs are timers of a dedicated one-thread async queue, so they run off the
 * IO thread on a worker that owns its own SQLite connection, and never run
 * concurrently with each other.
 */
#ifndef FSS_JOBS_H
#define FSS_JOBS_H

#include "fss_fio.h"

/* Schedules live_decay, checkin_timeout, hourly_rollup and wal_checkpoint
 * on `q`. */
void fss_jobs_register(fio_io_async_s *q);
/* Frees the repeating timers after the reactor stopped: cstl destroys its own
 * timer queue at exit but not those of async queues. */
void fss_jobs_release(fio_io_async_s *q);

#endif /* FSS_JOBS_H */
