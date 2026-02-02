# DNEG Plugins Analysis for RodeoFX Pipeline

This document analyzes the DNEG plugins available in xStudio and evaluates their potential for porting to the RodeoFX pipeline.

---

## Available DNEG Plugins

### 1. Ivy Data Source

**Location:** `src/plugin/data_source/dneg/ivy/`

**Type:** Data Source

**Description:**
Integrates xStudio with DNEG's Ivy asset management system via GraphQL queries to the pipequery service.

**Key Features:**
- `ivy://load` URI handling for loading versions and files
- Automatic discovery of file variants ("leaves") for a version
- Audio auto-loading with configurable default audio source selection
- Shotgun metadata enrichment when available
- Multi-source support for decomposing PSREFs into separate render passes
- Worker pool with 5 threads for concurrent media source creation
- Python API via `xstudio_pipequery` module for custom GraphQL queries

**Technical Details:**
- Uses HTTP POST to pipequery service for GraphQL queries
- Stores metadata at `/metadata/ivy/` and `/metadata/shotgun/` paths
- Extracts show and stalk UUIDs from file paths and metadata

---

### 2. ShotBrowser Data Source

**Location:** `src/plugin/data_source/dneg/shotbrowser/`

**Type:** Data Source

**Description:**
Comprehensive Shotgun (ShotGrid) integration providing query engine capabilities, preset management, and media population.

**Key Features:**
- Advanced query engine for complex Shotgun data retrieval
- Preset system with hierarchical tree structure, favorites, and visibility flags
- Action processing (GET, POST, PUT) against Shotgun API
- UI models for displaying query results
- Project/asset/shot/sequence/episode/playlist browsing
- Version querying with customizable field selection
- Playlist creation, reordering, and management
- Notes and annotations handling
- Tag management (create, rename, attach)
- Artist and pipeline step information retrieval
- Media caching and downloading from Shotgun
- Integrated authentication (OAuth, session token, credentials)
- Worker pool with 8 threads for concurrent task processing
- History tracking and event broadcasting

---

### 3. DNRun Utility

**Location:** `src/plugin/utility/dneg/dnrun/`

**Type:** Utility

**Description:**
External request handler that enables other applications to send media loading requests to xStudio via Unix domain sockets using the SwiftWind protocol.

**Key Features:**
- Unix domain socket server with abstract namespace (`dnrun-v1-{xstudio}-{SHOW}-{login}`)
- Netstring protocol for message framing (length:payload,)
- Request queue management for multiple clients
- JSON request parsing for media paths and options
- Multi-path loading in single request
- Quick view support for immediate playback
- A/B compare mode for media comparison
- Plugin URI routing to plugin manager
- Automatic media selection after loading
- Up to 10 concurrent connections with poll-based processing

---

### 4. Conform ShotBrowser

**Location:** `src/plugin/conform/dneg/conform_shotbrowser/`

**Type:** Conformer

**Description:**
Timeline conforming plugin that matches media to clips based on Shotgun metadata and enables complex editorial workflows.

**Key Features:**
- Automatic conform track creation on timelines
- Metadata-based matching (project/shot/twig)
- Clip flagging with color coding (red=invalid, green=valid)
- Preset-based conform task execution via Shotgun queries
- Sequence detection from OTIO/EDL files via Ivy
- Optional media reuse to avoid duplicate imports
- Marker processing (FCP XML, DNEG turnover, generic)
- Frame range management (cut/comp ranges from metadata)
- Intelligent shot name extraction from multiple sources
- Automatic empty track purging on import
- Project detection from timeline file paths

---

## Porting Recommendations

### Summary Table

| Plugin | Interest | Effort | Value | Recommendation |
|--------|----------|--------|-------|----------------|
| **DNRun** | High | Low | High | Port first |
| **ShotBrowser** | High | Medium | High | Port if using ShotGrid |
| **Conform ShotBrowser** | Medium | Medium | Medium | Port for editorial workflows |
| **Ivy** | Low | High | Low | Skip - too DNEG-specific |

---

### 1. DNRun Utility - **Recommended**

**Interest Level:** ⭐⭐⭐ High

**Rationale:**
- Pipeline-agnostic architecture - works with any tool that can write to a Unix socket
- Minimal dependencies on DNEG-specific systems
- Enables integration with DCCs (Nuke, Maya, Houdini, etc.)
- Simple JSON protocol easy to implement in Python/C++

**Porting Effort:** Low
- Rename socket namespace to RodeoFX convention
- Update show detection logic for RodeoFX paths
- Remove DNEG-specific URI handling

**Example Use Cases:**
- Load media from Nuke with a single Python call
- Quick review from Maya asset browser
- A/B comparison triggered from pipeline tools

---

### 2. ShotBrowser Data Source - **Recommended if using ShotGrid**

**Interest Level:** ⭐⭐⭐ High

**Rationale:**
- Full ShotGrid browsing UI out of the box
- Query engine and preset system highly reusable
- Playlist management integrates with ShotGrid reviews
- Tag and notes handling useful for review workflows

**Porting Effort:** Medium
- Customize query presets for RodeoFX entity structure
- Update authentication flow if different
- Modify field mappings for RodeoFX-specific Shotgun fields
- Update media path resolution for RodeoFX storage

**Prerequisites:**
- RodeoFX must be using ShotGrid/Shotgun
- Understanding of RodeoFX's Shotgun schema

---

### 3. Conform ShotBrowser - **Consider for Editorial**

**Interest Level:** ⭐⭐ Medium

**Rationale:**
- Valuable for editorial/conform workflows
- OTIO/EDL support is pipeline-agnostic
- Metadata matching can be adapted to any schema

**Porting Effort:** Medium
- Depends on ShotBrowser being ported first
- Update metadata field mappings
- Customize shot name extraction patterns
- Adapt frame range handling to RodeoFX conventions

**Prerequisites:**
- ShotBrowser plugin ported
- Editorial workflows using OTIO or EDL

---

### 4. Ivy Data Source - **Not Recommended**

**Interest Level:** ⭐ Low

**Rationale:**
- Tightly coupled to DNEG's Ivy asset management system
- GraphQL schema specific to Ivy backend
- Would require complete rewrite for different asset manager

**Porting Effort:** High
- Essentially building a new plugin from scratch
- Only reusable patterns are the worker pool and URI handling

**Alternative:**
If RodeoFX has an asset management system, build a new data source plugin using Ivy as a reference architecture rather than porting it directly.

---

## Recommended Porting Order

1. **DNRun** - Quick win, immediate value for DCC integration
2. **ShotBrowser** - Core ShotGrid integration (if applicable)
3. **Conform ShotBrowser** - Editorial workflow support (if needed)

---

## Technical Considerations

### Socket Protocol (DNRun)

The netstring protocol used by DNRun is simple:
```
{length}:{json_payload},
```

Example request:
```json
{
  "paths": ["/rdo/shows/myshow/seq/shot/render.exr"],
  "quick_view": true,
  "compare": false
}
```

### ShotGrid Integration

ShotBrowser uses the Shotgun REST API with these endpoints:
- Entity queries (shots, versions, playlists)
- Media downloads
- Note creation
- Tag management

Authentication methods supported:
- OAuth 2.0
- Session tokens
- Username/password credentials

### Plugin Architecture

All plugins follow the xStudio plugin architecture:
- Factory registration via `plugin_factory_collection_ptr()`
- Actor-based message handling (CAF framework)
- Preference storage via JSON paths
- Event broadcasting for UI updates
