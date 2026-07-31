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

#include "glide_flow/gles_text_renderer.hpp"

namespace glide::flow {

struct OsdTheme {
    RgbaColor text { .red = 0.92F, .green = 0.96F, .blue = 1.0F, .alpha = 0.98F };
    RgbaColor bar_background { .red = 0.055F, .green = 0.075F, .blue = 0.095F, .alpha = 80.0F / 255.0F };
    RgbaColor primary { .red = 0.60F, .green = 1.0F, .blue = 0.72F, .alpha = 0.90F };
    RgbaColor secondary { .red = 0.33F, .green = 0.66F, .blue = 1.0F, .alpha = 0.90F };
};

} // namespace glide::flow
