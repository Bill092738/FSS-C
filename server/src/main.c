/* FSS server entry point.
 *
 * One process, one reactor thread, one HTTP task queue whose worker threads
 * each own a SQLite connection (roadmap 4.2). Migrations run on the main
 * thread before the reactor starts (roadmap 5.1).
 */
#include "api.h"
#include "attrs.h"
#include "auth.h"
#include "db.h"
#include "rules_load.h"
#include "spots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static fio_io_async_s HTTP_Q = FIO_IO_ASYN_INIT;

static struct {
  const char *bind;
  const char *db;
  const char *migrations;
  const char *public_dir;
  const char *rules;
  const char *seed;
  int threads;
  int log;
  int migrate_only;
  int materialize;
  int bench_password;
} CFG = {
    .bind = "0.0.0.0:8080",
    .db = "data/fss.db",
    .migrations = "server/migrations",
    .public_dir = "web/dist",
    .rules = "server/config/rules.json",
    .threads = 4,
    .log = 1,
};

static fio_str_info_s PUBLIC_DIR; /* empty when the folder does not exist */

static void usage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [options]\n"
          "  --bind ADDR          listen address (default %s)\n"
          "  --db PATH            SQLite database file (default %s)\n"
          "  --migrations DIR     migrations folder (default %s)\n"
          "  --public DIR         static front-end folder (default %s)\n"
          "  --rules FILE         rule overrides (default %s)\n"
          "  --threads N          HTTP worker threads (default %d)\n"
          "  --quiet              disable request logging\n"
          "  --migrate-only       apply migrations and exit\n"
          "  --seed FILE          execute an SQL file in one transaction and exit\n"
          "  --materialize        recompute every spot from its claims and exit\n"
          "  --bench-password     time one Argon2id hash with the current parameters\n",
          prog, CFG.bind, CFG.db, CFG.migrations, CFG.public_dir, CFG.rules,
          CFG.threads);
}

static int parse_args(int argc, char const *argv[]) {
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    const char *next = i + 1 < argc ? argv[i + 1] : NULL;
#define FSS_ARG_STR(name, field)                                               \
  if (!strcmp(a, name)) {                                                      \
    if (!next)                                                                 \
      return -1;                                                               \
    CFG.field = next;                                                          \
    ++i;                                                                       \
    continue;                                                                  \
  }
    FSS_ARG_STR("--bind", bind)
    FSS_ARG_STR("--db", db)
    FSS_ARG_STR("--migrations", migrations)
    FSS_ARG_STR("--public", public_dir)
    FSS_ARG_STR("--rules", rules)
    FSS_ARG_STR("--seed", seed)
#undef FSS_ARG_STR
    if (!strcmp(a, "--threads") && next) {
      CFG.threads = atoi(next);
      ++i;
      if (CFG.threads < 1 || CFG.threads > 64)
        return -1;
      continue;
    }
    if (!strcmp(a, "--quiet")) {
      CFG.log = 0;
      continue;
    }
    if (!strcmp(a, "--migrate-only")) {
      CFG.migrate_only = 1;
      continue;
    }
    if (!strcmp(a, "--bench-password")) {
      CFG.bench_password = 1;
      continue;
    }
    if (!strcmp(a, "--materialize")) {
      CFG.materialize = 1;
      continue;
    }
    return -1;
  }
  return 0;
}

/* Executes an SQL file inside one transaction (dev seeds, fixtures). */
static int run_sql_file(const char *db_path, const char *file) {
  FILE *f = fopen(file, "rb");
  if (!f) {
    FIO_LOG_ERROR("cannot open %s", file);
    return -1;
  }
  char *sql = NULL;
  size_t len = 0;
  char chunk[1 << 14];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)))
    sql = fio_bstr_write(sql, chunk, n);
  fclose(f);
  sqlite3 *db = fss_db_open(db_path);
  int rc = -1;
  char *err = NULL;
  (void)len;
  if (db && sql &&
      sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &err) == SQLITE_OK &&
      sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK &&
      sqlite3_exec(db, "COMMIT;", NULL, NULL, &err) == SQLITE_OK)
    rc = 0;
  if (rc) {
    FIO_LOG_ERROR("%s: %s", file, err ? err : "failed");
    if (db)
      sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
  }
  sqlite3_free(err);
  sqlite3_close_v2(db);
  fio_bstr_free(sql);
  return rc;
}

static int is_dir(const char *path) {
  struct stat s;
  return path && !stat(path, &s) && S_ISDIR(s.st_mode);
}

/* Root handler: static front-end files, SPA fallback, JSON 404 otherwise.
 * Static files are served explicitly (instead of the listener's
 * `public_folder`) because routes inherit `public_folder`, which would make
 * the API prefix probe the file system first. */
static void on_root(fio_http_s *h) {
  fio_str_info_s method = fio_http_method(h);
  int is_get = (method.len == 3 && !memcmp(method.buf, "GET", 3)) ||
               (method.len == 4 && !memcmp(method.buf, "HEAD", 4));
  fio_str_info_s path = fio_http_path(h);
  if (is_get && PUBLIC_DIR.len) {
    if (!fio_http_static_file_response(h, PUBLIC_DIR, path, 0))
      return;
    /* SPA fallback: extension-less paths render the app shell */
    const char *slash = path.buf ? memrchr(path.buf, '/', path.len) : NULL;
    int has_ext = slash && memchr(slash, '.', path.len - (slash - path.buf));
    if (!has_ext &&
        !fio_http_static_file_response(h, PUBLIC_DIR,
                                       FIO_STR_INFO1("/index.html"), 0))
      return;
  }
  fss_send_error(h, 404, "not_found", "not found");
}

/* Prints the cost of one password hash (roadmap 10.5 targets ~100 ms). */
static int bench_password(void) {
  uint8_t salt[FSS_PW_SALT_LEN], hash[FSS_PW_HASH_LEN];
  char params[64];
  fss_pw_params_str(FSS_PW_PARAMS, params, sizeof(params));
  int64_t best = INT64_MAX;
  for (int i = 0; i < 5; ++i) {
    int64_t t = fss_now_ms();
    if (fss_pw_hash("benchmark-password", 18, salt, hash, FSS_PW_PARAMS))
      return 1;
    t = fss_now_ms() - t;
    if (t < best)
      best = t;
  }
  printf("%s: %lld ms per hash (best of 5)\n", params, (long long)best);
  return 0;
}

int main(int argc, char const *argv[]) {
  if (parse_args(argc, argv)) {
    usage(argv[0]);
    return 2;
  }
  if (CFG.bench_password)
    return bench_password();
  int version = fss_db_migrate(CFG.db, CFG.migrations);
  if (version < 0)
    return 1;
  FIO_LOG_INFO("database %s at schema version %d", CFG.db, version);
  if (CFG.migrate_only)
    return 0;
  if (fss_rules_load(CFG.rules))
    return 1;
  if (fss_attrs_load(CFG.db) <= 0) {
    FIO_LOG_FATAL("attribute registry (attr_def) could not be loaded");
    return 1;
  }
  if (CFG.seed || CFG.materialize) {
    if (CFG.seed && run_sql_file(CFG.db, CFG.seed))
      return 1;
    if (CFG.materialize) {
      int n = fss_spots_materialize_all(CFG.db);
      if (n < 0)
        return 1;
      FIO_LOG_INFO("materialized %d spots", n);
    }
    return 0;
  }

  if (is_dir(CFG.public_dir))
    PUBLIC_DIR = FIO_STR_INFO1((char *)CFG.public_dir);
  else
    FIO_LOG_INFO("no front-end folder at %s (static files disabled)",
                 CFG.public_dir);

  fio_state_callback_add(FIO_CALL_ON_WORKER_THREAD_START, fss_db_thread_open,
                         (void *)CFG.db);
  fio_state_callback_add(FIO_CALL_ON_WORKER_THREAD_END, fss_db_thread_close,
                         NULL);
  fio_io_async_attach(&HTTP_Q, (uint32_t)CFG.threads);

  fio_http_listener_s *l = fio_http_listen(CFG.bind, .on_http = on_root,
                                           .queue = &HTTP_Q,
                                           .max_body_size = 8 << 20,
                                           .ws_max_msg_size = 16 << 10,
                                           .log = (uint8_t)CFG.log);
  if (!l) {
    FIO_LOG_FATAL("could not listen on %s", CFG.bind);
    return 1;
  }
  fio_http_route(l, "/api/v1", .on_http = fss_api_dispatch);
  FIO_LOG_INFO("FSS listening on %s (%d HTTP threads)", CFG.bind, CFG.threads);
  fio_io_start(0);
  return 0;
}
