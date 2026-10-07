# FSS Implementation Progress

Tracks implementation of `FSS Roadmap.md` (the roadmap). All other project
documents, code, and comments are written in English.

Last updated: 2026-10-07

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
| M4 | Community: check-ins, karma ledger, badges, reputation, rate limits (incl. `/auth/*` per IP), photos, spot discovery confirmation | 7.4–7.6, 10.5 | idempotency + rate-limit tests; simulator shows cheaters suppressed | **Done** (backend; web wiring in progress) |
| M5 | Forecast and Phase 2: hourly rollup, forecast, Study With Me, heatmap (k-anonymity) | 7.7, 8.1, 8.2 | forecast endpoint on simulated history; k=5 filter tests | Not started |
| M6 | Monetization, front-end, load test: offers/coupons, merchant redeem, admin insights + CSV, web app, k6 | 8.3, 8.4, 10.2, 10.4 | end-to-end demo; k6 report written back into docs | Not started (web app started early, see below) |

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
.venv/bin/python tests/sim/simulate.py    # cheater simulator (see its docstring)
```

Test status (2026-10-07): 191 unit checks and 77 API tests pass on the release
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
  migrations/0003_community.sql  rep_ledger, photo_vote, spot_confirm, job_cursor, badges, check-in columns
  src/fss_fio.h, fio_impl.c  cstl include / single implementation unit
  src/fss_fio_ext.h          exported wrappers for cstl FIO_IFUNCs (udata2)
  src/main.c                 CLI, migrations, worker DB connections, job queue, listener, static/SPA
  src/db.c                   PRAGMAs, migrations, per-thread stmt cache, tx, SQL functions
  src/http.c                 JSON responses, error envelope, query/body helpers
  src/api.c                  /api/v1 route table and dispatcher
  src/api_meta.c             /health, /campuses, /campuses/:slug
  src/api_auth.c             /auth/register|login|logout, GET|PATCH /me
  src/api_spots.c            GET|POST /spots, GET /spots/:id, POST /spots/:id/claims, POST /claims/:id/vote
  src/api_reports.c          POST /spots/:id/reports (+ report karma, event confirmation)
  src/api_checkins.c         POST /checkins, /checkins/:id/heartbeat, /checkins/:id/end
  src/api_community.c        spot confirmation, photos, photo votes, /me/karma, /debug/jobs
  src/community.c            karma ledger, reputation ledger, badges, reputation rollup
  src/checkin.c              closing check-ins (karma, live state), checkin_timeout
  src/fence.c                position parsing and fence factor for a building
  src/live.c                 occupancy_live, event visibility, commit + publish outbox, live_decay
  src/ws.c                   /ws: upgrade auth, sub / unsub / view, replay filter
  src/jobs.c                 live_decay, checkin_timeout, hourly_rollup, wal_checkpoint on the 1-thread job queue
  src/geo.c                  point in polygon, distance to fence (pure)
  src/auth.c                 Argon2id, session tokens, cookie, Origin check
  src/attrs.c                in-memory attr_def registry + value validation
  src/spots.c                claim -> spot materialization + FTS row
  src/rules.c                pure formulas (fence factor, estimator, ...) and tunables; rules_load.c reads and validates rules.json
  src/router.c, util.c       pure path matcher, query / number / FTS helpers
  tests/unit/                dependency-free C harness
tests/api/                   pytest black-box suite (fresh server + temp DB per module)
tests/sim/simulate.py        crowd simulator with cheaters (roadmap 10.4)
```

### Endpoints (roadmap 9.1)

| Endpoint | Notes |
| --- | --- |
| `GET /health` | schema version, server time |
| `GET /campuses`, `GET /campuses/:slug` | campus config incl. the attr_def registry for front-end filters |
| `POST /auth/register`, `/auth/login`, `/auth/logout` | Argon2id (19 MiB, t=4, ~91 ms here); `fss_sid` cookie (HttpOnly, SameSite=Lax); DB stores blake2b-256 of the token; login spends a hash for unknown accounts |
| `GET /me`, `PATCH /me` | profile, langs, courses, campus, karma, reputation, badges, open `checkin` |
| `GET /me/karma` | karma ledger, newest first; `limit` (≤ 200), `cursor` (entry id) |
| `GET /spots` | `campus`, `bbox` (R*Tree), `must`/`not` (bitmap; `must=outlets` means ≥ 1), `quiet` 0–3, `vibe`, `near` (meters, equirectangular), `q` (FTS5, every token quoted), `limit`, `cursor` |
| `GET /spots/:id` | materialized attributes with confidence, all claims with evidence and source URL, live state |
| `POST /spots` | user submission, starts `hidden`, optional initial claims, 5/day |
| `POST /spots/:id/claims` | user claim (prior 0.5), 10/day, duplicates return 409 with the existing claim id |
| `POST /claims/:id/vote` | `v` = 1 / -1 / 0; weight = reputation × 1.5 with a verified check-in in the building; own claims rejected |
| `POST /spots/:id/reports` | `{"level": 0-2}` or `{"event": "outlet_broken" \| "wifi_down" \| "closed_event"}`, optional `lat`/`lon`/`accuracy_m` (all three); returns the report (`fence`, `weight`), `karma` and the new `live` state or `event` state; active spots only (409 otherwise); 429 after 3 other buildings in an hour |
| `POST /spots/:id/confirm` | `{}`; confirms a submitted (hidden) spot; active after 2 other users; 403 `own_spot`, 409 `already_active` |
| `POST /spots/:id/photos` | raw image body, `Content-Type: image/*`; JPEG / PNG / WebP by magic bytes; ≤ 5 MB (413); 20 per day; 409 `duplicate_photo` with `photo_id`; served at `/uploads/<sha256>.<ext>` |
| `POST /photos/:id/vote` | `v` = 1 / -1 / 0, weighted like claim votes; own photos rejected; hidden at p < 0.3 with D ≥ 3 |
| `POST /checkins` | `{"spot_id"}` + optional position; 409 `checkin_open` with `checkin_id`; an expired open session is ended first |
| `POST /checkins/:id/heartbeat` | optional position (missing = outside); returns the session, ended when 2 outside in a row or expired |
| `POST /checkins/:id/end` | `{}`; returns the session and the `karma` credited |
| `POST /debug/jobs/:name` | `live_decay`, `checkin_timeout`, `hourly_rollup` at the request clock; only with `--test-clock` (404 otherwise) |

`GET /spots/:id` also returns `events` (visible event reports with `until`),
`present` (open verified check-ins), `confirmations` and up to 20 visible
`photos`.

Every `/auth/*` request counts against a per-IP limit of 10 per minute (429
`rate_limited` with `Retry-After`).

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

### Community (roadmap 7.4–7.6, 10.5)

- **Check-ins (7.4):** a position counts as inside when g ≥ 0.6. A session is
  `verified` once one position was inside. A heartbeat credits the time since
  the previous position when both were inside, at most 20 minutes per beat.
  Two consecutive outside heartbeats end it (`outside`); no heartbeat for 30
  minutes ends it at the last heartbeat (`stale`); 6 hours after the start it
  ends there (`timeout`). Expiry is checked by the `checkin_timeout` job (5
  min), by heartbeats and when the user checks in again.
- **Implicit report (7.4):** open verified check-ins at a spot with a known
  capacity add a fresh observation of level `2 × present / capacity` (capped
  at 2) and weight 0.3 to the estimate. Starting, verifying and ending a
  check-in recompute the spot.
- **Karma (7.5):** `fss_karma_award` writes `karma_ledger` (UNIQUE `(user,
  reason, ref_type, ref_id)`, `ON CONFLICT DO NOTHING`), updates `user.karma`
  in the same transaction and queues `{"t":"karma","delta","total","reason","at"}`
  on `user:{id}`. Daily caps sum the points of a reason over the last 24 h; an
  award that would exceed the cap is cut down or skipped.

  | Reason | Points | When | Cap per 24 h |
  | --- | --- | --- | --- |
  | `report` | +2 | crowding report with g ≥ 0.6; once per spot per 10 min | 20 reports |
  | `event_confirmed` | +5 | another user reports the same event within its TTL; paid once per episode (the author's first report in the window) | — |
  | `photo` / `photo_hidden` | +3 / −(what was paid) | upload / hidden by votes | 5 photos |
  | `claim_accepted` | +3 | the user's claim has 2 up votes | 10 claims |
  | `spot_discovered` | +20 | the user's spot is confirmed by 2 other users | 2 spots |
  | `checkin` | +1 per verified hour | the session ends | 4 |
- **Badges (7.5):** `badge.rule_json` = `{"count": counter, "gte": n}`;
  counters `reports`, `spots_discovered`, `claims_accepted`, `photos`,
  `events_confirmed`, `checkin_hours`, `reputation`, `karma` (defined in
  `community.c`). Checked after every karma award and every report, granted in
  the same transaction, pushed as `{"t":"badge","key","name","at"}`. Ten
  badges are seeded by `0003_community.sql`.
- **Reputation (7.6):** `rep_ledger` mirrors the karma ledger (same UNIQUE
  key); `user.reputation` stays in [0.1, 3.0]. +0.05 for an accepted claim,
  confirmed event, activated spot, photo with 2 up votes, or a crowding report
  that agrees with the consensus; −0.1 for a claim or photo rejected by votes
  (p < 0.3, D ≥ 3) or a report that contradicts the consensus.
- **Reputation rollup (`hourly_rollup`, 7.6):** judges crowding reports once
  the 30 minutes after them are past (cursor in `job_cursor`). The consensus is
  the other users' reports at the same spot within ±30 min: each user counts
  once with their mean level and strongest weight (decayed by the time
  distance); conf = 1 − e^(−Σw). With conf ≥ 0.8, |level − consensus| ≥ 1.5
  rejects and ≤ 0.5 accepts the report.
- **Spot discovery (7.5):** `POST /spots/:id/confirm` records one confirmation
  per user (`spot_confirm`); the second other user activates the spot.
- **Photos (7.5, 10.5):** stored as `<sha256>.<ext>` in `--uploads` (default
  `data/uploads`), written to a temporary name and renamed outside the write
  transaction; served by the root handler at `/uploads/` with a one-year
  max-age.
- **Per-IP limit (10.5):** a 4096-slot fixed-window table (`fss_rl_hit`,
  pure and unit-tested) behind a mutex, keyed by the socket's peer address.

#### Simulator (roadmap 10.4)

`tests/sim/simulate.py` registers honest students and cheaters on a
`--test-clock` server, moves them between four buildings every hour, lets them
report every 10 minutes (cheaters report the opposite of the truth) and runs
`hourly_rollup` every simulated hour. `test_simulator_suppresses_cheaters`
runs it with 10 honest students, 4 cheaters, 6 hours, seed 7.

Results (10 honest + 4 cheaters, 6 h, about 300 reports; error = mean |est −
truth| of the live estimate in hours 3–6):

| Seed | Reputation honest | Reputation cheaters | Error, equal weights | Error, server | Error, honest only | Cheater error removed |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 1.37 | 0.19 | 0.715 | 0.596 | 0.493 | 54 % |
| 2 | 1.38 | 0.10 | 0.736 | 0.568 | 0.443 | 57 % |
| 3 | 1.31 | 0.18 | 0.728 | 0.551 | 0.453 | 64 % |
| 4 | 1.28 | 0.58 | 0.855 | 0.762 | 0.544 | 30 % |
| 5 | 1.26 | 0.16 | 0.803 | 0.644 | 0.484 | 50 % |
| 6 | 1.23 | 0.51 | 0.776 | 0.711 | 0.542 | 28 % |
| 7 (test) | 1.33 | 0.15 | 0.819 | 0.607 | 0.438 | 56 % |

"Honest only" is the floor: the estimate lags when the truth flips every hour
and leans on the forecast prior. Cheaters stay above 0.5 where they happen to
cluster at the same spots, because the consensus there is weak or wrong.

### Web front-end (roadmap 10.2, started ahead of M6)

`web/` covers everything M0–M3 serve, with placeholders for M4–M6 features
listed in `web/src/lib/features.ts`. Details in [web/README.md](web/README.md).

- `make dev` runs the backend and Vite (proxying `/api` and `/ws`); `make web`
  builds `web/dist`, which the C binary serves with SPA fallback; `make
  web-test` runs typecheck, oxlint and 18 Vitest tests.
- Live updates: the map sends `view` for its viewport and the detail page
  subscribes `spot:{id}`; pushes update the React Query cache in place. The
  socket reconnects with backoff and re-subscribes with `since`.
- Checked in Chromium against the real backend (dev proxy and the C binary
  serving `web/dist`): 19 of 20 end-to-end checks pass; the failing one is
  the subscription bug below.

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
| Front-end framework | Vite + Preact + MapLibre | Vite + React 19 + TypeScript + Tailwind CSS 4 + MapLibre | project decision when the web app started |
| Buildings for new spots | not specified | the new-spot form lists buildings of active spots | no endpoint lists a campus's buildings yet |
| Photo upload | `multipart` | the raw image as the body, `Content-Type: image/*` | HTML forms can send multipart cross-site, image types they cannot: keeps the CSRF baseline of 10.5 without a token |
| Upload folder | `public_folder/uploads/` | `--uploads DIR` (default `data/uploads`), served at `/uploads/` | `web/dist` is a build output that `make web` replaces |
| Reputation updates | batched in `hourly_rollup` | crowding reports in `hourly_rollup`; claim, photo, event and spot outcomes in the transaction that decides them | those outcomes are single events with a natural idempotency key; reports need the consensus window to be complete |
| Reputation ledger | not specified | `rep_ledger` with the karma ledger's UNIQUE key | idempotent adjustments and an audit trail |
| Report consensus | "same period" | other users' reports within ±30 min, one vote per user; ≥ 1.5 off rejects, ≤ 0.5 off accepts | the roadmap defines only the rejection; one vote per user keeps a spammer from forming a consensus |
| Daily caps | "per day" | rolling 24 h, in points (count × points per award) | same as the claim and spot limits; check-in karma varies per award |
| Report karma | +2 per report with g ≥ 0.6 | also at most once per user and spot per 10 min | otherwise repeating a report earns karma that the estimate ignores (7.6 dedupe) |
| Event confirmation reward | "+5 when confirmed by others" | +5 to each other user with a live report of the event, once per episode | the confirmer earns nothing for the event itself |
| Check-in "verified" | "是否验证在场" | inside (g ≥ 0.6) at the start or at any heartbeat | presence can start after the check-in |
| Check-in karma | +1 for a verified hour, 4 per day | +1 per whole verified hour, up to 4 points per day | "满 1 小时" read per hour |
| Check-in ends | 2 outside heartbeats or 6 h | also 30 min without a heartbeat (`stale`), ending at the last heartbeat | a closed app would otherwise count as present for 6 h |
| Implicit check-in report | in-place count / capacity, weight 0.3 | verified open check-ins only; skipped without a capacity | unverified check-ins are free to fake |
| Rate-limit key | IP | socket peer address only | `fio_http_from` trusts `Forwarded` / `X-Forwarded-For`, which clients set |
| Test hooks | `X-FSS-Now` | plus `POST /debug/jobs/:name` | jobs run on the real clock; tests and the simulator need them at the test clock |

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

## Open bugs

- **WebSocket: a disconnect drops the channel for every other subscriber.**
  Two connections subscribe to `spot:1` (or `bldg:1`, also through `view`);
  one closes; a report that changes spot 1's color reaches nobody. Without the
  close both receive it. Same result with cstl `a24d0be` and the version
  before it, so it is not caused by the vendor bump. `ws.c` relies on cstl to
  release a closed connection's subscriptions (`fss_ws_on_close`), so the
  cause is probably in that release path. No API test covers two subscribers
  with one leaving. The web app refetches every 60 s as a fallback.
- **WebSocket close frames echo the reserved code 1005** when the client's
  close frame has no status code (cstl's parser stores 1005 for an empty
  payload, `fio-stl.h` near line 24134, and the echo sends it). Browsers
  reject the frame. The web client closes with 1000.

## Next steps

1. Web: check-in button with background heartbeat, karma toasts from
   `user:{id}`, karma history, photos, spot confirmation (placeholders in
   `web/src/lib/features.ts`).
2. M5: `hourly_rollup` also aggregates `occupancy_hourly`; `forecast_slot`
   replaces the `crowd_typical` prior.
3. M1 pipeline can start any time; it only needs the schema.
4. Reports still have no per-user frequency cap besides the building limit,
   the dedupe window and the karma cap.

## Open questions

- Milestone definitions above are inferred; replace them if the roadmap's
  diagram defines M0–M6 differently.
- First campus is assumed to be OSU (roadmap 11.2); until M1 runs, development
  uses the synthetic "demo" campus.
- Event kinds (`outlet_broken`, `wifi_down`, `closed_event`) and the trusted
  reputation threshold (1.5) are assumptions; adjust `EVENT_KINDS` in `live.c`
  and `event_trusted_rep` in `rules.json`.
- The badge set (`0003_community.sql`) and the report agreement threshold
  (`rep_report_agree` = 0.5) are assumptions; the roadmap names only Campus
  Explorer and the rejection rule.
- Reputation grows by +0.05 per agreeing report, so active honest users reach
  the 3.0 cap within days. If that makes a few users dominate the estimate, cap
  the daily gain or decay reputation toward 1.0.
