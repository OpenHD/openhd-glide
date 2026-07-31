# Isolated Artosyn/P401 RKMPP debugging

This workflow reproduces the Artosyn H.264/RTP input without an Artosyn radio,
KMS/DRM, GlideUI, Flow, MAVLink, or systemd restart loop.

## Build the decoder-only probe

```bash
cmake -S . -B build -DOPENHD_GLIDE_REQUIRE_RKMPP=ON
cmake --build build --target glide-rkmpp-rtp-probe -j2
```

The probe binds UDP/5600, uses Glide's production RTP depacketizer and RKMPP
decoder, exercises the same one-frame scanout ownership used by Glide, and
prints decoder statistics. It never opens DRM/KMS or presents a frame.

## Run an isolated test on the RK3588

```bash
examples/run-rkmpp-isolated-probe.sh 30 5600
```

The script stops and runtime-masks these services for the duration of the test:

- `openhd-glide.service`
- `p401-openhd-bridge.service`
- `p401-direct-daemon.service`

From another machine, replay the captured midstream input:

```bash
python3 tools/artosyn_rtp_fixture.py replay \
  testdata/artosyn/p401-midstream-no-idr.rtp \
  192.168.7.186 \
  --cycles 5
```

Use `--packet-delay-us 500` for the accelerated sequence used by the original
reset reproduction. Sequence numbers and RTP timestamps are normalized across
cycles so the decoder sees one continuous stream.

## Interpret the baseline

The no-IDR fixture should result in received and submitted access units but
zero decoded frames. The process and board must remain alive. That is a safe
baseline: P-frames received without their reference state cannot be decoded.

Any header-injection experiment must be a separate build and should first be
run through this probe. Do not test experimental header injection through the
systemd KMS stack.

To reproduce the known unsafe packaged-header path without Artosyn or KMS:

```bash
build/glide-rkmpp-rtp-probe \
  --duration 30 \
  --debug-inject-x20-header
```

Then replay the same fixture. Access-unit submission previously caused a hard
RK3588 reset in this test. The explicit option now serves as a regression test
for the FPVue-style NAL submission path. Production Glide never force-enables
header injection; it applies the X20 seed only after detecting the exact X20
SPS and PPS.

## FPVue comparison

FPVue_RK3566's `openhd` branch uses the same 692-byte
`other/x20_header.h264` file as Glide. The file is a short decodable seed, not
only SPS/PPS: it contains an IDR and two following P-frames. FPVue also creates
an external 16-buffer DRM output pool when MPP reports an info change, registers
the pool with `MPP_DEC_SET_EXT_BUF_GROUP`, and only then acknowledges
`MPP_DEC_SET_INFO_CHANGE_READY`.

The original Glide decoder acknowledged the info change without provisioning
an output pool, while retaining the currently scanned-out `MppFrame`. Adding a
DMA-backed 16-buffer group allowed all three seed frames to decode, but the
timestamp-aggregated access-unit feed still stalled and reset the board.

The decisive difference is packetization. FPVue requests
`stream-format=byte-stream,alignment=nal` and submits Annex-B NALs to MPP
independently. Glide used to aggregate every NAL sharing an RTP timestamp into
one MPP packet; on the RK3588 vendor MPP build that path wedges after the X20
seed. Glide now submits parameter sets and other non-VCL NALs independently,
while combining only VCL slices that belong to the same picture. The latter is
required for Raspberry Pi x264's `sliced-threads=true` output: treating each
slice as a complete picture displays only a top band and leaves the rest green.
Single-slice X20 pictures remain one-NAL access units.

The isolated regression passed five fixture cycles: 2,000 RTP packets, 656 NAL
submissions, 521 decoded frames, zero submit stalls, and an unchanged boot ID.

For an Artosyn/X20/P401 installation that must join a stream after its startup
parameter sets have already passed, enable the scoped service option:

```bash
GLIDE_RKMPP_X20_FORCE=1
```

This adds `--rkmpp-x20-force` only to the native RKMPP backend and injects the
seed once before the first H.264 NAL. Leave it disabled for normal H.264
encoders. Auto-detection still injects after observing the exact X20 SPS/PPS
when forced mode is disabled.

The full KMS controller was also tested with five fixture cycles and forced
recovery: it presented at 60 fps, decoded 521 frames, reported zero submit
stalls, kept the service active, and retained the same board boot ID.

FPVue uses GStreamer's `h264parse` with NAL alignment and normally detects the
X20 SPS/PPS during startup before injecting the seed. The captured fixture
starts mid-GOP and contains no SPS, PPS, or IDR, so it is intentionally a
stricter restart test than FPVue's usual startup path.

Run the probe with a valid stream containing SPS, PPS, and IDR to verify that
the same isolated path produces frames:

```bash
build/glide-rkmpp-rtp-probe --duration 30 --require-frame
```

Exit status 3 means no frame was decoded.

## Capture a new live Artosyn stream

On a host receiving the P401 bridge output:

```bash
examples/capture-artosyn-rtp.sh artosyn-session.pcap 10 any 5600
python3 tools/artosyn_rtp_fixture.py extract \
  artosyn-session.pcap \
  artosyn-session.rtp
python3 tools/artosyn_rtp_fixture.py inspect artosyn-session.rtp
```

Capture once while video is already running to obtain a midstream fixture, and
once from encoder startup to obtain SPS/PPS/IDR initialization traffic.

## Capture a kernel reset

Use an RK3588 UART serial console while running the isolated probe. SSH and
journald output may be lost during a hard reset. After reboot, also inspect:

```bash
sudo ls -la /sys/fs/pstore
sudo cat /sys/fs/pstore/* 2>/dev/null
journalctl -b -1 -k
last -x
```

The decisive A/B comparison uses the same fixture, pacing, probe binary, and
board state. Only the decoder recovery implementation should differ.

The recorded isolated reproduction and its before/after boot IDs are in
`testdata/artosyn/header-injection-reproduction.txt`.
