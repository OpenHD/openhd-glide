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

#include <cstdint>
#include <string>
#include <vector>

namespace glide::mavlink {

inline constexpr std::uint16_t openhd_ground_tcp_port = 5760;

struct UdpBridgeOptions {
    std::uint16_t listen_port { 14550 };
    std::uint8_t system_id { 255 };
    std::uint8_t component_id { 190 };
};

enum class NetworkTransport {
    udp,
    tcp,
};

class UdpBridge {
public:
    UdpBridge() = default;
    ~UdpBridge();

    UdpBridge(const UdpBridge&) = delete;
    UdpBridge& operator=(const UdpBridge&) = delete;

    bool start(UdpBridgeOptions options = {});
    bool connect_to(NetworkTransport transport, const std::string& host, std::uint16_t port);
    std::vector<std::string> poll();
    bool handle_action_line(const std::string& line);
    void close();
    bool running() const;
    std::string connection_description() const;
    const std::string& last_error() const;

private:
    bool send_param_set(const std::string& target, const std::string& name, const std::string& value);
    bool send_param_ext_set(const std::string& target, const std::string& name, const std::string& value);
    bool send_command_long(const std::string& command, const std::string& arguments);
    bool send_packet(std::uint32_t message_id, std::uint8_t crc_extra, const std::vector<std::uint8_t>& payload);

    std::intptr_t fd_ { -1 };
    UdpBridgeOptions options_ {};
    NetworkTransport transport_ { NetworkTransport::udp };
    std::string remote_host_;
    std::uint16_t remote_port_ {};
    bool listening_ {};
    std::vector<std::uint8_t> stream_buffer_;
    std::uint8_t sequence_ {};
    std::uint8_t flight_controller_system_id_ { 1 };
    std::uint8_t flight_controller_component_id_ { 1 };
    std::uint8_t air_system_id_ { 1 };
    std::uint8_t air_component_id_ { 1 };
    std::uint8_t ground_system_id_ { 100 };
    std::uint8_t ground_component_id_ { 1 };
    std::string last_error_;

    struct PeerStorage;
    PeerStorage* peer_ {};
};

} // namespace glide::mavlink
