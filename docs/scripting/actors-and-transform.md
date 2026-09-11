# Actors & Transform

Inside any script method, `this` refers to the current script instance.

- `actor` / `this.actor` — the `Actor` this script is attached to.
- `transform` / `this.transform` — shortcut to `actor`'s transform. Bare
  `transform` and `actor` work directly inside a method body without
  writing `this.` first.

## Transform members

| Member | Type | Notes |
| --- | --- | --- |
| `position` | `Vector3` | World-relative to parent; local space. |
| `rotation` | `Vector3` | Euler angles, in degrees. |
| `scale` | `Vector3` | Local scale. |
| `forward` | `Vector3` | Unit direction the transform currently faces (+Z rotated by `rotation`). |
| `right` | `Vector3` | Unit direction to the transform's right (+X rotated by `rotation`). |
| `up` | `Vector3` | Unit direction "up" for the transform (+Y rotated by `rotation`). |

`forward`/`right`/`up` are read-only derived directions — assign to
`position`/`rotation`/`scale` to actually move or turn something:

```cscript
func update(float delta)
{
    transform.rotation.y += move_speed * delta;
    transform.position += transform.forward * move_speed * delta;
}
```

Reading a vector member and mutating a field of it in place also works and
writes back to the transform:

```cscript
var r = transform.rotation;
r.y = r.y + speed * delta;
transform.rotation = r;
```

## Components

`get_component(type_of(SomeType))` returns the first component of that type
on the current actor (or `null` if none is attached):

```cscript
Actor a = this.actor;
Actor b = a.get_component(type_of(Actor));
```

Works both as `this.get_component(...)` (shorthand for the owning actor) and
explicitly on an `Actor` value.
