/* Check-in sessions (roadmap 7.4): domain logic shared by the API and the
 * checkin_timeout job. Functions run inside the caller's write transaction. */
#ifndef FSS_CHECKIN_H
#define FSS_CHECKIN_H

#include "live.h"

#include <stdint.h>

/* Ends an open check-in at `end_at` (reason: user / outside / timeout /
 * stale), credits karma for its verified hours and recomputes the spot's
 * live state. *karma receives the points credited (may be NULL). Returns
 * SQLITE_OK, SQLITE_NOTFOUND when it is not open, or an SQLite error. */
int fss_checkin_close(int64_t id, int64_t end_at, const char *reason,
                      int64_t now, int *karma, fss_outbox_s *box);

/* Appends {"id",...} for check-in `id` to a fio_bstr (NULL on error). */
char *fss_checkin_json(char *dest, int64_t id);

/* checkin_timeout job: ends every check-in that is stale or past its time
 * limit. Runs its own transaction. Returns the number ended or -1. */
int fss_checkin_timeout_all(int64_t now);

#endif /* FSS_CHECKIN_H */
