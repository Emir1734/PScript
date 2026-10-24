# Destructuring

Destructuring unpacks a value into named targets. The right-hand side (or function argument) is evaluated **once**. Tuple values stay immutable; destructuring only copies elements or fields into variables.

**Bölüm 1:** basic list/tuple/nested patterns, exact element counts.

**Bölüm 2:** `_` wildcard and `*rest` rest patterns for list/tuple.

**Bölüm 3:** dict, struct, and function-parameter destructuring (this document completes the topic).

See also [Variables](variables.md), [Scope](scope.md), [Collections](collections.md), [Structs and enums](structs-enums.md), and [Functions](functions.md).

---

## List destructuring

```phs
[a, b, c] = [10, 20, 30]
print(a, b, c)
```

Output: `10 20 30`.

With a runtime value:

```phs
func pair():
    return [1, 2]

[x, y] = pair()
print(x, y)
```

---

## Tuple destructuring

```phs
(a, b) = (10, 20)
(a, b, c) = (1, 2, 3)
```

Parentheses are required for tuple patterns (same as tuple literals).

---

## Nested destructuring

```phs
[a, (b, c)] = [10, (20, 30)]
print(a, b, c)
```

The shape of the pattern must match the shape of the value at each level.

---

## Wildcard (`_`)

Use `_` in a pattern to ignore a value. Wildcard slots do **not** create variables, symbols, or slots, and no value is read or stored for them.

```phs
[_, b, _] = [10, 20, 30]
print(b)
```

```phs
(_, b) = (10, 20)
print(b)
```

Nested:

```phs
[a, (_, c)] = [10, (20, 30)]
```

Outside destructuring patterns, `_` is still a normal identifier if you declare it (`_ = 1`). Inside a pattern, `_` always means wildcard.

---

## Rest (`*name`)

Rest captures remaining elements into a new **list** value. Rest is only allowed as the **last** slot in a list or tuple pattern, and at most **one** rest per pattern.

```phs
[a, *rest] = [1, 2, 3, 4]
print(a)
print(rest)
```

Output:

```text
1
[2, 3, 4]
```

```phs
[a, b, *rest] = [1, 2, 3, 4, 5]
```

→ `rest` is `[3, 4, 5]`.

### Rest result type

**List/tuple rest always produces a `List`**, even when destructuring a tuple:

```phs
(a, *rest) = (1, 2, 3, 4)
print(rest)
```

Output: `[2, 3, 4]`.

### Empty rest

```phs
[a, *rest] = [1]
print(rest)
print(rest.length())
```

Output:

```text
[]
0
```

### Exact match vs rest match

| Pattern | RHS | Result |
| ------- | --- | ------ |
| `[a, b]` | `[1, 2, 3]` | `destructuring mismatch` (exact count) |
| `[a, *rest]` | `[1, 2, 3]` | OK — `a = 1`, `rest = [2, 3]` |
| `[a, b]` | `[1]` | `destructuring mismatch` |

With rest, the source must have **at least** as many elements as fixed slots (identifiers, wildcards, and nested patterns) before the rest. Extra elements go to rest.

### Nested rest

```phs
[a, (b, *rest)] = [10, (20, 30, 40)]
print(a)
print(b)
print(rest)
```

Output:

```text
10
20
[30, 40]
```

---

## Dict destructuring

Dict patterns use **string keys** only (matching PScript’s dict design). Element order in the pattern does not matter; bindings are resolved by key.

```phs
{"name": name, "age": age} = person
```

If `person` is a dict, this binds `name` from `person["name"]` and `age` from `person["age"]`.

### Wildcard

```phs
{"name": _, "age": age} = person
```

### Missing key

If a required key is absent at runtime, the program reports a controlled destructuring error (`missing dict key`).

### Nested dict

```phs
{"user": {"name": name, "age": age}} = data
```

### Dict rest (`*rest`)

```phs
{"name": name, *rest} = person
```

`*rest` must be last. It binds a **new `Dict`** containing all key/value pairs not matched by earlier entries in the pattern. At most one rest per dict pattern.

```phs
person = {"name": "Emir", "age": 30, "city": "Istanbul"}
{"name": name, *rest} = person
print(name)
print(rest)
```

Output:

```text
Emir
{"age": 30, "city": "Istanbul"}
```

Dict rest uses the same `*name` token as list/tuple rest; the result type is always a `Dict` in dict patterns.

---

## Struct destructuring

Struct patterns use the struct type name and field names:

```phs
Player{name, age} = player
```

Fields are matched by **name**, not position. The struct type in the pattern must match the value’s type.

### Nested struct

```phs
Player{name, address: Address{city}} = player
```

Use `field: nested_pattern` when the field itself should be destructured.

### Access modifiers

`private` and `protected` fields cannot be read from outside the struct type, including via destructuring. Attempting to bind such a field is a compile-time error (`inaccessible struct field`).

### Unknown field

Referencing a field name that does not exist on the struct type is a compile-time error (`unknown struct field`).

### Struct rest

```phs
Player{name, *rest} = player
```

`*rest` binds a **new `Dict`** of remaining **public** fields not named earlier in the pattern. Private/protected fields are not included in rest.

---

## Function parameter destructuring

Parameters may be destructuring patterns. The caller still passes one argument per parameter; the callee destructures that argument when the frame is set up.

```phs
func add((a, b)):
    a + b

add((10, 20))
```

`add((10, 20))` passes a single tuple argument; `a` and `b` are function-local bindings.

List parameter:

```phs
func first([a, b]):
    a

first([1, 2])
```

Dict parameter:

```phs
func greet({"name": name}):
    print(name)

greet({"name": "Emir"})
```

Struct parameter:

```phs
struct Point:
    int x
func read_x(Point{x}):
    return x
```

Nested parameter:

```phs
func test([a, (b, c)]):
    a + b + c
```

Wildcard and rest follow the same rules as assignment patterns. Rest in a parameter list:

```phs
func tail([a, *rest]):
    rest.length()
```

### Arity and call semantics

Function call arity is unchanged: one destructuring parameter still counts as **one** parameter. Arguments are evaluated under normal call rules, then each argument value is destructured once inside the callee frame. Bindings are placed in the function’s local scope/slots.

Duplicate names within one parameter pattern are rejected at compile time (`duplicate binding`).

---

## Rules and errors

| Rule | Error |
| ---- | ----- |
| Without rest: left and right element counts must match (list/tuple) | `destructuring mismatch` |
| With rest: source length ≥ fixed slot count (list/tuple) | `destructuring mismatch` |
| RHS must be list (list pattern) or tuple (tuple pattern) | `cannot destructure value` |
| Dict key missing at runtime | `missing dict key` |
| Unknown struct field | `unknown struct field` |
| Private/protected struct field in pattern | `inaccessible struct field` |
| Rest not last, or more than one rest | `rest pattern must be last…` / `multiple rest patterns…` |
| Invalid pattern in parameter position | `invalid parameter pattern` |
| Duplicate names in one pattern | `duplicate binding` |
| `*_` or other invalid rest form | `invalid destructuring pattern` |

Literal list/tuple RHS: count rules checked at compile time when possible. Dynamic RHS: `CheckDestruct` plus `DestructRest` / `DestructDictRest` as needed.

---

## Scope

Each **non-wildcard** identifier in a pattern uses normal assignment rules (`resolve_assign`). In functions, destructured names are function-local.

```phs
outer = 0
if true:
    [x, y] = [3, 4]
    outer = x + y
print(outer)
```

```phs
func test((a, b)):
    print(a + b)
```

`a` and `b` are locals of `test`, not globals.

---

## Eval-once

Assignment:

```phs
{"x": x} = get_data()
```

`get_data()` runs once.

Function call:

```phs
func test([a, b]):
    a + b

test(get_data())
```

`get_data()` runs once for that argument. The same guarantee applies to nested patterns.

---

## Newlines and following lines

PScript does **not** end an expression at a newline. Put destructuring on its own statement group or inside a block when needed.

---

## What is not supported

- Destructuring in expression position (only assignment statements and parameters)
- Rest in non-final position (`[*rest, a]`, `{*rest, "x": x}`)
- Multiple rest slots in one pattern
- Non-string dict keys in patterns
- Destructuring `class` instances (struct/record only)

Destructuring is **complete** after Bölüm 3. The next roadmap topic is **Exceptions / Error Handling**.
