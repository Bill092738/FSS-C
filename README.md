# FSS

FSS helps students find a place to study on campus. Spots (libraries, lounges,
cafés, ...) have searchable attributes such as outlets, noise level and vibe.
The attributes come from user claims and votes. Students on site report how
crowded a spot is, and subscribed clients get the new state over a WebSocket.
Check-ins, karma, badges and a reputation score reward useful contributions.

The backend is a single C executable built on
[facil.io cstl](https://github.com/facil-io/cstl) with one SQLite database file.

The web front-end in `web/` uses React, TypeScript, Tailwind CSS and Vite.

> **Status: work in progress.** Part of the backend works today: the
> foundation (M0), the core API (M2), real-time occupancy (M3) and the
> community features (M4). The web front-end covers M0–M4 and shows
> placeholders for the rest. The data pipeline, forecasts and everything after
> are not built yet. See [PROGRESS.md](PROGRESS.md) for details.

## What works

- `GET /api/v1/health`, campus listing and campus config
- Accounts: register, login, logout, profile (`/auth/*`, `/me`)
- Spot search with filters (bounding box, attributes, quiet level, distance,
  full-text), spot details, user-submitted spots
- Claims and weighted votes on spot attributes
- Crowding reports (green / yellow / red) and event reports (broken outlets,
  Wi-Fi down, closed), weighted by a geofence check of the reporter's position
- A live crowding estimate per spot that decays over time and falls back to a
  default forecast
- WebSocket push on `/ws`: subscribe to a spot, a building or a map viewport;
  reconnecting clients can replay missed messages with `since`
- Check-ins with heartbeats and verified study time; open check-ins feed the
  crowding estimate
- Karma (ledger with daily caps), badges, reputation that rises and falls with
  confirmed or rejected contributions, pushed to the user over the WebSocket
- Photos (upload, votes, automatic hiding) and confirmation of user-submitted
  spots
- A per-IP limit on `/auth/*`
- A synthetic "Demo University" seed (not real data) and a simulator with
  cheating users (`tests/sim/simulate.py`)
- The web app ([web/README.md](web/README.md)): map and list with live crowd
  colors, search and filters, spot details with claims and votes, crowd and
  event reports with geolocation, check-ins, photos, spot confirmation,
  accounts, profile and karma history; placeholders for the features below

## What does not work yet

- Forecasts from history, heatmaps, Study With Me (M5); until then the
  estimate falls back to the spot's typical crowd level
- Offers, merchant/admin tools (M6)
- The data pipeline that loads real campus data (M1); `pipeline/` is empty

## Requirements

- Linux, `make`, gcc (clang for the sanitizer build)
- OpenSSL development libraries (`-lssl -lcrypto`)
- Python 3 for the API tests
- Node.js 20+ for the web front-end

cstl and SQLite are vendored in `server/vendor/`.

## Quick start

```sh
python3 -m venv .venv && .venv/bin/pip install pytest websockets   # once
make            # build server/build/fss (first build takes about 1.5 min)
make seed       # create data/fss.db from the demo fixture
make run        # serve on http://0.0.0.0:8080
```

For the web app:

```sh
make dev        # backend on :8080 and Vite on :5173; open http://localhost:5173
make web        # or build web/dist, which `make run` then serves on :8080
```

Or try the API directly:

```sh
curl http://localhost:8080/api/v1/health
curl "http://localhost:8080/api/v1/spots?campus=demo"
```

Live updates: connect a WebSocket client to `ws://localhost:8080/ws` and send
`{"op":"sub","ch":"spot:1"}`. A logged-in user's
`POST /api/v1/spots/1/reports` with `{"level":2,"lat":40.0,"lon":-83.015,"accuracy_m":10}`
then pushes `{"t":"live","spot":1,"color":"red",...}` to the subscriber.

## Tests

```sh
make test       # C unit tests + pytest API tests
make debug      # ASan/UBSan build at server/build/fss-debug
FSS_BIN=server/build/fss-debug make api
make debug test SAN_CC=gcc   # if clang's sanitizer runtime is not installed
make web-test   # front-end typecheck, lint and unit tests
```

## Layout

```
server/        C backend (src/, migrations/, config/rules.json, vendor/)
tests/api/     pytest black-box API tests
data/seed/     demo seed data
pipeline/      data pipeline (not started)
web/           front-end: React + TypeScript + Tailwind (Vite)
```

## Docs

- [PROGRESS.md](PROGRESS.md): current status, decisions, next steps
- [FSS Roadmap.md](FSS%20Roadmap.md): full technical plan (in Chinese)
