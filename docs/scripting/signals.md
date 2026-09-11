# Signals

Signals are Godot-style events a class can declare, connect handlers to,
and emit.

```cscript
class Enemy : Actor
{
    signal hit;
    signal scored(points);

    var log = "";
    var total = 0;

    func on_hit() { log = log + "H"; }
    func on_scored(p) { total = total + p; }

    func run()
    {
        hit.connect(on_hit);
        scored.connect(on_scored);

        hit.emit();
        emit_signal("scored", 5);   // Godot-3 style, by name
    }
}
```

## Declaring

`signal name;` or `signal name(param);` inside a class body. Parameter names
are for documentation only — any arguments passed to `emit`/`emit_signal`
are forwarded to connected handlers positionally.

## Connecting

`signal_name.connect(handler)` where `handler` is a bare function reference
(a Callable bound to the object it's declared on). Connecting the same
handler twice is a no-op. `signal_name.disconnect(handler)` removes it;
`signal_name.is_connected(handler)` checks.

## Emitting

Two equivalent styles:

- `signal_name.emit(args...)` — direct, when you have the signal value.
- `emit_signal("signal_name", args...)` — by name, Godot-3 style, callable
  from `this` or (as `obj.emit_signal(...)`) on another script object.

Emitting a signal with no connections is a safe no-op.
