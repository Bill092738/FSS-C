/* Spot domain logic shared by the API, the seed loader and the pipeline hand-off. */
#ifndef FSS_SPOTS_H
#define FSS_SPOTS_H

#include <stdint.h>

/* Recomputes a spot's materialized columns (features, noise, outlets, temp,
 * capacity, vibe, attrs_json, quality) and its FTS row from its claims
 * (roadmap rule 1 and 7.6). Must run inside a write transaction on the calling
 * thread's connection. Returns SQLITE_OK or an SQLite error code. */
int fss_spot_materialize(int64_t spot_id);

/* Materializes every spot using a private connection (CLI / seed use).
 * Returns the number of spots processed or -1. */
int fss_spots_materialize_all(const char *db_path);

#endif /* FSS_SPOTS_H */
