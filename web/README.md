# FSS web

The FSS front-end: React 19, TypeScript, Tailwind CSS 4 and Vite, with
MapLibre GL on OpenStreetMap tiles, TanStack Query for server state and React
Router. It talks only to the C backend in `server/` (REST on `/api/v1`, live
updates on the `/ws` WebSocket).

The roadmap (10.2) names Preact; this app uses React instead, as decided when
the front-end work started.

## Run

From the repository root:

```sh
make seed     # once: data/fss.db from the demo fixture
make dev      # backend on :8080 + Vite on :5173; open http://localhost:5173
make web      # production build into web/dist
make run      # the C binary serves web/dist (SPA fallback) and the API on :8080
make web-test # typecheck, lint, unit tests
```

Inside `web/`: `npm run dev | build | typecheck | lint | test`.

The Vite dev server proxies `/api` and `/ws` to `FSS_BACKEND` (default
`http://localhost:8080`). The proxy keeps the browser's `Host` header
(`changeOrigin: false`) because the backend rejects writes whose `Origin` host
differs from `Host`.

Set `VITE_MAP_STYLE` to a MapLibre style URL to replace the OSM raster tiles.
Heavy real traffic must not use `tile.openstreetmap.org` (OSM tile usage
policy).

## What works and what is a placeholder

| Area | Status |
| --- | --- |
| Map + list, search (FTS), filters generated from `attr_def`, scenario presets, "near me" | Working (`GET /spots`, `GET /campuses/:slug`) |
| Live crowd colors on map, list and detail | Working (`/ws`: `view` for the map viewport, `sub spot:{id}` on the detail page) |
| Spot detail: attributes with confidence, claims with evidence, votes, corrections | Working |
| Crowd and event reports with browser geolocation | Working (`POST /spots/:id/reports`) |
| Register, log in/out, profile, karma, reputation, badges | Working |
| Suggest a new spot | Working; buildings come from existing spots (no buildings endpoint yet) |
| Check-in, photos, karma history | Placeholder (M4) |
| Forecast curve, Study With Me, heatmap | Placeholder (M5) |
| Coupons, merchant redeem, admin insights | Placeholder (M6) |

Every placeholder reads its title, milestone and the endpoints it waits for
from `src/lib/features.ts`. When the backend ships one, add the call to
`src/lib/api.ts` and replace the `<ComingSoon>` where it is used.

## Layout

```
src/
  main.tsx              providers: React Query, LiveProvider, router
  app/                  router (pages of roadmap 10.2) and the app shell
  pages/                one file per page; PlaceholderPages.tsx for M4–M6
  components/           map, filters, report panel, claims, UI primitives
  lib/
    api.ts              typed REST client and the error envelope (ApiError)
    types.ts            response shapes, kept in sync with server/src/api_*.c
    live.ts             WebSocket client: ref-counted channels, reconnect, `since` replay
    liveContext.ts      React hooks; applies pushes to the query cache
    queries.ts          React Query hooks and the selected campus
    features.ts         registry of not-yet-built features
    filters.ts, attrs.ts, geo.ts, auth.ts
```

## Backend issues found while building this

- **A disconnect drops the channel for every other subscriber.** When two
  WebSocket connections subscribe to the same channel (`spot:*`, `bldg:*`,
  through `sub` or `view`) and one of them closes, the other stops receiving
  messages on that channel. Seen with cstl `a24d0be` and with the version
  before it. Reproduce: two clients `sub` `spot:1`, close one, change spot
  1's color with a report; the remaining client gets nothing (it does when
  neither closes). In the app this shows up when a user leaves the map
  while another user is watching the same buildings. The front-end refetches
  lists and details every 60 s as a fallback.
- **Close frames echo the reserved code 1005.** A client close frame without
  a status code (what a browser's `ws.close()` sends) is answered with code
  1005, which RFC 6455 forbids on the wire; Chrome reports "broken close
  frame". The client closes with an explicit 1000 to avoid it.
