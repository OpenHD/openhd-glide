/******************************************************************************
 * OpenHD
 *
 * Licensed under the GNU General Public License (GPL) Version 3.
 ******************************************************************************/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace glide::openhd {

inline constexpr std::uint8_t system_id_ground = 100;
inline constexpr std::uint8_t system_id_air = 101;
inline constexpr std::uint8_t component_id_camera_primary = 100;
inline constexpr std::uint8_t component_id_camera_secondary = 101;
inline constexpr std::uint8_t component_id_link = 191;

// Wire-level values mirrored from OpenHD. Keep these synchronized with:
//   ohd_common/inc/openhd_platform.h
//   ohd_video/misc/camera_registry.json
//   ohd_telemetry/lib/mavlink-headers/mavlink/v2.0/openhd/
struct IdEntry {
    int id;
    std::string_view key;
    std::string_view name;
};

namespace platform {

inline constexpr std::array<IdEntry, 24> ids {{
    { 0, "X_PLATFORM_TYPE_UNKNOWN", "UNKNOWN" },
    { 1, "X_PLATFORM_TYPE_X86", "X86" },
    { 10, "X_PLATFORM_TYPE_RPI_OLD", "RPI<=3" },
    { 11, "X_PLATFORM_TYPE_RPI_4", "RPI 4" },
    { 12, "X_PLATFORM_TYPE_RPI_CM4", "RPI CM4" },
    { 13, "X_PLATFORM_TYPE_RPI_5", "RPI 5" },
    { 20, "X_PLATFORM_TYPE_ROCKCHIP_RK3566_RADXA_ZERO3W", "RADXA ZERO3W" },
    { 21, "X_PLATFORM_TYPE_ROCKCHIP_RK3588_RADXA_ROCK5_A", "RADXA RK3588S" },
    { 22, "X_PLATFORM_TYPE_ROCKCHIP_RK3588_RADXA_ROCK5_B", "RADXA RK3588" },
    { 23, "X_PLATFORM_TYPE_ROCKCHIP_RV1126", "RV1126" },
    { 24, "X_PLATFORM_TYPE_ROCKCHIP_RK3566_RADXA_CM3", "RADXA CM3" },
    { 25, "X_PLATFORM_TYPE_LUCKFOX_RV110X", "RV110X" },
    { 27, "X_PLATFORM_TYPE_LUCKFOX_LYRA", "ERR-UNDEFINED{27}" },
    { 28, "X_PLATFORM_TYPE_OPENHD_X21", "X21/RV1126(B)" },
    { 30, "X_PLATFORM_TYPE_ALWINNER_X20", "X20" },
    { 31, "X_PLATFORM_TYPE_ALWINNER_CUBIE_A7S", "ERR-UNDEFINED{31}" },
    { 32, "X_PLATFORM_TYPE_ALWINNER_CUBIE_A7Z", "A733" },
    { 36, "X_PLATFORM_TYPE_OPENIPC_SIGMASTAR_UNDEFINED", "OPENIPC SIGMASTAR" },
    { 40, "X_PLATFORM_TYPE_NVIDIA_XAVIER", "NVIDIA_XAVIER" },
    { 46, "X_PLATFORM_TYPE_QUALCOMM_QRB5165", "QUALCOMM_QRB5165" },
    { 47, "X_PLATFORM_TYPE_QUALCOMM_QCS405", "QUALCOMM_QCS405" },
    { 51, "X_PLATFORM_TYPE_ORQA", "ORQA" },
    { 52, "X_PLATFORM_TYPE_UVX_MOD", "UVX_MOD" },
    { 61, "X_PLATFORM_TYPE_NXP_IMX8", "NXP_IMX8" },
}};

} // namespace platform

namespace camera {

inline constexpr std::array<IdEntry, 69> ids {{
    { 0, "X_CAM_TYPE_DUMMY_SW", "DUMMY" },
    { 2, "X_CAM_TYPE_EXTERNAL", "EXTERNAL" },
    { 3, "X_CAM_TYPE_EXTERNAL_IP", "EXTERNAL_IP" },
    { 4, "X_CAM_TYPE_DEVELOPMENT_FILESRC", "DEV_FILESRC" },
    { 10, "X_CAM_TYPE_USB_GENERIC", "USB" },
    { 11, "X_CAM_TYPE_USB_INFIRAY", "INFIRAY" },
    { 12, "X_CAM_TYPE_USB_INFIRAY_T2", "INFIRAY_T2" },
    { 13, "X_CAM_TYPE_USB_INFIRAY_X2", "INFIRAY_X2" },
    { 14, "X_CAM_TYPE_USB_INFIRAY_P2_PRO", "INFIRAY_P2_PRO" },
    { 15, "X_CAM_TYPE_USB_FLIR_VUE", "FLIR VUE" },
    { 16, "X_CAM_TYPE_USB_FLIR_BOSON", "FLIR BOSON" },
    { 20, "X_CAM_TYPE_RPI_MMAL_HDMI_TO_CSI", "MMAL_HDMI" },
    { 30, "X_CAM_TYPE_RPI_LIBCAMERA_RPIF_V1_OV5647", "RPIF_V1_OV5647" },
    { 31, "X_CAM_TYPE_RPI_LIBCAMERA_RPIF_V2_IMX219", "RPIF_V2_IMX219" },
    { 32, "X_CAM_TYPE_RPI_LIBCAMERA_RPIF_V3_IMX708", "RPIF_V3_IMX708" },
    { 33, "X_CAM_TYPE_RPI_LIBCAMERA_RPIF_HQ_IMX477", "RPIF_HQ_IMX477" },
    { 40, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_SKYMASTERHDR_IMX708", "ARDUCAM_SKYMASTERHDR" },
    { 41, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_SKYVISIONPRO_IMX519", "ARDUCAM_SKYVISIONPRO" },
    { 42, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_IMX477M", "ARDUCAM_IMX477M" },
    { 43, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_IMX462", "ARDUCAM_IMX462" },
    { 44, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_IMX327", "ARDUCAM_IMX327" },
    { 45, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_IMX290", "ARDUCAM_IMX290" },
    { 46, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_IMX462_LOWLIGHT_MINI", "ARDUCAM_IMX462_LOWLIGHT_MINI" },
    { 47, "X_CAM_TYPE_RPI_LIBCAMERA_ARDUCAM_IMX662", "ARDUCAM_IMX662" },
    { 60, "X_CAM_TYPE_RPI_V4L2_VEYE_2MP", "VEYE_2MP" },
    { 61, "X_CAM_TYPE_RPI_V4L2_VEYE_CSIMX307", "VEYE_IMX307" },
    { 62, "X_CAM_TYPE_RPI_V4L2_VEYE_CSSC132", "VEYE_CSSC132" },
    { 63, "X_CAM_TYPE_RPI_V4L2_VEYE_MVCAM", "VEYE_MVCAM" },
    { 70, "X_CAM_TYPE_X20_HDZERO_GENERIC", "X20_HDZERO_GENERIC" },
    { 71, "X_CAM_TYPE_X20_HDZERO_RUNCAM_V1", "X20_HDZERO_RUNCAM_V1" },
    { 72, "X_CAM_TYPE_X20_HDZERO_RUNCAM_V2", "X20_HDZERO_RUNCAM_V2" },
    { 73, "X_CAM_TYPE_X20_HDZERO_RUNCAM_V3", "X20_HDZERO_RUNCAM_V3" },
    { 74, "X_CAM_TYPE_X20_HDZERO_RUNCAM_NANO_90", "X20_HDZERO_RUNCAM_NANO" },
    { 75, "X_CAM_TYPE_X20_OHD_Jaguar", "X20_OHD_Jaguar" },
    { 76, "X_CAM_TYPE_X21_OHD_Jaguar", "X21_OHD_Jaguar" },
    { 77, "X_CAM_TYPE_A733_IMX415", "A733_IMX415" },
    { 78, "X_CAM_TYPE_A733_IMX219", "A733_IMX219" },
    { 79, "X_CAM_TYPE_A733_IMX214", "A733_IMX214" },
    { 80, "X_CAM_TYPE_ROCK_5_HDMI_IN", "ROCK_5_HDMI_IN" },
    { 81, "X_CAM_TYPE_ROCK_5_OV5647", "ROCK_5_OV5647" },
    { 82, "X_CAM_TYPE_ROCK_5_IMX219", "ROCK_5_IMX219" },
    { 83, "X_CAM_TYPE_ROCK_5_IMX708", "ROCK_5_IMX708" },
    { 84, "X_CAM_TYPE_ROCK_5_IMX462", "ROCK_5_IMX462" },
    { 85, "X_CAM_TYPE_ROCK_5_IMX415", "ROCK_5_IMX415" },
    { 86, "X_CAM_TYPE_ROCK_5_IMX477", "ROCK_5_IMX415" },
    { 87, "X_CAM_TYPE_ROCK_5_IMX519", "ROCK_5_IMX415" },
    { 88, "X_CAM_TYPE_ROCK_5_OHD_Jaguar", "ROCK_5_OHD_Jaguar" },
    { 90, "X_CAM_TYPE_ROCK_3_HDMI_IN", "ROCK_3_HDMI_IN" },
    { 91, "X_CAM_TYPE_ROCK_3_OV5647", "ROCK_3_OV5647" },
    { 92, "X_CAM_TYPE_ROCK_3_IMX219", "ROCK_3_IMX219" },
    { 93, "X_CAM_TYPE_ROCK_3_IMX708", "ROCK_3_IMX708" },
    { 94, "X_CAM_TYPE_ROCK_3_IMX462", "ROCK_3_IMX462" },
    { 95, "X_CAM_TYPE_ROCK_3_IMX519", "ROCK_3_IMX519" },
    { 96, "X_CAM_TYPE_ROCK_3_OHD_Jaguar", "ROCK_3_OHD_Jaguar" },
    { 97, "X_CAM_TYPE_ROCK_3_VEYE", "ROCK_3_VEYE" },
    { 101, "X_CAM_TYPE_NVIDIA_XAVIER_IMX577", "XAVIER_IMX577" },
    { 110, "X_CAM_TYPE_OPENIPC_GENERIC", "OPENIPC_X" },
    { 120, "X_CAM_TYPE_QC_IMX577", "CORETRONIC IMX577" },
    { 121, "X_CAM_TYPE_QC_OV9282", "CORETRONIC OV9282" },
    { 122, "X_CAM_TYPE_ORQA_HORNET", "ORQA_HORNET" },
    { 123, "X_CAM_TYPE_ORQA_JAGUAR", "ORQA_JAGUAR" },
    { 124, "X_CAM_TYPE_ORQA_REKINDLE", "ORQA_REKINDLE" },
    { 125, "X_CAM_TYPE_ORQA_ORCA_DIGITAL_V2", "ORQA_ORCA_DIGITAL_V2" },
    { 130, "X_CAM_TYPE_NXP_IMX8_V4L2", "NXP_IMX8_V4L2" },
    { 131, "X_CAM_TYPE_NXP_IMX8_OS08A20", "NXP_IMX8_OS08A20" },
    { 140, "X_CAM_TYPE_ROCKCHIP_RV110X", "LUCKFOX_MIS5001" },
    { 141, "X_CAM_TYPE_ROCKCHIP_RV1126_CSI", "RV1126B_CSI" },
    { 142, "X_CAM_TYPE_ROCKCHIP_RV1126_TEST", "RV1126B_TEST" },
    { 255, "X_CAM_TYPE_DISABLED", "DISABLED" },
}};

inline std::string type_to_string(int camera_type)
{
    for (const auto& entry : ids) {
        if (entry.id == camera_type) {
            return std::string(entry.name);
        }
    }
    return "UNKNOWN (" + std::to_string(camera_type) + ")";
}

} // namespace camera

namespace wifi_card {

inline constexpr std::array<IdEntry, 17> ids {{
    { 0, "OPENHD_RTL_88X2AU", "OPENHD_RTL_88X2AU" },
    { 1, "OPENHD_RTL_88X2BU", "OPENHD_RTL_88X2BU" },
    { 2, "OPENHD_RTL_88X2CU", "OPENHD_RTL_88X2CU" },
    { 3, "OPENHD_RTL_88X2EU", "OPENHD_RTL_88X2EU" },
    { 4, "RTL_88X2AU", "RTL_88X2AU" },
    { 5, "RTL_88X2BU", "RTL_88X2BU" },
    { 6, "ATHEROS", "ATHEROS" },
    { 7, "MT_7921u", "MT_7921u" },
    { 8, "RALINK", "RALINK" },
    { 9, "INTEL", "INTEL" },
    { 10, "BROADCOM", "BROADCOM" },
    { 11, "OPENHD_RTL_8852BU", "OPENHD_RTL_8852BU" },
    { 12, "OPENHD_EMULATED", "OPENHD_EMULATED" },
    { 13, "AIC", "AIC" },
    { 14, "QUALCOMM", "QUALCOMM" },
    { 15, "UNKNOWN", "UNKNOWN" },
    { 16, "ARTOSYN", "ARTOSYN" },
}};

inline std::string type_to_string(int card_type)
{
    for (const auto& entry : ids) {
        if (entry.id == card_type) {
            return std::string(entry.name);
        }
    }
    return "UNKNOWN";
}

} // namespace wifi_card

template <std::size_t Size>
consteval bool ids_are_unique(const std::array<IdEntry, Size>& entries)
{
    for (std::size_t left = 0; left < Size; ++left) {
        for (std::size_t right = left + 1; right < Size; ++right) {
            if (entries[left].id == entries[right].id) {
                return false;
            }
        }
    }
    return true;
}

static_assert(ids_are_unique(platform::ids), "OpenHD platform IDs must be unique");
static_assert(ids_are_unique(camera::ids), "OpenHD camera IDs must be unique");
static_assert(ids_are_unique(wifi_card::ids), "OpenHD Wi-Fi card IDs must be unique");

namespace wire {

inline constexpr std::uint32_t stats_monitor_mode_wifi_link_message_id = 1211;
inline constexpr std::uint32_t stats_monitor_mode_wifi_card_message_id = 1212;
inline constexpr std::uint32_t core_status_message_id = 1227;

inline constexpr std::size_t wifi_link_frequency_mhz_offset = 26;
inline constexpr std::size_t wifi_link_rate_kbits_offset = 28;
inline constexpr std::size_t wifi_link_packet_loss_offset = 32;
inline constexpr std::size_t wifi_link_channel_width_offset = 33;
inline constexpr std::size_t wifi_link_mcs_index_offset = 34;
inline constexpr std::size_t wifi_link_snr_antenna1_offset = 38;
inline constexpr std::size_t wifi_link_snr_antenna2_offset = 39;
inline constexpr std::size_t wifi_link_temperature_offset = 40;

inline constexpr std::size_t wifi_card_rssi_offset = 23;
inline constexpr std::size_t wifi_card_type_offset = 21;
inline constexpr std::size_t wifi_card_quality_offset = 29;
inline constexpr std::size_t wifi_card_snr_antenna1_offset = 34;
inline constexpr std::size_t wifi_card_snr_antenna2_offset = 35;
inline constexpr std::size_t wifi_card_temperature_offset = 36;

inline constexpr std::size_t core_status_cpu_temperature_offset = 14;
inline constexpr std::size_t core_status_platform_type_offset = 18;

} // namespace wire

} // namespace glide::openhd
