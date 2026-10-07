"""Black-box API test fixtures (roadmap 10.4).

Each test module gets a fresh server process with its own temporary database.
`server` starts from an empty schema; `demo` additionally loads the synthetic
fixture data/seed/demo.sql; `spawn` starts extra servers with rule overrides.
Servers run with --test-clock, so an `X-FSS-Now` header sets the request's
clock, and POST /api/v1/debug/jobs/{name} runs a periodic job at that clock.
Every client shares 127.0.0.1, so the per-IP /auth limit is raised unless a
test passes its own value (TEST_RULES). The binary defaults to server/build/fss; set FSS_BIN to test another
build (for example server/build/fss-debug).
"""

from __future__ import annotations

import http.cookiejar
import json
import os
import socket
import subprocess
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get("FSS_BIN", ROOT / "server" / "build" / "fss"))
MIGRATIONS = ROOT / "server" / "migrations"
DEMO_SEED = ROOT / "data" / "seed" / "demo.sql"
TEST_RULES = {"auth_ip_per_min": 1_000_000}


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@dataclass
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self):
        return json.loads(self.body)


class Client:
    """HTTP client with its own cookie jar (one instance per simulated user)."""

    def __init__(self, base: str):
        self.base = base
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar))

    def new_client(self) -> "Client":
        return Client(self.base)

    @property
    def ws_url(self) -> str:
        return self.base.replace("http://", "ws://", 1) + "/ws"

    def cookie_header(self) -> dict[str, str]:
        """Session cookie as a header, for WebSocket handshakes."""
        return {"Cookie": "; ".join(f"{c.name}={c.value}" for c in self.jar)}

    def request(self, method: str, path: str, body=None, headers=None, raw: bytes | None = None) -> Response:
        data = raw
        hdrs = dict(headers or {})
        if body is not None:
            data = json.dumps(body).encode()
            hdrs.setdefault("Content-Type", "application/json")
        req = urllib.request.Request(self.base + path, data=data, method=method, headers=hdrs)
        try:
            with self.opener.open(req, timeout=10) as r:
                return Response(r.status, {k.lower(): v for k, v in r.headers.items()}, r.read())
        except urllib.error.HTTPError as e:
            return Response(e.code, {k.lower(): v for k, v in e.headers.items()}, e.read())

    def get(self, path, **kw):
        return self.request("GET", path, **kw)

    def post(self, path, body=None, **kw):
        return self.request("POST", path, body=body, **kw)

    def run_job(self, name: str, now: int | None = None) -> int:
        """Runs a periodic job synchronously (needs --test-clock)."""
        hdrs = {"X-FSS-Now": str(now)} if now else {}
        r = self.post(f"/api/v1/debug/jobs/{name}", headers=hdrs)
        assert r.status == 200, r.body
        return r.json()["result"]

    def patch(self, path, body=None, **kw):
        return self.request("PATCH", path, body=body, **kw)

    def register(self, email: str | None = None, password: str = "correct horse battery", **extra) -> dict:
        email = email or f"u{uuid.uuid4().hex[:10]}@example.edu"
        r = self.post("/api/v1/auth/register", {"email": email, "password": password, **extra})
        assert r.status == 201, r.body
        return r.json()["user"]


def _run(args: list[str]) -> None:
    proc = subprocess.run([str(BIN), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stdout + proc.stderr)


def _start(tmp: Path, seed: Path | None, rules: dict | None = None, clock: bool = True):
    if not BIN.exists():
        pytest.skip(f"server binary not built: {BIN}")
    db = tmp / "test.db"
    common = ["--db", str(db), "--migrations", str(MIGRATIONS)]
    (tmp / "rules.json").write_text(json.dumps({**TEST_RULES, **(rules or {})}))
    extra = ["--rules", str(tmp / "rules.json"), "--uploads", str(tmp / "uploads")]
    if seed:
        _run([*common, "--migrate-only"])
        _run([*common, "--seed", str(seed), "--materialize"])
    port = _free_port()
    log = open(tmp / "server.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), *common, "--bind", f"127.0.0.1:{port}",
         "--public", str(tmp / "no-public"), "--threads", "2", "--quiet",
         *(["--test-clock"] if clock else []), *extra],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    client = Client(f"http://127.0.0.1:{port}")
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            if client.get("/api/v1/health").status == 200:
                break
        except OSError:
            pass
        if proc.poll() is not None:
            raise RuntimeError((tmp / "server.log").read_text())
        time.sleep(0.05)
    else:
        proc.kill()
        raise RuntimeError("server did not start")
    client.db_path = db
    client.uploads = tmp / "uploads"
    return proc, log, client


def _stop(proc, log):
    proc.send_signal(2)  # SIGINT: graceful shutdown
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()


@pytest.fixture(scope="module")
def server(tmp_path_factory):
    proc, log, client = _start(tmp_path_factory.mktemp("fss"), None)
    yield client
    _stop(proc, log)


@pytest.fixture(scope="module")
def demo(tmp_path_factory):
    proc, log, client = _start(tmp_path_factory.mktemp("fss-demo"), DEMO_SEED)
    yield client
    _stop(proc, log)


@pytest.fixture(scope="module")
def spawn(tmp_path_factory):
    """Starts demo-seeded servers with rule overrides: spawn(rules={...})."""
    running = []

    def _spawn(rules: dict | None = None, clock: bool = True, seed: Path | None = DEMO_SEED) -> Client:
        tmp = tmp_path_factory.mktemp("fss-spawn")
        proc, log, client = _start(tmp, seed, rules, clock)
        running.append((proc, log))
        return client

    yield _spawn
    for proc, log in running:
        _stop(proc, log)
