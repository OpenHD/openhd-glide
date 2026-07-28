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

#pragma once

#include "dev/kms_dmabuf_video_plane.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

typedef struct _ImxDmaBuffer ImxDmaBuffer;
typedef struct _ImxDmaBufferAllocator ImxDmaBufferAllocator;
typedef struct _ImxVpuApiDecoder ImxVpuApiDecoder;

namespace glide::video {

class ImxVpuRtpDecoder {
public:
    ImxVpuRtpDecoder() = default;
    ~ImxVpuRtpDecoder();

    ImxVpuRtpDecoder(const ImxVpuRtpDecoder&) = delete;
    ImxVpuRtpDecoder& operator=(const ImxVpuRtpDecoder&) = delete;

    bool start(std::uint16_t udp_port, const std::string& codec);
    bool poll(glide::dev::DmabufVideoFrame& frame);
    void mark_presented();
    std::string stats() const;
    const std::string& last_error() const;

private:
    bool init_socket(std::uint16_t udp_port);
    bool init_decoder(const std::string& codec);
    bool handle_rtp_packet(const std::uint8_t* packet, std::size_t size);
    bool append_h264_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint32_t timestamp);
    bool append_h265_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint32_t timestamp);
    bool queue_nal(const std::uint8_t* data, std::size_t size, std::uint32_t timestamp);
    bool submit_access_unit();
    bool decode_available();
    bool update_stream_info();
    bool add_framebuffers(std::size_t count);
    bool deliver_ready_frame(glide::dev::DmabufVideoFrame& frame);
    void return_frame(ImxDmaBuffer*& buffer);
    void clear_decoder_frames();
    void deallocate_pool();
    void cleanup();

    int socket_fd_ { -1 };
    ImxDmaBufferAllocator* allocator_ {};
    ImxVpuApiDecoder* decoder_ {};
    ImxDmaBuffer* stream_buffer_ {};
    std::vector<ImxDmaBuffer*> framebuffer_pool_;
    std::deque<ImxDmaBuffer*> ready_frames_;
    ImxDmaBuffer* current_frame_ {};
    std::vector<std::uint8_t> fragment_;
    std::vector<std::uint8_t> access_unit_;
    bool h265_ {};
    bool frames_from_pool_ {};
    bool have_sequence_ {};
    bool have_timestamp_ {};
    bool have_access_unit_ {};
    std::uint16_t expected_sequence_ {};
    std::uint32_t last_timestamp_ {};
    std::uint32_t fragment_timestamp_ {};
    std::uint32_t access_unit_timestamp_ {};
    std::uint32_t width_ {};
    std::uint32_t height_ {};
    std::uint32_t y_stride_ {};
    std::uint32_t uv_stride_ {};
    std::uint32_t y_offset_ {};
    std::uint32_t uv_offset_ {};
    glide::dev::DmabufYuvColorSpace yuv_color_space_ { glide::dev::DmabufYuvColorSpace::unspecified };
    glide::dev::DmabufYuvRange yuv_range_ { glide::dev::DmabufYuvRange::unspecified };
    std::string output_description_;
    std::uint64_t rtp_packets_ {};
    std::uint64_t sequence_gaps_ {};
    std::uint64_t late_packets_ {};
    std::uint64_t incomplete_fragments_ {};
    std::uint64_t access_units_ {};
    std::uint64_t decoded_frames_ {};
    std::uint64_t skipped_frames_ {};
    std::uint64_t dropped_frames_ {};
    std::string last_error_;
};

bool imx_vpu_decoder_available();

} // namespace glide::video
