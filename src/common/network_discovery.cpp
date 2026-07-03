/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#include "common/network_discovery.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace glide::net {
namespace {

constexpr const char* discover_prefix = "OPENHD_GLIDE_DISCOVER";
constexpr const char* here_prefix = "OPENHD_GLIDE_HERE";

#if defined(__linux__)
bool set_nonblocking(int fd)
{
    const auto flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string address_to_string(const sockaddr_in& address)
{
    char buffer[INET_ADDRSTRLEN] {};
    if (inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)) == nullptr) {
        return {};
    }
    return buffer;
}

std::string local_hostname()
{
    char buffer[128] {};
    if (gethostname(buffer, sizeof(buffer) - 1U) == 0 && buffer[0] != '\0') {
        return buffer;
    }
    return "glide";
}

std::vector<sockaddr_in> broadcast_addresses(std::uint16_t port)
{
    std::vector<sockaddr_in> result;
    ifaddrs* interfaces {};
    if (getifaddrs(&interfaces) != 0) {
        return result;
    }
    for (auto* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto flags = item->ifa_flags;
        if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        sockaddr_in destination {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(port);
        if ((flags & IFF_BROADCAST) != 0 && item->ifa_broadaddr != nullptr) {
            destination.sin_addr = reinterpret_cast<sockaddr_in*>(item->ifa_broadaddr)->sin_addr;
        } else if (item->ifa_netmask != nullptr) {
            const auto local = reinterpret_cast<sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr;
            const auto mask = reinterpret_cast<sockaddr_in*>(item->ifa_netmask)->sin_addr.s_addr;
            destination.sin_addr.s_addr = local | ~mask;
        } else {
            destination.sin_addr.s_addr = INADDR_BROADCAST;
        }
        result.push_back(destination);
    }
    freeifaddrs(interfaces);

    sockaddr_in fallback {};
    fallback.sin_family = AF_INET;
    fallback.sin_port = htons(port);
    fallback.sin_addr.s_addr = INADDR_BROADCAST;
    result.push_back(fallback);
    return result;
}

std::vector<std::string> local_ipv4_addresses()
{
    std::vector<std::string> result;
    ifaddrs* interfaces {};
    if (getifaddrs(&interfaces) != 0) {
        return result;
    }
    for (auto* item = interfaces; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        const auto* address = reinterpret_cast<sockaddr_in*>(item->ifa_addr);
        result.push_back(address_to_string(*address));
    }
    freeifaddrs(interfaces);
    return result;
}

std::string run_ip_command(const std::vector<std::string>& args)
{
    const auto pid = fork();
    if (pid < 0) {
        return std::string("fork failed: ") + std::strerror(errno);
    }
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 2U);
        argv.push_back(const_cast<char*>("ip"));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp("ip", argv.data());
        _exit(127);
    }

    int status {};
    if (waitpid(pid, &status, 0) < 0) {
        return std::string("waitpid failed: ") + std::strerror(errno);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ostringstream stream;
        stream << "ip";
        for (const auto& arg : args) {
            stream << ' ' << arg;
        }
        stream << " failed with status " << status;
        return stream.str();
    }
    return {};
}
#endif

std::string sanitize_token(std::string value)
{
    for (auto& character : value) {
        if (character == ' ' || character == '\n' || character == '\r' || character == '\t') {
            character = '_';
        }
    }
    return value;
}

} // namespace

bool DiscoveryService::start(const DiscoveryOptions& options)
{
    stop();
    options_ = options;
    hostname_ = sanitize_token(
#if defined(__linux__)
        local_hostname()
#else
        "glide"
#endif
    );

#if defined(__linux__)
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        last_error_ = std::string("socket failed: ") + std::strerror(errno);
        return false;
    }

    int enabled = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(options_.discovery_udp_port);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = std::string("bind UDP discovery failed: ") + std::strerror(errno);
        stop();
        return false;
    }
    set_nonblocking(fd_);
    pending_lines_.push_back("state net idle");
    return true;
#else
    last_error_ = "network discovery requires Linux sockets";
    return false;
#endif
}

void DiscoveryService::stop()
{
#if defined(__linux__)
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
#endif
    scan_until_ = {};
    next_probe_ = {};
}

bool DiscoveryService::running() const
{
    return fd_ >= 0;
}

const std::string& DiscoveryService::last_error() const
{
    return last_error_;
}

void DiscoveryService::start_scan(std::chrono::milliseconds duration)
{
    if (!running()) {
        return;
    }
    peers_.clear();
    scan_until_ = std::chrono::steady_clock::now() + duration;
    next_probe_ = {};
    pending_lines_.push_back("state net scanning");
}

std::vector<std::string> DiscoveryService::poll_state_lines()
{
    const auto now = std::chrono::steady_clock::now();
    if (running() && scan_until_ != std::chrono::steady_clock::time_point {} && now >= next_probe_) {
        send_broadcast_probe();
        next_probe_ = now + std::chrono::milliseconds(450);
    }

#if defined(__linux__)
    if (running()) {
        char buffer[512] {};
        for (;;) {
            sockaddr_in sender {};
            socklen_t sender_size = sizeof(sender);
            const auto bytes = recvfrom(fd_, buffer, sizeof(buffer) - 1U, 0, reinterpret_cast<sockaddr*>(&sender), &sender_size);
            if (bytes <= 0) {
                break;
            }
            buffer[bytes] = '\0';
            handle_packet(std::string(buffer), address_to_string(sender));
        }
    }
#endif

    if (scan_until_ != std::chrono::steady_clock::time_point {} && now >= scan_until_) {
        scan_until_ = {};
        pending_lines_.push_back("state net done " + std::to_string(peers_.size()));
    }

    auto lines = std::move(pending_lines_);
    pending_lines_.clear();
    return lines;
}

const std::vector<Peer>& DiscoveryService::peers() const
{
    return peers_;
}

bool DiscoveryService::send_broadcast_probe()
{
#if defined(__linux__)
    if (!running()) {
        return false;
    }
    const auto payload = std::string(discover_prefix) + " 1 " + hostname_ + " " + std::to_string(options_.video_udp_port);
    bool sent {};
    for (const auto& destination : broadcast_addresses(options_.discovery_udp_port)) {
        sent = sendto(fd_, payload.data(), payload.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)) >= 0 || sent;
    }
    return sent;
#else
    return false;
#endif
}

void DiscoveryService::handle_packet(const std::string& payload, const std::string& sender_address)
{
    if (sender_address.empty()) {
        return;
    }

    std::istringstream stream(payload);
    std::string prefix;
    int version {};
    std::string hostname;
    std::uint16_t video_port {};
    stream >> prefix >> version >> hostname >> video_port;
    if (version != 1) {
        return;
    }

    if (prefix == discover_prefix) {
#if defined(__linux__)
        const auto reply = std::string(here_prefix) + " 1 " + hostname_ + " " + std::to_string(options_.video_udp_port);
        sockaddr_in destination {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(options_.discovery_udp_port);
        inet_pton(AF_INET, sender_address.c_str(), &destination.sin_addr);
        sendto(fd_, reply.data(), reply.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#endif
        return;
    }

    if (prefix != here_prefix || video_port == 0) {
        return;
    }
#if defined(__linux__)
    const auto local_addresses = local_ipv4_addresses();
    if (std::find(local_addresses.begin(), local_addresses.end(), sender_address) != local_addresses.end()) {
        return;
    }
#endif
    upsert_peer(Peer {
        .address = sender_address,
        .hostname = sanitize_token(hostname.empty() ? "glide" : hostname),
        .video_port = video_port,
        .last_seen = std::chrono::steady_clock::now(),
    });
}

void DiscoveryService::upsert_peer(Peer peer)
{
    auto existing = std::find_if(peers_.begin(), peers_.end(), [&](const auto& item) {
        return item.address == peer.address;
    });
    std::size_t index {};
    if (existing != peers_.end()) {
        index = static_cast<std::size_t>(std::distance(peers_.begin(), existing));
        *existing = std::move(peer);
    } else {
        peers_.push_back(std::move(peer));
        index = peers_.size() - 1U;
    }
    const auto& item = peers_[index];
    pending_lines_.push_back("state net peer " + item.address + " " + item.hostname + " " + std::to_string(item.video_port));
}

std::vector<Peer> scan_blocking(std::uint16_t discovery_udp_port, std::uint16_t video_udp_port, std::chrono::milliseconds duration)
{
    DiscoveryService service;
    if (!service.start(DiscoveryOptions { .discovery_udp_port = discovery_udp_port, .video_udp_port = video_udp_port })) {
        return {};
    }
    service.start_scan(duration);
    const auto deadline = std::chrono::steady_clock::now() + duration + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < deadline) {
        service.poll_state_lines();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return service.peers();
}

std::string configure_point_to_point(const PointToPointOptions& options)
{
#if defined(__linux__)
    if (options.interface_name.empty()) {
        return "missing interface name";
    }
    const auto role = options.role == "air" ? std::string("air") : std::string("ground");
    const auto cidr = role == "air" ? std::string("10.77.0.2/30") : std::string("10.77.0.1/30");
    if (const auto error = run_ip_command({ "link", "set", options.interface_name, "up" }); !error.empty()) {
        return error;
    }
    if (const auto error = run_ip_command({ "addr", "flush", "dev", options.interface_name }); !error.empty()) {
        return error;
    }
    if (const auto error = run_ip_command({ "addr", "add", cidr, "dev", options.interface_name }); !error.empty()) {
        return error;
    }
    return "ok " + options.interface_name + " " + cidr;
#else
    (void)options;
    return "point-to-point setup requires Linux iproute2";
#endif
}

} // namespace glide::net
