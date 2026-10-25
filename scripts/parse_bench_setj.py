import json
import sys
from collections import defaultdict

_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_default = os.path.join(_root, "artifacts", "benchmarks", "bench_baseline_setJ_2026-10-18.json")
p = sys.argv[1] if len(sys.argv) > 1 else _default
with open(p, "r", encoding="utf-8") as f:
    data = json.load(f)
recs = data["results"]

def ms(r):
    return r["median_ns"] / 1e6

gates = ["loops", "num_lu_divmod", "closure_call", "closure_nested", "closure_recursive"]
print("SET_J gates warm_steady (ms):")
for g in gates:
    name = "jit.exec." + g
    rows = [r for r in recs if r["name"] == name and r["phase"] == "warm_steady"]
    print(f"\n{g}:")
    for r in sorted(rows, key=lambda x: x["tier"]):
        print(
            f"  {r['tier']:16} med={ms(r):.4f} min={r['min_ns']/1e6:.4f} "
            f"max={r['max_ns']/1e6:.4f} n={r['sample_count']}"
        )

print("\n--- compiler (steady, ms, n=7) ---")
for r in recs:
    if r["category"] == "compiler" and r["phase"] == "steady":
        print(f"{r['name']:30} {r['tier']:10} med={ms(r):.4f}")

print("\n--- jit.compile ---")
for r in recs:
    if r["name"].startswith("jit.compile"):
        print(f"{r['name']} {r['tier']} {r['phase']} med={ms(r):.4f} n={r['sample_count']}")

print("\n--- selected steady jit.exec (no warm_steady) vm/baseline/opt ---")
pick = [
    "branching",
    "callvalue_mono",
    "callvalue_poly",
    "closure_create",
    "closure_mutable",
    "closure_shared",
    "recursive_sum",
    "dict_lu_str_hit",
    "field_access",
]
for g in pick:
    rows = [r for r in recs if r["name"] == "jit.exec." + g and r["phase"] == "steady"]
    if not rows:
        continue
    parts = ", ".join(f"{r['tier']}={ms(r):.3f}" for r in sorted(rows, key=lambda x: x["tier"]))
    print(f"{g}: {parts}")

ws = defaultdict(dict)
for r in recs:
    if not r["name"].startswith("jit.exec."):
        continue
    if r["phase"] != "warm_steady":
        continue
    slug = r["name"][len("jit.exec.") :]
    ws[slug][r["tier"]] = ms(r)
better = worse = tie = 0
for slug, t in ws.items():
    if "vm" not in t or "optimizing_jit" not in t:
        continue
    if t["optimizing_jit"] < t["vm"] * 0.99:
        better += 1
    elif t["optimizing_jit"] > t["vm"] * 1.01:
        worse += 1
    else:
        tie += 1
print(f"\nwarm_steady slugs={len(ws)} opt faster than vm: {better}, slower: {worse}, ~tie: {tie}")
