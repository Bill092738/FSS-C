/* SQLite access layer.
 *
 * Thread model (roadmap 4.2 / 5.1): SQLite is built with SQLITE_THREADSAFE=2,
 * so each connection belongs to exactly one thread. Every worker thread of the
 * HTTP / job queues opens its own connection through the
 * FIO_CALL_ON_WORKER_THREAD_START state callback and closes it on
 * FIO_CALL_ON_WORKER_THREAD_END. Prepared statements are cached per thread,
 * keyed by the address of the SQL string literal.
 */
#ifndef FSS_DB_H
#define FSS_DB_H

#include "sqlite3.h"

#include <stdint.h>

/* Opens a connection and applies the standard PRAGMAs. NULL on failure. */
sqlite3 *fss_db_open(const char *path);

/* Applies migrations/NNNN_*.sql with NNNN > PRAGMA user_version, in order,
 * each inside its own transaction. Returns the resulting version or -1. */
int fss_db_migrate(const char *db_path, const char *migrations_dir);

/* State callbacks: `arg` is the database path (must outlive the process). */
void fss_db_thread_open(void *db_path);
void fss_db_thread_close(void *ignored);

/* The calling thread's connection (NULL if the thread has none). */
sqlite3 *fss_db(void);

/* Returns a cached, reset statement for `sql` (a string with static storage).
 * NULL on prepare failure (logged). Call fss_stmt_release when done. */
sqlite3_stmt *fss_stmt(const char *sql);
void fss_stmt_release(sqlite3_stmt *st);

/* Write transactions always use BEGIN IMMEDIATE (roadmap 5.1). */
int fss_tx_begin(void);
int fss_tx_commit(void);
void fss_tx_rollback(void);

/* Collects column `col` of every row as pre-rendered JSON text and appends a
 * JSON array ("[a,b,...]") to the fio_bstr `*dest` (NULL allowed). Row order
 * is preserved. Returns SQLITE_DONE on success, otherwise the SQLite error. */
int fss_json_rows(char **dest, sqlite3_stmt *st, int col);

#endif /* FSS_DB_H */
