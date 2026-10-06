#include "jobs.h"

#include "db.h"
#include "http.h"
#include "live.h"
#include "rules.h"

/* Timer callbacks return non-zero to stop; every job keeps running. */

static int job_live_decay(void *a, void *b) {
  (void)a, (void)b;
  if (fss_db() && fss_live_decay_all(fss_now_ms()) < 0)
    FIO_LOG_ERROR("live_decay failed: %s", sqlite3_errmsg(fss_db()));
  return 0;
}

static int job_wal_checkpoint(void *a, void *b) {
  (void)a, (void)b;
  int log = 0, done = 0;
  if (fss_db() &&
      sqlite3_wal_checkpoint_v2(fss_db(), NULL, SQLITE_CHECKPOINT_PASSIVE, &log,
                                &done) != SQLITE_OK)
    FIO_LOG_WARNING("wal_checkpoint: %s", sqlite3_errmsg(fss_db()));
  return 0;
}

void fss_jobs_release(fio_io_async_s *q) { fio_timer_destroy(&q->timers); }

void fss_jobs_register(fio_io_async_s *q) {
  fio_io_async_every(q, .fn = job_live_decay,
                     .every = (uint32_t)FSS_RULES.job_live_decay_ms,
                     .repetitions = -1);
  fio_io_async_every(q, .fn = job_wal_checkpoint,
                     .every = (uint32_t)FSS_RULES.job_wal_checkpoint_ms,
                     .repetitions = -1);
}
