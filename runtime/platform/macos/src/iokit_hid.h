// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// A thin IOKit HID interface for controller_hid_macos.cpp. IOKit's CoreFoundation headers declare
// COM types (IUnknown, REFIID) that collide with the Win32 compat headers, so they stay in
// iokit_hid.cpp. Every call runs on the worker thread that owns the manager's run loop, except
// set_output_report, which IOKit accepts from any thread.

#include <cstddef>
#include <cstdint>
#include <span>

namespace galaxy::macos::hid {

using Device = void*;  // IOHIDDeviceRef
using Manager = void*; // IOHIDManagerRef
using RunLoop = void*; // CFRunLoopRef

struct ManagerCallbacks {
    void* context = nullptr;
    void (*matched)(void* context, Device device) = nullptr;   // device retained for the callee
    void (*removed)(void* context, Device device) = nullptr;
};

// Matches `vendor` with any of `products` on the calling thread's run loop; null when IOKit refuses.
[[nodiscard]] Manager create_manager(std::uint16_t vendor, std::span<const std::uint16_t> products,
    const ManagerCallbacks& callbacks, int& open_result);
void destroy_manager(Manager manager);

[[nodiscard]] RunLoop current_run_loop();
void wake_run_loop(RunLoop loop);
// Runs the calling thread's run loop for at most `seconds`, returning after one handled source.
void run_loop_once(double seconds);

void retain(Device device);
void release(Device device);
[[nodiscard]] bool open_device(Device device);
void close_device(Device device);
[[nodiscard]] bool is_bluetooth(Device device);
[[nodiscard]] long max_input_report_size(Device device);
[[nodiscard]] long product_id(Device device);
// The USB location ID (0 when the transport has none), shared by the device's other USB interfaces.
[[nodiscard]] long location_id(Device device);

using InputCallback = void (*)(void* context, const std::uint8_t* report, std::size_t size);
// Input reports (report ID first) into `buffer` until unregister_input; returns a registration.
[[nodiscard]] void* register_input(Device device, std::uint8_t* buffer, std::size_t size, InputCallback callback,
    void* context);
void unregister_input(Device device, std::uint8_t* buffer, std::size_t size, void* registration);

// One output report; `report[0]` is the report ID.
bool set_output_report(Device device, const std::uint8_t* report, std::size_t size) noexcept;

// Synchronous enumeration: a device of `vendor` with one of `products` is attached.
[[nodiscard]] bool device_present(std::uint16_t vendor, std::span<const std::uint16_t> products);

}  // namespace galaxy::macos::hid
