# Offline map packages

Glide consumes local `.glidemap` files. It never downloads map data while flying. The WebUI is expected to let the
user select a center/radius, generate or download the resulting package, and place it in the platform's map folder.

Runtime search paths are:

- the bundled `assets/maps/packages` directory;
- `/usr/share/openhd-glide/assets/maps/packages` and `/usr/local/share/openhd-glide/assets/maps/packages`;
- `$XDG_DATA_HOME/openhd-glide/maps` or `~/.local/share/openhd-glide/maps`;
- `GLIDE_MAP_PACKAGE_DIR`, when explicitly configured.

When GPS moves into a package's bounds, Glide selects it automatically. Overlapping packages are ranked by highest
maximum zoom and then smallest covered area.

The integrated map renderer lives in `glide-flow`. It uploads a small atlas of nearby tiles to an OpenGL ES texture;
sub-tile motion and fractional zoom therefore change texture coordinates without regenerating an LVGL framebuffer.
The UI sends visibility, viewport, zoom, position, heading, and home through `ui map state ...` IPC messages.

## GLDMAP1 binary format

All integers are unsigned little-endian. Coordinates are IEEE-754 `float64` degrees. Strings are UTF-8.

| Field | Type |
| --- | --- |
| Magic (`GLDMAP1\0`) | 8 bytes |
| Version (`1`) | `uint32` |
| Tile size (`256`) | `uint16` |
| Minimum / maximum zoom | two `uint8` |
| South, west, north, east | four `float64` |
| Name length / attribution length | two `uint16` |
| Tile count | `uint32` |
| Name / attribution | variable strings |
| Tile index | repeated entry below |
| PNG payloads | indexed data |

Each tile-index entry is `zoom:uint8, x:uint32, y:uint32, absolute_offset:uint64, byte_length:uint32`. Tiles use the
standard XYZ/Web-Mercator addressing scheme and contain 256x256 RGB or RGBA PNG data.

The canonical writer is `scripts/generate_offline_osm_tiles.py`. WebUI code may call that generator initially or
implement the documented container directly.
