# openhd-glide-ethernet

`openhd-glide-ethernet` is a small helper for direct Ethernet camera streaming between an OpenHD air unit and a Glide display unit.

## Receiver

Run this on the display unit:

```sh
openhd-glide-ethernet -receive
```

The receiver:

- auto-detects the local platform
- uses native RKMPP video on Rockchip devices
- listens for RTP/MJPEG video on UDP port `5600`
- advertises itself through Glide Ethernet discovery on UDP port `5620`
- writes receiver logs to `/tmp/openhd-glide-ethernet-receive.log`

`-receive` re-runs itself through `sudo` when needed because KMS display access normally requires root.

## Sender

Run this on the camera unit:

```sh
openhd-glide-ethernet -send
```

The sender:

- auto-detects the local platform
- searches the subnet for receivers
- prints discovered receiver IP addresses
- asks which receiver to use
- asks which video mode to stream
- starts `rpicam-vid` or `libcamera-vid` with RTP/JPEG packetization

Preset modes:

- `720p 60 MJPEG quality 75`
- `720p 120 MJPEG quality 65`
- `1080p 30 MJPEG quality 75`
- `1536x864 60 MJPEG quality 75`
- `1536x864 120 MJPEG quality 65`
- `Custom`

For `Custom`, enter width, height, FPS, and MJPEG quality.

## Logs

Console output is intentionally minimal.

Sender logs:

```sh
cat /tmp/openhd-glide-ethernet-camera.log
cat /tmp/openhd-glide-ethernet-gst.log
```

Receiver logs:

```sh
tail -f /tmp/openhd-glide-ethernet-receive.log
```

## Notes

For the current Raspberry Pi 5 to Radxa Zero 3 Ethernet setup, `720p 60 MJPEG quality 75` is the most stable tested mode. Higher MJPEG modes can saturate the path and cause RTP packet gaps.
