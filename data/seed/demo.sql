-- Synthetic development fixture. NOT real campus data.
--
-- "Demo University" exists so that M2+ can be developed and tested before the
-- Phase 0 pipeline (M1) produces real OSU data. Coordinates are arbitrary and
-- buildings are simple squares. Load with:
--   server/build/fss --db data/fss.db --seed data/seed/demo.sql --materialize

INSERT INTO campus (id, slug, name, tz, bbox_json, calendar_json) VALUES
  (1, 'demo', 'Demo University (synthetic)', 'America/New_York',
   '[-83.0200,39.9960,-83.0080,40.0050]',
   '{"terms":[{"name":"Autumn 2026","start":"2026-08-25","end":"2026-12-15"}],
     "exam_weeks":[{"start":"2026-12-07","end":"2026-12-15"}],
     "breaks":[{"start":"2026-11-25","end":"2026-11-29"}]}');

INSERT INTO building (id, campus_id, name, aliases_json, lat, lon, fence_json, hours_json) VALUES
  (1, 1, 'Central Library', '["Main Library","CL"]', 40.0000, -83.0150,
   '[[-83.0154,39.9997],[-83.0146,39.9997],[-83.0146,40.0003],[-83.0154,40.0003],[-83.0154,39.9997]]',
   '{"mon-thu":"07:00-02:00","fri":"07:00-22:00","sat-sun":"10:00-22:00"}'),
  (2, 1, 'Engineering Hall', '["EH","Eng Hall"]', 40.0030, -83.0120,
   '[[-83.0124,40.0027],[-83.0116,40.0027],[-83.0116,40.0033],[-83.0124,40.0033],[-83.0124,40.0027]]',
   '{"daily":"06:00-24:00"}'),
  (3, 1, 'Student Union', '["Union","SU"]', 39.9980, -83.0100,
   '[[-83.0105,39.9977],[-83.0095,39.9977],[-83.0095,39.9983],[-83.0105,39.9983],[-83.0105,39.9977]]',
   '{"daily":"07:00-23:00"}'),
  (4, 1, 'Science Building', '["Sci"]', 40.0015, -83.0180,
   '[[-83.0184,40.0012],[-83.0176,40.0012],[-83.0176,40.0018],[-83.0184,40.0018],[-83.0184,40.0012]]',
   NULL);

INSERT INTO building_rtree (id, min_lon, max_lon, min_lat, max_lat) VALUES
  (1, -83.0154, -83.0146, 39.9997, 40.0003),
  (2, -83.0124, -83.0116, 40.0027, 40.0033),
  (3, -83.0105, -83.0095, 39.9977, 39.9983),
  (4, -83.0184, -83.0176, 40.0012, 40.0018);

INSERT INTO source_doc (id, campus_id, url, kind, fetched_at, license_note) VALUES
  (1, 1, 'fixture://demo/official', 'manual', 1790000000000, 'synthetic fixture');

INSERT INTO spot (id, building_id, name, floor, lat, lon, summary, status, created_at, updated_at) VALUES
  (1, 1, 'Reading Room', '1', 40.0001, -83.0151, 'Large silent reading room with long tables.', 'active', 1790000000000, 1790000000000),
  (2, 1, 'Basement Carrels', 'B', 39.9999, -83.0149, 'Individual carrels, cold, open late.', 'active', 1790000000000, 1790000000000),
  (3, 1, 'Group Study Rooms', '3', 40.0002, -83.0148, 'Bookable rooms with whiteboards.', 'active', 1790000000000, 1790000000000),
  (4, 2, 'Atrium', '1', 40.0030, -83.0121, 'Busy open atrium near the cafe.', 'active', 1790000000000, 1790000000000),
  (5, 2, 'Lab Lounge', '2', 40.0031, -83.0119, 'Card-access lounge with monitors.', 'active', 1790000000000, 1790000000000),
  (6, 3, 'Food Court Tables', '1', 39.9980, -83.0101, 'Loud, food everywhere.', 'active', 1790000000000, 1790000000000),
  (7, 3, 'Quiet Lounge', '2', 39.9981, -83.0099, 'Sunny lounge, few outlets.', 'active', 1790000000000, 1790000000000),
  (8, 4, 'Basement Hallway', 'B', 40.0014, -83.0181, NULL, 'active', 1790000000000, 1790000000000),
  (9, 4, 'Roof Terrace', '5', 40.0016, -83.0179, NULL, 'hidden', 1790000000000, 1790000000000);

-- Official-looking claims (prior 0.8)
INSERT INTO claim (spot_id, attr, value, source, source_doc_id, prior, created_at)
SELECT column1, column2, column3, 'official', 1, 0.8, 1790000000000 FROM (VALUES
  (1, 'indoor', 'true'), (1, 'noise', '1'), (1, 'outlets', '2'), (1, 'large_tables', 'true'),
  (1, 'natural_light', 'true'), (1, 'food_allowed', 'false'), (1, 'vibe', '"deep_work"'),
  (1, 'capacity', '120'), (1, 'wifi_quality', '3'),
  (2, 'indoor', 'true'), (2, 'noise', '0'), (2, 'outlets', '3'), (2, 'late_night', 'true'),
  (2, 'temp', '-2'), (2, 'vibe', '"deep_work"'), (2, 'food_nearby', 'false'),
  (3, 'indoor', 'true'), (3, 'group_ok', 'true'), (3, 'whiteboard', 'true'),
  (3, 'reservable', 'true'), (3, 'noise', '2'), (3, 'vibe', '"collab"'), (3, 'outlets', '2'),
  (4, 'indoor', 'true'), (4, 'noise', '3'), (4, 'food_allowed', 'true'), (4, 'outlets', '1'),
  (4, 'vibe', '"casual"'), (4, 'group_ok', 'true'),
  (5, 'indoor', 'true'), (5, 'whiteboard', 'true'), (5, 'monitors', 'true'), (5, 'outlets', '3'),
  (5, 'late_night', 'true'), (5, 'access_card_required', 'true'), (5, 'noise', '2'),
  (5, 'vibe', '"collab"'),
  (6, 'indoor', 'true'), (6, 'noise', '4'), (6, 'food_allowed', 'true'), (6, 'calls_allowed', 'true'),
  (6, 'food_nearby', 'true'), (6, 'vibe', '"casual"'),
  (7, 'indoor', 'true'), (7, 'noise', '1'), (7, 'outlets', '1'), (7, 'natural_light', 'true'),
  (7, 'vibe', '"deep_work"'), (7, 'hours', '"07:00-23:00"'),
  (8, 'outlets', '2'),
  (9, 'indoor', 'false'), (9, 'noise', '1')
);

-- Weak LLM-style claims (prior 0.45 < 0.55): stored but not materialized
INSERT INTO claim (spot_id, attr, value, source, source_doc_id, evidence, prior, created_at) VALUES
  (8, 'noise', '0', 'llm', 1, 'dead quiet down there', 0.45, 1790000000000),
  (8, 'late_night', 'true', 'llm', 1, 'open all night during finals', 0.45, 1790000000000);
