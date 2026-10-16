// SPDX-License-Identifier: GPL-3.0-only
//
// IOUSBHost behind iokit_usb.h (no Win32 compat headers in this file).

#include "iokit_usb.h"

#import <Foundation/Foundation.h>
#import <IOUSBHost/IOUSBHost.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBHostFamilyDefinitions.h>

#include <cstring>

namespace galaxy::macos::usb {

namespace {

struct BulkInterface {
    IOUSBHostInterface* interface = nil;
    IOUSBHostPipe* in = nil;
    IOUSBHostPipe* out = nil;
};

// The USB device's location ID, from the interface or the device above it.
std::uint32_t location_of(io_service_t service) {
    std::uint32_t location = 0;
    CFTypeRef value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR(kUSBHostPropertyLocationID),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (value != nullptr) {
        if (CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &location);
        }
        CFRelease(value);
    }
    return location;
}

// Opens the interface (exclusively: nil while another client holds it) and its first bulk pipe of
// each direction.
BulkInterface* open_service(io_service_t service, int& error) {
    NSError* failure = nil;
    IOUSBHostInterface* interface = [[IOUSBHostInterface alloc] initWithIOService:service
                                                                          options:IOUSBHostObjectInitOptionsNone
                                                                            queue:nil
                                                                            error:&failure
                                                                  interestHandler:nil];
    if (interface == nil) {
        error = failure != nil ? static_cast<int>(failure.code) : kIOReturnError;
        return nullptr;
    }
    auto* bulk = new BulkInterface{interface, nil, nil};
    const IOUSBConfigurationDescriptor* configuration = interface.configurationDescriptor;
    const IOUSBInterfaceDescriptor* descriptor = interface.interfaceDescriptor;
    const IOUSBEndpointDescriptor* endpoint = nullptr;
    while (configuration != nullptr && descriptor != nullptr &&
           (endpoint = IOUSBGetNextEndpointDescriptor(configuration, descriptor,
                reinterpret_cast<const IOUSBDescriptorHeader*>(endpoint))) != nullptr) {
        if (IOUSBGetEndpointType(endpoint) != kIOUSBEndpointTypeBulk) continue;
        const bool in = IOUSBGetEndpointDirection(endpoint) == kIOUSBEndpointDirectionIn;
        if (in ? bulk->in != nil : bulk->out != nil) continue;
        IOUSBHostPipe* pipe = [interface copyPipeWithAddress:IOUSBGetEndpointAddress(endpoint) error:nil];
        if (in) {
            bulk->in = pipe;
        } else {
            bulk->out = pipe;
        }
    }
    if (bulk->in == nil || bulk->out == nil) {
        error = kIOReturnNotFound;
        close_interface(bulk);
        return nullptr;
    }
    return bulk;
}

// A transfer buffer of exactly `size` bytes (the transfer length), mapped for the controller when
// IOUSBHost can.
NSMutableData* transfer_buffer(IOUSBHostInterface* interface, std::size_t size) {
    NSMutableData* data = [interface ioDataWithCapacity:size error:nil];
    return data != nil && data.length == size ? data : [NSMutableData dataWithLength:size];
}

}  // namespace

Interface open_bulk_interface(std::uint16_t vendor, std::uint16_t product, std::uint8_t number,
    std::uint32_t location_id, int& error) {
    error = 0;
    @autoreleasepool {
        CFMutableDictionaryRef matching = [IOUSBHostInterface createMatchingDictionaryWithVendorID:@(vendor)
                                                                                         productID:@(product)
                                                                                         bcdDevice:nil
                                                                                   interfaceNumber:@(number)
                                                                                configurationValue:nil
                                                                                    interfaceClass:nil
                                                                                 interfaceSubclass:nil
                                                                                 interfaceProtocol:nil
                                                                                             speed:nil
                                                                                    productIDArray:nil];
        io_iterator_t iterator = IO_OBJECT_NULL;
        // Consumes `matching`.
        if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS) return nullptr;
        BulkInterface* bulk = nullptr;
        while (bulk == nullptr) {
            const io_service_t service = IOIteratorNext(iterator);
            if (service == IO_OBJECT_NULL) break;
            // The interface object keeps its own reference to the service.
            if (location_id == 0u || location_of(service) == location_id) bulk = open_service(service, error);
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        return bulk;
    }
}

void close_interface(Interface handle) noexcept {
    auto* bulk = static_cast<BulkInterface*>(handle);
    if (bulk == nullptr) return;
    @autoreleasepool {
        bulk->in = nil;
        bulk->out = nil;
        [bulk->interface destroy];
        bulk->interface = nil;
    }
    delete bulk;
}

bool bulk_write(Interface handle, const std::uint8_t* data, std::size_t size, unsigned timeout_ms) noexcept {
    auto* bulk = static_cast<BulkInterface*>(handle);
    if (bulk == nullptr || data == nullptr || size == 0u) return false;
    @autoreleasepool {
        NSMutableData* buffer = transfer_buffer(bulk->interface, size);
        std::memcpy(buffer.mutableBytes, data, size);
        NSUInteger done = 0;
        return [bulk->out sendIORequestWithData:buffer
                               bytesTransferred:&done
                              completionTimeout:timeout_ms / 1000.0
                                          error:nil] &&
               done == size;
    }
}

std::size_t bulk_read(Interface handle, std::uint8_t* data, std::size_t size, unsigned timeout_ms) noexcept {
    auto* bulk = static_cast<BulkInterface*>(handle);
    if (bulk == nullptr || data == nullptr || size == 0u) return 0;
    @autoreleasepool {
        NSMutableData* buffer = transfer_buffer(bulk->interface, size);
        NSUInteger done = 0;
        if (![bulk->in sendIORequestWithData:buffer
                            bytesTransferred:&done
                           completionTimeout:timeout_ms / 1000.0
                                       error:nil]) {
            return 0;
        }
        const std::size_t got = done < size ? done : size;
        std::memcpy(data, buffer.bytes, got);
        return got;
    }
}

}  // namespace galaxy::macos::usb
