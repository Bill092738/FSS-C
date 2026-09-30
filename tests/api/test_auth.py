"""M2: registration, login, sessions, profile, CSRF baseline (roadmap 9.1, 10.5)."""

import uuid


def email():
    return f"a{uuid.uuid4().hex[:10]}@example.edu"


def test_register_sets_http_only_session_cookie(demo):
    c = demo.new_client()
    r = c.post("/api/v1/auth/register", {"email": email(), "password": "longenough1", "campus": "demo"})
    assert r.status == 201
    cookie = r.headers["set-cookie"]
    assert cookie.startswith("fss_sid=")
    assert "HttpOnly" in cookie or "httponly" in cookie.lower()
    assert "samesite=lax" in cookie.lower()
    user = r.json()["user"]
    assert user["campus"] == "demo"
    assert user["karma"] == 0 and user["reputation"] == 1.0
    assert "pw_hash" not in user
    assert c.get("/api/v1/me").json()["user"]["id"] == user["id"]


def test_duplicate_email_is_409_case_insensitive(demo):
    e = email()
    demo.new_client().register(e)
    r = demo.new_client().post("/api/v1/auth/register", {"email": e.upper(), "password": "longenough1"})
    assert r.status == 409
    assert r.json()["error"]["code"] == "email_taken"


def test_register_validation(demo):
    c = demo.new_client()
    assert c.post("/api/v1/auth/register", {"email": "nope", "password": "longenough1"}).status == 422
    assert c.post("/api/v1/auth/register", {"email": email(), "password": "short"}).status == 422
    r = c.post("/api/v1/auth/register", {"email": email(), "password": "longenough1", "campus": "atlantis"})
    assert r.json()["error"]["code"] == "unknown_campus"


def test_login_logout(demo):
    e = email()
    demo.new_client().register(e, password="p4ssw0rd-ok")
    c = demo.new_client()
    assert c.get("/api/v1/me").status == 401
    r = c.post("/api/v1/auth/login", {"email": e, "password": "wrong-password"})
    assert r.status == 401 and r.json()["error"]["code"] == "invalid_credentials"
    r = c.post("/api/v1/auth/login", {"email": e, "password": "p4ssw0rd-ok"})
    assert r.status == 200
    assert c.get("/api/v1/me").status == 200
    assert c.post("/api/v1/auth/logout", {}).status == 200
    assert c.get("/api/v1/me").status == 401


def test_unknown_account_login_is_401(demo):
    r = demo.new_client().post("/api/v1/auth/login", {"email": email(), "password": "whatever-123"})
    assert r.status == 401


def test_forged_cookie_is_anonymous(demo):
    c = demo.new_client()
    r = c.get("/api/v1/me", headers={"Cookie": "fss_sid=" + "0" * 64})
    assert r.status == 401


def test_patch_me(demo):
    c = demo.new_client()
    c.register()
    r = c.patch("/api/v1/me", {"display_name": "Ada", "major": "CSE",
                               "langs": ["en", "zh"], "courses": ["CSE 2221"], "campus": "demo"})
    assert r.status == 200, r.body
    u = r.json()["user"]
    assert (u["display_name"], u["major"], u["langs"], u["courses"]) == ("Ada", "CSE", ["en", "zh"], ["CSE 2221"])
    # untouched fields stay
    u = c.patch("/api/v1/me", {"major": "ECE"}).json()["user"]
    assert u["display_name"] == "Ada" and u["major"] == "ECE"
    assert c.patch("/api/v1/me", {"langs": "en"}).status == 422
    assert c.patch("/api/v1/me", {"courses": ["x"] * 21}).status == 422


def test_json_content_type_required(demo):
    r = demo.new_client().post("/api/v1/auth/login", raw=b"email=a&password=b",
                               headers={"Content-Type": "application/x-www-form-urlencoded"})
    assert r.status == 415


def test_cross_origin_write_rejected(demo):
    c = demo.new_client()
    r = c.post("/api/v1/auth/register", {"email": email(), "password": "longenough1"},
               headers={"Origin": "https://evil.example"})
    assert r.status == 403
    host = demo.base.split("://", 1)[1]
    r = c.post("/api/v1/auth/register", {"email": email(), "password": "longenough1"},
               headers={"Origin": "http://" + host})
    assert r.status == 201
