/******************************************************************************
 * OpenHD - offline Glide map package reader
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace glide::ui {

class OfflineMapPackage {
public:
    bool open(const std::filesystem::path& path, std::string* error = nullptr);
    bool contains(double latitude_deg, double longitude_deg) const;
    bool has_tile(int zoom, int x, int y) const;
    bool read_tile(int zoom, int x, int y, std::vector<std::uint8_t>& bytes) const;

    const std::string& name() const { return name_; }
    const std::filesystem::path& path() const { return path_; }
    int min_zoom() const { return min_zoom_; }
    int max_zoom() const { return max_zoom_; }
    double covered_area() const { return (east_ - west_) * (north_ - south_); }

private:
    struct TileEntry {
        std::uint64_t offset {};
        std::uint32_t length {};
    };

    static std::uint64_t tile_key(int zoom, int x, int y);

    std::filesystem::path path_;
    std::string name_;
    std::string attribution_;
    double south_ {};
    double west_ {};
    double north_ {};
    double east_ {};
    int min_zoom_ {};
    int max_zoom_ {};
    std::map<std::uint64_t, TileEntry> tiles_;
};

std::vector<OfflineMapPackage> discover_offline_map_packages(const std::filesystem::path& bundled_root);

} // namespace glide::ui
