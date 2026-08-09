#include "dev/desktop_video_pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#if OPENHD_GLIDE_HAS_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#endif

namespace glide::dev {

namespace {

struct CodecPipeline {
    const char* depay;
    const char* parser;
    const char* decoder;
    const char* encoding;
};

bool codec_pipeline(const std::string& requested_codec, CodecPipeline& result)
{
    auto codec = requested_codec;
    std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const bool h265 = codec == "h265" || codec == "hevc";
    const bool mjpeg = codec == "mjpeg" || codec == "mjpg" || codec == "jpeg";
    if (!h265 && !mjpeg && codec != "h264") return false;
    result = {
        .depay = mjpeg ? "rtpjpegdepay" : (h265 ? "rtph265depay" : "rtph264depay"),
        .parser = mjpeg ? "jpegparse" : (h265 ? "h265parse config-interval=-1 disable-passthrough=true"
                                               : "h264parse config-interval=-1 disable-passthrough=true"),
        .decoder = mjpeg ? "avdec_mjpeg output-corrupt=false"
                         : (h265 ? "avdec_h265 output-corrupt=true" : "avdec_h264 output-corrupt=true"),
        .encoding = mjpeg ? "JPEG" : (h265 ? "H265" : "H264"),
    };
    return true;
}

std::string decode_tail(const CodecPipeline& codec)
{
    // Never drop encoded access units: losing one P-frame corrupts every
    // dependent frame until the next IDR. The appsink may safely discard old
    // frames after decoding when rendering falls behind.
    return std::string(" ! ") + codec.depay + " name=video-depay"
        + " ! " + codec.parser
        + " ! " + codec.decoder
        + " ! videoconvert ! video/x-raw,format=RGBA"
        + " ! appsink name=desktop-video sync=false drop=true max-buffers=2 enable-last-sample=false";
}

} // namespace

struct DesktopVideoPipeline::Impl {
#if OPENHD_GLIDE_HAS_GSTREAMER
    GstElement* pipeline {};
    GstElement* sink {};
    GstBus* bus {};
    GstSample* sample {};
    GstMapInfo map {};
    GstVideoInfo info {};
    std::vector<std::uint8_t> packed_frame;
#endif
};

DesktopVideoPipeline::DesktopVideoPipeline() : impl_(new Impl) {}

DesktopVideoPipeline::~DesktopVideoPipeline()
{
    release_frame();
#if OPENHD_GLIDE_HAS_GSTREAMER
    if (impl_->pipeline != nullptr) gst_element_set_state(impl_->pipeline, GST_STATE_NULL);
    if (impl_->sink != nullptr) gst_object_unref(impl_->sink);
    if (impl_->bus != nullptr) gst_object_unref(impl_->bus);
    if (impl_->pipeline != nullptr) gst_object_unref(impl_->pipeline);
#endif
    delete impl_;
}

bool DesktopVideoPipeline::start(std::uint16_t port, const std::string& requested_codec)
{
#if OPENHD_GLIDE_HAS_GSTREAMER
    gst_init(nullptr, nullptr);
    CodecPipeline codec {};
    if (!codec_pipeline(requested_codec, codec)) {
        last_error_ = "desktop video codec must be h264, h265, or mjpeg";
        return false;
    }
    const auto description = std::string("udpsrc port=") + std::to_string(port) + " buffer-size=8388608"
        + " caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=" + codec.encoding + ",payload=96\""
        + " ! rtpjitterbuffer latency=50 drop-on-latency=false do-lost=true"
        + decode_tail(codec);
    return start_pipeline(description);
#else
    (void)port;
    (void)requested_codec;
    last_error_ = "GStreamer support was not found at build time";
    return false;
#endif
}

bool DesktopVideoPipeline::start_pipeline(const std::string& description)
{
#if OPENHD_GLIDE_HAS_GSTREAMER
    GError* error {};
    impl_->pipeline = gst_parse_launch(description.c_str(), &error);
    if (impl_->pipeline == nullptr) {
        last_error_ = error != nullptr ? error->message : "failed to create desktop video pipeline";
        if (error != nullptr) g_error_free(error);
        return false;
    }
    impl_->sink = gst_bin_get_by_name(GST_BIN(impl_->pipeline), "desktop-video");
    if (auto* depay = gst_bin_get_by_name(GST_BIN(impl_->pipeline), "video-depay"); depay != nullptr) {
        auto* object_class = G_OBJECT_GET_CLASS(depay);
        if (g_object_class_find_property(object_class, "request-keyframe") != nullptr) {
            // OpenHD's one-way RTP path has no RTCP return channel. Waiting for
            // a requested keyframe can therefore leave a late-joining decoder
            // black indefinitely.
            g_object_set(depay, "request-keyframe", FALSE, nullptr);
        }
        if (g_object_class_find_property(object_class, "wait-for-keyframe") != nullptr) {
            g_object_set(depay, "wait-for-keyframe", FALSE, nullptr);
        }
        gst_object_unref(depay);
    }
    impl_->bus = gst_element_get_bus(impl_->pipeline);
    if (impl_->sink == nullptr || gst_element_set_state(impl_->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        last_error_ = "failed to start GStreamer software video pipeline";
        return false;
    }
    return true;
#else
    (void)description;
    last_error_ = "GStreamer support was not found at build time";
    return false;
#endif
}

bool DesktopVideoPipeline::poll(DesktopVideoFrame& frame)
{
    frame = {};
#if OPENHD_GLIDE_HAS_GSTREAMER
    release_frame();
    if (impl_->bus != nullptr) {
        if (auto* message = gst_bus_pop_filtered(impl_->bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)); message != nullptr) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* error {};
                gchar* debug {};
                gst_message_parse_error(message, &error, &debug);
                last_error_ = error != nullptr ? error->message : "GStreamer video error";
                if (error != nullptr) g_error_free(error);
                if (debug != nullptr) g_free(debug);
            } else {
                last_error_ = "video stream ended";
            }
            gst_message_unref(message);
            return false;
        }
    }
    impl_->sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->sink), 0);
    if (impl_->sample == nullptr) return true;
    auto* caps = gst_sample_get_caps(impl_->sample);
    auto* buffer = gst_sample_get_buffer(impl_->sample);
    if (caps == nullptr || buffer == nullptr || !gst_video_info_from_caps(&impl_->info, caps)
        || !gst_buffer_map(buffer, &impl_->map, GST_MAP_READ)) {
        last_error_ = "could not map decoded desktop video frame";
        release_frame();
        return false;
    }
    frame.width = GST_VIDEO_INFO_WIDTH(&impl_->info);
    frame.height = GST_VIDEO_INFO_HEIGHT(&impl_->info);
    const auto source_stride = static_cast<std::uint32_t>(GST_VIDEO_INFO_PLANE_STRIDE(&impl_->info, 0));
    frame.stride = frame.width * 4U;
    if (source_stride == frame.stride) {
        frame.pixels = impl_->map.data;
    } else {
        impl_->packed_frame.resize(static_cast<std::size_t>(frame.stride) * frame.height);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(
                impl_->packed_frame.data() + static_cast<std::size_t>(row) * frame.stride,
                impl_->map.data + static_cast<std::size_t>(row) * source_stride,
                frame.stride);
        }
        frame.pixels = impl_->packed_frame.data();
    }
#endif
    return true;
}

void DesktopVideoPipeline::release_frame()
{
#if OPENHD_GLIDE_HAS_GSTREAMER
    if (impl_->sample != nullptr) {
        if (impl_->map.data != nullptr) {
            gst_buffer_unmap(gst_sample_get_buffer(impl_->sample), &impl_->map);
            impl_->map = {};
        }
        gst_sample_unref(impl_->sample);
        impl_->sample = nullptr;
    }
#endif
}

bool DesktopVideoPipeline::running() const
{
#if OPENHD_GLIDE_HAS_GSTREAMER
    return impl_->pipeline != nullptr;
#else
    return false;
#endif
}

const std::string& DesktopVideoPipeline::last_error() const { return last_error_; }

} // namespace glide::dev
