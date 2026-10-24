# Standard library

PScript ships with a built-in standard library. Stdlib modules are always available without publishing a package. Application dependencies use the project package manager (`pscript.toml` / `pscript install`); see [Package & Ecosystem](../package-ecosystem.md).

Standard library modules resolve when no user `.phs` file exists for the same specifier. A local `math.phs` next to your program still takes precedence over `import math`.

## Import syntax

```phs
import math
import math as m
import string
import collections
import path
import fs
import io

from math import sqrt, pi
from string import join
from collections import Stack, Queue, Deque
from path import join
from fs import read, write
from io import open
import time
import datetime
import random
import json
import re
import encoding
import log
import crypto
import sql
import archive
```

---

## Bölüm 1

### Global built-ins

| Function | Semantics |
|----------|-----------|
| `abs(x)` | Absolute value. `int` → `int`, `float` → `float`. |
| `min(a, b, ...)` | Minimum of two or more numeric values. Returns `int` if all inputs are `int`, otherwise `float`. |
| `max(a, b, ...)` | Maximum of two or more numeric values. Same return-type rule as `min`. |
| `round(x)` | Rounds to the nearest integer (half away from zero). `int` input returns unchanged `int`. `float` input returns `float` when the rounded value is not exactly representable as `int`. |
| `clamp(value, min, max)` | Clamps `value` to `[min, max]`. Raises a value error when `min > max`. |

### `math` module

Constants: `math.pi`, `math.e` (floating-point).

Functions: `sqrt`, `pow`, `floor`, `ceil`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `log`, `log10`, `exp`.

- Accept `int` and/or `float` using normal numeric conversion rules.
- Domain errors (e.g. `sqrt(-1)`, `log(0)`) raise value errors through the existing exception system.
- NaN and infinity are not part of the PScript number model.

### String methods and `string` module

Strings remain a primitive type. Methods:

| Method | Returns | Notes |
|--------|---------|-------|
| `length()` | `int` | Byte length (UTF-8 bytes), not Unicode code-point count. |
| `trim()` | `string` | Removes leading/trailing ASCII whitespace. |
| `lower()` / `upper()` | `string` | ASCII case conversion. |
| `contains(s)` | `bool` | Substring search. |
| `starts_with(s)` / `ends_with(s)` | `bool` | Prefix/suffix test. |
| `replace(from, to)` | `string` | Literal substring replacement (all occurrences). |
| `split(separator)` | `list` | Split by non-empty substring separator. |

Module helper:

```phs
import string
string.join(["a", "b", "c"], ",")   # "a,b,c"
```

### `collections` module

Generic containers (use `from collections import …` for typed annotations):

| Type | Operations |
|------|------------|
| `Stack<T>` | `push`, `pop`, `peek`, `is_empty`, `length` |
| `Queue<T>` | `enqueue`, `dequeue`, `peek`, `is_empty`, `length` |
| `Deque<T>` | `push_front`, `push_back`, `pop_front`, `pop_back`, `front`, `back`, `is_empty`, `length` |

```phs
from collections import Stack
Stack<int> s = Stack()
s.push(1)
print(s.pop())
```

Empty `pop` / `peek` / `front` / `back` raise value errors. Generic element types are checked at compile time for mutating methods when the collection variable has a typed annotation (e.g. `Stack<int>`).

---

## Bölüm 2: `path`, `fs`, `io`

Three modules with separate responsibilities:

| Module | Role |
|--------|------|
| `path` | Path manipulation only — never touches the filesystem |
| `fs` | Convenient one-shot filesystem operations |
| `io` | Open file handles and stream read/write |

Sync APIs are **synchronous**. Async helpers (`async_read`, `async_write`, `async_append`, handle `async_*` methods) return **`Task`** values and require `await` inside `async func`. Blocking work runs on a dedicated I/O worker thread; the scheduler remains cooperative on the main VM thread.

### Text encoding

- `fs.read`, `fs.write`, `fs.append`, and `io` text operations use **UTF-8 bytes** stored in PScript strings.
- No platform-specific locale encoding is applied.
- Binary `read_bytes` / `write_bytes` APIs are not included in this release.

### `path` module

Pure path operations. **Does not** open, create, delete, list, or stat files.

| Function | Returns | Semantics |
|----------|---------|-----------|
| `join(a, b)` | `string` | Join two path segments. Uses `/` in returned paths (`generic_string`). |
| `basename(path)` | `string` | Final path component. |
| `dirname(path)` | `string` | Parent path (`""` when none). |
| `extension(path)` | `string` | Extension including dot (e.g. `.gz` for `archive.tar.gz`). |
| `stem(path)` | `string` | Filename without final extension (e.g. `archive.tar`). |
| `normalize(path)` | `string` | **Lexical** normalization only (`.`, `..`, duplicate separators). No filesystem access. |
| `absolute(path)` | `string` | Makes a path absolute using the process current working directory. |
| `resolve(path)` | `string` | `absolute` + lexical normalization. **Does not** follow symlinks. |
| `is_absolute(path)` | `bool` | Whether the path is absolute on the current platform. |
| `is_relative(path)` | `bool` | `not is_absolute(path)`. |
| `relative(target, base)` | `string` | Relative path from `base` to `target`. Returns `"."` when equal. Errors when roots differ (e.g. different Windows drive letters). |

**Security note:** `normalize()` and `resolve()` are **not** sandbox boundaries. They do not validate paths against an allowed directory.

### `fs` module

| Function | Returns | Semantics |
|----------|---------|-----------|
| `exists(path)` | `bool` | `false` when not found; propagates non-not-found OS errors. |
| `is_file(path)` | `bool` | Regular file test. |
| `is_dir(path)` | `bool` | Directory test. |
| `read(path)` | `string` | Read entire file as UTF-8 text. Errors when missing, not a file, or unreadable. |
| `write(path, content)` | `null` | Create/overwrite file. Errors on directory path or missing parent directory. |
| `append(path, content)` | `null` | Append UTF-8 text; creates file if needed. |
| `copy(source, dest)` | `null` | Copy file; overwrites destination. |
| `move(source, dest)` | `null` | Rename/move. |
| `remove(path)` | `null` | Delete a **file** (not a directory). |
| `create_dir(path)` | `null` | Create one directory. No-op if directory already exists. Errors when parent is missing or path is a file. |
| `remove_dir(path)` | `null` | Remove an **empty** directory. |
| `list_dir(path)` | `list` | Sorted list of entry names (not full paths). |

**Async helpers (each returns `Task`):**

| Function | Await result |
|----------|--------------|
| `async_read(path)` | `string` |
| `async_write(path, content)` | `null` |
| `async_append(path, content)` | `null` |

Errors use the existing runtime exception categories (`RuntimeException`, etc.) with messages that include the operation and path where possible.

### `io` module

```phs
import io

f = io.open("notes.txt", "w")
f.write("hello")
f.flush()
f.close()
```

#### `io.open(path, mode)`

Supported modes: `"r"`, `"w"`, `"a"`, `"r+"`, `"w+"`, `"a+"`.

Returns a native file-handle object with methods:

| Method | Semantics |
|--------|-----------|
| `read()` | Read remaining bytes as UTF-8 string (read-capable modes). |
| `write(text)` | Write UTF-8 string (write-capable modes). |
| `async_read()` | Returns `Task` → `string` (async; same errors as `read`). |
| `async_write(text)` | Returns `Task` → `null` (async; same errors as `write`). |
| `flush()` | Flush buffers. |
| `close()` | Close handle; idempotent second `close()` is safe. |
| `is_open()` | `bool` — `false` after `close()`. |

Use-after-close raises a runtime error. Call `close()` explicitly; do not rely on garbage collection for timely OS handle release.

`FileHandle` cannot be constructed directly; use `io.open`.

#### Integration example

```phs
import path
import fs

p = path.join("data", "save.txt")
if fs.exists(p):
    text = fs.read(p)
```

---

## Bölüm 3: `os`, `process`, `sys`

Three modules for operating-system integration, subprocess execution, and runtime metadata.

| Module | Role |
|--------|------|
| `os` | OS information, environment variables, working directory, home/temp paths |
| `process` | Process ID, synchronous subprocess execution, process exit |
| `sys` | PScript runtime version and CLI arguments |

All APIs in this section are **synchronous**.

### `os` module

| Function | Returns | Semantics |
|----------|---------|-----------|
| `name()` | `string` | Human-readable OS family (`Windows`, `Linux`, …) |
| `platform()` | `string` | Documented platform id (`win32`, `linux`, …) |
| `arch()` | `string` | Process architecture (`x86_64`, `arm64`, `x86`, …) |
| `cwd()` | `string` | Current working directory |
| `chdir(path)` | `null` | Changes process working directory; errors on failure |
| `env()` | `map<string,string>` | Snapshot of environment variables |
| `set_env(name, value)` | `null` | Creates or updates a variable |
| `remove_env(name)` | `null` | Removes a variable (no-op if missing) |
| `has_env(name)` | `bool` | Whether a variable exists |
| `home_dir()` | `string` | User home directory |
| `temp_dir()` | `string` | System temporary directory |

`os.cwd()` returns paths; it does not manipulate strings via `path.*`. Use `path` for joining/normalization.

### `process` module

| Function | Returns | Semantics |
|----------|---------|-----------|
| `id()` | `int` | Current process ID |
| `exit(code)` | — | Terminates the process with integer exit code (does not return) |
| `run(command)` | object | Runs `command` synchronously via platform shell (`cmd /c` on Windows, `sh -c` on Linux) |

`process.run()` result fields:

| Field | Type | Semantics |
|-------|------|-----------|
| `exit_code` | `int` | Child process exit code |
| `stdout` | `string` | Captured standard output (UTF-8 bytes) |
| `stderr` | `string` | Captured standard error (UTF-8 bytes) |

Future (not implemented): `spawn()`, `wait()`, `kill()`.

### `sys` module

| Name | Type | Semantics |
|------|------|-----------|
| `sys.args` | `list<string>` | CLI arguments **after** the script path |
| `sys.version` | `string` | PScript version from build metadata (matches `pscript --version`, e.g. `1.0.0`) |

Example:

```phs
# pscript main.phs hello world
import sys
print(sys.args)      # [hello, world]
print(sys.version)   # 1.0.0
```

---

## Bölüm 4: `time`, `datetime`, `random`

Three modules for wall-clock time, calendar values, and non-cryptographic randomness.

| Module | Role |
|--------|------|
| `time` | Wall-clock timestamps, monotonic measurement, synchronous sleep |
| `datetime` | `DateTime` / `Duration` values, parsing, formatting |
| `random` | Deterministic pseudo-random numbers (not cryptographically secure) |

### Sync sleep vs async `delay()`

| API | Behavior |
|-----|----------|
| `time.sleep(seconds)` | **Synchronous blocking** sleep in the current thread |
| `await delay(duration)` | **Async cooperative** wait via the scheduler |

`delay(duration)` returns a `Task`. Use **`await delay(...)`** inside `async func` for intentional waits. A bare `delay(...)` expression statement is a compile error. There is no global `wait()`; use `wait_all`, `wait_any`, or `TaskGroup.wait()`.

Duration literals require a unit suffix: **`ms`**, **`s`**, **`m`**, or **`h`**. Examples:

```phs
await delay(500ms)
await delay(1.5s)
await delay(2m)
await delay(1h)
```

Fractional values are supported (`await delay(0.5s)`). Durations normalize to whole milliseconds (half away from zero). Bare integers such as `delay(10)` are invalid.

These are intentionally separate from `time.sleep()`. `time.sleep()` must not suspend async tasks cooperatively.

### `time` module

| Function | Returns | Semantics |
|----------|---------|-----------|
| `now()` | `DateTime` | Current local wall-clock time (same value type as `datetime.now()`) |
| `unix()` | `float` | Seconds since Unix epoch (1970-01-01 00:00:00 UTC), fractional |
| `monotonic()` | `float` | Monotonic clock seconds (`steady_clock`); absolute value is not meaningful — use differences |
| `sleep(seconds)` | `null` | Blocking sleep; accepts fractional seconds; negative values raise `ValueException` |

### `datetime` module

Factory functions:

| Function | Returns | Semantics |
|----------|---------|-----------|
| `now()` | `DateTime` | Current local wall-clock time |
| `today()` | `DateTime` | Local date at midnight (`00:00:00`) |
| `utc_now()` | `DateTime` | Current UTC wall-clock time |
| `from_unix(timestamp)` | `DateTime` | Build from Unix timestamp (`int` or `float`, fractional allowed) |
| `create(y, m, d, h, min, s)` | `DateTime` | Local wall-clock components; validated (leap years supported) |
| `parse(text)` | `DateTime` | Parse deterministic format `YYYY-MM-DD HH:MM:SS` as local time |

`DateTime` fields (read-only via member access): `year`, `month`, `day`, `hour`, `minute`, `second`.

`DateTime.format()` returns the default formatted string (`YYYY-MM-DD HH:MM:SS`). An optional pattern argument supports `%Y`, `%m`, `%d`, `%H`, `%M`, `%S` tokens.

#### `Duration`

Subtracting two `DateTime` values yields a `Duration`:

```phs
import datetime
a = datetime.create(2026, 1, 2, 0, 0, 0)
b = datetime.create(2026, 1, 1, 0, 0, 0)
d = a - b
print(d.seconds)   # 86400
```

Supported operators:

| Expression | Result |
|------------|--------|
| `datetime2 - datetime1` | `Duration` (may be negative) |
| `datetime + duration` | `DateTime` |
| `datetime - duration` | `DateTime` |
| `datetime == != < <= > >=` | `bool` |
| `duration == != < <= > >=` | `bool` |
| `-duration` | `Duration` |

`Duration.seconds` is a floating-point value with microsecond internal precision.

#### Timezone limitations (v1)

- Local time and UTC only
- No IANA timezone database, `offset`, or DST modeling
- `create()` / `parse()` / `now()` / `today()` use the process local timezone
- `utc_now()` / `from_unix()` expose UTC field breakdown

#### Precision

- Internal representation: UTC microseconds since Unix epoch (`DateTime`), signed microseconds (`Duration`)
- User-facing seconds are floating-point where fractional values are exposed (`time.unix()`, `Duration.seconds`)

### `random` module

Non-cryptographic PRNG using C++ `<random>` (`std::mt19937_64`). **Not suitable for security** — use a future `crypto` module for secure randomness.

| Function | Semantics |
|----------|-----------|
| `int(min, max)` | Inclusive integer range |
| `float()` | `[0.0, 1.0)` |
| `float(min, max)` | `[min, max)` half-open float range |
| `choice(items)` | Random element from list/tuple; empty sequence raises `ValueException` |
| `shuffle(list)` | In-place Fisher–Yates shuffle of a mutable list; returns `null` |
| `seed(value)` | Reseed global generator (`int`, `float`, or `string`); returns `null` |

`type(datetime_value)` returns `"datetime"`. `type(duration_value)` returns `"duration"`.

---

## Bölüm 5: `json`, `re`, `encoding`

Three modules for JSON serialization, regular expressions, and text/binary encoding.

| Module | Role |
|--------|------|
| `json` | Encode/decode JSON; convenience file read/write |
| `re` | Regular-expression search, replace, and split |
| `encoding` | Convert between `string` and `bytes` (`utf8`, `utf16`, `ascii`) |

PScript has no named-argument syntax. Optional flags use **positional booleans** (e.g. `json.encode(data, true)` for pretty output).

### `json` module

| Function | Returns | Semantics |
|----------|---------|-----------|
| `encode(value)` | `string` | Compact JSON (default) |
| `encode(value, pretty)` | `string` | When `pretty` is `true`, indented human-readable JSON |
| `decode(text)` | value | Parse JSON text into PScript values |
| `read(path)` | value | Read UTF-8 JSON file and decode |
| `write(path, value)` | `null` | Write compact JSON to a UTF-8 file |
| `write(path, value, pretty)` | `null` | Optional pretty output (second flag positional) |

JSON mapping:

| JSON | PScript |
|------|---------|
| object | `map<string, any>` (`dict`) |
| array | `list<any>` |
| string | `string` |
| number (integer) | `int` |
| number (fractional) | `float` |
| boolean | `bool` |
| null | `null` |

Tuples encode as JSON arrays. Class, struct, enum, `bytes`, `set`, `range`, functions, modules, tasks, and other non-JSON values raise `ValueException` on encode. Dictionary keys must already be strings (existing PScript map semantics).

Invalid JSON raises `ValueException` with a deterministic message (`invalid JSON`, etc.). Filesystem errors from `read` / `write` use the existing runtime/filesystem exception paths.

### `re` module

Engine: C++ `<regex>` (ECMAScript grammar). Invalid patterns raise `ValueException`.

| Function | Returns | Semantics |
|----------|---------|-----------|
| `is_match(pattern, text)` | `bool` | **`std::regex_match` semantics: the pattern must match the entire input string** |
| `search(pattern, text)` | `Match` or `null` | First match anywhere in the text; `null` when not found |
| `find_all(pattern, text)` | `list<Match>` | All non-overlapping matches |
| `replace(pattern, replacement, text)` | `string` | Replace all matches |
| `replace(pattern, replacement, text, count)` | `string` | Replace at most `count` matches (`count=0` replaces nothing; negative count raises `ValueException`) |
| `split(pattern, text)` | `list<string>` | Split on regex matches (empty segments preserved) |

`Match` fields (native class instance):

| Field | Type | Semantics |
|-------|------|-----------|
| `found` | `bool` | Always `true` for returned matches |
| `text` | `string` | Matched substring |
| `start` | `int` | Start index (0-based, inclusive) |
| `end` | `int` | End index (0-based, exclusive) |
| `groups` | `list<string>` | Capture groups 1..n (empty string for non-participating groups) |

### `Bytes` type

Introduced by `encoding.encode()`. A first-class runtime value — **not** a `list<int>`.

| Property | Semantics |
|----------|-----------|
| `type(x)` | `"bytes"` |
| `length()` | Number of bytes |
| Indexing `b[i]` | `int` in `0..255` |
| Equality | Byte-wise (`==`, `!=`) |
| Iteration | Yields each byte as `int` (iterator protocol) |
| Mutability | Immutable (index assignment raises) |

Used by `net` and `http` payloads. Current `fs.read()` / `io` remain string-oriented. Designed for future binary `io`, `fs`, and `crypto` APIs.

### `encoding` module

| Function | Returns | Semantics |
|----------|---------|-----------|
| `encode(text)` | `bytes` | UTF-8 (default) |
| `encode(text, name)` | `bytes` | Named encoding: `utf8`, `utf-8`, `utf16`, `utf-16`, `ascii` |
| `decode(data)` | `string` | UTF-8 (default) |
| `decode(data, name)` | `string` | Named decoding |

Encoding rules:

| Encoding | Behavior |
|----------|----------|
| **UTF-8** | Full Unicode; invalid sequences raise `ValueException` on decode |
| **UTF-16** | UTF-16LE with required BOM `0xFF 0xFE` (deterministic, not host-endian) |
| **ASCII** | Code points U+0000..U+007F only; encode/decode errors raise `ValueException` |

Not included in this section: Base64, hex, URL encoding, JSON encoding (use `json`), cryptographic encoding (future `crypto` module).

---

## Bölüm 6: `net`

TCP, UDP, and DNS foundation for localhost-first networking. HTTP is Bölüm 7 and does not replace `Connection` or `bytes`.

```phs
import net

connection = net.connect("example.com", 80)
connection.write("hello")
data = connection.read()   # bytes
connection.close()
```

### Module functions

| Function | Returns | Notes |
|----------|---------|-------|
| `connect(host, port)` | `Connection` | Sync blocks; in async context returns `Task` → `await net.connect(...)` |
| `listen(host, port)` | `Server` | Sync blocking listen/accept path; port `0` selects an ephemeral port |
| `udp()` | `UdpSocket` | Binds to an ephemeral local port |
| `resolve(host)` | `list<string>` | IPv4/IPv6 addresses; duplicates removed |

Port must be `0..65535` (`ValueException` otherwise). Host must be non-empty.

### `Connection`

| Method | Returns | Notes |
|--------|---------|-------|
| `read()` | `bytes` | Blocking sync read; async via `await`. **EOF → `NetworkException` (`connection closed`)**, not `null` |
| `write(data)` | `int` | `string` (UTF-8) or `bytes`; bytes written |
| `close()` | `null` | Idempotent |
| `is_open()` | `bool` | |

### `Server`

| Method | Returns |
|--------|---------|
| `accept()` | `Connection` |
| `close()` | `null` |
| `is_open()` | `bool` |
| `local_port()` | `int` | Assigned port when listening on `0` |

### `UdpSocket`

| Method | Returns |
|--------|---------|
| `send(data, host, port)` | `int` |
| `receive()` | `Packet` |
| `close()` | `null` |
| `is_open()` | `bool` |
| `local_port()` | `int` |

### `Packet` (UDP receive result)

| Field | Type |
|-------|------|
| `data` | `bytes` |
| `host` | `string` |
| `port` | `int` |

### Sync vs async

Inside an active async scheduler (`async func main()`):

```phs
connection = await net.connect("127.0.0.1", 8080)
data = await connection.read()
await connection.write("ping")
client = await server.accept()
```

Outside async code, the same calls block the calling thread. There is **no** separate `async_connect` API family.

Async network work runs on **per-operation I/O threads**; completions resume tasks on the scheduler thread. Use existing `task.cancel()` and `await timeout(task, ms)` — no networking-specific timeout API.

### Exceptions

`NetworkException` (prelude, subclass of `RuntimeException`) covers DNS failures, connect/listen/accept errors, closed sockets, read/write failures, and UDP errors. Cooperative cancellation still surfaces as `CancellationException`. HTTP client and server failures use the same `NetworkException`.

---

## Bölüm 7: `http`

HTTP/1.1 client and a localhost server on top of `net`. There is no separate `async_get` family: inside `async func main()`, `get` / `post` / `request` return a `Task`.

```phs
import http

response = http.get("http://127.0.0.1:8080/")
print(response.status)
```

### Client

| Function | Returns | Notes |
|----------|---------|-------|
| `get(url)` | `Response` | Sync blocks; in async context `await http.get(url)` |
| `post(url, body)` | `Response` | `body` is `string` (UTF-8 bytes) or `bytes` |
| `request(method, url, body, headers)` | `Response` | `body` may be `null`; `headers` is `dict` of `string` |

Each call opens one connection, sends one HTTP/1.1 request, reads one response, then closes the socket. URLs must start with `http://` or `https://`. Redirects stop after 10. `https://` uses the built-in TLS client; if TLS is not compiled in, the call raises `NetworkException`.

### `Response`

| Field | Type |
|-------|------|
| `status` | `int` |
| `headers` | `dict` |
| `body` | `bytes` |

### Server

`server(port)` returns `HttpServer`. Port `0` binds an ephemeral port (`local_port()`). The listener is `127.0.0.1` only. One request per connection; no keep-alive API.

| Method | Notes |
|--------|-------|
| `get(path, handler)` | Exact path match, method `GET` |
| `post(path, handler)` | Exact path match, method `POST` |
| `start()` | Requires `async func main()`. Begins accepting |
| `close()` | Idempotent |
| `is_open()` | `bool` |
| `local_port()` | `int` |

The handler receives a `Request` and returns `string` or `bytes` (status 200). No matching route is 404.

### `Request`

| Field | Type |
|-------|------|
| `method` | `string` |
| `path` | `string` |
| `headers` | `dict` |
| `body` | `bytes` |

```phs
import http

async func main():
    server = http.server(0)
    func ping(req):
        return "pong"
    server.get("/ping", ping)
    server.start()
    url = "http://127.0.0.1:" + str(server.local_port()) + "/ping"
    response = await http.get(url)
    print(response.status)
    server.close()
```

Client work (`get` / `post` / `request`) goes through `AsyncIoService` (`submit_http_request` → `http_request_sync`). The accept loop on `HttpServer.start()` runs on a dedicated thread outside that service and posts handler calls back to the scheduler. That split is intentional for V1; the public API does not expose it.

`task.cancel()` and `await timeout(task, ms)` apply to client tasks. There is no HTTP-specific timeout function. Protocol and socket failures are `NetworkException`. Bad argument shapes are `TypeException` or `ValueException`.

### Not in this section (HTTP)

HTTP/2, WebSocket, chunked streaming, connection pooling, keep-alive as a user-facing API, TLS server, certificate pinning, and a custom CA store.

---

## Bölüm 7: `log`

Structured logging to **stderr** (program output via `print()` stays on **stdout**).

### Import

```phs
import log
```

### Levels

| Constant | Meaning |
|----------|---------|
| `log.DEBUG` | Verbose diagnostic |
| `log.INFO` | Normal operational messages (default minimum level) |
| `log.WARNING` | Recoverable problems |
| `log.ERROR` | Failures |

Functions: `log.debug(message, data?)`, `log.info(...)`, `log.warning(...)`, `log.error(...)`.

- `message` must be a `string`.
- Optional `data` must be a `dict` of structured fields (not a separate interpolation syntax).

### Default behavior

- Minimum level: **INFO** (`log.debug` is filtered until the level is lowered).
- Output stream: **stderr** through the runtime error stream (tests and CLI can redirect it).
- Each line includes an ISO-like local timestamp, level name, **source file and line** of the call site, message, and formatted `key=value` fields.

Example:

```phs
log.info("User logged in", {"user_id": 42, "method": "password"})
```

Typical line:

```text
2026-09-22T12:40:12 INFO app.phs:4 User logged in user_id=42 method="password"
```

### `set_level`

```phs
log.set_level(log.DEBUG)
log.set_level("INFO")
```

Argument is a level string (`DEBUG`, `INFO`, `WARNING`, `ERROR`). Invalid names raise `ValueException`.

### Async

Logging is synchronous. Calls from `async func` bodies and tasks are allowed and write immediately on the calling thread.

### Not in this section

Logger hierarchies, handlers/filters, plugins, rotation, network appenders, async/background log queues, `log.exception()`, JSON log mode, and extra severity names (`TRACE`, `FATAL`, etc.).

---

## Bölüm 7: `crypto`

Cryptographic hashing, HMAC, secure random bytes, and constant-time comparison. All hash/HMAC results are `bytes`. Inputs accept `bytes` or `string` (UTF-8 octets). **Synchronous only** in V1.

### Import

```phs
import crypto
```

### One-shot digests

| Function | Output size |
|----------|-------------|
| `crypto.sha256(data)` | 32 |
| `crypto.sha512(data)` | 64 |
| `crypto.sha3_256(data)` | 32 |
| `crypto.sha3_512(data)` | 64 |
| `crypto.blake2b(data)` | 64 |
| `crypto.blake2s(data)` | 32 |

`BLAKE2` requires an OpenSSL-enabled build; on Windows BCrypt-only builds these functions raise `ValueException`.

### HMAC

```phs
crypto.hmac_sha256(key, data)   # 32 bytes
crypto.hmac_sha512(key, data)   # 64 bytes
```

### Random and compare

```phs
crypto.random_bytes(length)   # CSPRNG; length 0 .. 1_048_576
crypto.compare(a, b)          # constant-time; false if lengths differ
```

Use `crypto.compare` for secrets (MACs, tokens), not `==` on `bytes`.

### Streaming and files

```phs
hasher = crypto.hasher("sha256")   # algorithm name string
hasher.update(chunk)               # bytes or string; repeatable
digest = hasher.digest()           # bytes; hasher is finalized

crypto.hash_file(path, algorithm)  # reads file in chunks (binary)
crypto.hash_io(file_handle, algorithm)  # open `io` handle; binary read
```

Algorithm names for `hasher`, `hash_file`, and `hash_io`: `sha256`, `sha512`, `sha3_256`, `sha3_512`, `blake2b`, `blake2s`.

### KDF

```phs
crypto.pbkdf2_sha256(password, salt, iterations, length)
crypto.scrypt(password, salt, length, options)      # OpenSSL builds only
crypto.argon2id(password, salt, length, options)    # OpenSSL 3.2+ only
```

`password` / `salt`: `bytes` or `string`. `options` is a `dict` or `null` (defaults).

| Function | `options` keys (all optional) |
|----------|-------------------------------|
| `scrypt` | `n`, `r`, `p` (defaults `16384`, `8`, `1`) |
| `argon2id` | `memory_kib`, `iterations`, `parallelism` (defaults `65536`, `2`, `1`) |

### Authenticated encryption

AES-GCM: key **16 / 24 / 32** bytes, nonce **exactly 12** bytes, tag **16** bytes.

ChaCha20-Poly1305: key **32** bytes, nonce **12** bytes, tag **16** bytes.

```phs
package = crypto.encrypt_aes_gcm(key, nonce, plaintext, aad)
# dict: { "ciphertext": bytes, "tag": bytes }

plaintext = crypto.decrypt_aes_gcm(key, nonce, package["ciphertext"], package["tag"], aad)

package = crypto.encrypt_chacha20_poly1305(key, nonce, plaintext, aad)
plaintext = crypto.decrypt_chacha20_poly1305(key, nonce, package["ciphertext"], package["tag"], aad)
```

`aad` (additional authenticated data) is `bytes`, `string`, or `null` for none. Use `crypto.random_bytes` for keys and nonces; **never reuse a nonce with the same key**.

Authentication failure (wrong key, tag, nonce, or AAD) raises `ValueException` with message `authentication failed` — no plaintext is returned.

### Backend availability

| Feature | OpenSSL build | Windows BCrypt-only |
|---------|---------------|---------------------|
| PBKDF2-SHA256 | OpenSSL `PKCS5_PBKDF2_HMAC` | HMAC-SHA256 composition |
| scrypt | `EVP_PBE_scrypt` | not available (explicit error) |
| Argon2id | OpenSSL 3.2+ KDF | not available |
| AES-GCM | EVP AES-GCM | BCrypt GCM |
| ChaCha20-Poly1305 | EVP | BCrypt (when supported by OS) |

### Errors

Wrong argument types → `TypeException`. Invalid lengths, unsupported algorithm/backend, or parameter limits → `ValueException` / `RuntimeException`.

### Not in this section

RSA/ECC, signatures, key exchange, TLS, public-key encryption, and low-level native crypto handles.

---

## Bölüm 8: `sql` (SQLite)

SQLite access via `sql.open`. Parameter binding uses positional `?` placeholders (no string concatenation for values).

### Import

```phs
import sql

db = sql.open("app.db")
db = sql.open(":memory:")
```

### `Database`

| Method | Description |
|--------|-------------|
| `db.close()` | Close connection; finalizes open `Rows` / `Statement` handles |
| `db.is_open()` | `true` while connection is open |
| `db.execute(sql, params?)` | `INSERT` / `UPDATE` / `DELETE` / DDL; returns `ExecuteResult` |
| `db.query(sql, params?)` | `SELECT`; returns streaming `Rows` |
| `db.query_one(sql, params?)` | First row or `null` |
| `db.prepare(sql)` | Reusable prepared `Statement` |
| `db.begin()` | Start an explicit `Transaction` on this connection |
| `db.async_execute(sql, params?)` | `execute` on the async I/O worker; returns `Task` → `await` |
| `db.async_query(sql, params?)` | `query` asynchronously |
| `db.async_query_one(sql, params?)` | `query_one` asynchronously |
| `db.async_prepare(sql)` | `prepare` asynchronously |

`params` is a `List` (or omitted / `null` when there are no placeholders). Supported bind types: `int`, `float`, `string`, `Bytes`, `null`.

`Database` cannot be constructed with `Database()`; use `sql.open`.

### `ExecuteResult`

| Field | Description |
|-------|-------------|
| `affected_rows` | Rows changed (SQLite `changes`) |
| `last_insert_id` | Last `INTEGER PRIMARY KEY` row id |

### `Rows` and `Row`

`db.query` returns `Rows` — iterable via `for row in rows` (iterator protocol). Rows are **not** loaded entirely into a `List`. Call `rows.close()` to release the cursor early; iteration completion also finalizes the native statement.

`Row` supports column access: `row["name"]`, `row["age"]`.

SQLite → PScript column types: `INTEGER` → `int`, `REAL` → `float`, `TEXT` → `string`, `BLOB` → `Bytes`, `NULL` → `null`.

### `Statement`

| Method | Description |
|--------|-------------|
| `stmt.execute(params?)` | Same as `db.execute` on the prepared SQL |
| `stmt.query(params?)` | Same as `db.query`; **consumes** the statement (cursor moves to `Rows`) |
| `stmt.query_one(params?)` | First row or `null`; statement remains open for reuse |
| `stmt.close()` | Finalize the prepared statement |

### `Transaction`

`tx = db.begin()` runs `BEGIN` on the same SQLite connection (autocommit off until `commit` / `rollback`).

| Method | Description |
|--------|-------------|
| `tx.execute` / `query` / `query_one` / `prepare` | Same as `Database` (same binding and types) |
| `tx.async_*` | Async variants (same names as on `Database`) |
| `tx.savepoint(name)` | `SAVEPOINT` (name: letters, digits, `_` only) |
| `tx.rollback_to(name)` | `ROLLBACK TO`; transaction stays active |
| `tx.release(name)` | `RELEASE SAVEPOINT` |
| `tx.commit()` | `COMMIT`; idempotent second `commit()` is a no-op |
| `tx.rollback()` | `ROLLBACK`; idempotent second `rollback()` is a no-op |

After `commit` or `rollback`, further SQL on that `Transaction` raises `ValueException`. Closing the `Database` or runtime shutdown rolls back any still-active transaction and finalizes SQL resources.

### Async SQL

Same model as `fs.async_read` / `io` handles: methods return a `Task`; use `await` inside `async func`. Work runs on the `AsyncIoService` worker thread (native SQLite only; no VM execution on the worker). Connection access is serialized with a per-database lock.

`Task.cancel()` and `timeout(task, ms)` integrate with other async I/O: cancellation calls `sqlite3_interrupt` and rolls back an active `Transaction` tied to the operation if needed.

### Errors

Syntax / SQLite failures → `RuntimeException`. Closed database/statement/rows, parameter count mismatch → `ValueException`. Unsupported parameter types → `TypeException`.

### Not in this section

Connection pool, ORM, query builder, migrations, other SQL drivers.

### Backend notes (V1)

Single SQLite connection per `Database`; no pool. Serialized access via mutex (safe with async worker). Not a substitute for multi-writer server databases.

---

## Bölüm 9: `testing` and `cli`

Small, practical modules for unit tests and command-line programs. They integrate with the existing VM, async scheduler, and host `program_args` (arguments after the script path).

### `testing` module

Discover and run module-level functions whose names start with `test_`:

```phs
import testing

func test_addition():
    testing.equal(2 + 2, 4)

testing.run()
```

Output is written to the program’s normal stdout (`Testing...`, per-test `ok` / `x` lines, then `N passed, M failed`). If any test fails, the process exit code is set to `1` (without aborting the host before the summary).

| API | Description |
|-----|-------------|
| `testing.run()` | Discover `test_*` functions in the current module, run them in name order |
| `testing.equal(actual, expected)` | Fails if values differ (`Runtime::equals`) |
| `testing.not_equal(actual, expected)` | Fails if values are equal |
| `testing.true(value)` | Fails unless `value` is bool `true` |
| `testing.false(value)` | Fails unless `value` is bool `false` |
| `testing.null(value)` | Fails unless `value` is null |
| `testing.not_null(value)` | Fails unless `value` is non-null |
| `testing.throws(func)` | Passes if calling `func()` raises an exception |
| `testing.fail(message)` | Unconditional failure |

Assertion failures raise `ValueException` with message prefix `Testing assertion failed:` so the runner can distinguish them from internal errors.

**Async tests:** use `async func test_...()` and call `testing.run()` from `async func main()` so the scheduler is already active. The runner also enables the scheduler for async tests when needed. Example with async SQL:

```phs
import testing
import sql

async func test_sql_async_cancel():
    db = sql.open(":memory:")
    task = db.async_execute("INSERT INTO t (v) VALUES (?)", [1])
    await delay(0ms)
    task.cancel()
    try:
        await task
        testing.fail("expected cancel")
    catch CancellationException:
        testing.true(true)

async func main():
    testing.run()
```

V1 does not include mocks, fixtures, coverage, benchmarks, or a filter argument on `testing.run()`.

### `cli` module

Parse `runtime.program_args()` (set by the `pscript` host from argv after the script file). Register options before reading them; the first access triggers parsing.

```phs
import cli

cli.help("My application")
cli.version("myapp 1.0.0")

name = cli.option("name", "World")
verbose = cli.flag("verbose")

print(name)
```

| API | Description |
|-----|-------------|
| `cli.args()` | List of positional argument strings |
| `cli.arg(index)` | Positional at `index` (non-negative); error if out of range |
| `cli.option(name)` | Required string option `--name` |
| `cli.option(name, default)` | Optional option with default string |
| `cli.flag(name)` | `true` if `--name` was passed |
| `cli.has(name)` | Whether the option/flag was present on the command line |
| `cli.command(name)` | `true` if the first positional matches registered subcommand `name` |
| `cli.help(text)` | Set help text; `--help` / `-h` prints usage and exits `0` |
| `cli.version(text)` | Set version string; `--version` / `-V` prints it and exits `0` |

Supports `--opt value`, `--opt=value`, flags, `--` to pass remaining tokens as positionals, and a single registered subcommand as the first positional. Unknown options and missing required values raise `ValueException` with readable messages.

### `archive` module

Read and write archive files on Windows and Linux. Supported formats: **ZIP** (including ZIP64), **TAR** (USTAR and PAX; GNU long-name read compatibility), and **TAR.GZ** (gzip layer used only inside this module — there is no separate `compress` stdlib).

```phs
import archive

a = archive.open("game.zip", "r")
for entry in a:
    print(entry["name"])

data = a.read("config.json")
a.close()
```

```phs
a = archive.create("game.zip")          # format from extension, or pass "tar" / "tar.gz"
a.add("assets/")
a.add("game.phs")
a.write("config.json", encoding.encode("{}"))
a.close()
```

| API | Description |
|-----|-------------|
| `archive.open(path, mode?)` | Open existing archive. `mode`: `r` (read) or `w`/`a` (write/update for ZIP). Optional compression level (ZIP/TAR.GZ). |
| `archive.create(path, format?)` | Create a new archive. `format`: `zip`, `tar`, or `tar.gz` (default from extension). |
| `list()` | List of entry path strings |
| `entries()` | List of entry metadata dicts (`name`, `size`, `compressed_size`, `is_file`, `is_directory`, `is_symlink`, `modified`, `mode`) |
| `read(name)` | Entry contents as `bytes` |
| `write(name, data)` | Write `bytes` or `string` data |
| `add(path, name?)` | Add file or directory (recursive) |
| `extract(name, destination?)` | Extract one entry (default destination: current directory) |
| `extract_all(destination?)` | Extract all entries |
| `open_entry(name)` | Materialize one entry to a temp file and return an `io` read `FileHandle` |
| `close()` | Finalize and close |
| Optional limits dict | `max_entries`, `max_single_file_bytes`, `max_total_extract_bytes` (last argument) |

**Security (always on):** entry paths are normalized to forward-slash relative form; `..`, absolute paths, and drive-letter paths are rejected. Extraction resolves under the destination directory (no path traversal or destination escape). Symbolic links are only created when the target stays inside the destination. Device/special entries are not extracted as regular files. Built-in limits guard entry count and extracted size (defaults are high enough for normal use).

**Errors:** invalid or truncated archives, CRC mismatch (ZIP), bad gzip, unsupported encrypted ZIP, missing entries, and limit violations raise runtime errors — no silent corruption.

**Platform notes:** host filesystem paths use the OS separator; paths stored inside archives use `/`. TAR permission and timestamp metadata are preserved where the format provides them.

---

## Deliberately excluded

- Counter, PriorityQueue, LinkedList, OrderedMap, and other extra collection types
- Binary I/O (`read_bytes`, `write_bytes`)
- `fs.size()`, `fs.modified()`, `io.seek()`, `io.tell()`
- Async process APIs (`spawn`, `wait`, `kill`)
- HTTP/2, WebSocket, chunked streaming, connection pool, keep-alive client API, TLS server
- Package distribution beyond built-in modules
- Path sandboxing / automatic traversal blocking
