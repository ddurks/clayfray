# clayfray

A 1v1 fighter where both players are clay that dents and slices for real.
Custom C++ engine: SDL3 + Dawn (WebGPU), whole scene is a sphere-traced SDF —
no triangle pipeline.

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
d
