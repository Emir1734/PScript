# Design decisions

Short notes on choices that are easy to re-litigate. Language rules live in `docs/LANGUAGE_SPEC.md` and `docs/language/`.

---

## Execution pipeline

PScript compiles to **register bytecode** and runs on a **register VM** in C++20. There is no stack VM, LLVM, or AOT. An optional **native JIT** (baseline and optimizing tiers) may execute hot functions when enabled; observable behavior must match the VM (see `stabilization` tests and [Compatibility](../COMPATIBILITY.md)). Runtime values are C++ `Value` objects; variable *names* are resolved by the compiler to numeric slots.

---

## Scope (current)

Visibility is **lexical block scope**, implemented in the compiler (`Scope` / `Symbol`), not by hashing names at runtime.

- Assignment updates a name that is already visible, or introduces it in the current block if it is new.
- An explicit type declaration always allocates a new slot in the current block (shadowing).
- Duplicate declarations in one block are a compile error.
- Variables are not hoisted.

`SymbolKind::{Variable, Function, Class}` and `SymbolAccess::{Public, Private}` record what a name is. Function declarations (top-level and nested) are hoisted as `Function` symbols in their lexical scope; class declarations are pre-registered as `Class` symbols. Ordinary variables are still not hoisted. Member access modifiers (`public`, `protected`, `private`) apply to struct/class fields and are enforced at compile time, including inheritance-aware protected/private rules for classes.

---

## Functions Aşama 3

`func` / `return`, parameters as locals, `CALL` / `CALL_VALUE` / `RETURN` / local and upvalue ops, and per-call frames. Calls are expressions. Argument count for a static top-level call is checked at compile time; calls through values check at runtime.

**Automatic return** applies only to the last *top-level* statement of a function body, and only when that statement is an `ExprStmt`. Nested `func` is skipped for that rule. Control-flow bodies (`if` / loops / `match`) do not auto-return. Explicit `return` always wins. Otherwise the function yields `null`.

**Nested functions** compile to separate `FunctionInfo` entries. The enclosing frame stores a closure cell (`MAKE_CLOSURE`). Nested names are hoisted in that lexical scope.

**Capture** is by **binding**: each local is a heap `Cell` (`shared_ptr`). Closures copy those pointers. Mutation (`=`, `+=`, `++`) is visible to all aliases of the cell. Globals are not captured. Capture sets are computed at compile time from actual uses; unused locals are omitted. Each outer invocation allocates new cells, so two `make_counter()` results do not share state.

**Lifetime:** a local that is never captured dies with the CallFrame. A captured local lives until the last closure that holds it is gone.

Prefix type declarations (`int x`) require the type name and the variable on the **same line**, so a trailing identifier in a function is not parsed as `type name`.

Not decided yet: default/variadic parameters, function type annotations, overloading.

---

## Standard library — `DateTime` / `Duration` (Bölüm 4)

`DateTime` and `Duration` are native stdlib classes stored as `ValueKind::Class` with sidecars (`NativeDateTimeData`, `NativeDurationData`) on `ClassData`, following the same pattern as `FileHandle` and native collections.

- **Instant storage:** UTC microseconds since Unix epoch (`DateTime`); signed microseconds (`Duration`).
- **Field access:** `year` … `second` and `Duration.seconds` are populated on the class field vector for `GetField`; sidecars are authoritative for arithmetic and equality.
- **Operators:** `datetime ± duration`, `datetime - datetime`, comparisons, and unary `-duration` are implemented in the VM (`binary_arith`, `binary_compare`, `unary_minus`) and `Runtime::equals`, not via user-visible methods.
- **Clocks:** `time.monotonic()` uses `std::chrono::steady_clock`; wall-clock APIs use `system_clock` + `localtime` / `gmtime` (no timezone database).
- **Random:** process-global `std::mt19937_64`; not suitable for cryptography (deferred to a future `crypto` module).
- **Native arity:** selected native stdlib functions (e.g. `random.float`) validate their own argument counts at runtime; the VM skips strict `FunctionInfo.arity` checks for native module calls so 0- or 2-argument forms can share one export.

---

## Standard library — `Bytes` + Bölüm 5 (`json`, `re`, `encoding`)

`Bytes` is a first-class `ValueKind::Bytes` with `BytesData { vector<uint8_t> }` — not a `list<int>` or native class sidecar. This keeps future binary I/O, networking, and crypto APIs type-safe.

- **`type()`:** returns `"bytes"` via `Runtime::runtime_type_name` (and `kind_name`).
- **Indexing:** `b[i]` returns `int` 0..255; assignment is rejected.
- **Iteration:** `NativeIteratorKind::Bytes` yields each byte as `int`.
- **Equality:** byte-wise vector comparison in `Runtime::equals`.
- **`length()`:** method dispatch on `ValueKind::Bytes` (same pattern as strings/lists).

`json` uses an in-tree recursive parser/encoder (no external JSON library). `JsonParser` owns its input `std::string` (not `string_view`) to avoid dangling temporaries from file reads.

`re` uses `std::regex` (ECMAScript). `Match` is a field-bag native class (`found`, `text`, `start`, `end`, `groups`). `search` not-found returns `null`.

`encoding` UTF-16 convention: UTF-16LE with required BOM bytes `FF FE`. Invalid UTF-8/UTF-16/ASCII sequences raise `ValueException`; no silent replacement.

## Standard library — Bölüm 6 (`net`)

**Scope:** TCP connect/listen/accept, UDP send/receive, DNS `resolve`. HTTP is a separate module (Bölüm 7) and does not replace `Connection` or `bytes`.

**Bytes integration:** TCP/UDP payloads use real `ValueKind::Bytes` — no parallel network buffer type. TCP `read()` never returns `string`; EOF is **`NetworkException` (`connection closed`)**, not `null`.

**Sync/async API:** One function surface (`net.connect`, `connection.read`, …). When `Runtime::async_scheduler_active()`, native calls submit `AsyncIoService` work and return `Task`; otherwise they block on the calling thread. No `async_connect` family.

**Async I/O architecture:** Extended `AsyncIoService` with network `IoKind`s. File/stdin I/O keeps the original **single worker thread**; network operations run on **short-lived dedicated threads** so accept/connect cannot deadlock while preserving stdin cancellation behaviour.

**Cancellation / timeout:** Reuse `task.cancel()` → `CancellationException` and `await timeout(task, ms)` → `TimeoutException`. Connect operations honour cooperative cancel via non-blocking connect + poll; in-flight reads use blocking recv on worker threads (cancel during read closes the race at task cancel boundaries).

**Exceptions:** Added prelude `NetworkException` and `RuntimeErrorCategory::Network`.

**Platform:** Windows (`Winsock2`) and Linux (`socket`/`poll`/`getaddrinfo`) behind `src/runtime/net_ops.{h,cpp}`. Socket handles stored in `NativeNetworkData` on `ClassData::native_network`.

**Testing:** Localhost/`127.0.0.1`, ephemeral ports via `listen(..., 0)` / `udp().local_port()`, no external internet hosts. Tests avoid fixed ports (PID-scoped patterns where needed).

**Helper API:** `Server.local_port()` and `UdpSocket.local_port()` expose ephemeral bind ports for tests and production servers listening on port `0`.

## Standard library — Bölüm 7 (`http`)

**Client:** `get`, `post`, and `request` share one surface. When the scheduler is active they call `AsyncIoService::submit_http_request` and return a `Task`; otherwise they call `http_request_sync` on the caller thread. `submit_http_request` is an `HttpRequest` work item (dedicated thread, same completion path as `net`). Each request is connect → optional TLS → one HTTP/1.1 exchange → close. No connection pool.

**Response / request values:** `Response` and `Request` are native classes with fields. Body is `ValueKind::Bytes`. Headers are a `dict` of strings.

**Server deviation:** `http.server(port)` listens on `127.0.0.1`. `start()` spawns a private `std::thread` that calls `tcp_accept_sync`. That loop is **not** `submit_net_accept`. Handlers return to the VM through `Runtime::queue_callable_invoke` and `std::promise`; `http_server_poll_pending_handlers` writes the status line. V1 keeps this split and does not add a second public API. A later change may move accept onto `AsyncIoService` without renaming `get` / `post` / `start`.

**Exceptions:** Protocol, DNS, connect, and TLS failures use `RuntimeErrorCategory::Network` → `NetworkException`. Argument shape uses `TypeException` or `ValueException`. `task.cancel()` and `timeout()` are unchanged.

**Out of scope:** HTTP/2, WebSocket, chunked streaming, keep-alive client API, TLS server, certificate pinning, custom CA, raw `select`/`poll` in user code.

**Testing:** Same localhost / ephemeral-port rule as `net`. Client tests talk to `http.server(0)` inside `async func main()`.

---

## Match

`match` is a statement. Fallback is `_`, not `else`. Cases do not fall through.

---

## Test isolation (VM / compiler harness)

Filesystem and module VM tests use **per-process, per-run temporary directories** under the OS temp folder (`%TEMP%` / `std::filesystem::temp_directory_path()`), not the repository or build tree.

- Path pattern: `<prefix>-pid<PID>-run<N>` so concurrent `pscript_*_tests.exe` processes never share the same fixture directory.
- Filesystem tests set the process working directory to that directory for the duration of the test (`CwdScope` RAII) and restore the original cwd afterward. This prevents relative paths such as `log.txt` or `data/` from leaking into `build/` or colliding across runs.
- Module tests write fixture `.phs` files (including `main.phs`) into the isolated directory and run with that directory as cwd so compile-time module resolution and any runtime filesystem side effects stay inside the fixture.
- CTest sets `WORKING_DIRECTORY` to the CMake binary dir for all unit tests; harness isolation does not depend on where CTest is launched from.

Do not use fixed relative paths like `run-1` without a process id when adding new filesystem or module tests.
