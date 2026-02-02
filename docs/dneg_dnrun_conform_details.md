# DNRun and Conform ShotBrowser - Detailed Analysis

## DNRun Utility Plugin

**Source:** `src/plugin/utility/dneg/dnrun/src/dnrun.cpp`

### Overview

DNRun is a Unix domain socket server that enables external applications (Nuke, Maya, Houdini, pipeline tools) to send media loading requests to xStudio. It uses the netstring protocol for message framing.

### Architecture

```
┌─────────────────┐     Unix Socket      ┌─────────────────┐
│  External App   │ ─────────────────────│     DNRun       │
│  (Nuke, Maya)   │   netstring/JSON     │    Plugin       │
└─────────────────┘                      └────────┬────────┘
                                                  │
                                                  ▼
                                         ┌─────────────────┐
                                         │    xStudio      │
                                         │    Session      │
                                         └─────────────────┘
```

### Socket Configuration

| Parameter | Value |
|-----------|-------|
| Socket Type | `AF_UNIX` (Unix domain socket) |
| Mode | `SOCK_STREAM | SOCK_NONBLOCK` |
| Namespace | Abstract (Linux-specific) |
| Socket Name | `dnrun-v1-xstudio-{SHOW}-{login}` |
| Max Connections | 10 |
| Poll Timeout | 100ms (connected) / 5000ms (disconnected) |

### Netstring Protocol

The netstring format is: `{length}:{payload},`

**Example:**
```
47:{"args":{"paths":["/path/to/file.exr"]}},
```

**Parsing Rules:**
- Header: digits followed by `:`
- Maximum header length: 6 characters (max payload ~999,999 bytes)
- Leading zeros are invalid
- Payload: exact number of bytes specified in header
- Tail: single `,` character

### JSON Request Format

```json
{
  "args": {
    "paths": [
      "/path/to/media1.exr",
      "/path/to/media2.mov"
    ],
    "quickview": true,
    "compare": "ab"
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `args.paths` | array | List of media paths to load (required) |
| `args.quickview` | bool | Launch quick viewer for loaded media |
| `args.compare` | string | Set to `"ab"` for A/B comparison mode |

### Supported Path Types

1. **POSIX paths** - Regular file paths (`/path/to/file.exr`)
2. **Plugin URIs** - Custom URI schemes handled by data source plugins (`ivy://load/...`)
3. **Unsupported** - `xstudio://` URIs (logged as warning)

### Request Processing Flow

```
1. Socket receives data
2. Buffer netstring messages
3. On connection close, parse complete messages
4. For each request:
   a. Parse JSON
   b. Get session and playlist actors
   c. For each path:
      - If plugin URI → route to plugin manager
      - If POSIX path → parse and add to playlist
   d. If quickview → open quick viewer window
   e. Select loaded media in playlist
```

### Key Code Sections

**Socket Creation (lines 65-104):**
```cpp
sock_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
sun_.sun_family = AF_UNIX;
strncpy(sun_.sun_path + 1, port_name_v1_.c_str(), sizeof(sun_.sun_path) - 1);
bind(sock_, (struct sockaddr *)&sun_, sizeof(sun_.sun_family) + port_name_v1_.size() + 1);
listen(sock_, connection_max);
```

**Media Loading (lines 409-434):**
```cpp
// Parse CLI path with frame list
caf::uri uri = parse_cli_posix_path(path, fl, true);

// Add to playlist
if (fl.empty())
    new_media = request_receive<UuidActor>(*sys, playlist, playlist::add_media_atom_v,
                                            path, uri, Uuid());
else
    new_media = request_receive<UuidActor>(*sys, playlist, playlist::add_media_atom_v,
                                            path, uri, fl, Uuid());
```

### Porting Considerations for RodeoFX

| Aspect | Current (DNEG) | RodeoFX Adaptation |
|--------|----------------|-------------------|
| Socket name | `dnrun-v1-xstudio-{SHOW}-{login}` | `rdorun-v1-xstudio-{SHOW}-{login}` |
| Show env var | `SHOW` | `RDO_CURRENT_SHOW` or `SHOW` |
| Plugin URIs | `ivy://` | Custom RodeoFX URIs if needed |
| Poll intervals | 100ms / 5000ms | Keep same or adjust |

### Example Client (Python)

```python
import socket
import json
import os

def send_to_xstudio(paths, quickview=False, compare=None):
    """Send media load request to xStudio via DNRun socket."""
    show = os.environ.get('SHOW', 'default')
    login = os.getlogin()
    socket_name = f'\0dnrun-v1-xstudio-{show}-{login}'

    request = {
        "args": {
            "paths": paths,
            "quickview": quickview
        }
    }
    if compare:
        request["args"]["compare"] = compare

    payload = json.dumps(request)
    netstring = f"{len(payload)}:{payload},"

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(socket_name)
    sock.sendall(netstring.encode())
    sock.close()

# Usage
send_to_xstudio(["/path/to/render.exr"], quickview=True)
send_to_xstudio(["/path/a.exr", "/path/b.exr"], compare="ab")
```

---

## Conform ShotBrowser Plugin

**Source:** `src/plugin/conform/dneg/conform_shotbrowser/src/conform_shotbrowser.cpp`

### Overview

The Conform ShotBrowser plugin provides timeline conforming capabilities by matching media to clips based on Shotgun metadata. It creates "conform tracks" on timelines and populates them with clips that can be matched to media.

### Architecture

```
┌─────────────────┐
│    Timeline     │
│   (OTIO/EDL)    │
└────────┬────────┘
         │
         ▼
┌─────────────────┐     Query      ┌─────────────────┐
│    Conform      │ ──────────────▶│   ShotBrowser   │
│   ShotBrowser   │                │   Data Source   │
└────────┬────────┘                └────────┬────────┘
         │                                  │
         │                                  ▼
         │                         ┌─────────────────┐
         │                         │    ShotGrid     │
         │                         │      API        │
         │                         └─────────────────┘
         ▼
┌─────────────────┐
│  Conform Track  │
│  (clips matched │
│   to media)     │
└─────────────────┘
```

### Preferences

| Setting | Path | Default | Description |
|---------|------|---------|-------------|
| Purge on Import | `/plugin/conformer/shotbrowser/purge_sequence_on_import` | `true` | Delete empty tracks when importing timeline |
| Reuse Media | `/plugin/conformer/shotbrowser/reuse_media` | `true` | Reuse existing media instead of re-importing |

### Conform Tasks

Tasks are loaded from ShotBrowser presets with specific flags:
- `Replace` - Replace existing media
- `Conform` - Match media to clips
- `Compare` - Compare media versions

Tasks are filtered by:
- Not hidden
- Marked as favourite
- Has appropriate flag

### Key Operations

#### 1. Prepare Timeline (`prepare_timeline`)

Creates a conform track from an imported timeline:

```
1. Extract project from timeline path (/jobs/{PROJECT}/...)
2. Purge empty video/audio tracks (optional)
3. Clone top video track as "Conform Track"
4. For each clip, extract metadata from:
   a. Clip stalk UUID (FEAT ANIM workflow)
   b. Turnover markers (auto markers)
   c. Premiere markers (FCP XML)
   d. Media metadata (fallback)
5. Set clip colors:
   - Green (#FF00FF00) = valid metadata
   - Red (#FFFF0000) = invalid/missing metadata
6. Lock conform track
7. Wait for timeline sync
```

#### 2. Conform Request (`conform_request`)

Matches media to clips based on project/shot:

```
1. Build clip lookup maps (project, shot, meta_shot)
2. For each media item:
   a. Extract project/shot from metadata
   b. Find matching clips by project + shot + meta_shot
   c. Return matched clip UUIDs
3. Apply frame range overrides from cut metadata
```

#### 3. Conform Task Request (`conform_task_request`)

Executes a preset-based conform task via ShotGrid:

```
1. Get query ID from task name
2. For each clip:
   a. Build ShotGrid query with clip metadata
   b. Execute preset query via ShotBrowser
   c. If reuse_media enabled:
      - Check if media already exists (by Ivy UUID)
      - Reuse existing media actors
   d. Add new media to container
3. Return matched media for each clip
```

#### 4. Conform Media (`conform_media`)

Finds sequence files (OTIO) for media:

```
1. Get "View In Sequence" query from presets
2. For each media:
   a. Query ShotGrid for sequence version
   b. Query Ivy for version files
   c. Find OTIO file from preferred sequence sources
   d. Return (name, uri, metadata) tuple
```

### Metadata JSON Pointers

| Pointer | Description |
|---------|-------------|
| `/metadata/shotgun/version/attributes/sg_ivy_dnuuid` | Ivy UUID for media matching |
| `/metadata/shotgun/version/relationships/entity/data/type` | Entity type (Shot/Sequence) |
| `/metadata/shotgun/version/attributes/sg_twig_type_code` | Twig type (cut/edl) |
| `/metadata/shotgun/version/attributes/sg_pipe_tag_3` | Fake shot for meta matching |
| `/metadata/external/DNeg/cut/start` | Cut start frame |
| `/metadata/external/DNeg/cut/end` | Cut end frame |
| `/metadata/external/DNeg/comp/start` | Comp start frame |
| `/metadata/external/DNeg/comp/end` | Comp end frame |
| `/metadata/external/DNeg/cut/override` | Apply cut frame override |
| `/metadata/external/DNeg/comp/override` | Apply comp frame override |

### Marker Processing

The plugin processes three types of markers:

#### 1. DNEG Turnover Markers
```json
{
  "show": "PROJECT",
  "shot": "SHOT_NAME",
  "cut": {"start": 1001, "end": 1100},
  "comp": {"start": 1001, "end": 1100}
}
```

#### 2. FCP XML Markers (Premiere)
```json
{
  "fcp_xml": {
    "comment": "1001, 1001-1100, 1100"
  }
}
```
Comment format: `comp_start, cut_start-cut_end, comp_end`

#### 3. Clip Stalk UUID (FEAT ANIM)
```json
{
  "media_stalk_dnuuid": "uuid-here",
  "shot_label": "SHOT_NAME"
}
```

### Clip Color Coding

| Color | Hex | Meaning |
|-------|-----|---------|
| Green | `#FF00FF00` | Valid metadata, ready for conform |
| Red | `#FFFF0000` | Missing/invalid metadata |

### Porting Considerations for RodeoFX

| Aspect | Current (DNEG) | RodeoFX Adaptation |
|--------|----------------|-------------------|
| Project regex | `/jobs/{PROJECT}/` or `/hosts/.../user_data/{PROJECT}/` | `/rdo/shows/{PROJECT}/` |
| Metadata paths | `/metadata/external/DNeg/...` | `/metadata/external/RodeoFX/...` |
| Ivy integration | Required for OTIO lookup | Replace with RodeoFX asset system |
| Shot name validation | `^[a-zA-Z0-9_]+$` | Adjust for RodeoFX naming conventions |
| Preset flags | `Replace`, `Conform`, `Compare`, `View In Sequence` | Keep or customize |

### Dependencies

The Conform ShotBrowser plugin requires:

1. **ShotBrowser Data Source** - For ShotGrid queries and preset management
2. **Ivy Data Source** - For OTIO file lookup (optional for RodeoFX)
3. **Timeline Actor** - For track manipulation
4. **Query Engine** - For metadata extraction (`QueryEngine::get_project_name`, `QueryEngine::get_shot_name`)

### Query Engine Integration

The plugin uses `QueryEngine` from ShotBrowser for metadata extraction:

```cpp
#include "../../../../data_source/dneg/shotbrowser/src/query_engine.hpp"

// Extract project name from metadata
auto project = QueryEngine::get_project_name(metadata);

// Extract shot name from metadata
auto shot = QueryEngine::get_shot_name(metadata);

// Get project ID for ShotGrid queries
auto project_id = QueryEngine::get_project_id(metadata, JsonStore());
```

---

## Summary: Porting Priority

### DNRun - Easy Port

**Effort:** Low (1-2 days)

**Changes Required:**
1. Rename socket namespace
2. Update environment variable names
3. Remove/replace plugin URI handling if not needed
4. Update logging/error messages

**Value:** High - Immediate DCC integration

### Conform ShotBrowser - Medium Port

**Effort:** Medium (1-2 weeks)

**Changes Required:**
1. Port ShotBrowser first (dependency)
2. Update all metadata JSON pointers for RodeoFX schema
3. Replace Ivy with RodeoFX asset system
4. Update project/shot extraction regex patterns
5. Customize marker processing for RodeoFX workflows
6. Update preset flags and task definitions

**Value:** High for editorial workflows, Medium otherwise
