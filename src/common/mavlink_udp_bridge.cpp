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

#include "common/mavlink_udp_bridge.hpp"
#include "common/openhd_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace glide::mavlink {
namespace {

struct Frame {
    std::uint8_t sysid {};
    std::uint8_t compid {};
    std::uint32_t msgid {};
    std::vector<std::uint8_t> payload;
};

std::uint16_t crc_accumulate(std::uint8_t data, std::uint16_t crc)
{
    data ^= static_cast<std::uint8_t>(crc & 0xffU);
    data ^= static_cast<std::uint8_t>(data << 4U);
    return static_cast<std::uint16_t>((crc >> 8U) ^ (static_cast<std::uint16_t>(data) << 8U) ^ (static_cast<std::uint16_t>(data) << 3U) ^ (static_cast<std::uint16_t>(data) >> 4U));
}

template <typename T>
T read_le(const std::vector<std::uint8_t>& payload, std::size_t offset)
{
    T value {};
    if (offset + sizeof(T) <= payload.size()) {
        std::memcpy(&value, payload.data() + offset, sizeof(T));
    }
    return value;
}

float read_float(const std::vector<std::uint8_t>& payload, std::size_t offset)
{
    return read_le<float>(payload, offset);
}

int read_i8(const std::vector<std::uint8_t>& payload, std::size_t offset)
{
    return static_cast<int>(read_le<std::int8_t>(payload, offset));
}

std::string c_string(const std::vector<std::uint8_t>& payload, std::size_t offset, std::size_t length)
{
    if (offset >= payload.size()) {
        return {};
    }
    const auto count = std::min(length, payload.size() - offset);
    std::string text(reinterpret_cast<const char*>(payload.data() + offset), count);
    const auto nul = text.find('\0');
    if (nul != std::string::npos) {
        text.resize(nul);
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

void put_u8(std::vector<std::uint8_t>& payload, std::uint8_t value)
{
    payload.push_back(value);
}

template <typename T>
void put_le(std::vector<std::uint8_t>& payload, T value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    payload.insert(payload.end(), bytes, bytes + sizeof(T));
}

void put_fixed_string(std::vector<std::uint8_t>& payload, const std::string& value, std::size_t length)
{
    const auto copy = std::min(value.size(), length);
    payload.insert(payload.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(copy));
    payload.insert(payload.end(), length - copy, 0);
}

std::string trim_payload_string(const std::string& text)
{
    auto result = text;
    result.erase(std::remove(result.begin(), result.end(), '\r'), result.end());
    result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
    return result;
}

std::string mode_from_heartbeat(std::uint8_t base_mode, std::uint32_t custom_mode)
{
    if ((base_mode & 0x80U) == 0) {
        return "Disarmed";
    }
    if (custom_mode != 0) {
        return "Mode " + std::to_string(custom_mode);
    }
    return "Armed";
}

std::optional<std::string> decode_frame(const Frame& frame)
{
    const auto& p = frame.payload;
    std::ostringstream line;
    line << std::fixed << std::setprecision(2);

    switch (frame.msgid) {
    case 0: {
        const auto type = read_le<std::uint8_t>(p, 4);
        const auto autopilot = read_le<std::uint8_t>(p, 5);
        if (frame.sysid == openhd::system_id_ground) {
            line << "mav alive ground 1";
        } else if (frame.sysid == openhd::system_id_air) {
            line << "mav alive air 1";
        } else if (autopilot != 8 || type == 1 || type == 2 || type == 13 || type == 14 || type == 15) {
            line << "mav alive fc 1";
        }
        return line.str().empty() ? std::nullopt : std::optional<std::string>(line.str());
    }
    case 1: {
        const auto voltage_mv = read_le<std::uint16_t>(p, 14);
        const auto battery_remaining = read_le<std::int8_t>(p, 30);
        if (voltage_mv > 0 || battery_remaining >= 0) {
            line << "mav battery " << (static_cast<float>(voltage_mv) / 1000.0F) << ' ' << static_cast<int>(battery_remaining);
            return line.str();
        }
        break;
    }
    case 22: {
        const auto value = read_float(p, 0);
        const auto name = c_string(p, 8, 16);
        if (!name.empty()) {
            line << "mav param auto " << name << ' ' << value;
            return line.str();
        }
        break;
    }
    case 24: {
        const auto lat = read_le<std::int32_t>(p, 8);
        const auto lon = read_le<std::int32_t>(p, 12);
        const auto alt = read_le<std::int32_t>(p, 16);
        const auto satellites = read_le<std::uint8_t>(p, 29);
        line << "mav position " << (static_cast<double>(lat) / 10000000.0) << ' ' << (static_cast<double>(lon) / 10000000.0) << ' ' << (static_cast<float>(alt) / 1000.0F);
        return line.str() + "\nmav gps " + std::to_string(static_cast<int>(satellites));
    }
    case 30: {
        constexpr float rad_to_deg = 57.29577951308232F;
        line << "mav attitude " << (read_float(p, 4) * rad_to_deg) << ' ' << (read_float(p, 8) * rad_to_deg) << ' ' << (read_float(p, 12) * rad_to_deg);
        return line.str();
    }
    case 33: {
        const auto lat = read_le<std::int32_t>(p, 4);
        const auto lon = read_le<std::int32_t>(p, 8);
        const auto relative_alt = read_le<std::int32_t>(p, 16);
        line << "mav position " << (static_cast<double>(lat) / 10000000.0) << ' ' << (static_cast<double>(lon) / 10000000.0) << ' ' << (static_cast<float>(relative_alt) / 1000.0F);
        return line.str();
    }
    case 65: {
        line << "mav rc";
        for (std::size_t i = 4; i < 12; i += 2) {
            line << ' ' << read_le<std::uint16_t>(p, i);
        }
        return line.str();
    }
    case 74:
        line << "mav speed " << read_float(p, 16) << ' ' << read_float(p, 0);
        return line.str();
    case 253: {
        const auto severity = read_le<std::uint8_t>(p, 0);
        auto text = trim_payload_string(c_string(p, 1, p.size() > 51 ? 254 : 50));
        if (!text.empty()) {
            line << "mav message [" << static_cast<int>(severity) << "] " << text;
            return line.str();
        }
        break;
    }
    case 322: {
        const auto name = c_string(p, 5, 16);
        const auto value = c_string(p, 21, 128);
        if (!name.empty()) {
            line << "mav param auto " << name << ' ' << value;
            return line.str();
        }
        break;
    }
    case openhd::wire::stats_monitor_mode_wifi_link_message_id: {
        const auto frequency_mhz = read_le<std::uint16_t>(p, openhd::wire::wifi_link_frequency_mhz_offset);
        const auto rate_kbits = read_le<std::uint16_t>(p, openhd::wire::wifi_link_rate_kbits_offset);
        const auto channel_width_mhz = read_le<std::uint8_t>(p, openhd::wire::wifi_link_channel_width_offset);
        const auto mcs_index = read_le<std::uint8_t>(p, openhd::wire::wifi_link_mcs_index_offset);
        const auto packet_loss = std::clamp(read_i8(p, openhd::wire::wifi_link_packet_loss_offset), 0, 100);
        const auto rc_quality = 100 - packet_loss;
        line << "mav openhd wifi_link "
             << frequency_mhz << ' '
             << static_cast<int>(channel_width_mhz) << ' '
             << static_cast<int>(mcs_index) << ' '
             << (static_cast<float>(rate_kbits) / 1000.0F) << ' '
             << rc_quality << ' '
             << read_i8(p, openhd::wire::wifi_link_snr_antenna1_offset) << ' '
             << read_i8(p, openhd::wire::wifi_link_snr_antenna2_offset) << ' '
             << read_i8(p, openhd::wire::wifi_link_temperature_offset);
        return line.str();
    }
    case openhd::wire::stats_monitor_mode_wifi_card_message_id: {
        const auto card_type = read_le<std::uint8_t>(p, openhd::wire::wifi_card_type_offset);
        line << "mav openhd wifi_card "
             << read_i8(p, openhd::wire::wifi_card_rssi_offset) << ' '
             << std::clamp(read_i8(p, openhd::wire::wifi_card_quality_offset), 0, 100) << ' '
             << read_i8(p, openhd::wire::wifi_card_snr_antenna1_offset) << ' '
             << read_i8(p, openhd::wire::wifi_card_snr_antenna2_offset) << ' '
             << read_i8(p, openhd::wire::wifi_card_temperature_offset) << '\n'
             << "mav param auto "
             << (frame.sysid == openhd::system_id_ground ? "GROUND_CHIPSET " : "AIR_CHIPSET ")
             << openhd::wifi_card::type_to_string(card_type);
        return line.str();
    }
    case openhd::wire::core_status_message_id:
        line << "mav openhd core "
             << read_i8(p, openhd::wire::core_status_cpu_temperature_offset) << ' '
             << static_cast<int>(read_le<std::uint8_t>(p, openhd::wire::core_status_platform_type_offset));
        return line.str();
    default:
        break;
    }
    return std::nullopt;
}

std::vector<Frame> parse_datagram(const std::uint8_t* data, std::size_t size)
{
    std::vector<Frame> frames;
    for (std::size_t i = 0; i < size;) {
        if (data[i] != 0xfe && data[i] != 0xfd) {
            ++i;
            continue;
        }
        const bool mavlink2 = data[i] == 0xfd;
        const auto header_len = mavlink2 ? 10U : 6U;
        if (i + header_len + 2U > size) {
            break;
        }
        const auto payload_len = data[i + 1U];
        const auto signature_len = mavlink2 && (data[i + 2U] & 0x01U) != 0 ? 13U : 0U;
        const auto frame_len = header_len + payload_len + 2U + signature_len;
        if (i + frame_len > size) {
            break;
        }

        Frame frame;
        if (mavlink2) {
            frame.sysid = data[i + 5U];
            frame.compid = data[i + 6U];
            frame.msgid = static_cast<std::uint32_t>(data[i + 7U]) | (static_cast<std::uint32_t>(data[i + 8U]) << 8U) | (static_cast<std::uint32_t>(data[i + 9U]) << 16U);
            frame.payload.assign(data + i + 10U, data + i + 10U + payload_len);
        } else {
            frame.sysid = data[i + 3U];
            frame.compid = data[i + 4U];
            frame.msgid = data[i + 5U];
            frame.payload.assign(data + i + 6U, data + i + 6U + payload_len);
        }
        frames.push_back(std::move(frame));
        i += frame_len;
    }
    return frames;
}

std::uint8_t target_system_for(const std::string& target, std::uint8_t air, std::uint8_t ground, std::uint8_t fc)
{
    if (target == "ground") {
        return ground;
    }
    if (target == "fc") {
        return fc;
    }
    return air;
}

std::uint8_t target_component_for(const std::string& target, std::uint8_t air, std::uint8_t ground, std::uint8_t fc)
{
    if (target == "ground") {
        return ground;
    }
    if (target == "fc") {
        return fc;
    }
    return air;
}

} // namespace

struct UdpBridge::PeerStorage {
#if defined(__linux__)
    sockaddr_storage address {};
    socklen_t length {};
#endif
    bool valid {};
};

UdpBridge::~UdpBridge()
{
    close();
}

bool UdpBridge::start(UdpBridgeOptions options)
{
#if defined(__linux__)
    close();
    options_ = options;
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        last_error_ = std::strerror(errno);
        return false;
    }
    const int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(options.listen_port);
    if (bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = std::strerror(errno);
        close();
        return false;
    }
    const auto flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    peer_ = new PeerStorage {};
    return true;
#else
    (void)options;
    last_error_ = "MAVLink UDP bridge requires Linux sockets";
    return false;
#endif
}

std::vector<std::string> UdpBridge::poll()
{
    std::vector<std::string> lines;
#if defined(__linux__)
    if (fd_ < 0) {
        return lines;
    }
    std::array<std::uint8_t, 2048> buffer {};
    for (;;) {
        sockaddr_storage peer_address {};
        socklen_t peer_length = sizeof(peer_address);
        const auto received = recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&peer_address), &peer_length);
        if (received <= 0) {
            break;
        }
        if (peer_ != nullptr) {
            peer_->address = peer_address;
            peer_->length = peer_length;
            peer_->valid = true;
        }
        for (const auto& frame : parse_datagram(buffer.data(), static_cast<std::size_t>(received))) {
            if (frame.msgid == 0) {
                const auto type = read_le<std::uint8_t>(frame.payload, 4);
                const auto autopilot = read_le<std::uint8_t>(frame.payload, 5);
                if (frame.sysid == openhd::system_id_ground) {
                    ground_system_id_ = frame.sysid;
                    ground_component_id_ = frame.compid;
                } else if (frame.sysid == openhd::system_id_air) {
                    air_system_id_ = frame.sysid;
                    air_component_id_ = frame.compid;
                } else if (autopilot != 8 || type == 1 || type == 2 || type == 13 || type == 14 || type == 15) {
                    flight_controller_system_id_ = frame.sysid;
                    flight_controller_component_id_ = frame.compid;
                    const auto base_mode = read_le<std::uint8_t>(frame.payload, 6);
                    const auto custom_mode = read_le<std::uint32_t>(frame.payload, 0);
                    lines.push_back(std::string("mav armed ") + ((base_mode & 0x80U) != 0 ? "1" : "0"));
                    lines.push_back("mav mode " + mode_from_heartbeat(base_mode, custom_mode));
                }
            }
            if (auto decoded = decode_frame(frame)) {
                std::istringstream decoded_lines(*decoded);
                std::string line;
                while (std::getline(decoded_lines, line)) {
                    if (!line.empty()) {
                        lines.push_back(std::move(line));
                    }
                }
            }
        }
    }
#endif
    return lines;
}

bool UdpBridge::handle_action_line(const std::string& line)
{
    std::istringstream stream(line);
    std::string prefix;
    std::string action;
    stream >> prefix >> action;
    if (prefix != "mav") {
        return false;
    }
    if (action == "set") {
        std::string target;
        std::string name;
        stream >> target >> name;
        std::string value;
        std::getline(stream, value);
        if (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
        }
        return send_param_ext_set(target, name, value) || send_param_set(target, name, value);
    }
    if (action == "command") {
        std::string command;
        stream >> command;
        std::string arguments;
        std::getline(stream, arguments);
        if (!arguments.empty() && arguments.front() == ' ') {
            arguments.erase(arguments.begin());
        }
        return send_command_long(command, arguments);
    }
    return false;
}

bool UdpBridge::send_param_set(const std::string& target, const std::string& name, const std::string& value)
{
    char* end {};
    const auto numeric = std::strtof(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    std::vector<std::uint8_t> payload;
    put_le<float>(payload, numeric);
    put_u8(payload, target_system_for(target, air_system_id_, ground_system_id_, flight_controller_system_id_));
    put_u8(payload, target_component_for(target, air_component_id_, ground_component_id_, flight_controller_component_id_));
    put_fixed_string(payload, name, 16);
    put_u8(payload, 9);
    return send_packet(23, 168, payload);
}

bool UdpBridge::send_param_ext_set(const std::string& target, const std::string& name, const std::string& value)
{
    std::vector<std::uint8_t> payload;
    put_u8(payload, target_system_for(target, air_system_id_, ground_system_id_, flight_controller_system_id_));
    put_u8(payload, target_component_for(target, air_component_id_, ground_component_id_, flight_controller_component_id_));
    put_fixed_string(payload, name, 16);
    put_fixed_string(payload, value, 128);
    put_u8(payload, 9);
    return send_packet(323, 78, payload);
}

bool UdpBridge::send_command_long(const std::string& command, const std::string&)
{
    float params[7] {};
    std::uint16_t command_id {};
    if (command == "scan") {
        command_id = 31000;
    } else {
        return false;
    }
    std::vector<std::uint8_t> payload;
    for (const auto param : params) {
        put_le<float>(payload, param);
    }
    put_le<std::uint16_t>(payload, command_id);
    put_u8(payload, air_system_id_);
    put_u8(payload, air_component_id_);
    put_u8(payload, 0);
    return send_packet(76, 152, payload);
}

bool UdpBridge::send_packet(std::uint32_t message_id, std::uint8_t crc_extra, const std::vector<std::uint8_t>& payload)
{
#if defined(__linux__)
    if (fd_ < 0 || peer_ == nullptr || !peer_->valid || payload.size() > 255U) {
        return false;
    }
    std::vector<std::uint8_t> packet;
    packet.reserve(12U + payload.size());
    packet.push_back(0xfd);
    packet.push_back(static_cast<std::uint8_t>(payload.size()));
    packet.push_back(0);
    packet.push_back(0);
    packet.push_back(sequence_++);
    packet.push_back(options_.system_id);
    packet.push_back(options_.component_id);
    packet.push_back(static_cast<std::uint8_t>(message_id & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((message_id >> 8U) & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((message_id >> 16U) & 0xffU));
    packet.insert(packet.end(), payload.begin(), payload.end());
    std::uint16_t crc = 0xffffU;
    for (std::size_t i = 1; i < packet.size(); ++i) {
        crc = crc_accumulate(packet[i], crc);
    }
    crc = crc_accumulate(crc_extra, crc);
    packet.push_back(static_cast<std::uint8_t>(crc & 0xffU));
    packet.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xffU));
    return sendto(fd_, packet.data(), packet.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&peer_->address), peer_->length) == static_cast<ssize_t>(packet.size());
#else
    (void)message_id;
    (void)crc_extra;
    (void)payload;
    return false;
#endif
}

void UdpBridge::close()
{
#if defined(__linux__)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    delete peer_;
    peer_ = nullptr;
}

bool UdpBridge::running() const
{
    return fd_ >= 0;
}

const std::string& UdpBridge::last_error() const
{
    return last_error_;
}

} // namespace glide::mavlink
