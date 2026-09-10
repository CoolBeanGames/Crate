# Crate

A small 3D game engine aimed at PSX-style horror games — a melding of ideas from
Unreal, Unity and Godot. Written in C++17.

Early days: everything happens on `main` and stays deliberately basic. Larger
subsystems (rendering, scripting, assets) move to their own branches later.

## Building (Windows)

Requires Visual Studio 2022/2026 with the "Desktop development with C++"
workload (MSVC, the bundled CMake and Ninja). No other dependencies — Dear ImGui
is vendored under `third_party/imgui`, and the editor renders with Win32 +
Direct3D 11 from the Windows SDK.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1        # Debug
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 Release
```

The editor lands at `build/Crate.exe`. `scripts/screenshot.ps1` launches it and
saves a PNG.

## Layout

| Path | What |
| --- | --- |
| `src/core/` | Math + `Transform` (TRS, Euler degrees). |
| `src/scene/` | `Actor` base, `Actor2D` / `Actor3D` and leaf types, `Scene`. |
| `src/editor/` | ImGui editor shell: dockspace, panels, Zen theme, console. |
| `src/main.cpp` | Win32 + D3D11 bootstrap; owns the window and swap chain only. |
| `tests/` | Framework-free checks for the actor hierarchy, run via CTest. |

## Editor panels

- **Top** — menu bar (File / Edit / View / Object / Window), Play/Stop, Settings.
- **Left** — Hierarchy: every actor in the scene, selectable, right-click to add
  or delete.
- **Right** — Inspector: transform plus type-specific fields for the selection.
- **Middle** — Viewport: Scene View / Game View / Scripts tabs.
- **Bottom** — Asset Browser / Engine Console / Game Console tabs.

## Actors

Everything in a scene is an `Actor`: it owns a local `Transform` and can be
parented to another actor, inheriting its world transform.

```
Actor
├── Actor2D ── SpriteActor, UIControlActor
└── Actor3D ── MeshActor
```
