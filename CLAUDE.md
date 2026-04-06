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
- Build single plugin: `cmake --build build --target media_hook_rodeo -j$(sysctl -n hw.ncpu)`
- Plugin output: `build/xSTUDIO.app/Contents/PlugIns/xstudio/`

## Plugins
- `src/plugin/media_hook/rodeo/rdohook/` — RodeoFX media hook: OCIO context, slate trimming, path remapping
- Standalone build guarded by `if(DEFINED XSTUDIO_GLOBAL_VERSION) return()` in plugin CMakeLists.txt

## macOS Path Handling
- Media paths from OTIO/database use `/rdo/shows/...` (Linux/NFS convention)
- On macOS: NFS mounts at `/rdo/shows`, Samba mounts at `/Volumes/shows`
- RodeoMediaHook detects mount type at startup and normalizes paths via `normalize_path()`
- OCIO configs reference `/shows/...` search paths — rewritten to temp file with correct prefix on macOS
