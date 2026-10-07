# FSS monorepo entry points (roadmap 10.3). Run from the repository root.
PY ?= .venv/bin/python

.PHONY: all server debug unit api test run seed clean web web-test dev
all: server

server:
	@$(MAKE) --no-print-directory -C server

debug:
	@$(MAKE) --no-print-directory -C server debug

unit:
	@$(MAKE) --no-print-directory -C server unit

# Black-box API tests; set FSS_BIN=server/build/fss-debug to run them on ASan.
api: server
	$(PY) -m pytest -q tests/api

test: unit api

run: server
	server/build/fss

# Creates data/fss.db from the synthetic demo fixture (refuses to overwrite).
seed: server
	@test ! -e data/fss.db || { echo "data/fss.db exists; remove it first"; exit 1; }
	server/build/fss --db data/fss.db --migrate-only
	server/build/fss --db data/fss.db --seed data/seed/demo.sql --materialize

clean:
	@$(MAKE) --no-print-directory -C server clean

# Front-end (web/): Vite + React + TypeScript + Tailwind. Needs Node 20+.
web/node_modules: web/package-lock.json
	cd web && npm ci
	@touch $@

# Builds web/dist, which `make run` serves (with SPA fallback) on :8080.
web: web/node_modules
	cd web && npm run build

web-test: web/node_modules
	cd web && npm run typecheck && npm run lint && npm test

# Backend on :8080 and the Vite dev server on :5173, which proxies /api and
# /ws to it (roadmap 10.3). Open http://localhost:5173. Ctrl-C stops both.
dev: server web/node_modules
	@trap 'kill 0' INT TERM EXIT; server/build/fss & cd web && npm run dev
