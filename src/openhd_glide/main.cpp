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
#include "common/mavlink_udp_bridge.hpp"
#include "common/network_discovery.hpp"
#include "common/preview_control.hpp"
#include "dev/kms_atomic_compositor.hpp"
#include "dev/kms_dmabuf_video_plane.hpp"
#include "glide_flow/altitude_widget.hpp"
#include "glide_flow/fps_counter.hpp"
#include "glide_flow/fps_overlay.hpp"
#include "glide_flow/gles_text_renderer.hpp"
#include "glide_flow/link_overview.hpp"
#include "glide_flow/osd_theme.hpp"
#include "glide_flow/performance_horizon.hpp"
#include "glide_flow/speed_widget.hpp"
#include "platform/core_assignment.hpp"
#include "platform/cpu_topology.hpp"
#include "platform/drm_probe.hpp"
#include "platform/process_probe.hpp"
#include "video/cedar_rtp_decoder.hpp"
#include "video/rockchip_mpp_rtp_decoder.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if OPENHD_GLIDE_HAS_GSTREAMER
#include <gst/allocators/gstdmabuf.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#endif

namespace {

volatile std::sig_atomic_t stop_requested = 0;

std::string rgb_hex(std::uint32_t rgb)
{
    std::ostringstream stream;
    stream << std::hex << std::setw(6) << std::setfill('0') << (rgb & 0xffffffU);
    return stream.str();
}

glide::flow::RgbaColor color_from_rgb(std::uint32_t rgb, float alpha)
{
    return glide::flow::RgbaColor {
        .red = static_cast<float>((rgb >> 16U) & 0xffU) / 255.0F,
        .green = static_cast<float>((rgb >> 8U) & 0xffU) / 255.0F,
        .blue = static_cast<float>(rgb & 0xffU) / 255.0F,
        .alpha = alpha,
    };
}

glide::flow::OsdTheme load_osd_theme()
{
    return glide::flow::OsdTheme {
        .text = color_from_rgb(glide::preview_control::theme_color("bar_text"), 0.98F),
        .bar_background = color_from_rgb(glide::preview_control::theme_color("bar_background"), 0.94F),
        .primary = color_from_rgb(glide::preview_control::theme_color("primary"), 0.92F),
        .secondary = color_from_rgb(glide::preview_control::theme_color("secondary"), 0.90F),
    };
}

bool argb_buffer_has_visible_alpha(const void* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t stride_bytes)
{
    if (pixels == nullptr || width == 0 || height == 0 || stride_bytes < width * 4U) {
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(pixels);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = bytes + static_cast<std::size_t>(y) * stride_bytes;
        for (std::uint32_t x = 0; x < width; ++x) {
            if (row[static_cast<std::size_t>(x) * 4U + 3U] != 0) {
                return true;
            }
        }
    }
    return false;
}

void draw_connecting_indicator(
    glide::flow::GlesTextRenderer& renderer,
    glide::flow::SurfaceSize surface,
    const glide::flow::OsdTheme& theme,
    std::chrono::steady_clock::time_point now,
    const std::string& label = "CONNECTING")
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
    std::string text = label;
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

void send_theme_state(glide::ipc::Server& ipc_server, int client_id)
{
    for (const auto* key : { "bar_text", "bar_background", "primary", "secondary" }) {
        ipc_server.send_line(client_id, std::string("state theme ") + key + " " + rgb_hex(glide::preview_control::theme_color(key)));
    }
}

bool handle_theme_line(const std::string& line, glide::ipc::Server& ipc_server)
{
    if (line == "get theme") {
        return false;
    }
    if (line.rfind("set theme ", 0) != 0) {
        return false;
    }
    const auto key_start = std::string("set theme ").size();
    const auto split = line.find(' ', key_start);
    if (split == std::string::npos) {
        return true;
    }
    const auto key = line.substr(key_start, split - key_start);
    const auto value = line.substr(split + 1U);
    if (value.size() == 6) {
        glide::preview_control::set_theme_color(key, static_cast<std::uint32_t>(std::stoul(value, nullptr, 16)));
        ipc_server.broadcast_line("state theme " + key + " " + rgb_hex(glide::preview_control::theme_color(key)));
    }
    return true;
}

void request_stop(int)
{
    stop_requested = 1;
}

struct Options {
    bool preview_stack {};
    bool kms_stack {};
    bool kms_video_preview {};
    std::uint32_t preview_width { 1280 };
    std::uint32_t flow_height { 720 };
    std::uint32_t display_refresh_hz {};
    std::uint32_t ui_width { 760 };
    std::uint32_t ui_height { 720 };
    std::uint16_t view_udp_port { 5600 };
    std::uint16_t mavlink_udp_port { 14550 };
    std::string view_udp_codec { "h264" };
    int view_plane_id { -1 };
    int flow_plane_id { -1 };
    int ui_plane_id { -1 };
    int view_connector_id { -1 };
    float ui_opacity { 0.35F };
    int preview_x { 80 };
    int preview_y { 40 };
    std::string ipc_socket { glide::ipc::default_socket_path };
    bool vertical_stack {};
    bool flow_overlay { true };
    double flow_fps { 30.0 };
    double present_fps {};
    bool atomic_kms {};
    bool vblank_wait {};
    bool native_cedar_video {};
    bool native_rkmpp_video {};
    bool async_flow { true };
    bool flow_debug_solid {};
    bool flow_static_scanout {};
    bool flow_primary_readback {};
    double flow_render_scale { 1.0 };
    bool ui_overlay {};
    std::uint32_t ui_debug_color { 0xCC0B1722U };
    std::string ui_buffer_path { "/tmp/openhd-glide-ui.argb" };
    std::string writeback_record_path {};
    std::uint32_t writeback_record_frames { 30 };
    std::uint32_t writeback_record_every { 6 };
    bool mavlink_udp { true };
    bool ethernet_discover {};
    bool ethernet_p2p {};
    std::string ethernet_p2p_role { "ground" };
    std::string ethernet_p2p_interface {};
};

struct RuntimeControlState {
    bool fps_enabled { true };
    bool coordinates_enabled { true };
    bool compact_readouts { false };
    bool top_bar_enabled { true };
    std::string osd_layout { glide::preview_control::osd_layout() };
};

void start_mavlink_bridge(glide::mavlink::UdpBridge& bridge, const Options& options);
void start_network_discovery(glide::net::DiscoveryService& discovery, const Options& options);

void send_initial_control_state(glide::ipc::Server& ipc_server, int client_id, const RuntimeControlState& state)
{
    ipc_server.send_line(client_id, std::string("state fps ") + (state.fps_enabled ? "1" : "0"));
    ipc_server.send_line(client_id, std::string("state coords ") + (state.coordinates_enabled ? "1" : "0"));
    ipc_server.send_line(client_id, std::string("state compact ") + (state.compact_readouts ? "1" : "0"));
    ipc_server.send_line(client_id, std::string("state topbar ") + (state.top_bar_enabled ? "1" : "0"));
    ipc_server.send_line(client_id, "state osd " + state.osd_layout);
    send_theme_state(ipc_server, client_id);
}

void send_network_state(glide::ipc::Server& ipc_server, int client_id, const glide::net::DiscoveryService& discovery)
{
    ipc_server.send_line(client_id, "state net idle");
    for (const auto& peer : discovery.peers()) {
        ipc_server.send_line(client_id, "state net peer " + peer.address + " " + peer.hostname + " " + std::to_string(peer.video_port));
    }
}

bool handle_network_line(const std::string& line, glide::ipc::Server& ipc_server, int client_id, glide::net::DiscoveryService& discovery)
{
    if (line == "get net") {
        send_network_state(ipc_server, client_id, discovery);
        return true;
    }
    if (line == "net scan") {
        discovery.start_scan();
        return true;
    }
    if (line.rfind("net p2p ", 0) == 0) {
        std::istringstream stream(line);
        std::string prefix;
        std::string command;
        std::string role;
        std::string interface_name;
        stream >> prefix >> command >> role >> interface_name;
        const auto result = glide::net::configure_point_to_point(glide::net::PointToPointOptions {
            .interface_name = interface_name,
            .role = role,
        });
        ipc_server.broadcast_line("state net p2p " + result);
        if (result.rfind("ok ", 0) == 0) {
            discovery.start_scan();
        }
        return true;
    }
    return false;
}

bool poll_runtime_controls(
    glide::ipc::Server& ipc_server,
    glide::mavlink::UdpBridge& mavlink_bridge,
    glide::net::DiscoveryService& discovery,
    RuntimeControlState& state,
    glide::mavlink::Snapshot& mavlink)
{
    bool received_mavlink {};
    for (const auto& line : mavlink_bridge.poll()) {
        if (line.rfind("mav ", 0) == 0) {
            std::istringstream state_lines(line);
            std::string state_line;
            while (std::getline(state_lines, state_line)) {
                glide::mavlink::apply_ipc_line(mavlink, state_line);
                if (glide::mavlink::is_osd_telemetry_line(state_line)) {
                    received_mavlink = true;
                }
            }
        }
        ipc_server.broadcast_line(line);
    }
    for (const auto& line : discovery.poll_state_lines()) {
        ipc_server.broadcast_line(line);
    }
    for (const auto& event : ipc_server.poll()) {
        if (event.line.rfind("hello ", 0) == 0) {
            send_initial_control_state(ipc_server, event.client_id, state);
            send_network_state(ipc_server, event.client_id, discovery);
        } else if (event.line == "get fps") {
            ipc_server.send_line(event.client_id, std::string("state fps ") + (state.fps_enabled ? "1" : "0"));
        } else if (event.line == "get coords") {
            ipc_server.send_line(event.client_id, std::string("state coords ") + (state.coordinates_enabled ? "1" : "0"));
        } else if (event.line == "get compact") {
            ipc_server.send_line(event.client_id, std::string("state compact ") + (state.compact_readouts ? "1" : "0"));
        } else if (event.line == "get topbar") {
            ipc_server.send_line(event.client_id, std::string("state topbar ") + (state.top_bar_enabled ? "1" : "0"));
        } else if (event.line == "get osd") {
            ipc_server.send_line(event.client_id, "state osd " + state.osd_layout);
        } else if (event.line == "get theme") {
            send_theme_state(ipc_server, event.client_id);
        } else if (handle_network_line(event.line, ipc_server, event.client_id, discovery)) {
        } else if (event.line == "set fps 0" || event.line == "set fps 1") {
            state.fps_enabled = event.line.back() == '1';
            glide::preview_control::set_fps_overlay_enabled(state.fps_enabled);
            ipc_server.broadcast_line(std::string("state fps ") + (state.fps_enabled ? "1" : "0"));
        } else if (event.line == "set coords 0" || event.line == "set coords 1") {
            state.coordinates_enabled = event.line.back() == '1';
            glide::preview_control::set_coordinates_overlay_enabled(state.coordinates_enabled);
            ipc_server.broadcast_line(std::string("state coords ") + (state.coordinates_enabled ? "1" : "0"));
        } else if (event.line == "set compact 0" || event.line == "set compact 1") {
            state.compact_readouts = event.line.back() == '1';
            glide::preview_control::set_compact_readouts_enabled(state.compact_readouts);
            ipc_server.broadcast_line(std::string("state compact ") + (state.compact_readouts ? "1" : "0"));
        } else if (event.line == "set topbar 0" || event.line == "set topbar 1") {
            state.top_bar_enabled = event.line.back() == '1';
            glide::preview_control::set_top_bar_enabled(state.top_bar_enabled);
            ipc_server.broadcast_line(std::string("state topbar ") + (state.top_bar_enabled ? "1" : "0"));
        } else if (event.line == "set osd drone" || event.line == "set osd rocket" || event.line == "set osd rover" || event.line == "set osd ship") {
            state.osd_layout = event.line.substr(8);
            glide::preview_control::set_osd_layout(state.osd_layout);
            ipc_server.broadcast_line("state osd " + state.osd_layout);
        } else if (handle_theme_line(event.line, ipc_server)) {
        } else if (event.line.rfind("mav ", 0) == 0) {
            mavlink_bridge.handle_action_line(event.line);
            ipc_server.broadcast_line(event.line);
        } else if (event.line.rfind("ui ", 0) == 0) {
            ipc_server.broadcast_line(event.line);
        } else if (event.line == "ping") {
            ipc_server.send_line(event.client_id, "pong");
        }
    }
    return received_mavlink;
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--preview-stack") {
            options.preview_stack = true;
        } else if (argument == "--kms-stack" || argument == "--kmd-stack") {
            options.kms_stack = true;
        } else if (argument == "--kms-video-preview") {
            options.kms_video_preview = true;
        } else if (argument == "--preview-width" && i + 1 < argc) {
            options.preview_width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--ui-height" && i + 1 < argc) {
            options.ui_height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--ui-width" && i + 1 < argc) {
            options.ui_width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--flow-height" && i + 1 < argc) {
            options.flow_height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--display-refresh-hz" && i + 1 < argc) {
            options.display_refresh_hz = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--view-udp-port" && i + 1 < argc) {
            options.view_udp_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        } else if (argument == "--mavlink-udp-port" && i + 1 < argc) {
            options.mavlink_udp_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        } else if (argument == "--view-udp-codec" && i + 1 < argc) {
            options.view_udp_codec = argv[++i];
            std::transform(options.view_udp_codec.begin(), options.view_udp_codec.end(), options.view_udp_codec.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        } else if (argument == "--view-plane-id" && i + 1 < argc) {
            options.view_plane_id = std::stoi(argv[++i]);
        } else if (argument == "--flow-plane-id" && i + 1 < argc) {
            options.flow_plane_id = std::stoi(argv[++i]);
        } else if (argument == "--ui-plane-id" && i + 1 < argc) {
            options.ui_plane_id = std::stoi(argv[++i]);
        } else if (argument == "--view-connector-id" && i + 1 < argc) {
            options.view_connector_id = std::stoi(argv[++i]);
        } else if (argument == "--preview-x" && i + 1 < argc) {
            options.preview_x = std::stoi(argv[++i]);
        } else if (argument == "--preview-y" && i + 1 < argc) {
            options.preview_y = std::stoi(argv[++i]);
        } else if (argument == "--ui-opacity" && i + 1 < argc) {
            options.ui_opacity = std::clamp(std::stof(argv[++i]), 0.05F, 1.0F);
        } else if (argument == "--ipc-socket" && i + 1 < argc) {
            options.ipc_socket = argv[++i];
        } else if (argument == "--vertical-stack") {
            options.vertical_stack = true;
        } else if (argument == "--native-cedar-video") {
            options.native_cedar_video = true;
            options.native_rkmpp_video = false;
        } else if (argument == "--native-rkmpp-video") {
            options.native_rkmpp_video = true;
            options.native_cedar_video = false;
        } else if (argument == "--gstreamer-video") {
            options.native_cedar_video = false;
            options.native_rkmpp_video = false;
        } else if (argument == "--no-flow") {
            options.flow_overlay = false;
        } else if (argument == "--flow-fps" && i + 1 < argc) {
            options.flow_fps = std::clamp(std::stod(argv[++i]), 0.0, 240.0);
        } else if (argument == "--present-fps" && i + 1 < argc) {
            options.present_fps = std::clamp(std::stod(argv[++i]), 0.0, 240.0);
        } else if (argument == "--atomic-kms") {
            options.atomic_kms = true;
        } else if (argument == "--vblank-wait") {
            options.vblank_wait = true;
        } else if (argument == "--no-vblank-wait") {
            options.vblank_wait = false;
        } else if (argument == "--async-flow") {
            options.async_flow = true;
        } else if (argument == "--sync-flow") {
            options.async_flow = false;
        } else if (argument == "--flow-debug-solid") {
            options.flow_debug_solid = true;
        } else if (argument == "--flow-static-scanout") {
            options.flow_static_scanout = true;
        } else if (argument == "--flow-primary-readback") {
            options.flow_primary_readback = true;
        } else if (argument == "--flow-render-scale" && i + 1 < argc) {
            options.flow_render_scale = std::clamp(std::stod(argv[++i]), 0.25, 1.0);
        } else if (argument == "--ui-overlay") {
            options.ui_overlay = true;
        } else if (argument == "--ui-debug-color" && i + 1 < argc) {
            options.ui_debug_color = static_cast<std::uint32_t>(std::stoul(argv[++i], nullptr, 0));
        } else if (argument == "--ui-buffer-path" && i + 1 < argc) {
            options.ui_buffer_path = argv[++i];
        } else if (argument == "--writeback-record" && i + 1 < argc) {
            options.writeback_record_path = argv[++i];
        } else if (argument == "--writeback-record-frames" && i + 1 < argc) {
            options.writeback_record_frames = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--writeback-record-every" && i + 1 < argc) {
            options.writeback_record_every = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--no-mavlink") {
            options.mavlink_udp = false;
        } else if (argument == "--ethernet-discover") {
            options.ethernet_discover = true;
        } else if (argument == "--ethernet-p2p" && i + 2 < argc) {
            options.ethernet_p2p = true;
            options.ethernet_p2p_role = argv[++i];
            options.ethernet_p2p_interface = argv[++i];
        }
    }
    if (options.flow_primary_readback) {
        options.flow_static_scanout = false;
        options.flow_render_scale = 1.0;
    }
    return options;
}

std::string executable_dir(char* argv0)
{
    const std::string path = argv0 != nullptr ? argv0 : "";
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return path.substr(0, slash + 1);
}

std::string sibling_executable(char* argv0, const char* name)
{
    const auto dir = executable_dir(argv0);
    if (dir.empty()) {
        return name;
    }
    return dir + name;
}

std::vector<std::string> preview_args(const char* executable, const Options& options, bool ui)
{
    const auto y = (ui || !options.vertical_stack) ? options.preview_y : options.preview_y + static_cast<int>(options.ui_height);
    const auto height = ui ? options.ui_height : options.flow_height;
    const auto width = ui ? options.ui_width : options.preview_width;
    auto args = std::vector<std::string> {
        executable,
        "--preview",
        "--width",
        std::to_string(width),
        "--height",
        std::to_string(height),
        "--x",
        std::to_string(options.preview_x),
        "--y",
        std::to_string(y),
        "--borderless",
        "--ipc-socket",
        options.ipc_socket,
    };
    if (ui) {
        args.emplace_back("--always-on-top");
        args.emplace_back("--transparent-clear");
        args.emplace_back("--opacity");
        args.emplace_back(std::to_string(options.ui_opacity));
    }
    return args;
}

std::vector<std::string> view_args(const char* executable, const Options& options)
{
    auto args = std::vector<std::string> {
        executable,
        "--stay-alive",
        "--ipc-socket",
        options.ipc_socket,
    };
    if (options.kms_stack || options.preview_stack) {
        args.emplace_back("--udp-video");
        args.emplace_back("--udp-port");
        args.emplace_back(std::to_string(options.view_udp_port));
        args.emplace_back("--udp-codec");
        args.emplace_back(options.view_udp_codec);
    }
    return args;
}

std::vector<std::string> kms_flow_args(const char* executable, const Options& options)
{
    auto args = std::vector<std::string> {
        executable,
        "--kms",
        "--stay-alive",
        "--width",
        std::to_string(options.preview_width),
        "--height",
        std::to_string(options.flow_height),
        "--ipc-socket",
        options.ipc_socket,
    };
    if (options.display_refresh_hz != 0) {
        args.emplace_back("--display-refresh-hz");
        args.emplace_back(std::to_string(options.display_refresh_hz));
    }
    return args;
}

std::vector<std::string> headless_ui_args(const char* executable, const Options& options)
{
    return {
        executable,
        "--headless",
        "--ipc-socket",
        options.ipc_socket,
    };
}

#if OPENHD_GLIDE_HAS_GSTREAMER
constexpr std::uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(a)
        | (static_cast<std::uint32_t>(b) << 8U)
        | (static_cast<std::uint32_t>(c) << 16U)
        | (static_cast<std::uint32_t>(d) << 24U);
}

std::uint32_t drm_format_from_name(std::string name)
{
    const auto modifier_separator = name.find(':');
    if (modifier_separator != std::string::npos) {
        name = name.substr(0, modifier_separator);
    }
    if (name == "YU12" || name == "I420") {
        return fourcc('Y', 'U', '1', '2');
    }
    if (name == "NV12") {
        return fourcc('N', 'V', '1', '2');
    }
    if (name == "NV21") {
        return fourcc('N', 'V', '2', '1');
    }
    if (name == "YV12") {
        return fourcc('Y', 'V', '1', '2');
    }
    if (name == "XR24" || name == "BGRx") {
        return fourcc('X', 'R', '2', '4');
    }
    return 0;
}

std::uint64_t drm_modifier_from_name(const std::string& name)
{
    const auto modifier_separator = name.find(':');
    if (modifier_separator == std::string::npos || modifier_separator + 1U >= name.size()) {
        return 0;
    }

    const auto modifier_text = name.substr(modifier_separator + 1U);
    char* end {};
    const auto value = std::strtoull(modifier_text.c_str(), &end, 0);
    if (end == modifier_text.c_str() || (end != nullptr && *end != '\0')) {
        return 0;
    }
    return static_cast<std::uint64_t>(value);
}

std::string caps_to_string(GstCaps* caps)
{
    if (caps == nullptr) {
        return "unknown-caps";
    }
    auto* text = gst_caps_to_string(caps);
    std::string result = text != nullptr ? text : "unknown-caps";
    if (text != nullptr) {
        g_free(text);
    }
    return result;
}

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment)
{
    return ((value + alignment - 1U) / alignment) * alignment;
}

std::string describe_memory(GstMemory* memory)
{
    if (memory == nullptr) {
        return "null-memory";
    }

    gsize offset {};
    const auto size = gst_memory_get_sizes(memory, &offset, nullptr);
    const auto* allocator = memory->allocator;
    const auto* memory_type = allocator != nullptr && allocator->mem_type != nullptr ? allocator->mem_type : "unknown";
    std::ostringstream description;
    description << "type=" << memory_type
                << " size=" << size
                << " offset=" << offset
                << " dmabuf=" << (gst_is_dmabuf_memory(memory) ? "yes" : "no")
                << " fd=" << (gst_is_fd_memory(memory) ? "yes" : "no");
    return description.str();
}

std::string describe_buffer_memories(GstBuffer* buffer)
{
    if (buffer == nullptr) {
        return "buffer=null";
    }
    std::ostringstream description;
    const auto memory_count = gst_buffer_n_memory(buffer);
    description << "memory_count=" << memory_count;
    for (guint i = 0; i < memory_count; ++i) {
        description << " mem[" << i << "]={" << describe_memory(gst_buffer_peek_memory(buffer, i)) << "}";
    }
    return description.str();
}

std::string fourcc_to_string(std::uint32_t format)
{
    std::string text(4, ' ');
    text[0] = static_cast<char>(format & 0xFFU);
    text[1] = static_cast<char>((format >> 8U) & 0xFFU);
    text[2] = static_cast<char>((format >> 16U) & 0xFFU);
    text[3] = static_cast<char>((format >> 24U) & 0xFFU);
    return text;
}

struct GstIngressStats {
    std::atomic<std::uint64_t> rtp_packets {};
    std::atomic<std::uint64_t> rtp_bytes {};
    std::atomic<std::uint64_t> parsed_access_units {};
};

GstPadProbeReturn count_rtp_packet_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
{
    auto* stats = static_cast<GstIngressStats*>(user_data);
    if (auto* buffer = gst_pad_probe_info_get_buffer(info); buffer != nullptr) {
        stats->rtp_packets.fetch_add(1, std::memory_order_relaxed);
        stats->rtp_bytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn count_parsed_access_unit_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
{
    auto* stats = static_cast<GstIngressStats*>(user_data);
    if (gst_pad_probe_info_get_buffer(info) != nullptr) {
        stats->parsed_access_units.fetch_add(1, std::memory_order_relaxed);
    }
    return GST_PAD_PROBE_OK;
}

int fd_from_memory(GstMemory* memory)
{
    if (memory == nullptr) {
        return -1;
    }
    if (gst_is_dmabuf_memory(memory)) {
        return gst_dmabuf_memory_get_fd(memory);
    }
    if (gst_is_fd_memory(memory)) {
        return gst_fd_memory_get_fd(memory);
    }
    return -1;
}

std::uint32_t infer_plane_count(std::uint32_t drm_format)
{
    if (drm_format == fourcc('Y', 'U', '1', '2')) {
        return 3;
    }
    if (drm_format == fourcc('Y', 'V', '1', '2')) {
        return 3;
    }
    if (drm_format == fourcc('N', 'V', '1', '2')) {
        return 2;
    }
    if (drm_format == fourcc('N', 'V', '2', '1')) {
        return 2;
    }
    if (drm_format == fourcc('X', 'R', '2', '4')) {
        return 1;
    }
    return 0;
}

bool infer_video_layout(
    std::uint32_t drm_format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t memory_count,
    std::array<std::uint32_t, 4>& strides,
    std::array<std::uint32_t, 4>& offsets)
{
    strides = {};
    offsets = {};

    if (drm_format == fourcc('Y', 'U', '1', '2')) {
        const auto y_stride = align_up(width, 32);
        const auto uv_stride = align_up(width / 2U, 16);
        const auto y_size = y_stride * height;
        const auto uv_size = uv_stride * ((height + 1U) / 2U);
        strides[0] = y_stride;
        strides[1] = uv_stride;
        strides[2] = uv_stride;
        if (memory_count == 1) {
            offsets[1] = y_size;
            offsets[2] = y_size + uv_size;
        }
        return true;
    }

    if (drm_format == fourcc('Y', 'V', '1', '2')) {
        const auto y_stride = align_up(width, 32);
        const auto uv_stride = align_up(width / 2U, 16);
        const auto y_size = y_stride * height;
        const auto uv_size = uv_stride * ((height + 1U) / 2U);
        strides[0] = y_stride;
        strides[1] = uv_stride;
        strides[2] = uv_stride;
        if (memory_count == 1) {
            offsets[1] = y_size;
            offsets[2] = y_size + uv_size;
        }
        return true;
    }

    if (drm_format == fourcc('N', 'V', '1', '2')) {
        const auto y_stride = align_up(width, 32);
        strides[0] = y_stride;
        strides[1] = y_stride;
        if (memory_count == 1) {
            offsets[1] = y_stride * height;
        }
        return true;
    }

    if (drm_format == fourcc('N', 'V', '2', '1')) {
        const auto y_stride = align_up(width, 32);
        strides[0] = y_stride;
        strides[1] = y_stride;
        if (memory_count == 1) {
            offsets[1] = y_stride * height;
        }
        return true;
    }

    if (drm_format == fourcc('X', 'R', '2', '4')) {
        strides[0] = align_up(width * 4U, 64);
        return true;
    }

    return false;
}

bool extract_dmabuf_frame(GstSample* sample, glide::dev::DmabufVideoFrame& frame, std::string& error)
{
    auto* caps = gst_sample_get_caps(sample);
    auto* structure = caps != nullptr ? gst_caps_get_structure(caps, 0) : nullptr;
    auto* buffer = gst_sample_get_buffer(sample);
    auto* meta = buffer != nullptr ? gst_buffer_get_video_meta(buffer) : nullptr;
    if (structure == nullptr || buffer == nullptr) {
        error = "decoded sample is missing caps or buffer";
        return false;
    }

    int width {};
    int height {};
    if (!gst_structure_get_int(structure, "width", &width) || !gst_structure_get_int(structure, "height", &height) || width <= 0 || height <= 0) {
        error = "decoded sample has invalid dimensions";
        return false;
    }

    const auto* drm_format_text = gst_structure_get_string(structure, "drm-format");
    if (drm_format_text == nullptr) {
        drm_format_text = gst_structure_get_string(structure, "format");
    }
    const auto drm_format = drm_format_text != nullptr ? drm_format_from_name(drm_format_text) : 0;
    const auto drm_modifier = drm_format_text != nullptr ? drm_modifier_from_name(drm_format_text) : 0;
    if (drm_format == 0) {
        error = "unsupported decoded DRM format in caps: " + caps_to_string(caps);
        return false;
    }

    const auto plane_count = meta != nullptr ? meta->n_planes : infer_plane_count(drm_format);
    if (plane_count == 0 || plane_count > frame.fds.size()) {
        error = "decoded sample has invalid video plane metadata";
        return false;
    }

    const auto memory_count = gst_buffer_n_memory(buffer);
    if (memory_count == 0) {
        error = "decoded sample contains no memory";
        return false;
    }

    frame = {};
    frame.fds = { -1, -1, -1, -1 };
    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.drm_format = drm_format;
    frame.plane_count = plane_count;

    std::array<std::uint32_t, 4> inferred_strides {};
    std::array<std::uint32_t, 4> inferred_offsets {};
    if (meta == nullptr && !infer_video_layout(
                               drm_format,
                               frame.width,
                               frame.height,
                               static_cast<std::uint32_t>(memory_count),
                               inferred_strides,
                               inferred_offsets)) {
        error = "decoded sample is missing video metadata and layout could not be inferred";
        return false;
    }

    for (std::uint32_t plane = 0; plane < plane_count; ++plane) {
        const auto memory_index = memory_count == 1 ? 0 : plane;
        auto* memory = gst_buffer_peek_memory(buffer, memory_index);
        const auto fd = fd_from_memory(memory);
        if (fd < 0) {
            error = "decoded sample memory is not DMABUF/fd-backed despite DMABUF caps; " + describe_buffer_memories(buffer);
            return false;
        }
        frame.fds[plane] = fd;
        frame.strides[plane] = meta != nullptr ? static_cast<std::uint32_t>(meta->stride[plane]) : inferred_strides[plane];
        frame.offsets[plane] = meta != nullptr ? static_cast<std::uint32_t>(meta->offset[plane]) : inferred_offsets[plane];
        frame.modifiers[plane] = drm_modifier;
    }
    return true;
}

struct MappedCpuVideoFrame {
    GstVideoFrame mapped {};
    bool valid {};
    glide::dev::CpuVideoFrame frame {};

    ~MappedCpuVideoFrame()
    {
        if (valid) {
            gst_video_frame_unmap(&mapped);
        }
    }

    MappedCpuVideoFrame(const MappedCpuVideoFrame&) = delete;
    MappedCpuVideoFrame& operator=(const MappedCpuVideoFrame&) = delete;
    MappedCpuVideoFrame() = default;
};

bool extract_bgrx_cpu_frame(GstSample* sample, MappedCpuVideoFrame& mapped, std::string& error)
{
    auto* caps = gst_sample_get_caps(sample);
    auto* buffer = gst_sample_get_buffer(sample);
    if (caps == nullptr || buffer == nullptr) {
        error = "decoded sample is missing caps or buffer";
        return false;
    }

    GstVideoInfo info {};
    if (!gst_video_info_from_caps(&info, caps)) {
        error = "decoded CPU sample has invalid video caps: " + caps_to_string(caps);
        return false;
    }
    if (GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRx) {
        error = "decoded CPU sample is not BGRx: " + caps_to_string(caps);
        return false;
    }
    if (!gst_video_frame_map(&mapped.mapped, &info, buffer, GST_MAP_READ)) {
        error = "failed to map decoded CPU video frame";
        return false;
    }

    mapped.valid = true;
    mapped.frame = glide::dev::CpuVideoFrame {
        .width = static_cast<std::uint32_t>(GST_VIDEO_INFO_WIDTH(&info)),
        .height = static_cast<std::uint32_t>(GST_VIDEO_INFO_HEIGHT(&info)),
        .stride = static_cast<std::uint32_t>(GST_VIDEO_FRAME_PLANE_STRIDE(&mapped.mapped, 0)),
        .pixels = GST_VIDEO_FRAME_PLANE_DATA(&mapped.mapped, 0),
    };
    return true;
}
#endif

struct SharedUiBuffer {
    int fd { -1 };
    void* map {};
    std::size_t size {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::string path;

    void close()
    {
#if defined(__linux__)
        if (map != nullptr && map != MAP_FAILED) {
            munmap(map, size);
        }
        if (fd >= 0) {
            ::close(fd);
        }
#endif
        fd = -1;
        map = nullptr;
        size = 0;
    }

    bool open_if_needed(const std::string& requested_path, std::uint32_t requested_width, std::uint32_t requested_height)
    {
#if defined(__linux__)
        const auto requested_size = static_cast<std::size_t>(requested_width) * requested_height * sizeof(std::uint32_t);
        if (fd >= 0 && map != nullptr && path == requested_path && width == requested_width && height == requested_height && size == requested_size) {
            return true;
        }
        close();
        fd = ::open(requested_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        struct stat info {};
        if (fstat(fd, &info) != 0 || static_cast<std::size_t>(info.st_size) < requested_size) {
            close();
            return false;
        }
        map = mmap(nullptr, requested_size, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            map = nullptr;
            close();
            return false;
        }
        path = requested_path;
        width = requested_width;
        height = requested_height;
        size = requested_size;
        return true;
#else
        (void)requested_path;
        (void)requested_width;
        (void)requested_height;
        return false;
#endif
    }
};

int run_kms_video_preview(const Options& options)
{
#if OPENHD_GLIDE_DEVICE_KMS && (OPENHD_GLIDE_HAS_GSTREAMER || OPENHD_GLIDE_HAS_CEDAR)
    stop_requested = 0;
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    const bool use_atomic_kms = options.atomic_kms || options.flow_overlay || options.ui_overlay;
    glide::dev::KmsAtomicCompositor compositor;
    glide::dev::KmsDmabufVideoPlane legacy_output;
    SharedUiBuffer shared_ui;
    auto ui_height = options.ui_height != 0 ? options.ui_height : options.flow_height;
    const auto flow_render_width = static_cast<std::uint32_t>(std::lround(static_cast<double>(options.preview_width) * options.flow_render_scale));
    const auto flow_render_height = static_cast<std::uint32_t>(std::lround(static_cast<double>(options.flow_height) * options.flow_render_scale));
    if (use_atomic_kms) {
        if (!compositor.create(
                options.preview_width,
                options.flow_height,
                options.display_refresh_hz,
                options.view_plane_id,
                options.flow_plane_id,
                options.ui_plane_id,
                flow_render_width,
                flow_render_height,
                options.ui_overlay ? options.ui_width : 0,
                options.ui_overlay ? ui_height : 0,
                options.flow_primary_readback)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
            return 1;
        }
        compositor.set_vblank_wait_enabled(options.vblank_wait);
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            options.vblank_wait ? "atomic KMS video commits are paced by page-flip events" : "atomic KMS page-flip pacing is disabled");
        if (options.ui_overlay) {
            if (!compositor.publish_solid_ui_frame(options.ui_debug_color)) {
                glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
                return 1;
            }
            glide::log(
                glide::LogLevel::info,
                "OpenHD-Glide",
                "UI debug overlay enabled width=" + std::to_string(options.ui_width)
                    + " height=" + std::to_string(ui_height)
                    + " buffer=" + options.ui_buffer_path);
        }
        if (!options.writeback_record_path.empty()) {
            if (!compositor.enable_writeback_recording(
                    options.writeback_record_path,
                    options.writeback_record_every,
                    options.writeback_record_frames)) {
                glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
                return 1;
            }
        }
    } else {
        if (!legacy_output.create(options.preview_width, options.flow_height, options.display_refresh_hz, options.view_plane_id)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", legacy_output.last_error());
            return 1;
        }
        legacy_output.set_vblank_wait_enabled(options.vblank_wait);
    }

    const bool async_flow = use_atomic_kms && (options.flow_overlay || options.ui_overlay) && options.async_flow;
    std::thread async_flow_thread;
    std::thread ui_buffer_thread;
    struct ScopedThreadJoin {
        std::thread& thread;
        ~ScopedThreadJoin()
        {
            if (thread.joinable()) {
                stop_requested = 1;
                thread.join();
            }
        }
    };

    glide::flow::GlesTextRenderer flow_renderer;
    glide::flow::AltitudeWidgetRenderer altitude_widget;
    glide::flow::SpeedWidgetRenderer speed_widget;
    glide::flow::LinkOverviewRenderer link_overview;
    glide::flow::PerformanceHorizon performance_horizon;
    std::atomic<double> video_plane_fps { 0.0 };
    std::atomic<bool> video_signal_present { false };
    std::atomic<bool> telemetry_signal_present { false };
    auto flow_surface = use_atomic_kms ? compositor.surface_size() : glide::flow::SurfaceSize { options.preview_width, options.flow_height };
    bool flow_runtime_logged {};
    bool ui_buffer_logged_ready {};
    constexpr auto ui_buffer_interval = std::chrono::milliseconds(16);
    auto next_ui_buffer_upload = std::chrono::steady_clock::time_point {};
    const auto flow_frame_interval = options.flow_fps > 0.0
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / options.flow_fps))
        : std::chrono::steady_clock::duration::max();
    auto next_flow_frame = std::chrono::steady_clock::now();
    auto last_video_frame_time = std::chrono::steady_clock::time_point {};
    constexpr auto video_signal_timeout = std::chrono::milliseconds(750);
    auto last_telemetry_time = std::chrono::steady_clock::time_point {};
    constexpr auto telemetry_signal_timeout = std::chrono::milliseconds(1500);
    ScopedThreadJoin async_flow_join { async_flow_thread };
    ScopedThreadJoin ui_buffer_join { ui_buffer_thread };
    RuntimeControlState control_state;
    glide::mavlink::Snapshot mavlink_snapshot;
    std::mutex mavlink_snapshot_mutex;
    glide::ipc::Server ipc_server;
    const bool ipc_enabled = ipc_server.listen_on(options.ipc_socket);
    if (!ipc_enabled) {
        glide::log(glide::LogLevel::warning, "OpenHD-Glide", "IPC controller disabled: " + ipc_server.last_error());
    }
    glide::mavlink::UdpBridge mavlink_bridge;
    start_mavlink_bridge(mavlink_bridge, options);
    glide::net::DiscoveryService network_discovery;
    start_network_discovery(network_discovery, options);
    const auto poll_control = [&]() {
        if (ipc_enabled) {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mavlink_snapshot_mutex);
            if (poll_runtime_controls(ipc_server, mavlink_bridge, network_discovery, control_state, mavlink_snapshot)) {
                last_telemetry_time = now;
                telemetry_signal_present.store(true, std::memory_order_relaxed);
            } else if (last_telemetry_time != std::chrono::steady_clock::time_point {}
                && now - last_telemetry_time >= telemetry_signal_timeout) {
                telemetry_signal_present.store(false, std::memory_order_relaxed);
            }
        }
    };

    const auto render_flow_overlay = [&]() {
        if (!flow_renderer.available()) {
            return;
        }
        if (!use_atomic_kms) {
            return;
        }
        flow_surface = compositor.surface_size();
        if (!flow_runtime_logged) {
            glide::log(glide::LogLevel::info, "OpenHD-Glide", flow_renderer.runtime_description());
            if (flow_renderer.likely_software_renderer()) {
                glide::log(glide::LogLevel::warning, "OpenHD-Glide", "Flow OpenGL ES renderer looks like a software fallback");
            } else {
                glide::log(glide::LogLevel::info, "OpenHD-Glide", "Flow OpenGL ES renderer appears hardware accelerated");
            }
            flow_runtime_logged = true;
        }

        flow_renderer.clear(0.0F, 0.0F, 0.0F, 0.0F, flow_surface);
        if (options.flow_debug_solid) {
            flow_renderer.draw_filled_quad(
                { 0.0F, 0.0F },
                { static_cast<float>(flow_surface.width), 0.0F },
                { 0.0F, static_cast<float>(flow_surface.height) },
                { static_cast<float>(flow_surface.width), static_cast<float>(flow_surface.height) },
                { .red = 1.0F, .green = 0.0F, .blue = 0.75F, .alpha = 0.55F },
                flow_surface);
        }
        if (options.flow_overlay) {
            const auto theme = load_osd_theme();
            const bool has_telemetry = telemetry_signal_present.load(std::memory_order_relaxed);
            const bool has_video = video_signal_present.load(std::memory_order_relaxed);
            if (!has_telemetry) {
                draw_connecting_indicator(flow_renderer, flow_surface, theme, std::chrono::steady_clock::now());
            } else {
                glide::mavlink::Snapshot mavlink;
                {
                    std::lock_guard<std::mutex> lock(mavlink_snapshot_mutex);
                    mavlink = mavlink_snapshot;
                }
                const auto link_sample = link_sample_from_mavlink(mavlink, glide::preview_control::coordinates_overlay_enabled());
                flow_renderer.set_text_color(theme.primary);
                if (glide::preview_control::top_bar_enabled()) {
                    link_overview.draw_top(flow_renderer, flow_surface, link_sample, theme);
                }
                link_overview.draw_bottom(flow_renderer, flow_surface, link_sample, theme);
                performance_horizon.draw(
                    flow_renderer,
                    flow_surface,
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
                const auto compact_readouts = glide::preview_control::compact_readouts_enabled();
                speed_widget.draw(
                    flow_renderer,
                    flow_surface,
                    glide::flow::SpeedSample { .speed_mps = mavlink.speed_valid ? mavlink.ground_speed_mps : 0.0F },
                    theme,
                    compact_readouts);
                altitude_widget.draw(
                    flow_renderer,
                    flow_surface,
                    glide::flow::AltitudeSample { .altitude_m = mavlink.altitude_valid ? mavlink.altitude_m : 0.0F },
                    theme,
                    compact_readouts);
                if (!has_video) {
                    draw_connecting_indicator(flow_renderer, flow_surface, theme, std::chrono::steady_clock::now(), "WAITING FOR VIDEO");
                }
            }
        }

        if (options.ui_overlay && shared_ui.open_if_needed(options.ui_buffer_path, options.ui_width, ui_height)) {
            if (!ui_buffer_logged_ready) {
                glide::log(glide::LogLevel::info, "OpenHD-Glide", "UI buffer backend connected to " + options.ui_buffer_path);
                ui_buffer_logged_ready = true;
            }
            const auto now = std::chrono::steady_clock::now();
            const bool update_ui_buffer = now >= next_ui_buffer_upload;
            if (compositor.ui_overlay_plane_active()) {
                if (update_ui_buffer && !compositor.publish_ui_frame_from_argb(shared_ui.map, options.ui_width, ui_height, options.ui_width * 4U)) {
                    glide::log(glide::LogLevel::warning, "OpenHD-Glide", compositor.last_error());
                }
            } else {
                if (update_ui_buffer) {
                    flow_renderer.update_argb_texture(shared_ui.map, options.ui_width, ui_height, options.ui_width * 4U);
                }
                const auto ui_scale_x = static_cast<float>(flow_surface.width) / static_cast<float>(options.preview_width);
                const auto ui_scale_y = static_cast<float>(flow_surface.height) / static_cast<float>(options.flow_height);
                flow_renderer.draw_cached_argb_texture_scaled(
                    { 0.0F, 0.0F },
                    static_cast<float>(options.ui_width) * ui_scale_x,
                    static_cast<float>(ui_height) * ui_scale_y,
                    flow_surface);
            }
            if (update_ui_buffer) {
                next_ui_buffer_upload = now + ui_buffer_interval;
            }
        } else if (options.ui_overlay) {
            ui_buffer_logged_ready = false;
        }
    };

    const auto should_update_flow = [&](std::uint64_t presented_frames) {
        if (presented_frames == 0) {
            return true;
        }
        if (!options.flow_overlay && !options.ui_overlay) {
            return false;
        }
        if (options.flow_fps <= 0.0) {
            return true;
        }
        return std::chrono::steady_clock::now() >= next_flow_frame;
    };

    const auto update_flow_deadline = [&]() {
        if (options.flow_fps > 0.0) {
            next_flow_frame = std::chrono::steady_clock::now() + flow_frame_interval;
        }
    };

    const auto present_waiting_overlay_frame = [&](bool update_flow, bool force) {
        if (!use_atomic_kms || (!options.flow_overlay && !options.ui_overlay)) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && options.flow_fps > 0.0 && now < next_flow_frame) {
            return true;
        }
        if (update_flow) {
            render_flow_overlay();
        }
        if (!compositor.present_overlay(update_flow)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
            return false;
        }
        update_flow_deadline();
        return true;
    };

    const auto maybe_present_waiting_overlay = [&]() {
        if (!use_atomic_kms || (!options.flow_overlay && !options.ui_overlay)) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool timed_out =
            video_signal_present.load(std::memory_order_relaxed)
            && last_video_frame_time != std::chrono::steady_clock::time_point {}
            && now - last_video_frame_time >= video_signal_timeout;
        if (timed_out) {
            video_signal_present.store(false, std::memory_order_relaxed);
            video_plane_fps.store(0.0, std::memory_order_relaxed);
        }
        if (video_signal_present.load(std::memory_order_relaxed)) {
            return true;
        }
        return present_waiting_overlay_frame(!async_flow, false);
    };

    if (!async_flow && !present_waiting_overlay_frame(true, true)) {
        return 1;
    }

    if (async_flow) {
        if (!compositor.release_flow_context()) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
            return 1;
        }

        async_flow_thread = std::thread([&compositor,
                                             &shared_ui,
                                             &video_plane_fps,
                                             &video_signal_present,
                                             &telemetry_signal_present,
                                             &mavlink_snapshot,
                                             &mavlink_snapshot_mutex,
                                             flow_frame_interval,
                                             flow_fps = options.flow_fps,
                                             flow_overlay = options.flow_overlay,
                                             flow_debug_solid = options.flow_debug_solid,
                                             flow_static_scanout = options.flow_static_scanout,
                                             flow_primary_readback = options.flow_primary_readback,
                                             ui_overlay = options.ui_overlay,
                                             ui_buffer_path = options.ui_buffer_path,
                                             ui_width = options.ui_width,
                                             ui_height,
                                             preview_width = options.preview_width,
                                             flow_height = options.flow_height,
                                             ui_buffer_interval]() {
            if (!compositor.make_flow_context_current()) {
                glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
                stop_requested = 1;
                return;
            }

            if (flow_debug_solid) {
                compositor.release_flow_context();
                while (stop_requested == 0) {
                    if (!compositor.publish_solid_flow_frame(0xFFFF00FFU)) {
                        glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
                        stop_requested = 1;
                        break;
                    }
                    if (flow_fps > 0.0) {
                        std::this_thread::sleep_for(flow_frame_interval);
                    } else {
                        std::this_thread::yield();
                    }
                }
                return;
            }

            {
                glide::flow::GlesTextRenderer renderer;
                glide::flow::AltitudeWidgetRenderer altitude;
                glide::flow::SpeedWidgetRenderer speed;
                glide::flow::LinkOverviewRenderer links;
                glide::flow::PerformanceHorizon horizon;
                auto surface = compositor.surface_size();
                bool runtime_logged {};
                bool scanout_published {};
                bool ui_buffer_logged_ready {};
                bool ui_buffer_visible {};
                auto next_ui_buffer_upload = std::chrono::steady_clock::time_point {};
                std::uint64_t flow_frames_since_log {};
                auto last_flow_log = std::chrono::steady_clock::now();
                auto next_frame = std::chrono::steady_clock::now();

                while (stop_requested == 0) {
                    surface = compositor.surface_size();
                    if (!runtime_logged) {
                        glide::log(glide::LogLevel::info, "OpenHD-Glide", renderer.runtime_description());
                        if (renderer.likely_software_renderer()) {
                            glide::log(glide::LogLevel::warning, "OpenHD-Glide", "Flow OpenGL ES renderer looks like a software fallback");
                        } else {
                            glide::log(glide::LogLevel::info, "OpenHD-Glide", "Flow OpenGL ES renderer appears hardware accelerated");
                        }
                        runtime_logged = true;
                    }

                    renderer.clear(0.0F, 0.0F, 0.0F, 0.0F, surface);
                    if (flow_debug_solid) {
                        renderer.draw_filled_quad(
                            { 0.0F, 0.0F },
                            { static_cast<float>(surface.width), 0.0F },
                            { 0.0F, static_cast<float>(surface.height) },
                            { static_cast<float>(surface.width), static_cast<float>(surface.height) },
                            { .red = 1.0F, .green = 0.0F, .blue = 0.75F, .alpha = 0.55F },
                            surface);
                    }
                    if (flow_overlay) {
                        const auto theme = load_osd_theme();
                        const bool has_telemetry = telemetry_signal_present.load(std::memory_order_relaxed);
                        const bool has_video = video_signal_present.load(std::memory_order_relaxed);
                        if (!has_telemetry) {
                            draw_connecting_indicator(renderer, surface, theme, std::chrono::steady_clock::now());
                        } else {
                            glide::mavlink::Snapshot mavlink;
                            {
                                std::lock_guard<std::mutex> lock(mavlink_snapshot_mutex);
                                mavlink = mavlink_snapshot;
                            }
                            const auto link_sample = link_sample_from_mavlink(mavlink, glide::preview_control::coordinates_overlay_enabled());
                            renderer.set_text_color(theme.primary);
                            if (glide::preview_control::top_bar_enabled()) {
                                links.draw_top(renderer, surface, link_sample, theme);
                            }
                            links.draw_bottom(renderer, surface, link_sample, theme);
                            horizon.draw(
                                renderer,
                                surface,
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
                            const auto compact_readouts = glide::preview_control::compact_readouts_enabled();
                            speed.draw(
                                renderer,
                                surface,
                                glide::flow::SpeedSample { .speed_mps = mavlink.speed_valid ? mavlink.ground_speed_mps : 0.0F },
                                theme,
                                compact_readouts);
                            altitude.draw(
                                renderer,
                                surface,
                                glide::flow::AltitudeSample { .altitude_m = mavlink.altitude_valid ? mavlink.altitude_m : 0.0F },
                                theme,
                                compact_readouts);
                            if (!has_video) {
                                draw_connecting_indicator(renderer, surface, theme, std::chrono::steady_clock::now(), "WAITING FOR VIDEO");
                            }
                        }
                    }

                    if (ui_overlay && shared_ui.open_if_needed(ui_buffer_path, ui_width, ui_height)) {
                        if (!ui_buffer_logged_ready) {
                            glide::log(glide::LogLevel::info, "OpenHD-Glide", "UI buffer backend connected to " + ui_buffer_path);
                            ui_buffer_logged_ready = true;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= next_ui_buffer_upload) {
                            ui_buffer_visible = argb_buffer_has_visible_alpha(shared_ui.map, ui_width, ui_height, ui_width * 4U);
                            if (ui_buffer_visible) {
                                renderer.update_argb_texture(shared_ui.map, ui_width, ui_height, ui_width * 4U);
                            }
                            next_ui_buffer_upload = now + ui_buffer_interval;
                        }
                        if (ui_buffer_visible) {
                            const auto ui_scale_x = static_cast<float>(surface.width) / static_cast<float>(preview_width);
                            const auto ui_scale_y = static_cast<float>(surface.height) / static_cast<float>(flow_height);
                            renderer.draw_cached_argb_texture_scaled(
                                { 0.0F, 0.0F },
                                static_cast<float>(ui_width) * ui_scale_x,
                                static_cast<float>(ui_height) * ui_scale_y,
                                surface);
                        }
                    } else if (ui_overlay) {
                        ui_buffer_logged_ready = false;
                        ui_buffer_visible = false;
                    }

                    if (!flow_static_scanout || !scanout_published) {
                        const bool published = flow_primary_readback
                            ? compositor.publish_rendered_flow_frame_to_primary()
                            : compositor.publish_rendered_flow_frame();
                        if (!published) {
                            glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
                            stop_requested = 1;
                            break;
                        }
                        scanout_published = true;
                    }

                    ++flow_frames_since_log;
                    const auto log_now = std::chrono::steady_clock::now();
                    if (log_now - last_flow_log >= std::chrono::seconds(1)) {
                        const auto elapsed = std::chrono::duration<double>(log_now - last_flow_log).count();
                        std::ostringstream status;
                        status.setf(std::ios::fixed);
                        status.precision(1);
                        status << "Flow render/publish fps=" << (static_cast<double>(flow_frames_since_log) / elapsed)
                               << " ui_visible=" << (ui_buffer_visible ? "yes" : "no");
                        glide::log(glide::LogLevel::info, "OpenHD-Glide", status.str());
                        flow_frames_since_log = 0;
                        last_flow_log = log_now;
                    }

                    if (flow_fps > 0.0) {
                        next_frame += flow_frame_interval;
                        const auto now = std::chrono::steady_clock::now();
                        if (now + flow_frame_interval < next_frame) {
                            next_frame = now + flow_frame_interval;
                        }
                        if (now < next_frame) {
                            std::this_thread::sleep_until(next_frame);
                        }
                    } else {
                        std::this_thread::yield();
                    }
                }
            }

            compositor.release_flow_context();
        });

        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "Flow overlay rendering is decoupled from video presentation at " + std::to_string(options.flow_fps) + " fps");

        for (int attempt = 0; attempt < 12 && stop_requested == 0; ++attempt) {
            if (!present_waiting_overlay_frame(false, true)) {
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    if (use_atomic_kms && options.ui_overlay && async_flow && compositor.ui_overlay_plane_active()) {
        ui_buffer_thread = std::thread([&compositor, &shared_ui, ui_buffer_path = options.ui_buffer_path, ui_width = options.ui_width, ui_height]() {
            constexpr auto ui_interval = std::chrono::milliseconds(16);
            bool logged_ready {};
            while (stop_requested == 0) {
                if (shared_ui.open_if_needed(ui_buffer_path, ui_width, ui_height)) {
                    if (!logged_ready) {
                        glide::log(glide::LogLevel::info, "OpenHD-Glide", "UI buffer backend connected to " + ui_buffer_path);
                        logged_ready = true;
                    }
                    if (!compositor.publish_ui_frame_from_argb(shared_ui.map, ui_width, ui_height, ui_width * 4U)) {
                        glide::log(glide::LogLevel::warning, "OpenHD-Glide", compositor.last_error());
                    }
                } else {
                    logged_ready = false;
                }
                std::this_thread::sleep_for(ui_interval);
            }
            shared_ui.close();
        });
        glide::log(glide::LogLevel::info, "OpenHD-Glide", "UI buffer copy is decoupled from video presentation at 30 fps");
    }

    const auto present_video_frame = [&](const glide::dev::DmabufVideoFrame& frame, std::uint64_t presented_frames) {
        if (!use_atomic_kms) {
            if (!legacy_output.present(frame)) {
                glide::log(glide::LogLevel::error, "OpenHD-Glide", legacy_output.last_error());
                return false;
            }
            return true;
        }

        const bool update_flow = !async_flow && should_update_flow(presented_frames);
        if (update_flow) {
            if (options.flow_overlay || options.ui_overlay) {
                render_flow_overlay();
            } else {
                flow_renderer.clear(0.0F, 0.0F, 0.0F, 0.0F, flow_surface);
            }
        }
        if (!compositor.present(frame, update_flow)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
            return false;
        }
        video_signal_present.store(true, std::memory_order_relaxed);
        last_video_frame_time = std::chrono::steady_clock::now();
        if (compositor.writeback_recording_finished()) {
            stop_requested = 1;
        }
        if (update_flow) {
            update_flow_deadline();
        }
        return true;
    };

    const auto present_cpu_video_frame = [&](const glide::dev::CpuVideoFrame& frame, std::uint64_t presented_frames) {
        if (!use_atomic_kms) {
            if (!legacy_output.present(frame)) {
                glide::log(glide::LogLevel::error, "OpenHD-Glide", legacy_output.last_error());
                return false;
            }
            return true;
        }
        const bool update_flow = !async_flow && should_update_flow(presented_frames);
        if (update_flow) {
            if (options.flow_overlay || options.ui_overlay) {
                render_flow_overlay();
            } else {
                flow_renderer.clear(0.0F, 0.0F, 0.0F, 0.0F, flow_surface);
            }
        }
        if (!compositor.present(frame, update_flow)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", compositor.last_error());
            return false;
        }
        video_signal_present.store(true, std::memory_order_relaxed);
        last_video_frame_time = std::chrono::steady_clock::now();
        if (compositor.writeback_recording_finished()) {
            stop_requested = 1;
        }
        if (update_flow) {
            update_flow_deadline();
        }
        return true;
    };

#if OPENHD_GLIDE_HAS_CEDAR
    if (options.native_cedar_video) {
        glide::video::CedarRtpDecoder cedar;
        const auto cedar_fps = options.display_refresh_hz != 0 ? options.display_refresh_hz : 120U;
        if (!cedar.start(options.view_udp_port, options.preview_width, options.flow_height, cedar_fps)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", cedar.last_error());
            return 1;
        }

        glide::log(glide::LogLevel::info, "OpenHD-Glide", "native Cedar RTP/H264 decoder running at configured fps=" + std::to_string(cedar_fps));
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            use_atomic_kms ? "atomic KMS video+Flow compositor running" : "fast legacy KMS video plane running");

        std::uint64_t frames {};
        std::uint64_t frames_since_log {};
        auto last_log = std::chrono::steady_clock::now();
        while (stop_requested == 0) {
            poll_control();
            glide::dev::DmabufVideoFrame frame;
            if (!cedar.poll(frame)) {
                if (!cedar.last_error().empty()) {
                    glide::log(glide::LogLevel::error, "OpenHD-Glide", cedar.last_error());
                    return 1;
                }
                if (!maybe_present_waiting_overlay()) {
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (!present_video_frame(frame, frames)) {
                return 1;
            }
            cedar.mark_presented();
            ++frames;
            ++frames_since_log;

            const auto now = std::chrono::steady_clock::now();
            if (now - last_log >= std::chrono::seconds(1)) {
                const auto elapsed = std::chrono::duration<double>(now - last_log).count();
                std::ostringstream status;
                status.setf(std::ios::fixed);
                status.precision(1);
                const auto current_fps = static_cast<double>(frames_since_log) / elapsed;
                video_plane_fps.store(current_fps);
                status << "native Cedar " << (use_atomic_kms ? "atomic video+Flow" : "legacy video")
                       << " fps=" << current_fps
                       << " total_frames=" << frames
                       << ' ' << cedar.stats();
                glide::log(glide::LogLevel::info, "OpenHD-Glide", status.str());
                frames_since_log = 0;
                last_log = now;
            }
        }
        return 0;
    }
#else
    if (options.native_cedar_video) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "native Cedar decoder support was not found at build time");
        return 1;
    }
#endif

#if OPENHD_GLIDE_HAS_RKMPP
    if (options.native_rkmpp_video) {
        glide::video::RockchipMppRtpDecoder rkmpp;
        if (!rkmpp.start(options.view_udp_port, options.view_udp_codec)) {
            glide::log(glide::LogLevel::error, "OpenHD-Glide", rkmpp.last_error());
            return 1;
        }

        glide::log(glide::LogLevel::info, "OpenHD-Glide", "native Rockchip MPP RTP decoder running");
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            use_atomic_kms ? "atomic KMS video+Flow compositor running on dedicated presenter thread" : "fast legacy KMS video plane running on dedicated presenter thread");

        std::atomic<int> presenter_result { -1 };
        std::thread presenter_thread([&]() {
            std::uint64_t frames {};
            std::uint64_t frames_since_log {};
            auto last_log = std::chrono::steady_clock::now();
            auto last_present = std::chrono::steady_clock::time_point {};
            double present_interval_total_ms {};
            double present_interval_min_ms { 1000000.0 };
            double present_interval_max_ms {};
            std::uint64_t present_intervals_since_log {};
            auto last_waiting_log = std::chrono::steady_clock::now();
            int result {};

            glide::log(glide::LogLevel::info, "OpenHD-Glide", "RKMPP presenter thread owns decoder dequeue and KMS page-flip cadence");
            while (stop_requested == 0) {
                glide::dev::DmabufVideoFrame frame;
                if (!rkmpp.poll(frame)) {
                    if (!rkmpp.last_error().empty()) {
                        glide::log(glide::LogLevel::error, "OpenHD-Glide", rkmpp.last_error());
                        result = 1;
                        stop_requested = 1;
                        break;
                    }
                    if (!maybe_present_waiting_overlay()) {
                        result = 1;
                        stop_requested = 1;
                        break;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_waiting_log >= std::chrono::seconds(1)) {
                        glide::log(glide::LogLevel::info, "OpenHD-Glide", "waiting for native RKMPP decoded frame " + rkmpp.stats());
                        last_waiting_log = now;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }

                if (!present_video_frame(frame, frames)) {
                    result = 1;
                    stop_requested = 1;
                    break;
                }
                rkmpp.mark_presented();
                ++frames;
                ++frames_since_log;

                const auto now = std::chrono::steady_clock::now();
                if (last_present.time_since_epoch().count() != 0) {
                    const auto interval_ms = std::chrono::duration<double, std::milli>(now - last_present).count();
                    present_interval_total_ms += interval_ms;
                    present_interval_min_ms = std::min(present_interval_min_ms, interval_ms);
                    present_interval_max_ms = std::max(present_interval_max_ms, interval_ms);
                    ++present_intervals_since_log;
                }
                last_present = now;
                if (now - last_log >= std::chrono::seconds(1)) {
                    const auto elapsed = std::chrono::duration<double>(now - last_log).count();
                    std::ostringstream status;
                    status.setf(std::ios::fixed);
                    status.precision(1);
                    const auto current_fps = static_cast<double>(frames_since_log) / elapsed;
                    const auto present_interval_avg_ms = present_intervals_since_log != 0
                        ? present_interval_total_ms / static_cast<double>(present_intervals_since_log)
                        : 0.0;
                    video_plane_fps.store(current_fps);
                    status << "native Rockchip MPP " << (use_atomic_kms ? "atomic video+Flow" : "legacy video")
                           << " fps=" << current_fps
                           << " present_interval_ms(avg/min/max)=" << present_interval_avg_ms << '/'
                           << (present_intervals_since_log != 0 ? present_interval_min_ms : 0.0) << '/'
                           << present_interval_max_ms
                           << " total_frames=" << frames
                           << ' ' << rkmpp.stats();
                    glide::log(glide::LogLevel::info, "OpenHD-Glide", status.str());
                    frames_since_log = 0;
                    present_interval_total_ms = 0.0;
                    present_interval_min_ms = 1000000.0;
                    present_interval_max_ms = 0.0;
                    present_intervals_since_log = 0;
                    last_log = now;
                }
            }
            presenter_result.store(result, std::memory_order_release);
        });

        while (stop_requested == 0) {
            poll_control();
            if (presenter_result.load(std::memory_order_acquire) >= 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stop_requested = 1;
        if (presenter_thread.joinable()) {
            presenter_thread.join();
        }
        return presenter_result.load(std::memory_order_acquire) == 1 ? 1 : 0;
    }
#else
    if (options.native_rkmpp_video) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "native Rockchip MPP decoder support was not found at build time");
        return 1;
    }
#endif

#if OPENHD_GLIDE_HAS_GSTREAMER
    gst_init(nullptr, nullptr);
    struct DecoderPipelineCandidate {
        const char* name;
        const char* factory;
        const char* decoder;
        const char* output_caps;
    };

    const bool use_h265 = options.view_udp_codec == "h265" || options.view_udp_codec == "hevc";
    const bool use_h264 = options.view_udp_codec == "h264";
    const bool use_mjpeg = options.view_udp_codec == "mjpeg" || options.view_udp_codec == "mjpg" || options.view_udp_codec == "jpeg";
    if (!use_h264 && !use_h265 && !use_mjpeg) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "unsupported --view-udp-codec value; use h264, h265, or mjpeg");
        return 1;
    }

    const auto make_pipeline_text = [&](const DecoderPipelineCandidate& candidate, bool force_dmabuf_capture) {
        std::ostringstream text;
        text
            << "udpsrc name=video-source port=" << options.view_udp_port
            << " buffer-size=33554432 "
            << " caps=\"application/x-rtp,media=(string)video,clock-rate=(int)90000,encoding-name=(string)";
        if (use_mjpeg) {
            text
                << "JPEG,payload=(int)96\" "
                << "! rtpjpegdepay name=video-parse "
                << "! " << candidate.decoder << " "
                << "! videoconvert ";
        } else {
            text
                << (use_h265 ? "H265" : "H264")
                << ",payload=(int)96\" "
                << "! " << (use_h265 ? "rtph265depay" : "rtph264depay") << " "
                << "! " << (use_h265 ? "h265parse" : "h264parse") << " name=video-parse config-interval=-1 "
                << "! " << (use_h265 ? "video/x-h265" : "video/x-h264") << ",stream-format=byte-stream,parsed=true,alignment=au "
                << "! " << candidate.decoder << " ";
            if (force_dmabuf_capture && std::string(candidate.name) == (use_h265 ? "v4l2h265dec" : "v4l2h264dec")) {
                text << "capture-io-mode=dmabuf ";
            }
        }
        text
            << "! queue max-size-buffers=8 max-size-bytes=0 max-size-time=0 leaky=downstream "
            << "! ";
        if (candidate.output_caps[0] != '\0') {
            text << candidate.output_caps << " ! ";
        }
        text << "appsink name=video-sink sync=false async=false drop=true max-buffers=8 emit-signals=false";
        return text.str();
    };

    std::vector<DecoderPipelineCandidate> candidates;
    if (use_mjpeg) {
        candidates = {
            { "jpegdec", "jpegdec", "jpegdec", "video/x-raw,format=BGRx" },
            { "avdec_mjpeg", "avdec_mjpeg", "avdec_mjpeg", "video/x-raw,format=BGRx" },
        };
    } else if (use_h265) {
        candidates = {
            { "mppvideodec", "mppvideodec", "mppvideodec dma-feature=true arm-afbc=false fast-mode=true qos=false format=NV12", "video/x-raw(memory:DMABuf),format=NV12" },
            { "v4l2slh265dec", "v4l2slh265dec", "v4l2slh265dec", "video/x-raw(memory:DMABuf),format=DMA_DRM" },
            { "v4l2h265dec", "v4l2h265dec", "v4l2h265dec capture-io-mode=dmabuf", "video/x-raw(memory:DMABuf),format=DMA_DRM" },
            { "omxhevcvideodec", "omxhevcvideodec", "omxhevcvideodec disable-dma-feature=false", "" },
        };
    } else {
        candidates = {
            { "mppvideodec", "mppvideodec", "mppvideodec dma-feature=true arm-afbc=false fast-mode=true qos=false format=NV12", "video/x-raw(memory:DMABuf),format=NV12" },
            { "v4l2slh264dec", "v4l2slh264dec", "v4l2slh264dec", "video/x-raw(memory:DMABuf),format=DMA_DRM" },
            { "v4l2h264dec", "v4l2h264dec", "v4l2h264dec capture-io-mode=dmabuf", "video/x-raw(memory:DMABuf),format=DMA_DRM" },
            { "omxh264dec", "omxh264dec", "omxh264dec disable-dma-feature=false", "" },
        };
    }

    GError* error {};
    GstElement* pipeline {};
    std::string selected_pipeline_name;
    for (const auto& candidate : candidates) {
        auto* factory = gst_element_factory_find(candidate.factory);
        if (factory == nullptr) {
            glide::log(glide::LogLevel::warning, "OpenHD-Glide", std::string("skipping missing decoder element ") + candidate.factory);
            continue;
        }
        gst_object_unref(factory);

        const auto pipeline_text = make_pipeline_text(candidate, false);
        pipeline = gst_parse_launch(pipeline_text.c_str(), &error);
        if (pipeline != nullptr && error == nullptr) {
            selected_pipeline_name = candidate.name;
            break;
        }

        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            std::string("failed to create video preview pipeline for ") + candidate.name + ": "
                + (error != nullptr ? error->message : "unknown parse error"));
        if (error != nullptr) {
            g_error_free(error);
            error = nullptr;
        }
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }
    if (pipeline == nullptr) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to create any video preview pipeline");
        return 1;
    }

    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "video-sink");
    auto* bus = gst_element_get_bus(pipeline);
    GstIngressStats ingress_stats;
    gulong source_probe_id {};
    gulong parsed_probe_id {};
    if (sink == nullptr || bus == nullptr) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to access video preview appsink");
        if (bus != nullptr) {
            gst_object_unref(bus);
        }
        if (sink != nullptr) {
            gst_object_unref(sink);
        }
        gst_object_unref(pipeline);
        return 1;
    }
    if (auto* source = gst_bin_get_by_name(GST_BIN(pipeline), "video-source"); source != nullptr) {
        if (auto* source_pad = gst_element_get_static_pad(source, "src"); source_pad != nullptr) {
            source_probe_id = gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER, &count_rtp_packet_probe, &ingress_stats, nullptr);
            gst_object_unref(source_pad);
        }
        gst_object_unref(source);
    }
    if (auto* parse = gst_bin_get_by_name(GST_BIN(pipeline), "video-parse"); parse != nullptr) {
        if (auto* parse_pad = gst_element_get_static_pad(parse, "src"); parse_pad != nullptr) {
            parsed_probe_id = gst_pad_add_probe(parse_pad, GST_PAD_PROBE_TYPE_BUFFER, &count_parsed_access_unit_probe, &ingress_stats, nullptr);
            gst_object_unref(parse_pad);
        }
        gst_object_unref(parse);
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to start controller-owned KMS video preview");
        gst_object_unref(bus);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return 1;
    }

    const auto selected_codec_label = use_mjpeg ? std::string("mjpeg") : (use_h265 ? std::string("h265") : std::string("h264"));
    glide::log(glide::LogLevel::info, "OpenHD-Glide", "controller-owned KMS video plane running");
    glide::log(glide::LogLevel::info, "OpenHD-Glide", "selected decoder pipeline=" + selected_pipeline_name + " codec=" + selected_codec_label);
    if (use_mjpeg) {
        glide::log(glide::LogLevel::info, "OpenHD-Glide", "decoded MJPEG CPU frames are copied into a DRM dumb buffer and scanned out on a KMS plane");
    } else {
        glide::log(glide::LogLevel::info, "OpenHD-Glide", "KMS video importer build: forced decoder DMABUF capture + fd-backed memory diagnostics");
        glide::log(glide::LogLevel::info, "OpenHD-Glide", "decoded DMABUF frames are imported directly into DRM and scanned out on a KMS plane");
    }
    glide::preview_control::set_fps_overlay_enabled(true);
    glide::preview_control::set_coordinates_overlay_enabled(true);
    glide::preview_control::set_compact_readouts_enabled(false);
    glide::preview_control::set_top_bar_enabled(true);
    glide::log(
        glide::LogLevel::info,
        "OpenHD-Glide",
        use_atomic_kms ? "atomic KMS video+Flow compositor running" : "fast legacy KMS video plane running");
    if (!use_atomic_kms) {
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            options.vblank_wait ? "legacy video plane updates are paced to DRM vblank" : "legacy video plane vblank pacing is disabled");
    }

    std::uint64_t frames {};
    std::uint64_t frames_since_log {};
    std::uint64_t stale_samples_dropped {};
    std::uint64_t stale_samples_dropped_since_log {};
    std::uint64_t logged_rtp_packets {};
    std::uint64_t logged_parsed_access_units {};
    double sample_wait_ms_total {};
    double sample_wait_ms_max {};
    std::uint64_t sample_wait_count {};
    double present_ms_total {};
    double present_ms_max {};
    std::uint64_t present_count {};
    const double requested_present_fps = options.present_fps;
    const bool software_present_pacing = !use_atomic_kms && !options.vblank_wait && requested_present_fps > 0.0;
    const auto present_interval = software_present_pacing
        ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / requested_present_fps))
        : std::chrono::steady_clock::duration::zero();
    auto next_present = std::chrono::steady_clock::now();
    if (software_present_pacing) {
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "legacy video plane software present pacing enabled at " + std::to_string(requested_present_fps) + " fps");
    }
    auto last_log = std::chrono::steady_clock::now();
    GstSample* scanout_sample {};
    bool logged_caps {};
    std::uint32_t consecutive_frame_errors {};
    while (stop_requested == 0) {
        poll_control();
        while (auto* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* gst_error {};
                gchar* debug {};
                gst_message_parse_error(message, &gst_error, &debug);
                glide::log(glide::LogLevel::error, "OpenHD-Glide", gst_error != nullptr ? gst_error->message : "GStreamer error");
                if (debug != nullptr) {
                    glide::log(glide::LogLevel::warning, "OpenHD-Glide", debug);
                    g_free(debug);
                }
                if (gst_error != nullptr) {
                    g_error_free(gst_error);
                }
                gst_message_unref(message);
                stop_requested = 1;
                break;
            }
            glide::log(glide::LogLevel::warning, "OpenHD-Glide", "video preview pipeline reached EOS");
            gst_message_unref(message);
            stop_requested = 1;
            break;
        }

        const auto sample_wait_start = std::chrono::steady_clock::now();
        auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), GST_SECOND / 20);
        const auto sample_wait_end = std::chrono::steady_clock::now();
        const auto sample_wait_ms = std::chrono::duration<double, std::milli>(sample_wait_end - sample_wait_start).count();
        sample_wait_ms_total += sample_wait_ms;
        sample_wait_ms_max = std::max(sample_wait_ms_max, sample_wait_ms);
        ++sample_wait_count;
        if (sample == nullptr) {
            if (!maybe_present_waiting_overlay()) {
                stop_requested = 1;
                break;
            }
            continue;
        }
        while (auto* newer_sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) {
            gst_sample_unref(sample);
            sample = newer_sample;
            ++stale_samples_dropped;
            ++stale_samples_dropped_since_log;
        }
        if (software_present_pacing) {
            const auto now = std::chrono::steady_clock::now();
            if (now + present_interval < next_present) {
                next_present = now;
            }
            if (now < next_present) {
                std::this_thread::sleep_until(next_present);
                while (auto* newer_sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) {
                    gst_sample_unref(sample);
                    sample = newer_sample;
                    ++stale_samples_dropped;
                    ++stale_samples_dropped_since_log;
                }
            } else if (now - next_present > present_interval) {
                next_present = now;
            }
        }

        if (!logged_caps) {
            glide::log(glide::LogLevel::info, "OpenHD-Glide", "first decoded video sample caps=" + caps_to_string(gst_sample_get_caps(sample)));
            if (auto* buffer = gst_sample_get_buffer(sample); buffer != nullptr) {
                glide::log(glide::LogLevel::info, "OpenHD-Glide", "first decoded video sample memory=" + describe_buffer_memories(buffer));
            }
        }

        std::string frame_error;

        const auto present_start = std::chrono::steady_clock::now();
        bool presented {};
        if (use_mjpeg) {
            MappedCpuVideoFrame cpu_frame;
            if (!extract_bgrx_cpu_frame(sample, cpu_frame, frame_error)) {
                ++consecutive_frame_errors;
                glide::log(glide::LogLevel::warning, "OpenHD-Glide", "dropping decoded sample: " + frame_error);
                gst_sample_unref(sample);
                if (consecutive_frame_errors >= 30) {
                    glide::log(glide::LogLevel::error, "OpenHD-Glide", "too many consecutive decoded MJPEG samples could not be mapped");
                    stop_requested = 1;
                    break;
                }
                continue;
            }
            if (!logged_caps) {
                std::ostringstream layout;
                layout << "first mapped CPU video layout format=BGRx"
                       << " width=" << cpu_frame.frame.width
                       << " height=" << cpu_frame.frame.height
                       << " stride=" << cpu_frame.frame.stride;
                glide::log(glide::LogLevel::info, "OpenHD-Glide", layout.str());
                logged_caps = true;
            }
            consecutive_frame_errors = 0;
            presented = present_cpu_video_frame(cpu_frame.frame, frames);
        } else {
            glide::dev::DmabufVideoFrame frame;
            if (!extract_dmabuf_frame(sample, frame, frame_error)) {
                ++consecutive_frame_errors;
                glide::log(glide::LogLevel::warning, "OpenHD-Glide", "dropping decoded sample: " + frame_error);
                gst_sample_unref(sample);
                if (consecutive_frame_errors >= 30) {
                    glide::log(glide::LogLevel::error, "OpenHD-Glide", "too many consecutive decoded samples could not be imported");
                    stop_requested = 1;
                    break;
                }
                continue;
            }
            if (!logged_caps) {
                std::ostringstream layout;
                layout << "first imported DMABUF layout format=" << fourcc_to_string(frame.drm_format)
                       << " width=" << frame.width
                       << " height=" << frame.height
                       << " planes=" << frame.plane_count;
                for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
                    layout << " plane" << i
                           << "{fd=" << frame.fds[i]
                           << " stride=" << frame.strides[i]
                           << " offset=" << frame.offsets[i]
                           << " modifier=0x" << std::hex << frame.modifiers[i] << std::dec << "}";
                }
                glide::log(glide::LogLevel::info, "OpenHD-Glide", layout.str());
                logged_caps = true;
            }
            consecutive_frame_errors = 0;
            presented = present_video_frame(frame, frames);
        }
        const auto present_end = std::chrono::steady_clock::now();
        const auto present_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();
        present_ms_total += present_ms;
        present_ms_max = std::max(present_ms_max, present_ms);
        ++present_count;
        if (presented) {
            if (use_mjpeg) {
                gst_sample_unref(sample);
            } else {
                if (scanout_sample != nullptr) {
                    gst_sample_unref(scanout_sample);
                }
                scanout_sample = sample;
            }
            ++frames;
            ++frames_since_log;
            if (software_present_pacing) {
                next_present += present_interval;
            }
        } else {
            gst_sample_unref(sample);
            stop_requested = 1;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_log >= std::chrono::seconds(1)) {
            const auto video_fps_enabled = glide::preview_control::fps_overlay_enabled();
            const auto elapsed = std::chrono::duration<double>(now - last_log).count();
            if (video_fps_enabled) {
                std::ostringstream status;
                status.setf(std::ios::fixed);
                status.precision(1);
                const auto current_fps = static_cast<double>(frames_since_log) / elapsed;
                const auto rtp_packets = ingress_stats.rtp_packets.load(std::memory_order_relaxed);
                const auto parsed_access_units = ingress_stats.parsed_access_units.load(std::memory_order_relaxed);
                const auto rtp_packet_rate = static_cast<double>(rtp_packets - logged_rtp_packets) / elapsed;
                const auto parsed_au_fps = static_cast<double>(parsed_access_units - logged_parsed_access_units) / elapsed;
                logged_rtp_packets = rtp_packets;
                logged_parsed_access_units = parsed_access_units;
                video_plane_fps.store(current_fps);
                status << (use_mjpeg ? "KMS CPU video plane fps=" : "KMS DMABUF video plane fps=") << current_fps
                       << " parsed_au_fps=" << parsed_au_fps
                       << " rtp_packet_rate=" << rtp_packet_rate
                       << " sample_wait_avg_ms=" << (sample_wait_count > 0 ? sample_wait_ms_total / static_cast<double>(sample_wait_count) : 0.0)
                       << " sample_wait_max_ms=" << sample_wait_ms_max
                       << " present_avg_ms=" << (present_count > 0 ? present_ms_total / static_cast<double>(present_count) : 0.0)
                       << " present_max_ms=" << present_ms_max
                       << " total_frames=" << frames
                       << " total_parsed_au=" << parsed_access_units
                       << " stale_samples_dropped=" << stale_samples_dropped_since_log
                       << " total_stale_samples_dropped=" << stale_samples_dropped;
                glide::log(glide::LogLevel::info, "OpenHD-Glide", status.str());
            }
            frames_since_log = 0;
            stale_samples_dropped_since_log = 0;
            sample_wait_ms_total = 0.0;
            sample_wait_ms_max = 0.0;
            sample_wait_count = 0;
            present_ms_total = 0.0;
            present_ms_max = 0.0;
            present_count = 0;
            last_log = now;
        }
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (source_probe_id != 0) {
        if (auto* source = gst_bin_get_by_name(GST_BIN(pipeline), "video-source"); source != nullptr) {
            if (auto* source_pad = gst_element_get_static_pad(source, "src"); source_pad != nullptr) {
                gst_pad_remove_probe(source_pad, source_probe_id);
                gst_object_unref(source_pad);
            }
            gst_object_unref(source);
        }
    }
    if (parsed_probe_id != 0) {
        if (auto* parse = gst_bin_get_by_name(GST_BIN(pipeline), "video-parse"); parse != nullptr) {
            if (auto* parse_pad = gst_element_get_static_pad(parse, "src"); parse_pad != nullptr) {
                gst_pad_remove_probe(parse_pad, parsed_probe_id);
                gst_object_unref(parse_pad);
            }
            gst_object_unref(parse);
        }
    }
    if (scanout_sample != nullptr) {
        gst_sample_unref(scanout_sample);
    }
    gst_object_unref(bus);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return 0;
#else
    glide::log(glide::LogLevel::error, "OpenHD-Glide", "--gstreamer-video requested but GStreamer support was not found at build time");
    return 1;
#endif
#else
    (void)options;
    glide::log(glide::LogLevel::error, "OpenHD-Glide", "--kms-video-preview requires device KMS plus native Cedar or GStreamer support");
    return 1;
#endif
}

void start_mavlink_bridge(glide::mavlink::UdpBridge& bridge, const Options& options)
{
    if (!options.mavlink_udp) {
        return;
    }
    if (bridge.start(glide::mavlink::UdpBridgeOptions { .listen_port = options.mavlink_udp_port })) {
        glide::log(glide::LogLevel::info, "OpenHD-Glide", "MAVLink UDP bridge listening on 0.0.0.0:" + std::to_string(options.mavlink_udp_port));
    } else {
        glide::log(glide::LogLevel::warning, "OpenHD-Glide", "MAVLink UDP bridge disabled: " + bridge.last_error());
    }
}

void start_network_discovery(glide::net::DiscoveryService& discovery, const Options& options)
{
    if (discovery.start(glide::net::DiscoveryOptions {
            .discovery_udp_port = glide::net::discovery_port,
            .video_udp_port = options.view_udp_port,
            .role = "glide",
        })) {
        glide::log(
            glide::LogLevel::info,
            "OpenHD-Glide",
            "Ethernet Glide discovery listening on UDP/0.0.0.0:" + std::to_string(glide::net::discovery_port));
    } else {
        glide::log(glide::LogLevel::warning, "OpenHD-Glide", "Ethernet discovery disabled: " + discovery.last_error());
    }
}

void broadcast_mavlink_updates(glide::mavlink::UdpBridge& bridge, glide::ipc::Server& ipc_server)
{
    for (const auto& line : bridge.poll()) {
        ipc_server.broadcast_line(line);
    }
}

void broadcast_network_updates(glide::net::DiscoveryService& discovery, glide::ipc::Server& ipc_server)
{
    for (const auto& line : discovery.poll_state_lines()) {
        ipc_server.broadcast_line(line);
    }
}

#if defined(__linux__)
pid_t launch_child(const std::vector<std::string>& args)
{
    const auto pid = fork();
    if (pid != 0) {
        return pid;
    }

    std::vector<char*> exec_args;
    exec_args.reserve(args.size() + 1U);
    for (const auto& arg : args) {
        exec_args.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_args.push_back(nullptr);

    execv(exec_args.front(), exec_args.data());
    execvp(exec_args.front(), exec_args.data());
    _exit(127);
}

void terminate_child(pid_t pid)
{
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
}

int run_preview_stack(char* argv0, const Options& options)
{
    stop_requested = 0;
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    const auto ui_executable = sibling_executable(argv0, "glide-ui");
    const auto flow_executable = sibling_executable(argv0, "glide-flow");
    const auto view_executable = sibling_executable(argv0, "glide-view");
    const auto flow_args = preview_args(flow_executable.c_str(), options, false);
    const auto ui_args = preview_args(ui_executable.c_str(), options, true);
    const auto video_args = view_args(view_executable.c_str(), options);

    glide::preview_control::set_fps_overlay_enabled(true);
    glide::preview_control::set_compact_readouts_enabled(false);
    glide::preview_control::set_top_bar_enabled(true);
    glide::ipc::Server ipc_server;
    if (!ipc_server.listen_on(options.ipc_socket)) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to start IPC server: " + ipc_server.last_error());
        return 1;
    }
    glide::mavlink::UdpBridge mavlink_bridge;
    start_mavlink_bridge(mavlink_bridge, options);
    glide::net::DiscoveryService network_discovery;
    start_network_discovery(network_discovery, options);

    glide::log(glide::LogLevel::info, "OpenHD-Glide", "starting WSL preview stack");
    const auto view_pid = launch_child(video_args);
    if (view_pid < 0) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to launch glide-view worker");
        return 1;
    }

    const auto flow_pid = launch_child(flow_args);
    if (flow_pid < 0) {
        terminate_child(view_pid);
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to launch glide-flow preview");
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const auto ui_pid = launch_child(ui_args);
    if (ui_pid < 0) {
        terminate_child(view_pid);
        terminate_child(flow_pid);
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to launch glide-ui preview");
        return 1;
    }

    const auto flow_y = options.vertical_stack ? options.preview_y + static_cast<int>(options.ui_height) : options.preview_y;
    std::cout << "preview stack:\n"
              << "  glide-ui   x=" << options.preview_x << " y=" << options.preview_y
              << " width=" << options.ui_width << " height=" << options.ui_height
              << " opacity=" << options.ui_opacity << '\n'
              << "  glide-flow x=" << options.preview_x << " y=" << flow_y
              << " width=" << options.preview_width << " height=" << options.flow_height << '\n'
              << "  ipc        " << options.ipc_socket << '\n'
              << "  mavlink    " << (mavlink_bridge.running() ? ("udp/0.0.0.0:" + std::to_string(options.mavlink_udp_port)) : "disabled") << '\n';

    bool fps_enabled = true;
    bool coordinates_enabled = true;
    bool compact_readouts = false;
    bool top_bar_enabled = true;
    std::string osd_layout = glide::preview_control::osd_layout();

    while (stop_requested == 0) {
        broadcast_mavlink_updates(mavlink_bridge, ipc_server);
        broadcast_network_updates(network_discovery, ipc_server);
        for (const auto& event : ipc_server.poll()) {
            std::cout << "ipc[" << event.client_id << "] " << event.line << '\n';
            if (event.line.rfind("hello ", 0) == 0) {
                ipc_server.send_line(event.client_id, std::string("state fps ") + (fps_enabled ? "1" : "0"));
                ipc_server.send_line(event.client_id, std::string("state coords ") + (coordinates_enabled ? "1" : "0"));
                ipc_server.send_line(event.client_id, std::string("state compact ") + (compact_readouts ? "1" : "0"));
                ipc_server.send_line(event.client_id, std::string("state topbar ") + (top_bar_enabled ? "1" : "0"));
                ipc_server.send_line(event.client_id, "state osd " + osd_layout);
                send_theme_state(ipc_server, event.client_id);
                send_network_state(ipc_server, event.client_id, network_discovery);
            } else if (event.line == "get fps") {
                ipc_server.send_line(event.client_id, std::string("state fps ") + (fps_enabled ? "1" : "0"));
            } else if (event.line == "get coords") {
                ipc_server.send_line(event.client_id, std::string("state coords ") + (coordinates_enabled ? "1" : "0"));
            } else if (event.line == "get compact") {
                ipc_server.send_line(event.client_id, std::string("state compact ") + (compact_readouts ? "1" : "0"));
            } else if (event.line == "get topbar") {
                ipc_server.send_line(event.client_id, std::string("state topbar ") + (top_bar_enabled ? "1" : "0"));
            } else if (event.line == "get osd") {
                ipc_server.send_line(event.client_id, "state osd " + osd_layout);
            } else if (event.line == "get theme") {
                send_theme_state(ipc_server, event.client_id);
            } else if (handle_network_line(event.line, ipc_server, event.client_id, network_discovery)) {
            } else if (event.line == "set fps 0" || event.line == "set fps 1") {
                fps_enabled = event.line.back() == '1';
                glide::preview_control::set_fps_overlay_enabled(fps_enabled);
                ipc_server.broadcast_line(std::string("state fps ") + (fps_enabled ? "1" : "0"));
            } else if (event.line == "set coords 0" || event.line == "set coords 1") {
                coordinates_enabled = event.line.back() == '1';
                glide::preview_control::set_coordinates_overlay_enabled(coordinates_enabled);
                ipc_server.broadcast_line(std::string("state coords ") + (coordinates_enabled ? "1" : "0"));
            } else if (event.line == "set compact 0" || event.line == "set compact 1") {
                compact_readouts = event.line.back() == '1';
                glide::preview_control::set_compact_readouts_enabled(compact_readouts);
                ipc_server.broadcast_line(std::string("state compact ") + (compact_readouts ? "1" : "0"));
            } else if (event.line == "set topbar 0" || event.line == "set topbar 1") {
                top_bar_enabled = event.line.back() == '1';
                glide::preview_control::set_top_bar_enabled(top_bar_enabled);
                ipc_server.broadcast_line(std::string("state topbar ") + (top_bar_enabled ? "1" : "0"));
            } else if (event.line == "set osd drone" || event.line == "set osd rocket" || event.line == "set osd rover" || event.line == "set osd ship") {
                osd_layout = event.line.substr(8);
                glide::preview_control::set_osd_layout(osd_layout);
                ipc_server.broadcast_line("state osd " + osd_layout);
            } else if (handle_theme_line(event.line, ipc_server)) {
            } else if (event.line.rfind("mav ", 0) == 0) {
                mavlink_bridge.handle_action_line(event.line);
                ipc_server.broadcast_line(event.line);
            } else if (event.line.rfind("ui ", 0) == 0) {
                ipc_server.broadcast_line(event.line);
            } else if (event.line == "ping") {
                ipc_server.send_line(event.client_id, "pong");
            }
        }

        int status {};
        const auto exited = waitpid(-1, &status, WNOHANG);
        if (exited == ui_pid) {
            terminate_child(view_pid);
            terminate_child(flow_pid);
            break;
        }
        if (exited == flow_pid) {
            terminate_child(view_pid);
            terminate_child(ui_pid);
            break;
        }
        if (exited == view_pid) {
            terminate_child(flow_pid);
            terminate_child(ui_pid);
            break;
        }
        if (exited < 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    terminate_child(view_pid);
    terminate_child(ui_pid);
    terminate_child(flow_pid);
    waitpid(view_pid, nullptr, 0);
    waitpid(ui_pid, nullptr, 0);
    waitpid(flow_pid, nullptr, 0);
    glide::log(glide::LogLevel::info, "OpenHD-Glide", "preview stack stopped");
    return 0;
}

int run_kms_stack(char* argv0, const Options& options)
{
#if OPENHD_GLIDE_DEVICE_KMS
    stop_requested = 0;
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    const auto ui_executable = sibling_executable(argv0, "glide-ui");
    const auto flow_executable = sibling_executable(argv0, "glide-flow");
    const auto view_executable = sibling_executable(argv0, "glide-view");
    const auto flow_args = kms_flow_args(flow_executable.c_str(), options);
    const auto ui_args = headless_ui_args(ui_executable.c_str(), options);
    const auto video_args = view_args(view_executable.c_str(), options);

    glide::preview_control::set_fps_overlay_enabled(true);
    glide::preview_control::set_coordinates_overlay_enabled(true);
    glide::preview_control::set_compact_readouts_enabled(false);
    glide::preview_control::set_top_bar_enabled(true);
    glide::ipc::Server ipc_server;
    if (!ipc_server.listen_on(options.ipc_socket)) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to start IPC server: " + ipc_server.last_error());
        return 1;
    }
    glide::mavlink::UdpBridge mavlink_bridge;
    start_mavlink_bridge(mavlink_bridge, options);
    glide::net::DiscoveryService network_discovery;
    start_network_discovery(network_discovery, options);

    glide::log(glide::LogLevel::info, "OpenHD-Glide", "starting device DRM/KMS stack");
    const auto view_pid = launch_child(video_args);
    if (view_pid < 0) {
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to launch glide-view worker");
        return 1;
    }

    const auto flow_pid = launch_child(flow_args);
    if (flow_pid < 0) {
        terminate_child(view_pid);
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to launch glide-flow KMS worker");
        return 1;
    }

    const auto ui_pid = launch_child(ui_args);
    if (ui_pid < 0) {
        terminate_child(view_pid);
        terminate_child(flow_pid);
        glide::log(glide::LogLevel::error, "OpenHD-Glide", "failed to launch glide-ui headless worker");
        return 1;
    }

    const auto view_codec_label = (options.view_udp_codec == "mjpeg" || options.view_udp_codec == "mjpg" || options.view_udp_codec == "jpeg")
        ? "MJPEG"
        : (options.view_udp_codec == "h265" || options.view_udp_codec == "hevc" ? "H265" : "H264");
    std::cout << "device KMS stack:\n"
              << "  glide-view UDP RTP/" << view_codec_label << " decode port=" << options.view_udp_port
              << " (no KMS ownership)\n";
    if (options.view_plane_id >= 0) {
        glide::log(glide::LogLevel::warning, "OpenHD-Glide", "--view-plane-id is ignored until openhd-glide owns video plane import");
    }
    if (options.view_connector_id >= 0) {
        glide::log(glide::LogLevel::warning, "OpenHD-Glide", "--view-connector-id is ignored until openhd-glide owns video plane import");
    }
    std::cout << "  glide-flow drm/kms mode width=" << options.preview_width
              << " height=" << options.flow_height;
    if (options.display_refresh_hz != 0) {
        std::cout << " refresh=" << options.display_refresh_hz << "Hz";
    } else {
        std::cout << " refresh=highest available";
    }
    std::cout << '\n'
              << "  glide-ui   headless LVGL control worker until shared-buffer UI backend exists\n"
              << "  ipc        " << options.ipc_socket << '\n'
              << "  mavlink    " << (mavlink_bridge.running() ? ("udp/0.0.0.0:" + std::to_string(options.mavlink_udp_port)) : "disabled") << '\n';

    bool fps_enabled = true;
    bool coordinates_enabled = true;
    bool compact_readouts = false;
    bool top_bar_enabled = true;
    std::string osd_layout = glide::preview_control::osd_layout();

    while (stop_requested == 0) {
        broadcast_mavlink_updates(mavlink_bridge, ipc_server);
        broadcast_network_updates(network_discovery, ipc_server);
        for (const auto& event : ipc_server.poll()) {
            std::cout << "ipc[" << event.client_id << "] " << event.line << '\n';
            if (event.line.rfind("hello ", 0) == 0) {
                ipc_server.send_line(event.client_id, std::string("state fps ") + (fps_enabled ? "1" : "0"));
                ipc_server.send_line(event.client_id, std::string("state coords ") + (coordinates_enabled ? "1" : "0"));
                ipc_server.send_line(event.client_id, std::string("state compact ") + (compact_readouts ? "1" : "0"));
                ipc_server.send_line(event.client_id, std::string("state topbar ") + (top_bar_enabled ? "1" : "0"));
                ipc_server.send_line(event.client_id, "state osd " + osd_layout);
                send_theme_state(ipc_server, event.client_id);
                send_network_state(ipc_server, event.client_id, network_discovery);
            } else if (event.line == "get fps") {
                ipc_server.send_line(event.client_id, std::string("state fps ") + (fps_enabled ? "1" : "0"));
            } else if (event.line == "get coords") {
                ipc_server.send_line(event.client_id, std::string("state coords ") + (coordinates_enabled ? "1" : "0"));
            } else if (event.line == "get compact") {
                ipc_server.send_line(event.client_id, std::string("state compact ") + (compact_readouts ? "1" : "0"));
            } else if (event.line == "get topbar") {
                ipc_server.send_line(event.client_id, std::string("state topbar ") + (top_bar_enabled ? "1" : "0"));
            } else if (event.line == "get osd") {
                ipc_server.send_line(event.client_id, "state osd " + osd_layout);
            } else if (event.line == "get theme") {
                send_theme_state(ipc_server, event.client_id);
            } else if (handle_network_line(event.line, ipc_server, event.client_id, network_discovery)) {
            } else if (event.line == "set fps 0" || event.line == "set fps 1") {
                fps_enabled = event.line.back() == '1';
                glide::preview_control::set_fps_overlay_enabled(fps_enabled);
                ipc_server.broadcast_line(std::string("state fps ") + (fps_enabled ? "1" : "0"));
            } else if (event.line == "set coords 0" || event.line == "set coords 1") {
                coordinates_enabled = event.line.back() == '1';
                glide::preview_control::set_coordinates_overlay_enabled(coordinates_enabled);
                ipc_server.broadcast_line(std::string("state coords ") + (coordinates_enabled ? "1" : "0"));
            } else if (event.line == "set compact 0" || event.line == "set compact 1") {
                compact_readouts = event.line.back() == '1';
                glide::preview_control::set_compact_readouts_enabled(compact_readouts);
                ipc_server.broadcast_line(std::string("state compact ") + (compact_readouts ? "1" : "0"));
            } else if (event.line == "set topbar 0" || event.line == "set topbar 1") {
                top_bar_enabled = event.line.back() == '1';
                glide::preview_control::set_top_bar_enabled(top_bar_enabled);
                ipc_server.broadcast_line(std::string("state topbar ") + (top_bar_enabled ? "1" : "0"));
            } else if (event.line == "set osd drone" || event.line == "set osd rocket" || event.line == "set osd rover" || event.line == "set osd ship") {
                osd_layout = event.line.substr(8);
                glide::preview_control::set_osd_layout(osd_layout);
                ipc_server.broadcast_line("state osd " + osd_layout);
            } else if (handle_theme_line(event.line, ipc_server)) {
            } else if (event.line.rfind("mav ", 0) == 0) {
                mavlink_bridge.handle_action_line(event.line);
                ipc_server.broadcast_line(event.line);
            } else if (event.line.rfind("ui ", 0) == 0) {
                ipc_server.broadcast_line(event.line);
            } else if (event.line == "ping") {
                ipc_server.send_line(event.client_id, "pong");
            }
        }

        int status {};
        const auto exited = waitpid(-1, &status, WNOHANG);
        if (exited == ui_pid || exited == flow_pid || exited == view_pid || exited < 0) {
            terminate_child(view_pid);
            terminate_child(ui_pid);
            terminate_child(flow_pid);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    terminate_child(view_pid);
    terminate_child(ui_pid);
    terminate_child(flow_pid);
    waitpid(view_pid, nullptr, 0);
    waitpid(ui_pid, nullptr, 0);
    waitpid(flow_pid, nullptr, 0);
    glide::log(glide::LogLevel::info, "OpenHD-Glide", "device DRM/KMS stack stopped");
    return 0;
#else
    glide::log(glide::LogLevel::error, "OpenHD-Glide", "device DRM/KMS mode is disabled in this build");
    return 1;
#endif
}
#else
int run_preview_stack(char*, const Options&)
{
    glide::log(glide::LogLevel::error, "OpenHD-Glide", "--preview-stack currently requires Linux/WSL process launching");
    return 1;
}

int run_kms_stack(char*, const Options&)
{
    glide::log(glide::LogLevel::error, "OpenHD-Glide", "--kms-stack requires Linux process launching and DRM/KMS");
    return 1;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    const auto options = parse_options(argc, argv);
    if (options.ethernet_p2p) {
        const auto result = glide::net::configure_point_to_point(glide::net::PointToPointOptions {
            .interface_name = options.ethernet_p2p_interface,
            .role = options.ethernet_p2p_role,
        });
        std::cout << result << '\n';
        return result.rfind("ok ", 0) == 0 ? 0 : 1;
    }
    if (options.ethernet_discover) {
        const auto peers = glide::net::scan_blocking(glide::net::discovery_port, options.view_udp_port);
        if (peers.empty()) {
            std::cout << "no Glide peers found on the current subnet\n";
            return 1;
        }
        for (const auto& peer : peers) {
            std::cout << peer.hostname << " " << peer.address << ":" << peer.video_port << '\n';
        }
        return 0;
    }

    glide::log(glide::LogLevel::info, "OpenHD-Glide", "starting hardware discovery");

    const auto drm = glide::platform::probe_drm_planes();
    std::cout << glide::platform::describe_drm_probe(drm);

    const auto cpu = glide::platform::probe_cpu_topology();
    std::cout << glide::platform::describe_cpu_topology(cpu);

    const auto openhd_running = glide::platform::is_openhd_process_running();
    if (openhd_running) {
        glide::log(glide::LogLevel::info, "OpenHD-Glide", "OpenHD process detected; avoiding cpu0 for workers");
    }

    const auto assignments = glide::platform::assign_worker_cores(
        cpu,
        glide::platform::CoreAssignmentOptions {
            .avoid_core0 = openhd_running,
        });
    std::cout << glide::platform::describe_worker_core_assignments(assignments);

    glide::log(glide::LogLevel::info, "OpenHD-Glide", "hardware discovery complete");

    if (options.preview_stack) {
        return run_preview_stack(argv[0], options);
    }
    if (options.kms_video_preview) {
        return run_kms_video_preview(options);
    }
    if (options.kms_stack) {
        return run_kms_stack(argv[0], options);
    }

    return 0;
}
