# colour_settings.yaml Format

This document describes the `colour_settings.yaml` file format used by the DNEG media hook plugin to configure xStudio's OCIO display and view filtering.

## File Location

```
/tools/{SHOW}/data/colsci/colour_settings.yaml
```

On Windows:
```
N:\{SHOW}\data\colsci\colour_settings.yaml
```

## File Structure

The file is a standard YAML file. The DNEG media hook specifically parses the `xstudio:` section.

```yaml
# colour_settings.yaml
# Show-specific colour pipeline configuration

# Other application sections (ignored by xStudio)
nuke:
    default_colorspace: "scene_linear"

maya:
    rendering_space: "scene_linear"

# xStudio configuration section
xstudio:
    active_displays: "Rec709:sRGB:DCI-P3"
    active_views: "Client:Client graded:DNEG:Film"
```

## Parsing Rules

The DNEG media hook uses a simple line-based parser (not a full YAML parser):

| Rule | Description |
|------|-------------|
| Section detection | Lines starting with `xstudio:` begin the xStudio section |
| Indentation | Settings must use exactly 4 spaces (not tabs) |
| Key-value format | `key: value` with colon separator |
| Quotes | Optional surrounding quotes are stripped from values |
| Comments | Lines starting with `#` are ignored |
| Empty lines | Ignored |
| Section end | Any non-indented line after `xstudio:` ends the section |

## Supported Settings

### active_displays

Colon-separated list of OCIO display names to show in the xStudio UI.

```yaml
xstudio:
    active_displays: "Rec709:sRGB:DCI-P3:EIZO:HDR:Playback"
```

**Fallback:** If not set, falls back to `DN_REVIEW_XSTUDIO_OCIO_ACTIVE_DISPLAYS` environment variable.

### active_views

Colon-separated list of OCIO view names to show in the xStudio UI.

```yaml
xstudio:
    active_views: "Client:Client graded:DNEG:Film:Film primary:Un-tone-mapped:Raw"
```

**Fallback:** If not set, falls back to `DN_REVIEW_XSTUDIO_OCIO_ACTIVE_VIEWS` environment variable.

## Example Files

### Minimal Configuration

```yaml
xstudio:
    active_displays: "Rec709:sRGB"
    active_views: "Client:DNEG"
```

### Production Configuration

```yaml
# colour_settings.yaml
# Show: MYSHOW
# Pipeline Version: 2.1
# Last Updated: 2024-01-15

# Version indicator for other tools
version: 2

# Nuke configuration (not used by xStudio)
nuke:
    ocio_config: "/tools/MYSHOW/data/colsci/config_ocio-v2.2.ocio"
    default_colorspace: "scene_linear"
    viewer_process: "Client"

# Maya configuration (not used by xStudio)
maya:
    rendering_space: "scene_linear"
    texture_colorspace: "Utility - sRGB - Texture"

# xStudio viewer configuration
xstudio:
    # Calibrated displays available at this facility
    active_displays: "Rec709:sRGB:EIZO247X:EIZO279X:Playback:DCI-P3:HDR"

    # Production views - hide technical/debug views
    active_views: "Client:Client graded:DNEG:Film:Film primary:Un-tone-mapped:Raw"
```

### HDR Show Configuration

```yaml
# colour_settings.yaml
# HDR-enabled show configuration

xstudio:
    # Include HDR displays
    active_displays: "Rec709:sRGB:HDR:DCI-P3:Playback"

    # Include HDR-specific views
    active_views: "Client:Client graded:HDR:DNEG:Film:Film primary:Un-tone-mapped"
```

### Minimal Artist Desktop

```yaml
# colour_settings.yaml
# Simplified for artist desktops with sRGB monitors only

xstudio:
    active_displays: "sRGB:Rec709"
    active_views: "Client:DNEG:Raw"
```

## Priority Order

Settings are resolved in this order (later overrides earlier):

1. **Environment variable** (lowest priority)
   - `DN_REVIEW_XSTUDIO_OCIO_ACTIVE_DISPLAYS`
   - `DN_REVIEW_XSTUDIO_OCIO_ACTIVE_VIEWS`

2. **colour_settings.yaml** (highest priority)
   - `active_displays`
   - `active_views`

## Code Reference

The parsing is implemented in `dneg.cpp`:

```cpp
// File: src/plugin/media_hook/dneg/dnhook/src/dneg.cpp
// Function: read_colour_settings_yaml (lines 853-905)

std::map<std::string, std::string> read_colour_settings_yaml(const std::string &show) {
    std::map<std::string, std::string> variables;

    std::ifstream ifs(fmt::format("/tools/{}/data/colsci/colour_settings.yaml", show));
    if (!ifs.is_open())
        return {};

    bool xstudio_section = false;
    const std::string lines(std::istreambuf_iterator<char>{ifs}, {});

    for (const auto &line : utility::split(lines, '\n')) {
        if (line.empty() or utility::starts_with(line, "#"))
            continue;

        if (xstudio_section) {
            // Assume 4 space indentation
            if (utility::starts_with(line, "    ")) {
                const auto pos = line.find(":");
                if (pos != std::string::npos) {
                    const auto key = utility::trim(line.substr(0, pos));
                    auto value     = utility::trim(line.substr(pos + 1));

                    // Trim surrounding quotes
                    if (value[0] == '"')
                        value = value.substr(1, value.length() - 1);
                    if (value[value.length() - 1] == '"')
                        value = value.substr(0, value.length() - 1);

                    variables[key] = value;
                }
            } else {
                break;  // End of xstudio section
            }
        }

        if (utility::starts_with(line, "xstudio:")) {
            xstudio_section = true;
        }
    }

    return variables;
}
```

Settings are used in `colour_params` (lines 486-499):

```cpp
std::string active_displays =
    get_showvar_or(context["SHOW"], "DN_REVIEW_XSTUDIO_OCIO_ACTIVE_DISPLAYS", "");
active_displays =
    get_showsetting_or(context["SHOW"], "active_displays", active_displays);
if (!active_displays.empty()) {
    r["active_displays"] = active_displays;
}

std::string active_views =
    get_showvar_or(context["SHOW"], "DN_REVIEW_XSTUDIO_OCIO_ACTIVE_VIEWS", "");
active_views = get_showsetting_or(context["SHOW"], "active_views", active_views);
if (!active_views.empty()) {
    r["active_views"] = active_views;
}
```

## RodeoFX Adaptation

For RodeoFX, the file location and settings could be adapted:

```yaml
# /rdo/shows/{show}/_project_ref/_imaging/colour_settings.yaml

xstudio:
    active_displays: "sRGB:Rec709:DCI-P3"
    active_views: "Client-look:Client-look (non-wb):Neutral-look:Raw"
```

The RodeoFX media hook would need to implement similar parsing logic to read these settings.
