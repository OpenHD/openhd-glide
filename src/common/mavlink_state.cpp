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

#include "common/mavlink_state.hpp"
#include "common/openhd_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace glide::mavlink {
namespace {

std::string rest_after(std::istringstream& stream)
{
    std::string value;
    std::getline(stream, value);
    if (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }
    return value;
}

int clamp_percent(int value)
{
    return std::clamp(value, 0, 100);
}

void push_message(Snapshot& snapshot, std::string message)
{
    if (message.empty()) {
        return;
    }
    if (snapshot.message_count < snapshot.messages.size()) {
        snapshot.messages[snapshot.message_count++] = std::move(message);
        return;
    }
    for (std::size_t i = 1; i < snapshot.messages.size(); ++i) {
        snapshot.messages[i - 1] = std::move(snapshot.messages[i]);
    }
    snapshot.messages.back() = std::move(message);
}

std::string upper_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

int parse_int_or(int fallback, const std::string& value)
{
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

} // namespace

bool apply_ipc_line(Snapshot& snapshot, const std::string& line)
{
    std::istringstream stream(line);
    std::string prefix;
    std::string key;
    stream >> prefix >> key;
    if (prefix != "mav") {
        return false;
    }

    if (key == "alive") {
        std::string target;
        int alive {};
        stream >> target >> alive;
        if (target == "air") {
            snapshot.air_alive = alive != 0;
        } else if (target == "ground") {
            snapshot.ground_alive = alive != 0;
        } else if (target == "fc") {
            snapshot.fc_alive = alive != 0;
        }
        return true;
    }
    if (key == "armed") {
        int armed {};
        stream >> armed;
        snapshot.armed = armed != 0;
        return true;
    }
    if (key == "attitude") {
        stream >> snapshot.roll_degrees >> snapshot.pitch_degrees >> snapshot.yaw_degrees;
        snapshot.attitude_valid = true;
        return true;
    }
    if (key == "position") {
        stream >> snapshot.latitude_deg >> snapshot.longitude_deg >> snapshot.altitude_m;
        snapshot.position_valid = true;
        snapshot.altitude_valid = true;
        return true;
    }
    if (key == "speed") {
        stream >> snapshot.ground_speed_mps >> snapshot.airspeed_mps;
        snapshot.speed_valid = true;
        return true;
    }
    if (key == "battery") {
        stream >> snapshot.voltage_v >> snapshot.battery_percent;
        snapshot.battery_valid = true;
        return true;
    }
    if (key == "gps") {
        stream >> snapshot.satellites;
        return true;
    }
    if (key == "mode") {
        snapshot.flight_mode = rest_after(stream);
        return true;
    }
    if (key == "link") {
        stream >> snapshot.frequency_mhz >> snapshot.channel_width_mhz >> snapshot.mcs_index >> snapshot.tx_power_mw;
        if (stream >> snapshot.link_bitrate_mbit >> snapshot.link_quality_percent >> snapshot.link_rssi_dbm >> snapshot.link_txc_temp_c) {
            snapshot.rc_quality_percent = snapshot.link_quality_percent;
            stream >> snapshot.link_snr_antenna1_db >> snapshot.link_snr_antenna2_db;
        }
        snapshot.link_quality_percent = clamp_percent(snapshot.link_quality_percent);
        snapshot.rc_quality_percent = clamp_percent(snapshot.rc_quality_percent);
        return true;
    }
    if (key == "openhd") {
        std::string field;
        stream >> field;
        if (field == "wifi_card") {
            int rssi_dbm {};
            int quality_percent {};
            int snr_antenna1_db {};
            int snr_antenna2_db {};
            int txc_temp_c {};
            stream >> rssi_dbm >> quality_percent >> snr_antenna1_db >> snr_antenna2_db >> txc_temp_c;
            if (snapshot.link_rssi_dbm <= -127 || (rssi_dbm > -127 && rssi_dbm >= snapshot.link_rssi_dbm)) {
                snapshot.link_rssi_dbm = rssi_dbm;
                snapshot.link_quality_percent = clamp_percent(quality_percent);
                snapshot.link_snr_antenna1_db = snr_antenna1_db;
                snapshot.link_snr_antenna2_db = snr_antenna2_db;
                snapshot.link_txc_temp_c = txc_temp_c;
            }
            if (snapshot.rc_quality_percent == 0) {
                snapshot.rc_quality_percent = snapshot.link_quality_percent;
            }
        } else if (field == "wifi_link") {
            int frequency_mhz {};
            int channel_width_mhz {};
            int mcs_index {};
            float bitrate_mbit {};
            stream >> frequency_mhz >> channel_width_mhz >> mcs_index >> bitrate_mbit >> snapshot.rc_quality_percent >> snapshot.link_snr_antenna1_db >> snapshot.link_snr_antenna2_db >> snapshot.link_txc_temp_c;
            snapshot.link_stats_valid = true;
            if (frequency_mhz > 0) {
                snapshot.frequency_mhz = frequency_mhz;
            }
            if (channel_width_mhz > 0) {
                snapshot.channel_width_mhz = channel_width_mhz;
            }
            snapshot.mcs_index = mcs_index;
            snapshot.link_bitrate_mbit = std::max(0.0F, bitrate_mbit);
            snapshot.rc_quality_percent = clamp_percent(snapshot.rc_quality_percent);
        } else if (field == "core") {
            int platform_type {};
            stream >> snapshot.link_txc_temp_c >> platform_type;
            for (const auto& entry : openhd::platform::ids) {
                if (entry.id == platform_type) {
                    snapshot.platform = std::string(entry.name);
                    break;
                }
            }
        }
        return true;
    }
    if (key == "scan") {
        stream >> snapshot.scan_progress;
        snapshot.scan_progress = clamp_percent(snapshot.scan_progress);
        return true;
    }
    if (key == "rc") {
        for (auto& channel : snapshot.rc_channels) {
            stream >> channel;
        }
        return true;
    }
    if (key == "param") {
        std::string target;
        std::string param;
        stream >> target >> param;
        const auto value = rest_after(stream);
        const auto upper_param = upper_copy(param);
        if (upper_param == "SIYI_ACTIVE") {
            snapshot.siyi_active = parse_int_or(0, value) == 1;
        } else if (upper_param == "RESOLUTION_FPS" || upper_param == "VIDEO_FORMAT" || upper_param == "CAMERA_FORMAT") {
            snapshot.resolution_fps = value;
        } else if (upper_param == "ROTATION_FLIP" || upper_param == "ROTATION_DEG" || upper_param == "VIDEO_ROTATION") {
            snapshot.rotation = value;
        } else if (upper_param == "AIR_RECORDING_E" || upper_param == "RECORDING" || upper_param == "REC_ENABLED") {
            snapshot.recording = value;
        } else if (upper_param == "WIFI_MODE" && target == "air") {
            snapshot.air_wifi_mode = value;
        } else if (upper_param == "WIFI_MODE" && target == "ground") {
            snapshot.ground_wifi_mode = value;
        } else if (upper_param == "WIFI_HOTSPOT_E" && target == "air") {
            snapshot.air_hotspot = value;
        } else if (upper_param == "WIFI_HOTSPOT_E" && target == "ground") {
            snapshot.ground_hotspot = value;
        } else if (upper_param == "FREQ" || upper_param == "FREQUENCY" || upper_param == "FREQUENCY_MHZ" || upper_param == "WB_FREQUENCY") {
            snapshot.frequency_mhz = parse_int_or(snapshot.frequency_mhz, value);
        } else if (upper_param == "CHANNEL_WIDTH" || upper_param == "CHANNEL_WIDTH_MHZ" || upper_param == "WB_CHANNEL_WIDTH" || upper_param == "WB_CHANNEL_W" || upper_param == "BANDWIDTH") {
            snapshot.channel_width_mhz = parse_int_or(snapshot.channel_width_mhz, value);
        } else if (upper_param == "MCS" || upper_param == "MCS_INDEX" || upper_param == "WB_MCS_INDEX") {
            snapshot.mcs_index = parse_int_or(snapshot.mcs_index, value);
        } else if (upper_param == "TX_POWER" || upper_param == "TX_POWER_MW" || upper_param == "WB_TX_POWER_MW") {
            snapshot.tx_power_mw = parse_int_or(snapshot.tx_power_mw, value);
        } else if (upper_param == "OPENHD_VERSION" || upper_param == "VERSION") {
            snapshot.openhd_version = value;
        } else if (upper_param == "AIR_CHIPSET") {
            snapshot.air_chipset = value;
        } else if (upper_param == "GROUND_CHIPSET") {
            snapshot.ground_chipset = value;
        } else if (upper_param == "CAMERA" || upper_param == "CAMERA_TYPE") {
            const auto camera_type = parse_int_or(-1, value);
            snapshot.camera = camera_type >= 0 ? openhd::camera::type_to_string(camera_type) : value;
        }
        return true;
    }
    if (key == "status") {
        std::string field;
        stream >> field;
        const auto value = rest_after(stream);
        if (field == "openhd_version") {
            snapshot.openhd_version = value;
        } else if (field == "ground_chipset") {
            snapshot.ground_chipset = value;
        } else if (field == "air_chipset") {
            snapshot.air_chipset = value;
        } else if (field == "camera") {
            snapshot.camera = value;
        } else if (field == "recording") {
            snapshot.recording_status = value;
        }
        return true;
    }
    if (key == "message") {
        push_message(snapshot, rest_after(stream));
        return true;
    }
    return false;
}

bool is_osd_telemetry_line(const std::string& line)
{
    std::istringstream stream(line);
    std::string prefix;
    std::string key;
    stream >> prefix >> key;
    if (prefix != "mav") {
        return false;
    }

    if (key == "alive") {
        std::string target;
        int alive {};
        stream >> target >> alive;
        return alive != 0 && (target == "air" || target == "fc");
    }

    return key == "attitude"
        || key == "position"
        || key == "speed"
        || key == "battery"
        || key == "gps"
        || key == "link"
        || key == "openhd"
        || key == "rc"
        || key == "armed"
        || key == "mode"
        || key == "param"
        || key == "status"
        || key == "message";
}

std::string format_action_set_param(const std::string& target, const std::string& param, const std::string& value)
{
    return "mav set " + target + " " + param + " " + value;
}

std::string format_action_command(const std::string& command, const std::string& arguments)
{
    if (arguments.empty()) {
        return "mav command " + command;
    }
    return "mav command " + command + " " + arguments;
}

} // namespace glide::mavlink
