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

#include "common/logging.hpp"
#include "common/ipc.hpp"
#include "common/mavlink_state.hpp"
#include "common/preview_control.hpp"
#include "dev/desktop_video_pipeline.hpp"
#include "dev/kms_gles_window.hpp"
#include "dev/sdl_gles_window.hpp"
#include "glide_flow/altitude_widget.hpp"
#include "glide_flow/fps_counter.hpp"
#include "glide_flow/fps_overlay.hpp"
#include "glide_flow/gles_text_renderer.hpp"
#include "glide_flow/gpu_map_renderer.hpp"
#include "glide_flow/link_overview.hpp"
#include "glide_flow/naval_osd.hpp"
#include "glide_flow/osd_theme.hpp"
#include "glide_flow/performance_horizon.hpp"
#include "glide_flow/rocket_osd.hpp"
#include "glide_flow/rover_osd.hpp"
#include "glide_flow/speed_widget.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int)
{
    stop_requested = 1;
}

struct Options {
    glide::flow::SurfaceSize surface {
        .width = 1920,
        .height = 1080,
    };
    int x {};
    int y {};
    bool render_gles {};
    bool preview {};
    bool kms {};
    bool stay_alive {};
    bool positioned {};
    bool borderless {};
    bool udp_video {};
    std::uint16_t udp_port { 5600 };
    std::uint16_t secondary_udp_port { 5601 };
    std::string udp_codec { "h264" };
    std::string secondary_udp_codec;
    std::string ui_buffer_path;
    std::uint32_t display_refresh_hz {};
    std::string ipc_socket { glide::ipc::default_socket_path };
};

glide::flow::SurfaceSize parse_surface_size(int argc, char** argv)
{
    glide::flow::SurfaceSize surface {
        .width = 1920,
        .height = 1080,
    };

    for (int i = 1; i + 1 < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--width") {
            surface.width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--height") {
            surface.height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        }
    }

    return surface;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    options.surface = parse_surface_size(argc, argv);

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--render-gles") {
            options.render_gles = true;
        } else if (argument == "--preview") {
            options.preview = true;
            options.render_gles = true;
        } else if (argument == "--kms" || argument == "--kmd") {
            options.kms = true;
        } else if (argument == "--stay-alive") {
            options.stay_alive = true;
        } else if (argument == "--x" && i + 1 < argc) {
            options.x = std::stoi(argv[++i]);
            options.positioned = true;
        } else if (argument == "--y" && i + 1 < argc) {
            options.y = std::stoi(argv[++i]);
            options.positioned = true;
        } else if (argument == "--borderless") {
            options.borderless = true;
        } else if (argument == "--udp-video") {
            options.udp_video = true;
        } else if (argument == "--udp-port" && i + 1 < argc) {
            options.udp_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
            options.udp_video = true;
        } else if (argument == "--secondary-udp-port" && i + 1 < argc) {
            options.secondary_udp_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
            options.udp_video = true;
        } else if (argument == "--udp-codec" && i + 1 < argc) {
            options.udp_codec = argv[++i];
            options.udp_video = true;
        } else if (argument == "--secondary-udp-codec" && i + 1 < argc) {
            options.secondary_udp_codec = argv[++i];
        } else if (argument == "--ui-buffer-path" && i + 1 < argc) {
            options.ui_buffer_path = argv[++i];
        } else if (argument == "--display-refresh-hz" && i + 1 < argc) {
            options.display_refresh_hz = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--ipc-socket" && i + 1 < argc) {
            options.ipc_socket = argv[++i];
        }
    }

    return options;
}

class SharedUiBuffer {
public:
    ~SharedUiBuffer()
    {
#if defined(_WIN32)
        close();
#endif
    }

    bool poll(const std::string& path, std::uint32_t expected_width, std::uint32_t expected_height, glide::dev::DesktopVideoFrame& frame, bool& changed)
    {
        frame = {};
        changed = false;
#if defined(_WIN32)
        if (path.empty()) return false;
        if (map_ == nullptr && !open(path, expected_width, expected_height)) return false;
        const auto sequence = static_cast<std::uint32_t>(*sequence_);
        if (sequence != last_sequence_) {
            const auto* source = static_cast<const std::uint32_t*>(pixels_);
            rgba_.resize(static_cast<std::size_t>(width_) * height_ * 4U);
            for (std::size_t i = 0; i < static_cast<std::size_t>(width_) * height_; ++i) {
                const auto pixel = source[i];
                rgba_[i * 4U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xffU);
                rgba_[i * 4U + 1U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xffU);
                rgba_[i * 4U + 2U] = static_cast<std::uint8_t>(pixel & 0xffU);
                rgba_[i * 4U + 3U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xffU);
            }
            last_sequence_ = sequence;
            changed = true;
        }
        if (rgba_.empty()) return false;
        frame = { .pixels = rgba_.data(), .width = width_, .height = height_, .stride = width_ * 4U };
        return true;
#else
        (void)path;
        (void)expected_width;
        (void)expected_height;
        return false;
#endif
    }

private:
#if defined(_WIN32)
    void close()
    {
        if (map_ != nullptr) UnmapViewOfFile(map_);
        if (mapping_ != nullptr) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
        mapping_ = nullptr;
        map_ = nullptr;
        sequence_ = nullptr;
        pixels_ = nullptr;
    }

    bool open(const std::string& path, std::uint32_t expected_width, std::uint32_t expected_height)
    {
        file_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        mapping_ = CreateFileMappingA(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        map_ = mapping_ != nullptr ? MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0) : nullptr;
        if (map_ == nullptr) {
            close();
            return false;
        }
        const auto* header = static_cast<const std::uint32_t*>(map_);
        if (header[0] != 0x474C5549U || header[1] != expected_width || header[2] != expected_height) {
            close();
            return false;
        }
        width_ = header[1];
        height_ = header[2];
        sequence_ = reinterpret_cast<volatile long*>(const_cast<std::uint32_t*>(&header[3]));
        pixels_ = header + 4;
        return true;
    }

    HANDLE file_ { INVALID_HANDLE_VALUE };
    HANDLE mapping_ {};
    void* map_ {};
    volatile long* sequence_ {};
    const void* pixels_ {};
#endif
    std::uint32_t width_ {};
    std::uint32_t height_ {};
    std::uint32_t last_sequence_ {};
    std::vector<std::uint8_t> rgba_;
};

glide::flow::RgbaColor color_from_rgb(std::uint32_t rgb, float alpha)
{
    return glide::flow::RgbaColor {
        .red = static_cast<float>((rgb >> 16U) & 0xffU) / 255.0F,
        .green = static_cast<float>((rgb >> 8U) & 0xffU) / 255.0F,
        .blue = static_cast<float>(rgb & 0xffU) / 255.0F,
        .alpha = alpha,
    };
}

glide::flow::OsdTheme load_theme()
{
    return glide::flow::OsdTheme {
        .text = color_from_rgb(glide::preview_control::theme_color("bar_text"), 0.98F),
        .bar_background = color_from_rgb(glide::preview_control::theme_color("bar_background"), 80.0F / 255.0F),
        .primary = color_from_rgb(glide::preview_control::theme_color("primary"), 0.92F),
        .secondary = color_from_rgb(glide::preview_control::theme_color("secondary"), 0.90F),
    };
}

void draw_connecting_indicator(
    glide::flow::GlesTextRenderer& renderer,
    glide::flow::SurfaceSize surface,
    const glide::flow::OsdTheme& theme,
    std::chrono::steady_clock::time_point now)
{
    if (surface.width == 0 || surface.height == 0) {
        return;
    }

    constexpr float pi = 3.14159265358979323846F;
    const auto scale = std::max(0.80F, std::min(
        static_cast<float>(surface.width) / 1280.0F,
        static_cast<float>(surface.height) / 720.0F));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto dot_count = static_cast<int>((elapsed_ms / 450) % 4);
    std::string text = "CONNECTING";
    text.append(static_cast<std::size_t>(dot_count), '.');

    const auto text_scale = 22.0F * scale;
    const auto text_width = renderer.measure_text_width(text, text_scale);
    const auto panel_width = std::max(330.0F * scale, text_width + 108.0F * scale);
    const auto panel_height = 92.0F * scale;
    const auto panel_x = (static_cast<float>(surface.width) - panel_width) * 0.5F;
    const auto panel_y = (static_cast<float>(surface.height) - panel_height) * 0.5F;
    const auto center_y = panel_y + (panel_height * 0.5F);

    renderer.draw_filled_quad(
        { panel_x, panel_y },
        { panel_x + panel_width, panel_y },
        { panel_x, panel_y + panel_height },
        { panel_x + panel_width, panel_y + panel_height },
        { .red = 0.0F, .green = 0.0F, .blue = 0.0F, .alpha = 0.42F },
        surface);

    const auto spinner_center = glide::flow::RenderPoint {
        .x = panel_x + 48.0F * scale,
        .y = center_y,
    };
    const auto spinner_radius = 18.0F * scale;
    const auto phase = static_cast<int>((elapsed_ms / 85) % 12);
    for (int i = 0; i < 12; ++i) {
        const auto age = (i - phase + 12) % 12;
        const auto alpha = 0.18F + (static_cast<float>(11 - age) / 11.0F) * 0.70F;
        const auto angle = (static_cast<float>(i) / 12.0F) * 2.0F * pi;
        const auto inner = spinner_radius * 0.55F;
        const auto outer = spinner_radius;
        renderer.draw_line(
            { spinner_center.x + std::cos(angle) * inner, spinner_center.y + std::sin(angle) * inner },
            { spinner_center.x + std::cos(angle) * outer, spinner_center.y + std::sin(angle) * outer },
            3.0F * scale,
            { .red = theme.primary.red, .green = theme.primary.green, .blue = theme.primary.blue, .alpha = alpha },
            surface);
    }

    renderer.set_text_color(theme.text);
    renderer.draw(
        glide::flow::TextPlacement {
            .text = text,
            .x = panel_x + 82.0F * scale,
            .y = center_y + (text_scale * 0.36F),
            .scale = text_scale,
        },
        surface);
}

bool apply_theme_line(const std::string& line)
{
    if (line.rfind("state theme ", 0) != 0) {
        return false;
    }
    const auto key_start = std::string("state theme ").size();
    const auto split = line.find(' ', key_start);
    if (split == std::string::npos) {
        return true;
    }
    const auto key = line.substr(key_start, split - key_start);
    const auto value = line.substr(split + 1U);
    if (value.size() == 6) {
        glide::preview_control::set_theme_color(key, static_cast<std::uint32_t>(std::stoul(value, nullptr, 16)));
    }
    return true;
}

glide::flow::LinkOverviewSample link_sample_from_mavlink(const glide::mavlink::Snapshot& mavlink, bool show_coordinates)
{
    glide::flow::LinkOverviewSample sample;
    sample.show_coordinates = show_coordinates && mavlink.position_valid;
    sample.armed = mavlink.armed;
    sample.frequency_mhz = mavlink.frequency_mhz;
    sample.mcs = mavlink.mcs_index;
    sample.rssi_dbm = mavlink.link_rssi_dbm;
    sample.txc_temp_c = mavlink.link_txc_temp_c;
    sample.downlink_quality = mavlink.link_quality_percent;
    sample.rc_quality = mavlink.rc_quality_percent;
    sample.bitrate_mbit = mavlink.link_bitrate_mbit;
    sample.channel_width_mhz = mavlink.channel_width_mhz;
    sample.air_voltage_v = mavlink.battery_valid ? mavlink.voltage_v : 0.0F;
    sample.air_speed_mps = mavlink.speed_valid
        ? (mavlink.airspeed_mps > 0.0F ? mavlink.airspeed_mps : mavlink.ground_speed_mps)
        : 0.0F;
    sample.height_m = mavlink.altitude_valid ? mavlink.altitude_m : 0.0F;
    sample.satellites = mavlink.satellites;
    sample.flight_mode = mavlink.flight_mode != "N/A" ? mavlink.flight_mode.c_str() : nullptr;
    if (mavlink.position_valid) {
        sample.latitude_deg = mavlink.latitude_deg;
        sample.longitude_deg = mavlink.longitude_deg;
    }
    return sample;
}

} // namespace

int main(int argc, char** argv)
{
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    auto options = parse_options(argc, argv);
    glide::flow::FpsCounter fps_counter;
    glide::flow::FpsOverlay fps_overlay;
    glide::flow::GlesTextRenderer renderer;
    glide::flow::GlesTextRenderer ui_renderer;
    const auto* configured_tile_root = std::getenv("GLIDE_MINIMAP_TILE_ROOT");
    glide::flow::GpuMapRenderer gpu_map(configured_tile_root != nullptr ? configured_tile_root : "assets/maps");
    glide::flow::GpuMapState gpu_map_state;
    glide::flow::AltitudeWidgetRenderer altitude_widget;
    glide::flow::SpeedWidgetRenderer speed_widget;
    glide::flow::LinkOverviewRenderer link_overview;
    glide::flow::PerformanceHorizon performance_horizon;
    glide::flow::RocketOsdRenderer rocket_osd;
    glide::flow::RoverOsdRenderer rover_osd;
    glide::flow::NavalOsdRenderer naval_osd;
    glide::dev::KmsGlesWindow kms_window;
    glide::dev::SdlGlesWindow preview_window;
    std::array<glide::dev::DesktopVideoPipeline, 2> desktop_video;
    SharedUiBuffer shared_ui;
    // OpenHD camera 1 is the default view. Space toggles to camera 2 and back.
    std::size_t active_video_stream {};
    glide::ipc::Client ipc;
    glide::mavlink::Snapshot mavlink;
    bool coordinates_enabled = glide::preview_control::coordinates_overlay_enabled();
    bool compact_readouts = glide::preview_control::compact_readouts_enabled();
    bool top_bar_enabled = glide::preview_control::top_bar_enabled();
    bool telemetry_seen {};
    auto last_telemetry_time = std::chrono::steady_clock::time_point {};
    constexpr auto telemetry_signal_timeout = std::chrono::milliseconds(1500);
    std::string osd_layout = glide::preview_control::osd_layout();
    auto theme = load_theme();
    constexpr bool fps_overlay_enabled = false;
    std::uint32_t video_width {};
    std::uint32_t video_height {};
    if (options.secondary_udp_codec.empty()) options.secondary_udp_codec = options.udp_codec;
    const auto toggle_video_stream = [&] {
        if (!options.udp_video) return;
        active_video_stream = active_video_stream == 0 ? 1 : 0;
        video_width = 0;
        video_height = 0;
        glide::log(
            glide::LogLevel::info,
            "GlideFlow",
            "showing " + std::string(active_video_stream == 0 ? "primary video stream on UDP/" + std::to_string(options.udp_port)
                                                               : "secondary video stream on UDP/" + std::to_string(options.secondary_udp_port)));
    };

    if (options.preview) {
        if (!preview_window.create("GlideFlow Preview", glide::dev::WindowPlacement {
                .width = options.surface.width,
                .height = options.surface.height,
                .x = options.x,
                .y = options.y,
                .positioned = options.positioned,
                .borderless = options.borderless,
            })) {
            glide::log(glide::LogLevel::error, "GlideFlow", preview_window.last_error());
            return 1;
        }
        options.surface = preview_window.surface_size();
        if (options.udp_video) {
            if (!desktop_video[0].start(options.udp_port, options.udp_codec)) {
                glide::log(glide::LogLevel::error, "GlideFlow", desktop_video[0].last_error());
                return 1;
            }
            if (!desktop_video[1].start(options.secondary_udp_port, options.secondary_udp_codec)) {
                glide::log(glide::LogLevel::error, "GlideFlow", desktop_video[1].last_error());
                return 1;
            }
            glide::log(
                glide::LogLevel::info,
                "GlideFlow",
                "desktop video composition listening on primary UDP/" + std::to_string(options.udp_port)
                    + " and secondary UDP/" + std::to_string(options.secondary_udp_port));
            glide::log(
                glide::LogLevel::info,
                "GlideFlow",
                "default video stream is OpenHD camera 1 on UDP/" + std::to_string(options.udp_port));
        }
    }
    if (options.kms) {
#if OPENHD_GLIDE_DEVICE_KMS
        glide::log(glide::LogLevel::info, "GlideFlow", "DRM/KMS mode requested");
        if (!kms_window.create(options.surface.width, options.surface.height, options.display_refresh_hz)) {
            glide::log(glide::LogLevel::error, "GlideFlow", kms_window.last_error());
            return 1;
        }
        options.surface = kms_window.surface_size();
        options.render_gles = true;
        glide::log(
            glide::LogLevel::info,
            "GlideFlow",
            "DRM/KMS surface ready " + std::to_string(options.surface.width) + "x" + std::to_string(options.surface.height));
        glide::log(glide::LogLevel::info, "GlideFlow", renderer.runtime_description());
        if (renderer.likely_software_renderer()) {
            glide::log(glide::LogLevel::warning, "GlideFlow", "OpenGL ES renderer looks like a software fallback");
        } else {
            glide::log(glide::LogLevel::info, "GlideFlow", "OpenGL ES renderer appears hardware accelerated");
        }
#else
        glide::log(glide::LogLevel::error, "GlideFlow", "DRM/KMS mode is disabled in this build");
        return 1;
#endif
    }

    glide::log(glide::LogLevel::info, "GlideFlow", "OSD renderer started");
    if (ipc.connect_to(options.ipc_socket)) {
        ipc.send_line("hello glide-flow");
        ipc.send_line("status glide-flow ready");
    } else {
        glide::log(glide::LogLevel::warning, "GlideFlow", "IPC controller unavailable; waiting for telemetry");
    }
    if (options.render_gles && !renderer.available()) {
        glide::log(glide::LogLevel::warning, "GlideFlow", "OpenGL ES renderer unavailable; running layout path only");
    } else if (options.preview && options.render_gles) {
        glide::log(glide::LogLevel::info, "GlideFlow", renderer.runtime_description());
        if (renderer.likely_software_renderer()) {
            glide::log(glide::LogLevel::warning, "GlideFlow", "OpenGL ES renderer looks like a software fallback");
        } else {
            glide::log(glide::LogLevel::info, "GlideFlow", "OpenGL ES renderer appears hardware accelerated");
        }
    }

    glide::flow::TextPlacement placement = fps_overlay.layout(0.0, options.surface);

    constexpr auto preview_frame_time = std::chrono::microseconds(16667);

    for (unsigned int frame = 0; stop_requested == 0 && (options.preview || options.stay_alive || frame < 180); ++frame) {
        const auto frame_start = std::chrono::steady_clock::now();

        if (options.preview && !preview_window.poll()) {
            break;
        }
        std::string preview_key;
        while (preview_window.consume_key(preview_key)) {
            if (preview_key == "space" && options.udp_video) {
                toggle_video_stream();
                continue;
            }
            if (ipc.connected()) {
                ipc.send_line("ui key " + preview_key);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (ipc.connected()) {
            for (const auto& line : ipc.poll_lines()) {
                if (line == "ui key space") {
                    toggle_video_stream();
                } else if (glide::flow::apply_gpu_map_ipc_line(gpu_map_state, line)) {
                } else if (line == "state coords 0" || line == "state coords 1") {
                    coordinates_enabled = line.back() == '1';
                    glide::preview_control::set_coordinates_overlay_enabled(coordinates_enabled);
                } else if (line == "state compact 0" || line == "state compact 1") {
                    compact_readouts = line.back() == '1';
                    glide::preview_control::set_compact_readouts_enabled(compact_readouts);
                } else if (line == "state topbar 0" || line == "state topbar 1") {
                    top_bar_enabled = line.back() == '1';
                    glide::preview_control::set_top_bar_enabled(top_bar_enabled);
                } else if (line == "state osd drone" || line == "state osd rocket" || line == "state osd rover" || line == "state osd ship") {
                    osd_layout = line.substr(10);
                    glide::preview_control::set_osd_layout(osd_layout);
                } else if (apply_theme_line(line)) {
                    theme = load_theme();
                } else {
                    std::istringstream state_lines(line);
                    std::string state_line;
                    while (std::getline(state_lines, state_line)) {
                        const bool applied = glide::mavlink::apply_ipc_line(mavlink, state_line);
                        if (applied && glide::mavlink::is_osd_telemetry_line(state_line)) {
                            telemetry_seen = true;
                            last_telemetry_time = now;
                        }
                    }
                }
            }
        } else {
            coordinates_enabled = glide::preview_control::coordinates_overlay_enabled();
            compact_readouts = glide::preview_control::compact_readouts_enabled();
            top_bar_enabled = glide::preview_control::top_bar_enabled();
            osd_layout = glide::preview_control::osd_layout();
            theme = load_theme();
        }
        if (last_telemetry_time != std::chrono::steady_clock::time_point {} && now - last_telemetry_time >= telemetry_signal_timeout) {
            telemetry_seen = false;
        }

        options.surface = options.preview ? preview_window.surface_size() : options.surface;
        glide::dev::DesktopVideoFrame video_frame;
        auto& active_video = desktop_video[active_video_stream];
        if (active_video.running() && !active_video.poll(video_frame)) {
            glide::log(glide::LogLevel::error, "GlideFlow", active_video.last_error());
            break;
        }
        const auto fps = fps_counter.frame();
        if (fps) {
            placement = fps_overlay.layout(*fps, options.surface);
            if (ipc.connected()) {
                ipc.send_line("status glide-flow fps " + std::to_string(*fps));
            }
        }

        if (options.render_gles && renderer.available()) {
            renderer.clear(0.02F, 0.02F, 0.025F, 1.0F, options.surface);
            if (video_frame.pixels != nullptr) {
                if (renderer.update_rgba_texture(
                        video_frame.pixels, video_frame.width, video_frame.height, video_frame.stride)) {
                    if (video_width == 0 || video_height == 0) {
                        glide::log(
                            glide::LogLevel::info,
                            "GlideFlow",
                            "first software-decoded video frame " + std::to_string(video_frame.width)
                                + "x" + std::to_string(video_frame.height));
                    }
                    video_width = video_frame.width;
                    video_height = video_frame.height;
                } else {
                    glide::log(glide::LogLevel::error, "GlideFlow", "failed to upload decoded video: " + renderer.last_error());
                    break;
                }
            }
            if (video_width != 0 && video_height != 0) {
                const auto scale = std::min(
                    static_cast<float>(options.surface.width) / static_cast<float>(video_width),
                    static_cast<float>(options.surface.height) / static_cast<float>(video_height));
                const auto width = static_cast<float>(video_width) * scale;
                const auto height = static_cast<float>(video_height) * scale;
                renderer.draw_cached_argb_texture_scaled(
                    { (static_cast<float>(options.surface.width) - width) * 0.5F,
                        (static_cast<float>(options.surface.height) - height) * 0.5F },
                    width,
                    height,
                    options.surface);
            }
            if (!telemetry_seen) {
                draw_connecting_indicator(renderer, options.surface, theme, std::chrono::steady_clock::now());
            } else {
                renderer.set_text_color(theme.primary);
                const auto link_sample = link_sample_from_mavlink(mavlink, coordinates_enabled);
                if (osd_layout == "rocket") {
                    if (top_bar_enabled) {
                        link_overview.draw_top(renderer, options.surface, link_sample, theme);
                    }
                    rocket_osd.draw(renderer, options.surface, glide::flow::RocketOsdSample {}, theme);
                } else if (osd_layout == "rover") {
                    if (top_bar_enabled) {
                        link_overview.draw_top(renderer, options.surface, link_sample, theme);
                    }
                    link_overview.draw_bottom(renderer, options.surface, link_sample, theme);
                    rover_osd.draw(
                        renderer,
                        options.surface,
                        glide::flow::RoverOsdSample { .speed_kmh = mavlink.speed_valid ? mavlink.ground_speed_mps * 3.6F : 0.0F, .heading_degrees = mavlink.attitude_valid ? mavlink.yaw_degrees : 0.0F },
                        theme);
                } else if (osd_layout == "ship") {
                    if (top_bar_enabled) {
                        link_overview.draw_top(renderer, options.surface, link_sample, theme);
                    }
                    link_overview.draw_bottom(renderer, options.surface, link_sample, theme);
                    naval_osd.draw(
                        renderer,
                        options.surface,
                        glide::flow::NavalOsdSample { .heading_degrees = mavlink.attitude_valid ? mavlink.yaw_degrees : 0.0F },
                        theme);
                } else {
                    if (top_bar_enabled) {
                        link_overview.draw_top(renderer, options.surface, link_sample, theme);
                    }
                    link_overview.draw_bottom(renderer, options.surface, link_sample, theme);
                    performance_horizon.draw(
                        renderer,
                        options.surface,
                        glide::flow::AttitudeSample {
                            .roll_degrees = mavlink.attitude_valid ? mavlink.roll_degrees : 0.0F,
                            .pitch_degrees = mavlink.attitude_valid ? mavlink.pitch_degrees : 0.0F,
                        },
                        glide::flow::WindSample {
                            .direction_degrees = link_sample.wind_direction_deg,
                            .speed_mps = link_sample.wind_speed_mps,
                            .valid = false,
                        },
                        theme);
                    speed_widget.draw(
                        renderer,
                        options.surface,
                        glide::flow::SpeedSample { .speed_mps = mavlink.speed_valid ? mavlink.ground_speed_mps : 0.0F },
                        theme,
                        compact_readouts);
                    altitude_widget.draw(
                        renderer,
                        options.surface,
                        glide::flow::AltitudeSample { .altitude_m = mavlink.altitude_valid ? mavlink.altitude_m : 0.0F },
                        theme,
                        compact_readouts);
                }
                if (fps_overlay_enabled) {
                    renderer.draw(placement, options.surface);
                }
            }

            gpu_map.draw(renderer, options.surface, gpu_map_state);

            glide::dev::DesktopVideoFrame ui_frame;
            bool ui_changed {};
            if (shared_ui.poll(options.ui_buffer_path, options.surface.width, options.surface.height, ui_frame, ui_changed)) {
                if (ui_changed) {
                    ui_renderer.update_rgba_texture(ui_frame.pixels, ui_frame.width, ui_frame.height, ui_frame.stride);
                }
                ui_renderer.draw_cached_argb_texture_scaled(
                    { 0.0F, 0.0F },
                    static_cast<float>(options.surface.width),
                    static_cast<float>(options.surface.height),
                    options.surface);
            }
        }

        if (options.preview) {
            preview_window.swap();
        }
        active_video.release_frame();
        if (options.kms) {
            kms_window.swap();
        }

        if (options.preview) {
            std::this_thread::sleep_until(frame_start + preview_frame_time);
        } else if (!options.kms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    return 0;
}
