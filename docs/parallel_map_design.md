# `parallel.map` — design and thread-safety (PScript 1.0)

## API

- `parallel_map(items, fn)` builtin and `parallel.map(items, fn)` syntax sugar (compiler lowers to the builtin).
- `items`: any iterable materialized to a `List` of values.
- `fn`: callable with arity 1.

## Execution model

- When the main program runs under `Vm::run()`, the VM registers a **per-thread worker invoker factory** on the shared `Runtime`.
- Each OS worker thread used by `parallel_map` calls `create_parallel_thread_invoker()` once and receives a closure that:
  - Owns a **dedicated worker `Vm`** (bytecode copy, JIT disabled, sandboxed `invoke_callable_isolated`).
  - Takes a **global lock** on `Runtime::parallel_worker_invoke_mutex()` for the duration of each mapper invocation.

## Supported callables

`parallel_callable_is_worker_safe()` allows only:

- `ValueKind::Function` with **empty captures** (module-level functions).

Rejected at runtime with a clear error:

- Closures with captures, natives, or non-callable mapper.

## Ordering and errors

- Results are stored in `results[i]` by the worker that owns index `i`; order matches input order.
- First worker exception is captured; `failed` flag stops siblings from starting new items; all threads are joined; exception is rethrown.
- Empty input returns `[]` without spawning threads.
- Single-element input uses the main runtime invoker (no thread pool).

## Concurrency

- Peak active worker threads is tracked (`parallel_map_last_peak_concurrency()`) for tests.
- Mapper calls on different threads can overlap in time, but **shared `Runtime` mutation is serialized** by `parallel_worker_invoke_mutex_` because the runtime heap, builtin tables, and profiler are not designed for lock-free concurrent mutation.
- Worker VMs do not share register files or call stacks; only the `Runtime` reference and read-only bytecode/modules are shared.

## Explicit non-goals (1.0)

- Closures with captures or mutable shared state.
- Parallel mapper calls while the main VM is also invoking into the same `Runtime` (not exercised; lock reduces risk but nested `parallel_map` still shares one mutex).
- Lock-free allocator / concurrent GC (no GC; `shared_ptr` heaps still require serialization).

## Opt-out

Set environment variable `PSCRIPT_DISABLE_PARALLEL` (any value) to force sequential `invoke_callable` on the main runtime (same ordering semantics, no worker threads).

## Tests

- `tests/parallel_map_test.cpp` — ordering, concurrency peak, empty input, unsupported closure, worker error propagation.
- `tests/vm_test.cpp` — `parallel_map` / `parallel.map` smoke tests.
