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

#include "common/ipc.hpp"
#include "common/logging.hpp"
#include "common/mavlink_state.hpp"
#include "common/preview_control.hpp"
#include "glide_ui/minimap_widget.hpp"

#if OPENHD_GLIDE_HAS_LVGL
#include "lvgl.h"
#if OPENHD_GLIDE_HAS_LVGL_SDL
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int)
{
    stop_requested = 1;
}

struct HeadlessOptions {
    std::string ipc_socket { glide::ipc::default_socket_path };
};

HeadlessOptions parse_headless_options(int argc, char** argv)
{
    HeadlessOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--ipc-socket" && i + 1 < argc) {
            options.ipc_socket = argv[++i];
        }
    }
    return options;
}

int run_headless_ui(const HeadlessOptions& options)
{
    glide::log(glide::LogLevel::info, "GlideUI", "headless control worker started");

    glide::ipc::Client ipc;
    if (ipc.connect_to(options.ipc_socket)) {
        ipc.send_line("hello glide-ui");
        ipc.send_line("status glide-ui ready headless");
        ipc.send_line("get fps");
        ipc.send_line("get coords");
        ipc.send_line("get compact");
        ipc.send_line("get topbar");
        ipc.send_line("get osd");
        ipc.send_line("get theme");
        ipc.send_line("get net");
    } else {
        glide::log(glide::LogLevel::warning, "GlideUI", "IPC controller unavailable");
    }

    auto next_heartbeat = std::chrono::steady_clock::now();
    glide::mavlink::Snapshot mavlink;
    while (stop_requested == 0) {
        if (ipc.connected()) {
            for (const auto& line : ipc.poll_lines()) {
                if (line == "state fps 0" || line == "state fps 1") {
                    glide::preview_control::set_fps_overlay_enabled(line.back() == '1');
                } else if (line == "state coords 0" || line == "state coords 1") {
                    glide::preview_control::set_coordinates_overlay_enabled(line.back() == '1');
                } else if (line == "state compact 0" || line == "state compact 1") {
                    glide::preview_control::set_compact_readouts_enabled(line.back() == '1');
                } else if (line == "state topbar 0" || line == "state topbar 1") {
                    glide::preview_control::set_top_bar_enabled(line.back() == '1');
                } else if (line == "state osd drone" || line == "state osd rocket" || line == "state osd rover" || line == "state osd ship") {
                    glide::preview_control::set_osd_layout(line.substr(10));
                } else if (line.rfind("state theme ", 0) == 0) {
                    const auto key_start = std::string("state theme ").size();
                    const auto split = line.find(' ', key_start);
                    if (split != std::string::npos) {
                        const auto value = line.substr(split + 1U);
                        if (value.size() == 6) {
                            glide::preview_control::set_theme_color(line.substr(key_start, split - key_start), static_cast<std::uint32_t>(std::stoul(value, nullptr, 16)));
                        }
                    }
                } else {
                    glide::mavlink::apply_ipc_line(mavlink, line);
                }
            }
            if (std::chrono::steady_clock::now() >= next_heartbeat) {
                ipc.send_line("heartbeat glide-ui");
                next_heartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    return 0;
}

} // namespace

#if OPENHD_GLIDE_HAS_LVGL
namespace {

struct Options {
    std::uint32_t width { 760 };
    std::uint32_t height { 720 };
    int x {};
    int y {};
    float opacity { 1.0F };
    bool preview {};
    bool positioned {};
    bool borderless {};
    bool always_on_top {};
    bool transparent_clear {};
    bool headless {};
    bool buffer {};
    std::string buffer_path { "/tmp/openhd-glide-ui.argb" };
    std::string ipc_socket { glide::ipc::default_socket_path };
};

enum class SidebarPanel {
    dashboard = 0,
    link = 1,
    video = 2,
    camera = 3,
    osd = 4,
    telemetry = 5,
    recording = 6,
    system = 7,
    developer = 8,
    colors = 9,
};

enum class OverlayMode {
    menu,
    minimap,
    hidden,
};

struct UiState {
    glide::ipc::Client ipc;
    std::string ipc_socket { glide::ipc::default_socket_path };
    glide::mavlink::Snapshot mavlink;
    bool fps_enabled { true };
    bool coordinates_enabled { true };
    bool compact_readouts { false };
    bool top_bar_enabled { true };
    std::string osd_layout { "drone" };
    std::uint32_t theme_bar_text { 0xebf5ff };
    std::uint32_t theme_bar_background { 0x0e1318 };
    std::uint32_t theme_primary { 0x99ffb8 };
    std::uint32_t theme_secondary { 0x55a8ff };
    bool advanced_visible {};
    bool focus_panel {};
    bool panel_rebuild_pending {};
    OverlayMode overlay_mode { OverlayMode::hidden };
    bool animate_menu_in {};
    int selected_row {};
    int row_count {};
    std::chrono::steady_clock::time_point next_control_file_poll {};
    std::chrono::steady_clock::time_point next_panel_rebuild {};
    lv_obj_t* root {};
    SidebarPanel active_panel { SidebarPanel::dashboard };
    lv_obj_t* panel_title {};
    lv_obj_t* panel_body {};
    lv_obj_t* fps_switch {};
    lv_obj_t* fps_label {};
    lv_obj_t* coordinates_switch {};
    lv_obj_t* coordinates_label {};
    lv_obj_t* compact_switch {};
    lv_obj_t* compact_label {};
    lv_obj_t* top_bar_switch {};
    lv_obj_t* top_bar_label {};
    lv_obj_t* osd_dropdown {};
    lv_obj_t* osd_label {};
    std::array<lv_obj_t*, 4> theme_dropdowns {};
    std::array<lv_obj_t*, 4> theme_labels {};
    lv_obj_t* resolution_dropdown {};
    lv_obj_t* resolution_label {};
    lv_obj_t* display_hz_slider {};
    lv_obj_t* display_hz_label {};
    lv_obj_t* flow_fps_slider {};
    lv_obj_t* flow_fps_label {};
    lv_obj_t* flow_scale_slider {};
    lv_obj_t* flow_scale_label {};
    lv_obj_t* scan_bar {};
    lv_obj_t* scan_percent {};
    lv_obj_t* nav_buttons[10] {};
    std::unique_ptr<glide::ui::MinimapWidget> minimap;
    std::chrono::steady_clock::time_point minimap_started {};
    std::chrono::steady_clock::time_point minimap_last_render {};
    std::chrono::steady_clock::time_point next_ipc_reconnect {};
    std::uint32_t glide_width { 1920 };
    std::uint32_t glide_height { 1080 };
    std::uint32_t display_hz { 120 };
    std::uint32_t flow_fps { 60 };
    std::uint32_t flow_scale_percent { 50 };
    bool net_scanning {};
    std::string net_status { "Idle" };
    std::vector<std::string> net_peers;
};

void dispatch_key(UiState& state, const char* key);

struct BufferDisplay {
    int fd { -1 };
    std::uint32_t width {};
    std::uint32_t height {};
    std::size_t size {};
    void* map {};
    std::vector<std::uint32_t> draw_buffer;
};

BufferDisplay* active_buffer_display {};

struct LinuxKeyboardInput {
    std::vector<int> fds;
    std::chrono::steady_clock::time_point next_scan {};

    ~LinuxKeyboardInput()
    {
        close_all();
    }

    void close_all()
    {
#if defined(__linux__)
        for (const auto fd : fds) {
            if (fd >= 0) {
                close(fd);
            }
        }
#endif
        fds.clear();
    }
};

void clear_active_buffer_display()
{
    if (active_buffer_display == nullptr || active_buffer_display->map == nullptr) {
        return;
    }
    std::memset(active_buffer_display->map, 0, active_buffer_display->size);
#if defined(__linux__)
    msync(active_buffer_display->map, active_buffer_display->size, MS_ASYNC);
#endif
}

void close_buffer_display(BufferDisplay& display)
{
#if defined(__linux__)
    if (display.map != nullptr && display.map != MAP_FAILED) {
        msync(display.map, display.size, MS_SYNC);
        munmap(display.map, display.size);
    }
    if (display.fd >= 0) {
        close(display.fd);
    }
#endif
    display = {};
}

#if defined(__linux__)
std::string linux_key_name(unsigned short code)
{
    switch (code) {
    case KEY_UP:
        return "up";
    case KEY_DOWN:
        return "down";
    case KEY_LEFT:
        return "left";
    case KEY_RIGHT:
        return "right";
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SPACE:
        return "enter";
    case KEY_ESC:
    case KEY_BACKSPACE:
        return "back";
    case KEY_M:
    case KEY_MENU:
        return "m";
    case KEY_N:
        return "n";
    case KEY_EQUAL:
    case KEY_KPPLUS:
        return "+";
    case KEY_MINUS:
    case KEY_KPMINUS:
        return "-";
    default:
        return {};
    }
}

void open_keyboard_candidates(LinuxKeyboardInput& input)
{
    input.close_all();
    std::vector<std::filesystem::path> candidates;
    const auto by_path = std::filesystem::path("/dev/input/by-path");
    std::error_code error;
    if (std::filesystem::is_directory(by_path, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(by_path, error)) {
            const auto path = entry.path();
            const auto name = path.filename().string();
            if (name.find("event-kbd") != std::string::npos) {
                candidates.push_back(path);
            }
        }
    }

    if (candidates.empty()) {
        const auto input_dir = std::filesystem::path("/dev/input");
        if (std::filesystem::is_directory(input_dir, error)) {
            for (const auto& entry : std::filesystem::directory_iterator(input_dir, error)) {
                const auto path = entry.path();
                const auto name = path.filename().string();
                if (name.rfind("event", 0) == 0) {
                    candidates.push_back(path);
                }
            }
        }
    }

    for (const auto& path : candidates) {
        const auto fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            input.fds.push_back(fd);
        }
    }

    if (!input.fds.empty()) {
        glide::log(glide::LogLevel::info, "GlideUI", "Linux keyboard input enabled on " + std::to_string(input.fds.size()) + " event device(s)");
    }
}

void poll_linux_keyboard(LinuxKeyboardInput& input, UiState& state, std::chrono::steady_clock::time_point now)
{
    if (input.fds.empty() && now >= input.next_scan) {
        open_keyboard_candidates(input);
        input.next_scan = now + std::chrono::seconds(2);
    }

    for (const auto fd : input.fds) {
        input_event event {};
        while (read(fd, &event, sizeof(event)) == sizeof(event)) {
            if (event.type != EV_KEY || event.value == 0) {
                continue;
            }
            const auto key = linux_key_name(event.code);
            if (!key.empty()) {
                dispatch_key(state, key.c_str());
            }
        }
    }
}
#else
void poll_linux_keyboard(LinuxKeyboardInput&, UiState&, std::chrono::steady_clock::time_point) {}
#endif

bool create_buffer_display(BufferDisplay& target, const Options& options)
{
#if defined(__linux__)
    close_buffer_display(target);
    target.width = options.width;
    target.height = options.height;
    target.size = static_cast<std::size_t>(target.width) * target.height * sizeof(std::uint32_t);
    target.fd = open(options.buffer_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (target.fd < 0) {
        glide::log(glide::LogLevel::error, "GlideUI", "failed to open UI buffer " + options.buffer_path);
        return false;
    }
    if (ftruncate(target.fd, static_cast<off_t>(target.size)) != 0) {
        glide::log(glide::LogLevel::error, "GlideUI", "failed to resize UI buffer " + options.buffer_path);
        close_buffer_display(target);
        return false;
    }
    target.map = mmap(nullptr, target.size, PROT_READ | PROT_WRITE, MAP_SHARED, target.fd, 0);
    if (target.map == MAP_FAILED) {
        target.map = nullptr;
        glide::log(glide::LogLevel::error, "GlideUI", "failed to mmap UI buffer " + options.buffer_path);
        close_buffer_display(target);
        return false;
    }
    std::fill_n(static_cast<std::uint32_t*>(target.map), target.width * target.height, 0x00000000U);
    target.draw_buffer.resize(static_cast<std::size_t>(target.width) * target.height);
    return true;
#else
    (void)target;
    (void)options;
    glide::log(glide::LogLevel::error, "GlideUI", "buffer display requires Linux mmap support");
    return false;
#endif
}

void buffer_flush(lv_display_t* display, const lv_area_t* area, unsigned char* pixels)
{
    auto* target = static_cast<BufferDisplay*>(lv_display_get_user_data(display));
    if (target == nullptr || target->map == nullptr) {
        lv_display_flush_ready(display);
        return;
    }

    const int32_t x1 = std::max<int32_t>(0, area->x1);
    const int32_t y1 = std::max<int32_t>(0, area->y1);
    const int32_t x2 = std::min<int32_t>(static_cast<int32_t>(target->width) - 1, area->x2);
    const int32_t y2 = std::min<int32_t>(static_cast<int32_t>(target->height) - 1, area->y2);
    if (x2 >= x1 && y2 >= y1) {
        auto* destination = static_cast<std::uint32_t*>(target->map);
        auto* source = reinterpret_cast<const std::uint32_t*>(pixels);
        const auto area_width = static_cast<std::size_t>(area->x2 - area->x1 + 1);
        const auto copy_width = static_cast<std::size_t>(x2 - x1 + 1);
        for (int32_t y = y1; y <= y2; ++y) {
            const auto source_y = static_cast<std::size_t>(y - area->y1);
            const auto source_x = static_cast<std::size_t>(x1 - area->x1);
            auto* destination_row = destination + static_cast<std::size_t>(y) * target->width + static_cast<std::size_t>(x1);
            const auto* source_row = source + source_y * area_width + source_x;
            for (std::size_t x = 0; x < copy_width; ++x) {
                const auto pixel = source_row[x];
                destination_row[x] = (pixel != 0U && (pixel & 0xFF000000U) == 0U) ? (pixel | 0xFF000000U) : pixel;
            }
        }
        msync(target->map, target->size, MS_ASYNC);
    }
    lv_display_flush_ready(display);
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--preview") {
            options.preview = true;
        } else if (argument == "--headless" || argument == "--device") {
            options.headless = true;
        } else if (argument == "--width" && i + 1 < argc) {
            options.width = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--height" && i + 1 < argc) {
            options.height = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if (argument == "--x" && i + 1 < argc) {
            options.x = std::stoi(argv[++i]);
            options.positioned = true;
        } else if (argument == "--y" && i + 1 < argc) {
            options.y = std::stoi(argv[++i]);
            options.positioned = true;
        } else if (argument == "--opacity" && i + 1 < argc) {
            options.opacity = std::clamp(std::stof(argv[++i]), 0.05F, 1.0F);
        } else if (argument == "--borderless") {
            options.borderless = true;
        } else if (argument == "--always-on-top") {
            options.always_on_top = true;
        } else if (argument == "--transparent-clear") {
            options.transparent_clear = true;
        } else if (argument == "--buffer") {
            options.buffer = true;
        } else if (argument == "--buffer-path" && i + 1 < argc) {
            options.buffer_path = argv[++i];
        } else if (argument == "--ipc-socket" && i + 1 < argc) {
            options.ipc_socket = argv[++i];
        }
    }
    return options;
}

lv_color_t color(std::uint32_t rgb)
{
    return lv_color_hex(rgb);
}

void set_panel_style(lv_obj_t* object, std::uint32_t bg, std::uint8_t opacity = LV_OPA_COVER)
{
    lv_obj_set_style_bg_color(object, color(bg), 0);
    lv_obj_set_style_bg_opa(object, opacity, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, std::uint32_t text_color = 0xffffff)
{
    auto* object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color(text_color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    return object;
}

const char* panel_title(SidebarPanel panel)
{
    switch (panel) {
    case SidebarPanel::dashboard:
        return "Dashboard";
    case SidebarPanel::link:
        return "Link";
    case SidebarPanel::video:
        return "Video";
    case SidebarPanel::camera:
        return "Camera";
    case SidebarPanel::osd:
        return "OSD";
    case SidebarPanel::telemetry:
        return "Telemetry";
    case SidebarPanel::recording:
        return "Recording";
    case SidebarPanel::system:
        return "System";
    case SidebarPanel::developer:
        return "Status";
    case SidebarPanel::colors:
        return "Colors";
    }
    return "Dashboard";
}

const char* nav_symbol(int index)
{
    switch (index) {
    case 0:
        return LV_SYMBOL_HOME;
    case 1:
        return LV_SYMBOL_WIFI;
    case 2:
        return LV_SYMBOL_VIDEO;
    case 3:
        return LV_SYMBOL_IMAGE;
    case 4:
        return LV_SYMBOL_SETTINGS;
    case 5:
        return LV_SYMBOL_EYE_OPEN;
    case 6:
        return LV_SYMBOL_SAVE;
    case 7:
        return LV_SYMBOL_SETTINGS;
    case 8:
        return LV_SYMBOL_KEYBOARD;
    case 9:
        return LV_SYMBOL_EDIT;
    }
    return LV_SYMBOL_SETTINGS;
}

const char* nav_text(int index)
{
    switch (index) {
    case 0:
        return "Dashboard";
    case 1:
        return "Link";
    case 2:
        return "Video";
    case 3:
        return "Camera";
    case 4:
        return "OSD";
    case 5:
        return "Telemetry";
    case 6:
        return "Recording";
    case 7:
        return "System";
    case 8:
        return "Status";
    case 9:
        return "Colors";
    }
    return "Menu";
}

void send_fps_state(UiState& state)
{
    glide::preview_control::set_fps_overlay_enabled(state.fps_enabled);
    if (state.ipc.connected()) {
        state.ipc.send_line(std::string("set fps ") + (state.fps_enabled ? "1" : "0"));
    }
}

void sync_fps_controls(UiState& state)
{
    if (state.fps_switch == nullptr || state.fps_label == nullptr) {
        return;
    }

    if (state.fps_enabled) {
        lv_obj_add_state(state.fps_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.fps_label, "Video FPS enabled");
    } else {
        lv_obj_remove_state(state.fps_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.fps_label, "Video FPS disabled");
    }
}

void send_coordinates_state(UiState& state)
{
    glide::preview_control::set_coordinates_overlay_enabled(state.coordinates_enabled);
    if (state.ipc.connected()) {
        state.ipc.send_line(std::string("set coords ") + (state.coordinates_enabled ? "1" : "0"));
    }
}

void sync_coordinates_controls(UiState& state)
{
    if (state.coordinates_switch == nullptr || state.coordinates_label == nullptr) {
        return;
    }

    if (state.coordinates_enabled) {
        lv_obj_add_state(state.coordinates_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.coordinates_label, "Coordinates enabled");
    } else {
        lv_obj_remove_state(state.coordinates_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.coordinates_label, "Coordinates disabled");
    }
}

void send_compact_readouts_state(UiState& state)
{
    glide::preview_control::set_compact_readouts_enabled(state.compact_readouts);
    if (state.ipc.connected()) {
        state.ipc.send_line(std::string("set compact ") + (state.compact_readouts ? "1" : "0"));
    }
}

void sync_compact_readouts_controls(UiState& state)
{
    if (state.compact_switch == nullptr || state.compact_label == nullptr) {
        return;
    }

    if (state.compact_readouts) {
        lv_obj_add_state(state.compact_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.compact_label, "Speed/alt text");
    } else {
        lv_obj_remove_state(state.compact_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.compact_label, "Speed/alt ladder");
    }
}

void send_top_bar_state(UiState& state)
{
    glide::preview_control::set_top_bar_enabled(state.top_bar_enabled);
    if (state.ipc.connected()) {
        state.ipc.send_line(std::string("set topbar ") + (state.top_bar_enabled ? "1" : "0"));
    }
}

void sync_top_bar_controls(UiState& state)
{
    if (state.top_bar_switch == nullptr || state.top_bar_label == nullptr) {
        return;
    }

    if (state.top_bar_enabled) {
        lv_obj_add_state(state.top_bar_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.top_bar_label, "Top link bar enabled");
    } else {
        lv_obj_remove_state(state.top_bar_switch, LV_STATE_CHECKED);
        lv_label_set_text(state.top_bar_label, "Top link bar disabled");
    }
}

void send_osd_layout_state(UiState& state)
{
    glide::preview_control::set_osd_layout(state.osd_layout);
    if (state.ipc.connected()) {
        state.ipc.send_line("set osd " + state.osd_layout);
    }
}

void sync_osd_layout_controls(UiState& state)
{
    if (state.osd_dropdown == nullptr || state.osd_label == nullptr) {
        return;
    }

    const auto selected = state.osd_layout == "rocket" ? 1U : (state.osd_layout == "rover" ? 2U : (state.osd_layout == "ship" ? 3U : 0U));
    lv_dropdown_set_selected(state.osd_dropdown, selected);
    lv_label_set_text(state.osd_label, selected == 1U ? "Rocket OSD" : (selected == 2U ? "Rover OSD" : (selected == 3U ? "Ship OSD" : "Drone OSD")));
}

struct ColorPreset {
    const char* name;
    std::uint32_t rgb;
};

constexpr std::array<ColorPreset, 9> color_presets {{
    { "Default", 0x000000 },
    { "Yellow", 0xffd447 },
    { "Warm White", 0xfff4d6 },
    { "White", 0xebf5ff },
    { "Cyan", 0x70f4ff },
    { "Blue", 0x55a8ff },
    { "Deep Blue", 0x2456ff },
    { "Black", 0x02070a },
    { "Graphite", 0x0e1318 },
}};

constexpr std::array<const char*, 4> theme_keys {{ "bar_text", "bar_background", "primary", "secondary" }};
constexpr std::array<const char*, 4> theme_labels {{ "Font color", "Background color", "Primary color", "Secondary color" }};
constexpr const char* glide_config_path = "/etc/default/openhd-glide";

struct ResolutionPreset {
    const char* label;
    std::uint32_t width;
    std::uint32_t height;
};

constexpr std::array<ResolutionPreset, 4> resolution_presets {{
    { "1920 x 1080", 1920, 1080 },
    { "1600 x 900", 1600, 900 },
    { "1280 x 720", 1280, 720 },
    { "960 x 540", 960, 540 },
}};

constexpr std::array<const char*, 5> openhd_resolution_fps_presets {{
    "1920x1080@120",
    "1920x1080@60",
    "1280x720@120",
    "1280x720@60",
    "0x0@0",
}};

constexpr std::array<int, 4> rotation_presets {{ 0, 90, 180, 270 }};
constexpr std::array<int, 4> tx_power_presets_mw {{ 25, 200, 1200, 2000 }};

std::uint32_t default_theme_color(const char* key)
{
    const std::string name { key };
    if (name == "bar_text") {
        return 0xebf5ff;
    }
    if (name == "bar_background") {
        return 0x0e1318;
    }
    if (name == "primary") {
        return 0x99ffb8;
    }
    if (name == "secondary") {
        return 0x55a8ff;
    }
    return 0xffffff;
}

std::uint32_t& theme_color_ref(UiState& state, std::size_t index)
{
    if (index == 0U) {
        return state.theme_bar_text;
    }
    if (index == 1U) {
        return state.theme_bar_background;
    }
    if (index == 2U) {
        return state.theme_primary;
    }
    return state.theme_secondary;
}

std::uint32_t preset_rgb_for_index(std::size_t channel, std::uint16_t preset)
{
    if (preset == 0U) {
        return default_theme_color(theme_keys[channel]);
    }
    const auto index = std::min<std::size_t>(preset, color_presets.size() - 1U);
    return color_presets[index].rgb;
}

std::uint16_t preset_index_for_rgb(std::size_t channel, std::uint32_t rgb)
{
    const auto normalized = rgb & 0xffffffU;
    if (normalized == default_theme_color(theme_keys[channel])) {
        return 0U;
    }
    for (std::uint16_t i = 1; i < color_presets.size(); ++i) {
        if (color_presets[i].rgb == normalized) {
            return i;
        }
    }
    return 0U;
}

std::string rgb_hex(std::uint32_t rgb)
{
    std::ostringstream stream;
    stream << std::hex << std::setw(6) << std::setfill('0') << (rgb & 0xffffffU);
    return stream.str();
}

std::string trim(std::string value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1U);
}

std::string format_flow_scale(std::uint32_t percent)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << (static_cast<double>(percent) / 100.0);
    return stream.str();
}

std::uint32_t parse_u32_or(std::string value, std::uint32_t fallback)
{
    value = trim(std::move(value));
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(value));
    } catch (...) {
        return fallback;
    }
}

std::uint32_t parse_scale_percent_or(std::string value, std::uint32_t fallback)
{
    value = trim(std::move(value));
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint32_t>(std::clamp(std::lround(std::stod(value) * 100.0), 25L, 100L));
    } catch (...) {
        return fallback;
    }
}

void load_device_settings(UiState& state)
{
    std::ifstream file(glide_config_path);
    std::string line;
    while (std::getline(file, line)) {
        const auto split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }
        const auto key = trim(line.substr(0, split));
        const auto value = trim(line.substr(split + 1U));
        if (key == "GLIDE_WIDTH") {
            state.glide_width = parse_u32_or(value, state.glide_width);
        } else if (key == "GLIDE_HEIGHT") {
            state.glide_height = parse_u32_or(value, state.glide_height);
        } else if (key == "GLIDE_DISPLAY_HZ") {
            state.display_hz = parse_u32_or(value, state.display_hz);
        } else if (key == "GLIDE_FLOW_FPS") {
            state.flow_fps = parse_u32_or(value, state.flow_fps);
        } else if (key == "GLIDE_FLOW_RENDER_SCALE") {
            state.flow_scale_percent = parse_scale_percent_or(value, state.flow_scale_percent);
        }
    }
    state.display_hz = std::clamp<std::uint32_t>(state.display_hz, 24, 144);
    state.flow_fps = std::clamp<std::uint32_t>(state.flow_fps, 15, 120);
    state.flow_scale_percent = std::clamp<std::uint32_t>(state.flow_scale_percent, 25, 100);
}

std::uint16_t resolution_index_for_state(const UiState& state)
{
    for (std::uint16_t i = 0; i < resolution_presets.size(); ++i) {
        if (resolution_presets[i].width == state.glide_width && resolution_presets[i].height == state.glide_height) {
            return i;
        }
    }
    return 0;
}

void sync_video_settings_controls(UiState& state)
{
    if (state.resolution_dropdown != nullptr) {
        lv_dropdown_set_selected(state.resolution_dropdown, resolution_index_for_state(state));
    }
    if (state.resolution_label != nullptr) {
        lv_label_set_text(
            state.resolution_label,
            (std::to_string(state.glide_width) + " x " + std::to_string(state.glide_height)).c_str());
    }
    if (state.display_hz_slider != nullptr) {
        lv_slider_set_value(state.display_hz_slider, static_cast<int32_t>(state.display_hz), LV_ANIM_OFF);
    }
    if (state.display_hz_label != nullptr) {
        lv_label_set_text(state.display_hz_label, (std::to_string(state.display_hz) + " Hz").c_str());
    }
    if (state.flow_fps_slider != nullptr) {
        lv_slider_set_value(state.flow_fps_slider, static_cast<int32_t>(state.flow_fps), LV_ANIM_OFF);
    }
    if (state.flow_fps_label != nullptr) {
        lv_label_set_text(state.flow_fps_label, (std::to_string(state.flow_fps) + " fps").c_str());
    }
    if (state.flow_scale_slider != nullptr) {
        lv_slider_set_value(state.flow_scale_slider, static_cast<int32_t>(state.flow_scale_percent), LV_ANIM_OFF);
    }
    if (state.flow_scale_label != nullptr) {
        lv_label_set_text(state.flow_scale_label, (format_flow_scale(state.flow_scale_percent) + "x").c_str());
    }
}

bool write_or_replace_setting(std::vector<std::string>& lines, const std::string& key, const std::string& value)
{
    const auto prefix = key + "=";
    for (auto& line : lines) {
        if (line.rfind(prefix, 0) == 0) {
            if (line == prefix + value) {
                return false;
            }
            line = prefix + value;
            return true;
        }
    }
    lines.push_back(prefix + value);
    return true;
}

void persist_device_settings_and_restart(UiState& state)
{
    std::vector<std::string> lines;
    {
        std::ifstream input(glide_config_path);
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
        }
    }
    bool changed {};
    changed |= write_or_replace_setting(lines, "GLIDE_WIDTH", std::to_string(state.glide_width));
    changed |= write_or_replace_setting(lines, "GLIDE_HEIGHT", std::to_string(state.glide_height));
    changed |= write_or_replace_setting(lines, "GLIDE_DISPLAY_HZ", std::to_string(state.display_hz));
    changed |= write_or_replace_setting(lines, "GLIDE_FLOW_FPS", std::to_string(state.flow_fps));
    changed |= write_or_replace_setting(lines, "GLIDE_FLOW_RENDER_SCALE", format_flow_scale(state.flow_scale_percent));
    if (!changed) {
        return;
    }

    const auto temp_path = std::filesystem::temp_directory_path() / "openhd-glide.default.tmp";
    {
        std::ofstream output(temp_path);
        for (const auto& line : lines) {
            output << line << '\n';
        }
    }
    std::error_code copy_error;
    std::filesystem::copy_file(temp_path, glide_config_path, std::filesystem::copy_options::overwrite_existing, copy_error);
    std::error_code remove_error;
    std::filesystem::remove(temp_path, remove_error);
    if (copy_error) {
        glide::log(glide::LogLevel::error, "GlideUI", "failed to write /etc/default/openhd-glide: " + copy_error.message());
        return;
    }
    sync_video_settings_controls(state);
    glide::log(glide::LogLevel::info, "GlideUI", "saved video mode settings; restarting openhd-glide.service");
    std::system("systemctl restart --no-block openhd-glide.service >/dev/null 2>&1 &");
}

void set_resolution_preset(UiState& state, std::uint16_t index, bool persist)
{
    const auto bounded = std::min<std::size_t>(index, resolution_presets.size() - 1U);
    state.glide_width = resolution_presets[bounded].width;
    state.glide_height = resolution_presets[bounded].height;
    sync_video_settings_controls(state);
    if (persist) {
        persist_device_settings_and_restart(state);
    }
}

void sync_theme_controls(UiState& state);

bool apply_theme_state_line(UiState& state, const std::string& line)
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
        const auto rgb = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        for (std::size_t i = 0; i < theme_keys.size(); ++i) {
            if (key == theme_keys[i]) {
                theme_color_ref(state, i) = rgb;
                glide::preview_control::set_theme_color(key, rgb);
                break;
            }
        }
        sync_theme_controls(state);
    }
    return true;
}

void send_theme_color(UiState& state, std::size_t index)
{
    const auto rgb = theme_color_ref(state, index);
    glide::preview_control::set_theme_color(theme_keys[index], rgb);
    if (state.ipc.connected()) {
        state.ipc.send_line(std::string("set theme ") + theme_keys[index] + " " + rgb_hex(rgb));
    }
}

void sync_theme_controls(UiState& state)
{
    for (std::size_t i = 0; i < theme_keys.size(); ++i) {
        if (state.theme_dropdowns[i] != nullptr) {
            lv_dropdown_set_selected(state.theme_dropdowns[i], preset_index_for_rgb(i, theme_color_ref(state, i)));
        }
        if (state.theme_labels[i] != nullptr) {
            lv_label_set_text(state.theme_labels[i], theme_labels[i]);
        }
    }
}

bool update_bool(bool& target, bool value)
{
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

bool update_string(std::string& target, const std::string& value)
{
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

void send_mavlink_action(UiState& state, const std::string& line)
{
    if (state.ipc.connected()) {
        state.ipc.send_line(line);
    }
}

void send_network_action(UiState& state, const std::string& line)
{
    if (state.ipc.connected()) {
        state.ipc.send_line(line);
    }
}

void send_openhd_param(UiState& state, const std::string& target, const std::string& param, const std::string& value)
{
    send_mavlink_action(state, glide::mavlink::format_action_set_param(target, param, value));
}

std::string next_resolution_fps_value(const std::string& current)
{
    for (std::size_t i = 0; i < openhd_resolution_fps_presets.size(); ++i) {
        if (current.find(openhd_resolution_fps_presets[i]) != std::string::npos) {
            return openhd_resolution_fps_presets[(i + 1U) % openhd_resolution_fps_presets.size()];
        }
    }
    return openhd_resolution_fps_presets.front();
}

int next_value_in_cycle(int current, const int* values, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (current == values[i]) {
            return values[(i + 1U) % count];
        }
        if (current < values[i]) {
            return values[i];
        }
    }
    return values[0];
}

int next_rotation_value(const std::string& current)
{
    int parsed {};
    try {
        parsed = std::stoi(current);
    } catch (...) {
        parsed = 0;
    }
    return next_value_in_cycle(parsed, rotation_presets.data(), rotation_presets.size());
}

int next_channel_width_value(int current)
{
    return current >= 40 ? 20 : 40;
}

int next_mcs_value(int current)
{
    return current >= 4 ? 0 : std::max(0, current + 1);
}

int next_tx_power_value(int current)
{
    return next_value_in_cycle(current, tx_power_presets_mw.data(), tx_power_presets_mw.size());
}

lv_obj_t* value_row(UiState& state, const char* title, const std::string& value, std::uint32_t value_color = 0xffffff)
{
    const int row_index = state.row_count++;
    auto* row = lv_obj_create(state.panel_body);
    set_panel_style(row, state.focus_panel && state.selected_row == row_index ? 0x182f40 : 0x0d1b27, LV_OPA_70);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_border_width(row, state.focus_panel && state.selected_row == row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, color(0xff8a00), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* left = label(row, title, &lv_font_montserrat_14, 0xaab7c2);
    lv_obj_set_width(left, LV_PCT(45));
    auto* right = label(row, value.c_str(), &lv_font_montserrat_14, value_color);
    lv_obj_set_width(right, LV_PCT(48));
    lv_obj_set_style_text_align(right, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(right, LV_LABEL_LONG_DOT);
    return row;
}

lv_obj_t* action_button(UiState& state, const char* text)
{
    const int row_index = state.row_count++;
    auto* button = lv_button_create(state.panel_body);
    set_panel_style(button, state.focus_panel && state.selected_row == row_index ? 0x2d210e : 0x1a2530, LV_OPA_90);
    lv_obj_set_size(button, LV_PCT(100), 44);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_bg_color(button, color(0x332410), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, color(0x263746), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, color(0x5d6f7f), 0);

    auto* button_label = label(button, text, &lv_font_montserrat_14, 0xffffff);
    lv_obj_center(button_label);
    return button;
}

void setup_panel_column(lv_obj_t* body, int pad = 14)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, pad, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
}

void set_active_panel(UiState& state, SidebarPanel panel);
void build_sidebar(UiState& state, std::uint32_t width, std::uint32_t height);
void build_overlay(UiState& state, std::uint32_t width, std::uint32_t height);

bool panel_uses_mavlink(SidebarPanel panel)
{
    return panel == SidebarPanel::dashboard
        || panel == SidebarPanel::link
        || panel == SidebarPanel::video
        || panel == SidebarPanel::camera
        || panel == SidebarPanel::telemetry
        || panel == SidebarPanel::recording
        || panel == SidebarPanel::system
        || panel == SidebarPanel::developer;
}

void request_panel_rebuild(UiState& state, std::chrono::steady_clock::time_point now)
{
    state.panel_rebuild_pending = true;
    if (state.next_panel_rebuild <= now) {
        state.next_panel_rebuild = now + std::chrono::milliseconds(100);
    }
}

void rebuild_ui(UiState& state)
{
    if (state.root == nullptr) {
        return;
    }
    const auto width = lv_obj_get_width(state.root);
    const auto height = lv_obj_get_height(state.root);
    state.minimap.reset();
    lv_obj_clean(state.root);
    build_overlay(state, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    state.panel_rebuild_pending = false;
}

bool panel_uses_network(SidebarPanel panel)
{
    return panel == SidebarPanel::link || panel == SidebarPanel::system;
}

void show_menu(UiState& state)
{
    if (state.overlay_mode == OverlayMode::menu) {
        return;
    }
    state.overlay_mode = OverlayMode::menu;
    state.animate_menu_in = true;
    state.focus_panel = false;
    rebuild_ui(state);
}

void hide_overlay(UiState& state)
{
    if (state.overlay_mode == OverlayMode::hidden) {
        clear_active_buffer_display();
        return;
    }
    state.overlay_mode = OverlayMode::hidden;
    state.focus_panel = false;
    rebuild_ui(state);
}

void cycle_overlay_from_menu_key(UiState& state)
{
    if (state.overlay_mode == OverlayMode::hidden) {
        show_menu(state);
        return;
    } else if (state.overlay_mode == OverlayMode::menu) {
        hide_overlay(state);
        return;
    } else {
        show_menu(state);
        return;
    }
}

void cycle_overlay_from_map_key(UiState& state)
{
    if (state.overlay_mode == OverlayMode::hidden) {
        state.overlay_mode = OverlayMode::minimap;
    } else if (state.overlay_mode == OverlayMode::minimap) {
        state.overlay_mode = OverlayMode::menu;
    } else {
        state.overlay_mode = OverlayMode::hidden;
    }
    state.focus_panel = false;
    rebuild_ui(state);
}

void apply_terminal_key(UiState& state, const std::string& line)
{
    if (line == "ui show" || line == "ui menu") {
        show_menu(state);
        return;
    }
    if (line == "ui hide" || line == "ui close") {
        hide_overlay(state);
        return;
    }
    if (line == "ui toggle") {
        cycle_overlay_from_menu_key(state);
        return;
    }
    if (line.rfind("ui key ", 0) != 0) {
        return;
    }
    const auto key = line.substr(7);
    if (key == "m" || key == "menu") {
        cycle_overlay_from_menu_key(state);
        return;
    }
    if (key == "n" || key == "map") {
        cycle_overlay_from_map_key(state);
        return;
    }
    if ((key == "+" || key == "=" || key == "zoom-in") && state.overlay_mode == OverlayMode::minimap && state.minimap) {
        state.minimap->set_zoom(state.minimap->zoom() + 1);
        state.minimap->render();
        return;
    }
    if ((key == "-" || key == "_" || key == "zoom-out") && state.overlay_mode == OverlayMode::minimap && state.minimap) {
        state.minimap->set_zoom(state.minimap->zoom() - 1);
        state.minimap->render();
        return;
    }
    if (state.overlay_mode == OverlayMode::hidden && key == "enter") {
        show_menu(state);
        return;
    }
    if (state.overlay_mode != OverlayMode::menu) {
        return;
    }
    auto index = static_cast<int>(state.active_panel);
    if (key == "up") {
        if (state.focus_panel) {
            state.selected_row = std::max(0, state.selected_row - 1);
            rebuild_ui(state);
        } else {
            index = std::max(0, index - 1);
            set_active_panel(state, static_cast<SidebarPanel>(index));
        }
    } else if (key == "down") {
        if (state.focus_panel) {
            state.selected_row = std::min(std::max(0, state.row_count - 1), state.selected_row + 1);
            rebuild_ui(state);
        } else {
            index = std::min(9, index + 1);
            set_active_panel(state, static_cast<SidebarPanel>(index));
        }
    } else if (key == "left" || key == "back") {
        if (state.focus_panel) {
            state.focus_panel = false;
            rebuild_ui(state);
        } else {
            hide_overlay(state);
        }
    } else if (key == "right") {
        state.focus_panel = true;
        state.selected_row = 0;
        rebuild_ui(state);
    } else if (key == "enter") {
        if (!state.focus_panel) {
            state.focus_panel = true;
            state.selected_row = 0;
            rebuild_ui(state);
        } else if (state.active_panel == SidebarPanel::dashboard && state.selected_row == 4) {
            send_mavlink_action(state, glide::mavlink::format_action_command("scan", "bands=openhd width=" + std::to_string(state.mavlink.channel_width_mhz)));
        } else if (state.active_panel == SidebarPanel::link && state.selected_row == 4) {
            send_openhd_param(state, "air", "WB_CHANNEL_W", std::to_string(next_channel_width_value(state.mavlink.channel_width_mhz)));
        } else if (state.active_panel == SidebarPanel::link && state.selected_row == 5) {
            send_openhd_param(state, "air", "WB_MCS_INDEX", std::to_string(next_mcs_value(state.mavlink.mcs_index)));
        } else if (state.active_panel == SidebarPanel::link && state.selected_row == 6) {
            send_openhd_param(state, "air", "TX_POWER_MW", std::to_string(next_tx_power_value(state.mavlink.tx_power_mw)));
        } else if (state.active_panel == SidebarPanel::link && state.selected_row == 7) {
            send_openhd_param(state, "air", "WB_PIT_MODE", "1");
        } else if (state.active_panel == SidebarPanel::link && state.selected_row == 8) {
            send_openhd_param(state, "air", "WB_PIT_MODE", "0");
        } else if (state.active_panel == SidebarPanel::video && state.selected_row == 0) {
            const auto next = static_cast<std::uint16_t>((resolution_index_for_state(state) + 1U) % resolution_presets.size());
            set_resolution_preset(state, next, true);
        } else if (state.active_panel == SidebarPanel::video && state.selected_row == 1) {
            state.display_hz = state.display_hz >= 120 ? 60 : (state.display_hz >= 100 ? 120 : 100);
            sync_video_settings_controls(state);
            persist_device_settings_and_restart(state);
        } else if (state.active_panel == SidebarPanel::video && state.selected_row == 2) {
            state.flow_fps = state.flow_fps >= 60 ? 30 : 60;
            sync_video_settings_controls(state);
            persist_device_settings_and_restart(state);
        } else if (state.active_panel == SidebarPanel::video && state.selected_row == 3) {
            state.flow_scale_percent = state.flow_scale_percent >= 100 ? 25 : state.flow_scale_percent + 25;
            sync_video_settings_controls(state);
            persist_device_settings_and_restart(state);
        } else if (state.active_panel == SidebarPanel::osd && state.selected_row == 0) {
            if (state.osd_layout == "drone") {
                state.osd_layout = "rocket";
            } else if (state.osd_layout == "rocket") {
                state.osd_layout = "rover";
            } else if (state.osd_layout == "rover") {
                state.osd_layout = "ship";
            } else {
                state.osd_layout = "drone";
            }
            sync_osd_layout_controls(state);
            send_osd_layout_state(state);
        } else if (state.active_panel == SidebarPanel::osd && state.selected_row == 1) {
            state.top_bar_enabled = !state.top_bar_enabled;
            sync_top_bar_controls(state);
            send_top_bar_state(state);
        } else if (state.active_panel == SidebarPanel::osd && state.selected_row == 2) {
            state.fps_enabled = !state.fps_enabled;
            sync_fps_controls(state);
            send_fps_state(state);
        } else if (state.active_panel == SidebarPanel::osd && state.selected_row == 3) {
            state.coordinates_enabled = !state.coordinates_enabled;
            sync_coordinates_controls(state);
            send_coordinates_state(state);
        } else if (state.active_panel == SidebarPanel::osd && state.selected_row == 4) {
            state.compact_readouts = !state.compact_readouts;
            sync_compact_readouts_controls(state);
            send_compact_readouts_state(state);
        } else if (state.active_panel == SidebarPanel::colors && state.selected_row >= 0 && state.selected_row <= 1) {
            const auto index = static_cast<std::size_t>(state.selected_row);
            const auto current = preset_index_for_rgb(index, theme_color_ref(state, index));
            const auto next = static_cast<std::uint16_t>((current + 1U) % color_presets.size());
            const auto rgb = preset_rgb_for_index(index, next);
            theme_color_ref(state, index) = rgb;
            send_theme_color(state, index);
            sync_theme_controls(state);
        } else if (state.active_panel == SidebarPanel::camera && state.selected_row == 3) {
            send_openhd_param(state, "camera1", "RESOLUTION_FPS", next_resolution_fps_value(state.mavlink.resolution_fps));
        } else if (state.active_panel == SidebarPanel::camera && state.selected_row == 4) {
            send_openhd_param(state, "camera1", "ROTATION_DEG", std::to_string(next_rotation_value(state.mavlink.rotation)));
        } else if (state.active_panel == SidebarPanel::camera && state.selected_row == 5) {
            send_openhd_param(state, "camera1", "INTRA_REFRESH", "0");
        } else if (state.active_panel == SidebarPanel::camera && state.selected_row == 6) {
            send_openhd_param(state, "camera1", "INTRA_REFRESH", "1");
        } else if (state.active_panel == SidebarPanel::recording && state.selected_row == 2) {
            send_openhd_param(state, "camera1", "AIR_RECORDING_E", "0");
        } else if (state.active_panel == SidebarPanel::recording && state.selected_row == 3) {
            send_openhd_param(state, "camera1", "AIR_RECORDING_E", "1");
        } else if (state.active_panel == SidebarPanel::recording && state.selected_row == 4) {
            send_openhd_param(state, "camera1", "AIR_RECORDING_E", "2");
        } else if (state.active_panel == SidebarPanel::developer && state.selected_row == 5) {
            state.advanced_visible = !state.advanced_visible;
            rebuild_ui(state);
        }
    }
}

void dispatch_key(UiState& state, const char* key)
{
    apply_terminal_key(state, std::string("ui key ") + key);
}

void keyboard_event(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
    if (state == nullptr) {
        return;
    }

    switch (lv_event_get_key(event)) {
    case 'm':
    case 'M':
        dispatch_key(*state, "m");
        break;
    case 'n':
    case 'N':
        dispatch_key(*state, "n");
        break;
    case '+':
    case '=':
        dispatch_key(*state, "+");
        break;
    case '-':
    case '_':
        dispatch_key(*state, "-");
        break;
    case LV_KEY_UP:
        dispatch_key(*state, "up");
        break;
    case LV_KEY_DOWN:
        dispatch_key(*state, "down");
        break;
    case LV_KEY_LEFT:
    case LV_KEY_ESC:
    case LV_KEY_BACKSPACE:
        dispatch_key(*state, "back");
        break;
    case LV_KEY_RIGHT:
        dispatch_key(*state, "right");
        break;
    case LV_KEY_ENTER:
        dispatch_key(*state, "enter");
        break;
    default:
        break;
    }
}

void build_dashboard_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    const auto connection = state.mavlink.air_alive && state.mavlink.ground_alive
        ? std::string("Connected")
        : (state.mavlink.air_alive ? std::string("AIR only") : (state.mavlink.ground_alive ? std::string("GND only") : std::string("Searching")));
    const auto connection_color = state.mavlink.air_alive && state.mavlink.ground_alive ? 0x3df0b2 : 0xff8a00;
    value_row(state, "Link State", connection, connection_color);
    value_row(state, "Air Unit", state.mavlink.air_alive ? "Online" : "Waiting", state.mavlink.air_alive ? 0x3df0b2 : 0xff8a00);
    value_row(state, "Channels", "OpenHD [1-7]");
    value_row(state, "Bandwidth", std::to_string(state.mavlink.channel_width_mhz) + " MHz");

    auto* scan = action_button(state, "FIND AIR UNIT");
    lv_obj_add_event_cb(
        scan,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_mavlink_action(*state, glide::mavlink::format_action_command("scan", "bands=openhd width=" + std::to_string(state->mavlink.channel_width_mhz)));
        },
        LV_EVENT_CLICKED,
        &state);

    auto* progress_row = lv_obj_create(state.panel_body);
    set_panel_style(progress_row, 0x0b1722, LV_OPA_TRANSP);
    lv_obj_set_size(progress_row, LV_PCT(100), 70);
    state.scan_bar = lv_bar_create(progress_row);
    lv_obj_set_size(state.scan_bar, 300, 8);
    lv_obj_align(state.scan_bar, LV_ALIGN_CENTER, -42, 0);
    lv_bar_set_value(state.scan_bar, state.mavlink.scan_progress, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(state.scan_bar, color(0x203040), 0);
    lv_obj_set_style_bg_color(state.scan_bar, color(0x20b383), LV_PART_INDICATOR);
    state.scan_percent = label(progress_row, (std::to_string(state.mavlink.scan_progress) + "%").c_str(), &lv_font_montserrat_28);
    lv_obj_align(state.scan_percent, LV_ALIGN_CENTER, 210, 0);
}

void build_osd_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    auto* selector_section = label(state.panel_body, "Layout", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(selector_section, LV_PCT(100));

    const int selector_row_index = state.row_count++;
    auto* selector_row = lv_obj_create(state.panel_body);
    set_panel_style(selector_row, state.focus_panel && state.selected_row == selector_row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(selector_row, 6, 0);
    lv_obj_set_style_border_width(selector_row, state.focus_panel && state.selected_row == selector_row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(selector_row, color(0xff8a00), 0);
    lv_obj_set_size(selector_row, LV_PCT(100), 62);
    lv_obj_set_style_pad_left(selector_row, 16, 0);
    lv_obj_set_style_pad_right(selector_row, 16, 0);
    lv_obj_set_flex_flow(selector_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(selector_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state.osd_label = label(selector_row, "Drone OSD", &lv_font_montserrat_16, 0xdce8f0);
    state.osd_dropdown = lv_dropdown_create(selector_row);
    lv_dropdown_set_options(state.osd_dropdown, "Drone\nRocket\nRover\nShip");
    lv_obj_set_size(state.osd_dropdown, 150, 38);
    lv_obj_set_style_bg_color(state.osd_dropdown, color(0x162a3a), 0);
    lv_obj_set_style_text_color(state.osd_dropdown, color(0xffffff), 0);
    lv_obj_add_event_cb(
        state.osd_dropdown,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            const auto selected = lv_dropdown_get_selected(state->osd_dropdown);
            state->osd_layout = selected == 1U ? "rocket" : (selected == 2U ? "rover" : (selected == 3U ? "ship" : "drone"));
            sync_osd_layout_controls(*state);
            send_osd_layout_state(*state);
        },
        LV_EVENT_VALUE_CHANGED,
        &state);
    sync_osd_layout_controls(state);

    auto* future = label(state.panel_body, "Future: Sub", &lv_font_montserrat_12, 0x8fa4b4);
    lv_obj_set_width(future, LV_PCT(100));

    auto* section = label(state.panel_body, "Overlay", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(section, LV_PCT(100));

    const int top_bar_row_index = state.row_count++;
    auto* top_bar_row = lv_obj_create(state.panel_body);
    set_panel_style(top_bar_row, state.focus_panel && state.selected_row == top_bar_row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(top_bar_row, 6, 0);
    lv_obj_set_style_border_width(top_bar_row, state.focus_panel && state.selected_row == top_bar_row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(top_bar_row, color(0xff8a00), 0);
    lv_obj_set_size(top_bar_row, LV_PCT(100), 62);
    lv_obj_set_style_pad_left(top_bar_row, 16, 0);
    lv_obj_set_style_pad_right(top_bar_row, 16, 0);
    lv_obj_set_flex_flow(top_bar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state.top_bar_label = label(top_bar_row, "Top link bar", &lv_font_montserrat_16, 0xdce8f0);
    state.top_bar_switch = lv_switch_create(top_bar_row);
    lv_obj_set_size(state.top_bar_switch, 58, 30);
    lv_obj_add_event_cb(
        state.top_bar_switch,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            state->top_bar_enabled = lv_obj_has_state(state->top_bar_switch, LV_STATE_CHECKED);
            sync_top_bar_controls(*state);
            send_top_bar_state(*state);
        },
        LV_EVENT_VALUE_CHANGED,
        &state);
    sync_top_bar_controls(state);

    const int row_index = state.row_count++;
    auto* row = lv_obj_create(state.panel_body);
    set_panel_style(row, state.focus_panel && state.selected_row == row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, state.focus_panel && state.selected_row == row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, color(0xff8a00), 0);
    lv_obj_set_size(row, LV_PCT(100), 62);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state.fps_label = label(row, "Video FPS", &lv_font_montserrat_16, 0xdce8f0);
    state.fps_switch = lv_switch_create(row);
    lv_obj_set_size(state.fps_switch, 58, 30);
    lv_obj_add_event_cb(
        state.fps_switch,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            state->fps_enabled = lv_obj_has_state(state->fps_switch, LV_STATE_CHECKED);
            sync_fps_controls(*state);
            send_fps_state(*state);
        },
        LV_EVENT_VALUE_CHANGED,
        &state);
    sync_fps_controls(state);

    const int coordinates_row_index = state.row_count++;
    auto* coordinates_row = lv_obj_create(state.panel_body);
    set_panel_style(coordinates_row, state.focus_panel && state.selected_row == coordinates_row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(coordinates_row, 6, 0);
    lv_obj_set_style_border_width(coordinates_row, state.focus_panel && state.selected_row == coordinates_row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(coordinates_row, color(0xff8a00), 0);
    lv_obj_set_size(coordinates_row, LV_PCT(100), 62);
    lv_obj_set_style_pad_left(coordinates_row, 16, 0);
    lv_obj_set_style_pad_right(coordinates_row, 16, 0);
    lv_obj_set_flex_flow(coordinates_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(coordinates_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state.coordinates_label = label(coordinates_row, "Coordinates", &lv_font_montserrat_16, 0xdce8f0);
    state.coordinates_switch = lv_switch_create(coordinates_row);
    lv_obj_set_size(state.coordinates_switch, 58, 30);
    lv_obj_add_event_cb(
        state.coordinates_switch,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            state->coordinates_enabled = lv_obj_has_state(state->coordinates_switch, LV_STATE_CHECKED);
            sync_coordinates_controls(*state);
            send_coordinates_state(*state);
        },
        LV_EVENT_VALUE_CHANGED,
        &state);
    sync_coordinates_controls(state);

    const int compact_row_index = state.row_count++;
    auto* compact_row = lv_obj_create(state.panel_body);
    set_panel_style(compact_row, state.focus_panel && state.selected_row == compact_row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(compact_row, 6, 0);
    lv_obj_set_style_border_width(compact_row, state.focus_panel && state.selected_row == compact_row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(compact_row, color(0xff8a00), 0);
    lv_obj_set_size(compact_row, LV_PCT(100), 62);
    lv_obj_set_style_pad_left(compact_row, 16, 0);
    lv_obj_set_style_pad_right(compact_row, 16, 0);
    lv_obj_set_flex_flow(compact_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(compact_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state.compact_label = label(compact_row, "Speed/alt ladder", &lv_font_montserrat_16, 0xdce8f0);
    state.compact_switch = lv_switch_create(compact_row);
    lv_obj_set_size(state.compact_switch, 58, 30);
    lv_obj_add_event_cb(
        state.compact_switch,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            state->compact_readouts = lv_obj_has_state(state->compact_switch, LV_STATE_CHECKED);
            sync_compact_readouts_controls(*state);
            send_compact_readouts_state(*state);
        },
        LV_EVENT_VALUE_CHANGED,
        &state);
    sync_compact_readouts_controls(state);
}

void build_colors_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    auto* section = label(state.panel_body, "Theme", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(section, LV_PCT(100));

    const char* options = "Default\nYellow\nWarm White\nWhite\nCyan\nBlue\nDeep Blue\nBlack\nGraphite";
    for (std::size_t i = 0; i < theme_keys.size(); ++i) {
        const int row_index = state.row_count++;
        auto* row = lv_obj_create(state.panel_body);
        set_panel_style(row, state.focus_panel && state.selected_row == row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, state.focus_panel && state.selected_row == row_index ? 1 : 0, 0);
        lv_obj_set_style_border_color(row, color(0xff8a00), 0);
        lv_obj_set_size(row, LV_PCT(100), 62);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_set_style_pad_right(row, 16, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto* title = label(row, theme_labels[i], &lv_font_montserrat_14, 0xdce8f0);
        lv_obj_set_width(title, LV_PCT(42));
        state.theme_labels[i] = title;
        state.theme_dropdowns[i] = lv_dropdown_create(row);
        lv_obj_set_user_data(state.theme_dropdowns[i], reinterpret_cast<void*>(static_cast<std::intptr_t>(i)));
        lv_dropdown_set_options(state.theme_dropdowns[i], options);
        lv_obj_set_size(state.theme_dropdowns[i], 132, 38);
        lv_obj_set_style_bg_color(state.theme_dropdowns[i], color(0x162a3a), 0);
        lv_obj_set_style_text_color(state.theme_dropdowns[i], color(0xffffff), 0);
        lv_obj_add_event_cb(
            state.theme_dropdowns[i],
            [](lv_event_t* event) {
                auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
                auto* target = lv_event_get_target_obj(event);
                const auto index = static_cast<std::size_t>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(target)));
                const auto selected = lv_dropdown_get_selected(target);
                const auto rgb = preset_rgb_for_index(index, selected);
                theme_color_ref(*state, index) = rgb;
                send_theme_color(*state, index);
                sync_theme_controls(*state);
            },
            LV_EVENT_VALUE_CHANGED,
            &state);
    }
    sync_theme_controls(state);
}

void build_system_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    value_row(state, "Air WiFi Mode", state.mavlink.air_wifi_mode);
    value_row(state, "Ground WiFi Mode", state.mavlink.ground_wifi_mode);
    value_row(state, "Air Hotspot", state.mavlink.air_hotspot);
    value_row(state, "Ground Hotspot", state.mavlink.ground_hotspot);
}

void build_link_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    value_row(state, "Frequency", std::to_string(state.mavlink.frequency_mhz) + " MHz");
    value_row(state, "Channel Width", std::to_string(state.mavlink.channel_width_mhz) + " MHz");
    value_row(state, "Modulation", "MCS " + std::to_string(state.mavlink.mcs_index));
    value_row(state, "TX Power", std::to_string(state.mavlink.tx_power_mw) + " mW");
    auto* ethernet_section = label(state.panel_body, "Ethernet Glide", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(ethernet_section, LV_PCT(100));
    value_row(state, "Discovery", state.net_status, state.net_scanning ? 0xff8a00 : 0x20b383);
    if (state.net_peers.empty()) {
        value_row(state, "Peer", "None found", 0xb3c6d6);
    } else {
        for (const auto& peer : state.net_peers) {
            value_row(state, "Peer", peer, 0x20b383);
        }
    }
    auto* net_scan = action_button(state, "SCAN ETHERNET GLIDE");
    lv_obj_add_event_cb(
        net_scan,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            state->net_scanning = true;
            state->net_status = "Scanning";
            state->net_peers.clear();
            send_network_action(*state, "net scan");
            set_active_panel(*state, SidebarPanel::link);
        },
        LV_EVENT_CLICKED,
        &state);
    auto* p2p_ground = action_button(state, "P2P GROUND ETH0");
    lv_obj_add_event_cb(
        p2p_ground,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_network_action(*state, "net p2p ground eth0");
        },
        LV_EVENT_CLICKED,
        &state);
    auto* p2p_air = action_button(state, "P2P AIR ETH0");
    lv_obj_add_event_cb(
        p2p_air,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_network_action(*state, "net p2p air eth0");
        },
        LV_EVENT_CLICKED,
        &state);

    auto* radio_section = label(state.panel_body, "OpenHD Radio", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(radio_section, LV_PCT(100));
    auto* width = action_button(state, "CYCLE CHANNEL WIDTH");
    lv_obj_add_event_cb(
        width,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "air", "WB_CHANNEL_W", std::to_string(next_channel_width_value(state->mavlink.channel_width_mhz)));
        },
        LV_EVENT_CLICKED,
        &state);
    auto* mcs = action_button(state, "CYCLE AIR MCS");
    lv_obj_add_event_cb(
        mcs,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "air", "WB_MCS_INDEX", std::to_string(next_mcs_value(state->mavlink.mcs_index)));
        },
        LV_EVENT_CLICKED,
        &state);
    auto* power = action_button(state, "CYCLE TX POWER");
    lv_obj_add_event_cb(
        power,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "air", "TX_POWER_MW", std::to_string(next_tx_power_value(state->mavlink.tx_power_mw)));
        },
        LV_EVENT_CLICKED,
        &state);
    auto* pit_on = action_button(state, "PIT MODE ON");
    lv_obj_add_event_cb(
        pit_on,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "air", "WB_PIT_MODE", "1");
        },
        LV_EVENT_CLICKED,
        &state);
    auto* pit_off = action_button(state, "PIT MODE OFF");
    lv_obj_add_event_cb(
        pit_off,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "air", "WB_PIT_MODE", "0");
        },
        LV_EVENT_CLICKED,
        &state);
}

void video_slider_changed(lv_event_t* event)
{
    auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
    auto* target = lv_event_get_target_obj(event);
    if (state == nullptr || target == nullptr) {
        return;
    }
    if (target == state->display_hz_slider) {
        state->display_hz = static_cast<std::uint32_t>(lv_slider_get_value(target));
    } else if (target == state->flow_fps_slider) {
        state->flow_fps = static_cast<std::uint32_t>(lv_slider_get_value(target));
    } else if (target == state->flow_scale_slider) {
        state->flow_scale_percent = static_cast<std::uint32_t>(lv_slider_get_value(target));
    }
    sync_video_settings_controls(*state);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        persist_device_settings_and_restart(*state);
    }
}

void video_slider_row(
    UiState& state,
    const char* title,
    lv_obj_t*& slider,
    lv_obj_t*& value_label,
    std::int32_t min,
    std::int32_t max)
{
    const int row_index = state.row_count++;
    auto* row = lv_obj_create(state.panel_body);
    set_panel_style(row, state.focus_panel && state.selected_row == row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_border_width(row, state.focus_panel && state.selected_row == row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(row, color(0xff8a00), 0);
    lv_obj_set_size(row, LV_PCT(100), 68);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* title_label = label(row, title, &lv_font_montserrat_14, 0xdce8f0);
    lv_obj_set_width(title_label, LV_PCT(32));
    slider = lv_slider_create(row);
    lv_obj_set_width(slider, LV_PCT(36));
    lv_slider_set_range(slider, min, max);
    lv_obj_set_style_bg_color(slider, color(0x203040), 0);
    lv_obj_set_style_bg_color(slider, color(0xff8a00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, color(0xebf5ff), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, video_slider_changed, LV_EVENT_VALUE_CHANGED, &state);
    lv_obj_add_event_cb(slider, video_slider_changed, LV_EVENT_RELEASED, &state);

    value_label = label(row, "", &lv_font_montserrat_14, 0xffd447);
    lv_obj_set_width(value_label, LV_PCT(24));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
}

void build_video_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    auto* section = label(state.panel_body, "Video Mode", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(section, LV_PCT(100));

    const int resolution_row_index = state.row_count++;
    auto* resolution_row = lv_obj_create(state.panel_body);
    set_panel_style(resolution_row, state.focus_panel && state.selected_row == resolution_row_index ? 0x2d210e : 0x0f2130, LV_OPA_80);
    lv_obj_set_style_radius(resolution_row, 6, 0);
    lv_obj_set_style_border_width(resolution_row, state.focus_panel && state.selected_row == resolution_row_index ? 1 : 0, 0);
    lv_obj_set_style_border_color(resolution_row, color(0xff8a00), 0);
    lv_obj_set_size(resolution_row, LV_PCT(100), 62);
    lv_obj_set_style_pad_left(resolution_row, 16, 0);
    lv_obj_set_style_pad_right(resolution_row, 16, 0);
    lv_obj_set_flex_flow(resolution_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(resolution_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    state.resolution_label = label(resolution_row, "1920 x 1080", &lv_font_montserrat_16, 0xdce8f0);
    state.resolution_dropdown = lv_dropdown_create(resolution_row);
    lv_dropdown_set_options(state.resolution_dropdown, "1920 x 1080\n1600 x 900\n1280 x 720\n960 x 540");
    lv_obj_set_size(state.resolution_dropdown, 160, 38);
    lv_obj_set_style_bg_color(state.resolution_dropdown, color(0x162a3a), 0);
    lv_obj_set_style_text_color(state.resolution_dropdown, color(0xffffff), 0);
    lv_obj_add_event_cb(
        state.resolution_dropdown,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            set_resolution_preset(*state, lv_dropdown_get_selected(state->resolution_dropdown), true);
        },
        LV_EVENT_VALUE_CHANGED,
        &state);

    video_slider_row(state, "Display Hz", state.display_hz_slider, state.display_hz_label, 24, 120);
    video_slider_row(state, "Flow FPS", state.flow_fps_slider, state.flow_fps_label, 15, 120);
    video_slider_row(state, "Flow Scale", state.flow_scale_slider, state.flow_scale_label, 25, 100);

    sync_video_settings_controls(state);

    auto* current = label(state.panel_body, "Signal", &lv_font_montserrat_18, 0xffffff);
    lv_obj_set_width(current, LV_PCT(100));
    value_row(state, "Incoming", state.mavlink.resolution_fps);
    value_row(state, "Rotation", state.mavlink.rotation);
    value_row(state, "Codec", "H.264 RTP");
    value_row(state, "KMS Plane", "DMABUF scanout");
}

void build_camera_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    value_row(state, "Camera", state.mavlink.camera, state.mavlink.camera == "N/A" ? 0xdf4c7c : 0x20b383);
    value_row(state, "Resolution/FPS", state.mavlink.resolution_fps);
    value_row(state, "Rotation", state.mavlink.rotation);

    auto* resolution = action_button(state, "CYCLE AIR RESOLUTION/FPS");
    lv_obj_add_event_cb(
        resolution,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "RESOLUTION_FPS", next_resolution_fps_value(state->mavlink.resolution_fps));
        },
        LV_EVENT_CLICKED,
        &state);

    auto* rotation = action_button(state, "CYCLE CAMERA ROTATION");
    lv_obj_add_event_cb(
        rotation,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "ROTATION_DEG", std::to_string(next_rotation_value(state->mavlink.rotation)));
        },
        LV_EVENT_CLICKED,
        &state);

    auto* intra_off = action_button(state, "INTRA REFRESH OFF");
    lv_obj_add_event_cb(
        intra_off,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "INTRA_REFRESH", "0");
        },
        LV_EVENT_CLICKED,
        &state);

    auto* intra_cyclic = action_button(state, "INTRA REFRESH CYCLIC");
    lv_obj_add_event_cb(
        intra_cyclic,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "INTRA_REFRESH", "1");
        },
        LV_EVENT_CLICKED,
        &state);
}

void build_recording_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    value_row(state, "Recording", state.mavlink.recording);
    value_row(state, "Status", state.mavlink.recording_status);
    auto* off = action_button(state, "AIR RECORDING OFF");
    lv_obj_add_event_cb(
        off,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "AIR_RECORDING_E", "0");
        },
        LV_EVENT_CLICKED,
        &state);
    auto* on = action_button(state, "AIR RECORDING ON");
    lv_obj_add_event_cb(
        on,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "AIR_RECORDING_E", "1");
        },
        LV_EVENT_CLICKED,
        &state);
    auto* auto_arm = action_button(state, "RECORD ON ARMING");
    lv_obj_add_event_cb(
        auto_arm,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            send_openhd_param(*state, "camera1", "AIR_RECORDING_E", "2");
        },
        LV_EVENT_CLICKED,
        &state);
}

void build_rc_panel(UiState& state)
{
    setup_panel_column(state.panel_body);
    value_row(state, "FC", state.mavlink.fc_alive ? "Online" : "Waiting", state.mavlink.fc_alive ? 0x20b383 : 0xff8a00);
    value_row(state, "Mode", state.mavlink.flight_mode);
    value_row(state, "Armed", state.mavlink.armed ? "Yes" : "No", state.mavlink.armed ? 0x20b383 : 0xb3c6d6);
    if (state.mavlink.attitude_valid) {
        value_row(state, "Roll/Pitch", std::to_string(static_cast<int>(std::round(state.mavlink.roll_degrees))) + " / " + std::to_string(static_cast<int>(std::round(state.mavlink.pitch_degrees))) + " deg");
    }
    if (state.mavlink.altitude_valid) {
        value_row(state, "Altitude", std::to_string(static_cast<int>(std::round(state.mavlink.altitude_m))) + " m");
    }
    if (state.mavlink.speed_valid) {
        value_row(state, "Ground Speed", std::to_string(static_cast<int>(std::round(state.mavlink.ground_speed_mps * 3.6F))) + " km/h");
    }
    if (state.mavlink.battery_valid) {
        value_row(state, "Battery", std::to_string(state.mavlink.battery_percent) + "% / " + std::to_string(state.mavlink.voltage_v).substr(0, 4) + " V");
    }
    if (state.mavlink.position_valid) {
        value_row(state, "Position", std::to_string(state.mavlink.latitude_deg).substr(0, 9) + ", " + std::to_string(state.mavlink.longitude_deg).substr(0, 9));
    }
    value_row(state, "Satellites", std::to_string(state.mavlink.satellites));
    for (int i = 0; i < 4; ++i) {
        value_row(state, ("CH" + std::to_string(i + 1)).c_str(), std::to_string(state.mavlink.rc_channels[i]) + " us");
    }
}

void build_status_panel(UiState& state)
{
    setup_panel_column(state.panel_body, 12);
    const auto connection = state.mavlink.air_alive && state.mavlink.ground_alive
        ? std::string("Connected")
        : (state.mavlink.air_alive ? std::string("AIR only") : (state.mavlink.ground_alive ? std::string("GND only") : std::string("Not connected")));
    const auto connection_color = state.mavlink.air_alive && state.mavlink.ground_alive ? 0x20b383 : 0xdf4c7c;
    value_row(state, "Connection", connection, connection_color);
    value_row(state, "OpenHD Version", state.mavlink.openhd_version, state.mavlink.openhd_version == "N/A" ? 0xdf4c7c : 0x20b383);
    value_row(state, "Platform", state.mavlink.platform, state.mavlink.platform == "N/A" ? 0xdf4c7c : 0x20b383);
    value_row(state, "Chipset GND", state.mavlink.ground_chipset, state.mavlink.ground_alive ? 0x20b383 : 0xb3c6d6);
    value_row(state, "Chipset AIR", state.mavlink.air_chipset, state.mavlink.air_alive ? 0x20b383 : 0xdf4c7c);
    value_row(state, "Camera", state.mavlink.camera, state.mavlink.air_alive ? 0x20b383 : 0xdf4c7c);

    auto* advanced = action_button(state, state.advanced_visible ? "HIDE MAVLINK MESSAGES" : "SHOW MAVLINK MESSAGES");
    lv_obj_add_event_cb(
        advanced,
        [](lv_event_t* event) {
            auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
            state->advanced_visible = !state->advanced_visible;
            set_active_panel(*state, SidebarPanel::developer);
        },
        LV_EVENT_CLICKED,
        &state);

    if (state.advanced_visible) {
        for (std::size_t i = 0; i < state.mavlink.message_count; ++i) {
            value_row(state, "MAVLink", state.mavlink.messages[i], 0xd7e3ec);
        }
    }
}

void build_placeholder_panel(UiState& state)
{
    lv_obj_set_style_pad_all(state.panel_body, 42, 0);
    auto* text = label(state.panel_body, "No controls available.", &lv_font_montserrat_24, 0xb3c6d6);
    lv_obj_align(text, LV_ALIGN_TOP_LEFT, 0, 0);
}

void clear_panel(UiState& state)
{
    lv_obj_clean(state.panel_body);
    state.fps_switch = nullptr;
    state.fps_label = nullptr;
    state.coordinates_switch = nullptr;
    state.coordinates_label = nullptr;
    state.compact_switch = nullptr;
    state.compact_label = nullptr;
    state.top_bar_switch = nullptr;
    state.top_bar_label = nullptr;
    state.osd_dropdown = nullptr;
    state.osd_label = nullptr;
    state.theme_dropdowns.fill(nullptr);
    state.theme_labels.fill(nullptr);
    state.resolution_dropdown = nullptr;
    state.resolution_label = nullptr;
    state.display_hz_slider = nullptr;
    state.display_hz_label = nullptr;
    state.flow_fps_slider = nullptr;
    state.flow_fps_label = nullptr;
    state.flow_scale_slider = nullptr;
    state.flow_scale_label = nullptr;
    state.scan_bar = nullptr;
    state.scan_percent = nullptr;
    state.row_count = 0;
    state.selected_row = std::max(0, state.selected_row);
}

void set_active_panel(UiState& state, SidebarPanel panel)
{
    state.active_panel = panel;
    state.panel_rebuild_pending = false;
    state.selected_row = 0;
    state.focus_panel = false;
    if (state.panel_title == nullptr || state.panel_body == nullptr) {
        rebuild_ui(state);
        return;
    }
    lv_label_set_text(state.panel_title, panel_title(panel));
    clear_panel(state);

    for (int i = 0; i < 10; ++i) {
        if (state.nav_buttons[i] == nullptr) {
            continue;
        }
        if (static_cast<int>(panel) == i) {
            lv_obj_add_state(state.nav_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(state.nav_buttons[i], LV_STATE_CHECKED);
        }
    }

    if (panel == SidebarPanel::dashboard) {
        build_dashboard_panel(state);
    } else if (panel == SidebarPanel::link) {
        build_link_panel(state);
    } else if (panel == SidebarPanel::video) {
        build_video_panel(state);
    } else if (panel == SidebarPanel::camera) {
        build_camera_panel(state);
    } else if (panel == SidebarPanel::osd) {
        build_osd_panel(state);
    } else if (panel == SidebarPanel::telemetry) {
        build_rc_panel(state);
    } else if (panel == SidebarPanel::recording) {
        build_recording_panel(state);
    } else if (panel == SidebarPanel::system) {
        build_system_panel(state);
    } else if (panel == SidebarPanel::developer) {
        build_status_panel(state);
    } else if (panel == SidebarPanel::colors) {
        build_colors_panel(state);
    } else {
        build_placeholder_panel(state);
    }
}

void nav_clicked(lv_event_t* event)
{
    auto* state = static_cast<UiState*>(lv_event_get_user_data(event));
    auto index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    set_active_panel(*state, static_cast<SidebarPanel>(index));
}

void send_initial_ipc_requests(UiState& state, const char* backend_name)
{
    state.ipc.send_line("hello glide-ui");
    state.ipc.send_line(std::string("status glide-ui ready ") + backend_name);
    state.ipc.send_line("get fps");
    state.ipc.send_line("get coords");
    state.ipc.send_line("get compact");
    state.ipc.send_line("get topbar");
    state.ipc.send_line("get osd");
    state.ipc.send_line("get theme");
    state.ipc.send_line("get net");
}

bool apply_network_state_line(UiState& state, const std::string& line)
{
    if (line == "state net idle") {
        state.net_scanning = false;
        state.net_status = "Idle";
        return true;
    }
    if (line == "state net scanning") {
        state.net_scanning = true;
        state.net_status = "Scanning";
        state.net_peers.clear();
        return true;
    }
    if (line.rfind("state net peer ", 0) == 0) {
        std::istringstream stream(line);
        std::string state_word;
        std::string net_word;
        std::string peer_word;
        std::string address;
        std::string hostname;
        std::string port;
        stream >> state_word >> net_word >> peer_word >> address >> hostname >> port;
        if (!address.empty()) {
            const auto text = hostname.empty() ? address : hostname + " " + address + ":" + port;
            if (std::find(state.net_peers.begin(), state.net_peers.end(), text) == state.net_peers.end()) {
                state.net_peers.push_back(text);
            }
            state.net_status = "Peer found";
        }
        return true;
    }
    if (line.rfind("state net done ", 0) == 0) {
        state.net_scanning = false;
        state.net_status = state.net_peers.empty() ? "No peers" : "Ready";
        return true;
    }
    if (line.rfind("state net p2p ", 0) == 0) {
        state.net_status = line.substr(std::string("state net p2p ").size());
        return true;
    }
    return false;
}

bool connect_ipc_if_needed(UiState& state, std::chrono::steady_clock::time_point now, const char* backend_name)
{
    if (state.ipc.connected()) {
        return true;
    }
    if (now < state.next_ipc_reconnect) {
        return false;
    }
    state.next_ipc_reconnect = now + std::chrono::milliseconds(500);
    if (!state.ipc.connect_to(state.ipc_socket)) {
        return false;
    }
    send_initial_ipc_requests(state, backend_name);
    glide::log(glide::LogLevel::info, "GlideUI", "IPC controller connected");
    return true;
}

void animate_slide_from_left(lv_obj_t* object, int hidden_x)
{
    const auto target_x = lv_obj_get_x(object);
    lv_obj_set_x(object, hidden_x);

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, [](void* target, int32_t value) {
        lv_obj_set_x(static_cast<lv_obj_t*>(target), value);
    });
    lv_anim_set_values(&animation, hidden_x, target_x);
    lv_anim_set_time(&animation, 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void build_sidebar(UiState& state, std::uint32_t width, std::uint32_t height)
{
    auto* screen = lv_screen_active();
    state.root = screen;
    set_panel_style(screen, 0x000000, LV_OPA_TRANSP);

    const int safe_width = static_cast<int>(std::max<std::uint32_t>(width, 220));
    const int safe_height = static_cast<int>(std::max<std::uint32_t>(height, 360));
    const bool compact = safe_width < 520;
    const int menu_width = std::clamp(safe_width / 3, 210, compact ? 260 : 250);
    const int menu_height = std::min(safe_height - 28, 540);
    const int detail_width = compact ? 0 : std::max(260, safe_width - menu_width - 42);
    const int detail_height = std::min(menu_height, safe_height - 28);

    auto* rail = lv_obj_create(screen);
    set_panel_style(rail, 0x020d15, LV_OPA_80);
    lv_obj_set_style_radius(rail, 8, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, color(0x33546a), 0);
    lv_obj_set_size(rail, menu_width, menu_height);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(rail, 16, 0);
    lv_obj_set_style_pad_bottom(rail, 8, 0);
    lv_obj_set_style_pad_left(rail, 0, 0);
    lv_obj_set_style_pad_right(rail, 0, 0);
    lv_obj_set_style_pad_row(rail, 2, 0);

    auto* brand = label(rail, "OPENHD", &lv_font_montserrat_20, 0xffffff);
    lv_obj_set_width(brand, LV_PCT(100));
    lv_obj_set_style_pad_left(brand, 26, 0);
    lv_obj_set_height(brand, 32);

    auto* accent = lv_obj_create(rail);
    set_panel_style(accent, 0xff8a00);
    lv_obj_set_size(accent, 70, 1);
    lv_obj_set_style_margin_left(accent, 38, 0);
    lv_obj_set_style_margin_bottom(accent, 8, 0);

    for (int i = 0; i < 10; ++i) {
        auto* button = lv_button_create(rail);
        state.nav_buttons[i] = button;
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<std::intptr_t>(i)));
        lv_obj_set_size(button, LV_PCT(100), 36);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_margin_left(button, 6, 0);
        lv_obj_set_style_margin_right(button, 10, 0);
        const bool checked = static_cast<int>(state.active_panel) == i && !state.focus_panel;
        lv_obj_set_style_bg_color(button, color(checked ? 0x102131 : 0x020d15), 0);
        lv_obj_set_style_bg_opa(button, checked ? LV_OPA_80 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(button, color(0x172a3a), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(button, color(0x233746), LV_STATE_PRESSED);
        lv_obj_add_event_cb(button, nav_clicked, LV_EVENT_CLICKED, &state);

        if (checked) {
            auto* active_bar = lv_obj_create(button);
            set_panel_style(active_bar, 0xff8a00);
            lv_obj_set_size(active_bar, 4, LV_PCT(100));
            lv_obj_align(active_bar, LV_ALIGN_LEFT_MID, -6, 0);
        }

        auto* row = lv_obj_create(button);
        set_panel_style(row, 0x000000, LV_OPA_TRANSP);
        lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(row, 28, 0);
        lv_obj_set_style_pad_column(row, 12, 0);

        auto* icon = label(row, nav_symbol(i), &lv_font_montserrat_18, checked ? 0xff8a00 : 0xdce5ec);
        lv_obj_set_width(icon, 24);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
        auto* text = label(row, nav_text(i), &lv_font_montserrat_14, checked ? 0xff9a1f : 0xd7dde3);
        lv_obj_set_width(text, LV_PCT(70));
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
    }

    auto* footer = lv_obj_create(rail);
    set_panel_style(footer, 0x020d15, LV_OPA_TRANSP);
    lv_obj_set_size(footer, LV_PCT(100), 28);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(footer, 26, 0);
    lv_obj_set_style_pad_right(footer, 20, 0);
    auto* back = label(footer, LV_SYMBOL_LEFT " BACK", &lv_font_montserrat_12, 0xb4c0cb);
    lv_obj_set_width(back, LV_PCT(45));
    auto* select = label(footer, "SELECT " LV_SYMBOL_OK, &lv_font_montserrat_12, 0xb4c0cb);
    lv_obj_set_width(select, LV_PCT(45));
    lv_obj_set_style_text_align(select, LV_TEXT_ALIGN_RIGHT, 0);

    if (compact) {
        state.panel_title = nullptr;
        state.panel_body = nullptr;
        if (state.animate_menu_in) {
            lv_obj_update_layout(screen);
            animate_slide_from_left(rail, -menu_width - 24);
            state.animate_menu_in = false;
        }
        return;
    }

    auto* panel = lv_obj_create(screen);
    set_panel_style(panel, 0x07131d, LV_OPA_70);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, color(state.focus_panel ? 0xff8a00 : 0x284154), 0);
    lv_obj_set_size(panel, detail_width, detail_height);
    lv_obj_align_to(panel, rail, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    auto* header = lv_obj_create(panel);
    set_panel_style(header, 0x07131d, LV_OPA_TRANSP);
    lv_obj_set_size(header, LV_PCT(100), 46);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    state.panel_title = label(header, "Dashboard", &lv_font_montserrat_20);
    lv_obj_align(state.panel_title, LV_ALIGN_LEFT_MID, 16, 0);

    state.panel_body = lv_obj_create(panel);
    set_panel_style(state.panel_body, 0x07131d, LV_OPA_TRANSP);
    lv_obj_set_size(state.panel_body, LV_PCT(100), detail_height - 58);
    lv_obj_align(state.panel_body, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    const auto active = state.active_panel;
    lv_label_set_text(state.panel_title, panel_title(active));
    clear_panel(state);
    if (active == SidebarPanel::dashboard) {
        build_dashboard_panel(state);
    } else if (active == SidebarPanel::link) {
        build_link_panel(state);
    } else if (active == SidebarPanel::video) {
        build_video_panel(state);
    } else if (active == SidebarPanel::camera) {
        build_camera_panel(state);
    } else if (active == SidebarPanel::osd) {
        build_osd_panel(state);
    } else if (active == SidebarPanel::telemetry) {
        build_rc_panel(state);
    } else if (active == SidebarPanel::recording) {
        build_recording_panel(state);
    } else if (active == SidebarPanel::system) {
        build_system_panel(state);
    } else if (active == SidebarPanel::developer) {
        build_status_panel(state);
    } else if (active == SidebarPanel::colors) {
        build_colors_panel(state);
    }

    if (state.animate_menu_in) {
        lv_obj_update_layout(screen);
        animate_slide_from_left(rail, -menu_width - 24);
        animate_slide_from_left(panel, -detail_width - menu_width - 48);
        state.animate_menu_in = false;
    }
}

void build_minimap_card(UiState& state, std::uint32_t width, std::uint32_t height)
{
    auto* screen = lv_screen_active();
    state.root = screen;
    set_panel_style(screen, 0x000000, LV_OPA_TRANSP);

    const int safe_width = static_cast<int>(std::max<std::uint32_t>(width, 220));
    const int safe_height = static_cast<int>(std::max<std::uint32_t>(height, 220));
    const int diameter = std::clamp(std::min(safe_width, safe_height) - 32, 180, 360);

    auto* shadow = lv_obj_create(screen);
    set_panel_style(shadow, 0x000000, LV_OPA_50);
    lv_obj_set_style_radius(shadow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(shadow, diameter + 18, diameter + 18);
    lv_obj_align(shadow, LV_ALIGN_BOTTOM_LEFT, 16, -16);

    auto* frame = lv_obj_create(screen);
    set_panel_style(frame, 0x050808, LV_OPA_COVER);
    lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, color(0xe9d8b5), 0);
    lv_obj_set_style_pad_all(frame, 8, 0);
    lv_obj_set_size(frame, diameter, diameter);
    lv_obj_align(frame, LV_ALIGN_BOTTOM_LEFT, 20, -20);

    glide::ui::MinimapOptions options;
    options.tile_root = std::getenv("GLIDE_MINIMAP_TILE_ROOT") != nullptr ? std::getenv("GLIDE_MINIMAP_TILE_ROOT") : "assets/maps";
    options.zoom = std::getenv("GLIDE_MINIMAP_ZOOM") != nullptr ? std::stoi(std::getenv("GLIDE_MINIMAP_ZOOM")) : 15;
    options.grid_tiles = std::getenv("GLIDE_MINIMAP_GRID") != nullptr ? std::stoi(std::getenv("GLIDE_MINIMAP_GRID")) : 5;
    options.round = true;
    state.minimap = std::make_unique<glide::ui::MinimapWidget>(frame, diameter - 16, diameter - 16, options);
    lv_obj_center(state.minimap->object());
    state.minimap_started = std::chrono::steady_clock::now();
    state.minimap_last_render = {};
}

void build_overlay(UiState& state, std::uint32_t width, std::uint32_t height)
{
    state.minimap.reset();
    if (state.overlay_mode == OverlayMode::menu) {
        build_sidebar(state, width, height);
        return;
    }
    auto* screen = lv_screen_active();
    state.root = screen;
    set_panel_style(screen, 0x000000, LV_OPA_TRANSP);
    if (state.overlay_mode == OverlayMode::minimap) {
        build_minimap_card(state, width, height);
    } else {
        clear_active_buffer_display();
    }
}

void update_minimap(UiState& state, std::chrono::steady_clock::time_point now)
{
    if (state.overlay_mode != OverlayMode::minimap || !state.minimap) {
        return;
    }
    if (state.minimap_last_render.time_since_epoch().count() != 0 && now - state.minimap_last_render < std::chrono::milliseconds(100)) {
        return;
    }
    constexpr double home_lat = 38.8976763;
    constexpr double home_lon = -77.0365298;
    const double seconds = std::chrono::duration<double>(now - state.minimap_started).count();
    const double progress = std::min(1.0, seconds / 240.0);
    const double eased = progress * progress * (3.0 - 2.0 * progress);
    constexpr double city_lat = 38.8951100;
    constexpr double city_lon = -77.0219570;
    state.minimap->set_home(home_lat, home_lon);
    state.minimap->set_position(glide::ui::MinimapPosition {
        .latitude_deg = home_lat + (city_lat - home_lat) * eased,
        .longitude_deg = home_lon + (city_lon - home_lon) * eased,
        .heading_deg = 102.0F,
    });
    state.minimap->render();
    state.minimap_last_render = now;
}

void poll_local_key_file(UiState& state)
{
    const auto path = std::filesystem::temp_directory_path() / "openhd-glide-ui.key";
    std::ifstream file(path);
    if (!file) {
        return;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    file.close();
    std::ofstream clear(path, std::ios::trunc);
    clear.close();

    for (const auto& key_line : lines) {
        if (key_line.rfind("ui key ", 0) == 0) {
            apply_terminal_key(state, key_line);
        } else {
            apply_terminal_key(state, "ui key " + key_line);
        }
    }
}

void poll_ipc(UiState& state, std::chrono::steady_clock::time_point now)
{
    if (!connect_ipc_if_needed(state, now, "lvgl-buffer")) {
        if (now < state.next_control_file_poll) {
            return;
        }
        state.next_control_file_poll = now + std::chrono::milliseconds(250);

        if (update_bool(state.fps_enabled, glide::preview_control::fps_overlay_enabled())) {
            sync_fps_controls(state);
        }
        if (update_bool(state.coordinates_enabled, glide::preview_control::coordinates_overlay_enabled())) {
            sync_coordinates_controls(state);
        }
        if (update_bool(state.compact_readouts, glide::preview_control::compact_readouts_enabled())) {
            sync_compact_readouts_controls(state);
        }
        if (update_bool(state.top_bar_enabled, glide::preview_control::top_bar_enabled())) {
            sync_top_bar_controls(state);
        }
        if (update_string(state.osd_layout, glide::preview_control::osd_layout())) {
            sync_osd_layout_controls(state);
        }
        for (std::size_t i = 0; i < theme_keys.size(); ++i) {
            theme_color_ref(state, i) = glide::preview_control::theme_color(theme_keys[i]);
        }
        sync_theme_controls(state);
        poll_local_key_file(state);
        return;
    }

    for (const auto& line : state.ipc.poll_lines()) {
        if (line == "state fps 0" || line == "state fps 1") {
            if (update_bool(state.fps_enabled, line.back() == '1')) {
                glide::preview_control::set_fps_overlay_enabled(state.fps_enabled);
                sync_fps_controls(state);
            }
        } else if (line == "state coords 0" || line == "state coords 1") {
            if (update_bool(state.coordinates_enabled, line.back() == '1')) {
                glide::preview_control::set_coordinates_overlay_enabled(state.coordinates_enabled);
                sync_coordinates_controls(state);
            }
        } else if (line == "state compact 0" || line == "state compact 1") {
            if (update_bool(state.compact_readouts, line.back() == '1')) {
                glide::preview_control::set_compact_readouts_enabled(state.compact_readouts);
                sync_compact_readouts_controls(state);
            }
        } else if (line == "state topbar 0" || line == "state topbar 1") {
            if (update_bool(state.top_bar_enabled, line.back() == '1')) {
                glide::preview_control::set_top_bar_enabled(state.top_bar_enabled);
                sync_top_bar_controls(state);
            }
        } else if (line == "state osd drone" || line == "state osd rocket" || line == "state osd rover" || line == "state osd ship") {
            if (update_string(state.osd_layout, line.substr(10))) {
                glide::preview_control::set_osd_layout(state.osd_layout);
                sync_osd_layout_controls(state);
            }
        } else if (apply_theme_state_line(state, line)) {
        } else if (apply_network_state_line(state, line)) {
            if (panel_uses_network(state.active_panel)) {
                request_panel_rebuild(state, now);
            }
        } else if (glide::mavlink::apply_ipc_line(state.mavlink, line)) {
            if (panel_uses_mavlink(state.active_panel)) {
                request_panel_rebuild(state, now);
            }
        } else {
            apply_terminal_key(state, line);
        }
    }

    if (state.panel_rebuild_pending && now >= state.next_panel_rebuild) {
        set_active_panel(state, state.active_panel);
    }
}

void set_sdl_position(const Options& options)
{
    if (!options.positioned) {
        return;
    }
    const auto position = std::to_string(options.x) + "," + std::to_string(options.y);
    setenv("SDL_VIDEO_WINDOW_POS", position.c_str(), 1);
}

} // namespace

int main(int argc, char** argv)
{
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    const auto options = parse_options(argc, argv);
    if (options.headless) {
        return run_headless_ui(HeadlessOptions {
            .ipc_socket = options.ipc_socket,
        });
    }

    glide::log(glide::LogLevel::info, "GlideUI", "LVGL UI started");

    if (!options.preview && !options.buffer) {
        return 0;
    }

    set_sdl_position(options);
    lv_init();
    BufferDisplay buffer_display;
    LinuxKeyboardInput keyboard_input;
#if OPENHD_GLIDE_HAS_LVGL_SDL
    if (options.preview) {
        auto* display = lv_sdl_window_create(static_cast<int32_t>(options.width), static_cast<int32_t>(options.height));
        lv_sdl_window_set_title(display, "GlideUI LVGL Preview");
        lv_sdl_window_set_resizeable(display, false);
        lv_sdl_mouse_create();
        auto* keyboard = lv_sdl_keyboard_create();
        auto* group = lv_group_create();
        lv_group_set_default(group);
        lv_indev_set_group(keyboard, group);
    }
#else
    if (options.preview) {
        glide::log(glide::LogLevel::error, "GlideUI", "LVGL SDL preview requires SDL2 development files; use --buffer on device builds");
        return 1;
    }
#endif
    if (options.buffer) {
        if (!create_buffer_display(buffer_display, options)) {
            return 1;
        }
        active_buffer_display = &buffer_display;
        auto* display = lv_display_create(static_cast<int32_t>(options.width), static_cast<int32_t>(options.height));
        lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);
        lv_display_set_user_data(display, &buffer_display);
        lv_display_set_flush_cb(display, buffer_flush);
        lv_display_set_buffers(
            display,
            buffer_display.draw_buffer.data(),
            nullptr,
            buffer_display.draw_buffer.size() * sizeof(std::uint32_t),
            LV_DISPLAY_RENDER_MODE_PARTIAL);
        glide::log(glide::LogLevel::info, "GlideUI", "LVGL buffer backend writing " + options.buffer_path);
    }

    UiState state;
    state.ipc_socket = options.ipc_socket;
    state.fps_enabled = glide::preview_control::fps_overlay_enabled();
    state.coordinates_enabled = glide::preview_control::coordinates_overlay_enabled();
    state.compact_readouts = glide::preview_control::compact_readouts_enabled();
    state.top_bar_enabled = glide::preview_control::top_bar_enabled();
    state.osd_layout = glide::preview_control::osd_layout();
    load_device_settings(state);
    for (std::size_t i = 0; i < theme_keys.size(); ++i) {
        theme_color_ref(state, i) = glide::preview_control::theme_color(theme_keys[i]);
    }
    auto* screen = lv_screen_active();
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(screen, keyboard_event, LV_EVENT_KEY, &state);
    if (lv_group_get_default() != nullptr) {
        lv_group_add_obj(lv_group_get_default(), screen);
        lv_group_focus_obj(screen);
    }

    if (state.ipc.connect_to(state.ipc_socket)) {
        send_initial_ipc_requests(state, options.buffer ? "lvgl-buffer" : "lvgl-sdl");
    } else {
        glide::log(glide::LogLevel::warning, "GlideUI", "IPC controller unavailable; retrying in background");
    }

    build_overlay(state, options.width, options.height);

    auto last_tick = std::chrono::steady_clock::now();
    constexpr auto ui_frame_interval = std::chrono::milliseconds(16);
    while (stop_requested == 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
        if (elapsed.count() > 0) {
            lv_tick_inc(static_cast<uint32_t>(elapsed.count()));
            last_tick = now;
        }

        if (options.buffer) {
            poll_linux_keyboard(keyboard_input, state, now);
        }
        poll_ipc(state, now);
        if (options.buffer && state.overlay_mode == OverlayMode::hidden) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        update_minimap(state, now);
        lv_timer_handler();
        std::this_thread::sleep_for(ui_frame_interval);
    }

#if OPENHD_GLIDE_HAS_LVGL_SDL
    if (options.preview) {
        lv_sdl_quit();
    }
#endif
    active_buffer_display = nullptr;
    close_buffer_display(buffer_display);
    return 0;
}
#else
int main(int argc, char** argv)
{
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    bool headless {};
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--headless" || argument == "--device") {
            headless = true;
        }
    }
    if (headless) {
        return run_headless_ui(parse_headless_options(argc, argv));
    }

    glide::log(glide::LogLevel::error, "GlideUI", "LVGL SDL preview requires SDL2 development files; use --headless for device IPC tests");
    return 1;
}
#endif
