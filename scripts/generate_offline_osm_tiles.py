#!/usr/bin/env python3
################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
################################################################################

"""Create a small offline raster map from real OpenStreetMap road geometry.

This fetches vector data from an Overpass endpoint once, then renders ordinary
XYZ PNG tiles locally. It deliberately does not download from the public OSM
raster tile service, whose usage policy prohibits offline prefetching.
"""

import argparse
import json
import math
import pathlib
import struct
import urllib.parse
import urllib.request
import zlib


TILE_SIZE = 256


def tile_xy(lat, lon, zoom):
    lat = max(-85.05112878, min(85.05112878, lat))
    n = 1 << zoom
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) * 0.5 * n
    return x, y


def tile_lon(x, zoom):
    return x / (1 << zoom) * 360.0 - 180.0


def tile_lat(y, zoom):
    return math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * y / (1 << zoom)))))


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def write_png(path, width, height, rgb):
    stride = width * 3
    raw = b"".join(b"\0" + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(height))
    payload = b"\x89PNG\r\n\x1a\n"
    payload += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    payload += png_chunk(b"IDAT", zlib.compress(raw, 7))
    payload += png_chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)


def write_glidemap(path, root, name, attribution, bounds, zooms):
    """Write Glide's dependency-free, indexed single-file offline map."""
    tiles = []
    for zoom in zooms:
        zoom_root = root / str(zoom)
        if not zoom_root.exists():
            continue
        for tile_path in zoom_root.glob("*/*.png"):
            tiles.append((zoom, int(tile_path.parent.name), int(tile_path.stem), tile_path.read_bytes()))
    tiles.sort(key=lambda item: (item[0], item[1], item[2]))
    if not tiles:
        raise RuntimeError("cannot create an empty map package")
    name_bytes = name.encode("utf-8")
    attribution_bytes = attribution.encode("utf-8")
    header_format = "<8sIHBBddddHHI"
    entry_format = "<BIIQI"
    data_offset = struct.calcsize(header_format) + len(name_bytes) + len(attribution_bytes) + len(tiles) * struct.calcsize(entry_format)
    entries = []
    offset = data_offset
    for zoom, x, y, payload in tiles:
        entries.append(struct.pack(entry_format, zoom, x, y, offset, len(payload)))
        offset += len(payload)
    south, west, north, east = bounds
    header = struct.pack(
        header_format, b"GLDMAP1\0", 1, TILE_SIZE, min(zooms), max(zooms),
        south, west, north, east, len(name_bytes), len(attribution_bytes), len(tiles))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(header)
        output.write(name_bytes)
        output.write(attribution_bytes)
        for entry in entries:
            output.write(entry)
        for _, _, _, payload in tiles:
            output.write(payload)
    print(f"Packed {len(tiles)} tiles into {path} ({path.stat().st_size / (1024 * 1024):.1f} MiB)")


def put_disc(image, width, height, x, y, radius, color):
    left = max(0, x - radius)
    right = min(width - 1, x + radius)
    top = max(0, y - radius)
    bottom = min(height - 1, y + radius)
    radius_sq = radius * radius
    for py in range(top, bottom + 1):
        for px in range(left, right + 1):
            if (px - x) ** 2 + (py - y) ** 2 <= radius_sq:
                offset = (py * width + px) * 3
                image[offset:offset + 3] = bytes(color)


def line(image, width, height, x0, y0, x1, y1, stroke, color):
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    error = dx + dy
    radius = max(1, stroke // 2)
    while True:
        put_disc(image, width, height, x0, y0, radius, color)
        if x0 == x1 and y0 == y1:
            return
        twice = error * 2
        if twice >= dy:
            error += dy
            x0 += sx
        if twice <= dx:
            error += dx
            y0 += sy


def road_style(tags):
    highway = tags.get("highway", "")
    if highway in {"motorway", "trunk"}:
        return 4, (67, 76, 77)
    if highway in {"primary", "secondary"}:
        return 3, (59, 69, 70)
    if highway in {"tertiary", "unclassified"}:
        return 2, (51, 61, 62)
    if highway in {"residential", "living_street", "service"}:
        return 2, (43, 53, 54)
    if highway in {"track", "path", "footway", "cycleway"}:
        return 1, (34, 43, 44)
    return 1, (39, 48, 49)


def fetch_osm(endpoint, south, west, north, east):
    elements = []
    # Separate queries are substantially cheaper for public Overpass servers
    # than a union over a dense urban bounding box.
    for tag in ("highway", "waterway"):
        query = f'''[out:json][timeout:90];
way["{tag}"]({south},{west},{north},{east});
out geom;'''
        request = urllib.request.Request(
            endpoint,
            data=urllib.parse.urlencode({"data": query}).encode("utf-8"),
            headers={"User-Agent": "OpenHD-Glide-offline-map/0.1 (+https://github.com/OpenHD/openhd-glide)"},
        )
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                elements.extend(json.load(response).get("elements", []))
        except Exception as error:
            if tag == "waterway":
                print(f"Skipping optional waterway data for this cell: {error}")
                continue
            raise
    return {"elements": elements}


def fetch_osm_resilient(endpoint, south, west, north, east, allow_partial, split_depth=0):
    try:
        return fetch_osm(endpoint, south, west, north, east)
    except Exception as error:
        if split_depth >= 2 and allow_partial:
            print(f"WARNING: omitting unavailable Overpass sub-cell: {error}")
            return {"elements": []}
        if split_depth >= 2:
            raise
        print(f"Overpass cell failed ({error}); splitting it into four smaller cells")
        middle_lat = (south + north) * 0.5
        middle_lon = (west + east) * 0.5
        elements = {}
        for child_south, child_west, child_north, child_east in (
            (south, west, middle_lat, middle_lon),
            (south, middle_lon, middle_lat, east),
            (middle_lat, west, north, middle_lon),
            (middle_lat, middle_lon, north, east),
        ):
            child = fetch_osm_resilient(
                endpoint, child_south, child_west, child_north, child_east, allow_partial, split_depth + 1)
            for element in child.get("elements", []):
                elements[(element.get("type"), element.get("id"))] = element
        return {"elements": list(elements.values())}


def fetch_osm_grid(endpoint, south, west, north, east, grid_size, cache_root, allow_partial):
    """Split dense urban requests to stay below public Overpass time limits."""
    if grid_size <= 1:
        return fetch_osm(endpoint, south, west, north, east)
    cache_root.mkdir(parents=True, exist_ok=True)
    elements = {}
    for row in range(grid_size):
        cell_south = south + (north - south) * row / grid_size
        cell_north = south + (north - south) * (row + 1) / grid_size
        for column in range(grid_size):
            cell_west = west + (east - west) * column / grid_size
            cell_east = west + (east - west) * (column + 1) / grid_size
            cache_path = cache_root / f"cell-{grid_size}-{row}-{column}.json"
            if cache_path.exists():
                print(f"Using cached Overpass cell {row * grid_size + column + 1}/{grid_size * grid_size}")
                data = json.loads(cache_path.read_text(encoding="utf-8"))
            else:
                print(f"Downloading Overpass cell {row * grid_size + column + 1}/{grid_size * grid_size}")
                data = fetch_osm_resilient(endpoint, cell_south, cell_west, cell_north, cell_east, allow_partial)
                cache_path.write_text(json.dumps(data, separators=(",", ":")), encoding="utf-8")
            for element in data.get("elements", []):
                elements[(element.get("type"), element.get("id"))] = element
    return {"elements": list(elements.values())}


def main():
    parser = argparse.ArgumentParser(description="Render a small offline XYZ tile pack from real OSM roads.")
    parser.add_argument("--root", default="assets/maps")
    parser.add_argument("--zoom", type=int, default=15)
    parser.add_argument("--min-zoom", type=int, help="lowest zoom for a multi-zoom package")
    parser.add_argument("--max-zoom", type=int, help="highest zoom for a multi-zoom package")
    parser.add_argument("--lat", type=float, default=51.2373245)
    parser.add_argument("--lon", type=float, default=7.1616353)
    parser.add_argument("--radius", type=int, default=2, help="tiles around the center (2 creates a 5x5 pack)")
    parser.add_argument("--radius-km", type=float, help="geographic radius; creates a square covering this radius")
    parser.add_argument("--package", help="write all generated zooms to one .glidemap file")
    parser.add_argument("--name", default="OpenHD offline map")
    parser.add_argument("--endpoint", default="https://overpass-api.de/api/interpreter")
    parser.add_argument("--query-grid", type=int, default=1,
                        help="split the Overpass request into NxN cells for dense urban regions")
    parser.add_argument("--allow-partial", action="store_true",
                        help="warn and omit a sub-cell only after recursive Overpass retries fail")
    args = parser.parse_args()

    min_zoom = args.min_zoom if args.min_zoom is not None else args.zoom
    max_zoom = args.max_zoom if args.max_zoom is not None else args.zoom
    if not 0 <= min_zoom <= max_zoom <= 22:
        parser.error("zoom range must satisfy 0 <= min <= max <= 22")
    zooms = list(range(min_zoom, max_zoom + 1))
    if args.radius_km:
        lat_delta = args.radius_km / 111.32
        lon_delta = args.radius_km / (111.32 * math.cos(math.radians(args.lat)))
        requested_bounds = (args.lat - lat_delta, args.lon - lon_delta, args.lat + lat_delta, args.lon + lon_delta)
    else:
        center_x, center_y = tile_xy(args.lat, args.lon, max_zoom)
        min_x = math.floor(center_x) - args.radius
        min_y = math.floor(center_y) - args.radius
        max_x = min_x + args.radius * 2 + 1
        max_y = min_y + args.radius * 2 + 1
        requested_bounds = (tile_lat(max_y, max_zoom), tile_lon(min_x, max_zoom), tile_lat(min_y, max_zoom), tile_lon(max_x, max_zoom))
    south, west, north, east = requested_bounds

    print(f"Downloading OSM road geometry for {south:.6f},{west:.6f},{north:.6f},{east:.6f}")
    if not 1 <= args.query_grid <= 8:
        parser.error("query grid must be between 1 and 8")
    root = pathlib.Path(args.root)
    data = fetch_osm_grid(
        args.endpoint, south, west, north, east, args.query_grid,
        root / ".overpass-cache", args.allow_partial)
    source_waterways = []
    source_roads = []
    for element in data.get("elements", []):
        geometry = element.get("geometry", [])
        if len(geometry) < 2:
            continue
        tags = element.get("tags", {})
        (source_waterways if "waterway" in tags else source_roads).append((geometry, tags))

    total_tiles = 0
    for zoom in zooms:
        west_x, north_y = tile_xy(north, west, zoom)
        east_x, south_y = tile_xy(south, east, zoom)
        min_x, min_y = math.floor(west_x), math.floor(north_y)
        max_x, max_y = math.floor(east_x), math.floor(south_y)
        count_x, count_y = max_x - min_x + 1, max_y - min_y + 1
        width, height = count_x * TILE_SIZE, count_y * TILE_SIZE
        image = bytearray((9, 14, 15)) * (width * height)
        def projected(items):
            output = []
            for geometry, tags in items:
                points = []
                for point in geometry:
                    px, py = tile_xy(point["lat"], point["lon"], zoom)
                    points.append((round((px - min_x) * TILE_SIZE), round((py - min_y) * TILE_SIZE)))
                output.append((points, tags))
            return output
        for points, _ in projected(source_waterways):
            for start, end in zip(points, points[1:]):
                line(image, width, height, *start, *end, 3, (13, 31, 35))
        roads = projected(source_roads)
        roads.sort(key=lambda item: road_style(item[1])[0])
        for points, tags in roads:
            stroke, road_color = road_style(tags)
            for start, end in zip(points, points[1:]):
                line(image, width, height, *start, *end, stroke + 2, (6, 10, 11))
                line(image, width, height, *start, *end, stroke, road_color)
        row_bytes = width * 3
        tile_bytes = TILE_SIZE * 3
        for tile_y_offset in range(count_y):
            for tile_x_offset in range(count_x):
                tile = bytearray()
                for py in range(TILE_SIZE):
                    source = ((tile_y_offset * TILE_SIZE + py) * row_bytes) + tile_x_offset * tile_bytes
                    tile.extend(image[source:source + tile_bytes])
                x, y = min_x + tile_x_offset, min_y + tile_y_offset
                write_png(root / str(zoom) / str(x) / f"{y}.png", TILE_SIZE, TILE_SIZE, tile)
        total_tiles += count_x * count_y
        print(f"Rendered zoom {zoom}: {count_x}x{count_y} tiles")

    (root / "ATTRIBUTION.txt").write_text(
        "Map data (c) OpenStreetMap contributors, https://www.openstreetmap.org/copyright\n",
        encoding="utf-8",
    )
    attribution = "Map data (c) OpenStreetMap contributors, https://www.openstreetmap.org/copyright"
    if args.package:
        write_glidemap(pathlib.Path(args.package), root, args.name, attribution, requested_bounds, zooms)
    print(f"Rendered {len(source_roads)} road ways and {len(source_waterways)} waterways into {total_tiles} tiles")


if __name__ == "__main__":
    main()
