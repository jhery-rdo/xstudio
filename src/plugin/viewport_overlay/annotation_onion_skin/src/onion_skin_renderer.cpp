// SPDX-License-Identifier: Apache-2.0
#include "onion_skin_renderer.hpp"
#include "onion_skin_plugin.hpp"
#include "onion_skin_render_data.hpp"
#include "xstudio/media_reader/image_buffer.hpp"
#include "xstudio/utility/blind_data.hpp"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#include <GL/gl.h>
#endif

using namespace xstudio;
using namespace xstudio::ui::viewport;

namespace {

const char *composite_vertex_shader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
out vec2 uv;

void main()
{
    gl_Position = vec4(aPos, 0.0, 1.0);
    uv = aPos * 0.5 + 0.5;
}
)";

const char *composite_fragment_shader = R"(
#version 330 core
in vec2 uv;
out vec4 FragColor;

uniform sampler2D fbo_texture;
uniform float opacity;
uniform vec3 tint_colour;

void main()
{
    vec4 c = texture(fbo_texture, uv);
    // Apply tint to the annotation colour, preserve alpha structure
    vec3 tinted = c.rgb * tint_colour;
    FragColor = vec4(tinted, c.a * opacity);
}
)";

} // anonymous namespace


void OnionSkinRenderer::init_gl() {

    if (gl_initialized_)
        return;

    // Create fullscreen quad VAO/VBO
    // Two triangles covering [-1,1] x [-1,1]
    static const float quad_vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };

    glGenVertexArrays(1, &quad_vao_);
    glGenBuffers(1, &quad_vbo_);

    glBindVertexArray(quad_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // Create offscreen FBO renderer
    offscreen_fbo_ = std::make_unique<opengl::OpenGLOffscreenRenderer>(GL_RGBA8);

    // Create canvas renderer (reuses the same one used by annotations plugin)
    canvas_renderer_ = std::make_unique<opengl::OpenGLCanvasRenderer>();

    // Create composite shader
    composite_shader_ = std::make_unique<opengl::GLShaderProgram>(
        composite_vertex_shader, composite_fragment_shader);

    gl_initialized_ = true;
}


void OnionSkinRenderer::render_image_overlay(
    const Imath::M44f &transform_window_to_viewport_space,
    const Imath::M44f &transform_viewport_to_image_space,
    const float viewport_du_dpixel,
    const float device_pixel_ratio,
    const xstudio::media_reader::ImageBufPtr &frame) {

    // Retrieve our render data from the frame's blind data
    auto blind = frame.plugin_blind_data(OnionSkinPlugin::PLUGIN_UUID);
    const auto *render_data =
        dynamic_cast<const OnionSkinRenderData *>(blind.get());
    if (!render_data || render_data->neighbors.empty())
        return;

    init_gl();

    // ── Save GL state that we (and the canvas renderer) will modify ──
    GLfloat prev_clear_color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, prev_clear_color);

    GLboolean prev_blend    = glIsEnabled(GL_BLEND);
    GLboolean prev_depth    = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_scissor  = glIsEnabled(GL_SCISSOR_TEST);
    GLint prev_blend_src, prev_blend_dst, prev_blend_src_a, prev_blend_dst_a;
    glGetIntegerv(GL_BLEND_SRC_RGB, &prev_blend_src);
    glGetIntegerv(GL_BLEND_DST_RGB, &prev_blend_dst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_blend_dst_a);
    GLint prev_blend_eq_rgb, prev_blend_eq_a;
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prev_blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prev_blend_eq_a);

    GLint prev_active_tex;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
    GLint prev_tex_2d;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex_2d);

    GLint prev_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    GLint prev_vao;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);

    // ── Determine FBO dimensions from current GL viewport ──
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    const Imath::V2f fbo_dims(
        static_cast<float>(vp[2]) * device_pixel_ratio,
        static_cast<float>(vp[3]) * device_pixel_ratio);

    offscreen_fbo_->resize(fbo_dims);

    const float img_aspect = media_reader::image_aspect(frame);

    // ── Render each neighboring annotation: farthest first, nearest last ──
    for (const auto &neighbor : render_data->neighbors) {

        // 1. Render the canvas into the offscreen FBO
        offscreen_fbo_->begin();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        canvas_renderer_->render_canvas(
            neighbor.canvas,
            transform_window_to_viewport_space,
            transform_viewport_to_image_space,
            viewport_du_dpixel,
            device_pixel_ratio,
            img_aspect,
            false); // don't hide strokes

        offscreen_fbo_->end();

        // 2. Composite the FBO texture onto the viewport with opacity and tint
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);

        composite_shader_->use();

        GLint loc_opacity = glGetUniformLocation(composite_shader_->program_, "opacity");
        GLint loc_tint    = glGetUniformLocation(composite_shader_->program_, "tint_colour");
        GLint loc_tex     = glGetUniformLocation(composite_shader_->program_, "fbo_texture");

        glUniform1f(loc_opacity, neighbor.opacity);
        glUniform3f(loc_tint, neighbor.tint.x, neighbor.tint.y, neighbor.tint.z);
        glUniform1i(loc_tex, 0);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(offscreen_fbo_->texture_target(), offscreen_fbo_->texture_handle());

        glBindVertexArray(quad_vao_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);

        glBindTexture(offscreen_fbo_->texture_target(), 0);
        composite_shader_->stop_using();
    }

    // ── Restore all GL state ──
    glClearColor(prev_clear_color[0], prev_clear_color[1],
                 prev_clear_color[2], prev_clear_color[3]);

    if (prev_blend)   glEnable(GL_BLEND);   else glDisable(GL_BLEND);
    if (prev_depth)   glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    glBlendFuncSeparate(prev_blend_src, prev_blend_dst,
                        prev_blend_src_a, prev_blend_dst_a);
    glBlendEquationSeparate(prev_blend_eq_rgb, prev_blend_eq_a);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, prev_tex_2d);
    glActiveTexture(prev_active_tex);

    glUseProgram(prev_program);
    glBindVertexArray(prev_vao);
}
