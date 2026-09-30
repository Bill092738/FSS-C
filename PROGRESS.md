# FSS Implementation Progress

Tracks implementation of `FSS Roadmap.md` (the roadmap). All other project
documents, code, and comments are written in English.

Last updated: 2026-09-29

## Milestones

The roadmap's milestone diagram (section 11) is not embedded in the Markdown
file, so the milestones below are derived from its sections. M1 and M2 can run
in parallel: M2 starts on a hand-made seed (roadmap 11).

| # | Milestone | Roadmap sections | Gate | Status |
| --- | --- | --- | --- | --- |
| M0 | Foundation: vendoring, build, DB layer, migrations, router, error envelope, test harnesses | 4.1–4.4, 5.1, 10.1, 10.3 | `make test` green; `/api/v1/health` answers | **Done** |
| M1 | Phase 0 pipeline (collect, buildings, extract, resolve, load) for OSU | 6 | 100+ active spots loaded; 100-claim audit ≥ 90% | Not started |
| M2 | Core API: auth, campuses, spot search/detail, claims + votes + materialization, dev seed | 5.3, 7.1, 7.6 (claims), 9.1, 10.5 | pytest covers search filters, auth, claim materialization | **Done** (IP rate limit on `/auth/*` moved to M4) |
| M3 | Real time: reports, geofence, occupancy estimate, WebSocket + Pub/Sub, timers | 4.5, 4.6, 7.2, 7.3, 9.2 | two-client WebSocket test sees color change; replay with `since` | Next |
| M4 | Community: check-ins, karma ledger, badges, reputation, rate limits (incl. `/auth/*` per IP), photos, spot discovery confirmation | 7.4–7.6, 10.5 | idempotency + rate-limit tests; simulator shows cheaters suppressed | Not started |
| M5 | Forecast and Phase 2: hourly rollup, forecast, Study With Me, heatmap (k-anonymity) | 7.7, 8.1, 8.2 | forecast endpoint on simulated history; k=5 filter tests | Not started |
| M6 | Monetization, front-end, load test: offers/coupons, merchant redeem, admin insights + CSV, web app, k6 | 8.3, 8.4, 10.2, 10.4 | end-to-end demo; k6 report written back into docs | Not started |

## How to run

```
python3 -m venv .venv && .venv/bin/pip install pytest websockets   # once
make              # server/build/fss (first build ~1.5 min: cstl + SQLite)
make debug        # server/build/fss-debug with ASan/UBSan (clang)
make test         # C unit tests + pytest API tests
make seed         # data/fss.db from the synthetic demo fixture
make run          # serves on 0.0.0.0:8080 using data/fss.db
FSS_BIN=server/build/fss-debug make api   # API tests against the ASan build
server/build/fss --help                   # all CLI options
```

Test status (2026-09-29): 70 unit checks and 31 API tests pass on the release
build and on the ASan/UBSan build; server logs of ASan runs contain no leak or
sanitizer reports.

## Implemented

### Repository layout (roadmap 10.1)

```
Makefile                     make / debug / unit / api / test / run / seed
data/seed/demo.sql           synthetic "Demo University" fixture (NOT real data)
server/
  Makefile                   release (gcc -O2) and debug (clang ASan+UBSan)
  config/rules.json          tunables (roadmap 7), unknown keys rejected
  vendor/                    fio-stl.h (cstl 24a5701), SQLite 3.53.4, VERSIONS.md
  migrations/0001_core.sql   core schema (5.2) + attr_def v1 (6.4)
  src/fss_fio.h, fio_impl.c  cstl include / single implementation unit
  src/main.c                 CLI, migrations, worker DB connections, listener, static/SPA
  src/db.c                   PRAGMAs, migrations, per-thread stmt cache, tx, SQL functions
  src/http.c                 JSON responses, error envelope, query/body helpers
  src/api.c                  /api/v1 route table and dispatcher
  src/api_meta.c             /health, /campuses, /campuses/:slug
  src/api_auth.c             /auth/register|login|logout, GET|PATCH /me
  src/api_spots.c            GET|POST /spots, GET /spots/:id, POST /spots/:id/claims, POST /claims/:id/vote
  src/auth.c                 Argon2id, session tokens, cookie, Origin check
  src/attrs.c                in-memory attr_def registry + value validation
  src/spots.c                claim -> spot materialization + FTS row
  src/rules.c                pure formulas and tunables; rules_load.c reads rules.json
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

Every write endpoint requires `Content-Type: application/json` and rejects a
mismatching `Origin` (roadmap 10.5).

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

Known limitation: FTS uses `unicode61` as specified, which does not segment
Chinese text; a query token must equal a whole token (e.g. the tag "白板"
matches, a substring of a longer Chinese phrase does not). A `trigram`
tokenizer would fix this if needed.

## cstl API notes (verified against docs/headers of 24a5701)

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
- **FIO_EXTERN gap (affects M3):** `fio_http_udata`, `fio_http_udata2`,
  `fio_http_cdata`, `fio_http_controller` (and their `_set`) are `FIO_IFUNC`
  declarations whose bodies are only emitted in the implementation unit
  (`432 http types.h` is guarded by `FIO_EXTERN_COMPLETE`). Application units
  therefore cannot call them. Plan: export thin non-static wrappers from
  `src/fio_impl.c` (e.g. `fss_http_udata2_set`) that call the documented
  functions.

## Bugs found and fixed

- Worker threads raced on `PRAGMA journal_mode=WAL` before `busy_timeout`
  was active; one worker could end up without a connection (503s). Fixed by
  calling `sqlite3_busy_timeout` before any PRAGMA; a worker that still cannot
  open the database now stops the server instead of serving 503s.

## Next steps

1. M3: `POST /spots/:id/reports`, geofence (7.2) and estimator (7.3) as pure
   functions with unit tests, `occupancy_live` updates, `/ws` with
   authentication, `sub`/`unsub`/`view`, Pub/Sub history replay, timers
   (`live_decay`, `wal_checkpoint`) on a 1-thread job queue, debug-only
   `X-FSS-Now` clock injection.
2. M1 pipeline can start any time; it only needs `0001_core.sql`.

## Open questions

- Milestone definitions above are inferred; replace them if the roadmap's
  diagram defines M0–M6 differently.
- First campus is assumed to be OSU (roadmap 11.2); until M1 runs, development
  uses the synthetic "demo" campus.
