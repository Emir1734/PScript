# Register bytecode

Instructions are `{ opcode, a, b, c }` with dest-first encoding for most ops. This is register bytecode, not a stack machine.

---

## Modules in a `Bytecode` object

- `constants` — interned literals
- `variables` — **global** slots only
- `calls` — built-in / method call metadata (`CallInfo { name, argc }`)
- `functions` — user functions (`FunctionInfo`: name, arity, entry IP, local_count, register_count, `captures`, `is_async`, `returns_value`)
- `structs` — struct type metadata (`StructInfo`: name, fields with optional type names)
- `classes` — class type metadata (`ClassInfo`: name, `parent_index`, own fields, flattened `layout`, methods, `constructor_index`)
- `class_calls` — class instantiation and method dispatch metadata (`ClassCallInfo`: kind, class index, function index, argc)
- `enums` — enum type metadata (`EnumInfo`: name, member names)
- `instructions`
- `instruction_locations` — parallel `SourceLocation { line, column }` per instruction (user line numbers exclude prelude via `source_line_base`)
- `source_file` — filename for stack traces (`<stdin>` default)
- `source_line_base` — prelude line offset subtracted at emit time
- `register_count` — maximum register file size used for validation (max of main and all functions)
- `modules` — import graph metadata (`ModuleDescriptor`: specifier, canonical path, `bytecode_index`, `exports[]`)
- `module_bytecodes` — compiled bytecode for each loaded user module (top-level + functions/classes)

`ModuleExport` records `{ name, ExportKind, index }` for top-level variables, functions, and classes.

---

## Import opcodes (Modules)

| Opcode | Encoding | Meaning |
| ------ | -------- | ------- |
| `IMPORT_MODULE` | `dest, module_index` | Initialize module (if needed) and store a `ValueKind::Module` namespace |
| `IMPORT_NAME` | `dest, module_index, const_index` | Initialize module (if needed) and copy one export (name string in `constants`) |

Module bytecode may contain these opcodes with indices into the **root** program's `modules` table; standalone module bytecode validation skips module-index bounds checks when `modules` is empty.

`ClassInfo` may carry `module_index` and `external_index` when a class is linked from an imported module into the importer bytecode (cross-module inheritance / typed imports).

---

## Function-related opcodes

| Opcode | Encoding | Meaning |
| ------ | -------- | ------- |
| `CALL` | `dest, first, function_index` | Arguments occupy consecutive caller registers `first .. first+arity-1`. |
| `CALL_VALUE` | `dest, callee, argc` | Call the function value in `callee`; arguments occupy `callee+1 .. callee+argc`. |
| `RETURN` | `src` | Yield the value in `src` and restore the caller frame. |
| `LOAD_LOCAL` | `dest, local_index` | Read a cell in the current function frame. |
| `STORE_LOCAL` | `local_index, src` | Write a cell in the current function frame. |
| `LOAD_UPVALUE` | `dest, upvalue_index` | Read a captured cell. |
| `STORE_UPVALUE` | `upvalue_index, src` | Write a captured cell. |
| `MAKE_CLOSURE` | `dest, function_index` | Build a function value; copy captured cells from the current frame. |
| `LOAD_FUNCTION` | `dest, function_index` | Build a function value with an empty capture list (top-level `func`). |
| `AWAIT` | `dest, task_reg` | If `task_reg` is pending, suspend the current async task; otherwise write result into `dest` or throw stored exception. |
| `MAKE_LIST` | `dest, first, count` | List value from consecutive registers. |
| `MAKE_TUPLE` | `dest, first, count` | Immutable tuple value. |
| `MAKE_SET` | `dest, first, count` | Set value; duplicates dropped. |
| `MAKE_DICT` | `dest, first, pair_count` | Dict value; pairs of key/value registers. |
| `CheckDestruct` | `collection, expected_count, kind` | Validate runtime shape. `kind` `0`/`1` = list/tuple exact count; `2`/`3` = list/tuple minimum count (rest patterns). |
| `DestructRest` | `dest, source, start_index` | Copy tail from index `start_index` into a new `List` (list or tuple source). Used for `*rest`. |
| `DestructDictRest` | `dest, source, packed` | Build a new `Dict` from `source` excluding keys in the packed register list. Used for dict `*rest`. |
| `CoerceToFloat` | `dest, src` | Convert `int` to `float` (in-place when `dest == src`). |
| `CheckAssign` | `dest, src, type_check_index` | Runtime assignability check using `Bytecode.type_checks[type_check_index]`. For generic list/dict targets, nested element/key/value checks use `TypeCheckInfo.type_arg_indices`. |
| `MAKE_STRUCT` | `dest, struct_index` | Struct instance; fields start as `null`. |
| `GET_FIELD` | `dest, object, field_index` | Read a struct field by index. |
| `SET_FIELD` | `object, field_index, value` | Write a struct field by index. |
| `LOAD_ENUM` | `dest, enum_index, member_index` | Load an enum member value. |
| `INVOKE_CLASS` | `dest, first_arg, class_call_index` | Create a class instance (flattened field layout); optionally enter constructor. |
| `CALL_CLASS_METHOD` | `dest, receiver, class_call_index` | Call a class method; arguments follow `receiver` in registers. |
| `INVOKE_SUPER` | `first_arg, class_call_index` | Call parent constructor on existing `this` (local 0). |

`ClassInfo.layout` is the flattened inheritance chain (parent fields first, then own fields). Each `ClassFieldLayout` records `name`, `access`, and `declaring_class_index`. Method dispatch uses `function_index` into `Bytecode.functions`, not a per-class method table index at runtime.

`ClassCallKind`: `Instance`, `Method`, `SuperMethod`, `SuperCtor`.

Existing `LOAD_VAR` / `STORE_VAR` still address global `variables`. `CALL_BUILTIN` / `CALL_METHOD` are unchanged. Built-in names in `Bytecode.calls` include `int`, `float`, `str`, `bool`, `type`, `range`, `input`, and `print`; conversion and `type()` do not add dedicated opcodes.

### Exception opcodes (Bölüm 1–3)

| Opcode | Encoding | Meaning |
| ------ | -------- | ------- |
| `ENTER_TRY` | `handler_index` | Push `HandlerFrame` on VM handler stack |
| `LEAVE_TRY` | `handler_index` | Pop handler on normal try/catch completion; jump to `finally_ip` when present |
| `THROW` | `src` | Unwind using `Bytecode.handlers[handler_index]` metadata |
| `END_FINALLY` | — | Re-throw `pending_exception` or complete `pending_control_` |
| `DEFER_RETURN` | `handler_index`, `src` | Defer return through `finally` |
| `DEFER_BREAK` | `handler_index`, `jump_index` | Defer loop break through `finally` |
| `DEFER_CONTINUE` | `handler_index`, `jump_index` | Defer loop continue through `finally` |

`HandlerInfo` stores `catches[]` (`CatchEntry`: `catch_ip`, `catch_slot`, `catch_is_local`, `catch_class_index`), `finally_ip`, and `end_ip`. `catch_class_index == 0xFFFFFFFF` means untyped catch.

`Capture` is `{ index, from_local }`: `from_local` true means `index` is a parent local slot; false means a parent upvalue index.

Function bodies are emitted after the main `HALT`. Nested function code sits immediately before the enclosing function’s `FunctionInfo.entry`. `MAKE_CLOSURE` is the first work at `entry`.

Automatic return does not add opcodes: the compiler emits a normal `RETURN` of the last top-level expression (or `null`).

---

## Async / Await (Bölüm 1)

- `FunctionInfo.is_async` — async callees are spawned as `Task` values instead of synchronous `CALL` entry when the scheduler is active.
- `Bytecode.async_main_function_index` — index of `async func main()` when present (`0xFFFFFFFF` otherwise).
- **`AWAIT dest, task_reg`** — suspend if needed; no blocking spin loop.
- Built-in **`delay`** is recorded in `calls` like other builtins and returns a timer `Task` at runtime.

Suspend/resume metadata lives in runtime `TaskData` / `AsyncExecutionState`, not in extra bytecode tables.
