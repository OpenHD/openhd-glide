<!--
OpenHD

Licensed under the GNU General Public License (GPL) Version 3.

This software is provided "as-is," without warranty of any kind, express or
implied, including but not limited to the warranties of merchantability,
fitness for a particular purpose, and non-infringement. For details, see the
full license in the LICENSE file provided with this source code.

Non-Military Use Only:
This software and its associated components are explicitly intended for
civilian and non-military purposes. Use in any military or defense
applications is strictly prohibited unless explicitly and individually
licensed otherwise by the OpenHD Team.

Contributors:
A full list of contributors can be found at the OpenHD GitHub repository:
https://github.com/OpenHD

© OpenHD, All Rights Reserved.
-->


# openhd-glide

OpenHD-Glide is a three-worker rendering stack:

- `glide-view`: UDP video rendering worker
- `glide-flow`: OpenGL ES OSD worker
- `glide-ui`: LVGL-based UI worker

The first implemented controller step probes DRM/KMS planes and CPU topology.
It also computes the first worker CPU assignment plan: `glide-view` gets the strongest isolated core, `glide-flow` gets the next priority, and `glide-ui` is lowest priority.

## Build

```sh
cmake -S . -B build
cmake --build build
```

For a target device DRM/KMS build:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config libdrm-dev libgbm-dev libgles2-mesa-dev libegl1-mesa-dev libfreetype-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
cmake --preset device-kms
cmake --build --preset device-kms -j$(nproc)
```

Or use the platform-aware helper. It detects Raspberry Pi, Rockchip, Allwinner, or generic Linux,
warns about missing DRM/GBM/GStreamer pieces, and can install the matching apt packages:

```sh
scripts/build-auto.sh
scripts/build-auto.sh --deps
scripts/build-auto.sh --deps-only
```

By default the helper requires the KMS/GBM and GStreamer development packages because the target
video path depends on them. Use `--allow-missing` only for a degraded development build.
Set `GLIDE_PLATFORM=raspberrypi`, `rockchip`, `allwinner`, or `generic` to override detection.

If the device CMake is too old for presets, use the equivalent direct configure:

```sh
cmake -S . -B build-kms -DCMAKE_BUILD_TYPE=Release -DOPENHD_GLIDE_DEVICE_KMS=ON
cmake --build build-kms -j$(nproc)
```

Or use the device helper script, which configures, builds, and installs the binaries:

```sh
scripts/compile-install.sh
```

To install the common build/runtime dependencies first:

```sh
scripts/compile-install.sh --deps
```

On Radxa Zero 3W / RK3566 Bookworm images that use `bookworm-backports` for DRM or Mesa runtime
packages, apt may report exact-version conflicts such as `libdrm-dev` requiring Debian's
`libdrm2 (= 2.4.114-1+b1)` while `libdrm2 2.4.123-1~bpo12+1` is selected. Install the matching
graphics development packages from backports:

```sh
sudo apt install -y -t bookworm-backports libdrm2 libdrm-radeon1 libdrm-nouveau2 libdrm-amdgpu1 libgbm1 libegl-mesa0 libglapi-mesa libdrm-dev libgbm-dev libgles2-mesa-dev libegl1-mesa-dev
```

`scripts/compile-install.sh --deps` detects this case and applies the same backports target
automatically. Set `GLIDE_APT_BACKPORTS_TARGET` if the image uses a differently named backports suite.

The package workflow applies the same split install inside OpenHD-ChrootCompiler's chroot build script.

### Rockchip RK3566/RK3568 Dependencies

Radxa/Rockchip Debian images may ship GStreamer runtime packages from the Radxa RK3568 repository
(`1.22.9` on the tested Rock 3A image) while the matching development packages are hidden if the
Radxa apt sources are commented out. In that state, `scripts/compile-install.sh --deps` can fail with
`libgstreamer1.0-dev` or `libgstreamer-plugins-base1.0-dev` dependency conflicts, or CMake can disable
GStreamer support.

Enable the Radxa sources before installing dependencies:

```sh
sudo sed -i 's/^#deb /deb /' /etc/apt/sources.list.d/70-radxa.list /etc/apt/sources.list.d/80-radxa-rk3568.list
sudo apt update
```

Then install the normal dependencies plus the Rockchip MPP/RGA development packages:

```sh
sudo apt install -y build-essential cmake pkg-config libdrm-dev libgbm-dev libgles2-mesa-dev libegl1-mesa-dev libfreetype-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev librockchip-mpp-dev librga-dev gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav gstreamer1.0-rockchip1
```

`librga-dev` is required because Radxa's `gstreamer-video-1.0.pc` depends on `librga.pc`. Without it,
`pkg-config --exists gstreamer-video-1.0` fails and OpenHD-Glide builds without GStreamer support.
`librockchip-mpp-dev` is required for the native RKMPP decoder path; without it, the Rockchip scripts
fall back to the generic GStreamer decoder build behavior.

If the image has the obsolete `gstreamer1.0-plugins-rtp` package installed, remove it. It is a `1.14.x`
plugin that can conflict with the `1.22.x` RTP elements from `gstreamer1.0-plugins-good`:

```sh
sudo apt remove -y gstreamer1.0-plugins-rtp
rm -f ~/.cache/gstreamer-1.0/registry.*
```

On RK3566/RK3568, use the native RKMPP scripts for the low-latency path. They use GStreamer only for
RTP depay/parse and feed byte-stream NAL data directly into Rockchip MPP with immediate output and fast-play
decoder controls, following the approach used by PixelPilot/FPVue. The older GStreamer `mppvideodec`
path is still available as a portable fallback, but it can output only about half-rate on some RK3566
streams. If MPP only works under `sudo`, fix permissions for the Rockchip MPP/RGA/video device nodes or
run the current device smoke tests with `sudo`.

RK3566/RK3568 VOP2 plane layout can expose only one linear ARGB overlay plane. In that case the
controller keeps video on the NV12 plane and composites the LVGL UI buffer into the Flow/OSD GL surface
instead of requiring a second ARGB KMS overlay plane. Boards that expose enough linear ARGB overlay
planes still use a separate UI overlay plane normally.

The helper accepts environment overrides:

```sh
GLIDE_BUILD_DIR=build-kms GLIDE_INSTALL_PREFIX=/usr/local GLIDE_JOBS=8 scripts/compile-install.sh
```

### Orange Pi Zero 3W Bookworm

For Orange Pi Zero 3W Bookworm images, use the board helper to install the normal target-device dependencies,
build the DRM/KMS binary, and start the Allwinner/Cedar KMS preview path:

```sh
examples/buildrun-orangepi-zero3w-bookworm.sh 5600 h264
```

The helper defaults to `GLIDE_ORANGEPI_MODE=all`, which runs dependency install, build, and run. Use
`GLIDE_ORANGEPI_MODE=deps`, `build`, or `run` to execute only one step. It also defaults to
`GLIDE_ORANGEPI_BACKEND=cedar`; set `GLIDE_ORANGEPI_BACKEND=gstreamer` to use the portable GStreamer KMS path
instead. Display sizing follows the normal KMS script overrides:

```sh
GLIDE_ORANGEPI_MODE=run GLIDE_WIDTH=auto GLIDE_HEIGHT=auto GLIDE_DISPLAY_HZ=0 examples/buildrun-orangepi-zero3w-bookworm.sh
```

On Linux, install `libdrm` development headers to enable real DRM plane discovery. Without `libdrm`, or on non-Linux platforms, the probe builds and reports that DRM discovery is unavailable.

Install OpenGL ES 2.0 development files to enable the first `glide-flow` renderer path. Until the DRM/EGL surface is added, `glide-flow` runs the FPS layout path and prints where the top-right counter will render. Passing `--render-gles` submits the FPS glyphs through GLES and requires a current EGL/GLES context.
Install SDL2 development files to enable desktop preview windows for WSL/Windows development. The preview path is intentionally not the production display path; it exists so `glide-flow` and `glide-ui` can be developed without DRM/KMS.
Install FreeType development files to enable antialiased TrueType OSD text. Without FreeType, `glide-flow` falls back to the simpler stroke font.
`glide-ui` fetches LVGL v9.2.2 at configure time and uses LVGL's SDL backend for the development preview.

## Run

```sh
./build/openhd-glide
```

Target device KMS stack test:

```sh
sudo ./build-kms/openhd-glide --kms-stack --preview-width 1920 --flow-height 1080 --display-refresh-hz 120
```

`--kmd-stack` is accepted as an alias for `--kms-stack`. The current device path starts the three workers and IPC,
uses DRM/KMS discovery from the controller, runs `glide-flow` through GBM/EGL directly on the active KMS connector,
and runs `glide-ui` headless until the LVGL shared-buffer/plane backend exists.

`glide-view` listens for UDP RTP video on port 5600 by default and decodes through GStreamer into `appsink`.
It supports H.264, H.265, and MJPEG with `--udp-codec h264|h265|mjpeg`; H.264 remains the default for
existing senders.
For desktop and WSL debugging, add `--display` to present decoded frames through GStreamer's `autovideosink`.
It intentionally does not use `kmssink`, because `kmssink` would compete for DRM/KMS master. The next production step is
Unix-socket FD passing so `glide-view` can hand decoded DMABUFs to `openhd-glide`, while only `openhd-glide` imports
buffers and programs KMS planes.

```sh
sudo ./build-kms/openhd-glide --kms-stack --view-udp-port 5600 --preview-width 1920 --flow-height 1080 --display-refresh-hz 120
```

Standalone View decode test:

```sh
sudo ./build-kms/glide-view --udp-video --udp-port 5600
```

This command does not display video. It should log `first decoded sample ...` and `decoded fps=...` once RTP/H.264
frames arrive. If those lines do not appear, the sender is not reaching the receiver or the stream caps do not match.
To show a desktop debug window instead, use:

```sh
./build-kms/glide-view --udp-video --udp-port 5600 --udp-codec mjpeg --display
```

On Rockchip RK3566/RK3568 images, the hardware path should use `mppvideodec`, and running under `sudo` may be required
until the MPP/RGA/video device nodes have suitable permissions. On Allwinner BSP images, the hardware path may use
`omxh264dec` instead of `v4l2h264dec`, and running under `sudo` is often required because the cedar and DMA heap device
nodes are root-only by default.

Temporary controller-owned KMS video preview:

```sh
sudo ./build-kms/openhd-glide --kms-video-preview --gstreamer-video --no-flow --view-udp-port 5600 --preview-width 1920 --flow-height 1080 --display-refresh-hz 120
```

This displays the UDP video without `kmssink` by decoding in `openhd-glide`, importing the decoded FD into DRM,
and scanning it out on a KMS video plane. It still uses a black primary framebuffer only to keep the CRTC active.
Flow is rendered on an ARGB overlay plane. With `--ui-overlay`, the controller also places a left-side ARGB UI
overlay plane or composites the LVGL buffer into the Flow/OSD plane when RK3566 exposes only one usable ARGB plane.
Native Cedar remains available only through the explicit `--native-cedar-video` flag.
On Raspberry Pi, start with the GStreamer path and the real KMS driver (`vc4-kms-v3d`). Raspberry Pi 5
images should not be treated as H.264 hardware decode/encode targets; the current practical bring-up path is
RTP/MJPEG in video-only KMS mode, where Glide decodes JPEG in software, copies BGRx into a DRM dumb buffer,
and scans it out on a KMS plane. The same CPU-backed scanout path also works with Flow/UI overlays. H.265 can
still use V4L2/DMABUF when the sender provides H.265 and the OS exposes the HEVC decoder.

Example run scripts cover the current device modes. Each script takes the UDP video port as its first optional
argument, defaulting to `5600`; GStreamer/view scripts usually default to H.264 and take `h264`, `h265`, or
`mjpeg` as the second optional argument where supported. The RKMPP scripts use native MPP decode for H.264, H.265,
and MJPEG. The Raspberry Pi video-only script defaults to MJPEG.
Set `GLIDE_WIDTH` and `GLIDE_HEIGHT` to override the default `1920x1080`.
Device KMS scripts default to `GLIDE_DISPLAY_HZ=0`, which auto-selects the highest refresh mode exposed by the connected display. Set `GLIDE_DISPLAY_HZ` to a non-zero value to request a specific refresh rate.

```sh
# Native Rockchip MPP decode, fastest RK3566/RK3568 video-only path.
examples/run-kms-video-rkmpp-video-only.sh 5600 h264

# Native Rockchip MPP decode, KMS video plane plus Flow and LVGL UI.
examples/run-kms-video-rkmpp-flow-ui.sh 5600 h264

# GStreamer hardware decode, KMS video plane plus Flow overlay at full video rate.
examples/run-kms-video-gstreamer-flow.sh 5600 h264

# GStreamer hardware decode, KMS video plane plus Flow and LVGL UI overlay planes.
examples/run-kms-video-gstreamer-flow-ui.sh 5600 h264

# GStreamer hardware decode, KMS video plane plus Flow overlay capped to 30 fps.
examples/run-kms-video-gstreamer-flow-30fps.sh 5600 h264

# GStreamer decode, fastest video-only legacy KMS plane path.
examples/run-kms-video-gstreamer-video-only.sh 5600 h264
examples/run-kms-video-gstreamer-video-only.sh 5600 mjpeg

# Raspberry Pi KMS video-only bring-up path. Defaults to RTP/MJPEG.
examples/run-kms-video-rpi-video-only.sh 5600
examples/run-kms-video-rpi-video-only.sh 5600 mjpeg

# Native Cedar RTP/H.264 decode debugging path, KMS video plane plus Flow overlay.
examples/run-kms-video-cedar-flow.sh 5600

# Native Cedar RTP/H.264 decode debugging path, fastest video-only legacy KMS plane path.
examples/run-kms-video-cedar-video-only.sh 5600

# Standalone glide-view decode-only test.
examples/run-glide-view-decode-only.sh 5600 h264
examples/run-glide-view-decode-only.sh 5600 mjpeg

# WSL/desktop visible glide-view test.
GLIDE_VIEW_DISPLAY=1 examples/run-wsl-video-decode.sh 5600 mjpeg

# Multi-process KMS stack smoke test.
examples/run-kms-stack.sh 5600 h264
```

Installed helper scripts are placed in `${CMAKE_INSTALL_PREFIX}/share/openhd-glide/examples`.

### Autostart Service

Debian packages install and enable `openhd-glide.service` for boot autostart. The service runs
`openhd-glide --kms-video-preview`, starts `glide-ui --buffer` for the LVGL UI overlay, and uses
`/etc/default/openhd-glide` for target-specific settings.

The default service configuration listens for RTP/H.264 on UDP port `5600`, uses the portable
GStreamer decoder path, and starts a `1920x1080` Flow/UI KMS stack at `120 Hz`. For Rockchip
RK3566/RK3568 packages built with native MPP support, set:

```sh
sudo sed -i 's/^GLIDE_VIDEO_BACKEND=.*/GLIDE_VIDEO_BACKEND=rkmpp/' /etc/default/openhd-glide
```

For Allwinner/Cedar tests, set `GLIDE_VIDEO_BACKEND=cedar` instead. Other common overrides are
`GLIDE_WIDTH`, `GLIDE_HEIGHT`, `GLIDE_DISPLAY_HZ`, `GLIDE_FLOW_FPS`, `GLIDE_VIEW_PORT`, and
`GLIDE_VIEW_CODEC`.

The package enables the service but does not force a first start during installation unless
`GLIDE_START_ON_INSTALL=1` is already set in `/etc/default/openhd-glide`. Start it manually for an
immediate test:

```sh
sudo systemctl start openhd-glide
sudo journalctl -u openhd-glide -f
```

Send a test pattern from another machine with GStreamer:

```sh
examples/stream-videotestsrc-to-glide-view.sh <target-ip> 5600
examples/stream-mjpeg-videotestsrc-to-glide.sh <target-ip> 5600
```

Use the Pi's actual network IP as `<target-ip>`. For a sender running on the same Pi, use `127.0.0.1`.
If both devices are running Glide on the same Ethernet subnet, scan for the receiver instead of typing
an address:

```sh
./build-kms/openhd-glide --ethernet-discover --view-udp-port 5600
examples/stream-videotestsrc-to-glide-auto.sh 5600
```

For a direct cable without a router, put one device in the ground preset and the other in the air preset,
then run the same scan/stream command. Replace `eth0` with the actual Ethernet interface name if needed:

```sh
sudo ./build-kms/openhd-glide --ethernet-p2p ground eth0
sudo ./build-kms/openhd-glide --ethernet-p2p air eth0
```

On a Raspberry Pi 5 camera sender, use `rpicam-vid`/`libcamera-vid` directly for MJPEG capture,
with GStreamer only packetizing the camera stream into RTP:

```sh
examples/stream-rpicam-to-glide.sh <target-ip> 5600
examples/stream-rpicam-to-glide.sh auto 5600
```

When run from an interactive terminal, the script prints `rpicam-vid --list-cameras` output, lets you
choose a sensor mode, then prompts for FPS and MJPEG quality before starting the stream. Useful overrides are
`GLIDE_CAMERA_INTERACTIVE=0`, `GLIDE_CAMERA_INDEX`, `GLIDE_CAMERA_WIDTH`, `GLIDE_CAMERA_HEIGHT`,
`GLIDE_CAMERA_FPS`, `GLIDE_MJPEG_QUALITY`, and `GLIDE_RPICAM_EXTRA_ARGS`.

The Linux H.264 videotest sender, `examples/stream-videotestsrc-to-glide-view.sh`, requires a hardware encoder
such as `v4l2h264enc` by default. Set `GLIDE_ALLOW_SOFTWARE_ENCODER=1` only when you intentionally want a
non-performance `x264enc` fallback. That script performs a small encoder self-test first. If `v4l2h264enc` fails with `bcm2835-codec ... ret -3` in
`dmesg`, the Raspberry Pi encoder driver is failing independently of Glide; use another sender with hardware H.264 or
fix the Pi encoder stack before using it for performance measurements.
For Raspberry Pi 5 MJPEG test-pattern bring-up without a camera, use `examples/stream-mjpeg-videotestsrc-to-glide.sh`;
it uses `jpegenc` and `rtpjpegpay` instead of an H.264 encoder.

To avoid sender-side encoding entirely, stream a downloaded H.264 MP4:

```sh
examples/stream-h264-file-to-glide-view.sh <target-ip> 5600
examples/stream-blurbusters-1080p120-to-glide-view.sh <target-ip> 5600 h264
```

This downloads a pre-encoded Big Buck Bunny H.264 1080p60 30-second MP4 by default, then streams only
`filesrc ! qtdemux ! h264parse ! rtph264pay ! udpsink`. There is no `videotestsrc` and no encoder. To use a 720p60
H.264 MP4 instead, pass the local file path as the third argument or set `GLIDE_TEST_VIDEO_URL` before running the
script.

For the 1080p120 Blurbusters sender, pass `h264` or `h265` after the port. H.265 mode expects a pre-encoded H.265 MP4
path or URL; it does not transcode the H.264 Blurbusters download.

On Windows, use:

```bat
examples\stream-videotestsrc-to-glide-view.bat <target-ip> 5600
```

The Windows sender requires a hardware encoder (`nvh264enc`, `qsvh264enc`, `d3d11h264enc`, `amfh264enc`) by default.
Set `GLIDE_ALLOW_SOFTWARE_ENCODER=1` only when you intentionally want a non-performance `x264enc` fallback.

Windows can also stream the downloaded H.264 file without encoding:

```bat
examples\stream-h264-file-to-glide-view.bat <target-ip> 5600
```

```sh
./build/glide-flow --width 1920 --height 1080
```

```sh
./build/glide-flow --preview --width 1280 --height 720
./build/glide-ui --preview --width 1280 --height 220
./build/openhd-glide --preview-stack
```

The WSL helper scripts can also run each worker independently:

```sh
examples/run-wsl-video-decode.sh 5600 h264
examples/run-wsl-flow-preview.sh
examples/run-wsl-ui-preview.sh
examples/run-wsl-minimap-preview.sh
```

For WSL development, `openhd-glide --preview-stack` starts `glide-view`, `glide-flow`, and then places a
`glide-ui` LVGL/SDL preview over the left side. In WSL, the UI preview is kept as a sidebar surface to avoid
covering the Flow preview. The sidebar `OSD` panel has toggles for the top link bar, FPS, coordinates, and the
Flow speed/altitude ladder versus compact text mode. Flow renders OSD elements as display-only output; widget
visibility and settings live in the UI menu and controller IPC. The custom wind indicator is rendered by Flow inside
the performance horizon.
The UI preview also owns the LVGL minimap layer. Press `M` to cycle menu -> minimap -> hidden, or press `N` to
cycle minimap -> menu -> hidden. When the minimap is visible, `+` zooms in and `-` zooms out. The two views share one
layer and are never displayed at the same time.
The layout can be adjusted:

```sh
./build/openhd-glide --preview-stack --preview-width 1280 --flow-height 720 --ui-width 760 --preview-x 60 --preview-y 40 --ui-opacity 1.0
```

The default development IPC socket is `/tmp/openhd-glide.sock`; override it with `--ipc-socket <path>`.

### LVGL minimap demo

`glide-minimap-demo` is a standalone LVGL/SDL preview for the reusable minimap widget in
`src/glide_ui/minimap_widget.*`. It is intentionally offline-only: it reads local raster PNG tiles from
`assets/maps/{z}/{x}/{y}.png`, uses Web Mercator tile math, caches decoded tiles, and draws a small 3x3 or 5x5
tile grid with the aircraft kept centered. The demo simulates aircraft motion and heading, and renders the home
marker, route trail, aircraft icon, and border into a raster LVGL canvas. The map is heading-up: the card aligns to
the current flight direction while the outer compass ring shows N/S/E/W and heading ticks.
If a requested local tile is missing, the widget creates a deterministic free offline test tile at that path before
rendering it, so `glide-minimap-demo --preview` works from a clean directory without paid APIs or tile downloads.

Generate the tiny fake local tile set and run it on WSL/Linux:

```sh
python3 scripts/generate_fake_minimap_tiles.py --root assets/maps --zoom 15
cmake -S . -B build-wsl
cmake --build build-wsl --target glide-minimap-demo -j"$(nproc)"
./build-wsl/glide-minimap-demo --preview --width 420 --height 420 --tile-root assets/maps --zoom 15
```

For a Linux buffer backend smoke test, use:

```sh
./build-wsl/glide-minimap-demo --buffer --width 420 --height 420 --tile-root assets/maps --zoom 15 --buffer-path /tmp/openhd-glide-minimap.argb
```

The helper script does the same build and tile generation steps:

```sh
examples/run-wsl-minimap-preview.sh
```

`examples/run-wsl-ui-preview.sh` starts that full preview stack by default, generates the fake minimap tiles, and
exports `GLIDE_MINIMAP_TILE_ROOT`, so the integrated UI preview can show the round minimap with `M` or `N`
immediately. Use `GLIDE_UI_ONLY=1 examples/run-wsl-ui-preview.sh` for the old standalone UI window. Use `+` and `-`
to zoom the map.

To install the WSL build into `~/.local`:

```sh
cmake -S . -B build-wsl -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-wsl --target glide-ui glide-minimap-demo -j"$(nproc)"
cmake --install build-wsl
python3 "$HOME/.local/share/openhd-glide/scripts/generate_fake_minimap_tiles.py" --root "$HOME/.local/share/openhd-glide/assets/maps" --zoom 15
GLIDE_MINIMAP_TILE_ROOT="$HOME/.local/share/openhd-glide/assets/maps" "$HOME/.local/bin/glide-ui" --preview --width 760 --height 720
```

No paid map APIs are used. The fake generator creates only a small deterministic terrain tile fixture around the demo
home position: elevation shading, forest patches, lowlands/water, and contour/ridge lines. Real deployments should
place their own offline terrain tiles under the same `assets/maps/{z}/{x}/{y}.png` layout. Missing real tiles are
replaced by generated offline terrain test tiles, never fetched from the network.

Terminal and MAVLink-state IPC helpers:

```sh
./build-kms/glide-send ui key down
./build-kms/glide-send ui key enter
./build-kms/glide-send ui key right
./build-kms/glide-send ui key left
./build-kms/glide-send mav alive air 1
./build-kms/glide-send mav alive ground 1
./build-kms/glide-send mav link 5745 20 2 1200
./build-kms/glide-send mav scan 42
./build-kms/glide-send mav message "FC heartbeat received"
```

`mav ...` lines are the internal bridge contract: the controller publishes decoded MAVLink state to workers, and UI
actions emit `mav set ...` / `mav command ...` lines that the controller can translate into OpenHD parameter writes and
commands.

`openhd-glide --preview-stack` and `--kms-stack` now start a controller-owned MAVLink UDP bridge on `0.0.0.0:14550`
by default. Override it with `--mavlink-udp-port <port>` or disable it with `--no-mavlink`. The bridge decodes common
MAVLink flight data (`HEARTBEAT`, `ATTITUDE`, `GPS_RAW_INT`, `GLOBAL_POSITION_INT`, `VFR_HUD`, `SYS_STATUS`,
`RC_CHANNELS`, `STATUSTEXT`) and OpenHD/QOpenHD-style parameter/status traffic (`PARAM_VALUE`, `PARAM_EXT_VALUE`,
plus custom OpenHD message IDs as status lines), then broadcasts normalized `mav ...` state to Flow and UI. Menu
actions are translated back into MAVLink `PARAM_EXT_SET`, numeric `PARAM_SET`, or command-long packets when a peer has
been seen on the UDP socket.

UI navigation is directional: `up/down` moves through the sidebar or focused setting rows, `right` enters the settings
panel, `left` returns to the sidebar or collapses it, and `enter` activates the focused row.

See [docs/architecture.md](docs/architecture.md) for the initial process architecture.
See [AGENTS.md](AGENTS.md) for project goals and current implementation notes for future AI agents.

## License

OpenHD-Glide is licensed under the GNU General Public License v3.0 only. See [LICENSE](LICENSE).
