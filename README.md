# SDL Skyroads

A native **C++17** reimplementation of the 1993 DOS game **SkyRoads**, rendered
and driven through SDL2. It loads the original game's data files directly,
reproduces the ship simulation deterministically, and renders a 320×200
software framebuffer — no emulator required. Built to be portable to platforms
that only have a C/C++ toolchain.

## Status

| Layer | What it does | State |
|-------|--------------|-------|
| `src/data` | Asset loaders (LZS/SND/DAT/REC/EXE) + the SkyRoads LZ decompressor | ✅ |
| `src/core` | Deterministic ship simulation, demo playback, app/menu state machine | ✅ |
| `src/renderer` | 320×200 software renderer (world, road, ship, HUD, menus) | ✅ |
| `src/audio` | PCM sample playback + software OPL-style synth for MUZAX, mixed to 48 kHz | ✅ |
| `src/platform` | SDL2 host (window, input, audio) | ✅ |
| `src/cli` | `summary` / `demo-sim` inspection & verification CLI | ✅ |

The data layer and ship simulation are covered by equivalence tests
(`data`, `core`, `renderer`, `audio` suites).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The core libraries and CLI need only a C++17 compiler (no external
dependencies). The SDL host needs SDL2.

## Game data files (you provide these)

This repository contains **only source code** — none of the original SkyRoads
game data is redistributed here. SkyRoads is **freeware**, released by its
creators, Bluemoon Interactive. Download the original game from them:

- **http://www.bluemoon.ee/history/skyroads/**

Unzip it and copy the data files into a `skyroads-assets/` directory next to
this README:

```
skyroads-assets/
  ROADS.LZS  TREKDAT.LZS  MUZAX.LZS  INTRO.LZS  ANIM.LZS  *.LZS (worlds, menus)
  INTRO.SND  SFX.SND
  OXY_DISP.DAT  FUL_DISP.DAT  SPEED.DAT
  DEMO.REC
  SKYROADS.EXE   (optional — see below)
```

The game does **not** need `SKYROADS.EXE` at runtime: the few tables the DOS
renderer kept inside the executable were reverse-engineered and baked into the
code. The EXE is only used by the `summary` inspection command and one
validation test. `skyroads-assets/` is git-ignored so the data stays local.

## Run

```sh
./build/skyroads-sdl skyroads-assets            # play (needs SDL2 + a display)
./build/skyroads-cli summary  skyroads-assets   # asset baseline
./build/skyroads-cli demo-sim skyroads-assets 200
```

Controls: ↑/↓ throttle/brake · ←/→ steer · Space skip intro / jump · Enter
select/launch/continue · Tab debug views · Esc back · Q quit.

Flow: intro → main menu → **Start** → pick a world/level → **Enter** to launch.
Crash or finish returns you to the level-select screen.

## Test

```sh
SKYROADS_ASSETS="$PWD/skyroads-assets" ctest --test-dir build --output-on-failure
```

`data`, `core`, and `audio` work with any genuine SkyRoads data set; the
renderer and EXE-validation checks expect the original 30472-byte build.

## Acknowledgements

- **Bluemoon Interactive** — creators of SkyRoads, released as freeware.
- This project used the *SkyRoads-Codex* reverse-engineering / reimplementation
  effort as its starting point.
