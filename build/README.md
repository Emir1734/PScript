# Local CMake build trees

All out-of-source builds live under this directory. The default CI and day-to-day tree is **`main`**:

```text
cmake -S . -B build/main
cmake --build build/main --config Release
```

Other named trees (for example `release`, `debug`, `jit-final5`, `verify-jit`) are preserved when consolidating experiments; each subdirectory is a separate CMake binary directory.

Generated contents under `build/` are not committed except this README.
