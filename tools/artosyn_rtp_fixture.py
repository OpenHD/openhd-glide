#!/usr/bin/env python3
"""Extract, inspect, and replay deterministic Artosyn/P401 RTP fixtures."""

from __future__ import annotations

import argparse
import hashlib
import json
import socket
import statistics
import struct
import time
from pathlib import Path


MAGIC = b"OHDRTP1\n"
RECORD = struct.Struct(">QI")


def read_pcap(path: Path):
    data = path.read_bytes()
    magics = {
        b"\xd4\xc3\xb2\xa1": ("<", 1_000_000),
        b"\xa1\xb2\xc3\xd4": (">", 1_000_000),
        b"\x4d\x3c\xb2\xa1": ("<", 1_000_000_000),
        b"\xa1\xb2\x3c\x4d": (">", 1_000_000_000),
    }
    if len(data) < 24 or data[:4] not in magics:
        raise ValueError("only classic microsecond/nanosecond PCAP files are supported")
    endian, fraction_scale = magics[data[:4]]
    link_type = struct.unpack_from(endian + "I", data, 20)[0]
    offset = 24
    while offset + 16 <= len(data):
        seconds, fraction, captured, _ = struct.unpack_from(endian + "IIII", data, offset)
        offset += 16
        frame = data[offset : offset + captured]
        offset += captured
        timestamp_us = seconds * 1_000_000 + fraction * 1_000_000 // fraction_scale
        yield timestamp_us, link_type, frame


def udp_payload(link_type: int, frame: bytes, destination_port: int):
    if link_type == 1:  # Ethernet
        if len(frame) < 14:
            return None
        ethernet_type = struct.unpack_from("!H", frame, 12)[0]
        offset = 14
        if ethernet_type == 0x8100 and len(frame) >= 18:
            ethernet_type = struct.unpack_from("!H", frame, 16)[0]
            offset = 18
        if ethernet_type != 0x0800:
            return None
    elif link_type == 113:  # Linux cooked capture v1
        if len(frame) < 16 or struct.unpack_from("!H", frame, 14)[0] != 0x0800:
            return None
        offset = 16
    else:
        raise ValueError(f"unsupported PCAP link type {link_type}")

    if len(frame) < offset + 20 or frame[offset] >> 4 != 4:
        return None
    header_length = (frame[offset] & 0x0F) * 4
    if header_length < 20 or len(frame) < offset + header_length + 8:
        return None
    if frame[offset + 9] != 17:
        return None
    udp = offset + header_length
    if struct.unpack_from("!H", frame, udp + 2)[0] != destination_port:
        return None
    return frame[udp + 8 :]


def read_fixture(path: Path):
    data = path.read_bytes()
    if not data.startswith(MAGIC):
        raise ValueError("not an OpenHD RTP fixture")
    offset = len(MAGIC)
    records = []
    while offset < len(data):
        if offset + RECORD.size > len(data):
            raise ValueError("truncated fixture record")
        timestamp_us, size = RECORD.unpack_from(data, offset)
        offset += RECORD.size
        if offset + size > len(data):
            raise ValueError("truncated fixture payload")
        records.append((timestamp_us, data[offset : offset + size]))
        offset += size
    return records


def rtp_details(payload: bytes):
    if len(payload) < 12 or payload[0] >> 6 != 2:
        return None
    csrc_count = payload[0] & 0x0F
    header_size = 12 + csrc_count * 4
    if payload[0] & 0x10:
        if len(payload) < header_size + 4:
            return None
        extension_words = struct.unpack_from("!H", payload, header_size + 2)[0]
        header_size += 4 + extension_words * 4
    if len(payload) < header_size:
        return None
    return {
        "sequence": struct.unpack_from("!H", payload, 2)[0],
        "timestamp": struct.unpack_from("!I", payload, 4)[0],
        "marker": bool(payload[1] & 0x80),
        "payload": payload[header_size:],
    }


def h264_nal_types(records):
    counts: dict[int, int] = {}
    for _, packet in records:
        details = rtp_details(packet)
        if not details or not details["payload"]:
            continue
        body = details["payload"]
        nal_type = body[0] & 0x1F
        if nal_type == 28 and len(body) >= 2:
            nal_type = body[1] & 0x1F
        counts[nal_type] = counts.get(nal_type, 0) + 1
    return counts


def summarize(records):
    valid = [rtp_details(packet) for _, packet in records]
    valid = [details for details in valid if details]
    timestamps = []
    for details in valid:
        if not timestamps or timestamps[-1] != details["timestamp"]:
            timestamps.append(details["timestamp"])
    duration_us = records[-1][0] - records[0][0] if len(records) > 1 else 0
    return {
        "packets": len(records),
        "rtp_packets": len(valid),
        "access_units": len(timestamps),
        "capture_duration_us": duration_us,
        "h264_nal_packet_counts": {
            str(key): value for key, value in sorted(h264_nal_types(records).items())
        },
    }


def command_extract(args):
    records = []
    first_timestamp = None
    for timestamp_us, link_type, frame in read_pcap(args.pcap):
        payload = udp_payload(link_type, frame, args.port)
        if payload is None:
            continue
        if first_timestamp is None:
            first_timestamp = timestamp_us
        records.append((timestamp_us - first_timestamp, payload))
    if not records:
        raise SystemExit(f"no UDP/{args.port} packets found")

    with args.output.open("wb") as output:
        output.write(MAGIC)
        for timestamp_us, payload in records:
            output.write(RECORD.pack(timestamp_us, len(payload)))
            output.write(payload)

    metadata = summarize(records)
    metadata.update(
        {
            "format": MAGIC.decode("ascii").strip(),
            "udp_destination_port": args.port,
            "source_pcap_sha256": hashlib.sha256(args.pcap.read_bytes()).hexdigest(),
            "fixture_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
        }
    )
    metadata_path = args.metadata or args.output.with_suffix(args.output.suffix + ".json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2))


def command_inspect(args):
    print(json.dumps(summarize(read_fixture(args.fixture)), indent=2))


def inferred_timestamp_step(records):
    timestamps = []
    for _, packet in records:
        details = rtp_details(packet)
        if details and (not timestamps or timestamps[-1] != details["timestamp"]):
            timestamps.append(details["timestamp"])
    differences = [
        (current - previous) & 0xFFFFFFFF
        for previous, current in zip(timestamps, timestamps[1:])
        if current != previous
    ]
    return int(statistics.median(differences)) if differences else 1500


def command_replay(args):
    records = read_fixture(args.fixture)
    if not records:
        raise SystemExit("fixture is empty")
    sequence = args.sequence
    output_timestamp = args.timestamp
    timestamp_step = args.timestamp_step or inferred_timestamp_step(records)
    sent = 0
    access_units = 0
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    for _ in range(args.cycles):
        previous_source_timestamp = None
        previous_capture_us = records[0][0]
        for capture_us, original in records:
            packet = bytearray(original)
            details = rtp_details(packet)
            if not details:
                continue
            source_timestamp = details["timestamp"]
            if previous_source_timestamp is None or source_timestamp != previous_source_timestamp:
                if previous_source_timestamp is not None:
                    output_timestamp = (output_timestamp + timestamp_step) & 0xFFFFFFFF
                access_units += 1
            previous_source_timestamp = source_timestamp
            struct.pack_into("!H", packet, 2, sequence & 0xFFFF)
            struct.pack_into("!I", packet, 4, output_timestamp)
            sequence += 1

            if args.packet_delay_us is not None:
                time.sleep(args.packet_delay_us / 1_000_000)
            elif not args.no_pace:
                delta_us = max(0, capture_us - previous_capture_us)
                time.sleep(delta_us / 1_000_000 / args.speed)
            previous_capture_us = capture_us
            udp.sendto(packet, (args.host, args.port))
            sent += 1
        output_timestamp = (output_timestamp + timestamp_step) & 0xFFFFFFFF

    print(
        f"sent {sent} RTP packets / {access_units} access units "
        f"to {args.host}:{args.port}"
    )


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    extract = commands.add_parser("extract", help="extract UDP RTP from a classic PCAP")
    extract.add_argument("pcap", type=Path)
    extract.add_argument("output", type=Path)
    extract.add_argument("--port", type=int, default=5600)
    extract.add_argument("--metadata", type=Path)
    extract.set_defaults(function=command_extract)

    inspect = commands.add_parser("inspect", help="summarize a fixture")
    inspect.add_argument("fixture", type=Path)
    inspect.set_defaults(function=command_inspect)

    replay = commands.add_parser("replay", help="replay a fixture over UDP")
    replay.add_argument("fixture", type=Path)
    replay.add_argument("host")
    replay.add_argument("--port", type=int, default=5600)
    replay.add_argument("--cycles", type=int, default=1)
    replay.add_argument("--speed", type=float, default=1.0)
    replay.add_argument("--no-pace", action="store_true")
    replay.add_argument("--packet-delay-us", type=int)
    replay.add_argument("--sequence", type=int, default=1000)
    replay.add_argument("--timestamp", type=int, default=90000)
    replay.add_argument("--timestamp-step", type=int)
    replay.set_defaults(function=command_replay)
    return result


if __name__ == "__main__":
    arguments = parser().parse_args()
    arguments.function(arguments)
