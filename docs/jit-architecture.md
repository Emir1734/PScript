# PScript JIT Architecture (implementation notes)

Tiering: VM → Baseline JIT → Optimizing JIT. Eligibility is per function; unsupported opcodes keep the function on the VM path.

## JIT-supported bytecode (function body)

| Opcode | Baseline | Optimizing | Notes |
|--------|----------|------------|--------|
| LoadConst, LoadLocal, StoreLocal, LoadFunction | yes | yes | via helpers |
| Call | yes | yes | nested JIT + per-site inline cache |
| CallValue | yes | yes | no captures on *target fn* required; closure targets use capture cells + IC token |
| LoadUpvalue, StoreUpvalue, MakeClosure | yes | yes | JIT entry binds `JitExecState.upvalues` from closure captures |
| Add, Sub, Mul, Div, Mod | yes | yes | baseline: string `+` uses `Runtime::concat_strings`; optimizing: `Int*` only when IR has no string `LoadConst` |
| Equal, NotEqual, comparisons | yes | yes | |
| UnaryNot | yes | yes | |
| GetIndex, GetField | yes | yes | optimizing: list/struct guards + fast paths |
| MakeStruct, SetField | yes | yes | optimizing: escape/scalar may eliminate NoEscape structs |
| GetIter, IterNext | yes | yes | native iterator fast path |
| Jump, JumpIfFalse, Return | yes | yes | |

Functions with async, constructors, implicit `this`, **direct self-recursion** (`Call` to the same function index), or unsupported opcodes remain VM-only. Nested functions with captures are JIT-eligible when their bytecode uses only supported opcodes.

## Call inline cache

Each `Call` / `CallValue` site gets a `JitCallIcSlot` on the compiled function. Runtime: monomorphic → polymorphic (4) → megamorphic. Monomorphic hits skip repeated compile lookup when the cached entry matches (`function_index`, native entry, closure token, optimization generation). On a monomorphic miss with a **different** target, the previous mono entry is moved to `poly[0]` and the slot becomes polymorphic (the old mono entry is not overwritten).

Before nested JIT, `jit_invoke_direct_call` may take a **direct leaf** fast path for capture-free callees whose bytecode is only `LoadLocal(0)`, `LoadConst`, `Add`, `Return` (e.g. `return x + 1`), with eligibility cached per function index.

Nested JIT uses a per-`Vm` `nested_jit_call_scratch_` `JitExecState` (reused across calls), mirrors arity arguments into callee `registers[0..arity-1]`, avoids re-allocating callee local `Cell`s when `local_count` is unchanged, and only clears non-argument register slots between calls. Parameter locals are updated in place without nulling the full local file on every entry.

`JitCallIcEntry` caches callee invoke metadata (arity, register counts, IR size, deopt context, nested IC vector) so monomorphic hits can call `prepare_nested_mono_callee` and invoke the compiled entry without `compile_if_needed` or `JitCompiledCode` lookup. Dispatch-tier `Call` sites emit monomorphic IC guards plus begin → indirect `entry(callee)` → complete (`jit_nested_complete_mono_call_emit`); IC miss falls through to `jit_helper_call_slow` → `jit_invoke_direct_call`. `CallValue` mono hits use a single fused helper (`jit_nested_invoke_mono_call_value_emit`: begin + callee entry + complete) after the same IC guards. `jit_helper_call` / `jit_helper_call_value` use the begin → entry → complete fast path in C++ helpers when safe.

Reentrant nested JIT from `nested_jit_call_scratch_` saves the in-flight frame on `nested_jit_scratch_stack_` (heap) while the callee runs in the single scratch buffer. Self-recursive calls at `jit_call_depth >= 1` bypass further JIT entry and run one interpreter `Call` (`jit_invoke_call_from_interpreter` with `jit_vm_fallback_depth_`) so deep recursion does not grow the native stack. Other nested `Call` sites from scratch use the same interpreter fallback when JIT invoke cannot run. `JitIcMetrics` counters are resolved via `ic_metrics_for_state` (state pointer or `host_vm` engine metrics). Integer/float returns copy scalar payload without full `Value` moves; callee args use lightweight int/float register mirroring when kinds match.

The linear numeric emitter inlines `return x + 1` callees inside hot loops (no nested call) when bytecode matches the leaf `arg + const` pattern, keeping `simple_calls`-style kernels on the sequential JIT path.

Hot numeric loops (`divmod_loop`, `loops`, etc.) use the **sequential numeric emitter** when IR passes `ir_eligible_for_linear_numeric` (no helper dispatch per `IntDiv`/`IntMod`). Native `idiv` is emitted for integer division/modulo; compile-time constant divisors skip reloading the divisor `Value` each iteration. Per-instruction `fallback`/`deopt` polling is limited to runtime-helper sites; type guards on optimizing tier use IR integer-shadow tracking to avoid repeated kind checks on loop-carried registers.

Stable `CallValue` (function value loaded once per register/local) may be lowered to direct `Call` only when the resolved target function has **no closure captures** (`captures.empty()`). Closures keep `CallValue` so `inline_stable_call_value_closures` can remap upvalues to `LoadClosureCapture`. Multi-capture callees (`captures.size() > 1`) are not CallValue-inlined. Fused nested-mono `CallValue` entry (`jit_nested_begin_mono_call_value`) is skipped when the target has more than one capture (slow `jit_invoke_direct_call` path with closure cells). Direct `Call` monomorphic nested entry (`jit_nested_begin_mono_call`) is skipped when the target function declares captures (lowering `CallValue`→`Call` is only allowed for captureless targets). The `closure_capture0_const` fast path applies only when the callee has exactly one capture (factory `make_adder(base)` pattern).

## Optimizing pipeline (advanced)

Order (high level): stable CallValue lowering → optional **leaf-only** inlining (early exit when all calls inlined) → constant fold → DCE → (else CSE, escape, memory guards, int spec only when no remaining calls, branch/LICM when calls remain). Integer `Int*` ops deopt on non-int operands; separate loop `GuardInteger` inserts were removed as redundant.

`JitCompiledCode::ir_instruction_count` caches final IR size for nested JIT entry without rebuilding IR.

## OSR (on-stack replacement)

Loop back-edges may transfer into compiled code when trip count reaches `osr_loop_trip_threshold` (default 1000). OSR uses `JitCompiledMetadata::osr_entries` and is supported for **both** baseline and optimizing tiers when metadata is present. **OSR is skipped** for functions whose bytecode contains `CallValue` (closure calls in hot loops): interpreter state at the loop header is not yet transferred safely into JIT for those sites. Such functions still enter JIT at function entry when call depth allows (`frames_.size() <= 2`). Optimizing tier code invalidation still disables OSR for that cache entry.

## Optimizing pipeline early exit (CallValue)

When IR still contains `CallValue` after stable lowering, the pipeline skips leaf inlining (avoids inlining closure factories into hot loops) and applies integer specialization + branch opts only. Plain `Call` sites still run the full inlining / escape pipeline.

## Benchmarking

`jit.exec.*` workloads report `steady` as **one fresh `Vm` per tier and a single `vm.run()`** (does not use `--warmup` / `--iterations`). For hot JIT comparison use **`warm_steady`**: same `Vm`, first timed run is `cold`, then median of `--iterations` runs after `--warmup`. `call_mono_hits` increments on fused/emitted mono success paths when metrics are enabled.

`--filter jit.exec.` runs only `jit.exec.*` workloads (not the paired `runtime.*` suites or `jit.exec.kernel` amortized rows). Use `--filter jit.exec.kernel` for kernel amortized exec.

## Call-aware inlining (optimizing tier)

Callee must be a **leaf** (no `Call` / `CallValue` / control flow in callee IR). Callers may contain loops. Non-leaf callees stay on the JIT→JIT direct call path.

Inlining uses a **cost model** (not hotness alone): estimated callee IR cost (guards/field ops weighted), per-caller growth budget (~192 insn), up to 8 sites per pass, benefit score from loop context + PGO (`very_hot`, loop iterations, callee `call_count`). Direct recursion and caller←callee cycles are rejected. When IR still has `CallValue`, inlining is skipped entirely (closure factories).

Multi-pass inlining (up to 6 passes) can inline `A→B` then remaining `B→C` calls inside cloned IR subject to the same budgets.

## Escape analysis and allocation elimination

Conservative intraprocedural analysis for `MakeStruct` / `SetField` / `GetField`:

- **NoEscape (eligible):** struct register only used for field init and field reads; not returned, compared, passed to calls, indexed, or moved to another register.
- **Escaped:** return of object register, call arguments, `Equal`/`NotEqual` on object registers, `MoveReg` of object register, unknown uses.
- **Scalar replacement:** `GetField` from a non-escaped site becomes `MoveReg` from the stored field value register.
- **Elimination:** `MakeStruct` + `SetField` nopped when all fields were recorded and the site did not escape.

Pipeline order: CFG → inlining → const fold → DCE → CSE → **escape/scalar** → DCE → memory guards → guard dedup → integer spec → branch → LICM.

Deoptimization: scalar replacement rewrites `GetField` to `MoveReg`; full allocation elimination (noping `MakeStruct`/`SetField`) is not enabled at runtime yet.

## Deoptimization

Optimizing tier uses integer guards, value-kind guards for specialization, with existing deopt metadata. Other ops fall back to the VM on runtime errors via `JitExecState::fallback`.

JIT helpers copy register operands (`read_reg`) when more than one slot is read before writing, so growing `JitExecState::registers` cannot invalidate references held across helper calls (Debug builds are especially sensitive to this).

## Benchmarks

See `tests/pscript_benchmark.cpp` and `tests/benchmark/bench_workloads.h`. `jit.exec.*` records compare VM, baseline JIT, and optimizing JIT; selected workloads also emit `cold` and `warm_steady` phases. Linux x64: supported in source, not verified in CI here.
