/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#include "common/network_discovery.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct Mode {
    std::string label;
    int width;
    int height;
    int fps;
    int quality;
};

constexpr std::uint16_t video_port = 5600;

const std::vector<Mode> modes {
    { "720p 60 MJPEG quality 75 (recommended)", 1280, 720, 60, 75 },
    { "720p 120 MJPEG quality 65", 1280, 720, 120, 65 },
    { "1080p 30 MJPEG quality 75", 1920, 1080, 30, 75 },
    { "1536x864 60 MJPEG quality 75", 1536, 864, 60, 75 },
    { "1536x864 120 MJPEG quality 65", 1536, 864, 120, 65 },
};

std::string read_file(const std::string& path)
{
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool command_exists(const std::string& command)
{
#if defined(__linux__)
    const auto shell = "command -v " + command + " >/dev/null 2>&1";
    return std::system(shell.c_str()) == 0;
#else
    (void)command;
    return false;
#endif
}

std::string camera_binary()
{
    if (command_exists("rpicam-vid")) {
        return "rpicam-vid";
    }
    if (command_exists("libcamera-vid")) {
        return "libcamera-vid";
    }
    return {};
}

std::string find_openhd_glide()
{
#if defined(__linux__)
    const std::vector<std::string> candidates {
        "/opt/openhd-glide/build-kms/openhd-glide",
        "/usr/local/bin/openhd-glide",
        "/usr/bin/openhd-glide",
    };
    for (const auto& candidate : candidates) {
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }
    if (command_exists("openhd-glide")) {
        return "openhd-glide";
    }
#endif
    return {};
}

std::string detected_platform()
{
    const auto compatible = read_file("/proc/device-tree/compatible");
    const auto model = read_file("/proc/device-tree/model");
    if (contains(compatible, "rockchip") || contains(model, "Rockchip") || contains(model, "Radxa")) {
        return "rockchip";
    }
    if (contains(compatible, "raspberrypi") || contains(model, "Raspberry Pi")) {
        return "raspberry-pi";
    }
    if (contains(compatible, "allwinner") || contains(model, "Orange Pi")) {
        return "allwinner";
    }
    return "linux";
}

int run_shell(const std::string& command)
{
#if defined(__linux__)
    execl("/bin/sh", "sh", "-lc", command.c_str(), nullptr);
    std::perror("execl");
    return 127;
#else
    std::cerr << "openhd-glide-ethernet requires Linux\n";
    (void)command;
    return 1;
#endif
}

int prompt_index(int count, int default_index)
{
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) {
        return default_index;
    }
    char* end {};
    const auto value = std::strtol(line.c_str(), &end, 10);
    if (end == line.c_str() || value < 1 || value > count) {
        return default_index;
    }
    return static_cast<int>(value - 1);
}

int prompt_int(const std::string& prompt, int default_value, int min_value, int max_value)
{
    std::cout << prompt << " [" << default_value << "]: ";
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) {
        return default_value;
    }
    char* end {};
    const auto value = std::strtol(line.c_str(), &end, 10);
    if (end == line.c_str()) {
        return default_value;
    }
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return static_cast<int>(value);
}

std::string quote(const std::string& value)
{
    std::string out = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out += "'";
    return out;
}

int receive()
{
#if defined(__linux__)
    if (geteuid() != 0) {
        execlp("sudo", "sudo", "openhd-glide-ethernet", "-receive", nullptr);
        std::perror("sudo");
        return 1;
    }
#endif

    const auto platform = detected_platform();
    const auto glide = find_openhd_glide();
    if (glide.empty()) {
        std::cerr << "openhd-glide binary not found. Build or install OpenHD-Glide first.\n";
        return 1;
    }

    std::cout << "Detected platform: " << platform << "\n";
    std::cout << "Receiver ready on UDP/RTP MJPEG port " << video_port << "\n";
    std::cout << "Log: /tmp/openhd-glide-ethernet-receive.log\n";

    std::string command = "systemctl stop openhd-glide 2>/dev/null || true; pkill -INT -x openhd-glide 2>/dev/null || true; ";
    command += "sysctl -w net.core.rmem_max=33554432 net.core.rmem_default=33554432 net.core.netdev_max_backlog=5000 >/dev/null 2>&1 || true; ";
    command += quote(glide);
    command += " --kms-video-preview --no-flow --view-udp-port 5600 --view-udp-codec mjpeg --preview-width 1920 --flow-height 1080 --display-refresh-hz 0";
    if (platform == "rockchip") {
        command += " --native-rkmpp-video";
    }
    command += " > /tmp/openhd-glide-ethernet-receive.log 2>&1";
    return run_shell(command);
}

int send()
{
    const auto platform = detected_platform();
    const auto camera = camera_binary();
    if (camera.empty()) {
        std::cerr << "No rpicam-vid/libcamera-vid command found on this sender.\n";
        return 1;
    }
    if (!command_exists("gst-launch-1.0")) {
        std::cerr << "gst-launch-1.0 is required for RTP/JPEG packetization.\n";
        return 1;
    }

    std::cout << "Detected platform: " << platform << "\n";
    std::cout << "Searching for receivers...\n";
    const auto peers = glide::net::scan_blocking(glide::net::discovery_port, video_port, std::chrono::milliseconds(3500));
    if (peers.empty()) {
        std::cerr << "No receivers found. Start `openhd-glide-ethernet -receive` on the display unit first.\n";
        return 1;
    }

    for (std::size_t i = 0; i < peers.size(); ++i) {
        std::cout << "  " << (i + 1U) << ") " << peers[i].address << "  " << peers[i].hostname << "  video:" << peers[i].video_port << "\n";
    }
    std::cout << "Receiver [1]: ";
    const auto peer_index = prompt_index(static_cast<int>(peers.size()), 0);
    const auto target = peers[static_cast<std::size_t>(peer_index)];
    std::cout << "Selected receiver: " << target.address << "\n";

    std::cout << "Video mode:\n";
    for (std::size_t i = 0; i < modes.size(); ++i) {
        std::cout << "  " << (i + 1U) << ") " << modes[i].label << "\n";
    }
    std::cout << "  " << (modes.size() + 1U) << ") Custom\n";
    std::cout << "Mode [1]: ";
    const auto mode_index = prompt_index(static_cast<int>(modes.size() + 1U), 0);
    Mode mode = mode_index == static_cast<int>(modes.size())
        ? Mode { "custom", 1280, 720, 60, 75 }
        : modes[static_cast<std::size_t>(mode_index)];
    if (mode_index == static_cast<int>(modes.size())) {
        mode.width = prompt_int("Width", mode.width, 160, 7680);
        mode.height = prompt_int("Height", mode.height, 120, 4320);
        mode.fps = prompt_int("FPS", mode.fps, 1, 240);
        mode.quality = prompt_int("MJPEG quality", mode.quality, 1, 100);
    }

    std::cout << "Streaming to " << target.address << ':' << target.video_port << "\n";
    std::cout << "Mode: " << mode.width << 'x' << mode.height << '@' << mode.fps << " quality=" << mode.quality << "\n";

    std::ostringstream command;
    command
        << quote(camera)
        << " --timeout 0 --codec mjpeg"
        << " --width " << mode.width
        << " --height " << mode.height
        << " --framerate " << mode.fps
        << " --quality " << mode.quality
        << " --output - --nopreview 2>/tmp/openhd-glide-ethernet-camera.log"
        << " | gst-launch-1.0 -q fdsrc fd=0 do-timestamp=true ! jpegparse ! rtpjpegpay pt=96 mtu=1200 ! udpsink host="
        << quote(target.address)
        << " port=" << target.video_port
        << " sync=false async=false 2>/tmp/openhd-glide-ethernet-gst.log";
    return run_shell(command.str());
}

void usage()
{
    std::cerr
        << "usage:\n"
        << "  openhd-glide-ethernet -receive\n"
        << "  openhd-glide-ethernet -send\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        usage();
        return 2;
    }
    const std::string mode = argv[1];
    if (mode == "-receive" || mode == "--receive") {
        return receive();
    }
    if (mode == "-send" || mode == "--send") {
        return send();
    }
    usage();
    return 2;
}
