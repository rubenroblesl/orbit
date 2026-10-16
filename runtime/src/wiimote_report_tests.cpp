// Decoding of Wii Remote input reports and the host's output reports (galaxy/wiimote_report.h).
//
// The fixed byte arrays below are reports as a Wii Remote sends them, assembled by hand from the
// documented layouts (they are not captures from a physical remote); each is also checked against
// build_wiimote_input_report, the encoder of the virtual remote, where that encoder covers it.

#include "galaxy/wiimote_report.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using galaxy::input::DecodedWiimoteReport;
using galaxy::input::WiimoteIrFormat;

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

template <std::size_t N>
std::optional<DecodedWiimoteReport> decode(const std::array<std::uint8_t, N>& report) {
    return galaxy::input::decode_wiimote_input_report(report);
}

bool same_buttons(const galaxy::input::WiimoteButtons& a, const galaxy::input::WiimoteButtons& b) {
    return a.left == b.left && a.right == b.right && a.down == b.down && a.up == b.up && a.plus == b.plus &&
           a.two == b.two && a.one == b.one && a.b == b.b && a.a == b.a && a.minus == b.minus && a.home == b.home;
}

bool same_bytes(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected, const char* message) {
    const bool same = actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin());
    if (!same) {
        std::cerr << "FAILED: " << message << " actual=";
        for (const std::uint8_t byte : actual) std::cerr << std::hex << static_cast<unsigned>(byte) << ' ';
        std::cerr << std::dec << '\n';
    }
    return same;
}

bool decodes_core_buttons() {
    bool ok = true;
    const auto a_held = decode(std::array<std::uint8_t, 4>{0xa1, 0x30, 0x00, 0x08});
    ok &= expect(a_held.has_value() && a_held->report_id == 0x30, "0x30 decodes");
    ok &= expect(a_held->buttons.a && !a_held->buttons.b && !a_held->has_accel, "0x30 A alone");
    ok &= expect(a_held->ir_format == WiimoteIrFormat::None && a_held->extension_size == 0u, "0x30 has no IR or extension");

    const auto all = decode(std::array<std::uint8_t, 4>{0xa1, 0x30, 0x1f, 0x9f});
    galaxy::input::WiimoteButtons every{true, true, true, true, true, true, true, true, true, true, true};
    ok &= expect(all.has_value() && same_buttons(all->buttons, every), "0x30 every button");
    ok &= expect(galaxy::input::wiimote_buttons_text(all->buttons) == "A B 1 2 - + HOME UP DOWN LEFT RIGHT",
        "button text lists every button");

    // The accelerometer's low bits ride in the unused button bits; they are not buttons.
    const auto none = decode(std::array<std::uint8_t, 4>{0xa1, 0x30, 0x60, 0x60});
    ok &= expect(none.has_value() && same_buttons(none->buttons, {}), "accelerometer bits are not buttons");
    ok &= expect(galaxy::input::wiimote_buttons_text(none->buttons) == "-", "no button text");
    return ok;
}

bool decodes_accelerometer_with_low_bits() {
    bool ok = true;
    // A remote lying face up: the rest values of the virtual remote.
    const auto flat = decode(std::array<std::uint8_t, 7>{0xa1, 0x31, 0x00, 0x00, 0x7f, 0x81, 0x99});
    ok &= expect(flat.has_value() && flat->has_accel, "0x31 has the accelerometer");
    ok &= expect(flat->accel.x == galaxy::input::kWiimoteRestAccelX && flat->accel.y == galaxy::input::kWiimoteRestAccelY &&
                     flat->accel.z == galaxy::input::kWiimoteRestAccelZ,
        "0x31 flat remote");

    // A held, X1:0 = 3, Y1 = 1, Z1 = 1.
    const auto low = decode(std::array<std::uint8_t, 7>{0xa1, 0x31, 0x60, 0x68, 0x7f, 0x81, 0x99});
    ok &= expect(low.has_value() && low->buttons.a && !low->buttons.b && !low->buttons.minus, "0x31 buttons beside low bits");
    ok &= expect(low->accel.x == 0x1ffu && low->accel.y == 0x206u && low->accel.z == 0x266u, "0x31 low bits");
    return ok;
}

bool decodes_extended_ir() {
    bool ok = true;
    // Two sensor bar dots: (420, 380) size 3 and (700, 240) size 2.
    const auto report = decode(std::array<std::uint8_t, 19>{0xa1, 0x33, 0x00, 0x00, 0x7f, 0x81, 0x99, 0xa4, 0x7c,
        0x53, 0xbc, 0xf0, 0x22, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});
    ok &= expect(report.has_value() && report->ir_format == WiimoteIrFormat::Extended, "0x33 extended IR");
    ok &= expect(report->ir[0].visible && report->ir[0].x == 420u && report->ir[0].y == 380u && report->ir[0].size == 3u,
        "0x33 first dot");
    ok &= expect(report->ir[1].visible && report->ir[1].x == 700u && report->ir[1].y == 240u && report->ir[1].size == 2u,
        "0x33 second dot");
    ok &= expect(!report->ir[2].visible && !report->ir[3].visible, "0x33 absent dots");
    ok &= expect(galaxy::input::describe_wiimote_report(*report) == "33 - accel=508,516,612 ir=(420,380)(700,240)",
        "0x33 description");
    return ok;
}

bool decodes_basic_ir_and_nunchuk() {
    bool ok = true;
    // B held, dots (420, 380) and (700, 240), the second pair absent, then the Nunchuk: stick
    // centred, accelerometer 0x1eb/0x1f9/0x272, Z held (active low), C released.
    const std::array<std::uint8_t, 23> bytes{0xa1, 0x37, 0x00, 0x04, 0x7f, 0x81, 0x99, 0xa4, 0x7c, 0x52, 0xbc, 0xf0,
        0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x80, 0x7a, 0x7e, 0x9c, 0x9e};
    const auto report = decode(bytes);
    ok &= expect(report.has_value() && report->buttons.b && report->has_accel, "0x37 decodes");
    ok &= expect(report->ir_format == WiimoteIrFormat::Basic, "0x37 basic IR");
    ok &= expect(report->ir[0].visible && report->ir[0].x == 420u && report->ir[0].y == 380u, "0x37 first dot");
    ok &= expect(report->ir[1].visible && report->ir[1].x == 700u && report->ir[1].y == 240u, "0x37 second dot");
    ok &= expect(!report->ir[2].visible && !report->ir[3].visible, "0x37 absent pair");
    ok &= expect(report->extension_offset == 17u && report->extension_size == 6u, "0x37 extension bytes");

    const auto nunchuk = galaxy::input::decode_nunchuk_extension(
        std::span<const std::uint8_t, 6>(bytes.data() + report->extension_offset, 6));
    ok &= expect(nunchuk.stick_x == 0x80u && nunchuk.stick_y == 0x80u, "Nunchuk stick");
    ok &= expect(nunchuk.accel.x == 0x1ebu && nunchuk.accel.y == 0x1f9u && nunchuk.accel.z == 0x272u, "Nunchuk accelerometer");
    ok &= expect(nunchuk.z && !nunchuk.c, "Nunchuk buttons are active low");
    return ok;
}

bool matches_the_virtual_remote_encoder() {
    galaxy::input::WiimoteInputSnapshot snapshot;
    snapshot.buttons.a = true;
    snapshot.buttons.minus = true;
    snapshot.buttons.right = true;
    // Y0 and Z0 are not sent, so they are even here.
    snapshot.accel = {0x1e3, 0x21a, 0x2c6};
    snapshot.ir[0] = {true, 0x123, 0x2a1, 5};
    snapshot.ir[1] = {true, 0x3f0, 0x010, 1};
    snapshot.ir[3] = {true, 0x200, 0x180, 4};
    snapshot.nunchuk.stick_x = 0x21;
    snapshot.nunchuk.stick_y = 0xe0;
    snapshot.nunchuk.accel = {0x1a7, 0x2fe, 0x301};
    snapshot.nunchuk.c = true;

    bool ok = true;
    for (std::uint8_t mode = 0x30; mode <= 0x37; ++mode) {
        std::array<std::uint8_t, galaxy::input::kMaxWiimoteInputReportBytes> bytes{};
        const auto built = galaxy::input::build_wiimote_input_report(mode, snapshot, bytes);
        const auto report =
            galaxy::input::decode_wiimote_input_report(std::span<const std::uint8_t>(bytes.data(), built.size));
        if (!expect(report.has_value() && report->report_id == mode, "encoded report decodes")) return false;
        ok &= expect(same_buttons(report->buttons, snapshot.buttons), "encoded buttons");
        const bool accel = mode == 0x31 || mode == 0x33 || mode == 0x35 || mode == 0x37;
        ok &= expect(report->has_accel == accel, "encoded accelerometer presence");
        if (accel) {
            ok &= expect(report->accel.x == snapshot.accel.x && report->accel.y == snapshot.accel.y &&
                             report->accel.z == snapshot.accel.z,
                "encoded accelerometer");
        }
        if (report->ir_format != WiimoteIrFormat::None) {
            for (std::size_t i = 0; i < 4; ++i) {
                const auto& got = report->ir[i];
                const auto& want = snapshot.ir[i];
                ok &= expect(got.visible == want.visible, "encoded IR visibility");
                if (want.visible) ok &= expect(got.x == want.x && got.y == want.y, "encoded IR position");
                if (want.visible && report->ir_format == WiimoteIrFormat::Extended) {
                    ok &= expect(got.size == want.size, "encoded IR size");
                }
            }
        }
        if (report->extension_size >= 6u) {
            const auto nunchuk = galaxy::input::decode_nunchuk_extension(
                std::span<const std::uint8_t, 6>(bytes.data() + report->extension_offset, 6));
            ok &= expect(nunchuk.stick_x == 0x21u && nunchuk.stick_y == 0xe0u && nunchuk.c && !nunchuk.z,
                "encoded Nunchuk stick and buttons");
            ok &= expect(nunchuk.accel.x == 0x1a7u && nunchuk.accel.y == 0x2feu && nunchuk.accel.z == 0x301u,
                "encoded Nunchuk accelerometer");
        }
    }
    return ok;
}

bool decodes_replies() {
    bool ok = true;
    // Status: LED 1, extension plugged in, battery 0xc8.
    const auto status = decode(std::array<std::uint8_t, 8>{0xa1, 0x20, 0x00, 0x00, 0x12, 0x00, 0x00, 0xc8});
    ok &= expect(status.has_value() && status->status.has_value(), "0x20 decodes");
    ok &= expect(status->status->led_mask == 0x10u && status->status->extension_connected &&
                     !status->status->ir_enabled && !status->status->battery_low && status->status->battery_level == 0xc8u,
        "0x20 fields");

    galaxy::input::WiimoteDeviceStatus device;
    device.led_mask = 0x90;
    device.battery_low = true;
    device.extension_connected = false;
    device.ir_enabled = true;
    device.speaker_enabled = true;
    device.battery_level = 0x12;
    std::array<std::uint8_t, 8> built{};
    (void)galaxy::input::build_wiimote_status_report(device, built);
    const auto rebuilt = decode(built);
    ok &= expect(rebuilt.has_value() && rebuilt->status->led_mask == 0x90u && rebuilt->status->battery_low &&
                     !rebuilt->status->extension_connected && rebuilt->status->ir_enabled &&
                     rebuilt->status->speaker_enabled && rebuilt->status->battery_level == 0x12u,
        "encoded 0x20 fields");

    // The Nunchuk's identifier, read from 0xA400FA.
    const auto read = decode(std::array<std::uint8_t, 23>{0xa1, 0x21, 0x00, 0x00, 0x50, 0x00, 0xfa, 0x00, 0x00, 0xa4,
        0x20, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    ok &= expect(read.has_value() && read->memory.has_value(), "0x21 decodes");
    ok &= expect(read->memory->size == 6u && read->memory->error == 0u && read->memory->offset == 0x00fau, "0x21 fields");
    ok &= expect(galaxy::input::classify_wiimote_extension(std::span<const std::uint8_t, 6>(read->memory->data.data(), 6)) ==
                     galaxy::input::WiimoteExtensionKind::Nunchuk,
        "0x21 Nunchuk identifier");

    const std::array<std::uint8_t, 3> data{0x01, 0x02, 0x03};
    std::array<std::uint8_t, 23> memory{};
    (void)galaxy::input::build_wiimote_read_memory_report({}, 0x0016, 0, data, memory);
    const auto rebuilt_read = decode(memory);
    ok &= expect(rebuilt_read.has_value() && rebuilt_read->memory->size == 3u && rebuilt_read->memory->offset == 0x0016u &&
                     rebuilt_read->memory->data[2] == 0x03u,
        "encoded 0x21 fields");

    std::array<std::uint8_t, 6> ack{};
    (void)galaxy::input::build_wiimote_ack_report({}, 0x16, 0x00, ack);
    const auto acked = decode(ack);
    ok &= expect(acked.has_value() && acked->ack.has_value() && acked->ack->report_id == 0x16u && acked->ack->error == 0u,
        "0x22 decodes");
    ok &= expect(galaxy::input::describe_wiimote_report(*acked) == "22 - ack report=16 error=0", "0x22 description");
    return ok;
}

bool rejects_malformed_reports() {
    bool ok = true;
    ok &= expect(!decode(std::array<std::uint8_t, 4>{0x30, 0x00, 0x08, 0x00}).has_value(), "unframed report rejected");
    ok &= expect(!decode(std::array<std::uint8_t, 10>{0xa1, 0x37, 0, 0, 0, 0, 0, 0, 0, 0}).has_value(),
        "short 0x37 rejected");
    ok &= expect(!decode(std::array<std::uint8_t, 7>{0xa1, 0x20, 0, 0, 0, 0, 0}).has_value(), "short 0x20 rejected");
    ok &= expect(!decode(std::array<std::uint8_t, 23>{0xa1, 0x3d}).has_value(), "unsupported 0x3d rejected");
    ok &= expect(!decode(std::array<std::uint8_t, 3>{0xa1, 0x30, 0x00}).has_value(), "truncated buttons rejected");
    return ok;
}

bool decrypts_rmge01_extension_bytes() {
    const std::array<std::uint8_t, 8> additive{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    const std::array<std::uint8_t, 8> xor_key{0x9a, 0x8b, 0x7c, 0x6d, 0x5e, 0x4f, 0x30, 0x21};
    bool ok = true;
    for (const std::uint8_t mode : {std::uint8_t{0x32}, std::uint8_t{0x35}, std::uint8_t{0x37}}) {
        galaxy::input::WiimoteInputSnapshot snapshot;
        snapshot.nunchuk.stick_x = 0x40;
        snapshot.nunchuk.z = true;
        std::array<std::uint8_t, galaxy::input::kMaxWiimoteInputReportBytes> plain{};
        const auto built = galaxy::input::build_wiimote_input_report(mode, snapshot, plain);
        auto cipher = plain;
        const std::span<std::uint8_t> report(cipher.data(), built.size);
        ok &= expect(galaxy::input::encrypt_rmge01_nunchuk_extension_payload(report, additive, xor_key), "encrypts");
        ok &= expect(galaxy::input::decrypt_rmge01_nunchuk_extension_payload(report, additive, xor_key), "decrypts");
        ok &= same_bytes(report, std::span<const std::uint8_t>(plain.data(), built.size), "decryption inverts encryption");
    }
    std::array<std::uint8_t, 7> no_extension{0xa1, 0x31, 0, 0, 0x80, 0x80, 0x80};
    ok &= expect(!galaxy::input::decrypt_rmge01_nunchuk_extension_payload(no_extension, additive, xor_key),
        "no extension bytes to decrypt");
    return ok;
}

bool classifies_extensions() {
    using galaxy::input::WiimoteExtensionKind;
    const auto kind = [](std::array<std::uint8_t, 6> id) { return galaxy::input::classify_wiimote_extension(id); };
    bool ok = true;
    ok &= expect(kind({0xff, 0xff, 0xff, 0xff, 0xff, 0xff}) == WiimoteExtensionKind::None, "no extension");
    ok &= expect(kind({0x00, 0x00, 0xa4, 0x20, 0x00, 0x00}) == WiimoteExtensionKind::Nunchuk, "Nunchuk");
    ok &= expect(kind({0xff, 0x00, 0xa4, 0x20, 0x00, 0x00}) == WiimoteExtensionKind::Nunchuk, "third-party Nunchuk");
    ok &= expect(kind({0x00, 0x00, 0xa4, 0x20, 0x01, 0x01}) == WiimoteExtensionKind::ClassicController, "Classic");
    ok &= expect(kind({0x01, 0x00, 0xa4, 0x20, 0x01, 0x01}) == WiimoteExtensionKind::ClassicControllerPro, "Classic Pro");
    ok &= expect(kind({0x00, 0x00, 0xa4, 0x20, 0x04, 0x05}) == WiimoteExtensionKind::Unknown, "MotionPlus is not handled");
    return ok;
}

bool decodes_accelerometer_calibration() {
    bool ok = true;
    // The virtual remote's EEPROM block (native_host.cpp create_virtual_wiimote_eeprom).
    const std::array<std::uint8_t, 10> plain{0x80, 0x80, 0x80, 0x00, 0x99, 0x99, 0x99, 0x00, 0x00, 0xa0};
    const auto calibration = galaxy::input::decode_wiimote_accel_calibration(plain);
    ok &= expect(calibration.has_value(), "calibration checksum");
    ok &= expect(calibration->zero_g.x == 0x200u && calibration->zero_g.z == 0x200u && calibration->one_g.y == 0x264u,
        "calibration points");

    const std::array<std::uint8_t, 10> low_bits{0x7e, 0x81, 0x7d, 0x27, 0x99, 0x9a, 0x98, 0x1c, 0x00, 0xdf};
    const auto precise = galaxy::input::decode_wiimote_accel_calibration(low_bits);
    ok &= expect(precise.has_value(), "calibration with low bits");
    ok &= expect(precise->zero_g.x == 0x1fau && precise->zero_g.y == 0x205u && precise->zero_g.z == 0x1f7u,
        "calibration zero-g low bits");
    ok &= expect(precise->one_g.x == 0x265u && precise->one_g.y == 0x26bu && precise->one_g.z == 0x260u,
        "calibration one-g low bits");

    std::array<std::uint8_t, 10> corrupt = plain;
    corrupt[9] = 0xa1;
    ok &= expect(!galaxy::input::decode_wiimote_accel_calibration(corrupt).has_value(), "bad checksum rejected");
    const std::array<std::uint8_t, 10> flat{0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0x80, 0x00, 0x00, 0x55};
    ok &= expect(!galaxy::input::decode_wiimote_accel_calibration(flat).has_value(), "rangeless calibration rejected");

    const galaxy::input::Axis10 shake{static_cast<std::uint16_t>(0x200 + 2 * 0x64), 0x200, 0x264};
    const galaxy::input::AccelG g = galaxy::input::wiimote_accel_in_g(shake, *calibration);
    ok &= expect(std::fabs(g.x - 2.0f) < 1e-4f && std::fabs(g.y) < 1e-4f && std::fabs(g.z - 1.0f) < 1e-4f,
        "acceleration in g");
    return ok;
}

bool builds_output_reports() {
    using galaxy::input::WiimoteMemorySpace;
    bool ok = true;
    const auto check = [&](const galaxy::input::WiimoteOutputReport& report, std::vector<std::uint8_t> expected,
                           const char* message) { ok &= same_bytes(report.span(), expected, message); };
    check(galaxy::input::wiimote_output_leds(0x1, false), {0xa2, 0x11, 0x10}, "LED 1");
    check(galaxy::input::wiimote_output_leds(0x9, true), {0xa2, 0x11, 0x91}, "LEDs 1 and 4 with rumble");
    check(galaxy::input::wiimote_output_rumble(true), {0xa2, 0x10, 0x01}, "rumble");
    check(galaxy::input::wiimote_output_report_mode(0x37, true, false), {0xa2, 0x12, 0x04, 0x37}, "continuous 0x37");
    check(galaxy::input::wiimote_output_report_mode(0x30, false, true), {0xa2, 0x12, 0x01, 0x30}, "0x30 with rumble");
    check(galaxy::input::wiimote_output_enable(0x13, true, false), {0xa2, 0x13, 0x04}, "IR clock");
    check(galaxy::input::wiimote_output_enable(0x1a, true, true), {0xa2, 0x1a, 0x05}, "IR camera with rumble");
    check(galaxy::input::wiimote_output_status_request(false), {0xa2, 0x15, 0x00}, "status request");
    const std::array<std::uint8_t, 1> init{0x55};
    std::vector<std::uint8_t> write{0xa2, 0x16, 0x04, 0xa4, 0x00, 0xf0, 0x01, 0x55};
    write.resize(23, 0x00);
    check(galaxy::input::wiimote_output_write_memory(WiimoteMemorySpace::Registers, galaxy::input::kWiimoteExtensionInit1,
              init, false),
        write, "extension handshake write");
    check(galaxy::input::wiimote_output_read_memory(WiimoteMemorySpace::Registers,
              galaxy::input::kWiimoteExtensionIdentifier, 6, false),
        {0xa2, 0x17, 0x04, 0xa4, 0x00, 0xfa, 0x00, 0x06}, "extension identifier read");
    check(galaxy::input::wiimote_output_read_memory(WiimoteMemorySpace::Eeprom, galaxy::input::kWiimoteAccelCalibration,
              10, true),
        {0xa2, 0x17, 0x01, 0x00, 0x00, 0x16, 0x00, 0x0a}, "calibration read with rumble");
    ok &= expect(galaxy::input::wiimote_output_write_memory(WiimoteMemorySpace::Registers, 0, {}, false).size == 0u,
        "empty write rejected");
    const std::array<std::uint8_t, 17> too_long{};
    ok &= expect(galaxy::input::wiimote_output_write_memory(WiimoteMemorySpace::Registers, 0, too_long, false).size == 0u,
        "17-byte write rejected");

    // Each one is a report the bridges pass on (the report ID first, at its own length).
    const auto mode = galaxy::input::wiimote_output_report_mode(0x33, true, false);
    std::array<std::uint8_t, 3> hid{};
    const auto converted = galaxy::input::build_wiimote_hid_output_report(mode.span(), hid);
    ok &= expect(converted.status == galaxy::input::WiimoteReportBuildStatus::Ok && hid[0] == 0x12u && hid[2] == 0x33u,
        "output report converts for the HID bridge");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= decodes_core_buttons();
    ok &= decodes_accelerometer_with_low_bits();
    ok &= decodes_extended_ir();
    ok &= decodes_basic_ir_and_nunchuk();
    ok &= matches_the_virtual_remote_encoder();
    ok &= decodes_replies();
    ok &= rejects_malformed_reports();
    ok &= decrypts_rmge01_extension_bytes();
    ok &= classifies_extensions();
    ok &= decodes_accelerometer_calibration();
    ok &= builds_output_reports();
    if (ok) std::cout << "wiimote report tests passed\n";
    return ok ? 0 : 1;
}
