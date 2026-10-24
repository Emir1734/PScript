# Functions

Functions are named, callable blocks. This is **Aşama 3**: nested functions, lexical closures (capture by binding), first-class function values, plus Aşama 2 automatic return and recursion. See also [Scope](scope.md) and [Variables](variables.md).

---

## Declaration

Use the keyword `func`. The body uses the same block rules as `if` / `while`: either `:` with an indented suite, or `{ }` with an indented body. You cannot mix `:` and `{` on the same header.

```phs
func add(a, b):
    return a + b
```

```phs
func add(a, b) {
    return a + b
}
```

Zero parameters:

```phs
func hello():
    print("hi")
```

A function declaration is a statement. The name is a `Function` symbol in the current scope. Duplicate names in that scope are a compile-time `duplicate declaration`.

Nested `func` inside another `func` is valid; see [Nested functions](#nested-functions).

---

## Parameters

Parameters are ordinary local variables of the function. They live in the function’s local scope.

```phs
func test(x):
    print(x)

test(50)
```

Typed parameters use the same annotation syntax as variables, including generics:

```phs
func sum(list<int> values):
    print(values[0] + values[1])

sum([1, 2])       # OK
# sum(["a", "b"]) # compile error
```

`any` parameters keep Bölüm 1 semantics (no static argument check).

Typed parameters use the same annotation syntax as variables, including generics:

```phs
func sum(list<int> values):
    print(values[0] + values[1])

sum([1, 2])       # OK
# sum(["a", "b"]) # compile error
```

`any` parameters keep Bölüm 1 semantics (no static argument check).

This prints `50`.

The same name cannot appear twice in one parameter list (`duplicate parameter 'x'`). Trailing commas in the parameter list are invalid, matching call arguments.

Prefix parameter types are supported (`func test(int x):`). Postfix parameter types are also supported (`func test(x: int):`).

### Generic functions

```phs
func identity<T>(value: T) -> T:
    return value
```

Type parameters are inferred at call sites (`identity(10)`, `identity("hi")`). Explicit type arguments use call syntax `identity<int>(10)` when needed.

### Default parameters

```phs
func greet(name: string, prefix: string = "Hello"):
    print(prefix + " " + name)
```

### Variadic parameters

```phs
func sum(values: int...):
    pass
```

Extra arguments are passed as a `list` in the final parameter slot.

### Overloading

Multiple functions may share a name when parameter types differ. Ambiguous calls are compile-time errors; identical signatures are rejected.

### Parameter destructuring

A parameter may be a destructuring pattern instead of a simple name. The caller still passes one argument per parameter; the callee unpacks that value when the frame is created.

```phs
func add((a, b)):
    a + b

add((10, 20))
```

List, dict, and struct patterns are supported:

```phs
func first([a, b]):
    a

func greet({"name": name}):
    print(name)

struct Point:
    int x
func read_x(Point{x}):
    return x
```

Wildcard (`_`) and rest (`*rest`) follow the same rules as assignment destructuring. Duplicate names within one pattern are rejected. See [Destructuring](destructuring.md).

---

## Calls

A call is an expression: `name(args)`. Empty argument lists are allowed (`test()`). Trailing commas are invalid.

Because it is an expression, these are all valid:

```phs
result = add(10, 20)
print(add(5, 7))
x = add(1, 2) * 3
```

The number of arguments must match the number of parameters. For a call that names a known top-level function, mismatch is a compile-time error with the call’s location, for example `'add' expects 2 arguments`. The same message is a runtime error when the callee is a function value (`f = add` then `f(1)`).

Typed parameters are checked at the call site: `func test(float x):` accepts `test(10)` via implicit `int`→`float` coercion. Class parameters accept subclass instances (`func take(Animal a):` + `take(Dog())`). Assigning from an `any` argument uses a runtime check. See [Types](types.md#type-compatibility).

An unknown callee that is not a built-in and not a visible variable is compile-time `unknown function '…'`. Calling a non-function value is runtime `invalid function value call`. Built-ins such as `range`, `input`, `type`, and conversions (`int`, `float`, `str`, `bool`) still use the existing `CallExpr` / `CALL_BUILTIN` path.

`throw` and runtime errors inside a function unwind through `CallFrame`s until a matching `try`/`catch` in a caller (or the current function) handles it. Uncaught exceptions terminate the program with `Unhandled exception: …` (type, message, stack trace, cause chain). See [Control flow](control-flow.md).

A function may be stored and called through a variable:

```phs
func add(a, b):
    return a + b

f = add
print(f(10, 20))
```

This prints `30`.

---

## Return

`return` is a reserved word and a statement. It is valid only inside a function (`return outside function` otherwise).

`return expression` yields that value and stops the function immediately:

```phs
func test():
    return 10
    print("never")
```

`print("never")` does not run.

A `return` on its own line with no expression yields `null`.

`return` leaves the **function**, not only the nearest `if` or loop. `RETURN` does not use the loop break/continue patch lists.

Inside `try`/`catch` with a `finally` block, `return` is deferred: the `finally` body runs first, then the return completes (`DEFER_RETURN`). A `return` or `throw` inside `finally` overrides any pending return or exception from the `try`/`catch` region.

```phs
func test(x):
    if x > 10:
        return 100
    return 0
```

`test(20)` is `100`. `test(5)` is `0`.

```phs
func find():
    for item in [1, 2, 3]:
        return item
    return null
```

This returns `1` and does not finish the loop.

If execution reaches the end of the function with no value, the result is `null`. A last-line `print` is a statement, not a value:

```phs
func test():
    print("hello")

x = test()
print(x)
```

Output:

```text
hello
null
```

---

## Automatic return

Only the **last statement of the function’s top-level body** can auto-return, and only if that statement is a value-producing **expression statement** (`ExprStmt`): a trailing identifier, arithmetic, call, and similar expressions. Inside a function those expressions are valid statements; at top level they are still `invalid statement`.

```phs
func square(x) {
    x * x
}

print(square(5))
```

This prints `25`.

```phs
func add(a, b):
    a + b

print(add(10, 20))
```

This prints `30`.

```phs
func test():
    x = 10
    x
```

This returns `10`. Assignment (`x = 10`) is not an expression statement, so a function that ends with only that assignment returns `null`.

**Not** automatic return:

- `print(...)` (it is `PrintStmt`)
- assignment / declaration statements
- a call to a function that does not itself produce an automatic-return value (for example `hello()` when `hello` only runs `print` and has no trailing expression)
- an expression that is only inside `if` / `elif` / `else` / `while` / `for` / `match`

When the last statement is such a void-producing call, the call runs for its side effects and the enclosing function returns `null`. The call’s `null` result is **not** auto-returned as if it were a meaningful value:

```phs
func hello():
    print("hello")

func wrapper():
    hello()

wrapper()
print("ok")
```

Output:

```text
hello
ok
```

There is no extra `null` line from `hello()` inside `wrapper`. Functions that do return a value still work as before (`print(add(2, 3))` prints `5`).

```phs
func test():
    if true:
        10
```

The last top-level statement is `if`, so the result is `null`. The `10` is evaluated and discarded.

A later top-level expression still auto-returns:

```phs
func test():
    if true:
        10
    20
```

This returns `20`.

**Explicit `return` always wins.** Code after `return` does not run, even if it is a trailing expression:

```phs
func test():
    return 10
    20
```

This returns `10`.

Return type annotations use `->` after the parameter list. The compiler checks `return` expressions and implicit tail returns against the annotation; `async func` with `-> T` is treated as returning `Task<T>` at call sites and `T` inside the body.

```phs
func add(a: int, b: int) -> int:
    return a + b
```

```phs
async func load() -> int:
    return 10
```

---

## Scope

A call has its own local frame. Names created only inside the function are not visible outside:

```phs
func test():
    x = 10
    print(x)

test()
print(x)
```

The last line is compile-time `undefined variable 'x'`.

Globals are visible from a function:

```phs
x = 10

func test():
    print(x)

test()
```

This prints `10`.

Assignment without a type still follows the existing rule: update the visible binding. Here the global `x` becomes `20`:

```phs
x = 10

func test():
    x = 20

test()
print(x)
```

An explicit declaration creates a new local slot and does not change the global:

```phs
x = 10

func test():
    int x = 20

test()
print(x)
```

This prints `10`.

A parameter shadows an outer name:

```phs
x = 10

func test(x):
    print(x)

test(50)
print(x)
```

Output:

```text
50
10
```

---

## Hoisting

Function declarations are hoisted in their lexical scope (top-level and nested). A call may appear before the `func` in that scope. Ordinary variables are **not** hoisted.

```phs
result = add(10, 20)

func add(a, b):
    return a + b
```

Ordinary variables are **not** hoisted. This is still invalid:

```phs
print(x)
x = 10
```

Class declarations are hoisted (pre-registered) like functions; see [Classes](classes.md).

---

## Recursion

A function may call itself. Each call has its own CallFrame (return IP, destination register, registers, locals, parameters). Locals such as `x = n` are not shared with the caller.

```phs
func factorial(n):
    if n <= 1:
        return 1
    return n * factorial(n - 1)

print(factorial(5))
```

This prints `120`. Recursive calls work in expressions (`factorial(5) + 10`, `factorial(3) * factorial(4)`). Nested functions may recurse by name as well; each call still has its own CallFrame.

---

## Nested functions

A `func` may appear inside another `func`. The inner function has its own lexical scope (parameters and locals) and can call other functions, including itself.

```phs
func outer():
    func inner():
        print("hello")

    inner()
```

Nested declarations in the same scope must use distinct names (`duplicate declaration`). Nested functions are hoisted in that lexical scope:

```phs
func outer():
    result = inner()

    func inner():
        return 10

    return result
```

`outer()` returns `10`. Ordinary variables are still not hoisted:

```phs
func outer():
    print(x)
    x = 10
```

This is compile-time `undefined variable`.

---

## Capture and closures

A nested function may read and write bindings from enclosing function scopes. That is **lexical capture**. Global names are not put on a capture list; they keep using the existing global slots.

```phs
func outer():
    x = 10
    y = 20

    func inner():
        return x

    inner()
```

`inner` captures `x` only. `y` is not captured if it is never used inside `inner`.

Capture is **by binding** (shared cell), not a copy of the current value:

```phs
func outer():
    x = 10

    func inner():
        return x

    x = 20
    return inner
```

`outer()()` yields `20`. `+=`, `++`, and assignment on a captured name update the same binding.

A **normal local** lives only as long as its function frame. A **captured** local lives as long as any closure still holds that binding (heap cell). Each invocation of the outer function creates its own environment:

```phs
func make_counter():
    count = 0

    func increment():
        count += 1
        return count

    return increment

a = make_counter()
b = make_counter()
print(a())
print(a())
print(b())
```

Output:

```text
1
2
1
```

Returning `inner` without calling it, storing the closure in a variable, and calling it later are all valid. Printing a function value uses `<function name>`.

---

## Not in this version

Interface/protocol constraints on type parameters are reserved for a later release; `T: SomeType` parses but is not fully enforced yet.

---

## Async / Await (Bölüm 1–2)

Async functions use the same block and parameter rules as ordinary functions, with the `async` keyword before `func`:

```phs
async func load():
    await delay(1ms)
    return 100
```

### Rules

| Rule | |
| ---- | - |
| `async func` | Declares a function that returns a **`Task`** when called |
| `await expr` | Valid **only** inside an async function; `expr` must be a `Task` (or `any` with runtime check) |
| Normal `func` + `await` | Compile error |
| Top-level `await` | Allowed when there is no `async func main()`; compiler emits `__pscript_module_entry__` and the VM runs the scheduler |
| `async constructor` | Compile error |
| `async func main()` | Entry point: top-level imports run first, then the VM scheduler runs until all tasks finish |
| `async func f()` without `await` at top level (sync script) | Returns a `Task` immediately; prefer `await f()` for ordering. Inside `async func main()`, fire-and-forget calls are scheduled on the active scheduler |
| Cancellation / timeout | Task control in Update Bölüm 1 |
| Async file I/O / `async_input()` | Update Bölüm 2 |
| Threads / thread pool | Out of scope |

Calling an async function does **not** run its body immediately. It schedules work and gives you a `Task`:

```phs
async func value():
    return 42

async func main():
    task = value()
    print(await task)
```

`await` is cooperative: while one async call waits, the scheduler can run other tasks (for example other `delay` timers).

### Timer: `delay`

```phs
async func main():
    await delay(1ms)
    print("done")
```

`delay(duration)` is a built-in that returns a `Task`. Duration literals use suffixes `ms`, `s`, `m`, or `h` (for example `await delay(1.5s)`). Use `await delay(...)` for intentional waits. The scheduler sleeps only when no task is runnable.

### Exceptions

Exceptions thrown inside an async task are stored on the task until `await` propagates them:

```phs
async func fail():
    throw ValueException("boom", null)

async func main():
    try:
        await fail()
    catch e:
        print(e.message)
```

Existing `try` / `catch` / `finally` / `cause` / stack-trace semantics apply to the awaiting async frame. `return` inside `try` with a `finally` block runs `finally` before the async function completes.

### Scheduler (Bölüm 2)

- Multiple async tasks interleave at `await` / `delay` boundaries on **one OS thread**.
- Spawning without `await` still runs the task before program exit; an unhandled failure in such a task is reported at shutdown.
- `await` on a failed task rethrows its stored exception into the caller; a surrounding `catch` handles it normally.

### Closures, classes, and modules

Async nested functions capture enclosing bindings the same way as sync closures. Async class methods preserve `this` across suspend/resume. Imported async functions (`await math.compute()`) behave like local async calls.

See also `docs/architecture/scheduler.md`, `docs/architecture/register-vm.md`, and `docs/LANGUAGE_SPEC.md` §32–§35.

### Task control (Update Bölüm 1)

#### Cancellation

```phs
async func work():
    try:
        await delay(1ms)
    finally:
        cleanup()
    return

async func main():
    task = work()
    task.cancel()
    try:
        await task
    catch e:
        print(e.message)   # Task was cancelled
```

- `task.cancel()` is cooperative; `finally` always runs.
- `CancellationException` is catchable (subclass of `RuntimeException`).
- Cancelling a terminal task is a no-op.

#### `race()`

```phs
result = await race(task_a, task_b)
```

First finished task wins. Losers **keep running** — `race()` does not cancel them.

#### `timeout()`

```phs
result = await timeout(task, 3s)
```

Completes within 3000 ms → result. Otherwise → target task is cancelled (`finally` runs) and **`TimeoutException`** is thrown. A failed inner task propagates its own exception instead.

### Async I/O + `async_input()` (Update Bölüm 2)

Sync `fs` / `io` / `input()` are unchanged. Async variants return `Task` values and require `await` inside `async func`.

#### Async `fs`

```phs
import fs
data = await fs.async_read("notes.txt")
await fs.async_write("out.txt", data)
await fs.async_append("log.txt", "line\n")
```

#### Async `FileHandle`

```phs
import io
f = io.open("data.txt", "w")
await f.async_write("hello")
f.close()
f = io.open("data.txt", "r")
print(await f.async_read())
f.close()
```

#### `async_input()`

```phs
async func main():
    print("Name?")
    name = await async_input()
    print(name)
```

Optional prompt: `await async_input("Name? ")` (prints prompt, then reads a line).

Works with `race()`, `timeout()`, and `task.cancel()` using the same semantics as §34.

Blocking file/stdin work runs on a single dedicated I/O worker thread; the scheduler thread stays cooperative.

### Finalized semantics (Bölüm 3)

Async/Await is fully integrated with PScript’s existing systems. Boundaries are fixed: sync code is unchanged; async code uses cooperative Tasks, `await` only inside `async func`, and the CLI runs `async main()` through the scheduler after top-level init.
