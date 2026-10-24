# Modules and imports

PScript `.phs` files act as **modules**. Importing a module compiles it (once per canonical path), runs its top-level code once, caches the result, and exposes **public** top-level names.

## Syntax

```phs
import math
import math as m
import utils.math

from math import add
from math import add, subtract
from math import add as plus
```

- `import name` binds the module namespace to `name` (or the last segment of a dotted path).
- `from ... import` brings individual exports into the importer scope.
- `as` creates an alias. Duplicate importer bindings are compile-time errors.

## Visibility

Module-level declarations support `pub` / `priv` (and long forms `public` / `private`).

```phs
# math.phs
pub func add(a, b):
    return a + b

priv func helper():
    return 0

pub value = 42
priv secret = 99

func legacy():   # default public
    return 1
```

- **Default:** top-level names without a modifier are **public** exports.
- **`pub`:** exported; visible via `import math` and `from math import …`.
- **`priv`:** not exported; `math.secret` and `from math import secret` fail.
- **`prot` / `protected`:** not allowed at module level (compile-time error). Use `protected` only on class/struct fields.

## Module namespace (live)

```phs
import counter
counter.increment()
print(counter.value)   # live module state
```

Public mutable globals can be updated through the namespace:

```phs
import config
config.value = 20
```

Private members cannot be read or written through the namespace.

## From-import (snapshot vs types)

```phs
import counter
from counter import value

counter.increment()
print(counter.value)   # 1  (live)
print(value)           # 0  (snapshot at import time)
```

- Variables and functions imported with `from … import` are **snapshots** at import execution.
- The module namespace from `import counter` always reflects **live** module globals.
- Classes, enums, and structs imported with `from … import` are **compile-time linked** into the importer (inheritance and typed member access work across modules).

## Path resolution

Resolution is relative to the importing file's directory. Dotted paths map to nested files (`utils.math` → `utils/math.phs`). Path traversal (`..`, `/`, `\`) is rejected. Canonical paths deduplicate the same file.

## Circular imports

Cycles (`a → b → a`) compile via a loading placeholder. During initialization, a module may be in **Loading** state; accessing exports not yet assigned raises a controlled runtime error instead of crashing.

Failed initialization marks the module **Failed**; subsequent imports report the same failure.

## Cross-module features

- Functions and closures keep module global bindings.
- Classes support constructors, methods, and **inheritance across modules** (`from base import Animal` then `class Dog : Animal`).
- Enum and struct exports are supported (`from types import Color`, `from data import Player`).
- Type checking and generics metadata cross module boundaries through linked type tables.
- Exceptions during import/init preserve stack traces and `cause` chains.

## Async functions in modules (Bölüm 2)

- Module exports may be `async func`; calling `await math.compute()` spawns a task like a local async call.
- Top-level `import` statements in the main file run before `async main()` starts, so namespace imports are initialized.
- Suspend/resume preserves module execution context (`execution_bytecode_`, module globals, saved call frames) the same way as synchronous module-bound calls.

## Errors

| Situation | When | Result |
| --------- | ---- | ------ |
| Missing file | Runtime import | `cannot find module '…'` |
| Missing / private export | `from … import` | Compile-time `has no export '…'` |
| Duplicate binding | Compile | `duplicate declaration of '…'` |
| Init failure | Runtime | Propagates with normal exception semantics |
| Partial / circular access | Runtime | `uninitialized module export '…'` |

## Built-in modules

Only user `.phs` modules are loaded today. Built-in modules (`import io` without a file) are a reserved future extension; the loader is structured so filesystem modules can gain a sibling provider later.

## Examples

See `tests/modules/` for sample layouts and `tests/vm_test.cpp` / `tests/compiler_test.cpp` for integration coverage.
