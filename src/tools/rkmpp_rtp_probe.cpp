/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 *
 * Non-Military Use Only.
 ******************************************************************************/

#include "video/rockchip_mpp_rtp_decoder.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> running { true };

void request_stop(int)
{
    running.store(false, std::memory_order_release);
}

struct Options {
    std::uint16_t port { 5600 };
    std::string codec { "h264" };
    int duration_seconds { 30 };
    int stats_interval_ms { 1000 };
    std::uint64_t exit_after_frames {};
    bool require_frame {};
    bool debug_inject_x20_header {};
};

void print_usage()
{
    std::cout
        << "Usage: glide-rkmpp-rtp-probe [options]\n"
        << "  --port PORT             RTP UDP port (default 5600)\n"
        << "  --codec h264|h265       Codec (default h264)\n"
        << "  --duration SECONDS      Stop after this many seconds; 0 runs forever\n"
        << "  --stats-ms MILLISECONDS Decoder statistics interval\n"
        << "  --exit-after-frames N   Stop after polling N decoded frames\n"
        << "  --require-frame         Exit 3 if no frame was decoded\n"
        << "  --debug-inject-x20-header\n"
        << "                          Reproduce the unsafe packaged-header path\n"
        << "\nThis probe uses RTP depacketization and RKMPP only. It does not open DRM/KMS,\n"
        << "start GlideUI, connect MAVLink, or communicate with Artosyn hardware.\n";
}

bool parse_unsigned(const char* text, std::uint64_t& value)
{
    char* end {};
    const auto parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_options(int argc, char** argv, Options& options)
{
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage();
            std::exit(0);
        }
        if (argument == "--require-frame") {
            options.require_frame = true;
            continue;
        }
        if (argument == "--debug-inject-x20-header") {
            options.debug_inject_x20_header = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }
        std::uint64_t value {};
        if (argument == "--codec") {
            options.codec = argv[++index];
        } else if (argument == "--port" && parse_unsigned(argv[++index], value) && value <= 65535) {
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--duration" && parse_unsigned(argv[++index], value)) {
            options.duration_seconds = static_cast<int>(value);
        } else if (argument == "--stats-ms" && parse_unsigned(argv[++index], value) && value > 0) {
            options.stats_interval_ms = static_cast<int>(value);
        } else if (argument == "--exit-after-frames" && parse_unsigned(argv[++index], value)) {
            options.exit_after_frames = value;
        } else {
            std::cerr << "Invalid option or value: " << argument << '\n';
            return false;
        }
    }
    return options.codec == "h264" || options.codec == "h265";
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }
    if (!glide::video::rockchip_mpp_decoder_available()) {
        std::cerr << "This build does not have Rockchip MPP support\n";
        return 2;
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    glide::video::RockchipMppRtpDecoder decoder;
    if (options.debug_inject_x20_header) {
        std::cerr
            << "Running the X20 seed regression path; access-unit submission previously\n"
            << "hard-reset this RK3588, while FPVue-style NAL submission must remain stable\n";
    }
    if (!decoder.start(options.port, options.codec, options.debug_inject_x20_header)) {
        std::cerr << "Failed to start RKMPP probe: " << decoder.last_error() << '\n';
        return 2;
    }

    std::cout << "RKMPP RTP probe listening on UDP/" << options.port
              << " codec=" << options.codec << '\n';
    const auto started = std::chrono::steady_clock::now();
    auto next_stats = started;
    std::uint64_t frames {};

    while (running.load(std::memory_order_acquire)) {
        glide::dev::DmabufVideoFrame frame;
        if (decoder.poll(frame)) {
            ++frames;
            decoder.mark_presented();
            if (options.exit_after_frames > 0 && frames >= options.exit_after_frames) {
                break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_stats) {
            std::cout << "probe_frames=" << frames << ' ' << decoder.stats() << std::endl;
            next_stats = now + std::chrono::milliseconds(options.stats_interval_ms);
        }
        if (options.duration_seconds > 0
            && now - started >= std::chrono::seconds(options.duration_seconds)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "final probe_frames=" << frames << ' ' << decoder.stats() << std::endl;
    if (!decoder.last_error().empty()) {
        std::cerr << "Decoder error: " << decoder.last_error() << '\n';
    }
    return options.require_frame && frames == 0 ? 3 : 0;
}
