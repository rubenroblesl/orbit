// SPDX-License-Identifier: GPL-3.0-only
//
// controller_hid.h on macOS: Switch Pro Controllers and Switch 2 Pro Controllers over IOKit HID.
//
// One worker thread runs a CFRunLoop with an IOHIDManager matching both controllers. A new
// controller is initialised as on Windows (controller_hid.cpp). Switch Pro (USB or Bluetooth):
// USB handshake, stick and IMU calibration from SPI flash, full input reports with the IMU,
// vibration and the player light. Pro Controller 2 (USB): its vendor interface (USB interface 1,
// opened with IOUSBHost in iokit_usb.mm where Windows needs WinUSB) gives the stick and gyro
// calibration from flash and takes the init sequence and the player light. Reports then arrive
// through an input-report callback and are decoded with the shared protocol code
// (switch_controller.h). Devices are opened without seizing them, so the Game Controller framework
// keeps seeing them; the XInput emulation hides a controller that this layer drives
// (controller_hid_owns_switch_pro / _pro2), so it is never read twice.
//
// A Pro Controller 2 over Bluetooth is left to the Game Controller framework. One whose vendor
// interface cannot be opened (another program holds it) stays open here but counts as driven only
// once its reports stream (that program started it).
//
// Vibration follows the Windows worker: the game's published motor state (rumble::motor_should_run)
// goes to the controller in use, Switch Pro, Pro Controller 2 or XInput; every other motor is turned
// off, and a running motor is refreshed (and stopped when the publications stall) on the worker's
// timer.

#include "galaxy/controller_hid.h"

#include <windows.h>
#include <xinput.h>

#include "galaxy/content_viewport.h"
#include "galaxy/rumble.h"

#include "iokit_hid.h"
#include "iokit_usb.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace galaxy::host {

// The XInput emulation (xinput_gamecontroller.mm) asks whether a Switch Pro / Pro Controller 2 is
// driven here.
bool controller_hid_owns_switch_pro() noexcept;
bool controller_hid_owns_switch_pro2() noexcept;

namespace {

namespace sw = input::switch_hid;
using input::ControlScheme;

std::uint64_t steady_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool env_flag(const char* name) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return false;
    return !(std::strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "off") == 0 ||
             strcasecmp(value, "no") == 0);
}

namespace iokit = galaxy::macos::hid;
namespace usb = galaxy::macos::usb;

// One output report (the report ID is the first byte, as on Windows).
bool write_report(iokit::Device device, const std::uint8_t* data, std::size_t size) noexcept {
    return iokit::set_output_report(device, data, size);
}

// The motor-off report for a Switch controller.
bool write_rumble_off(iokit::Device device, std::uint16_t output_length, bool pro2) noexcept {
    std::array<std::uint8_t, 128> report{};
    const std::size_t length = std::min<std::size_t>(output_length, report.size());
    const std::span<std::uint8_t> out(report.data(), length);
    const std::size_t built = pro2
        ? input::rumble::build_switch2_rumble_report(0, 0, 0, out)
        : input::rumble::build_switch_pro_rumble_report(0, input::rumble::switch_pro_motor_rumble(0, 0), out);
    return built != 0u && write_report(device, report.data(), length);
}

// The Pro Controller 2's vendor interface (USB interface 1) and its bulk pipes, through IOUSBHost
// (controller_hid.cpp WinUsbPipes).
class VendorPipes {
public:
    explicit VendorPipes(usb::Interface handle) : interface_(handle) {}
    ~VendorPipes() { usb::close_interface(interface_); }
    VendorPipes(const VendorPipes&) = delete;
    VendorPipes& operator=(const VendorPipes&) = delete;

    bool send(const sw::Switch2Command& cmd) {
        return usb::bulk_write(interface_, cmd.bytes.data(), cmd.size, kTimeoutMs);
    }
    // Reads up to `size` bytes in 64-byte packets until a short packet.
    std::size_t receive(std::uint8_t* data, std::size_t size) {
        std::size_t total = 0;
        while (total < size) {
            const std::size_t chunk = std::min<std::size_t>(64u, size - total);
            const std::size_t got = usb::bulk_read(interface_, data + total, chunk, kTimeoutMs);
            total += got;
            if (got < chunk) break;
        }
        return total;
    }

private:
    static constexpr unsigned kTimeoutMs = 500;  // the WinUSB pipe timeout on Windows
    usb::Interface interface_;
};

struct Device {
    iokit::Device ref = nullptr;  // retained
    void* input_registration = nullptr;
    ControlScheme scheme = ControlScheme::SwitchPro;
    bool bluetooth = false;
    bool opened = false;
    // Switch Pro: always. Pro Controller 2: started through its vendor interface, or streaming.
    bool driven = false;
    std::vector<std::uint8_t> input_buffer;  // IOKit fills it before each callback
    std::uint16_t output_length = 64;
    // Reports received while a synchronous exchange waits (initialisation).
    bool collecting = false;
    std::deque<std::vector<std::uint8_t>> collected;
    sw::StickCal left{};
    sw::StickCal right{};
    sw::ImuCal imu{};
    sw::Switch2ImuState switch2_imu{};
    sw::SwitchMotion motion{};
    std::uint64_t last_report_us = 0;
    std::unique_ptr<VendorPipes> vendor;  // Pro Controller 2
    std::size_t slot = 0;
    std::uint8_t packet = 0;
    bool rumble_on = false;
    std::uint8_t rumble_level = 0;
    std::uint8_t rumble_strength = 0;
    std::uint8_t rumble_sequence = 0;  // Pro Controller 2 rumble reports
    std::uint64_t rumble_sent_us = 0;
};

struct Published {
    bool connected = false;
    SwitchPadState state{};
};

struct Hub {
    std::mutex lifecycle;
    std::thread worker;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};
    std::atomic<iokit::RunLoop> run_loop{nullptr};
    std::mutex lock;
    std::array<Published, kMaxSwitchPads> pads{};
    std::atomic<unsigned> pro_count{0};
    std::atomic<unsigned> pro2_count{0};  // driven ones
    // Vibration: the game state from the emulation thread.
    std::mutex rumble_lock;
    RumbleRequest rumble_request{};
    std::uint64_t rumble_published_us = 0;
    std::atomic<bool> rumble_changed{false};
    std::atomic<bool> rumble_shutdown{false};
    std::atomic<std::uint8_t> xinput_rumbling{0};
    // A controller whose motor may run, for stop_all_rumble on any thread.
    std::shared_mutex rumble_device_lock;
    std::array<iokit::Device, kMaxSwitchPads> rumble_devices{};
    std::array<std::uint16_t, kMaxSwitchPads> rumble_lengths{};
    std::array<bool, kMaxSwitchPads> rumble_pro2{};
};

Hub& hub() {
    static Hub* instance = new Hub;  // never destroyed: the worker may outlive static destruction
    return *instance;
}

void wake_worker() noexcept {
    iokit::wake_run_loop(hub().run_loop.load(std::memory_order_acquire));
}

// Switch output pacing (controller_hid.cpp): Switch Pro writes at least 30 ms apart with a held
// rumble repeated every 40 ms; the Pro Controller 2 takes a fresh motor packet every 12 ms.
constexpr std::uint64_t kSwitchRumbleGapUs = 30'000;
constexpr std::uint64_t kSwitchRumbleRefreshUs = 40'000;
constexpr std::uint64_t kSwitch2RumbleRefreshUs = 12'000;

class Worker {
public:
    void run() {
        Hub& h = hub();
        h.run_loop.store(iokit::current_run_loop(), std::memory_order_release);
        const std::uint16_t products[] = {sw::kProControllerProductId, sw::kPro2ControllerProductId};
        int opened = 0;
        manager_ = iokit::create_manager(sw::kNintendoVendorId, products,
            iokit::ManagerCallbacks{this, &Worker::on_matched, &Worker::on_removed}, opened);
        if (manager_ == nullptr || opened != 0) {
            std::cerr << "[controller] HID manager unavailable (0x" << std::hex << opened << std::dec
                      << "); Switch controllers use the Game Controller framework\n";
        }
        while (!h.stopping.load(std::memory_order_acquire)) {
            const double timeout = rumble_timer_ms_ != 0u ? rumble_timer_ms_ / 1000.0 : 0.5;
            iokit::run_loop_once(timeout);
            open_matched();
            service_rumble();
        }
        stop_xinput_rumble();
        for (auto& device : devices_) close_device(*device);
        devices_.clear();
        iokit::destroy_manager(manager_);
        manager_ = nullptr;
        for (iokit::Device ref : matched_) iokit::release(ref);
        matched_.clear();
        h.run_loop.store(nullptr, std::memory_order_release);
    }

private:
    // --- Device lifetime ---

    static void on_matched(void* context, iokit::Device ref) {
        auto* self = static_cast<Worker*>(context);
        self->matched_.push_back(ref);  // retained; opened outside the callback (init waits on the run loop)
    }

    static void on_removed(void* context, iokit::Device ref) {
        auto* self = static_cast<Worker*>(context);
        if (ref == self->opening_) self->opening_removed_ = true;  // open_device drops it after its init
        for (auto it = self->devices_.begin(); it != self->devices_.end(); ++it) {
            if ((*it)->ref == ref) {
                (*it)->opened = false;  // the device is gone: no off report
                self->close_device(**it);
                self->devices_.erase(it);
                break;
            }
        }
        self->publish_counts();
    }

    static void on_report(void* context, const std::uint8_t* report, std::size_t length) {
        auto* device = static_cast<Device*>(context);
        if (device->collecting) {
            if (device->collected.size() < 64u) device->collected.emplace_back(report, report + length);
            return;
        }
        active_worker_->handle_report(*device, std::span<const std::uint8_t>(report, length));
    }

    void open_matched() {
        while (!matched_.empty()) {
            iokit::Device ref = matched_.front();
            matched_.erase(matched_.begin());
            if (devices_.size() < kMaxSwitchPads) open_device(ref);
            iokit::release(ref);
        }
    }

    std::size_t free_slot() const {
        for (std::size_t slot = 0; slot < kMaxSwitchPads; ++slot) {
            const bool used = std::any_of(devices_.begin(), devices_.end(), [slot](const auto& d) { return d->slot == slot; });
            if (!used) return slot;
        }
        return kMaxSwitchPads;
    }

    void open_device(iokit::Device ref) {
        const ControlScheme scheme = iokit::product_id(ref) == sw::kPro2ControllerProductId ? ControlScheme::SwitchPro2
                                                                                           : ControlScheme::SwitchPro;
        const bool bluetooth = iokit::is_bluetooth(ref);
        if (scheme == ControlScheme::SwitchPro2 && bluetooth) {
            std::cerr << "[controller] pro controller 2 over bluetooth: its init needs USB; left to the Game "
                         "Controller framework\n";
            return;
        }
        auto device = std::make_unique<Device>();
        device->scheme = scheme;
        device->slot = free_slot();
        if (device->slot >= kMaxSwitchPads) return;
        if (!iokit::open_device(ref)) {
            std::cerr << "[controller] " << input::control_scheme_id(scheme) << ": cannot open the HID device\n";
            return;
        }
        iokit::retain(ref);
        device->ref = ref;
        device->opened = true;
        device->bluetooth = bluetooth;
        device->input_buffer.resize(static_cast<std::size_t>(std::max(iokit::max_input_report_size(ref), 64L)));
        // Output reports, report ID included: Switch Pro 64 bytes over USB, 49 over Bluetooth (as SDL
        // sends); Pro Controller 2 its 64-byte rumble report.
        device->output_length = scheme == ControlScheme::SwitchPro2
            ? static_cast<std::uint16_t>(input::rumble::kSwitch2RumbleReportBytes)
            : (bluetooth ? 49u : 64u);
        device->input_registration = iokit::register_input(ref, device->input_buffer.data(),
            device->input_buffer.size(), &Worker::on_report, device.get());
        opening_ = ref;
        opening_removed_ = false;
        if (scheme == ControlScheme::SwitchPro2) {
            device->driven = init_switch2(*device);
        } else {
            init_switch_pro(*device);
            device->driven = true;
        }
        opening_ = nullptr;
        if (opening_removed_) {
            device->opened = false;  // unplugged during the init
            close_device(*device);
            return;
        }
        {
            std::lock_guard lock(hub().lock);
            Published& published = hub().pads[device->slot];
            published = {};
            published.connected = true;
            published.state.connected = true;
            published.state.scheme = scheme;
            published.state.left = device->left;
            published.state.right = device->right;
            published.state.slot = static_cast<std::uint8_t>(device->slot);
        }
        std::cerr << "[controller] connected " << input::control_scheme_id(scheme)
                  << (device->bluetooth ? " bluetooth" : " usb") << '\n';
        devices_.push_back(std::move(device));
        publish_counts();
    }

    void close_device(Device& device) {
        if (device.slot < kMaxSwitchPads) {
            std::unique_lock lock(hub().rumble_device_lock);
            hub().rumble_devices[device.slot] = nullptr;
        }
        if (device.opened && device.rumble_on) {
            (void)write_rumble_off(device.ref, device.output_length, device.scheme == ControlScheme::SwitchPro2);
        }
        device.rumble_on = false;
        if (device.ref != nullptr) {
            iokit::unregister_input(device.ref, device.input_buffer.data(), device.input_buffer.size(),
                device.input_registration);
            device.input_registration = nullptr;
            if (device.opened) iokit::close_device(device.ref);
            iokit::release(device.ref);
            device.ref = nullptr;
        }
        device.vendor.reset();
        if (device.slot < kMaxSwitchPads) {
            std::lock_guard lock(hub().lock);
            hub().pads[device.slot] = {};
        }
        std::cerr << "[controller] disconnected " << input::control_scheme_id(device.scheme) << '\n';
    }

    // A Pro Controller 2 counts once driven, so one that another program holds and that sends nothing
    // stays an XInput pad (xinput_gamecontroller.mm hid_owned).
    void publish_counts() {
        unsigned pro = 0;
        unsigned pro2 = 0;
        for (const auto& device : devices_) {
            if (!device->driven) continue;
            (device->scheme == ControlScheme::SwitchPro2 ? pro2 : pro)++;
        }
        hub().pro_count.store(pro, std::memory_order_release);
        hub().pro2_count.store(pro2, std::memory_order_release);
    }

    // --- Initialisation (controller_hid.cpp init_switch_pro) ---

    // Waits up to `timeout_ms` for a collected report that `accept` takes.
    template <typename Accept>
    bool wait_report(Device& device, unsigned timeout_ms, Accept accept) {
        const std::uint64_t deadline = steady_us() + std::uint64_t{timeout_ms} * 1000u;
        for (;;) {
            while (!device.collected.empty()) {
                std::vector<std::uint8_t> report = std::move(device.collected.front());
                device.collected.pop_front();
                if (accept(std::span<const std::uint8_t>(report))) return true;
            }
            const std::uint64_t now = steady_us();
            if (now >= deadline) return false;
            iokit::run_loop_once(std::min(0.01, static_cast<double>(deadline - now) / 1e6));
        }
    }

    bool subcommand(Device& device, std::uint8_t id, std::span<const std::uint8_t> data, std::uint32_t spi_address,
        std::vector<std::uint8_t>* reply_data) {
        std::vector<std::uint8_t> out(std::max<std::size_t>(device.output_length, 64u));
        for (int attempt = 0; attempt < 3; ++attempt) {
            const std::size_t size = id == sw::kSubcommandSpiRead
                ? sw::build_spi_read(device.packet++, spi_address, data.empty() ? 0 : data[0], out)
                : sw::build_subcommand(device.packet++, id, data, out);
            device.collected.clear();
            if (size == 0u || !write_report(device.ref, out.data(), device.output_length)) continue;
            const bool matched = wait_report(device, 150u, [&](std::span<const std::uint8_t> report) {
                const auto reply = sw::match_subcommand_reply(report, id, spi_address);
                if (!reply.matched) return false;
                if (reply_data != nullptr) reply_data->assign(reply.data.begin(), reply.data.end());
                return true;
            });
            if (matched) return true;
        }
        return false;
    }

    bool usb_command(Device& device, std::uint8_t command, bool wait) {
        std::vector<std::uint8_t> out(std::max<std::size_t>(device.output_length, 64u));
        (void)sw::build_usb_command(command, out);
        device.collected.clear();
        if (!write_report(device.ref, out.data(), device.output_length)) return false;
        if (!wait) return true;
        return wait_report(device, 150u, [command](std::span<const std::uint8_t> report) {
            return report.size() >= 2u && report[0] == sw::kReportUsbAck && report[1] == command;
        });
    }

    void init_switch_pro(Device& device) {
        device.collecting = true;
        if (!device.bluetooth) {
            if (usb_command(device, sw::kUsbHandshake, true)) {
                (void)usb_command(device, sw::kUsbHighSpeed, true);
                (void)usb_command(device, sw::kUsbHandshake, true);
                (void)usb_command(device, sw::kUsbForceHid, false);
            } else {
                device.bluetooth = true;  // no USB acknowledgement
            }
        }
        std::vector<std::uint8_t> data;
        bool user_left = false;
        bool user_right = false;
        const std::uint8_t user_length = 22;
        if (subcommand(device, sw::kSubcommandSpiRead, std::span(&user_length, 1), sw::kSpiUserStickCal, &data) &&
            data.size() >= 22u) {
            if (data[0] == 0xb2 && data[1] == 0xa1) {
                device.left = sw::parse_left_stick_cal(std::span<const std::uint8_t, 9>(data.data() + 2, 9));
                user_left = true;
            }
            if (data[11] == 0xb2 && data[12] == 0xa1) {
                device.right = sw::parse_right_stick_cal(std::span<const std::uint8_t, 9>(data.data() + 13, 9));
                user_right = true;
            }
        }
        const std::uint8_t factory_length = 18;
        if ((!user_left || !user_right) &&
            subcommand(device, sw::kSubcommandSpiRead, std::span(&factory_length, 1), sw::kSpiFactoryStickCal, &data) &&
            data.size() >= 18u) {
            if (!user_left) device.left = sw::parse_left_stick_cal(std::span<const std::uint8_t, 9>(data.data(), 9));
            if (!user_right) device.right = sw::parse_right_stick_cal(std::span<const std::uint8_t, 9>(data.data() + 9, 9));
        }
        const std::uint8_t imu_length = 24;
        if (subcommand(device, sw::kSubcommandSpiRead, std::span(&imu_length, 1), sw::kSpiFactoryImuCal, &data) &&
            data.size() >= 24u) {
            device.imu = sw::parse_imu_cal(std::span<const std::uint8_t, 24>(data.data(), 24));
        }
        const std::uint8_t user_imu_length = 26;
        if (subcommand(device, sw::kSubcommandSpiRead, std::span(&user_imu_length, 1), sw::kSpiUserImuCal, &data) &&
            data.size() >= 26u && data[0] == 0xb2 && data[1] == 0xa1) {
            device.imu = sw::parse_imu_cal(std::span<const std::uint8_t, 24>(data.data() + 2, 24));
        }
        const std::uint8_t full_mode = sw::kReportFullState;
        const std::uint8_t on = 1;
        const std::uint8_t player_one = 0x01;
        const std::uint8_t vibration = rumble_output_disabled() ? 0 : 1;
        const bool mode_ok = subcommand(device, sw::kSubcommandSetReportMode, std::span(&full_mode, 1), 0, nullptr);
        const bool imu_ok = subcommand(device, sw::kSubcommandEnableImu, std::span(&on, 1), 0, nullptr);
        (void)subcommand(device, sw::kSubcommandEnableVibration, std::span(&vibration, 1), 0, nullptr);
        (void)subcommand(device, sw::kSubcommandPlayerLights, std::span(&player_one, 1), 0, nullptr);
        device.collecting = false;
        device.collected.clear();
        std::cerr << "[controller] switch pro (" << (device.bluetooth ? "bluetooth" : "usb") << ") report 0x30 "
                  << (mode_ok ? "on" : "unconfirmed") << ", imu " << (imu_ok ? "on" : "unconfirmed") << '\n';
    }

    // --- Pro Controller 2 initialisation (controller_hid.cpp init_switch2) ---

    static bool flash_read_switch2(VendorPipes& pipes, std::uint32_t address, std::array<std::uint8_t, 0x40>& out) {
        if (!pipes.send(sw::switch2_flash_read(address))) return false;
        std::array<std::uint8_t, 0x50> reply{};
        if (pipes.receive(reply.data(), reply.size()) < 0x50) return false;
        std::memcpy(out.data(), reply.data() + 0x10, out.size());
        return true;
    }

    // Opens the vendor interface of the same USB device (its location ID), reads the calibration and
    // sends the init sequence; true once the controller was told to stream input.
    bool init_switch2(Device& device) {
        device.collecting = true;  // reports before the init are not used
        const auto location = static_cast<std::uint32_t>(iokit::location_id(device.ref));
        int error = 0;
        usb::Interface handle = nullptr;
        // The vendor interface can register a moment after the HID interface matched.
        const std::uint64_t deadline = steady_us() + 1'000'000u;
        for (;;) {
            handle = usb::open_bulk_interface(sw::kNintendoVendorId, sw::kPro2ControllerProductId, 1, location, error);
            if (handle != nullptr || error != 0 || opening_removed_ || steady_us() >= deadline) break;
            iokit::run_loop_once(0.05);
        }
        const auto finish = [&device](bool started) {
            device.collecting = false;
            device.collected.clear();
            return started;
        };
        if (handle == nullptr) {
            std::cerr << "[controller] pro controller 2 (usb): vendor interface unavailable (0x" << std::hex << error
                      << std::dec << "); input arrives only if another program started the controller\n";
            return finish(false);
        }
        device.vendor = std::make_unique<VendorPipes>(handle);
        VendorPipes& pipes = *device.vendor;
        std::array<std::uint8_t, 0x40> block{};
        if (flash_read_switch2(pipes, 0x13040, block)) {
            float bias[3]{};
            std::memcpy(bias, block.data() + 4, sizeof(bias));
            for (int i = 0; i < 3; ++i) {
                device.switch2_imu.gyro_bias[static_cast<std::size_t>(i)] = std::isfinite(bias[i]) ? bias[i] : 0.0f;
            }
        }
        if (flash_read_switch2(pipes, 0x13080, block)) {
            device.left = sw::parse_switch2_stick_cal(std::span<const std::uint8_t, 9>(block.data() + 0x28, 9));
        }
        if (flash_read_switch2(pipes, 0x130c0, block)) {
            device.right = sw::parse_switch2_stick_cal(std::span<const std::uint8_t, 9>(block.data() + 0x28, 9));
        }
        if (flash_read_switch2(pipes, 0x1fc040, block) && block[0] == 0xb2 && block[1] == 0xa1) {
            device.left = sw::parse_switch2_stick_cal(std::span<const std::uint8_t, 9>(block.data() + 2, 9));
        }
        if (flash_read_switch2(pipes, 0x1fc080, block) && block[0] == 0xb2 && block[1] == 0xa1) {
            device.right = sw::parse_switch2_stick_cal(std::span<const std::uint8_t, 9>(block.data() + 2, 9));
        }
        std::array<std::uint8_t, 64> reply{};
        for (const sw::Switch2Command& cmd : sw::switch2_init_sequence()) {
            if (!pipes.send(cmd)) {
                std::cerr << "[controller] pro controller 2: init command failed\n";
                return finish(false);
            }
            (void)pipes.receive(reply.data(), reply.size());
        }
        // The LED reply is read into a whole packet (Windows asks for 8 bytes): a longer reply cannot
        // overrun the transfer.
        if (pipes.send(sw::switch2_player_led(0x01))) (void)pipes.receive(reply.data(), reply.size());
        std::cerr << "[controller] pro controller 2 (usb): started through its vendor interface\n";
        return finish(true);
    }

    // --- Input ---

    void refresh_config() {
        const std::uint64_t generation = input::controls_config_generation();
        if (generation != config_generation_) {
            config_ = input::get_controls_config();
            config_generation_ = generation;
        }
    }

    void handle_report(Device& device, std::span<const std::uint8_t> report) {
        const bool pro2 = device.scheme == ControlScheme::SwitchPro2;
        sw::DecodedState decoded{};
        const bool ok = pro2 ? sw::decode_switch2_report(report, device.switch2_imu, decoded)
                             : sw::decode_pro_report(report, device.imu, decoded);
        if (!ok) return;
        if (!device.driven) {
            device.driven = true;  // a Pro Controller 2 that another program started
            publish_counts();
        }
        refresh_config();
        const input::SchemeTuning& tuning = config_.tune(device.scheme);
        const std::uint64_t now = steady_us();
        float dt_report = device.last_report_us != 0 ? static_cast<float>(now - device.last_report_us) * 1e-6f : 0.0f;
        dt_report = std::clamp(dt_report, 0.0f, 0.05f);
        device.last_report_us = now;
        float gyro_x = 0.0f;
        float gyro_y = 0.0f;
        bool shake_started = false;
        sw::MotionSettings motion{};
        motion.shake = tuning.motion_shake != input::MotionShake::Off;
        motion.shake_sensitivity = tuning.shake_sensitivity;
        motion.gyro_pointer = tuning.gyro_pointer;
        motion.gyro_sensitivity = tuning.gyro_sensitivity;
        motion.picture_aspect = static_cast<float>(galaxy::kWiiContentAspect);
        for (std::size_t i = 0; i < decoded.imu_count; ++i) {
            // Switch Pro: three samples 5 ms apart; Pro 2: one per report.
            const float dt = pro2 ? dt_report : 0.005f;
            const std::uint64_t t = now - static_cast<std::uint64_t>((decoded.imu_count - 1u - i) * 5000u);
            const sw::SwitchMotion::Output out = device.motion.feed(decoded.imu[i], dt, t, motion);
            shake_started |= out.shake_started;
            gyro_x += out.pointer_x;
            gyro_y += out.pointer_y;
        }
        std::lock_guard lock(hub().lock);
        Published& published = hub().pads[device.slot];
        published.connected = true;
        SwitchPadState& state = published.state;
        state.connected = true;
        state.scheme = device.scheme;
        state.decoded = decoded;
        state.left = device.left;
        state.right = device.right;
        if (shake_started) state.shake_until_us = now + sw::SwitchMotion::kShakeHoldUs;
        state.gyro_x = std::clamp(state.gyro_x + gyro_x, -4.0f, 4.0f);
        state.gyro_y = std::clamp(state.gyro_y + gyro_y, -4.0f, 4.0f);
        state.imu_live = decoded.imu_count != 0u;
        state.gyro_rest_windows = device.motion.rest_windows();
        state.gravity = device.motion.gravity();
        state.gravity_valid = device.motion.gravity_ready();
        state.slot = static_cast<std::uint8_t>(device.slot);
    }

    // --- Vibration (controller_hid.cpp service_rumble) ---

    void service_rumble() {
        Hub& h = hub();
        RumbleRequest request;
        std::uint64_t published = 0;
        {
            std::lock_guard lock(h.rumble_lock);
            request = h.rumble_request;
            published = h.rumble_published_us;
        }
        const std::uint64_t now = steady_us();
        input::rumble::GateInputs gate{};
        gate.game_motor_on = request.level != 0u;
        gate.now_us = now;
        gate.published_us = published;
        gate.focused = request.focused;
        gate.paused = request.paused;
        gate.strength = request.strength;
        const bool run = !rumble_output_disabled() && !h.rumble_shutdown.load(std::memory_order_acquire) &&
                         input::rumble::motor_should_run(gate);
        const std::uint8_t level = run ? request.level : 0u;
        const bool xinput = level != 0u && request.target.kind == RumbleTarget::Kind::XInput &&
                            request.target.slot < XUSER_MAX_COUNT;
        const std::uint8_t want_mask = xinput ? static_cast<std::uint8_t>(1u << request.target.slot) : 0u;
        const std::uint8_t want_level = xinput ? level : 0u;
        const std::uint8_t want_strength = xinput ? request.strength : 0u;
        if (want_mask != xinput_want_mask_ || want_level != xinput_want_level_ || want_strength != xinput_want_strength_) {
            xinput_want_mask_ = want_mask;
            xinput_want_level_ = want_level;
            xinput_want_strength_ = want_strength;
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
                const auto bit = static_cast<std::uint8_t>(1u << slot);
                if ((want_mask & bit) != 0u) {
                    const input::rumble::XInputMotors speeds = input::rumble::xinput_motor_speeds(want_level, want_strength);
                    XINPUT_VIBRATION vibration{};
                    vibration.wLeftMotorSpeed = speeds.left;
                    vibration.wRightMotorSpeed = speeds.right;
                    h.xinput_rumbling.fetch_or(bit, std::memory_order_acq_rel);
                    if (XInputSetState(slot, &vibration) == ERROR_SUCCESS) {
                        xinput_on_ |= bit;
                    } else {
                        h.xinput_rumbling.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
                    }
                } else if ((xinput_on_ & bit) != 0u) {
                    XINPUT_VIBRATION off{};
                    (void)XInputSetState(slot, &off);
                    xinput_on_ = static_cast<std::uint8_t>(xinput_on_ & ~bit);
                    h.xinput_rumbling.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
                }
            }
        }
        unsigned timer_ms = xinput_on_ != 0u ? 25u : 0u;
        for (auto& device : devices_) {
            const bool target = request.target.kind == RumbleTarget::Kind::Switch && request.target.slot == device->slot;
            if (service_switch_rumble(*device, target ? level : std::uint8_t{0}, request.strength, now)) {
                const unsigned period = device->scheme == ControlScheme::SwitchPro2 ? 10u : 25u;
                timer_ms = timer_ms == 0u ? period : std::min(timer_ms, period);
            }
        }
        rumble_timer_ms_ = timer_ms;
    }

    bool service_switch_rumble(Device& device, std::uint8_t level, std::uint8_t strength, std::uint64_t now) {
        const bool pro2 = device.scheme == ControlScheme::SwitchPro2;
        const bool want = level != 0u;
        if (!want && !device.rumble_on) return false;
        const bool changed = want != device.rumble_on ||
                             (want && (level != device.rumble_level || strength != device.rumble_strength));
        const std::uint64_t since = now - device.rumble_sent_us;
        if (!changed && since < (pro2 ? kSwitch2RumbleRefreshUs : kSwitchRumbleRefreshUs)) return true;
        if (!pro2 && device.rumble_sent_us != 0u && since < kSwitchRumbleGapUs) return true;
        std::array<std::uint8_t, 128> report{};
        const std::size_t length = std::min<std::size_t>(device.output_length, report.size());
        const std::span<std::uint8_t> out(report.data(), length);
        const std::size_t built =
            pro2 ? input::rumble::build_switch2_rumble_report(device.rumble_sequence++, level, strength, out)
                 : input::rumble::build_switch_pro_rumble_report(
                       device.packet++, input::rumble::switch_pro_motor_rumble(level, strength), out);
        if (built == 0u) {
            device.rumble_on = false;
            return false;
        }
        Hub& h = hub();
        if (want) {
            std::unique_lock lock(h.rumble_device_lock);
            h.rumble_devices[device.slot] = device.ref;
            h.rumble_lengths[device.slot] = static_cast<std::uint16_t>(length);
            h.rumble_pro2[device.slot] = pro2;
        }
        (void)write_report(device.ref, report.data(), length);
        device.rumble_sent_us = now;
        device.rumble_on = want;
        device.rumble_level = level;
        device.rumble_strength = strength;
        if (!want) {
            std::unique_lock lock(h.rumble_device_lock);
            h.rumble_devices[device.slot] = nullptr;
        }
        return want;
    }

    void stop_xinput_rumble() {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const auto bit = static_cast<std::uint8_t>(1u << slot);
            if ((xinput_on_ & bit) == 0u) continue;
            XINPUT_VIBRATION off{};
            (void)XInputSetState(slot, &off);
            hub().xinput_rumbling.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
        }
        xinput_on_ = 0u;
        xinput_want_mask_ = 0u;
        xinput_want_level_ = 0u;
        xinput_want_strength_ = 0u;
    }

public:
    static inline Worker* active_worker_ = nullptr;

private:
    iokit::Manager manager_ = nullptr;
    std::vector<iokit::Device> matched_;
    // The device open_device is initialising, and whether it was removed meanwhile.
    iokit::Device opening_ = nullptr;
    bool opening_removed_ = false;
    std::vector<std::unique_ptr<Device>> devices_;
    input::ControlsConfig config_ = input::default_controls_config();
    std::uint64_t config_generation_ = 0;
    unsigned rumble_timer_ms_ = 0;
    std::uint8_t xinput_on_ = 0;
    std::uint8_t xinput_want_mask_ = 0;
    std::uint8_t xinput_want_level_ = 0;
    std::uint8_t xinput_want_strength_ = 0;
};

}  // namespace

void ensure_controller_hid_started() noexcept {
    Hub& h = hub();
    if (h.started.load(std::memory_order_acquire)) return;
    std::lock_guard lock(h.lifecycle);
    if (h.started.load(std::memory_order_relaxed)) return;
    try {
        static const bool exit_hook = [] {
            std::atexit([] { stop_all_rumble(); });
            return true;
        }();
        (void)exit_hook;
        h.stopping.store(false, std::memory_order_release);
        h.worker = std::thread([] {
            pthread_setname_np("nebula-controller-hid");
            Worker worker;
            Worker::active_worker_ = &worker;
            worker.run();
            Worker::active_worker_ = nullptr;
        });
        h.started.store(true, std::memory_order_release);
    } catch (...) {
    }
}

void stop_controller_hid() noexcept {
    Hub& h = hub();
    std::lock_guard lock(h.lifecycle);
    if (!h.started.load(std::memory_order_acquire)) return;
    h.stopping.store(true, std::memory_order_release);
    for (int attempt = 0; attempt < 100 && h.run_loop.load(std::memory_order_acquire) == nullptr; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    wake_worker();
    if (h.worker.joinable()) h.worker.join();
    h.started.store(false, std::memory_order_release);
}

std::size_t read_switch_pads(std::span<SwitchPadState> out, bool consume_gyro) noexcept {
    Hub& h = hub();
    if (!h.started.load(std::memory_order_acquire)) return 0;
    std::size_t count = 0;
    std::lock_guard lock(h.lock);
    for (Published& published : h.pads) {
        if (!published.connected || count >= out.size()) continue;
        out[count++] = published.state;
        if (consume_gyro) {
            published.state.gyro_x = 0.0f;
            published.state.gyro_y = 0.0f;
        }
    }
    return count;
}

std::uint64_t controller_hid_now_us() noexcept { return steady_us(); }

bool rumble_output_disabled() noexcept {
    static const bool disabled = env_flag("GALAXY_HIDE_WINDOW") || env_flag("GALAXY_RUMBLE_TRACE");
    return disabled;
}

void publish_rumble_request(const RumbleRequest& request) noexcept {
    Hub& h = hub();
    bool changed = false;
    {
        std::lock_guard lock(h.rumble_lock);
        const RumbleRequest previous = h.rumble_request;
        h.rumble_request = request;
        h.rumble_published_us = steady_us();
        changed = previous.level != request.level || previous.strength != request.strength ||
                  !(previous.target == request.target) || previous.focused != request.focused ||
                  previous.paused != request.paused;
    }
    if (changed && h.started.load(std::memory_order_acquire)) wake_worker();
}

void stop_all_rumble() noexcept {
    Hub& h = hub();
    h.rumble_shutdown.store(true, std::memory_order_release);
    const std::uint8_t rumbling = h.xinput_rumbling.exchange(0u, std::memory_order_acq_rel);
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        if ((rumbling & (1u << slot)) == 0u) continue;
        XINPUT_VIBRATION off{};
        (void)XInputSetState(slot, &off);
    }
    // Never blocks: the worker holds the lock only to publish or retire a device.
    std::shared_lock lock(h.rumble_device_lock, std::try_to_lock);
    if (!lock.owns_lock()) return;
    for (std::size_t slot = 0; slot < kMaxSwitchPads; ++slot) {
        if (h.rumble_devices[slot] != nullptr) {
            (void)write_rumble_off(h.rumble_devices[slot], h.rumble_lengths[slot], h.rumble_pro2[slot]);
        }
    }
}

ControllerHidStatus controller_hid_status() noexcept {
    ControllerHidStatus status{};
    status.switch_pro = hub().pro_count.load(std::memory_order_acquire);
    status.switch_pro2 = hub().pro2_count.load(std::memory_order_acquire);
    return status;
}

bool controller_hid_owns_switch_pro() noexcept { return hub().pro_count.load(std::memory_order_acquire) != 0u; }
bool controller_hid_owns_switch_pro2() noexcept { return hub().pro2_count.load(std::memory_order_acquire) != 0u; }

bool switch_controller_present_now() noexcept {
    const std::uint16_t products[] = {sw::kProControllerProductId, sw::kPro2ControllerProductId};
    return iokit::device_present(sw::kNintendoVendorId, products);
}

}  // namespace galaxy::host
