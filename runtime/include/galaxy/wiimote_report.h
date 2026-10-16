#pragma once

// Decoding of real Wii Remote input reports and building of host-side output reports.
//
// The game decodes the reports of a real Wii Remote itself (they reach its Bluetooth stack
// unchanged, see RealWiimoteHidBridge in native_host.cpp). The host decodes them only to describe
// a remote: the macOS bridge's GALAXY_TRACE_WIIMOTE trace and the nebula_wiimote_probe tool, which
// also drives a remote on its own with the output reports below. Reports are in the framed form
// the bridges exchange: A1 + report ID + payload for input, A2 + report ID + payload for output.
// Layouts follow build_wiimote_input_report (native_input.h), whose reports these decode.

#include "galaxy/native_input.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace galaxy::input {

enum class WiimoteIrFormat : std::uint8_t {
    None,
    Basic,     // 10 bytes: two pairs of 5, positions only (modes 0x36, 0x37)
    Extended,  // 12 bytes: four dots of 3, with size (mode 0x33)
};

struct WiimoteStatusReport {
    std::uint8_t led_mask = 0;  // LED 1..4 in bits 4..7, as the remote reports it
    bool battery_low = false;
    bool extension_connected = false;
    bool speaker_enabled = false;
    bool ir_enabled = false;
    std::uint8_t battery_level = 0;
};

struct WiimoteMemoryReadReport {
    std::uint8_t error = 0;  // 0 ok, 7 write-only/absent, 8 bad address
    std::uint8_t size = 0;   // 1..16 bytes in this report
    std::uint16_t offset = 0;  // low 16 bits of the first byte's address
    std::array<std::uint8_t, 16> data{};
};

struct WiimoteAckReport {
    std::uint8_t report_id = 0;  // the output report acknowledged
    std::uint8_t error = 0;
};

struct DecodedWiimoteReport {
    std::uint8_t report_id = 0;
    WiimoteButtons buttons{};
    bool has_accel = false;
    Axis10 accel{};
    WiimoteIrFormat ir_format = WiimoteIrFormat::None;
    std::array<IrDot, 4> ir{};
    // Extension bytes inside the framed report (still encrypted when the game enabled that).
    std::size_t extension_offset = 0;
    std::size_t extension_size = 0;
    std::optional<WiimoteStatusReport> status;      // 0x20
    std::optional<WiimoteMemoryReadReport> memory;  // 0x21
    std::optional<WiimoteAckReport> ack;            // 0x22
};

// Decodes a framed input report (A1 first) of a supported ID (0x20-0x22, 0x30-0x37); nullopt for
// anything else or a report shorter than its ID's size.
[[nodiscard]] std::optional<DecodedWiimoteReport> decode_wiimote_input_report(
    std::span<const std::uint8_t> report) noexcept;

// For logs: "A B HOME" ("-" when nothing is held).
[[nodiscard]] std::string wiimote_buttons_text(const WiimoteButtons& buttons);
// For logs: "37 A accel=512,516,612 ir=(512,384)(600,380) ext=6", the status, read and ack fields of
// 0x20-0x22.
[[nodiscard]] std::string describe_wiimote_report(const DecodedWiimoteReport& report);

// The six Nunchuk bytes (unencrypted) at the start of an extension block.
[[nodiscard]] NunchukState decode_nunchuk_extension(
    std::span<const std::uint8_t, 6> bytes) noexcept;

// Inverse of encrypt_rmge01_nunchuk_extension_payload: decodes the extension bytes of a complete
// framed report with the tables RMGE01's WPAD derived for the channel.
[[nodiscard]] bool decrypt_rmge01_nunchuk_extension_payload(
    std::span<std::uint8_t> report,
    std::span<const std::uint8_t, 8> additive_key,
    std::span<const std::uint8_t, 8> xor_key) noexcept;

enum class WiimoteExtensionKind : std::uint8_t {
    None,
    Nunchuk,
    ClassicController,
    ClassicControllerPro,
    Unknown,
};

// The six identification bytes read from 0xA400FA after the unencrypted extension handshake.
[[nodiscard]] WiimoteExtensionKind classify_wiimote_extension(
    std::span<const std::uint8_t, 6> identifier) noexcept;

// Accelerometer calibration: the ten bytes at EEPROM 0x0016 (the remote keeps a copy at 0x0020).
// The defaults are the rest values of a flat remote with 100 counts per g.
struct WiimoteAccelCalibration {
    Axis10 zero_g{kWiimoteRestAccelX, kWiimoteRestAccelY, kWiimoteRestAccelZ - 100u};
    Axis10 one_g{kWiimoteRestAccelX + 100u, kWiimoteRestAccelY + 100u, kWiimoteRestAccelZ};
};

// nullopt when the checksum does not match or an axis has no range.
[[nodiscard]] std::optional<WiimoteAccelCalibration> decode_wiimote_accel_calibration(
    std::span<const std::uint8_t, 10> bytes) noexcept;

struct AccelG {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] AccelG wiimote_accel_in_g(
    const Axis10& raw,
    const WiimoteAccelCalibration& calibration) noexcept;

// ----------------------------------------------------------------------------------------------
// Output reports (A2 + report ID + payload). The rumble flag is bit 0 of every report's first
// payload byte, so each builder takes the motor state to keep.

struct WiimoteOutputReport {
    std::array<std::uint8_t, 23> bytes{};
    std::size_t size = 0;
    [[nodiscard]] std::span<const std::uint8_t> span() const noexcept {
        return std::span<const std::uint8_t>(bytes.data(), size);
    }
};

enum class WiimoteMemorySpace : std::uint8_t {
    Eeprom = 0x00,
    Registers = 0x04,  // IR camera, speaker, extension (0xA4xxxx, 0xB0xxxx)
};

// `leds`: LED 1..4 in bits 0..3.
[[nodiscard]] WiimoteOutputReport wiimote_output_leds(std::uint8_t leds, bool rumble) noexcept;
[[nodiscard]] WiimoteOutputReport wiimote_output_rumble(bool rumble) noexcept;
[[nodiscard]] WiimoteOutputReport wiimote_output_report_mode(
    std::uint8_t mode,
    bool continuous,
    bool rumble) noexcept;
// 0x13 (IR camera clock), 0x14 (speaker), 0x19 (speaker mute), 0x1A (IR camera).
[[nodiscard]] WiimoteOutputReport wiimote_output_enable(
    std::uint8_t report_id,
    bool enable,
    bool rumble) noexcept;
[[nodiscard]] WiimoteOutputReport wiimote_output_status_request(bool rumble) noexcept;
// Up to 16 bytes; size 0 (an invalid report) when `data` is empty or longer.
[[nodiscard]] WiimoteOutputReport wiimote_output_write_memory(
    WiimoteMemorySpace space,
    std::uint32_t address,
    std::span<const std::uint8_t> data,
    bool rumble) noexcept;
[[nodiscard]] WiimoteOutputReport wiimote_output_read_memory(
    WiimoteMemorySpace space,
    std::uint32_t address,
    std::uint16_t size,
    bool rumble) noexcept;

// Register addresses of the IR camera and the extension port.
inline constexpr std::uint32_t kWiimoteIrControl = 0xB00030u;
inline constexpr std::uint32_t kWiimoteIrSensitivity1 = 0xB00000u;
inline constexpr std::uint32_t kWiimoteIrSensitivity2 = 0xB0001Au;
inline constexpr std::uint32_t kWiimoteIrMode = 0xB00033u;
inline constexpr std::uint32_t kWiimoteExtensionInit1 = 0xA400F0u;
inline constexpr std::uint32_t kWiimoteExtensionInit2 = 0xA400FBu;
inline constexpr std::uint32_t kWiimoteExtensionIdentifier = 0xA400FAu;
inline constexpr std::uint32_t kWiimoteAccelCalibration = 0x000016u;

// The Wii's sensitivity setting 3 (blocks for 0xB00000 and 0xB0001A).
inline constexpr std::array<std::uint8_t, 9> kWiimoteIrSensitivityLevel3Block1{
    0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64};
inline constexpr std::array<std::uint8_t, 2> kWiimoteIrSensitivityLevel3Block2{0x63, 0x03};

}  // namespace galaxy::input
