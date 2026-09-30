"""M0: health, routing and error envelope."""


def test_health(server):
    r = server.get("/api/v1/health")
    assert r.status == 200
    assert r.headers["content-type"].startswith("application/json")
    body = r.json()
    assert body["ok"] is True
    assert body["schema"] >= 1


def test_trailing_slash_is_ignored(server):
    assert server.get("/api/v1/health/").status == 200


def test_unknown_endpoint_uses_error_envelope(server):
    r = server.get("/api/v1/definitely-not-here")
    assert r.status == 404
    assert r.json()["error"]["code"] == "not_found"


def test_wrong_method_is_405(server):
    r = server.post("/api/v1/health", body={})
    assert r.status == 405
    assert r.json()["error"]["code"] == "method_not_allowed"


def test_campuses_empty(server):
    r = server.get("/api/v1/campuses")
    assert r.status == 200
    assert r.json() == {"campuses": []}


def test_unknown_campus(server):
    assert server.get("/api/v1/campuses/nowhere").status == 404
