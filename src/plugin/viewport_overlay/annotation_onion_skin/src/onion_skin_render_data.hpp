// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vector>

#include <Imath/ImathVec.h>

#include "xstudio/ui/canvas/canvas.hpp"
#include "xstudio/utility/blind_data.hpp"


namespace xstudio {
namespace ui {
    namespace viewport {

        struct NeighborAnnotation {
            canvas::Canvas canvas;
            int frame_offset{0};
            float opacity{0.0f};
            Imath::V3f tint{1.0f, 1.0f, 1.0f};
        };

        class OnionSkinRenderData : public utility::BlindDataObject {
          public:
            OnionSkinRenderData() = default;
            explicit OnionSkinRenderData(std::vector<NeighborAnnotation> n)
                : neighbors(std::move(n)) {}
            ~OnionSkinRenderData() override = default;

            std::vector<NeighborAnnotation> neighbors;
        };

    } // namespace viewport
} // namespace ui
} // namespace xstudio
