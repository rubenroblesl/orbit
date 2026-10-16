// SPDX-License-Identifier: GPL-3.0-only
//
// nebula_wiimote_probe: checks a Wii Remote paired with this Mac without the game, through the same
// IOKit bridge the runtime uses (galaxy_macos_wiimote.h). It lights the LEDs, rumbles, reads the
// battery and the accelerometer calibration, starts the Nunchuk and the IR camera like a Wii does,
// then shows the buttons, the pointer dots, the motion and the Nunchuk live.
//
//   nebula_wiimote_probe [--seconds N] [--record FILE]
//
// HOME (or Ctrl-C) ends it; hold B to rumble. A remote that disconnects is waited for and set up
// again when it comes back. --record writes every input report as hex lines
// ("<ms> a1 37 ..."), the material for the decoder's tests. GALAXY_TRACE_WIIMOTE=1 adds the
// bridge's own trace.

#include "galaxy_macos_wiimote.h"

#include "galaxy/wiimote_report.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace in = galaxy::input;
namespace wiimote = galaxy::macos::wiimote;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

std::atomic<bool> g_interrupted{false};

void on_interrupt(int) { g_interrupted.store(true); }

struct Options {
    double seconds = 60.0;
    const char* record = nullptr;
};

class Probe {
public:
    Probe(std::unique_ptr<in::NativeHidIoBackend> bridge, FILE* record)
        : bridge_(std::move(bridge)), record_(record), start_(Clock::now()) {}

    void send(const in::WiimoteOutputReport& report) {
        std::vector<std::byte> bytes(report.size);
        std::transform(report.bytes.begin(), report.bytes.begin() + static_cast<std::ptrdiff_t>(report.size),
            bytes.begin(), [](std::uint8_t value) { return static_cast<std::byte>(value); });
        const Clock::time_point begin = Clock::now();
        (void)bridge_->write_output(bytes);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
        write_us_.push_back(static_cast<double>(us));
    }

    // Reads reports until `done` returns true for one of them or `timeout` passes.
    std::optional<in::DecodedWiimoteReport> wait_for(
        const std::function<bool(const in::DecodedWiimoteReport&)>& done, milliseconds timeout) {
        const Clock::time_point deadline = Clock::now() + timeout;
        while (Clock::now() < deadline && !g_interrupted.load()) {
            if (std::optional<in::DecodedWiimoteReport> report = next()) {
                if (done(*report)) return report;
                continue;
            }
            std::this_thread::sleep_for(milliseconds(1));
        }
        return std::nullopt;
    }

    std::optional<in::DecodedWiimoteReport> next() {
        std::vector<std::byte> raw;
        if (!bridge_->poll_input(raw)) return std::nullopt;
        std::vector<std::uint8_t> bytes(raw.size());
        std::transform(raw.begin(), raw.end(), bytes.begin(), [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
        if (record_ != nullptr) {
            std::fprintf(record_, "%lld", static_cast<long long>(elapsed_ms()));
            for (const std::uint8_t byte : bytes) std::fprintf(record_, " %02x", byte);
            std::fputc('\n', record_);
        }
        std::optional<in::DecodedWiimoteReport> report = in::decode_wiimote_input_report(bytes);
        if (report.has_value()) {
            ++reports_;
            if (report->extension_size != 0u) {
                last_extension_.assign(bytes.begin() + static_cast<std::ptrdiff_t>(report->extension_offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(report->extension_offset + report->extension_size));
            }
        }
        return report;
    }

    bool write_register(std::uint32_t address, std::span<const std::uint8_t> data) {
        send(in::wiimote_output_write_memory(in::WiimoteMemorySpace::Registers, address, data, rumble_));
        const auto ack = wait_for([](const auto& r) { return r.ack.has_value() && r.ack->report_id == 0x16; },
            milliseconds(500));
        return ack.has_value() && ack->ack->error == 0u;
    }

    std::optional<in::WiimoteMemoryReadReport> read(in::WiimoteMemorySpace space, std::uint32_t address,
        std::uint16_t size) {
        send(in::wiimote_output_read_memory(space, address, size, rumble_));
        const auto reply = wait_for([](const auto& r) { return r.memory.has_value(); }, milliseconds(500));
        if (!reply.has_value()) return std::nullopt;
        return reply->memory;
    }

    std::optional<in::WiimoteStatusReport> status() {
        send(in::wiimote_output_status_request(rumble_));
        const auto reply = wait_for([](const auto& r) { return r.status.has_value(); }, milliseconds(1000));
        if (!reply.has_value()) return std::nullopt;
        return reply->status;
    }

    void set_rumble(bool on) {
        if (on == rumble_) return;
        rumble_ = on;
        send(in::wiimote_output_rumble(on));
    }

    long long elapsed_ms() const {
        return std::chrono::duration_cast<milliseconds>(Clock::now() - start_).count();
    }

    std::uint64_t reports() const { return reports_; }
    const std::vector<std::uint8_t>& last_extension() const { return last_extension_; }
    const std::vector<double>& write_us() const { return write_us_; }
    bool rumble() const { return rumble_; }
    void open() { bridge_->open(); }
    in::NativeHidDeviceLink link() const { return bridge_->device_link(); }
    // A remote that connects again starts with its motor off.
    void forget_rumble() { rumble_ = false; }
    void close() { bridge_->close(); }

private:
    std::unique_ptr<in::NativeHidIoBackend> bridge_;
    FILE* record_;
    Clock::time_point start_;
    bool rumble_ = false;
    std::uint64_t reports_ = 0;
    std::vector<std::uint8_t> last_extension_;
    std::vector<double> write_us_;
};

const char* extension_name(in::WiimoteExtensionKind kind) {
    switch (kind) {
    case in::WiimoteExtensionKind::None: return "none";
    case in::WiimoteExtensionKind::Nunchuk: return "Nunchuk";
    case in::WiimoteExtensionKind::ClassicController: return "Classic Controller (not used by the game)";
    case in::WiimoteExtensionKind::ClassicControllerPro: return "Classic Controller Pro (not used by the game)";
    case in::WiimoteExtensionKind::Unknown: return "unknown extension";
    }
    return "?";
}

// The unencrypted handshake, then the identifier at 0xA400FA.
in::WiimoteExtensionKind start_extension(Probe& probe) {
    const std::uint8_t init1[] = {0x55};
    const std::uint8_t init2[] = {0x00};
    if (!probe.write_register(in::kWiimoteExtensionInit1, init1) ||
        !probe.write_register(in::kWiimoteExtensionInit2, init2)) {
        std::printf("  extension: the handshake was not acknowledged\n");
    }
    const auto id = probe.read(in::WiimoteMemorySpace::Registers, in::kWiimoteExtensionIdentifier, 6);
    if (!id.has_value() || id->error != 0u) {
        std::printf("  extension: its identifier could not be read\n");
        return in::WiimoteExtensionKind::Unknown;
    }
    return in::classify_wiimote_extension(std::span<const std::uint8_t, 6>(id->data.data(), 6));
}

// The IR camera at the Wii's sensitivity 3, basic mode (report 0x37).
bool start_ir_camera(Probe& probe) {
    probe.send(in::wiimote_output_enable(0x13, true, probe.rumble()));
    probe.send(in::wiimote_output_enable(0x1a, true, probe.rumble()));
    const std::uint8_t enable[] = {0x08};
    const std::uint8_t basic_mode[] = {0x01};
    bool ok = probe.write_register(in::kWiimoteIrControl, enable);
    ok &= probe.write_register(in::kWiimoteIrSensitivity1, in::kWiimoteIrSensitivityLevel3Block1);
    ok &= probe.write_register(in::kWiimoteIrSensitivity2, in::kWiimoteIrSensitivityLevel3Block2);
    ok &= probe.write_register(in::kWiimoteIrMode, basic_mode);
    ok &= probe.write_register(in::kWiimoteIrControl, enable);
    return ok;
}

struct Live {
    in::WiimoteButtons buttons;
    in::AccelG accel{0.0f, 0.0f, 1.0f};
    std::array<in::IrDot, 4> ir{};
    std::optional<in::NunchukState> nunchuk;
};

std::string signed_g(float value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%+.2f", value);
    return text;
}

std::string live_line(const Live& live) {
    std::string line = "buttons: " + in::wiimote_buttons_text(live.buttons);
    line += " | accel g: " + signed_g(live.accel.x) + " " + signed_g(live.accel.y) + " " + signed_g(live.accel.z);
    int dots = 0;
    std::string first;
    for (const in::IrDot& dot : live.ir) {
        if (!dot.visible) continue;
        if (dots++ == 0) first = " (" + std::to_string(dot.x) + "," + std::to_string(dot.y) + ")";
    }
    line += " | IR: " + std::to_string(dots) + (dots == 1 ? " dot" : " dots") + first;
    if (live.nunchuk.has_value()) {
        const in::NunchukState& n = *live.nunchuk;
        line += " | Nunchuk: stick " + std::to_string(static_cast<int>(n.stick_x) - 128) + "," +
                std::to_string(static_cast<int>(n.stick_y) - 128);
        if (n.c) line += " C";
        if (n.z) line += " Z";
    }
    return line;
}

double magnitude(const in::AccelG& g) { return std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z); }

struct Setup {
    in::WiimoteAccelCalibration calibration;
    in::WiimoteExtensionKind extension = in::WiimoteExtensionKind::None;
};

// What a Wii does with a remote that connects: LEDs, status, calibration, extension, IR camera,
// report mode.
Setup set_up(Probe& probe) {
    Setup setup;
    probe.send(in::wiimote_output_leds(0x1, false));
    probe.set_rumble(true);
    std::this_thread::sleep_for(milliseconds(250));
    probe.set_rumble(false);
    std::printf("  LED 1 is on and the remote rumbled for a moment.\n");

    const std::optional<in::WiimoteStatusReport> status = probe.status();
    if (status.has_value()) {
        std::printf("  battery: about %d%%%s; extension port: %s\n",
            std::min(100, status->battery_level * 100 / 0xc8), status->battery_low ? " (low)" : "",
            status->extension_connected ? "in use" : "empty");
    } else {
        std::printf("  status: no reply\n");
    }

    const auto block = probe.read(in::WiimoteMemorySpace::Eeprom, in::kWiimoteAccelCalibration, 10);
    std::optional<in::WiimoteAccelCalibration> decoded;
    if (block.has_value() && block->error == 0u && block->size == 10u) {
        decoded = in::decode_wiimote_accel_calibration(std::span<const std::uint8_t, 10>(block->data.data(), 10));
    }
    if (decoded.has_value()) {
        setup.calibration = *decoded;
        const in::WiimoteAccelCalibration& c = setup.calibration;
        std::printf("  accelerometer calibration: zero %u,%u,%u, one g %u,%u,%u\n", c.zero_g.x, c.zero_g.y,
            c.zero_g.z, c.one_g.x, c.one_g.y, c.one_g.z);
    } else {
        std::printf("  accelerometer calibration: not readable, using typical values\n");
    }

    if (status.has_value() && status->extension_connected) setup.extension = start_extension(probe);
    std::printf("  extension: %s\n", extension_name(setup.extension));

    std::printf("  IR camera: %s\n", start_ir_camera(probe) ? "on" : "on, but some setup writes were not acknowledged");
    probe.send(in::wiimote_output_report_mode(0x37, true, probe.rumble()));
    return setup;
}

void run(Probe& probe, const Options& options) {
    std::printf("Connected.\n");
    Setup setup = set_up(probe);
    std::uint64_t attachments = probe.link().attachments;

    // Output speed: the game streams speaker sound at about 100 output reports a second.
    const std::size_t first_write = probe.write_us().size();
    for (unsigned i = 0; i < 20u; ++i) probe.send(in::wiimote_output_leds(static_cast<std::uint8_t>(1u << (i % 4u)), false));
    probe.send(in::wiimote_output_leds(0x1, false));
    const std::vector<double>& writes = probe.write_us();
    double total = 0.0;
    double slowest = 0.0;
    for (std::size_t i = first_write; i < writes.size(); ++i) {
        total += writes[i];
        slowest = std::max(slowest, writes[i]);
    }
    const double average_ms = total / static_cast<double>(writes.size() - first_write) / 1000.0;
    std::printf("  output reports: %zu written, average %.1f ms, slowest %.1f ms%s\n", writes.size() - first_write,
        average_ms, slowest / 1000.0,
        average_ms > 8.0 ? " (slow: the game's speaker sound may not keep up)" : "");

    const bool terminal = isatty(STDOUT_FILENO) != 0;
    std::printf("\nLive for %.0f s. Point at the sensor bar, shake, use the Nunchuk; hold B to rumble, HOME ends.\n",
        options.seconds);
    Live live;
    std::set<std::string> seen_buttons;
    int most_dots = 0;
    double strongest = 0.0;
    unsigned shakes = 0;
    bool shaking = false;
    bool nunchuk_c = false;
    bool nunchuk_z = false;
    int stick_range = 0;
    const std::uint64_t reports_before = probe.reports();
    const Clock::time_point begin = Clock::now();
    Clock::time_point next_line = begin;
    bool home = false;
    bool waiting = false;
    while (!g_interrupted.load() && !home &&
           Clock::now() - begin < std::chrono::duration<double>(options.seconds)) {
        // A remote that disconnects is waited for and set up again when it is back, as the game does.
        const in::NativeHidDeviceLink link = probe.link();
        if (!link.attached) {
            if (!waiting) {
                std::printf("%sThe Wii Remote disconnected. Press one of its buttons (or 1+2) to connect it again.\n",
                    terminal ? "\r\033[K" : "");
                probe.forget_rumble();
                live = Live{};
                waiting = true;
            }
            std::this_thread::sleep_for(milliseconds(10));
            continue;
        }
        if (link.attachments != attachments) {
            attachments = link.attachments;
            waiting = false;
            std::printf("%sConnected again.\n", terminal ? "\r\033[K" : "");
            setup = set_up(probe);
        }
        std::optional<in::DecodedWiimoteReport> report = probe.next();
        if (!report.has_value()) {
            std::this_thread::sleep_for(milliseconds(1));
        } else if (report->status.has_value()) {
            // The remote sends a status report by itself when an extension is plugged in or out,
            // and stops its data reports until the mode is set again.
            setup.extension =
                report->status->extension_connected ? start_extension(probe) : in::WiimoteExtensionKind::None;
            std::printf("%sextension changed: %s\n", terminal ? "\r\033[K" : "", extension_name(setup.extension));
            probe.send(in::wiimote_output_report_mode(0x37, true, probe.rumble()));
            live.nunchuk.reset();
        } else if (report->report_id >= 0x30u) {
            live.buttons = report->buttons;
            const std::string held = in::wiimote_buttons_text(report->buttons);
            for (std::size_t from = 0; held != "-";) {
                const std::size_t space = held.find(' ', from);
                seen_buttons.insert(held.substr(from, space - from));
                if (space == std::string::npos) break;
                from = space + 1;
            }
            home = report->buttons.home;
            probe.set_rumble(report->buttons.b);
            if (report->has_accel) {
                live.accel = in::wiimote_accel_in_g(report->accel, setup.calibration);
                const double g = magnitude(live.accel);
                strongest = std::max(strongest, g);
                if (!shaking && g > 2.2) ++shakes;
                shaking = g > 1.6 ? (shaking || g > 2.2) : false;
            }
            if (report->ir_format != in::WiimoteIrFormat::None) {
                live.ir = report->ir;
                const int dots = static_cast<int>(std::count_if(
                    report->ir.begin(), report->ir.end(), [](const in::IrDot& dot) { return dot.visible; }));
                most_dots = std::max(most_dots, dots);
            }
            if (setup.extension == in::WiimoteExtensionKind::Nunchuk && probe.last_extension().size() >= 6u) {
                live.nunchuk = in::decode_nunchuk_extension(std::span<const std::uint8_t, 6>(probe.last_extension().data(), 6));
                nunchuk_c |= live.nunchuk->c;
                nunchuk_z |= live.nunchuk->z;
                stick_range = std::max({stick_range, std::abs(static_cast<int>(live.nunchuk->stick_x) - 128),
                    std::abs(static_cast<int>(live.nunchuk->stick_y) - 128)});
            }
        }
        if (Clock::now() >= next_line) {
            if (terminal) {
                std::printf("\r\033[K%s", live_line(live).c_str());
                std::fflush(stdout);
                next_line = Clock::now() + milliseconds(100);
            } else {
                std::printf("%s\n", live_line(live).c_str());
                next_line = Clock::now() + milliseconds(1000);
            }
        }
    }
    probe.set_rumble(false);
    const double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    std::printf("%s\nSummary\n", terminal ? "\r\033[K" : "");
    std::printf("  input reports: %.0f a second\n",
        seconds > 0.0 ? static_cast<double>(probe.reports() - reports_before) / seconds : 0.0);
    std::string buttons;
    for (const std::string& name : seen_buttons) buttons += (buttons.empty() ? "" : " ") + name;
    std::printf("  buttons pressed: %s\n", buttons.empty() ? "none" : buttons.c_str());
    std::printf("  pointer: %s\n", most_dots >= 2 ? "the sensor bar was seen (2 dots or more)"
                                    : most_dots == 1 ? "only one dot seen: move back or check the sensor bar"
                                                     : "no dots seen: is the sensor bar (or another IR source) on?");
    std::printf("  shakes: %u (strongest %.1f g)\n", shakes, strongest);
    if (setup.extension == in::WiimoteExtensionKind::Nunchuk) {
        std::printf("  Nunchuk: stick moved up to %d of about 100%s%s\n", stick_range, nunchuk_c ? ", C pressed" : "",
            nunchuk_z ? ", Z pressed" : "");
    }
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            options.seconds = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            options.record = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--seconds N] [--record FILE]\n", argv[0]);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) return 64;
    std::signal(SIGINT, on_interrupt);

    FILE* record = nullptr;
    if (options.record != nullptr) {
        record = std::fopen(options.record, "w");
        if (record == nullptr) {
            std::perror(options.record);
            return 1;
        }
    }

    std::printf("Looking for a Wii Remote. Pair it first in System Settings > Bluetooth\n"
                "(press 1+2, or the red SYNC button under the battery cover).\n");
    std::fflush(stdout);
    wiimote::BridgeOptions bridge_options;
    bridge_options.discovery_timeout = std::chrono::seconds(5);
    const char* trace = std::getenv("GALAXY_TRACE_WIIMOTE");
    bridge_options.trace = trace != nullptr && *trace != '\0' && std::strcmp(trace, "0") != 0;
    Probe probe(wiimote::make_bridge(wiimote::make_iokit_transport(), bridge_options), record);

    int exit_code = 0;
    try {
        probe.open();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        exit_code = 2;
    }
    if (exit_code == 0) {
        try {
            run(probe, options);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "\nThe Wii Remote stopped answering: %s\n", error.what());
            exit_code = 3;
        }
        try {
            probe.set_rumble(false);
            probe.send(in::wiimote_output_leds(0x0, false));
        } catch (...) {
        }
    }
    probe.close();
    if (record != nullptr) std::fclose(record);
    return exit_code;
}
