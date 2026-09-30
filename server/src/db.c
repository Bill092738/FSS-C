#include "db.h"

#include "fss_fio.h"
#include "rules.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* *****************************************************************************
Connection setup
***************************************************************************** */

/* busy_timeout is set through sqlite3_busy_timeout() before these run:
 * switching a fresh file to WAL takes a lock that concurrently starting
 * worker threads would otherwise fail on with SQLITE_BUSY. */
static const char FSS_DB_PRAGMAS[] = "PRAGMA journal_mode=WAL;"
                                     "PRAGMA synchronous=NORMAL;"
                                     "PRAGMA foreign_keys=ON;"
                                     "PRAGMA temp_store=MEMORY;"
                                     "PRAGMA cache_size=-20000;";

/* SQL wrappers around rules.c so that SQL and C share one implementation. */
static void sql_fss_conf(sqlite3_context *ctx, int argc, sqlite3_value **v) {
  (void)argc;
  sqlite3_result_double(
      ctx, fss_claim_confidence(sqlite3_value_double(v[0]),
                                sqlite3_value_double(v[1]),
                                sqlite3_value_double(v[2]),
                                FSS_RULES.claim_prior_votes));
}

static void sql_fss_color(sqlite3_context *ctx, int argc, sqlite3_value **v) {
  (void)argc;
  if (sqlite3_value_type(v[0]) == SQLITE_NULL) {
    sqlite3_result_null(ctx);
    return;
  }
  sqlite3_result_text(ctx, fss_live_color(sqlite3_value_double(v[0])), -1,
                      SQLITE_STATIC);
}

static int register_functions(sqlite3 *db) {
  int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS;
  int rc = sqlite3_create_function(db, "fss_conf", 3, flags, NULL,
                                   sql_fss_conf, NULL, NULL);
  if (rc == SQLITE_OK)
    rc = sqlite3_create_function(db, "fss_color", 1, flags, NULL,
                                 sql_fss_color, NULL, NULL);
  return rc;
}

sqlite3 *fss_db_open(const char *path) {
  sqlite3 *db = NULL;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path, &db, flags, NULL) != SQLITE_OK) {
    FIO_LOG_ERROR("sqlite open(%s): %s", path, db ? sqlite3_errmsg(db) : "OOM");
    sqlite3_close_v2(db);
    return NULL;
  }
  sqlite3_busy_timeout(db, 5000);
  char *err = NULL;
  if (sqlite3_exec(db, FSS_DB_PRAGMAS, NULL, NULL, &err) != SQLITE_OK) {
    FIO_LOG_ERROR("sqlite pragmas: %s", err ? err : "?");
    sqlite3_free(err);
    sqlite3_close_v2(db);
    return NULL;
  }
  if (register_functions(db) != SQLITE_OK) {
    FIO_LOG_ERROR("sqlite functions: %s", sqlite3_errmsg(db));
    sqlite3_close_v2(db);
    return NULL;
  }
  return db;
}

/* *****************************************************************************
Migrations
***************************************************************************** */

typedef struct {
  int version;
  char name[256];
} fss_migration_s;

static int migration_cmp(const void *a, const void *b) {
  return ((const fss_migration_s *)a)->version -
         ((const fss_migration_s *)b)->version;
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  char *buf = NULL;
  if (fseek(f, 0, SEEK_END) == 0) {
    long len = ftell(f);
    if (len >= 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc(len + 1))) {
      if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        buf = NULL;
      } else {
        buf[len] = 0;
      }
    }
  }
  fclose(f);
  return buf;
}

static int user_version(sqlite3 *db) {
  sqlite3_stmt *st = NULL;
  int v = -1;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, NULL) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW)
    v = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return v;
}

int fss_db_migrate(const char *db_path, const char *dir) {
  sqlite3 *db = fss_db_open(db_path);
  if (!db)
    return -1;
  int current = user_version(db);
  fss_migration_s list[256];
  size_t count = 0;
  DIR *d = opendir(dir);
  if (!d) {
    FIO_LOG_ERROR("migrations folder missing: %s", dir);
    sqlite3_close_v2(db);
    return -1;
  }
  struct dirent *e;
  while ((e = readdir(d)) && count < 256) {
    size_t n = strlen(e->d_name);
    if (n < 9 || n >= sizeof(list[0].name) || strcmp(e->d_name + n - 4, ".sql"))
      continue;
    int v = 0, digits = 0;
    while (digits < 4 && e->d_name[digits] >= '0' && e->d_name[digits] <= '9')
      v = v * 10 + (e->d_name[digits++] - '0');
    if (digits != 4 || e->d_name[4] != '_')
      continue;
    list[count].version = v;
    memcpy(list[count].name, e->d_name, n + 1);
    ++count;
  }
  closedir(d);
  qsort(list, count, sizeof(list[0]), migration_cmp);

  for (size_t i = 0; i < count && current >= 0; ++i) {
    if (list[i].version <= current)
      continue;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, list[i].name);
    char *sql = read_file(path);
    if (!sql) {
      FIO_LOG_ERROR("cannot read migration %s", path);
      current = -1;
      break;
    }
    char set_version[64];
    snprintf(set_version, sizeof(set_version), "PRAGMA user_version=%d;",
             list[i].version);
    char *err = NULL;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err);
    if (rc == SQLITE_OK)
      rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc == SQLITE_OK)
      rc = sqlite3_exec(db, set_version, NULL, NULL, &err);
    if (rc == SQLITE_OK)
      rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &err);
    free(sql);
    if (rc != SQLITE_OK) {
      FIO_LOG_ERROR("migration %s failed: %s", list[i].name, err ? err : "?");
      sqlite3_free(err);
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
      current = -1;
      break;
    }
    FIO_LOG_INFO("applied migration %s", list[i].name);
    current = list[i].version;
  }
  sqlite3_close_v2(db);
  return current;
}

/* *****************************************************************************
Per-thread connection and statement cache
***************************************************************************** */

#define FSS_STMT_CACHE_SIZE 256 /* power of 2 */

typedef struct {
  const char *sql;
  sqlite3_stmt *st;
} fss_stmt_slot_s;

static __thread sqlite3 *tl_db;
static __thread fss_stmt_slot_s tl_stmts[FSS_STMT_CACHE_SIZE];

void fss_db_thread_open(void *db_path) {
  if (tl_db)
    return;
  tl_db = fss_db_open((const char *)db_path);
  if (!tl_db && fio_io_is_running()) {
    FIO_LOG_FATAL("worker thread could not open the database, stopping");
    fio_io_stop();
  }
}

void fss_db_thread_close(void *ignored) {
  (void)ignored;
  for (size_t i = 0; i < FSS_STMT_CACHE_SIZE; ++i) {
    if (tl_stmts[i].st)
      sqlite3_finalize(tl_stmts[i].st);
    tl_stmts[i] = (fss_stmt_slot_s){0};
  }
  sqlite3_close_v2(tl_db);
  tl_db = NULL;
}

sqlite3 *fss_db(void) { return tl_db; }

sqlite3_stmt *fss_stmt(const char *sql) {
  if (!tl_db)
    return NULL;
  size_t h = ((uintptr_t)sql >> 3) * 0x9E3779B97F4A7C15ULL;
  for (size_t n = 0; n < FSS_STMT_CACHE_SIZE; ++n) {
    fss_stmt_slot_s *slot = &tl_stmts[(h + n) & (FSS_STMT_CACHE_SIZE - 1)];
    if (slot->sql == sql)
      return slot->st;
    if (slot->sql)
      continue;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v3(tl_db, sql, -1, SQLITE_PREPARE_PERSISTENT, &st,
                           NULL) != SQLITE_OK) {
      FIO_LOG_ERROR("prepare failed: %s\n  SQL: %s", sqlite3_errmsg(tl_db), sql);
      return NULL;
    }
    slot->sql = sql;
    slot->st = st;
    return st;
  }
  FIO_LOG_ERROR("statement cache full");
  return NULL;
}

void fss_stmt_release(sqlite3_stmt *st) {
  if (!st)
    return;
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
}

/* *****************************************************************************
Transactions
***************************************************************************** */

static const char SQL_BEGIN[] = "BEGIN IMMEDIATE";
static const char SQL_COMMIT[] = "COMMIT";
static const char SQL_ROLLBACK[] = "ROLLBACK";

static int exec_cached(const char *sql) {
  sqlite3_stmt *st = fss_stmt(sql);
  if (!st)
    return SQLITE_ERROR;
  int rc = sqlite3_step(st);
  fss_stmt_release(st);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int fss_tx_begin(void) { return exec_cached(SQL_BEGIN); }
int fss_tx_commit(void) { return exec_cached(SQL_COMMIT); }
void fss_tx_rollback(void) {
  if (tl_db && !sqlite3_get_autocommit(tl_db))
    exec_cached(SQL_ROLLBACK);
}

/* *****************************************************************************
JSON helpers
***************************************************************************** */

int fss_json_rows(char **dest, sqlite3_stmt *st, int col) {
  char *out = fio_bstr_write(*dest, "[", 1);
  int rc, first = 1;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const unsigned char *txt = sqlite3_column_text(st, col);
    int len = sqlite3_column_bytes(st, col);
    if (!first)
      out = fio_bstr_write(out, ",", 1);
    out = txt ? fio_bstr_write(out, txt, (size_t)len)
              : fio_bstr_write(out, "null", 4);
    first = 0;
  }
  *dest = fio_bstr_write(out, "]", 1);
  return rc;
}
