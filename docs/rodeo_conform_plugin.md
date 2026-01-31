# Rodeo Conform Plugin

The Rodeo Conform Plugin provides automatic conforming functionality for RodeoFX timelines in xStudio. It matches media to timeline clips based on show and shot codes, enabling quick population of tracks with the correct versions from ShotGrid.

## Quick Start: Step-by-Step Workflow

This walkthrough shows the complete flow from importing an edit to having a fully conformed timeline.

### Step 1: Import Your Edit

1. **File → Open** or drag-and-drop your edit file (EDL, XML, or OTIO)
2. The timeline appears in the viewer with the imported clips
3. The bottom video track becomes your "conform track" - this defines the shot structure

### Step 2: Verify the Conform Track

1. Look at the bottom video track (V1)
2. Each clip should show the shot code in its name (e.g., "206044_0010")
3. **Green clips** = valid shot metadata found
4. **Red clips** = shot metadata missing or invalid (labeled "UNKNOWN")

If you see red clips, check that:
- The source media has shot information in the filename or metadata
- The clip name matches the expected pattern (SEQ_SHOT)

### Step 3: Create Empty Tracks for Departments

1. **Right-click on track header area** → Add Video Track
2. Name the track based on what you want to conform:
   - `Comp` - Latest Comp versions
   - `Comp: Delivered` - Only delivered Comp versions
   - `Anim: Pushed` - Pushed Animation versions
3. Repeat for each department you need

**Example track stack (top to bottom):**
```
V4: Comp: Delivered
V3: Comp
V2: Anim: Pushed
V1: [Conform Track - your imported edit]
```

### Step 4: Run Conform

**Option A: Using Replace Menu (C++ Plugin)**
1. Select media in the playlist or clips in the timeline
2. Right-click → **Replace → Rodeo: [Preset Name]** (e.g., "Rodeo: Comp: Latest")
3. The selected items are replaced/conformed with matching versions

**Option B: Using Auto-Conform Menu (Python Plugin)**
1. Right-click anywhere in the timeline
2. Select **Auto-Conform → [Preset Name]** (e.g., "Comp: Latest")
3. A new track is created and populated with matching versions

**Option C: Conform all tracks by name (Python Plugin)**
1. Create empty tracks with descriptive names (see Step 3)
2. Right-click anywhere in the timeline
3. Select **Auto-Conform → From Track Names**
4. All empty tracks are populated based on their names

> **Note:** Tasks prefixed with "Rodeo:" use the C++ conform plugin. Tasks without the prefix use the Python plugin.

### Step 5: Review Results

1. Scrub through the timeline to check the conformed tracks
2. Clips show the version name (e.g., "206044_0010.comp.v007")
3. Gaps appear where no matching version was found
4. Use Compare mode to check versions against the offline

### Complete Example

```
Timeline: drb_206044_cut_context_v01

Before Auto-Conform:
┌─────────────────────────────────────────────────┐
│ V3: Comp: Delivered  [empty]                    │
│ V2: Comp             [empty]                    │
│ V1: 206044_0010 | 206044_0020 | 206044_0030     │  ← Conform track
└─────────────────────────────────────────────────┘

After "Auto-Conform → From Track Names":
┌─────────────────────────────────────────────────┐
│ V3: Comp: Delivered  .comp.v005 | .comp.v003 | [gap]     │
│ V2: Comp             .comp.v007 | .comp.v006 | .comp.v002│
│ V1: 206044_0010 | 206044_0020 | 206044_0030     │
└─────────────────────────────────────────────────┘
```

---

## Features

- **Auto-Conform from Presets** - Populate tracks using predefined department/status combinations
- **Track Name Parsing** - Automatically determine conform parameters from track names like "Comp: Latest"
- **Media-to-Clip Matching** - Match media to clips based on project code and shot code
- **Timeline Preparation** - Create and configure conform tracks from existing timeline structure
- **Media Reuse** - Optionally reuse existing media in the playlist to avoid duplicates

## How It Works

### Conform Track

The conform plugin uses a "conform track" as the reference for shot structure and timing. This is typically:

1. A track marked with the `is_conform_track` metadata flag
2. The track directly above a track named "Offline"
3. The bottom video track (V1)

The conform track contains clips that define the shot boundaries and timing. Each clip should have shot metadata (either from import or set manually).

### Conforming Process

1. **Extract Shot Structure** - Read clips from the conform track to get shot codes and frame ranges
2. **Query ShotGrid** - Find versions matching the shot codes, filtered by department and status
3. **Create/Populate Track** - Add media to the playlist and create clips on the target track
4. **Set Metadata** - Apply ShotGrid metadata to clips for future operations

## Using Auto-Conform

### From Presets

Access auto-conform from the timeline context menu or main menu:

- **Right-click in timeline** → Auto-Conform → [Preset Name]
- **Rodeo FX menu** → Auto-Conform → [Preset Name]

Common presets include:
- **Comp: Latest** - Latest Comp department versions (any status)
- **Comp: Delivered** - Comp versions with delivered status (dlvr, cfin)
- **Anim: Latest** - Latest Animation department versions
- **Anim: Pushed** - Animation versions with pushed status (push, apr)

### From Track Names

Create empty tracks with descriptive names, then use **Auto-Conform → From Track Names** to populate all of them at once.

**Supported track name formats:**

| Track Name | Department | Status Filter |
|------------|------------|---------------|
| `Comp` | Comp | Any |
| `Anim` | Anim | Any |
| `Delivered` | Any | dlvr, cfin |
| `Comp: Latest` | Comp | Any |
| `Comp: Delivered` | Comp | dlvr, cfin |
| `Anim: Pushed` | Anim | push, apr |
| `Lighting: Pending` | Lighting | pdlvr |

**Recognized departments:** Comp, Anim, Lighting, FX, Layout, Env, CFX, CMM, MP

**Status keywords:**
- `Latest` → All statuses
- `Delivered` → dlvr, cfin
- `Pending` or `Pending Delivery` → pdlvr
- `Pushed` → push, apr
- `Approved` → apr

## Timeline Setup

### Preparing a Timeline for Conform

1. Import your edit (EDL, XML, or OTIO)
2. Ensure the bottom track contains clips with shot information
3. The plugin will automatically extract shot codes from:
   - Clip metadata (`/shotgrid/shot_code`)
   - Media metadata (`/metadata/external/RodeoFX/shot`)
   - Clip names matching pattern `SEQ_SHOT` (e.g., `206044_0010`)

### Creating a Conform Track

If your timeline doesn't have proper shot metadata, you can prepare it:

1. Import the timeline
2. The plugin will analyze clips and markers to extract shot information
3. Valid shots are marked green, invalid shots are marked red
4. Review and fix any red clips before conforming

## Preferences

Access preferences via **Edit → Preferences → RodeoFX**

| Setting | Description | Default |
|---------|-------------|---------|
| **Reuse Media** | Reuse existing media in playlist when conforming (avoids duplicates) | On |
| **Purge Sequence on Import** | Remove extra tracks when importing turnover sequences | Off |

## Metadata

The plugin reads and writes the following metadata paths:

### Read (for matching)
- `/shotgrid/shot_code` - Shot code (e.g., "206044_0010")
- `/shotgrid/project_code` - Project/show code (e.g., "drb")
- `/metadata/external/RodeoFX/show` - Show from media hook
- `/metadata/external/RodeoFX/shot` - Shot from media hook

### Written (to clips)
- `/shotgrid/version_id` - ShotGrid version ID
- `/shotgrid/version_name` - Full version code
- `/shotgrid/department` - Department name
- `/shotgrid/status` - Version status
- `/colour_pipeline/ocio_context/show_code` - For OCIO context
- `/colour_pipeline/ocio_context/shot_code` - For OCIO context

## Tips

1. **Check the conform track first** - Make sure shot codes are correctly identified (green clips) before conforming

2. **Use track names for batch conform** - Create multiple empty tracks with names like "Comp", "Anim: Pushed", then use "From Track Names" to populate all at once

3. **Enable media reuse** - Keep "Reuse Media" enabled to avoid duplicate media when re-conforming

4. **Timeline naming** - Name your timeline with the show code first (e.g., "drb_206044_cut_v01") to help the plugin identify the project

## Troubleshooting

### "No conform track found"
- Ensure your timeline has at least one video track with clips
- The bottom video track is used by default

### "No shot codes found"
- Check that clips have proper shot metadata
- Clip names should match pattern like `206044_0010`
- Try preparing the timeline first to extract shot info from markers/media

### "No versions found"
- Verify the show code is correct (extracted from timeline name)
- Check that versions exist in ShotGrid for the requested department/status
- Ensure ShotGrid connection is working (RDO Browser must be loaded)

### Clips showing as red (UNKNOWN)
- Shot metadata couldn't be determined
- Check the source media or markers for shot information
- Manually set the shot code in clip properties if needed
