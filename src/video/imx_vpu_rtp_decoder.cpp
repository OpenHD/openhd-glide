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
 ******************************************************************************/

#include "video/imx_vpu_rtp_decoder.hpp"

#if OPENHD_GLIDE_HAS_IMXVPU
extern "C" {
#include <imxvpuapi2/imxvpuapi2.h>
}

#include <arpa/inet.h>
#include <drm_fourcc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#endif

namespace glide::video {

bool imx_vpu_decoder_available()
{
#if OPENHD_GLIDE_HAS_IMXVPU
    return true;
#else
    return false;
#endif
}

ImxVpuRtpDecoder::~ImxVpuRtpDecoder()
{
    cleanup();
}

bool ImxVpuRtpDecoder::start(std::uint16_t udp_port, const std::string& codec)
{
#if OPENHD_GLIDE_HAS_IMXVPU
    return init_decoder(codec) && init_socket(udp_port);
#else
    (void)udp_port;
    (void)codec;
    last_error_ = "native i.MX VPU decoder support was not found at build time";
    return false;
#endif
}

bool ImxVpuRtpDecoder::poll(glide::dev::DmabufVideoFrame& frame)
{
#if OPENHD_GLIDE_HAS_IMXVPU
    last_error_.clear();
    if (current_frame_ == nullptr && deliver_ready_frame(frame)) {
        return true;
    }

    std::array<std::uint8_t, 65536> packet {};
    for (;;) {
        const auto received = recv(socket_fd_, packet.data(), packet.size(), MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            last_error_ = std::string("failed to receive i.MX VPU RTP packet: ") + std::strerror(errno);
            return false;
        }
        if (received == 0) {
            break;
        }
        if (!handle_rtp_packet(packet.data(), static_cast<std::size_t>(received))) {
            return false;
        }
        if (current_frame_ == nullptr && deliver_ready_frame(frame)) {
            return true;
        }
    }
    return current_frame_ == nullptr && deliver_ready_frame(frame);
#else
    (void)frame;
    return false;
#endif
}

void ImxVpuRtpDecoder::mark_presented()
{
#if OPENHD_GLIDE_HAS_IMXVPU
    return_frame(current_frame_);
#endif
}

std::string ImxVpuRtpDecoder::stats() const
{
#if OPENHD_GLIDE_HAS_IMXVPU
    return "rtp_packets=" + std::to_string(rtp_packets_)
        + " rtp_sequence_gaps=" + std::to_string(sequence_gaps_)
        + " late_or_duplicate_packets=" + std::to_string(late_packets_)
        + " incomplete_fragments=" + std::to_string(incomplete_fragments_)
        + " access_units=" + std::to_string(access_units_)
        + " decoded_frames=" + std::to_string(decoded_frames_)
        + " skipped_frames=" + std::to_string(skipped_frames_)
        + " dropped_frames=" + std::to_string(dropped_frames_)
        + (output_description_.empty() ? std::string {} : " " + output_description_);
#else
    return {};
#endif
}

const std::string& ImxVpuRtpDecoder::last_error() const
{
    return last_error_;
}

#if OPENHD_GLIDE_HAS_IMXVPU
namespace {

constexpr std::array<std::uint8_t, 4> start_code { 0x00, 0x00, 0x00, 0x01 };

std::string decoder_error(const char* operation, ImxVpuApiDecReturnCodes code)
{
    return std::string(operation) + ": " + imx_vpu_api_dec_return_code_string(code);
}

} // namespace

bool ImxVpuRtpDecoder::init_socket(std::uint16_t udp_port)
{
    socket_fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd_ < 0) {
        last_error_ = std::string("failed to create i.MX VPU UDP socket: ") + std::strerror(errno);
        return false;
    }

    const int receive_buffer = 32 * 1024 * 1024;
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(udp_port);
    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = std::string("failed to bind i.MX VPU RTP socket: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool ImxVpuRtpDecoder::init_decoder(const std::string& codec)
{
    h265_ = codec == "h265" || codec == "hevc";
    if (!h265_ && codec != "h264") {
        last_error_ = "native i.MX VPU supports --view-udp-codec h264 or h265";
        return false;
    }

    const auto* global_info = imx_vpu_api_dec_get_global_info();
    if (global_info == nullptr || !(global_info->flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_HAS_DECODER)) {
        last_error_ = "i.MX VPU API reports no hardware decoder";
        return false;
    }
    frames_from_pool_ = (global_info->flags & IMX_VPU_API_DEC_GLOBAL_INFO_FLAG_DECODED_FRAMES_ARE_FROM_BUFFER_POOL) != 0;
    if (!frames_from_pool_) {
        last_error_ = "i.MX VPU output is not backed by its DMA framebuffer pool";
        return false;
    }

    int error {};
    allocator_ = imx_dma_buffer_allocator_new(&error);
    if (allocator_ == nullptr) {
        last_error_ = std::string("failed to create i.MX DMA allocator: ") + std::strerror(error);
        return false;
    }

    if (global_info->min_required_stream_buffer_size != 0) {
        stream_buffer_ = imx_dma_buffer_allocate(
            allocator_,
            global_info->min_required_stream_buffer_size,
            global_info->required_stream_buffer_physaddr_alignment,
            &error);
        if (stream_buffer_ == nullptr) {
            last_error_ = std::string("failed to allocate i.MX VPU stream buffer: ") + std::strerror(error);
            return false;
        }
    }

    ImxVpuApiDecOpenParams params {};
    params.compression_format = h265_ ? IMX_VPU_API_COMPRESSION_FORMAT_H265 : IMX_VPU_API_COMPRESSION_FORMAT_H264;
    params.flags = IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_ENABLE_FRAME_REORDERING
        | IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SEMI_PLANAR_COLOR_FORMAT
        | IMX_VPU_API_DEC_OPEN_PARAMS_FLAG_USE_SUGGESTED_COLOR_FORMAT;
    params.suggested_color_format = IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT;
    const auto result = imx_vpu_api_dec_open(&decoder_, &params, stream_buffer_);
    if (result != IMX_VPU_API_DEC_RETURN_CODE_OK) {
        last_error_ = decoder_error("failed to open native i.MX VPU decoder", result);
        return false;
    }
    return true;
}

bool ImxVpuRtpDecoder::handle_rtp_packet(const std::uint8_t* packet, std::size_t size)
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

    ++rtp_packets_;
    if (have_sequence_) {
        const auto delta = static_cast<std::int16_t>(sequence - expected_sequence_);
        if (delta < 0) {
            const auto timestamp_delta = have_timestamp_ ? static_cast<std::int32_t>(timestamp - last_timestamp_) : 0;
            if (delta < -1000 || timestamp_delta < -90000) {
                fragment_.clear();
                access_unit_.clear();
                have_access_unit_ = false;
            } else {
                ++late_packets_;
                return true;
            }
        } else if (delta > 0) {
            ++sequence_gaps_;
            fragment_.clear();
            access_unit_.clear();
            have_access_unit_ = false;
        }
    }
    expected_sequence_ = static_cast<std::uint16_t>(sequence + 1U);
    have_sequence_ = true;
    last_timestamp_ = timestamp;
    have_timestamp_ = true;

    return h265_
        ? append_h265_payload(packet + offset, payload_size, marker, timestamp)
        : append_h264_payload(packet + offset, payload_size, marker, timestamp);
}

bool ImxVpuRtpDecoder::append_h264_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint32_t timestamp)
{
    if (size == 0) {
        return true;
    }
    const auto nal_type = payload[0] & 0x1FU;
    if (nal_type >= 1 && nal_type <= 23) {
        return queue_nal(payload, size, timestamp) && (!marker || submit_access_unit());
    }
    if (nal_type == 24) {
        std::size_t offset = 1;
        while (offset + 2U <= size) {
            const auto nal_size = (static_cast<std::size_t>(payload[offset]) << 8U) | payload[offset + 1U];
            offset += 2U;
            if (offset + nal_size > size) {
                ++incomplete_fragments_;
                return true;
            }
            if (!queue_nal(payload + offset, nal_size, timestamp)) {
                return false;
            }
            offset += nal_size;
        }
        return !marker || submit_access_unit();
    }
    if (nal_type == 28 && size >= 2) {
        const auto fu_header = payload[1];
        const bool start = (fu_header & 0x80U) != 0;
        const bool end = (fu_header & 0x40U) != 0;
        if (start) {
            fragment_.clear();
            fragment_.push_back(static_cast<std::uint8_t>((payload[0] & 0xE0U) | (fu_header & 0x1FU)));
            fragment_timestamp_ = timestamp;
        } else if (fragment_.empty() || fragment_timestamp_ != timestamp) {
            ++incomplete_fragments_;
            return true;
        }
        fragment_.insert(fragment_.end(), payload + 2, payload + size);
        if (end) {
            const auto result = queue_nal(fragment_.data(), fragment_.size(), timestamp);
            fragment_.clear();
            return result && (!marker || submit_access_unit());
        }
    }
    return true;
}

bool ImxVpuRtpDecoder::append_h265_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint32_t timestamp)
{
    if (size < 2) {
        return true;
    }
    const auto nal_type = (payload[0] >> 1U) & 0x3FU;
    if (nal_type <= 47) {
        return queue_nal(payload, size, timestamp) && (!marker || submit_access_unit());
    }
    if (nal_type == 48) {
        std::size_t offset = 2;
        while (offset + 2U <= size) {
            const auto nal_size = (static_cast<std::size_t>(payload[offset]) << 8U) | payload[offset + 1U];
            offset += 2U;
            if (offset + nal_size > size) {
                ++incomplete_fragments_;
                return true;
            }
            if (!queue_nal(payload + offset, nal_size, timestamp)) {
                return false;
            }
            offset += nal_size;
        }
        return !marker || submit_access_unit();
    }
    if (nal_type == 49 && size >= 3) {
        const auto fu_header = payload[2];
        const bool start = (fu_header & 0x80U) != 0;
        const bool end = (fu_header & 0x40U) != 0;
        if (start) {
            fragment_.clear();
            fragment_.push_back(static_cast<std::uint8_t>((payload[0] & 0x81U) | ((fu_header & 0x3FU) << 1U)));
            fragment_.push_back(payload[1]);
            fragment_timestamp_ = timestamp;
        } else if (fragment_.empty() || fragment_timestamp_ != timestamp) {
            ++incomplete_fragments_;
            return true;
        }
        fragment_.insert(fragment_.end(), payload + 3, payload + size);
        if (end) {
            const auto result = queue_nal(fragment_.data(), fragment_.size(), timestamp);
            fragment_.clear();
            return result && (!marker || submit_access_unit());
        }
    }
    return true;
}

bool ImxVpuRtpDecoder::queue_nal(const std::uint8_t* data, std::size_t size, std::uint32_t timestamp)
{
    if (data == nullptr || size == 0) {
        return true;
    }
    if (have_access_unit_ && access_unit_timestamp_ != timestamp && !submit_access_unit()) {
        return false;
    }
    if (!have_access_unit_) {
        have_access_unit_ = true;
        access_unit_timestamp_ = timestamp;
    }
    access_unit_.insert(access_unit_.end(), start_code.begin(), start_code.end());
    access_unit_.insert(access_unit_.end(), data, data + size);
    return true;
}

bool ImxVpuRtpDecoder::submit_access_unit()
{
    if (!have_access_unit_ || access_unit_.empty()) {
        have_access_unit_ = false;
        access_unit_.clear();
        return true;
    }

    ImxVpuApiEncodedFrame encoded {};
    encoded.data = access_unit_.data();
    encoded.data_size = access_unit_.size();
    encoded.pts = access_unit_timestamp_;
    encoded.dts = access_unit_timestamp_;
    const auto result = imx_vpu_api_dec_push_encoded_frame(decoder_, &encoded);
    if (result != IMX_VPU_API_DEC_RETURN_CODE_OK) {
        last_error_ = decoder_error("failed to push access unit into i.MX VPU", result);
        return false;
    }
    ++access_units_;
    have_access_unit_ = false;
    access_unit_.clear();
    return decode_available();
}

bool ImxVpuRtpDecoder::decode_available()
{
    for (unsigned int iteration = 0; iteration < 128; ++iteration) {
        ImxVpuApiDecOutputCodes output {};
        const auto result = imx_vpu_api_dec_decode(decoder_, &output);
        if (result != IMX_VPU_API_DEC_RETURN_CODE_OK) {
            last_error_ = decoder_error("native i.MX VPU decode failed", result);
            return false;
        }

        switch (output) {
        case IMX_VPU_API_DEC_OUTPUT_CODE_NO_OUTPUT_YET_AVAILABLE:
            break;
        case IMX_VPU_API_DEC_OUTPUT_CODE_NEW_STREAM_INFO_AVAILABLE:
            if (!update_stream_info()) {
                return false;
            }
            break;
        case IMX_VPU_API_DEC_OUTPUT_CODE_NEED_ADDITIONAL_FRAMEBUFFER:
            if (!add_framebuffers(1)) {
                return false;
            }
            break;
        case IMX_VPU_API_DEC_OUTPUT_CODE_DECODED_FRAME_AVAILABLE: {
            ImxVpuApiRawFrame raw {};
            const auto get_result = imx_vpu_api_dec_get_decoded_frame(decoder_, &raw);
            if (get_result != IMX_VPU_API_DEC_RETURN_CODE_OK) {
                last_error_ = decoder_error("failed to get decoded i.MX VPU frame", get_result);
                return false;
            }
            ready_frames_.push_back(raw.fb_dma_buffer);
            ++decoded_frames_;
            while (ready_frames_.size() > 6U) {
                auto* stale = ready_frames_.front();
                ready_frames_.pop_front();
                return_frame(stale);
                ++dropped_frames_;
            }
            break;
        }
        case IMX_VPU_API_DEC_OUTPUT_CODE_FRAME_SKIPPED: {
            ImxVpuApiDecSkippedFrameReasons reason {};
            void* context {};
            std::uint64_t pts {};
            std::uint64_t dts {};
            imx_vpu_api_dec_get_skipped_frame_info(decoder_, &reason, &context, &pts, &dts);
            ++skipped_frames_;
            break;
        }
        case IMX_VPU_API_DEC_OUTPUT_CODE_MORE_INPUT_DATA_NEEDED:
            return true;
        case IMX_VPU_API_DEC_OUTPUT_CODE_VIDEO_PARAMETERS_CHANGED:
            last_error_ = "i.MX VPU stream parameters changed; restart the native decoder";
            return false;
        case IMX_VPU_API_DEC_OUTPUT_CODE_EOS:
            last_error_ = "i.MX VPU decoder reached end of stream";
            return false;
        }
    }
    last_error_ = "i.MX VPU decoder did not request more input after 128 decode steps";
    return false;
}

bool ImxVpuRtpDecoder::update_stream_info()
{
    const auto* info = imx_vpu_api_dec_get_stream_info(decoder_);
    if (info == nullptr) {
        last_error_ = "i.MX VPU returned no stream information";
        return false;
    }
    if (info->color_format != IMX_VPU_API_COLOR_FORMAT_SEMI_PLANAR_YUV420_8BIT) {
        last_error_ = std::string("i.MX VPU output cannot be imported as linear NV12: ")
            + imx_vpu_api_color_format_string(info->color_format);
        return false;
    }

    clear_decoder_frames();
    deallocate_pool();
    const auto& metrics = info->decoded_frame_framebuffer_metrics;
    width_ = static_cast<std::uint32_t>(metrics.actual_frame_width);
    height_ = static_cast<std::uint32_t>(metrics.actual_frame_height);
    y_stride_ = static_cast<std::uint32_t>(metrics.y_stride);
    uv_stride_ = static_cast<std::uint32_t>(metrics.uv_stride);
    y_offset_ = static_cast<std::uint32_t>(metrics.y_offset);
    uv_offset_ = static_cast<std::uint32_t>(metrics.u_offset);

    yuv_color_space_ = glide::dev::DmabufYuvColorSpace::unspecified;
    if (info->flags & IMX_VPU_API_DEC_STREAM_INFO_FLAG_COLOR_DESCRIPTION_AVAILABLE) {
        switch (info->color_description.matrix_coefficients) {
        case 1:
            yuv_color_space_ = glide::dev::DmabufYuvColorSpace::rec709;
            break;
        case 5:
        case 6:
            yuv_color_space_ = glide::dev::DmabufYuvColorSpace::rec601;
            break;
        case 9:
        case 10:
            yuv_color_space_ = glide::dev::DmabufYuvColorSpace::rec2020;
            break;
        default:
            break;
        }
    }
    yuv_range_ = info->video_full_range_flag
        ? glide::dev::DmabufYuvRange::full
        : glide::dev::DmabufYuvRange::narrow;

    const auto color_space_name =
        yuv_color_space_ == glide::dev::DmabufYuvColorSpace::rec601 ? "bt601"
        : yuv_color_space_ == glide::dev::DmabufYuvColorSpace::rec709 ? "bt709"
        : yuv_color_space_ == glide::dev::DmabufYuvColorSpace::rec2020 ? "bt2020"
                                                                      : "unspecified";
    std::ostringstream output;
    output << "output=linear-nv12"
           << " size=" << width_ << 'x' << height_
           << " strides=" << y_stride_ << '/' << uv_stride_
           << " offsets=" << y_offset_ << '/' << uv_offset_
           << " color=" << color_space_name
           << " range=" << (yuv_range_ == glide::dev::DmabufYuvRange::full ? "full" : "narrow");
    output_description_ = output.str();
    return add_framebuffers(info->min_num_required_framebuffers);
}

bool ImxVpuRtpDecoder::add_framebuffers(std::size_t count)
{
    const auto* info = imx_vpu_api_dec_get_stream_info(decoder_);
    if (info == nullptr) {
        last_error_ = "cannot allocate i.MX VPU framebuffers without stream information";
        return false;
    }

    const auto old_size = framebuffer_pool_.size();
    framebuffer_pool_.resize(old_size + count, nullptr);
    std::vector<void*> contexts(count, nullptr);
    int error {};
    for (std::size_t index = old_size; index < framebuffer_pool_.size(); ++index) {
        framebuffer_pool_[index] = imx_dma_buffer_allocate(
            allocator_,
            info->min_fb_pool_framebuffer_size,
            info->fb_pool_framebuffer_alignment,
            &error);
        if (framebuffer_pool_[index] == nullptr) {
            last_error_ = std::string("failed to allocate i.MX VPU framebuffer: ") + std::strerror(error);
            return false;
        }
    }
    if (count == 0) {
        return true;
    }
    const auto result = imx_vpu_api_dec_add_framebuffers_to_pool(
        decoder_, framebuffer_pool_.data() + old_size, contexts.data(), count);
    if (result != IMX_VPU_API_DEC_RETURN_CODE_OK) {
        last_error_ = decoder_error("failed to add i.MX VPU framebuffer pool", result);
        return false;
    }
    return true;
}

bool ImxVpuRtpDecoder::deliver_ready_frame(glide::dev::DmabufVideoFrame& frame)
{
    if (ready_frames_.empty()) {
        return false;
    }
    current_frame_ = ready_frames_.front();
    ready_frames_.pop_front();
    const auto fd = imx_dma_buffer_get_fd(current_frame_);
    if (fd < 0) {
        last_error_ = "decoded i.MX VPU frame has no DMA-BUF file descriptor";
        return false;
    }

    frame = {};
    frame.fds = { -1, -1, -1, -1 };
    frame.width = width_;
    frame.height = height_;
    frame.drm_format = DRM_FORMAT_NV12;
    frame.plane_count = 2;
    frame.fds[0] = fd;
    frame.fds[1] = fd;
    frame.strides[0] = y_stride_;
    frame.strides[1] = uv_stride_;
    frame.offsets[0] = y_offset_;
    frame.offsets[1] = uv_offset_;
    frame.yuv_color_space = yuv_color_space_;
    frame.yuv_range = yuv_range_;
    return true;
}

void ImxVpuRtpDecoder::return_frame(ImxDmaBuffer*& buffer)
{
    if (decoder_ != nullptr && buffer != nullptr) {
        imx_vpu_api_dec_return_framebuffer_to_decoder(decoder_, buffer);
        buffer = nullptr;
    }
}

void ImxVpuRtpDecoder::clear_decoder_frames()
{
    return_frame(current_frame_);
    while (!ready_frames_.empty()) {
        auto* buffer = ready_frames_.front();
        ready_frames_.pop_front();
        return_frame(buffer);
    }
}

void ImxVpuRtpDecoder::deallocate_pool()
{
    for (auto*& buffer : framebuffer_pool_) {
        if (buffer != nullptr) {
            imx_dma_buffer_deallocate(buffer);
            buffer = nullptr;
        }
    }
    framebuffer_pool_.clear();
}

void ImxVpuRtpDecoder::cleanup()
{
    clear_decoder_frames();
    if (decoder_ != nullptr) {
        imx_vpu_api_dec_close(decoder_);
        decoder_ = nullptr;
    }
    deallocate_pool();
    if (stream_buffer_ != nullptr) {
        imx_dma_buffer_deallocate(stream_buffer_);
        stream_buffer_ = nullptr;
    }
    if (allocator_ != nullptr) {
        imx_dma_buffer_allocator_destroy(allocator_);
        allocator_ = nullptr;
    }
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}
#else
void ImxVpuRtpDecoder::cleanup()
{
}
#endif

} // namespace glide::video
