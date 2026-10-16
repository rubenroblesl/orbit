// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Real Wii Remotes on macOS: the IOKit HID counterpart of the Windows RealWiimoteHidBridge
// (native_host.cpp). A Wii Remote paired in System Settings › Bluetooth is a HID device (vendor
// 057E; product 0306 for the RVL-CNT-01, 0330 for the RVL-CNT-01-TR). As on Windows, its input
// reports reach the game's Bluetooth stack unchanged and the game's output reports (LEDs, rumble,
// report mode, IR camera, extension handshake, speaker) reach the remote unchanged, so the game
// itself reads the buttons, the pointer, the motion and the Nunchuk. A remote that disconnects is
// waited for: the bridge reports the device link (input::NativeHidIoBackend::device_link) and
// opens the remote again when it comes back.
//
// The bridge's logic is separate from IOKit (Transport) so it can be tested with a fake remote.

#include "galaxy/native_input.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace galaxy::macos::wiimote {

inline constexpr std::uint16_t kNintendoVendorId = 0x057e;
inline constexpr std::uint16_t kWiiRemoteProductId = 0x0306;    // RVL-CNT-01
inline constexpr std::uint16_t kWiiRemotePlusProductId = 0x0330;  // RVL-CNT-01-TR (MotionPlus inside)

struct DeviceInfo {
    std::uint16_t product_id = 0;
    bool bluetooth = false;
    std::size_t max_input_report = 0;
};

// What a transport reports, from its own thread.
class TransportSink {
public:
    virtual ~TransportSink() = default;
    virtual void remote_opened(const DeviceInfo& info) noexcept = 0;
    // A remote was found but could not be opened (another app may hold it).
    virtual void remote_open_failed(const char* reason) noexcept = 0;
    virtual void remote_removed() noexcept = 0;
    // One input report, report ID first.
    virtual void input_report(std::span<const std::uint8_t> report) noexcept = 0;
    virtual void transport_failed(const char* reason) noexcept = 0;
};

// The HID side: discovery, input reports and output reports of the first Wii Remote it opens.
class Transport {
public:
    virtual ~Transport() = default;
    // Starts reporting to `sink` from a thread of its own and returns.
    virtual void start(TransportSink& sink) = 0;
    // Asks the thread to finish; does not wait.
    virtual void request_stop() noexcept = 0;
    // Waits for the thread; the sink is not called after this returns.
    virtual void stop() noexcept = 0;
    // One output report (report ID first) to the open remote; false when it was not accepted.
    [[nodiscard]] virtual bool write(std::span<const std::uint8_t> report) noexcept = 0;
};

struct BridgeOptions {
    // How long open() waits for a connected remote to be matched.
    std::chrono::milliseconds discovery_timeout{2000};
    // Output retries, as the Windows bridge does for a remote whose radio link is waking up.
    std::chrono::milliseconds write_retry_interval{50};
    unsigned write_attempts = 40;
    // Input reports held for the worker's 1 ms polls; the oldest is dropped past this.
    std::size_t max_pending_input = 256;
    bool trace = false;  // GALAXY_TRACE_WIIMOTE
};

// The input::NativeHidIoBackend behind input::NativeHidIoWorker for RuntimeInputMode::RealWiimote.
[[nodiscard]] std::unique_ptr<input::NativeHidIoBackend> make_bridge(
    std::unique_ptr<Transport> transport,
    BridgeOptions options);

// IOKit HID (IOHIDManager on a run loop thread of its own).
[[nodiscard]] std::unique_ptr<Transport> make_iokit_transport();

// The runtime's bridge: IOKit, with the trace from GALAXY_TRACE_WIIMOTE.
[[nodiscard]] std::unique_ptr<input::NativeHidIoBackend> make_wiimote_hid_backend();

// Synchronous check: a Wii Remote is connected to this Mac.
[[nodiscard]] bool wiimote_connected();

}  // namespace galaxy::macos::wiimote
