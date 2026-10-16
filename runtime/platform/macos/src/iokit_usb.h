// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// A thin IOUSBHost interface for controller_hid_macos.cpp: one USB interface opened from user space
// with its bulk IN and OUT pipes (the Pro Controller 2's vendor interface, which Windows reaches
// through WinUSB). IOUSBHost is Objective-C and its IOKit headers declare COM types that collide
// with the Win32 compat headers, so it stays in iokit_usb.mm. Every call is synchronous and runs on
// the controller worker thread.

#include <cstddef>
#include <cstdint>

namespace galaxy::macos::usb {

using Interface = void*;  // IOUSBHostInterface and its pipes

// Opens interface `number` of the USB device `vendor`:`product` whose location ID is
// `location_id` (any location when 0) and finds its bulk pipes. Null on failure, with the IOReturn
// in `error` (0 when no such interface is registered).
[[nodiscard]] Interface open_bulk_interface(std::uint16_t vendor, std::uint16_t product, std::uint8_t number,
    std::uint32_t location_id, int& error);
void close_interface(Interface handle) noexcept;

// One bulk OUT transfer of `size` bytes.
bool bulk_write(Interface handle, const std::uint8_t* data, std::size_t size, unsigned timeout_ms) noexcept;
// One bulk IN transfer of at most `size` bytes; the bytes received (0 on error or timeout).
std::size_t bulk_read(Interface handle, std::uint8_t* data, std::size_t size, unsigned timeout_ms) noexcept;

}  // namespace galaxy::macos::usb
