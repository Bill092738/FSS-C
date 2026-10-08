"""M4: reputation rollup (roadmap 7.6) and the cheater simulator (10.4).

The M4 gate: the simulator shows that reputation suppresses cheaters.
"""

import sqlite3
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sim"))
import simulate  # noqa: E402

MIN = 60_000
LIBRARY = {"lat": 40.0000, "lon": -83.0150, "accuracy_m": 10}


def at(t):
    return {"X-FSS-Now": str(t)}


def test_rollup_judges_reports_against_consensus(spawn):
    srv = spawn()
    honest = [srv.new_client() for _ in range(5)]
    cheater = srv.new_client()
    ids = {}
    for c in honest + [cheater]:
        ids[c] = c.register()["id"]
    t = int(time.time() * 1000) - 2 * 3600_000
    for c in honest:
        assert c.post("/api/v1/spots/1/reports", {"level": 2, **LIBRARY}, headers=at(t)).status == 201
    cheater.post("/api/v1/spots/1/reports", {"level": 0, **LIBRARY}, headers=at(t))
    # a lone report elsewhere has no consensus and is not judged
    honest[0].post("/api/v1/spots/2/reports", {"level": 0, **LIBRARY}, headers=at(t))

    # the consensus window (30 min after the report) is not complete yet
    assert srv.run_job("hourly_rollup", t + 29 * MIN) == 0
    assert srv.run_job("hourly_rollup", t + 31 * MIN) == 6
    reps = {c: c.get("/api/v1/me").json()["user"]["reputation"] for c in honest + [cheater]}
    # honest: 4 peers at 2, one at 0 -> consensus 1.6, |2 - 1.6| <= 0.5: agreed
    assert all(reps[c] == pytest.approx(1.05) for c in honest)
    # cheater: consensus 2.0, |0 - 2| >= 1.5: rejected
    assert reps[cheater] == pytest.approx(0.9)
    # the cursor moved: nothing is judged twice
    assert srv.run_job("hourly_rollup", t + 90 * MIN) == 0
    db = sqlite3.connect(srv.db_path)
    rows = db.execute("SELECT reason, count(*) FROM rep_ledger GROUP BY reason ORDER BY reason").fetchall()
    db.close()
    assert rows == [("report_agreed", 5), ("report_rejected", 1)]


def test_reputation_is_clamped(spawn):
    srv = spawn(rules={"rep_reject": 0.5})
    honest = [srv.new_client() for _ in range(3)]
    cheater = srv.new_client()
    for c in honest + [cheater]:
        c.register()
    t = int(time.time() * 1000) - 5 * 3600_000
    for k in range(4):
        tk = t + k * 40 * MIN
        for c in honest:
            c.post("/api/v1/spots/1/reports", {"level": 2, **LIBRARY}, headers=at(tk))
        cheater.post("/api/v1/spots/1/reports", {"level": 0, **LIBRARY}, headers=at(tk))
    srv.run_job("hourly_rollup", t + 4 * 3600_000)
    assert cheater.get("/api/v1/me").json()["user"]["reputation"] == pytest.approx(0.1)


def test_simulator_suppresses_cheaters(spawn):
    srv = spawn()
    result = simulate.run(srv.base, honest=10, cheaters=4, hours=6, seed=7)
    print(simulate.format_result(result))
    assert result["rep_honest"] > 1.2
    assert result["rep_cheaters"] < 0.4
    # the reputation-weighted live estimate removes a good part of the error
    # the cheaters add: equal weights -> weighted, measured against the
    # honest-only floor (seeds 1-6 remove 28-64 %, see PROGRESS.md)
    damage = result["err_unweighted"] - result["err_honest_only"]
    removed = result["err_unweighted"] - result["err_weighted"]
    assert damage > 0.1 and removed / damage > 0.4
