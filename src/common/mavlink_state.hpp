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

#include <array>
#include <string>

namespace glide::mavlink {

struct Snapshot {
    bool air_alive {};
    bool ground_alive {};
    bool fc_alive {};
    bool armed {};
    bool attitude_valid {};
    bool altitude_valid {};
    bool speed_valid {};
    bool position_valid {};
    bool battery_valid {};
    bool link_stats_valid {};
    int frequency_mhz {};
    int channel_width_mhz {};
    int mcs_index {};
    int tx_power_mw {};
    int link_rssi_dbm { -128 };
    int link_txc_temp_c { -128 };
    int link_quality_percent {};
    int rc_quality_percent {};
    int link_snr_antenna1_db { -128 };
    int link_snr_antenna2_db { -128 };
    int scan_progress { 0 };
    int rc_channels[4] {};
    int battery_percent { -1 };
    int satellites {};
    float roll_degrees {};
    float pitch_degrees {};
    float yaw_degrees {};
    float altitude_m {};
    float ground_speed_mps {};
    float airspeed_mps {};
    float voltage_v {};
    float link_bitrate_mbit {};
    double latitude_deg {};
    double longitude_deg {};
    std::string flight_mode { "N/A" };
    std::string resolution_fps { "N/A" };
    std::string rotation { "N/A" };
    std::string recording { "N/A" };
    std::string recording_status { "N/A" };
    std::string air_wifi_mode { "N/A" };
    std::string ground_wifi_mode { "N/A" };
    std::string air_hotspot { "N/A" };
    std::string ground_hotspot { "N/A" };
    std::string openhd_version { "N/A" };
    std::string platform { "N/A" };
    std::string ground_chipset { "N/A" };
    std::string air_chipset { "N/A" };
    std::string camera { "N/A" };
    std::array<std::string, 5> messages {};
    std::size_t message_count {};
};

bool apply_ipc_line(Snapshot& snapshot, const std::string& line);
bool is_osd_telemetry_line(const std::string& line);
std::string format_action_set_param(const std::string& target, const std::string& param, const std::string& value);
std::string format_action_command(const std::string& command, const std::string& arguments = {});

} // namespace glide::mavlink
