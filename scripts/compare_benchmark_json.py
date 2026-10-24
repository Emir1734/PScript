#!/usr/bin/env python3
"""Compare two PScript benchmark JSON reports by stable record keys."""

from __future__ import annotations

import argparse
import json
import sys
from typing import Any


def record_key(r: dict[str, Any]) -> str:
    return f"{r['name']}|{r['category']}|{r['tier']}|{r['phase']}"


def load(path: str) -> dict[str, Any]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def quality(r: dict[str, Any]) -> str:
    return r.get("measurement_quality", "unknown")


def classify_delta(old: dict[str, Any], new: dict[str, Any], delta_ns: int) -> str:
    q_old = quality(old)
    q_new = quality(new)
    if q_old in ("noisy", "insufficient_samples", "single_sample") or q_new in (
        "noisy",
        "insufficient_samples",
        "single_sample",
    ):
        return "inconclusive"
    sc_old = int(old.get("sample_count", 0))
    sc_new = int(new.get("sample_count", 0))
    if sc_old < 4 or sc_new < 4:
        return "inconclusive"
    iqr = max(int(old.get("iqr_ns", 0)), int(new.get("iqr_ns", 0)), 0)
    if abs(delta_ns) <= iqr * 2:
        return "inconclusive"
    if delta_ns < 0:
        return "improvement"
    if delta_ns > 0:
        return "regression"
    return "unchanged"


def pct_change(old_median: int, delta_ns: int) -> float | None:
    if old_median <= 0:
        return None
    return 100.0 * delta_ns / old_median


def env_warning(old_doc: dict[str, Any], new_doc: dict[str, Any]) -> list[str]:
    warnings: list[str] = []
    for key in ("configuration", "platform", "logical_processor_count"):
        if old_doc.get(key) != new_doc.get(key):
            warnings.append(f"environment mismatch: {key} old={old_doc.get(key)!r} new={new_doc.get(key)!r}")
    for label, doc in (("old", old_doc), ("new", new_doc)):
        env = doc.get("environment") or {}
        busy = env.get("host_cpu_busy_fraction_during_run")
        if busy is not None and float(busy) > 0.35:
            warnings.append(f"{label} run host CPU busy ~{float(busy) * 100:.0f}% (timing variance likely elevated)")
    if old_doc.get("warmup") != new_doc.get("warmup") or old_doc.get("iterations") != new_doc.get("iterations"):
        warnings.append(
            f"benchmark config differs: warmup/iterations "
            f"old={old_doc.get('warmup')}/{old_doc.get('iterations')} "
            f"new={new_doc.get('warmup')}/{new_doc.get('iterations')}"
        )
    return warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old_json")
    parser.add_argument("new_json")
    parser.add_argument("--filter", default="", help="name prefix filter")
    parser.add_argument("--min-delta-ms", type=float, default=0.0, help="only show |delta| >= this many ms")
    parser.add_argument("--top", type=int, default=0, help="limit output rows (0 = all)")
    args = parser.parse_args()

    old_doc = load(args.old_json)
    new_doc = load(args.new_json)
    old_map = {record_key(r): r for r in old_doc.get("results", [])}
    new_map = {record_key(r): r for r in new_doc.get("results", [])}

    keys = sorted(set(old_map) | set(new_map))
    rows: list[tuple[str, str, dict[str, Any], dict[str, Any], int, str]] = []

    for key in keys:
        name = key.split("|", 1)[0]
        if args.filter and not name.startswith(args.filter):
            continue
        old_r = old_map.get(key)
        new_r = new_map.get(key)
        if old_r is None or new_r is None:
            rows.append((key, "missing", old_r or {}, new_r or {}, 0, "inconclusive"))
            continue
        delta = int(new_r["median_ns"]) - int(old_r["median_ns"])
        if abs(delta) < int(args.min_delta_ms * 1_000_000):
            continue
        verdict = classify_delta(old_r, new_r, delta)
        rows.append((key, name, old_r, new_r, delta, verdict))

    rows.sort(key=lambda t: abs(t[4]), reverse=True)
    if args.top > 0:
        rows = rows[: args.top]

    print(f"old: {args.old_json} (benchmark_version={old_doc.get('benchmark_version')}, n={len(old_map)})")
    print(f"new: {args.new_json} (benchmark_version={new_doc.get('benchmark_version')}, n={len(new_map)})")
    for w in env_warning(old_doc, new_doc):
        print(f"WARNING: {w}")
    print()
    print(
        f"{'verdict':12} {'delta_ms':>10} {'pct':>8} {'old_med_ms':>12} {'new_med_ms':>12} "
        f"{'iqr_ms':>10} {'n':>5} name tier phase"
    )
    for key, _name, old_r, new_r, delta, verdict in rows:
        if verdict == "missing":
            print(f"{verdict:12} {'—':>10} {'—':>8} {'—':>12} {'—':>12} {'—':>10} {'—':>5} {key}")
            continue
        old_m = int(old_r["median_ns"])
        new_m = int(new_r["median_ns"])
        iqr = max(int(old_r.get("iqr_ns", 0)), int(new_r.get("iqr_ns", 0)))
        pct = pct_change(old_m, delta)
        pct_s = f"{pct:+.1f}" if pct is not None else "—"
        print(
            f"{verdict:12} {delta / 1e6:+.3f} {pct_s:>8} {old_m / 1e6:12.3f} {new_m / 1e6:12.3f} "
            f"{iqr / 1e6:10.3f} {int(new_r.get('sample_count', 0)):5d} "
            f"{new_r.get('name')} {new_r.get('tier')} {new_r.get('phase')}"
        )

    improvements = sum(1 for r in rows if r[5] == "improvement")
    regressions = sum(1 for r in rows if r[5] == "regression")
    inconclusive = sum(1 for r in rows if r[5] == "inconclusive")
    print()
    print(f"summary: improvement={improvements} regression={regressions} inconclusive={inconclusive} shown={len(rows)}")
    print("Note: timing deltas alone do not prove causation; treat noisy/inconclusive rows accordingly.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
