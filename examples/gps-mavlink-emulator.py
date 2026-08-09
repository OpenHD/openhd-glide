#!/usr/bin/env python3
################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
################################################################################

"""Send a moving, dependency-free MAVLink GPS simulation over UDP."""

import argparse
import math
import socket
import struct
import time


CRC_EXTRA = {
    0: 50,    # HEARTBEAT
    24: 24,   # GPS_RAW_INT
    30: 39,   # ATTITUDE
    33: 104,  # GLOBAL_POSITION_INT
    74: 20,   # VFR_HUD
}


def crc_accumulate(value, crc):
    value ^= crc & 0xFF
    value ^= (value << 4) & 0xFF
    return ((crc >> 8) ^ (value << 8) ^ (value << 3) ^ (value >> 4)) & 0xFFFF


class MavlinkSender:
    def __init__(self, host, port, system_id=1, component_id=1):
        self.destination = (host, port)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.system_id = system_id
        self.component_id = component_id
        self.sequence = 0

    def send(self, message_id, payload):
        header = struct.pack(
            "<BBBBB",
            len(payload),
            self.sequence,
            self.system_id,
            self.component_id,
            message_id,
        )
        crc = 0xFFFF
        for value in header + payload:
            crc = crc_accumulate(value, crc)
        crc = crc_accumulate(CRC_EXTRA[message_id], crc)
        frame = b"\xFE" + header + payload + struct.pack("<H", crc)
        self.socket.sendto(frame, self.destination)
        self.sequence = (self.sequence + 1) & 0xFF


def offset_coordinate(latitude, longitude, north_m, east_m):
    latitude_out = latitude + north_m / 111_320.0
    longitude_scale = max(0.01, math.cos(math.radians(latitude)))
    longitude_out = longitude + east_m / (111_320.0 * longitude_scale)
    return latitude_out, longitude_out


def route_state(pattern, elapsed, radius_m, speed_mps):
    angular_speed = speed_mps / max(1.0, radius_m)
    angle = elapsed * angular_speed
    if pattern == "line":
        span = max(10.0, radius_m * 2.0)
        phase = (elapsed * speed_mps / span) % 2.0
        east = (-radius_m + phase * span) if phase <= 1.0 else (radius_m - (phase - 1.0) * span)
        east_velocity = speed_mps if phase <= 1.0 else -speed_mps
        return 0.0, east, 0.0, east_velocity
    if pattern == "figure8":
        east = radius_m * math.sin(angle)
        north = radius_m * 0.5 * math.sin(2.0 * angle)
        east_velocity = speed_mps * math.cos(angle)
        north_velocity = speed_mps * math.cos(2.0 * angle)
        return north, east, north_velocity, east_velocity

    east = radius_m * math.sin(angle)
    north = radius_m * math.cos(angle)
    east_velocity = speed_mps * math.cos(angle)
    north_velocity = -speed_mps * math.sin(angle)
    return north, east, north_velocity, east_velocity


def clamp_i16(value):
    return max(-32768, min(32767, round(value)))


def send_telemetry(sender, boot_ms, latitude, longitude, altitude_m, north_velocity, east_velocity, heading_deg):
    lat_int = round(latitude * 10_000_000.0)
    lon_int = round(longitude * 10_000_000.0)
    altitude_mm = round(altitude_m * 1000.0)
    ground_speed = math.hypot(north_velocity, east_velocity)
    heading_cdeg = round((heading_deg % 360.0) * 100.0) % 36000

    sender.send(33, struct.pack(
        "<IiiiihhhH",
        boot_ms,
        lat_int,
        lon_int,
        altitude_mm,
        altitude_mm,
        clamp_i16(north_velocity * 100.0),
        clamp_i16(east_velocity * 100.0),
        0,
        heading_cdeg,
    ))
    sender.send(24, struct.pack(
        "<QiiiHHHHBB",
        boot_ms * 1000,
        lat_int,
        lon_int,
        altitude_mm,
        80,
        120,
        min(65535, round(ground_speed * 100.0)),
        heading_cdeg,
        3,
        14,
    ))
    sender.send(30, struct.pack(
        "<Iffffff",
        boot_ms,
        0.04 * math.sin(boot_ms / 700.0),
        0.03 * math.cos(boot_ms / 900.0),
        math.radians(heading_deg),
        0.0,
        0.0,
        0.0,
    ))
    sender.send(74, struct.pack(
        "<ffffhH",
        ground_speed,
        ground_speed,
        altitude_m,
        0.0,
        round(heading_deg) % 360,
        55,
    ))


def main():
    parser = argparse.ArgumentParser(description="Simulate moving MAVLink GPS telemetry for OpenHD Glide.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=14550)
    parser.add_argument("--lat", type=float, default=51.2373245, help="route center latitude (default: TMDT Wuppertal)")
    parser.add_argument("--lon", type=float, default=7.1616353, help="route center longitude (default: TMDT Wuppertal)")
    parser.add_argument("--altitude", type=float, default=80.0, help="altitude in metres")
    parser.add_argument("--speed", type=float, default=12.0, help="ground speed in m/s")
    parser.add_argument("--radius", type=float, default=180.0, help="circle/figure-eight radius in metres")
    parser.add_argument("--rate", type=float, default=10.0, help="position update rate in Hz")
    parser.add_argument("--pattern", choices=("circle", "figure8", "line"), default="figure8")
    parser.add_argument("--duration", type=float, default=0.0, help="seconds to run; zero runs until Ctrl+C")
    args = parser.parse_args()

    sender = MavlinkSender(args.host, args.port)
    start = time.monotonic()
    next_update = start
    next_heartbeat = start
    print(f"GPS emulator -> udp://{args.host}:{args.port}")
    print(f"Route: {args.pattern}, center {args.lat:.7f},{args.lon:.7f}, {args.speed:.1f} m/s; Ctrl+C to stop")
    try:
        while True:
            now = time.monotonic()
            elapsed = now - start
            if args.duration > 0.0 and elapsed >= args.duration:
                break
            if now >= next_heartbeat:
                # MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_GENERIC, armed, active, MAVLink 1.
                sender.send(0, struct.pack("<IBBBBB", 0, 2, 0, 0x80, 4, 3))
                next_heartbeat += 1.0
            north, east, north_velocity, east_velocity = route_state(args.pattern, elapsed, args.radius, args.speed)
            latitude, longitude = offset_coordinate(args.lat, args.lon, north, east)
            heading = math.degrees(math.atan2(east_velocity, north_velocity)) % 360.0
            boot_ms = round(elapsed * 1000.0) & 0xFFFFFFFF
            send_telemetry(sender, boot_ms, latitude, longitude, args.altitude, north_velocity, east_velocity, heading)
            if int(elapsed) != int(max(0.0, elapsed - 1.0 / max(1.0, args.rate))):
                print(f"  {latitude:.7f}, {longitude:.7f}  hdg {heading:5.1f}  t={elapsed:5.1f}s")
            next_update += 1.0 / max(1.0, args.rate)
            time.sleep(max(0.0, next_update - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        sender.socket.close()
        print("GPS emulator stopped")


if __name__ == "__main__":
    main()
