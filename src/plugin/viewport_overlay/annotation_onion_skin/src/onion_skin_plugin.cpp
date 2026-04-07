// SPDX-License-Identifier: Apache-2.0
#include "onion_skin_plugin.hpp"
#include "onion_skin_render_data.hpp"
#include "onion_skin_renderer.hpp"
#include "xstudio/media_reader/image_buffer.hpp"
#include "xstudio/bookmark/bookmark.hpp"
#include "xstudio/plugin_manager/plugin_base.hpp"
#include "xstudio/utility/blind_data.hpp"

#include <algorithm>
#include <cmath>

using namespace xstudio;
using namespace xstudio::ui::viewport;

OnionSkinPlugin::OnionSkinPlugin(
    caf::actor_config &cfg, const utility::JsonStore &init_settings)
    : plugin::HUDPluginBase(cfg, "Annotation Onion Skin", init_settings, 10.0f) {

    frames_before_ = add_integer_attribute("Frames Before", "Before", 2, 0, 5);
    add_hud_settings_attribute(frames_before_);
    frames_before_->set_tool_tip(
        "Number of previous annotated frames to show as onion skins");
    frames_before_->set_redraw_viewport_on_change(true);

    frames_after_ = add_integer_attribute("Frames After", "After", 2, 0, 5);
    add_hud_settings_attribute(frames_after_);
    frames_after_->set_tool_tip("Number of future annotated frames to show as onion skins");
    frames_after_->set_redraw_viewport_on_change(true);

    base_opacity_ =
        add_float_attribute("Base Opacity", "Opacity", 0.4f, 0.05f, 1.0f, 0.05f);
    add_hud_settings_attribute(base_opacity_);
    base_opacity_->set_tool_tip("Opacity of the nearest neighboring annotation");
    base_opacity_->set_redraw_viewport_on_change(true);

    opacity_falloff_ =
        add_float_attribute("Opacity Falloff", "Falloff", 0.5f, 0.1f, 1.0f, 0.05f);
    add_hud_settings_attribute(opacity_falloff_);
    opacity_falloff_->set_tool_tip(
        "Multiplier applied per frame step further from current frame");
    opacity_falloff_->set_redraw_viewport_on_change(true);

    past_tint_ = add_colour_attribute(
        "Previous Tint", "Prev Tint", utility::ColourTriplet(1.0f, 0.3f, 0.3f));
    add_hud_settings_attribute(past_tint_);
    past_tint_->set_tool_tip("Tint colour for annotations from previous frames");
    past_tint_->set_redraw_viewport_on_change(true);

    future_tint_ = add_colour_attribute(
        "Next Tint", "Next Tint", utility::ColourTriplet(0.3f, 1.0f, 0.3f));
    add_hud_settings_attribute(future_tint_);
    future_tint_->set_tool_tip("Tint colour for annotations from future frames");
    future_tint_->set_redraw_viewport_on_change(true);

    add_hud_description(
        "Shows annotations from neighboring frames as semi-transparent, "
        "color-tinted overlays on the current frame.");

    frames_before_->set_preference_path("/plugin/annotation_onion_skin/frames_before");
    frames_after_->set_preference_path("/plugin/annotation_onion_skin/frames_after");
    base_opacity_->set_preference_path("/plugin/annotation_onion_skin/base_opacity");
    opacity_falloff_->set_preference_path("/plugin/annotation_onion_skin/opacity_falloff");
    past_tint_->set_preference_path("/plugin/annotation_onion_skin/past_tint");
    future_tint_->set_preference_path("/plugin/annotation_onion_skin/future_tint");
}

plugin::ViewportOverlayRendererPtr
OnionSkinPlugin::make_overlay_renderer(const std::string & /*viewport_name*/) {
    return plugin::ViewportOverlayRendererPtr(new OnionSkinRenderer());
}

utility::BlindDataObjectPtr OnionSkinPlugin::onscreen_render_data(
    const media_reader::ImageBufPtr &image,
    const std::string & /*viewport_name*/,
    const utility::Uuid & /*playhead_uuid*/,
    const bool /*is_hero_image*/,
    const bool /*images_are_in_grid_layout*/) const {

    if (!visible() || !image)
        return {};

    const int current_frame  = image.playhead_logical_frame();
    const int want_before    = static_cast<int>(frames_before_->value());
    const int want_after     = static_cast<int>(frames_after_->value());
    const float base_opac    = base_opacity_->value();
    const float falloff      = opacity_falloff_->value();
    const auto &prev_colour  = past_tint_->value();
    const auto &next_colour  = future_tint_->value();

    if (want_before == 0 && want_after == 0)
        return {};

    const auto &all_bookmarks = image.all_timeline_bookmarks();
    if (all_bookmarks.empty())
        return {};

    // Collect annotated frames from the full timeline bookmark set.
    // Bookmarks are sorted by start_frame_ (done by SubPlayhead).
    // We walk backward and forward from the current frame position to find
    // the nearest annotated neighbor frames.

    std::vector<NeighborAnnotation> neighbors;

    // Helper: compute opacity for a given distance from current frame
    auto compute_opacity = [&](int distance) -> float {
        return base_opac * std::pow(falloff, static_cast<float>(distance - 1));
    };

    // Find past annotations (walk backward)
    if (want_before > 0) {
        int found = 0;
        // Iterate backward through bookmarks to find annotated frames before current
        for (auto it = all_bookmarks.rbegin(); it != all_bookmarks.rend() && found < want_before;
             ++it) {
            const auto &bm = *it;
            if (!bm || !bm->annotation_ || !bm->annotation_->user_data())
                continue;
            // This bookmark's frame range must be entirely before the current frame
            if (bm->end_frame_ >= current_frame)
                continue;

            // Access the Canvas via the public user_data() API
            const auto *canvas =
                static_cast<const ui::canvas::Canvas *>(bm->annotation_->user_data());
            if (!canvas || canvas->empty())
                continue;

            int offset = bm->end_frame_ - current_frame; // negative
            found++;

            NeighborAnnotation na;
            na.canvas       = *canvas;
            na.frame_offset = offset;
            na.opacity      = compute_opacity(found);
            na.tint = Imath::V3f(prev_colour.r, prev_colour.g, prev_colour.b);
            neighbors.push_back(std::move(na));
        }
    }

    // Find future annotations (walk forward)
    if (want_after > 0) {
        int found = 0;
        for (auto it = all_bookmarks.begin(); it != all_bookmarks.end() && found < want_after;
             ++it) {
            const auto &bm = *it;
            if (!bm || !bm->annotation_ || !bm->annotation_->user_data())
                continue;
            // This bookmark's frame range must be entirely after the current frame
            if (bm->start_frame_ <= current_frame)
                continue;

            const auto *canvas =
                static_cast<const ui::canvas::Canvas *>(bm->annotation_->user_data());
            if (!canvas || canvas->empty())
                continue;

            int offset = bm->start_frame_ - current_frame; // positive
            found++;

            NeighborAnnotation na;
            na.canvas       = *canvas;
            na.frame_offset = offset;
            na.opacity      = compute_opacity(found);
            na.tint = Imath::V3f(next_colour.r, next_colour.g, next_colour.b);
            neighbors.push_back(std::move(na));
        }
    }

    if (neighbors.empty())
        return {};

    // Sort farthest-to-nearest so nearest draws on top
    std::sort(neighbors.begin(), neighbors.end(), [](const auto &a, const auto &b) {
        return std::abs(a.frame_offset) > std::abs(b.frame_offset);
    });

    return std::make_shared<OnionSkinRenderData>(std::move(neighbors));
}


extern "C" {
plugin_manager::PluginFactoryCollection *plugin_factory_collection_ptr() {
    return new plugin_manager::PluginFactoryCollection(
        std::vector<std::shared_ptr<plugin_manager::PluginFactory>>(
            {std::make_shared<plugin_manager::PluginFactoryTemplate<OnionSkinPlugin>>(
                OnionSkinPlugin::PLUGIN_UUID,
                "AnnotationOnionSkin",
                plugin_manager::PluginFlags::PF_HEAD_UP_DISPLAY |
                    plugin_manager::PluginFlags::PF_VIEWPORT_OVERLAY,
                true,
                "RodeoFX",
                "Annotation Onion Skinning Overlay")}));
}
}
