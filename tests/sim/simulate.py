"""Crowd simulator (roadmap 10.4): virtual students report crowding, some of
them cheat, and the server's reputation rollup should push the cheaters'
influence down.

Model (deterministic for a given seed):
  * four spots in four buildings of the demo fixture; every hour each spot is
    either empty (0) or full (2), drawn at random;
  * every hour each student picks a spot; every 10 minutes they report with
    probability `p_report`, from inside the building's fence;
  * honest students report the truth (10% of the time "1" instead);
    cheaters always report the opposite extreme (2 - truth);
  * at the start of every hour the server's hourly_rollup job runs.

The server must run with --test-clock (it is driven through X-FSS-Now and
POST /api/v1/debug/jobs/hourly_rollup) and with a high auth_ip_per_min, since
every student registers from the same address. The tests start such a server;
by hand:

    echo '{"auth_ip_per_min": 1000}' > /tmp/sim-rules.json
    server/build/fss --db /tmp/sim.db --test-clock --rules /tmp/sim-rules.json &
    server/build/fss --db /tmp/sim.db --seed data/seed/demo.sql --materialize
    python3 tests/sim/simulate.py --base http://127.0.0.1:8080

Result: mean reputation of honest students and cheaters, and the mean absolute
error of the live estimate against the truth during the second half of the
run: the server's (reputation-weighted) estimate versus the same reports with
equal weights and versus the honest reports alone (the floor: the estimate lags
when the truth flips and leans on the forecast prior), both recomputed here
with the roadmap 7.3 formula.
"""

from __future__ import annotations

import argparse
import http.cookiejar
import json
import random
import time
import urllib.error
import urllib.request
import uuid

MIN = 60_000
HOUR = 60 * MIN
# spot id -> fence center of its building (demo fixture)
SPOTS = {
    1: (40.0000, -83.0150),  # Central Library
    4: (40.0030, -83.0120),  # Engineering Hall
    6: (39.9980, -83.0100),  # Student Union
    8: (40.0015, -83.0180),  # Science Building
}
# roadmap 7.3 defaults (server/config/rules.json)
HALF_LIFE, WINDOW, DEDUPE, W0, FORECAST = 20 * MIN, 90 * MIN, 10 * MIN, 0.5, 1.0


class Student:
    def __init__(self, base: str, cheater: bool):
        self.base = base
        self.cheater = cheater
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.id = None

    def call(self, method: str, path: str, body=None, now: int | None = None) -> dict:
        headers = {"Content-Type": "application/json"}
        if now is not None:
            headers["X-FSS-Now"] = str(now)
        req = urllib.request.Request(self.base + path, method=method, headers=headers,
                                     data=None if body is None else json.dumps(body).encode())
        try:
            with self.opener.open(req, timeout=10) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"{method} {path}: {e.code} {e.read()!r}") from None

    def register(self):
        email = f"sim-{uuid.uuid4().hex[:12]}@example.edu"
        user = self.call("POST", "/api/v1/auth/register",
                         {"email": email, "password": "simulated student"})["user"]
        self.id = user["id"]

    def reputation(self) -> float:
        return self.call("GET", "/api/v1/me")["user"]["reputation"]


def estimate(reports: list[tuple[int, int, float, int]], now: int) -> float:
    """Roadmap 7.3 with every reporter at the same weight.
    reports: (user, level, weight, at) for one spot."""
    live = [r for r in reports if now - WINDOW <= r[3] <= now]
    counted = [r for r in live
               if not any(n[0] == r[0] and r[3] < n[3] <= min(now, r[3] + DEDUPE) for n in live)]
    num, den = W0 * FORECAST, W0
    for _, level, weight, t in counted:
        w = weight * 2 ** (-(now - t) / HALF_LIFE)
        num += w * level
        den += w
    return num / den


def run(base: str, honest: int = 10, cheaters: int = 4, hours: int = 6,
        seed: int = 1, p_report: float = 0.6, start: int | None = None) -> dict:
    rng = random.Random(seed)
    students = [Student(base, False) for _ in range(honest)] + \
               [Student(base, True) for _ in range(cheaters)]
    for s in students:
        s.register()
    cheater_ids = {s.id for s in students if s.cheater}
    t0 = start or int(time.time() * 1000) - (hours + 1) * HOUR
    runner = students[0]
    history: dict[int, list] = {spot: [] for spot in SPOTS}
    err_w, err_u, err_h, n_reports = [], [], [], 0

    for hour in range(hours):
        th = t0 + hour * HOUR
        runner.call("POST", "/api/v1/debug/jobs/hourly_rollup", now=th)
        truth = {spot: rng.choice((0, 2)) for spot in SPOTS}
        where = {s: rng.choice(list(SPOTS)) for s in students}
        for step in range(6):
            t = th + step * 10 * MIN + 1
            last_live: dict[int, float] = {}
            for i, s in enumerate(students):
                if rng.random() >= p_report:
                    continue
                spot = where[s]
                lat, lon = SPOTS[spot]
                if s.cheater:
                    level = 2 - truth[spot]
                else:
                    level = 1 if rng.random() < 0.1 else truth[spot]
                r = s.call("POST", f"/api/v1/spots/{spot}/reports",
                           {"level": level, "lat": lat, "lon": lon, "accuracy_m": 10},
                           now=t + i)
                n_reports += 1
                history[spot].append((s.id, level, r["report"]["fence"], t + i))
                last_live[spot] = (r["live"]["est"], t + i)
            if hour >= hours // 2:
                for spot, (est, tr) in last_live.items():
                    err_w.append(abs(est - truth[spot]))
                    err_u.append(abs(estimate(history[spot], tr) - truth[spot]))
                    honest_only = [r for r in history[spot] if r[0] not in cheater_ids]
                    err_h.append(abs(estimate(honest_only, tr) - truth[spot]))
    runner.call("POST", "/api/v1/debug/jobs/hourly_rollup", now=t0 + hours * HOUR + 31 * MIN)

    reps = {s: s.reputation() for s in students}
    mean = lambda xs: sum(xs) / len(xs) if xs else float("nan")  # noqa: E731
    return {
        "honest": honest, "cheaters": cheaters, "hours": hours, "reports": n_reports,
        "rep_honest": mean([r for s, r in reps.items() if not s.cheater]),
        "rep_cheaters": mean([r for s, r in reps.items() if s.cheater]),
        "err_weighted": mean(err_w),
        "err_unweighted": mean(err_u),
        "err_honest_only": mean(err_h),
        "samples": len(err_w),
    }


def format_result(r: dict) -> str:
    return (f"{r['honest']} honest + {r['cheaters']} cheaters, {r['hours']} h, "
            f"{r['reports']} reports\n"
            f"  mean reputation: honest {r['rep_honest']:.2f}, cheaters {r['rep_cheaters']:.2f}\n"
            f"  live estimate error (second half, {r['samples']} samples): "
            f"reputation-weighted {r['err_weighted']:.3f}, equal weights {r['err_unweighted']:.3f},"
            f" honest reports only {r['err_honest_only']:.3f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", default="http://127.0.0.1:8080")
    ap.add_argument("--honest", type=int, default=10)
    ap.add_argument("--cheaters", type=int, default=4)
    ap.add_argument("--hours", type=int, default=6)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    print(format_result(run(a.base, a.honest, a.cheaters, a.hours, a.seed)))


if __name__ == "__main__":
    main()
