# Assets, the virtual file system and `.pak` archives

## Virtual file system (`vfs/vfs.h`)

Every asset the engine reads (models, textures, fonts, scripts, sounds and music) is named by an **asset path**: a relative, `/`-separated UTF-8 path such as `meshes/helmet.glb`. Paths can't be absolute, and they can't contain empty, `.` or `..` segments, backslashes, colons or control characters. The limit is 255 bytes.

A `WPE_Vfs` maps asset paths onto **mounts**:

- **Directories** (`wpe_vfs_mount_directory`): the usual setup during development. Their files are read live, so hot reload sees changes.
- **Archives** (`wpe_vfs_mount_pak`): the shipped form.

Later mounts override earlier ones. So a patch archive mounted last replaces individual files of the base game, and a development directory mounted over a shipped archive overrides it.

Paths are case-sensitive inside archives. Inside directories, case follows the host file system, which is case-insensitive on Windows. Use lowercase names.

glTF files may refer to their buffers and textures with `..` (for example `../textures/wood.png`), as long as the result stays inside the VFS.

## Asset manager (`assets/asset_manager.h`)

The asset manager loads assets through a VFS, caches them by `(type, path)` and reference-counts them. Each asset is identified by a generational `WPE_AssetHandle`, so a stale handle is detected rather than silently reaching a newer asset.

- `wpe_asset_load(assets, WPE_ASSET_MODEL, "meshes/helmet.glb", &handle)` loads the asset, or returns the cached one, with one more reference.
- `wpe_asset_release` drops a reference. Releasing the last reference destroys the asset.
- Components that reference assets hold their own reference for as long as they exist:
  - mesh renderers and animators (model)
  - colliders (convex and mesh shapes)
  - audio sources (sound)

  The usual pattern is: load, set the handle in the component, add the component, and release your own reference whenever it suits you.
- Assets created in code, such as procedural meshes, are handed over with `wpe_asset_add(assets, type, "procedural/box", model, &handle)`. From then on they behave like loaded ones.
- **Built-in types:** models (glTF/GLB) and fonts. The audio module registers sounds (WAV, FLAC, MP3). Games can register their own types from `WPE_ASSET_USER_FIRST` with a load/destroy pair.

**Destroy order:** destroy everything that holds handles first (script host, audio, physics, the ECS world), then the asset manager, then the VFS. If anything still holds a reference at shutdown, a warning names the asset.

## The `wpe_pak` tool

```bash
wpe_pak create game.pak assets [--level 1..19]   # pack every file below a directory (zstd level, default 9)
wpe_pak list game.pak                             # sizes, compression and CRC-32 of every file
wpe_pak verify game.pak                           # read every file and check its CRC-32
wpe_pak extract game.pak out_dir                  # unpack
```

- Files are packed in sorted path order, so the same input always gives a byte-identical archive.
- Files whose names aren't valid asset paths are reported and skipped.
- Each file is compressed with zstd only when that saves at least 5%. Already-compressed formats (PNG, JPEG, MP3, FLAC) are therefore stored as they are and can be streamed straight from the archive.

Try it with the script demo: `wpe_pak create assets.pak assets`, then `script_demo --pak assets.pak`.

## `.pak` format, version 1

All integers are little-endian. An archive is a fixed header, then the file data, then an index.

**Header** (40 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Magic `WPE_PAK\0` |
| 8 | 4 | Format version (1) |
| 12 | 4 | Entry count (at most 2^20) |
| 16 | 8 | Index offset (the index is the last thing in the file) |
| 24 | 8 | Index size in bytes (at most 256 MiB) |
| 32 | 4 | CRC-32 of the index bytes |
| 36 | 4 | Reserved (0) |

**Data:** each file's bytes, either stored as they are or as a single zstd frame that records its content size, back to back starting at offset 40.

**Index:** one record per file, sorted by path (bytewise, ascending), with unique paths.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | Data offset |
| 8 | 8 | Stored size |
| 16 | 8 | Uncompressed size (equal to the stored size when not compressed) |
| 24 | 4 | CRC-32 (IEEE 802.3) of the uncompressed contents |
| 28 | 2 | Flags: bit 0 = zstd; all other bits 0 |
| 30 | 2 | Path length in bytes (1 to 255) |
| 32 | 4 | Reserved (0) |
| 36 | n | Path (UTF-8, no terminator; must be a valid asset path) |

A writer leaves the header zeroed until it has finished writing, so an interrupted write never produces a valid archive.

Readers reject an archive if any of these hold:

- the magic is wrong, or the version is unknown;
- the index checksum doesn't match;
- an offset falls outside the data area;
- a path is invalid, unsorted or duplicated;
- reserved fields aren't zero.

Each file's CRC-32 is checked whenever the whole file is read. Files streamed straight from an archive (stored music) are not checked.
