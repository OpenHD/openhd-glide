/******************************************************************************
 * OpenHD - GPU accelerated offline map layer
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#include "glide_flow/gpu_map_renderer.hpp"

#include "common/logging.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <zlib.h>

namespace glide::flow {
namespace {

constexpr int kTileSize = 256;
constexpr double kPi = 3.14159265358979323846;

struct WorldPixel { double x {}; double y {}; };

WorldPixel world_pixel(double latitude, double longitude, int zoom)
{
    latitude = std::clamp(latitude, -85.05112878, 85.05112878);
    const double scale = static_cast<double>(kTileSize) * static_cast<double>(1U << zoom);
    const double radians = latitude * kPi / 180.0;
    return { (longitude + 180.0) / 360.0 * scale,
        (1.0 - std::log(std::tan(radians) + 1.0 / std::cos(radians)) / kPi) * 0.5 * scale };
}

std::uint32_t be32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | bytes[offset + 3];
}

std::uint8_t paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c)
{
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

bool decode_png(const std::vector<std::uint8_t>& bytes, std::vector<std::uint32_t>& pixels)
{
    static constexpr std::array<std::uint8_t, 8> signature { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (bytes.size() < 8 || !std::equal(signature.begin(), signature.end(), bytes.begin())) return false;
    int width = 0, height = 0, color_type = 0;
    std::vector<std::uint8_t> compressed;
    for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
        const auto length = be32(bytes, offset);
        offset += 4;
        if (offset + 8 + length > bytes.size()) return false;
        const std::string type(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        offset += 4;
        if (type == "IHDR") {
            width = static_cast<int>(be32(bytes, offset));
            height = static_cast<int>(be32(bytes, offset + 4));
            if (bytes[offset + 8] != 8 || (bytes[offset + 9] != 2 && bytes[offset + 9] != 6) || bytes[offset + 12] != 0) return false;
            color_type = bytes[offset + 9];
        } else if (type == "IDAT") {
            compressed.insert(compressed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
        }
        offset += length + 4;
        if (type == "IEND") break;
    }
    if (width != kTileSize || height != kTileSize || compressed.empty()) return false;
    const int channels = color_type == 6 ? 4 : 3;
    const std::size_t stride = static_cast<std::size_t>(width * channels);
    std::vector<std::uint8_t> filtered((stride + 1U) * static_cast<std::size_t>(height));
    uLongf output_size = static_cast<uLongf>(filtered.size());
    if (uncompress(filtered.data(), &output_size, compressed.data(), static_cast<uLong>(compressed.size())) != Z_OK || output_size != filtered.size()) return false;
    std::vector<std::uint8_t> raw(stride * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const auto filter = filtered[static_cast<std::size_t>(y) * (stride + 1U)];
        const auto* source = filtered.data() + static_cast<std::size_t>(y) * (stride + 1U) + 1U;
        auto* row = raw.data() + static_cast<std::size_t>(y) * stride;
        const auto* previous = y > 0 ? raw.data() + static_cast<std::size_t>(y - 1) * stride : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const std::uint8_t left = x >= static_cast<std::size_t>(channels) ? row[x - channels] : std::uint8_t { 0 };
            const std::uint8_t up = previous != nullptr ? previous[x] : std::uint8_t { 0 };
            const std::uint8_t upper_left = previous != nullptr && x >= static_cast<std::size_t>(channels) ? previous[x - channels] : std::uint8_t { 0 };
            std::uint8_t predictor = 0;
            if (filter == 1) predictor = left;
            else if (filter == 2) predictor = up;
            else if (filter == 3) predictor = static_cast<std::uint8_t>((static_cast<int>(left) + up) / 2);
            else if (filter == 4) predictor = paeth(left, up, upper_left);
            else if (filter != 0) return false;
            row[x] = static_cast<std::uint8_t>(source[x] + predictor);
        }
    }
    pixels.resize(static_cast<std::size_t>(width * height));
    for (int i = 0; i < width * height; ++i) {
        const auto base = static_cast<std::size_t>(i * channels);
        const auto alpha = channels == 4 ? raw[base + 3] : 0xffU;
        pixels[static_cast<std::size_t>(i)] = (static_cast<std::uint32_t>(alpha) << 24U)
            | (static_cast<std::uint32_t>(raw[base]) << 16U) | (static_cast<std::uint32_t>(raw[base + 1]) << 8U) | raw[base + 2];
    }
    return true;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes)
{
    FILE* file = nullptr;
#if defined(_WIN32)
    fopen_s(&file, path.string().c_str(), "rb");
#else
    file = std::fopen(path.string().c_str(), "rb");
#endif
    if (file == nullptr) return false;
    std::array<std::uint8_t, 4096> chunk {};
    bytes.clear();
    while (const auto count = std::fread(chunk.data(), 1, chunk.size(), file)) bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
    std::fclose(file);
    return true;
}

} // namespace

bool apply_gpu_map_ipc_line(GpuMapState& state, const std::string& line)
{
    if (line == "ui map hidden") { state.visible = false; return true; }
    if (line.rfind("ui map state ", 0) != 0) return false;
    std::istringstream stream(line.substr(13));
    int visible = 0, home_valid = 0;
    if (!(stream >> visible >> state.x >> state.y >> state.width >> state.height >> state.zoom >> state.pan_x >> state.pan_y
          >> state.latitude >> state.longitude >> state.heading >> home_valid >> state.home_latitude >> state.home_longitude)) return false;
    state.visible = visible != 0;
    state.home_valid = home_valid != 0;
    return true;
}

GpuMapRenderer::GpuMapRenderer(std::filesystem::path tile_root)
    : tile_root_(std::move(tile_root)), packages_(glide::ui::discover_offline_map_packages(tile_root_))
{
}

void GpuMapRenderer::select_package(double latitude, double longitude)
{
    auto* best = static_cast<glide::ui::OfflineMapPackage*>(nullptr);
    for (auto& package : packages_) {
        if (!package.contains(latitude, longitude)) continue;
        if (best == nullptr || package.max_zoom() > best->max_zoom() || (package.max_zoom() == best->max_zoom() && package.covered_area() < best->covered_area())) best = &package;
    }
    if (best != active_package_) {
        active_package_ = best;
        atlas_zoom_ = -1;
        glide::log(glide::LogLevel::info, "GlideGpuMap", best != nullptr ? "selected " + best->name() : "using unpacked XYZ fallback");
    }
}

bool GpuMapRenderer::has_tile(int zoom, int x, int y) const
{
    if (active_package_ != nullptr && active_package_->has_tile(zoom, x, y)) return true;
    return std::filesystem::is_regular_file(tile_root_ / std::to_string(zoom) / std::to_string(x) / (std::to_string(y) + ".png"));
}

bool GpuMapRenderer::load_tile(int zoom, int x, int y, std::vector<std::uint32_t>& pixels)
{
    std::vector<std::uint8_t> bytes;
    if (active_package_ != nullptr) active_package_->read_tile(zoom, x, y, bytes);
    if (bytes.empty()) read_file(tile_root_ / std::to_string(zoom) / std::to_string(x) / (std::to_string(y) + ".png"), bytes);
    return !bytes.empty() && decode_png(bytes, pixels);
}

bool GpuMapRenderer::ensure_atlas(const GpuMapState& state)
{
    select_package(state.latitude, state.longitude);
    int zoom = std::clamp(static_cast<int>(std::floor(state.zoom)), 13, 18);
    while (zoom > 13) {
        const auto probe = world_pixel(state.latitude, state.longitude, zoom);
        if (has_tile(zoom, static_cast<int>(probe.x) / kTileSize, static_cast<int>(probe.y) / kTileSize)) break;
        --zoom;
    }
    const double scale = std::pow(2.0, state.zoom - zoom);
    auto center = world_pixel(state.latitude, state.longitude, zoom);
    center.x -= state.pan_x / scale;
    center.y -= state.pan_y / scale;
    const double half_width = state.width / (2.0 * scale);
    const double half_height = state.height / (2.0 * scale);
    const int min_x = static_cast<int>(std::floor((center.x - half_width) / kTileSize)) - 1;
    const int min_y = static_cast<int>(std::floor((center.y - half_height) / kTileSize)) - 1;
    const int max_x = static_cast<int>(std::floor((center.x + half_width) / kTileSize)) + 1;
    const int max_y = static_cast<int>(std::floor((center.y + half_height) / kTileSize)) + 1;
    if (atlas_zoom_ == zoom && atlas_min_x_ == min_x && atlas_min_y_ == min_y && atlas_tiles_x_ == max_x - min_x + 1 && atlas_tiles_y_ == max_y - min_y + 1) return true;
    atlas_zoom_ = zoom; atlas_min_x_ = min_x; atlas_min_y_ = min_y;
    atlas_tiles_x_ = max_x - min_x + 1; atlas_tiles_y_ = max_y - min_y + 1;
    const int width = atlas_tiles_x_ * kTileSize;
    const int height = atlas_tiles_y_ * kTileSize;
    atlas_.assign(static_cast<std::size_t>(width * height), 0xff090e0fU);
    std::vector<std::uint32_t> tile;
    for (int ty = 0; ty < atlas_tiles_y_; ++ty) for (int tx = 0; tx < atlas_tiles_x_; ++tx) {
        if (!load_tile(zoom, min_x + tx, min_y + ty, tile)) continue;
        for (int row = 0; row < kTileSize; ++row) {
            std::copy_n(tile.begin() + static_cast<std::ptrdiff_t>(row * kTileSize), kTileSize,
                atlas_.begin() + static_cast<std::ptrdiff_t>((ty * kTileSize + row) * width + tx * kTileSize));
        }
    }
    if (!texture_renderer_.update_argb_texture(atlas_.data(), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(width * 4))) return false;
    ++texture_uploads_;
    glide::log(glide::LogLevel::info, "GlideGpuMap", "uploaded " + std::to_string(atlas_tiles_x_) + "x" + std::to_string(atlas_tiles_y_) + " tile atlas at Z" + std::to_string(zoom));
    return true;
}

void GpuMapRenderer::draw(GlesTextRenderer& geometry, SurfaceSize surface, const GpuMapState& state)
{
    if (!state.visible || state.width < 1.0F || state.height < 1.0F || !ensure_atlas(state)) return;
    if (trail_.empty() || std::hypot((state.latitude - trail_.back().latitude) * 111320.0,
            (state.longitude - trail_.back().longitude) * 111320.0 * std::cos(state.latitude * kPi / 180.0)) >= 2.0) {
        trail_.push_back({ state.latitude, state.longitude });
        if (trail_.size() > 2000) trail_.erase(trail_.begin(), trail_.begin() + 500);
    }
    const double scale = std::pow(2.0, state.zoom - atlas_zoom_);
    auto center = world_pixel(state.latitude, state.longitude, atlas_zoom_);
    center.x -= state.pan_x / scale;
    center.y -= state.pan_y / scale;
    const double atlas_origin_x = static_cast<double>(atlas_min_x_ * kTileSize);
    const double atlas_origin_y = static_cast<double>(atlas_min_y_ * kTileSize);
    const double source_width = state.width / scale;
    const double source_height = state.height / scale;
    const double atlas_width = static_cast<double>(atlas_tiles_x_ * kTileSize);
    const double atlas_height = static_cast<double>(atlas_tiles_y_ * kTileSize);
    const float u0 = static_cast<float>((center.x - source_width * 0.5 - atlas_origin_x) / atlas_width);
    const float v0 = static_cast<float>((center.y - source_height * 0.5 - atlas_origin_y) / atlas_height);
    const float u1 = static_cast<float>((center.x + source_width * 0.5 - atlas_origin_x) / atlas_width);
    const float v1 = static_cast<float>((center.y + source_height * 0.5 - atlas_origin_y) / atlas_height);
    geometry.draw_filled_quad({state.x, state.y}, {state.x + state.width, state.y}, {state.x, state.y + state.height}, {state.x + state.width, state.y + state.height}, {0.035F, 0.055F, 0.06F, 1.0F}, surface);
    texture_renderer_.draw_cached_argb_texture_region({state.x, state.y}, state.width, state.height, u0, v0, u1, v1, surface);

    const auto screen_point = [&](double lat, double lon) {
        const auto point = world_pixel(lat, lon, atlas_zoom_);
        return RenderPoint { state.x + state.width * 0.5F + static_cast<float>((point.x - center.x) * scale),
            state.y + state.height * 0.5F + static_cast<float>((point.y - center.y) * scale) };
    };
    const auto inside = [&](RenderPoint p) { return p.x >= state.x && p.x <= state.x + state.width && p.y >= state.y && p.y <= state.y + state.height; };
    for (std::size_t index = 1; index < trail_.size(); ++index) {
        const auto a = screen_point(trail_[index - 1].latitude, trail_[index - 1].longitude);
        const auto b = screen_point(trail_[index].latitude, trail_[index].longitude);
        if ((index % 2U) == 0U && (inside(a) || inside(b))) geometry.draw_line(a, b, 2.0F, {0.91F, 0.28F, 0.10F, 0.92F}, surface);
    }
    if (state.home_valid) {
        const auto home = screen_point(state.home_latitude, state.home_longitude);
        if (inside(home)) geometry.draw_circle_outline(home, 8.0F, 2.5F, {0.95F, 0.30F, 0.10F, 1.0F}, surface);
    }
    const RenderPoint aircraft { state.x + state.width * 0.5F, state.y + state.height * 0.5F };
    const float angle = state.heading * static_cast<float>(kPi / 180.0);
    const RenderPoint nose { aircraft.x + std::sin(angle) * 16.0F, aircraft.y - std::cos(angle) * 16.0F };
    const RenderPoint left { aircraft.x + std::sin(angle - 2.35F) * 11.0F, aircraft.y - std::cos(angle - 2.35F) * 11.0F };
    const RenderPoint right { aircraft.x + std::sin(angle + 2.35F) * 11.0F, aircraft.y - std::cos(angle + 2.35F) * 11.0F };
    geometry.draw_line(nose, left, 3.0F, {0.97F, 1.0F, 0.98F, 1.0F}, surface);
    geometry.draw_line(left, right, 3.0F, {0.97F, 1.0F, 0.98F, 1.0F}, surface);
    geometry.draw_line(right, nose, 3.0F, {0.97F, 1.0F, 0.98F, 1.0F}, surface);
}

} // namespace glide::flow
