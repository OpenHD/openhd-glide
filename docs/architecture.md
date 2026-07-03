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


# OpenHD-Glide Architecture

OpenHD-Glide is structured as a controller plus three workload binaries. The controller is responsible for system discovery, process startup, lifecycle supervision, CPU affinity assignment, and the initial display-plane allocation strategy.

## Processes

- `openhd-glide`: controller process. Performs DRM/KMS and CPU discovery first, then starts and initializes the workload binaries.
- `glide-view`: video ingest/decode worker. Receives video over UDP and must not become DRM/KMS master.
- `glide-flow`: OSD worker. Renders telemetry and overlay content with OpenGL ES.
- `glide-ui`: UI worker. Hosts the interactive UI layer with LVGL; the current development backend is LVGL's SDL window driver.

The current repository builds all four executables. The intended deployed system still has three rendering/UI workload binaries; `openhd-glide` is the supervising controller.

## Startup Sequence

1. Controller starts.
2. Controller probes DRM/KMS devices and reports all visible planes, positions, sizes, supported formats, and possible CRTCs.
3. Controller probes CPU topology and reports logical cores, online state, package/core IDs, and current/max frequencies where the platform exposes them.
4. Controller selects core assignments for `glide-view`, `glide-flow`, and `glide-ui`.
5. Controller starts the three workers and passes their initial configuration.
6. Workers report readiness.
7. Controller begins health monitoring and restart policy handling.

## Display Model

The first hardware probe uses DRM/KMS planes because the target pipeline needs deterministic composition:

- video can occupy a dedicated plane where supported;
- OSD can use an overlay plane or an OpenGL ES surface;
- UI can use a separate plane or be composited above OSD, depending on hardware limits.

The probe currently reports all planes exposed through `libdrm` universal planes. Plane claiming and atomic modesetting are the next step.

## GlideView Video

`glide-view --udp-video` receives RTP video on UDP port 5600 by default and decodes with GStreamer into `appsink`. It supports H.264, H.265, and MJPEG via `--udp-codec h264|h265|mjpeg`. For desktop and WSL debugging, `--display` replaces the final `appsink` with `videoconvert ! autovideosink` so the same worker can show a visible video window. It intentionally does not use `kmssink`, because `kmssink` would make the View process compete for KMS master. The intended device pipeline is:

- `udpsrc`
- `rtph264depay`
- `h264parse`
- hardware-oriented `v4l2h264dec`, `v4l2slh264dec`, or board-specific decoders such as `omxh264dec` on Allwinner BSP images
- `appsink`

The queues are constrained to one buffer and leaky mode to keep latency low. `glide-view` logs decoded frame rate, first-sample caps, and first-sample memory type in appsink mode, and frame rate in display mode. On Allwinner BSP images, the OMX path may require root privileges because `/dev/cedar_dev`, `/dev/cedar_dev_ve2`, and DMA heap nodes are often root-only. The next boundary is Unix-socket fd passing: View should export decoded DMABUFs to `openhd-glide`, and `openhd-glide` should be the only process that imports buffers and programs KMS planes.

The current composition limitation is that `glide-flow --kms` still owns fullscreen scanout. For real video-under-OSD composition, `openhd-glide` needs to own KMS, put View's decoded DMABUF on a video plane, and put Flow's render target on an alpha-capable overlay plane above it.

`openhd-glide --kms-video-preview` is the first visible controller-owned video bring-up path. It decodes RTP/H.264 in the controller, requests `video/x-raw(memory:DMABuf),format=DMA_DRM`, imports the decoded DMABUF FDs into DRM framebuffers, and places them on a KMS plane. It currently creates a black primary framebuffer only to keep the CRTC active. Flow is rendered on an ARGB overlay plane above the video plane. With `--ui-overlay`, the controller also places a left-side ARGB UI overlay plane. `glide-ui --buffer` renders LVGL into a shared ARGB file, while the controller copies that buffer into the KMS UI plane on a separate 30 fps thread so UI work stays out of the video presentation loop. The next step is to move the video DMABUF handoff back across the `glide-view` Unix socket.

## GlideFlow OSD

The first OSD elements are an FPS counter positioned in the bottom-left corner, a compact performance horizon centered on the render surface, left/right/bottom link overview panels, a left-side speed widget, and a right-side altitude widget. The current implementation includes:

- frame-rate measurement over a one-second window;
- deterministic top-right text layout with a fixed bitmap font;
- an OpenGL ES 2.0 text renderer that prefers FreeType antialiased TrueType glyphs and falls back to lightweight vector strokes when FreeType is unavailable.
- a port of QOpenHD's `PerformanceHorizonWidget2`: split horizon bars shifted by pitch, rotated by roll, with a fixed center circle/crossbar.
- a port of QOpenHD's left and right link overview widgets: downlink RSSI/temp/MCS/SNR blocks and uplink/frequency/bitrate/record/RC blocks. Flow only displays this bar; visibility is controlled by the UI menu through controller IPC.
- a port of QOpenHD's bottom link overview widget: full-width bottom panel with center flight mode/time notch and telemetry slots.
- a port of QOpenHD's speed widget: left-side pointer box plus vertical ladder ticks/labels.
- a port of QOpenHD's altitude widget: right-side pointer box plus vertical ladder ticks/labels.
- simulated attitude data for preview until telemetry IPC exists.
- simulated link data for preview until telemetry IPC exists.
- simulated speed data for preview until telemetry IPC exists.
- simulated altitude data for preview until telemetry IPC exists.

The next rendering step is to add the DRM/EGL surface setup so `glide-flow` can own a real overlay plane or render target instead of only exercising the layout path from the command-line binary.

## Development Preview

`glide-flow` uses SDL2/OpenGL ES preview and `glide-ui` uses LVGL's SDL preview backend for development on WSL or desktop systems without DRM/KMS. These backends are intentionally convenience layers. They are slower and less deterministic than the target DRM/KMS layout, but they allow OSD and UI work to be developed visually before the production display path is available.

Preview commands:

- `glide-flow --preview --width 1280 --height 720`
- `glide-ui --preview --width 1280 --height 220`
- `openhd-glide --preview-stack`

`openhd-glide --preview-stack` is the current WSL controller path. It performs the same discovery step,
then starts `glide-flow` and `glide-ui` as child processes with explicit SDL window positions. The default
layout places a narrow UI sidebar window over the left edge of the Flow window, using borderless windows.
This avoids the unreliable transparent-window behavior in WSL while still exercising UI-to-Flow control.
Use `--preview-width`, `--flow-height`, `--ui-width`, `--ui-height`, `--preview-x`, `--preview-y`, and
`--ui-opacity` to tune the layout. `--vertical-stack` keeps the older UI-above-Flow layout for comparison.

The WSL preview stack starts a controller-owned Unix domain socket at `/tmp/openhd-glide.sock` by default.
`glide-view`, `glide-flow`, and `glide-ui` register with `hello <worker>` messages and can exchange simple
line-based status/control messages with the controller. `glide-ui` currently uses this path for an FPS toggle
inside the preview sidebar. The controller receives `set ...` menu updates and broadcasts `state ...` lines, which
`glide-flow` consumes before drawing display-only OSD elements. The socket path can be changed with `--ipc-socket`.

The LVGL UI is intentionally backend-isolated: the current SDL display driver is for WSL/Windows-style iteration. The target backend should render the same LVGL tree into shared buffers or a plane-owned buffer path supplied by `openhd-glide`, without letting `glide-ui` become DRM/KMS master.

The UI and Flow consume a small controller-broadcast `mav ...` IPC vocabulary. The controller owns the MAVLink UDP
bridge by default on `0.0.0.0:14550` (`--mavlink-udp-port`, `--no-mavlink`) and keeps rendering processes decoupled
from transport details. It decodes common flight messages into snapshots such as `mav attitude ...`, `mav position ...`,
`mav speed ...`, `mav battery ...`, `mav rc ...`, plus OpenHD/QOpenHD-style `PARAM_VALUE` and `PARAM_EXT_VALUE` settings
into `mav param ...`. UI actions emit `mav set ...` or `mav command ...`; the controller translates those back into
MAVLink parameter writes or command-long packets once it has seen a UDP peer.

## Device KMS Mode

`openhd-glide --kms-stack` is the target-device entry point. It starts the same Unix-socket controller and launches:

- `glide-view --stay-alive`
- `glide-flow --kms --stay-alive`
- `glide-ui --headless`

This mode is intentionally separate from the SDL preview path. It validates device DRM/KMS plane discovery, process startup, CPU assignment, and IPC without creating SDL windows. `glide-flow --kms` currently creates a fullscreen KMS/GBM/EGL scanout surface on the active connector and renders the current OSD through OpenGL ES. That is temporary: the target production model is single KMS ownership in `openhd-glide`, with Flow and View producing buffers rather than taking KMS master. UI remains headless in this path until LVGL can render into a shared-buffer or plane-backed target owned by the controller.

## CPU Model

Each worker is expected to run on a different CPU core where the target hardware makes that useful. The controller uses discovered topology and frequency data to prefer faster online cores for video and rendering workloads.

Priority order is:

1. `glide-view`
2. `glide-flow`
3. `glide-ui`

`glide-view` always gets its own core and prefers the fastest online core. `glide-flow` gets the next fastest core when possible. `glide-ui` is lowest priority, but still gets the next fastest isolated core when enough cores are available. If only two usable cores are available, `glide-ui` shares with `glide-flow` so the video renderer remains isolated.

If an existing OpenHD process is detected, the controller avoids `cpu0` for worker assignment so the base OpenHD process can keep that core.

The assignment policy is implemented, but process launch and actual OS affinity application are still next.

## IPC Direction

The controller-to-worker API starts as a small Unix socket protocol and is not locked yet. The planned boundary is:

- startup configuration: plane IDs, UDP ports, render dimensions, CPU affinity, and feature flags;
- runtime commands: start, stop, pause, reconfigure, set overlay state, set UI state;
- health: ready, heartbeat, frame/render stats, and fatal errors.

Unix domain sockets are the default Linux target because they are simple, local, and easy to supervise.
The current prototype is newline-delimited text:

- `hello <worker>`
- `status <worker> <details>`
- `heartbeat <worker>`
- `get fps`
- `set fps 0|1`
- `state fps 0|1`
- `get topbar`
- `set topbar 0|1`
- `state topbar 0|1`
