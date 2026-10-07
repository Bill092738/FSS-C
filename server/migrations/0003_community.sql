-- 0003: community (roadmap 7.4 check-ins, 7.5 karma and badges, 7.6
-- reputation and moderation, 10.5 uploads).

-- Check-ins: whether the last position (start or heartbeat) was inside the
-- fence, so a heartbeat only credits time spent inside; and why it ended.
ALTER TABLE checkin ADD COLUMN last_in INTEGER NOT NULL DEFAULT 0;
ALTER TABLE checkin ADD COLUMN end_reason TEXT
  CHECK (end_reason IS NULL OR end_reason IN ('user', 'outside', 'timeout', 'stale'));
CREATE INDEX checkin_open_spot ON checkin(spot_id) WHERE end_at IS NULL;
CREATE INDEX checkin_user ON checkin(user_id, start_at);

-- Reputation changes, idempotent like karma_ledger (roadmap 7.6). `delta` is
-- the requested change; user.reputation is clamped to [rep_min, rep_max].
CREATE TABLE rep_ledger (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES user(id),
  delta REAL NOT NULL,
  reason TEXT NOT NULL,
  ref_type TEXT NOT NULL,
  ref_id INTEGER NOT NULL,
  at INTEGER NOT NULL,
  UNIQUE (user_id, reason, ref_type, ref_id)
);

-- Daily caps sum a user's recent awards per reason.
CREATE INDEX karma_ledger_user_reason ON karma_ledger(user_id, reason, at);

-- Badge counters and daily limits read contributions by author.
CREATE INDEX claim_user ON claim(user_id, created_at) WHERE user_id IS NOT NULL;
CREATE INDEX spot_created_by ON spot(created_by, created_at) WHERE created_by IS NOT NULL;
CREATE INDEX photo_spot ON photo(spot_id, at) WHERE status = 'visible';
CREATE INDEX photo_user ON photo(user_id, at);

CREATE TABLE photo_vote (
  photo_id INTEGER NOT NULL REFERENCES photo(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES user(id),
  v INTEGER NOT NULL CHECK (v IN (-1, 1)),
  weight REAL NOT NULL,
  at INTEGER NOT NULL,
  PRIMARY KEY (photo_id, user_id)
) WITHOUT ROWID;

-- Spot discovery (7.5): a submitted spot becomes active once
-- `spot_confirm_min` other users confirmed it.
CREATE TABLE spot_confirm (
  spot_id INTEGER NOT NULL REFERENCES spot(id),
  user_id INTEGER NOT NULL REFERENCES user(id),
  at INTEGER NOT NULL,
  PRIMARY KEY (spot_id, user_id)
) WITHOUT ROWID;

-- Progress of batch jobs that walk history (hourly_rollup).
CREATE TABLE job_cursor (name TEXT PRIMARY KEY, at INTEGER NOT NULL) WITHOUT ROWID;

-- Badges (7.5). rule_json: {"count": <counter>, "gte": <threshold>}; the
-- counters are defined in server/src/community.c.
INSERT INTO badge (key, name, rule_json) VALUES
  ('first_report',    'First Report',    '{"count":"reports","gte":1}'),
  ('crowd_watcher',   'Crowd Watcher',   '{"count":"reports","gte":50}'),
  ('pathfinder',      'Pathfinder',      '{"count":"spots_discovered","gte":1}'),
  ('campus_explorer', 'Campus Explorer', '{"count":"spots_discovered","gte":5}'),
  ('fact_checker',    'Fact Checker',    '{"count":"claims_accepted","gte":5}'),
  ('photographer',    'Photographer',    '{"count":"photos","gte":5}'),
  ('sentinel',        'Sentinel',        '{"count":"events_confirmed","gte":3}'),
  ('deep_focus',      'Deep Focus',      '{"count":"checkin_hours","gte":10}'),
  ('trusted_voice',   'Trusted Voice',   '{"count":"reputation","gte":2.0}'),
  ('centurion',       'Centurion',       '{"count":"karma","gte":100}');
