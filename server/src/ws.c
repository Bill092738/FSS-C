#include "ws.h"

#include "auth.h"
#include "db.h"
#include "fss_fio_ext.h"
#include "http.h"
#include "live.h"
#include "rules.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* *****************************************************************************
Per-connection state

Lives in the handle's udata2. Between authentication and on_open udata2 holds
the user id itself (an integer, so a refused upgrade leaks nothing); on_open
replaces it with this struct and on_close frees it. cstl runs a connection's
on_message callbacks one at a time, so the struct needs no lock.
***************************************************************************** */

typedef enum { CH_SPOT = 1, CH_BLDG = 2 } ch_kind_e;

#define SUB_EXPLICIT 1 /* requested with "sub" */
#define SUB_VIEW 2     /* added by "view" */

typedef struct {
  uint64_t key; /* kind << 56 | id */
  uint8_t flags;
} ws_sub_s;

typedef struct {
  int64_t uid; /* 0 for anonymous connections */
  size_t n, cap;
  ws_sub_s subs[];
} ws_conn_s;

static uint64_t ch_key(ch_kind_e kind, int64_t id) {
  return ((uint64_t)kind << 56) | (uint64_t)id;
}

static int ch_name(char *buf, size_t cap, uint64_t key) {
  return snprintf(buf, cap, "%s:%llu", (key >> 56) == CH_SPOT ? "spot" : "bldg",
                  (unsigned long long)(key & ((1ULL << 56) - 1)));
}

static ws_sub_s *sub_find(ws_conn_s *c, uint64_t key) {
  for (size_t i = 0; i < c->n; ++i)
    if (c->subs[i].key == key)
      return &c->subs[i];
  return NULL;
}

/* Forwards a Pub/Sub message to the socket. The cache's replay may start one
 * message before `replay_since` (its search steps back one slot), so older
 * messages are dropped here; udata holds the subscription's replay tick. */
static void forward_message(fio_pubsub_msg_s *msg) {
  if (msg->timestamp < (uint64_t)(uintptr_t)msg->udata)
    return;
  FIO_HTTP_WEBSOCKET_SUBSCRIBE_DIRECT_TEXT(msg);
}

static void channel_subscribe(fio_http_s *h, uint64_t key, int64_t since) {
  char ch[48];
  int len = ch_name(ch, sizeof(ch), key);
  uint64_t tick = since ? (uint64_t)fss_pubsub_tick(since) : 0;
  /* a cursor ahead of the server clock must not filter live messages */
  uint64_t now = (uint64_t)fio_io_last_tick();
  if (tick > now)
    tick = now;
  fio_http_subscribe(h, .channel = FIO_BUF_INFO2(ch, (size_t)len),
                     .on_message = forward_message,
                     .udata = (void *)(uintptr_t)tick, .replay_since = tick);
}

/* Clears `flag`; drops the subscription once no reason to keep it remains. */
static void sub_release(fio_http_s *h, ws_conn_s *c, ws_sub_s *s, uint8_t flag) {
  s->flags &= (uint8_t)~flag;
  if (s->flags)
    return;
  char ch[48];
  int len = ch_name(ch, sizeof(ch), s->key);
  fio_pubsub_unsubscribe(.io = fio_http_io(h),
                         .channel = FIO_BUF_INFO2(ch, (size_t)len));
  *s = c->subs[--c->n];
}

/* *****************************************************************************
Replies (written only from this connection's own callbacks)
***************************************************************************** */

static void reply(fio_http_s *h, char *json) {
  fio_http_websocket_write(h, json, fio_bstr_len(json), 1);
  fio_bstr_free(json);
}

static void reply_error(fio_http_s *h, fio_str_info_s op, const char *code,
                        const char *message) {
  static const char head[] = "{\"t\":\"error\",\"op\":";
  char *out = fio_bstr_write(NULL, head, sizeof(head) - 1);
  out = op.len ? fss_bstr_json_str(out, op.buf, op.len)
               : fio_bstr_write(out, "null", 4);
  out = fio_bstr_printf(out, ",\"code\":\"%s\",\"message\":", code);
  out = fss_bstr_json_str(out, message, strlen(message));
  reply(h, fio_bstr_write(out, "}", 1));
}

static void reply_ok_channel(fio_http_s *h, const char *op, fio_str_info_s ch) {
  char *out = fio_bstr_printf(NULL, "{\"t\":\"ok\",\"op\":\"%s\",\"ch\":", op);
  out = fss_bstr_json_str(out, ch.buf, ch.len);
  reply(h, fio_bstr_write(out, "}", 1));
}

/* *****************************************************************************
Upgrade and lifetime
***************************************************************************** */

int fss_ws_authenticate(fio_http_s *h) {
  fio_str_info_s path = fio_http_opath(h);
  if (!(path.len == 3 && !memcmp(path.buf, "/ws", 3)) &&
      !(path.len == 4 && !memcmp(path.buf, "/ws/", 4)))
    return -1;
  /* browsers send cookies with cross-site WebSocket handshakes */
  if (!fss_origin_ok(h) || !fss_db())
    return -1;
  /* anonymous clients may watch public channels; an invalid or expired
   * session cookie simply makes the connection anonymous, as on REST */
  fss_http_udata2_set(h, (void *)(intptr_t)fss_auth_user(h));
  return 0;
}

void fss_ws_on_open(fio_http_s *h) {
  size_t cap = (size_t)FSS_RULES.ws_max_subs;
  ws_conn_s *c = malloc(sizeof(*c) + cap * sizeof(c->subs[0]));
  if (!c) {
    FIO_LOG_ERROR("WebSocket: out of memory, closing connection");
    fss_http_udata2_set(h, NULL);
    fio_io_close(fio_http_io(h));
    return;
  }
  *c = (ws_conn_s){.uid = (int64_t)(intptr_t)fss_http_udata2(h), .cap = cap};
  fss_http_udata2_set(h, c);
  if (c->uid) {
    /* personal notifications (karma, coupons); clients cannot subscribe to
     * other users' channels */
    char ch[40];
    int len = snprintf(ch, sizeof(ch), "user:%lld", (long long)c->uid);
    fio_http_subscribe(h, .channel = FIO_BUF_INFO2(ch, (size_t)len),
                       .on_message = FIO_HTTP_WEBSOCKET_SUBSCRIBE_DIRECT_TEXT);
  }
}

void fss_ws_on_close(fio_http_s *h) {
  /* subscriptions belong to the IO and are released by cstl */
  free(fss_http_udata2(h));
  fss_http_udata2_set(h, NULL);
}

/* *****************************************************************************
Channel parsing and existence checks
***************************************************************************** */

static const char SQL_WS_SPOT[] =
    "SELECT 1 FROM spot WHERE id = ?1 AND status != 'merged'";
static const char SQL_WS_BLDG[] = "SELECT 1 FROM building WHERE id = ?1";

/* Returns 1 if the row exists, 0 if not, -1 on database error. */
static int row_exists(const char *sql, int64_t id) {
  sqlite3_stmt *st = fss_stmt(sql);
  if (!st)
    return -1;
  sqlite3_bind_int64(st, 1, id);
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

static int has_prefix(fio_str_info_s s, const char *p, size_t n) {
  return s.len > n && !memcmp(s.buf, p, n);
}

/* Parses "<prefix>:<positive id>"; returns the id or 0. */
static int64_t parse_id(fio_str_info_s s, size_t prefix_len) {
  int64_t id;
  if (fss_parse_i64(s.buf + prefix_len, s.len - prefix_len, &id) || id <= 0 ||
      id >= (1LL << 56))
    return 0;
  return id;
}

/* Resolves a client channel name. Returns the key, or 0 after replying with
 * an error. Sets *own_user when the channel is the caller's user channel. */
static uint64_t resolve_channel(fio_http_s *h, ws_conn_s *c, fio_str_info_s op,
                                fio_str_info_s ch, int *own_user) {
  *own_user = 0;
  ch_kind_e kind;
  size_t plen;
  if (has_prefix(ch, "spot:", 5)) {
    kind = CH_SPOT;
    plen = 5;
  } else if (has_prefix(ch, "bldg:", 5)) {
    kind = CH_BLDG;
    plen = 5;
  } else if (has_prefix(ch, "user:", 5)) {
    if (c->uid && parse_id(ch, 5) == c->uid) {
      *own_user = 1;
      return 0;
    }
    reply_error(h, op, "forbidden", "user channels are private");
    return 0;
  } else if (has_prefix(ch, "swm:", 4)) {
    reply_error(h, op, "not_available", "Study With Me is not available yet");
    return 0;
  } else {
    reply_error(h, op, "bad_channel", "channel must be spot:{id} or bldg:{id}");
    return 0;
  }
  int64_t id = parse_id(ch, plen);
  if (!id) {
    reply_error(h, op, "bad_channel", "channel must be spot:{id} or bldg:{id}");
    return 0;
  }
  int found = row_exists(kind == CH_SPOT ? SQL_WS_SPOT : SQL_WS_BLDG, id);
  if (found <= 0) {
    reply_error(h, op, found < 0 ? "db_error" : "not_found",
                found < 0 ? "database error" : "no such spot or building");
    return 0;
  }
  return ch_key(kind, id);
}

/* *****************************************************************************
Operations
***************************************************************************** */

static fio_str_info_s str_field(FIOBJ o, const char *key) {
  FIOBJ v = fiobj_hash_get2(o, key, strlen(key));
  return FIOBJ_TYPE_IS(v, FIOBJ_T_STRING) ? fiobj2cstr(v) : (fio_str_info_s){0};
}

static void op_sub(fio_http_s *h, ws_conn_s *c, fio_str_info_s op, FIOBJ msg) {
  fio_str_info_s ch = str_field(msg, "ch");
  FIOBJ since_o = fiobj_hash_get2(msg, "since", 5);
  int64_t since = 0;
  if (since_o) {
    if (!FIOBJ_TYPE_IS(since_o, FIOBJ_T_NUMBER) || fiobj2i(since_o) < 0) {
      reply_error(h, op, "bad_since", "since must be epoch milliseconds");
      return;
    }
    since = fiobj2i(since_o);
  }
  int own_user;
  uint64_t key = resolve_channel(h, c, op, ch, &own_user);
  if (own_user) { /* subscribed automatically at connect */
    reply_ok_channel(h, "sub", ch);
    return;
  }
  if (!key)
    return;
  ws_sub_s *s = sub_find(c, key);
  if (!s) {
    if (c->n >= c->cap) {
      reply_error(h, op, "too_many_subscriptions", "subscription limit reached");
      return;
    }
    s = &c->subs[c->n++];
    *s = (ws_sub_s){.key = key};
  }
  /* subscribing again replaces the subscription, which lets a reconnecting
   * client ask for a replay on a channel it already watches */
  if (!s->flags || since)
    channel_subscribe(h, key, since);
  s->flags |= SUB_EXPLICIT;
  /* cstl defers the subscription to the IO thread and writes go through the
   * same queue, so this acknowledgement leaves after the subscription is live */
  reply_ok_channel(h, "sub", ch);
}

static void op_unsub(fio_http_s *h, ws_conn_s *c, fio_str_info_s op,
                     FIOBJ msg) {
  fio_str_info_s ch = str_field(msg, "ch");
  int own_user;
  uint64_t key = resolve_channel(h, c, op, ch, &own_user);
  if (own_user) {
    reply_error(h, op, "forbidden", "the user channel cannot be unsubscribed");
    return;
  }
  if (!key)
    return;
  ws_sub_s *s = sub_find(c, key);
  if (s)
    sub_release(h, c, s, SUB_EXPLICIT);
  reply_ok_channel(h, "unsub", ch);
}

static const char SQL_WS_CAMPUS[] = "SELECT id FROM campus WHERE slug = ?1";
static const char SQL_WS_VIEW[] =
    "SELECT b.id FROM building b JOIN building_rtree r ON r.id = b.id"
    " WHERE b.campus_id = ?1 AND r.min_lon <= ?4 AND r.max_lon >= ?2"
    "   AND r.min_lat <= ?5 AND r.max_lat >= ?3 ORDER BY b.id";

/* Map viewport (roadmap 9.2): subscribe to bldg:{id} for every building in
 * view and drop view subscriptions that left it. */
static void op_view(fio_http_s *h, ws_conn_s *c, fio_str_info_s op,
                    FIOBJ msg) {
  fio_str_info_s slug = str_field(msg, "campus");
  FIOBJ bb = fiobj_hash_get2(msg, "bbox", 4);
  double box[4];
  int ok = slug.len && FIOBJ_TYPE_IS(bb, FIOBJ_T_ARRAY) &&
           fiobj_array_count(bb) == 4;
  for (uint32_t i = 0; ok && i < 4; ++i) {
    FIOBJ v = fiobj_array_get(bb, i);
    ok = FIOBJ_TYPE_IS(v, FIOBJ_T_NUMBER) || FIOBJ_TYPE_IS(v, FIOBJ_T_FLOAT);
    box[i] = ok ? fiobj2f(v) : 0;
  }
  if (!ok || !(box[0] <= box[2]) || !(box[1] <= box[3]) ||
      fabs(box[1]) > 90 || fabs(box[3]) > 90 || fabs(box[0]) > 180 ||
      fabs(box[2]) > 180) {
    reply_error(h, op, "bad_view",
                "view needs campus and bbox [minLon,minLat,maxLon,maxLat]");
    return;
  }
  sqlite3_stmt *st = fss_stmt(SQL_WS_CAMPUS);
  if (!st) {
    reply_error(h, op, "db_error", "database error");
    return;
  }
  sqlite3_bind_text(st, 1, slug.buf, (int)slug.len, SQLITE_STATIC);
  int64_t campus = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
  fss_stmt_release(st);
  if (!campus) {
    reply_error(h, op, "not_found", "unknown campus");
    return;
  }
  if (!(st = fss_stmt(SQL_WS_VIEW))) {
    reply_error(h, op, "db_error", "database error");
    return;
  }
  sqlite3_bind_int64(st, 1, campus);
  for (int i = 0; i < 4; ++i)
    sqlite3_bind_double(st, i + 2, box[i]);
  /* Collect the new view first (bounded by the subscription limit). */
  uint64_t *keys = malloc(c->cap * sizeof(*keys));
  size_t nk = 0;
  int truncated = 0, rc = SQLITE_NOMEM;
  while (keys && (rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (nk == c->cap) {
      truncated = 1;
      rc = SQLITE_DONE;
      break;
    }
    keys[nk++] = ch_key(CH_BLDG, sqlite3_column_int64(st, 0));
  }
  fss_stmt_release(st);
  if (rc != SQLITE_DONE) {
    if (keys)
      reply_error(h, op, "db_error", "database error");
    else
      reply_error(h, op, "unavailable", "out of memory");
    free(keys);
    return;
  }
  /* drop buildings that left the view */
  for (size_t i = c->n; i-- > 0;) {
    ws_sub_s *s = &c->subs[i];
    if (!(s->flags & SUB_VIEW))
      continue;
    int keep = 0;
    for (size_t k = 0; k < nk && !keep; ++k)
      keep = keys[k] == s->key;
    if (!keep)
      sub_release(h, c, s, SUB_VIEW);
  }
  /* add the new ones while room remains */
  static const char head[] = "{\"t\":\"ok\",\"op\":\"view\",\"buildings\":[";
  char *out = fio_bstr_write(NULL, head, sizeof(head) - 1);
  size_t listed = 0;
  for (size_t k = 0; k < nk; ++k) {
    ws_sub_s *s = sub_find(c, keys[k]);
    if (!s) {
      if (c->n >= c->cap) {
        truncated = 1;
        continue;
      }
      s = &c->subs[c->n++];
      *s = (ws_sub_s){.key = keys[k]};
      channel_subscribe(h, keys[k], 0);
    }
    s->flags |= SUB_VIEW;
    out = fio_bstr_printf(out, "%s%llu", listed++ ? "," : "",
                          (unsigned long long)(keys[k] & ((1ULL << 56) - 1)));
  }
  free(keys);
  out = fio_bstr_printf(out, "],\"truncated\":%s}", truncated ? "true" : "false");
  reply(h, out);
}

void fss_ws_on_message(fio_http_s *h, fio_buf_info_s data, uint8_t is_text) {
  ws_conn_s *c = fss_http_udata2(h);
  fio_str_info_s none = {0};
  if (!c)
    return;
  if (!is_text) {
    reply_error(h, none, "bad_message", "messages must be JSON text");
    return;
  }
  FIOBJ msg = fiobj_json_parse2(data.buf, data.len, NULL);
  fio_str_info_s op = FIOBJ_TYPE_IS(msg, FIOBJ_T_HASH) ? str_field(msg, "op")
                                                       : none;
  if (!op.len)
    reply_error(h, none, "bad_message", "expected a JSON object with \"op\"");
  else if (!fss_db())
    reply_error(h, op, "unavailable", "database connection unavailable");
  else if (op.len == 3 && !memcmp(op.buf, "sub", 3))
    op_sub(h, c, op, msg);
  else if (op.len == 5 && !memcmp(op.buf, "unsub", 5))
    op_unsub(h, c, op, msg);
  else if (op.len == 4 && !memcmp(op.buf, "view", 4))
    op_view(h, c, op, msg);
  else if (op.len == 7 && !memcmp(op.buf, "swm_msg", 7))
    reply_error(h, op, "not_available", "Study With Me is not available yet");
  else
    reply_error(h, op, "unknown_op", "unknown op");
  fiobj_free(msg);
}
