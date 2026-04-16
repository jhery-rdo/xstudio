# RodeoFX xStudio Fork — Build Guide & Custom Changes

## Overview

This fork adds RodeoFX-specific C++ plugins and macOS fixes on top of upstream
ASWF xStudio. All changes are isolated under `#ifdef __APPLE__` guards or in
self-contained plugin directories, making upstream merges straightforward.

---

## Custom C++ Plugins

### 1. Media Hook — `libmedia_hook_rodeo.dylib`

**Source:** `src/plugin/media_hook/rodeo/rdohook/`
**Platforms:** macOS + Linux
**Build flag:** `-DSTUDIO_PLUGINS=rodeo`

Sets OCIO context variables (SEQ, SHOT, show) automatically during media
loading, trims slate frames from MOVs, and on macOS remaps Linux-convention
`/shows/` paths to the local NFS/SMB mount (`/rdo/shows/` or
`/Volumes/shows/`). Caches OCIO configs per-show for performance.

See [OCIO_MEDIA_HOOK_PLUGIN.md](OCIO_MEDIA_HOOK_PLUGIN.md) for full details.

### 2. Media Loader — `librdo_media_loader.dylib`

**Source:** `src/plugin/utility/rodeo/media_loader/`
**Platforms:** macOS + Linux
**Build flag:** `-DSTUDIO_PLUGINS=rodeo`

Fast-path C++ media loader used by the Python RDO Browser plugin. Instead of
multiple Python → C++ actor round-trips, Python sends a single JSON payload and
this plugin spawns all media/source actors directly in C++. Uses a 4-worker
pool for concurrent loading with MOV primary + EXR secondary dual-source
support.

### 3. Annotation Onion Skin — `libannotation_onion_skin.dylib`

**Source:** `src/plugin/viewport_overlay/annotation_onion_skin/`
**Platforms:** macOS + Linux
**Build flag:** none (always built)

Viewport overlay that shows annotations from neighboring frames as
semi-transparent, color-tinted layers. Configurable frame range (before/after),
opacity falloff, and tint colors. Caches annotations as the user scrubs.

### 4. macOS Sleep/Wake Observer (macOS only)

**Source:** `src/embedded_python/src/macos_sleep_observer.mm`
**Platforms:** macOS only
**Build flag:** none (automatically included on Apple builds)

After macOS sleep, NFS/SMB mounts go stale. The media pipeline and OCIO config
fail with "Device not configured" errors, making the app appear frozen. This
observer uses IOKit power management on a dedicated thread to detect wake, then
polls the mount point (`/rdo` or `/Volumes/rdo`) until the filesystem recovers.
Once the mount is back, the existing retry paths in the media pipeline succeed
naturally.

**Touch points in existing code** (`src/embedded_python/src/embedded_python.cpp`):
- `register_macos_sleep_observer()` called after `load_python_plugins()` in
  both `connect(int port)` and `connect(caf::actor)`.
- `unregister_macos_sleep_observer()` called in `finalize()`.
- All three call sites are `#ifdef __APPLE__` guarded.

**CMake** (`src/embedded_python/src/CMakeLists.txt`):
- Apple-only block adds the `.mm` source, enables OBJCXX, links IOKit +
  CoreFoundation frameworks.

---

## Build Instructions

### Prerequisites

- CMake 3.20+
- vcpkg (see main README for setup)
- Qt 6.5.x
- On macOS: Xcode command line tools

### macOS (Apple Silicon)

```bash
# Configure — use the MacOSRelease preset which sets vcpkg, Qt path, etc.
cmake --preset MacOSRelease

# Build all targets (includes RDO plugins + onion skin + sleep observer)
cmake --build build

# Or build just the RDO plugins
cmake --build build --target media_hook_rodeo
cmake --build build --target rdo_media_loader
cmake --build build --target annotation_onion_skin
cmake --build build --target embedded_python   # includes sleep observer
```

Other macOS presets: `MacOSDebug`, `MacOSRelWithDebInfo`, `MacOSIntelRelease`.

### Linux

```bash
cmake --preset LinuxRelease -DSTUDIO_PLUGINS=rodeo
cmake --build build
```

The sleep observer `.mm` file is skipped automatically (Apple-only in CMake).

### What `-DSTUDIO_PLUGINS=rodeo` does

The `build_studio_plugins()` macro in `cmake/macros.cmake` scans for plugin
directories under the named site folder. With `rodeo`, it discovers:
- `src/plugin/media_hook/rodeo/rdohook/` → `libmedia_hook_rodeo.dylib`
- `src/plugin/utility/rodeo/media_loader/` → `librdo_media_loader.dylib`

The annotation onion skin is under `src/plugin/viewport_overlay/` and is built
unconditionally (no flag needed).

---

## macOS Packaging

The RDO Python plugins, shared libraries, and preferences are bundled into the
app using `rdo_xstudio_user_tools/package_macos.sh`. The workflow:

1. Build xStudio (creates `build/xSTUDIO.app` — the "vanilla" app)
2. Copy the vanilla to `xstudio_binaries/1.2.0-rdo/xSTUDIO_vanilla.app`
3. Run `package_macos.sh` which:
   - Copies the vanilla app
   - Installs Python deps (shotgun_api3) into `Resources/python-lib/`
   - Copies `rdo_xstudio_user_tools` package into `Resources/python-lib/`
   - Copies RDO Python plugins into `Resources/plugin-python/`
   - Copies RDO C++ plugins from `build/xSTUDIO.app/Contents/PlugIns/xstudio/`
   - Installs RDO preferences into `Resources/rdo-config/preferences/`
   - Creates a launcher script that sets PYTHONPATH, XSTUDIO_PLUGIN_PATH, etc.
   - Builds a DMG

---

## Tracking Changes After Upstream Sync

All RDO changes are designed to be merge-friendly:

### Files that won't conflict (new, RDO-only)
- `src/embedded_python/src/macos_sleep_observer.mm`
- `src/plugin/media_hook/rodeo/` (entire directory)
- `src/plugin/utility/rodeo/` (entire directory)
- `src/plugin/viewport_overlay/annotation_onion_skin/` (entire directory)
- `docs/rodeo/` (this documentation)

### Files with small, guarded changes (may conflict on merge)
- `src/embedded_python/src/embedded_python.cpp` — three `#ifdef __APPLE__`
  blocks (extern declarations, register call, unregister call)
- `src/embedded_python/src/CMakeLists.txt` — one `if(APPLE)` block

### Quick check after a merge

```bash
# Verify the sleep observer file exists
ls src/embedded_python/src/macos_sleep_observer.mm

# Verify the #ifdef blocks are still in embedded_python.cpp
grep -n "register_macos_sleep_observer\|unregister_macos_sleep_observer" \
  src/embedded_python/src/embedded_python.cpp

# Verify CMake Apple block
grep -A3 "if(APPLE)" src/embedded_python/src/CMakeLists.txt

# Verify RDO plugin directories exist
ls src/plugin/media_hook/rodeo/rdohook/src/rdohook.cpp
ls src/plugin/utility/rodeo/media_loader/src/rdo_media_loader.cpp
ls src/plugin/viewport_overlay/annotation_onion_skin/src/onion_skin_plugin.cpp
```

If `embedded_python.cpp` conflicts during merge, re-apply the three blocks from
commit `3722ce7` (or search for `register_macos_sleep_observer` in git log).
