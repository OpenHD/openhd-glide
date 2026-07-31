/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 *
 * This software is provided "as-is," without warranty of any kind, express or
 * implied, including but not limited to the warranties of merchantability,
 * fitness for a particular purpose, and non-infringement. For details, see the
 * full license in the LICENSE file provided with this source code.
 *
 * Non-Military Use Only:
 * This software and its associated components are explicitly intended for
 * civilian and non-military purposes. Use in any military or defense
 * applications is strictly prohibited unless explicitly and individually
 * licensed otherwise by the OpenHD Team.
 *
 * Contributors:
 * A full list of contributors can be found at the OpenHD GitHub repository:
 * https://github.com/OpenHD
 *
 * © OpenHD, All Rights Reserved.
 ******************************************************************************/

#include "video/rockchip_mpp_rtp_decoder.hpp"

#if OPENHD_GLIDE_HAS_RKMPP
#include "common/logging.hpp"

#include <rockchip/rk_mpi.h>

#include <arpa/inet.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#endif

namespace glide::video {

#if OPENHD_GLIDE_HAS_RKMPP
namespace {
MppCtx as_ctx(void* ctx)
{
    return static_cast<MppCtx>(ctx);
}

MppApi* as_mpi(void* mpi)
{
    return static_cast<MppApi*>(mpi);
}

MppFrame as_frame(void* frame)
{
    return static_cast<MppFrame>(frame);
}

constexpr std::uint8_t start_code[] { 0x00, 0x00, 0x00, 0x01 };
constexpr std::uint8_t x20_sps[] { 0x67, 0x4d, 0x00, 0x29, 0x96, 0x54, 0x02, 0x80, 0x2d, 0x88 };
constexpr std::uint8_t x20_pps[] { 0x68, 0xee, 0x31, 0x12 };
constexpr std::array<std::uint8_t, 64> jpeg_luma_quantizer {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};
constexpr std::array<std::uint8_t, 64> jpeg_chroma_quantizer {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};
constexpr std::array<std::uint8_t, 28> jpeg_huffman_luma_dc {
    0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b,
};
constexpr std::array<std::uint8_t, 28> jpeg_huffman_chroma_dc {
    0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b,
};
constexpr std::array<std::uint8_t, 178> jpeg_huffman_luma_ac {
    0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03,
    0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d,
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
constexpr std::array<std::uint8_t, 178> jpeg_huffman_chroma_ac {
    0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04,
    0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77,
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

void append_start_code(std::vector<std::uint8_t>& output)
{
    output.insert(output.end(), std::begin(start_code), std::end(start_code));
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_marker(std::vector<std::uint8_t>& output, std::uint8_t marker)
{
    output.push_back(0xff);
    output.push_back(marker);
}

void make_jpeg_quant_tables(std::uint8_t q, std::array<std::uint8_t, 64>& luma, std::array<std::uint8_t, 64>& chroma)
{
    auto factor = static_cast<int>(q);
    if (factor < 1) {
        factor = 1;
    } else if (factor > 99) {
        factor = 99;
    }
    const auto scale = factor < 50 ? 5000 / factor : 200 - factor * 2;
    for (std::size_t i = 0; i < luma.size(); ++i) {
        const auto lq = std::clamp((static_cast<int>(jpeg_luma_quantizer[i]) * scale + 50) / 100, 1, 255);
        const auto cq = std::clamp((static_cast<int>(jpeg_chroma_quantizer[i]) * scale + 50) / 100, 1, 255);
        luma[i] = static_cast<std::uint8_t>(lq);
        chroma[i] = static_cast<std::uint8_t>(cq);
    }
}

void append_quant_header(std::vector<std::uint8_t>& output, const std::array<std::uint8_t, 64>& table, std::uint8_t table_id)
{
    append_marker(output, 0xdb);
    append_u16(output, 67);
    output.push_back(table_id);
    output.insert(output.end(), table.begin(), table.end());
}

void append_huffman_header(std::vector<std::uint8_t>& output, const std::uint8_t* table, std::size_t size, std::uint8_t table_id, std::uint8_t table_class)
{
    append_marker(output, 0xc4);
    append_u16(output, static_cast<std::uint16_t>(3U + size));
    output.push_back(static_cast<std::uint8_t>((table_class << 4U) | table_id));
    output.insert(output.end(), table, table + size);
}

void append_jpeg_header(
    std::vector<std::uint8_t>& output,
    std::uint8_t type,
    std::uint16_t width,
    std::uint16_t height,
    const std::array<std::uint8_t, 64>& luma,
    const std::array<std::uint8_t, 64>& chroma,
    std::uint16_t restart_interval)
{
    append_marker(output, 0xd8);
    append_marker(output, 0xe0);
    append_u16(output, 16);
    output.insert(output.end(), { 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 });
    append_quant_header(output, luma, 0);
    append_quant_header(output, chroma, 1);
    if (restart_interval != 0) {
        append_marker(output, 0xdd);
        append_u16(output, 4);
        append_u16(output, restart_interval);
    }
    append_marker(output, 0xc0);
    append_u16(output, 17);
    output.push_back(8);
    append_u16(output, height);
    append_u16(output, width);
    output.push_back(3);
    output.push_back(1);
    output.push_back(type == 0 ? 0x21 : 0x22);
    output.push_back(0);
    output.push_back(2);
    output.push_back(0x11);
    output.push_back(1);
    output.push_back(3);
    output.push_back(0x11);
    output.push_back(1);
    append_huffman_header(output, jpeg_huffman_luma_dc.data(), jpeg_huffman_luma_dc.size(), 0, 0);
    append_huffman_header(output, jpeg_huffman_luma_ac.data(), jpeg_huffman_luma_ac.size(), 0, 1);
    append_huffman_header(output, jpeg_huffman_chroma_dc.data(), jpeg_huffman_chroma_dc.size(), 1, 0);
    append_huffman_header(output, jpeg_huffman_chroma_ac.data(), jpeg_huffman_chroma_ac.size(), 1, 1);
    append_marker(output, 0xda);
    append_u16(output, 12);
    output.push_back(3);
    output.push_back(1);
    output.push_back(0);
    output.push_back(2);
    output.push_back(0x11);
    output.push_back(3);
    output.push_back(0x11);
    output.push_back(0);
    output.push_back(63);
    output.push_back(0);
}

} // namespace
#endif

bool rockchip_mpp_decoder_available()
{
#if OPENHD_GLIDE_HAS_RKMPP
    return true;
#else
    return false;
#endif
}

RockchipMppRtpDecoder::~RockchipMppRtpDecoder()
{
    cleanup();
}

bool RockchipMppRtpDecoder::start(
    std::uint16_t udp_port,
    const std::string& codec,
    bool force_x20_header)
{
#if OPENHD_GLIDE_HAS_RKMPP
    last_error_.clear();
    force_x20_header_ = force_x20_header;
    if (!init_mpp(codec)) {
        cleanup();
        return false;
    }
    if (!init_socket(udp_port)) {
        cleanup();
        return false;
    }
    running_.store(true, std::memory_order_release);
    feed_thread_ = std::thread(&RockchipMppRtpDecoder::feed_loop, this);
    frame_thread_ = mjpeg_ ? std::thread(&RockchipMppRtpDecoder::mjpeg_task_loop, this)
                           : std::thread(&RockchipMppRtpDecoder::frame_loop, this);
    return true;
#else
    (void)udp_port;
    (void)codec;
    (void)force_x20_header;
    last_error_ = "native Rockchip MPP decoder support was not found at build time";
    return false;
#endif
}

bool RockchipMppRtpDecoder::poll(glide::dev::DmabufVideoFrame& frame)
{
#if OPENHD_GLIDE_HAS_RKMPP
    void* selected {};
    {
        std::lock_guard lock(mutex_);
        if (ready_frames_.empty()) {
            return false;
        }
        while (ready_frames_.size() > 2) {
            auto dropped = ready_frames_.front();
            ready_frames_.pop_front();
            release_frame(dropped);
            ++dropped_decoded_frames_;
        }
        selected = ready_frames_.front();
        ready_frames_.pop_front();
    }

    if (pending_presented_frame_ != nullptr) {
        release_frame(pending_presented_frame_);
    }
    pending_presented_frame_ = selected;
    if (!frame_to_dmabuf(selected, frame)) {
        release_frame(pending_presented_frame_);
        return false;
    }
    return true;
#else
    (void)frame;
    return false;
#endif
}

void RockchipMppRtpDecoder::mark_presented()
{
#if OPENHD_GLIDE_HAS_RKMPP
    if (current_frame_ != nullptr) {
        release_frame(current_frame_);
    }
    current_frame_ = pending_presented_frame_;
    pending_presented_frame_ = nullptr;
#endif
}

std::string RockchipMppRtpDecoder::stats() const
{
#if OPENHD_GLIDE_HAS_RKMPP
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "parsed_units=" << parsed_units_
        << " submitted_packets=" << submitted_packets_
        << " decoded_frames=" << decoded_frames_
        << " queued_frames=" << ready_frames_.size()
        << " dropped_decoded_frames=" << dropped_decoded_frames_
        << " rtp_packets=" << rtp_packets_
        << " rtp_sequence_gaps=" << rtp_sequence_gaps_
        << " rtp_sequence_resyncs=" << rtp_sequence_resyncs_
        << " late_or_duplicate_packets=" << late_or_duplicate_packets_
        << " incomplete_fragments=" << incomplete_fragments_
        << " x20_header_injections=" << x20_header_injections_
        << " submit_stalls=" << submit_stalls_
        << " input=native-udp-rtp";
    return out.str();
#else
    return {};
#endif
}

const std::string& RockchipMppRtpDecoder::last_error() const
{
    return last_error_;
}

#if OPENHD_GLIDE_HAS_RKMPP
bool RockchipMppRtpDecoder::init_mpp(const std::string& codec)
{
    h265_ = codec == "h265" || codec == "hevc";
    mjpeg_ = codec == "mjpeg" || codec == "mjpg" || codec == "jpeg" || codec == "mjepg";
    const auto coding = mjpeg_ ? MPP_VIDEO_CodingMJPEG : (h265_ ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC);
    if (mpp_check_support_format(MPP_CTX_DEC, coding) != MPP_OK) {
        last_error_ = mjpeg_ ? "Rockchip MPP does not support MJPEG decode" : (h265_ ? "Rockchip MPP does not support HEVC decode" : "Rockchip MPP does not support AVC decode");
        return false;
    }
    MppCtx ctx {};
    MppApi* mpi {};
    if (mpp_create(&ctx, &mpi) != MPP_OK || ctx == nullptr || mpi == nullptr) {
        last_error_ = "mpp_create failed";
        return false;
    }
    ctx_ = ctx;
    mpi_ = mpi;
    configure_mpp();
    if (mpp_init(as_ctx(ctx_), MPP_CTX_DEC, coding) != MPP_OK) {
        last_error_ = "mpp_init decoder failed";
        return false;
    }
    configure_mpp();
    int output_block = MPP_POLL_NON_BLOCK;
    as_mpi(mpi_)->control(as_ctx(ctx_), MPP_SET_OUTPUT_BLOCK, &output_block);
    return true;
}

bool RockchipMppRtpDecoder::configure_mpp()
{
    MppDecCfg cfg {};
    if (mpp_dec_cfg_init(&cfg) != MPP_OK) {
        return false;
    }
    if (mpi_ != nullptr && ctx_ != nullptr && as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_GET_CFG, cfg) == MPP_OK) {
        RK_U32 split_parse = 1;
        mpp_dec_cfg_set_u32(cfg, "base:split_parse", split_parse);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_CFG, cfg);
    }
    mpp_dec_cfg_deinit(cfg);

    RK_U32 off {};
    RK_U32 on = 0xffff;
    if (mpi_ != nullptr && ctx_ != nullptr) {
        auto output_format = static_cast<MppFrameFormat>(MPP_FMT_YUV420SP);
        RK_U32 parser_split = mjpeg_ ? on : off;
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_PARSER_SPLIT_MODE, &parser_split);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_DISABLE_ERROR, &on);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_IMMEDIATE_OUT, &on);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_ENABLE_FAST_PLAY, &on);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_PARSER_FAST_MODE, &off);
    }
    return true;
}

bool RockchipMppRtpDecoder::init_socket(std::uint16_t udp_port)
{
    socket_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd_ < 0) {
        last_error_ = std::string("failed to create RKMPP UDP socket: ") + std::strerror(errno);
        return false;
    }

    const int receive_buffer = 32 * 1024 * 1024;
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(udp_port);
    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = std::string("failed to bind RKMPP UDP RTP socket: ") + std::strerror(errno);
        return false;
    }
    const auto flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);
    }
    return true;
}

bool RockchipMppRtpDecoder::handle_rtp_packet(const std::uint8_t* packet, std::size_t size)
{
    if (size < 12 || (packet[0] >> 6U) != 2) {
        return true;
    }

    const bool has_padding = (packet[0] & 0x20U) != 0;
    const bool has_extension = (packet[0] & 0x10U) != 0;
    const auto csrc_count = packet[0] & 0x0FU;
    const bool marker = (packet[1] & 0x80U) != 0;
    const auto sequence = static_cast<std::uint16_t>((static_cast<std::uint16_t>(packet[2]) << 8U) | packet[3]);
    const auto timestamp = (static_cast<std::uint32_t>(packet[4]) << 24U)
        | (static_cast<std::uint32_t>(packet[5]) << 16U)
        | (static_cast<std::uint32_t>(packet[6]) << 8U)
        | static_cast<std::uint32_t>(packet[7]);

    std::size_t offset = 12U + static_cast<std::size_t>(csrc_count) * 4U;
    if (offset > size) {
        return true;
    }
    if (has_extension) {
        if (offset + 4U > size) {
            return true;
        }
        const auto extension_words = (static_cast<std::size_t>(packet[offset + 2U]) << 8U) | packet[offset + 3U];
        offset += 4U + extension_words * 4U;
        if (offset > size) {
            return true;
        }
    }

    std::size_t payload_size = size - offset;
    if (has_padding && payload_size > 0) {
        const auto padding = packet[size - 1U];
        if (padding <= payload_size) {
            payload_size -= padding;
        }
    }
    if (payload_size == 0) {
        return true;
    }

    {
        std::lock_guard lock(mutex_);
        ++rtp_packets_;
        if (have_sequence_) {
            const auto sequence_delta = static_cast<std::int16_t>(sequence - expected_sequence_);
            if (sequence_delta < 0) {
                const auto timestamp_delta = have_rtp_timestamp_
                    ? static_cast<std::int32_t>(timestamp - last_rtp_timestamp_)
                    : 0;
                if (sequence_delta < -1000 || timestamp_delta < -90000) {
                    ++rtp_sequence_resyncs_;
                    fragment_.clear();
                    access_unit_.clear();
                    have_access_unit_ = false;
                } else {
                    ++late_or_duplicate_packets_;
                    return true;
                }
            }
            if (sequence_delta > 0) {
                ++rtp_sequence_gaps_;
                fragment_.clear();
                access_unit_.clear();
                have_access_unit_ = false;
            }
        }
        expected_sequence_ = static_cast<std::uint16_t>(sequence + 1U);
        last_rtp_timestamp_ = timestamp;
        have_rtp_timestamp_ = true;
        have_sequence_ = true;
    }

    if (mjpeg_) {
        return append_mjpeg_payload(packet + offset, payload_size, marker, timestamp);
    }
    return h265_ ? append_h265_payload(packet + offset, payload_size, marker, sequence, timestamp)
                 : append_h264_payload(packet + offset, payload_size, marker, sequence, timestamp);
}

bool RockchipMppRtpDecoder::append_h264_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint16_t, std::uint32_t timestamp)
{
    if (size == 0) {
        return true;
    }

    const auto nal_type = payload[0] & 0x1FU;
    if (nal_type >= 1 && nal_type <= 23) {
        return queue_nal(payload, size, timestamp) && (!marker || flush_access_unit());
    }
    if (nal_type == 24) {
        std::size_t offset = 1;
        while (offset + 2U <= size) {
            const auto nal_size = (static_cast<std::size_t>(payload[offset]) << 8U) | payload[offset + 1U];
            offset += 2U;
            if (offset + nal_size > size) {
                break;
            }
            if (!queue_nal(payload + offset, nal_size, timestamp)) {
                return false;
            }
            offset += nal_size;
        }
        return !marker || flush_access_unit();
    }
    if (nal_type == 28 && size >= 2) {
        const auto fu_indicator = payload[0];
        const auto fu_header = payload[1];
        const bool start = (fu_header & 0x80U) != 0;
        const bool end = (fu_header & 0x40U) != 0;
        const auto reconstructed = static_cast<std::uint8_t>((fu_indicator & 0xE0U) | (fu_header & 0x1FU));
        if (start) {
            fragment_.clear();
            fragment_.push_back(reconstructed);
            current_timestamp_ = timestamp;
        } else if (fragment_.empty() || current_timestamp_ != timestamp) {
            std::lock_guard lock(mutex_);
            ++incomplete_fragments_;
            return true;
        }
        fragment_.insert(fragment_.end(), payload + 2, payload + size);
        if (end) {
            const auto submitted = queue_nal(fragment_.data(), fragment_.size(), timestamp);
            fragment_.clear();
            return submitted && (!marker || flush_access_unit());
        }
    }
    return true;
}

bool RockchipMppRtpDecoder::append_h265_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint16_t, std::uint32_t timestamp)
{
    if (size < 2) {
        return true;
    }

    const auto nal_type = (payload[0] >> 1U) & 0x3FU;
    if (nal_type <= 47) {
        return queue_nal(payload, size, timestamp) && (!marker || flush_access_unit());
    }
    if (nal_type == 48) {
        std::size_t offset = 2;
        while (offset + 2U <= size) {
            const auto nal_size = (static_cast<std::size_t>(payload[offset]) << 8U) | payload[offset + 1U];
            offset += 2U;
            if (offset + nal_size > size) {
                break;
            }
            if (!queue_nal(payload + offset, nal_size, timestamp)) {
                return false;
            }
            offset += nal_size;
        }
        return !marker || flush_access_unit();
    }
    if (nal_type == 49 && size >= 3) {
        const auto fu_header = payload[2];
        const bool start = (fu_header & 0x80U) != 0;
        const bool end = (fu_header & 0x40U) != 0;
        const auto reconstructed_type = fu_header & 0x3FU;
        if (start) {
            fragment_.clear();
            fragment_.push_back(static_cast<std::uint8_t>((payload[0] & 0x81U) | (reconstructed_type << 1U)));
            fragment_.push_back(payload[1]);
            current_timestamp_ = timestamp;
        } else if (fragment_.empty() || current_timestamp_ != timestamp) {
            std::lock_guard lock(mutex_);
            ++incomplete_fragments_;
            return true;
        }
        fragment_.insert(fragment_.end(), payload + 3, payload + size);
        if (end) {
            const auto submitted = queue_nal(fragment_.data(), fragment_.size(), timestamp);
            fragment_.clear();
            return submitted && (!marker || flush_access_unit());
        }
    }
    return true;
}

bool RockchipMppRtpDecoder::append_mjpeg_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint32_t timestamp)
{
    if (size < 8) {
        return true;
    }

    const auto fragment_offset = (static_cast<std::uint32_t>(payload[1]) << 16U)
        | (static_cast<std::uint32_t>(payload[2]) << 8U)
        | static_cast<std::uint32_t>(payload[3]);
    auto type = payload[4];
    const auto q = payload[5];
    const auto width = static_cast<std::uint16_t>(payload[6] == 0 ? 2048 : payload[6] * 8U);
    const auto height = static_cast<std::uint16_t>(payload[7] == 0 ? 2048 : payload[7] * 8U);

    std::size_t payload_offset = 8;
    std::uint16_t restart_interval {};
    if ((type & 0x40U) != 0) {
        if (size < payload_offset + 4U) {
            return true;
        }
        restart_interval = static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[payload_offset]) << 8U) | payload[payload_offset + 1U]);
        payload_offset += 4U;
        type = static_cast<std::uint8_t>(type & ~0x40U);
    }

    std::array<std::uint8_t, 64> luma {};
    std::array<std::uint8_t, 64> chroma {};
    if (q >= 128 && fragment_offset == 0) {
        if (size < payload_offset + 4U) {
            return true;
        }
        const auto precision = payload[payload_offset + 1U];
        const auto table_length = static_cast<std::size_t>((static_cast<std::uint16_t>(payload[payload_offset + 2U]) << 8U) | payload[payload_offset + 3U]);
        payload_offset += 4U;
        if (precision != 0 || table_length < 128U || size < payload_offset + table_length) {
            std::lock_guard lock(mutex_);
            ++incomplete_fragments_;
            return true;
        }
        std::copy_n(payload + payload_offset, luma.size(), luma.begin());
        std::copy_n(payload + payload_offset + luma.size(), chroma.size(), chroma.begin());
        payload_offset += table_length;
    } else {
        make_jpeg_quant_tables(q, luma, chroma);
    }

    if (payload_offset >= size) {
        return true;
    }

    const auto* scan_data = payload + payload_offset;
    const auto scan_size = size - payload_offset;
    if (fragment_offset == 0) {
        if (have_access_unit_ && !access_unit_.empty() && access_unit_timestamp_ != timestamp) {
            queue_mjpeg_access_unit(mjpeg_width_, mjpeg_height_, access_unit_timestamp_);
        }
        access_unit_.clear();
        access_unit_timestamp_ = timestamp;
        have_access_unit_ = true;
        mjpeg_width_ = width;
        mjpeg_height_ = height;
        if (scan_size >= 2 && scan_data[0] == 0xff && scan_data[1] == 0xd8) {
            access_unit_.insert(access_unit_.end(), scan_data, scan_data + scan_size);
        } else {
            append_jpeg_header(access_unit_, type, width, height, luma, chroma, restart_interval);
            access_unit_.insert(access_unit_.end(), scan_data, scan_data + scan_size);
        }
    } else if (!have_access_unit_ || access_unit_timestamp_ != timestamp) {
        std::lock_guard lock(mutex_);
        ++incomplete_fragments_;
        return true;
    } else {
        access_unit_.insert(access_unit_.end(), scan_data, scan_data + scan_size);
    }

    if (!marker) {
        return true;
    }
    queue_mjpeg_access_unit(width, height, timestamp);
    return true;
}

void RockchipMppRtpDecoder::queue_mjpeg_access_unit(std::uint32_t width, std::uint32_t height, std::int64_t pts)
{
    if (access_unit_.size() < 2 || access_unit_[access_unit_.size() - 2U] != 0xff || access_unit_[access_unit_.size() - 1U] != 0xd9) {
        access_unit_.push_back(0xff);
        access_unit_.push_back(0xd9);
    }
    {
        std::lock_guard lock(mutex_);
        while (mjpeg_units_.size() >= 3) {
            mjpeg_units_.pop_front();
            ++dropped_decoded_frames_;
        }
        mjpeg_units_.push_back(MjpegAccessUnit {
            .data = std::move(access_unit_),
            .pts = pts,
            .width = width,
            .height = height,
        });
    }
    mjpeg_units_available_.notify_one();
    access_unit_.clear();
    have_access_unit_ = false;
}

bool RockchipMppRtpDecoder::submit_nal(const std::uint8_t* data, std::size_t size, std::int64_t pts)
{
    if (data == nullptr || size == 0) {
        return true;
    }
    if (!h265_ && update_x20_detection(data, size) && !inject_x20_header_if_needed()) {
        return false;
    }
    std::array<std::uint8_t, 4> prefix { 0x00, 0x00, 0x00, 0x01 };
    if (size >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data)) {
        return submit_packet(data, size, pts);
    }

    std::vector<std::uint8_t> annex_b;
    annex_b.reserve(prefix.size() + size);
    append_start_code(annex_b);
    annex_b.insert(annex_b.end(), data, data + size);
    return submit_packet(annex_b.data(), annex_b.size(), pts);
}

bool RockchipMppRtpDecoder::queue_nal(const std::uint8_t* data, std::size_t size, std::uint32_t timestamp)
{
    if (data == nullptr || size == 0) {
        return true;
    }
    if (force_x20_header_ && !h265_ && !mjpeg_ && submitted_packets_ == 0
        && !x20_sps_seen_ && !x20_pps_seen_ && !x20_header_injected_) {
        glide::log(
            glide::LogLevel::warning,
            "OpenHD-Glide",
            "injecting configured X20 recovery seed before first H264 NAL");
        if (!inject_x20_header_if_needed()) {
            return false;
        }
    }

    const auto nal_type = h265_ ? ((data[0] >> 1U) & 0x3FU) : (data[0] & 0x1FU);
    const bool is_vcl = h265_ ? nal_type <= 31U : (nal_type >= 1U && nal_type <= 5U);
    if (!is_vcl) {
        // Keep parameter sets and other non-picture NALs on FPVue's proven
        // alignment=nal path. In particular, do not rebuild the old packet
        // containing an X20 SPS/PPS/IDR seed followed by live data: that packet
        // shape can wedge the RK3588 MPP parser.
        return submit_nal(data, size, timestamp);
    }

    // One encoded picture may contain several VCL NALs. Raspberry Pi's x264
    // low-latency pipeline enables sliced-threads, so treating every slice as a
    // complete MPP packet produces a partially decoded top band and a green
    // remainder. Group only VCL NALs sharing the RTP timestamp; single-slice
    // X20 pictures still take this path as a one-NAL access unit.
    if (have_access_unit_ && access_unit_timestamp_ != timestamp) {
        // The RTP marker for the preceding picture was lost. Submitting a
        // partial multi-slice picture poisons subsequent references, so discard
        // it and wait for the new picture instead.
        access_unit_.clear();
        have_access_unit_ = false;
        std::lock_guard lock(mutex_);
        ++incomplete_fragments_;
    }
    if (!have_access_unit_) {
        access_unit_timestamp_ = timestamp;
        have_access_unit_ = true;
    }
    append_start_code(access_unit_);
    access_unit_.insert(access_unit_.end(), data, data + size);
    return true;
}

bool RockchipMppRtpDecoder::flush_access_unit()
{
    if (!have_access_unit_ || access_unit_.empty()) {
        have_access_unit_ = false;
        access_unit_.clear();
        return true;
    }
    const auto timestamp = access_unit_timestamp_;
    const auto submitted = submit_packet(access_unit_.data(), access_unit_.size(), timestamp);
    have_access_unit_ = false;
    access_unit_.clear();
    return submitted;
}

bool RockchipMppRtpDecoder::update_x20_detection(const std::uint8_t* data, std::size_t size)
{
    if (x20_header_injected_ || x20_checked_non_x20_ || data == nullptr || size == 0) {
        return x20_sps_seen_ && x20_pps_seen_ && !x20_header_injected_;
    }

    const auto nal_type = data[0] & 0x1FU;
    if (nal_type == 7) {
        if (size == std::size(x20_sps) && std::equal(std::begin(x20_sps), std::end(x20_sps), data)) {
            x20_sps_seen_ = true;
        } else {
            x20_checked_non_x20_ = true;
        }
    } else if (nal_type == 8) {
        if (size == std::size(x20_pps) && std::equal(std::begin(x20_pps), std::end(x20_pps), data)) {
            x20_pps_seen_ = true;
        } else {
            x20_checked_non_x20_ = true;
        }
    }
    return x20_sps_seen_ && x20_pps_seen_ && !x20_header_injected_;
}

bool RockchipMppRtpDecoder::inject_x20_header_if_needed()
{
    if (x20_header_injected_ || x20_header_missing_) {
        return true;
    }

    std::vector<std::string> paths;
    if (const auto* override_path = std::getenv("OPENHD_GLIDE_X20_HEADER"); override_path != nullptr && override_path[0] != '\0') {
        paths.emplace_back(override_path);
    }
    paths.emplace_back("/usr/share/openhd-glide/x20_header.h264");
    paths.emplace_back("/usr/local/share/openhd-glide/x20_header.h264");
    paths.emplace_back("/usr/local/bin/x20_header.h264");

    for (const auto& path : paths) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            continue;
        }
        std::vector<std::uint8_t> header(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        if (header.empty()) {
            continue;
        }
        if (!submit_packet(header.data(), header.size(), 0)) {
            return false;
        }
        x20_header_injected_ = true;
        ++x20_header_injections_;
        return true;
    }

    x20_header_missing_ = true;
    return true;
}

void RockchipMppRtpDecoder::feed_loop()
{
    std::uint8_t packet[65536];
    while (running_.load(std::memory_order_acquire)) {
        const auto received = recv(socket_fd_, packet, sizeof(packet), 0);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            last_error_ = std::string("failed to receive RKMPP RTP packet: ") + std::strerror(errno);
            running_.store(false, std::memory_order_release);
            break;
        }
        if (received == 0) {
            continue;
        }
        if (!handle_rtp_packet(packet, static_cast<std::size_t>(received))) {
            if (last_error_.empty()) {
                last_error_ = "native RKMPP RTP feed stopped after packet submit failure";
            }
            running_.store(false, std::memory_order_release);
            break;
        }
    }
}

bool RockchipMppRtpDecoder::submit_packet(const std::uint8_t* data, std::size_t size, std::int64_t pts)
{
    if (data == nullptr || size == 0 || mpi_ == nullptr || ctx_ == nullptr) {
        return false;
    }
    if (mjpeg_) {
        return false;
    }
    MppPacket packet {};
    MppBuffer input_buffer {};
    if (mpp_packet_init(&packet, const_cast<std::uint8_t*>(data), size) != MPP_OK) {
        last_error_ = "failed to allocate RKMPP input packet";
        return false;
    }
    mpp_packet_set_pos(packet, const_cast<std::uint8_t*>(data));
    mpp_packet_set_length(packet, size);
    mpp_packet_set_pts(packet, static_cast<RK_S64>(pts));

    const auto start = std::chrono::steady_clock::now();
    MPP_RET ret {};
    while (running_.load(std::memory_order_acquire)) {
        ret = as_mpi(mpi_)->decode_put_packet(as_ctx(ctx_), packet);
        if (ret == MPP_OK) {
            {
                std::lock_guard lock(mutex_);
                ++parsed_units_;
                ++submitted_packets_;
            }
            mpp_packet_deinit(&packet);
            return true;
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::milliseconds(100)) {
            {
                std::lock_guard lock(mutex_);
                ++submit_stalls_;
            }
            std::ostringstream error;
            error << "RKMPP decode_put_packet stalled for 100ms"
                  << " last_ret=" << static_cast<int>(ret)
                  << " packet_size=" << size
                  << " pts=" << pts
                  << ' ' << stats();
            last_error_ = error.str();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    mpp_packet_deinit(&packet);
    if (input_buffer != nullptr) {
        mpp_buffer_put(input_buffer);
    }
    return false;
}

bool RockchipMppRtpDecoder::configure_h26x_output_group(void* info_frame_ptr)
{
    if (info_frame_ptr == nullptr || mpi_ == nullptr || ctx_ == nullptr) {
        return false;
    }
    if (output_group_ != nullptr) {
        return true;
    }

    auto info_frame = as_frame(info_frame_ptr);
    MppBufferGroup group {};
    auto ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) {
        ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION);
    }
    if (ret != MPP_OK || group == nullptr) {
        last_error_ = "failed to allocate RKMPP H.26x output buffer group";
        return false;
    }

    auto buffer_size = static_cast<std::size_t>(mpp_frame_get_buf_size(info_frame));
    if (buffer_size == 0) {
        const auto hstride = static_cast<std::size_t>(mpp_frame_get_hor_stride(info_frame));
        const auto vstride = static_cast<std::size_t>(mpp_frame_get_ver_stride(info_frame));
        buffer_size = hstride * vstride * 2U;
    }
    constexpr std::size_t output_buffer_count = 16;
    ret = mpp_buffer_group_limit_config(group, buffer_size, output_buffer_count);
    if (ret != MPP_OK) {
        mpp_buffer_group_put(group);
        last_error_ = "failed to configure RKMPP H.26x output buffer group";
        return false;
    }
    ret = as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_EXT_BUF_GROUP, group);
    if (ret != MPP_OK) {
        mpp_buffer_group_put(group);
        last_error_ = "failed to register RKMPP H.26x output buffer group";
        return false;
    }
    output_group_ = group;
    return true;
}

bool RockchipMppRtpDecoder::submit_mjpeg_task(const std::uint8_t* data, std::size_t size, std::int64_t pts, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0) {
        last_error_ = "RKMPP MJPEG dimensions are not known";
        return false;
    }
    if (input_group_ == nullptr) {
        MppBufferGroup group {};
        auto ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK) {
            ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION);
        }
        if (ret != MPP_OK) {
            ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_NORMAL);
        }
        if (ret != MPP_OK || group == nullptr) {
            last_error_ = "failed to allocate RKMPP MJPEG input buffer group";
            return false;
        }
        input_group_ = group;
    }
    if (output_group_ == nullptr) {
        MppBufferGroup group {};
        auto ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM);
        if (ret != MPP_OK) {
            ret = mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_ION);
        }
        if (ret != MPP_OK || group == nullptr) {
            last_error_ = "failed to allocate RKMPP MJPEG output buffer group";
            return false;
        }
        output_group_ = group;
    }

    MppBuffer input_buffer {};
    auto* input_group = static_cast<MppBufferGroup>(input_group_);
    if (mpp_buffer_get(input_group, &input_buffer, size) != MPP_OK || input_buffer == nullptr) {
        last_error_ = "failed to allocate RKMPP MJPEG input buffer";
        return false;
    }
    auto* input_ptr = mpp_buffer_get_ptr(input_buffer);
    if (input_ptr == nullptr) {
        last_error_ = "RKMPP MJPEG input buffer is not CPU-mappable";
        mpp_buffer_put(input_buffer);
        return false;
    }
    std::memcpy(input_ptr, data, size);

    MppPacket packet {};
    if (mpp_packet_init_with_buffer(&packet, input_buffer) != MPP_OK) {
        last_error_ = "failed to allocate RKMPP MJPEG input packet";
        mpp_buffer_put(input_buffer);
        return false;
    }
    mpp_packet_set_pos(packet, input_ptr);
    mpp_packet_set_length(packet, size);
    mpp_packet_set_pts(packet, static_cast<RK_S64>(pts));

    const auto hstride = (width + 15U) & ~15U;
    const auto vstride = (height + 15U) & ~15U;
    const auto output_size = static_cast<std::size_t>(hstride) * vstride * 4U;
    MppBuffer output_buffer {};
    auto* output_group = static_cast<MppBufferGroup>(output_group_);
    if (mpp_buffer_get(output_group, &output_buffer, output_size) != MPP_OK || output_buffer == nullptr) {
        last_error_ = "failed to allocate RKMPP MJPEG output buffer";
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }

    MppFrame frame {};
    if (mpp_frame_init(&frame) != MPP_OK || frame == nullptr) {
        last_error_ = "failed to allocate RKMPP MJPEG output frame";
        mpp_buffer_put(output_buffer);
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }
    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, hstride);
    mpp_frame_set_ver_stride(frame, vstride);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, output_buffer);

    MppTask task {};
    auto ret = as_mpi(mpi_)->poll(as_ctx(ctx_), MPP_PORT_INPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) {
        last_error_ = "RKMPP MJPEG input poll failed ret=" + std::to_string(static_cast<int>(ret));
        release_frame(reinterpret_cast<void*&>(frame));
        mpp_buffer_put(output_buffer);
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }
    ret = as_mpi(mpi_)->dequeue(as_ctx(ctx_), MPP_PORT_INPUT, &task);
    if (ret != MPP_OK || task == nullptr) {
        last_error_ = "RKMPP MJPEG input dequeue failed ret=" + std::to_string(static_cast<int>(ret));
        release_frame(reinterpret_cast<void*&>(frame));
        mpp_buffer_put(output_buffer);
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }
    mpp_task_meta_set_packet(task, KEY_INPUT_PACKET, packet);
    mpp_task_meta_set_frame(task, KEY_OUTPUT_FRAME, frame);
    ret = as_mpi(mpi_)->enqueue(as_ctx(ctx_), MPP_PORT_INPUT, task);
    if (ret != MPP_OK) {
        last_error_ = "RKMPP MJPEG input enqueue failed ret=" + std::to_string(static_cast<int>(ret));
        release_frame(reinterpret_cast<void*&>(frame));
        mpp_buffer_put(output_buffer);
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }

    ret = as_mpi(mpi_)->poll(as_ctx(ctx_), MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
    if (ret != MPP_OK) {
        last_error_ = "RKMPP MJPEG output poll failed ret=" + std::to_string(static_cast<int>(ret));
        release_frame(reinterpret_cast<void*&>(frame));
        mpp_buffer_put(output_buffer);
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }
    task = nullptr;
    ret = as_mpi(mpi_)->dequeue(as_ctx(ctx_), MPP_PORT_OUTPUT, &task);
    if (ret != MPP_OK || task == nullptr) {
        last_error_ = "RKMPP MJPEG output dequeue failed ret=" + std::to_string(static_cast<int>(ret));
        release_frame(reinterpret_cast<void*&>(frame));
        mpp_buffer_put(output_buffer);
        mpp_packet_deinit(&packet);
        mpp_buffer_put(input_buffer);
        return false;
    }
    MppFrame output_frame {};
    mpp_task_meta_get_frame(task, KEY_OUTPUT_FRAME, &output_frame);
    as_mpi(mpi_)->enqueue(as_ctx(ctx_), MPP_PORT_OUTPUT, task);

    {
        std::lock_guard lock(mutex_);
        while (ready_frames_.size() >= 4) {
            auto dropped = ready_frames_.front();
            ready_frames_.pop_front();
            release_frame(dropped);
            ++dropped_decoded_frames_;
        }
        ready_frames_.push_back(output_frame != nullptr ? output_frame : frame);
        ++decoded_frames_;
        ++parsed_units_;
        ++submitted_packets_;
    }
    mpp_buffer_put(output_buffer);
    mpp_packet_deinit(&packet);
    mpp_buffer_put(input_buffer);
    return true;
}

void RockchipMppRtpDecoder::mjpeg_task_loop()
{
    while (running_.load(std::memory_order_acquire)) {
        MjpegAccessUnit unit;
        {
            std::unique_lock lock(mutex_);
            mjpeg_units_available_.wait_for(lock, std::chrono::milliseconds(10), [&]() {
                return !running_.load(std::memory_order_acquire) || !mjpeg_units_.empty();
            });
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            if (mjpeg_units_.empty()) {
                continue;
            }
            while (mjpeg_units_.size() > 1) {
                mjpeg_units_.pop_front();
                ++dropped_decoded_frames_;
            }
            unit = std::move(mjpeg_units_.front());
            mjpeg_units_.pop_front();
        }
        if (!submit_mjpeg_task(unit.data.data(), unit.data.size(), unit.pts, unit.width, unit.height)) {
            running_.store(false, std::memory_order_release);
            mjpeg_units_available_.notify_all();
            break;
        }
    }
}

void RockchipMppRtpDecoder::frame_loop()
{
    while (running_.load(std::memory_order_acquire)) {
        MppFrame frame {};
        const auto ret = mpi_ != nullptr ? as_mpi(mpi_)->decode_get_frame(as_ctx(ctx_), &frame) : MPP_NOK;
        if (!running_.load(std::memory_order_acquire)) {
            release_frame(frame);
            break;
        }
        if (ret != MPP_OK || frame == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (mpp_frame_get_info_change(frame)) {
            if (!configure_h26x_output_group(frame)) {
                release_frame(frame);
                running_.store(false, std::memory_order_release);
                break;
            }
            as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
            release_frame(frame);
            continue;
        }
        if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
            release_frame(frame);
            continue;
        }
        if (mpp_frame_get_buffer(frame) == nullptr) {
            release_frame(frame);
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            while (ready_frames_.size() >= 4) {
                auto dropped = ready_frames_.front();
                ready_frames_.pop_front();
                release_frame(dropped);
                ++dropped_decoded_frames_;
            }
                ready_frames_.push_back(frame);
            ++decoded_frames_;
        }
    }
}

bool RockchipMppRtpDecoder::frame_to_dmabuf(void* frame_ptr, glide::dev::DmabufVideoFrame& out)
{
    auto frame = as_frame(frame_ptr);
    auto* buffer = mpp_frame_get_buffer(frame);
    if (buffer == nullptr) {
        last_error_ = "MPP decoded frame is missing a buffer";
        return false;
    }
    MppBufferInfo info {};
    if (mpp_buffer_info_get(buffer, &info) != MPP_OK || info.fd < 0) {
        last_error_ = "MPP decoded frame buffer is not fd-backed";
        return false;
    }
    const auto width = static_cast<std::uint32_t>(mpp_frame_get_width(frame));
    const auto height = static_cast<std::uint32_t>(mpp_frame_get_height(frame));
    const auto hstride = static_cast<std::uint32_t>(mpp_frame_get_hor_stride(frame));
    const auto vstride = static_cast<std::uint32_t>(mpp_frame_get_ver_stride(frame));
    const auto fmt = mpp_frame_get_fmt(frame);
    const auto base_fmt = static_cast<MppFrameFormat>(fmt & MPP_FRAME_FMT_MASK);
    const auto fbc = static_cast<std::uint32_t>(fmt & MPP_FRAME_FBC_MASK);
#ifdef MPP_FRAME_TILE_FLAG
    const auto tiled = (fmt & MPP_FRAME_TILE_FLAG) != 0;
#else
    const auto tiled = false;
#endif
    const auto buffer_size = static_cast<std::uint64_t>(mpp_frame_get_buf_size(frame));
    if (width == 0 || height == 0 || hstride == 0 || vstride == 0) {
        last_error_ = "MPP decoded frame has invalid dimensions";
        return false;
    }
    if (base_fmt != MPP_FMT_YUV420SP || fbc != MPP_FRAME_FBC_NONE || tiled) {
        std::ostringstream error;
        error << "MPP decoded frame is not linear NV12"
              << " fmt=0x" << std::hex << static_cast<std::uint32_t>(fmt)
              << " base=0x" << static_cast<std::uint32_t>(base_fmt)
              << " fbc=0x" << fbc
              << " tiled=" << std::dec << (tiled ? 1 : 0);
        last_error_ = error.str();
        return false;
    }
    out = {};
    out.width = width;
    out.height = height;
    out.drm_format = DRM_FORMAT_NV12;
    out.plane_count = 2;
    out.fds[0] = info.fd;
    out.fds[1] = info.fd;
    out.strides[0] = hstride;
    out.strides[1] = hstride;
    out.offsets[0] = 0;
    out.offsets[1] = hstride * vstride;
    if (!logged_first_layout_) {
        std::ostringstream layout;
        layout << "first native RKMPP DMABUF layout"
               << " fmt=0x" << std::hex << static_cast<std::uint32_t>(fmt)
               << std::dec
               << " width=" << width
               << " height=" << height
               << " hstride=" << hstride
               << " vstride=" << vstride
               << " buffer_size=" << buffer_size
               << " fd=" << info.fd
               << " drm_format=NV12"
               << " luma_height=" << vstride
               << " offset_uv=" << out.offsets[1];
        glide::log(glide::LogLevel::info, "OpenHD-Glide", layout.str());
        logged_first_layout_ = true;
    }
    return true;
}

void RockchipMppRtpDecoder::release_frame(void*& frame)
{
    if (frame != nullptr) {
        auto native_frame = as_frame(frame);
        mpp_frame_deinit(&native_frame);
        frame = nullptr;
    }
}
#endif

void RockchipMppRtpDecoder::cleanup()
{
#if OPENHD_GLIDE_HAS_RKMPP
    running_.store(false, std::memory_order_release);
    mjpeg_units_available_.notify_all();
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
    }
    if (feed_thread_.joinable()) {
        feed_thread_.join();
    }
    if (frame_thread_.joinable()) {
        frame_thread_.join();
    }
    {
        std::lock_guard lock(mutex_);
        for (auto& frame : ready_frames_) {
            release_frame(frame);
        }
        ready_frames_.clear();
        mjpeg_units_.clear();
    }
    release_frame(pending_presented_frame_);
    release_frame(current_frame_);
    if (input_group_ != nullptr) {
        auto* group = static_cast<MppBufferGroup>(input_group_);
        mpp_buffer_group_put(group);
        input_group_ = nullptr;
    }
    if (output_group_ != nullptr) {
        auto* group = static_cast<MppBufferGroup>(output_group_);
        mpp_buffer_group_put(group);
        output_group_ = nullptr;
    }
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (mpi_ != nullptr && ctx_ != nullptr) {
        as_mpi(mpi_)->reset(as_ctx(ctx_));
    }
    if (ctx_ != nullptr) {
        mpp_destroy(as_ctx(ctx_));
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
#endif
}

} // namespace glide::video
