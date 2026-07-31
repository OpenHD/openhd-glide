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

#include "dev/kms_atomic_compositor.hpp"
#include "common/logging.hpp"

#if OPENHD_GLIDE_HAS_KMS_GBM
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/types.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#endif

namespace glide::dev {

bool kms_atomic_compositor_available()
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    return true;
#else
    return false;
#endif
}

KmsAtomicCompositor::~KmsAtomicCompositor()
{
    cleanup();
}

#if OPENHD_GLIDE_HAS_KMS_GBM
namespace {

std::string errno_suffix()
{
    return std::string(": ") + std::strerror(errno);
}

bool is_rk3588_device()
{
    static const bool detected = [] {
        const int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }

        std::string compatible;
        char buffer[256];
        ssize_t bytes_read {};
        while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
            compatible.append(buffer, static_cast<std::size_t>(bytes_read));
        }
        close(fd);
        return compatible.find("rockchip,rk3588") != std::string::npos;
    }();
    return detected;
}

void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void* data)
{
    if (data != nullptr) {
        *static_cast<bool*>(data) = true;
    }
}

std::string egl_error_message(const char* prefix)
{
    std::ostringstream stream;
    stream << prefix << " EGL error=0x" << std::hex << eglGetError();
    return stream.str();
}

std::uint32_t property_id(int drm_fd, std::uint32_t object_id, std::uint32_t object_type, const char* name)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (properties == nullptr) {
        return 0;
    }

    std::uint32_t id {};
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr && std::strcmp(property->name, name) == 0) {
            id = property->prop_id;
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (id != 0) {
            break;
        }
    }

    drmModeFreeObjectProperties(properties);
    return id;
}

bool add_property(
    int drm_fd,
    drmModeAtomicReq* request,
    std::uint32_t object_id,
    std::uint32_t object_type,
    const char* name,
    std::uint64_t value,
    std::string& error)
{
    const auto id = property_id(drm_fd, object_id, object_type, name);
    if (id == 0) {
        error = std::string("missing atomic KMS property ") + name;
        return false;
    }
    if (drmModeAtomicAddProperty(request, object_id, id, value) < 0) {
        error = std::string("failed to add atomic KMS property ") + name + errno_suffix();
        return false;
    }
    return true;
}

bool add_optional_property(
    int drm_fd,
    drmModeAtomicReq* request,
    std::uint32_t object_id,
    std::uint32_t object_type,
    const char* name,
    std::uint64_t value,
    std::string& error)
{
    const auto id = property_id(drm_fd, object_id, object_type, name);
    if (id == 0) {
        return true;
    }
    if (drmModeAtomicAddProperty(request, object_id, id, value) < 0) {
        error = std::string("failed to add atomic KMS property ") + name + errno_suffix();
        return false;
    }
    return true;
}

bool add_optional_range_edge_property(
    int drm_fd,
    drmModeAtomicReq* request,
    std::uint32_t object_id,
    std::uint32_t object_type,
    const char* name,
    bool maximum,
    std::string& error)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (properties == nullptr) {
        return true;
    }

    bool ok { true };
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr
            && std::strcmp(property->name, name) == 0
            && (property->flags & DRM_MODE_PROP_RANGE) != 0
            && (property->flags & DRM_MODE_PROP_IMMUTABLE) == 0
            && property->count_values >= 2) {
            const auto value = maximum ? property->values[1] : property->values[0];
            ok = add_optional_property(drm_fd, request, object_id, object_type, name, value, error);
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (!ok) {
            break;
        }
    }

    drmModeFreeObjectProperties(properties);
    return ok;
}

bool add_optional_range_from_bottom_property(
    int drm_fd,
    drmModeAtomicReq* request,
    std::uint32_t object_id,
    std::uint32_t object_type,
    const char* name,
    std::uint64_t offset_from_bottom,
    std::string& error)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (properties == nullptr) {
        return true;
    }

    bool ok { true };
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr
            && std::strcmp(property->name, name) == 0
            && (property->flags & DRM_MODE_PROP_RANGE) != 0
            && (property->flags & DRM_MODE_PROP_IMMUTABLE) == 0
            && property->count_values >= 2) {
            const auto minimum = property->values[0];
            const auto maximum = property->values[1];
            const auto value = minimum + offset_from_bottom < maximum ? minimum + offset_from_bottom : maximum;
            ok = add_optional_property(drm_fd, request, object_id, object_type, name, value, error);
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (!ok) {
            break;
        }
    }

    drmModeFreeObjectProperties(properties);
    return ok;
}

bool add_optional_enum_property(
    int drm_fd,
    drmModeAtomicReq* request,
    std::uint32_t object_id,
    std::uint32_t object_type,
    const char* name,
    const char* enum_name,
    std::string& error)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (properties == nullptr) {
        return true;
    }

    bool ok { true };
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr
            && std::strcmp(property->name, name) == 0
            && (property->flags & DRM_MODE_PROP_ENUM) != 0
            && (property->flags & DRM_MODE_PROP_IMMUTABLE) == 0) {
            for (int j = 0; j < property->count_enums; ++j) {
                if (std::strcmp(property->enums[j].name, enum_name) == 0) {
                    ok = add_optional_property(drm_fd, request, object_id, object_type, name, property->enums[j].value, error);
                    break;
                }
            }
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (!ok) {
            break;
        }
    }

    drmModeFreeObjectProperties(properties);
    return ok;
}

bool get_property_value(int drm_fd, std::uint32_t object_id, std::uint32_t object_type, const char* name, std::uint64_t& value)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (properties == nullptr) {
        return false;
    }

    bool found {};
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr && std::strcmp(property->name, name) == 0) {
            value = properties->prop_values[i];
            found = true;
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (found) {
            break;
        }
    }

    drmModeFreeObjectProperties(properties);
    return found;
}

bool set_range_edge(int drm_fd, std::uint32_t object_id, std::uint32_t object_type, const char* name, bool maximum)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, object_id, object_type);
    if (properties == nullptr) {
        return false;
    }

    bool set {};
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr
            && std::strcmp(property->name, name) == 0
            && (property->flags & DRM_MODE_PROP_RANGE) != 0
            && property->count_values >= 2) {
            const auto value = maximum ? property->values[1] : property->values[0];
            set = drmModeObjectSetProperty(drm_fd, object_id, object_type, property->prop_id, value) == 0;
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (set) {
            break;
        }
    }

    drmModeFreeObjectProperties(properties);
    return set;
}

std::uint32_t plane_type(int drm_fd, std::uint32_t plane_id)
{
    std::uint64_t value {};
    get_property_value(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", value);
    return static_cast<std::uint32_t>(value);
}

std::uint64_t plane_name_value(int drm_fd, std::uint32_t plane_id)
{
    std::uint64_t value {};
    get_property_value(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE, "NAME", value);
    return value;
}

bool plane_is_vop2_secondary_area(int drm_fd, std::uint32_t plane_id)
{
    const auto name = plane_name_value(drm_fd, plane_id);
    return name == 0x2 || name == 0x4 || name == 0x8
        || name == 0x2000 || name == 0x4000 || name == 0x8000
        || name == 0x20000;
}

bool plane_supports_format(drmModePlane* plane, std::uint32_t format)
{
    return std::find(plane->formats, plane->formats + plane->count_formats, format) != plane->formats + plane->count_formats;
}

bool plane_can_host_alpha_overlay(std::uint32_t type)
{
    return type == DRM_PLANE_TYPE_OVERLAY || type == DRM_PLANE_TYPE_CURSOR;
}

bool plane_supports_video_scanout_format(drmModePlane* plane)
{
    return plane_supports_format(plane, DRM_FORMAT_NV12)
        || plane_supports_format(plane, DRM_FORMAT_NV21)
        || plane_supports_format(plane, DRM_FORMAT_NV16)
        || plane_supports_format(plane, DRM_FORMAT_YUYV)
        || plane_supports_format(plane, DRM_FORMAT_YVYU)
        || plane_supports_format(plane, DRM_FORMAT_VYUY);
}

constexpr std::uint64_t arm_afbc_16x16_modifier = 0x0800000000000001ULL;
constexpr std::uint64_t arm_afbc_16x16_sparse_modifier = 0x0800000000000041ULL;
constexpr std::uint64_t arm_afbc_16x16_ytr_modifier = 0x0800000000000011ULL;
constexpr std::uint64_t arm_afbc_16x16_ytr_sparse_modifier = 0x0800000000000051ULL;
constexpr std::uint64_t arm_afbc_16x16_cbr_modifier = 0x0800000000000081ULL;
constexpr std::uint64_t arm_afbc_16x16_sparse_cbr_modifier = 0x08000000000000c1ULL;
constexpr std::uint64_t arm_afbc_16x16_ytr_cbr_modifier = 0x0800000000000091ULL;
constexpr std::uint64_t arm_afbc_16x16_ytr_sparse_cbr_modifier = 0x08000000000000d1ULL;
constexpr std::uint64_t arm_afbc_16x16_ytr_split_sparse_modifier = 0x0800000000000071ULL;

struct FormatModifierBlobHeader {
    std::uint32_t version {};
    std::uint32_t flags {};
    std::uint32_t count_formats {};
    std::uint32_t formats_offset {};
    std::uint32_t count_modifiers {};
    std::uint32_t modifiers_offset {};
};

struct FormatModifierRecord {
    std::uint64_t formats {};
    std::uint32_t offset {};
    std::uint32_t pad {};
    std::uint64_t modifier {};
};

static_assert(sizeof(FormatModifierBlobHeader) == 24);
static_assert(sizeof(FormatModifierRecord) == 24);

bool blob_has_range(const drmModePropertyBlobRes* blob, std::uint64_t offset, std::uint64_t count, std::size_t element_size)
{
    if (blob == nullptr || blob->data == nullptr || element_size == 0) {
        return false;
    }
    const auto length = static_cast<std::uint64_t>(blob->length);
    if (offset > length) {
        return false;
    }
    return count <= (length - offset) / static_cast<std::uint64_t>(element_size);
}

template <typename T>
bool read_blob_value(const drmModePropertyBlobRes* blob, std::uint64_t offset, T& value)
{
    if (!blob_has_range(blob, offset, 1, sizeof(T))) {
        return false;
    }
    std::memcpy(&value, static_cast<const unsigned char*>(blob->data) + offset, sizeof(T));
    return true;
}

bool in_formats_blob_supports_modifier(const drmModePropertyBlobRes* blob, std::uint32_t format, std::uint64_t modifier)
{
    constexpr std::uint32_t format_blob_current_version = 1;

    FormatModifierBlobHeader header {};
    if (!read_blob_value(blob, 0, header)) {
        return false;
    }
    if (header.version != format_blob_current_version || header.flags != 0) {
        return false;
    }
    if (!blob_has_range(blob, header.formats_offset, header.count_formats, sizeof(std::uint32_t))
        || !blob_has_range(blob, header.modifiers_offset, header.count_modifiers, sizeof(FormatModifierRecord))) {
        return false;
    }

    for (std::uint32_t format_index = 0; format_index < header.count_formats; ++format_index) {
        std::uint32_t blob_format {};
        const auto format_offset = static_cast<std::uint64_t>(header.formats_offset)
            + static_cast<std::uint64_t>(format_index) * sizeof(blob_format);
        if (!read_blob_value(blob, format_offset, blob_format)) {
            return false;
        }
        if (blob_format != format) {
            continue;
        }

        for (std::uint32_t modifier_index = 0; modifier_index < header.count_modifiers; ++modifier_index) {
            FormatModifierRecord record {};
            const auto modifier_offset = static_cast<std::uint64_t>(header.modifiers_offset)
                + static_cast<std::uint64_t>(modifier_index) * sizeof(record);
            if (!read_blob_value(blob, modifier_offset, record)) {
                return false;
            }
            if (record.modifier != modifier || format_index < record.offset) {
                continue;
            }

            const auto bit = format_index - record.offset;
            if (bit < 64U && (record.formats & (std::uint64_t { 1 } << bit)) != 0) {
                return true;
            }
        }
    }

    return false;
}

bool plane_supports_format_modifier(int drm_fd, std::uint32_t plane_id, std::uint32_t format, std::uint64_t modifier)
{
    auto* properties = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (properties == nullptr) {
        return true;
    }

    std::uint64_t blob_id {};
    bool has_in_formats {};
    for (std::uint32_t i = 0; i < properties->count_props; ++i) {
        auto* property = drmModeGetProperty(drm_fd, properties->props[i]);
        if (property != nullptr && std::strcmp(property->name, "IN_FORMATS") == 0) {
            blob_id = properties->prop_values[i];
            has_in_formats = true;
        }
        if (property != nullptr) {
            drmModeFreeProperty(property);
        }
        if (has_in_formats) {
            break;
        }
    }
    drmModeFreeObjectProperties(properties);

    if (!has_in_formats || blob_id == 0) {
        return true;
    }

    auto* blob = drmModeGetPropertyBlob(drm_fd, static_cast<std::uint32_t>(blob_id));
    if (blob == nullptr) {
        return true;
    }

    const bool supported = in_formats_blob_supports_modifier(blob, format, modifier);
    drmModeFreePropertyBlob(blob);
    return supported;
}

std::string fourcc_string(std::uint32_t format)
{
    std::array<char, 5> text {
        static_cast<char>(format & 0xffU),
        static_cast<char>((format >> 8U) & 0xffU),
        static_cast<char>((format >> 16U) & 0xffU),
        static_cast<char>((format >> 24U) & 0xffU),
        '\0',
    };
    return text.data();
}

drmModeModeInfo vesa_1280x960_60_mode()
{
    drmModeModeInfo mode {};
    mode.clock = 108000;
    mode.hdisplay = 1280;
    mode.hsync_start = 1376;
    mode.hsync_end = 1488;
    mode.htotal = 1800;
    mode.vdisplay = 960;
    mode.vsync_start = 961;
    mode.vsync_end = 964;
    mode.vtotal = 1000;
    mode.vrefresh = 60;
    mode.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    mode.type = DRM_MODE_TYPE_USERDEF;
    std::snprintf(mode.name, sizeof(mode.name), "1280x960");
    return mode;
}

bool can_use_builtin_mode(std::uint32_t requested_width, std::uint32_t requested_height, std::uint32_t requested_refresh_hz, drmModeModeInfo& mode)
{
    if (requested_width == 1280 && requested_height == 960 && (requested_refresh_hz == 0 || requested_refresh_hz == 60)) {
        mode = vesa_1280x960_60_mode();
        return true;
    }
    return false;
}

std::string drm_driver_name(int drm_fd)
{
    auto* version = drmGetVersion(drm_fd);
    if (version == nullptr) {
        return {};
    }
    std::string name;
    if (version->name != nullptr && version->name_len > 0) {
        name.assign(version->name, version->name + version->name_len);
    }
    drmFreeVersion(version);
    return name;
}

void configure_mesa_runtime_for_board()
{
    if (access("/usr/lib/aarch64-linux-gnu/dri/sun4i-drm_dri.so", R_OK) != 0) {
        return;
    }
    if (std::getenv("LIBGL_DRIVERS_PATH") == nullptr) {
        setenv("LIBGL_DRIVERS_PATH", "/usr/lib/aarch64-linux-gnu/dri", 0);
    }
}

} // namespace
#endif

bool KmsAtomicCompositor::create(
    std::uint32_t requested_width,
    std::uint32_t requested_height,
    std::uint32_t requested_refresh_hz,
    int preferred_video_plane_id,
    int preferred_flow_plane_id,
    int preferred_ui_plane_id,
    std::uint32_t flow_render_width,
    std::uint32_t flow_render_height,
    std::uint32_t ui_width,
    std::uint32_t ui_height,
    bool primary_flow_readback)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    preferred_video_plane_id_ = preferred_video_plane_id;
    preferred_flow_plane_id_ = preferred_flow_plane_id;
    preferred_ui_plane_id_ = preferred_ui_plane_id;
    ui_surface_.width = ui_width;
    ui_surface_.height = ui_height;
    primary_flow_readback_ = primary_flow_readback;
    if (primary_flow_readback_) {
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "Flow primary-plane readback mode enabled; keeping Flow FB_ID stable");
    }
    return open_card()
        && choose_connector_and_mode(requested_width, requested_height, requested_refresh_hz)
        && [&] {
            flow_surface_ = surface_;
            if (ui_surface_.width != 0 && ui_surface_.height != 0) {
                const auto requested_ui_width = ui_surface_.width;
                const auto requested_ui_height = ui_surface_.height;
                ui_surface_.width = std::min(ui_surface_.width, surface_.width);
                ui_surface_.height = std::min(ui_surface_.height, surface_.height);
                if (ui_surface_.width != requested_ui_width || ui_surface_.height != requested_ui_height) {
                    glide::log(
                        glide::LogLevel::warning,
                        "OpenHD-Glide",
                        "UI overlay surface clamped from " + std::to_string(requested_ui_width) + "x"
                            + std::to_string(requested_ui_height) + " to "
                            + std::to_string(ui_surface_.width) + "x" + std::to_string(ui_surface_.height)
                            + " for selected KMS mode "
                            + std::to_string(surface_.width) + "x" + std::to_string(surface_.height));
                }
            }
            if (!primary_flow_readback_ && flow_render_width != 0 && flow_render_height != 0) {
                flow_surface_.width = std::clamp(flow_render_width, 64U, surface_.width);
                flow_surface_.height = std::clamp(flow_render_height, 64U, surface_.height);
                if (flow_surface_.width != surface_.width || flow_surface_.height != surface_.height) {
                    glide::log(
                        glide::LogLevel::info,
                        "OpenHD-Glide",
                        "Flow render surface scaled to " + std::to_string(flow_surface_.width) + "x"
                            + std::to_string(flow_surface_.height) + " and KMS-scaled to "
                            + std::to_string(surface_.width) + "x" + std::to_string(surface_.height));
                }
            }
            return true;
        }()
        && create_primary_buffer()
        && create_gbm_device()
        && create_flow_surface()
        && create_egl();
#else
    (void)requested_width;
    (void)requested_height;
    (void)requested_refresh_hz;
    (void)preferred_video_plane_id;
    (void)preferred_flow_plane_id;
    (void)preferred_ui_plane_id;
    (void)flow_render_width;
    (void)flow_render_height;
    (void)ui_width;
    (void)ui_height;
    (void)primary_flow_readback;
    last_error_ = "atomic KMS compositor support was not found at build time";
    return false;
#endif
}

bool KmsAtomicCompositor::present(const DmabufVideoFrame& video_frame, bool update_flow_frame)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    const auto video_modifier = video_frame.modifiers[0];
    if (video_plane_id_ == 0 || video_plane_format_ != video_frame.drm_format || video_plane_modifier_ != video_modifier) {
        video_plane_id_ = 0;
        video_plane_format_ = 0;
        video_plane_modifier_ = 0;
        video_on_primary_ = false;
        destroy_video_framebuffer_cache();
        if (!choose_video_plane(video_frame.drm_format, video_modifier)) {
            return false;
        }
    }
    if (!primary_flow_readback_ && flow_plane_id_ == 0 && !choose_flow_plane()) {
        return false;
    }
    if (ui_surface_.width != 0 && ui_surface_.height != 0 && ui_plane_id_ == 0 && !choose_ui_plane()) {
        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            "UI KMS overlay disabled: " + last_error_);
        ui_surface_ = {};
    }

    auto* video = find_or_import_video_framebuffer(video_frame);
    if (video == nullptr) {
        return false;
    }

    std::uint32_t flow_framebuffer {};
    bool flow_is_solid_dumb {};
    void* flow_bo {};
    if (!primary_flow_readback_ && !acquire_flow_framebuffer(update_flow_frame, flow_framebuffer, flow_bo, flow_is_solid_dumb)) {
        return false;
    }

    // The decoder releases the previously presented MPP frame immediately
    // after this function returns. A nonblocking atomic commit can return
    // before the page flip, while KMS is still scanning that previous DMABUF,
    // allowing MPP to reuse and overwrite it mid-scan. Keep decoded-DMABUF
    // commits synchronous so return also marks the safe buffer-release point.
    if (!commit(video->framebuffer, video_frame.width, video_frame.height,
                flow_framebuffer, solid_ui_.framebuffer, false)) {
        release_acquired_flow_framebuffer(flow_framebuffer, flow_bo, flow_is_solid_dumb);
        return false;
    }

    current_video_frame_ = video_frame;
    current_video_framebuffer_ = video->framebuffer;
    current_video_width_ = video_frame.width;
    current_video_height_ = video_frame.height;
    adopt_flow_framebuffer(flow_framebuffer, flow_bo, flow_is_solid_dumb);
    return true;
#else
    (void)video_frame;
    (void)update_flow_frame;
    return false;
#endif
}

bool KmsAtomicCompositor::present(const CpuVideoFrame& video_frame, bool update_flow_frame)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (video_frame.width == 0 || video_frame.height == 0 || video_frame.pixels == nullptr || video_frame.stride < video_frame.width * 4U) {
        last_error_ = "invalid CPU video frame";
        return false;
    }

    constexpr auto cpu_frame_format = DRM_FORMAT_XRGB8888;
    if (video_plane_id_ == 0 || video_plane_format_ != cpu_frame_format || video_plane_modifier_ != DRM_FORMAT_MOD_LINEAR) {
        video_plane_id_ = 0;
        video_plane_format_ = 0;
        video_plane_modifier_ = 0;
        video_on_primary_ = false;
        destroy_video_framebuffer_cache();
        if (!choose_video_plane(cpu_frame_format, DRM_FORMAT_MOD_LINEAR)) {
            return false;
        }
    }
    if (cpu_video_.framebuffer == 0 || cpu_video_width_ != video_frame.width || cpu_video_height_ != video_frame.height) {
        destroy_cpu_video_buffer();
        if (!create_cpu_video_buffer(video_frame.width, video_frame.height)) {
            return false;
        }
    }
    if (!primary_flow_readback_ && flow_plane_id_ == 0 && !choose_flow_plane()) {
        return false;
    }
    if (ui_surface_.width != 0 && ui_surface_.height != 0 && ui_plane_id_ == 0 && !choose_ui_plane()) {
        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            "UI KMS overlay disabled: " + last_error_);
        ui_surface_ = {};
    }

    auto* dst = static_cast<unsigned char*>(cpu_video_.map);
    const auto* src = static_cast<const unsigned char*>(video_frame.pixels);
    const auto row_bytes = static_cast<std::size_t>(video_frame.width) * 4U;
    for (std::uint32_t y = 0; y < video_frame.height; ++y) {
        std::memcpy(
            dst + static_cast<std::size_t>(y) * cpu_video_.pitch,
            src + static_cast<std::size_t>(y) * video_frame.stride,
            row_bytes);
    }

    std::uint32_t flow_framebuffer {};
    bool flow_is_solid_dumb {};
    void* flow_bo {};
    if (!primary_flow_readback_ && !acquire_flow_framebuffer(update_flow_frame, flow_framebuffer, flow_bo, flow_is_solid_dumb)) {
        return false;
    }

    const bool video_only_update = !update_flow_frame && flow_bo == nullptr;
    if (!commit(cpu_video_.framebuffer, video_frame.width, video_frame.height, flow_framebuffer, solid_ui_.framebuffer, video_only_update)) {
        release_acquired_flow_framebuffer(flow_framebuffer, flow_bo, flow_is_solid_dumb);
        return false;
    }

    current_video_framebuffer_ = cpu_video_.framebuffer;
    current_video_width_ = video_frame.width;
    current_video_height_ = video_frame.height;
    adopt_flow_framebuffer(flow_framebuffer, flow_bo, flow_is_solid_dumb);
    return true;
#else
    (void)video_frame;
    (void)update_flow_frame;
    return false;
#endif
}

bool KmsAtomicCompositor::present_overlay(bool update_flow_frame)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (!primary_flow_readback_ && flow_plane_id_ == 0 && !choose_flow_plane()) {
        return false;
    }
    if (ui_surface_.width != 0 && ui_surface_.height != 0 && ui_plane_id_ == 0 && !choose_ui_plane()) {
        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            "UI KMS overlay disabled: " + last_error_);
        ui_surface_ = {};
    }

    std::uint32_t flow_framebuffer {};
    bool flow_is_solid_dumb {};
    void* flow_bo {};
    if (!primary_flow_readback_ && !acquire_flow_framebuffer(update_flow_frame, flow_framebuffer, flow_bo, flow_is_solid_dumb)) {
        return false;
    }

    if (!commit(current_video_framebuffer_, current_video_width_, current_video_height_, flow_framebuffer, solid_ui_.framebuffer)) {
        release_acquired_flow_framebuffer(flow_framebuffer, flow_bo, flow_is_solid_dumb);
        return false;
    }

    adopt_flow_framebuffer(flow_framebuffer, flow_bo, flow_is_solid_dumb);
    return true;
#else
    (void)update_flow_frame;
    return false;
#endif
}

void KmsAtomicCompositor::set_vblank_wait_enabled(bool enabled)
{
    vblank_wait_enabled_ = enabled;
    vblank_wait_failed_ = false;
}

bool KmsAtomicCompositor::enable_writeback_recording(const std::string& path, std::uint32_t every_n_frames, std::uint32_t max_frames)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (path.empty()) {
        last_error_ = "writeback recording path is empty";
        return false;
    }
    if (surface_.width == 0 || surface_.height == 0) {
        last_error_ = "writeback recording needs an initialized KMS surface";
        return false;
    }
    if (!choose_writeback_connector()) {
        return false;
    }
    if (!create_writeback_buffer()) {
        return false;
    }

    const auto output_fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (output_fd < 0) {
        last_error_ = "failed to open writeback recording output " + path + errno_suffix();
        destroy_writeback_buffer();
        return false;
    }

    writeback_.output_fd = output_fd;
    writeback_.every_n_frames = every_n_frames == 0 ? 1 : every_n_frames;
    writeback_.max_frames = max_frames;
    writeback_.captured_frames = 0;
    writeback_.enabled = true;
    glide::log(
        glide::LogLevel::info,
        "OpenHD-Glide",
        "KMS writeback raw recorder enabled connector=" + std::to_string(writeback_.connector_id)
            + " output=" + path
            + " frame_bytes=" + std::to_string(writeback_.buffer.size)
            + " every_n_frames=" + std::to_string(writeback_.every_n_frames)
            + " max_frames=" + std::to_string(writeback_.max_frames));
    return true;
#else
    (void)path;
    (void)every_n_frames;
    (void)max_frames;
    last_error_ = "KMS writeback recording support was not found at build time";
    return false;
#endif
}

bool KmsAtomicCompositor::writeback_recording_finished() const
{
    return writeback_.enabled && writeback_.max_frames != 0 && writeback_.captured_frames >= writeback_.max_frames;
}

bool KmsAtomicCompositor::make_flow_context_current()
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    auto display = static_cast<EGLDisplay>(egl_display_);
    auto surface = static_cast<EGLSurface>(egl_surface_);
    auto context = static_cast<EGLContext>(egl_context_);
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT) {
        last_error_ = "Flow EGL context is unavailable";
        return false;
    }
    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        last_error_ = egl_error_message("failed to make Flow EGL context current");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool KmsAtomicCompositor::release_flow_context()
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    auto display = static_cast<EGLDisplay>(egl_display_);
    if (display == EGL_NO_DISPLAY) {
        return true;
    }
    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
        last_error_ = egl_error_message("failed to release Flow EGL context");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool KmsAtomicCompositor::publish_rendered_flow_frame()
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    std::uint32_t framebuffer {};
    void* bo {};
    if (!lock_flow_framebuffer(framebuffer, bo)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(flow_framebuffer_mutex_);
    if (pending_flow_bo_ != nullptr) {
        gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(pending_flow_bo_));
        pending_flow_bo_ = nullptr;
    }
    pending_flow_framebuffer_ = 0;
    pending_flow_framebuffer_ = framebuffer;
    pending_flow_bo_ = bo;
    pending_flow_is_solid_dumb_ = false;
    return true;
#else
    return false;
#endif
}

bool KmsAtomicCompositor::publish_rendered_flow_frame_to_primary()
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (!primary_flow_readback_) {
        last_error_ = "primary Flow readback mode is not enabled";
        return false;
    }
    return read_flow_frame_into_primary();
#else
    return false;
#endif
}

bool KmsAtomicCompositor::publish_solid_flow_frame(std::uint32_t argb)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (solid_flow_.framebuffer == 0 && !create_solid_flow_buffer()) {
        return false;
    }
    if (solid_flow_.map == nullptr) {
        last_error_ = "solid Flow debug buffer is not mapped";
        return false;
    }

    for (std::uint32_t y = 0; y < flow_surface_.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(
            static_cast<unsigned char*>(solid_flow_.map) + static_cast<std::size_t>(y) * solid_flow_.pitch);
        std::fill(row, row + flow_surface_.width, argb);
    }
    msync(solid_flow_.map, solid_flow_.size, MS_SYNC);
    drmModeDirtyFB(drm_fd_, solid_flow_.framebuffer, nullptr, 0);

    std::lock_guard<std::mutex> lock(flow_framebuffer_mutex_);
    if (pending_flow_bo_ != nullptr) {
        gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(pending_flow_bo_));
    }
    pending_flow_framebuffer_ = solid_flow_.framebuffer;
    pending_flow_bo_ = nullptr;
    pending_flow_is_solid_dumb_ = true;
    return true;
#else
    return false;
#endif
}

bool KmsAtomicCompositor::publish_solid_ui_frame(std::uint32_t argb)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (ui_surface_.width == 0 || ui_surface_.height == 0) {
        last_error_ = "UI overlay surface is disabled";
        return false;
    }
    if (solid_ui_.framebuffer == 0 && !create_solid_ui_buffer()) {
        return false;
    }
    if (solid_ui_.map == nullptr) {
        last_error_ = "solid UI debug buffer is not mapped";
        return false;
    }

    for (std::uint32_t y = 0; y < ui_surface_.height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(
            static_cast<unsigned char*>(solid_ui_.map) + static_cast<std::size_t>(y) * solid_ui_.pitch);
        std::fill(row, row + ui_surface_.width, argb);
    }
    msync(solid_ui_.map, solid_ui_.size, MS_SYNC);
    drmModeDirtyFB(drm_fd_, solid_ui_.framebuffer, nullptr, 0);
    return true;
#else
    (void)argb;
    return false;
#endif
}

bool KmsAtomicCompositor::publish_ui_frame_from_argb(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride_bytes)
{
#if OPENHD_GLIDE_HAS_KMS_GBM
    if (pixels == nullptr) {
        last_error_ = "UI buffer pixels are null";
        return false;
    }
    if (ui_surface_.width == 0 || ui_surface_.height == 0) {
        return true;
    }
    if (width != ui_surface_.width || height != ui_surface_.height) {
        last_error_ = "UI buffer dimensions do not match the KMS UI plane";
        return false;
    }
    if (stride_bytes < width * 4U) {
        last_error_ = "UI buffer stride is too small";
        return false;
    }
    if (solid_ui_.framebuffer == 0 && !create_solid_ui_buffer()) {
        return false;
    }
    if (solid_ui_.map == nullptr) {
        last_error_ = "solid UI buffer is not mapped";
        return false;
    }

    const auto* source = static_cast<const unsigned char*>(pixels);
    auto* destination = static_cast<unsigned char*>(solid_ui_.map);
    const auto copy_bytes = static_cast<std::size_t>(width) * 4U;
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(
            destination + static_cast<std::size_t>(y) * solid_ui_.pitch,
            source + static_cast<std::size_t>(y) * stride_bytes,
            copy_bytes);
    }
    msync(solid_ui_.map, solid_ui_.size, MS_ASYNC);
    drmModeDirtyFB(drm_fd_, solid_ui_.framebuffer, nullptr, 0);
    return true;
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride_bytes;
    return false;
#endif
}

bool KmsAtomicCompositor::ui_overlay_plane_active() const
{
    return ui_surface_.width != 0 && ui_surface_.height != 0 && ui_plane_id_ != 0;
}

flow::SurfaceSize KmsAtomicCompositor::surface_size() const
{
    return flow_surface_.width != 0 && flow_surface_.height != 0 ? flow_surface_ : surface_;
}

const std::string& KmsAtomicCompositor::last_error() const
{
    return last_error_;
}

#if OPENHD_GLIDE_HAS_KMS_GBM
bool KmsAtomicCompositor::open_card()
{
    for (const auto* path : { "/dev/dri/card1", "/dev/dri/card0", "/dev/dri/card2" }) {
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        auto* resources = drmModeGetResources(fd);
        if (resources == nullptr) {
            close(fd);
            continue;
        }

        bool has_connected_connector {};
        for (int i = 0; i < resources->count_connectors; ++i) {
            auto* connector = drmModeGetConnector(fd, resources->connectors[i]);
            if (connector != nullptr
                && connector->connection == DRM_MODE_CONNECTED
                && connector->count_modes > 0) {
                has_connected_connector = true;
            }
            if (connector != nullptr) {
                drmModeFreeConnector(connector);
            }
            if (has_connected_connector) {
                break;
            }
        }

        drmModeFreeResources(resources);
        if (!has_connected_connector) {
            close(fd);
            continue;
        }

        drm_fd_ = fd;
        card_path_ = path;
        drmSetClientCap(drm_fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        if (drmSetClientCap(drm_fd_, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
            last_error_ = "failed to enable DRM atomic client capability" + errno_suffix();
            close(drm_fd_);
            drm_fd_ = -1;
            return false;
        }
        return true;
    }

    last_error_ = "no DRM card with a connected connector was found";
    return false;
}

bool KmsAtomicCompositor::choose_connector_and_mode(std::uint32_t requested_width, std::uint32_t requested_height, std::uint32_t requested_refresh_hz)
{
    auto* resources = drmModeGetResources(drm_fd_);
    if (resources == nullptr) {
        last_error_ = "failed to read DRM resources";
        return false;
    }

    drmModeConnector* chosen_connector {};
    for (int i = 0; i < resources->count_connectors; ++i) {
        auto* connector = drmModeGetConnector(drm_fd_, resources->connectors[i]);
        if (connector != nullptr
            && connector->connection == DRM_MODE_CONNECTED
            && connector->count_modes > 0) {
            chosen_connector = connector;
            break;
        }
        if (connector != nullptr) {
            drmModeFreeConnector(connector);
        }
    }

    if (chosen_connector == nullptr) {
        drmModeFreeResources(resources);
        last_error_ = "no connected DRM connector with modes was found";
        return false;
    }

    drmModeModeInfo selected_mode = chosen_connector->modes[0];
    drmModeModeInfo highest_refresh_mode = chosen_connector->modes[0];
    bool found_resolution = false;
    bool found_exact_refresh = false;
    bool found_refresh_fallback = false;
    std::uint64_t best_refresh_fallback_score = std::numeric_limits<std::uint64_t>::max();
    for (int i = 0; i < chosen_connector->count_modes; ++i) {
        const auto& candidate = chosen_connector->modes[i];
        if (candidate.vrefresh > highest_refresh_mode.vrefresh) {
            highest_refresh_mode = candidate;
        }
        if (requested_refresh_hz != 0 && static_cast<std::uint32_t>(candidate.vrefresh) == requested_refresh_hz) {
            const auto aspect_error = static_cast<std::uint64_t>(std::llabs(
                static_cast<long long>(candidate.hdisplay) * static_cast<long long>(requested_height)
                - static_cast<long long>(requested_width) * static_cast<long long>(candidate.vdisplay)));
            const auto area = static_cast<std::uint64_t>(candidate.hdisplay) * static_cast<std::uint64_t>(candidate.vdisplay);
            const auto requested_area = static_cast<std::uint64_t>(requested_width) * static_cast<std::uint64_t>(requested_height);
            const auto area_error = area > requested_area ? area - requested_area : requested_area - area;
            const auto score = aspect_error * 100000000ULL + area_error;
            if (!found_refresh_fallback || score < best_refresh_fallback_score) {
                selected_mode = candidate;
                best_refresh_fallback_score = score;
                found_refresh_fallback = true;
            }
        }
        if (candidate.hdisplay != requested_width || candidate.vdisplay != requested_height) {
            continue;
        }
        if (!found_resolution) {
            selected_mode = candidate;
            found_resolution = true;
        }
        if (requested_refresh_hz != 0 && static_cast<std::uint32_t>(candidate.vrefresh) == requested_refresh_hz) {
            selected_mode = candidate;
            found_exact_refresh = true;
            break;
        }
        if (candidate.vrefresh > selected_mode.vrefresh) {
            selected_mode = candidate;
        }
    }
    if (found_resolution && requested_refresh_hz != 0 && !found_exact_refresh) {
        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            "requested "
                + std::to_string(requested_width) + "x" + std::to_string(requested_height)
                + "@" + std::to_string(requested_refresh_hz)
                + "Hz is unavailable; using highest refresh for that resolution");
    }
    const bool requested_native_mode = requested_width == 0 || requested_height == 0;
    bool using_builtin_mode = false;
    if (requested_native_mode) {
        selected_mode = chosen_connector->modes[0];
    } else if (!found_resolution) {
        using_builtin_mode = can_use_builtin_mode(requested_width, requested_height, requested_refresh_hz, selected_mode);
        if (!using_builtin_mode && requested_refresh_hz == 0) {
            selected_mode = highest_refresh_mode;
        }
    }
    if (found_resolution && requested_refresh_hz != 0 && !found_exact_refresh) {
        if (!found_refresh_fallback) {
            found_refresh_fallback = true;
        }
    }
    if (found_resolution || requested_native_mode || using_builtin_mode) {
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "atomic KMS mode selected "
                + std::to_string(selected_mode.hdisplay) + "x" + std::to_string(selected_mode.vdisplay)
                + "@" + std::to_string(selected_mode.vrefresh)
                + "Hz on connector " + std::to_string(chosen_connector->connector_id)
                + (using_builtin_mode
                        ? " (built-in VESA timing)"
                        : requested_native_mode
                        ? " (connected display preferred mode)"
                        : (requested_refresh_hz != 0 ? (" (requested " + std::to_string(requested_refresh_hz) + "Hz)") : " (highest refresh auto-selection)")));
    } else {
        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            "no exact resolution match for requested "
                + std::to_string(requested_width) + "x" + std::to_string(requested_height)
                + (found_refresh_fallback ? "; using requested-refresh output mode " : "; using highest-refresh connected mode ")
                + std::to_string(selected_mode.hdisplay) + "x" + std::to_string(selected_mode.vdisplay)
                + "@" + std::to_string(selected_mode.vrefresh) + "Hz");
    }

    drmModeEncoder* selected_encoder {};
    if (chosen_connector->encoder_id != 0) {
        selected_encoder = drmModeGetEncoder(drm_fd_, chosen_connector->encoder_id);
    }
    if (selected_encoder == nullptr) {
        for (int i = 0; i < chosen_connector->count_encoders; ++i) {
            auto* encoder = drmModeGetEncoder(drm_fd_, chosen_connector->encoders[i]);
            if (encoder != nullptr) {
                selected_encoder = encoder;
                break;
            }
        }
    }

    if (selected_encoder == nullptr) {
        drmModeFreeConnector(chosen_connector);
        drmModeFreeResources(resources);
        last_error_ = "connected DRM connector has no usable encoder";
        return false;
    }

    std::uint32_t selected_crtc {};
    int selected_crtc_index = -1;
    if (selected_encoder->crtc_id != 0) {
        selected_crtc = selected_encoder->crtc_id;
        for (int i = 0; i < resources->count_crtcs; ++i) {
            if (resources->crtcs[i] == selected_crtc) {
                selected_crtc_index = i;
                break;
            }
        }
    }
    if (selected_crtc == 0) {
        for (int i = 0; i < resources->count_crtcs; ++i) {
            if ((selected_encoder->possible_crtcs & (1 << i)) != 0) {
                selected_crtc = resources->crtcs[i];
                selected_crtc_index = i;
                break;
            }
        }
    }

    if (selected_crtc == 0 || selected_crtc_index < 0) {
        drmModeFreeEncoder(selected_encoder);
        drmModeFreeConnector(chosen_connector);
        drmModeFreeResources(resources);
        last_error_ = "failed to select a DRM CRTC for the connector";
        return false;
    }

    connector_id_ = chosen_connector->connector_id;
    crtc_id_ = selected_crtc;
    crtc_index_ = selected_crtc_index;
    surface_ = flow::SurfaceSize {
        .width = selected_mode.hdisplay,
        .height = selected_mode.vdisplay,
    };
    mode_ = new drmModeModeInfo(selected_mode);
    original_crtc_ = drmModeGetCrtc(drm_fd_, crtc_id_);
    if (drmModeCreatePropertyBlob(drm_fd_, mode_, sizeof(drmModeModeInfo), &mode_blob_id_) != 0) {
        drmModeFreeEncoder(selected_encoder);
        drmModeFreeConnector(chosen_connector);
        drmModeFreeResources(resources);
        last_error_ = "failed to create DRM mode property blob" + errno_suffix();
        return false;
    }

    drmModeFreeEncoder(selected_encoder);
    drmModeFreeConnector(chosen_connector);
    drmModeFreeResources(resources);
    return true;
}

bool KmsAtomicCompositor::create_primary_buffer()
{
    drm_mode_create_dumb create {};
    create.width = surface_.width;
    create.height = surface_.height;
    create.bpp = 32;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        last_error_ = "failed to create primary DRM buffer" + errno_suffix();
        return false;
    }
    primary_.handle = create.handle;
    primary_.pitch = create.pitch;
    primary_.size = create.size;

    std::uint32_t handles[4] { primary_.handle, 0, 0, 0 };
    std::uint32_t strides[4] { primary_.pitch, 0, 0, 0 };
    std::uint32_t offsets[4] {};
    const auto primary_format = primary_flow_readback_ ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    if (drmModeAddFB2(drm_fd_, surface_.width, surface_.height, primary_format, handles, strides, offsets, &primary_.framebuffer, 0) != 0) {
        last_error_ = "failed to create primary DRM framebuffer" + errno_suffix();
        destroy_primary_buffer();
        return false;
    }

    drm_mode_map_dumb map {};
    map.handle = primary_.handle;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        last_error_ = "failed to map primary DRM buffer" + errno_suffix();
        destroy_primary_buffer();
        return false;
    }
    primary_.map = mmap(nullptr, primary_.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, static_cast<off_t>(map.offset));
    if (primary_.map == MAP_FAILED) {
        primary_.map = nullptr;
        last_error_ = "failed to mmap primary DRM buffer" + errno_suffix();
        destroy_primary_buffer();
        return false;
    }
    std::memset(primary_.map, 0, static_cast<std::size_t>(primary_.size));

    auto* planes = drmModeGetPlaneResources(drm_fd_);
    if (planes == nullptr) {
        last_error_ = "failed to read KMS plane resources";
        return false;
    }
    for (std::uint32_t i = 0; i < planes->count_planes; ++i) {
        auto* plane = drmModeGetPlane(drm_fd_, planes->planes[i]);
        if (plane == nullptr) {
            continue;
        }
        const bool usable_crtc = (plane->possible_crtcs & (1 << crtc_index_)) != 0;
        if (usable_crtc && plane_supports_format(plane, primary_format) && plane_type(drm_fd_, plane->plane_id) == DRM_PLANE_TYPE_PRIMARY) {
            primary_plane_id_ = plane->plane_id;
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }
    drmModeFreePlaneResources(planes);
    if (primary_plane_id_ == 0) {
        last_error_ = std::string("no primary KMS plane supports ") + fourcc_string(primary_format);
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::choose_writeback_connector()
{
    if (writeback_.connector_id != 0) {
        return true;
    }

#ifdef DRM_CLIENT_CAP_WRITEBACK_CONNECTORS
    drmSetClientCap(drm_fd_, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
#endif

    for (std::uint32_t id = 1; id < 1024; ++id) {
        auto* connector = drmModeGetConnector(drm_fd_, id);
        if (connector == nullptr) {
            continue;
        }
        const bool has_writeback_props =
            property_id(drm_fd_, id, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID") != 0
            && property_id(drm_fd_, id, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_OUT_FENCE_PTR") != 0;
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
        const bool is_writeback = connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
#else
        const bool is_writeback = has_writeback_props;
#endif
        drmModeFreeConnector(connector);
        if (is_writeback && has_writeback_props) {
            writeback_.connector_id = id;
            return true;
        }
    }

    last_error_ = "no DRM writeback connector with WRITEBACK_FB_ID was found";
    return false;
}

bool KmsAtomicCompositor::create_writeback_buffer()
{
    if (writeback_.buffer.framebuffer != 0) {
        return true;
    }

    auto& buffer = writeback_.buffer;
    drm_mode_create_dumb create {};
    create.width = surface_.width;
    create.height = surface_.height;
    create.bpp = 32;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        last_error_ = "failed to create writeback DRM buffer" + errno_suffix();
        return false;
    }
    buffer.handle = create.handle;
    buffer.pitch = create.pitch;
    buffer.size = create.size;

    std::uint32_t handles[4] { buffer.handle, 0, 0, 0 };
    std::uint32_t strides[4] { buffer.pitch, 0, 0, 0 };
    std::uint32_t offsets[4] {};
    if (drmModeAddFB2(drm_fd_, surface_.width, surface_.height, DRM_FORMAT_ARGB8888, handles, strides, offsets, &buffer.framebuffer, 0) != 0) {
        last_error_ = "failed to create writeback DRM framebuffer" + errno_suffix();
        destroy_writeback_buffer();
        return false;
    }

    drm_mode_map_dumb map {};
    map.handle = buffer.handle;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        last_error_ = "failed to map writeback DRM buffer" + errno_suffix();
        destroy_writeback_buffer();
        return false;
    }
    buffer.map = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, static_cast<off_t>(map.offset));
    if (buffer.map == MAP_FAILED) {
        buffer.map = nullptr;
        last_error_ = "failed to mmap writeback DRM buffer" + errno_suffix();
        destroy_writeback_buffer();
        return false;
    }
    std::memset(buffer.map, 0, static_cast<std::size_t>(buffer.size));
    return true;
}

bool KmsAtomicCompositor::choose_video_plane(std::uint32_t drm_format, std::uint64_t modifier)
{
    auto* planes = drmModeGetPlaneResources(drm_fd_);
    if (planes == nullptr) {
        last_error_ = "failed to read KMS plane resources";
        return false;
    }

    if (preferred_video_plane_id_ >= 0 && static_cast<std::uint32_t>(preferred_video_plane_id_) == primary_plane_id_) {
        auto* plane = drmModeGetPlane(drm_fd_, primary_plane_id_);
        if (plane == nullptr) {
            drmModeFreePlaneResources(planes);
            last_error_ = "primary KMS plane " + std::to_string(primary_plane_id_) + " does not exist";
            return false;
        }
        const bool supported = plane_supports_format(plane, drm_format)
            && plane_supports_format_modifier(drm_fd_, primary_plane_id_, drm_format, modifier);
        drmModeFreePlane(plane);
        if (!supported) {
            drmModeFreePlaneResources(planes);
            last_error_ = "primary KMS plane " + std::to_string(primary_plane_id_)
                + " cannot scan out decoded " + fourcc_string(drm_format);
            return false;
        }

        drmModeFreePlaneResources(planes);
        video_plane_id_ = primary_plane_id_;
        video_plane_format_ = drm_format;
        video_plane_modifier_ = modifier;
        video_on_primary_ = true;
        set_range_edge(drm_fd_, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", false);
        set_range_edge(drm_fd_, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", false);
        set_range_edge(drm_fd_, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true);
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "selected primary KMS plane " + std::to_string(primary_plane_id_) + " for decoded " + fourcc_string(drm_format));
        return true;
    }

    auto try_plane = [&](drmModePlane* plane) {
        const bool usable_crtc = (plane->possible_crtcs & (1 << crtc_index_)) != 0;
        const bool reserved_for_flow = preferred_flow_plane_id_ >= 0 && plane->plane_id == static_cast<std::uint32_t>(preferred_flow_plane_id_);
        const bool available = plane->plane_id != primary_plane_id_ && plane->plane_id != flow_plane_id_ && !reserved_for_flow;
        if (usable_crtc
            && available
            && plane_supports_format(plane, drm_format)
            && plane_supports_format_modifier(drm_fd_, plane->plane_id, drm_format, modifier)) {
            video_plane_id_ = plane->plane_id;
            video_plane_format_ = drm_format;
            video_plane_modifier_ = modifier;
            return true;
        }
        return false;
    };

    if (preferred_video_plane_id_ >= 0) {
        auto* plane = drmModeGetPlane(drm_fd_, static_cast<std::uint32_t>(preferred_video_plane_id_));
        if (plane != nullptr) {
            if (try_plane(plane)) {
                drmModeFreePlane(plane);
            } else {
                drmModeFreePlane(plane);
                drmModeFreePlaneResources(planes);
                last_error_ = "preferred KMS video plane " + std::to_string(preferred_video_plane_id_)
                    + " cannot scan out decoded " + fourcc_string(drm_format) + " on the selected CRTC";
                return false;
            }
        } else {
            drmModeFreePlaneResources(planes);
            last_error_ = "preferred KMS video plane " + std::to_string(preferred_video_plane_id_) + " does not exist";
            return false;
        }
    }

    for (std::uint32_t i = 0; i < planes->count_planes && video_plane_id_ == 0; ++i) {
        auto* plane = drmModeGetPlane(drm_fd_, planes->planes[i]);
        if (plane == nullptr) {
            continue;
        }
        if (!plane_is_vop2_secondary_area(drm_fd_, plane->plane_id)) {
            try_plane(plane);
        }
        drmModeFreePlane(plane);
    }

    if (video_plane_id_ == 0) {
        auto* primary_plane = drmModeGetPlane(drm_fd_, primary_plane_id_);
        if (primary_plane != nullptr
            && plane_supports_format(primary_plane, drm_format)
            && plane_supports_format_modifier(drm_fd_, primary_plane_id_, drm_format, modifier)) {
            video_plane_id_ = primary_plane_id_;
            video_plane_format_ = drm_format;
            video_plane_modifier_ = modifier;
            video_on_primary_ = true;
            drmModeFreePlane(primary_plane);
        } else {
            if (primary_plane != nullptr) {
                drmModeFreePlane(primary_plane);
            }
            drmModeFreePlaneResources(planes);
            last_error_ = "no KMS plane supports decoded video format";
            return false;
        }
    }
    drmModeFreePlaneResources(planes);
    if (video_on_primary_) {
        set_range_edge(drm_fd_, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", false);
        set_range_edge(drm_fd_, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", false);
        set_range_edge(drm_fd_, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true);
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "selected primary KMS plane " + std::to_string(primary_plane_id_) + " for decoded " + fourcc_string(drm_format));
        return true;
    }
    set_range_edge(drm_fd_, video_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", false);
    set_range_edge(drm_fd_, video_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", false);
    set_range_edge(drm_fd_, video_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true);
    glide::log(
        glide::LogLevel::info,
        "OpenHD-Glide",
        "selected KMS video plane " + std::to_string(video_plane_id_) + " for decoded " + fourcc_string(drm_format));
    return true;
}

bool KmsAtomicCompositor::choose_flow_plane()
{
    auto* planes = drmModeGetPlaneResources(drm_fd_);
    if (planes == nullptr) {
        last_error_ = "failed to read KMS plane resources";
        return false;
    }

    auto try_plane = [&](drmModePlane* plane) {
        const auto type = plane_type(drm_fd_, plane->plane_id);
        const bool usable_crtc = (plane->possible_crtcs & (1 << crtc_index_)) != 0;
        const bool reserved_for_ui = preferred_ui_plane_id_ >= 0 && plane->plane_id == static_cast<std::uint32_t>(preferred_ui_plane_id_);
        const bool available = plane->plane_id != primary_plane_id_ && plane->plane_id != video_plane_id_ && plane->plane_id != ui_plane_id_ && !reserved_for_ui;
        return usable_crtc
            && available
            && plane_supports_format(plane, DRM_FORMAT_ARGB8888)
            && plane_can_host_alpha_overlay(type);
    };

    if (preferred_flow_plane_id_ >= 0) {
        auto* plane = drmModeGetPlane(drm_fd_, static_cast<std::uint32_t>(preferred_flow_plane_id_));
        if (plane != nullptr) {
            if (try_plane(plane)) {
                flow_plane_id_ = plane->plane_id;
                drmModeFreePlane(plane);
            } else {
                drmModeFreePlane(plane);
                drmModeFreePlaneResources(planes);
                last_error_ = "preferred KMS Flow plane " + std::to_string(preferred_flow_plane_id_)
                    + " cannot scan out ARGB8888 on the selected CRTC";
                return false;
            }
        } else {
            drmModeFreePlaneResources(planes);
            last_error_ = "preferred KMS Flow plane " + std::to_string(preferred_flow_plane_id_) + " does not exist";
            return false;
        }
    }

    for (std::uint32_t i = 0; i < planes->count_planes && preferred_flow_plane_id_ < 0; ++i) {
        auto* plane = drmModeGetPlane(drm_fd_, planes->planes[i]);
        if (plane == nullptr) {
            continue;
        }

        if (!plane_is_vop2_secondary_area(drm_fd_, plane->plane_id) && try_plane(plane)) {
            flow_plane_id_ = plane->plane_id;
            if (!plane_supports_video_scanout_format(plane)) {
                drmModeFreePlane(plane);
                break;
            }
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    if (flow_plane_id_ == 0) {
        last_error_ = "no KMS alpha plane supports ARGB8888 Flow scanout";
        return false;
    }
    set_range_edge(drm_fd_, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", true);
    set_range_edge(drm_fd_, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", true);
    set_range_edge(drm_fd_, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true);
    glide::log(
        glide::LogLevel::info,
        "OpenHD-Glide",
        "selected KMS Flow plane " + std::to_string(flow_plane_id_) + " for ARGB8888 overlay");
    return true;
}

bool KmsAtomicCompositor::choose_ui_plane()
{
    auto* planes = drmModeGetPlaneResources(drm_fd_);
    if (planes == nullptr) {
        last_error_ = "failed to read KMS plane resources";
        return false;
    }
    const bool allow_video_capable_plane = is_rk3588_device();

    auto try_plane = [&](drmModePlane* plane) {
        const auto type = plane_type(drm_fd_, plane->plane_id);
        const bool usable_crtc = (plane->possible_crtcs & (1 << crtc_index_)) != 0;
        const bool available = plane->plane_id != primary_plane_id_ && plane->plane_id != video_plane_id_ && plane->plane_id != flow_plane_id_;
        return usable_crtc
            && available
            && plane_supports_format(plane, DRM_FORMAT_ARGB8888)
            && plane_supports_format_modifier(drm_fd_, plane->plane_id, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR)
            && (allow_video_capable_plane || !plane_supports_video_scanout_format(plane))
            && plane_can_host_alpha_overlay(type);
    };

    if (preferred_ui_plane_id_ >= 0) {
        auto* plane = drmModeGetPlane(drm_fd_, static_cast<std::uint32_t>(preferred_ui_plane_id_));
        if (plane != nullptr) {
            if (try_plane(plane)) {
                ui_plane_id_ = plane->plane_id;
                drmModeFreePlane(plane);
            } else {
                drmModeFreePlane(plane);
                drmModeFreePlaneResources(planes);
                last_error_ = "preferred KMS UI plane " + std::to_string(preferred_ui_plane_id_)
                    + " cannot scan out linear ARGB8888 on the selected CRTC";
                return false;
            }
        } else {
            drmModeFreePlaneResources(planes);
            last_error_ = "preferred KMS UI plane " + std::to_string(preferred_ui_plane_id_) + " does not exist";
            return false;
        }
    }

    for (std::uint32_t i = 0; i < planes->count_planes && preferred_ui_plane_id_ < 0; ++i) {
        auto* plane = drmModeGetPlane(drm_fd_, planes->planes[i]);
        if (plane == nullptr) {
            continue;
        }

        if (!plane_is_vop2_secondary_area(drm_fd_, plane->plane_id) && try_plane(plane)) {
            ui_plane_id_ = plane->plane_id;
            drmModeFreePlane(plane);
            break;
        }
        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planes);
    if (ui_plane_id_ == 0) {
        last_error_ = "no KMS alpha plane supports linear ARGB8888 UI scanout";
        return false;
    }
    set_range_edge(drm_fd_, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", true);
    set_range_edge(drm_fd_, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", true);
    set_range_edge(drm_fd_, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true);
    glide::log(
        glide::LogLevel::info,
        "OpenHD-Glide",
        "selected KMS UI plane " + std::to_string(ui_plane_id_) + " for linear ARGB8888 overlay");
    return true;
}

bool KmsAtomicCompositor::create_gbm_device()
{
    configure_mesa_runtime_for_board();

    gbm_device_ = gbm_create_device(drm_fd_);
    if (gbm_device_ == nullptr) {
        last_error_ = "failed to create GBM device";
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::create_flow_surface()
{
    std::uint32_t use_flags = GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
    gbm_surface* modifier_surface {};
    const auto driver_name = drm_driver_name(drm_fd_);
    const bool rockchip_drm = driver_name.find("rockchip") != std::string::npos;
    if (rockchip_drm && is_rk3588_device()) {
        // The RK3588 vendor VOP2 driver can advertise Panfrost's preferred
        // AFBC variants through IN_FORMATS and still reject them from
        // rockchip_vop2_mod_supported() during the atomic commit. Request a
        // linear surface explicitly so GBM does not choose an unusable AFBC
        // modifier for the Flow plane.
        const std::array<std::uint64_t, 1> linear_modifier {
            DRM_FORMAT_MOD_LINEAR,
        };
#if OPENHD_GLIDE_HAS_GBM_SURFACE_CREATE_WITH_MODIFIERS2
        modifier_surface = gbm_surface_create_with_modifiers2(
            static_cast<gbm_device*>(gbm_device_),
            flow_surface_.width,
            flow_surface_.height,
            GBM_FORMAT_ARGB8888,
            linear_modifier.data(),
            linear_modifier.size(),
            use_flags);
#else
        modifier_surface = gbm_surface_create_with_modifiers(
            static_cast<gbm_device*>(gbm_device_),
            flow_surface_.width,
            flow_surface_.height,
            GBM_FORMAT_ARGB8888,
            linear_modifier.data(),
            linear_modifier.size());
#endif
    } else if (rockchip_drm) {
        const std::array<std::uint64_t, 9> rockchip_overlay_modifiers {
            arm_afbc_16x16_modifier,
            arm_afbc_16x16_sparse_modifier,
            arm_afbc_16x16_ytr_modifier,
            arm_afbc_16x16_ytr_sparse_modifier,
            arm_afbc_16x16_cbr_modifier,
            arm_afbc_16x16_sparse_cbr_modifier,
            arm_afbc_16x16_ytr_cbr_modifier,
            arm_afbc_16x16_ytr_sparse_cbr_modifier,
            arm_afbc_16x16_ytr_split_sparse_modifier,
        };
#if OPENHD_GLIDE_HAS_GBM_SURFACE_CREATE_WITH_MODIFIERS2
        modifier_surface = gbm_surface_create_with_modifiers2(
            static_cast<gbm_device*>(gbm_device_),
            flow_surface_.width,
            flow_surface_.height,
            GBM_FORMAT_ARGB8888,
            rockchip_overlay_modifiers.data(),
            rockchip_overlay_modifiers.size(),
            use_flags);
#else
        modifier_surface = gbm_surface_create_with_modifiers(
            static_cast<gbm_device*>(gbm_device_),
            flow_surface_.width,
            flow_surface_.height,
            GBM_FORMAT_ARGB8888,
            rockchip_overlay_modifiers.data(),
            rockchip_overlay_modifiers.size());
#endif
    }
    gbm_surface_ = modifier_surface;
    if (gbm_surface_ == nullptr) {
        gbm_surface_ = gbm_surface_create(
            static_cast<gbm_device*>(gbm_device_),
            flow_surface_.width,
            flow_surface_.height,
            GBM_FORMAT_ARGB8888,
            use_flags);
    }
    if (gbm_surface_ == nullptr) {
        last_error_ = "failed to create Flow GBM overlay surface";
        return false;
    }
    glide::log(
        glide::LogLevel::info,
        "OpenHD-Glide",
        "Flow GBM overlay surface created for DRM driver '" + (driver_name.empty() ? std::string("unknown") : driver_name)
            + (rockchip_drm && is_rk3588_device() && modifier_surface != nullptr
                    ? "' with an RK3588-compatible linear modifier"
                    : rockchip_drm && modifier_surface != nullptr
                    ? "' with Rockchip AFBC modifier candidates"
                    : "' with default scanout allocation"));
    return true;
}

bool KmsAtomicCompositor::create_egl()
{
    configure_mesa_runtime_for_board();

    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display = EGL_NO_DISPLAY;
    if (get_platform_display != nullptr) {
        display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device_, nullptr);
    }
    if (display == EGL_NO_DISPLAY) {
        display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm_device_));
    }
    if (display == EGL_NO_DISPLAY) {
        last_error_ = "failed to get EGL display for GBM";
        return false;
    }
    if (eglInitialize(display, nullptr, nullptr) != EGL_TRUE) {
        last_error_ = egl_error_message("failed to initialize EGL");
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        last_error_ = egl_error_message("failed to bind OpenGL ES API");
        return false;
    }

    const std::array<EGLint, 13> config_attributes {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config {};
    EGLint config_count {};
    if (eglChooseConfig(display, config_attributes.data(), &config, 1, &config_count) != EGL_TRUE || config_count == 0) {
        last_error_ = egl_error_message("failed to choose EGL config");
        return false;
    }

    const std::array<EGLint, 3> context_attributes {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes.data());
    if (context == EGL_NO_CONTEXT) {
        last_error_ = egl_error_message("failed to create OpenGL ES context");
        return false;
    }
    EGLSurface surface = eglCreateWindowSurface(
        display,
        config,
        reinterpret_cast<EGLNativeWindowType>(gbm_surface_),
        nullptr);
    if (surface == EGL_NO_SURFACE) {
        eglDestroyContext(display, context);
        last_error_ = egl_error_message("failed to create EGL window surface");
        return false;
    }
    if (eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        eglDestroySurface(display, surface);
        eglDestroyContext(display, context);
        last_error_ = egl_error_message("failed to make EGL context current");
        return false;
    }

    eglSwapInterval(display, 0);
    egl_display_ = display;
    egl_config_ = config;
    egl_context_ = context;
    egl_surface_ = surface;
    return true;
}

bool KmsAtomicCompositor::create_solid_flow_buffer()
{
    drm_mode_create_dumb create {};
    create.width = flow_surface_.width;
    create.height = flow_surface_.height;
    create.bpp = 32;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        last_error_ = "failed to create solid Flow debug buffer" + errno_suffix();
        return false;
    }
    solid_flow_.handle = create.handle;
    solid_flow_.pitch = create.pitch;
    solid_flow_.size = create.size;

    std::uint32_t handles[4] { solid_flow_.handle, 0, 0, 0 };
    std::uint32_t strides[4] { solid_flow_.pitch, 0, 0, 0 };
    std::uint32_t offsets[4] {};
    if (drmModeAddFB2(drm_fd_, flow_surface_.width, flow_surface_.height, DRM_FORMAT_ARGB8888, handles, strides, offsets, &solid_flow_.framebuffer, 0) != 0) {
        last_error_ = "failed to create solid Flow debug framebuffer" + errno_suffix();
        destroy_solid_flow_buffer();
        return false;
    }

    drm_mode_map_dumb map {};
    map.handle = solid_flow_.handle;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        last_error_ = "failed to map solid Flow debug buffer" + errno_suffix();
        destroy_solid_flow_buffer();
        return false;
    }
    solid_flow_.map = mmap(nullptr, solid_flow_.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, static_cast<off_t>(map.offset));
    if (solid_flow_.map == MAP_FAILED) {
        solid_flow_.map = nullptr;
        last_error_ = "failed to mmap solid Flow debug buffer" + errno_suffix();
        destroy_solid_flow_buffer();
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::create_solid_ui_buffer()
{
    drm_mode_create_dumb create {};
    create.width = ui_surface_.width;
    create.height = ui_surface_.height;
    create.bpp = 32;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        last_error_ = "failed to create solid UI debug buffer" + errno_suffix();
        return false;
    }
    solid_ui_.handle = create.handle;
    solid_ui_.pitch = create.pitch;
    solid_ui_.size = create.size;

    std::uint32_t handles[4] { solid_ui_.handle, 0, 0, 0 };
    std::uint32_t strides[4] { solid_ui_.pitch, 0, 0, 0 };
    std::uint32_t offsets[4] {};
    if (drmModeAddFB2(drm_fd_, ui_surface_.width, ui_surface_.height, DRM_FORMAT_ARGB8888, handles, strides, offsets, &solid_ui_.framebuffer, 0) != 0) {
        last_error_ = "failed to create solid UI debug framebuffer" + errno_suffix();
        destroy_solid_ui_buffer();
        return false;
    }

    drm_mode_map_dumb map {};
    map.handle = solid_ui_.handle;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        last_error_ = "failed to map solid UI debug buffer" + errno_suffix();
        destroy_solid_ui_buffer();
        return false;
    }
    solid_ui_.map = mmap(nullptr, solid_ui_.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, static_cast<off_t>(map.offset));
    if (solid_ui_.map == MAP_FAILED) {
        solid_ui_.map = nullptr;
        last_error_ = "failed to mmap solid UI debug buffer" + errno_suffix();
        destroy_solid_ui_buffer();
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::create_cpu_video_buffer(std::uint32_t width, std::uint32_t height)
{
    drm_mode_create_dumb create {};
    create.width = width;
    create.height = height;
    create.bpp = 32;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        last_error_ = "failed to create CPU video DRM buffer" + errno_suffix();
        return false;
    }
    cpu_video_.handle = create.handle;
    cpu_video_.pitch = create.pitch;
    cpu_video_.size = create.size;

    std::uint32_t handles[4] { cpu_video_.handle, 0, 0, 0 };
    std::uint32_t strides[4] { cpu_video_.pitch, 0, 0, 0 };
    std::uint32_t offsets[4] {};
    if (drmModeAddFB2(drm_fd_, width, height, DRM_FORMAT_XRGB8888, handles, strides, offsets, &cpu_video_.framebuffer, 0) != 0) {
        last_error_ = "failed to create CPU video DRM framebuffer" + errno_suffix();
        destroy_cpu_video_buffer();
        return false;
    }

    drm_mode_map_dumb map {};
    map.handle = cpu_video_.handle;
    if (drmIoctl(drm_fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        last_error_ = "failed to map CPU video DRM buffer" + errno_suffix();
        destroy_cpu_video_buffer();
        return false;
    }
    cpu_video_.map = mmap(nullptr, cpu_video_.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, static_cast<off_t>(map.offset));
    if (cpu_video_.map == MAP_FAILED) {
        cpu_video_.map = nullptr;
        last_error_ = "failed to mmap CPU video DRM buffer" + errno_suffix();
        destroy_cpu_video_buffer();
        return false;
    }

    cpu_video_width_ = width;
    cpu_video_height_ = height;
    return true;
}

bool KmsAtomicCompositor::read_flow_frame_into_primary()
{
    if (primary_.map == nullptr || primary_.framebuffer == 0) {
        last_error_ = "primary Flow readback buffer is not mapped";
        return false;
    }

    const auto width = surface_.width;
    const auto height = surface_.height;
    if (width == 0 || height == 0) {
        return true;
    }

    const auto rgba_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    flow_readback_buffer_.resize(rgba_bytes);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, flow_readback_buffer_.data());
    const auto error = glGetError();
    if (error != GL_NO_ERROR) {
        std::ostringstream stream;
        stream << "failed to read Flow GLES framebuffer GL error=0x" << std::hex << error;
        last_error_ = stream.str();
        return false;
    }

    const auto* source = flow_readback_buffer_.data();
    auto* destination = static_cast<unsigned char*>(primary_.map);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto source_y = height - 1U - y;
        const auto* source_row = source + static_cast<std::size_t>(source_y) * static_cast<std::size_t>(width) * 4U;
        auto* destination_row = destination + static_cast<std::size_t>(y) * primary_.pitch;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto* pixel = source_row + static_cast<std::size_t>(x) * 4U;
            auto* output = destination_row + static_cast<std::size_t>(x) * 4U;
            output[0] = pixel[2];
            output[1] = pixel[1];
            output[2] = pixel[0];
            output[3] = pixel[3];
        }
    }

    msync(primary_.map, static_cast<std::size_t>(primary_.size), MS_ASYNC);
    drmModeDirtyFB(drm_fd_, primary_.framebuffer, nullptr, 0);
    return true;
}

bool KmsAtomicCompositor::make_flow_framebuffer_key(void* bo, FlowFramebufferKey& key)
{
    auto* buffer = static_cast<gbm_bo*>(bo);
    if (buffer == nullptr) {
        last_error_ = "Flow GBM buffer is unavailable";
        return false;
    }

    key = {};
    key.width = gbm_bo_get_width(buffer);
    key.height = gbm_bo_get_height(buffer);
    key.format = gbm_bo_get_format(buffer);
    key.modifier = gbm_bo_get_modifier(buffer);
    key.plane_count = static_cast<std::uint32_t>(std::min(gbm_bo_get_plane_count(buffer), 4));
    if (key.plane_count > 0) {
        for (std::uint32_t plane = 0; plane < key.plane_count; ++plane) {
            key.handles[plane] = gbm_bo_get_handle_for_plane(buffer, static_cast<int>(plane)).u32;
            key.strides[plane] = gbm_bo_get_stride_for_plane(buffer, static_cast<int>(plane));
            key.offsets[plane] = gbm_bo_get_offset(buffer, static_cast<int>(plane));
        }
        return true;
    }

    key.plane_count = 1;
    key.handles[0] = gbm_bo_get_handle(buffer).u32;
    key.strides[0] = gbm_bo_get_stride(buffer);
    key.offsets[0] = 0;
    return true;
}

bool KmsAtomicCompositor::find_or_add_flow_framebuffer(void* bo, std::uint32_t& framebuffer_id)
{
    FlowFramebufferKey key;
    if (!make_flow_framebuffer_key(bo, key)) {
        return false;
    }

    for (const auto& cached : flow_framebuffer_cache_) {
        if (cached.key.width == key.width
            && cached.key.height == key.height
            && cached.key.format == key.format
            && cached.key.plane_count == key.plane_count
            && cached.key.modifier == key.modifier
            && cached.key.handles == key.handles
            && cached.key.strides == key.strides
            && cached.key.offsets == key.offsets) {
            framebuffer_id = cached.framebuffer;
            return true;
        }
    }

    if (!add_gbm_framebuffer(bo, framebuffer_id)) {
        return false;
    }
    flow_framebuffer_cache_.push_back(CachedFlowFramebuffer { .key = key, .framebuffer = framebuffer_id });
    return true;
}

bool KmsAtomicCompositor::add_gbm_framebuffer(void* bo, std::uint32_t& framebuffer_id)
{
    FlowFramebufferKey key;
    if (!make_flow_framebuffer_key(bo, key)) {
        return false;
    }

    std::uint64_t modifiers[4] {};
    for (std::uint32_t plane = 0; plane < key.plane_count && plane < 4U; ++plane) {
        modifiers[plane] = key.modifier;
    }
    if (key.modifier != DRM_FORMAT_MOD_INVALID && key.modifier != DRM_FORMAT_MOD_LINEAR) {
        if (drmModeAddFB2WithModifiers(
                drm_fd_,
                key.width,
                key.height,
                key.format,
                key.handles.data(),
                key.strides.data(),
                key.offsets.data(),
                modifiers,
                &framebuffer_id,
                DRM_MODE_FB_MODIFIERS)
            == 0) {
            return true;
        }
        if (errno != EINVAL) {
            last_error_ = "failed to add Flow GBM framebuffer with modifiers" + errno_suffix();
            return false;
        }
    }

    if (drmModeAddFB2(
            drm_fd_,
            key.width,
            key.height,
            key.format,
            key.handles.data(),
            key.strides.data(),
            key.offsets.data(),
            &framebuffer_id,
            0)
        != 0) {
        last_error_ = "failed to add Flow GBM framebuffer" + errno_suffix();
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::lock_flow_framebuffer(std::uint32_t& framebuffer_id, void*& bo)
{
    auto display = static_cast<EGLDisplay>(egl_display_);
    auto surface = static_cast<EGLSurface>(egl_surface_);
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) {
        last_error_ = "Flow EGL surface is unavailable";
        return false;
    }
    if (eglSwapBuffers(display, surface) != EGL_TRUE) {
        last_error_ = egl_error_message("failed to swap Flow EGL overlay surface");
        return false;
    }

    bo = gbm_surface_lock_front_buffer(static_cast<gbm_surface*>(gbm_surface_));
    if (bo == nullptr) {
        last_error_ = "failed to lock Flow GBM front buffer";
        return false;
    }
    if (!find_or_add_flow_framebuffer(bo, framebuffer_id)) {
        gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(bo));
        bo = nullptr;
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::import_video_frame(const DmabufVideoFrame& frame, ImportedFramebuffer& imported)
{
    if (frame.plane_count == 0 || frame.plane_count > 4) {
        last_error_ = "invalid DMABUF plane count";
        return false;
    }

    std::uint32_t handles[4] {};
    std::map<int, std::uint32_t> imported_fds;
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        if (frame.fds[i] < 0) {
            last_error_ = "decoded DMABUF frame has an invalid fd";
            return false;
        }
        const auto existing = imported_fds.find(frame.fds[i]);
        if (existing != imported_fds.end()) {
            handles[i] = existing->second;
            imported.handles[i] = handles[i];
            continue;
        }
        if (drmPrimeFDToHandle(drm_fd_, frame.fds[i], &handles[i]) != 0) {
            last_error_ = "failed to import decoded DMABUF fd into DRM" + errno_suffix();
            imported.handles = { handles[0], handles[1], handles[2], handles[3] };
            destroy_imported(imported);
            return false;
        }
        imported_fds.emplace(frame.fds[i], handles[i]);
        imported.handles[i] = handles[i];
    }

    std::uint32_t strides[4] { frame.strides[0], frame.strides[1], frame.strides[2], frame.strides[3] };
    std::uint32_t offsets[4] { frame.offsets[0], frame.offsets[1], frame.offsets[2], frame.offsets[3] };
    std::uint64_t modifiers[4] { frame.modifiers[0], frame.modifiers[1], frame.modifiers[2], frame.modifiers[3] };
    const bool has_modifier = modifiers[0] != DRM_FORMAT_MOD_INVALID && modifiers[0] != DRM_FORMAT_MOD_LINEAR;
    const auto add_result = has_modifier
        ? drmModeAddFB2WithModifiers(drm_fd_, frame.width, frame.height, frame.drm_format, handles, strides, offsets, modifiers, &imported.framebuffer, DRM_MODE_FB_MODIFIERS)
        : drmModeAddFB2(drm_fd_, frame.width, frame.height, frame.drm_format, handles, strides, offsets, &imported.framebuffer, 0);
    if (add_result != 0) {
        last_error_ = "failed to create DRM framebuffer from decoded DMABUF" + errno_suffix();
        destroy_imported(imported);
        return false;
    }
    return true;
}

bool KmsAtomicCompositor::make_frame_key(const DmabufVideoFrame& frame, FrameKey& key)
{
    if (frame.plane_count == 0 || frame.plane_count > 4) {
        last_error_ = "invalid DMABUF plane count";
        return false;
    }

    key = {};
    key.width = frame.width;
    key.height = frame.height;
    key.drm_format = frame.drm_format;
    key.plane_count = frame.plane_count;
    key.strides = frame.strides;
    key.offsets = frame.offsets;
    key.modifiers = frame.modifiers;
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        if (frame.fds[i] < 0) {
            last_error_ = "decoded DMABUF frame has an invalid fd";
            return false;
        }
        struct stat info {};
        if (fstat(frame.fds[i], &info) != 0) {
            last_error_ = "failed to identify decoded DMABUF fd";
            return false;
        }
        key.device_ids[i] = static_cast<std::uint64_t>(info.st_dev);
        key.inodes[i] = static_cast<std::uint64_t>(info.st_ino);
    }
    return true;
}

KmsAtomicCompositor::ImportedFramebuffer* KmsAtomicCompositor::find_or_import_video_framebuffer(const DmabufVideoFrame& frame)
{
    FrameKey key;
    if (!make_frame_key(frame, key)) {
        return nullptr;
    }

    ++frame_serial_;
    for (auto& cached : video_framebuffer_cache_) {
        if (cached.key.width == key.width
            && cached.key.height == key.height
            && cached.key.drm_format == key.drm_format
            && cached.key.plane_count == key.plane_count
            && cached.key.device_ids == key.device_ids
            && cached.key.inodes == key.inodes
            && cached.key.strides == key.strides
            && cached.key.offsets == key.offsets
            && cached.key.modifiers == key.modifiers) {
            cached.last_used = frame_serial_;
            return &cached.imported;
        }
    }

    evict_video_framebuffer_if_needed();
    CachedFramebuffer cached;
    cached.key = key;
    cached.last_used = frame_serial_;
    if (!import_video_frame(frame, cached.imported)) {
        return nullptr;
    }
    video_framebuffer_cache_.push_back(cached);
    return &video_framebuffer_cache_.back().imported;
}

void KmsAtomicCompositor::evict_video_framebuffer_if_needed()
{
    constexpr std::size_t max_cached_framebuffers = 12;
    if (video_framebuffer_cache_.size() < max_cached_framebuffers) {
        return;
    }

    auto victim = video_framebuffer_cache_.begin();
    for (auto it = video_framebuffer_cache_.begin(); it != video_framebuffer_cache_.end(); ++it) {
        if (it->last_used < victim->last_used) {
            victim = it;
        }
    }
    destroy_imported(victim->imported);
    video_framebuffer_cache_.erase(victim);
}

bool KmsAtomicCompositor::acquire_flow_framebuffer(bool update_flow_frame, std::uint32_t& framebuffer_id, void*& bo, bool& solid_dumb)
{
    framebuffer_id = current_flow_framebuffer_;
    bo = nullptr;
    solid_dumb = current_flow_is_solid_dumb_;

    if (update_flow_frame) {
        solid_dumb = false;
        return lock_flow_framebuffer(framebuffer_id, bo);
    }

    std::lock_guard<std::mutex> lock(flow_framebuffer_mutex_);
    if (pending_flow_framebuffer_ != 0) {
        framebuffer_id = pending_flow_framebuffer_;
        bo = pending_flow_bo_;
        solid_dumb = pending_flow_is_solid_dumb_;
        pending_flow_framebuffer_ = 0;
        pending_flow_bo_ = nullptr;
        pending_flow_is_solid_dumb_ = false;
    }
    return true;
}

void KmsAtomicCompositor::release_acquired_flow_framebuffer(std::uint32_t framebuffer_id, void* bo, bool solid_dumb)
{
    (void)framebuffer_id;
    (void)solid_dumb;
    if (bo != nullptr) {
        if (gbm_surface_ != nullptr) {
            gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(bo));
        }
    }
}

void KmsAtomicCompositor::adopt_flow_framebuffer(std::uint32_t framebuffer_id, void* bo, bool solid_dumb)
{
    if (framebuffer_id == current_flow_framebuffer_ && bo == nullptr) {
        return;
    }

    if (framebuffer_id != current_flow_framebuffer_ && current_flow_bo_ != nullptr) {
        gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(current_flow_bo_));
    }
    if (framebuffer_id != current_flow_framebuffer_ || bo != nullptr) {
        current_flow_framebuffer_ = framebuffer_id;
        current_flow_bo_ = bo;
        current_flow_is_solid_dumb_ = solid_dumb;
    }
}

KmsAtomicCompositor::PlaneRect KmsAtomicCompositor::scaled_video_destination(std::uint32_t width, std::uint32_t height) const
{
    if (width == 0 || height == 0 || surface_.width == 0 || surface_.height == 0) {
        return PlaneRect { .x = 0, .y = 0, .width = surface_.width, .height = surface_.height };
    }

    const auto scaled_width_from_height =
        (static_cast<std::uint64_t>(surface_.height) * static_cast<std::uint64_t>(width))
        / static_cast<std::uint64_t>(height);
    if (scaled_width_from_height <= surface_.width) {
        const auto width = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, scaled_width_from_height));
        return PlaneRect {
            .x = (surface_.width - width) / 2U,
            .y = 0,
            .width = width,
            .height = surface_.height,
        };
    }

    const auto scaled_height_from_width =
        (static_cast<std::uint64_t>(surface_.width) * static_cast<std::uint64_t>(height))
        / static_cast<std::uint64_t>(width);
    const auto scaled_height = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, scaled_height_from_width));
    return PlaneRect {
        .x = 0,
        .y = (surface_.height - scaled_height) / 2U,
        .width = surface_.width,
        .height = scaled_height,
    };
}

KmsAtomicCompositor::PlaneRect KmsAtomicCompositor::scaled_video_destination(const DmabufVideoFrame& video_frame) const
{
    return scaled_video_destination(video_frame.width, video_frame.height);
}

bool KmsAtomicCompositor::commit(
    std::uint32_t video_framebuffer,
    std::uint32_t video_width,
    std::uint32_t video_height,
    std::uint32_t flow_framebuffer,
    std::uint32_t ui_framebuffer,
    bool nonblocking_allowed)
{
    if (!wait_for_pending_page_flip()) {
        return false;
    }

    auto* request = drmModeAtomicAlloc();
    if (request == nullptr) {
        last_error_ = "failed to allocate DRM atomic request";
        return false;
    }

    const bool has_video = video_framebuffer != 0 && video_width != 0 && video_height != 0;
    const bool initial_commit = !modeset_committed_;
    const bool update_primary_plane = initial_commit || (has_video && video_on_primary_);
    const bool update_video_plane = has_video || (!has_video && video_plane_id_ != 0 && !video_on_primary_);
    const bool update_flow_plane =
        flow_framebuffer != 0
        && flow_plane_id_ != 0
        && (initial_commit || flow_framebuffer != current_flow_framebuffer_);
    const bool update_ui_plane = ui_framebuffer != 0 && ui_plane_id_ != 0 && initial_commit;
    const auto video_destination = has_video && !video_on_primary_
        ? scaled_video_destination(video_width, video_height)
        : PlaneRect { .x = 0, .y = 0, .width = surface_.width, .height = surface_.height };
    bool ok = true;
    if (initial_commit) {
        ok = ok && add_property(drm_fd_, request, connector_id_, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, crtc_id_, DRM_MODE_OBJECT_CRTC, "MODE_ID", mode_blob_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, crtc_id_, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", primary_flow_readback_, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", primary_flow_readback_, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true, last_error_);
        ok = ok && add_optional_enum_property(
            drm_fd_,
            request,
            primary_plane_id_,
            DRM_MODE_OBJECT_PLANE,
            "pixel blend mode",
            primary_flow_readback_ ? "Coverage" : "None",
            last_error_);
    }
    if (has_video && !video_on_primary_ && video_plane_id_ != 0) {
        ok = ok && add_optional_range_from_bottom_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", 1, last_error_);
        ok = ok && add_optional_range_from_bottom_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", 1, last_error_);
        ok = ok && add_optional_enum_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "pixel blend mode", "None", last_error_);
    }
    if (update_flow_plane) {
        ok = ok && add_optional_range_edge_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", true, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", true, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true, last_error_);
        ok = ok && add_optional_enum_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "pixel blend mode", "Coverage", last_error_);
    }
    if (update_ui_plane) {
        ok = ok && add_optional_range_edge_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "zpos", true, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "ZPOS", true, last_error_);
        ok = ok && add_optional_range_edge_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "alpha", true, last_error_);
        ok = ok && add_optional_enum_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "pixel blend mode", "Coverage", last_error_);
    }

    const auto primary_has_video = has_video && video_on_primary_;
    const auto primary_framebuffer = primary_has_video ? video_framebuffer : primary_.framebuffer;
    const auto primary_src_width = primary_has_video ? video_width : surface_.width;
    const auto primary_src_height = primary_has_video ? video_height : surface_.height;
    if (update_primary_plane) {
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "FB_ID", primary_framebuffer, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_X", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_W", primary_src_width << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_H", primary_src_height << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_X", primary_has_video ? video_destination.x : 0, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_Y", primary_has_video ? video_destination.y : 0, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_W", primary_has_video ? video_destination.width : surface_.width, last_error_);
        ok = ok && add_property(drm_fd_, request, primary_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_H", primary_has_video ? video_destination.height : surface_.height, last_error_);
    }

    if (has_video && !video_on_primary_ && update_video_plane) {
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "FB_ID", video_framebuffer, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_X", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_W", video_width << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_H", video_height << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_X", video_destination.x, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_Y", video_destination.y, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_W", video_destination.width, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_H", video_destination.height, last_error_);
    } else if (!has_video && video_plane_id_ != 0 && !video_on_primary_) {
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "FB_ID", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, video_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0, last_error_);
    }

    if (update_flow_plane) {
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "FB_ID", flow_framebuffer, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_X", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_W", flow_surface_.width << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_H", flow_surface_.height << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_W", surface_.width, last_error_);
        ok = ok && add_property(drm_fd_, request, flow_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_H", surface_.height, last_error_);
    }

    if (update_ui_plane) {
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "FB_ID", ui_framebuffer, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_X", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_W", ui_surface_.width << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "SRC_H", ui_surface_.height << 16U, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_W", ui_surface_.width, last_error_);
        ok = ok && add_property(drm_fd_, request, ui_plane_id_, DRM_MODE_OBJECT_PLANE, "CRTC_H", ui_surface_.height, last_error_);
    }

    const bool capture_writeback =
        writeback_.enabled
        && writeback_.output_fd >= 0
        && writeback_.connector_id != 0
        && writeback_.buffer.framebuffer != 0
        && (writeback_.max_frames == 0 || writeback_.captured_frames < writeback_.max_frames)
        && (frame_serial_ % writeback_.every_n_frames) == 0;
    if (capture_writeback) {
        if (writeback_.fence_fd >= 0) {
            close(writeback_.fence_fd);
            writeback_.fence_fd = -1;
        }
        ok = ok && add_property(drm_fd_, request, writeback_.connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id_, last_error_);
        ok = ok && add_property(drm_fd_, request, writeback_.connector_id, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID", writeback_.buffer.framebuffer, last_error_);
        ok = ok && add_property(
            drm_fd_,
            request,
            writeback_.connector_id,
            DRM_MODE_OBJECT_CONNECTOR,
            "WRITEBACK_OUT_FENCE_PTR",
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&writeback_.fence_fd)),
            last_error_);
    }

    if (!ok) {
        drmModeAtomicFree(request);
        return false;
    }

    const bool use_nonblocking_commit = modeset_committed_ && vblank_wait_enabled_ && nonblocking_allowed && !capture_writeback;
    std::uint32_t flags = modeset_committed_ ? 0 : DRM_MODE_ATOMIC_ALLOW_MODESET;
    void* user_data {};
    if (use_nonblocking_commit) {
        flags |= DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
        page_flip_event_received_ = false;
        user_data = &page_flip_event_received_;
    }
    if (drmModeAtomicCommit(drm_fd_, request, flags, user_data) != 0) {
        last_error_ = "failed to commit atomic KMS video+Flow/UI frame" + errno_suffix();
        drmModeAtomicFree(request);
        return false;
    }

    modeset_committed_ = true;
    if (use_nonblocking_commit) {
        page_flip_pending_ = !page_flip_event_received_;
    }
    drmModeAtomicFree(request);

    if (capture_writeback) {
        bool fence_signaled {};
        if (writeback_.fence_fd >= 0) {
            pollfd fence_poll {
                .fd = writeback_.fence_fd,
                .events = POLLIN,
                .revents = 0,
            };
            fence_signaled = poll(&fence_poll, 1, 1000) > 0;
            close(writeback_.fence_fd);
            writeback_.fence_fd = -1;
        }
        if (!fence_signaled) {
            last_error_ = "timed out waiting for KMS writeback fence";
            return false;
        }

        msync(writeback_.buffer.map, static_cast<std::size_t>(writeback_.buffer.size), MS_INVALIDATE);
        const auto* bytes = static_cast<const unsigned char*>(writeback_.buffer.map);
        std::uint64_t remaining = writeback_.buffer.size;
        while (remaining > 0) {
            const auto chunk = write(writeback_.output_fd, bytes, static_cast<std::size_t>(remaining));
            if (chunk <= 0) {
                last_error_ = "failed to write KMS writeback frame" + errno_suffix();
                return false;
            }
            bytes += chunk;
            remaining -= static_cast<std::uint64_t>(chunk);
        }

        ++writeback_.captured_frames;
        if (writeback_.captured_frames == writeback_.max_frames && writeback_.output_fd >= 0) {
            close(writeback_.output_fd);
            writeback_.output_fd = -1;
            glide::log(
                glide::LogLevel::info,
                "OpenHD-Glide",
                "KMS writeback raw recorder finished frames=" + std::to_string(writeback_.captured_frames));
        }
    }
    return true;
}

void KmsAtomicCompositor::wait_for_vblank()
{
    if (!vblank_wait_enabled_ || vblank_wait_failed_ || drm_fd_ < 0) {
        return;
    }

    drmVBlank vblank {};
    vblank.request.type = DRM_VBLANK_RELATIVE;
    if (crtc_index_ > 0) {
        vblank.request.type = static_cast<drmVBlankSeqType>(
            vblank.request.type | ((crtc_index_ << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK));
    }
    vblank.request.sequence = 1;
    if (drmWaitVBlank(drm_fd_, &vblank) != 0) {
        vblank_wait_failed_ = true;
        glide::log(glide::LogLevel::warning, "OpenHD-Glide", "DRM atomic vblank wait failed; continuing with immediate commits" + errno_suffix());
    }
}

bool KmsAtomicCompositor::wait_for_pending_page_flip()
{
    if (!page_flip_pending_) {
        return true;
    }

    drmEventContext event_context {};
    event_context.version = DRM_EVENT_CONTEXT_VERSION;
    event_context.page_flip_handler = page_flip_handler;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (page_flip_pending_ && std::chrono::steady_clock::now() < deadline) {
        pollfd drm_poll {
            .fd = drm_fd_,
            .events = POLLIN,
            .revents = 0,
        };
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        const auto timeout_ms = static_cast<int>(std::max<long long>(1, remaining_ms));
        const auto poll_result = poll(&drm_poll, 1, timeout_ms);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_error_ = "failed while waiting for atomic KMS page-flip event" + errno_suffix();
            return false;
        }
        if (poll_result == 0) {
            continue;
        }
        if ((drm_poll.revents & POLLIN) != 0 && drmHandleEvent(drm_fd_, &event_context) != 0) {
            last_error_ = "failed to handle atomic KMS page-flip event" + errno_suffix();
            return false;
        }
        if (page_flip_event_received_) {
            page_flip_pending_ = false;
        }
    }

    if (page_flip_pending_) {
        last_error_ = "timed out waiting for atomic KMS page-flip event";
        return false;
    }
    return true;
}

void KmsAtomicCompositor::destroy_imported(ImportedFramebuffer& imported)
{
    if (drm_fd_ >= 0 && imported.framebuffer != 0) {
        drmModeRmFB(drm_fd_, imported.framebuffer);
        imported.framebuffer = 0;
    }
    if (drm_fd_ >= 0) {
        std::set<std::uint32_t> closed;
        for (auto& handle : imported.handles) {
            if (handle == 0 || closed.count(handle) != 0) {
                handle = 0;
                continue;
            }
            drm_gem_close close_args {};
            close_args.handle = handle;
            drmIoctl(drm_fd_, DRM_IOCTL_GEM_CLOSE, &close_args);
            closed.insert(handle);
            handle = 0;
        }
    }
}

void KmsAtomicCompositor::destroy_video_framebuffer_cache()
{
    current_video_frame_ = {};
    current_video_framebuffer_ = 0;
    for (auto& cached : video_framebuffer_cache_) {
        destroy_imported(cached.imported);
    }
    video_framebuffer_cache_.clear();
}

void KmsAtomicCompositor::destroy_flow_framebuffer_cache()
{
    for (auto& cached : flow_framebuffer_cache_) {
        if (drm_fd_ >= 0 && cached.framebuffer != 0) {
            drmModeRmFB(drm_fd_, cached.framebuffer);
            cached.framebuffer = 0;
        }
    }
    flow_framebuffer_cache_.clear();
}

void KmsAtomicCompositor::destroy_primary_buffer()
{
    if (primary_.map != nullptr) {
        munmap(primary_.map, static_cast<std::size_t>(primary_.size));
        primary_.map = nullptr;
    }
    if (drm_fd_ >= 0 && primary_.framebuffer != 0) {
        drmModeRmFB(drm_fd_, primary_.framebuffer);
        primary_.framebuffer = 0;
    }
    if (drm_fd_ >= 0 && primary_.handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = primary_.handle;
        drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        primary_.handle = 0;
    }
}

void KmsAtomicCompositor::destroy_writeback_buffer()
{
    auto& buffer = writeback_.buffer;
    if (buffer.map != nullptr) {
        munmap(buffer.map, static_cast<std::size_t>(buffer.size));
        buffer.map = nullptr;
    }
    if (drm_fd_ >= 0 && buffer.framebuffer != 0) {
        drmModeRmFB(drm_fd_, buffer.framebuffer);
        buffer.framebuffer = 0;
    }
    if (drm_fd_ >= 0 && buffer.handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = buffer.handle;
        drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        buffer.handle = 0;
    }
    buffer = {};
}

void KmsAtomicCompositor::close_writeback_recorder()
{
    if (writeback_.fence_fd >= 0) {
        close(writeback_.fence_fd);
        writeback_.fence_fd = -1;
    }
    if (writeback_.output_fd >= 0) {
        close(writeback_.output_fd);
        writeback_.output_fd = -1;
    }
    destroy_writeback_buffer();
    writeback_.enabled = false;
}

void KmsAtomicCompositor::destroy_solid_flow_buffer()
{
    if (solid_flow_.map != nullptr) {
        munmap(solid_flow_.map, static_cast<std::size_t>(solid_flow_.size));
        solid_flow_.map = nullptr;
    }
    if (drm_fd_ >= 0 && solid_flow_.framebuffer != 0) {
        drmModeRmFB(drm_fd_, solid_flow_.framebuffer);
        solid_flow_.framebuffer = 0;
    }
    if (drm_fd_ >= 0 && solid_flow_.handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = solid_flow_.handle;
        drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        solid_flow_.handle = 0;
    }
    solid_flow_ = {};
}

void KmsAtomicCompositor::destroy_solid_ui_buffer()
{
    if (solid_ui_.map != nullptr) {
        munmap(solid_ui_.map, static_cast<std::size_t>(solid_ui_.size));
        solid_ui_.map = nullptr;
    }
    if (drm_fd_ >= 0 && solid_ui_.framebuffer != 0) {
        drmModeRmFB(drm_fd_, solid_ui_.framebuffer);
        solid_ui_.framebuffer = 0;
    }
    if (drm_fd_ >= 0 && solid_ui_.handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = solid_ui_.handle;
        drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        solid_ui_.handle = 0;
    }
    solid_ui_ = {};
}

void KmsAtomicCompositor::destroy_cpu_video_buffer()
{
    if (cpu_video_.map != nullptr) {
        munmap(cpu_video_.map, static_cast<std::size_t>(cpu_video_.size));
        cpu_video_.map = nullptr;
    }
    if (drm_fd_ >= 0 && cpu_video_.framebuffer != 0) {
        drmModeRmFB(drm_fd_, cpu_video_.framebuffer);
        cpu_video_.framebuffer = 0;
    }
    if (drm_fd_ >= 0 && cpu_video_.handle != 0) {
        drm_mode_destroy_dumb destroy {};
        destroy.handle = cpu_video_.handle;
        drmIoctl(drm_fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        cpu_video_.handle = 0;
    }
    cpu_video_ = {};
    cpu_video_width_ = 0;
    cpu_video_height_ = 0;
}

void KmsAtomicCompositor::cleanup()
{
    wait_for_pending_page_flip();
    if (drm_fd_ >= 0 && original_crtc_ != nullptr) {
        auto* crtc = static_cast<drmModeCrtc*>(original_crtc_);
        drmModeSetCrtc(
            drm_fd_,
            crtc->crtc_id,
            crtc->buffer_id,
            crtc->x,
            crtc->y,
            &connector_id_,
            1,
            &crtc->mode);
    }
    if (original_crtc_ != nullptr) {
        drmModeFreeCrtc(static_cast<drmModeCrtc*>(original_crtc_));
        original_crtc_ = nullptr;
    }
    pending_flow_framebuffer_ = 0;
    pending_flow_is_solid_dumb_ = false;
    if (pending_flow_bo_ != nullptr && gbm_surface_ != nullptr) {
        gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(pending_flow_bo_));
        pending_flow_bo_ = nullptr;
    }
    current_flow_framebuffer_ = 0;
    current_flow_is_solid_dumb_ = false;
    if (current_flow_bo_ != nullptr && gbm_surface_ != nullptr) {
        gbm_surface_release_buffer(static_cast<gbm_surface*>(gbm_surface_), static_cast<gbm_bo*>(current_flow_bo_));
        current_flow_bo_ = nullptr;
    }

    auto display = static_cast<EGLDisplay>(egl_display_);
    if (display != EGL_NO_DISPLAY && display != nullptr) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != nullptr && static_cast<EGLSurface>(egl_surface_) != EGL_NO_SURFACE) {
            eglDestroySurface(display, static_cast<EGLSurface>(egl_surface_));
        }
        if (egl_context_ != nullptr && static_cast<EGLContext>(egl_context_) != EGL_NO_CONTEXT) {
            eglDestroyContext(display, static_cast<EGLContext>(egl_context_));
        }
        eglTerminate(display);
    }
    egl_display_ = nullptr;
    egl_context_ = nullptr;
    egl_surface_ = nullptr;

    destroy_flow_framebuffer_cache();

    if (gbm_surface_ != nullptr) {
        gbm_surface_destroy(static_cast<gbm_surface*>(gbm_surface_));
        gbm_surface_ = nullptr;
    }
    if (gbm_device_ != nullptr) {
        gbm_device_destroy(static_cast<gbm_device*>(gbm_device_));
        gbm_device_ = nullptr;
    }

    destroy_video_framebuffer_cache();
    close_writeback_recorder();
    destroy_solid_flow_buffer();
    destroy_solid_ui_buffer();
    destroy_cpu_video_buffer();
    destroy_primary_buffer();

    if (drm_fd_ >= 0 && mode_blob_id_ != 0) {
        drmModeDestroyPropertyBlob(drm_fd_, mode_blob_id_);
        mode_blob_id_ = 0;
    }
    if (mode_ != nullptr) {
        delete static_cast<drmModeModeInfo*>(mode_);
        mode_ = nullptr;
    }
    if (drm_fd_ >= 0) {
        close(drm_fd_);
        drm_fd_ = -1;
    }
}
#endif

} // namespace glide::dev
