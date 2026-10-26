# WillPowerEngine — Project Specification

A high-performance, production-grade software rasterizer 3D game engine for Windows and Linux.

**Design goal:** Simple and straightforward. Stable, fully tested, and real-world deployable — no hacks, no shortcuts.

**Target resolutions:** Primarily 320×240 to 640×480, scalable up to 1280×960.

---

## 1. Development Workflow

- **Autonomy:** Ask questions only when necessary; otherwise work autonomously.
- **Testing is paramount:**
  - Extensive unit tests (simple framework preferred).
  - Automated test runs (CI or scripted local runs).
- **Test scene:** A dedicated scene exercising every engine feature — all components and the entire scripting API.
- **Quality bar:** Production-grade throughout; this is not a hack project.
- **Agent support:** Maintain `AGENTS.md` plus relevant skill files containing concrete development guidelines.
- **Scope:** Do not look outside the working directory.
- **Repository:** https://github.com/MikaLaurila/WillPowerEngine.git

## 2. Tech Stack

| Concern | Choice |
|---|---|
| Language / Build | **C23**, **CMake** + **Ninja** |
| Windowing / Input | **SDL3** |
| Rendering | Custom **software rasterizer** (CPU, multithreaded) |
| Math | **cgml** or similar |
| Audio | **miniaudio** |
| Scripting | **Luau** |
| Editor UI | **cimgui** |
| Physics | **Box3D** |
| Compression | **zstd** or **LZ4** (for `.pak` archives) |

## 3. Core Architecture

- Static library for the core engine; separate executables for the editor and the game runtime (final design may adjust this split).
- Simple **ECS** for authoring scenes with entities and components.
- **Game export:** produce a distributable standalone executable that runs the game without editing capability.
- **AI-agent-friendly editor:** the editor must be fully controllable by AI agents — all tools available without human intervention.

## 4. Renderer & Asset Pipeline

### Rasterizer
- Custom high-performance, multithreaded software rasterizer supporting both 2D and 3D.
- Custom **vertex and fragment shaders**.
- Traditional (non-PBR) material workflow: diffuse / specular / glossiness + normal/bump maps.
- Post-processing effects via a simple mechanism (chromatic aberration, PS1-style dithering, etc.) — ideally definable from Luau.

### Lighting & Effects
- **Vertex lighting**, with triangle subdivision near the camera (LOD-style tessellation) to preserve lighting quality at close range.
- Light types: directional, a limited number of spot lights, and point lights.
- Shadows.
- Fog.
- SSAO if feasible.

### Assets
- glTF import: meshes, materials, and textures (Khronos glTF sample assets will be provided for testing).
- Skinned mesh animation.
- Bitmap font rendering (test font provided; characters 32–126; magenta is the separator/key color in the font bitmap).

## 5. Physics

- **Box3D**, updated in a fixed timestep game loop.
- Authored via components in the editor; controlled via scripting.

## 6. Behavior / Scripting

- **Luau** scripts attached to entities.
- Script lifecycle callbacks:
  - `on_update(dt)` — variable-rate frame update.
  - `on_fixed_update(dt)` — fixed-timestep update, aligned with physics.
- Scripts can control entities/components, create and destroy entities, and spawn prefabs.
- Hot reload support (see Editor).

## 7. Editor

Build these tools first; add further ones as they prove useful for game development:

- Scene save/load
- Drag-and-drop assets/prefabs
- Undo/redo
- Console
- Grid
- Play-in-editor with state isolation (play/stop restores scene state)
- Luau hot reload
- Transform gizmo (move/rotate/scale, local/world space)
- Viewport entity picking
- Blender-style viewport navigation (MMB orbit around soft focus pivot, Shift+MMB pan, wheel dolly, `F` to focus/frame an entity)
- Entity hierarchy (add/delete entities, add child entities)
- Entity inspector (add/remove/edit components)
- Debug physics drawing
- Asset browser
