# Cooperative Async Scheduler (Bölüm 2)

The async scheduler lives inside the register VM (`Vm::run_async_scheduler`). It is **not** a separate runtime or thread pool.

## Design constraints

- **One OS thread for VM execution** — cooperative task switching at `await`, timer wait, and I/O completion wait. Blocking file/stdin work runs on a **single dedicated I/O worker thread** (not a thread pool).
- **Deterministic** — runnable queue + timer wake list; no busy-wait polling.
- **VM-owned** — scheduler state (`run_queue_`, `timer_tasks_`, `tracked_tasks_`, `current_task_`) is private to `Vm`.
- **Out of scope (Bölüm 2 core):** multithreading, top-level `await`, async constructors. *(Task control: Update Bölüm 1. Async I/O: Update Bölüm 2 — see below.)*

See also: `register-vm.md` (suspend/resume), `bytecode.md` (`AWAIT`), `LANGUAGE_SPEC.md` §33.

---

## Program startup with `async main()`

When `Bytecode.async_main_function_index` is set:

1. **`run_top_level_init()`** — executes module-level bytecode (imports, top-level assignments) until `HALT`. Preserves `variables_` / module state for the program.
2. **`run_async_scheduler()`** — enqueues and runs the root `async main` task and all spawned/timer tasks until idle.

Normal `func main()` programs skip the scheduler and use the synchronous loop only.

---

## Task lifecycle

`TaskData` states:

| State | Meaning |
|-------|---------|
| `Pending` | Created or suspended; eligible for the run queue when runnable |
| `Running` | Currently executing on the VM |
| `Completed` | Finished with a stored `result`; **never run again** |
| `Failed` | Finished with a stored `exception`; **never run again** unless exception was consumed via `await` |
| `Cancelled` | Cooperatively cancelled with `CancellationException`; **never run again** unless exception was consumed via `await` |

Valid transitions:

- `Pending → Running → Completed`
- `Pending → Running → Failed`
- `Pending → Running → Cancelled`
- `Pending → Running → Pending` (suspend at `await` or while waiting on a timer-backed task)

Terminal tasks (`Completed` / `Failed` / `Cancelled`) are rejected by `is_task_runnable()` and `start_task_execution()`.

Task VM state (`AsyncExecutionState`: IP, registers, locals, frames, handler stack, module context) is **separate** from task lifecycle fields (`state`, `result`, `exception`).

---

## Scheduler loop

```
while (has pending work):
    process_io_completions()
    process_ready_timers()
    if run_queue empty:
        wait_for_scheduler_wake()   // timers + I/O completions; no busy-wait
        continue
    task = pop run_queue
    run_task_until_yield(task)     // until await, return, or throw
check_scheduler_shutdown()
```

**Runnable work** includes:

- Tasks in `run_queue_` with non-terminal state
- Timer tasks whose deadline has not yet passed (keeps loop alive)
- Timer tasks ready to complete (moved to completed, waking waiters)
- In-flight async I/O operations (keeps loop alive until completion or cancel)
- Pending I/O completion queue entries

The loop **does not exit early** when the root task completes; spawned-but-not-awaited tasks still run.

---

## `await` integration

- Operand must be a `Task` value.
- **Completed** — result copied to destination register; no suspend.
- **Failed** — exception rethrown into the awaiting frame via `throw_value`; `exception_consumed` set on the failed task so shutdown does not treat it as unhandled.
- **Cancelled** — same propagation path as **Failed**, using the stored `CancellationException`.
- **Pending / Running** — save `AsyncExecutionState` on the current task, link waiter list, yield to scheduler.

On resume, execution continues at the instruction **after** `AWAIT` (IP already advanced by `fetch`).

---

## Timer / `delay()`

- `delay(duration)` creates a timer `TaskData` entry in `timer_tasks_` (duration normalized to milliseconds).
- Calling task awaits that timer task; current task suspends.
- When the deadline passes, `process_ready_timers()` completes the timer task and wakes waiters.
- If no task is runnable, `sleep_until_next_timer()` blocks the OS thread until the next deadline (not a spin loop).

---

## Unhandled async failures

All tasks created by `spawn_async_function` and `create_timer_task` are registered in `tracked_tasks_` (strong refs until shutdown).

At scheduler shutdown, `check_scheduler_shutdown()`:

1. Fails if the **root** task ended in `Failed`.
2. Fails if any other tracked task is `Failed` **and** `exception_consumed == false` (spawned task failed without its exception being propagated through an `await` that the caller handled).

This matches synchronous unhandled-exception reporting (`fail_uncaught` / `VmError`).

---

## Stack traces in async/module code

Exception stack frames resolve against the **active bytecode** at the throw site (`execution_bytecode_`) and per-`CallFrame` module context (`module_index`, `saved_code`). Module function names and source locations appear correctly in async stack traces.

---

## Finalized scope (Update Bölüm 1 — Task Control)

### `task.cancel()`

Cooperative cancellation via `Task.cancel()` (VM method dispatch). Sets `cancel_requested`; the task observes it at `await` and between bytecode steps. **`finally` blocks run** before the task reaches `Cancelled`. Fire-and-forget cancelled tasks that nobody awaits do not fail shutdown (same as consumed failures).

### `race(t1, t2, …)`

Meta-task registered as a waiter on each contestant. First terminal contestant wins (deterministic order on ties). **Does not cancel losers.**

### `timeout(task, ms)`

Meta-task plus internal timer. Target completes first → return result / propagate failure. Timer fires first → cancel target (with `finally`), fail meta with `TimeoutException`. Timer disarmed on early completion.

### Still out of scope

Iterator protocol, multithreading, top-level `await`, async constructors, `process.spawn()` / `wait()` / `kill()`. Public `net` and `http` are in the standard library (see Update Bölüm 2).

---

## Update Bölüm 2 — Async I/O + `async_input()`

### Async file / handle I/O

- `fs.async_read` / `async_write` / `async_append` and `FileHandle.async_read` / `async_write` return `Task` values immediately.
- Work is queued to `AsyncIoService` (single worker thread); completions call `complete_task` / `fail_task` / `cancel_task` on the scheduler thread via `process_io_completions()`.
- Pending I/O tasks (`io_operation_id != 0`) are not executed as bytecode; they complete externally.

### `async_input()`

- Builtin handled like `delay()` / `race()` / `timeout()` in `CALL_BUILTIN`.
- Uses the same worker + completion path as file I/O; optional prompt is printed on the scheduler thread before the worker blocks on stdin.

### Cancellation

- `request_task_cancellation()` also calls `AsyncIoService::cancel_task_operations()`.
- Cancelled in-flight work posts a cancelled completion; `finally` on the awaiting task is preserved.

### Spawned tasks + module imports

- After top-level init, the scheduler snapshots program globals into `scheduler_globals_`.
- Spawned async tasks inherit this snapshot when `variables_` is empty so imported modules (e.g. `fs`) remain visible.

### Network and HTTP (`net`, `http`)

- Public `import net` and `import http` use the same scheduler. There is no second async runtime for client calls.
- File/stdin I/O stays on the **single** `AsyncIoService` worker. Network `IoKind`s (`NetConnect`, `NetRead`, `NetWrite`, `NetAccept`, `NetUdpSend`, `NetUdpReceive`, `NetResolve`, `HttpRequest`) run on **short-lived dedicated threads** so a blocking accept cannot stall stdin cancellation.
- Inside an active scheduler, `net.connect` / `read` / `write` / `accept` / UDP send-receive / `resolve` and `http.get` / `post` / `request` return a `Task` immediately. `listen`, `udp`, `http.server`, `close`, and `local_port` stay synchronous.
- Completions still enter through `process_io_completions()`. `task.cancel()` calls `cancel_task_operations()`. `await timeout(task, ms)` is the only timeout API.
- `HttpServer.start()` is the one deviation: its accept loop is a private thread, not `submit_net_accept`. Handlers are queued back onto the scheduler (`queue_callable_invoke`); responses are written from `http_server_poll_pending_handlers`. The public HTTP API does not change because of that split.
- `run_socket_readiness_self_test()` remains an internal Windows `select` / POSIX `poll` check. User code does not call it.

---

## VM safety on suspend/resume

`save_current_execution_state` moves and then **clears** live `registers_`, `variables_`, `locals_`, `upvalues_`, `frames_`, and `handler_stack_` so a nested task cannot append stale frames (Bölüm 1 regression fix).

`finish_current_async_task` (Bölüm 2) handles async return through `finally` and syncs module globals before marking a module-bound task completed.

Module async calls through `CallMethod` use `spawn_async_function` when `FunctionInfo.is_async` (same as `CallValue`).
