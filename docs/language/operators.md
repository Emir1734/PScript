# Operators

Operators build expressions from values and names. Operand kinds are in [Types](types.md).

```phs
x = 10
y = 20

result = x + y * 2
print(result)
```

`*` runs before `+`, so this prints `50`.

---

## Arithmetic

| Operator | Meaning |
| -------- | ------- |
| `+` | add |
| `-` | subtract |
| `*` | multiply |
| `/` | divide |
| `%` | remainder |

```phs
print(1 + 2)
print(5 - 2)
print(3 * 4)
print(8 / 2)
print(7 % 3)
print(1 + 2.5)
```

Output:

```text
3
3
12
4
1
3.5
```

**Rules**

- `+`, `-`, `*`, `/` require numbers (`int` or `float`).
- Two `int` values produce an `int`. If either side is `float`, the result is `float`.
- Integer `/` truncates toward zero (`7 / 2` is `3`).
- `%` requires two `int` values. `7 % 3.0` is invalid.
- Divide by zero: `division by zero`. Remainder by zero: `modulo by zero`.
- Strings, bools, and `null` are not valid arithmetic operands.

```phs
print(true + 1)
print("a" + "b")
```

Both fail at runtime: `unsupported operand type`.

---

## Comparison

| Operator | Meaning |
| -------- | ------- |
| `==` | equal |
| `!=` | not equal |
| `<` | less |
| `<=` | less or equal |
| `>` | greater |
| `>=` | greater or equal |

Every comparison yields a `bool`.

```phs
print(1 == 1)
print(1 != 2)
print(1 < 2)
print(3 >= 4)
print(1 == 1.0)
```

Output:

```text
true
true
true
false
true
```

**Rules**

- `==` and `!=` work for numbers, strings, bools, and `null`.
- An `int` and a `float` compare by numeric value (`1 == 1.0` is `true`).
- `<`, `<=`, `>`, `>=` require numbers. ` "a" < "b" ` is `unsupported operand type`.
- `true == 1` is `false` (bool is not a number).
- `null == null` is `true`.

---

## Unary `+` and `-`

```phs
a = 5
print(-a)
print(+a)
print(-1.5)
```

Output:

```text
-5
5
-1.5
```

The sign is not part of the number token. Unary `+` / `-` apply to `int` and `float` only. `-"hello"` is `unsupported operand type`. Unary `+` does not change the kind (`+5` stays `int`). Unary `~` is bitwise not (integers only).

---

## Logical

Word forms and symbol forms are equivalent:

| Word | Symbol |
| ---- | ------ |
| `and` | `&&` |
| `or` | `\|\|` |
| `not` | `!` |

```phs
if age >= 18 and score >= 50:
    print("Passed")

if age >= 18 && score >= 50:
    print("Passed")

if not active:
    print("Inactive")

if !active:
    print("Inactive")
```

**Truthiness** uses the same rules as `if` / `while` ([Control flow](control-flow.md)). `not` / `!` yield a `bool`: truthy operand → `false`, falsy → `true`.

**Short-circuit**

- `and` / `&&`: if the left side is falsy, the right side is **not** evaluated; the result is the left value.
- `or` / `||`: if the left side is truthy, the right side is **not** evaluated; the result is the left value.
- Otherwise the result is the right value.

```phs
# something() does not run
false and something()
true or something()
```

This is compiled with conditional jumps (`JUMP_IF_FALSE` / `JUMP_IF_TRUE`), not by evaluating both sides then combining them.

`!` alone is unary not. `!=` is still one comparison token. `! =` is not `!=`.

Logical `&&` / `||` / `!` are not the bitwise operators `&` / `|` / `~`.

---

## Assignment and compound assignment

`=` assigns. It is right-associative: `a = b = 1` assigns `1` to `b`, then that value to `a`.

Compound operators are each one token. `x += y` has the same result as `x = x + y` using the matching arithmetic operator (`+=` `-=` `*=` `/=` `%=`). The left side is evaluated once (so `list[i] += 1` loads and stores that index without computing `i` twice).

The expression value of `x += y` is the new value of `x`.

Targets must be assignable: a variable or an index expression. `(x + 1) += 2` is `invalid assignment`.

---

## Increment and decrement

`++` and `--` work as prefix and postfix on assignable targets.

```phs
x = 10
y = x++    # y is 10, x is 11
x = 10
y = ++x    # y is 11, x is 11
x--
--x
```

- Prefix (`++x` / `--x`): add or subtract 1, then the expression is the **new** value.
- Postfix (`x++` / `x--`): the expression is the **old** value, then the variable is updated.
- `5++`, `++5`, and `(x + 1)++` are `invalid increment`.

`++` / `--` use integer `1` with the same `+` / `-` rules, so they work on `int` and `float`.

---

## Bitwise

Integers only: `&` `|` `^` `~` `<<` `>>`.

```phs
print(5 & 3)   # 1
print(5 | 3)   # 7
print(5 ^ 3)   # 6
print(~0)      # -1
print(5 << 2)  # 20
print(20 >> 1) # 10
```

`float`, `string`, `bool`, `null`, and collections are `unsupported operand type`. A shift count not in `0 … 63` is `invalid shift`. Right shift is arithmetic (sign-preserving). Left shift uses the 64-bit pattern.

These do **not** short-circuit and are not logical `and` / `or` / `not`.

---

## Membership

`in` and `not in` yield `bool`.

| Container | Meaning |
| --------- | ------- |
| list | value present (same equality as `==`) |
| dict | **key** present (key must be a string) |
| string | substring (both sides strings; same as `contains`) |
| range | integer is a value the range would yield (`start`/`stop`/`step`, stop exclusive). The range is not turned into a list. |

```phs
if "apple" in fruits:
    print("Found")

if "apple" not in fruits:
    print("Not found")
```

Other container kinds: `cannot use 'in' with <kind>`. `not x in y` is `(not x) in y` because unary `not` is tighter; write `x not in y` or `not (x in y)`.

---

## Ternary

```phs
x = age >= 18 ? "Adult" : "Young"
print(score >= 50 ? "Passed" : "Failed")
```

Only one branch runs: if the condition is truthy, the middle expression; otherwise the expression after `:`. Compiled with `JUMP_IF_FALSE` / `JUMP`, not by evaluating both sides.

Right-associative: `a ? b : c ? d : e` is `a ? b : (c ? d : e)`.

A missing `:` is a syntax error. The condition uses the same truthiness as `if`. `a ? b : c = d` is invalid because a ternary is not an assignable target; use `a ? b : (c = d)`.

---

## Precedence

From tightest to loosest:

1. Primary / `()` grouping, calls, indexing, member calls
2. Postfix `++` `--`
3. Unary `+` `-` `not` `!` `~` prefix `++` `--`
4. `*` `/` `%`
5. `+` `-`
6. `<<` `>>`
7. Bitwise `&`
8. Bitwise `^`
9. Bitwise `|`
10. Comparisons `==` `!=` `<` `<=` `>` `>=`
11. `in` / `not in`
12. `and` / `&&`
13. `or` / `||`
14. Ternary `?:` (right-associative)
15. `=` `+=` `-=` `*=` `/=` `%=` (right-associative)

Rows 4–13 are left-associative. Parentheses override precedence.

```phs
print(1 + 2 * 3)          # 7
print(a & 1 == 0)         # (a & 1) == 0
print(a | b ^ c)          # a | (b ^ c)
print(a > b and c > d)    # (a > b) and (c > d)
x = condition ? 10 : 20
x += 2
```

---

## Longest match

Each of these is one token: `==` `!=` `<=` `>=` `&&` `||` `++` `--` `+=` `-=` `*=` `/=` `%=` `<<` `>>`. `=` is assignment. A lone `!` is unary not. A lone `&` / `|` is bitwise, not logical.

```phs
print(1 == 1)
x = 1
x += 1
```
