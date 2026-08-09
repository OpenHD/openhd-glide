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
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#elif defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace glide::mavlink {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket invalid_socket = INVALID_SOCKET;
bool ensure_socket_runtime()
{
    static const bool initialized = [] {
        WSADATA data {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}
int socket_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
void close_native_socket(NativeSocket socket) { closesocket(socket); }
bool set_nonblocking(NativeSocket socket)
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}
#elif defined(__linux__)
using NativeSocket = int;
constexpr NativeSocket invalid_socket = -1;
bool ensure_socket_runtime() { return true; }
int socket_error() { return errno; }
bool would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
void close_native_socket(NativeSocket socket) { ::close(socket); }
bool set_nonblocking(NativeSocket socket)
{
    const auto flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

bool connect_with_timeout(NativeSocket socket, const sockaddr* address, int address_length, int& error)
{
    if (!set_nonblocking(socket)) {
        error = socket_error();
        return false;
    }
    if (::connect(socket, address, address_length) == 0) return true;
    error = socket_error();
#if defined(_WIN32)
    if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) return false;
#else
    if (error != EINPROGRESS && !would_block(error)) return false;
#endif
    fd_set writable;
    fd_set failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(socket, &writable);
    FD_SET(socket, &failed);
    timeval timeout { 1, 0 };
    const auto selected = select(static_cast<int>(socket) + 1, nullptr, &writable, &failed, &timeout);
    if (selected <= 0) {
#if defined(_WIN32)
        error = selected == 0 ? WSAETIMEDOUT : socket_error();
#else
        error = selected == 0 ? ETIMEDOUT : socket_error();
#endif
        return false;
    }
    int connect_error {};
#if defined(_WIN32)
    int option_length = sizeof(connect_error);
#else
    socklen_t option_length = sizeof(connect_error);
#endif
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connect_error), &option_length) != 0 || connect_error != 0) {
        error = connect_error != 0 ? connect_error : socket_error();
        return false;
    }
    error = 0;
    return true;
}

NativeSocket native_socket(std::intptr_t value) { return static_cast<NativeSocket>(value); }
std::intptr_t stored_socket(NativeSocket value) { return static_cast<std::intptr_t>(value); }

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
        line << "mav position " << std::setprecision(7)
             << (static_cast<double>(lat) / 10000000.0) << ' '
             << (static_cast<double>(lon) / 10000000.0) << ' '
             << std::setprecision(2) << (static_cast<float>(alt) / 1000.0F);
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
        line << "mav position " << std::setprecision(7)
             << (static_cast<double>(lat) / 10000000.0) << ' '
             << (static_cast<double>(lon) / 10000000.0) << ' '
             << std::setprecision(2) << (static_cast<float>(relative_alt) / 1000.0F);
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
        const auto name = c_string(p, 4, 16);
        const auto type = read_le<std::uint8_t>(p, 148);
        if (!name.empty()) {
            line << "mav param auto " << name << ' ';
            switch (type) {
            case 1: line << static_cast<unsigned int>(read_le<std::uint8_t>(p, 20)); break;
            case 2: line << static_cast<int>(read_le<std::int8_t>(p, 20)); break;
            case 3: line << read_le<std::uint16_t>(p, 20); break;
            case 4: line << read_le<std::int16_t>(p, 20); break;
            case 5: line << read_le<std::uint32_t>(p, 20); break;
            case 6: line << read_le<std::int32_t>(p, 20); break;
            case 7: line << read_le<std::uint64_t>(p, 20); break;
            case 8: line << read_le<std::int64_t>(p, 20); break;
            case 9: line << read_le<float>(p, 20); break;
            case 10: line << read_le<double>(p, 20); break;
            default: line << c_string(p, 20, 128); break;
            }
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

std::vector<Frame> parse_stream(std::vector<std::uint8_t>& bytes)
{
    std::vector<Frame> frames;
    std::size_t consumed {};
    for (std::size_t i = 0; i < bytes.size();) {
        if (bytes[i] != 0xfe && bytes[i] != 0xfd) {
            ++i;
            consumed = i;
            continue;
        }
        const bool mavlink2 = bytes[i] == 0xfd;
        const auto header_len = mavlink2 ? 10U : 6U;
        if (i + header_len + 2U > bytes.size()) break;
        const auto payload_len = bytes[i + 1U];
        const auto signature_len = mavlink2 && (bytes[i + 2U] & 0x01U) != 0 ? 13U : 0U;
        const auto frame_len = header_len + payload_len + 2U + signature_len;
        if (i + frame_len > bytes.size()) break;
        auto parsed = parse_datagram(bytes.data() + i, frame_len);
        if (!parsed.empty()) frames.push_back(std::move(parsed.front()));
        i += frame_len;
        consumed = i;
    }
    if (consumed != 0) bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(consumed));
    return frames;
}

std::uint8_t target_system_for(const std::string& target, std::uint8_t air, std::uint8_t ground, std::uint8_t fc)
{
    if (target == "ground") {
        return openhd::system_id_ground;
    }
    if (target == "fc") {
        return fc;
    }
    if (target == "air" || target == "camera1" || target == "camera2") {
        return openhd::system_id_air;
    }
    return air != 0 ? air : (ground != 0 ? ground : openhd::system_id_air);
}

std::uint8_t target_component_for(const std::string& target, std::uint8_t air, std::uint8_t ground, std::uint8_t fc)
{
    if (target == "camera1") {
        return openhd::component_id_camera_primary;
    }
    if (target == "camera2") {
        return openhd::component_id_camera_secondary;
    }
    if (target == "ground") {
        return openhd::component_id_link;
    }
    if (target == "fc") {
        return fc;
    }
    if (target == "air") {
        return openhd::component_id_link;
    }
    return air != 0 ? air : (ground != 0 ? ground : openhd::component_id_link);
}

} // namespace

struct UdpBridge::PeerStorage {
#if defined(_WIN32) || defined(__linux__)
    sockaddr_storage address {};
    int length {};
#endif
    bool valid {};
};

UdpBridge::~UdpBridge()
{
    close();
}

bool UdpBridge::start(UdpBridgeOptions options)
{
#if defined(_WIN32) || defined(__linux__)
    close();
    if (!ensure_socket_runtime()) {
        last_error_ = "failed to initialize socket runtime";
        return false;
    }
    options_ = options;
    transport_ = NetworkTransport::udp;
    listening_ = true;
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) {
        last_error_ = "UDP socket error " + std::to_string(socket_error());
        return false;
    }
    fd_ = stored_socket(socket);
    const int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(options.listen_port);
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = "UDP bind error " + std::to_string(socket_error());
        close();
        return false;
    }
    set_nonblocking(socket);
    peer_ = new PeerStorage {};
    last_error_.clear();
    return true;
#else
    (void)options;
    last_error_ = "MAVLink UDP bridge requires Linux sockets";
    return false;
#endif
}

bool UdpBridge::connect_to(NetworkTransport transport, const std::string& host, std::uint16_t port)
{
#if defined(_WIN32) || defined(__linux__)
    close();
    if (!ensure_socket_runtime() || host.empty() || port == 0) {
        last_error_ = "invalid telemetry endpoint";
        return false;
    }

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = transport == NetworkTransport::tcp ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = transport == NetworkTransport::tcp ? IPPROTO_TCP : IPPROTO_UDP;
    addrinfo* resolved {};
    const auto port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
        last_error_ = "could not resolve telemetry host " + host;
        return false;
    }

    NativeSocket connected = invalid_socket;
    int last_connect_error = 0;
    auto* selected = resolved;
    for (; selected != nullptr; selected = selected->ai_next) {
        connected = ::socket(selected->ai_family, selected->ai_socktype, selected->ai_protocol);
        if (connected == invalid_socket) continue;
        if (transport == NetworkTransport::tcp) {
            if (connect_with_timeout(connected, selected->ai_addr, static_cast<int>(selected->ai_addrlen), last_connect_error)) break;
        } else {
            if (selected->ai_family == AF_INET) {
                sockaddr_in local {};
                local.sin_family = AF_INET;
                local.sin_addr.s_addr = htonl(INADDR_ANY);
                if (bind(connected, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) break;
            } else if (selected->ai_family == AF_INET6) {
                sockaddr_in6 local {};
                local.sin6_family = AF_INET6;
                if (bind(connected, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) break;
            }
        }
        if (transport != NetworkTransport::tcp) last_connect_error = socket_error();
        close_native_socket(connected);
        connected = invalid_socket;
    }
    if (connected == invalid_socket || selected == nullptr) {
        freeaddrinfo(resolved);
        last_error_ = "could not connect telemetry "
            + std::string(transport == NetworkTransport::tcp ? "TCP" : "UDP") + " endpoint " + host + ":" + port_text
            + " (socket error " + std::to_string(last_connect_error) + ")";
        return false;
    }

    fd_ = stored_socket(connected);
    transport_ = transport;
    remote_host_ = host;
    remote_port_ = port;
    listening_ = false;
    peer_ = new PeerStorage {};
    if (transport == NetworkTransport::udp) {
        std::memcpy(&peer_->address, selected->ai_addr, selected->ai_addrlen);
        peer_->length = static_cast<int>(selected->ai_addrlen);
        peer_->valid = true;
    }
    freeaddrinfo(resolved);
    set_nonblocking(connected);
    last_error_.clear();
    return true;
#else
    (void)transport;
    (void)host;
    (void)port;
    last_error_ = "network telemetry is unavailable on this platform";
    return false;
#endif
}

std::vector<std::string> UdpBridge::poll()
{
    std::vector<std::string> lines;
#if defined(_WIN32) || defined(__linux__)
    if (fd_ < 0) {
        return lines;
    }
    std::array<std::uint8_t, 2048> buffer {};
    for (;;) {
        int received {};
        std::vector<Frame> frames;
        if (transport_ == NetworkTransport::tcp) {
            received = recv(native_socket(fd_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (received > 0) {
                stream_buffer_.insert(stream_buffer_.end(), buffer.begin(), buffer.begin() + received);
                frames = parse_stream(stream_buffer_);
            }
        } else {
            sockaddr_storage peer_address {};
#if defined(_WIN32)
            int peer_length = sizeof(peer_address);
#else
            socklen_t peer_length = sizeof(peer_address);
#endif
            received = static_cast<int>(recvfrom(native_socket(fd_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&peer_address), &peer_length));
            if (received > 0) {
                if (peer_ != nullptr) {
                    peer_->address = peer_address;
                    peer_->length = static_cast<int>(peer_length);
                    peer_->valid = true;
                }
                frames = parse_datagram(buffer.data(), static_cast<std::size_t>(received));
            }
        }
        if (received <= 0) {
            const auto error = socket_error();
            if (received == 0 && transport_ == NetworkTransport::tcp) {
                last_error_ = "TCP telemetry peer disconnected";
                close();
            }
            if (received < 0 && !would_block(error)) last_error_ = "telemetry receive error " + std::to_string(error);
            break;
        }
        for (const auto& frame : frames) {
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
    char* integer_end {};
    const auto integer_value = std::strtol(value.c_str(), &integer_end, 10);
    const bool is_integer = integer_end != value.c_str() && *integer_end == '\0'
        && integer_value >= std::numeric_limits<std::int32_t>::min()
        && integer_value <= std::numeric_limits<std::int32_t>::max();
    std::vector<std::uint8_t> payload;
    put_u8(payload, target_system_for(target, air_system_id_, ground_system_id_, flight_controller_system_id_));
    put_u8(payload, target_component_for(target, air_component_id_, ground_component_id_, flight_controller_component_id_));
    put_fixed_string(payload, name, 16);
    if (is_integer) {
        put_le<std::int32_t>(payload, static_cast<std::int32_t>(integer_value));
        payload.insert(payload.end(), 124, 0);
        put_u8(payload, 6); // MAV_PARAM_EXT_TYPE_INT32
    } else {
        put_fixed_string(payload, value, 128);
        put_u8(payload, 11); // MAV_PARAM_EXT_TYPE_CUSTOM
    }
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
#if defined(_WIN32) || defined(__linux__)
    if (fd_ < 0 || payload.size() > 255U
        || (transport_ == NetworkTransport::udp && (peer_ == nullptr || !peer_->valid))) {
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
    int sent {};
    if (transport_ == NetworkTransport::tcp) {
        sent = send(native_socket(fd_), reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0);
    } else {
        sent = sendto(
            native_socket(fd_),
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
#if defined(__linux__)
            MSG_NOSIGNAL,
#else
            0,
#endif
            reinterpret_cast<const sockaddr*>(&peer_->address),
            peer_->length);
    }
    return sent == static_cast<int>(packet.size());
#else
    (void)message_id;
    (void)crc_extra;
    (void)payload;
    return false;
#endif
}

void UdpBridge::close()
{
#if defined(_WIN32) || defined(__linux__)
    if (fd_ >= 0) {
        close_native_socket(native_socket(fd_));
        fd_ = -1;
    }
#endif
    delete peer_;
    peer_ = nullptr;
    stream_buffer_.clear();
    remote_host_.clear();
    remote_port_ = 0;
    listening_ = false;
}

bool UdpBridge::running() const
{
    return fd_ >= 0;
}

std::string UdpBridge::connection_description() const
{
    if (!running()) return "Disconnected";
    if (listening_) return "UDP listening 0.0.0.0:" + std::to_string(options_.listen_port);
    return std::string(transport_ == NetworkTransport::tcp ? "TCP " : "UDP ")
        + remote_host_ + ":" + std::to_string(remote_port_);
}

const std::string& UdpBridge::last_error() const
{
    return last_error_;
}

} // namespace glide::mavlink
