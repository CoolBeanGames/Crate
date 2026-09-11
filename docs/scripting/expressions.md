# Expressions & Operators

Expressions compare or combine values:

```cscript
1 + 2        // 3
x + y        // sum of two variables
1 == 1       // true
1 != 1       // false
1 == 2       // false
10 % 3       // 1  (modulo)
2.0 * 3      // 6
```

## Strings

`+` concatenates strings:

```cscript
"text" + "text"   // "texttext"
```

Every type except `string` and `char` has a `.str()` function that converts
it to a string, so it can be concatenated:

```cscript
"text" + 1.str()   // "text1"
```

## Assignment

Plain `=` and the compound forms `+=`, `-=`, `*=`, `/=`, `%=` — `a += b` is
shorthand for `a = a + b`:

```cscript
transform.rotation.y += move_speed * delta;
transform.position += transform.forward * move_speed * delta;
```

## Comparison

`==`, `!=` compare value and type together — `1 == "1"` is `false` (different
types), not just a numeric comparison.

See [Control Flow](control-flow.md) for `if`/`switch` using these
expressions as conditions.
