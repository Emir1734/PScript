# PScript Language Specification v0.2

This document is the source-level specification for PScript **Lexer v0.1**, **Language Basics v0.2**, **Control Flow**, **Collections v1**, **Loops v1**, **logical operators**, **`input` / conversions**, **`match` v1**, **Operators 2.0**, **Scope**, **Functions Aşama 3**, **Async/Await Core (Bölüm 1)**, **Classes and inheritance**, **Destructuring** (Bölüm 1–3, complete), **Type system** (Bölüm 1–3 complete), **Exceptions** (Bölüm 1–3 complete), **Modules / Imports** (complete — user `.phs` modules, visibility, circular imports, caching, cross-module types), and **Standard library Bölüm 1–5** (`math`, `string`, `collections`, global numeric built-ins; `path`/`fs`/`io`; `os`/`process`/`sys`; `time`/`datetime`/`random`; `json`/`re`/`encoding`).

Parser grammar beyond the implemented statement set and later language features remain unspecified. Do not invent extra syntax from other languages.

---

## 0. Language overview

PScript is a general-purpose language intended to be easy, professional, and powerful.

| Decision | Status |
| -------- | ------ |
| Implementation language | C++20 only. Rust is not an implementation language. |
| Execution | Compile to bytecode; execute on a VM. Not transpiled to C++. |
| Pipeline | `Source → Lexer → Parser/AST → Register Bytecode Compiler → Register Bytecode → Register VM → C++ Runtime` |
| Source files | `.phs` |
| Comments | `#` starts a single-line comment |
| Variables | Type may be inferred from the assigned value, or declared explicitly (`int x = …` or `x: int = …`). A typed name without `=` starts as `null`. |
| Constants | `const` is reserved; constant bindings are not in v0.2 |
| Types | `int`, `float`, `bool`, and `string` are variable types. `null` is a literal/value, not a variable type. Lists, dictionaries, tuples, sets, ranges, structs, and enums are values. Struct/record type names may annotate variables. |
| Safety | A safe memory model is a long-term goal. Ownership ideas may be inspired by Rust, but PScript must not inherit Rust’s complexity. Details are undecided. |
| Errors | Lexer, parser, and runtime errors must include useful messages with line and column |
| Modules | `import` / `as` / `from`; user modules/libraries. Package projects use `pscript.toml` (see `docs/package-ecosystem.md`). |
| Access | `public` / `pub`, `protected` / `prot`, `private` / `priv`. Default access is public. |
| Syntax forms | Multiple readable forms with the same meaning are allowed when they are unambiguous. The lexer must not forbid this. |

Built-in functions such as output (`print`) are **not** keywords. These source shapes are valid:

```
print("Hello")
print "Hello"
print("Hello", "World")
print "Score:", score
```

`print` is a built-in. It is not a reserved word.

---

## 1. Source text

- Source files are read as a sequence of Unicode code points decoded from UTF-8.
- Identifiers and other non-string tokens use **ASCII only**.
- Column tracking counts code points, not UTF-8 bytes.

---

## 2. Identifiers

Identifiers are case-sensitive.

**Start character:** ASCII letter (`A`–`Z`, `a`–`z`) or underscore (`_`).

**Following characters:** ASCII letter, decimal digit (`0`–`9`), or underscore.

`_` is allowed in variable and function names.

Unicode letters, digits, or other scripts are **not** identifier characters in v0.1.

Examples of valid identifiers: `x`, `_count`, `player2`, `HTTP`.

`True`, `FALSE`, and `Null` are identifiers. They are not the literals `true`, `false`, or `null`.

A reserved word (Section 3) is never tokenized as an identifier.

---

## 3. Keywords and literals

### 3.1 Reserved words

These words are reserved. They are distinct token kinds, not identifiers.

| Word      | Token kind | Role |
| --------- | ---------- | ---- |
| `true`    | `True`     | boolean literal |
| `false`   | `False`    | boolean literal |
| `null`    | `Null`     | null literal |
| `const`   | `Const`    | constant binding |
| `if`      | `If`       | conditional |
| `elif`    | `Elif`     | conditional |
| `elseif`  | `ElseIf`   | conditional (same role as `elif`) |
| `else`    | `Else`     | conditional |
| `for`     | `For`      | loop |
| `while`   | `While`    | loop |
| `in`      | `In`       | `for` iteration |
| `break`   | `Break`    | loop control |
| `continue`| `Continue` | loop control |
| `func`    | `Func`     | function |
| `return`  | `Return`   | return from function |
| `struct`  | `Struct`   | struct type |
| `record`  | `Record`   | record type (same as struct) |
| `enum`    | `Enum`     | enum type |
| `class`   | `Class`    | class |
| `public`  | `Public`   | member access modifier |
| `pub`     | `Pub`      | member access modifier (alias of `public`) |
| `private` | `Private`  | member access modifier |
| `priv`    | `Priv`     | member access modifier (alias of `private`) |
| `protected` | `Protected` | member access modifier |
| `prot`    | `Prot`     | member access modifier (alias of `protected`) |
| `pass`    | `Pass`     | no-op statement |
| `constructor` | `Constructor` | class constructor |
| `super`   | `Super`    | parent class access (methods / ctor) |
| `parent`  | `Parent`   | alias of `super` |
| `this`    | `This`     | current class instance |
| `import`  | `Import`   | module import |
| `from`    | `From`     | module import |
| `as`      | `As`       | import alias |
| `match`   | `Match`    | match statement |
| `input`   | `Input`    | built-in input (reserved; parsed as a call name) |
| `and`     | `And`      | logical and |
| `or`      | `Or`       | logical or |
| `not`     | `Not`      | logical not |
| `async`   | `Async`    | async function declaration |
| `await`   | `Await`    | await a Task (async functions only) |

Built-in functions are not keywords unless listed above. `print`, `range`, `fn`, `let`, `int`, `float`, `str`, `bool`, and `type` (conversion / introspection calls) are **not** reserved. `input` **is** reserved. `return` **is** reserved (Functions Aşama 3).

### 3.2 Boolean and null literals

- `true` and `false` are boolean literals.
- `null` is the null literal.

---

## 4. Numeric literals

Signs (`+`, `-`) are not part of numeric literals. They are separate operator tokens (Section 6). Unary vs binary use is a parser concern.

Digit separators, hexadecimal, binary, octal prefixes, and imaginary suffixes are **not** part of v0.1.

### 4.1 Integers

An integer is a non-empty sequence of decimal digits `0`–`9`.

Leading zeros are allowed. The lexer keeps the lexeme as written (`007` is an integer lexeme `007`). Numeric value conversion is not a lexer concern.

An integer must not be immediately followed by an identifier start character. `1x` is an invalid numeric form, not `1` then `x`.

### 4.2 Floating-point

A floating-point literal is:

```
digits "." digits
```

Both the integer part and the fraction part must contain at least one digit. Examples: `0.5`, `10.0`, `3.1415`.

A `.` that is not part of this form is the punctuation token `Dot` (Section 7), for example `obj.field`.

### 4.3 Exponent notation

Exponent notation (`1e3`, `1.0E-2`) is **not** supported in v0.1. A digit sequence immediately followed by `e` or `E` is an invalid numeric form.

### 4.4 Invalid numeric forms

The lexer must reject at least:

| Form        | Reason |
| ----------- | ------ |
| `1.`        | Missing fraction digits (not `1` then `Dot`) |
| `.5`        | Missing integer digits (not `Dot` then `5`) |
| `1.2.3`     | Extra decimal point |
| `1e3`       | Exponent notation is not in v0.1 |
| `1x`        | Digit sequence joined to identifier chars |
| `0x10`      | Hexadecimal is not in v0.1 |

A lone `.` is **not** an invalid number; it is `Dot`.

A decimal point is recognized as starting a float only when it appears in `digits "." digits`. After a valid `digits "." digits` lexeme, a further `.` or identifier character makes the form invalid (`1.2.3`, `1.2f`).

---

## 5. String literals

v0.1 strings use **double quotes only**: `"..."`.

Single quotes are not string delimiters. `'` is an invalid character unless later approved.

### 5.1 Contents

- A real newline (LF, or CR that begins a newline as defined in Section 9) is **not** allowed inside a string. That is an **unterminated string** error at the opening `"`.
- The string ends at the next unescaped `"`.
- Unescaped `"` cannot appear inside the string.

### 5.2 Escape sequences

`\` starts an escape. The supported escapes are:

| Escape | Meaning              |
| ------ | -------------------- |
| `\"`   | double quote         |
| `\\`   | backslash            |
| `\n`   | newline (U+000A)     |
| `\t`   | horizontal tab       |
| `\r`   | carriage return      |

No other escapes are supported (`\x`, `\u`, `\'`, `\0`, octal, and unknown letters are invalid).

### 5.3 Invalid escape

If `\` is followed by end of file, a newline, or any character other than `"`, `\`, `n`, `t`, or `r`, the lexer reports an **invalid escape** at the `\`.

### 5.4 Unterminated string

The lexer reports an **unterminated string** at the opening `"` when:

- EOF is reached before a closing `"`, or
- a newline appears before a closing `"`.

---

## 6. Operators

The lexer uses **longest match**.

### 6.1 Arithmetic

| Lexeme | Kind      |
| ------ | --------- |
| `+`    | `Plus`    |
| `-`    | `Minus`   |
| `*`    | `Star`    |
| `/`    | `Slash`   |
| `%`    | `Percent` |
| `++`   | `PlusPlus` |
| `--`   | `MinusMinus` |

### 6.2 Assignment

| Lexeme | Kind          |
| ------ | ------------- |
| `=`    | `Equal`       |
| `+=`   | `PlusEqual`   |
| `-=`   | `MinusEqual`  |
| `*=`   | `StarEqual`   |
| `/=`   | `SlashEqual`  |
| `%=`   | `PercentEqual` |

### 6.3 Comparison

| Lexeme | Kind           |
| ------ | -------------- |
| `>`    | `Greater`      |
| `<`    | `Less`         |
| `>=`   | `GreaterEqual` |
| `<=`   | `LessEqual`    |
| `==`   | `EqualEqual`   |
| `!=`   | `BangEqual`    |

### 6.4 Logical and bitwise

| Lexeme | Kind       |
| ------ | ---------- |
| `&&`   | `AmpAmp`   |
| `\|\|` | `PipePipe` |
| `!`    | `Bang`     |
| `&`    | `Amp`      |
| `\|`   | `Pipe`     |
| `^`    | `Caret`    |
| `~`    | `Tilde`    |
| `<<`   | `LessLess` |
| `>>`   | `GreaterGreater` |
| `?`    | `Question` |
| `??`   | `NullCoalesce` |

`and` / `or` / `not` are keywords (Section 3). `??=` is rejected at lex time. `in` is the existing `In` keyword; `not in` is `Not` then `In`.

### 6.5 Longest match

When the next characters could form more than one operator, the lexer emits the longest established operator:

- `==` not two `=` tokens
- `!=` not `!` then `=`
- `<=` / `>=` not `<`/`>` then `=`
- `<<` / `>>` not two comparison tokens
- `&&` / `||` not two bitwise `&` / `|`
- `++` / `--` / `+=` / `-=` not two arithmetic tokens

A lone `!` is `Bang`. A lone `&` / `|` is bitwise `Amp` / `Pipe`.

### 6.6 Not in this version

Do not add `**`, `->`, `::`, bitwise compound (`&=` …), or a comma operator.

---

## 7. Punctuation

| Lexeme | Kind           | Notes |
| ------ | -------------- | ----- |
| `(`    | `LeftParen`    | grouping and calls; not required on every call |
| `)`    | `RightParen`   | |
| `{`    | `LeftBrace`    | block form |
| `}`    | `RightBrace`   | |
| `[`    | `LeftBracket`  | |
| `]`    | `RightBracket` | |
| `,`    | `Comma`        | |
| `:`    | `Colon`        | type annotations and an alternate block form |
| `.`    | `Dot`          | member access; not a float by itself |

`{ }` and `:` are both established as possible block delimiters. Which forms are accepted in which constructs is a parser decision. The lexer must emit both.

Semicolon `;` is **not** a v0.1 token. It is an invalid character until approved.

The decimal point in a floating-point literal (Section 4.2) is part of that number token, not `Dot`.

---

## 8. Comments

`#` starts a single-line comment.

The comment runs from `#` through the last character before the next newline (Section 9.2), or through EOF if the file ends on that line.

Comments produce no tokens. `#` inside a string is not a comment.

---

## 9. Whitespace and newlines

Whitespace is not emitted as tokens.

### 9.1 Horizontal whitespace

Space (U+0020) and tab (U+0009) are skipped as trivia and each advances the column by 1.

A tab at the start of a line (before any non-trivia token on that line), including after leading spaces, is **invalid indent**. Block indentation uses spaces only. Tabs remain allowed inside strings and as mid-line trivia after a token has already started the line.

The lexer does not emit `INDENT` / `DEDENT` tokens. The parser uses each token’s `column` (and `line`) to decide block membership.

### 9.2 Newlines

Newlines are skipped and update location as follows:

| Sequence | Meaning                         |
| -------- | ------------------------------- |
| `LF`     | one newline                     |
| `CR LF`  | one newline (CRLF is atomic)    |
| `CR`     | one newline if not followed by `LF` |

After a newline, line is incremented by 1 and column is set to 1.

CRLF is never two newlines and never leaves `\r` as a leftover character.

Other Unicode space characters are **not** whitespace in v0.1. They are invalid characters.

Newlines are not tokens. They still affect token `line` / `column`, which the parser uses for indented blocks (Section 17).

---

## 10. Line and column tracking

- Line numbers start at **1**.
- Column numbers start at **1**.
- A token’s location is the line and column of its **first** source character.
- After emitting a token, the current position is the first character after that lexeme.
- EOF’s location is the position where the file ended (the column after the last character, or column 1 on an empty file).

---

## 11. End of file

After all tokens, the lexer yields exactly one `Eof` token.

`Eof` has an empty lexeme. It does not consume input.

Further `next` calls after `Eof` continue to return `Eof`.

---

## 12. Lexer errors

Lexing fails on the first error. The lexer does not skip the bad input and continue.

Every error includes:

- a message
- line and column of the offending character (opening `"` for unterminated strings; `\` for invalid escapes; the first digit or `.` of `.5` for invalid numbers; the unexpected character otherwise)

| Condition | Error |
| --------- | ----- |
| Character that is not whitespace, and does not start any v0.1 token | Invalid character |
| Unsupported or truncated `\` sequence in a string | Invalid escape |
| String without a closing `"` before newline or EOF | Unterminated string |
| Numeric form that does not match Section 4 | Invalid numeric form |

---

## 13. Token representation

Each token provides:

| Field    | Meaning |
| -------- | ------- |
| `kind`   | token kind (below) |
| `line`   | 1-based start line |
| `column` | 1-based start column |
| `lexeme` | exact original source text of the token |

`Eof` has an empty lexeme.

The lexer does **not** attach decoded numeric values. Integer and float tokens keep the original lexeme only.

String tokens keep the original lexeme, including the surrounding quotes and escape spellings. The lexer must still validate escapes; a decoded string value is not required for v0.1.

### 13.1 Token kinds in v0.1

- `Identifier`
- `True`, `False`, `Null`
- `Const`, `If`, `Elif`, `ElseIf`, `Else`, `For`, `While`, `In`, `Break`, `Continue`, `Match`, `Input`, `And`, `Or`, `Not`, `Func`, `Return`, `Class`, `Constructor`, `This`, `Super`, `Parent`, `Pass`
- `Public`, `Pub`, `Private`, `Priv`, `Protected`, `Prot`
- `Import`, `From`, `As`
- `Integer`, `Float`
- `String`
- `Plus`, `Minus`, `Star`, `Slash`, `Percent`, `PlusPlus`, `MinusMinus`
- `Equal`, `EqualEqual`, `BangEqual`, `Less`, `LessEqual`, `Greater`, `GreaterEqual`
- `PlusEqual`, `MinusEqual`, `StarEqual`, `SlashEqual`, `PercentEqual`
- `AmpAmp`, `PipePipe`, `Bang`, `Amp`, `Pipe`, `Caret`, `Tilde`, `LessLess`, `GreaterGreater`, `Question`
- `LeftParen`, `RightParen`, `LeftBrace`, `RightBrace`, `LeftBracket`, `RightBracket`
- `Comma`, `Colon`, `Dot`
- `Eof`

---

## 14. Open decisions

These are **not** fully specified. Do not invent final syntax or extra tokens for them.

1. **Optional parentheses** — `print` uses a dedicated statement form (`print "Hello"`, comma-separated arguments). A separate **whitespace-call whitelist** covers core scalar builtins only: `abs`, `min`, `max`, `round`, `clamp`, `int`, `float`, `str`, `bool` (for example `abs -1`, `min 1, 2`). User `func`, methods, `range`, I/O, async, and reflection builtins require parentheses.
4. **Type annotation grammar** — prefix and colon forms for `int` / `float` / `bool` / `string` are specified in Section 16. Function parameter/return type annotations are not specified.
5. **Semicolon**, single-quoted strings, raw/multiline strings, `\x`/`\u` escapes.
6. **Additional numeric syntax** — exponents, hex/binary, digit separators, trailing/leading-dot floats.
7. **Additional operators** — exponentiation, arrow, bitwise compound assignment (`&=` …).
8. **Whether newlines are significant** outside block-based syntax — ordinary statements are still token-sequenced; `if` / `for` / `while` / `match` / `func` blocks use indentation (Section 17).
9. **Ownership / safe memory** concrete rules.
10. **Package/distribution** system.
11. **`for` over strings** (async iterators are in §37).
12. **Default/variadic parameters**, function type annotations, and overloading — not part of Functions Aşama 3. Nested functions, closures, and function values are Section 24.

---

## 15. Lexer v0.1 completeness

This specification is sufficient for Lexer v0.1: the token set in Section 13.1 and the identifier, number, string, comment, whitespace, operator, punctuation, location, EOF, and error rules above.

It is **not** a complete language specification. Basic user modules are Section 31. Classes and inheritance are Section 25. Functions Aşama 3 is Section 24.

---

## 16. Language Basics v0.2

This section specifies the statements implemented in v0.2. Newlines are not tokens; statements are sequenced from the token stream.

### 16.1 Programs

A program is a sequence of statements. An empty program is valid.

Implemented statements:

- assignment / declaration
- `print`
- collection literals, index read/write, and method calls (Section 18)

Parser Control Flow adds `if` / `elif` / `elseif` / `else` (Section 17). Loops v1 adds `while`, `for ... in`, `break`, `continue`, and `range()` (Section 19). Logical operators, `input` / conversions, and `match` are Sections 20–21. Functions Aşama 3 is Section 24.

Not in this section: `class` (Section 25), `import` (Section 31).

### 16.2 Assignment and declarations

Three forms have the same assignment meaning when an initializer is present:

```
x = expression
int x = expression
x: int = expression
```

The prefix type and colon type forms are equivalent. The type name and the variable must be on the same line (`int x` / `x: int`). Allowed type names:

| Type name | Value kind |
| --------- | ---------- |
| `int`     | integer    |
| `float`   | float      |
| `bool`    | bool       |
| `string`  | string     |

`str` is an alias for `string` in type annotations and the same string value kind.

`null` is not a variable type.

A type annotation without an initializer is allowed (`int x` or `x: int`). The compiler stores `null` as the starting value. The declared type is still used for later assignments (`int score` then `score = "hello"` is `Type error: expected int, got string`). Assigning the `null` literal to a typed variable (`int x = null`) is a type error; the implicit start value is not treated as a typed initializer.

Unknown type names are compile errors (`Type error: unknown type '…'`).

### 16.3 Type inference

```
x = 10
name = "Emir"
```

If no explicit type is given, the variable is inferred from the initializer. Later assignments to an inferred variable may change the stored value kind. Inference is not a full unification / union type system.

### 16.4 Explicit type checking

If a variable has an explicit type, every initializer and later assignment must match that type exactly. There is no implicit conversion (`int` is not accepted where `float` is required, and vice versa).

Valid:

```
int x = 10
float score = 92.5
string name = "Emir"
bool active = true
int n = 10 + 20
float t = 10.5 + 2.5
```

Invalid:

```
int x = "Hello"
float x = true
string x = 10
bool x = "true"
int x = 10
x = "hello"
```

Compile error form:

```
Type error: expected int, got string
```

Errors include source line and column. Checking is performed at compile time from literals, operators, and known variable types. Type metadata is not added to register bytecode instructions.

A second explicit declaration of the same name in the **same scope** is `duplicate declaration of '…'` (Section 23). Inner-scope declarations may shadow outer names. Assignment without a type annotation updates a visible binding or introduces the name in the current scope.

### 16.5 `print`

`print` is a built-in, not a keyword. Parentheses are optional. Arguments are a comma-separated expression list. At least one argument is required.

```
print("Hello")
print(score)
print "Hello"
print score

print("Hello", "World")
print("Score:", score)
print("A:", 10, "B:", 20)

print "Hello", "World"
print "Score:", score
print "A:", 10, "B:", 20
```

Trailing or leading commas are invalid (`print("Hello",)`, `print(, 10)`).

The wrapping parentheses of the argument list are the `(` **immediately** after `print` (`print(…)`). If there is space before `(`, that parenthesis belongs to the argument expression and the full expression is parsed:

```
print (x * y) / 2
print((x * y) / 2)
```

Both print `20` when `x` is `10` and `y` is `4`. Bare arguments are each a complete expression (`print x + y * 2`, `print x + 10, y * 2, (x + y) / 2`).

Output: arguments are converted with the runtime formatter, separated by a single space, then a newline.

### 16.6 Whitespace calls (core builtins)

Only these builtins may omit parentheses when followed by arguments separated by whitespace: `abs`, `min`, `max`, `round`, `clamp`, `int`, `float`, `str`, `bool`. Examples: `abs -1` (unary minus binds to the argument), `min 1, 2`. This is not general call syntax; user functions, methods, `range`, I/O, async, and reflection builtins require `name(...)`. `print` uses Section 16.5 instead.

Duration literals (`500ms`, `1.5s`, `2m`, `1h`) are allowed only as arguments to APIs that expect durations (such as `delay` and the timeout argument of `timeout`). They are not general numeric literals and cannot be assigned to variables.

```
print("X:", 10, "Y:", 20)
```

prints:

```
X: 10 Y: 20
```

Single-argument `print` compiles to `PRINT src`. Multiple arguments compile each value into consecutive registers and emit `PRINT_N first, count`. This is not a general function-call (`CALL`) mechanism.

### 16.6 Expressions

v0.2 expressions plus Operators 2.0: literals, identifiers, unary `+` / `-` / `not` / `!` / `~`, prefix/postfix `++` `--`, arithmetic, shifts, bitwise `&` `^` `|`, comparisons, `in` / `not in`, logical `and`/`or`, ternary `?:`, assignment / compound assignment, parentheses, collections, and built-in calls.

Precedence (tightest first): primary/`()`; postfix `++` `--`; unary `+ - not ! ~` and prefix `++ --`; `* / %`; `+ -`; `<< >>`; `&`; `^`; `|`; comparisons; `in` / `not in`; `and` / `&&`; `or` / `||`; null-coalescing `??` (right-assoc); ternary `?:` (right-assoc); assignment `=` `+=` `-=` `*=` `/=` `%=` (right-assoc).

`??` is short-circuit: the right operand is evaluated only when the left is `null`.

`%` requires integer operands. Mixed `int`/`float` arithmetic yields `float` at runtime. Bitwise operators require `int`. Logical operators short-circuit (Section 20). Ternary evaluates one branch (Section 22).

---

## 17. Control Flow v0.1

This section specifies `if` syntax, shared block rules, and truthiness. `while` / `for` reuse the same blocks (Section 19). `match` reuses the same `:` / `{ }` block rules (Section 21). User-facing examples: `docs/language/control-flow.md`.

### 17.1 Forms

`if` is required. `elif` is the primary extra-branch keyword. `elseif` is an alternative spelling of `elif` and produces the same AST branch. `else` is optional and has no condition.

`else if` (two tokens) is **not** valid.

`if`, `elif`, and `elseif` each take one condition expression parsed with the Section 16.6 grammar. The condition value need not be `bool`; branch selection uses truthy/falsy (Section 17.5).

Block delimiters are `:` **or** `{ }` per header — not both. `if a > b: { }` is a syntax error. Both forms require indented bodies. Indentation is **semantic for block-based syntax** (`if` / `elif` / `elseif` / `else` / `while` / `for` / `func`): it is validated against the header column. The lexer does not emit `INDENT` / `DEDENT`. Ordinary statements are not Python-style: leading spaces on a top-level statement do not start a block.

There is no `INDENT` / `DEDENT` token. Colon block bounds come from token columns relative to the header column.

`elif` / `elseif` / `else` must share the header column of the `if` they continue, except `} elif` / `} else` on the same line as the closing brace.

### 17.2 Colon blocks

After `:`, the body must start on a **later line** and at a column **greater than** the `if`/`elif`/`else` header column. That first body column is the suite indent. Further body statements must use **exactly** that column. A later token at a column less than or equal to the header ends the suite (so a following `print` at column 1 is outside the `if`). A column between header and suite indent, or deeper than the suite indent, is a syntax error.

Colon bodies do **not** run until `EOF` or the next unindented statement without this rule. This is invalid:

```
if score > 50:
print("Passed")
```

This is valid; `print("Done")` is outside the `if`:

```
if score > 50:
    print("Passed")
    print("Good job")

print("Done")
```

### 17.3 Brace blocks

`{` follows the condition directly. Do not write `:` before `{`. `}` is the explicit closer. Body statements (if any) must start on a later line than `{` and be indented past the header column, with a consistent body indent until `}`. Same-line bodies and unindented bodies are syntax errors:

```
if score > 50 {
    print("Passed")
    print("Good job")
}
```

```
if score > 50 {
print("Passed")
}
```

```
if score > 50: {
}
```

is invalid.

Empty `{}` is allowed. After `}`, `elif` / `elseif` / `else` may continue on the same line.

### 17.4 Conditions and precedence

All three conditional keywords call the same expression parser. Examples:

```
if (a > b):
    …
elif ((a - 5) / 3):
    …
elseif (a + b * 2) > 10:
    …
```

Precedence matches Section 16.6 (including logical `and`/`or` and unary `not`/`!`). For `if a + b * 2 > 10`, multiplication binds before addition, then comparison: `(a + (b * 2)) > 10`. Parentheses override grouping: `if (a + b) * 2 > 10`.

### 17.5 Truthy / falsy

Conditional branch selection uses `Runtime::is_truthy`:

| Value | Truthy? |
| ----- | ------- |
| `true` | yes |
| `false` | no |
| integer `0` | no |
| float `0.0` | no |
| other numbers | yes |
| `""` | no |
| other strings | yes |
| `null` | no |
| empty list `[]` | no |
| non-empty list | yes |
| empty dictionary `{}` | no |
| non-empty dictionary | yes |
| empty tuple `()` | no |
| non-empty tuple | yes |
| empty set | no |
| non-empty set | yes |

### 17.6 AST

```
IfStmt
    condition
    body
    elif_branches[]
        condition
        body
    else_body? 
```

`elif` and `elseif` both fill `elif_branches`. Nested `if` is an ordinary statement inside a body.

### 17.7 Errors

The parser reports located errors for at least: missing condition, missing `:` / `{` block, mixing `:` with `{`, missing or inconsistent indent, `elif` / `else` that is not attached to an `if` or is misaligned, `else if`, unclosed `{`, an unexpected `}`, incomplete `for` headers, missing `while` / `if` conditions, empty `match`, empty match case, invalid match pattern, and `else` used as a match fallback.

---

## 18. Collections v1

User-facing examples: `docs/language/collections.md`.

### 18.1 Lists

`[e1, e2, …]` is a list literal. `[]` is empty. Indexes are 0-based integers. Mixed element kinds are allowed. Nested lists are allowed. There is no `list<int>` (or other generic list type).

Read: `expr[index]`. Write: `expr[index] = value`. Out-of-range or non-integer list indexes are runtime errors. Indexing a non-list with a non-string key is `not a list`.

Trailing commas are invalid.

### 18.2 Dictionaries

`{ "k": v, … }` is a dictionary literal. `{}` is empty. Keys in a literal must be string literals. Runtime keys (including `player[name]` when `name` is a string) must be strings.

Read: `expr[key]`. Write updates an existing key or inserts a new one. Missing key on read is `undefined dictionary key`. Indexing a non-dictionary with a string key is `not a dictionary`.

### 18.3 Tuples

`()` is an empty tuple. `(e,)` is a one-element tuple. `(e1, e2, …)` is a tuple. `(e)` is grouping, not a tuple. Trailing commas are invalid except the required comma in `(e,)`. Indexes are 0-based. Mixed kinds and nesting are allowed. Tuples are immutable: `t[i] = v` is compile-time `tuple is immutable` when the object’s static type is tuple, and the same runtime error otherwise. Methods: `length`, `contains`, `find`, `first`, `last`. Empty `first`/`last` → `tuple is empty`. `find` missing → `-1`. Tuple equality is structural. Tuple destructuring is specified in §26.

### 18.4 Sets

`{e1, e2, …}` is a set literal. Values are unique (runtime `equals`). Unordered; not indexable (`cannot index set`). Mutable: `add`, `remove`, `clear`. `{}` remains an empty **dictionary**. A `{` expression on the same line as `{` that can start an expression is a dict or set; `{` on a later line after `if`/`while` with no condition is still “missing condition”; `{` after a parsed condition is a brace **block**. `"k": v` after `{` is a dict; comma-separated values without `:` are a set. `{1: 2}` is still `dictionary key must be a string`.

Empty set has no dedicated literal; `clear()` or `difference` can yield one. Methods: `add`, `remove`, `contains`, `length`, `is_empty`, `clear`. Operations `union` / `intersection` / `difference` take a set, return a **new** set, and do not mutate operands. Wrong type: `'union' expects a set` (and the same for the other names). `remove` missing → `undefined set value`.

### 18.5 Methods

`object.method(args)` is a `MethodCallExpr`. A call may be a statement (`ExprStmt`) or any other expression. Trailing commas in the argument list are invalid.

List methods: `append`, `prepend`, `first`, `last`, `find`, `contains`, `count`, `remove`, `remove_at`, `clear`, `is_empty`, `length`, `sort`, `reverse`.

Dictionary methods: `get`, `contains_key`, `remove`, `keys`, `values`, `clear`, `is_empty`, `length`. Dictionary keys remain strings.

Tuple methods: `length`, `contains`, `find`, `first`, `last`.

Set methods: `add`, `remove`, `contains`, `length`, `is_empty`, `clear`, `union`, `intersection`, `difference`.

String methods: `find`, `contains`, `replace`, `starts_with`, `ends_with`, `first`, `last`, `is_empty`, `length`. `replace` returns a new string.

`find` on a list, tuple, or string returns `-1` when there is no match. `sort()` requires all numbers or all strings; otherwise `cannot sort incomparable values`. Semantics live in `Runtime::call_method`.

### 18.6 Bytecode

Register instructions (collection data lives on `Value` in the runtime):

- `MAKE_LIST dest, first, count`
- `MAKE_DICT dest, first, pair_count`
- `MAKE_TUPLE dest, first, count`
- `MAKE_SET dest, first, count`
- `GET_INDEX dest, object, index`
- `SET_INDEX object, index, value`
- `CALL_METHOD dest, first, call_index` — object at `first`, arguments at `first+1 …`; `call_index` selects a `CallInfo { name, argc }`

### 18.7 Typed declarations without initializer

`int x` / `x: int` intern the variable, emit `LOAD_CONST` `null` and `STORE_VAR`, and keep the declared type for later `check_type`. This is not an explicit `null` assignment.

---

## 19. Structs, records, and enums

See [structs-enums.md](language/structs-enums.md) for the full guide.

### 19.1 Struct / record

`struct` and `record` declare the same type model. Fields are listed in a `:` or `{ }` block. Optional per-field types use variable annotation syntax.

`typealias Name = type` and `typealias Name<T> = type` introduce compile-time type synonyms (no new runtime type). Cyclic aliases are rejected during type resolution.

```phs
struct Player:
    name
    number
p = Player()
p.name = "Emir"
```

- Instance: `TypeName()` with zero arguments.
- Field read/write: `obj.field` (not a method call; `obj.method()` still requires parentheses).
- Each struct **name** is a distinct type identity.
- Equality is structural within the same struct type.
- No methods, inheritance, constructors, or `this` (those belong to classes — Section 25).
- Field access modifiers: `public`/`pub`, `protected`/`prot`, `private`/`priv` (default `public`). Modifiers apply only to struct/class members, not variables or functions. External access to non-public fields is a compile-time error.

Compile-time `unknown field` when the struct type is known. Runtime `cannot access field on non-struct value` otherwise. Non-public field access: `cannot access private field 'name'` / `cannot access protected field 'name'`.

### 19.2 Enum

```phs
enum Color:
    RED
    GREEN
    BLUE
c = Color.RED
```

- Members are unique; duplicates are compile-time errors.
- `Type.MEMBER` yields a typed enum value (not a plain string/int).
- Equality requires the same enum type and member.
- `match` supports enum member literals as patterns.
- Arithmetic and ordering on enums are not supported.

### 19.3 Bytecode

Metadata: `StructInfo { name, fields[] }`, `EnumInfo { name, members[] }`.

| Opcode | Encoding | Meaning |
| ------ | -------- | ------- |
| `MAKE_STRUCT` | `dest, struct_index` | Empty struct instance; fields initialized to `null`. |
| `GET_FIELD` | `dest, object, field_index` | Read field by compile-time index. |
| `SET_FIELD` | `object, field_index, value` | Write field by compile-time index. |
| `LOAD_ENUM` | `dest, enum_index, member_index` | Enum member value. |

---

## 20. pass statement

`pass` is a no-op statement. It emits no bytecode instructions.

```phs
pass
if condition:
    pass
func noop():
    pass
```

- Valid only as a statement, not an expression (`x = pass` is `invalid expression`).
- When `pass` is the last statement in a function body, automatic return yields `null` (same as an empty body or a non-expression final statement).

---

## 19. Loops v1

User-facing examples: `docs/language/control-flow.md`.

### 19.1 `while`

```
while condition :
    body
```

or

```
while condition { body }
```

Parentheses around `condition` are optional. The condition is any expression (`parse_if_condition` / the shared expression parser). Runtime uses `Runtime::is_truthy`. Missing condition (`while:`) is a parser error.

AST: `WhileStmt { condition, body }`.

Bytecode sketch:

```
loop_start:
    <condition>
    JUMP_IF_FALSE -> loop_end
    <body>
    JUMP -> loop_start
loop_end:
```

`continue` patches to `loop_start`. `break` patches to `loop_end`.

### 19.2 `for ... in`

```
for item in collection :
    body
```

This version implements the **iterator protocol** for built-in collections and user-defined classes.

AST: `ForStmt { item, iterable, body }`.

#### Iterator protocol

An **iterable** provides `iter()`, which returns an **iterator** object. An iterator provides `next()`, which returns the next value on each call. When iteration is exhausted the iterator throws **`StopIteration`** (a built-in exception class). **`null` is a valid iteration value** and does not terminate the loop; only `StopIteration` does.

`for ... in` obtains an iterator once (`GET_ITER`), then repeatedly calls `next()` (`ITER_NEXT`). The VM consumes `StopIteration` **only when it originates from the iterator `next()` call** made by `ITER_NEXT`. A `throw StopIteration()` inside the loop body is **not** treated as normal loop termination.

Built-in iterables: `List`, `Tuple`, `Dict` (keys, insertion order), `Set`, `Range`. User classes implement `iter()` / `next()` with ordinary method dispatch (inheritance, modules, closures).

Compilation:

```
collection = <iterable>
iterator = GET_ITER collection
loop_start:
    ITER_NEXT item, iterator -> loop_end
    STORE item
    <body>
    JUMP -> loop_start
loop_end:
```

`continue` patches to the instruction after `ITER_NEXT` (loop body entry). `break` patches to `loop_end`. Nested loops use a compiler `LoopContext` stack (`break_jumps`, `continue_jumps`).

Manual use: `it = collection.iter()` then `it.next()` uses the same protocol.

**Async iteration (Bölüm 1):** Inside `async func`, `async for item in expr:` / `async for item in expr { … }` repeatedly calls `anext()` or `next()` on the iterator, `await`s the result, and stops on `StopIteration` (same class as sync iteration). Requires the prelude exception types (`compile_with_builtins` / CLI).

### 19.3 `range()`

`range` is a runtime built-in. Call syntax is `CallExpr` compiled to `CALL_BUILTIN`.

- `range(stop)` → start `0`, step `1`
- `range(start, stop)` → step `1`
- `range(start, stop, step)`

Stop is exclusive. `step == 0` → `range step cannot be zero`. Non-integer arguments → `'range' expects integer arguments`. Wrong arity → `'range' expects 1 to 3 arguments`.

`ValueKind::Range` holds `{ start, stop, step }`. `ITER_LEN` / `ITER_GET` compute the current integer without allocating a list. Format: `range(start, stop, step)`. Truthiness: length ≠ 0.

### 19.4 `break` / `continue`

Keywords `Break` and `Continue`. AST: `BreakStmt`, `ContinueStmt`. They affect only the innermost loop. Outside any loop the compiler reports `break outside loop` / `continue outside loop` with the keyword’s location.

### 19.5 Function calls

`name(args)` after an identifier is `CallExpr`. `input(...)` is the same `CallExpr` shape; the callee token is `Input` with name `"input"`. Built-in callees: `range`, `print`, `input`, `int`, `float`, `str`, `bool`, `type` (`CALL_BUILTIN`). User `func` names resolve to `Function` symbols (`CALL`). Other names are compile-time `unknown function '…'`. Statement `print(...)` remains `PrintStmt`. User calls and `range(...)` / `input(...)` / conversions / `type(...)` may be a statement (`ExprStmt`) or an expression.

### 19.6 Bytecode

- `CALL_BUILTIN dest, first, call_index` — arguments at `first …`; `CallInfo { name, argc }`
- `CALL dest, first, function_index` — user function; arguments at `first …`; `FunctionInfo { name, arity, entry, local_count, register_count }`
- `RETURN src`
- `LOAD_LOCAL dest, local_index` / `STORE_LOCAL local_index, src`
- `ITER_LEN dest, collection`
- `ITER_GET dest, collection, index`

`JUMP` / `JUMP_IF_FALSE` / `JUMP_IF_TRUE` are reused. Iterator length/get and `range` / `input` / conversions / `type` live in `Runtime`; the VM only dispatches.

---

## 20. Logical operators, `input`, conversions

User-facing: `docs/language/operators.md`, `docs/language/types.md`.

### 20.1 Logical operators

Parser: `parse_or` → `parse_and` → `parse_comparison`. Unary `not` / `!` share `parse_unary` with `+` / `-`.

`and` and `&&` are the same `BinaryExpr` role (`TokenKind::And` vs `AmpAmp`). `or` / `||` likewise. `not` / `!` are `UnaryExpr`.

Compiler `compile_logical_into`:

```
<left> into dest
AND: JUMP_IF_FALSE dest -> after_right
OR:  JUMP_IF_TRUE  dest -> after_right
<right> into dest
after_right:
```

The right operand is compiled only on the fall-through path. Unary not emits `UNARY_NOT dest, src` (`bool` from `!Runtime::is_truthy`).

The result of `and`/`or` is the last evaluated operand value (not forced to `bool`).

### 20.2 `input`

`input` is a keyword. `input()` / `input(prompt)` is `CallExpr` compiled to `CALL_BUILTIN`.

- 0 args: no prompt
- 1 string arg: write prompt with no extra newline, flush, then `getline`
- always returns `string` (line without `\n`; trailing `\r` stripped)
- no automatic number/bool parsing
- EOF / missing stream: `end of input`
- non-string prompt: `'input' expects a string prompt`
- extra args: `'input' expects 0 or 1 argument`

`Runtime` takes optional `istream` (`Runtime(ostream, istream)`). Default construction uses `std::cin`.

### 20.3 Conversions

Built-ins `int` / `float` / `str` / `bool` (identifiers, so `int x` declarations remain valid). One argument. Invalid conversions throw `RuntimeError`. No cast syntax (`as`, `(type)value`).

- `int`: int unchanged; float truncates toward zero; bool → 0/1; trimmed string via `stoll` base 10, whole string; else fail
- `float`: numeric/bool; trimmed string via `stod`; else fail
- `str`: `Runtime::format` of the value
- `bool`: bool unchanged; numbers non-zero; null false; collections/range use `is_truthy`; string after trim+ASCII lower: `true`/`1` true, `false`/`0`/empty false, else fail

Values stored in `any` convert using their runtime `ValueKind`, not the annotation.

---

## 21. Match v1

User-facing: `docs/language/control-flow.md`.

`match` is a statement and a value-producing expression (`MatchExpr`). No `else` fallback (`else` remains `if`-only). Parser error: `match uses '_' as the fallback, not else`. Case guards use `if` after the pattern (`n if n > 10:`), not a separate `match if` form.

Forms:

```
match expr :
    cases
```

```
match expr { cases }
```

Mixing `:` and `{` on the header is invalid. Empty `match` (`no cases`) is a parser error. Empty case body is `empty match case`.

AST:

```
MatchStmt
    value
    cases[]
        MatchCase
            literals[]     # literal patterns (optional unary +/-)
            wildcard       # _
            binding?       # identifier, not _
            guard?         # expression after `if`
            body[]
```

A case is one of: wildcard `_`; binding identifier with optional `if` guard; or one or more comma-separated literals. `_` cannot have a guard or commas. Bindings cannot be mixed with commas.

Semantics:

- evaluate `value` once into a register
- cases top to bottom; first match wins; no fallthrough
- literal: `Equal` against the stored value; `JUMP_IF_TRUE` to body; miss → next case
- multiple literals: any hit jumps to the same body
- `_`: body, then jump to match end; later cases are compile-time `unreachable match case`
- binding: new compiler scope + `STORE_VAR` of the match value; optional `JUMP_IF_FALSE` on guard; body; restore previous name mapping (`pop_scope`) so the binding does not leak
- unguarded binding is exhaustive (later cases unreachable)
- duplicate literal keys: compile-time `duplicate match pattern`

Nested `match` and nested `if`/`while`/`for` in case bodies are ordinary statements.

Opcodes added for this work: `UNARY_NOT`, `JUMP_IF_TRUE`. Register VM encoding unchanged (`{opcode, a, b, c}`).

---

## 22. Operators 2.0

User-facing: `docs/language/operators.md`.

AST: `AssignExpr` (plain and compound), `UpdateExpr` (prefix/postfix `++` `--`), `MembershipExpr`, `TernaryExpr`. Targets are `IdentifierExpr` or `IndexExpr`.

Compound assignment compiles as load target (once) → binary op → store. Prefix `++` stores then yields the new value; postfix yields the old value then stores.

Ternary:

```
<condition>
JUMP_IF_FALSE -> else
<then> into dest
JUMP -> end
else:
<else> into dest
end:
```

Membership: `CONTAINS dest, value, container`; `not in` then `UNARY_NOT`. Runtime `contains` for list/tuple/set/dict/string/range without materializing ranges.

New opcodes: `UNARY_BIT_NOT`, `BIT_AND`, `BIT_OR`, `BIT_XOR`, `SHL`, `SHR`, `CONTAINS`.

---

## 23. Scope

User-facing: `docs/language/scope.md`.

Lexical block scope, resolved at compile time to slots. Globals use `STORE_VAR` / `LOAD_VAR`. Function parameters and function-locals use cells addressed by `LOAD_LOCAL` / `STORE_LOCAL` or, when captured, `LOAD_UPVALUE` / `STORE_UPVALUE`. The compiler keeps a stack of `Scope` objects (global first). Each scope has `symbols` (names introduced here) and restoration records so `pop_scope` restores the parent visibility map. Symbol kinds include `Variable`, `Function`, and `Class`; access defaults to `Public`. Function declarations (including nested `func`) are hoisted in their lexical scope (Section 24). Class declarations are pre-registered (hoisted) before bodies are compiled; parent classes may appear later in source. Ordinary variables are never hoisted: a use in the same function before the introducing assignment is `undefined variable`. Member access modifiers are enforced for struct/class fields (Section 25).

Bodies of `if` / `elif` / `else` / `while` / `for` / `match` cases are compiled with `compile_scoped_block` (push/pop). `for` also pushes a header scope; the item name uses assignment lookup (update a visible binding, or introduce in that header scope).

- **Assignment** (`AssignStmt` without a type, `AssignExpr`, `++`/`--`): `resolve_assign` — existing visible slot, else introduce inferred in the current scope.
- **Declaration** (prefix or colon type): `declare_variable` — error if the current scope already defines the name (`duplicate declaration of '…'`); otherwise a new slot that may shadow a parent.
- **Lookup**: `undefined variable '…'` if the name is not in the current visibility map. `print(x)` before `x = 10` fails; there is no variable hoisting.

Match case bindings use `declare_variable` in the case scope so they do not leak.

---

## 24. Functions Aşama 3

User-facing: `docs/language/functions.md`. Architecture: `docs/architecture/compiler.md`, `docs/architecture/bytecode.md`, `docs/architecture/register-vm.md`.

### 24.1 Syntax

`func name(params) :` indented suite, or `func name(params) {` indented body `}`. Same mix ban as other headers. Parameters are identifiers; no trailing comma; duplicate names → `duplicate parameter '…'`. Nested `func` inside `func` (and inside nested blocks of a function) is valid. Duplicate nested names in one scope → `duplicate declaration of '…'`.

`return` is `TokenKind::Return`. Optional expression only if it starts on the same line as `return`. Outside a function → `return outside function`.

Inside a function, a value-producing expression may be a statement (`ExprStmt`) so the last line can auto-return. Nested `FuncStmt` is not an `ExprStmt` and is skipped for automatic-return selection. Prefix / colon type declarations require the type tokens on the **same line** as the name (`int x`), so a bare identifier on its own line is not parsed as a type.

AST: `FuncStmt { name, parameters, body }`, `ReturnStmt { value }`. Calls remain `CallExpr`.

### 24.2 Compiler

Top-level `func` names are registered as `SymbolKind::Function` **before** main statements are compiled (hoisting). Nested `func` names are hoisted at the start of their lexical block. Variable uses are still not hoisted: the compiler may allocate a local slot early so nested functions can capture it, but a load in the owning function before the introducing store is `undefined variable`. Function bodies are compiled **after** `HALT` so they see global slots already introduced in main. Nested function *code* is emitted before the enclosing function’s `entry`; `MAKE_CLOSURE` runs at `entry`. Parameters are `declare_variable` in a function scope (`local` cells). Direct calls to a known top-level function check arity at compile time (`'name' expects N arguments`). Calls through a variable or nested function use `CALL_VALUE`; arity is checked at runtime.

Capture analysis: a load/store of a non-global name owned by an enclosing function interned a `Capture` `{ index, from_local }` on `FunctionInfo`. Unused locals are not captured. Globals stay `LOAD_VAR` / `STORE_VAR`.

`compile_function_body`: if the last top-level non-`FuncStmt` is `ExprStmt`, emit its value and `RETURN`. Otherwise compile it and emit `null` + `RETURN`. Nested `if` / loop / `match` expressions are not auto-returned. Explicit `return` emits `RETURN` immediately; it is not a loop jump.

Do not hold a `FunctionInfo&` across `hoist_functions` / nested compile: `Bytecode.functions` may reallocate.

### 24.3 VM

`CALL` pushes a `CallFrame` (return IP, dest register, saved registers, local cells, upvalue cells), allocates the callee’s register file and a **new cell** for each local, copies arguments into locals `0 .. arity-1`, jumps to `entry`. `MAKE_CLOSURE` copies `shared_ptr<Cell>` from current locals/upvalues according to `FunctionInfo.captures`. `LOAD_FUNCTION` builds a function value with empty captures. `CALL_VALUE` requires `ValueKind::Function` (`invalid function value call` otherwise), checks arity (`'name' expects N arguments`), and enters with the closure’s captured cells as upvalues. `RETURN` restores the caller. Recursion uses CallFrames; no global mutable call state.

A normal local cell dies with its frame unless a closure still holds that `shared_ptr`. Capture is by **binding** (shared cell), not by copying the current value.

Not specified: default/variadic parameters, type annotations on functions, overloading.

---

## 25. Classes and inheritance

User-facing: `docs/language/classes.md`. Architecture: `docs/architecture/compiler.md`, `docs/architecture/bytecode.md`.

### 25.1 Syntax

```phs
class Player:
    string name

class FootballPlayer : Player:
    int number
```

Brace blocks and `pass` in empty bodies are supported. Inheritance uses `Child : Parent`, not `extends`. **Multiple inheritance and `override` are not supported.**

Keywords: `class`, `constructor`, `func`, `this`, `super`, `parent`, `pass`, and member access modifiers (`public`/`pub`, `protected`/`prot`, `private`/`priv`).

### 25.2 Semantics

- Class instances are reference values (`ValueKind::Class`).
- Fields, methods, and constructors follow the access matrix in `docs/language/classes.md`.
- Field and method lookup walk the inheritance chain (child → parent → …).
- Method calls dispatch on the runtime class (overrides apply). `super.method()` / `parent.method()` call the parent implementation.
- Constructors: implicit `this` at local 0; no return values; parent initialization via `super(...)` / `parent(...)` when required.
- `this` is valid only inside class methods/constructors.

### 25.3 Compiler

Two-pass class compilation: `predeclare_class` (register name and parent), `finalize_class` (fields, methods, constructor indices), `validate_class_cycles`, `rebuild_all_class_layouts` (flattened field layout parent-first), `validate_class_constructors`, then compile method/constructor bodies.

`ClassInfo` stores `parent_index`, own `fields`, flattened `layout` (with `declaring_class_index` and access per slot), `methods`, and `constructor_index`.

### 25.4 Bytecode / VM

| Opcode | Meaning |
| ------ | ------- |
| `INVOKE_CLASS` | Allocate instance (flattened layout), optionally call constructor |
| `CALL_CLASS_METHOD` | Call method by `function_index` with receiver + args |
| `INVOKE_SUPER` | Parent constructor on existing `this` (chaining) |
| `GET_FIELD` / `SET_FIELD` | Field access by flattened layout index |

`ClassCallInfo` records `ClassCallKind` (`Instance`, `Method`, `SuperMethod`, `SuperCtor`), `class_index`, `function_index`, `argc`.

Compile-time errors include: `unknown parent class`, `multiple inheritance is not supported`, `circular inheritance`, `missing required parent constructor call`, `invalid use of 'super'` / `'parent'`, and access violations on inherited members.

---

## 26. Destructuring (complete)

User-facing: `docs/language/destructuring.md`.

### 26.1 Syntax

**Assignment** — parsed when a destructuring lookahead is followed by `=`:

```phs
[a, b, c] = [10, 20, 30]
(x, y) = (10, 20)
[a, (b, c)] = [10, (20, 30)]
[_, b, _] = [10, 20, 30]
[a, *rest] = [1, 2, 3, 4]
(a, *rest) = (1, 2, 3)
{"name": name, "age": age} = person
{"name": name, *rest} = person
Player{name, age} = player
Player{name, address: Address{city}} = player
```

**Parameters** — a parameter may be a `DestructPattern` instead of a simple identifier:

```phs
func add((a, b)): ...
func first([a, b]): ...
func greet({"name": name}): ...
func read(Point{x}): ...
```

`DestructPatternKind`: `Identifier`, `Wildcard`, `Rest`, `List`, `Tuple`, `Dict`, `Struct`. Statement: `DestructureAssignStmt`. `Parameter.pattern` optional.

Not supported: rest not last (`[*a, b]`, `{*rest, "x": x}`), multiple rest in one pattern, non-string dict keys, class-instance destructuring.

### 26.2 Semantics

- RHS / argument evaluated once.
- List/tuple without rest: exact element count at each collection level.
- List/tuple with rest: fixed slots satisfied; remainder → **new `List`** (tuple sources included).
- Dict: key-based binding; order irrelevant; missing key → runtime error.
- Dict rest: remainder → **new `Dict`**; at most one rest, last position.
- Struct: field-name binding via `TypeName{fields}`; unknown field → compile error; private/protected → compile error.
- Struct rest: remaining public fields → **new `Dict`**.
- `_`: no symbol/slot.
- `*name`: normal binding; rest must be last; at most one per pattern.
- Parameter destructuring: arity unchanged; bindings are callee locals after frame setup.

### 26.3 Compiler / bytecode

- `CheckDestruct collection, count, kind` — `kind` `0`/`1` = list/tuple exact; `2`/`3` = list/tuple minimum (rest patterns).
- `DestructRest dest, source, start_index` — list/tuple tail → new `List`.
- `DestructDictRest dest, source, packed` — dict tail → new `Dict`; `packed` encodes excluded string-key registers.
- Dict slots: `GetIndex` with string key register. Struct slots: `GetField` with compile-time field index and access check.
- Parameters: arguments stored in hidden locals (`__argN`), then `compile_parameter_destructure()` reuses assignment destructuring logic.
- `FunctionInfo.returns_value` — void trailing `ExprStmt` in function bodies (§24).

### 26.4 Void function calls as statements

When a function’s last top-level statement is an `ExprStmt` calling a callee that does not return a value, compile the call as a statement and emit `RETURN null`. Does not change the `null` value system or explicit `return` / recursion / closure behavior.

---

## 27. Type system — `any` and compatibility (Bölüm 1)

User-facing: `docs/language/types.md`.

### 27.1 `any`

- Syntax: `any x = …` (prefix or `x: any = …`).
- `StaticType::Any` in compiler; no new runtime `ValueKind`.
- Runtime values keep their actual kind (`any x = 10` stores an `int` value).
- Distinct from inference (`x = 10` infers `int` and rejects later incompatible assignments).

### 27.2 Compatibility (`is_assignable`)

Central helper in `src/compiler/type_compat.cpp`. Not the same as exact equality.

| Rule | |
| ---- | - |
| Target `any` | always assignable |
| Source `any` → typed target | compile OK + runtime `CheckAssign` |
| `int` → `float` | compile OK + `CoerceToFloat` |
| `float` → `int` | compile error |
| Child class → parent class | compile OK (walk `ClassInfo.parent_index`) |
| Parent → child | compile error |
| Same struct / enum index | compile OK |
| Different struct / enum | compile error |
| `Unknown` source | compile OK (legacy inference paths) |

### 27.3 Bytecode

- `CoerceToFloat dest, src` — `int` → `float` in place.
- `CheckAssign dest, src, type_check_index` — runtime validation using `Bytecode.type_checks` (`TypeCheckInfo`).
- `FunctionInfo.parameter_type_names` — call-site argument checking.

Function return types are not changed in this section; compatibility infrastructure is shared with assignment and parameters.

---

## 28. Type system — generics (Bölüm 2)

User-facing: `docs/language/types.md`, `docs/language/collections.md`.

### 28.1 Syntax

- `list<T>` — element type `T`
- `map<K,V>` — key `K`, value `V` (`K` must be `string` at compile time)
- Nested: `list<list<int>>`, `map<string,list<int>>`
- Aliases: `List`, `Dict`, `dict` normalize to `list` / `map`
- Parsed only in type annotation contexts; expression `a < b` unchanged
- `>>` in types: lexer emits `GreaterGreater`; parser splits one `>` for nested closes

### 28.2 Representation

`TypeDescriptor.type_args` in `src/compiler/types.h`. No generic tags on runtime `Value`.

### 28.3 Checking

- Assignments and parameters: `is_assignable` requires invariant generic match
- Literals: homogeneous inference; mixed literals → untyped collection
- Empty `[]` / `{}` with explicit generic annotation: OK
- Methods: `append` / `prepend` / `insert` checked against list element type when known
- `any` → generic: compile OK + runtime `CheckAssign` with nested `type_arg_indices`

### 28.4 Not in this section

- `tuple<…>`, `set<T>` syntax
- Function return type annotations
- Exception-based error reporting

---

## 29. Type system — conversion + `type()` (Bölüm 3)

User-facing: `docs/language/types.md`.

### 29.1 Explicit conversion

- API: `int(value)`, `float(value)`, `str(value)`, `bool(value)` only
- Compiler: `is_known_builtin` includes these names; `CALL_BUILTIN` dispatch
- Runtime: `Runtime::convert_int` / `convert_float` / `convert_bool`; `str` via `format`
- Errors: `cannot convert <kind> to <target>` (invalid numeric strings fail loudly)
- Assignment compatibility (`is_assignable`, `CheckAssign`) is separate from conversion

### 29.2 `type()`

- Built-in `type(value)` → `string` (`Runtime::runtime_type_name` / `kind_name`)
- Returns runtime `ValueKind` name: `int`, `float`, `string`, `bool`, `null`, `list`, `dict`, `tuple`, `set`, `struct`, `class`, `enum`, `range`, `function`
- No generic metadata (`list<int>` values report `"list"`)
- Compiler infers `StaticType::String` for `type(...)` calls

### 29.3 Type System Completion

Sections 27–29 complete the type system for this milestone:

1. `any` + compatibility (Bölüm 1)
2. Generics (`list<T>`, `map<K,V>`) (Bölüm 2)
3. Conversion + `type()` + finalization (Bölüm 3)

Exception system is out of scope.

---

## 30. Exceptions (Bölüm 1–3)

User-facing: `docs/language/control-flow.md`, `docs/language/types.md`.

### 30.1 Syntax

Keywords: `try`, `catch`, `finally`, `throw`. `try` requires one or more `catch` clauses and/or `finally`. Catch forms: `catch name:` (untyped) or `catch TypeName name:` (typed). Block forms: colon+indent or `{ }`.

AST: `TryStmt { try_body, catches[], finally_body? }`, `CatchClause { type_name?, binding, body }`, `ThrowStmt { value }`.

`throw` is a statement only. Payload is any `Value`; typed exceptions are `ValueKind::Class` instances. No new `ValueKind`.

### 30.2 Built-in and user exception types

CLI execution prepends a prelude defining `Exception` → `RuntimeException` → `ValueException` / `TypeException` / `NameException` / `IndexException` / `StopIteration` / `CancellationException` / `TimeoutException` as ordinary classes (`message`, `cause`, `file`, `line`, `column`, `stack_trace` fields). User types use `class MyError : Exception:` and the same inheritance rules as other classes.

### 30.3 Catch matching

VM `catch_entry_matches` uses `is_class_or_subclass` for typed clauses; untyped clauses match any value. Multiple catches are ordered; `HandlerFrame.next_catch` supports rethrow to a later clause in the same `try`. Compile-time: unknown catch types and unreachable subtyping order are rejected.

### 30.4 Scope

Catch bindings use `declare_variable` with optional type name (typed catch). Bindings are block-local; use outside the catch block is `undefined variable`.

### 30.5 Bytecode

`HandlerInfo { catches[], finally_ip, end_ip }` with `CatchEntry { catch_ip, catch_slot, catch_is_local, catch_class_index }`.

Opcodes:

| Opcode | Meaning |
| ------ | ------- |
| `ENTER_TRY h` | Push handler frame |
| `LEAVE_TRY h` | Pop handler; jump to `finally_ip` if set |
| `THROW src` | Unwind to handler or across `CallFrame`s |
| `END_FINALLY` | Re-throw `pending_exception` or complete `pending_control_` |
| `DEFER_RETURN h, src` | Save return; pop handler; run `finally` |
| `DEFER_BREAK h, jump` | Save break target; pop handler; run `finally` |
| `DEFER_CONTINUE h, jump` | Save continue target; pop handler; run `finally` |

### 30.6 VM

`handler_stack_` tracks active handlers with `next_catch`. On match, `unwind_to_frame_depth` restores the try-site register file before the catch body. `pending_control_` holds deferred return/break/continue through `finally`. `return`/`throw` inside `finally` clear pending state and take priority.

### 30.7 Runtime error integration (Bölüm 3)

`Runtime::fail` / `RuntimeError` in opcode dispatch is caught by the VM and converted via `raise_runtime_error` into a built-in exception instance (`RuntimeErrorCategory` → `TypeException` / `NameException` / `IndexException` / `ValueException` / `RuntimeException`). Lexer/parser/compiler errors stay compile-time (`Compile error:`).

Propagation uses the same `throw_value` / handler / `CallFrame` unwind path as user `throw`. `finally` semantics from Bölüm 2 are unchanged.

### 30.8 Source location and stack trace

The compiler emits `instruction_locations[]` parallel to `instructions` and stores `Bytecode.source_file` / `source_line_base` (prelude offset). `capture_stack_trace` builds `stack_trace` as `list[dict]` with `function`, `file`, `line`, `column`. User `throw` enriches exception class instances via `enrich_exception_value`.

### 30.9 Unhandled output

Top-level uncaught values produce multi-line CLI output:

```
Unhandled exception: <summary>
  at <function> (<file>:<line>:<column>)
caused by:
...
```

Primitive payloads use `Runtime::format`. Internal VM faults (`invalid register`, corrupt bytecode) remain non-catchable `VmError` / `Error:` lines.

---

## 31. Modules / Imports (complete)

User-facing: `docs/language/modules.md`.

### 31.1 Syntax

```phs
import math
import math as m
import utils.math

from math import add
from math import add, subtract
from math import add as plus
```

AST: `ImportStmt { module, module_alias?, bindings[], from_import }`, `ImportBinding { name, alias?, loc }`.

### 31.2 Module visibility and exports

Top-level module declarations accept `pub` / `priv` (and `public` / `private` aliases). **Default visibility is public** when no modifier is written.

- `pub` names appear in the module export table and are visible through `import` / `from … import`.
- `priv` names are module-internal. `math.secret` and `from math import secret` are rejected.
- `prot` / `protected` at module level is a **compile-time error** (`protected is not allowed at module level`). Class/struct field `protected` rules are unchanged.

Exports may be variables, functions, classes, enums, and structs. Only **public** symbols are exported.

### 31.3 Compile-time loading and cache

`ModuleCompileCache` resolves `.phs` paths relative to `CompileOptions.source_file`, compiles dependencies recursively, and stores:

- `Bytecode.modules[]` — `ModuleDescriptor` (specifier, canonical path, bytecode index, exports)
- `Bytecode.module_bytecodes[]` — per-module bytecode blobs

Canonical filesystem paths deduplicate module identity (`math.phs`, `./math.phs`, `subdir/../math.phs`). Compile states: **Loading**, **Loaded**, **Failed**. A placeholder descriptor is registered before recursive compilation so circular imports resolve to the same module index instead of infinite recursion.

Partial export tables are refreshed after the declaration pass so `from … import` can validate against symbols declared before later module body statements.

### 31.4 Circular imports

Cycles such as `a → b → a` are supported at compile time via the loading placeholder. At runtime, module init states are **Unloaded → Loading → Loaded** (or **Failed**). Re-entering `Loading` during initialization does not restart top-level execution.

Accessing an export whose global slot is not initialized yet produces a controlled runtime error (`uninitialized module export '…'`). Failed modules stay failed; later imports surface the same failure message.

### 31.5 Importer bindings and semantics

- `import math` → `IMPORT_MODULE` stores a `ValueKind::Module` namespace (**live** member lookup).
- `from math import value` → snapshot at import execution time for variables/functions; **compile-time linked** types (class / enum / struct) bind to the importer symbol table without a runtime `IMPORT_NAME`.
- `math.add(...)` uses `CALL_METHOD`; module-bound functions/classes switch `execution_bytecode_` and restore importer state via `CallFrame`.
- `math.member` / `math.member = x` use dynamic `GET_INDEX` / `SET_INDEX` on the module namespace (live mutable globals when public).
- Cross-module inheritance (`from base import Animal` then `class Dog : Animal`) links class metadata into the importer bytecode with `module_index` for runtime dispatch.

### 31.6 Module execution

On first import, the VM runs module top-level bytecode in isolated globals, caches them in `module_states_`, and marks the module **Loaded**. Module init failures set **Failed** and propagate through the existing exception / cause / stack-trace machinery.

### 31.7 Errors

- Missing module: runtime `NameException`-category error (`cannot find module '…'`).
- Missing or private export in `from … import`: compile-time error (`has no export '…'`).
- Duplicate importer binding / alias: compile-time error.
- Module init / nested import errors propagate with stack frames and `caused by:` chains preserved.

### 31.8 Built-in standard library modules

When file resolution fails, these specifiers load native modules bundled with PScript: `math`, `string`, `collections`, `path`, `fs`, `io`, `os`, `process`, `sys`, `time`, `datetime`, `random`, `json`, `re`, `encoding`. User `.phs` files in the importer directory still win (same cache/canonical-path rules as file modules).

User-facing reference: `docs/language/stdlib.md`.

Native modules use the same `import` / `from … import` syntax and export tables as file modules. Implementation is native (C++) for `math`, `path`, `fs`, and `io`; string methods are on the string primitive plus `string.join`; `collections` provides generic `Stack` / `Queue` / `Deque` classes.

**Bölüm 2 (`path`, `fs`, `io`):** `path` is lexical/path-only (no filesystem mutation). `fs` provides synchronous whole-file and directory helpers with UTF-8 text I/O, plus async helpers `async_read` / `async_write` / `async_append` (Update Bölüm 2). `io.open` returns a native file handle with `read` / `write` / `flush` / `close` / `is_open` and async `async_read` / `async_write`. Errors propagate through the existing exception hierarchy.

**Bölüm 3 (`os`, `process`, `sys`):** `os` exposes environment, working directory, and directory helpers without duplicating `path`/`fs`. `process.run` executes a shell command synchronously and returns `{exit_code, stdout, stderr}`. `process.exit(code)` integrates with CLI exit codes. `sys.args` lists CLI arguments after the script path; `sys.version` comes from `CMake project(VERSION)`. Async process control is deferred.

**Bölüm 4 (`time`, `datetime`, `random`):** `time.now()` returns a native `DateTime` value; `time.unix()` / `time.monotonic()` expose fractional seconds; `time.sleep()` blocks synchronously (distinct from async `delay()`). `datetime` provides factory functions, field access, deterministic `parse`/`format`, and `DateTime`/`Duration` arithmetic via existing VM binary operators. Internal precision is microseconds; user-facing seconds are floating-point where exposed. No IANA timezone database in v1. `random` uses `std::mt19937_64` (not cryptographically secure). `type()` reports `"datetime"` and `"duration"` for the new native class sidecars.

**Bölüm 5 (`json`, `re`, `encoding`):** `json` encodes/decodes null, bool, numbers, strings, lists, tuples (as arrays), and string-key maps; unsupported values raise `ValueException`. `json.valid(text)` returns `true` when `decode` would succeed and `false` otherwise (no exception). Pretty output uses a positional `true` flag (no named parameters). `re.is_match` uses full-string `regex_match` semantics; `search` returns `Match` or `null`. `encoding` introduces first-class `ValueKind::Bytes` (indexing, iteration, `type()` → `"bytes"`). UTF-16 is UTF-16LE with BOM. ASCII is strict U+0000..U+007F. Existing `fs`/`io` string APIs are unchanged.

### 31.9 Limitations

- Package distribution / versioned dependencies are out of scope.
- `import *` is not supported.
- Enum namespace exports (`types.Color`) are runtime dict views; compile-time `Color.RED` after `from types import Color` uses linked enum metadata.

---

## 32. Async / Await — Core (Bölüm 1)

User-facing: `docs/language/functions.md`. Architecture: `docs/architecture/register-vm.md`, `docs/architecture/bytecode.md`, `docs/architecture/compiler.md`.

### 32.1 Syntax

- `async func name(params):` declares an async function. Same block rules as `func`.
- `await expr` suspends the current async function until the awaited `Task` completes.
- `async` is valid only on function declarations (including async class methods). **Async constructors are rejected** at parse time.
- **`await` in a normal `func` is a compile-time error** (`await outside async function`).
- **Top-level `await`** is allowed in scripts that do not define `async func main()`. The compiler synthesizes an internal async entry (`__pscript_module_entry__`) and the VM runs it through the scheduler (same path as `async func main()`).
- Nested async functions and async methods are supported.

```phs
async func fetch():
    await delay(500ms)
    return 42

async func main():
    x = await fetch()
    print(x)
```

### 32.2 Semantics

- An **async function call** does not run the body synchronously. It returns a **`Task`** value immediately.
- **`await`** accepts a `Task` (or `any` with a runtime check). It is **non-blocking**: the VM scheduler runs other ready tasks while waiting.
- From the caller’s perspective, `async func foo() -> T` behaves like returning **`Task<T>`**; the compiler maps return/`await` typing without exposing a user-facing `Task` generic in this milestone.
- **`async func main()`** is the program entry when present. The CLI runs it through the VM scheduler and waits for that root task to finish. A normal `func main()` keeps synchronous execution.
- **Fire-and-forget:** calling an async function without `await` at top level (sync scripts) returns a `Task` immediately; the VM drains spawned background tasks after synchronous top-level bytecode finishes so short scripts still observe async side effects (for example `print` inside the async body).
- **No cancellation**, **no timeout**, **no async I/O**, and **no multithreading** in Bölüm 1 (see later sections for extensions).

### 32.3 Tasks and scheduler

- Runtime `ValueKind::Task` wraps internal `TaskData` (`Pending`, `Running`, `Completed`, `Failed`).
- A completed task stores its result; a failed task stores the exception value until an `await` rethrows it into the awaiting async frame.
- The scheduler is **single-thread cooperative**: ready tasks run until they `await` or complete; timer tasks become ready when their deadline passes.
- First async primitive: **`delay(duration)`** built-in → `Task` that completes after the given duration without blocking the OS thread (scheduler sleep when idle). Duration literals use suffixes `ms`, `s`, `m`, or `h` (for example `await delay(1.5s)`). Values normalize to whole milliseconds (half away from zero). Bare integer arguments are invalid. Use `await delay(...)` for intentional waits; a bare `delay(...)` expression statement is a compile error.

### 32.4 Exceptions

- An uncaught exception inside an async task marks the task **Failed** and stores the enriched exception object.
- **`await`** on a failed task throws that exception into the awaiting function, preserving existing **cause**, **finally**, **stack trace**, and **source location** behavior.

### 32.5 Closures and modules

- Async functions use the same capture model as sync functions. Captured cells survive suspend/resume.
- Module-bound async calls restore `execution_bytecode_`, module globals, and `CallFrame` state across suspend/resume like synchronous module calls.

### 32.6 Bytecode / VM (summary)

- `FunctionInfo.is_async` — callee returns a `Task` instead of entering synchronously when `scheduler_active_`.
- `Bytecode.async_main_function_index` — root async entry for the scheduler.
- **`AWAIT dest, task_reg`** — suspend current task if `task_reg` is pending; otherwise move result/exception into `dest`.
- Suspend saves IP, register file, locals, upvalues, call frames, handler stack, module execution context, and pending control/exception state into the waiting task; resume restores it and clears VM frames so nested task execution cannot corrupt the waiter’s saved stack.

## 33. Async / Await — Scheduler & Runtime (Bölüm 2)

Architecture detail: `docs/architecture/scheduler.md`.

### 33.1 Task lifecycle

- States: `Pending`, `Running`, `Completed`, `Failed`.
- A task may re-enter `Pending` after `Running` when it suspends at `await` or while waiting on a timer task.
- **Completed** and **Failed** tasks are terminal: the scheduler never runs them again.
- Task fields (`state`, `result`, `exception`) are separate from saved VM execution state (`AsyncExecutionState`).

### 33.2 Scheduler behavior

- VM-owned, **single OS thread**, cooperative.
- Maintains a runnable queue and timer list; switches tasks only at `await` / timer suspension.
- Runs **all** tracked tasks to completion (including fire-and-forget spawns), not only the root `async main` body.
- When the run queue is empty but timers are pending, sleeps until the next deadline (no busy-wait).
- Top-level imports and statements execute **before** the scheduler starts (`run_top_level_init`).

### 33.3 Multiple cooperative tasks

Independent async calls (each with its own `delay`, closure captures, or module entry) interleave at suspension points: while task A waits, task B may run, and vice versa.

### 33.4 Unhandled async exceptions

- A failed task stores its exception until an `await` propagates it.
- When `await` rethrows a failed task’s exception, that task is marked **consumed**; a caller `catch` handles it like a synchronous throw.
- At scheduler shutdown, **unawaited** failed tasks (including background spawns never awaited) produce the same unhandled-exception `VmError` as synchronous code.

### 33.5 `finally` + `await`

`try` / `finally` with `await`, nested `await` in `finally`, and `return` through `finally` in async functions preserve the existing exception state machine. Async completion after `finally` uses the same root-frame detection as direct `return`.

### 33.6 Modules, classes, closures

- Imported async functions (`import math` → `await math.compute()`) spawn tasks through the module call path.
- Async instance methods keep `this` across suspend/resume; overridden async methods dispatch normally.
- Closures captured by async functions remain valid after task completion.

## 34. Async / Await — Task Control (Update Bölüm 1)

Task control extends the cooperative scheduler with **cancellation**, **`race()`**, and **`timeout()`**. No new scheduler type, thread pool, or async I/O.

### 34.1 `task.cancel()`

- Method on `Task` values: `task.cancel()`.
- **Cooperative** — does not kill OS threads; the VM observes cancellation at safe points (`await`, between bytecode steps).
- Terminal tasks (`Completed`, `Failed`, `Cancelled`) ignore further cancel requests.
- Cancelled tasks store a `CancellationException` and transition to **`Cancelled`** (terminal).
- `await` on a cancelled task rethrows that exception into the awaiting frame (catchable like other exceptions).
- **`try` / `finally` is preserved** — cancellation runs `finally` before the task becomes terminal.
- Multiple calls to `cancel()` on the same task are idempotent.

### 34.2 `race(task1, task2, …)`

Built-in: `await race(a, b, …)` (variadic `Task` arguments).

- Returns the result of the **first** contestant to reach a terminal state.
- If the winner **failed**, the failed task’s exception is propagated.
- If the winner was **cancelled**, `CancellationException` is propagated.
- **Losers are not cancelled** — they keep running normally.
- Tie-breaking when multiple contestants finish in the same scheduler step uses **argument order** (deterministic).

### 34.3 `timeout(task, duration)`

Built-in: `await timeout(task, duration)` where the second argument is a duration literal (`ms`, `s`, `m`, `h`).

- If `task` completes in time → returns the task’s result.
- If `task` fails in time → propagates the task’s failure (not `TimeoutException`).
- If the deadline passes first → the target task receives cooperative cancellation, `finally` runs, and the caller gets **`TimeoutException`**.
- Timer tasks created for the deadline are disarmed when the target finishes early.
- Wrapping a `race(...)` meta-task times out the whole race; losers inside the race are still not auto-cancelled unless they were the timed-out target’s inner work.

### 34.4 New exception types

Prelude classes (subclasses of `RuntimeException`):

- `StopIteration` — normal iterator exhaustion (`for` consumes this only from protocol `next()` calls)
- `CancellationException` — cooperative task cancellation
- `TimeoutException` — `timeout()` deadline exceeded
- `NetworkException` — DNS, TCP, UDP, and HTTP client/server failures (`net` and `http`)

Both participate in the existing exception fields (`message`, `cause`, `stack_trace`, …).

### 34.5 Explicit non-goals (unchanged)

Thread pool, top-level `await`, async constructors, `process.spawn()` / `wait()` / `kill()`, JIT/AOT/LLVM/Rust/Stack VM, and alternate async runtimes remain out of scope. Async file I/O and `async_input()` are documented in §35.

## 35. Async / Await — Async I/O + `async_input()` (Update Bölüm 2)

Async I/O extends the cooperative scheduler with **non-blocking file and network operations** and **`async_input()`**. File/stdin blocking work runs on a **single dedicated I/O worker thread**; network operations use **short-lived helper threads**. Completions are delivered back to the scheduler thread.

### 35.1 Async file I/O (`fs`)

Module functions (each returns a `Task`):

- `await fs.async_read(path)` → file contents (`string`)
- `await fs.async_write(path, content)` → `null`
- `await fs.async_append(path, content)` → `null`

Synchronous `fs.read` / `fs.write` / `fs.append` are unchanged.

Errors use the same `RuntimeException` hierarchy as sync I/O. Cooperative cancellation (`task.cancel()`, `timeout()`) is supported; `finally` is preserved.

### 35.2 Async file handle I/O (`io`)

`FileHandle` methods (return `Task`):

- `await handle.async_read()` → `string`
- `await handle.async_write(text)` → `null`

Synchronous `read` / `write` / `flush` / `close` / `is_open` are unchanged. Use-after-close semantics match sync handles.

### 35.3 `async_input()`

Built-in: `await async_input()` or `await async_input(prompt)` (optional string prompt printed before waiting).

- Returns one line of stdin text (UTF-8), without the trailing newline.
- EOF → `RuntimeException` with message `end of input` (same as sync `input()`).
- While waiting, the scheduler can run other tasks (timer, other I/O, etc.).
- Works with `task.cancel()`, `race()`, and `timeout()` using the same semantics as §34.

Sync `input()` is unchanged.

### 35.4 Scheduler / I/O integration

- I/O waiters are **not** busy-polled on the scheduler thread.
- Pending I/O tasks stay off the runnable queue until the worker posts a completion.
- The scheduler loop processes **I/O completions**, **timers**, and the **run queue** in a deterministic order; I/O wakeups use the same condition variable as timer waits.
- Terminal I/O tasks are never re-enqueued.

### 35.5 `race()` / `timeout()` with I/O

- `race()` still **does not cancel** losing tasks (including I/O tasks).
- `timeout(io_task, ms)` cancels the target I/O task on deadline and raises `TimeoutException` after target `finally` runs.

### 35.6 `net` module (Bölüm 6)

Public `import net` provides TCP client/server, UDP datagrams, and DNS resolution. TCP `read()` returns **`bytes`**; EOF raises **`NetworkException`**, not `null`. The same sync API becomes awaitable inside async code (`await net.connect(...)`, `await connection.read()`, …) without a separate `async_*` function family. `listen`, `udp()`, `close`, `is_open`, and `local_port` stay synchronous.

### 35.7 `http` module (Bölüm 7)

Public `import http` is HTTP/1.1 on top of `net`.

- Client: `get(url)`, `post(url, body)`, `request(method, url, body, headers)` return `Response` (`status`, `headers`, `body` as `bytes`). Inside the scheduler they return a `Task` (`await http.get(url)`). Each call is one connection, one request, then close. URLs are `http://` or `https://`. Redirects stop after 10.
- Async client work is `AsyncIoService::submit_http_request` → `http_request_sync` (TCP, TLS when the URL is `https` and TLS is compiled in).
- Server: `server(port)` binds `127.0.0.1` (`0` is ephemeral). `get` / `post` register an exact path handler. `start()` requires `async func main()`. A handler receives `Request` (`method`, `path`, `headers`, `body`) and returns `string` or `bytes` (status 200). Unmatched paths are 404.
- `HttpServer.start()` accepts on a **private thread**, not `submit_net_accept`. Handler calls are posted to the scheduler; this is a V1 limit, not a second public API.
- Failures are `NetworkException`. Cancellation and `timeout()` keep §34 semantics. There is no HTTP-specific timeout function.

Out of this section: HTTP/2, WebSocket, chunked streaming, connection pooling, keep-alive as a user API, TLS server, certificate pinning, custom CA.

### 35.8 Explicit non-goals (unchanged)

Iterator protocol, thread pool, top-level `await`, async constructors, `process.spawn()` / `wait()` / `kill()`, JIT/AOT/LLVM/Rust/Stack VM, and alternate async runtimes remain out of scope. Moving the HTTP accept loop onto `AsyncIoService` is deferred; the public `http` API above stays.



## 36. Async / Await — Finalization (Bölüm 3)

Async/Await core (Tasks, scheduler, `delay`, modules/classes/closures) is **finalized** for the baseline milestone. Task control (`cancel`, `race`, `timeout`) is documented in §34. Async I/O is documented in §35.

### 36.1 Integration guarantees

- All existing PScript subsystems (lexer through CLI) keep their synchronous semantics when async is not used.
- `async func main()` runs top-level imports/init first, then the cooperative scheduler until all tasks finish.
- Sync → async calls produce a `Task`; async → sync calls run synchronously inside the active task without sharing VM state across tasks.
- Module-bound async calls (`await math.compute()`, async class methods in modules) use the same spawn/suspend/resume paths as local async code.

### 36.2 Exceptions and stack traces

- Failed tasks store exceptions until `await` propagates them; consumed failures are not re-reported at shutdown.
- Unhandled background tasks fail at scheduler shutdown with the standard `Unhandled exception:` format.
- Stack traces and source locations use the **active bytecode** (including module code) at the throw site.

### 36.3 Explicit non-goals (unchanged)

Iterator protocol, thread pool, top-level `await`, async constructors, process spawn/wait/kill APIs, JIT/AOT/LLVM/Rust/Stack VM, and alternate async runtimes are out of scope. `net` and `http` are in §35.6–§35.7.

## 37. Advanced Async — Bölüm 1

Structured concurrency and async resource helpers on the existing scheduler (no new worker pool).

### 37.1 `TaskGroup`

- Native class `TaskGroup()` with `spawn(task)`, `await g.wait()` (list of results in spawn order), and scope helpers `enter` / `exit`.
- **`async with TaskGroup() as g:`** pushes the group on the VM stack; `exit` waits for live children and pops the stack. Child tasks spawned while the group is active are tracked automatically.
- Child failure cancels siblings (via existing cancellation). Parent cancel propagates to owned groups and children.
- Built-ins **`wait_all(t1, t2, …)`** and **`wait_any(t1, t2, …)`** (`wait_any` = `race`) compose meta-tasks like §34.

### 37.2 `async with`

- **`async with manager:`** / **`async with manager as x:`** / brace body — calls `await manager.enter()` (or `aenter`), binds `as`, runs the body, then `await manager.exit(...)` in a `finally` path. Uses the same exception/`finally` machinery as `try`/`finally`.

### 37.3 `AsyncStream`

- Native `AsyncStream([...])` (optional list of string chunks) for in-memory async read/write/iterate; `async for chunk in stream` is supported. Intended as a minimal hook for future streaming I/O, not HTTP/2/WebSocket.

### 37.4 Limits

- `async with` passes the active exception (if any) into `exit` / `aexit` as the first argument after the manager; `null` on normal completion.
- Multi-child `TaskGroup.wait()` may deliver the failure to the caller slightly after sibling `finally` output (ordering is scheduler-dependent).


