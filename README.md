# clayfray

A 1v1 fighter where both players are clay that dents and slices for real.
Custom C++ engine: SDL3 + Dawn (WebGPU), whole scene is a sphere-traced SDF —
no triangle pipeline. Art direction: *The Trap Door* (1984). See `PLAN.md`
for the roadmap and `reference/ART_DIRECTION.md` for the measured style guide.

## Build (macOS)

```sh
brew install cmake ninja ccache   # once
cmake -B build -G Ninja           # first configure fetches ~3GB of deps (Dawn)
cmake --build build
```

## Run

```sh
./build/clayfray                  # windowed look-dev: orbit with left-drag,
                                  # zoom with wheel, tweak everything in the panel
./build/clayfray --screenshot lookdev/shot.png --size 1280x720 --aa 2
```

Shaders in `shaders/` hot-reload while the windowed app runs.

`--screenshot` renders headless (no window) and writes a PNG — used for
look-dev iteration against `reference/contact_sheet.png`.
Options: `--size WxH`, `--frames N` (sim frames before capture), `--time T`
(start clock, changes boil/grain seeds), `--aa N` (rays per pixel axis),
`--character file.glb` (override the character asset).

## Editing the fighter

The character is content: `assets/fighter.glb` auto-loads when present
(delete/rename it to fall back to the built-in analytic blob).

- **Edit visually**: open `assets/fighter.blend` in Blender, sculpt/reshape,
  keep it watertight and chunky, then export:
  `/Applications/Blender.app/Contents/MacOS/Blender --background assets/fighter.blend --python assets/export_fighter.py`
- **Rebuild from scratch** (regenerates both .blend and .glb from the
  parametric script): `/Applications/Blender.app/Contents/MacOS/Blender --background --python assets/build_fighter.py`
- Authoring rules: body = one closed skinned mesh with pre-linearized vertex
  colors; rigid props (eyes…) = separate objects named `marble_*` with their
  color as the material base color. Thin spindly shapes will misbehave —
  clay proportions are load-bearing.
