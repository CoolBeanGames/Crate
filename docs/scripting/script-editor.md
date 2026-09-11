# Script Editor

Switch to Script Editor mode from the top of the main window. The left side
lists every script; selecting one opens it and lists its functions below.

## Editing

- **Syntax highlighting** for keywords, types, strings, and numbers.
- **Auto-bracket insertion**: typing `{` or `(` inserts the matching closer
  and places the caret between them. Typing a closer that's already sitting
  at the caret steps over it instead of duplicating it.
- **Auto-indent**: pressing Enter after a line ending in `{` indents the new
  line one level deeper; pressing Enter between an auto-paired `{}` splits
  it into three lines (body indented, closing brace dropped to its own line
  at the original indent). A line consisting of just `}` auto-dedents to
  match its opening brace.
- **Save**: Ctrl+S, the Save button, or automatically whenever you press
  Play. Saving recompiles the script — see [Getting Started](getting-started.md#editing-and-recompiling).

## Autocomplete

Autocomplete opens live as you type — as soon as you start an identifier or
type `.`, not only via Ctrl+Space. As soon as a variable, function, or class
exists anywhere in the project, the scripting engine is aware of it and it's
available:

- **Plain identifier completions** offer the current class's fields,
  functions, and parameters, plus every local variable declared anywhere in
  the enclosing function body (including inside `if`/`do`/`do_async`/`switch`
  blocks).
- **Member completions** (after `.`) try to resolve the real type of the
  expression before the dot — e.g. `transform.` offers `position`,
  `rotation`, `scale`, `forward`, `right`, `up`; `transform.rotation.`
  offers `x`/`y`/`z` — instead of listing every known type's members. If the
  receiver's type can't be resolved, it falls back to listing everything
  known, so you're never left with nothing.
- Typing another `.` while the popup is already open re-targets it at the
  new receiver instead of closing.
