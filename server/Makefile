# FSS server build (roadmap 10.3).
#
#   make              release build   -> build/fss
#   make debug        ASan/UBSan build -> build/fss-debug
#   make unit         build and run the C unit tests
#   make clean
#
# The cstl implementation (src/fio_impl.c) and the SQLite amalgamation are
# compiled once per mode; application sources rebuild in well under a second.

CC      ?= cc
# Sanitizer builds use clang: gcc's libasan is often not installed.
SAN_CC  ?= clang
MODE    ?= release
OUT      = build/$(MODE)

ifeq ($(MODE),debug)
  CC     = $(SAN_CC)
  OPT    = -O0 -g -DDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer
  LDOPT  = -fsanitize=address,undefined
  BIN    = build/fss-debug
else
  OPT    = -O2 -g
  LDOPT  =
  BIN    = build/fss
endif

CPPFLAGS = -isystem vendor -Isrc
CFLAGS   = -std=gnu11 $(OPT) -MMD -MP
WARN     = -Wall -Wextra -Wshadow -Wno-unused-parameter
LDLIBS   = -lssl -lcrypto -lpthread -lm

# SQLite compile options (roadmap 5.1)
SQLITE_DEFS = -DSQLITE_THREADSAFE=2 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE \
              -DSQLITE_DQS=0 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
              -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
              -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_MATH_FUNCTIONS

APP_SRC  = $(filter-out src/fio_impl.c,$(wildcard src/*.c))
APP_OBJ  = $(APP_SRC:src/%.c=$(OUT)/%.o)
LIB_OBJ  = $(OUT)/fio_impl.o build/sqlite3.o

# Pure modules (no cstl / SQLite dependency) exercised by the unit tests.
PURE_SRC = src/util.c src/router.c src/rules.c
TEST_SRC = $(wildcard tests/unit/*.c)
TEST_BIN = build/unit-tests

.PHONY: all release debug unit clean
all release:
	@$(MAKE) --no-print-directory MODE=release build/fss
debug:
	@$(MAKE) --no-print-directory MODE=debug build/fss-debug

$(BIN): $(APP_OBJ) $(LIB_OBJ)
	$(CC) $(LDOPT) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: src/%.c | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -c $< -o $@

# cstl emits warnings we do not own; silence them for this unit only.
$(OUT)/fio_impl.o: src/fio_impl.c src/fss_fio.h vendor/fio-stl.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -c $< -o $@

# SQLite is always optimized and never sanitized (it is not our code).
build/sqlite3.o: vendor/sqlite3.c Makefile | build
	$(CC) -std=gnu11 -O2 -g $(SQLITE_DEFS) -w -c $< -o $@

$(TEST_BIN): $(TEST_SRC) $(PURE_SRC) src/*.h tests/unit/*.h | build
	$(SAN_CC) $(CPPFLAGS) -Itests/unit -std=gnu11 -O0 -g $(WARN) \
	  -fsanitize=address,undefined -o $@ $(TEST_SRC) $(PURE_SRC) -lm

unit: $(TEST_BIN)
	./$(TEST_BIN)

$(OUT) build:
	mkdir -p $@

clean:
	rm -rf build

-include $(wildcard $(OUT)/*.d)
