// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

#include "xstudio/plugin_manager/plugin_base.hpp"
#include "xstudio/ui/opengl/opengl_canvas_renderer.hpp"
#include "xstudio/ui/opengl/opengl_offscreen_renderer.hpp"
#include "xstudio/ui/opengl/shader_program_base.hpp"

namespace xstudio {
namespace ui {
    namespace viewport {

        class OnionSkinRenderer : public plugin::ViewportOverlayRenderer {

          public:
            OnionSkinRenderer() = default;
            ~OnionSkinRenderer() = default;

            void render_image_overlay(
                const Imath::M44f &transform_window_to_viewport_space,
                const Imath::M44f &transform_viewport_to_image_space,
                const float viewport_du_dpixel,
                const float device_pixel_ratio,
                const xstudio::media_reader::ImageBufPtr &frame) override;

            float stack_order() const override { return 1.5f; }

          private:
            void init_gl();

            std::unique_ptr<opengl::OpenGLCanvasRenderer> canvas_renderer_;
            std::unique_ptr<opengl::OpenGLOffscreenRenderer> offscreen_fbo_;
            std::unique_ptr<opengl::GLShaderProgram> composite_shader_;

            GLuint quad_vao_{0};
            GLuint quad_vbo_{0};
            bool gl_initialized_{false};
        };

    } // namespace viewport
} // namespace ui
} // namespace xstudio
