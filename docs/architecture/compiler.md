# Compiler

The compiler walks the AST and emits **register bytecode**. Names are resolved to numeric slots at compile time. The VM does not search by string.

Pipeline: Parser AST → this compiler → `Bytecode` → Register VM.

---

## Passes

For a program:

1. Push the global `Scope`.
2. **Declare imports:** each top-level `ImportStmt` resolves/compiles dependencies via `ModuleCompileCache`, declares importer bindings, and validates `from … import` exports when paths resolve.
3. **Hoist functions:** every top-level `FuncStmt` is recorded as a `Function` symbol (`name`, arity, `function_index` into `Bytecode.functions`). Bodies are not compiled yet. This is why a call may appear before the `func` in source.
4. Compile all non-function, non-import-hoist statements (the script’s main path).
5. Emit `HALT`.
6. Compile each function body, filling `FunctionInfo.entry`, `local_count`, `register_count`, and `captures`.

Import statements in the main path emit `IMPORT_MODULE` / `IMPORT_NAME` during step 4 (types linked at declare time skip `IMPORT_NAME`). Module files compile with `set_module_mode(true)`; **public** top-level symbols are collected into `ModuleDescriptor.exports` after the declaration pass (partial refresh for circular imports) and again at the end of compilation.

`declare_imported_symbol` links class/enum/struct exports into the importer bytecode (`link_class_from_module`, etc.). Module-level `pub` / `priv` set `Symbol.access`; `prot` at module scope is rejected. `ModuleCompileCache` registers loading placeholders before recursive compilation.

Main code therefore sits at instruction 0. Function bodies are placed after `HALT` and are reached only by `CALL` / `CALL_VALUE`. Ordinary variables are still not hoisted: a use in the owning function before an assignment is `undefined variable`.

When compiling a function:

1. Push a function scope; declare parameters (initialized).
2. Hoist nested `func` in that body (local slot + `Function` symbol).
3. Predeclare current-block assignments so nested bodies can capture later locals (slot allocation only; loads still require initialization).
4. Compile nested function code (before this function’s `entry`). `Bytecode.functions` may grow; do not keep a `FunctionInfo&` across this step.
5. Set `entry`, emit `MAKE_CLOSURE` + `STORE_LOCAL` for each nested function, then compile the body (`FuncStmt` lines are skipped; they are not automatic-return candidates).

Nested `func` inside `if` / loop / `match` blocks is hoisted at that block; a `JUMP` skips the nested code in the executable stream, then `MAKE_CLOSURE` runs in the block.

---

## Symbols and slots

`Scope` / `Symbol` distinguish `Variable`, `Function`, and `Class`. Access defaults to `Public`. Symbols record `owner_function` and an initialization flag. Struct/class field access modifiers are enforced separately via `MemberAccess` metadata.

- Global variables occupy `Bytecode.variables` slots (`LOAD_VAR` / `STORE_VAR`).
- Function parameters, function-local variables, and nested function values occupy per-frame **cells** (`LOAD_LOCAL` / `STORE_LOCAL`).
- An enclosing local used from a nested function becomes an upvalue (`LOAD_UPVALUE` / `STORE_UPVALUE`) listed on `FunctionInfo.captures`. Globals are never captured.
- Assignment vs declaration follows the existing scope rules (`resolve_assign` vs `declare_variable`), including from inside a function.

Duplicate nested function names and duplicate parameters are compile-time errors (parameters in the parser).

---

## Calls and return

A `CallExpr` that names a **top-level** `Function` symbol becomes `CALL dest, first, function_index` after checking arity against `FunctionInfo`.

A call through a **variable** or a **nested function** name becomes `CALL_VALUE dest, callee, argc` (callee in one register, arguments in the following registers). Top-level function values also use `LOAD_FUNCTION` when the identifier is used as a value (`f = add`).

Built-ins remain `CALL_BUILTIN` (`print`, `range`, `input`, `int`, `float`, `str`, `bool`, `type`, `delay`). Conversion and `type()` are resolved by name in `is_known_builtin`; no separate opcodes.

`return` emits `RETURN src`. That opcode leaves the function immediately (including from inside `if` / loops). Loop `break` / `continue` patching is unchanged.

**Automatic return:** after compiling all but the last top-level non-`FuncStmt`, if that statement is `ExprStmt`, its value is compiled and `RETURN`’d. Otherwise it is compiled normally and the compiler emits load-`null` + `RETURN`. Nested block expressions are not rewritten into returns. Nested function declarations are never treated as the automatic-return statement.

`for` item stores and match bindings use the same local/global/upvalue store helpers as other assignments.

**Type compatibility** (`type_compat.cpp`): `is_assignable(target, source)` drives assignment and call-site checks. `int`→`float` emits `CoerceToFloat`; `any`→typed emits `CheckAssign` with `TypeCheckInfo`. Class subtyping uses `is_class_or_subclass`. Parameter types are stored in `FunctionInfo.parameter_type_names` at `declare_function` time so top-level calls can validate arguments before function bodies are compiled.

**Generics (Bölüm 2):** `TypeDescriptor` carries `type_args` for `list<T>` and `map<K,V>`. Type strings are parsed in `compiler.cpp` (`parse_type_spec`). Collection literals get optional inferred element/value types when homogeneous; mixed literals stay untyped. `check_collection_literal` validates literals against declared generics; `check_method_argument_types` checks `append` / `prepend` / `insert` on typed lists. Generic compatibility is invariant (exact `type_args` match). Runtime list/dict layout is unchanged; `CheckAssign` walks nested `TypeCheckInfo.type_arg_indices` for `any`→generic assignments.

**Conversion + `type()` (Bölüm 3):** explicit conversion uses existing `CALL_BUILTIN` to `Runtime::convert_*`; `type()` returns a string via `Runtime::runtime_type_name`. Compiler static inference treats `type(...)` as `string`. Assignment compatibility remains separate from conversion.

**Exceptions (Bölüm 1–3):** `compile_try` emits `HandlerInfo` with multiple `CatchEntry` rows, `ENTER_TRY` / `LEAVE_TRY` / `END_FINALLY`, and per-catch `LEAVE_TRY` after each handler body. Typed catch uses `declare_variable(binding, type_name)` and `catch_class_index`; `validate_catch_clauses` checks reachability. `TryContext.has_finally` drives `DEFER_RETURN` / `DEFER_BREAK` / `DEFER_CONTINUE` from `try`/`catch` bodies. `compile_with_builtins` / CLI prepend the built-in exception class prelude (`message`, `cause`, `file`, `line`, `column`, `stack_trace`). `emit()` appends `instruction_locations` from `current_emit_loc_` adjusted by `CompileOptions.source_line_base`. `compile(std::string)` does not prepend the prelude (tests use explicit builtins when needed). `CompileOptions.source_file` is preserved across `Compiler::compile` reset.

**Destructuring assignment** (`DestructureAssignStmt`): RHS compiled once into a temporary register. Wildcard (`_`) skips binding. List/tuple rest (`*name`) must be last; tail bound via `DestructRest` (or `MakeList` for literal RHS) as a **List**. Dict patterns use `GetIndex` with string keys; dict rest via `DestructDictRest` into a **Dict**. Struct patterns use `GetField` with compile-time field resolution and access checks; struct rest compiles remaining public fields into a dict. Exact list/tuple patterns use `CheckDestruct` kinds `0`/`1`; rest patterns use minimum-count kinds `2`/`3`.

**Parameter destructuring**: each argument is stored in a hidden local (`__argN`) at function entry, then `compile_parameter_destructure()` reuses the same `compile_destructure_pattern()` path as assignment. Arity and call semantics are unchanged.

See `docs/language/destructuring.md`.

**Void trailing calls:** when the last top-level statement of a function body is an `ExprStmt` whose expression does not produce a value (for example `print(...)` or a call to a function whose body does not auto-return a value), the compiler emits that statement normally and returns `null` instead of auto-returning the call’s result. `FunctionInfo.returns_value` records this for mutual recursion analysis.

List, dict, tuple, and set literals emit `MAKE_LIST` / `MAKE_DICT` / `MAKE_TUPLE` / `MAKE_SET`. Index assignment on a statically known tuple or set is a compile error.

Struct and enum declarations are hoisted like functions. `TypeName()` emits `MAKE_STRUCT`. Field access emits `GET_FIELD` / `SET_FIELD` with compile-time field indices when the struct type is known. `Enum.MEMBER` emits `LOAD_ENUM`. Struct and enum names occupy symbol slots as types, not variables.

Struct field access metadata (`MemberAccess`: public, protected, private) is stored in `StructFieldInfo` and enforced at compile time for external access. `PassStmt` compiles to nothing (no extra opcodes).

---

## Classes and inheritance

Class compilation uses multiple passes:

1. **Predeclare** — register each class name (`SymbolKind::Class`) and record optional `parent_name`.
2. **Finalize** — resolve parent index, register own fields/methods/constructor function indices.
3. **Validate** — detect circular inheritance; rebuild flattened field layouts (parent-before-child order); validate parent constructor chaining rules.
4. **Compile bodies** — emit constructor/method bytecode after main code and top-level functions.

Field and method lookup walk the inheritance chain at compile time. Access checks use `declaring_class_index` and `is_class_or_subclass` for protected semantics. `super.greet()` / `parent.greet()` resolve the parent method and emit `CALL_CLASS_METHOD` with `SuperMethod`. `super(...)` / `parent(...)` in a child constructor emit `INVOKE_SUPER`.

`this` is lowered to `LOAD_LOCAL 0` inside class methods and constructors (`FunctionInfo.has_implicit_this`).

Class instances use `INVOKE_CLASS`; `GET_FIELD` / `SET_FIELD` reuse struct field indices against the flattened layout.

---

## Async / Await (Bölüm 1)

- Parser sets `FuncStmt.is_async`; `await` is parsed as a unary expression with `async_function_depth_` enforcement.
- `declare_function` stores `FunctionInfo.is_async`.
- `async_function_depth_` tracks whether `await` is legal; `compile_expr_into` on `AwaitExpr` emits **`AWAIT`** after compiling the operand (`StaticType::Task` or `any`).
- Async calls to known functions still emit `CALL` / `CALL_VALUE` / `CALL_CLASS_METHOD`; the VM spawns a `Task` when `fn.is_async && scheduler_active_`.
- `async func main()` sets `Bytecode.async_main_function_index` after the function pass.
- Async functions always have `returns_value = true` at compile time (explicit `return` last statements no longer force `returns_value = false`).
- Built-in `delay` is treated like other known builtins for inference (`StaticType::Task`).
