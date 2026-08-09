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

#include "glide_ui/minimap_widget.hpp"

#include "common/logging.hpp"

#include "lvgl.h"
#if OPENHD_GLIDE_HAS_LVGL_SDL
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
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

struct Options {
    std::uint32_t width { 360 };
    std::uint32_t height { 360 };
    int x {};
    int y {};
    bool positioned {};
    bool preview {};
    bool buffer {};
    std::string buffer_path { "/tmp/openhd-glide-minimap.argb" };
    std::string tile_root { "assets/maps" };
    int zoom { 15 };
    int grid_tiles { 5 };
    double latitude { 38.8976763 };
    double longitude { -77.0365298 };
};

struct DemoState {
    glide::ui::MinimapWidget* minimap {};
};

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--preview") {
            options.preview = true;
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
        } else if (argument == "--tile-root" && i + 1 < argc) {
            options.tile_root = argv[++i];
        } else if (argument == "--buffer") {
            options.buffer = true;
        } else if (argument == "--buffer-path" && i + 1 < argc) {
            options.buffer_path = argv[++i];
        } else if (argument == "--zoom" && i + 1 < argc) {
            options.zoom = std::stoi(argv[++i]);
        } else if (argument == "--grid" && i + 1 < argc) {
            options.grid_tiles = std::stoi(argv[++i]);
        } else if (argument == "--lat" && i + 1 < argc) {
            options.latitude = std::stod(argv[++i]);
        } else if (argument == "--lon" && i + 1 < argc) {
            options.longitude = std::stod(argv[++i]);
        }
    }
    return options;
}

struct BufferDisplay {
    int fd { -1 };
    std::uint32_t width {};
    std::uint32_t height {};
    std::size_t size {};
    void* map {};
    std::vector<std::uint32_t> draw_buffer;
};

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

bool create_buffer_display(BufferDisplay& target, const Options& options)
{
#if defined(__linux__)
    close_buffer_display(target);
    target.width = options.width;
    target.height = options.height;
    target.size = static_cast<std::size_t>(target.width) * target.height * sizeof(std::uint32_t);
    target.fd = open(options.buffer_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (target.fd < 0) {
        glide::log(glide::LogLevel::error, "GlideMinimapDemo", "failed to open minimap buffer " + options.buffer_path);
        return false;
    }
    if (ftruncate(target.fd, static_cast<off_t>(target.size)) != 0) {
        glide::log(glide::LogLevel::error, "GlideMinimapDemo", "failed to resize minimap buffer " + options.buffer_path);
        close_buffer_display(target);
        return false;
    }
    target.map = mmap(nullptr, target.size, PROT_READ | PROT_WRITE, MAP_SHARED, target.fd, 0);
    if (target.map == MAP_FAILED) {
        target.map = nullptr;
        glide::log(glide::LogLevel::error, "GlideMinimapDemo", "failed to mmap minimap buffer " + options.buffer_path);
        close_buffer_display(target);
        return false;
    }
    std::fill_n(static_cast<std::uint32_t*>(target.map), target.width * target.height, 0xff000000U);
    target.draw_buffer.resize(static_cast<std::size_t>(target.width) * target.height);
    return true;
#else
    (void)target;
    (void)options;
    glide::log(glide::LogLevel::error, "GlideMinimapDemo", "buffer display requires Linux mmap support");
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
            std::memcpy(
                destination + static_cast<std::size_t>(y) * target->width + static_cast<std::size_t>(x1),
                source + source_y * area_width + source_x,
                copy_width * sizeof(std::uint32_t));
        }
#if defined(__linux__)
        msync(target->map, target->size, MS_ASYNC);
#endif
    }
    lv_display_flush_ready(display);
}

void set_sdl_position(const Options& options)
{
    if (!options.positioned) {
        return;
    }
    const auto position = std::to_string(options.x) + "," + std::to_string(options.y);
#if defined(_WIN32)
    _putenv_s("SDL_VIDEO_WINDOW_POS", position.c_str());
#else
    setenv("SDL_VIDEO_WINDOW_POS", position.c_str(), 1);
#endif
}

lv_color_t color(std::uint32_t rgb)
{
    return lv_color_hex(rgb);
}

void style_screen(lv_obj_t* screen)
{
    lv_obj_set_style_bg_color(screen, color(0x050b10), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
}

void keyboard_event(lv_event_t* event)
{
    if (lv_event_get_code(event) != LV_EVENT_KEY) {
        return;
    }
    auto* state = static_cast<DemoState*>(lv_event_get_user_data(event));
    if (state == nullptr || state->minimap == nullptr) {
        return;
    }
    const auto key = lv_event_get_key(event);
    if (key == '+' || key == '=') {
        state->minimap->set_zoom(state->minimap->zoom() + 0.25);
        state->minimap->render();
    } else if (key == '-' || key == '_') {
        state->minimap->set_zoom(state->minimap->zoom() - 0.25);
        state->minimap->render();
    }
}

} // namespace

int main(int argc, char** argv)
{
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    const auto options = parse_options(argc, argv);
    if (!options.preview && !options.buffer) {
        glide::log(glide::LogLevel::info, "GlideMinimapDemo", "nothing to do without --preview or --buffer");
        return 0;
    }

    set_sdl_position(options);
    lv_init();
    BufferDisplay buffer_display;
#if OPENHD_GLIDE_HAS_LVGL_SDL
    if (options.preview) {
    auto* display = lv_sdl_window_create(static_cast<int32_t>(options.width), static_cast<int32_t>(options.height));
    lv_sdl_window_set_title(display, "OpenHD Glide LVGL Minimap");
    lv_sdl_window_set_resizeable(display, false);
    lv_sdl_mouse_create();
    auto* keyboard = lv_sdl_keyboard_create();
    auto* group = lv_group_create();
    lv_group_set_default(group);
    lv_indev_set_group(keyboard, group);
    }
#else
    if (options.preview) {
        glide::log(glide::LogLevel::error, "GlideMinimapDemo", "LVGL SDL preview requires SDL2 development files; use --buffer on device builds");
        return 1;
    }
#endif
    if (options.buffer) {
        if (!create_buffer_display(buffer_display, options)) {
            return 1;
        }
        auto* display = lv_display_create(static_cast<int32_t>(options.width), static_cast<int32_t>(options.height));
        lv_display_set_user_data(display, &buffer_display);
        lv_display_set_flush_cb(display, buffer_flush);
        lv_display_set_buffers(
            display,
            buffer_display.draw_buffer.data(),
            nullptr,
            buffer_display.draw_buffer.size() * sizeof(std::uint32_t),
            LV_DISPLAY_RENDER_MODE_PARTIAL);
        glide::log(glide::LogLevel::info, "GlideMinimapDemo", "LVGL buffer backend writing " + options.buffer_path);
    }

    auto* screen = lv_screen_active();
    style_screen(screen);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    const int size = static_cast<int>(std::min(options.width, options.height));
    glide::ui::MinimapOptions minimap_options;
    minimap_options.tile_root = options.tile_root;
    minimap_options.zoom = options.zoom;
    minimap_options.grid_tiles = options.grid_tiles;
    glide::ui::MinimapWidget minimap(screen, size, size, minimap_options);
    DemoState demo_state { .minimap = &minimap };
    lv_obj_add_event_cb(screen, keyboard_event, LV_EVENT_KEY, &demo_state);
    if (lv_group_get_default() != nullptr) {
        lv_group_add_obj(lv_group_get_default(), screen);
        lv_group_focus_obj(screen);
    }
    lv_obj_center(minimap.object());

    const double home_lat = options.latitude;
    const double home_lon = options.longitude;
    minimap.set_home(home_lat, home_lon);
    minimap.set_position(glide::ui::MinimapPosition {
        .latitude_deg = home_lat,
        .longitude_deg = home_lon,
        .heading_deg = 0.0F,
    });
    minimap.render();

    auto started = std::chrono::steady_clock::now();
    auto last_tick = started;
    auto last_render = started;
    while (stop_requested == 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick);
        if (elapsed.count() > 0) {
            lv_tick_inc(static_cast<uint32_t>(elapsed.count()));
            last_tick = now;
        }

        if (now - last_render >= std::chrono::milliseconds(100)) {
            const double seconds = std::chrono::duration<double>(now - started).count();
            const double progress = std::min(1.0, seconds / 240.0);
            const double eased = progress * progress * (3.0 - 2.0 * progress);
            const double city_lat = home_lat + 0.0025;
            const double city_lon = home_lon + 0.0145;
            glide::ui::MinimapPosition position {
                .latitude_deg = home_lat + (city_lat - home_lat) * eased,
                .longitude_deg = home_lon + (city_lon - home_lon) * eased,
                .heading_deg = 102.0F,
            };
            minimap.set_position(position);
            minimap.render();
            last_render = now;
        }

        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

#if OPENHD_GLIDE_HAS_LVGL_SDL
    if (options.preview) {
    lv_sdl_quit();
    }
#endif
    close_buffer_display(buffer_display);
    return 0;
}
