#pragma once

#include <cstdint>
#include <string>

namespace glide::dev {

struct DesktopVideoFrame {
    const void* pixels {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t stride {};
};

class DesktopVideoPipeline {
public:
    DesktopVideoPipeline();
    ~DesktopVideoPipeline();
    DesktopVideoPipeline(const DesktopVideoPipeline&) = delete;
    DesktopVideoPipeline& operator=(const DesktopVideoPipeline&) = delete;

    bool start(std::uint16_t port, const std::string& codec);
    bool poll(DesktopVideoFrame& frame);
    void release_frame();
    bool running() const;
    const std::string& last_error() const;

private:
    bool start_pipeline(const std::string& description);

    struct Impl;
    Impl* impl_ {};
    std::string last_error_;
};

} // namespace glide::dev
