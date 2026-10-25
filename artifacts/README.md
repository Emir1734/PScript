# Local artifacts (not shipped)

This directory holds **developer-local** outputs moved out of the repository root: benchmark JSON, run logs, performance reports, and scratch files.

Nothing under `benchmarks/`, `reports/`, or `logs/` is committed. Regenerate with the usual benchmark or CI scripts; defaults now write under `artifacts/benchmarks/` unless you pass explicit paths.

To sweep stray root-level bench/log files into here:

```powershell
./scripts/archive_root_artifacts.ps1
```
