# Functions

```cscript
func name(type param, type param) : return_type
{
    // code
}
```

- Any number of parameters, each written as `type name`.
- The `: return_type` is optional. You can still `return` a value without
  declaring one — the return type annotation only documents intent.
- Declaring a function registers it for autocomplete immediately, even in
  other scripts that reference the class it belongs to.

```cscript
func custom_func(int a, int b) : int
{
    return a + b;
}

// no declared return type — still returns a value
func perimeter(int sides)
{
    return side * sides;
}
```

## Lifecycle functions

Every actor-derived class can define:

- `func start()` — called once when play begins.
- `func update(float delta)` — called every frame; `delta` is the time in
  seconds since the last `update`.
- `func physics_update(float delta)` — called once per physics step.

None are required, but the New Script template includes empty versions of
all three.

## Overriding, `abstract`, callables

See [Classes](classes.md) for `abstract`/override rules and `this.base`.

A bare function reference (just the name, no parentheses) used as a value —
e.g. passed to `signal.connect(...)` — becomes a **Callable** bound to
`this`:

```cscript
func on_hit() { log = log + "H"; }
// ...
hit.connect(on_hit);   // on_hit is passed as a Callable, not called here
```

Call a stored callable with `callable_var(args)` or `callable_var.call(args)`.
See [Signals](signals.md).
