-- 0002: real-time state (roadmap 7.2, 7.3).
--
-- report.weight keeps the time-independent part of a report's weight
-- (reputation x fence factor); the 20-minute decay is applied when the
-- estimate is computed. The fence factor itself is stored so that event
-- reports can tell a trusted in-fence report (g = 1.0) from the rest.
ALTER TABLE report ADD COLUMN fence REAL NOT NULL DEFAULT 0.2;

-- Teleport check (7.6) and per-user history read reports by user and time;
-- report_user_at from 0001 covers that. Event lookups need the kind.
CREATE INDEX report_spot_event ON report(spot_id, event, at) WHERE event IS NOT NULL;
