# Scope

PScript uses **lexical block scope**. A name is resolved by looking in the current block, then each enclosing block, then the global scope. This is done at compile time: each variable is a bytecode slot, not a runtime string lookup.

See also [Variables](variables.md).

---

## Global scope

Top-level statements live in the global scope.

```phs
x = 10

if true {
    print(x)
}
```

This prints `10`. The inner block can read `x`.

---

## Block scope

These constructs each introduce a scope for their body (and `for` also has a header scope for the loop variable):

- `if` / `elif` / `else` bodies
- `while` body
- `for` header + body
- each `match` case (including the binding, if any)
- `{ }` and `:` bodies attached to those headers
- a `func` body (parameters and top-level function locals share that function scope)

A name created only inside a block is not visible after the block.

```phs
x = 10

if true {
    y = 20
    print(y)
}

print(x)
```

`print(x)` is valid. `print(y)` after the `if` is a compile-time `undefined variable`.

Nested blocks see their parents:

```phs
x = 10

if true {
    y = 20

    if true {
        print(x)
        print(y)
    }
}
```

---

## Assignment vs declaration

**Assignment** (`x = …`, `x += …`, `x++`) updates the **visible** binding. If the name is not visible at all, the compiler introduces it in the **current** scope (inferred, not hoisted).

```phs
x = 10

if true {
    x = 20
}

print(x)
```

Prints `20`. There was no inner declaration, so the global `x` was updated.

**Explicit declaration** (`int x`, `x: int`, `int x = 20`, …) always creates a **new** variable in the current scope. It may shadow an outer name.

```phs
x = 10

if true {
    int x = 20
    print(x)
}

print(x)
```

Prints `20` then `10`.

A `for` item is an assignment target, not a typed declaration. If `item` already exists, the loop writes that variable (and it remains after the loop). If it does not exist, it is created in the `for` scope and does not leak.

---

## Same-scope duplicate declaration

```phs
int x = 10
int x = 20
```

Compile-time error: `duplicate declaration of 'x'`.

The same applies to mixing an inferred introduction and a later declaration in one scope (`x = 1` then `int x = 2` at the same level).

---

## Undefined variable

```phs
print(x)
```

Compile-time `undefined variable 'x'` with line and column. Names are not hoisted:

```phs
print(x)
x = 10
```

is also invalid.

---

## Functions

`func` names (including nested `func`) are hoisted in their lexical scope; ordinary variables are not. Parameters and locals live in the function’s local scope. A normal local dies with that call frame. If a nested function captures the binding, the cell lives as long as the closure. Recursive calls do not share uncaptured locals. Assignment vs declaration uses the same rules as everywhere else. Automatic return is only the last top-level expression of the function body; nested `func` declarations are not treated as that expression. Details: [Functions](functions.md).

Member access modifiers (`pub`, `prot`, `priv`) apply to struct and class fields, not to variable or function bindings. Class declarations are hoisted (pre-registered) before their bodies compile; a child class may appear before its parent in source when there is no circular dependency.

