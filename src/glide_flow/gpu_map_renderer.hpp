/******************************************************************************
 * OpenHD - GPU accelerated offline map layer
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#pragma once

#include "glide_flow/gles_text_renderer.hpp"
#include "glide_ui/offline_map_package.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace glide::flow {

struct GpuMapState {
    bool visible {};
    float x {};
    float y {};
    float width {};
    float height {};
    double zoom { 15.0 };
    double pan_x {};
    double pan_y {};
    double latitude {};
    double longitude {};
    float heading {};
    bool home_valid {};
    double home_latitude {};
    double home_longitude {};
};

bool apply_gpu_map_ipc_line(GpuMapState& state, const std::string& line);

class GpuMapRenderer {
public:
    explicit GpuMapRenderer(std::filesystem::path tile_root = "assets/maps");
    void draw(GlesTextRenderer& geometry, SurfaceSize surface, const GpuMapState& state);
    std::uint64_t texture_uploads() const { return texture_uploads_; }

private:
    struct TrailPoint { double latitude {}; double longitude {}; };
    bool ensure_atlas(const GpuMapState& state);
    bool load_tile(int zoom, int x, int y, std::vector<std::uint32_t>& pixels);
    bool has_tile(int zoom, int x, int y) const;
    void select_package(double latitude, double longitude);

    std::filesystem::path tile_root_;
    std::vector<glide::ui::OfflineMapPackage> packages_;
    glide::ui::OfflineMapPackage* active_package_ {};
    GlesTextRenderer texture_renderer_;
    std::vector<std::uint32_t> atlas_;
    int atlas_zoom_ { -1 };
    int atlas_min_x_ {};
    int atlas_min_y_ {};
    int atlas_tiles_x_ {};
    int atlas_tiles_y_ {};
    std::vector<TrailPoint> trail_;
    std::uint64_t texture_uploads_ {};
};

} // namespace glide::flow
