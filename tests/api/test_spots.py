"""M2: spot search (7.1), detail, claims, votes and materialization (7.6).

Runs against the synthetic demo fixture (data/seed/demo.sql).
"""


def ids(r):
    assert r.status == 200, r.body
    return [s["id"] for s in r.json()["spots"]]


def test_campus_exposes_attr_registry(demo):
    c = demo.get("/api/v1/campuses/demo").json()
    assert c["spots"] == 8  # the hidden spot is not counted
    keys = {a["key"]: a for a in c["attrs"]}
    assert keys["whiteboard"]["kind"] == "flag" and keys["whiteboard"]["bit"] == 1
    assert keys["noise"]["domain"] == {"min": 0, "max": 4}
    assert keys["vibe"]["domain"] == ["deep_work", "collab", "casual"]


def test_search_excludes_hidden_spots(demo):
    assert 9 not in ids(demo.get("/api/v1/spots?campus=demo"))
    assert set(ids(demo.get("/api/v1/spots?campus=demo"))) == set(range(1, 9))


def test_hard_filters(demo):
    assert ids(demo.get("/api/v1/spots?must=whiteboard&not=access_card_required")) == [3]
    assert set(ids(demo.get("/api/v1/spots?must=late_night,outlets"))) == {2, 5}
    assert set(ids(demo.get("/api/v1/spots?must=food_allowed"))) == {4, 6}
    assert set(ids(demo.get("/api/v1/spots?vibe=collab"))) == {3, 5}


def test_quiet_filter_and_ranking(demo):
    got = ids(demo.get("/api/v1/spots?quiet=3"))
    assert set(got) == {1, 2, 7}  # noise <= 1
    assert got[0] == 2            # noise 0 ranks first


def test_weak_llm_claims_do_not_materialize(demo):
    spot = demo.get("/api/v1/spots/8").json()["spot"]
    assert spot["noise"] is None and spot["features"] == 0
    assert spot["attrs"] == {"outlets": {"v": 2, "p": 0.8}}
    assert {c["attr"] for c in spot["claims"]} == {"outlets", "noise", "late_night"}
    assert 8 not in ids(demo.get("/api/v1/spots?must=late_night"))


def test_near_orders_by_distance(demo):
    r = demo.get("/api/v1/spots?near=40.0030,-83.0120&limit=2").json()
    assert [s["id"] for s in r["spots"]] == [4, 5]
    assert r["spots"][0]["dist_m"] < 20
    assert r["next"] == "2"
    page2 = demo.get("/api/v1/spots?near=40.0030,-83.0120&limit=2&cursor=2").json()
    assert not {4, 5} & {s["id"] for s in page2["spots"]}


def test_bbox_uses_building_rtree(demo):
    assert set(ids(demo.get("/api/v1/spots?bbox=-83.0130,40.0020,-83.0110,40.0040"))) == {4, 5}


def test_full_text_search(demo):
    assert set(ids(demo.get("/api/v1/spots?q=basement"))) == {2, 8}
    assert ids(demo.get("/api/v1/spots?q=Library%20whiteboard")) == [3]
    assert set(ids(demo.get("/api/v1/spots?q=%E7%99%BD%E6%9D%BF"))) == {3, 5}  # label_zh tag
    # FTS syntax is neutralized, not an error
    assert demo.get('/api/v1/spots?q=%22%20OR%20NEAR(').status == 200


def test_search_validation(demo):
    for q, code in [("must=noise", "attr_not_filterable"), ("must=nope", "unknown_attr"),
                    ("quiet=9", "bad_quiet"), ("bbox=1,2,3", "bad_bbox"),
                    ("must=whiteboard&not=whiteboard", "conflicting_filters"),
                    ("limit=0", "bad_limit"), ("campus=atlantis", "not_found")]:
        r = demo.get("/api/v1/spots?" + q)
        assert r.status in (400, 404, 422), q
        assert r.json()["error"]["code"] == code, q


def test_spot_detail_404(demo):
    assert demo.get("/api/v1/spots/99999").status == 404
    assert demo.get("/api/v1/spots/abc").status == 404


def test_claims_require_login(demo):
    r = demo.new_client().post("/api/v1/spots/1/claims", {"attr": "whiteboard", "value": True})
    assert r.status == 401


def test_claim_validation(demo):
    c = demo.new_client()
    c.register()
    assert c.post("/api/v1/spots/1/claims", {"attr": "nope", "value": 1}).json()["error"]["code"] == "unknown_attr"
    assert c.post("/api/v1/spots/1/claims", {"attr": "noise", "value": 9}).json()["error"]["code"] == "invalid_value"
    assert c.post("/api/v1/spots/1/claims", {"attr": "vibe", "value": "party"}).status == 422
    assert c.post("/api/v1/spots/1/claims", {"attr": "whiteboard", "value": 1}).status == 422
    assert c.post("/api/v1/spots/99999/claims", {"attr": "whiteboard", "value": True}).status == 404
    r = c.post("/api/v1/spots/1/claims", {"attr": "outlets", "value": 2})
    assert r.status == 409 and r.json()["error"]["code"] == "duplicate_claim"


def test_user_claim_needs_votes_to_materialize(demo):
    author, v1, v2, v3 = (demo.new_client() for _ in range(4))
    for c in (author, v1, v2, v3):
        c.register()
    # user prior 0.5 < 0.55: stored, not materialized
    r = author.post("/api/v1/spots/7/claims", {"attr": "whiteboard", "value": True, "evidence": "saw one by the window"})
    assert r.status == 201, r.body
    spot = r.json()["spot"]
    claim = next(c for c in spot["claims"] if c["attr"] == "whiteboard")
    assert claim["p"] == 0.5 and claim["source"] == "user"
    assert "whiteboard" not in spot["attrs"]
    assert 7 not in ids(demo.get("/api/v1/spots?must=whiteboard"))

    assert author.post(f"/api/v1/claims/{claim['id']}/vote", {"v": 1}).status == 403  # own claim
    r = v1.post(f"/api/v1/claims/{claim['id']}/vote", {"v": 1})
    assert r.status == 200
    assert abs(r.json()["claim"]["p"] - 3.5 / 6) < 1e-3  # (5*0.5 + 1) / 6
    assert 7 in ids(demo.get("/api/v1/spots?must=whiteboard"))

    # re-voting replaces, does not add
    r = v1.post(f"/api/v1/claims/{claim['id']}/vote", {"v": 1})
    assert r.json()["claim"]["up"] == 1

    # two down votes push it below the threshold again
    v2.post(f"/api/v1/claims/{claim['id']}/vote", {"v": -1})
    r = v3.post(f"/api/v1/claims/{claim['id']}/vote", {"v": -1})
    assert abs(r.json()["claim"]["p"] - 3.5 / 8) < 1e-3
    assert 7 not in ids(demo.get("/api/v1/spots?must=whiteboard"))

    # withdrawing votes
    v2.post(f"/api/v1/claims/{claim['id']}/vote", {"v": 0})
    r = v3.post(f"/api/v1/claims/{claim['id']}/vote", {"v": 0})
    assert r.json()["claim"]["down"] == 0
    assert 7 in ids(demo.get("/api/v1/spots?must=whiteboard"))


def test_conflicting_claims_highest_confidence_wins(demo):
    a, b = demo.new_client(), demo.new_client()
    a.register()
    b.register()
    # official says noise 1 (p 0.8) for spot 7; a user says 3 and gets one vote
    r = a.post("/api/v1/spots/7/claims", {"attr": "noise", "value": 3})
    cid = next(c["id"] for c in r.json()["spot"]["claims"] if c["attr"] == "noise" and c["value"] == 3)
    b.post(f"/api/v1/claims/{cid}/vote", {"v": 1})
    assert demo.get("/api/v1/spots/7").json()["spot"]["noise"] == 1


def test_vote_validation(demo):
    c = demo.new_client()
    c.register()
    assert c.post("/api/v1/claims/1/vote", {"v": 2}).status == 422
    assert c.post("/api/v1/claims/99999/vote", {"v": 1}).status == 404
    assert demo.new_client().post("/api/v1/claims/1/vote", {"v": 1}).status == 401


def test_submit_spot_starts_hidden(demo):
    c = demo.new_client()
    c.register()
    r = c.post("/api/v1/spots", {"building_id": 2, "name": "Stairwell Nook", "floor": "3",
                                 "claims": [{"attr": "outlets", "value": 1}, {"attr": "noise", "value": 1}]})
    assert r.status == 201, r.body
    spot = r.json()["spot"]
    assert spot["status"] == "hidden"
    assert spot["lat"] == 40.003 and spot["lon"] == -83.012  # building position
    assert len(spot["claims"]) == 2
    assert spot["id"] not in ids(demo.get("/api/v1/spots?campus=demo"))
    assert c.post("/api/v1/spots", {"building_id": 999, "name": "x"}).status == 422
    assert c.post("/api/v1/spots", {"name": "x"}).status == 422
    r = c.post("/api/v1/spots", {"building_id": 2, "name": "x", "claims": [{"attr": "noise", "value": 99}]})
    assert r.status == 422
