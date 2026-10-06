/* WebSocket protocol on /ws (roadmap 4.5, 9.2).
 *
 * Client -> server:
 *   {"op":"sub","ch":"spot:42","since":1790000000000}   since is optional
 *   {"op":"unsub","ch":"spot:42"}
 *   {"op":"view","campus":"demo","bbox":[minLon,minLat,maxLon,maxLat]}
 * Server -> client: Pub/Sub message bodies as published (see live.c), plus
 *   {"t":"ok","op":...} acknowledgements and {"t":"error","op":...,"code":...}.
 *
 * Callback threads: authentication and on_message run on the HTTP task queue
 * (they may use the database); on_open and on_close must not touch it.
 * Business code never writes to another connection's socket: it publishes,
 * and subscriptions forward the message (roadmap 4.5).
 */
#ifndef FSS_WS_H
#define FSS_WS_H

#include "fss_fio.h"

int fss_ws_authenticate(fio_http_s *h);
void fss_ws_on_open(fio_http_s *h);
void fss_ws_on_message(fio_http_s *h, fio_buf_info_s msg, uint8_t is_text);
void fss_ws_on_close(fio_http_s *h);

#endif /* FSS_WS_H */
