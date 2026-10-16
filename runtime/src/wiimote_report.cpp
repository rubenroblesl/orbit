#include "galaxy/wiimote_report.h"

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace galaxy::input {

namespace {

struct ReportLayout {
    std::size_t size = 0;            // framed size, A1 included
    bool accel = false;              // three accelerometer bytes after the buttons
    WiimoteIrFormat ir = WiimoteIrFormat::None;
    std::size_t extension_size = 0;  // extension bytes after everything else
};

std::optional<ReportLayout> data_report_layout(std::uint8_t report_id) noexcept {
    switch (report_id) {
    case 0x30: return ReportLayout{4, false, WiimoteIrFormat::None, 0};
    case 0x31: return ReportLayout{7, true, WiimoteIrFormat::None, 0};
    case 0x32: return ReportLayout{12, false, WiimoteIrFormat::None, 8};
    case 0x33: return ReportLayout{19, true, WiimoteIrFormat::Extended, 0};
    case 0x34: return ReportLayout{23, false, WiimoteIrFormat::None, 19};
    case 0x35: return ReportLayout{23, true, WiimoteIrFormat::None, 16};
    case 0x36: return ReportLayout{23, false, WiimoteIrFormat::Basic, 9};
    case 0x37: return ReportLayout{23, true, WiimoteIrFormat::Basic, 6};
    default: return std::nullopt;
    }
}

WiimoteButtons decode_buttons(std::uint8_t byte0, std::uint8_t byte1) noexcept {
    WiimoteButtons buttons;
    buttons.left = (byte0 & 0x01u) != 0u;
    buttons.right = (byte0 & 0x02u) != 0u;
    buttons.down = (byte0 & 0x04u) != 0u;
    buttons.up = (byte0 & 0x08u) != 0u;
    buttons.plus = (byte0 & 0x10u) != 0u;
    buttons.two = (byte1 & 0x01u) != 0u;
    buttons.one = (byte1 & 0x02u) != 0u;
    buttons.b = (byte1 & 0x04u) != 0u;
    buttons.a = (byte1 & 0x08u) != 0u;
    buttons.minus = (byte1 & 0x10u) != 0u;
    buttons.home = (byte1 & 0x80u) != 0u;
    return buttons;
}

// The low accelerometer bits ride in the unused button bits: X1:0 in byte 0 bits 6:5, Y1 in byte 1
// bit 5 and Z1 in byte 1 bit 6 (Y0 and Z0 are not sent).
Axis10 decode_accel(std::uint8_t byte0, std::uint8_t byte1, const std::uint8_t* high) noexcept {
    Axis10 accel;
    accel.x = static_cast<std::uint16_t>((high[0] << 2) | ((byte0 >> 5) & 0x03u));
    accel.y = static_cast<std::uint16_t>((high[1] << 2) | (((byte1 >> 5) & 0x01u) << 1));
    accel.z = static_cast<std::uint16_t>((high[2] << 2) | (((byte1 >> 6) & 0x01u) << 1));
    return accel;
}

IrDot ir_dot(std::uint16_t x, std::uint16_t y, std::uint8_t size) noexcept {
    IrDot dot;
    dot.x = x;
    dot.y = y;
    dot.size = size;
    dot.visible = !(x == 0x03ffu && y == 0x03ffu);
    return dot;
}

void decode_ir_basic(const std::uint8_t* bytes, std::array<IrDot, 4>& dots) noexcept {
    for (std::size_t pair = 0; pair < 2u; ++pair) {
        const std::uint8_t* p = bytes + pair * 5u;
        const std::uint16_t x1 = static_cast<std::uint16_t>(p[0] | (((p[2] >> 4) & 0x03u) << 8));
        const std::uint16_t y1 = static_cast<std::uint16_t>(p[1] | (((p[2] >> 6) & 0x03u) << 8));
        const std::uint16_t x2 = static_cast<std::uint16_t>(p[3] | ((p[2] & 0x03u) << 8));
        const std::uint16_t y2 = static_cast<std::uint16_t>(p[4] | (((p[2] >> 2) & 0x03u) << 8));
        dots[pair * 2u] = ir_dot(x1, y1, 0);
        dots[pair * 2u + 1u] = ir_dot(x2, y2, 0);
    }
}

void decode_ir_extended(const std::uint8_t* bytes, std::array<IrDot, 4>& dots) noexcept {
    for (std::size_t i = 0; i < 4u; ++i) {
        const std::uint8_t* p = bytes + i * 3u;
        const std::uint16_t x = static_cast<std::uint16_t>(p[0] | (((p[2] >> 4) & 0x03u) << 8));
        const std::uint16_t y = static_cast<std::uint16_t>(p[1] | (((p[2] >> 6) & 0x03u) << 8));
        dots[i] = ir_dot(x, y, static_cast<std::uint8_t>(p[2] & 0x0fu));
    }
}

std::uint16_t axis10_from(std::uint8_t high, std::uint8_t low_bits) noexcept {
    return static_cast<std::uint16_t>((high << 2) | (low_bits & 0x03u));
}

WiimoteOutputReport output(std::uint8_t report_id, std::initializer_list<std::uint8_t> payload) noexcept {
    WiimoteOutputReport report;
    report.bytes[0] = kWiimoteHidOutput;
    report.bytes[1] = report_id;
    std::size_t cursor = 2;
    for (const std::uint8_t byte : payload) report.bytes[cursor++] = byte;
    report.size = cursor;
    return report;
}

std::uint8_t rumble_bit(bool rumble) noexcept { return rumble ? 0x01u : 0x00u; }

}  // namespace

std::optional<DecodedWiimoteReport> decode_wiimote_input_report(
    std::span<const std::uint8_t> report) noexcept {
    if (report.size() < 4u || report[0] != kWiimoteHidInput) return std::nullopt;
    DecodedWiimoteReport decoded;
    decoded.report_id = report[1];
    const std::uint8_t byte0 = report[2];
    const std::uint8_t byte1 = report[3];
    decoded.buttons = decode_buttons(byte0, byte1);

    switch (decoded.report_id) {
    case 0x20: {
        if (report.size() < 8u) return std::nullopt;
        WiimoteStatusReport status;
        const std::uint8_t flags = report[4];
        status.led_mask = static_cast<std::uint8_t>(flags & 0xf0u);
        status.battery_low = (flags & 0x01u) != 0u;
        status.extension_connected = (flags & 0x02u) != 0u;
        status.speaker_enabled = (flags & 0x04u) != 0u;
        status.ir_enabled = (flags & 0x08u) != 0u;
        status.battery_level = report[7];
        decoded.status = status;
        return decoded;
    }
    case 0x21: {
        if (report.size() < 23u) return std::nullopt;
        WiimoteMemoryReadReport memory;
        memory.size = static_cast<std::uint8_t>((report[4] >> 4) + 1u);
        memory.error = static_cast<std::uint8_t>(report[4] & 0x0fu);
        memory.offset = static_cast<std::uint16_t>((report[5] << 8) | report[6]);
        std::copy_n(report.begin() + 7, memory.data.size(), memory.data.begin());
        decoded.memory = memory;
        return decoded;
    }
    case 0x22: {
        if (report.size() < 6u) return std::nullopt;
        decoded.ack = WiimoteAckReport{report[4], report[5]};
        return decoded;
    }
    default:
        break;
    }

    const std::optional<ReportLayout> layout = data_report_layout(decoded.report_id);
    if (!layout.has_value() || report.size() < layout->size) return std::nullopt;
    std::size_t cursor = 4;
    if (layout->accel) {
        decoded.has_accel = true;
        decoded.accel = decode_accel(byte0, byte1, report.data() + cursor);
        cursor += 3;
    }
    decoded.ir_format = layout->ir;
    if (layout->ir == WiimoteIrFormat::Basic) {
        decode_ir_basic(report.data() + cursor, decoded.ir);
        cursor += 10;
    } else if (layout->ir == WiimoteIrFormat::Extended) {
        decode_ir_extended(report.data() + cursor, decoded.ir);
        cursor += 12;
    }
    decoded.extension_offset = layout->extension_size != 0u ? cursor : 0u;
    decoded.extension_size = layout->extension_size;
    return decoded;
}

std::string wiimote_buttons_text(const WiimoteButtons& buttons) {
    std::string text;
    const auto add = [&](bool held, const char* name) {
        if (!held) return;
        if (!text.empty()) text += ' ';
        text += name;
    };
    add(buttons.a, "A");
    add(buttons.b, "B");
    add(buttons.one, "1");
    add(buttons.two, "2");
    add(buttons.minus, "-");
    add(buttons.plus, "+");
    add(buttons.home, "HOME");
    add(buttons.up, "UP");
    add(buttons.down, "DOWN");
    add(buttons.left, "LEFT");
    add(buttons.right, "RIGHT");
    return text.empty() ? "-" : text;
}

std::string describe_wiimote_report(const DecodedWiimoteReport& report) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%02x ", report.report_id);
    std::string text = buffer;
    text += wiimote_buttons_text(report.buttons);
    if (report.status.has_value()) {
        const WiimoteStatusReport& status = *report.status;
        std::snprintf(buffer, sizeof(buffer), " status leds=%x battery=%u%s%s%s%s", status.led_mask >> 4,
            status.battery_level, status.battery_low ? " low" : "", status.extension_connected ? " extension" : "",
            status.ir_enabled ? " ir" : "", status.speaker_enabled ? " speaker" : "");
        text += buffer;
    }
    if (report.memory.has_value()) {
        std::snprintf(buffer, sizeof(buffer), " read offset=%04x size=%u error=%u", report.memory->offset,
            report.memory->size, report.memory->error);
        text += buffer;
    }
    if (report.ack.has_value()) {
        std::snprintf(buffer, sizeof(buffer), " ack report=%02x error=%u", report.ack->report_id, report.ack->error);
        text += buffer;
    }
    if (report.has_accel) {
        std::snprintf(buffer, sizeof(buffer), " accel=%u,%u,%u", report.accel.x, report.accel.y, report.accel.z);
        text += buffer;
    }
    if (report.ir_format != WiimoteIrFormat::None) {
        text += " ir=";
        bool any = false;
        for (const IrDot& dot : report.ir) {
            if (!dot.visible) continue;
            std::snprintf(buffer, sizeof(buffer), "(%u,%u)", dot.x, dot.y);
            text += buffer;
            any = true;
        }
        if (!any) text += "none";
    }
    if (report.extension_size != 0u) {
        std::snprintf(buffer, sizeof(buffer), " ext=%zu", report.extension_size);
        text += buffer;
    }
    return text;
}

NunchukState decode_nunchuk_extension(std::span<const std::uint8_t, 6> bytes) noexcept {
    NunchukState nunchuk;
    nunchuk.stick_x = bytes[0];
    nunchuk.stick_y = bytes[1];
    const std::uint8_t low = bytes[5];
    nunchuk.accel.x = axis10_from(bytes[2], static_cast<std::uint8_t>(low >> 2));
    nunchuk.accel.y = axis10_from(bytes[3], static_cast<std::uint8_t>(low >> 4));
    nunchuk.accel.z = axis10_from(bytes[4], static_cast<std::uint8_t>(low >> 6));
    // The buttons are active low.
    nunchuk.c = (low & 0x02u) == 0u;
    nunchuk.z = (low & 0x01u) == 0u;
    return nunchuk;
}

bool decrypt_rmge01_nunchuk_extension_payload(
    std::span<std::uint8_t> report,
    std::span<const std::uint8_t, 8> additive_key,
    std::span<const std::uint8_t, 8> xor_key) noexcept {
    const std::optional<DecodedWiimoteReport> decoded = decode_wiimote_input_report(report);
    if (!decoded.has_value() || decoded->extension_size == 0u) return false;
    for (std::size_t i = 0; i < decoded->extension_size; ++i) {
        std::uint8_t& byte = report[decoded->extension_offset + i];
        byte = static_cast<std::uint8_t>((byte ^ xor_key[i % xor_key.size()]) + additive_key[i % additive_key.size()]);
    }
    return true;
}

WiimoteExtensionKind classify_wiimote_extension(std::span<const std::uint8_t, 6> identifier) noexcept {
    const auto is = [&](std::array<std::uint8_t, 6> expected) {
        return std::equal(expected.begin(), expected.end(), identifier.begin());
    };
    if (is({0xff, 0xff, 0xff, 0xff, 0xff, 0xff}) || is({0x00, 0x00, 0x00, 0x00, 0x00, 0x00})) {
        return WiimoteExtensionKind::None;
    }
    // Byte 0 is a variant byte on some third-party Nunchuks; bytes 2..5 identify the device.
    if (identifier[2] == 0xa4 && identifier[3] == 0x20 && identifier[4] == 0x00 && identifier[5] == 0x00) {
        return WiimoteExtensionKind::Nunchuk;
    }
    if (is({0x00, 0x00, 0xa4, 0x20, 0x01, 0x01})) return WiimoteExtensionKind::ClassicController;
    if (is({0x01, 0x00, 0xa4, 0x20, 0x01, 0x01})) return WiimoteExtensionKind::ClassicControllerPro;
    return WiimoteExtensionKind::Unknown;
}

std::optional<WiimoteAccelCalibration> decode_wiimote_accel_calibration(
    std::span<const std::uint8_t, 10> bytes) noexcept {
    std::uint8_t checksum = 0x55u;
    for (std::size_t i = 0; i < 9u; ++i) checksum = static_cast<std::uint8_t>(checksum + bytes[i]);
    if (checksum != bytes[9]) return std::nullopt;
    // Bytes 0-2 and 4-6 are bits 9:2 of the zero-g and one-g points; bytes 3 and 7 carry bits 1:0
    // (X in bits 5:4, Y in 3:2, Z in 1:0).
    WiimoteAccelCalibration calibration;
    calibration.zero_g.x = axis10_from(bytes[0], static_cast<std::uint8_t>(bytes[3] >> 4));
    calibration.zero_g.y = axis10_from(bytes[1], static_cast<std::uint8_t>(bytes[3] >> 2));
    calibration.zero_g.z = axis10_from(bytes[2], bytes[3]);
    calibration.one_g.x = axis10_from(bytes[4], static_cast<std::uint8_t>(bytes[7] >> 4));
    calibration.one_g.y = axis10_from(bytes[5], static_cast<std::uint8_t>(bytes[7] >> 2));
    calibration.one_g.z = axis10_from(bytes[6], bytes[7]);
    if (calibration.one_g.x <= calibration.zero_g.x || calibration.one_g.y <= calibration.zero_g.y ||
        calibration.one_g.z <= calibration.zero_g.z) {
        return std::nullopt;
    }
    return calibration;
}

AccelG wiimote_accel_in_g(const Axis10& raw, const WiimoteAccelCalibration& calibration) noexcept {
    const auto axis = [](std::uint16_t value, std::uint16_t zero, std::uint16_t one) {
        const float range = static_cast<float>(one) - static_cast<float>(zero);
        return range > 0.0f ? (static_cast<float>(value) - static_cast<float>(zero)) / range : 0.0f;
    };
    return AccelG{axis(raw.x, calibration.zero_g.x, calibration.one_g.x),
        axis(raw.y, calibration.zero_g.y, calibration.one_g.y),
        axis(raw.z, calibration.zero_g.z, calibration.one_g.z)};
}

WiimoteOutputReport wiimote_output_leds(std::uint8_t leds, bool rumble) noexcept {
    return output(0x11, {static_cast<std::uint8_t>(((leds & 0x0fu) << 4) | rumble_bit(rumble))});
}

WiimoteOutputReport wiimote_output_rumble(bool rumble) noexcept { return output(0x10, {rumble_bit(rumble)}); }

WiimoteOutputReport wiimote_output_report_mode(std::uint8_t mode, bool continuous, bool rumble) noexcept {
    return output(0x12, {static_cast<std::uint8_t>((continuous ? 0x04u : 0x00u) | rumble_bit(rumble)), mode});
}

WiimoteOutputReport wiimote_output_enable(std::uint8_t report_id, bool enable, bool rumble) noexcept {
    return output(report_id, {static_cast<std::uint8_t>((enable ? 0x04u : 0x00u) | rumble_bit(rumble))});
}

WiimoteOutputReport wiimote_output_status_request(bool rumble) noexcept { return output(0x15, {rumble_bit(rumble)}); }

WiimoteOutputReport wiimote_output_write_memory(
    WiimoteMemorySpace space,
    std::uint32_t address,
    std::span<const std::uint8_t> data,
    bool rumble) noexcept {
    if (data.empty() || data.size() > 16u) return {};
    WiimoteOutputReport report = output(0x16,
        {static_cast<std::uint8_t>(static_cast<std::uint8_t>(space) | rumble_bit(rumble)),
            static_cast<std::uint8_t>(address >> 16), static_cast<std::uint8_t>(address >> 8),
            static_cast<std::uint8_t>(address), static_cast<std::uint8_t>(data.size())});
    std::copy(data.begin(), data.end(), report.bytes.begin() + 7);
    report.size = 23;  // the data field is always 16 bytes
    return report;
}

WiimoteOutputReport wiimote_output_read_memory(
    WiimoteMemorySpace space,
    std::uint32_t address,
    std::uint16_t size,
    bool rumble) noexcept {
    return output(0x17,
        {static_cast<std::uint8_t>(static_cast<std::uint8_t>(space) | rumble_bit(rumble)),
            static_cast<std::uint8_t>(address >> 16), static_cast<std::uint8_t>(address >> 8),
            static_cast<std::uint8_t>(address), static_cast<std::uint8_t>(size >> 8),
            static_cast<std::uint8_t>(size)});
}

}  // namespace galaxy::input
