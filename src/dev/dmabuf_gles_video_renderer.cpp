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
 ******************************************************************************/

#include "dev/dmabuf_gles_video_renderer.hpp"

#if OPENHD_GLIDE_HAS_KMS_GBM
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <unistd.h>
#endif

namespace glide::dev {

DmabufGlesVideoRenderer::~DmabufGlesVideoRenderer()
{
    cleanup();
}

const std::string& DmabufGlesVideoRenderer::last_error() const
{
    return last_error_;
}

#if OPENHD_GLIDE_HAS_KMS_GBM
namespace {

constexpr auto vertex_shader_source = R"glsl(
attribute vec2 a_position;
attribute vec2 a_texcoord;
varying vec2 v_texcoord;

void main()
{
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texcoord = a_texcoord;
}
)glsl";

constexpr auto fragment_shader_source = R"glsl(
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES u_texture;
varying vec2 v_texcoord;

void main()
{
    gl_FragColor = texture2D(u_texture, v_texcoord);
}
)glsl";

std::string egl_error(const char* operation)
{
    std::ostringstream stream;
    stream << operation << " EGL error=0x" << std::hex << eglGetError();
    return stream.str();
}

std::string gl_error(const char* operation)
{
    std::ostringstream stream;
    stream << operation << " GL error=0x" << std::hex << glGetError();
    return stream.str();
}

GLuint compile_shader(GLenum type, const char* source, std::string& error)
{
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status {};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return shader;
    }

    GLint log_size {};
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_size);
    std::string log(static_cast<std::size_t>(std::max(log_size, 1)), '\0');
    glGetShaderInfoLog(shader, log_size, nullptr, log.data());
    error = "failed to compile DMA-BUF video shader: " + log;
    glDeleteShader(shader);
    return 0;
}

} // namespace
#endif

bool DmabufGlesVideoRenderer::draw(const DmabufVideoFrame& frame, flow::SurfaceSize surface)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    last_error_.clear();
    if (!initialize()) {
        return false;
    }
    if (frame.drm_format != DRM_FORMAT_NV12 || frame.plane_count != 2) {
        last_error_ = "single-plane GLES video currently requires a two-plane NV12 DMA-BUF";
        return false;
    }
    if (surface.width == 0 || surface.height == 0 || frame.width == 0 || frame.height == 0) {
        last_error_ = "invalid video or output dimensions for GLES composition";
        return false;
    }

    auto* image = find_or_import(frame);
    if (image == nullptr) {
        return false;
    }

    const auto source_aspect = static_cast<float>(frame.width) / static_cast<float>(frame.height);
    const auto output_aspect = static_cast<float>(surface.width) / static_cast<float>(surface.height);
    float x_extent { 1.0F };
    float y_extent { 1.0F };
    if (source_aspect > output_aspect) {
        y_extent = output_aspect / source_aspect;
    } else {
        x_extent = source_aspect / output_aspect;
    }

    const std::array<GLfloat, 16> vertices {
        -x_extent, y_extent, 0.0F, 0.0F,
        x_extent, y_extent, 1.0F, 0.0F,
        -x_extent, -y_extent, 0.0F, 1.0F,
        x_extent, -y_extent, 1.0F, 1.0F,
    };

    glViewport(0, 0, static_cast<GLsizei>(surface.width), static_cast<GLsizei>(surface.height));
    glDisable(GL_BLEND);
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, image->texture);
    glUniform1i(sampler_location_, 0);
    glVertexAttribPointer(
        static_cast<GLuint>(position_location_),
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        vertices.data());
    glEnableVertexAttribArray(static_cast<GLuint>(position_location_));
    glVertexAttribPointer(
        static_cast<GLuint>(texcoord_location_),
        2,
        GL_FLOAT,
        GL_FALSE,
        4 * sizeof(GLfloat),
        vertices.data() + 2);
    glEnableVertexAttribArray(static_cast<GLuint>(texcoord_location_));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (const auto error = glGetError(); error != GL_NO_ERROR) {
        std::ostringstream stream;
        stream << "failed to draw imported NV12 DMA-BUF GL error=0x" << std::hex << error;
        last_error_ = stream.str();
        return false;
    }

    // The VPU has no explicit-fence handoff yet. Finish sampling before the
    // decoder is allowed to reuse this framebuffer.
    glFinish();
    return true;
#else
    (void)frame;
    (void)surface;
    last_error_ = "KMS/GBM/EGL support was not found at build time";
    return false;
#endif
}

#if OPENHD_GLIDE_HAS_KMS_GBM
bool DmabufGlesVideoRenderer::initialize()
{
    if (initialized_) {
        return true;
    }

    const auto display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY) {
        last_error_ = "single-plane compositor has no current EGL display";
        return false;
    }
    const auto* egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (egl_extensions == nullptr || std::strstr(egl_extensions, "EGL_EXT_image_dma_buf_import") == nullptr) {
        last_error_ = "EGL_EXT_image_dma_buf_import is unavailable";
        return false;
    }
    const auto* gl_extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (gl_extensions == nullptr || std::strstr(gl_extensions, "GL_OES_EGL_image_external") == nullptr) {
        last_error_ = "GL_OES_EGL_image_external is unavailable";
        return false;
    }

    create_image_ = reinterpret_cast<void*>(eglGetProcAddress("eglCreateImageKHR"));
    destroy_image_ = reinterpret_cast<void*>(eglGetProcAddress("eglDestroyImageKHR"));
    bind_image_ = reinterpret_cast<void*>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (create_image_ == nullptr || destroy_image_ == nullptr || bind_image_ == nullptr) {
        last_error_ = "required EGLImage DMA-BUF entry points are unavailable";
        return false;
    }

    std::string shader_error;
    const auto vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source, shader_error);
    const auto fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source, shader_error);
    if (vertex_shader == 0 || fragment_shader == 0) {
        if (vertex_shader != 0) {
            glDeleteShader(vertex_shader);
        }
        if (fragment_shader != 0) {
            glDeleteShader(fragment_shader);
        }
        last_error_ = shader_error;
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertex_shader);
    glAttachShader(program_, fragment_shader);
    glLinkProgram(program_);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    GLint linked {};
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        last_error_ = "failed to link DMA-BUF external-texture shader";
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    position_location_ = glGetAttribLocation(program_, "a_position");
    texcoord_location_ = glGetAttribLocation(program_, "a_texcoord");
    sampler_location_ = glGetUniformLocation(program_, "u_texture");
    if (position_location_ < 0 || texcoord_location_ < 0 || sampler_location_ < 0) {
        last_error_ = "DMA-BUF video shader locations are unavailable";
        return false;
    }

    egl_display_ = display;
    initialized_ = true;
    return true;
}

bool DmabufGlesVideoRenderer::make_key(const DmabufVideoFrame& frame, ImageKey& key)
{
    if (frame.fds[0] < 0) {
        last_error_ = "NV12 DMA-BUF has no luma-plane file descriptor";
        return false;
    }
    struct stat info {};
    if (fstat(frame.fds[0], &info) != 0) {
        last_error_ = "failed to identify NV12 DMA-BUF";
        return false;
    }
    key = {};
    key.device = static_cast<std::uint64_t>(info.st_dev);
    key.inode = static_cast<std::uint64_t>(info.st_ino);
    key.width = frame.width;
    key.height = frame.height;
    key.format = frame.drm_format;
    key.plane_count = frame.plane_count;
    key.yuv_color_space = frame.yuv_color_space;
    key.yuv_range = frame.yuv_range;
    for (std::uint32_t plane = 0; plane < frame.plane_count; ++plane) {
        key.strides[plane] = frame.strides[plane];
        key.offsets[plane] = frame.offsets[plane];
        key.modifiers[plane] = frame.modifiers[plane];
    }
    return true;
}

DmabufGlesVideoRenderer::CachedImage* DmabufGlesVideoRenderer::find_or_import(const DmabufVideoFrame& frame)
{
    ImageKey key;
    if (!make_key(frame, key)) {
        return nullptr;
    }
    for (auto& cached : images_) {
        bool equal = cached.key.device == key.device
            && cached.key.inode == key.inode
            && cached.key.width == key.width
            && cached.key.height == key.height
            && cached.key.format == key.format
            && cached.key.plane_count == key.plane_count
            && cached.key.yuv_color_space == key.yuv_color_space
            && cached.key.yuv_range == key.yuv_range;
        for (std::uint32_t plane = 0; equal && plane < key.plane_count; ++plane) {
            equal = cached.key.strides[plane] == key.strides[plane]
                && cached.key.offsets[plane] == key.offsets[plane]
                && cached.key.modifiers[plane] == key.modifiers[plane];
        }
        if (equal) {
            cached.last_used = ++serial_;
            return &cached;
        }
    }

    CachedImage image;
    if (!import_image(frame, key, image)) {
        return nullptr;
    }
    image.last_used = ++serial_;
    images_.push_back(image);
    evict_if_needed();
    return &images_.back();
}

bool DmabufGlesVideoRenderer::import_image(const DmabufVideoFrame& frame, const ImageKey& key, CachedImage& cached)
{
    EGLint egl_color_space = EGL_ITU_REC709_EXT;
    switch (frame.yuv_color_space) {
    case DmabufYuvColorSpace::rec601:
        egl_color_space = EGL_ITU_REC601_EXT;
        break;
    case DmabufYuvColorSpace::rec2020:
        egl_color_space = EGL_ITU_REC2020_EXT;
        break;
    case DmabufYuvColorSpace::rec709:
    case DmabufYuvColorSpace::unspecified:
        break;
    }
    const EGLint egl_sample_range =
        frame.yuv_range == DmabufYuvRange::full ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT;

    std::vector<EGLint> attributes {
        EGL_WIDTH, static_cast<EGLint>(frame.width),
        EGL_HEIGHT, static_cast<EGLint>(frame.height),
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(frame.drm_format),
        EGL_DMA_BUF_PLANE0_FD_EXT, frame.fds[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<EGLint>(frame.offsets[0]),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(frame.strides[0]),
        EGL_DMA_BUF_PLANE1_FD_EXT, frame.fds[1],
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(frame.offsets[1]),
        EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(frame.strides[1]),
        EGL_YUV_COLOR_SPACE_HINT_EXT, egl_color_space,
        EGL_SAMPLE_RANGE_HINT_EXT, egl_sample_range,
        EGL_NONE,
    };

    auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(create_image_);
    const auto image = create_image(
        static_cast<EGLDisplay>(egl_display_),
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        nullptr,
        attributes.data());
    if (image == EGL_NO_IMAGE_KHR) {
        last_error_ = egl_error("failed to import NV12 DMA-BUF");
        return false;
    }

    GLuint texture {};
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    auto bind_image = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(bind_image_);
    bind_image(GL_TEXTURE_EXTERNAL_OES, image);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(destroy_image_);
        destroy_image(static_cast<EGLDisplay>(egl_display_), image);
        last_error_ = gl_error("failed to bind imported NV12 EGLImage");
        return false;
    }

    cached.key = key;
    cached.image = image;
    cached.texture = texture;
    return true;
}

void DmabufGlesVideoRenderer::evict_if_needed()
{
    constexpr std::size_t maximum_cached_images = 16;
    while (images_.size() > maximum_cached_images) {
        auto oldest = std::min_element(images_.begin(), images_.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.last_used < rhs.last_used;
        });
        destroy_image(*oldest);
        images_.erase(oldest);
    }
}

void DmabufGlesVideoRenderer::destroy_image(CachedImage& image)
{
    if (image.texture != 0) {
        const GLuint texture = image.texture;
        glDeleteTextures(1, &texture);
        image.texture = 0;
    }
    if (image.image != nullptr && egl_display_ != nullptr && destroy_image_ != nullptr) {
        auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(destroy_image_);
        destroy_image(static_cast<EGLDisplay>(egl_display_), static_cast<EGLImageKHR>(image.image));
        image.image = nullptr;
    }
}

void DmabufGlesVideoRenderer::cleanup()
{
    for (auto& image : images_) {
        destroy_image(image);
    }
    images_.clear();
    if (program_ != 0) {
        const GLuint program = program_;
        glDeleteProgram(program);
        program_ = 0;
    }
}
#else
void DmabufGlesVideoRenderer::cleanup()
{
}
#endif

} // namespace glide::dev
