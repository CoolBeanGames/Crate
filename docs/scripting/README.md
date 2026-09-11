# cScript Wiki

cScript is Crate's built-in scripting language (`.cscript` files). This wiki
covers the language as implemented today. When a scripting feature changes
or a new one is added, update the relevant page here in the same change.

## Pages

- [Getting Started](getting-started.md) — writing, importing and attaching your first script.
- [Types](types.md) — built-in value types (`int`, `float`, `string`, `char`, `bool`, `var`, arrays, `Vector3`/`Vector2`).
- [Classes](classes.md) — `class`, inheritance, `abstract`, `static`, naming rules.
- [Functions](functions.md) — declaring functions, parameters, return types, overriding, `this.base`.
- [Expressions & Operators](expressions.md) — arithmetic, comparison, string concatenation, `.str()`.
- [Control Flow](control-flow.md) — `if`/`else`, `switch`, `do`, `do_async`, `break`/`continue`.
- [Actors & Transform](actors-and-transform.md) — `this.actor`, `transform`, `position`/`rotation`/`scale`/`forward`/`right`/`up`, `get_component`.
- [Signals](signals.md) — declaring, connecting to and emitting signals.
- [Built-in Functions](builtins.md) — `print`, `type_of`, `Math.*`, `Input.*`, `str()`.
- [Script Editor](script-editor.md) — syntax highlighting, autocomplete, auto-bracket/indent, save/compile.

## Quick example

```cscript
class Mover : Actor3D
{
    float speed = 20.0;
    var elapsed = 0;

    func start()
    {
        print("Mover ready on " + actor.name);
    }

    func update(float delta)
    {
        elapsed = elapsed + delta;
        transform.position += transform.forward * speed * delta;
    }

    func physics_update(float delta)
    {
    }
}
```
