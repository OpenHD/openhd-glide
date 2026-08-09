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

#include "lvgl.h"

#include <cstdint>
#include <string>

namespace glide::ui {

struct MinimapPosition {
    double latitude_deg {};
    double longitude_deg {};
    float heading_deg {};
};

struct MinimapOptions {
    std::string tile_root { "assets/maps" };
    int zoom { 15 };
    int tile_size { 256 };
    int grid_tiles { 5 };
    std::uint32_t background_rgb { 0x090e0f };
    bool round { true };
    bool render_on_interaction { true };
};

class MinimapWidget {
public:
    MinimapWidget(lv_obj_t* parent, int width, int height, MinimapOptions options);
    ~MinimapWidget();

    MinimapWidget(const MinimapWidget&) = delete;
    MinimapWidget& operator=(const MinimapWidget&) = delete;

    lv_obj_t* object() const { return canvas_; }
    void set_home(double latitude_deg, double longitude_deg);
    void set_position(const MinimapPosition& position);
    void set_zoom(double zoom);
    double zoom() const;
    void pan_by_pixels(int delta_x, int delta_y);
    void recenter();
    double pan_x() const;
    double pan_y() const;
    void render();
    double home_distance_m() const;
    double meters_per_pixel() const;

private:
    struct Impl;
    Impl* impl_ {};
    lv_obj_t* canvas_ {};
};

} // namespace glide::ui
