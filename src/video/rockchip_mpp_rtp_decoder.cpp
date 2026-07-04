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

void append_start_code(std::vector<std::uint8_t>& output)
{
    output.insert(output.end(), std::begin(start_code), std::end(start_code));
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

bool RockchipMppRtpDecoder::start(std::uint16_t udp_port, const std::string& codec)
{
#if OPENHD_GLIDE_HAS_RKMPP
    last_error_.clear();
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
    frame_thread_ = std::thread(&RockchipMppRtpDecoder::frame_loop, this);
    return true;
#else
    (void)udp_port;
    (void)codec;
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
    const auto coding = h265_ ? MPP_VIDEO_CodingHEVC : MPP_VIDEO_CodingAVC;
    if (mpp_check_support_format(MPP_CTX_DEC, coding) != MPP_OK) {
        last_error_ = h265_ ? "Rockchip MPP does not support HEVC decode" : "Rockchip MPP does not support AVC decode";
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
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_OUTPUT_FORMAT, &output_format);
        as_mpi(mpi_)->control(as_ctx(ctx_), MPP_DEC_SET_PARSER_SPLIT_MODE, &off);
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

    return h265_
        ? append_h265_payload(packet + offset, payload_size, marker, sequence, timestamp)
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
    if (have_access_unit_ && access_unit_timestamp_ != timestamp && !flush_access_unit()) {
        return false;
    }
    if (!have_access_unit_) {
        have_access_unit_ = true;
        access_unit_timestamp_ = timestamp;
        access_unit_.clear();
    }

    if (!h265_ && update_x20_detection(data, size) && !inject_x20_header_if_needed()) {
        return false;
    }

    constexpr std::array<std::uint8_t, 4> prefix { 0x00, 0x00, 0x00, 0x01 };
    if (size >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data)) {
        access_unit_.insert(access_unit_.end(), data, data + size);
    } else {
        access_unit_.insert(access_unit_.end(), prefix.begin(), prefix.end());
        access_unit_.insert(access_unit_.end(), data, data + size);
    }
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
    MppPacket packet {};
    if (mpp_packet_init(&packet, const_cast<std::uint8_t*>(data), size) != MPP_OK) {
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
            std::lock_guard lock(mutex_);
            ++submit_stalls_;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    mpp_packet_deinit(&packet);
    return false;
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
    }
    release_frame(pending_presented_frame_);
    release_frame(current_frame_);
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
