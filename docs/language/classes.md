# Classes and Inheritance

PScript classes are reference-like object types with fields, methods, constructors, and single inheritance. Execution follows the standard pipeline: source → lexer → parser → AST → register bytecode compiler → register VM → C++ runtime.

**Not supported:** `extends`, `override`, and multiple inheritance.

---

## Class declaration

Colon block:

```phs
class Player:
    string name
    int score
```

Brace block:

```phs
class Point {
    x
    y
}
```

An empty body may use `pass`:

```phs
class Marker:
    pass
```

Class names are hoisted (pre-registered) so a child may appear before its parent in source, as long as there is no circular dependency.

---

## Fields

Fields use the same optional type syntax as variables (`string name`, `int score`). **Default access is private** for class members (fields, methods, properties). Use `public`, `protected`, or `private` explicitly when needed.

```phs
class Player:
    public name
    protected score
    private secret
    public rating
```

Field read/write uses dot syntax: `obj.field`. This is not a method call.

Instances are created with `TypeName(...)` when a constructor exists, or `TypeName()` when there is no constructor (or only a zero-argument parent chain applies).

---

## Properties

Properties use `get` / `set` members and are accessed like fields (`obj.score`). A getter-only property is read-only; assignment without a setter is a compile-time error.

```phs
class Player:
    private int _score

    get score():
        return this._score

    set score(value):
        this._score = value
```

Brace syntax:

```phs
class Counter {
    private int n

    get value(): int {
        return this.n
    }
}
```

Computed properties omit backing storage and run the getter body on each read.

---

## Access modifiers

Modifiers apply only to class (and struct) members, not to variables or functions at statement level.

| Modifier | Aliases | Declaring class | Child class | External |
| -------- | ------- | --------------: | ----------: | -------: |
| public | `pub` | yes | yes | yes |
| protected | `prot` | yes | yes | no |
| private | `priv` | yes | no | no |

Invalid uses (for example `private x = 1`) produce `access modifier cannot be used here`.

Compile-time errors:

- `cannot access private field`
- `cannot access protected field`

---

## `pass`

`pass` is a no-op statement. It emits no bytecode.

Valid in class bodies (empty class), methods, constructors, and control-flow blocks. Invalid as an expression (`x = pass` → `invalid expression`).

---

## Constructor

At most one `constructor` per class. The implicit first parameter is `this` (local slot 0).

```phs
class Player:
    string name

    constructor(name):
        this.name = name

p = Player("Emir")
```

Rules:

- Constructors cannot return a value (`constructor cannot return a value`).
- A constructor implicitly returns the instance (`this`).
- Wrong arity is a compile-time error (`'Player' expects N arguments`).

---

## Methods

```phs
class Player:
    int score

    constructor():
        this.score = 0

    func add(value):
        this.score += value

    func get():
        this.score
```

Method calls require parentheses: `p.add(10)`. Dispatch uses the **runtime object's class** (virtual dispatch): an overridden method on the child runs when called on a child instance.

Nested `func` inside a method follows the same closure rules as ordinary functions.

---

## `this`

`this` refers to the current instance inside a class method or constructor. Invalid outside class context (`invalid use of 'this'`).

`this.field` accesses fields on the instance, including inherited fields. Local variables in a method shadow field names; use `this.name` to access the field.

---

## Subtyping (assignment and parameters)

A value of a **child** class may be assigned to a variable or parameter typed as a **parent** class:

```phs
class Animal:
    pass
class Dog : Animal:
    pass

Animal a = Dog()
func take(Animal x):
    pass
take(Dog())
```

Multi-level chains work (`Dog` → `Mammal` → `Animal`). The reverse is invalid: `Dog d = Animal()` is a compile-time type error.

Subtyping applies only along class inheritance. Struct types and enum types require exact identity.

---

## Single inheritance

Syntax uses a colon between the class name and the parent name. **`extends` is not used.**

```phs
class Player:
    string name

class FootballPlayer : Player:
    int number
```

Brace form:

```phs
class FootballPlayer : Player {
    int number
}
```

**Multiple inheritance is not supported** (`multiple inheritance is not supported`).

Parent class must exist (`unknown parent class 'Player'`). A class cannot inherit from itself. Cycles are rejected at compile time (`circular inheritance`).

Inheritance chain example:

```text
FootballPlayer → Player
```

Field and method lookup walk from the derived class toward the base class.

---

## Inherited fields

Child classes include parent **public** and **protected** fields in their flattened field layout. Private parent fields are not accessible from the child.

```phs
class Player:
    public string name
    protected int score

class FootballPlayer : Player:
    func setup():
        this.name = "Emir"
        this.score = 10
```

External code:

```phs
p = FootballPlayer()
p.name = "Emir"    # OK (public)
p.number = 10      # OK (own field)
p.score            # ERROR (protected)
```

Unknown fields: `unknown field 'x'`.

---

## Inherited methods and overriding

Child classes inherit parent methods. Defining a method with the same name **overrides** the parent implementation. There is no `override` keyword.

```phs
class Player:
    func greet():
        print("Player")

class FootballPlayer : Player:
    func greet():
        print("FootballPlayer")

p = FootballPlayer()
p.greet()   # prints FootballPlayer
```

Unknown methods: `unknown method 'x'`.

---

## `super` and `parent`

`super` and `parent` are aliases. They refer to the **parent class**, not a separate object.

### Parent method call

Calls the parent implementation, bypassing the child override:

```phs
class FootballPlayer : Player:
    func greet():
        super.greet()
        print("FootballPlayer")
```

`parent.greet()` behaves the same.

### Parent constructor call

Only valid inside a child **constructor**:

```phs
class FootballPlayer : Player:
    constructor(name, number):
        super(name)
        this.number = number
```

`parent(name)` is equivalent. The parent constructor runs on the **same instance** (`this`); it does not allocate a new object.

`super` / `parent` cannot be assigned like variables. Using them outside a constructor as a call (`super()`) or outside a class (`super.greet()`) is `invalid use of 'super'` / `invalid use of 'parent'`.

---

## Constructor chaining

When the parent declares a constructor:

| Parent constructor | Child constructor | Behavior |
| ------------------ | ----------------- | -------- |
| none | any | No parent init required |
| zero-arg | none | Parent zero-arg ctor auto-called on instance creation |
| zero-arg | present, no `super()` | Compiler inserts `super()` at start of child ctor |
| with parameters | none | Compile error: child must declare a ctor |
| with parameters | present, no `super(...)` | `missing required parent constructor call` |
| with parameters | `super(...)` / `parent(...)` | OK |

Example:

```phs
class Player:
    string name
    constructor(name):
        this.name = name

class FootballPlayer : Player:
    int number
    constructor(name, number):
        super(name)
        this.number = number

p = FootballPlayer("Emir", 10)
```

---

## Classes vs structs

| Feature | Struct / record | Class |
| ------- | ---------------- | ----- |
| Instance model | Value (structural) | Reference (`shared_ptr`) |
| Methods | no | yes |
| Constructor | no (`Type()` only) | yes |
| `this` | no | yes |
| Inheritance | no | single inheritance |
| Equality | structural (same type) | reference identity (`==`) |

---

## Errors (summary)

| Situation | Message (typical) |
| --------- | ----------------- |
| Unknown parent | `unknown parent class '…'` |
| Multiple parents | `multiple inheritance is not supported` |
| Cycle | `circular inheritance` |
| Private field from child/external | `cannot access private field` |
| Protected field from external | `cannot access protected field` |
| Missing parent ctor call | `missing required parent constructor call` |
| Invalid `super` / `parent` | `invalid use of 'super'` |
| Unknown inherited member | `unknown field '…'` / `unknown method '…'` |
