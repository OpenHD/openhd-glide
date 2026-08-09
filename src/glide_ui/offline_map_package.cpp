/******************************************************************************
 * OpenHD - offline Glide map package reader
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#include "glide_ui/offline_map_package.hpp"

#include "common/logging.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <set>

namespace glide::ui {
namespace {

template <typename T>
bool read_value(std::ifstream& stream, T& value)
{
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return stream.good();
}

bool read_string(std::ifstream& stream, std::uint16_t length, std::string& value)
{
    value.resize(length);
    stream.read(value.data(), static_cast<std::streamsize>(length));
    return stream.good();
}

void add_packages_from(const std::filesystem::path& root, std::vector<OfflineMapPackage>& result, std::set<std::filesystem::path>& seen)
{
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return;
    for (const auto& item : std::filesystem::directory_iterator(root, error)) {
        if (error || !item.is_regular_file() || item.path().extension() != ".glidemap") continue;
        const auto canonical = std::filesystem::weakly_canonical(item.path(), error);
        if (error || !seen.insert(canonical).second) continue;
        OfflineMapPackage package;
        std::string message;
        if (package.open(canonical, &message)) {
            glide::log(glide::LogLevel::info, "GlideMap", "loaded offline package " + package.name() + " from " + canonical.string());
            result.push_back(std::move(package));
        } else {
            glide::log(glide::LogLevel::warning, "GlideMap", "ignored " + canonical.string() + ": " + message);
        }
    }
}

} // namespace

std::uint64_t OfflineMapPackage::tile_key(int zoom, int x, int y)
{
    return (static_cast<std::uint64_t>(zoom & 0x3f) << 58U)
        | (static_cast<std::uint64_t>(x & 0x1fffffff) << 29U)
        | static_cast<std::uint64_t>(y & 0x1fffffff);
}

bool OfflineMapPackage::open(const std::filesystem::path& path, std::string* error)
{
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail("cannot open file");
    std::array<char, 8> magic {};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    const std::array<char, 8> expected { 'G', 'L', 'D', 'M', 'A', 'P', '1', '\0' };
    if (!stream || magic != expected) return fail("not a GLDMAP1 package");

    std::uint32_t version = 0;
    std::uint16_t tile_size = 0;
    std::uint8_t min_zoom = 0;
    std::uint8_t max_zoom = 0;
    std::uint16_t name_length = 0;
    std::uint16_t attribution_length = 0;
    std::uint32_t tile_count = 0;
    if (!read_value(stream, version) || !read_value(stream, tile_size)
        || !read_value(stream, min_zoom) || !read_value(stream, max_zoom)
        || !read_value(stream, south_) || !read_value(stream, west_)
        || !read_value(stream, north_) || !read_value(stream, east_)
        || !read_value(stream, name_length) || !read_value(stream, attribution_length)
        || !read_value(stream, tile_count)) {
        return fail("truncated header");
    }
    if (version != 1 || tile_size != 256 || min_zoom > max_zoom || tile_count > 10000000U
        || south_ < -90.0 || north_ > 90.0 || west_ < -180.0 || east_ > 180.0
        || south_ >= north_ || west_ >= east_) {
        return fail("unsupported or invalid header");
    }
    if (!read_string(stream, name_length, name_) || !read_string(stream, attribution_length, attribution_)) {
        return fail("truncated metadata");
    }
    tiles_.clear();
    for (std::uint32_t index = 0; index < tile_count; ++index) {
        std::uint8_t zoom = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        TileEntry entry;
        if (!read_value(stream, zoom) || !read_value(stream, x) || !read_value(stream, y)
            || !read_value(stream, entry.offset) || !read_value(stream, entry.length)) {
            return fail("truncated tile index");
        }
        tiles_[tile_key(zoom, static_cast<int>(x), static_cast<int>(y))] = entry;
    }
    path_ = path;
    min_zoom_ = min_zoom;
    max_zoom_ = max_zoom;
    return true;
}

bool OfflineMapPackage::contains(double latitude_deg, double longitude_deg) const
{
    return latitude_deg >= south_ && latitude_deg <= north_ && longitude_deg >= west_ && longitude_deg <= east_;
}

bool OfflineMapPackage::has_tile(int zoom, int x, int y) const
{
    return tiles_.find(tile_key(zoom, x, y)) != tiles_.end();
}

bool OfflineMapPackage::read_tile(int zoom, int x, int y, std::vector<std::uint8_t>& bytes) const
{
    const auto found = tiles_.find(tile_key(zoom, x, y));
    if (found == tiles_.end()) return false;
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) return false;
    stream.seekg(static_cast<std::streamoff>(found->second.offset));
    bytes.resize(found->second.length);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

std::vector<OfflineMapPackage> discover_offline_map_packages(const std::filesystem::path& bundled_root)
{
    std::vector<OfflineMapPackage> result;
    std::set<std::filesystem::path> seen;
    add_packages_from(bundled_root, result, seen);
    add_packages_from(bundled_root / "packages", result, seen);
    add_packages_from("/usr/share/openhd-glide/assets/maps/packages", result, seen);
    add_packages_from("/usr/local/share/openhd-glide/assets/maps/packages", result, seen);
    if (const auto* configured = std::getenv("GLIDE_MAP_PACKAGE_DIR")) add_packages_from(configured, result, seen);
#ifdef _WIN32
    if (const auto* local = std::getenv("LOCALAPPDATA")) add_packages_from(std::filesystem::path(local) / "OpenHD-Glide" / "maps", result, seen);
#else
    if (const auto* xdg = std::getenv("XDG_DATA_HOME")) {
        add_packages_from(std::filesystem::path(xdg) / "openhd-glide" / "maps", result, seen);
    } else if (const auto* home = std::getenv("HOME")) {
        add_packages_from(std::filesystem::path(home) / ".local" / "share" / "openhd-glide" / "maps", result, seen);
    }
#endif
    return result;
}

} // namespace glide::ui
