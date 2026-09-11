# Built-in Functions

## Global

| Function | Notes |
| --- | --- |
| `print(...)` | Logs to the game console. Multiple args are joined with a space. |
| `type_of(Type)` | Returns a type reference, used with `get_component`/declarations. |
| `str(value)` | Converts any value to a `string`; equivalent to `value.str()`. |
| `Vector3(x, y, z)` | Constructs a `Vector3`. |
| `Vector2(x, y)` | Constructs a `Vector2` (a `Vector3` with `z` fixed at 0). |
| `emit_signal(name, args...)` | Emits a signal on `this` by name. See [Signals](signals.md). |

## `Math`

| Function | Notes |
| --- | --- |
| `Math.clamp(v, lo, hi)` | |
| `Math.lerp(a, b, t)` | |
| `Math.sin(x)` / `Math.cos(x)` / `Math.tan(x)` | (`sine`/`cosine` also accepted) |
| `Math.sqrt(x)` / `Math.exp(x)` / `Math.pow(x, y)` | |
| `Math.abs(x)` | |
| `Math.floor(x)` / `Math.ceil(x)` / `Math.round(x)` | |
| `Math.min(a, b)` / `Math.max(a, b)` | |
| `Math.deg2rad(x)` / `Math.rad2deg(x)` | |
| `Math.rand_f()` | Random `float` in `[0, 1)`. |
| `Math.rand_i()` | Random non-negative `int`. |
| `Math.rand_f_range(lo, hi)` | Random `float` in `[lo, hi)`. |
| `Math.rand_i_range(lo, hi)` | Random `int`, inclusive of both ends. |

## `Input`

| Function | Notes |
| --- | --- |
| `Input.get_button(name)` | Returns an input-button object for a named action from the Input Map. |
| `Input.get_axis(name)` | Returns a `Vector2` for a named 2-axis input. |
| `Input.is_pressed(name)` | `bool`, held down this frame. |
| `Input.is_just_pressed(name)` | `bool`, went down this frame. |
| `Input.is_just_released(name)` | `bool`, went up this frame. |

A button from `Input.get_button(name)` also exposes `pressed`,
`just_pressed`, and `just_released` as **signals** you can `connect` to
instead of polling every frame.

## Arrays

| Method | Notes |
| --- | --- |
| `arr.add(value)` | Appends `value`. |
| `arr.length()` | Element count. |
| `arr[index]` | Access/assign an element. |

## Universal `.str()`

Every value (except `string`/`char`, which already are strings) has
`.str()` for string conversion — see [Expressions](expressions.md).
