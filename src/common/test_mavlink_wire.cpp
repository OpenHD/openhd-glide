// SPDX-License-Identifier: GPL-3.0-only
// Standalone protocol regression test; no UI/KMS runtime is started.
#include "mavlink_udp_bridge.cpp"
#include <cassert>

int main() {
    using namespace glide::mavlink;
    assert(read_le<std::uint32_t>({0x34, 0x12}, 0) == 0x1234);
    assert(read_le<std::uint16_t>({0x34}, 0) == 0x34);
    assert(read_le<std::uint16_t>({0x34}, 1) == 0);
    assert(read_le<std::int16_t>({0xff, 0xff}, 0) == -1);
    assert(glide::openhd::generated::crc_extra(1211) == 134);
    assert(!glide::openhd::generated::crc_extra(0xffffff));
}
