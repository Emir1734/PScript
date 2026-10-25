import json
import os
import sys

_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_default = os.path.join(_root, "artifacts", "benchmarks", "bench_full_verify_2026-10-22.json")
path = sys.argv[1] if len(sys.argv) > 1 else _default
with open(path, encoding="utf-8") as f:
    d = json.load(f)
fail = [r for r in d["results"] if r.get("median_ns", 0) >= 100_000_000]
fail.sort(key=lambda r: -r["median_ns"])
for r in fail[:20]:
    print(
        f"{r['median_ns'] / 1e6:.1f}ms  {r.get('category', '')}  {r.get('name', '')}  "
        f"tier={r.get('tier', '')} phase={r.get('phase', '')}"
    )
print("count", len(fail))
