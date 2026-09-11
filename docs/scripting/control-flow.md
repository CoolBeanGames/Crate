# Control Flow

## `if` / `else`

```cscript
if (health <= 0)
{
    die();
}
else
{
    regen();
}
```

## `switch`

```cscript
switch (turn_axis)
{
    case 0:
    {
        turn_speed = 0;
    }
    case 1:
        turn_speed = 1;   // a single statement doesn't need braces
    default:
    {
        turn_speed = -1;
    }
}
```

## `do` — loop while a condition holds

```cscript
do (count < 5)
{
    count = count + 1;
}
```

Runs its body repeatedly for as long as the condition is true, all within
the same frame (like a normal `while` loop in other languages).

## `do_async` — one iteration per frame

```cscript
func update(float delta)
{
    do_async(count < 5)
    {
        count = count + 1;
        print("count = " + count.str());
    }
    phase = "done";
}
```

Same condition/body shape as `do`, but only processes **one cycle per
frame** instead of draining the whole loop immediately — useful for
spreading work across frames without blocking. Its resume position is reset
whenever the owning script recompiles.

## `break` / `continue`

Standard loop control inside `do` / `do_async` bodies: `break` exits the
loop, `continue` skips to the next condition check.
