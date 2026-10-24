# Structs, Records, and Enums

PScript provides lightweight data modeling with **struct** / **record** and named **enum** values. These are not classes: no methods, inheritance, constructors, or `this`. See [Classes](classes.md) for OOP features.

---

## Struct and Record

`struct` and `record` declare the same underlying type. Use whichever keyword reads better; semantics are identical.

### Declaration

Colon block (indented fields):

```phs
struct Player:
    name
    number
    rating
```

Brace block:

```phs
record Player {
    name
    number
    rating
}
```

Optional field types use the same prefix or colon forms as variables:

```phs
struct Player:
    string name
    int number
```

Typed fields help nested access and static field validation when a field holds another struct type.

### Access modifiers

Struct fields may declare member access. Modifiers apply only to type members, not to variables or functions.

| Modifier | Aliases | Default |
| -------- | ------- | ------- |
| public | `pub` | yes (when omitted) |
| protected | `prot` | no |
| private | `priv` | no |

```phs
struct Player:
    pub name
    private secret
    prot internal_id
    score
```

Semantics:

- **public** — readable and writable from any scope (current behavior for all fields).
- **private** — only accessible from methods/constructors of the declaring type (struct or class).
- **protected** — accessible from the declaring type and derived classes (classes only for inheritance); not from external code.

Invalid uses (for example `private x = 1` at statement level) produce `access modifier cannot be used here`.

### Instance creation

```phs
p = Player()
```

`Player()` takes **no arguments**. Extra arguments are a compile-time error.

Fields start as `null`. Assign after creation:

```phs
p.name = "Emir"
p.number = 10
```

### Field access

```phs
print(p.name)
p.rating = 85.5
```

Unknown fields are rejected at compile time when the struct type is known (`unknown field 'missing'`). Field access on a non-struct value fails at runtime (`cannot access field on non-struct value`).

### Type identity

Each declared struct name is a distinct type. `Player` and `Team` are different even if they list the same field names.

### Equality

Struct equality is **structural** within the same type: same `type_index` and pairwise `equals` on every field. Two different struct types are never equal, even with identical field values.

```phs
struct Player:
    name
struct Team:
    name
p = Player()
t = Team()
p.name = "A"
t.name = "A"
print(p == t)   # false
```

### Limitations

- No methods, inheritance, constructors, or `this`
- Private/protected fields cannot be accessed from outside the type yet (no inner methods)
- Struct destructuring: `Player{name, age} = player` (see [Destructuring](destructuring.md)); private/protected fields cannot be bound from outside the type
- No enum destructuring syntax
- Field indices are resolved at compile time when the struct type is known

### Difference from classes

Classes add methods, constructors, `this`, reference semantics, and single inheritance. Structs are plain mutable value records with named fields. See [Classes](classes.md).

---

## Enum

Enums model small named constant sets for conditions, `match`, and APIs.

### Declaration

```phs
enum Color:
    RED
    GREEN
    BLUE
```

Brace form is also supported. Duplicate member names are a **compile-time** error.

### Member access

```phs
c = Color.RED
print(c)              # Color.RED
print(c == Color.RED) # true
```

Members are typed enum values, not plain strings or ints. `Color.RED` and `Status.RED` are different types and never compare equal.

### Match integration

```phs
match color:
    Color.RED:
        print("red")
    Color.GREEN:
        print("green")
    Color.BLUE:
        print("blue")
```

Enum members are valid match literals. Existing `match` rules apply: first match wins, no fallthrough, duplicate patterns rejected, guards and bindings unchanged.

### Equality

Enum equality requires the same enum type and member index. Cross-type comparison is always false.

### Unsupported operations

- Ordering (`<`, `>`) is not defined
- Arithmetic (`Color.RED + Color.GREEN`) fails at runtime with `unsupported operand type`
- Custom numeric values per member are not supported in this milestone

### Limitations

- Members are simple identifiers only (no `RED = 1` syntax yet)
- Enum types cannot be assigned like variables (`cannot assign to enum type 'Color'`)
