# RodeoFX Media Hook Plugin for OCIO Context

## Overview

This plugin replaces the Python-based OCIO context setting in `rdo_xstudio_user_tools` with a native C++ media hook plugin that automatically sets OCIO context during media loading. This provides:
- Better performance (no Python overhead)
- Automatic context injection (before colour pipeline initializes)
- Consistent behavior across all media loading workflows

## Files Created

| File | Description |
|------|-------------|
| `src/plugin/media_hook/rodeo/rdohook/src/rdohook.cpp` | Main plugin implementation |
| `src/plugin/media_hook/rodeo/rdohook/src/CMakeLists.txt` | CMake build configuration |
| `src/plugin/media_hook/rodeo/rdohook/src/plugin_rodeo_media_hook.json` | Plugin preferences |
| `src/plugin/media_hook/rodeo/rdohook/test/CMakeLists.txt` | Test framework placeholder |

## Build Instructions

Build xStudio with the RodeoFX plugin enabled:

```bash
cmake -DSTUDIO_PLUGINS=rodeo ...
```

The plugin will be discovered automatically by the existing `build_studio_plugins()` macro in `src/plugin/media_hook/CMakeLists.txt`.

## Plugin Features

### 1. OCIO Context Injection (`modify_metadata`)

Automatically sets OCIO context variables on media metadata:

```json
{
  "colour_pipeline": {
    "ocio_context": {
      "SEQ": "505113",
      "SHOT": "0020",
      "REZ_RDO_OCIO_CONFIG_ROOT": "/rdo/software/rez/packages/rdo_ocio_config/2.5.0",
      "RDO_CURRENT_SHOW": "cdl"
    },
    "ocio_config": "/rdo/shows/SHOW/_project_ref/_imaging/_ocio2_xstudio/SHOW_config.ocio",
    "override_view": "raw",
    "input_colorspace": "Utility - Raw"
  }
}
```

### 2. Slate Frame Trimming (`modify_media_reference`)

Automatically trims the first frame (slate) from MOVs in:
- `/movhistory/` folders
- `/.published/` folders

### 3. Path Pattern Support

Extracts SEQ/SHOT from various RodeoFX path formats:

| Pattern | Example |
|---------|---------|
| `/shows/SHOW/SEQ/SHOT/` | `/shows/myshow/jtz/5010/` |
| `/shows/SHOW/SEQ/SEQ_SHOT/` | `/shows/myshow/jtz/jtz_5010/` |
| `/.published/SEQ/SEQ_SHOT/` | `/.published/505113/505113_0010/` |
| Filename pattern | `jtz_5010_comp_v01.mov` |
| 6-digit SEQ | `/505113/505113_0010/` |

### 4. File Type Detection

| File Type | Override View | Input Colorspace |
|-----------|---------------|------------------|
| MOVs (.mov, .mp4, .mxf, .qt) | `raw` | `Utility - Raw` |
| Stills (.tiff, .jpeg, .png) | `raw` | `Utility - Raw` |
| EXRs with `.lineup.` | `Client-look (non-wb)` | (scene-linear) |
| Other EXRs | (none) | (scene-linear) |

## Configurable Preferences

Access in xStudio Preferences under "RodeoFX" category:

| Setting | Default | Description |
|---------|---------|-------------|
| Auto Trim Slate | `true` | Trim first frame from MOVs in /movhistory or .published |
| REZ OCIO Config Root | `/rdo/software/rez/packages/rdo_ocio_config/2.5.0` | Path to REZ OCIO config package |
| Default Show Code | `cdl` | Fallback show code when path extraction fails |

## Environment Variable Fallbacks

The plugin checks these environment variables as fallbacks:

| Variable | Purpose |
|----------|---------|
| `SEQ` | Sequence code fallback |
| `SHOT` | Shot code fallback |
| `RDO_CURRENT_SHOW` | Show code fallback |
| `REZ_RDO_OCIO_CONFIG_ROOT` | Override REZ config root |

## Comparison with Python Implementation

| Aspect | Python (rdo_xstudio_user_tools) | C++ Plugin |
|--------|--------------------------------|------------|
| Timing | After media added to session | During media loading |
| Performance | Python overhead | Native C++ |
| Consistency | Manual calls required | Automatic for all media |
| Configuration | Hardcoded paths | Preferences UI |

## Migration Path

After the C++ plugin is working:

1. **Remove Python OCIO context calls** from:
   - `playlist_ops.py` - Remove `set_ocio_context_on_media_source()` calls
   - `swap_clip_media.py` - Remove OCIO context setting before media switch
   - `cut_context.py` - Remove OCIO context embedding in OTIO clips

2. **Keep Python utilities** that are still needed:
   - `setProjectEnvironment()` - May still be useful for setting global OCIO config

## Verification

1. **Build test**: Compile xstudio with `-DSTUDIO_PLUGINS=rodeo`
2. **Plugin load test**: Check xstudio logs for "RodeoFX Media Hook" plugin registration
3. **Functional test**: Load media from various RodeoFX paths and verify:
   - OCIO context is set (check `/colour_pipeline/ocio_context` metadata)
   - Correct SEQ/SHOT extraction
   - MOVs use raw view
   - EXRs use correct views
4. **Comparison test**: Compare colour output between Python and C++ approaches

## Architecture Notes

The plugin follows the same pattern as the DNEG media hook:

- Extends `MediaHook` base class
- Implements `modify_metadata()` and `modify_media_reference()` overrides
- Uses `MediaHookPlugin<MediaHookActor<T>>` registration pattern
- Preferences stored via `module::*Attribute` system

Plugin UUID: `a7b8c9d0-e1f2-4a5b-8c7d-9e0f1a2b3c4d`
