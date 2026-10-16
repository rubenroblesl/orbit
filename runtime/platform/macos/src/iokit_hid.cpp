// SPDX-License-Identifier: GPL-3.0-only
//
// IOKit HID behind iokit_hid.h (no Win32 compat headers in this file).

#include "iokit_hid.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <vector>

namespace galaxy::macos::hid {

namespace {

struct ManagerState {
    IOHIDManagerRef manager = nullptr;
    ManagerCallbacks callbacks;
};

struct InputRegistration {
    InputCallback callback;
    void* context;
};

long number_property(IOHIDDeviceRef device, CFStringRef key) {
    const auto value = static_cast<CFTypeRef>(IOHIDDeviceGetProperty(device, key));
    long number = 0;
    if (value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID()) {
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &number);
    }
    return number;
}

CFArrayRef matching_array(std::uint16_t vendor, std::span<const std::uint16_t> products) {
    CFMutableArrayRef array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    for (const std::uint16_t product : products) {
        CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const int vendor_value = vendor;
        const int product_value = product;
        CFNumberRef vendor_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor_value);
        CFNumberRef product_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &product_value);
        CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), vendor_number);
        CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), product_number);
        CFRelease(vendor_number);
        CFRelease(product_number);
        CFArrayAppendValue(array, match);
        CFRelease(match);
    }
    return array;
}

void on_matched(void* context, IOReturn, void*, IOHIDDeviceRef device) {
    auto* state = static_cast<ManagerState*>(context);
    CFRetain(device);
    state->callbacks.matched(state->callbacks.context, device);
}

void on_removed(void* context, IOReturn, void*, IOHIDDeviceRef device) {
    auto* state = static_cast<ManagerState*>(context);
    state->callbacks.removed(state->callbacks.context, device);
}

void on_report(void* context, IOReturn result, void*, IOHIDReportType, std::uint32_t, std::uint8_t* report,
    CFIndex length) {
    if (result != kIOReturnSuccess || length <= 0) return;
    auto* registration = static_cast<InputRegistration*>(context);
    registration->callback(registration->context, report, static_cast<std::size_t>(length));
}

}  // namespace

Manager create_manager(std::uint16_t vendor, std::span<const std::uint16_t> products,
    const ManagerCallbacks& callbacks, int& open_result) {
    auto* state = new ManagerState{IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone), callbacks};
    if (state->manager == nullptr) {
        delete state;
        open_result = kIOReturnError;
        return nullptr;
    }
    CFArrayRef matching = matching_array(vendor, products);
    IOHIDManagerSetDeviceMatchingMultiple(state->manager, matching);
    CFRelease(matching);
    IOHIDManagerRegisterDeviceMatchingCallback(state->manager, on_matched, state);
    IOHIDManagerRegisterDeviceRemovalCallback(state->manager, on_removed, state);
    IOHIDManagerScheduleWithRunLoop(state->manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    open_result = IOHIDManagerOpen(state->manager, kIOHIDOptionsTypeNone);
    return state;
}

void destroy_manager(Manager manager) {
    auto* state = static_cast<ManagerState*>(manager);
    if (state == nullptr) return;
    IOHIDManagerUnscheduleFromRunLoop(state->manager, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDManagerClose(state->manager, kIOHIDOptionsTypeNone);
    CFRelease(state->manager);
    delete state;
}

RunLoop current_run_loop() { return CFRunLoopGetCurrent(); }

void wake_run_loop(RunLoop loop) {
    if (loop != nullptr) CFRunLoopWakeUp(static_cast<CFRunLoopRef>(loop));
}

void run_loop_once(double seconds) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true); }

void retain(Device device) { CFRetain(static_cast<IOHIDDeviceRef>(device)); }
void release(Device device) { CFRelease(static_cast<IOHIDDeviceRef>(device)); }

bool open_device(Device device) {
    return IOHIDDeviceOpen(static_cast<IOHIDDeviceRef>(device), kIOHIDOptionsTypeNone) == kIOReturnSuccess;
}

void close_device(Device device) { IOHIDDeviceClose(static_cast<IOHIDDeviceRef>(device), kIOHIDOptionsTypeNone); }

bool is_bluetooth(Device device) {
    const auto value = static_cast<CFTypeRef>(
        IOHIDDeviceGetProperty(static_cast<IOHIDDeviceRef>(device), CFSTR(kIOHIDTransportKey)));
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) return false;
    return CFStringFind(static_cast<CFStringRef>(value), CFSTR("Bluetooth"), 0).location != kCFNotFound;
}

long max_input_report_size(Device device) {
    return number_property(static_cast<IOHIDDeviceRef>(device), CFSTR(kIOHIDMaxInputReportSizeKey));
}

long product_id(Device device) { return number_property(static_cast<IOHIDDeviceRef>(device), CFSTR(kIOHIDProductIDKey)); }

long location_id(Device device) {
    return number_property(static_cast<IOHIDDeviceRef>(device), CFSTR(kIOHIDLocationIDKey));
}

void* register_input(Device device, std::uint8_t* buffer, std::size_t size, InputCallback callback, void* context) {
    auto* registration = new InputRegistration{callback, context};
    IOHIDDeviceRegisterInputReportCallback(static_cast<IOHIDDeviceRef>(device), buffer, static_cast<CFIndex>(size),
        on_report, registration);
    // Devices of a scheduled manager are already on its run loop.
    return registration;
}

void unregister_input(Device device, std::uint8_t* buffer, std::size_t size, void* registration) {
    IOHIDDeviceRegisterInputReportCallback(static_cast<IOHIDDeviceRef>(device), buffer, static_cast<CFIndex>(size),
        nullptr, nullptr);
    delete static_cast<InputRegistration*>(registration);
}

bool set_output_report(Device device, const std::uint8_t* report, std::size_t size) noexcept {
    if (device == nullptr || report == nullptr || size == 0u) return false;
    return IOHIDDeviceSetReport(static_cast<IOHIDDeviceRef>(device), kIOHIDReportTypeOutput, report[0], report,
               static_cast<CFIndex>(size)) == kIOReturnSuccess;
}

bool device_present(std::uint16_t vendor, std::span<const std::uint16_t> products) {
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager == nullptr) return false;
    CFArrayRef matching = matching_array(vendor, products);
    IOHIDManagerSetDeviceMatchingMultiple(manager, matching);
    CFRelease(matching);
    bool present = false;
    if (CFSetRef devices = IOHIDManagerCopyDevices(manager)) {
        present = CFSetGetCount(devices) > 0;
        CFRelease(devices);
    }
    CFRelease(manager);
    return present;
}

}  // namespace galaxy::macos::hid
