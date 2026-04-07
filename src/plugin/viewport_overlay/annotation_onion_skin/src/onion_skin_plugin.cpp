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
#include <variant>

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

    // Helper: blend a colour toward a tint
    auto tint_colour = [](const utility::ColourTriplet &c,
                          const utility::ColourTriplet &tint) -> utility::ColourTriplet {
        return {c.r * tint.r, c.g * tint.g, c.b * tint.b};
    };

    // Helper: create a Canvas copy with opacity and tint baked into every item
    auto make_tinted_canvas = [&](const ui::canvas::Canvas &src, float opacity,
                                  const utility::ColourTriplet &tint) -> ui::canvas::Canvas {
        ui::canvas::Canvas out(src); // copy
        // Iterate and replace each item with a tinted version
        for (auto it = out.begin(); it != out.end(); ++it) {
            auto item = *it; // copy the variant
            std::visit(
                [&](auto &v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, ui::canvas::Stroke>) {
                        v.set_opacity(v.opacity() * opacity);
                        v.set_colour(tint_colour(v.colour(), tint));
                    } else if constexpr (std::is_same_v<T, ui::canvas::Caption>) {
                        v.set_opacity(v.opacity() * opacity);
                        v.set_colour(tint_colour(v.colour(), tint));
                        v.set_bg_opacity(v.background_opacity() * opacity);
                    } else {
                        // Quad, Polygon, Ellipse — public members
                        v.opacity *= opacity;
                        v.colour = tint_colour(v.colour, tint);
                    }
                },
                item);
            out.overwrite_item(it, item);
        }
        return out;
    };

    auto compute_opacity = [&](int distance) -> float {
        return base_opac * std::pow(falloff, static_cast<float>(distance - 1));
    };

    // Collect canvases: farthest first, nearest last
    struct Candidate {
        const ui::canvas::Canvas *canvas;
        int abs_distance;
        float opacity;
        utility::ColourTriplet tint;
    };
    std::vector<Candidate> candidates;

    // Walk backward for past annotations
    if (want_before > 0) {
        int found = 0;
        for (auto it = all_bookmarks.rbegin();
             it != all_bookmarks.rend() && found < want_before; ++it) {
            const auto &bm = *it;
            if (!bm || !bm->annotation_ || !bm->annotation_->user_data())
                continue;
            if (bm->end_frame_ >= current_frame)
                continue;
            const auto *canvas =
                static_cast<const ui::canvas::Canvas *>(bm->annotation_->user_data());
            if (!canvas || canvas->empty())
                continue;
            found++;
            candidates.push_back(
                {canvas, current_frame - bm->end_frame_, compute_opacity(found), prev_colour});
        }
    }

    // Walk forward for future annotations
    if (want_after > 0) {
        int found = 0;
        for (auto it = all_bookmarks.begin();
             it != all_bookmarks.end() && found < want_after; ++it) {
            const auto &bm = *it;
            if (!bm || !bm->annotation_ || !bm->annotation_->user_data())
                continue;
            if (bm->start_frame_ <= current_frame)
                continue;
            const auto *canvas =
                static_cast<const ui::canvas::Canvas *>(bm->annotation_->user_data());
            if (!canvas || canvas->empty())
                continue;
            found++;
            candidates.push_back(
                {canvas, bm->start_frame_ - current_frame, compute_opacity(found), next_colour});
        }
    }

    if (candidates.empty())
        return {};

    // Sort farthest-to-nearest so nearest draws on top
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &a, const auto &b) { return a.abs_distance > b.abs_distance; });

    // Build tinted canvas copies
    std::vector<ui::canvas::Canvas> canvases;
    canvases.reserve(candidates.size());
    for (const auto &c : candidates) {
        canvases.push_back(make_tinted_canvas(*c.canvas, c.opacity, c.tint));
    }

    return std::make_shared<OnionSkinRenderData>(std::move(canvases));
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
