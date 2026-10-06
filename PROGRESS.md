# FSS Implementation Progress

Tracks implementation of `FSS Roadmap.md` (the roadmap). All other project
documents, code, and comments are written in English.

Last updated: 2026-10-06

## Milestones

The roadmap's milestone diagram (section 11) is not embedded in the Markdown
file, so the milestones below are derived from its sections. M1 and M2 can run
in parallel: M2 starts on a hand-made seed (roadmap 11).

| # | Milestone | Roadmap sections | Gate | Status |
| --- | --- | --- | --- | --- |
| M0 | Foundation: vendoring, build, DB layer, migrations, router, error envelope, test harnesses | 4.1–4.4, 5.1, 10.1, 10.3 | `make test` green; `/api/v1/health` answers | **Done** |
| M1 | Phase 0 pipeline (collect, buildings, extract, resolve, load) for OSU | 6 | 100+ active spots loaded; 100-claim audit ≥ 90% | Not started |
| M2 | Core API: auth, campuses, spot search/detail, claims + votes + materialization, dev seed | 5.3, 7.1, 7.6 (claims), 9.1, 10.5 | pytest covers search filters, auth, claim materialization | **Done** (IP rate limit on `/auth/*` moved to M4) |
| M3 | Real time: reports, geofence, occupancy estimate, WebSocket + Pub/Sub, timers | 4.5, 4.6, 7.2, 7.3, 9.2 | two-client WebSocket test sees color change; replay with `since` | **Done** (forecast prior is the spot's `crowd_typical` until M5) |
| M4 | Community: check-ins, karma ledger, badges, reputation, rate limits (incl. `/auth/*` per IP), photos, spot discovery confirmation | 7.4–7.6, 10.5 | idempotency + rate-limit tests; simulator shows cheaters suppressed | Next |
| M5 | Forecast and Phase 2: hourly rollup, forecast, Study With Me, heatmap (k-anonymity) | 7.7, 8.1, 8.2 | forecast endpoint on simulated history; k=5 filter tests | Not started |
| M6 | Monetization, front-end, load test: offers/coupons, merchant redeem, admin insights + CSV, web app, k6 | 8.3, 8.4, 10.2, 10.4 | end-to-end demo; k6 report written back into docs | Not started |

## How to run

```
python3 -m venv .venv && .venv/bin/pip install pytest websockets   # once
make              # server/build/fss (first build ~1.5 min: cstl + SQLite)
make debug        # server/build/fss-debug with ASan/UBSan (clang)
make test         # C unit tests + pytest API tests
make debug test SAN_CC=gcc   # where clang's sanitizer runtime is missing
make seed         # data/fss.db from the synthetic demo fixture
make run          # serves on 0.0.0.0:8080 using data/fss.db
FSS_BIN=server/build/fss-debug make api   # API tests against the ASan build
server/build/fss --help                   # all CLI options
```

Test status (2026-10-06): 110 unit checks and 51 API tests pass on the release
build and on the ASan/UBSan build (built with `SAN_CC=gcc`: this container has
gcc's libasan but not clang's runtime). Server logs of the ASan runs contain no
sanitizer reports and no cstl leak-counter errors, also after a shutdown with
open WebSocket connections.

## Implemented

### Repository layout (roadmap 10.1)

```
Makefile                     make / debug / unit / api / test / run / seed
data/seed/demo.sql           synthetic "Demo University" fixture (NOT real data)
server/
  Makefile                   release (gcc -O2) and debug (clang ASan+UBSan)
  config/rules.json          tunables (roadmap 7), unknown keys rejected
  vendor/                    fio-stl.h (cstl a24d0be), SQLite 3.53.4, VERSIONS.md
  migrations/0001_core.sql   core schema (5.2) + attr_def v1 (6.4)
  migrations/0002_live.sql   report.fence, event index
  src/fss_fio.h, fio_impl.c  cstl include / single implementation unit
  src/fss_fio_ext.h          exported wrappers for cstl FIO_IFUNCs (udata2)
  src/main.c                 CLI, migrations, worker DB connections, job queue, listener, static/SPA
  src/db.c                   PRAGMAs, migrations, per-thread stmt cache, tx, SQL functions
  src/http.c                 JSON responses, error envelope, query/body helpers
  src/api.c                  /api/v1 route table and dispatcher
  src/api_meta.c             /health, /campuses, /campuses/:slug
  src/api_auth.c             /auth/register|login|logout, GET|PATCH /me
  src/api_spots.c            GET|POST /spots, GET /spots/:id, POST /spots/:id/claims, POST /claims/:id/vote
  src/api_reports.c          POST /spots/:id/reports
  src/live.c                 occupancy_live, event visibility, commit + publish outbox, live_decay
  src/ws.c                   /ws: upgrade auth, sub / unsub / view, replay filter
  src/jobs.c                 live_decay and wal_checkpoint timers on the 1-thread job queue
  src/geo.c                  point in polygon, distance to fence (pure)
  src/auth.c                 Argon2id, session tokens, cookie, Origin check
  src/attrs.c                in-memory attr_def registry + value validation
  src/spots.c                claim -> spot materialization + FTS row
  src/rules.c                pure formulas (fence factor, estimator, ...) and tunables; rules_load.c reads and validates rules.json
  src/router.c, util.c       pure path matcher, query / number / FTS helpers
  tests/unit/                dependency-free C harness
tests/api/                   pytest black-box suite (fresh server + temp DB per module)
```

### Endpoints (roadmap 9.1)

| Endpoint | Notes |
| --- | --- |
| `GET /health` | schema version, server time |
| `GET /campuses`, `GET /campuses/:slug` | campus config incl. the attr_def registry for front-end filters |
| `POST /auth/register`, `/auth/login`, `/auth/logout` | Argon2id (19 MiB, t=4, ~91 ms here); `fss_sid` cookie (HttpOnly, SameSite=Lax); DB stores blake2b-256 of the token; login spends a hash for unknown accounts |
| `GET /me`, `PATCH /me` | profile, langs, courses, campus, karma, reputation, badges |
| `GET /spots` | `campus`, `bbox` (R*Tree), `must`/`not` (bitmap; `must=outlets` means ≥ 1), `quiet` 0–3, `vibe`, `near` (meters, equirectangular), `q` (FTS5, every token quoted), `limit`, `cursor` |
| `GET /spots/:id` | materialized attributes with confidence, all claims with evidence and source URL, live state |
| `POST /spots` | user submission, starts `hidden`, optional initial claims, 5/day |
| `POST /spots/:id/claims` | user claim (prior 0.5), 10/day, duplicates return 409 with the existing claim id |
| `POST /claims/:id/vote` | `v` = 1 / -1 / 0; weight = reputation × 1.5 with a verified check-in in the building; own claims rejected |
| `POST /spots/:id/reports` | `{"level": 0-2}` or `{"event": "outlet_broken" \| "wifi_down" \| "closed_event"}`, optional `lat`/`lon`/`accuracy_m` (all three); returns the report (`fence`, `weight`) and the new `live` state or `event` state; active spots only (409 otherwise); 429 after 3 other buildings in an hour |

`GET /spots/:id` also returns `events`: visible event reports with `until`.

Every write endpoint requires `Content-Type: application/json` and rejects a
mismatching `Origin` (roadmap 10.5).

### Real time (roadmap 3, 4.5, 4.6, 7.2, 7.3, 9.2)

- **Geofence (7.2):** the fence polygon comes from `building.fence_json` via
  `json_each`; ray casting plus the distance to the boundary in an
  equirectangular projection give g = 1.0 / 0.6 / 0.2.
- **Estimate (7.3):** `report.weight` stores reputation × g; the estimator
  applies `2^(-Δt/20 min)`, ignores reports older than 90 min, and mixes in the
  forecast with w0 = 0.5. `basis` is `reports` when Σw ≥ 0.5. Only a user's last
  report per spot within 10 minutes counts (7.6).
- **Events (7.3):** visible for 4 h after the last report once 2 distinct users
  reported them, or one user with g = 1.0 and reputation ≥ 1.5.
- **Publish:** writers queue messages in an outbox; `fss_live_commit` commits and
  publishes under one lock, so subscribers see changes in commit order. `live`
  messages go out only when color or basis changes; both `live` and `event`
  messages go to `spot:{id}` and `bldg:{building}`.
- **WebSocket `/ws` (9.2):** `sub` (`spot:*`, `bldg:*`, own `user:*`; optional
  `since`), `unsub`, `view` (campus + bbox → `bldg:*`, view subscriptions are
  dropped when they leave the view; explicit ones stay). Up to 200
  subscriptions per connection. Logged-in connections are subscribed to
  `user:{id}` automatically. Every op is answered with `{"t":"ok",...}` or
  `{"t":"error","code":...}`; the `sub` acknowledgement is sent after the
  subscription is live. `swm:*` / `swm_msg` answer `not_available` until M5.
- **Replay:** the cstl in-memory history is attached; `since` (epoch ms, the
  `at` of the last message seen) replays messages with `at >= since`.
- **Jobs (4.6):** `live_decay` (60 s) recomputes every spot with conf > 0 and
  publishes changes; `wal_checkpoint` (10 min) runs `PASSIVE`. Both run on a
  one-thread async queue with its own connection.
- **Test clock (10.4):** with `--test-clock`, an API request's `X-FSS-Now`
  header (epoch ms) is the request's clock.

## Decisions and deviations from the roadmap

| Topic | Roadmap | Implemented | Reason |
| --- | --- | --- | --- |
| Static files | listener `public_folder` | explicit `fio_http_static_file_response` in the root handler | cstl docs: routes inherit `public_folder`, so `/api/v1/*` would probe the file system first |
| JSON list ordering | `json_group_array` over an ordered subquery | rows rendered by `json_object`, array assembled in C | roadmap 5.3: SQLite does not guarantee that order |
| Path parameters | `/:id/reports` | `#id` = numeric segment, `:slug` = any segment | non-numeric ids are rejected before the handler runs |
| Formulas in SQL | inline SQL expressions | `fss_conf()` / `fss_color()` SQL functions backed by `rules.c` | one implementation for C, SQL and unit tests |
| Sanitizer builds | gcc `-fsanitize` | clang (`SAN_CC`) | gcc's libasan is not installed here |
| Vendor warnings | disable `-Wstringop-overflow` for `fio_impl.c` | `-w` for `fio_impl.c`, `-isystem vendor` elsewhere | gcc 15 warns more than gcc 13 |
| SQLite | system lib or amalgamation | amalgamation 3.53.4 + `SQLITE_ENABLE_MATH_FUNCTIONS` | no system headers; `sqrt` for distances |
| Argon2 cost | tune to ~100 ms | m=19 MiB, t=4, p=1 (91 ms); stored per user in `pw_params` | `fss --bench-password` re-measures |
| Search ranking | `w_quiet*noise + w_dist*deg² + w_live*est` | same terms, distance in meters/100, plus `- w_quality*quality` | degrees² is not isotropic; quality lets verified spots rank first |
| `must` on ordinals | not specified | only `outlets` (≥ 1); other ordinals → 422 `attr_not_filterable` | "must=late_night,outlets" appears in roadmap 7.1 |
| Duplicate user claim | not specified | 409 `duplicate_claim` with the existing claim id | same value should be a vote, not a second claim |
| Schema additions | — | `user.role`, `user.pw_params`, `spot.attrs_json`, `spot.created_by`, `attr_def.grp`/`sort`, `checkin.last_beat_at`/`verified_ms`/`outside_beats`, `auth_session.created_at`; vote tallies are REAL | roles (8.4), stored hash params (10.5), detail page, weighted votes (7.6), heartbeats (7.4) |
| FTS | `content=''` | `content='', contentless_delete=1`; tags hold flag keys + English and Chinese labels + vibe | replace a spot's row on re-materialization; searchable labels |
| `karma_ledger.ref_type/ref_id` | nullable | `NOT NULL` | roadmap 5.3: NULLs defeat the UNIQUE idempotency key |
| Test clock | `X-FSS-Now` in debug builds only | any build, only with `--test-clock` (off by default) | the same API tests run against the release and the ASan binary |
| Fence factor | inside + accuracy ≤ 50 → 1.0, outside within accuracy ≤ 100 → 0.6 | also 0.6 when inside with 50 < accuracy ≤ 100 | distance to the building is 0, so the 0.6 row applies rather than 0.2 |
| Forecast in the estimate | `forecast_slot` | spot's materialized `crowd_typical`, else `live_default_forecast` (1.0) | forecasts are M5; the table stays empty until then |
| Report limits (7.6) | in-memory `FIO_MAP` sharded by user | SQL: dedupe inside the estimator query, building limit counted from `report` (index `report_user_at`) | persistent across restarts, no locking, reports are stored anyway |
| Report storage | `weight` | `weight` = reputation × g (time-independent part) + new `fence` column | decay is applied at estimation time; events need g to find trusted reports |
| Event reports | "TTL 4 h", "high reputation user" | kinds `outlet_broken`, `wifi_down`, `closed_event`; `until` = last report + 4 h; trusted = reputation ≥ 1.5 (`event_trusted_rep`) | the roadmap names no threshold or full kind list |
| Hidden spots | not specified | reports return 409 `spot_not_active` | crowding of an unconfirmed spot is meaningless |
| WebSocket auth | verify the cookie, reject otherwise | path must be `/ws`; Origin must match Host; no or invalid cookie = anonymous connection limited to public channels | spot and building state is public on REST too; Origin stops cross-site hijacking |
| WebSocket replies | only Pub/Sub bodies | plus `{"t":"ok"}` / `{"t":"error"}` per op | clients (and tests) know when a subscription is live |
| `campus:{slug}:live` | channel for map viewers | not published; `view` subscribes to `bldg:*` | 9.2 defines `view` through building channels; avoids a campus-wide broadcast |
| `since` / `at` | epoch ms passed to `replay_since` | `at` = publish tick + fixed epoch offset; `since` converted back | cstl Pub/Sub timestamps are monotonic ms (see notes) |
| Job timers | `fio_io_run_every` on the IO thread + `fio_io_async` | `fio_io_async_every` on the job queue | the documented helper does both steps |

Known limitation: FTS uses `unicode61` as specified, which does not segment
Chinese text; a query token must equal a whole token (e.g. the tag "白板"
matches, a substring of a longer Chinese phrase does not). A `trigram`
tokenizer would fix this if needed.

## cstl API notes (verified against docs/headers of a24d0be)

- `FIO_HTTP` pulls in JSON, multipart, URL-encoded, Pub/Sub, IPC and IO
  (`000 dependencies.h`). `FIO_RAND` is needed for `fio_rand_bytes_secure`.
- Linking needs `-lssl -lcrypto` because OpenSSL headers are present.
- `FIO_CALL_ON_WORKER_THREAD_START/END` are forced by queue worker threads
  (`102 queue.h`), so per-thread SQLite connections are opened there.
- `fio_timer_schedule_args_s` uses `on_finish` (`401 io api.md` still says
  `on_stop`).
- `fio_bstr_write_escape` escapes with JSON string rules.
- Upgraded-connection callbacks, `queue`, `log` and limits are read from the
  listener's root settings only.
- **FIO_EXTERN gap:** `fio_http_udata`, `fio_http_udata2`, `fio_http_cdata`,
  `fio_http_controller` (and their `_set`) are `FIO_IFUNC` declarations whose
  bodies are only emitted in the implementation unit (`432 http types.h` is
  guarded by `FIO_EXTERN_COMPLETE`). `src/fio_impl.c` exports
  `fss_http_udata2` / `fss_http_udata2_set` (declared in `fss_fio_ext.h`).
- **WebSocket callback threads:** `on_authenticate_websocket` and `on_message`
  run on the listener's `queue` (so they can use the worker's SQLite
  connection); `on_open` (protocol attach) and `on_close` run on the IO thread.
  A connection's `on_message` calls are serialized (the socket is suspended
  until the callback returns). Routes inherit `on_authenticate_websocket`, so
  the callback must check the path itself (`fio_http_opath`).
- **Subscription order:** `fio_pubsub_subscribe` defers the registration to the
  IO queue, and `fio_io_write2` (used by `fio_http_websocket_write`) queues the
  write on the same queue, so a reply written right after subscribing leaves
  after the subscription exists. Subscription callbacks default to the IO
  queue. `fio_io_env_set` replaces an existing subscription with the same
  channel.
- **Pub/Sub timestamps are monotonic:** `fio_pubsub_publish` defaults
  `timestamp` to `fio_io_last_tick()`, which is `CLOCK_MONOTONIC` ms, and
  `replay_since` is compared with it. The roadmap's epoch-ms `since` therefore
  cannot be passed through unchanged.
- **History replay starts one message early:** the cache's binary search in
  `fio___pubsub_history_cache_replay` steps back one slot after finding the
  first message at or after `since`, so one older message is replayed. It never
  skips messages. `ws.c` drops messages older than the subscription's replay
  tick.
- **History setup:** no history manager is attached by default; call
  `fio_pubsub_history_attach(fio_pubsub_history_cache(0), prio)`. The size
  argument only applies while no limit is set, and the module constructor
  already sets 256 MiB (or `WEBSITE_MEMORY_LIMIT[_MB|_KB]`), so passing a size
  later has no effect.
- **Async queue timers leak at exit:** `fio_io_async_every` timers live in the
  queue's `timers`, which cstl does not destroy on shutdown (it destroys only
  the reactor's own timer queue). `fss_jobs_release` calls `fio_timer_destroy`
  after `fio_io_start` returns.
- **Error pages (since `a24d0be`):** `fio_http_send_error_response` (also
  used internally, e.g. for 413/431) looks for `<status>.html` in `./` and then
  in the route's `public_folder`. FSS sets no `public_folder` and sends its own
  JSON errors, so only a `<status>.html` in the working directory is picked up,
  as before.
- **`compress_ws` stays off:** with permessage-deflate the per-connection
  compressor is shared by every write, so replies and Pub/Sub forwarding from
  different threads could interleave. Without it, each frame is one
  `fio_io_write2` call.

## Bugs found and fixed

- Worker threads raced on `PRAGMA journal_mode=WAL` before `busy_timeout`
  was active; one worker could end up without a connection (503s). Fixed by
  calling `sqlite3_busy_timeout` before any PRAGMA; a worker that still cannot
  open the database now stops the server instead of serving 503s.

## Next steps

1. M4: check-ins (open / heartbeat / end, `checkin_timeout` job), check-ins as
   implicit reports of weight 0.3, karma ledger (+2 for a report with g ≥ 0.6,
   +5 for a confirmed event, daily caps) with `{"t":"karma"}` on `user:{id}`,
   badges, reputation updates, per-IP `/auth/*` limit, photos, spot discovery
   confirmation. Reports have no per-user frequency cap yet besides the
   building limit and the dedupe window.
2. M5 replaces the `crowd_typical` forecast prior with `forecast_slot`.
3. M1 pipeline can start any time; it only needs the schema.

## Open questions

- Milestone definitions above are inferred; replace them if the roadmap's
  diagram defines M0–M6 differently.
- First campus is assumed to be OSU (roadmap 11.2); until M1 runs, development
  uses the synthetic "demo" campus.
- Event kinds (`outlet_broken`, `wifi_down`, `closed_event`) and the trusted
  reputation threshold (1.5) are assumptions; adjust `EVENT_KINDS` in `live.c`
  and `event_trusted_rep` in `rules.json`.
