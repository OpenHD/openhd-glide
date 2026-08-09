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
#include "glide_ui/offline_map_package.hpp"

#include "common/logging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

namespace glide::ui {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxCachedTiles = 25;
constexpr double kFallbackLatitude = 51.2373245;
constexpr double kFallbackLongitude = 7.1616353;

std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return (static_cast<std::uint32_t>(a) << 24U) | (static_cast<std::uint32_t>(r) << 16U)
        | (static_cast<std::uint32_t>(g) << 8U) | static_cast<std::uint32_t>(b);
}

std::uint32_t rgb(std::uint32_t value)
{
    return 0xff000000U | (value & 0x00ffffffU);
}

double clamp_latitude(double latitude_deg)
{
    return std::clamp(latitude_deg, -85.05112878, 85.05112878);
}

struct WorldPixel {
    double x {};
    double y {};
};

WorldPixel web_mercator_pixel(double latitude_deg, double longitude_deg, int zoom, int tile_size)
{
    const auto scale = static_cast<double>(tile_size) * static_cast<double>(1U << zoom);
    const auto lat_rad = clamp_latitude(latitude_deg) * kPi / 180.0;
    return {
        (longitude_deg + 180.0) / 360.0 * scale,
        (1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / kPi) / 2.0 * scale,
    };
}

void put_pixel(std::vector<std::uint32_t>& buffer, int width, int height, int x, int y, std::uint32_t color)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    buffer[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = color;
}

void fill_rect(std::vector<std::uint32_t>& buffer, int width, int height, int x, int y, int w, int h, std::uint32_t color)
{
    const int x1 = std::max(0, x);
    const int y1 = std::max(0, y);
    const int x2 = std::min(width, x + w);
    const int y2 = std::min(height, y + h);
    if (x2 <= x1 || y2 <= y1) {
        return;
    }
    for (int yy = y1; yy < y2; ++yy) {
        std::fill(
            buffer.begin() + static_cast<std::ptrdiff_t>(yy * width + x1),
            buffer.begin() + static_cast<std::ptrdiff_t>(yy * width + x2),
            color);
    }
}

void blend_pixel(std::vector<std::uint32_t>& buffer, int width, int height, int x, int y, std::uint32_t color)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    const auto source_a = (color >> 24U) & 0xffU;
    if (source_a == 0xffU) {
        put_pixel(buffer, width, height, x, y, color);
        return;
    }
    const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    const auto destination = buffer[index];
    const auto inv_a = 255U - source_a;
    const auto sr = (color >> 16U) & 0xffU;
    const auto sg = (color >> 8U) & 0xffU;
    const auto sb = color & 0xffU;
    const auto dr = (destination >> 16U) & 0xffU;
    const auto dg = (destination >> 8U) & 0xffU;
    const auto db = destination & 0xffU;
    buffer[index] = argb(
        0xff,
        static_cast<std::uint8_t>((sr * source_a + dr * inv_a) / 255U),
        static_cast<std::uint8_t>((sg * source_a + dg * inv_a) / 255U),
        static_cast<std::uint8_t>((sb * source_a + db * inv_a) / 255U));
}

void fill_circle(std::vector<std::uint32_t>& buffer, int width, int height, int cx, int cy, int radius, std::uint32_t color)
{
    const int rr = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= rr) {
                blend_pixel(buffer, width, height, cx + x, cy + y, color);
            }
        }
    }
}

void draw_line(std::vector<std::uint32_t>& buffer, int width, int height, int x0, int y0, int x1, int y1, std::uint32_t color)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        put_pixel(buffer, width, height, x0, y0, color);
        put_pixel(buffer, width, height, x0 + 1, y0, color);
        put_pixel(buffer, width, height, x0, y0 + 1, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void draw_line_width(
    std::vector<std::uint32_t>& buffer,
    int width,
    int height,
    int x0,
    int y0,
    int x1,
    int y1,
    int thickness,
    std::uint32_t color)
{
    const int radius = std::max(0, thickness / 2);
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            if (ox * ox + oy * oy <= radius * radius) {
                draw_line(buffer, width, height, x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
            }
        }
    }
}

void draw_circle(std::vector<std::uint32_t>& buffer, int width, int height, int cx, int cy, int radius, std::uint32_t color)
{
    const int rr = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= rr) {
                put_pixel(buffer, width, height, cx + x, cy + y, color);
            }
        }
    }
}

void fill_triangle(
    std::vector<std::uint32_t>& buffer,
    int width,
    int height,
    std::array<std::pair<int, int>, 3> points,
    std::uint32_t color)
{
    const int min_x = std::max(0, std::min({ points[0].first, points[1].first, points[2].first }));
    const int max_x = std::min(width - 1, std::max({ points[0].first, points[1].first, points[2].first }));
    const int min_y = std::max(0, std::min({ points[0].second, points[1].second, points[2].second }));
    const int max_y = std::min(height - 1, std::max({ points[0].second, points[1].second, points[2].second }));
    const auto edge = [](std::pair<int, int> a, std::pair<int, int> b, int x, int y) {
        return (x - a.first) * (b.second - a.second) - (y - a.second) * (b.first - a.first);
    };
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const int e0 = edge(points[0], points[1], x, y);
            const int e1 = edge(points[1], points[2], x, y);
            const int e2 = edge(points[2], points[0], x, y);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                put_pixel(buffer, width, height, x, y, color);
            }
        }
    }
}

void draw_circle_outline(std::vector<std::uint32_t>& buffer, int width, int height, int cx, int cy, int radius, int thickness, std::uint32_t color)
{
    const int outer = radius * radius;
    const int inner_radius = std::max(0, radius - thickness);
    const int inner = inner_radius * inner_radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            const int distance = x * x + y * y;
            if (distance <= outer && distance >= inner) {
                blend_pixel(buffer, width, height, cx + x, cy + y, color);
            }
        }
    }
}

void draw_segment_letter(
    std::vector<std::uint32_t>& buffer,
    int width,
    int height,
    int x,
    int y,
    char letter,
    int scale,
    std::uint32_t color)
{
    std::array<const char*, 7> rows {};
    if (letter == 'N') {
        rows = { "1001", "1101", "1101", "1011", "1011", "1001", "1001" };
    } else if (letter == 'S') {
        rows = { "1111", "1000", "1000", "1110", "0001", "0001", "1110" };
    } else if (letter == 'E') {
        rows = { "1111", "1000", "1000", "1110", "1000", "1000", "1111" };
    } else if (letter == 'W') {
        rows = { "10001", "10001", "10101", "10101", "10101", "11011", "10001" };
    } else {
        return;
    }

    const int glyph_width = static_cast<int>(std::strlen(rows[0]));
    for (int gy = 0; gy < 7; ++gy) {
        for (int gx = 0; gx < glyph_width; ++gx) {
            if (rows[gy][gx] == '1') {
                fill_rect(buffer, width, height, x + gx * scale, y + gy * scale, scale, scale, color);
            }
        }
    }
}

void draw_compass_label(
    std::vector<std::uint32_t>& buffer,
    int width,
    int height,
    int cx,
    int cy,
    int radius,
    float heading_deg,
    char label,
    float bearing_deg)
{
    const float angle = (bearing_deg - heading_deg) * static_cast<float>(kPi / 180.0);
    const int label_width = label == 'W' ? 5 * 2 : 4 * 2;
    const int label_height = 7 * 2;
    const int x = cx + static_cast<int>(std::lround(std::sin(angle) * radius)) - label_width / 2;
    const int y = cy - static_cast<int>(std::lround(std::cos(angle) * radius)) - label_height / 2;
    draw_segment_letter(buffer, width, height, x + 1, y + 1, label, 2, argb(0xff, 0x04, 0x10, 0x13));
    draw_segment_letter(buffer, width, height, x, y, label, 2, argb(0xff, 0xf9, 0xff, 0xf4));
}

double hash_noise(int x, int y, int seed)
{
    auto value = static_cast<std::uint32_t>(x * 374761393U + y * 668265263U + seed * 1442695041U);
    value = (value ^ (value >> 13U)) * 1274126177U;
    value ^= value >> 16U;
    return static_cast<double>(value & 0xffffU) / 65535.0;
}

double smoothstep(double value)
{
    return value * value * (3.0 - 2.0 * value);
}

double value_noise(double x, double y, double scale, int seed)
{
    const int gx = static_cast<int>(std::floor(x / scale));
    const int gy = static_cast<int>(std::floor(y / scale));
    const double tx = smoothstep((x / scale) - gx);
    const double ty = smoothstep((y / scale) - gy);
    const double a = hash_noise(gx, gy, seed);
    const double b = hash_noise(gx + 1, gy, seed);
    const double c = hash_noise(gx, gy + 1, seed);
    const double d = hash_noise(gx + 1, gy + 1, seed);
    return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (c * (1.0 - tx) + d * tx) * ty;
}

std::uint32_t mix_rgb(std::uint32_t a, std::uint32_t b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const auto ar = static_cast<double>((a >> 16U) & 0xffU);
    const auto ag = static_cast<double>((a >> 8U) & 0xffU);
    const auto ab = static_cast<double>(a & 0xffU);
    const auto br = static_cast<double>((b >> 16U) & 0xffU);
    const auto bg = static_cast<double>((b >> 8U) & 0xffU);
    const auto bb = static_cast<double>(b & 0xffU);
    return argb(
        0xff,
        static_cast<std::uint8_t>(ar * (1.0 - t) + br * t),
        static_cast<std::uint8_t>(ag * (1.0 - t) + bg * t),
        static_cast<std::uint8_t>(ab * (1.0 - t) + bb * t));
}

std::uint32_t crc32_bytes(const std::uint8_t* data, std::size_t size)
{
    return static_cast<std::uint32_t>(crc32(0L, data, static_cast<uInt>(size)));
}

void append_be32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_png_chunk(std::vector<std::uint8_t>& bytes, const char* type, const std::vector<std::uint8_t>& data)
{
    append_be32(bytes, static_cast<std::uint32_t>(data.size()));
    const auto type_start = bytes.size();
    bytes.insert(bytes.end(), type, type + 4);
    bytes.insert(bytes.end(), data.begin(), data.end());
    append_be32(bytes, crc32_bytes(bytes.data() + type_start, bytes.size() - type_start));
}

bool write_png(const std::filesystem::path& path, int width, int height, const std::vector<std::uint32_t>& pixels)
{
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<std::size_t>(width) * 3U + 1U) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < width; ++x) {
            const auto pixel = pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
            raw.push_back(static_cast<std::uint8_t>((pixel >> 16U) & 0xffU));
            raw.push_back(static_cast<std::uint8_t>((pixel >> 8U) & 0xffU));
            raw.push_back(static_cast<std::uint8_t>(pixel & 0xffU));
        }
    }

    uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
        return false;
    }
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> png { 137, 80, 78, 71, 13, 10, 26, 10 };
    std::vector<std::uint8_t> ihdr;
    append_be32(ihdr, static_cast<std::uint32_t>(width));
    append_be32(ihdr, static_cast<std::uint32_t>(height));
    ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 });
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", compressed);
    append_png_chunk(png, "IEND", {});

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const auto written = std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
    return written == png.size();
}

void generate_gta_tile(int z, int tile_x, int tile_y, int size, std::vector<std::uint32_t>& pixels)
{
    (void)z;
    pixels.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), rgb(0x102018));

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double wx = static_cast<double>(tile_x * size + x);
            const double wy = static_cast<double>(tile_y * size + y);
            const double elevation = value_noise(wx, wy, 520.0, 7) * 0.48 + value_noise(wx + 91.0, wy - 37.0, 190.0, 13) * 0.32
                + value_noise(wx - 19.0, wy + 143.0, 68.0, 29) * 0.20;
            const double forest = value_noise(wx + 600.0, wy - 180.0, 155.0, 41);
            const double wet = value_noise(wx - 310.0, wy + 760.0, 260.0, 53);
            const double slope = std::abs(value_noise(wx + 4.0, wy, 115.0, 17) - value_noise(wx - 4.0, wy, 115.0, 17))
                + std::abs(value_noise(wx, wy + 4.0, 115.0, 17) - value_noise(wx, wy - 4.0, 115.0, 17));

            std::uint32_t color {};
            if (elevation < 0.27 && wet > 0.52) {
                color = mix_rgb(0x11334a, 0x1b6670, elevation / 0.27);
            } else if (elevation < 0.36) {
                color = mix_rgb(0x314d2f, 0x55703a, (elevation - 0.20) / 0.16);
            } else if (elevation < 0.62) {
                color = mix_rgb(0x254f2d, 0x63783f, (elevation - 0.36) / 0.26);
            } else if (elevation < 0.80) {
                color = mix_rgb(0x766f4a, 0x8c8464, (elevation - 0.62) / 0.18);
            } else {
                color = mix_rgb(0x8f8f86, 0xd5d1ba, (elevation - 0.80) / 0.20);
            }

            if (forest > 0.58 && elevation > 0.31 && elevation < 0.72) {
                color = mix_rgb(color, 0x143f23, std::min(0.55, (forest - 0.58) * 1.8));
            }
            if (slope > 0.10) {
                color = mix_rgb(color, 0x0b1714, std::min(0.35, slope * 1.8));
            }
            if (std::abs(std::fmod(elevation * 21.0, 1.0) - 0.5) < 0.018 && elevation > 0.32) {
                color = mix_rgb(color, 0xb7d0b0, 0.22);
            }

            // A restrained, high-contrast road hierarchy in the visual
            // language of an in-game navigation map. World-pixel equations
            // keep roads continuous across generated tile boundaries.
            const auto periodic_distance = [](double value, double period) {
                const double wrapped = std::fmod(std::abs(value), period);
                return std::min(wrapped, period - wrapped);
            };
            const double major_vertical = periodic_distance(wx + std::sin(wy / 210.0) * 34.0, 420.0);
            const double major_horizontal = periodic_distance(wy + std::sin(wx / 260.0) * 28.0, 360.0);
            const double local_vertical = periodic_distance(wx + std::sin(wy / 95.0) * 12.0, 118.0);
            const double local_horizontal = periodic_distance(wy + std::sin(wx / 110.0) * 10.0, 126.0);
            const double major = std::min(major_vertical, major_horizontal);
            const double local = std::min(local_vertical, local_horizontal);
            if (major < 5.5) {
                color = rgb(0x29302d);
            }
            if (major < 3.2) {
                color = rgb(0xe4e1d5);
            } else if (local < 1.35) {
                color = rgb(0xb8b8ae);
            }
            pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x)] = color;
        }
    }
}

void draw_rotated_triangle(
    std::vector<std::uint32_t>& buffer,
    int width,
    int height,
    int cx,
    int cy,
    float heading_deg,
    std::uint32_t color)
{
    const float angle = (heading_deg - 90.0F) * static_cast<float>(kPi / 180.0);
    const float ca = std::cos(angle);
    const float sa = std::sin(angle);
    const std::array<std::pair<float, float>, 3> points { { { 16.0F, 0.0F }, { -10.0F, -8.0F }, { -7.0F, 8.0F } } };
    std::array<std::pair<int, int>, 3> rotated {};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto [x, y] = points[i];
        rotated[i] = {
            cx + static_cast<int>(std::lround(x * ca - y * sa)),
            cy + static_cast<int>(std::lround(x * sa + y * ca)),
        };
    }
    fill_triangle(buffer, width, height, rotated, color);
    draw_line_width(buffer, width, height, rotated[0].first, rotated[0].second, rotated[1].first, rotated[1].second, 3, argb(0xff, 0x07, 0x15, 0x1a));
    draw_line_width(buffer, width, height, rotated[1].first, rotated[1].second, rotated[2].first, rotated[2].second, 3, argb(0xff, 0x07, 0x15, 0x1a));
    draw_line_width(buffer, width, height, rotated[2].first, rotated[2].second, rotated[0].first, rotated[0].second, 3, argb(0xff, 0x07, 0x15, 0x1a));
    fill_triangle(buffer, width, height, rotated, color);
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

struct PngImage {
    int width {};
    int height {};
    std::vector<std::uint32_t> pixels;
};

std::uint8_t paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c)
{
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

bool decode_png_bytes(const std::vector<std::uint8_t>& bytes, PngImage& image)
{
    static constexpr std::array<std::uint8_t, 8> signature { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (bytes.size() < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        return false;
    }

    int width = 0;
    int height = 0;
    int color_type = 0;
    std::vector<std::uint8_t> compressed;
    std::size_t offset = 8;
    while (offset + 12 <= bytes.size()) {
        const auto length = read_be32(bytes, offset);
        offset += 4;
        if (offset + 4 + length + 4 > bytes.size()) {
            return false;
        }
        const std::string type(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        offset += 4;
        if (type == "IHDR") {
            width = static_cast<int>(read_be32(bytes, offset));
            height = static_cast<int>(read_be32(bytes, offset + 4));
            const int bit_depth = bytes[offset + 8];
            color_type = bytes[offset + 9];
            const int interlace = bytes[offset + 12];
            if (bit_depth != 8 || interlace != 0 || (color_type != 2 && color_type != 6)) {
                return false;
            }
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        } else if (type == "IEND") {
            break;
        }
        offset += length + 4;
    }
    if (width <= 0 || height <= 0 || compressed.empty()) {
        return false;
    }

    const int channels = color_type == 6 ? 4 : 3;
    const std::size_t stride = static_cast<std::size_t>(width * channels);
    std::vector<std::uint8_t> filtered((stride + 1U) * static_cast<std::size_t>(height));
    uLongf filtered_size = static_cast<uLongf>(filtered.size());
    if (uncompress(filtered.data(), &filtered_size, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK) {
        return false;
    }
    if (filtered_size != filtered.size()) {
        return false;
    }

    std::vector<std::uint8_t> raw(stride * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const auto filter = filtered[static_cast<std::size_t>(y) * (stride + 1U)];
        const auto* source = filtered.data() + static_cast<std::size_t>(y) * (stride + 1U) + 1U;
        auto* row = raw.data() + static_cast<std::size_t>(y) * stride;
        const auto* previous = y > 0 ? raw.data() + static_cast<std::size_t>(y - 1) * stride : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const std::uint8_t left = x >= static_cast<std::size_t>(channels) ? row[x - static_cast<std::size_t>(channels)] : 0;
            const std::uint8_t up = previous != nullptr ? previous[x] : 0;
            const std::uint8_t up_left = previous != nullptr && x >= static_cast<std::size_t>(channels) ? previous[x - static_cast<std::size_t>(channels)] : 0;
            std::uint8_t predictor = 0;
            if (filter == 1) {
                predictor = left;
            } else if (filter == 2) {
                predictor = up;
            } else if (filter == 3) {
                predictor = static_cast<std::uint8_t>((static_cast<int>(left) + static_cast<int>(up)) / 2);
            } else if (filter == 4) {
                predictor = paeth(left, up, up_left);
            } else if (filter != 0) {
                return false;
            }
            row[x] = static_cast<std::uint8_t>(source[x] + predictor);
        }
    }

    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto base = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels);
            const auto r = raw[base];
            const auto g = raw[base + 1];
            const auto b = raw[base + 2];
            const auto a = channels == 4 ? raw[base + 3] : 0xffU;
            image.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = argb(a, r, g, b);
        }
    }
    return true;
}

bool decode_png(const std::filesystem::path& path, PngImage& image)
{
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) return false;
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 4096> chunk {};
    while (const auto count = std::fread(chunk.data(), 1, chunk.size(), file)) {
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
    }
    std::fclose(file);
    return decode_png_bytes(bytes, image);
}

} // namespace

struct MinimapWidget::Impl {
    explicit Impl(int width_value, int height_value, MinimapOptions options_value)
        : width(width_value)
        , height(height_value)
        , options(std::move(options_value))
    {
        options.grid_tiles = options.grid_tiles >= 5 ? 5 : 3;
        position.latitude_deg = kFallbackLatitude;
        position.longitude_deg = kFallbackLongitude;
        home_latitude = kFallbackLatitude;
        home_longitude = kFallbackLongitude;
        has_home = true;
        pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), rgb(options.background_rgb));
        packages = discover_offline_map_packages(options.tile_root);
        select_package(position.latitude_deg, position.longitude_deg);
    }

    struct Tile {
        int z {};
        int x {};
        int y {};
        int width {};
        int height {};
        std::vector<std::uint32_t> pixels;
        std::uint64_t last_used {};
        bool loaded {};
    };

    int width {};
    int height {};
    MinimapOptions options;
    MinimapPosition position;
    bool has_home {};
    double home_latitude {};
    double home_longitude {};
    std::vector<MinimapPosition> trail;
    std::vector<std::uint32_t> pixels;
    std::map<std::string, Tile> cache;
    std::vector<OfflineMapPackage> packages;
    OfflineMapPackage* active_package {};
    std::uint64_t tick {};
    double pan_x {};
    double pan_y {};
    double zoom_level { static_cast<double>(options.zoom) };

    void select_package(double latitude, double longitude)
    {
        OfflineMapPackage* best = nullptr;
        for (auto& package : packages) {
            if (!package.contains(latitude, longitude)) continue;
            if (best == nullptr || package.max_zoom() > best->max_zoom()
                || (package.max_zoom() == best->max_zoom() && package.covered_area() < best->covered_area())) {
                best = &package;
            }
        }
        if (best != active_package) {
            active_package = best;
            cache.clear();
            if (active_package != nullptr) {
                glide::log(glide::LogLevel::info, "GlideMap", "selected offline package " + active_package->name());
            }
        }
    }

    Tile* tile(int z, int x, int y)
    {
        const int max_tile = 1 << z;
        if (y < 0 || y >= max_tile) {
            return nullptr;
        }
        x = ((x % max_tile) + max_tile) % max_tile;
        const auto key = std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y);
        auto& result = cache[key];
        result.last_used = ++tick;
        if (!result.loaded) {
            result.z = z;
            result.x = x;
            result.y = y;
            PngImage image;
            bool decoded = false;
            if (active_package != nullptr) {
                std::vector<std::uint8_t> bytes;
                decoded = active_package->read_tile(z, x, y, bytes) && decode_png_bytes(bytes, image);
            }
            if (!decoded) {
                const auto path = std::filesystem::path(options.tile_root) / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png");
                decoded = decode_png(path, image);
            }
            if (decoded) {
                result.width = image.width;
                result.height = image.height;
                result.pixels = std::move(image.pixels);
            } else {
                result.width = options.tile_size;
                result.height = options.tile_size;
                result.pixels.assign(
                    static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.height),
                    rgb(options.background_rgb));
            }
            result.loaded = true;
            prune_cache();
        }
        return &result;
    }

    bool has_tile_at(int zoom, double latitude, double longitude) const
    {
        const auto world = web_mercator_pixel(latitude, longitude, zoom, options.tile_size);
        const int tile_x = static_cast<int>(std::floor(world.x / static_cast<double>(options.tile_size)));
        const int tile_y = static_cast<int>(std::floor(world.y / static_cast<double>(options.tile_size)));
        if (active_package != nullptr && active_package->has_tile(zoom, tile_x, tile_y)) return true;
        const auto path = std::filesystem::path(options.tile_root) / std::to_string(zoom) / std::to_string(tile_x) / (std::to_string(tile_y) + ".png");
        return std::filesystem::is_regular_file(path);
    }

    std::uint32_t sample_world_nearest(int zoom, double world_x, double world_y)
    {
        const int tile_x = static_cast<int>(std::floor(world_x / static_cast<double>(options.tile_size)));
        const int tile_y = static_cast<int>(std::floor(world_y / static_cast<double>(options.tile_size)));
        auto* source = tile(zoom, tile_x, tile_y);
        if (source == nullptr || source->pixels.empty()) {
            return rgb(options.background_rgb);
        }
        int pixel_x = static_cast<int>(std::floor(world_x - static_cast<double>(tile_x * options.tile_size)));
        int pixel_y = static_cast<int>(std::floor(world_y - static_cast<double>(tile_y * options.tile_size)));
        pixel_x = std::clamp(pixel_x, 0, source->width - 1);
        pixel_y = std::clamp(pixel_y, 0, source->height - 1);
        return source->pixels[static_cast<std::size_t>(pixel_y) * static_cast<std::size_t>(source->width) + static_cast<std::size_t>(pixel_x)];
    }

    std::uint32_t sample_world(int zoom, double world_x, double world_y)
    {
        const double x0 = std::floor(world_x);
        const double y0 = std::floor(world_y);
        const double tx = world_x - x0;
        const double ty = world_y - y0;
        const auto p00 = sample_world_nearest(zoom, x0, y0);
        const auto p10 = sample_world_nearest(zoom, x0 + 1.0, y0);
        const auto p01 = sample_world_nearest(zoom, x0, y0 + 1.0);
        const auto p11 = sample_world_nearest(zoom, x0 + 1.0, y0 + 1.0);
        const auto channel = [&](unsigned shift) {
            const double top = static_cast<double>((p00 >> shift) & 0xffU) * (1.0 - tx)
                + static_cast<double>((p10 >> shift) & 0xffU) * tx;
            const double bottom = static_cast<double>((p01 >> shift) & 0xffU) * (1.0 - tx)
                + static_cast<double>((p11 >> shift) & 0xffU) * tx;
            return static_cast<std::uint8_t>(std::clamp(std::lround(top * (1.0 - ty) + bottom * ty), 0L, 255L));
        };
        return rgb((static_cast<std::uint32_t>(channel(16)) << 16U)
            | (static_cast<std::uint32_t>(channel(8)) << 8U)
            | static_cast<std::uint32_t>(channel(0)));
    }

    void prune_cache()
    {
        while (cache.size() > kMaxCachedTiles) {
            auto victim = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it->second.last_used < victim->second.last_used) {
                    victim = it;
                }
            }
            cache.erase(victim);
        }
    }
};

MinimapWidget::MinimapWidget(lv_obj_t* parent, int width, int height, MinimapOptions options)
    : impl_(new Impl(width, height, std::move(options)))
{
    canvas_ = lv_canvas_create(parent);
    lv_obj_set_size(canvas_, width, height);
    lv_canvas_set_buffer(canvas_, impl_->pixels.data(), width, height, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_style_radius(canvas_, 16, 0);
    lv_obj_set_style_clip_corner(canvas_, true, 0);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        canvas_,
        [](lv_event_t* event) {
            auto* self = static_cast<MinimapWidget*>(lv_event_get_user_data(event));
            auto* indev = lv_indev_active();
            if (self == nullptr) return;
            if (lv_event_get_code(event) == LV_EVENT_PRESSING && indev != nullptr) {
                lv_point_t movement {};
                lv_indev_get_vect(indev, &movement);
                if (movement.x != 0 || movement.y != 0) {
                    self->pan_by_pixels(movement.x, movement.y);
                    if (self->impl_->options.render_on_interaction) self->render();
                }
            } else if (lv_event_get_code(event) == LV_EVENT_LONG_PRESSED) {
                self->recenter();
                if (self->impl_->options.render_on_interaction) self->render();
            }
        },
        LV_EVENT_ALL,
        this);
    lv_obj_invalidate(canvas_);
}

MinimapWidget::~MinimapWidget()
{
    delete impl_;
}

void MinimapWidget::set_home(double latitude_deg, double longitude_deg)
{
    impl_->home_latitude = latitude_deg;
    impl_->home_longitude = longitude_deg;
    impl_->has_home = true;
}

void MinimapWidget::set_position(const MinimapPosition& position)
{
    impl_->position = position;
    impl_->select_package(position.latitude_deg, position.longitude_deg);
    if (impl_->trail.empty()) {
        impl_->trail.push_back(position);
        return;
    }
    const auto last = impl_->trail.back();
    const int trail_zoom = std::clamp(static_cast<int>(std::floor(impl_->zoom_level)), 12, 18);
    const auto dx = web_mercator_pixel(position.latitude_deg, position.longitude_deg, trail_zoom, impl_->options.tile_size).x
        - web_mercator_pixel(last.latitude_deg, last.longitude_deg, trail_zoom, impl_->options.tile_size).x;
    const auto dy = web_mercator_pixel(position.latitude_deg, position.longitude_deg, trail_zoom, impl_->options.tile_size).y
        - web_mercator_pixel(last.latitude_deg, last.longitude_deg, trail_zoom, impl_->options.tile_size).y;
    if (std::hypot(dx, dy) >= 0.5) {
        impl_->trail.push_back(position);
        if (impl_->trail.size() > 2000) {
            impl_->trail.erase(impl_->trail.begin(), impl_->trail.begin() + 500);
        }
    }
}

void MinimapWidget::set_zoom(double zoom)
{
    zoom = std::clamp(zoom, 13.0, 18.0);
    if (std::abs(impl_->zoom_level - zoom) < 0.001) {
        return;
    }
    impl_->zoom_level = zoom;
}

double MinimapWidget::zoom() const
{
    return impl_->zoom_level;
}

void MinimapWidget::pan_by_pixels(int delta_x, int delta_y)
{
    impl_->pan_x += static_cast<double>(delta_x);
    impl_->pan_y += static_cast<double>(delta_y);
}

void MinimapWidget::recenter()
{
    impl_->pan_x = 0.0;
    impl_->pan_y = 0.0;
}

double MinimapWidget::pan_x() const { return impl_->pan_x; }
double MinimapWidget::pan_y() const { return impl_->pan_y; }

void MinimapWidget::render()
{
    auto& state = *impl_;
    std::fill(state.pixels.begin(), state.pixels.end(), rgb(state.options.background_rgb));
    const int cx = state.width / 2;
    const int cy = state.height / 2;
    const int round_radius = std::min(state.width, state.height) / 2 - 2;

    int tile_zoom = std::clamp(static_cast<int>(std::floor(state.zoom_level)), 13, 18);
    while (tile_zoom > 13 && !state.has_tile_at(tile_zoom, state.position.latitude_deg, state.position.longitude_deg)) {
        --tile_zoom;
    }
    const double zoom_scale = std::pow(2.0, state.zoom_level - static_cast<double>(tile_zoom));
    const auto aircraft_world = web_mercator_pixel(state.position.latitude_deg, state.position.longitude_deg, tile_zoom, state.options.tile_size);
    const WorldPixel center_world {
        aircraft_world.x - (state.options.round ? 0.0 : state.pan_x / zoom_scale),
        aircraft_world.y - (state.options.round ? 0.0 : state.pan_y / zoom_scale),
    };
    const float map_heading_deg = state.options.round ? state.position.heading_deg : 0.0F;
    const float heading_rad = map_heading_deg * static_cast<float>(kPi / 180.0);
    const double cos_heading = std::cos(heading_rad);
    const double sin_heading = std::sin(heading_rad);
    const int map_radius = state.options.round ? round_radius - 12 : std::max(state.width, state.height);

    for (int y = 0; y < state.height; ++y) {
        for (int x = 0; x < state.width; ++x) {
            const double screen_dx = static_cast<double>(x - cx);
            const double screen_dy = static_cast<double>(y - cy);
            if (state.options.round && screen_dx * screen_dx + screen_dy * screen_dy > static_cast<double>(map_radius * map_radius)) {
                state.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(state.width) + static_cast<std::size_t>(x)] = argb(0x00, 0x00, 0x00, 0x00);
                continue;
            }

            const double world_dx = screen_dx * cos_heading - screen_dy * sin_heading;
            const double world_dy = screen_dx * sin_heading + screen_dy * cos_heading;
            auto sample = state.sample_world(tile_zoom, center_world.x + world_dx / zoom_scale, center_world.y + world_dy / zoom_scale);
            const auto r = (sample >> 16U) & 0xffU;
            const auto g = (sample >> 8U) & 0xffU;
            const auto b = sample & 0xffU;
            const auto luminance = (r * 54U + g * 183U + b * 19U) / 256U;
            sample = argb(
                0xff,
                static_cast<std::uint8_t>(8U + (luminance * 38U) / 100U),
                static_cast<std::uint8_t>(13U + (luminance * 42U) / 100U),
                static_cast<std::uint8_t>(14U + (luminance * 43U) / 100U));
            state.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(state.width) + static_cast<std::size_t>(x)] = sample;
        }
    }

    auto to_screen = [&](double latitude, double longitude) {
        const auto world = web_mercator_pixel(latitude, longitude, tile_zoom, state.options.tile_size);
        const double world_dx = (world.x - center_world.x) * zoom_scale;
        const double world_dy = (world.y - center_world.y) * zoom_scale;
        return std::pair<int, int> {
            static_cast<int>(std::lround(static_cast<double>(state.width) / 2.0 + world_dx * cos_heading + world_dy * sin_heading)),
            static_cast<int>(std::lround(static_cast<double>(state.height) / 2.0 - world_dx * sin_heading + world_dy * cos_heading)),
        };
    };

    for (std::size_t i = 1; i < state.trail.size(); ++i) {
        const auto [x0, y0] = to_screen(state.trail[i - 1].latitude_deg, state.trail[i - 1].longitude_deg);
        const auto [x1, y1] = to_screen(state.trail[i].latitude_deg, state.trail[i].longitude_deg);
        draw_line_width(state.pixels, state.width, state.height, x0, y0, x1, y1, 5, argb(0xd8, 0x08, 0x0d, 0x0e));
        const double length = std::hypot(static_cast<double>(x1 - x0), static_cast<double>(y1 - y0));
        if (length > 0.0) {
            for (double start = 0.0; start < length; start += 10.0) {
                const double end = std::min(length, start + 5.0);
                const double a = start / length;
                const double b = end / length;
                draw_line_width(
                    state.pixels,
                    state.width,
                    state.height,
                    static_cast<int>(std::lround(x0 + (x1 - x0) * a)),
                    static_cast<int>(std::lround(y0 + (y1 - y0) * a)),
                    static_cast<int>(std::lround(x0 + (x1 - x0) * b)),
                    static_cast<int>(std::lround(y0 + (y1 - y0) * b)),
                    3,
                    argb(0xff, 0xe9, 0x58, 0x27));
            }
        }
    }

    if (state.has_home) {
        const auto [home_x, home_y] = to_screen(state.home_latitude, state.home_longitude);
        fill_rect(state.pixels, state.width, state.height, home_x - 5, home_y - 2, 10, 9, argb(0xff, 0xe9, 0x58, 0x27));
        fill_triangle(
            state.pixels,
            state.width,
            state.height,
            { { { home_x - 7, home_y - 2 }, { home_x, home_y - 9 }, { home_x + 7, home_y - 2 } } },
            argb(0xff, 0xe9, 0x58, 0x27));
        fill_rect(state.pixels, state.width, state.height, home_x - 1, home_y + 2, 3, 5, argb(0xff, 0x09, 0x0e, 0x0f));
    }

    if (state.options.round) {
        for (int y = 0; y < state.height; ++y) {
            for (int x = 0; x < state.width; ++x) {
                const int dx = x - cx;
                const int dy = y - cy;
                if (dx * dx + dy * dy > round_radius * round_radius) {
                    state.pixels[static_cast<std::size_t>(y * state.width + x)] = argb(0x00, 0x00, 0x00, 0x00);
                }
            }
        }
        fill_circle(state.pixels, state.width, state.height, cx, cy, round_radius, argb(0x18, 0xb7, 0xff, 0xff));
        draw_circle_outline(state.pixels, state.width, state.height, cx, cy, round_radius, 9, argb(0xff, 0xfb, 0xff, 0xf3));
        draw_circle_outline(state.pixels, state.width, state.height, cx, cy, round_radius - 10, 2, argb(0xff, 0x63, 0xc9, 0xd4));

        for (int bearing = 0; bearing < 360; bearing += 15) {
            const float angle = (static_cast<float>(bearing) - state.position.heading_deg) * static_cast<float>(kPi / 180.0);
            const int outer = round_radius - 3;
            const int inner = outer - (bearing % 90 == 0 ? 16 : (bearing % 45 == 0 ? 12 : 7));
            const int x0 = cx + static_cast<int>(std::lround(std::sin(angle) * outer));
            const int y0 = cy - static_cast<int>(std::lround(std::cos(angle) * outer));
            const int x1 = cx + static_cast<int>(std::lround(std::sin(angle) * inner));
            const int y1 = cy - static_cast<int>(std::lround(std::cos(angle) * inner));
            draw_line_width(
                state.pixels,
                state.width,
                state.height,
                x0,
                y0,
                x1,
                y1,
                bearing % 90 == 0 ? 3 : 2,
                bearing % 90 == 0 ? argb(0xff, 0xf9, 0xff, 0xf4) : argb(0xff, 0x75, 0xd5, 0xdf));
        }

        draw_compass_label(state.pixels, state.width, state.height, cx, cy, round_radius - 28, state.position.heading_deg, 'N', 0.0F);
        draw_compass_label(state.pixels, state.width, state.height, cx, cy, round_radius - 28, state.position.heading_deg, 'E', 90.0F);
        draw_compass_label(state.pixels, state.width, state.height, cx, cy, round_radius - 28, state.position.heading_deg, 'S', 180.0F);
        draw_compass_label(state.pixels, state.width, state.height, cx, cy, round_radius - 28, state.position.heading_deg, 'W', 270.0F);

        draw_line_width(state.pixels, state.width, state.height, cx - 10, cy - round_radius + 22, cx, cy - round_radius + 10, 3, argb(0xff, 0x75, 0xd5, 0xdf));
        draw_line_width(state.pixels, state.width, state.height, cx + 10, cy - round_radius + 22, cx, cy - round_radius + 10, 3, argb(0xff, 0x75, 0xd5, 0xdf));
    }

    const auto [aircraft_x, aircraft_y] = to_screen(state.position.latitude_deg, state.position.longitude_deg);
    draw_circle(state.pixels, state.width, state.height, aircraft_x, aircraft_y, 18, argb(0xff, 0x05, 0x11, 0x16));
    draw_rotated_triangle(
        state.pixels,
        state.width,
        state.height,
        aircraft_x,
        aircraft_y,
        state.options.round ? 0.0F : state.position.heading_deg,
        argb(0xff, 0xf7, 0xff, 0xf8));

    if (!state.options.round) {
        fill_rect(state.pixels, state.width, state.height, 0, 0, state.width, 2, argb(0xff, 0xb8, 0x43, 0x25));
        fill_rect(state.pixels, state.width, state.height, 0, state.height - 2, state.width, 2, argb(0xff, 0xb8, 0x43, 0x25));
        fill_rect(state.pixels, state.width, state.height, 0, 0, 2, state.height, argb(0xff, 0xb8, 0x43, 0x25));
        fill_rect(state.pixels, state.width, state.height, state.width - 2, 0, 2, state.height, argb(0xff, 0xb8, 0x43, 0x25));
    }

    lv_obj_invalidate(canvas_);
}

double MinimapWidget::home_distance_m() const
{
    if (!impl_->has_home) return 0.0;
    constexpr double earth_radius_m = 6371000.0;
    const double lat1 = impl_->position.latitude_deg * kPi / 180.0;
    const double lat2 = impl_->home_latitude * kPi / 180.0;
    const double dlat = lat2 - lat1;
    const double dlon = (impl_->home_longitude - impl_->position.longitude_deg) * kPi / 180.0;
    const double a = std::sin(dlat * 0.5) * std::sin(dlat * 0.5)
        + std::cos(lat1) * std::cos(lat2) * std::sin(dlon * 0.5) * std::sin(dlon * 0.5);
    return earth_radius_m * 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
}

double MinimapWidget::meters_per_pixel() const
{
    constexpr double earth_circumference_m = 40075016.686;
    const double latitude = impl_->position.latitude_deg * kPi / 180.0;
    return std::cos(latitude) * earth_circumference_m
        / (static_cast<double>(impl_->options.tile_size) * std::pow(2.0, impl_->zoom_level));
}

} // namespace glide::ui
