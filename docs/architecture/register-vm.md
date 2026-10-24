# Register VM

The VM executes register bytecode. There is no stack VM, LLVM, AOT, or JIT.

---

## State

- **Global variables:** `variables_`, indexed by compiler global slots. Shared across calls.
- **Current register file:** `registers_`. Temporary values for the current frame (main or a function).
- **Current locals:** `locals_` — `shared_ptr<Cell>` per local slot. Empty at top level. Inside a function, parameters occupy `0 .. arity-1` and later locals follow. Each call allocates **new** cells.
- **Current upvalues:** `upvalues_` — captured cells from the callee’s `ClosureData`.
- **Call stack:** each `CallFrame` stores `return_ip`, the caller `dest` register, and the saved caller `registers`, `locals`, and `upvalues`. Module-bound calls also store `module_index`, `saved_code`, and `saved_variables`.
- **Module state:** `module_states_[i]` tracks `Unloaded` / `Loading` / `Loaded` / `Failed`, per-module global slots, and a cached failure message. `execution_bytecode_` may point at `bytecode_` (main) or an entry in `bytecode_.module_bytecodes`. Re-entrant import during `Loading` does not restart initialization; failed modules stay failed.

A call does not mutate a single global function-state blob. Recursion uses `CALL` / `CALL_VALUE` / `RETURN`: each activation has its own registers and local cells. `factorial(n)` calling `factorial(n - 1)` therefore keeps a distinct `n` per frame.

`ValueKind::Function` holds `shared_ptr<ClosureData>` (`function_index`, `name`, captured cells). Format is `<function name>`. Truthiness is true. Equality is pointer equality of the closure object.

---

## `CALL`

1. Read argument values from the caller register file.
2. Push a frame with the current registers, locals, upvalues, return IP, and destination register.
3. Allocate a new register file of `FunctionInfo.register_count` and a new cell for each `local_count`.
4. Copy arguments into locals `0 .. arity-1`.
5. Upvalues start empty (top-level named call).
6. Jump to `FunctionInfo.entry`.

---

## `MAKE_CLOSURE` / `LOAD_FUNCTION` / `CALL_VALUE`

`MAKE_CLOSURE` builds a function value and copies `shared_ptr<Cell>` from the current locals or upvalues according to `FunctionInfo.captures`. Those cells remain alive after `RETURN` if the closure value is stored.

`LOAD_FUNCTION` builds a function value with no captures (a top-level `func` used as a value).

`CALL_VALUE`:

1. The callee register must be `ValueKind::Function` (`invalid function value call` otherwise).
2. `argc` must match `FunctionInfo.arity` (`'name' expects N arguments`).
3. Enter like `CALL`, but install the closure’s capture list as the new frame’s upvalues.

`invalid closure access` is used for a bad upvalue index at runtime.

---

## `RETURN`

1. Read the result from the callee register file.
2. Pop the frame (restore caller registers, locals, and upvalues).
3. Write the result into the caller destination register.
4. Continue at the instruction after `CALL` / `CALL_VALUE`.

If the compiler emitted the implicit trailing `RETURN` of `null`, that is the result when the function did not `return` and the last top-level statement was not an automatic-return expression.

`RETURN` always pops one call frame. It is not a loop `break`. Local cells with no remaining `shared_ptr` (no closure captured them) are freed with the frame. Captured cells stay with the closure.

---

## Exceptions (Bölüm 1–3)

Separate from the call stack, `handler_stack_` tracks active `try` regions (`ENTER_TRY` / `LEAVE_TRY`). Each `HandlerFrame` records `handler_index`, `frame_depth`, and `next_catch` (for rethrow within the same `try`).

`THROW` and `raise_runtime_error` both call `throw_value`: enrich exception metadata (`stack_trace`, `file`, `line`, `column`), walk handlers innermost-first (typed or untyped `CatchEntry` match via `is_class_or_subclass`, bind catch slot, `unwind_to_frame_depth`, jump to `catch_ip`; or run `finally` with `pending_exception`), then unwind `CallFrame`s until a handler catches or `fail_uncaught` formats `Unhandled exception: …`.

`pending_control_` holds deferred `return` / `break` / `continue` through `finally` (`DEFER_*` opcodes). `END_FINALLY` completes deferred control or re-throws `pending_exception`. `return` / `throw` inside `finally` clear pending state.

`exception_support` resolves built-in exception classes from the prelude, maps `RuntimeErrorCategory`, captures stack frames from `instruction_locations` + `CallFrame` return IPs, and formats cause chains (cycle-safe). Internal VM invariant violations remain `VmError` (not catchable).

---

## Class opcodes

- **`INVOKE_CLASS`** — allocate a `ValueKind::Class` instance using the flattened `ClassInfo.layout` field names; optionally enter the constructor (`function_index`) with arguments from registers. If the class has no constructor but the parent has a zero-arg constructor, the VM may chain that parent ctor on the new instance.
- **`CALL_CLASS_METHOD`** — enter a method `FunctionInfo` with `this` bound to the receiver (local 0). Dispatch index is compile-time resolved (`function_index`); overrides are handled by the compiler choosing the derived method for normal calls and the parent method for `super` / `parent` calls.
- **`INVOKE_SUPER`** — enter the parent constructor on the existing `this` (local 0) without creating a new instance. Used for `super(...)` / `parent(...)` in child constructors.

`GET_FIELD` / `SET_FIELD` work on class instances using flattened layout indices, same encoding as structs.

---

## Async scheduler and suspend/resume (Bölüm 1–2)

When `Bytecode.async_main_function_index` is set, `Vm::run()` runs **`run_top_level_init()`** (imports / top-level statements until `HALT`), then **`run_async_scheduler()`** instead of the synchronous loop.

**Task model (`ValueKind::Task`):** internal states `Pending`, `Running`, `Completed`, `Failed`. Stores result, exception, optional timer wake time, `exception_consumed` (set when `await` rethrows a failed task’s exception), and `AsyncExecutionState` while suspended.

**Scheduler (single OS thread, cooperative):**

1. Enqueue root `async main` task.
2. Loop: expire timer tasks → dequeue runnable task → `start_task_execution` until yield.
3. Yield on `AWAIT` (pending task), async return (including through `finally`), or `fail_task`.
4. When a task completes, wake waiters enqueued from `await`.
5. On idle queue, sleep until next timer deadline (no busy-wait).
6. After all work finishes, `check_scheduler_shutdown()` reports unhandled failed tasks.

**Async call:** `spawn_async_function` allocates `TaskData`, writes `Task` to the call destination register, tracks the task, and enqueues it (no synchronous `enter_function`). Module namespace calls (`math.foo()`) use the same spawn path when `is_async`.

**`AWAIT`:** if the task is already completed/failed, handle immediately; otherwise save `AsyncExecutionState` (IP, registers, globals/module globals, locals, upvalues, **call frames**, handler stack, pending control/exception, `execution_bytecode_`) on the **current task**, link waiter/wake lists, and yield. Saved state clears live `frames_` / `handler_stack_` / register file so nested task execution cannot append stale frames.

**Resume:** restore saved state, write awaited result (or `throw_value` on failure with `exception_consumed`), continue at the instruction after `AWAIT` (fetch already advanced IP past the opcode).

**Async return + `finally`:** `complete_pending_control` and `return_from_function` call `finish_current_async_task` when the last frame pops inside a running task (including after `finally` runs).

**Timer `delay`:** creates a timer task; scheduler sleeps until the next deadline when the run queue is empty.

**Exceptions:** uncaught throws in an async task call `fail_task`; `await` propagates the stored exception into the awaiting frame with existing enrichment/unwind rules. Failed tasks whose exceptions were consumed via `await` are not reported again at shutdown.

**Stack traces / source locations:** `capture_stack_trace` resolves function names and file/line/column against the active bytecode (main or module) and per-frame module context, so async/module throws report correct frames.

**Module calls:** saved/restored `CallFrame` entries include module context (`saved_code`, `saved_variables`, `module_index`) like synchronous module-bound calls. Module globals are synced when a module-bound async task completes.

Full scheduler reference: `docs/architecture/scheduler.md`.

## Finalized status (Bölüm 3)

Async/Await is integrated across functions, closures, classes, inheritance, modules, exceptions, control flow, type checking, and CLI. See `LANGUAGE_SPEC.md` §34.
