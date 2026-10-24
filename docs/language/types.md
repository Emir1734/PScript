# Types

PScript values have one of these kinds. Variable annotations use built-in type names plus `any`, struct types, class types, and enum types; `null` is only a value. Lists, dictionaries, tuples, sets, and ranges are values, not annotation names. How you attach types to names is in [Variables](variables.md).

---

## any

The `any` type is fully dynamic. A variable declared as `any` can hold any PScript value; the runtime `ValueKind` of the stored value is preserved (no wrapper type).

```phs
any x = 10
x = "hello"
x = true
x = null
```

**Inference vs explicit `any`**

```phs
x = 10        # inferred as int; later `x = "hello"` is a type error
any x = 10    # declared any; later `x = "hello"` is allowed
```

Assigning an `any` value to a typed variable uses a **runtime** compatibility check on the actual value.

---

## Generic collections (`list<T>`, `map<K,V>`)

PScript supports **static** generic annotations for lists and dictionaries. Canonical syntax uses lowercase names:

```phs
list<int> scores = [10, 20, 30]
list<string> names = ["a", "b"]
map<string, int> table = {"Emir": 100}
```

Aliases `List<T>`, `Dict<K,V>`, and `dict<K,V>` parse as `list` / `map`. Runtime `List` and `Dict` values are unchanged — generics exist for **compile-time** checking only.

**Nested generics**

```phs
list<list<int>> matrix = [[1, 2]]
map<string, list<int>> rows = {"a": [1, 2]}
```

**Rules**

| Topic | Behavior |
| ----- | -------- |
| Compatibility | Invariant: `list<int>` is not assignable to `list<string>` |
| Empty typed collections | `list<int> x = []` and `map<string,int> x = {}` are valid |
| Untyped `x = []` | Still allowed; no element type is inferred without a target |
| Literal inference | `[1,2,3]` → `list<int>`, `["a"]` → `list<string>`, `[1,2.5]` → `list<float>` |
| Mixed literals | `[1, "a"]` stays an untyped `list` (no homogeneous element type) |
| Map keys | `K` must be `string` (matches runtime string-key dict rule) |
| `any` → generic | Compile OK; runtime `CheckAssign` validates elements/values |
| Tuple / set / union / task generics | `tuple<int,string>`, `set<int>`, `int\|string`, `task<int>` (static checking; runtime uses existing value kinds) |
| User generic types | `struct Pair<T,U>`, `class Box<T>` with `Pair<int,string>` / `Box<int>` in annotations (compile-time only; runtime values are unchanged) |

`<` and `>` in type annotations are parsed only in **type contexts** (declarations, parameters, struct fields). Expression comparisons such as `a < b` are unchanged. Adjacent closing brackets use the `>>` lexer rule split in type context (`list<list<int>>`).

---

## Type compatibility

Compatibility is not the same as exact type equality.

| Rule | Example |
| ---- | ------- |
| Same primitive / collection kind | `int x = 10` |
| `int` → `float` (implicit, safe) | `float x = 10` |
| `float` → `int` | **not** implicit; use `int(value)` |
| Child class → parent class variable/param | `Animal a = Dog()` |
| Parent → child | **invalid** |
| Same struct / enum type | `Player p = Player()` |
| Different struct / enum types | **invalid** |
| Any target | any value assignable |
| Any source → typed target | runtime check on actual value |

Class subtyping follows the inheritance chain (multi-level). Struct and enum identity require the exact type.

### Async / Task (Bölüm 1–2)

Async functions use **`task`** / **`task<T>`** return annotations (e.g. `async func load() -> int` is checked as `task<int>`). `await` on a `task<T>` value is checked as `T` when the static type is known.

- Calling an async function infers/produces `Task`.
- `await expr` checks that `expr` is a `Task` (or `any` with runtime validation).
- Assigning a `Task` to a non-task typed variable follows normal compatibility rules (`any` accepts anything; exact typed targets require a matching static type).
- Runtime task states (`Pending`, `Running`, `Completed`, `Failed`) are internal; user code interacts through `Task` values, `await`, and exception propagation.

---

## int

A 64-bit signed integer.

```phs
count = 10
int total = 0
print(count)
```

**Rules**

- Literal: one or more decimal digits (`0`, `10`, `007`).
- No hex, binary, exponents, or digit separators.
- Leading zeros are allowed; the value is still decimal (`007` is `7`).
- `int` arithmetic stays `int` when both sides are `int` (including `/`, which truncates toward zero).
- Bitwise operators (`&` `|` `^` `~` `<<` `>>`) require `int`.

```phs
print(8 / 2)
print(7 / 2)
```

Output:

```text
4
3
```

---

## float

A floating-point number (IEEE-754 double precision).

```phs
float score = 92.5
print(score)
print(1 + 2.5)
```

Output:

```text
92.5
3.5
```

**Rules**

- Literal form: `digits.digits`. Both the integer part and the fraction need at least one digit.
- `0.5` and `10.0` are valid. `.5` and `1.` are not.
- Exponent notation (`1e3`) is not supported.
- If either operand of `+`, `-`, `*`, or `/` is a `float`, the result is a `float`.
- An `int` literal may be assigned to a `float` variable (`float x = 10` coerces to `10.0` at runtime). Use a float literal such as `10.0` when you want a float literal directly.

---

## bool

Boolean `true` or `false`. The names are lowercase only.

```phs
bool active = true
print(active)
print(1 < 2)
```

Output:

```text
true
true
```

**Rules**

- `True` and `FALSE` are identifiers, not bool literals.
- Comparison operators produce `bool`. See [Operators](operators.md).
- `bool ok = "true"` is a type error. The string `"true"` is not a bool. Use `bool("true")` for conversion.

---

## string

Text in double quotes.

```phs
string name = "Emir"
print("Hello")
print("Score:", name)
```

**Rules**

- Single quotes are not strings.
- A real newline inside quotes is an error (unterminated string).
- Escapes: `\"` `\\` `\n` `\t` `\r`. Other escapes are invalid.
- `+` does not concatenate strings. `"a" + "b"` is a runtime error.
- String methods (`find`, `contains`, `replace`, `starts_with`, `ends_with`, `first`, `last`, `is_empty`, `length`) are documented in [Collections](collections.md).

```phs
print("line 1\nline 2")
print("say \"hi\"")
```

---

## null

The missing value. Written `null`.

```phs
value = null
print(value)
```

This prints `null`.

**Rules**

- `null` is not a variable type name.
- `Null` (capital N) is an identifier, not this literal.
- `==` / `!=` can compare `null` with `null`. Ordering (`<`, `>`) does not apply to `null`.

---

## Conversions

The only implicit conversion between kinds is **`int` → `float`** on assignment and typed function arguments. These built-ins convert explicitly for other cases:

| Call | Result |
| ---- | ------ |
| `int(value)` | `int` |
| `float(value)` | `float` |
| `str(value)` | `string` (same text `print` would write for that value) |
| `bool(value)` | `bool` |
| `type(value)` | `string` (runtime kind name; see below) |

There is no cast syntax (`value as int`, `(int)value`). Use the built-in calls above.

`str` is **not** a variable type name. Declarations still use `string`.

```phs
age = int(input("Age: "))
price = float(input("Price: "))
active = bool(input("Active: "))
label = str(10)
```

**`int`**

- `int` / `float` (truncates toward zero) / `bool` (`true` → `1`, `false` → `0`).
- String: trimmed decimal digits (`" 12 "` → `12`). Empty or leftover characters: `cannot convert string to int`.
- Other kinds: `cannot convert <kind> to int`.

**`float`**

- `int` / `float` / `bool` (`true` → `1.0`).
- String: trimmed `std::stod` form. Invalid: `cannot convert string to float`.
- Other kinds: `cannot convert <kind> to float`.

**`bool`**

- `bool`: unchanged.
- `int` / `float`: non-zero is `true`.
- `null`: `false`.
- list / dict / range: same as truthiness (empty is `false`).
- string (trimmed, ASCII case-insensitive): `true` / `1` → `true`; `false` / `0` / empty → `false`. Any other string: `cannot convert string to bool`.

**`input`**

Always returns a `string` (the line, without newline). Optional prompt must be a string. End of stream: `end of input`. Extra arguments or a non-string prompt are runtime errors.

---

## `type()`

`type(value)` returns a **string** naming the runtime kind of the value. It does not inspect compile-time generic annotations (`type(list<int> x)` is still `"list"`).

| Value kind | `type(...)` result |
| ---------- | ------------------ |
| integer | `"int"` |
| float | `"float"` |
| string | `"string"` |
| bool | `"bool"` |
| null | `"null"` |
| list | `"list"` |
| dict | `"dict"` |
| tuple | `"tuple"` |
| set | `"set"` |
| struct | `"struct"` |
| class | `"class"` |
| enum | `"enum"` |
| range | `"range"` |
| function | `"function"` |

```phs
print(type(10))           # int
print(type([1, 2, 3]))    # list
list<int> xs = [1, 2, 3]
print(type(xs))           # list (not list<int>)
```

Conversion and assignment compatibility are separate: `type()` reports what is stored, not what a variable was annotated with.

---

## What print writes

| Value | Output |
| ----- | ------ |
| integer | decimal digits, e.g. `10` |
| float | decimal form, e.g. `1.5` |
| bool | `true` or `false` |
| string | the text, without quotes |
| null | `null` |
| list | `[10, 20]` |
| dictionary | `{"name": Emir}` |
| tuple | `(10, 20)` or `(10,)` |
| set | `{1, 2, 3}` |
| struct | `Player{name: Emir, number: 10}` |
| enum | `Color.RED` |
| range | `range(0, 10, 1)` |

Lists, dictionaries, tuples, sets, and ranges: [Collections](collections.md). Structs and enums: [Structs and enums](structs-enums.md). Ranges: [Control flow](control-flow.md).

---

## struct / record

User-defined struct types are declared with `struct` or `record` (equivalent). Values are created with `TypeName()`, fields are accessed with dot notation, and equality is structural within the same type. See [Structs and enums](structs-enums.md).

---

## enum

User-defined enum types expose members as `Type.MEMBER`. Members keep enum identity (not plain strings). See [Structs and enums](structs-enums.md).

---

## Exceptions and types

`throw` carries any runtime `Value`; there is no separate exception `ValueKind`. Typed exceptions are ordinary class instances (`Exception` hierarchy and user subclasses).

**Built-in hierarchy** (CLI / `compile_with_builtins`): `Exception` → `RuntimeException` → `ValueException`, `TypeException`, `NameException`, `IndexException`. Fields: `message`, `cause`.

**Typed catch** — `catch TypeName binding:` uses compile-time class metadata; matching is inheritance-aware at runtime. Untyped `catch binding:` matches any thrown value.

**Chaining** — store a caught exception in another’s `cause` field via constructors (`MyError("msg", error)`).

Runtime failures are auto-wrapped into the built-in exception hierarchy (Bölüm 3). Exception objects expose `message`, `cause`, `file`, `line`, `column`, and `stack_trace` without a separate exception `ValueKind`.

---

## Not in this version

Classes are reference object types with inheritance; see [Classes](classes.md). Access modifiers apply to struct and class fields. There is no nullable type syntax. Generic annotations apply to `list` and `map` only (not `tuple<T>` or `set<T>`). `str(...)` converts to a string value; it is not a declaration type. Runtime values do not carry generic type parameters — `type()` returns the collection kind (`list`, `dict`), not `list<int>`.
