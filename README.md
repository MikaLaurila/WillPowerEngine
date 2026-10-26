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
>
> See [CHANGELOG.md](CHANGELOG.md) for progress.

Try the demos after building:

```bash
./build/samples/spinning_cubes                                   # --width 640 --height 480 --scale 2 --workers N
./build/samples/model_viewer assets/meshes/damaged_helmet.glb    # any .glb/.gltf; --dither, --max-texture N
./build/samples/lighting_demo --ssao --fog                       # also --ps1 --chroma --vignette --dither --no-shadows
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
SDL3, cglm, cgltf, stb_image, miniaudio, Luau, cimgui, Box3D, zstd.

## License

MIT; see [LICENSE](LICENSE). Test assets under `assets/meshes/` are CC BY 4.0; see
[assets/meshes/ATTRIBUTION.md](assets/meshes/ATTRIBUTION.md).
