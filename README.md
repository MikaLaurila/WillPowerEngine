# WillPowerEngine

A multithreaded **software rasterizer** 3D game engine in C23 for Windows and Linux, targeting
retro resolutions (320×240 to 640×480, scalable to 1280×960). It comes with an editor that AI
agents can drive fully, and it exports standalone game executables.

> Status: early development. The core module (results, logging, assertions, tracked allocation) and
> the test infrastructure are in place. See [CHANGELOG.md](CHANGELOG.md) for progress.

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
SDL3, cglm, miniaudio, Luau, cimgui, Box3D, zstd.

## License

MIT; see [LICENSE](LICENSE). Test assets under `assets/meshes/` are CC BY 4.0; see
[assets/meshes/ATTRIBUTION.md](assets/meshes/ATTRIBUTION.md).
