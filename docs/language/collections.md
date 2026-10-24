# Collections

Lists, dictionaries, tuples, and sets hold several values in one variable. Optional generic annotations `list<T>` and `map<string,V>` add compile-time element/value checking (see [Types](types.md)). Lists and tuples use `[index]`. Dictionaries use `["key"]`. Sets are not indexable. Methods cover the rest of the API. There is no `set()` constructor. List and tuple **destructuring assignment** is documented in [Destructuring](destructuring.md).

Method calls are expressions. They work in assignment, `print`, and `if`:

Method calls are expressions. They work in assignment, `print`, and `if`:

```phs
x = numbers.length()
if numbers.is_empty():
    print("empty")
```

A call by itself is a valid statement: `items.append(10)`.

`for item in numbers:` walks a list. `range()` is a built-in that produces a range value for `for` (see [Control flow](control-flow.md)). Dictionaries, tuples, and sets are not iterable in this version.

---

## Lists

A list is written with square brackets. Indexes start at `0`. Elements may mix kinds unless you declare a generic element type.

```phs
numbers = [10, 20, 30]
names = ["Emir", "Ali"]
mixed = [10, "hello", true]
empty = []

list<int> scores = [10, 20]
list<int> blank = []
scores.append(30)        # OK
# scores.append("x")     # compile error
```

Read and write with `[index]`:

```phs
numbers = [10, 20, 30]
print(numbers[0])
numbers[1] = 50
```

Nested lists:

```phs
grid = [[1, 2], [3, 4]]
print(grid[1][0])
```

This prints `3`.

### List methods

| Call | Effect |
| ---- | ------ |
| `append(x)` | Add `x` at the end. Any value. Returns `null`. |
| `prepend(x)` | Add `x` at the front. Returns `null`. |
| `first()` | First element. Error if empty. |
| `last()` | Last element. Error if empty. |
| `find(x)` | Index of the first equal element, or `-1`. |
| `contains(x)` | `true` if `find` would not be `-1`. |
| `count(x)` | How many elements equal `x`. |
| `remove(x)` | Remove the first equal element. Error if none. |
| `remove_at(i)` | Remove the integer index `i`. |
| `clear()` | Remove every element. |
| `is_empty()` | `true` if length is 0. |
| `length()` | Number of elements (`int`). |
| `sort()` | Sort in place, smallest first. See below. |
| `reverse()` | Reverse in place. |

`find` / `contains` / `count` compare with the same rules as `==` for numbers, strings, bools, and `null`. Mixed lists are allowed:

```phs
items = [10, "hello", true]
print(items.find("hello"))
print(items.contains(true))
print(items.count(10))
```

Output:

```text
1
true
1
```

### sort()

`sort()` only runs when every element is a number (`int` or `float`) or every element is a `string`. Mixed or other kinds are a runtime error (`cannot sort incomparable values`). No silent conversion.

Generic annotations (`list<int>`, `map<string,int>`) are compile-time only. `type(my_list)` returns `"list"`, not `list<int>`. See [Types](types.md).

```phs
numbers = [30, 10, 20]
numbers.sort()
print(numbers)

names = ["Mehmet", "Ali", "Emir"]
names.sort()
print(names)
```

```text
[10, 20, 30]
[Ali, Emir, Mehmet]
```

---

## Tuples

A tuple is an **immutable**, ordered collection. Indexes start at `0`. Elements may mix kinds. Nested tuples are allowed.

```phs
empty = ()
one = (10,)
numbers = (10, 20, 30)
mixed = (10, "hello", true)
nested = ((1, 2), (3, 4))
```

`(10)` is a parenthesized expression, not a tuple. The comma in `(10,)` is what makes a one-element tuple.

Read with `[index]`. Writes are rejected (`tuple is immutable`). There is no `append`, `remove`, or `sort`.

```phs
t = (10, 20, 30)
print(t[0])
print(t[2])
```

### Tuple methods

| Call | Effect |
| ---- | ------ |
| `length()` | Number of elements (`int`). |
| `contains(x)` | `true` if an element equals `x`. |
| `find(x)` | Index of the first equal element, or `-1`. |
| `first()` | First element. Error if empty (`tuple is empty`). |
| `last()` | Last element. Error if empty (`tuple is empty`). |

Equality of two tuples is structural (same length and pairwise `==`). Assignment aliases the same tuple value; because the tuple cannot change, that is safe.

Tuple destructuring uses parentheses: `(a, b) = t`. See [Destructuring](destructuring.md).

Dict destructuring uses string keys: `{"name": name, "age": age} = person`. Dict rest: `{"name": name, *rest} = person` binds remaining pairs into a new dict.

---

## Sets

A set is a **mutable** collection of unique values. It is unordered and not indexable.

```phs
numbers = {1, 2, 3}
mixed = {1, "hello", true}
```

`{}` is still an **empty dictionary**, not an empty set. `{ "name": "Emir" }` is a dictionary. `{1, 2, 3}` is a set. `{"a", "b"}` is a set of strings (comma, no `:`). An empty set is produced by `clear()` or `difference`, not by `{}`.

Brace **blocks** (`if true { ... }`) are unchanged: `{` after a finished condition is a block, not a set. A `{` that starts an expression on the same line (`if {}:` , `for x in {1, 2}:`) is a collection literal.

`numbers[0]` is invalid (`cannot index set`). Duplicates in a literal are dropped: `{1, 1, 2}` has length `2`.

### Set methods

| Call | Effect |
| ---- | ------ |
| `add(x)` | Insert `x` if it is not already present. Returns `null`. |
| `remove(x)` | Remove `x`. Error if missing (`undefined set value`). |
| `contains(x)` | `true` if `x` is present. |
| `length()` | Number of values. |
| `is_empty()` | `true` if length is 0. |
| `clear()` | Remove every value. |

### Set operations

These return a **new** set. The operands are not mutated.

| Call | Meaning |
| ---- | ------- |
| `union(other)` | Values in either set. |
| `intersection(other)` | Values in both. |
| `difference(other)` | Values in this set but not in `other`. |

`other` must be a set (`'union' expects a set`, and the same pattern for the other two).

```phs
a = {1, 2, 3}
b = {2, 3, 4}
c = a.union(b)
d = a.intersection(b)
e = a.difference(b)
```

`c` contains `1, 2, 3, 4`; `d` contains `2, 3`; `e` contains `1`. `a` and `b` still have three elements each.

Do not depend on print order. Assignment aliases the same set: `b = a` then `b.add(x)` is visible through `a`.

---

## Dictionaries

A dictionary maps string keys to values. Keys in a literal must be string literals.

```phs
player = {
    "name": "Emir",
    "score": 100
}

print(player["name"])
player["score"] = 150
player["level"] = 5
```

Assigning a missing key inserts it. Reading a missing key is a runtime error.

`{}` is an empty dictionary, not an `if` block. After `if` / `elif` / `elseif`, `{` starts a block unless it looks like a dictionary (`{}` or `"key":`). See [Control flow](control-flow.md).

### Dictionary methods

| Call | Effect |
| ---- | ------ |
| `get(key)` | Value for string `key`. Error if missing. |
| `contains_key(key)` | `true` if `key` exists. |
| `remove(key)` | Delete `key`. Error if missing. |
| `keys()` | New list of key strings. |
| `values()` | New list of values. |
| `clear()` | Remove every entry. |
| `is_empty()` | `true` if no keys. |
| `length()` | Number of keys. |

```phs
print(player.get("name"))
print(player.contains_key("score"))
print(player.keys())
print(player.values())
player.remove("score")
player.clear()
print(player.is_empty())
```

`get` is a method. Indexing `player["name"]` still works. There is no `set()` method.

---

## String methods

Strings also use `.method(...)`. Arguments must be strings.

| Call | Effect |
| ---- | ------ |
| `find(x)` | Byte index of the first match, or `-1`. |
| `contains(x)` | `true` if `x` occurs. |
| `replace(a, b)` | **New** string with all `a` replaced by `b`. Original unchanged. |
| `starts_with(x)` | `true` if the string begins with `x`. |
| `ends_with(x)` | `true` if the string ends with `x`. |
| `first()` | First character (as a string). Error if empty. |
| `last()` | Last character. Error if empty. |
| `is_empty()` | `true` if length is 0. |
| `length()` | Number of bytes / ASCII characters. |

```phs
text = "Hello World"
print(text.find("World"))
print(text.contains("Hello"))
print(text.replace("World", "PScript"))
print(text.starts_with("Hello"))
print(text.ends_with("World"))
print(text.first())
print(text.last())
print(text.length())
print(text.is_empty())
```

```text
6
true
Hello PScript
true
true
H
d
11
false
```

After `replace`, `text` is still `"Hello World"`.

---

## Truthiness

Empty collections are falsy; non-empty collections are truthy.

```phs
if []:
    print("wrong")
if [1]:
    print("list true")
if {}:
    print("wrong")
if {"a": 1}:
    print("dict true")
```

---

## Errors

Typical runtime messages:

- `'append' requires a list` is not used when the receiver is an `int`; you get `cannot call method 'append' on int`.
- Wrong method for the receiver: `unknown list method 'get'`, `unknown string method 'append'`.
- Wrong arity: `'append' expects 1 argument`, `'length' expects no arguments`.
- Empty `first()` / `last()`: `list is empty` / `tuple is empty` / `string is empty`.
- Tuple index write: `tuple is immutable`.
- Set index: `cannot index set`.
- Set `remove` of a missing value: `undefined set value`.
- `remove_at` out of range: `list index out of range`.
- `sort()` on mixed values: `cannot sort incomparable values`.
- Dictionary `get` / `remove` / `contains_key` need a string key.
- String methods need string arguments.

---

## Printing

```phs
print([10, 20])
print({})
```

```text
[10, 20]
{}
```
