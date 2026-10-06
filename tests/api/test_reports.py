"""M3: crowding / event reports, geofence (7.2), estimate (7.3), limits (7.6).

Runs against the synthetic demo fixture. Every test uses its own users and
spots, and the X-FSS-Now header to control time.
"""

import sqlite3
import time

import pytest

MIN = 60_000
# Fence centers of the demo buildings (fences are ~70 m squares)
LIBRARY = {"lat": 40.0000, "lon": -83.0150}   # building 1: spots 1, 2, 3
ENG_HALL = {"lat": 40.0030, "lon": -83.0120}  # building 2: spots 4, 5
UNION = {"lat": 39.9980, "lon": -83.0100}     # building 3: spots 6, 7
SCIENCE = {"lat": 40.0015, "lon": -83.0180}   # building 4: spot 8 (9 hidden)


def at(t: int) -> dict:
    return {"X-FSS-Now": str(t)}


def user(demo):
    c = demo.new_client()
    c.register()
    return c


def report(c, spot, t, **body):
    return c.post(f"/api/v1/spots/{spot}/reports", body, headers=at(t))


def inside(center, accuracy=10):
    return {**center, "accuracy_m": accuracy}


def estimate(obs, forecast=1.0, now=0, half_life=20 * MIN, w0=0.5):
    """Reference implementation of roadmap 7.3."""
    ws = [(w * 2 ** (-(now - t) / half_life), lvl) for lvl, w, t in obs]
    total = sum(w for w, _ in ws)
    return (w0 * forecast + sum(w * lvl for w, lvl in ws)) / (w0 + total)


def test_reports_require_login_json_and_origin(demo):
    anon = demo.new_client()
    assert anon.post("/api/v1/spots/1/reports", {"level": 1}).status == 401
    c = user(demo)
    r = c.post("/api/v1/spots/1/reports", raw=b'{"level":1}', headers={"Content-Type": "text/plain"})
    assert r.status == 415
    r = c.post("/api/v1/spots/1/reports", {"level": 1}, headers={"Origin": "http://evil.example"})
    assert r.status == 403


def test_report_validation(demo):
    c = user(demo)
    t = int(time.time() * 1000)
    for body in ({}, {"level": 1, "event": "wifi_down"}, {"level": 3}, {"level": -1},
                 {"level": "2"}, {"level": 1.5}, {"event": "fire"}, {"event": 1}):
        r = report(c, 1, t, **body)
        assert r.status == 422 and r.json()["error"]["code"] == "invalid_report", body
    for pos in ({"lat": 40.0}, {"lat": 40.0, "lon": -83.0},
                {"lat": 40.0, "lon": -83.0, "accuracy_m": 0},
                {"lat": 91, "lon": -83.0, "accuracy_m": 5},
                {"lat": "40", "lon": -83.0, "accuracy_m": 5}):
        r = report(c, 1, t, level=1, **pos)
        assert r.status == 422 and r.json()["error"]["code"] == "invalid_position", pos
    assert report(c, 99999, t, level=1).status == 404
    r = report(c, 9, t, level=1)  # hidden spot
    assert r.status == 409 and r.json()["error"]["code"] == "spot_not_active"
    assert demo.post("/api/v1/spots/1/reports", {"level": 1}, headers={"X-FSS-Now": "soon"}).status == 400


def test_geofence_factor(demo):
    c = user(demo)
    t = int(time.time() * 1000)
    cases = [
        (inside(LIBRARY, 10), 1.0, True),
        (inside(LIBRARY, 80), 0.6, True),         # inside, accuracy 50..100
        # ~8.5 m east of the fence: outside, within accuracy
        ({"lat": 40.0000, "lon": -83.0145, "accuracy_m": 20}, 0.6, False),
        ({"lat": 40.0000, "lon": -83.0145, "accuracy_m": 5}, 0.2, False),
        ({"lat": 40.0000, "lon": -83.0145, "accuracy_m": 150}, 0.2, False),
        ({"lat": 40.0100, "lon": -83.0100, "accuracy_m": 10}, 0.2, False),
        ({}, 0.2, False),                         # no position
    ]
    for i, (pos, g, in_fence) in enumerate(cases):
        r = report(c, 1, t + i, level=1, **pos)
        assert r.status == 201, r.body
        rep = r.json()["report"]
        assert rep["fence"] == g and rep["in_fence"] is in_fence, pos
        assert rep["weight"] == pytest.approx(g)  # reputation 1.0


def test_estimate_mixes_decayed_reports(demo):
    a, b = user(demo), user(demo)
    t = int(time.time() * 1000) + 3 * 3600_000
    r = report(a, 7, t, level=2, **inside(UNION))
    assert r.status == 201, r.body
    live = r.json()["live"]
    assert live == {"est": 1.67, "conf": 0.63, "basis": "reports", "color": "red", "at": t}

    # 20 minutes later the red report weighs 0.5; a fresh green report weighs 1
    r = report(b, 7, t + 20 * MIN, level=0, **inside(UNION))
    live = r.json()["live"]
    expected = estimate([(2, 1.0, t), (0, 1.0, t + 20 * MIN)], now=t + 20 * MIN)
    assert live["est"] == pytest.approx(expected, abs=0.005)
    assert live["color"] == "yellow" and live["basis"] == "reports"

    spot = demo.get("/api/v1/spots/7", headers=at(t + 20 * MIN)).json()["spot"]
    assert spot["live"]["est"] == live["est"] and spot["live"]["at"] == t + 20 * MIN
    hit = next(s for s in demo.get("/api/v1/spots?campus=demo").json()["spots"] if s["id"] == 7)
    assert hit["live"]["color"] == "yellow"


def test_out_of_fence_report_stays_forecast(demo):
    c = user(demo)
    t = int(time.time() * 1000) + 3 * 3600_000
    live = report(c, 6, t, level=2).json()["live"]  # g = 0.2
    assert live["basis"] == "forecast"
    assert live["est"] == pytest.approx(estimate([(2, 0.2, t)], now=t), abs=0.005)


def test_forecast_prior_uses_typical_crowd(demo):
    # spot 8 gets crowd_typical = 0 (official claim) -> the forecast is green
    db = sqlite3.connect(demo.db_path)
    db.execute("INSERT INTO claim (spot_id, attr, value, source, prior, created_at)"
               " VALUES (8, 'crowd_typical', '0', 'official', 0.8, 0)")
    db.commit()
    db.close()
    c, voter = user(demo), user(demo)
    # any claim change re-materializes the spot; vote on that claim to trigger it
    cid = next(x["id"] for x in demo.get("/api/v1/spots/8").json()["spot"]["claims"]
               if x["attr"] == "crowd_typical")
    assert voter.post(f"/api/v1/claims/{cid}/vote", {"v": 1}).status == 200
    t = int(time.time() * 1000)
    live = report(c, 8, t, level=1).json()["live"]
    assert live["est"] == pytest.approx(estimate([(1, 0.2, t)], forecast=0.0, now=t), abs=0.005)


def test_only_last_report_in_dedupe_window_counts(demo):
    c = user(demo)
    t = int(time.time() * 1000) + 6 * 3600_000
    report(c, 3, t, level=2, **inside(LIBRARY))
    live = report(c, 3, t + 5 * MIN, level=0, **inside(LIBRARY)).json()["live"]
    # the red report is superseded by the green one 5 minutes later
    assert live["est"] == pytest.approx(estimate([(0, 1.0, t + 5 * MIN)], now=t + 5 * MIN), abs=0.005)
    assert live["color"] == "green"
    # 15 minutes after the green one, it counts again alongside the new report
    now = t + 20 * MIN
    live = report(c, 3, now, level=2, **inside(LIBRARY)).json()["live"]
    assert live["est"] == pytest.approx(
        estimate([(0, 1.0, t + 5 * MIN), (2, 1.0, now)], now=now), abs=0.005)


def test_teleport_limit(demo):
    c = user(demo)
    t = int(time.time() * 1000) + 9 * 3600_000
    for i, spot in enumerate((1, 4, 6)):  # buildings 1, 2, 3
        assert report(c, spot, t + i, level=1).status == 201
    r = report(c, 8, t + 10, level=1)  # building 4
    assert r.status == 429 and r.json()["error"]["code"] == "rate_limited"
    assert report(c, 2, t + 11, level=1).status == 201  # building 1 again is fine
    assert report(c, 8, t + 61 * MIN, level=1).status == 201


def test_event_needs_confirmation(demo):
    a, b = user(demo), user(demo)
    t = int(time.time() * 1000) + 12 * 3600_000
    r = report(a, 5, t, event="outlet_broken")
    assert r.status == 201, r.body
    assert r.json()["report"]["event"] == "outlet_broken"
    assert r.json()["event"] == {"kind": "outlet_broken", "visible": False, "reports": 1,
                                 "until": t + 4 * 3600_000}
    assert demo.get("/api/v1/spots/5", headers=at(t)).json()["spot"]["events"] == []
    # a repeat by the same user does not confirm it
    assert report(a, 5, t + 1, event="outlet_broken").json()["event"]["visible"] is False

    r = report(b, 5, t + MIN, event="outlet_broken")
    ev = r.json()["event"]
    assert ev["visible"] is True and ev["reports"] == 2 and ev["until"] == t + MIN + 4 * 3600_000
    events = demo.get("/api/v1/spots/5", headers=at(t + 2 * MIN)).json()["spot"]["events"]
    assert events == [{"kind": "outlet_broken", "reports": 2, "until": t + MIN + 4 * 3600_000}]
    # events do not touch the crowding estimate
    assert demo.get("/api/v1/spots/5").json()["spot"]["live"] is None
    # gone after the TTL
    assert demo.get("/api/v1/spots/5", headers=at(t + MIN + 4 * 3600_000 + 1)).json()["spot"]["events"] == []


def test_trusted_in_fence_user_confirms_alone(demo):
    trusted, normal = user(demo), user(demo)
    uid = trusted.get("/api/v1/me").json()["user"]["id"]
    db = sqlite3.connect(demo.db_path)
    db.execute("UPDATE user SET reputation = 2.0 WHERE id = ?", (uid,))
    db.commit()
    db.close()
    t = int(time.time() * 1000) + 15 * 3600_000
    # high reputation but outside the fence: not enough
    assert report(trusted, 4, t, event="wifi_down").json()["event"]["visible"] is False
    # a normal user inside the fence: not enough either
    assert report(normal, 4, t + 1, event="closed_event", **inside(ENG_HALL)).json()["event"]["visible"] is False
    r = report(trusted, 4, t + 2, event="closed_event", **inside(ENG_HALL))
    assert r.json()["report"]["weight"] == pytest.approx(2.0)
    assert r.json()["event"]["visible"] is True


def test_without_test_clock_header_is_ignored(spawn):
    plain = spawn(clock=False)
    c = plain.new_client()
    c.register()
    before = int(time.time() * 1000)
    r = report(c, 1, 1_000_000, level=1)
    assert r.status == 201
    assert r.json()["report"]["at"] >= before
