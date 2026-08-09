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

#include "common/ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace glide::ipc {
namespace {

#if defined(__linux__) || defined(_WIN32)
std::vector<std::string> drain_lines(std::string& buffer)
{
    std::vector<std::string> lines;
    for (;;) {
        const auto newline = buffer.find('\n');
        if (newline == std::string::npos) break;
        auto line = buffer.substr(0, newline);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        buffer.erase(0, newline + 1U);
    }
    return lines;
}
#endif

#if defined(__linux__)
bool set_nonblocking(int fd)
{
    const auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

sockaddr_un socket_address(const std::string& path)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1U);
    return address;
}

#elif defined(_WIN32)
bool winsock_ready()
{
    static const bool ready = [] {
        WSADATA data {};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

bool set_nonblocking(std::intptr_t fd)
{
    u_long enabled = 1;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &enabled) == 0;
}

std::uint16_t endpoint_port(const std::string& endpoint)
{
    const auto separator = endpoint.find_last_of(':');
    if (separator == std::string::npos) return 32145;
    try { return static_cast<std::uint16_t>(std::stoul(endpoint.substr(separator + 1U))); }
    catch (...) { return 32145; }
}
#endif

} // namespace

Client::~Client()
{
    close();
}

bool Client::connect_to(std::string path)
{
#if defined(__linux__)
    close();
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }

    const auto address = socket_address(path);
    if (connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close();
        return false;
    }
    set_nonblocking(fd_);
    return true;
#elif defined(_WIN32)
    close();
    if (!winsock_ready()) return false;
    fd_ = static_cast<std::intptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (static_cast<SOCKET>(fd_) == INVALID_SOCKET) { fd_ = -1; return false; }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint_port(path));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(static_cast<SOCKET>(fd_), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close();
        return false;
    }
    set_nonblocking(fd_);
    return true;
#else
    (void)path;
    return false;
#endif
}

bool Client::connected() const
{
    return fd_ >= 0;
}

bool Client::send_line(const std::string& line)
{
#if defined(__linux__)
    if (fd_ < 0) {
        return false;
    }
    const auto payload = line + '\n';
    return send(fd_, payload.data(), payload.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size());
#elif defined(_WIN32)
    if (fd_ < 0) return false;
    const auto payload = line + '\n';
    return send(static_cast<SOCKET>(fd_), payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(payload.size());
#else
    (void)line;
    return false;
#endif
}

std::vector<std::string> Client::poll_lines()
{
    std::vector<std::string> lines;
#if defined(__linux__)
    if (fd_ < 0) {
        return lines;
    }

    char buffer[512] {};
    for (;;) {
        const auto bytes = recv(fd_, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            read_buffer_.append(buffer, static_cast<std::size_t>(bytes));
            continue;
        }
        if (bytes == 0) {
            close();
        }
        break;
    }
    lines = drain_lines(read_buffer_);
#elif defined(_WIN32)
    if (fd_ < 0) return lines;
    char buffer[512] {};
    for (;;) {
        const auto bytes = recv(static_cast<SOCKET>(fd_), buffer, sizeof(buffer), 0);
        if (bytes > 0) { read_buffer_.append(buffer, static_cast<std::size_t>(bytes)); continue; }
        if (bytes == 0) close();
        break;
    }
    lines = drain_lines(read_buffer_);
#endif
    return lines;
}

void Client::close()
{
#if defined(__linux__)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#elif defined(_WIN32)
    if (fd_ >= 0) { closesocket(static_cast<SOCKET>(fd_)); fd_ = -1; }
#endif
}

Server::~Server()
{
    close();
}

bool Server::listen_on(std::string path)
{
#if defined(__linux__)
    close();
    path_ = std::move(path);
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        last_error_ = std::strerror(errno);
        return false;
    }

    unlink(path_.c_str());
    const auto address = socket_address(path_);
    if (bind(listen_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = std::strerror(errno);
        close();
        return false;
    }
    chmod(path_.c_str(), 0666);
    if (listen(listen_fd_, 8) != 0) {
        last_error_ = std::strerror(errno);
        close();
        return false;
    }
    set_nonblocking(listen_fd_);
    return true;
#elif defined(_WIN32)
    close();
    path_ = std::move(path);
    if (!winsock_ready()) { last_error_ = "WSAStartup failed"; return false; }
    listen_fd_ = static_cast<std::intptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (static_cast<SOCKET>(listen_fd_) == INVALID_SOCKET) { listen_fd_ = -1; last_error_ = "socket failed"; return false; }
    const BOOL reuse = TRUE;
    setsockopt(
        static_cast<SOCKET>(listen_fd_),
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse),
        sizeof(reuse));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint_port(path_));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(static_cast<SOCKET>(listen_fd_), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0
        || listen(static_cast<SOCKET>(listen_fd_), 8) != 0) {
        last_error_ = "could not bind loopback IPC endpoint " + path_ + " (Winsock error " + std::to_string(WSAGetLastError()) + ")";
        close();
        return false;
    }
    set_nonblocking(listen_fd_);
    return true;
#else
    (void)path;
    last_error_ = "Unix domain sockets require Linux";
    return false;
#endif
}

std::vector<ServerEvent> Server::poll()
{
    std::vector<ServerEvent> events;
#if defined(__linux__)
    if (listen_fd_ < 0) {
        return events;
    }
    for (;;) {
        const auto fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            break;
        }
        set_nonblocking(fd);
        clients_.push_back(ClientState {
            .id = next_client_id_++,
            .fd = fd,
            .read_buffer = {},
        });
    }

    char buffer[512] {};
    std::vector<int> dead_clients;
    for (auto& client : clients_) {
        for (;;) {
            const auto bytes = recv(client.fd, buffer, sizeof(buffer), 0);
            if (bytes > 0) {
                client.read_buffer.append(buffer, static_cast<std::size_t>(bytes));
                continue;
            }
            if (bytes == 0) {
                dead_clients.push_back(client.id);
            }
            break;
        }
        for (auto& line : drain_lines(client.read_buffer)) {
            events.push_back(ServerEvent { .client_id = client.id, .line = std::move(line) });
        }
    }

    for (const auto id : dead_clients) {
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(), [id](const auto& client) {
                if (client.id == id) {
                    ::close(client.fd);
                    return true;
                }
                return false;
            }),
            clients_.end());
    }
#elif defined(_WIN32)
    if (listen_fd_ < 0) return events;
    for (;;) {
        const auto socket_value = accept(static_cast<SOCKET>(listen_fd_), nullptr, nullptr);
        if (socket_value == INVALID_SOCKET) break;
        const auto fd = static_cast<std::intptr_t>(socket_value);
        set_nonblocking(fd);
        clients_.push_back(ClientState { .id = next_client_id_++, .fd = fd, .read_buffer = {} });
    }
    char buffer[512] {};
    std::vector<int> dead_clients;
    for (auto& client : clients_) {
        for (;;) {
            const auto bytes = recv(static_cast<SOCKET>(client.fd), buffer, sizeof(buffer), 0);
            if (bytes > 0) { client.read_buffer.append(buffer, static_cast<std::size_t>(bytes)); continue; }
            if (bytes == 0) dead_clients.push_back(client.id);
            break;
        }
        for (auto& line : drain_lines(client.read_buffer)) events.push_back({ client.id, std::move(line) });
    }
    for (const auto id : dead_clients) {
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [id](const auto& client) {
            if (client.id == id) { closesocket(static_cast<SOCKET>(client.fd)); return true; }
            return false;
        }), clients_.end());
    }
#endif
    return events;
}

bool Server::send_line(int client_id, const std::string& line)
{
#if defined(__linux__)
    const auto it = std::find_if(clients_.begin(), clients_.end(), [client_id](const auto& client) {
        return client.id == client_id;
    });
    if (it == clients_.end()) {
        return false;
    }
    const auto payload = line + '\n';
    return send(it->fd, payload.data(), payload.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size());
#elif defined(_WIN32)
    const auto it = std::find_if(clients_.begin(), clients_.end(), [client_id](const auto& client) { return client.id == client_id; });
    if (it == clients_.end()) return false;
    const auto payload = line + '\n';
    return send(static_cast<SOCKET>(it->fd), payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(payload.size());
#else
    (void)client_id;
    (void)line;
    return false;
#endif
}

void Server::broadcast_line(const std::string& line)
{
    for (const auto& client : clients_) {
        send_line(client.id, line);
    }
}

void Server::close()
{
#if defined(__linux__)
    for (const auto& client : clients_) {
        ::close(client.fd);
    }
    clients_.clear();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (!path_.empty()) {
        unlink(path_.c_str());
    }
#elif defined(_WIN32)
    for (const auto& client : clients_) closesocket(static_cast<SOCKET>(client.fd));
    clients_.clear();
    if (listen_fd_ >= 0) { closesocket(static_cast<SOCKET>(listen_fd_)); listen_fd_ = -1; }
#endif
}

const std::string& Server::last_error() const
{
    return last_error_;
}

} // namespace glide::ipc
