# Control flow

PScript runs conditional code with `if`, `elif`, `elseif`, and `else`, pattern matching with `match`, and loops with `while` and `for ... in`. Each of these has a **header** and a **body** (one or more statements). `match` also has a body per case.

There are two ways to write a body:

- **Indentation block** — after `:`, the body is on the next lines, indented further than the header.
- **Brace block** — after `{`, the body is indented until `}`.

Use one form per header. Do not mix `:` and `{` in the same block header (`if x: { }` and `while x: { }` are invalid).

Use spaces for indent. A tab at the start of a line is an indent error.

The lexer does not emit `INDENT` / `DEDENT` tokens. `:` or `{` / `}` mark the block. Indentation is still checked: a `:` body must start on the next line, further indented than the header, and `{ }` bodies must be indented inside the braces.

---

## if

Every conditional chain starts with `if`. The condition is any expression. If it is truthy, the body runs.

```phs
if score > 90:
    print("A")
```

Valid — comparison with optional parentheses:

```phs
if (score > 90):
    print("A")
```

Valid — non-boolean condition (see [Truthy / Falsy](#truthy--falsy)):

```phs
if score:
    print("non-zero")
```

Invalid — missing condition:

```phs
if:
    print("A")
```

Invalid — missing block:

```phs
if score > 90
```

---

## elif

`elif` adds another branch. It is checked only when every earlier branch was skipped. Like `if`, it takes a condition and a body.

`elif` must line up with the `if` it belongs to (same indent column). In brace form, `elif` may follow `}` on the same line.

```phs
if score > 90:
    print("A")
elif score > 70:
    print("B")
```

Valid — expression condition:

```phs
if false:
    print("skip")
elif ((score - 5) / 3):
    print("B")
```

Invalid — `elif` without a leading `if`:

```phs
elif score > 70:
    print("B")
```

Invalid — misaligned `elif`:

```phs
if score > 90:
    print("A")
  elif score > 70:
    print("B")
```

---

## elseif

`elseif` is an alternative spelling of `elif`. It behaves the same way.

```phs
if score > 90:
    print("A")
elseif score > 50:
    print("C")
```

You may mix `elif` and `elseif` in one chain:

```phs
if score > 90:
    print("A")
elif score > 70:
    print("B")
elseif score > 50:
    print("C")
else:
    print("F")
```

`else if` (two words) is **not** valid:

```phs
if score > 90:
    print("A")
else if score > 70:
    print("B")
```

---

## else

`else` is optional. It has no condition. It runs when every earlier branch was skipped.

```phs
if score > 90:
    print("A")
else:
    print("F")
```

`else` must align with its `if`, or follow `}` on the same line in brace form.

Invalid — standalone `else`:

```phs
else:
    print("F")
```

---

## while

`while` repeats its body while the condition is truthy. The condition uses the same expression parser as `if`. Parentheses around the condition are optional.

```phs
x = 0
while x < 3:
    print(x)
    x = x + 1
```

Output:

```text
0
1
2
```

Valid — parentheses and a brace body:

```phs
while (x < 10) {
    print(x)
    x = x + 1
}
```

Valid — a full expression as the condition:

```phs
while ((x + 2) * y) < (z - 5):
    print(x)
```

If the condition is already falsy, the body never runs.

Invalid — missing condition:

```phs
while:
    print(x)
```

---

## for ... in

`for item in collection:` assigns `item` to each element of the collection, in order, then runs the body.

This version iterates **lists** and **`range()`** values. Dictionaries, strings, and other values are a runtime error (`cannot iterate …`).

```phs
numbers = [1, 2, 3]

for number in numbers:
    print(number)
```

Parentheses around the `item in collection` header are optional:

```phs
for (item in items):
    print(item)
```

Brace body:

```phs
for item in items {
    print(item)
}
```

The loop variable is an assignment target. If that name is already visible, the loop updates it and it still exists after the loop. If it is new, it belongs to the `for` scope and does not leak. Details: [Scope](scope.md).

An empty list or empty range skips the body.

Invalid:

```phs
for x:
    print(x)

for in items:
    print(x)

for x in:
    print(x)
```

---

## range()

`range` is a built-in function, not a keyword. It returns a range value (not a list). `for` walks it without building a list of every integer.

| Call | Values |
| ---- | ------ |
| `range(10)` | `0` … `9` |
| `range(2, 10)` | `2` … `9` |
| `range(2, 10, 2)` | `2`, `4`, `6`, `8` |
| `range(10, 0, -2)` | `10`, `8`, `6`, `4`, `2` |

The stop argument is exclusive. `step` may be negative. `step == 0` is a runtime error (`range step cannot be zero`). Arguments must be integers. One, two, or three arguments are required.

```phs
for i in range(10):
    print(i)
```

Printing a range shows `range(start, stop, step)`:

```phs
print(range(2, 10, 2))
```

```text
range(2, 10, 2)
```

An empty range (for example `range(5, 1)` with a positive step) is falsy, like an empty list.

---

## pass

`pass` is a no-op statement. Use it when a block must contain at least one statement but should do nothing.

```phs
if debug:
    pass

func placeholder():
    pass
```

`pass` is not an expression. A function whose body is only `pass` returns `null` under automatic-return rules.

---

## break

`break` ends the innermost loop. Nested loops: only the inner loop stops.

```phs
x = 0
while x < 10:
    if x == 5:
        break
    print(x)
    x = x + 1
```

Outside a loop, `break` is a compile-time error (`break outside loop`).

---

## continue

`continue` skips the rest of the current innermost-loop iteration and starts the next one.

```phs
x = 0
while x < 4:
    x = x + 1
    if x == 2:
        continue
    print(x)
```

In `for`, `continue` still advances to the next element.

Outside a loop, `continue` is a compile-time error (`continue outside loop`).

Combined example:

```phs
for i in range(10):
    if i == 3:
        continue

    if i == 7:
        break

    print(i)
```

Output:

```text
0
1
2
4
5
6
```

---

## match

`match` is a **statement** and a **value-producing expression**. Cases are tried **top to bottom**. The **first** matching case runs. There is **no fallthrough**: after a case body finishes, the rest of the `match` is skipped.

`else` is **not** a match fallback. Use `_`.

Two header forms (same as `if`: pick `:` **or** `{ }`, not both):

```phs
match score {
    100:
        print("Perfect")

    90, 95:
        print("Excellent")

    50, 60, 70:
        print("Passed")

    _:
        print("Other")
}
```

```phs
match score:
    1:
        print("One")

    _:
        print("Other")
```

The matched expression is evaluated **once**. Nested `match` is allowed. Case bodies may contain assignments, `print`, `if`, loops, and nested `match`.

### Patterns (v1)

1. **Literal** — integer, float, string, `true`, `false`, `null` (optional unary `+` / `-` on numbers):

```phs
match value {
    1:
        print("one")
    "hello":
        print("hi")
    true:
        print("yes")
    null:
        print("none")
    _:
        print("other")
}
```

Comparison uses the same equality rules as `==`.

2. **Multiple literals** — comma-separated; the case runs if **any** literal equals the value:

```phs
match n {
    1, 2, 3:
        print("Small")
    _:
        print("Other")
}
```

3. **Wildcard `_`** — matches any value. It cannot take a guard or appear in a comma list. A case after an unguarded `_` is a compile-time `unreachable match case`.

4. **Binding + optional guard** — an identifier (not `_`) binds the matched value for that case only. If `if` is present, the guard is evaluated in that binding’s scope; a falsy guard tries the next case.

```phs
match score {
    value if value >= 90:
        print("Excellent")

    value if value >= 50:
        print("Passed")

    _:
        print("Failed")
}
```

The binding is **not** a global. After the case, the name is restored (or removed) so it does not leak. An unguarded binding (`value:`) matches everything, like `_`, and makes later cases unreachable.

5. **Structural patterns** — list, tuple, dict, and struct/class shapes reuse destructuring syntax (`[a, b]`, `(x, y)`, `{"k": v}`, `Point{x, y}`, `User(name, age)`). Literals may appear inside list/tuple patterns (`[1, 2]`).

6. **Pattern combinators** — `or` / `|` and `and` / `&` combine patterns (distinct from `||` / `&&` in expressions). Example: `90 | 95:` or `[1, 2] or [3, 4]:`.

7. **Match expression** — use `match` where an expression is required:

```phs
label = match n {
    1: "one"
    2: "two"
    _: "other"
}
```

Brace cases use a single expression after `:`; colon blocks use one indented expression per case.

### Invalid

```phs
match x {
    else:
        print("no")
}

match x {}

match x: {
    _:
        print("mix")
}
```

---

## Conditions

`if`, `elif`, `elseif`, and `while` all use the same expression parser. A condition can be:

- a comparison (`a > b`, `x == 10`)
- arithmetic (`a + 1`, `(a - 5) / 3`)
- a logical expression (`age >= 18 and score >= 50`, `not active`, `!active`)
- membership (`item in list`, `key not in dict`)
- a ternary (`score >= 50 ? "Passed" : "Failed"`)
- a name or literal (`score`, `true`, `0`, `"hello"`)
- grouped with parentheses

The result does **not** have to be a `bool`. At runtime the value is tested with truthy/falsy rules (see below).

```phs
if (a > b):
    print("a")

elif ((a - 5) / 3):
    print("b")

elseif (a + b * 2) > 10:
    print("c")
```

Parentheses around the whole condition are optional when the expression is already unambiguous:

```phs
if score > 90:
    print("A")
```

---

## Operator precedence

In conditions, the full expression precedence in [Operators](operators.md) applies, including bitwise, `in` / `not in`, and ternary. Arithmetic still binds before comparisons; `and` before `or`.

```phs
if a + b * 2 > 10:
    print("yes")
```

`b * 2` is evaluated first, then `a + …`, then compared to `10`.

Parentheses change grouping:

```phs
if (a + b) * 2 > 10:
    print("yes")
```

More examples:

```phs
# (1 + 2) == 3  →  true
if 1 + 2 == 3:
    print("yes")

# a > b is evaluated first, then compared to true
if a > b == true:
    print("chained")
```

Full operator rules: [Operators](operators.md).

---

## Indentation blocks

After `:`, the body must start on a **later line**, indented **further in** than the header keyword (`if` / `elif` / `elseif` / `else` / `while` / `for` / `match`, or a `match` case). That indent level is the block boundary.

Every statement in the body uses the same indent. When a line returns to the header column or less, the block has ended.

```phs
if score > 90:
    print("A")
elif score > 70:
    print("B")
elseif score > 50:
    print("C")
else:
    print("F")
```

Code after the block is not part of it:

```phs
if score > 50:
    print("Passed")

print("Done")
```

Output:

```text
Passed
Done
```

Valid — multiple statements in one body:

```phs
if score > 50:
    print("Passed")
    print("Good job")
```

Invalid — body not indented:

```phs
if score > 50:
print("Passed")
```

Invalid — inconsistent indent inside the body:

```phs
if score > 50:
    print("a")
        print("b")
```

Indentation applies only to block-based syntax. Extra spaces on a normal top-level statement do not create a block:

```phs
x = 10
    print(x)
```

The indented `print` is still a top-level statement (unusual style, but valid).

---

## Brace blocks

Write `{` directly after the condition — **not** after `:`. `}` closes the block. Statements inside must still be indented on their own lines.

```phs
if score > 90 {
    print("A")
} elif score > 70 {
    print("B")
} elseif score > 50 {
    print("C")
} else {
    print("F")
}
```

`elif`, `elseif`, and `else` may follow the closing `}` on the same line.

Valid — empty body:

```phs
if debug {
}
```

Invalid — unindented body:

```phs
if score > 90 {
print("A")
}
```

Invalid — body on the same line as `{`:

```phs
if score > 90 { print("A") }
```

Invalid — mixing `:` and `{`:

```phs
if score > 90: {
    print("A")
}
```

Invalid — unclosed `{`:

```phs
if score > 90 {
    print("A")
```

---

## Nested conditions

An `if` may appear inside another `if` body. Each block keeps its own indent level.

```phs
if score > 50:
    if score > 90:
        print("Excellent")
    else:
        print("Passed")

print("Done")
```

With `score = 75`, output:

```text
Passed
Done
```

With `score = 95`:

```text
Excellent
Done
```

Brace nesting:

```phs
if score > 50 {
    if score > 90 {
        print("Excellent")
    } else {
        print("Passed")
    }
}
print("Done")
```

---

## Truthy / Falsy

When a condition is evaluated, its value is converted to “run this branch or skip it” using these rules:

| Value | Result |
| ----- | ------ |
| `true` | truthy |
| `false` | falsy |
| `0` | falsy |
| `0.0` | falsy |
| any other number | truthy |
| `""` | falsy |
| any other string | truthy |
| `null` | falsy |
| `[]` | falsy |
| a non-empty list | truthy |
| `{}` | falsy |
| a non-empty dictionary | truthy |
| an empty range | falsy |
| a non-empty range | truthy |

Examples:

```phs
if 10:
    print("runs")

if 0:
    print("skipped")

if "hello":
    print("runs")

if "":
    print("skipped")

if null:
    print("skipped")

if []:
    print("skipped")

if [1]:
    print("runs")
```

A comparison such as `score > 90` produces a `bool`, which follows the `true` / `false` rows above.

---

## try / catch / finally / throw

User exceptions use `try`, one or more optional `catch` clauses, optional `finally`, and `throw`. Colon and brace block forms match other control-flow statements.

```phs
try:
    throw ValueException("error", null)
catch ValueException error:
    print(error.message)
```

```phs
try {
    throw "error"
} catch error {
    print(error)
}
```

`try` requires at least one of `catch` or `finally`. Each `catch` names a binding (`catch error:` / `catch TypeName error:`).

**Untyped catch** — matches any thrown value (Bölüm 1 compatible):

```phs
catch error:
    print(error)
```

**Typed catch** — matches class instances using normal inheritance (`Child : Parent`):

```phs
catch Exception error:
    print(error.message)
catch MyError error:
    print(error.message)
```

Multiple `catch` clauses are tried in source order. A more specific type must appear before a general supertype; the compiler rejects unreachable later clauses (`unreachable catch clause`). Nested `try`/`catch` uses indentation: an outer `catch` at the same column as its `try` is not consumed by an inner `try`.

**Built-in exception hierarchy** (preloaded by the CLI; use `compile_with_builtins` in tests):

```
Exception
└── RuntimeException
    ├── ValueException
    ├── TypeException
    ├── NameException
    └── IndexException
```

Each built-in type is a normal `class` with `message`, `cause`, `file`, `line`, `column`, and `stack_trace` fields and `constructor(message, cause)`. The VM fills location and stack-trace metadata when an exception is raised.

**User-defined exceptions** — same class syntax as other types:

```phs
class MyError : Exception:
    constructor(message, cause):
        parent(message, cause)
```

**`throw`**

- Statement only (`throw expr`). Not an expression (`x = throw "x"` is invalid).
- Payload may be any `Value` (primitives) or a class instance (typed exceptions).
- `throw error` in a catch block rethrows the bound exception; later `catch` clauses in the same `try` may handle it.
- Exception chaining: pass the caught exception as `cause` when constructing a new one (`MyError("outer", error)` sets `cause`).

Lexer, parser, and compiler errors remain **compile-time** failures (`Compile error: …`). Runtime failures from builtins and VM operations are converted into catchable built-in exception instances (Bölüm 3).

```phs
try:
    int("hello")
catch TypeException error:
    print(error.message)
```

**Runtime error → exception mapping** (via message category):

| Category | Examples |
| -------- | -------- |
| `TypeException` | `cannot convert …`, `Type error: …`, `expects …`, `not a …`, `cannot call method …`, `cannot iterate …` |
| `NameException` | `undefined dictionary key`, `unknown field`, `unknown … method` |
| `IndexException` | `list index out of range`, `tuple index out of range` |
| `ValueException` | `unsupported operand type`, `division by zero`, `modulo by zero`, `invalid shift`, `value not found`, `is empty` |
| `RuntimeException` | other runtime failures |

Runtime errors propagate across `CallFrame`s the same way as `throw`, participate in typed catch (including parent types), and run `finally` blocks before continuing or failing.

**Semantics**

| Path | Behavior |
| ---- | -------- |
| Normal `try` completion | `finally` runs (if present), then execution continues after the construct |
| `throw` + matching `catch` | Unwind to handler frame depth; bind catch variable; run catch body; `finally` runs; then continue |
| `throw` + no matching `catch` | `finally` runs with pending exception; then propagation continues |
| `catch` + `throw error` | Resume catch matching from the next clause in the same `try` |
| Unhandled at top level | `Unhandled exception: …` with type, message, source location, stack trace, and optional `caused by:` chain |

**`finally` + control flow**

| Path | Behavior |
| ---- | -------- |
| `return` in `try`/`catch` with `finally` | Defer return value; run `finally`; then return |
| `break` / `continue` in `try` with `finally` | Defer loop jump; run `finally`; then break/continue |
| `return` in `finally` | Clears pending exception/deferred control; that return wins |
| `throw` in `finally` | Clears pending deferred control; that exception wins |

Catch bindings are block-local (compile-time scope). Exception propagation unwinds nested calls and restores the register file at the `try` frame depth before running a catch handler.

**Stack trace** — each exception may carry `stack_trace` as a `list` of `dict` entries (`function`, `file`, `line`, `column`). The compiler records `instruction_locations[]` and `Bytecode.source_file` so traces survive propagation. Unhandled output includes readable frames:

```
Unhandled exception: TypeException: cannot convert string to int
  at test (example.phs:3:12)
  at main (example.phs:5:1)
caused by:
ValueException: inner
  at inner (example.phs:2:5)
```

Cause chains are printed recursively with cycle protection. Primitive throws (`throw "error"`, `throw 42`, `throw null`) use the normal `Value` formatter in unhandled output.

---

## Invalid syntax / common mistakes

| Mistake | Example | Problem |
| ------- | ------- | ------- |
| Mixing `:` and `{` | `if x: { … }` / `while x: { … }` | One block form per header |
| Missing body indent | `if x:\nprint(1)` | Body must be indented after `:` |
| Missing body indent in braces | `if x {\nprint(1)\n}` | Body must be indented inside `{ }` |
| `else if` instead of `elif` | `else if x:` | Use `elif` or `elseif` |
| Standalone `elif` / `else` | `elif x:` at top level | Must follow an `if` |
| Misaligned branch keyword | `elif` not at same column as `if` | Branch headers must align |
| Tab at line start | `\tprint(1)` in a block | Use spaces only for indent |
| Missing condition | `if:` / `while:` | Condition is required |
| Missing block | `if x > 1` / `while x < 10` | Need `:` or `{` after the header |
| Incomplete `for` | `for x:` / `for in items:` / `for x in:` | Need `item in collection` |
| `break` / `continue` outside a loop | `break` at top level | Compile-time error |
| `else` inside `match` | `match x { else: … }` | Use `_`, not `else` |
| Empty `match` | `match x {}` | At least one case is required |
| Empty match case | `1:` with no body | Case body is required |
| Unreachable match case | a case after `_` | Compile-time `unreachable match case` |
| Duplicate literal pattern | two cases with `1:` | Compile-time `duplicate match pattern` |
| `try` without `catch` or `finally` | `try:\n    print(1)` | `try requires catch or finally` |
| `catch` without binding | `catch:` | `expected catch variable name` |
| Unknown catch type | `catch Missing e:` | `unknown catch type` |
| Unreachable catch | `catch Exception e:` then `catch MyError e:` when `MyError : Exception` | `unreachable catch clause` |
| `throw` as expression | `x = throw 1` | Parse/compile error |

When something is wrong, PScript reports a syntax error with a line and column.
