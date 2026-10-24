# Syntax

This page describes the PScript you can run today. A program is a `.phs` file: assignments, `print`, `input`, conditionals, `match`, loops, and imports from other `.phs` modules.

```phs
x = 10
name = "Emir"

print(name)
print("Value:", x)
```

`x` and `name` get values. The last two lines write those values. Run the file with:

```text
pscript example.phs
```

---

## Files

- Source files use the `.phs` extension.
- Source is UTF-8.
- Names (identifiers, keywords, operators) are ASCII. Unicode may appear inside strings.

---

## Whitespace and indentation

Spaces and newlines separate tokens. There are no semicolons.

For ordinary statements, extra spaces do not start a block. For **block-based** syntax (`if`, `elif`, `elseif`, `else`, `for`, `while`, `match`, and each `match` case), indentation is validated after `:` or inside `{ }`:

- After `:`, the body starts on the next line, indented past the header.
- In `{ }` blocks, statements inside `{` must also be indented; `}` closes the block.

Use `:` **or** `{ }` per header, not both (`if x: { }` is invalid). The lexer does not produce `INDENT` / `DEDENT`. Details: [Control flow](control-flow.md).

Use spaces to indent. A tab at the beginning of a line is invalid indent.

---

## Comments

`#` starts a comment through the end of the line.

```phs
# Starting score
score = 100
print(score)  # writes 100
```

`#` inside a string is not a comment:

```phs
print("Item #3")
```

---

## Statements and expressions

A **statement** does work: assign a variable, compound-assign, increment, print, branch, `match`, or loop.

An **expression** produces a value: a literal, a name, an operator, a call, or a collection.

```phs
result = 1 + 2 * 3
print(result)
```

`1 + 2 * 3` is an expression. The assignment and `print` are statements.

See [Operators](operators.md) for arithmetic, comparison, logical, bitwise, membership, ternary, assignment, and increment, [Variables](variables.md) for names, and [Scope](scope.md) for visibility.

---

## Identifiers

Names are case-sensitive.

- Start with `A`–`Z`, `a`–`z`, or `_`
- Continue with letters, digits, or `_`

Valid: `x`, `_count`, `player2`, `HTTP`.

`True`, `FALSE`, and `Null` are ordinary names. The literals are `true`, `false`, and `null`.

Some words are reserved and cannot be used as names, including `true`, `false`, `null`, `if`, `elif`, `elseif`, `else`, `for`, `while`, `in`, `break`, `continue`, `match`, `and`, `or`, `not`, `input`, `func`, `return`, `class`, `constructor`, `this`, `super`, `parent`, `pass`, `public`, `pub`, `protected`, `prot`, `private`, and `priv`.

`print`, `range`, `int`, `float`, `str`, `bool`, and `type` are built-ins, not reserved words. `int` / `float` / `bool` / `string` remain type names in declarations.

---

## Literals

| Kind | Example |
| ---- | ------- |
| Integer | `10`, `0`, `007` |
| Float | `1.5`, `92.5`, `0.5` |
| String | `"Emir"`, `"Hello"` |
| Bool | `true`, `false` |
| Null | `null` |

Integers are decimal digits only. Floats are `digits.digits` — both sides need at least one digit. `.5` and `1.` are invalid.

Strings use double quotes. Escapes: `\"`, `\\`, `\n`, `\t`, `\r`.

Details: [Types](types.md).

---

## Parentheses

Parentheses group expressions:

```phs
print((10 + 5) * 2)
```

This prints `30`. Without the inner parentheses, `*` runs first.

---

## if

Conditional statements (`if`, `elif`, `elseif`, `else`), `match`, and loops (`while`, `for ... in`, `break`, `continue`) are implemented. See [Control flow](control-flow.md).

---

## Collections

Lists `[10, 20]` and dictionaries `{"name": "Emir"}` are implemented. Index with `value[key]`. Methods such as `append`, `get`, and `length` use `object.method(args)` and are expressions. Details: [Collections](collections.md).

---

## input

`input` is a reserved word. It is a built-in that always returns a `string`. There is no automatic number or bool detection.

```phs
name = input()
name = input("Name: ")
age = int(input("Age: "))
```

- Zero arguments: read a line with no prompt.
- One string argument: write that prompt (no extra newline), then read a line.
- The result is the rest of the line, without the trailing newline.

Explicit conversions: `int(value)`, `float(value)`, `str(value)`, `bool(value)`. Runtime introspection: `type(value)`. No cast syntax. Details: [Types](types.md).

Exception statements: `try` / `catch [TypeName] <name>` (multiple catches allowed) / `finally` / `throw`. Details: [Control flow](control-flow.md).

---

## print

`print` is a built-in, not a keyword. Parentheses are optional. Arguments are separated by commas.

```phs
print("Hello")
print("Hello", "World")
print("Score:", score)

print "Hello"
print "Score:", score
print (x * y) / 2
```

- At least one argument is required.
- Arguments are written with a single space between them, then a newline.
- Trailing or leading commas are invalid: `print("Hello",)` and `print(, 10)` do not parse.
- `print(...)` is the argument-list form: the `(` must sit against `print`. A space before `(` starts a normal expression, so `print (x * y) / 2` is one argument, not a call that stops at `(x * y)`.

```phs
score = 10
print("X:", 10, "Y:", 20)
print("Score:", score)
```

Output:

```text
X: 10 Y: 20
Score: 10
```

---

## Functions

`func` declarations, calls, and `return` are implemented. See [Functions](functions.md).

---

## Imports

User `.phs` modules can be imported from the same directory tree as the importing file. See [Modules](modules.md).

---

## Not in this version

Modules support `pub` / `priv` visibility, circular imports, and cross-module classes/enums/structs. Built-in modules without `.phs` files and package distribution are not implemented yet.
