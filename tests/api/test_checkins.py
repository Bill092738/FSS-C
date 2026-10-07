"""M4: check-ins (roadmap 7.4): lifecycle, heartbeats, verified time, karma,
automatic ends (outside, stale, timeout) and the implicit crowding report.

Spot 1 (Reading Room, Central Library) has 120 seats in the demo fixture.
"""

import time

import pytest

MIN = 60_000
HOUR = 60 * MIN
LIBRARY = {"lat": 40.0000, "lon": -83.0150, "accuracy_m": 10}
FAR = {"lat": 40.0100, "lon": -83.0100, "accuracy_m": 10}


def at(t):
    return {"X-FSS-Now": str(t)}


def user(srv):
    c = srv.new_client()
    c.register()
    return c


def start(c, t, spot=1, **pos):
    return c.post("/api/v1/checkins", {"spot_id": spot, **pos}, headers=at(t))


def beat(c, cid, t, **pos):
    return c.post(f"/api/v1/checkins/{cid}/heartbeat", pos, headers=at(t))


def end(c, cid, t):
    return c.post(f"/api/v1/checkins/{cid}/end", {}, headers=at(t))


def base(offset_h):
    """Distinct time ranges per test (the module shares one server)."""
    return int(time.time() * 1000) + offset_h * 24 * HOUR


def estimate(obs, forecast=1.0, w0=0.5):
    total = sum(w for _, w in obs)
    return (w0 * forecast + sum(w * lvl for lvl, w in obs)) / (w0 + total)


def test_validation(demo):
    t = base(0)
    assert demo.new_client().post("/api/v1/checkins", {"spot_id": 1}).status == 401
    c = user(demo)
    for body in ({}, {"spot_id": "1"}, {"spot_id": 0}):
        r = c.post("/api/v1/checkins", body, headers=at(t))
        assert r.status == 422 and r.json()["error"]["code"] == "invalid_checkin", body
    assert start(c, t, spot=99999).status == 404
    r = start(c, t, spot=9)  # hidden
    assert r.status == 409 and r.json()["error"]["code"] == "spot_not_active"
    r = start(c, t, lat=40.0)
    assert r.status == 422 and r.json()["error"]["code"] == "invalid_position"
    r = c.post("/api/v1/checkins", raw=b'{"spot_id":1}', headers={"Content-Type": "text/plain"})
    assert r.status == 415
    assert beat(c, 99999, t).status == 404
    assert end(c, 99999, t).status == 404


def test_lifecycle_and_verified_time(demo):
    c, other = user(demo), user(demo)
    t = base(1)
    r = start(c, t, **LIBRARY)
    assert r.status == 201, r.body
    k = r.json()["checkin"]
    assert k["verified"] is True and k["in_fence"] is True and k["end_at"] is None
    assert k["start_at"] == t and k["verified_ms"] == 0
    cid = k["id"]
    me = c.get("/api/v1/me").json()["user"]
    assert me["checkin"]["id"] == cid and me["checkin"]["verified"] is True

    r = start(c, t + 1, spot=2, **LIBRARY)
    assert r.status == 409 and r.json()["error"]["checkin_id"] == cid
    # other users cannot see or touch it
    assert beat(other, cid, t + MIN, **LIBRARY).status == 404
    assert end(other, cid, t + MIN).status == 404

    k = beat(c, cid, t + 10 * MIN, **LIBRARY).json()["checkin"]
    assert k["verified_ms"] == 10 * MIN and k["outside_beats"] == 0
    # a 25-minute gap credits at most 20 minutes
    k = beat(c, cid, t + 35 * MIN, **LIBRARY).json()["checkin"]
    assert k["verified_ms"] == 30 * MIN
    r = end(c, cid, t + 40 * MIN)
    assert r.status == 200
    k = r.json()["checkin"]
    assert k["end_at"] == t + 40 * MIN and k["end_reason"] == "user"
    assert r.json()["karma"] == 0  # under an hour
    assert end(c, cid, t + 41 * MIN).status == 409
    r = beat(c, cid, t + 41 * MIN, **LIBRARY)
    assert r.status == 409 and r.json()["error"]["code"] == "checkin_ended"
    assert c.get("/api/v1/me").json()["user"]["checkin"] is None


def test_verified_hour_earns_karma(demo):
    c = user(demo)
    t = base(2)
    cid = start(c, t, **LIBRARY).json()["checkin"]["id"]
    for i in range(1, 8):  # 70 minutes inside
        assert beat(c, cid, t + i * 10 * MIN, **LIBRARY).status == 200
    r = end(c, cid, t + 75 * MIN)
    assert r.json()["karma"] == 1 and r.json()["checkin"]["verified_ms"] == 70 * MIN
    assert c.get("/api/v1/me").json()["user"]["karma"] == 1
    entries = c.get("/api/v1/me/karma").json()["entries"]
    assert entries[0]["reason"] == "checkin" and entries[0]["ref_id"] == cid


def test_outside_heartbeats_end_session(demo):
    c = user(demo)
    t = base(3)
    cid = start(c, t, **LIBRARY).json()["checkin"]["id"]
    k = beat(c, cid, t + 10 * MIN, **FAR).json()["checkin"]
    assert k["outside_beats"] == 1 and k["in_fence"] is False and k["verified_ms"] == 0
    k = beat(c, cid, t + 20 * MIN, **LIBRARY).json()["checkin"]  # back inside: reset
    assert k["outside_beats"] == 0 and k["verified_ms"] == 0  # previous was outside
    k = beat(c, cid, t + 30 * MIN, **LIBRARY).json()["checkin"]
    assert k["verified_ms"] == 10 * MIN
    beat(c, cid, t + 40 * MIN, **FAR)
    r = beat(c, cid, t + 50 * MIN)  # no position counts as outside
    k = r.json()["checkin"]
    assert k["end_reason"] == "outside" and k["end_at"] == t + 50 * MIN
    assert r.json()["karma"] == 0


def test_unverified_until_inside(demo):
    c = user(demo)
    t = base(4)
    k = start(c, t).json()["checkin"]  # no position
    assert k["verified"] is False
    k = beat(c, k["id"], t + 10 * MIN, **LIBRARY).json()["checkin"]
    assert k["verified"] is True and k["verified_ms"] == 0
    assert end(c, k["id"], t + 11 * MIN).status == 200


def test_stale_session_ends_at_last_heartbeat(demo):
    c = user(demo)
    t = base(5)
    cid = start(c, t, **LIBRARY).json()["checkin"]["id"]
    beat(c, cid, t + 10 * MIN, **LIBRARY)
    assert demo.run_job("checkin_timeout", t + 39 * MIN) == 0
    assert demo.run_job("checkin_timeout", t + 40 * MIN) >= 1
    k = c.get("/api/v1/me").json()["user"]["checkin"]
    assert k is None
    # a late heartbeat sees the ended session
    assert beat(c, cid, t + 41 * MIN, **LIBRARY).status == 409

    # without the job, the next check-in ends the stale one itself
    cid = start(c, t + HOUR, **LIBRARY).json()["checkin"]["id"]
    r = start(c, t + 2 * HOUR, spot=2, **LIBRARY)
    assert r.status == 201
    assert r.json()["checkin"]["id"] != cid


def test_timeout_job(spawn):
    srv = spawn(rules={"checkin_max_ms": 70 * MIN})
    c = user(srv)
    t = int(time.time() * 1000)
    cid = start(c, t, **LIBRARY).json()["checkin"]["id"]
    for i in range(1, 7):  # 60 minutes inside
        beat(c, cid, t + i * 10 * MIN, **LIBRARY)
    assert srv.run_job("checkin_timeout", t + 69 * MIN) == 0
    assert srv.run_job("checkin_timeout", t + 71 * MIN) == 1
    rows = c.get("/api/v1/me/karma").json()
    assert rows["karma"] == 1 and rows["entries"][0]["reason"] == "checkin"
    r = end(c, cid, t + 72 * MIN)
    assert r.status == 409

    # a heartbeat past the limit ends the session at the limit
    cid = start(c, t + 2 * HOUR, **LIBRARY).json()["checkin"]["id"]
    for i in range(1, 7):
        beat(c, cid, t + 2 * HOUR + i * 10 * MIN, **LIBRARY)
    k = beat(c, cid, t + 2 * HOUR + 75 * MIN, **LIBRARY).json()["checkin"]
    assert k["end_reason"] == "timeout" and k["end_at"] == t + 2 * HOUR + 70 * MIN


def test_checkins_feed_the_estimate(demo):
    a, b = user(demo), user(demo)
    t = base(6)
    ka = start(a, t, **LIBRARY).json()
    live = ka["live"]
    # one person in 120 seats: an implicit green report of weight 0.3
    assert live["est"] == pytest.approx(estimate([(2 / 120, 0.3)]), abs=0.005)
    assert live["basis"] == "forecast"
    spot = demo.get("/api/v1/spots/1", headers=at(t)).json()["spot"]
    assert spot["present"] == 1
    # unverified check-ins do not count
    kb = start(b, t + 1).json()
    assert kb["live"]["est"] == live["est"]
    k = beat(b, kb["checkin"]["id"], t + MIN, **LIBRARY).json()
    assert k["live"]["est"] == pytest.approx(estimate([(4 / 120, 0.3)]), abs=0.005)
    end(a, ka["checkin"]["id"], t + 2 * MIN)
    end(b, kb["checkin"]["id"], t + 2 * MIN)
    spot = demo.get("/api/v1/spots/1", headers=at(t + 2 * MIN)).json()["spot"]
    assert spot["present"] == 0 and spot["live"]["conf"] == 0 and spot["live"]["est"] == 1.0
