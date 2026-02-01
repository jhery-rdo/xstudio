# Smart C++ Conform Engine - Implementation Status

**Date:** 2026-02-01
**Status:** IN TESTING - Bug fixes applied

---

## Recent Changes (2026-02-01)

### Bug Fix: Python Data Source Not Loading

**Issue:** The Python `RdoShotGridDataSource` plugin was never being instantiated because xStudio only calls one `create_plugin_instance` per Python plugin package.

**Fix:** Updated `rdo_conform_plugin.py` to also create the data source plugin:

```python
def create_plugin_instance(connection):
    main_plugin = RdoConformPlugin(connection)

    # Also create the data source for C++ smart engine
    from rdo_xstudio_user_tools.tools.rdo_conform.rdo_shotgrid_datasource import RdoShotGridDataSource
    data_source = RdoShotGridDataSource(connection)
    main_plugin._shotgrid_datasource = data_source  # Prevent GC

    return main_plugin
```

### Enhanced Logging

Added info-level logging throughout the smart engine flow to help diagnose issues:
- Template track count and metadata entries
- Project code extraction attempts
- Data source registry lookup
- Shot structure extraction

---

## Overview

The Rodeo conform system has been refactored so C++ handles performance-critical work (timeline parsing, query batching, clip creation, track population) while Python only handles ShotGrid API calls.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    C++ Conform Engine                           │
│                  (conform_rodeo.cpp v2.0)                       │
│                                                                 │
│  1. Parse timeline → extract shot structure ✅                  │
│  2. Deduplicate shot codes → build batch query ✅               │
│  3. Send single request to data source ✅                       │
│  4. Receive versions_by_shot ✅                                 │
│  5. Create media actors from paths ✅                           │
│  6. Build ConformReply with media ✅                            │
└────────────────────────────────────────────────────────────────┘
                              │
                              │ mail(get_data_atom_v, query)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              C++ Data Source Bridge                             │
│          (rdo_shotgrid_datasource.cpp) ✅                       │
│                                                                 │
│  - Registered as "RDOSHOTGRID" in CAF registry                 │
│  - Finds Python plugin via plugin manager                       │
│  - Sets "SG Request" attribute with query                       │
│  - Polls "SG Response" attribute for result                     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ Attribute communication
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              Python Data Source                                 │
│          (rdo_shotgrid_datasource.py) ✅                        │
│                                                                 │
│  - Monitors "SG Request" attribute                              │
│  - Execute ShotGrid API call                                    │
│  - Write result to "SG Response" attribute                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## Files Modified/Created

### 1. Python Data Source (NEW)
**File:** `rdo_xstudio_user_tools/python/rdo_xstudio_user_tools/tools/rdo_conform/rdo_shotgrid_datasource.py`

- `RdoShotGridDataSource` class with attribute-based communication
- `_batch_query_versions()` - single optimized ShotGrid query
- Returns `versions_by_shot` with full version details

### 2. Python Conform Plugin (UPDATED)
**File:** `rdo_xstudio_user_tools/python/rdo_xstudio_user_tools/tools/rdo_conform/rdo_conform_plugin.py`

- `create_plugin_instance()` now creates BOTH plugins
- Data source kept as `main_plugin._shotgrid_datasource`

### 3. C++ Data Source Bridge (NEW)
**File:** `xstudio_fork/src/plugin/data_source/rodeo/rdo_shotgrid/src/rdo_shotgrid_datasource.cpp`

- Registers as "RDOSHOTGRID" in CAF actor registry
- Delegates to Python plugin via module attributes
- Handles `get_data_atom_v` requests with timeout

### 4. C++ Smart Conform Engine (UPDATED)
**File:** `xstudio_fork/src/plugin/conform/rodeo/conform_rodeo/src/conform_rodeo.cpp`

Key methods added:
- `get_data_source()` - retrieves RDOSHOTGRID from registry
- `send_batch_query()` - sends query to Python via data source
- `process_batch_response()` - creates media actors from version paths
- Full `ConformReply` with `create_media` and `insert_media` flags

### 5. QML Conform Tool (UPDATED)
**File:** `xstudio_fork/ui/qml/xstudio/tools/conform/XsConformTool.qml`

- Removed Python plugin bypass for Rodeo tasks
- All tasks now flow through C++ conform engine

---

## How to Test

### 1. Build
```bash
cd /mnt/users/jhery/Documents/code/xstudio_fork/build
cmake -DSTUDIO_PLUGINS=rodeo ..
make -j8
```

### 2. Run xStudio and load a timeline

### 3. Test Auto-Conform
- Right-click on a track → "Auto-Conform" → Select a Rodeo task (e.g., "Rodeo: Comp: Delivered")
- Or use timeline menu → "Auto-Conform Video" → Select task

### 4. Check logs for smart engine flow:
```
RodeoConformActor: Smart engine processing task 'Rodeo: Comp: Delivered' with N items
RodeoConformActor: template_tracks=1, metadata entries=N
RodeoConformActor: Template track 'Conform Track' with N children
RodeoConformActor: Using preset 'Comp: Delivered' (dept=Comp, status=dlvr)
RodeoConformActor: Project code: drb
RodeoConformActor: Extracted N structure items
RodeoConformActor: N unique shots to query
RodeoConformActor: Looking up 'RDOSHOTGRID' data source in registry
RodeoConformActor: Found RDOSHOTGRID data source
RodeoConformActor: Sending batch query to RDOSHOTGRID data source
RdoShotGridDataSource bridge created
RdoShotGridDataSource: attribute_changed called, attr=...
RdoShotGridDataSource: SG Request received, length=...
RdoShotGridDataSource: Querying N shots for Comp, status=['dlvr', 'cfin']
RodeoConformActor: Received response from data source
RodeoConformActor: Got N versions, M missing shots
RodeoConformActor: Created media for shot 206044_0010: ...
RodeoConformActor: Delivering ConformReply with N items
```

---

## Troubleshooting

### If you see "No template tracks, falling back to basic conform"
- The timeline may not have been prepared with a conform track
- Use the "Prepare Timeline" feature first, or ensure bottom video track exists

### If you see "Could not determine project code"
- The clips in the template track don't have project metadata
- Ensure clips have `shotgrid.project_code` or `external.RodeoFX.show` metadata

### If you see "RDOSHOTGRID data source NOT found in registry"
- The C++ data source bridge plugin isn't loading
- Check that `librdo_shotgrid.so` is in the plugin directory
- Check xStudio logs for plugin loading errors

### If Python data source isn't responding
- Check for `RdoShotGridDataSource initialized` in logs
- Verify ShotGrid connection is available

---

## Request/Response Format

### C++ → Python (BatchQueryVersions)
```json
{
  "operation": "BatchQueryVersions",
  "project_code": "drb",
  "shot_codes": ["206044_0010", "206044_0020"],
  "department": "Comp",
  "status_list": ["dlvr", "cfin"]
}
```

### Python → C++ (Response)
```json
{
  "success": true,
  "versions_by_shot": {
    "206044_0010": {
      "version_id": 12345,
      "version_code": "206044_0010.comp.v7",
      "sg_path_to_movie": "/path/to/movie.mov",
      "sg_path_to_frames": "/path/to/frames.%04d.exr",
      "sg_department": "Comp",
      "sg_status_list": "dlvr",
      "shot_id": 67890
    }
  },
  "missing_shots": ["206044_0030"]
}
```

---

## Files Summary

| File | Status | Description |
|------|--------|-------------|
| `rdo_shotgrid_datasource.py` | ✅ | Python data source for ShotGrid queries |
| `rdo_conform_plugin.py` | ✅ | Updated to create both plugins |
| `rdo_conform/__init__.py` | ✅ | Added data source export |
| `rdo_shotgrid_datasource.cpp` | ✅ | C++ data source bridge |
| `rdo_shotgrid/src/CMakeLists.txt` | ✅ | Build config for C++ bridge |
| `conform_rodeo.cpp` | ✅ | Smart engine with enhanced logging |
| `XsConformTool.qml` | ✅ | Updated to use C++ engine for Rodeo tasks |
