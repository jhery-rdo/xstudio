# Media Hook Feature Comparison: DNEG vs RodeoFX

## Overview

| Feature | DNEG | RodeoFX |
|---------|------|---------|
| **Plugin Author** | DNeg | xStudio (auto-enable) |
| **Source File** | `src/plugin/media_hook/dneg/dnhook/src/dneg.cpp` | `src/plugin/media_hook/rodeo/rdohook/src/rdohook.cpp` |

---

## Configurable Settings

| Setting | DNEG | RodeoFX |
|---------|------|---------|
| Auto Trim Slate | ✅ Based on metadata from `.dneg.mov`/`.dneg.webm` | ✅ Path-based (`/movhistory/` or `/.published/`) |
| Default Trim Behaviour | ✅ "Trim First Frame" / "Don't Trim" | ❌ |
| Adjust Timecode | ✅ Uses `timeline_range` from pipequery | ❌ |
| REZ OCIO Config Root | ❌ | ✅ Configurable path |
| Default Show Code | ❌ | ✅ Fallback show code |

---

## Path Detection

| Feature | DNEG | RodeoFX |
|---------|------|---------|
| Show extraction | `/jobs/{show}/` or `/hosts/.../user_data/{show}/` | `/rdo/shows/{show}/` or `/shows/{show}/` |
| Shot extraction | Regex from path structure | Multiple patterns: `SEQ/SHOT`, `SEQ_SHOT`, filenames |
| Stalk UUID detection | ✅ Scans for `.stalk_*` files | ❌ |

---

## OCIO Configuration

| Feature | DNEG | RodeoFX |
|---------|------|---------|
| Config path | `/tools/{show}/data/colsci/config_ocio-vX.Y.ocio` | `/rdo/shows/{show}/_project_ref/_imaging/_ocio2_xstudio/{show}_config.ocio` |
| Version detection | ✅ Auto-selects highest compatible OCIO version | ❌ Single config |
| Context variables | `SHOW`, `SHOT` | `SEQ`, `SHOT`, `REZ_RDO_OCIO_CONFIG_ROOT`, `RDO_CURRENT_SHOW` |
| Active displays/views | ✅ From showvars and `colour_settings.yaml` | ❌ |
| Pipeline version | ✅ Detects CMS1 vs legacy | ❌ |

---

## Input Colour Space Detection

| Category | DNEG | RodeoFX |
|----------|------|---------|
| Review proxies | `dneg_proxy_log:log` | N/A |
| Internal movies | `DNEG_Rec709` (CMS1) or `Film/Rec709` | `Utility - Raw` (passthrough) |
| Edit reference | `Client_Graded_Rec709:Client_Rec709:DNEG_Rec709` | N/A |
| Linear media (EXR) | `scene_linear:linear` | `scene_linear` (working space) |
| Log media (DPX/CIN) | `compositing_log:log` | N/A |
| Still images | `DNEG_sRGB` or `Film/sRGB` | `Utility - Raw` (passthrough) |

---

## Automatic View Assignment

| Media Type | DNEG | RodeoFX |
|------------|------|---------|
| Assets | `DNEG` | `Neutral-look` |
| Shots (out/ELEMENT) | `Client graded` / `Film primary` | `Client-look` |
| Lineup EXRs | N/A | `Client-look (non-wb)` |
| Movies/stills | `Client` / `Film` | `raw` (passthrough) |

---

## Additional Features

| Feature | DNEG | RodeoFX |
|---------|------|---------|
| Display detection | ✅ EIZO, DELL, KONA, playback room logic | ❌ |
| Viewing rules | ✅ `viewing_rules = true` | ❌ |
| Dynamic CDL | ✅ `$GRD_PRIMARY`, `$GRD_NEUTRAL`, `$GRD_ALT` | ❌ |
| Un-tone-mapped support | ✅ For edit_ref/movie/still media | ❌ |
| Clip metadata modification | ✅ Injects `DNEG_MEDIA_STALK_DNUUID` | ❌ |
| Frame rate inference | ✅ From `DN_FPS` showvar | ❌ |
| Path remapping | ✅ `/user_data` → `/hosts/{hostname}/user_data` | ❌ |

---

## Summary

### DNEG Media Hook

A comprehensive, enterprise-grade media hook with:

- **Deep pipeline integration** with DNEG's Ivy/Shotgun systems
- **Sophisticated OCIO version detection** - auto-selects highest compatible config version
- **Display detection rules** - automatic display selection for EIZO, DELL monitors, KONA cards, and playback rooms
- **Metadata-driven slate detection** - reads `slateFrames` from movie comments
- **Dynamic CDL support** for grading workflows (`GRD_primary`, `GRD_neutral`, `GRD_alt`)
- **Un-tone-mapped view support** for display-linear workflow media
- **Timeline range adjustment** from pipequery metadata

### RodeoFX Media Hook

A leaner, path-based approach:

- **Simple path pattern matching** for SEQ/SHOT extraction
- **MOV/stills treated as passthrough** (raw colorspace)
- **Automatic view selection** based on media type:
  - Lineup EXRs → `Client-look (non-wb)`
  - Asset EXRs → `Neutral-look`
  - Shot EXRs → `Client-look`
- **Environment variable fallbacks** for context (`SEQ`, `SHOT`, `RDO_CURRENT_SHOW`)
- **Configurable REZ OCIO root** for flexible deployment
