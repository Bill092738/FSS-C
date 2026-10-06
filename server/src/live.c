#include "live.h"

#include "db.h"
#include "fss_fio.h"
#include "rules.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* *****************************************************************************
Clock bridge: cstl Pub/Sub timestamps come from fio_io_last_tick(), which is
CLOCK_MONOTONIC milliseconds (not epoch time).
***************************************************************************** */

static int64_t PUBSUB_EPOCH_OFFSET;

static int64_t clock_ms(clockid_t id) {
  struct timespec ts;
  clock_gettime(id, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void fss_pubsub_clock_init(void) {
  PUBSUB_EPOCH_OFFSET = clock_ms(CLOCK_REALTIME) - clock_ms(CLOCK_MONOTONIC);
}
int64_t fss_pubsub_epoch(int64_t tick) { return tick + PUBSUB_EPOCH_OFFSET; }
int64_t fss_pubsub_tick(int64_t epoch_ms) {
  int64_t t = epoch_ms - PUBSUB_EPOCH_OFFSET;
  return t > 0 ? t : 1; /* 0 would mean "no replay" */
}

/* *****************************************************************************
Outbox
***************************************************************************** */

typedef enum { MSG_LIVE, MSG_EVENT } msg_kind_e;

struct fss_outbox_item_s {
  msg_kind_e kind;
  union {
    fss_live_state_s live;
    fss_event_state_s event;
  } u;
};

static void outbox_push(fss_outbox_s *box, struct fss_outbox_item_s item) {
  if (box->n == box->cap) {
    size_t cap = box->cap ? box->cap * 2 : 4;
    void *p = realloc(box->items, cap * sizeof(*box->items));
    if (!p) {
      FIO_LOG_ERROR("outbox: out of memory, dropping a live message");
      return;
    }
    box->items = p;
    box->cap = cap;
  }
  box->items[box->n++] = item;
}

void fss_outbox_clear(fss_outbox_s *box) {
  free(box->items);
  *box = (fss_outbox_s){0};
}

/* Appends a JSON number with at most `digits` decimals. */
static char *write_num(char *dest, double v, int digits) {
  return fio_bstr_printf(dest, "%.*f", digits, v);
}

char *fss_live_json(char *dest, const fss_live_state_s *s) {
  dest = fio_bstr_write(dest, "{\"est\":", 7);
  dest = write_num(dest, s->est, 2);
  dest = fio_bstr_write(dest, ",\"conf\":", 8);
  dest = write_num(dest, s->conf, 2);
  return fio_bstr_printf(dest, ",\"basis\":\"%s\",\"color\":\"%s\",\"at\":%lld}",
                         s->basis_reports ? "reports" : "forecast",
                         fss_live_color(s->est), (long long)s->updated_at);
}

char *fss_event_json(char *dest, const fss_event_state_s *e) {
  return fio_bstr_printf(
      dest, "{\"kind\":\"%s\",\"visible\":%s,\"reports\":%d,\"until\":%lld}",
      e->kind, e->visible ? "true" : "false", e->reports, (long long)e->until);
}

/* Pub/Sub message bodies (roadmap 9.2). `at` is the publish time in epoch ms
 * and doubles as the replay cursor for the WebSocket `since` field. */
static char *item_message(const struct fss_outbox_item_s *it, int64_t at) {
  if (it->kind == MSG_LIVE) {
    const fss_live_state_s *s = &it->u.live;
    char *m = fio_bstr_printf(NULL, "{\"t\":\"live\",\"spot\":%lld,\"color\":\"%s\",\"est\":",
                              (long long)s->spot, fss_live_color(s->est));
    m = write_num(m, s->est, 2);
    m = fio_bstr_write(m, ",\"conf\":", 8);
    m = write_num(m, s->conf, 2);
    return fio_bstr_printf(m, ",\"basis\":\"%s\",\"at\":%lld}",
                           s->basis_reports ? "reports" : "forecast",
                           (long long)at);
  }
  const fss_event_state_s *e = &it->u.event;
  return fio_bstr_printf(NULL,
                         "{\"t\":\"event\",\"spot\":%lld,\"kind\":\"%s\","
                         "\"until\":%lld,\"reports\":%d,\"at\":%lld}",
                         (long long)e->spot, e->kind, (long long)e->until,
                         e->reports, (long long)at);
}

static void publish(const char *prefix, int64_t id, char *msg, int64_t tick) {
  char ch[48];
  int len = snprintf(ch, sizeof(ch), "%s:%lld", prefix, (long long)id);
  fio_pubsub_publish(.channel = FIO_BUF_INFO2(ch, (size_t)len),
                     .message = FIO_BUF_INFO2(msg, fio_bstr_len(msg)),
                     .timestamp = (uint64_t)tick);
}

static pthread_mutex_t COMMIT_LOCK = PTHREAD_MUTEX_INITIALIZER;

int fss_live_commit(fss_outbox_s *box) {
  pthread_mutex_lock(&COMMIT_LOCK);
  int rc = fss_tx_commit();
  if (rc == SQLITE_OK && box->n) {
    int64_t tick = fio_io_last_tick();
    int64_t at = fss_pubsub_epoch(tick);
    for (size_t i = 0; i < box->n; ++i) {
      const struct fss_outbox_item_s *it = &box->items[i];
      char *msg = item_message(it, at);
      int64_t spot = it->kind == MSG_LIVE ? it->u.live.spot : it->u.event.spot;
      int64_t bldg =
          it->kind == MSG_LIVE ? it->u.live.building : it->u.event.building;
      publish("spot", spot, msg, tick);
      publish("bldg", bldg, msg, tick);
      fio_bstr_free(msg);
    }
  }
  pthread_mutex_unlock(&COMMIT_LOCK);
  if (rc != SQLITE_OK)
    fss_tx_rollback();
  fss_outbox_clear(box);
  return rc;
}

/* *****************************************************************************
Crowding estimate (roadmap 7.3)
***************************************************************************** */

/* The forecast is the spot's materialized "typical crowd" until M5 builds
 * forecast_slot from history. */
static const char SQL_LIVE_SPOT[] =
    "SELECT building_id,"
    " COALESCE(json_extract(attrs_json, '$.crowd_typical.v'), ?2)"
    " FROM spot WHERE id = ?1";
static const char SQL_LIVE_OLD[] =
    "SELECT est, basis FROM occupancy_live WHERE spot_id = ?1";
/* A report counts unless the same user reported this spot again within the
 * dedupe window after it (roadmap 7.6: "only the last report counts"). */
static const char SQL_LIVE_OBS[] =
    "SELECT r.level, r.weight, r.at FROM report r"
    " WHERE r.spot_id = ?1 AND r.level IS NOT NULL"
    "   AND r.at >= ?2 - ?3 AND r.at <= ?2"
    "   AND NOT EXISTS (SELECT 1 FROM report n"
    "     WHERE n.spot_id = r.spot_id AND n.user_id = r.user_id"
    "       AND n.level IS NOT NULL AND n.at <= ?2 AND n.at <= r.at + ?4"
    "       AND (n.at > r.at OR (n.at = r.at AND n.id > r.id)))";
static const char SQL_LIVE_UPSERT[] =
    "INSERT INTO occupancy_live (spot_id, est, conf, basis, updated_at)"
    " VALUES (?1, ?2, ?3, ?4, ?5)"
    " ON CONFLICT (spot_id) DO UPDATE SET est = excluded.est,"
    " conf = excluded.conf, basis = excluded.basis,"
    " updated_at = excluded.updated_at";

#define FSS_LIVE_OBS_STACK 128

int fss_live_update(int64_t spot, int64_t now, fss_live_state_s *out,
                    fss_outbox_s *box) {
  sqlite3_stmt *st = fss_stmt(SQL_LIVE_SPOT);
  if (!st)
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_double(st, 2, FSS_RULES.live_default_forecast);
  int rc = sqlite3_step(st);
  int64_t building = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  double forecast = rc == SQLITE_ROW ? sqlite3_column_double(st, 1) : 0;
  fss_stmt_release(st);
  if (rc != SQLITE_ROW)
    return rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc;

  /* previous state; without a row the spot showed its forecast */
  if (!(st = fss_stmt(SQL_LIVE_OLD)))
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, spot);
  rc = sqlite3_step(st);
  double old_est = forecast;
  int old_reports = 0;
  if (rc == SQLITE_ROW) {
    old_est = sqlite3_column_double(st, 0);
    const char *b = (const char *)sqlite3_column_text(st, 1);
    old_reports = b && !strcmp(b, "reports");
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return rc;

  if (!(st = fss_stmt(SQL_LIVE_OBS)))
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_int64(st, 2, now);
  sqlite3_bind_int64(st, 3, FSS_RULES.live_window_ms);
  sqlite3_bind_int64(st, 4, FSS_RULES.report_dedupe_ms);
  fss_live_obs_s stack[FSS_LIVE_OBS_STACK], *obs = stack;
  size_t n = 0, cap = FSS_LIVE_OBS_STACK;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (n == cap) {
      fss_live_obs_s *grown = malloc(cap * 2 * sizeof(*grown));
      if (!grown)
        break; /* estimate from the rows read so far */
      memcpy(grown, obs, n * sizeof(*obs));
      if (obs != stack)
        free(obs);
      obs = grown;
      cap *= 2;
    }
    obs[n++] = (fss_live_obs_s){.level = sqlite3_column_double(st, 0),
                                .weight = sqlite3_column_double(st, 1),
                                .at = sqlite3_column_int64(st, 2)};
  }
  fss_stmt_release(st);
  fss_live_est_s e = fss_live_estimate(obs, n, forecast, now);
  if (obs != stack)
    free(obs);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    return rc;

  *out = (fss_live_state_s){.spot = spot, .building = building, .est = e.est,
                            .conf = e.conf, .basis_reports = e.basis_reports,
                            .updated_at = now};
  if (!(st = fss_stmt(SQL_LIVE_UPSERT)))
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_double(st, 2, e.est);
  sqlite3_bind_double(st, 3, e.conf);
  sqlite3_bind_text(st, 4, e.basis_reports ? "reports" : "forecast", -1,
                    SQLITE_STATIC);
  sqlite3_bind_int64(st, 5, now);
  rc = sqlite3_step(st);
  fss_stmt_release(st);
  if (rc != SQLITE_DONE)
    return rc;

  /* publish only when what clients display changes (roadmap 7.3) */
  if (box && (strcmp(fss_live_color(old_est), fss_live_color(e.est)) ||
              old_reports != e.basis_reports))
    outbox_push(box, (struct fss_outbox_item_s){.kind = MSG_LIVE,
                                                .u.live = *out});
  return SQLITE_OK;
}

/* *****************************************************************************
Event reports (roadmap 7.3): a temporary status with a TTL, shown once
`event_min_users` distinct users reported it, or one trusted user did (fence
factor g = 1.0 and reputation >= event_trusted_rep; report.weight = r * g).
***************************************************************************** */

static const char *const EVENT_KINDS[] = {"outlet_broken", "wifi_down",
                                          "closed_event"};

const char *fss_event_kind(const char *s, size_t len) {
  for (size_t i = 0; i < sizeof(EVENT_KINDS) / sizeof(EVENT_KINDS[0]); ++i)
    if (strlen(EVENT_KINDS[i]) == len && !memcmp(EVENT_KINDS[i], s, len))
      return EVENT_KINDS[i];
  return NULL;
}

static const char SQL_EVENT_STATE[] =
    "SELECT s.building_id, count(DISTINCT r.user_id), max(r.at),"
    " COALESCE(max(r.fence >= ?4 AND r.weight >= ?5 * r.fence), 0)"
    " FROM spot s LEFT JOIN report r ON r.spot_id = s.id AND r.event = ?2"
    "   AND r.at > ?3 - ?6 AND r.at <= ?3"
    " WHERE s.id = ?1";

int fss_event_update(int64_t spot, const char *kind, int64_t now,
                     fss_event_state_s *out, fss_outbox_s *box) {
  sqlite3_stmt *st = fss_stmt(SQL_EVENT_STATE);
  if (!st)
    return SQLITE_ERROR;
  sqlite3_bind_int64(st, 1, spot);
  sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
  sqlite3_bind_int64(st, 3, now);
  sqlite3_bind_double(st, 4, FSS_RULES.fence_g_inside);
  sqlite3_bind_double(st, 5, FSS_RULES.event_trusted_rep);
  sqlite3_bind_int64(st, 6, FSS_RULES.event_ttl_ms);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    int users = sqlite3_column_int(st, 1);
    int64_t last = sqlite3_column_int64(st, 2);
    int trusted = sqlite3_column_int(st, 3);
    *out = (fss_event_state_s){
        .spot = spot,
        .building = sqlite3_column_int64(st, 0),
        .kind = kind,
        .visible = users > 0 && (users >= FSS_RULES.event_min_users || trusted),
        .reports = users,
        .until = users ? last + FSS_RULES.event_ttl_ms : 0,
    };
  }
  fss_stmt_release(st);
  if (rc != SQLITE_ROW)
    return rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc;
  if (box && out->visible)
    outbox_push(box, (struct fss_outbox_item_s){.kind = MSG_EVENT,
                                                .u.event = *out});
  return SQLITE_OK;
}

/* *****************************************************************************
live_decay job (roadmap 4.6)
***************************************************************************** */

/* Every report has weight > 0 (reputation >= 0.1, g >= 0.2), so a spot with
 * reports inside the window always has conf > 0; conf returns to exactly 0
 * once the last report leaves the window. */
static const char SQL_LIVE_ACTIVE[] =
    "SELECT spot_id FROM occupancy_live WHERE conf > 0";

int fss_live_decay_all(int64_t now) {
  /* the candidate list is read before taking the write lock so that idle
   * ticks never block writers; a spot that becomes active in between is
   * recomputed by its own report anyway */
  sqlite3_stmt *st = fss_stmt(SQL_LIVE_ACTIVE);
  if (!st)
    return -1;
  int64_t *ids = NULL;
  size_t n = 0, cap = 0;
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (n == cap) {
      cap = cap ? cap * 2 : 64;
      int64_t *p = realloc(ids, cap * sizeof(*ids));
      if (!p) {
        rc = SQLITE_NOMEM;
        break;
      }
      ids = p;
    }
    ids[n++] = sqlite3_column_int64(st, 0);
  }
  fss_stmt_release(st);
  if (rc != SQLITE_DONE || !n) {
    free(ids);
    return rc == SQLITE_DONE ? 0 : -1;
  }
  if (fss_tx_begin() != SQLITE_OK) {
    free(ids);
    return -1;
  }
  fss_outbox_s box = {0};
  for (size_t i = 0; i < n; ++i) {
    fss_live_state_s s;
    rc = fss_live_update(ids[i], now, &s, &box);
    if (rc == SQLITE_NOTFOUND) /* spot deleted under the row; skip */
      rc = SQLITE_OK;
    if (rc != SQLITE_OK)
      break;
  }
  free(ids);
  if (rc != SQLITE_OK) {
    fss_outbox_clear(&box);
    fss_tx_rollback();
    return -1;
  }
  return fss_live_commit(&box) == SQLITE_OK ? (int)n : -1;
}
