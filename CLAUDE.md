# xStudio Codebase Guide

## Architecture
- C++ Actor Framework (CAF) based — actors communicate via typed atom messages
- Key pattern: `mail(atom_v).request(actor, timeout).then(callback)` for async messaging
- Headers in `include/xstudio/`, implementations in `src/<module>/src/`
- Base classes often hold cached state (e.g., `PlayheadBase::duration_`) updated by event handlers

## Key Modules
- `src/playhead/` — Playback control, scrubbing, timeline position. `playhead_actor.cpp` is very large (~3000+ lines)
- `src/timeline/` — Timeline/cut management
- `src/media/` — Media sources and readers
- `ui/qml/` — QML-based UI layer

## Performance Gotchas
- Avoid async self-requests (`mail(...).request(this, infinite)`) in hot paths (e.g., scrubbing) — use cached member state instead
- `duration_flicks_atom_v` self-request triggers heavyweight `update_duration()` cascade — only needed on source changes, not per-frame

## Build & Version
- Version tags: `v1.0.0-alpha`, `1.1.0`
- Branch `develop` tracks active development
- Build dir: `build/` (Makefile generator, vcpkg dependencies)
- Standalone plugin build requires vanilla app bundle: `cp -R .../xSTUDIO_vanilla.app build/xSTUDIO.app`
- Build single plugin: `cmake --build build --target media_hook_rodeo -j$(sysctl -n hw.ncpu)`
- Build media loader: `cmake --build build --target rdo_media_loader -j$(sysctl -n hw.ncpu)`
- Plugin output: `build/xSTUDIO.app/Contents/PlugIns/xstudio/`
- Install to binary: `cp build/xSTUDIO.app/Contents/PlugIns/xstudio/lib*.dylib .../xstudio_binaries/1.2.0-rdo/xStudio.app/Contents/PlugIns/xstudio/`

## CAF Actor Gotchas
- `playlist::add_media_atom(UuidActorVector, Uuid)` returns `bool`, NOT `UuidActorVector`
- Subset `add_media_atom(UuidActor, Uuid)` returns `Uuid`, NOT `bool` — use fire-and-forget
- `FrameList` Python binding requires 3 args: `FrameList(start, end, step)` — no 2-arg constructor
- `FrameList` has no `.count()` method — use None tracking instead
- `parse_cli_posix_path()` doesn't understand `{:04d}` — convert to `####` first
- `event_based_actor` cannot use `.receive()` — must use `.then()` promise chains

## OCIO Config (macOS)
- Cross-platform configs contain Windows variables (`${WIN_S_DRIVE}`, `${WIN_REZ_RDO_OCIO_CONFIG_ROOT}`) — filter in `fix_ocio_config_for_macos`
- Python pre-warms OCIO config to `/tmp/xstudio_ocio_{show}_config.ocio` BEFORE C++ hook runs — C++ must always regenerate
- Delete cached OCIO config when testing changes: `rm -f /tmp/xstudio_ocio_*`
- `/Volumes/software/` Samba mount may not exist — LUT files are at `/rdo/software/` (NFS)

## Plugins
- `src/plugin/media_hook/rodeo/rdohook/` — RodeoFX media hook: OCIO context, slate trimming, path remapping
- Standalone build guarded by `if(DEFINED XSTUDIO_GLOBAL_VERSION) return()` in plugin CMakeLists.txt

## macOS Path Handling
- Media paths from OTIO/database use `/rdo/shows/...` (Linux/NFS convention)
- On macOS: NFS mounts at `/rdo/shows`, Samba mounts at `/Volumes/shows`
- RodeoMediaHook detects mount type at startup and normalizes paths via `normalize_path()`
- OCIO configs reference `/shows/...` search paths — rewritten to temp file with correct prefix on macOS
