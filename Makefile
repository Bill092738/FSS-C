# FSS monorepo entry points (roadmap 10.3). Run from the repository root.
PY ?= .venv/bin/python

.PHONY: all server debug unit api test run seed clean
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
