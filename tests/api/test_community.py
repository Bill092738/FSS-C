"""M4: karma ledger, badges, reputation, spot discovery, photos and the
per-IP /auth limit (roadmap 7.5, 7.6, 10.5).

Idempotency: every reward is keyed by (user, reason, ref); repeating the
trigger must not pay twice. Daily caps count a rolling 24 h.
"""

import asyncio
import json
import os
import sqlite3
import time

import pytest
import websockets

MIN = 60_000
HOUR = 60 * MIN
DAY = 24 * HOUR
LIBRARY = {"lat": 40.0000, "lon": -83.0150, "accuracy_m": 10}  # spots 1, 2, 3
ENG_HALL = {"lat": 40.0030, "lon": -83.0120, "accuracy_m": 10}  # spots 4, 5


def at(t):
    return {"X-FSS-Now": str(t)}


def user(srv):
    c = srv.new_client()
    c.register()
    return c


def me(c):
    return c.get("/api/v1/me").json()["user"]


def ledger(c):
    return c.get("/api/v1/me/karma?limit=200").json()["entries"]


def base(days):
    """Distinct time ranges per test (the module shares one server)."""
    return int(time.time() * 1000) + days * 3 * DAY


def report(c, spot, t, **body):
    return c.post(f"/api/v1/spots/{spot}/reports", body, headers=at(t))


def png(tag=None):
    return b"\x89PNG\r\n\x1a\n" + (tag or os.urandom(32))


def upload(c, spot, data, t, ctype="image/png"):
    return c.post(f"/api/v1/spots/{spot}/photos", raw=data,
                  headers={"Content-Type": ctype, **at(t)})


# -- reports --------------------------------------------------------------------

def test_report_karma_needs_presence_and_respects_dedupe(demo):
    c = user(demo)
    t = base(0)
    r = report(c, 1, t, level=1, **LIBRARY)
    assert r.json()["karma"] == 2
    assert report(c, 1, t + 5 * MIN, level=2, **LIBRARY).json()["karma"] == 0  # same spot
    assert report(c, 2, t + 6 * MIN, level=2, **LIBRARY).json()["karma"] == 2  # other spot
    assert report(c, 3, t + 7 * MIN, level=2).json()["karma"] == 0  # g = 0.2
    assert report(c, 1, t + 16 * MIN, level=1, **LIBRARY).json()["karma"] == 2
    u = me(c)
    assert u["karma"] == 6
    assert [b["key"] for b in u["badges"]] == ["first_report"]
    rows = ledger(c)
    assert [r["reason"] for r in rows] == ["report"] * 3
    assert all(r["delta"] == 2 and r["ref_type"] == "report" for r in rows)


def test_report_karma_daily_cap(demo):
    c = user(demo)
    t = base(1)
    earned = []
    for i in range(22):  # three spots in one building, each every 12 minutes
        r = report(c, 1 + i % 3, t + i * 4 * MIN, level=1, **LIBRARY)
        assert r.status == 201, r.body
        earned.append(r.json()["karma"])
    assert earned == [2] * 20 + [0, 0]
    assert me(c)["karma"] == 40
    # 24 h after the first rewards, the window has room again
    assert report(c, 1, t + DAY + 1, level=1, **LIBRARY).json()["karma"] == 2


def test_karma_and_badge_pushed_to_user_channel(demo):
    c = user(demo)
    t = base(2)

    async def main():
        async with websockets.connect(c.ws_url, additional_headers=c.cookie_header()) as ws:
            await asyncio.to_thread(report, c, 4, t, level=0, **ENG_HALL)
            got = {}
            while len(got) < 2:
                m = json.loads(await asyncio.wait_for(ws.recv(), 3))
                if m["t"] in ("karma", "badge"):
                    got[m["t"]] = m
            assert got["karma"]["delta"] == 2 and got["karma"]["total"] == 2
            assert got["karma"]["reason"] == "report" and got["karma"]["at"] > 0
            assert got["badge"]["key"] == "first_report"
            assert got["badge"]["name"] == "First Report"

    asyncio.run(main())


def test_event_confirmation_pays_once(demo):
    a, b, c = user(demo), user(demo), user(demo)
    t = base(3)
    assert report(a, 5, t, event="wifi_down").status == 201
    assert me(a)["karma"] == 0
    assert report(a, 5, t + MIN, event="wifi_down").status == 201  # own repeat
    assert me(a)["karma"] == 0
    report(b, 5, t + 2 * MIN, event="wifi_down")
    ua = me(a)
    assert ua["karma"] == 5 and ua["reputation"] == pytest.approx(1.05)
    assert [r["reason"] for r in ledger(a)] == ["event_confirmed"]
    assert me(b)["karma"] == 0
    # a third reporter confirms both earlier ones; `a` is not paid twice
    report(c, 5, t + 3 * MIN, event="wifi_down")
    assert me(a)["karma"] == 5 and me(b)["karma"] == 5 and me(c)["karma"] == 0


# -- claims -------------------------------------------------------------------------

def test_claim_accepted_after_two_up_votes(demo):
    author, v1, v2, v3 = (user(demo) for _ in range(4))
    r = author.post("/api/v1/spots/7/claims", {"attr": "whiteboard", "value": True})
    assert r.status == 201
    cid = next(x["id"] for x in r.json()["spot"]["claims"] if x["attr"] == "whiteboard")
    v1.post(f"/api/v1/claims/{cid}/vote", {"v": 1})
    assert me(author)["karma"] == 0
    v2.post(f"/api/v1/claims/{cid}/vote", {"v": 1})
    u = me(author)
    assert u["karma"] == 3 and u["reputation"] == pytest.approx(1.05)
    # re-voting, retracting and voting again never pays twice
    v1.post(f"/api/v1/claims/{cid}/vote", {"v": 0})
    v1.post(f"/api/v1/claims/{cid}/vote", {"v": 1})
    v3.post(f"/api/v1/claims/{cid}/vote", {"v": 1})
    u = me(author)
    assert u["karma"] == 3 and u["reputation"] == pytest.approx(1.05)
    assert [r["reason"] for r in ledger(author)] == ["claim_accepted"]


def test_claim_rejected_by_down_votes(demo):
    author = user(demo)
    r = author.post("/api/v1/spots/7/claims", {"attr": "noise", "value": 4})
    cid = next(x["id"] for x in r.json()["spot"]["claims"]
               if x["attr"] == "noise" and x["value"] == 4)
    for i in range(4):  # D = 4 -> p = 2.5 / 9 < 0.3
        user(demo).post(f"/api/v1/claims/{cid}/vote", {"v": -1})
        expected = 0.9 if i == 3 else 1.0
        assert me(author)["reputation"] == pytest.approx(expected), i
    user(demo).post(f"/api/v1/claims/{cid}/vote", {"v": -1})
    assert me(author)["reputation"] == pytest.approx(0.9)  # once


# -- spot discovery ---------------------------------------------------------------------

def test_spot_discovery_confirmation(demo):
    creator, a, b = user(demo), user(demo), user(demo)
    t = base(4)
    r = creator.post("/api/v1/spots", {"building_id": 2, "name": "Stairwell Nook"}, headers=at(t))
    spot = r.json()["spot"]
    assert spot["status"] == "hidden" and spot["confirmations"] == 0
    sid = spot["id"]

    def confirm(c):
        return c.post(f"/api/v1/spots/{sid}/confirm", {}, headers=at(t + MIN))

    r = confirm(creator)
    assert r.status == 403 and r.json()["error"]["code"] == "own_spot"
    assert confirm(a).json() == {"spot_id": sid, "status": "hidden", "confirmations": 1, "needed": 2}
    assert confirm(a).json()["confirmations"] == 1  # idempotent
    assert report(a, sid, t + MIN, level=1).status == 409  # still hidden
    assert confirm(b).json()["status"] == "active"
    r = confirm(b)
    assert r.status == 409 and r.json()["error"]["code"] == "already_active"
    u = me(creator)
    assert u["karma"] == 20 and u["reputation"] == pytest.approx(1.05)
    assert "pathfinder" in [x["key"] for x in u["badges"]]
    assert sid in [s["id"] for s in demo.get("/api/v1/spots?campus=demo&limit=50").json()["spots"]]
    assert report(a, sid, t + 2 * MIN, level=1).status == 201
    assert demo.get("/api/v1/spots/99999").status == 404
    assert a.post("/api/v1/spots/99999/confirm", {}).status == 404
    r = a.post("/api/v1/spots/1/confirm", {})
    assert r.status == 409


def test_spot_discovery_daily_cap(demo):
    creator = user(demo)
    voters = [user(demo), user(demo)]
    t = base(5)
    earned = []
    for i in range(3):
        sid = creator.post("/api/v1/spots", {"building_id": 3, "name": f"Corner {i}"},
                           headers=at(t + i)).json()["spot"]["id"]
        before = me(creator)["karma"]
        for v in voters:
            v.post(f"/api/v1/spots/{sid}/confirm", {}, headers=at(t + i * MIN))
        earned.append(me(creator)["karma"] - before)
    assert earned == [20, 20, 0]


# -- photos ----------------------------------------------------------------------------------

def test_photo_upload_serve_and_dedupe(demo):
    c = user(demo)
    t = base(6)
    data = png()
    r = upload(c, 2, data, t)
    assert r.status == 201, r.body
    photo = r.json()["photo"]
    assert r.json()["karma"] == 3
    assert photo["status"] == "visible" and photo["spot"] == 2
    assert photo["url"].startswith("/uploads/") and photo["url"].endswith(".png")
    served = demo.get(photo["url"])
    assert served.status == 200 and served.body == data
    assert served.headers["content-type"].startswith("image/png")
    assert (demo.uploads / photo["url"].rsplit("/", 1)[1]).read_bytes() == data
    assert [p["id"] for p in demo.get("/api/v1/spots/2").json()["spot"]["photos"]] == [photo["id"]]

    r = upload(user(demo), 3, data, t)
    assert r.status == 409 and r.json()["error"]["photo_id"] == photo["id"]
    assert upload(c, 2, b"\xff\xd8\xff" + os.urandom(16), t, "image/jpeg").status == 201
    webp = b"RIFF\x00\x00\x00\x00WEBP" + os.urandom(16)
    assert upload(c, 2, webp, t, "image/webp").status == 201

    r = upload(c, 2, b"GIF89a" + os.urandom(16), t, "image/gif")
    assert r.status == 415 and r.json()["error"]["code"] == "unsupported_image"
    r = c.post("/api/v1/spots/2/photos", {"x": 1})  # JSON is not an image
    assert r.status == 415
    assert upload(c, 2, b"", t).status == 422
    assert upload(c, 99999, png(), t).status == 404
    big = b"\x89PNG\r\n\x1a\n" + b"\0" * (5 << 20)
    assert upload(c, 2, big, t).status == 413
    assert demo.new_client().post("/api/v1/spots/2/photos", raw=png(),
                                  headers={"Content-Type": "image/png"}).status == 401
    r = c.post("/api/v1/spots/2/photos", raw=png(),
               headers={"Content-Type": "image/png", "Origin": "http://evil.example"})
    assert r.status == 403
    assert demo.get("/uploads/../test.db").status == 404
    assert demo.get("/uploads/nothing.png").status == 404


def test_photo_karma_cap_and_hiding(demo):
    c = user(demo)
    t = base(7)
    karma = [upload(c, 6, png(), t + i).json()["karma"] for i in range(6)]
    assert karma == [3, 3, 3, 3, 3, 0]
    assert me(c)["karma"] == 15
    photos = demo.get("/api/v1/spots/6").json()["spot"]["photos"]
    first = min(p["id"] for p in photos)

    r = c.post(f"/api/v1/photos/{first}/vote", {"v": -1})
    assert r.status == 403 and r.json()["error"]["code"] == "own_photo"
    assert c.post("/api/v1/photos/99999/vote", {"v": 1}).status == 404
    for i in range(4):
        r = user(demo).post(f"/api/v1/photos/{first}/vote", {"v": -1})
        assert r.status == 200
    assert r.json()["photo"]["status"] == "hidden"
    u = me(c)
    assert u["karma"] == 12 and u["reputation"] == pytest.approx(0.9)
    assert ledger(c)[0]["reason"] == "photo_hidden" and ledger(c)[0]["delta"] == -3
    assert first not in [p["id"] for p in demo.get("/api/v1/spots/6").json()["spot"]["photos"]]
    user(demo).post(f"/api/v1/photos/{first}/vote", {"v": -1})
    assert me(c)["karma"] == 12  # taken back once

    # the sixth photo earned nothing, so hiding it takes nothing back
    last = max(p["id"] for p in photos)
    for i in range(4):
        user(demo).post(f"/api/v1/photos/{last}/vote", {"v": -1})
    assert me(c)["karma"] == 12


def test_photo_upload_limit(spawn):
    srv = spawn(rules={"photo_daily_max": 2})
    c = user(srv)
    t = int(time.time() * 1000)
    assert upload(c, 1, png(), t).status == 201
    assert upload(c, 1, png(), t + 1).status == 201
    r = upload(c, 1, png(), t + 2)
    assert r.status == 429 and r.json()["error"]["code"] == "rate_limited"
    assert upload(c, 1, png(), t + DAY + 1).status == 201


# -- ledger, limits, test hooks ---------------------------------------------------------

def test_karma_ledger_pagination(demo):
    c = user(demo)
    t = base(8)
    for i in range(5):
        report(c, 1 + i % 3, t + i * 11 * MIN, level=1, **LIBRARY)
    page = c.get("/api/v1/me/karma?limit=2").json()
    assert page["karma"] == 10 and len(page["entries"]) == 2 and page["next"]
    ids = [e["id"] for e in page["entries"]]
    while page["next"]:
        page = c.get(f"/api/v1/me/karma?limit=2&cursor={page['next']}").json()
        ids += [e["id"] for e in page["entries"]]
    assert len(ids) == 5 and ids == sorted(ids, reverse=True)
    assert c.get("/api/v1/me/karma?limit=0").status == 400
    assert c.get("/api/v1/me/karma?cursor=x").status == 400
    assert demo.new_client().get("/api/v1/me/karma").status == 401


def test_ledger_idempotency_key(demo):
    """The UNIQUE key is the last line of defence against double credit."""
    db = sqlite3.connect(demo.db_path)
    row = db.execute("SELECT user_id, reason, ref_type, ref_id FROM karma_ledger LIMIT 1").fetchone()
    with pytest.raises(sqlite3.IntegrityError):
        db.execute("INSERT INTO karma_ledger (user_id, delta, reason, ref_type, ref_id, at)"
                   " VALUES (?, 1, ?, ?, ?, 0)", row)
    db.close()


def test_auth_rate_limit_per_ip(spawn):
    srv = spawn(rules={"auth_ip_per_min": 3})
    c = srv.new_client()
    t = int(time.time() * 1000)
    body = {"email": "nobody@example.edu", "password": "wrong password"}
    for i in range(3):
        assert c.post("/api/v1/auth/login", body, headers=at(t + i)).status == 401
    r = c.post("/api/v1/auth/login", body, headers=at(t + 10))
    assert r.status == 429 and r.json()["error"]["code"] == "rate_limited"
    assert 1 <= int(r.headers["retry-after"]) <= 60
    # other clients share the address; other endpoints are not limited
    other = srv.new_client()
    assert other.post("/api/v1/auth/register", {"email": "x@example.edu", "password": "long enough"},
                      headers=at(t + 20)).status == 429
    assert other.get("/api/v1/spots/1").status == 200
    # a new window
    assert c.post("/api/v1/auth/login", body, headers=at(t + 60_001)).status == 401


def test_debug_jobs_need_test_clock(spawn, demo):
    srv = spawn(clock=False)
    assert srv.post("/api/v1/debug/jobs/live_decay").status == 404
    assert demo.post("/api/v1/debug/jobs/nope").status == 404
    assert demo.run_job("live_decay") >= 0
