"""Black-box API test fixtures (roadmap 10.4).

Each test module gets a fresh server process with its own temporary database.
`server` starts from an empty schema; `demo` additionally loads the synthetic
fixture data/seed/demo.sql. The binary defaults to server/build/fss; set
FSS_BIN to test another build (for example server/build/fss-debug).
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


def _start(tmp: Path, seed: Path | None):
    if not BIN.exists():
        pytest.skip(f"server binary not built: {BIN}")
    db = tmp / "test.db"
    common = ["--db", str(db), "--migrations", str(MIGRATIONS)]
    if seed:
        _run([*common, "--migrate-only"])
        _run([*common, "--seed", str(seed), "--materialize"])
    port = _free_port()
    log = open(tmp / "server.log", "wb")
    proc = subprocess.Popen(
        [str(BIN), *common, "--bind", f"127.0.0.1:{port}",
         "--public", str(tmp / "no-public"), "--threads", "2", "--quiet"],
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
