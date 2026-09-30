# Vendored dependencies

| File | Upstream | Version | Integrity |
| --- | --- | --- | --- |
| `fio-stl.h` | https://github.com/facil-io/cstl | master @ `24a5701` (2026-08-21, "Defer error path in `fio_io_attach_fd`") | license in `fio-stl.LICENSE` |
| `sqlite3.c`, `sqlite3.h` | https://sqlite.org/2026/sqlite-amalgamation-3530400.zip | SQLite 3.53.4 | SHA3-256 of the zip: `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e` |

Upgrade policy (roadmap 11.1): bump one dependency per commit, record the new
commit / version here, and run `make test` plus `make debug` before merging.

The cstl API is verified against the module docs shipped in that commit
(`fio-stl/*.md`) and, where the docs are silent, against the headers
(`fio-stl/*.h`). Code must not rely on behavior that neither describes.
