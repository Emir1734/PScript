# Variables

A variable holds a value. You introduce one with `=` or with an explicit type declaration. Names follow [Scope](scope.md). Types are listed in [Types](types.md).

---

## Three forms

**Inference** — the type comes from the value:

```phs
x = 10
name = "Emir"
```

`x` is an `int`. `name` is a `string`.

**Prefix type:**

```phs
int x = 10
```

**Colon type:**

```phs
x: int = 10
```

Prefix and colon mean the same thing. Use whichever you prefer.

```phs
int score = 100
name: string = "Emir"
active = true

print(name, score, active)
```

Generic collection types use the same forms:

```phs
list<int> values = [1, 2, 3]
map<string, int> scores = {"Emir": 100}
list<int> empty = []
```

Untyped inference is unchanged: `values = []` does not pick an element type unless you annotate.

---

## Assignment

Write a new value with `=`:

```phs
int score = 100
score = 200
print(score)
```

This prints `200`. The explicit type stays `int`, so the new value must also be an `int`.

You can assign an expression, not only a literal:

```phs
int x = 10 + 20
float score = 10.5 + 2.5
print(x)
print(score)
```

---

## Explicit types

Allowed built-in type names: `int`, `float`, `bool`, `string`, `any`. User-defined struct, class, and enum type names are also allowed.

The only implicit conversion is **`int` → `float`** on assignment and typed call arguments. `float` → `int` requires `int(value)`. Other cross-kind changes use explicit built-ins (`float`, `str`, `bool`); there is no `as` or cast syntax. See [Types — Conversions](types.md#conversions).

```phs
int x = 10
float y = 10.5
bool ok = true
string name = "Emir"
```

Invalid:

```phs
int x = "Hello"
float y = true
string name = 10
bool ok = "true"
```

PScript reports a compile error such as:

```text
Type error: expected int, got string
```

The same check applies to a later assignment:

```phs
int score = 100
score = "hello"
```

This is a type error. `score` was declared as `int`.

`str` is not an alias for `string`. `null` is a value, not a variable type — you cannot write `null x = null`.

Unknown type names are compile errors (`Type error: unknown type '…'`).

---

## Inferred variables

If you omit the type, later assignments may change the stored kind:

```phs
x = 10
print(x)
x = "hello"
print(x)
```

This is allowed. First print is `10`, second is `hello`.

Once a name has an explicit type (other than `any`), it does not behave this way. Inference is not a union type system; it only records the latest assigned kind until you add an explicit type.

`any` is different: `any x = 10` then `x = "hello"` is allowed. See [Types](types.md#any).

---

## Declaration without a value

A typed name may be declared without `=`. The variable exists and starts as `null`. The declared type is kept for later assignments.

```phs
int score
string name

score = 100
name = "Emir"

print(score)
print(name)
```

This prints `100` then `Emir`. Before those assignments, `print(score)` writes `null`.

The next value must still match the declared type:

```phs
int score
score = "hello"
```

This is a compile-time type error (`expected int, got string`).

You cannot write `int score = null` to mean “uninitialized”; assigning the `null` literal to an `int` is also a type error. The implicit `null` is only the starting value of a typed declaration without `=`.

---

## Scope

Lexical block scope is described in [Scope](scope.md). Short rules:

- Assignment updates a visible name, or introduces the name in the current block if it is new.
- `int x` / `x: int` always declares in the current block and may shadow an outer `x`.
- The same name cannot be declared twice in one block.
- A block-local name is not visible after the block. Variables are not hoisted (`print(x)` then `x = 1` is invalid).
- Function parameters and function-locals are not visible outside the function. See [Functions](functions.md).

---

## null

`null` is a value you can assign and print:

```phs
value = null
print(value)
```

This prints `null`. It is not a type you put on a variable declaration. See [Types](types.md).
