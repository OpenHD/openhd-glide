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

#pragma once

#include "dev/kms_dmabuf_video_plane.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace glide::video {

class RockchipMppRtpDecoder {
public:
    RockchipMppRtpDecoder() = default;
    ~RockchipMppRtpDecoder();

    RockchipMppRtpDecoder(const RockchipMppRtpDecoder&) = delete;
    RockchipMppRtpDecoder& operator=(const RockchipMppRtpDecoder&) = delete;

    bool start(std::uint16_t udp_port, const std::string& codec);
    bool poll(glide::dev::DmabufVideoFrame& frame);
    void mark_presented();
    std::string stats() const;
    const std::string& last_error() const;

private:
    bool init_mpp(const std::string& codec);
    bool init_socket(std::uint16_t udp_port);
    bool configure_mpp();
    void feed_loop();
    void frame_loop();
    bool handle_rtp_packet(const std::uint8_t* packet, std::size_t size);
    bool append_h264_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint16_t sequence, std::uint32_t timestamp);
    bool append_h265_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint16_t sequence, std::uint32_t timestamp);
    bool append_mjpeg_payload(const std::uint8_t* payload, std::size_t size, bool marker, std::uint32_t timestamp);
    void queue_mjpeg_access_unit(std::uint32_t width, std::uint32_t height, std::int64_t pts);
    bool submit_nal(const std::uint8_t* data, std::size_t size, std::int64_t pts);
    bool queue_nal(const std::uint8_t* data, std::size_t size, std::uint32_t timestamp);
    bool flush_access_unit();
    bool update_x20_detection(const std::uint8_t* data, std::size_t size);
    bool inject_x20_header_if_needed();
    bool submit_packet(const std::uint8_t* data, std::size_t size, std::int64_t pts);
    bool submit_mjpeg_task(const std::uint8_t* data, std::size_t size, std::int64_t pts, std::uint32_t width, std::uint32_t height);
    void mjpeg_task_loop();
    bool frame_to_dmabuf(void* frame, glide::dev::DmabufVideoFrame& out);
    void release_frame(void*& frame);
    void cleanup();

    void* ctx_ {};
    void* mpi_ {};
    void* input_group_ {};
    void* output_group_ {};
    int socket_fd_ { -1 };
    bool h265_ {};
    bool mjpeg_ {};
    std::thread feed_thread_;
    std::thread frame_thread_;
    std::atomic<bool> running_ {};
    struct MjpegAccessUnit {
        std::vector<std::uint8_t> data;
        std::int64_t pts {};
        std::uint32_t width {};
        std::uint32_t height {};
    };
    mutable std::mutex mutex_;
    std::condition_variable mjpeg_units_available_;
    std::deque<void*> ready_frames_;
    std::deque<MjpegAccessUnit> mjpeg_units_;
    void* current_frame_ {};
    void* pending_presented_frame_ {};
    std::uint64_t parsed_units_ {};
    std::uint64_t decoded_frames_ {};
    std::uint64_t dropped_decoded_frames_ {};
    std::uint64_t rtp_packets_ {};
    std::uint64_t rtp_sequence_gaps_ {};
    std::uint64_t rtp_sequence_resyncs_ {};
    std::uint64_t late_or_duplicate_packets_ {};
    std::uint64_t incomplete_fragments_ {};
    std::uint64_t x20_header_injections_ {};
    std::uint64_t submitted_packets_ {};
    std::uint64_t submit_stalls_ {};
    bool have_sequence_ {};
    std::uint16_t expected_sequence_ {};
    bool have_rtp_timestamp_ {};
    std::uint32_t last_rtp_timestamp_ {};
    std::uint32_t current_timestamp_ {};
    bool x20_sps_seen_ {};
    bool x20_pps_seen_ {};
    bool x20_checked_non_x20_ {};
    bool x20_header_injected_ {};
    bool x20_header_missing_ {};
    bool logged_first_layout_ {};
    std::vector<std::uint8_t> fragment_;
    std::vector<std::uint8_t> access_unit_;
    bool have_access_unit_ {};
    std::uint32_t access_unit_timestamp_ {};
    std::uint32_t mjpeg_width_ {};
    std::uint32_t mjpeg_height_ {};
    std::string last_error_;
};

bool rockchip_mpp_decoder_available();

} // namespace glide::video
