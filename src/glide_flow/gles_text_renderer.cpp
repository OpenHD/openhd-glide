/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 *
 * This software is provided "as-is," without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability,
 * fitness for a particular purpose, and non-infringement. For details, see the
 * full license in the LICENSE file provided with this source code.
 *
 * Non-Military Use Only:
 * This software and its associated components are explicitly intended for
 * civilian and non-military purposes. Use in any military or defense
 * applications is strictly prohibited unless explicitly and individually
 * licensed otherwise by the OpenHD Team.
 *
 * Contributors:
 * A full list of contributors can be found at the OpenHD GitHub repository:
 * https://github.com/OpenHD
 *
 * © OpenHD, All Rights Reserved.
 ******************************************************************************/

#include "glide_flow/gles_text_renderer.hpp"

#include "glide_flow/vector_font.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>

#if OPENHD_GLIDE_HAS_GLESV2
#include <GLES2/gl2.h>
#endif

#if OPENHD_GLIDE_HAS_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace glide::flow {
namespace {

#if OPENHD_GLIDE_HAS_GLESV2
constexpr auto vertex_shader_source = R"glsl(
attribute vec2 a_position;

void main()
{
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)glsl";

constexpr auto fragment_shader_source = R"glsl(
precision mediump float;
uniform vec4 u_color;

void main()
{
    gl_FragColor = u_color;
}
)glsl";

constexpr auto text_vertex_shader_source = R"glsl(
attribute vec2 a_position;
attribute vec2 a_texcoord;
varying vec2 v_texcoord;

void main()
{
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)glsl";

constexpr auto text_fragment_shader_source = R"glsl(
precision mediump float;
uniform sampler2D u_texture;
uniform vec4 u_color;
varying vec2 v_texcoord;

void main()
{
    float alpha = texture2D(u_texture, v_texcoord).a;
    gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);
}
)glsl";

constexpr auto argb_fragment_shader_source = R"glsl(
precision mediump float;
uniform sampler2D u_texture;
varying vec2 v_texcoord;

void main()
{
    vec4 bgra = texture2D(u_texture, v_texcoord);
    gl_FragColor = vec4(bgra.b, bgra.g, bgra.r, bgra.a);
}
)glsl";

GLuint compile_shader(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

float to_ndc_x(float x, SurfaceSize surface)
{
    return (x / static_cast<float>(surface.width) * 2.0F) - 1.0F;
}

float to_ndc_y(float y, SurfaceSize surface)
{
    return 1.0F - (y / static_cast<float>(surface.height) * 2.0F);
}

void draw_quad(GLint position_location, const std::array<GLfloat, 8>& vertices)
{
    glVertexAttribPointer(
        static_cast<GLuint>(position_location),
        2,
        GL_FLOAT,
        GL_FALSE,
        0,
        vertices.data());
    glEnableVertexAttribArray(static_cast<GLuint>(position_location));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void draw_textured_quad(
    GLint position_location,
    GLint texcoord_location,
    const std::array<GLfloat, 16>& vertices)
{
    glVertexAttribPointer(
        static_cast<GLuint>(position_location),
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        vertices.data());
    glEnableVertexAttribArray(static_cast<GLuint>(position_location));
    glVertexAttribPointer(
        static_cast<GLuint>(texcoord_location),
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        vertices.data() + 2);
    glEnableVertexAttribArray(static_cast<GLuint>(texcoord_location));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void draw_stroke_quad(
    GLint position_location,
    RenderPoint start,
    RenderPoint end,
    float thickness,
    SurfaceSize surface)
{
    const auto dx = end.x - start.x;
    const auto dy = end.y - start.y;
    const auto length = std::sqrt((dx * dx) + (dy * dy));
    const auto half_thickness = thickness * 0.5F;

    if (length < 0.001F) {
        const std::array<GLfloat, 8> vertices {
            to_ndc_x(start.x - half_thickness, surface),
            to_ndc_y(start.y - half_thickness, surface),
            to_ndc_x(start.x + half_thickness, surface),
            to_ndc_y(start.y - half_thickness, surface),
            to_ndc_x(start.x - half_thickness, surface),
            to_ndc_y(start.y + half_thickness, surface),
            to_ndc_x(start.x + half_thickness, surface),
            to_ndc_y(start.y + half_thickness, surface),
        };
        draw_quad(position_location, vertices);
        return;
    }

    const auto nx = (-dy / length) * half_thickness;
    const auto ny = (dx / length) * half_thickness;
    const std::array<GLfloat, 8> vertices {
        to_ndc_x(start.x + nx, surface),
        to_ndc_y(start.y + ny, surface),
        to_ndc_x(start.x - nx, surface),
        to_ndc_y(start.y - ny, surface),
        to_ndc_x(end.x + nx, surface),
        to_ndc_y(end.y + ny, surface),
        to_ndc_x(end.x - nx, surface),
        to_ndc_y(end.y - ny, surface),
    };
    draw_quad(position_location, vertices);
}
#endif

} // namespace

std::string GlesTextRenderer::runtime_description() const
{
#if OPENHD_GLIDE_HAS_GLESV2
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));

    std::ostringstream stream;
    stream << "OpenGL ES runtime vendor='" << (vendor != nullptr ? vendor : "unknown")
           << "' renderer='" << (renderer != nullptr ? renderer : "unknown")
           << "' version='" << (version != nullptr ? version : "unknown") << "'";
    return stream.str();
#else
    return "OpenGL ES runtime unavailable at build time";
#endif
}

bool GlesTextRenderer::likely_software_renderer() const
{
#if OPENHD_GLIDE_HAS_GLESV2
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    auto haystack = std::string(renderer != nullptr ? renderer : "");
    haystack += " ";
    haystack += vendor != nullptr ? vendor : "";
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return haystack.find("llvmpipe") != std::string::npos
        || haystack.find("softpipe") != std::string::npos
        || haystack.find("software") != std::string::npos
        || haystack.find("swrast") != std::string::npos;
#else
    return true;
#endif
}

GlesTextRenderer::~GlesTextRenderer()
{
#if OPENHD_GLIDE_HAS_GLESV2
    for (const auto& [_, glyph] : glyph_cache_) {
        if (glyph.texture != 0) {
            const GLuint texture = glyph.texture;
            glDeleteTextures(1, &texture);
        }
    }
    if (text_program_ != 0) {
        glDeleteProgram(text_program_);
    }
    if (image_texture_ != 0) {
        const GLuint texture = image_texture_;
        glDeleteTextures(1, &texture);
    }
    if (image_program_ != 0) {
        glDeleteProgram(image_program_);
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
    }
#endif
#if OPENHD_GLIDE_HAS_FREETYPE
    if (freetype_face_ != nullptr) {
        FT_Done_Face(static_cast<FT_Face>(freetype_face_));
    }
    if (freetype_library_ != nullptr) {
        FT_Done_FreeType(static_cast<FT_Library>(freetype_library_));
    }
#endif
}

bool GlesTextRenderer::available() const
{
#if OPENHD_GLIDE_HAS_GLESV2
    return true;
#else
    return false;
#endif
}

bool GlesTextRenderer::ensure_initialized()
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (program_ != 0) {
        return true;
    }

    const GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    if (vertex_shader == 0) {
        last_error_ = "failed to compile FPS vertex shader";
        return false;
    }

    const GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (fragment_shader == 0) {
        glDeleteShader(vertex_shader);
        last_error_ = "failed to compile FPS fragment shader";
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "a_position");
    glLinkProgram(program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        glDeleteProgram(program);
        last_error_ = "failed to link FPS shader program";
        return false;
    }

    program_ = program;
    position_location_ = glGetAttribLocation(program_, "a_position");
    color_location_ = glGetUniformLocation(program_, "u_color");
    return position_location_ >= 0 && color_location_ >= 0;
#else
    last_error_ = "OpenGL ES 2.0 was not found at build time";
    return false;
#endif
}

bool GlesTextRenderer::ensure_text_initialized()
{
#if OPENHD_GLIDE_HAS_GLESV2 && OPENHD_GLIDE_HAS_FREETYPE
    if (text_program_ != 0 && freetype_face_ != nullptr) {
        return true;
    }
    if (text_init_attempted_) {
        return false;
    }
    text_init_attempted_ = true;

    FT_Library library {};
    if (FT_Init_FreeType(&library) != 0) {
        last_error_ = "failed to initialize FreeType";
        return false;
    }

    const std::array font_paths {
        // Orange OS and several embedded distributions install Noto directly
        // below /usr/share/fonts instead of the Debian truetype hierarchy.
        // Prefer the medium face for legible OSD text over moving video.
        "/usr/share/fonts/noto/NotoSans-Medium.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Medium.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    };

    FT_Face face {};
    for (const auto* path : font_paths) {
        if (std::filesystem::exists(path) && FT_New_Face(library, path, 0, &face) == 0) {
            break;
        }
    }

    if (face == nullptr) {
        FT_Done_FreeType(library);
        last_error_ = "failed to load a TrueType font for OSD text";
        return false;
    }

    const GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, text_vertex_shader_source);
    const GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, text_fragment_shader_source);
    if (vertex_shader == 0 || fragment_shader == 0) {
        if (vertex_shader != 0) {
            glDeleteShader(vertex_shader);
        }
        if (fragment_shader != 0) {
            glDeleteShader(fragment_shader);
        }
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        last_error_ = "failed to compile OSD text shader";
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        glDeleteProgram(program);
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        last_error_ = "failed to link OSD text shader";
        return false;
    }

    freetype_library_ = library;
    freetype_face_ = face;
    text_program_ = program;
    text_position_location_ = glGetAttribLocation(text_program_, "a_position");
    text_texcoord_location_ = glGetAttribLocation(text_program_, "a_texcoord");
    text_color_location_ = glGetUniformLocation(text_program_, "u_color");
    text_sampler_location_ = glGetUniformLocation(text_program_, "u_texture");
    return text_position_location_ >= 0
        && text_texcoord_location_ >= 0
        && text_color_location_ >= 0
        && text_sampler_location_ >= 0;
#else
    return false;
#endif
}

bool GlesTextRenderer::ensure_image_initialized()
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (image_program_ != 0 && image_texture_ != 0) {
        return true;
    }

    const GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, text_vertex_shader_source);
    const GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, argb_fragment_shader_source);
    if (vertex_shader == 0 || fragment_shader == 0) {
        if (vertex_shader != 0) {
            glDeleteShader(vertex_shader);
        }
        if (fragment_shader != 0) {
            glDeleteShader(fragment_shader);
        }
        last_error_ = "failed to compile ARGB image shader";
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        glDeleteProgram(program);
        last_error_ = "failed to link ARGB image shader";
        return false;
    }

    GLuint texture {};
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    image_program_ = program;
    image_position_location_ = glGetAttribLocation(image_program_, "a_position");
    image_texcoord_location_ = glGetAttribLocation(image_program_, "a_texcoord");
    image_sampler_location_ = glGetUniformLocation(image_program_, "u_texture");
    image_texture_ = texture;
    return image_position_location_ >= 0
        && image_texcoord_location_ >= 0
        && image_sampler_location_ >= 0;
#else
    last_error_ = "OpenGL ES 2.0 was not found at build time";
    return false;
#endif
}

GlesTextRenderer::GlyphTexture* GlesTextRenderer::load_glyph(char character, unsigned int pixel_size)
{
#if OPENHD_GLIDE_HAS_GLESV2 && OPENHD_GLIDE_HAS_FREETYPE
    if (!ensure_text_initialized()) {
        return nullptr;
    }

    const auto key = (pixel_size << 8U) | static_cast<unsigned char>(character);
    if (auto existing = glyph_cache_.find(key); existing != glyph_cache_.end()) {
        return &existing->second;
    }

    auto* face = static_cast<FT_Face>(freetype_face_);
    FT_Set_Pixel_Sizes(face, 0, pixel_size);
    if (FT_Load_Char(face, static_cast<unsigned char>(character), FT_LOAD_RENDER) != 0) {
        return nullptr;
    }

    GLuint texture {};
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_ALPHA,
        static_cast<GLsizei>(face->glyph->bitmap.width),
        static_cast<GLsizei>(face->glyph->bitmap.rows),
        0,
        GL_ALPHA,
        GL_UNSIGNED_BYTE,
        face->glyph->bitmap.buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const GlyphTexture glyph {
        .texture = texture,
        .width = static_cast<int>(face->glyph->bitmap.width),
        .height = static_cast<int>(face->glyph->bitmap.rows),
        .bearing_x = face->glyph->bitmap_left,
        .bearing_y = face->glyph->bitmap_top,
        .advance = static_cast<unsigned int>(face->glyph->advance.x),
    };

    auto [iterator, _] = glyph_cache_.insert({ key, glyph });
    return &iterator->second;
#else
    (void)character;
    (void)pixel_size;
    return nullptr;
#endif
}

float GlesTextRenderer::measure_text_width(std::string_view text, float scale)
{
#if OPENHD_GLIDE_HAS_GLESV2 && OPENHD_GLIDE_HAS_FREETYPE
    const auto pixel_size = static_cast<unsigned int>(std::clamp(scale * 1.35F, 10.0F, 72.0F));
    auto width = 0.0F;
    for (const auto character : text) {
        auto* glyph = load_glyph(character, pixel_size);
        width += glyph != nullptr ? static_cast<float>(glyph->advance >> 6U) : scale * 0.55F;
    }
    return width;
#else
    return vector_text_width(text) * scale;
#endif
}

bool GlesTextRenderer::draw_antialiased_text(const TextPlacement& placement, SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2 && OPENHD_GLIDE_HAS_FREETYPE
    if (!ensure_text_initialized()) {
        return false;
    }

    const auto pixel_size = static_cast<unsigned int>(std::clamp(placement.scale * 1.35F, 10.0F, 72.0F));
    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(text_program_);
    glUniform4f(text_color_location_, text_color_.red, text_color_.green, text_color_.blue, text_color_.alpha);
    glUniform1i(text_sampler_location_, 0);
    glActiveTexture(GL_TEXTURE0);

    auto x = placement.x;
    const auto baseline = placement.y;
    for (const auto character : placement.text) {
        auto* glyph = load_glyph(character, pixel_size);
        if (glyph == nullptr) {
            x += placement.scale * 0.55F;
            continue;
        }

        const auto xpos = x + static_cast<float>(glyph->bearing_x);
        const auto ypos = baseline - static_cast<float>(glyph->bearing_y);
        const auto width = static_cast<float>(glyph->width);
        const auto height = static_cast<float>(glyph->height);

        glBindTexture(GL_TEXTURE_2D, glyph->texture);
        const std::array<GLfloat, 16> vertices {
            to_ndc_x(xpos, surface),
            to_ndc_y(ypos, surface),
            0.0F,
            0.0F,
            to_ndc_x(xpos + width, surface),
            to_ndc_y(ypos, surface),
            1.0F,
            0.0F,
            to_ndc_x(xpos, surface),
            to_ndc_y(ypos + height, surface),
            0.0F,
            1.0F,
            to_ndc_x(xpos + width, surface),
            to_ndc_y(ypos + height, surface),
            1.0F,
            1.0F,
        };
        draw_textured_quad(text_position_location_, text_texcoord_location_, vertices);
        x += static_cast<float>(glyph->advance >> 6U);
    }

    return true;
#else
    (void)placement;
    (void)surface;
    return false;
#endif
}

void GlesTextRenderer::clear(float red, float green, float blue, float alpha, SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2
    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glClearColor(red, green, blue, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
#else
    (void)red;
    (void)green;
    (void)blue;
    (void)alpha;
    (void)surface;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
#endif
}

void GlesTextRenderer::draw_filled_quad(
    RenderPoint top_left,
    RenderPoint top_right,
    RenderPoint bottom_left,
    RenderPoint bottom_right,
    RgbaColor color,
    SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (!ensure_initialized()) {
        return;
    }

    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);
    glUniform4f(color_location_, color.red, color.green, color.blue, color.alpha);

    const std::array<GLfloat, 8> vertices {
        to_ndc_x(top_left.x, surface),
        to_ndc_y(top_left.y, surface),
        to_ndc_x(top_right.x, surface),
        to_ndc_y(top_right.y, surface),
        to_ndc_x(bottom_left.x, surface),
        to_ndc_y(bottom_left.y, surface),
        to_ndc_x(bottom_right.x, surface),
        to_ndc_y(bottom_right.y, surface),
    };
    draw_quad(position_location_, vertices);
#else
    (void)top_left;
    (void)top_right;
    (void)bottom_left;
    (void)bottom_right;
    (void)color;
    (void)surface;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
#endif
}

void GlesTextRenderer::draw_argb_pixels(
    const void* pixels,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride_bytes,
    RenderPoint top_left,
    SurfaceSize surface)
{
    if (update_argb_texture(pixels, width, height, stride_bytes)) {
        draw_cached_argb_texture(top_left, surface);
    }
}

bool GlesTextRenderer::update_argb_texture(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride_bytes)
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (pixels == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (stride_bytes != width * 4U) {
        last_error_ = "ARGB image stride must match width * 4";
        return false;
    }
    if (!ensure_image_initialized()) {
        return false;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image_texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (image_texture_width_ != width || image_texture_height_ != height) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels);
        image_texture_width_ = width;
        image_texture_height_ = height;
        image_texture_ready_ = true;
    } else {
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            pixels);
    }
    image_texture_ready_ = true;
    return true;
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride_bytes;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
    return false;
#endif
}

void GlesTextRenderer::draw_cached_argb_texture(RenderPoint top_left, SurfaceSize surface)
{
    draw_cached_argb_texture_scaled(
        top_left,
        static_cast<float>(image_texture_width_),
        static_cast<float>(image_texture_height_),
        surface);
}

void GlesTextRenderer::draw_cached_argb_texture_scaled(RenderPoint top_left, float width, float height, SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (!image_texture_ready_ || image_texture_width_ == 0 || image_texture_height_ == 0) {
        return;
    }
    if (!ensure_image_initialized()) {
        return;
    }

    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(image_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image_texture_);
    glUniform1i(image_sampler_location_, 0);
    const auto x = top_left.x;
    const auto y = top_left.y;
    const auto right = x + width;
    const auto bottom = y + height;
    const std::array<GLfloat, 16> vertices {
        to_ndc_x(x, surface),
        to_ndc_y(y, surface),
        0.0F,
        0.0F,
        to_ndc_x(right, surface),
        to_ndc_y(y, surface),
        1.0F,
        0.0F,
        to_ndc_x(x, surface),
        to_ndc_y(bottom, surface),
        0.0F,
        1.0F,
        to_ndc_x(right, surface),
        to_ndc_y(bottom, surface),
        1.0F,
        1.0F,
    };
    draw_textured_quad(image_position_location_, image_texcoord_location_, vertices);
#else
    (void)top_left;
    (void)width;
    (void)height;
    (void)surface;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
#endif
}

void GlesTextRenderer::set_text_color(RgbaColor color)
{
    text_color_ = color;
}

void GlesTextRenderer::draw_line(
    RenderPoint start,
    RenderPoint end,
    float thickness,
    RgbaColor color,
    SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (!ensure_initialized()) {
        return;
    }

    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);
    glUniform4f(color_location_, color.red, color.green, color.blue, color.alpha);
    draw_stroke_quad(position_location_, start, end, thickness, surface);
#else
    (void)start;
    (void)end;
    (void)thickness;
    (void)color;
    (void)surface;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
#endif
}

void GlesTextRenderer::draw_circle_outline(
    RenderPoint center,
    float radius,
    float thickness,
    RgbaColor color,
    SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2
    constexpr int segments = 72;
    constexpr float pi = 3.14159265358979323846F;

    for (int i = 0; i < segments; ++i) {
        const auto a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0F * pi;
        const auto a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 2.0F * pi;
        draw_line(
            RenderPoint {
                .x = center.x + std::cos(a0) * radius,
                .y = center.y + std::sin(a0) * radius,
            },
            RenderPoint {
                .x = center.x + std::cos(a1) * radius,
                .y = center.y + std::sin(a1) * radius,
            },
            thickness,
            color,
            surface);
    }
#else
    (void)center;
    (void)radius;
    (void)thickness;
    (void)color;
    (void)surface;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
#endif
}

void GlesTextRenderer::draw(const TextPlacement& placement, SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_GLESV2
    if (draw_antialiased_text(placement, surface)) {
        return;
    }

    if (!ensure_initialized()) {
        return;
    }

    // This assumes an EGL/OpenGL ES context is current. The DRM/EGL surface setup
    // will own context creation; GlideFlow only submits OSD geometry.
    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);

    float cursor_x = placement.x;
    for (const auto character : placement.text) {
        const auto* glyph = vector_glyph(character);
        if (glyph == nullptr) {
            cursor_x += 0.72F * placement.scale;
            continue;
        }

        for (const auto& stroke : glyph->strokes) {
            const RenderPoint shadow_start {
                .x = cursor_x + (stroke.start.x * placement.scale) + 2.0F,
                .y = placement.y - (stroke.start.y * placement.scale) + 2.0F,
            };
            const RenderPoint shadow_end {
                .x = cursor_x + (stroke.end.x * placement.scale) + 2.0F,
                .y = placement.y - (stroke.end.y * placement.scale) + 2.0F,
            };
            glUniform4f(color_location_, 0.0F, 0.0F, 0.0F, 0.45F);
            draw_stroke_quad(position_location_, shadow_start, shadow_end, placement.scale * 0.13F, surface);
        }

        for (const auto& stroke : glyph->strokes) {
            const RenderPoint start {
                .x = cursor_x + (stroke.start.x * placement.scale),
                .y = placement.y - (stroke.start.y * placement.scale),
            };
            const RenderPoint end {
                .x = cursor_x + (stroke.end.x * placement.scale),
                .y = placement.y - (stroke.end.y * placement.scale),
            };
            glUniform4f(color_location_, text_color_.red, text_color_.green, text_color_.blue, text_color_.alpha);
            draw_stroke_quad(position_location_, start, end, placement.scale * 0.095F, surface);
        }

        cursor_x += glyph->advance * placement.scale;
    }
#else
    (void)placement;
    (void)surface;
    last_error_ = "OpenGL ES 2.0 was not found at build time";
#endif
}

std::string GlesTextRenderer::last_error() const
{
    return last_error_;
}

} // namespace glide::flow
