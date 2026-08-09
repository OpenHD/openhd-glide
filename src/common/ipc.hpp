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

#include <string>
#include <vector>
#include <cstdint>

namespace glide::ipc {

#if defined(_WIN32)
constexpr const char* default_socket_path = "127.0.0.1:32145";
#else
constexpr const char* default_socket_path = "/tmp/openhd-glide.sock";
#endif

class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool connect_to(std::string path = default_socket_path);
    bool connected() const;
    bool send_line(const std::string& line);
    std::vector<std::string> poll_lines();
    void close();

private:
    std::intptr_t fd_ { -1 };
    std::string read_buffer_;
};

struct ServerEvent {
    int client_id {};
    std::string line;
};

class Server {
public:
    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool listen_on(std::string path = default_socket_path);
    std::vector<ServerEvent> poll();
    bool send_line(int client_id, const std::string& line);
    void broadcast_line(const std::string& line);
    void close();
    const std::string& last_error() const;

private:
    struct ClientState {
        int id {};
        std::intptr_t fd {};
        std::string read_buffer;
    };

    std::intptr_t listen_fd_ { -1 };
    int next_client_id_ { 1 };
    std::string path_;
    std::string last_error_;
    std::vector<ClientState> clients_;
};

} // namespace glide::ipc
