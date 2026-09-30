-- 0001: core schema (roadmap section 5.2) and the v1 attribute registry
-- (roadmap section 6.4). Timestamps are milliseconds since the Unix epoch
-- unless a column name says otherwise.

-- Campus and buildings ------------------------------------------------------
CREATE TABLE campus (
  id INTEGER PRIMARY KEY,
  slug TEXT UNIQUE NOT NULL,
  name TEXT NOT NULL,
  tz TEXT NOT NULL,                       -- IANA time zone, e.g. America/New_York
  bbox_json TEXT NOT NULL,                -- [minLon,minLat,maxLon,maxLat]
  calendar_json TEXT                      -- terms, exam weeks, breaks
);

CREATE TABLE building (
  id INTEGER PRIMARY KEY,
  campus_id INTEGER NOT NULL REFERENCES campus(id),
  name TEXT NOT NULL,
  aliases_json TEXT,                      -- alternative names (entity resolution)
  lat REAL NOT NULL,
  lon REAL NOT NULL,
  fence_json TEXT NOT NULL,               -- polygon [[lon,lat],...]
  hours_json TEXT
);
CREATE INDEX building_campus ON building(campus_id);
CREATE VIRTUAL TABLE building_rtree USING rtree(id, min_lon, max_lon, min_lat, max_lat);

-- Attribute registry: the single source of truth for the spot schema ---------
CREATE TABLE attr_def (
  key TEXT PRIMARY KEY,                   -- whiteboard, food_allowed, noise, ...
  kind TEXT NOT NULL CHECK (kind IN ('flag', 'ordinal', 'enum', 'text')),
  grp TEXT NOT NULL CHECK (grp IN ('location', 'infra', 'rules', 'vibe')),
  bit INTEGER UNIQUE CHECK (bit IS NULL OR (bit >= 0 AND bit < 63)),
  domain_json TEXT,                       -- ordinal: {"min":..,"max":..}; enum: [..]
  label_zh TEXT,
  label_en TEXT,
  sort INTEGER NOT NULL DEFAULT 0,
  CHECK ((kind = 'flag') = (bit IS NOT NULL))
);

-- Users (declared before tables that reference them) --------------------------
CREATE TABLE user (
  id INTEGER PRIMARY KEY,
  email TEXT UNIQUE NOT NULL COLLATE NOCASE,
  pw_hash BLOB NOT NULL,
  pw_salt BLOB NOT NULL,
  pw_params TEXT NOT NULL,                -- argon2 parameters used for pw_hash
  display_name TEXT,
  campus_id INTEGER REFERENCES campus(id),
  major TEXT,
  langs_json TEXT,
  courses_json TEXT,
  role TEXT NOT NULL DEFAULT 'student'
    CHECK (role IN ('student', 'merchant', 'campus_admin')),
  reputation REAL NOT NULL DEFAULT 1.0,   -- 0.1 .. 3.0
  karma INTEGER NOT NULL DEFAULT 0,       -- cached balance, karma_ledger is authoritative
  created_at INTEGER NOT NULL
);

CREATE TABLE auth_session (
  token_hash BLOB PRIMARY KEY,            -- blake2b(token); the token itself is never stored
  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX auth_session_user ON auth_session(user_id);

-- Phase 0 provenance ------------------------------------------------------------
CREATE TABLE source_doc (
  id INTEGER PRIMARY KEY,
  campus_id INTEGER REFERENCES campus(id),
  url TEXT UNIQUE,
  kind TEXT,                              -- official / reddit / blog / maps / discord / manual
  fetched_at INTEGER,
  content_sha256 BLOB,
  license_note TEXT
);

-- Spots: materialized columns are recomputed from claims, never written directly
CREATE TABLE spot (
  id INTEGER PRIMARY KEY,
  building_id INTEGER NOT NULL REFERENCES building(id),
  name TEXT NOT NULL,
  floor TEXT,
  lat REAL,
  lon REAL,
  features INTEGER NOT NULL DEFAULT 0,    -- flag bitmap, bit positions in attr_def
  noise INTEGER,                          -- 0 silent .. 4 loud
  outlets INTEGER,                        -- 0 none .. 3 plenty
  temp INTEGER,                           -- -2 very cold .. 2 very warm
  capacity INTEGER,                       -- estimated seats
  vibe TEXT,                              -- deep_work / collab / casual
  attrs_json TEXT,                        -- every materialized attribute value
  summary TEXT,
  pros_json TEXT,
  cons_json TEXT,
  quality REAL NOT NULL DEFAULT 0,        -- overall confidence 0..1
  status TEXT NOT NULL DEFAULT 'active'
    CHECK (status IN ('active', 'hidden', 'merged')),
  created_by INTEGER REFERENCES user(id), -- NULL for pipeline-created spots
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX spot_bldg ON spot(building_id) WHERE status = 'active';
CREATE VIRTUAL TABLE spot_fts USING fts5(
  name, summary, tags, content='', contentless_delete=1, tokenize='unicode61'
);

-- Claims and votes ----------------------------------------------------------------
CREATE TABLE claim (
  id INTEGER PRIMARY KEY,
  spot_id INTEGER NOT NULL REFERENCES spot(id),
  attr TEXT NOT NULL REFERENCES attr_def(key),
  value TEXT NOT NULL,                    -- JSON scalar text
  source TEXT NOT NULL CHECK (source IN ('llm', 'official', 'user')),
  user_id INTEGER REFERENCES user(id),
  source_doc_id INTEGER REFERENCES source_doc(id),
  evidence TEXT,                          -- verbatim quote
  prior REAL NOT NULL,                    -- initial confidence
  up REAL NOT NULL DEFAULT 0,             -- weighted up votes
  down REAL NOT NULL DEFAULT 0,           -- weighted down votes
  created_at INTEGER NOT NULL
);
CREATE INDEX claim_spot_attr ON claim(spot_id, attr);
CREATE INDEX claim_source_doc ON claim(source_doc_id) WHERE source_doc_id IS NOT NULL;

CREATE TABLE claim_vote (
  claim_id INTEGER NOT NULL REFERENCES claim(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES user(id),
  v INTEGER NOT NULL CHECK (v IN (-1, 1)),
  weight REAL NOT NULL,
  at INTEGER NOT NULL,
  PRIMARY KEY (claim_id, user_id)
) WITHOUT ROWID;

-- Live state ------------------------------------------------------------------------
CREATE TABLE report (
  id INTEGER PRIMARY KEY,
  spot_id INTEGER NOT NULL REFERENCES spot(id),
  user_id INTEGER NOT NULL REFERENCES user(id),
  level INTEGER CHECK (level IN (0, 1, 2)), -- green / yellow / red; NULL for events
  event TEXT,                              -- outlet_broken / closed_event / ...
  in_fence INTEGER NOT NULL,
  accuracy_m REAL,
  weight REAL NOT NULL,
  at INTEGER NOT NULL,
  CHECK ((level IS NULL) <> (event IS NULL))
);
CREATE INDEX report_spot_at ON report(spot_id, at DESC);
CREATE INDEX report_user_at ON report(user_id, at DESC);

CREATE TABLE checkin (
  id INTEGER PRIMARY KEY,
  spot_id INTEGER NOT NULL REFERENCES spot(id),
  user_id INTEGER NOT NULL REFERENCES user(id),
  start_at INTEGER NOT NULL,
  end_at INTEGER,
  last_beat_at INTEGER,
  verified_ms INTEGER NOT NULL DEFAULT 0, -- accumulated in-fence heartbeat time
  outside_beats INTEGER NOT NULL DEFAULT 0,
  verified INTEGER NOT NULL
);
CREATE UNIQUE INDEX checkin_open ON checkin(user_id) WHERE end_at IS NULL;
CREATE INDEX checkin_spot ON checkin(spot_id, start_at);

CREATE TABLE occupancy_live (
  spot_id INTEGER PRIMARY KEY REFERENCES spot(id),
  est REAL NOT NULL,                      -- continuous estimate 0..2
  conf REAL NOT NULL,
  basis TEXT NOT NULL CHECK (basis IN ('reports', 'forecast')),
  updated_at INTEGER NOT NULL
);

CREATE TABLE occupancy_hourly (
  spot_id INTEGER NOT NULL,
  hour_ts INTEGER NOT NULL,               -- UTC seconds, start of hour
  n_reports INTEGER NOT NULL,
  n_users INTEGER NOT NULL,
  mean_level REAL,
  n_checkins INTEGER NOT NULL,
  PRIMARY KEY (spot_id, hour_ts)
) WITHOUT ROWID;

CREATE TABLE forecast_slot (
  spot_id INTEGER NOT NULL,
  day_type TEXT NOT NULL,                 -- normal / exam / break
  dow INTEGER NOT NULL,                   -- 0 = Sunday, campus local time
  slot INTEGER NOT NULL,                  -- half-hour slot 0..47, campus local time
  level REAL NOT NULL,
  n INTEGER NOT NULL,
  PRIMARY KEY (spot_id, day_type, dow, slot)
) WITHOUT ROWID;

-- Karma and badges --------------------------------------------------------------------
CREATE TABLE karma_ledger (
  id INTEGER PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES user(id),
  delta INTEGER NOT NULL,
  reason TEXT NOT NULL,
  ref_type TEXT NOT NULL,                 -- must be non-NULL: NULLs defeat UNIQUE
  ref_id INTEGER NOT NULL,
  at INTEGER NOT NULL,
  UNIQUE (user_id, reason, ref_type, ref_id)  -- idempotency key
);

CREATE TABLE badge (key TEXT PRIMARY KEY, name TEXT NOT NULL, rule_json TEXT NOT NULL);
CREATE TABLE user_badge (
  user_id INTEGER NOT NULL REFERENCES user(id),
  badge_key TEXT NOT NULL REFERENCES badge(key),
  at INTEGER NOT NULL,
  PRIMARY KEY (user_id, badge_key)
) WITHOUT ROWID;

CREATE TABLE photo (
  id INTEGER PRIMARY KEY,
  spot_id INTEGER NOT NULL REFERENCES spot(id),
  user_id INTEGER NOT NULL REFERENCES user(id),
  path TEXT NOT NULL,
  sha256 BLOB UNIQUE,
  up REAL NOT NULL DEFAULT 0,
  down REAL NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'visible' CHECK (status IN ('visible', 'hidden')),
  at INTEGER NOT NULL
);

-- Attribute registry v1 (roadmap 6.4) -------------------------------------------------
-- Flags own a bit in spot.features; bit positions must never be reused.
INSERT INTO attr_def (key, kind, grp, bit, domain_json, label_zh, label_en, sort) VALUES
  ('indoor',               'flag',    'location', 0,    NULL, '室内',         'Indoor',              10),
  ('outlets',              'ordinal', 'infra',    NULL, '{"min":0,"max":3}', '插座', 'Outlets',       20),
  ('whiteboard',           'flag',    'infra',    1,    NULL, '白板',         'Whiteboard',          21),
  ('large_tables',         'flag',    'infra',    2,    NULL, '大桌子',       'Large tables',        22),
  ('monitors',             'flag',    'infra',    3,    NULL, '显示器',       'Monitors',            23),
  ('printers',             'flag',    'infra',    4,    NULL, '打印机',       'Printers',            24),
  ('wifi_quality',         'ordinal', 'infra',    NULL, '{"min":0,"max":3}', 'Wi-Fi 质量', 'Wi-Fi quality', 25),
  ('natural_light',        'flag',    'infra',    5,    NULL, '自然光',       'Natural light',       26),
  ('restroom_near',        'flag',    'infra',    6,    NULL, '洗手间近',     'Restroom nearby',     27),
  ('accessible',           'flag',    'infra',    7,    NULL, '无障碍',       'Accessible',          28),
  ('capacity',             'ordinal', 'infra',    NULL, '{"min":0,"max":2000}', '座位数', 'Seats', 29),
  ('food_allowed',         'flag',    'rules',    8,    NULL, '可以吃东西',   'Food allowed',        30),
  ('calls_allowed',        'flag',    'rules',    9,    NULL, '可以打电话',   'Calls allowed',       31),
  ('group_ok',             'flag',    'rules',    10,   NULL, '适合小组',     'Group friendly',      32),
  ('reservable',           'flag',    'rules',    11,   NULL, '可预约',       'Reservable',          33),
  ('hours',                'text',    'rules',    NULL, NULL, '开放时间',     'Hours',               34),
  ('access_card_required', 'flag',    'rules',    12,   NULL, '需刷卡',       'Card access',         35),
  ('late_night',           'flag',    'rules',    13,   NULL, '深夜开放',     'Open late',           36),
  ('noise',                'ordinal', 'vibe',     NULL, '{"min":0,"max":4}', '噪音', 'Noise',          40),
  ('temp',                 'ordinal', 'vibe',     NULL, '{"min":-2,"max":2}', '温度', 'Temperature',   41),
  ('vibe',                 'enum',    'vibe',     NULL, '["deep_work","collab","casual"]', '氛围', 'Vibe', 42),
  ('crowd_typical',        'ordinal', 'vibe',     NULL, '{"min":0,"max":2}', '平时拥挤度', 'Typical crowd', 43),
  ('food_nearby',          'flag',    'vibe',     14,   NULL, '附近有吃的',   'Food nearby',         44);
