# Artosyn RTP fixtures

`p401-midstream-no-idr.rtp` is a deterministic extraction of a live Artosyn
P401 UDP/RTP capture. It starts in the middle of an H.264 stream and contains
AUD and non-IDR slice packets, but no SPS, PPS, or IDR NAL units.

The fixture format starts with `OHDRTP1\n`. Each record contains a big-endian
capture timestamp in microseconds, a big-endian payload length, and one UDP
payload. Use `tools/artosyn_rtp_fixture.py` to inspect or replay it.

The source PCAP is intentionally not required at runtime. The adjacent JSON
records its SHA-256 digest and extraction statistics.
