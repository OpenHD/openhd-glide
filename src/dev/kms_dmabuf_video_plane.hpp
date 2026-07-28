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

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace glide::dev {

enum class DmabufYuvColorSpace : std::uint8_t {
    unspecified,
    rec601,
    rec709,
    rec2020,
};

enum class DmabufYuvRange : std::uint8_t {
    unspecified,
    narrow,
    full,
};

struct DmabufVideoFrame {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t drm_format {};
    std::uint32_t plane_count {};
    std::array<int, 4> fds { -1, -1, -1, -1 };
    std::array<std::uint32_t, 4> strides {};
    std::array<std::uint32_t, 4> offsets {};
    std::array<std::uint64_t, 4> modifiers {};
    DmabufYuvColorSpace yuv_color_space { DmabufYuvColorSpace::unspecified };
    DmabufYuvRange yuv_range { DmabufYuvRange::unspecified };
};

struct CpuVideoFrame {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t stride {};
    const void* pixels {};
};

class KmsDmabufVideoPlane {
public:
    KmsDmabufVideoPlane() = default;
    ~KmsDmabufVideoPlane();

    KmsDmabufVideoPlane(const KmsDmabufVideoPlane&) = delete;
    KmsDmabufVideoPlane& operator=(const KmsDmabufVideoPlane&) = delete;

    bool create(std::uint32_t requested_width, std::uint32_t requested_height, std::uint32_t requested_refresh_hz = 0, int preferred_plane_id = -1);
    bool present(const DmabufVideoFrame& frame);
    bool present(const CpuVideoFrame& frame);
    void set_vblank_wait_enabled(bool enabled);
    const std::string& last_error() const;

private:
    struct ImportedFramebuffer {
        std::uint32_t framebuffer {};
        std::array<std::uint32_t, 4> handles {};
    };

    struct FrameKey {
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t drm_format {};
        std::uint32_t plane_count {};
        std::array<std::uint64_t, 4> device_ids {};
        std::array<std::uint64_t, 4> inodes {};
        std::array<std::uint32_t, 4> strides {};
        std::array<std::uint32_t, 4> offsets {};
        std::array<std::uint64_t, 4> modifiers {};
    };

    struct CachedFramebuffer {
        FrameKey key {};
        ImportedFramebuffer imported {};
        std::uint64_t last_used {};
    };

    struct DumbBuffer {
        std::uint32_t handle {};
        std::uint32_t framebuffer {};
        std::uint32_t pitch {};
        std::uint64_t size {};
        void* map {};
    };

    bool open_card();
    bool choose_connector_and_mode(std::uint32_t requested_width, std::uint32_t requested_height, std::uint32_t requested_refresh_hz);
    bool create_primary_buffer();
    bool choose_video_plane(std::uint32_t drm_format, std::uint64_t modifier, int preferred_plane_id);
    bool configure_video_plane();
    bool create_cpu_video_buffer(std::uint32_t width, std::uint32_t height);
    bool import_frame(const DmabufVideoFrame& frame, ImportedFramebuffer& imported);
    bool make_frame_key(const DmabufVideoFrame& frame, FrameKey& key);
    ImportedFramebuffer* find_or_import_cached_framebuffer(const DmabufVideoFrame& frame);
    void evict_cached_framebuffer_if_needed();
    void wait_for_vblank();
    void destroy_imported(ImportedFramebuffer& imported);
    void destroy_framebuffer_cache();
    void destroy_cpu_video_buffer();
    void destroy_primary_buffer();
    void cleanup();

    int drm_fd_ { -1 };
    std::uint32_t connector_id_ {};
    std::uint32_t crtc_id_ {};
    int crtc_index_ { -1 };
    std::uint32_t video_plane_id_ {};
    std::uint32_t video_plane_format_ {};
    std::uint64_t video_plane_modifier_ {};
    int preferred_plane_id_ { -1 };
    std::uint32_t display_width_ {};
    std::uint32_t display_height_ {};
    void* original_crtc_ {};
    void* mode_ {};
    DumbBuffer primary_ {};
    DumbBuffer cpu_video_ {};
    std::uint32_t cpu_video_width_ {};
    std::uint32_t cpu_video_height_ {};
    std::vector<CachedFramebuffer> framebuffer_cache_;
    std::uint64_t frame_serial_ {};
    std::uint32_t current_framebuffer_ {};
    bool vblank_wait_enabled_ {};
    bool vblank_wait_failed_ {};
    std::string card_path_;
    std::string last_error_;
};

bool kms_dmabuf_video_plane_available();

} // namespace glide::dev
