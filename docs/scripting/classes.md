# Classes

```cscript
class Name : BaseClass
{
    // fields, functions
}
```

- Every class name starts with an uppercase letter and can't start with a
  digit; each name must be unique across all scripts.
- A class inherits from exactly one base — `Actor`, `Actor2D`, `Actor3D`, or
  any other class in the project (including another script's class).
- Code blocks use curly braces `{}`; each opening brace increases indentation
  by one level (the Script Editor does this automatically as you type — see
  [Script Editor](script-editor.md)).

## Inheritance and `this.base`

Overriding a function means declaring a function with the same name (and no
`abstract` keyword) in a derived class. Call the base class's version with
`this.base.function_name(...)`:

```cscript
class Base : Actor
{
    func greet() : string { return "base"; }
}

class Child : Base
{
    func greet() : string { return this.base.greet() + "+child"; }
}
```

## `abstract`

An `abstract` function has no body and must be overridden by every class
that inherits from it — calling it directly on a class that never overrides
it is a runtime error:

```cscript
class Shape : Actor
{
    abstract func area() : float;
    func describe() : string { return "area=" + this.area().str(); }
}

class Square : Shape
{
    float side = 3.0;
    func area() : float { return side * side; }   // required override
}
```

## `static`

A `static class`:

- Cannot be instanced on an actor — there is exactly one instance, ever.
- Runs its own `start()`/`update()`/`physics_update()` regardless of whether
  any actor uses it.
- Is called from anywhere via its class name directly, without an instance:

```cscript
static class GameState : Actor
{
    int score = 0;
    func add(int n) { score = score + n; }
}

class Scorer : Actor
{
    func bump() : int
    {
        GameState.add(10);
        return GameState.score;
    }
}
```
