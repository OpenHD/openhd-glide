/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace glide::net {

constexpr std::uint16_t discovery_port = 5620;

struct Peer {
    std::string address;
    std::string hostname;
    std::uint16_t video_port { 5600 };
    std::chrono::steady_clock::time_point last_seen {};
};

struct DiscoveryOptions {
    std::uint16_t discovery_udp_port { discovery_port };
    std::uint16_t video_udp_port { 5600 };
    std::string role { "glide" };
};

struct PointToPointOptions {
    std::string interface_name;
    std::string role { "ground" };
};

class DiscoveryService {
public:
    bool start(const DiscoveryOptions& options = {});
    void stop();
    bool running() const;
    const std::string& last_error() const;

    void start_scan(std::chrono::milliseconds duration = std::chrono::milliseconds(2500));
    std::vector<std::string> poll_state_lines();
    const std::vector<Peer>& peers() const;

private:
    bool send_broadcast_probe();
    void handle_packet(const std::string& payload, const std::string& sender_address);
    void upsert_peer(Peer peer);

    std::intptr_t fd_ { -1 };
    DiscoveryOptions options_;
    std::string hostname_ { "glide" };
    std::string last_error_;
    std::vector<Peer> peers_;
    std::vector<std::string> pending_lines_;
    std::chrono::steady_clock::time_point scan_until_ {};
    std::chrono::steady_clock::time_point next_probe_ {};
};

std::vector<Peer> scan_blocking(
    std::uint16_t discovery_udp_port = discovery_port,
    std::uint16_t video_udp_port = 5600,
    std::chrono::milliseconds duration = std::chrono::milliseconds(2500));

std::string configure_point_to_point(const PointToPointOptions& options);

} // namespace glide::net
