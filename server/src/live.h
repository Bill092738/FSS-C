/* Live crowding state and its publication (roadmap 3, 4.5, 7.3).
 *
 * Writers follow one pattern: BEGIN IMMEDIATE, change data, recompute the
 * materialized state with the functions below (which queue Pub/Sub messages
 * in an outbox), then fss_live_commit() commits and publishes. Commit and
 * publish happen under one lock so that subscribers see messages in commit
 * order.
 */
#ifndef FSS_LIVE_H
#define FSS_LIVE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  int64_t spot, building;
  double est, conf;
  int basis_reports;
  int64_t updated_at;
} fss_live_state_s;

typedef struct {
  int64_t spot, building;
  const char *kind; /* static string from the event registry */
  int visible;
  int reports;      /* distinct reporters within the TTL */
  int64_t until;    /* last report + TTL */
} fss_event_state_s;

/* Messages waiting for the commit. Zero-initialize before use. */
typedef struct {
  size_t n, cap;
  struct fss_outbox_item_s *items;
} fss_outbox_s;

/* Recomputes occupancy_live for `spot` at time `now` (inside the caller's
 * write transaction). Open verified check-ins add an implicit report (roadmap
 * 7.4). When the color or basis changed, a `live` message is queued for
 * spot:{id} and bldg:{building}. Returns SQLITE_OK or an error. */
int fss_live_update(int64_t spot, int64_t now, fss_live_state_s *out,
                    fss_outbox_s *box);

/* Reads the visibility of one event kind at `spot` (roadmap 7.3) and, when
 * visible, queues an `event` message. Returns SQLITE_OK or an error. */
int fss_event_update(int64_t spot, const char *kind, int64_t now,
                     fss_event_state_s *out, fss_outbox_s *box);

/* Returns the registry's static name for an event kind, or NULL. */
const char *fss_event_kind(const char *s, size_t len);

/* Commits the caller's transaction and, on success, publishes and releases
 * the outbox. On failure the outbox is released without publishing and the
 * transaction is rolled back. Returns SQLITE_OK or the commit error. */
int fss_live_commit(fss_outbox_s *box);
/* Queues a personal notification for user:{user} (roadmap 9.2: karma, badges,
 * coupons). `json` is a fio_bstr holding a JSON object without its closing
 * brace; the outbox takes ownership and appends `"at"` when publishing. */
void fss_outbox_user(fss_outbox_s *box, int64_t user, char *json);
/* Releases an outbox without publishing (rollback paths). */
void fss_outbox_clear(fss_outbox_s *box);

/* Appends {"est","conf","basis","color","at"} to a fio_bstr. */
char *fss_live_json(char *dest, const fss_live_state_s *s);
/* Appends {"kind","visible","reports","until"} to a fio_bstr. */
char *fss_event_json(char *dest, const fss_event_state_s *e);

/* Job: recomputes every spot whose estimate is not at rest (conf > 0) and
 * publishes changes. Uses the calling thread's connection. Returns the number
 * of spots recomputed or -1. */
int fss_live_decay_all(int64_t now);

/* Pub/Sub timestamps use cstl's monotonic tick; messages and the WebSocket
 * `since` field use epoch ms. The offset is fixed at startup. */
void fss_pubsub_clock_init(void);
int64_t fss_pubsub_epoch(int64_t tick);
int64_t fss_pubsub_tick(int64_t epoch_ms);

#endif /* FSS_LIVE_H */
