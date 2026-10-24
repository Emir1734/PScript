# Postfix `?` and `Result` (PScript 1.0)

## Status (implemented)

PScript 1.0 provides **typed `Result` values** and **postfix `?` error propagation** on top of the existing exception model.

### `Result` enum (prelude)

Injected via `exception_prelude_source()`:

```text
enum Result:
    Ok(value)
    Err(error)
```

Construct with `Result.Ok(payload)` / `Result.Err(payload)`. Inspect with `match` (or any existing enum APIs). **Falsy values are not failures.**

### Postfix `?`

- Parsed in `parse_postfix()` as `PropagateExpr`, disambiguated from ternary `?` … `:` via `looks_like_ternary_question()` (colon at the same source line, respecting nesting).
- Lowered in the compiler with `TestEnumMember` / `Return` / `GetEnumPayload`: if the operand is `Result.Err`, return that value from the **current function**; otherwise the operand’s payload is the expression value.
- **Only valid inside a function** (`in_function_`); otherwise compile error.
- Does not interact with `throw` / `try` / `catch` except that propagated `Err` values are normal return values, not exceptions.

### Compatibility

- Ternary `cond ? then : else` unchanged.
- Null-coalescing `??` unchanged.

### Tests

- `tests/additional_improvements_test.cpp` — unwrap, propagation, nesting, ternary/`??` compatibility, invalid top-level `?`.

### JIT

Propagation uses existing enum opcodes; JIT may deopt on those paths. VM semantics are authoritative.

## Prior deferral rationale (historical)

Earlier assessment noted the lack of a `Result` type and parser ambiguity. Those blockers are addressed for 1.0 as above; full Rust-style `?` on `Option` or automatic exception-to-`Result` conversion is **out of scope**.
