# WillPowerEngine

A multithreaded **software rasterizer** 3D game engine in C23 for Windows and Linux, targeting
retro resolutions (320×240 to 640×480, scalable to 1280×960). It comes with an editor that AI
agents can drive fully, and it exports standalone game executables.

> Status: early development. Done so far:
>
> - Core module, math, ECS and scene hierarchy
> - Job system and multithreaded software rasterizer, with an SDL3 window
> - glTF model loading, textured materials, skinned animation and bitmap-font text
> - Hybrid vertex/pixel lighting (directional, point and spot lights), directional shadows, fog,
>   adaptive subdivision and post-processing (SSAO, PS1 dither, chromatic aberration, ...)
> - Sandboxed Luau scripting with hot reload (see [docs/scripting.md](docs/scripting.md))
> - Box3D rigid body physics with colliders, triggers, raycasts and script collision callbacks
> - 3D audio (miniaudio) with streamed music, and a virtual file system with zstd `.pak` archives
>   and reference-counted assets (see [docs/assets.md](docs/assets.md))
> - Keyboard, mouse and gamepad input with named actions; JSON scene and project files; a game
>   runtime and export to a single standalone executable (see [docs/runtime.md](docs/runtime.md)
>   and [docs/scenes.md](docs/scenes.md))
>
> See [CHANGELOG.md](CHANGELOG.md) for progress.

Play the demo game (keyboard and mouse or a gamepad) and export it:

```bash
./build/bin/wpe_runtime --project assets                         # the game in assets/ (--watch: hot reload)
./build/bin/wpe_runtime --project assets --scene scenes/test_full.scene   # the master test scene
./build/bin/wpe_export assets demo_game.exe                      # one self-contained executable
```

The engine samples:

```bash
./build/samples/spinning_cubes                                   # --width 640 --height 480 --scale 2 --workers N
./build/samples/model_viewer assets/meshes/damaged_helmet.glb    # any .glb/.gltf; --dither, --max-texture N
./build/samples/lighting_demo --ssao --fog                       # also --ps1 --chroma --vignette --dither --no-shadows
./build/samples/script_demo --watch                              # edit assets/scripts/*.luau while it runs; --debug-physics --mute
./build/bin/wpe_pak create assets.pak assets && ./build/samples/script_demo --pak assets.pak     # run from an archive
```

## Documents

- [WillPowerEngine Spec (Refined).md](<WillPowerEngine Spec (Refined).md>): product specification.
- [WillPowerEngine AGENTS.md](<WillPowerEngine AGENTS.md>): development guidelines, coding standards and architecture.

## Building

Requirements: a C23 compiler (GCC 14+ or Clang 18+; on Windows, MSYS2 UCRT64 GCC), CMake 3.28+ and Ninja.
CMake downloads third-party dependencies at pinned versions the first time you configure, so that step needs network access.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cmake --build build --target test_all
```

## Dependencies

Each dependency is added in the milestone that needs it. All are permissively licensed:
SDL3, cglm, cgltf, stb_image, Luau, miniaudio, cimgui, Box3D, zstd.

## License

MIT; see [LICENSE](LICENSE). Test assets under `assets/meshes/` are CC BY 4.0; see
[assets/meshes/ATTRIBUTION.md](assets/meshes/ATTRIBUTION.md). The demo sounds were synthesized for this
repository (MIT); see [assets/sounds/ATTRIBUTION.md](assets/sounds/ATTRIBUTION.md).
