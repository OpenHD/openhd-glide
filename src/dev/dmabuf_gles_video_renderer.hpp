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

#pragma once

#include "dev/kms_dmabuf_video_plane.hpp"
#include "glide_flow/fps_overlay.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace glide::dev {

class DmabufGlesVideoRenderer {
public:
    DmabufGlesVideoRenderer() = default;
    ~DmabufGlesVideoRenderer();

    DmabufGlesVideoRenderer(const DmabufGlesVideoRenderer&) = delete;
    DmabufGlesVideoRenderer& operator=(const DmabufGlesVideoRenderer&) = delete;

    bool draw(const DmabufVideoFrame& frame, flow::SurfaceSize surface);
    const std::string& last_error() const;

private:
    struct ImageKey {
        std::uint64_t device {};
        std::uint64_t inode {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t format {};
        std::uint32_t plane_count {};
        std::uint32_t strides[4] {};
        std::uint32_t offsets[4] {};
        std::uint64_t modifiers[4] {};
    };

    struct CachedImage {
        ImageKey key {};
        void* image {};
        std::uint32_t texture {};
        std::uint64_t last_used {};
    };

    bool initialize();
    bool make_key(const DmabufVideoFrame& frame, ImageKey& key);
    CachedImage* find_or_import(const DmabufVideoFrame& frame);
    bool import_image(const DmabufVideoFrame& frame, const ImageKey& key, CachedImage& image);
    void evict_if_needed();
    void destroy_image(CachedImage& image);
    void cleanup();

    void* egl_display_ {};
    void* create_image_ {};
    void* destroy_image_ {};
    void* bind_image_ {};
    std::uint32_t program_ {};
    std::int32_t position_location_ { -1 };
    std::int32_t texcoord_location_ { -1 };
    std::int32_t sampler_location_ { -1 };
    std::vector<CachedImage> images_;
    std::uint64_t serial_ {};
    bool initialized_ {};
    std::string last_error_;
};

} // namespace glide::dev
