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

#include "glide_flow/link_overview.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace glide::flow {
namespace {

constexpr RgbaColor muted_color { .red = 0.45F, .green = 0.50F, .blue = 0.52F, .alpha = 0.75F };
constexpr RgbaColor warn_color { .red = 1.0F, .green = 0.84F, .blue = 0.18F, .alpha = 0.96F };
constexpr RgbaColor bad_color { .red = 1.0F, .green = 0.16F, .blue = 0.14F, .alpha = 0.96F };

std::string fixed_1(float value)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(1);
    stream << value;
    return stream.str();
}

std::string invalid_or_int_text(int value, std::string_view suffix = {})
{
    if (value <= -127) {
        return "N/A";
    }
    return std::to_string(value) + std::string(suffix);
}

std::string channel_text(int frequency_mhz, int channel_width_mhz)
{
    std::string text = frequency_mhz > 10
        ? std::to_string(frequency_mhz) + " MHz"
        : "FREQ N/A";
    if (channel_width_mhz == 40 || channel_width_mhz == 20) {
        text += " BW" + std::to_string(channel_width_mhz);
    } else {
        text += " BW N/A";
    }
    return text;
}

std::string bitrate_mcs_text(float bitrate_mbit, int mcs)
{
    if (bitrate_mbit <= 0.0F || mcs < 0) {
        return "RATE N/A";
    }
    std::string value = bitrate_mbit < 10.0F
        ? fixed_1(bitrate_mbit)
        : std::to_string(static_cast<int>(std::round(bitrate_mbit)));
    return value + " MBit/s [" + std::to_string(mcs) + "]";
}

RgbaColor quality_color(int value)
{
    if (value > 60) {
        return RgbaColor { .red = 0.30F, .green = 1.0F, .blue = 0.36F, .alpha = 0.92F };
    }
    if (value >= 20) {
        return warn_color;
    }
    return bad_color;
}

void draw_text(GlesTextRenderer& renderer, std::string text, float x, float baseline, float scale, SurfaceSize surface)
{
    renderer.draw(TextPlacement {
        .text = std::move(text),
        .x = x,
        .y = baseline,
        .scale = scale,
    }, surface);
}

void draw_centered_text(GlesTextRenderer& renderer, std::string text, float center_x, float baseline, float scale, SurfaceSize surface)
{
    const auto width = renderer.measure_text_width(text, scale);
    draw_text(renderer, std::move(text), center_x - width * 0.5F, baseline, scale, surface);
}

float layout_scale(SurfaceSize surface)
{
    return std::max(0.70F, std::min(
        static_cast<float>(surface.width) / 1280.0F,
        static_cast<float>(surface.height) / 720.0F));
}

float sx(float value, float scale)
{
    return value * scale;
}

void draw_panel_left(GlesTextRenderer& renderer, float x, float y, float width, float height, SurfaceSize surface, const OsdTheme& theme)
{
    renderer.draw_filled_quad(
        { x, y },
        { x + width, y },
        { x, y + height },
        { x + (width * 0.80F), y + height },
        theme.bar_background,
        surface);
}

void draw_panel_right(GlesTextRenderer& renderer, float x, float y, float width, float height, SurfaceSize surface, const OsdTheme& theme)
{
    renderer.draw_filled_quad(
        { x, y },
        { x + width, y },
        { x + (width * 0.20F), y + height },
        { x + width, y + height },
        theme.bar_background,
        surface);
}

void draw_bottom_panel(GlesTextRenderer& renderer, SurfaceSize surface, const OsdTheme& theme)
{
    const auto scale = layout_scale(surface);
    const auto height = sx(40.0F, scale);
    const auto y = static_cast<float>(surface.height) - height;
    const auto width = static_cast<float>(surface.width);
    const auto center_x = width * 0.5F;
    const auto top_y = y + sx(8.0F, scale);
    const auto notch_top_y = y - sx(6.0F, scale);
    const auto notch_width = sx(154.0F, scale);
    const auto notch_slope = sx(14.0F, scale);
    const auto notch_start = center_x - notch_width * 0.5F;
    const auto notch_end = center_x + notch_width * 0.5F;
    const auto notch_left = std::max(sx(6.0F, scale), notch_start - notch_slope);
    const auto notch_right = std::min(width - sx(6.0F, scale), notch_end + notch_slope);
    const auto bottom_y = y + height;

    renderer.draw_filled_quad({ 0.0F, top_y }, { notch_left, top_y }, { 0.0F, bottom_y }, { notch_left, bottom_y }, theme.bar_background, surface);
    renderer.draw_filled_quad({ notch_right, top_y }, { width, top_y }, { notch_right, bottom_y }, { width, bottom_y }, theme.bar_background, surface);
    renderer.draw_filled_quad({ notch_left, top_y }, { notch_start, notch_top_y }, { notch_left, bottom_y }, { notch_start, bottom_y }, theme.bar_background, surface);
    renderer.draw_filled_quad({ notch_start, notch_top_y }, { notch_end, notch_top_y }, { notch_start, bottom_y }, { notch_end, bottom_y }, theme.bar_background, surface);
    renderer.draw_filled_quad({ notch_end, notch_top_y }, { notch_right, top_y }, { notch_end, bottom_y }, { notch_right, bottom_y }, theme.bar_background, surface);
}

void draw_skew_blocks(GlesTextRenderer& renderer, float x, float y, int value, SurfaceSize surface, const OsdTheme& theme)
{
    const auto scale = layout_scale(surface);
    constexpr int count = 8;
    const auto block_width = sx(18.0F, scale);
    const auto block_height = sx(9.0F, scale);
    const auto skew = sx(5.0F, scale);
    const auto spacing = sx(4.0F, scale);

    for (int i = 0; i < count; ++i) {
        const auto bx = x + static_cast<float>(i) * (block_width + skew + spacing);
        const auto active = value >= (i + 1) * 10;
        const auto base = active ? quality_color(value) : muted_color;
        const RgbaColor fill {
            .red = base.red,
            .green = base.green,
            .blue = base.blue,
            .alpha = active ? 0.92F : 0.18F,
        };

        renderer.draw_filled_quad(
            { bx + skew, y },
            { bx + block_width + skew, y },
            { bx, y + block_height },
            { bx + block_width, y + block_height },
            fill,
            surface);
        renderer.draw_line({ bx, y + block_height }, { bx + skew, y }, sx(1.2F, scale), base, surface);
        renderer.draw_line({ bx + skew, y }, { bx + block_width + skew, y }, sx(1.2F, scale), base, surface);
        renderer.draw_line({ bx + block_width + skew, y }, { bx + block_width, y + block_height }, sx(1.2F, scale), base, surface);
        renderer.draw_line({ bx + block_width, y + block_height }, { bx, y + block_height }, sx(1.2F, scale), base, surface);
    }
}

void draw_left(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample, const OsdTheme& theme)
{
    const auto scale = layout_scale(surface);
    constexpr float x = 0.0F;
    constexpr float y = 0.0F;
    const auto width = sx(420.0F, scale);
    const auto height = sx(72.0F, scale);

    draw_panel_left(renderer, x, y, width, height, surface, theme);
    renderer.set_text_color(theme.text);
    draw_text(
        renderer,
        invalid_or_int_text(sample.rssi_dbm) + " DBM " + invalid_or_int_text(sample.txc_temp_c, "C"),
        x + sx(26.0F, scale),
        y + sx(36.0F, scale),
        sx(18.0F, scale),
        surface);
    draw_skew_blocks(renderer, x + sx(26.0F, scale), y + sx(50.0F, scale), sample.downlink_quality, surface, theme);
}

void draw_right(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample, const OsdTheme& theme)
{
    const auto scale = layout_scale(surface);
    const auto width = sx(420.0F, scale);
    const auto height = sx(72.0F, scale);
    const auto x = static_cast<float>(surface.width) - width;
    constexpr float y = 0.0F;

    draw_panel_right(renderer, x, y, width, height, surface, theme);
    draw_text(renderer, sample.uplink_ok ? "UP" : "NO", x + sx(74.0F, scale), y + sx(31.0F, scale), sx(15.0F, scale), surface);
    const auto text_center_x = x + sx(255.0F, scale);
    draw_centered_text(renderer, channel_text(sample.frequency_mhz, sample.channel_width_mhz), text_center_x, y + sx(24.0F, scale), sx(14.0F, scale), surface);
    draw_centered_text(renderer, bitrate_mcs_text(sample.bitrate_mbit, sample.mcs), text_center_x, y + sx(42.0F, scale), sx(14.0F, scale), surface);

    renderer.set_text_color(theme.text);
    const auto record_color = sample.recording ? bad_color : theme.text;
    renderer.draw_circle_outline({ x + width - sx(24.0F, scale), y + sx(33.0F, scale) }, sx(8.0F, scale), sx(3.0F, scale), record_color, surface);
    draw_skew_blocks(renderer, x + sx(162.0F, scale), y + sx(50.0F, scale), sample.rc_quality, surface, theme);
}

struct BottomSlot {
    std::string label;
    std::string value;
};

std::string distance_text(float meters)
{
    if (meters >= 1000.0F) {
        return fixed_1(meters / 1000.0F) + "km";
    }
    return std::to_string(static_cast<int>(std::round(std::max(0.0F, meters)))) + "m";
}

std::string coordinate_value_text(double value)
{
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(5);
    stream << value;
    return stream.str();
}

float estimated_text_width(std::string_view text, float scale)
{
    return static_cast<float>(text.size()) * scale * 0.56F;
}

std::string kmh_text(float meters_per_second)
{
    return std::to_string(static_cast<int>(std::round(std::max(0.0F, meters_per_second) * 3.6F))) + "km/h";
}

std::string flight_time_text(std::chrono::seconds duration)
{
    const auto total_seconds = std::max<std::chrono::seconds::rep>(0, duration.count());
    const auto minutes = total_seconds / 60;
    const auto seconds = total_seconds % 60;
    std::ostringstream stream;
    stream << minutes << ':';
    if (seconds < 10) {
        stream << '0';
    }
    stream << seconds;
    return stream.str();
}

std::vector<BottomSlot> primary_bottom_slots(const LinkOverviewSample& sample)
{
    return {
        { "G", fixed_1(sample.ground_voltage_v) + "V" },
        { "A", fixed_1(sample.air_voltage_v) + "V " + fixed_1(sample.air_current_a) + "A" },
        { "E", fixed_1(sample.mah_per_km) + "mAh/km" },
    };
}

std::vector<BottomSlot> secondary_bottom_slots(const LinkOverviewSample& sample)
{
    std::vector<BottomSlot> slots {
        { "W", kmh_text(sample.wind_speed_mps) },
        { "H", distance_text(sample.home_distance_m) },
        { "D", distance_text(sample.total_distance_m) },
    };
    return slots;
}

void draw_bottom_slot(
    GlesTextRenderer& renderer,
    const BottomSlot& slot,
    float center_x,
    float available_width,
    float baseline,
    float scale,
    SurfaceSize surface)
{
    if (slot.value.empty() || slot.value == "N/A") {
        return;
    }

    const auto text = slot.label.empty() ? slot.value : slot.label + slot.value;
    auto font_scale = sx(10.5F, scale);
    auto estimated_width = static_cast<float>(text.size()) * font_scale * 0.56F;
    if (estimated_width > available_width) {
        font_scale = std::max(sx(7.5F, scale), font_scale * (available_width / estimated_width));
        estimated_width = static_cast<float>(text.size()) * font_scale * 0.56F;
    }
    draw_text(renderer, text, center_x - estimated_width * 0.5F, baseline, font_scale, surface);
}

void draw_bottom_slots(
    GlesTextRenderer& renderer,
    const std::vector<BottomSlot>& slots,
    float start_x,
    float width,
    float baseline,
    float scale,
    SurfaceSize surface)
{
    if (slots.empty()) {
        return;
    }

    const auto slot_width = width / static_cast<float>(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        draw_bottom_slot(
            renderer,
            slots[i],
            start_x + (static_cast<float>(i) + 0.5F) * slot_width,
            slot_width - sx(3.0F, scale),
            baseline,
            scale,
            surface);
    }
}

void draw_gps_position(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample)
{
    if (!sample.show_coordinates) {
        return;
    }

    const auto scale = layout_scale(surface);
    const auto font_scale = sx(10.0F, scale);
    const auto satellite_scale = sx(22.0F, scale);
    const auto margin = sx(54.0F, scale);
    const auto satellite_gap = sx(14.0F, scale);
    const auto label_width = sx(23.0F, scale);
    const auto value_gap = sx(5.0F, scale);
    const auto bottom_bar_height = sx(40.0F, scale);
    const auto baseline_2 = static_cast<float>(surface.height) - bottom_bar_height - sx(9.0F, scale);
    const auto baseline_1 = baseline_2 - sx(14.0F, scale);
    const auto satellites = std::to_string(sample.satellites);
    const auto lat_value = coordinate_value_text(sample.latitude_deg);
    const auto lon_value = coordinate_value_text(sample.longitude_deg);
    const auto satellite_width = static_cast<float>(satellites.size()) * satellite_scale * 0.56F;
    const auto lat_decimal = lat_value.find('.');
    const auto lon_decimal = lon_value.find('.');
    const auto lat_integer = lat_decimal == std::string::npos ? lat_value : lat_value.substr(0, lat_decimal);
    const auto lon_integer = lon_decimal == std::string::npos ? lon_value : lon_value.substr(0, lon_decimal);
    const auto lat_fraction = lat_decimal == std::string::npos ? std::string {} : lat_value.substr(lat_decimal + 1);
    const auto lon_fraction = lon_decimal == std::string::npos ? std::string {} : lon_value.substr(lon_decimal + 1);
    const auto integer_width = std::max(estimated_text_width(lat_integer, font_scale), estimated_text_width(lon_integer, font_scale));
    const auto dot_width = estimated_text_width(".", font_scale);
    const auto fraction_width = std::max(estimated_text_width(lat_fraction, font_scale), estimated_text_width(lon_fraction, font_scale));
    const auto block_width = satellite_width + satellite_gap + label_width + value_gap + integer_width + dot_width + fraction_width;
    const auto x = std::max(margin, static_cast<float>(surface.width) - margin - block_width);
    const auto label_x = x + satellite_width + satellite_gap;
    const auto decimal_x = label_x + label_width + value_gap + integer_width;
    const auto fraction_x = decimal_x + dot_width;

    draw_text(renderer, satellites, x, baseline_2 - sx(1.0F, scale), satellite_scale, surface);
    draw_text(renderer, "LAT", label_x, baseline_1, font_scale, surface);
    draw_text(renderer, "LON", label_x, baseline_2, font_scale, surface);
    draw_text(renderer, lat_integer, decimal_x - estimated_text_width(lat_integer, font_scale), baseline_1, font_scale, surface);
    draw_text(renderer, lon_integer, decimal_x - estimated_text_width(lon_integer, font_scale), baseline_2, font_scale, surface);
    draw_text(renderer, ".", decimal_x, baseline_1, font_scale, surface);
    draw_text(renderer, ".", decimal_x, baseline_2, font_scale, surface);
    draw_text(renderer, lat_fraction, fraction_x, baseline_1, font_scale, surface);
    draw_text(renderer, lon_fraction, fraction_x, baseline_2, font_scale, surface);
}

void draw_bottom_impl(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample, const OsdTheme& theme)
{
    const auto scale = layout_scale(surface);
    const auto height = sx(40.0F, scale);
    const auto y = static_cast<float>(surface.height) - height;
    const auto center_x = static_cast<float>(surface.width) * 0.5F;
    const auto notch_safe_width = sx(168.0F, scale);
    const auto side_margin = sx(20.0F, scale);
    const auto content_top = y + sx(8.0F, scale);
    const auto side_baseline = content_top + sx(24.0F, scale);
    const auto left_width = std::max(0.0F, center_x - notch_safe_width * 0.5F - side_margin);
    const auto right_x = center_x + notch_safe_width * 0.5F;
    const auto right_width = std::max(0.0F, static_cast<float>(surface.width) - right_x - side_margin);
    const auto mode = sample.flight_mode != nullptr ? sample.flight_mode : "MANUAL";
    const auto mode_scale = sx(sample.armed ? 15.0F : 14.0F, scale);
    const auto timer = flight_time_text(sample.flight_time);
    const auto timer_scale = sx(8.6F, scale);
    const auto mode_width = renderer.measure_text_width(mode, mode_scale);
    const auto timer_width = renderer.measure_text_width(timer, timer_scale);

    draw_bottom_panel(renderer, surface, theme);
    renderer.set_text_color(theme.text);
    draw_gps_position(renderer, surface, sample);
    draw_text(renderer, mode, center_x - mode_width * 0.5F, y + sx(21.0F, scale), mode_scale, surface);
    draw_text(renderer, timer, center_x - timer_width * 0.5F, y + sx(35.0F, scale), timer_scale, surface);
    draw_bottom_slots(renderer, primary_bottom_slots(sample), side_margin, left_width, side_baseline, scale, surface);
    draw_bottom_slots(renderer, secondary_bottom_slots(sample), right_x, right_width, side_baseline, scale, surface);
}

} // namespace

LinkOverviewSample SimulatedLinkOverview::sample(std::chrono::steady_clock::time_point now) const
{
    const auto seconds = std::chrono::duration<float>(now - start_).count();
    const auto wave = std::sin(seconds * 0.55F);
    const auto faster = std::sin(seconds * 1.10F);

    return LinkOverviewSample {
        .rssi_dbm = -58 + static_cast<int>(std::round(wave * 5.0F)),
        .txc_temp_c = 62 + static_cast<int>(std::round(faster * 4.0F)),
        .mcs = 2,
        .downlink_quality = std::clamp(76 + static_cast<int>(std::round(wave * 18.0F)), 0, 100),
        .frequency_mhz = 5800,
        .channel_width_mhz = 20,
        .bitrate_mbit = 12.5F + faster * 1.8F,
        .recording = std::sin(seconds * 0.35F) > 0.0F,
        .uplink_ok = std::sin(seconds * 0.23F) > -0.85F,
        .rc_quality = std::clamp(68 + static_cast<int>(std::round(faster * 20.0F)), 0, 100),
        .ground_voltage_v = 11.8F + wave * 0.2F,
        .ground_mah = 0,
        .air_voltage_v = 15.7F + faster * 0.3F,
        .air_current_a = 4.2F + std::sin(seconds * 0.75F) * 1.4F,
        .air_speed_mps = 23.0F + std::sin(seconds * 0.36F) * 6.0F,
        .height_m = 86.0F + std::sin(seconds * 0.24F) * 18.0F,
        .home_distance_m = 145.0F + std::sin(seconds * 0.18F) * 45.0F,
        .total_distance_m = 1820.0F + seconds * 7.5F,
        .wind_speed_mps = 5.0F + std::sin(seconds * 0.31F) * 2.0F,
        .wind_direction_deg = 290.0F + std::sin(seconds * 0.14F) * 35.0F,
        .latitude_deg = 47.397742 + std::sin(seconds * 0.025F) * 0.003,
        .longitude_deg = 8.545594 + std::cos(seconds * 0.021F) * 0.003,
        .mah_per_km = 132.0F + std::sin(seconds * 0.42F) * 9.0F,
        .satellites = 14 + static_cast<int>(std::round(std::sin(seconds * 0.20F) * 2.0F)),
        .show_coordinates = true,
        .flight_time = std::chrono::seconds(static_cast<int>(seconds)),
        .armed = std::sin(seconds * 0.08F) > -0.30F,
        .flight_mode = "LOITER",
    };
}

void LinkOverviewRenderer::draw(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample, const OsdTheme& theme) const
{
    draw_left(renderer, surface, sample, theme);
    draw_right(renderer, surface, sample, theme);
    draw_bottom_impl(renderer, surface, sample, theme);
}

void LinkOverviewRenderer::draw_top(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample, const OsdTheme& theme) const
{
    draw_left(renderer, surface, sample, theme);
    draw_right(renderer, surface, sample, theme);
}

void LinkOverviewRenderer::draw_bottom(GlesTextRenderer& renderer, SurfaceSize surface, const LinkOverviewSample& sample, const OsdTheme& theme) const
{
    draw_bottom_impl(renderer, surface, sample, theme);
}

} // namespace glide::flow
