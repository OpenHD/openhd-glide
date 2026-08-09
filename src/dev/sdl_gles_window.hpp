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

#include "glide_flow/fps_overlay.hpp"

#include <cstdint>
#include <string>

struct SDL_Window;

namespace glide::dev {

struct WindowPlacement {
    std::uint32_t width {};
    std::uint32_t height {};
    int x {};
    int y {};
    float opacity { 1.0F };
    bool positioned {};
    bool borderless {};
    bool always_on_top {};
};

class SdlGlesWindow {
public:
    SdlGlesWindow() = default;
    ~SdlGlesWindow();

    SdlGlesWindow(const SdlGlesWindow&) = delete;
    SdlGlesWindow& operator=(const SdlGlesWindow&) = delete;

    bool create(const char* title, std::uint32_t width, std::uint32_t height);
    bool create(const char* title, WindowPlacement placement);
    bool poll();
    bool consume_click(int& x, int& y);
    bool consume_key(std::string& key);
    void swap();
    flow::SurfaceSize surface_size() const;
    const std::string& last_error() const;

private:
    SDL_Window* window_ {};
    void* context_ {};
    flow::SurfaceSize surface_ {};
    int click_x_ {};
    int click_y_ {};
    bool has_click_ {};
    std::string key_;
    std::string last_error_;
};

bool sdl_gles_available();

} // namespace glide::dev
