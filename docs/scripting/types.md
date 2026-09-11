# Types

## Primitives

| Type | Notes |
| --- | --- |
| `int` | Whole numbers. |
| `float` | Decimal numbers. |
| `string` | Text, double-quoted: `"hello"`. |
| `char` | A single character. |
| `bool` | `true` / `false`. |

## `var`

`var` declares an untyped field or local — its type is whatever value it
holds and is resolved at runtime:

```cscript
var count = 0;      // starts as int
var phase = "idle";  // starts as string
```

You can also let a typed declaration infer its type from the initializer
(implicit typing) instead of writing `var`, as long as the value on the
right makes the type obvious.

## Arrays

An array can hold any mix of types, including different types in the same
array. Access an element with `array_name[index]`.

```cscript
var things = [1, "two", 3.0];
add_to(things);       // things.add(value) appends
var n = things.length();
```

## Vectors

`Vector3(x, y, z)` and `Vector2(x, y)` (a `Vector3` with `z` fixed at 0) are
built-in value types with `.x` / `.y` / `.z` fields:

```cscript
var p = Vector3(1, 2, 3);
p.y = p.y + 1;
```

## Actor types

- `Actor` — the base type shared by everything in the scene.
- `Actor2D` — base for 2D actors.
- `Actor3D` — base for 3D actors (meshes, lights, etc.).

Uppercase names like `Actor` refer to the **type itself** (used for
declarations and `type_of`/`get_component` lookups); a variable holding an
**instance** — like `this.actor` — is lowercase by convention (`actor`).

```cscript
Actor a = this.actor;                 // the type-of-a-value form
Actor b = a.get_component(type_of(Actor));
```

See [Actors & Transform](actors-and-transform.md) for the full member list.
