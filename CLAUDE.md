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
