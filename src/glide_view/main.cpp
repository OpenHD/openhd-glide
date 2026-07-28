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
#include "common/logging.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if OPENHD_GLIDE_HAS_GSTREAMER
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#endif

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int)
{
    stop_requested = 1;
}

struct Options {
    std::string ipc_socket { glide::ipc::default_socket_path };
    std::uint16_t udp_port { 5600 };
    std::optional<int> plane_id;
    std::optional<int> connector_id;
    bool stay_alive {};
    bool udp_video {};
    bool display {};
    std::string udp_codec { "h264" };
};

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--ipc-socket" && i + 1 < argc) {
            options.ipc_socket = argv[++i];
        } else if (argument == "--stay-alive") {
            options.stay_alive = true;
        } else if (argument == "--udp-video") {
            options.udp_video = true;
        } else if (argument == "--display") {
            options.display = true;
            options.udp_video = true;
        } else if (argument == "--udp-codec" && i + 1 < argc) {
            options.udp_codec = argv[++i];
            std::transform(options.udp_codec.begin(), options.udp_codec.end(), options.udp_codec.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        } else if (argument == "--udp-port" && i + 1 < argc) {
            options.udp_port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
            options.udp_video = true;
        } else if (argument == "--plane-id" && i + 1 < argc) {
            options.plane_id = std::stoi(argv[++i]);
            options.udp_video = true;
        } else if (argument == "--connector-id" && i + 1 < argc) {
            options.connector_id = std::stoi(argv[++i]);
            options.udp_video = true;
        }
    }
    return options;
}

void poll_ipc(glide::ipc::Client& ipc, std::chrono::steady_clock::time_point& next_heartbeat)
{
    if (!ipc.connected()) {
        return;
    }

    for (const auto& line : ipc.poll_lines()) {
        if (line == "pong") {
            glide::log(glide::LogLevel::info, "GlideView", "IPC pong");
        }
    }
    if (std::chrono::steady_clock::now() >= next_heartbeat) {
        ipc.send_line("heartbeat glide-view");
        next_heartbeat = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
}

#if OPENHD_GLIDE_HAS_GSTREAMER
bool has_property(GObject* object, const char* name)
{
    return g_object_class_find_property(G_OBJECT_GET_CLASS(object), name) != nullptr;
}

void set_bool_property_if_present(GstElement* element, const char* name, bool value)
{
    auto* object = G_OBJECT(element);
    if (has_property(object, name)) {
        g_object_set(object, name, value ? TRUE : FALSE, nullptr);
    }
}

void set_int_property_if_present(GstElement* element, const char* name, int value)
{
    auto* object = G_OBJECT(element);
    if (has_property(object, name)) {
        g_object_set(object, name, value, nullptr);
    }
}

void configure_decoder(GstElement* decoder, const std::string& factory)
{
    if (factory.find("mpp") != std::string::npos) {
        set_bool_property_if_present(decoder, "dma-feature", true);
        set_bool_property_if_present(decoder, "arm-afbc", false);
        set_bool_property_if_present(decoder, "fast-mode", true);
        set_bool_property_if_present(decoder, "qos", false);
    }
    if (factory.find("omx") != std::string::npos) {
        set_bool_property_if_present(decoder, "disable-dma-feature", false);
    }
}

GstElement* make_element(const char* factory, const char* name)
{
    auto* element = gst_element_factory_make(factory, name);
    if (element == nullptr) {
        glide::log(glide::LogLevel::error, "GlideView", std::string("missing GStreamer element: ") + factory);
    }
    return element;
}

GstElement* make_decoder(const std::string& codec)
{
    const bool h265 = codec == "h265";
    const bool mjpeg = codec == "mjpeg";
    const auto* codec_label = mjpeg ? "MJPEG" : (h265 ? "H.265" : "H.264");
    const std::vector<const char*> factories = mjpeg
        ? std::vector<const char*> { "jpegdec", "avdec_mjpeg" }
        : h265
        ? std::vector<const char*> { "mppvideodec", "v4l2h265dec", "v4l2slh265dec", "omxh265dec", "avdec_h265" }
        : std::vector<const char*> { "mppvideodec", "v4l2h264dec", "v4l2slh264dec", "omxh264dec", "avdec_h264" };
    for (const auto* factory : factories) {
        if (auto* decoder = gst_element_factory_make(factory, "decoder"); decoder != nullptr) {
            configure_decoder(decoder, factory);

            if (std::string(factory).find("mpp") != std::string::npos || std::string(factory).find("v4l2") != std::string::npos || std::string(factory).find("omx") != std::string::npos) {
                glide::log(glide::LogLevel::info, "GlideView", std::string("using hardware-oriented decoder ") + factory);
            } else if (mjpeg && std::string(factory) == "jpegdec") {
                glide::log(glide::LogLevel::info, "GlideView", "using MJPEG decoder jpegdec");
            } else {
                glide::log(glide::LogLevel::warning, "GlideView", std::string("using software decoder fallback ") + factory);
            }
            return decoder;
        }
    }
    glide::log(glide::LogLevel::error, "GlideView", std::string("no ")+codec_label+" decoder found; install GStreamer codec plugins");
    return nullptr;
}

std::string sample_memory_description(GstSample* sample)
{
    auto* buffer = gst_sample_get_buffer(sample);
    if (buffer == nullptr || gst_buffer_n_memory(buffer) == 0) {
        return "empty-buffer";
    }

    auto* memory = gst_buffer_peek_memory(buffer, 0);
    if (memory == nullptr) {
        return "unknown-memory";
    }
    if (gst_memory_is_type(memory, "DMABuf")) {
        return "DMABuf";
    }
    if (gst_memory_is_type(memory, "GstFdMemory")) {
        return "fd-memory";
    }
    return "system-memory";
}

std::string sample_caps_description(GstSample* sample)
{
    auto* caps = gst_sample_get_caps(sample);
    if (caps == nullptr) {
        return "unknown-caps";
    }
    auto* text = gst_caps_to_string(caps);
    std::string result = text != nullptr ? text : "unknown-caps";
    if (text != nullptr) {
        g_free(text);
    }
    return result;
}

class VideoPipeline {
public:
    ~VideoPipeline()
    {
        stop();
    }

    bool start(const Options& options)
    {
        gst_init(nullptr, nullptr);

        if (options.plane_id || options.connector_id) {
            glide::log(glide::LogLevel::warning, "GlideView", "plane/connector ids are ignored; openhd-glide must own KMS");
        }

        pipeline_ = gst_pipeline_new("openhd-glide-view");
        auto* source = make_element("udpsrc", "udp-source");
        auto* capsfilter = make_element("capsfilter", "rtp-caps");
        const bool force_h265 = options.udp_codec == "h265";
        const bool force_h264 = options.udp_codec == "h264";
        const bool force_mjpeg = options.udp_codec == "mjpeg" || options.udp_codec == "mjpg" || options.udp_codec == "jpeg";
        const bool auto_codec = options.udp_codec == "auto" || options.udp_codec.empty();
        const bool use_h265 = force_h265;
        const bool use_mjpeg = force_mjpeg;
        if (!auto_codec && !force_h264 && !force_h265 && !force_mjpeg) {
            glide::log(glide::LogLevel::error, "GlideView", "unsupported --udp-codec value; use auto, h264, h265, or mjpeg");
            stop();
            return false;
        }

        auto* depay = make_element(use_mjpeg ? "rtpjpegdepay" : (use_h265 ? "rtph265depay" : "rtph264depay"), use_mjpeg ? "mjpeg-depay" : (use_h265 ? "h265-depay" : "h264-depay"));
        auto* parse = make_element(use_mjpeg ? "identity" : (use_h265 ? "h265parse" : "h264parse"), use_mjpeg ? "mjpeg-frame" : (use_h265 ? "h265-parse" : "h264-parse"));
        auto* video_capsfilter = make_element("capsfilter", "video-caps");
        auto* input_queue = make_element("queue", "input-queue");
        auto* decoder = make_decoder(use_mjpeg ? "mjpeg" : (use_h265 ? "h265" : "h264"));
        auto* output_queue = make_element("queue", "output-queue");
        auto* converter = options.display ? make_element("videoconvert", "display-convert") : nullptr;
        auto* sink = make_element(options.display ? "autovideosink" : "appsink", options.display ? "video-output" : "decoded-frame-sink");

        if (pipeline_ == nullptr || source == nullptr || capsfilter == nullptr || depay == nullptr || parse == nullptr
            || video_capsfilter == nullptr || input_queue == nullptr || decoder == nullptr || output_queue == nullptr
            || (options.display && converter == nullptr) || sink == nullptr) {
            stop();
            return false;
        }

        g_object_set(source, "port", static_cast<int>(options.udp_port), nullptr);
        set_int_property_if_present(source, "buffer-size", 33554432);
        if (auto* source_pad = gst_element_get_static_pad(source, "src"); source_pad != nullptr) {
            source_probe_id_ = gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_BUFFER, &VideoPipeline::source_probe, this, nullptr);
            gst_object_unref(source_pad);
        }
        if (auto* parse_pad = gst_element_get_static_pad(video_capsfilter, "src"); parse_pad != nullptr) {
            parsed_probe_id_ = gst_pad_add_probe(parse_pad, GST_PAD_PROBE_TYPE_BUFFER, &VideoPipeline::parsed_probe, this, nullptr);
            gst_object_unref(parse_pad);
        }
        if (auto* output_pad = gst_element_get_static_pad(output_queue, "src"); output_pad != nullptr) {
            decoded_probe_id_ = gst_pad_add_probe(output_pad, GST_PAD_PROBE_TYPE_BUFFER, &VideoPipeline::decoded_probe, this, nullptr);
            gst_object_unref(output_pad);
        }

        auto* caps = gst_caps_new_simple(
            "application/x-rtp",
            "media",
            G_TYPE_STRING,
            "video",
            "clock-rate",
            G_TYPE_INT,
            90000,
            "encoding-name",
            G_TYPE_STRING,
            use_mjpeg ? "JPEG" : (use_h265 ? "H265" : "H264"),
            "payload",
            G_TYPE_INT,
            96,
            nullptr);
        g_object_set(capsfilter, "caps", caps, nullptr);
        gst_caps_unref(caps);

        set_int_property_if_present(parse, "config-interval", -1);
        auto* video_caps = use_mjpeg
            ? gst_caps_new_empty_simple("image/jpeg")
            : gst_caps_new_simple(
                use_h265 ? "video/x-h265" : "video/x-h264",
                "stream-format",
                G_TYPE_STRING,
                "byte-stream",
                "parsed",
                G_TYPE_BOOLEAN,
                TRUE,
                "alignment",
                G_TYPE_STRING,
                "au",
                nullptr);
        g_object_set(video_capsfilter, "caps", video_caps, nullptr);
        gst_caps_unref(video_caps);

        set_int_property_if_present(input_queue, "max-size-buffers", 64);
        set_int_property_if_present(input_queue, "max-size-bytes", 0);
        set_int_property_if_present(input_queue, "max-size-time", 0);
        set_int_property_if_present(input_queue, "leaky", 0);
        set_int_property_if_present(output_queue, "max-size-buffers", 8);
        set_int_property_if_present(output_queue, "max-size-bytes", 0);
        set_int_property_if_present(output_queue, "max-size-time", 0);
        set_int_property_if_present(output_queue, "leaky", 2);

        set_bool_property_if_present(sink, "sync", false);
        set_bool_property_if_present(sink, "async", false);
        set_bool_property_if_present(sink, "qos", false);
        if (!options.display) {
            set_bool_property_if_present(sink, "drop", true);
            set_bool_property_if_present(sink, "emit-signals", false);
            set_int_property_if_present(sink, "max-buffers", 8);
        }

        gst_bin_add_many(
            GST_BIN(pipeline_),
            source,
            capsfilter,
            depay,
            parse,
            video_capsfilter,
            input_queue,
            decoder,
            output_queue,
            nullptr);
        if (options.display) {
            gst_bin_add_many(GST_BIN(pipeline_), converter, sink, nullptr);
        } else {
            gst_bin_add(GST_BIN(pipeline_), sink);
        }

        const bool linked = options.display
            ? gst_element_link_many(
                source,
                capsfilter,
                depay,
                parse,
                video_capsfilter,
                input_queue,
                decoder,
                output_queue,
                converter,
                sink,
                nullptr)
            : gst_element_link_many(
                source,
                capsfilter,
                depay,
                parse,
                video_capsfilter,
                input_queue,
                decoder,
                output_queue,
                sink,
                nullptr);
        if (!linked) {
            glide::log(glide::LogLevel::error, "GlideView", "failed to link GStreamer UDP decode pipeline");
            stop();
            return false;
        }

        display_ = options.display;
        if (!display_) {
            sink_ = sink;
        }
        bus_ = gst_element_get_bus(pipeline_);
        const auto result = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        if (result == GST_STATE_CHANGE_FAILURE) {
            glide::log(glide::LogLevel::error, "GlideView", "failed to start GStreamer UDP decode pipeline");
            stop();
            return false;
        }

        std::ostringstream status;
        status << "UDP RTP/" << (use_mjpeg ? "MJPEG" : (use_h265 ? "H265" : "H264")) << " decode listening on 0.0.0.0:" << options.udp_port
               << " output=" << (options.display ? "autovideosink" : "appsink");
        glide::log(glide::LogLevel::info, "GlideView", status.str());
        if (options.display) {
            glide::log(glide::LogLevel::info, "GlideView", "display mode enabled; using GStreamer desktop video sink");
        } else {
            glide::log(
                glide::LogLevel::warning,
                "GlideView",
                "decode-only mode does not display video; openhd-glide must import decoded buffers and own KMS");
        }
        return true;
    }

    bool poll()
    {
        if (!poll_bus()) {
            return false;
        }
        if (!display_) {
            pull_samples();
        }
        log_stats();
        return true;
    }

private:
    bool poll_bus()
    {
        if (bus_ == nullptr) {
            return true;
        }

        while (auto* message = gst_bus_pop_filtered(
                   bus_,
                   static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_STATE_CHANGED))) {
            switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError* error {};
                gchar* debug {};
                gst_message_parse_error(message, &error, &debug);
                glide::log(glide::LogLevel::error, "GlideView", error != nullptr ? error->message : "GStreamer error");
                if (debug != nullptr) {
                    glide::log(glide::LogLevel::warning, "GlideView", debug);
                }
                if (error != nullptr) {
                    g_error_free(error);
                }
                if (debug != nullptr) {
                    g_free(debug);
                }
                gst_message_unref(message);
                return false;
            }
            case GST_MESSAGE_EOS:
                glide::log(glide::LogLevel::warning, "GlideView", "GStreamer video pipeline reached EOS");
                gst_message_unref(message);
                return false;
            case GST_MESSAGE_STATE_CHANGED:
                if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline_)) {
                    GstState old_state {};
                    GstState new_state {};
                    GstState pending {};
                    gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
                    glide::log(
                        glide::LogLevel::info,
                        "GlideView",
                        std::string("pipeline state ") + gst_element_state_get_name(old_state) + " -> "
                            + gst_element_state_get_name(new_state));
                }
                break;
            default:
                break;
            }
            gst_message_unref(message);
        }
        return true;
    }

    void pull_samples()
    {
        if (sink_ == nullptr) {
            return;
        }

        while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink_), 0)) {
            if (!logged_first_sample_) {
                glide::log(glide::LogLevel::info, "GlideView", "first decoded sample caps=" + sample_caps_description(sample));
                glide::log(glide::LogLevel::info, "GlideView", "first decoded sample memory=" + sample_memory_description(sample));
                logged_first_sample_ = true;
            }

            gst_sample_unref(sample);
        }
    }

    void log_stats()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto frames = decoded_frames_.load(std::memory_order_relaxed);
        if (frames == 0 && now - last_waiting_log_ >= std::chrono::seconds(3)) {
            const auto packets = source_packets_.load();
            const auto bytes = source_bytes_.load();
            if (packets == 0) {
                glide::log(glide::LogLevel::warning, "GlideView", "no RTP packets received yet; check sender target IP, port, and network path");
            } else {
                std::ostringstream stream;
                stream << "received RTP packets=" << packets << " bytes=" << bytes
                       << " but no decoded samples yet; check RTP caps/decoder negotiation";
                glide::log(glide::LogLevel::warning, "GlideView", stream.str());
            }
            last_waiting_log_ = now;
        }
        if (now - last_packet_log_ >= std::chrono::seconds(1)) {
            const auto packets = source_packets_.load();
            if (packets > logged_source_packets_) {
                std::ostringstream stream;
                stream << "RTP ingress packets=" << packets << " bytes=" << source_bytes_.load();
                glide::log(glide::LogLevel::info, "GlideView", stream.str());
                logged_source_packets_ = packets;
            }
            last_packet_log_ = now;
        }
        if (now - last_fps_log_ >= std::chrono::seconds(1)) {
            const auto elapsed = std::chrono::duration<double>(now - last_fps_log_).count();
            const auto fps = static_cast<double>(frames - logged_decoded_frames_) / elapsed;
            if (frames > 0) {
                std::ostringstream stream;
                stream.setf(std::ios::fixed);
                stream.precision(1);
                const auto parsed_frames = parsed_frames_.load(std::memory_order_relaxed);
                const auto parsed_fps = static_cast<double>(parsed_frames - logged_parsed_frames_) / elapsed;
                logged_parsed_frames_ = parsed_frames;
                logged_decoded_frames_ = frames;
                stream << "decoded fps=" << fps
                       << " parsed_au_fps=" << parsed_fps
                       << " total_frames=" << frames
                       << " total_parsed_au=" << parsed_frames;
                glide::log(glide::LogLevel::info, "GlideView", stream.str());
            }
            last_fps_log_ = now;
        }
    }

    void stop()
    {
        if (pipeline_ != nullptr && source_probe_id_ != 0) {
            if (auto* source = gst_bin_get_by_name(GST_BIN(pipeline_), "udp-source"); source != nullptr) {
                if (auto* source_pad = gst_element_get_static_pad(source, "src"); source_pad != nullptr) {
                    gst_pad_remove_probe(source_pad, source_probe_id_);
                    gst_object_unref(source_pad);
                }
                gst_object_unref(source);
            }
            source_probe_id_ = 0;
        }
        if (pipeline_ != nullptr && parsed_probe_id_ != 0) {
            if (auto* video_caps = gst_bin_get_by_name(GST_BIN(pipeline_), "video-caps"); video_caps != nullptr) {
                if (auto* parse_pad = gst_element_get_static_pad(video_caps, "src"); parse_pad != nullptr) {
                    gst_pad_remove_probe(parse_pad, parsed_probe_id_);
                    gst_object_unref(parse_pad);
                }
                gst_object_unref(video_caps);
            }
            parsed_probe_id_ = 0;
        }
        if (pipeline_ != nullptr && decoded_probe_id_ != 0) {
            if (auto* output_queue = gst_bin_get_by_name(GST_BIN(pipeline_), "output-queue"); output_queue != nullptr) {
                if (auto* output_pad = gst_element_get_static_pad(output_queue, "src"); output_pad != nullptr) {
                    gst_pad_remove_probe(output_pad, decoded_probe_id_);
                    gst_object_unref(output_pad);
                }
                gst_object_unref(output_queue);
            }
            decoded_probe_id_ = 0;
        }
        if (pipeline_ != nullptr) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        }
        if (bus_ != nullptr) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        sink_ = nullptr;
    }

    GstElement* pipeline_ {};
    GstElement* sink_ {};
    GstBus* bus_ {};
    gulong source_probe_id_ {};
    gulong parsed_probe_id_ {};
    gulong decoded_probe_id_ {};
    bool display_ {};
    bool logged_first_sample_ {};
    std::uint64_t logged_source_packets_ {};
    std::uint64_t logged_parsed_frames_ {};
    std::uint64_t logged_decoded_frames_ {};
    std::atomic<std::uint64_t> source_packets_ {};
    std::atomic<std::uint64_t> source_bytes_ {};
    std::atomic<std::uint64_t> parsed_frames_ {};
    std::atomic<std::uint64_t> decoded_frames_ {};
    std::chrono::steady_clock::time_point last_fps_log_ { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_packet_log_ { std::chrono::steady_clock::now() };
    std::chrono::steady_clock::time_point last_waiting_log_ { std::chrono::steady_clock::now() };

    static GstPadProbeReturn source_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
    {
        auto* self = static_cast<VideoPipeline*>(user_data);
        if (auto* buffer = gst_pad_probe_info_get_buffer(info); buffer != nullptr) {
            self->source_packets_.fetch_add(1, std::memory_order_relaxed);
            self->source_bytes_.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
        }
        return GST_PAD_PROBE_OK;
    }

    static GstPadProbeReturn parsed_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
    {
        auto* self = static_cast<VideoPipeline*>(user_data);
        if (gst_pad_probe_info_get_buffer(info) != nullptr) {
            self->parsed_frames_.fetch_add(1, std::memory_order_relaxed);
        }
        return GST_PAD_PROBE_OK;
    }

    static GstPadProbeReturn decoded_probe(GstPad*, GstPadProbeInfo* info, gpointer user_data)
    {
        auto* self = static_cast<VideoPipeline*>(user_data);
        if (gst_pad_probe_info_get_buffer(info) != nullptr) {
            self->decoded_frames_.fetch_add(1, std::memory_order_relaxed);
        }
        return GST_PAD_PROBE_OK;
    }
};
#endif

} // namespace

int main(int argc, char** argv)
{
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);

    const auto options = parse_options(argc, argv);
    glide::log(glide::LogLevel::info, "GlideView", "video decode worker started");

    glide::ipc::Client ipc;
    if (ipc.connect_to(options.ipc_socket)) {
        ipc.send_line("hello glide-view");
        ipc.send_line("status glide-view ready");
    } else {
        glide::log(glide::LogLevel::warning, "GlideView", "IPC controller unavailable");
    }

    auto next_heartbeat = std::chrono::steady_clock::now();

    if (options.udp_video) {
#if OPENHD_GLIDE_HAS_GSTREAMER
        VideoPipeline pipeline;
        if (!pipeline.start(options)) {
            return 1;
        }
        if (ipc.connected()) {
            ipc.send_line("status glide-view udp-decode-ready");
        }
        while (stop_requested == 0) {
            if (!pipeline.poll()) {
                return 1;
            }
            poll_ipc(ipc, next_heartbeat);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;
#else
        glide::log(glide::LogLevel::error, "GlideView", "GStreamer support was not found at build time");
        return 1;
#endif
    }

    if (options.stay_alive) {
        while (stop_requested == 0) {
            poll_ipc(ipc, next_heartbeat);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    return 0;
}
