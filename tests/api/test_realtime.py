"""M3: WebSocket protocol (9.2), Pub/Sub push, replay and the decay job (4.6).

The M3 gate: one client subscribes, another reports, the subscriber sees the
color change; after reconnecting with `since` the missed message is replayed.
"""

import asyncio
import json
import time

import pytest
import websockets

LIBRARY = {"lat": 40.0000, "lon": -83.0150, "accuracy_m": 10}   # building 1
ENG_HALL = {"lat": 40.0030, "lon": -83.0120, "accuracy_m": 10}  # building 2
MIN = 60_000


@pytest.fixture(scope="module")
def rt(spawn):
    # fast decay ticks; a small subscription cap to exercise the limit
    return spawn(rules={"job_live_decay_ms": 250, "ws_max_subs": 3})


def user(c):
    u = c.new_client()
    u.register()
    return u


async def recv_json(ws, timeout=3.0):
    return json.loads(await asyncio.wait_for(ws.recv(), timeout))


async def recv_live(ws, spot, timeout=3.0):
    """Next `live` message for `spot` (skips other traffic)."""
    deadline = time.monotonic() + timeout
    while True:
        m = await recv_json(ws, max(0.01, deadline - time.monotonic()))
        if m.get("t") == "live" and m.get("spot") == spot:
            return m


async def silent(ws, timeout=0.6):
    try:
        m = await asyncio.wait_for(ws.recv(), timeout)
    except asyncio.TimeoutError:
        return True
    raise AssertionError(f"unexpected message: {m}")


async def op(ws, **msg):
    await ws.send(json.dumps(msg))
    return await recv_json(ws)


def run(coro):
    return asyncio.run(coro)


def test_two_clients_see_color_change(rt):
    reporter = user(rt)

    async def main():
        async with websockets.connect(rt.ws_url) as a, websockets.connect(rt.ws_url) as b:
            assert await op(a, op="sub", ch="spot:2") == {"t": "ok", "op": "sub", "ch": "spot:2"}
            assert (await op(b, op="sub", ch="bldg:1"))["t"] == "ok"
            r = await asyncio.to_thread(reporter.post, "/api/v1/spots/2/reports", {"level": 2, **LIBRARY})
            assert r.status == 201, r.body
            for ws in (a, b):
                m = await recv_live(ws, 2)
                assert m["color"] == "red" and m["basis"] == "reports"
                assert m["est"] == 1.67 and m["conf"] == 0.63
                assert abs(m["at"] - time.time() * 1000) < 5000  # epoch ms

    run(main())


def test_unchanged_color_is_not_pushed(rt):
    reporter = user(rt)

    async def main():
        async with websockets.connect(rt.ws_url) as ws:
            await op(ws, op="sub", ch="spot:6")
            # no position: g = 0.2, the estimate stays yellow / forecast
            r = await asyncio.to_thread(reporter.post, "/api/v1/spots/6/reports", {"level": 2})
            assert r.json()["live"]["color"] == "yellow"
            assert await silent(ws)

    run(main())


def test_reconnect_with_since_replays_missed_messages(rt):
    a, b = user(rt), user(rt)

    async def main():
        async with websockets.connect(rt.ws_url) as ws:
            await op(ws, op="sub", ch="spot:4")
            await asyncio.to_thread(a.post, "/api/v1/spots/4/reports", {"level": 2, **ENG_HALL})
            first = await recv_live(ws, 4)
            assert first["color"] == "red"
        # offline: red -> yellow
        r = await asyncio.to_thread(b.post, "/api/v1/spots/4/reports", {"level": 0, **ENG_HALL})
        assert r.json()["live"]["color"] == "yellow"
        async with websockets.connect(rt.ws_url) as ws:
            await op(ws, op="sub", ch="spot:4", since=first["at"] + 1)
            missed = await recv_live(ws, 4)
            assert missed["color"] == "yellow" and missed["at"] > first["at"]
            assert await silent(ws)
        async with websockets.connect(rt.ws_url) as ws:  # since is inclusive
            await op(ws, op="sub", ch="spot:4", since=first["at"])
            assert [(await recv_live(ws, 4))["color"] for _ in range(2)] == ["red", "yellow"]

    run(main())


def test_decay_job_pushes_when_reports_expire(rt):
    reporter = user(rt)

    async def main():
        async with websockets.connect(rt.ws_url) as ws:
            await op(ws, op="sub", ch="spot:3")
            # a report that is 90 minutes old (minus 1.5 s) by the real clock
            t = int(time.time() * 1000) - 90 * MIN + 1500
            r = await asyncio.to_thread(reporter.post, "/api/v1/spots/3/reports",
                                        {"level": 2, **LIBRARY}, headers={"X-FSS-Now": str(t)})
            assert r.json()["live"]["color"] == "red"
            assert (await recv_live(ws, 3))["color"] == "red"
            # the next live_decay tick sees the report almost expired
            m = await recv_live(ws, 3, timeout=3)
            assert m["color"] == "yellow" and m["basis"] == "forecast"
        await asyncio.sleep(2)  # past the window: back at rest
        live = rt.get("/api/v1/spots/3").json()["spot"]["live"]
        assert live["conf"] == 0 and live["est"] == 1.0

    run(main())


def test_handshake_rules(rt):
    async def rejected(url, **kw):
        with pytest.raises(websockets.exceptions.InvalidStatus) as e:
            async with websockets.connect(url, **kw):
                pass
        return e.value.response.status_code

    async def main():
        base = rt.base.replace("http://", "ws://")
        assert await rejected(base + "/") == 403
        assert await rejected(base + "/api/v1/ws") == 403
        assert await rejected(base + "/wsx") == 403
        assert await rejected(rt.ws_url, origin="http://evil.example") == 403
        host = rt.base.split("://", 1)[1]
        async with websockets.connect(rt.ws_url, origin=f"http://{host}") as ws:
            assert (await op(ws, op="sub", ch="spot:1"))["t"] == "ok"

    run(main())


def test_protocol_errors(rt):
    async def main():
        async with websockets.connect(rt.ws_url) as ws:
            async def code(**msg):
                m = await op(ws, **msg)
                assert m["t"] == "error", m
                return m["code"]

            assert await code(op="sub", ch="user:1") == "forbidden"
            assert await code(op="sub", ch="swm:1") == "not_available"
            assert await code(op="swm_msg", id=1, text="hi") == "not_available"
            assert await code(op="sub", ch="campus:demo:live") == "bad_channel"
            assert await code(op="sub", ch="spot:abc") == "bad_channel"
            assert await code(op="sub", ch="spot:0") == "bad_channel"
            assert await code(op="sub", ch="spot:99999") == "not_found"
            assert await code(op="sub", ch="bldg:99") == "not_found"
            assert await code(op="sub", ch="spot:1", since="yesterday") == "bad_since"
            assert await code(op="dance") == "unknown_op"
            assert await code(op="view", campus="demo") == "bad_view"
            assert await code(op="view", campus="atlantis", bbox=[0, 0, 1, 1]) == "not_found"
            await ws.send("not json")
            assert (await recv_json(ws))["code"] == "bad_message"
            await ws.send(b"\x00\x01")
            assert (await recv_json(ws))["code"] == "bad_message"
            # still usable afterwards
            assert (await op(ws, op="unsub", ch="spot:1"))["t"] == "ok"

    run(main())


def test_user_channel_is_private(rt):
    me = user(rt)
    uid = me.get("/api/v1/me").json()["user"]["id"]

    async def main():
        async with websockets.connect(rt.ws_url, additional_headers=me.cookie_header()) as ws:
            assert await op(ws, op="sub", ch=f"user:{uid}") == {"t": "ok", "op": "sub", "ch": f"user:{uid}"}
            assert (await op(ws, op="sub", ch=f"user:{uid + 1}"))["code"] == "forbidden"
            assert (await op(ws, op="unsub", ch=f"user:{uid}"))["code"] == "forbidden"
        # an invalid cookie makes the connection anonymous, it is not refused
        async with websockets.connect(rt.ws_url, additional_headers={"Cookie": "fss_sid=" + "0" * 64}) as ws:
            assert (await op(ws, op="sub", ch=f"user:{uid}"))["code"] == "forbidden"

    run(main())


def test_subscription_limit(rt):
    async def main():
        async with websockets.connect(rt.ws_url) as ws:
            for s in (1, 2, 3):
                assert (await op(ws, op="sub", ch=f"spot:{s}"))["t"] == "ok"
            assert (await op(ws, op="sub", ch="spot:2"))["t"] == "ok"  # already counted
            assert (await op(ws, op="sub", ch="spot:4"))["code"] == "too_many_subscriptions"
            await op(ws, op="unsub", ch="spot:1")
            assert (await op(ws, op="sub", ch="spot:4"))["t"] == "ok"
            # the whole campus has 4 buildings, only 0 slots are left
            v = await op(ws, op="view", campus="demo", bbox=[-83.03, 39.99, -83.00, 40.01])
            assert v == {"t": "ok", "op": "view", "buildings": [], "truncated": True}

    run(main())


def test_view_follows_the_map(rt):
    a, b = user(rt), user(rt)

    async def main():
        async with websockets.connect(rt.ws_url) as ws:
            v = await op(ws, op="view", campus="demo", bbox=[-83.0125, 40.0025, -83.0115, 40.0035])
            assert v == {"t": "ok", "op": "view", "buildings": [2], "truncated": False}
            v = await op(ws, op="view", campus="demo", bbox=[-83.03, 39.99, -83.00, 40.01])
            assert v["buildings"] == [1, 2, 3] and v["truncated"] is True
            await asyncio.to_thread(a.post, "/api/v1/spots/5/reports", {"level": 2, **ENG_HALL})
            assert (await recv_live(ws, 5))["color"] == "red"  # via bldg:2

            # pan to the library: building 2 leaves the view
            v = await op(ws, op="view", campus="demo", bbox=[-83.0155, 39.9995, -83.0145, 40.0005])
            assert v["buildings"] == [1]
            await asyncio.to_thread(b.post, "/api/v1/spots/5/reports", {"level": 0, **ENG_HALL})
            assert await silent(ws)

            # an explicit subscription survives view changes
            await op(ws, op="sub", ch="bldg:4")
            v = await op(ws, op="view", campus="demo", bbox=[-83.0105, 39.9975, -83.0095, 39.9985])
            assert v["buildings"] == [3]
            c = user(rt)
            science = {"lat": 40.0015, "lon": -83.0180, "accuracy_m": 10}
            await asyncio.to_thread(c.post, "/api/v1/spots/8/reports", {"level": 2, **science})
            assert (await recv_live(ws, 8))["color"] == "red"

    run(main())
