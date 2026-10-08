/* Karma, reputation and badges (roadmap 7.5, 7.6).
 *
 * Every function runs inside the caller's write transaction on the calling
 * thread's connection and queues personal notifications for user:{id} in the
 * caller's outbox, which fss_live_commit() publishes after the commit.
 *
 * Both ledgers are idempotent: (user, reason, ref_type, ref_id) is UNIQUE, so
 * a retried or repeated trigger credits a contribution once.
 */
#ifndef FSS_COMMUNITY_H
#define FSS_COMMUNITY_H

#include "live.h"

#include <stdint.h>

/* Credits `delta` karma (roadmap 7.5). `daily` caps the points credited for
 * `reason` within the last 24 h (0 = no cap); a capped award is cut down or
 * skipped. Queues {"t":"karma"} and checks badges. Returns the points
 * credited (0 when capped or already credited) or -1 on error. */
int fss_karma_award(int64_t uid, int delta, int64_t daily, const char *reason,
                    const char *ref_type, int64_t ref_id, int64_t now,
                    fss_outbox_s *box);

/* Takes back what an earlier award for (award_reason, ref) credited, as a
 * negative entry under `reason`. Returns the points removed, 0 when nothing
 * was credited or it was already taken back, -1 on error. */
int fss_karma_revoke(int64_t uid, const char *award_reason, const char *reason,
                     const char *ref_type, int64_t ref_id, int64_t now,
                     fss_outbox_s *box);

/* Adds `delta` to the user's reputation, clamped to [rep_min, rep_max]
 * (roadmap 7.6), once per (reason, ref). Returns 1 when applied, 0 when
 * already applied, -1 on error. */
int fss_rep_adjust(int64_t uid, double delta, const char *reason,
                   const char *ref_type, int64_t ref_id, int64_t now,
                   fss_outbox_s *box);

/* Awards every badge whose rule the user now satisfies and queues
 * {"t":"badge"}. Returns the number awarded or -1. */
int fss_badges_check(int64_t uid, int64_t now, fss_outbox_s *box);

/* Vote weight (roadmap 7.6): reputation, x vote_checkin_boost when the voter
 * has a verified check-in in `building`. Negative on error. */
double fss_voter_weight(int64_t uid, int64_t building);

/* hourly_rollup job, reputation part (roadmap 7.6): judges the crowding
 * reports that have a complete consensus window since the last run against
 * other users' reports, and adjusts their authors' reputation. Runs its own
 * transaction. Returns the number of reports judged or -1. */
int fss_reputation_rollup(int64_t now);

#endif /* FSS_COMMUNITY_H */
