// SPDX-License-Identifier: GPL-3.0-only

#include <windows.h>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#include <cstring>

namespace galaxy::host {

bool macos_module_text_range(void* module, const unsigned char*& base, std::size_t& size) noexcept {
    for (std::uint32_t i = 0, count = _dyld_image_count(); i < count; ++i) {
        void* const handle = dlopen(_dyld_get_image_name(i), RTLD_NOW | RTLD_NOLOAD);
        if (handle == nullptr) continue;
        dlclose(handle);
        if (handle != module) continue;
        const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
        const std::intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const auto* command = reinterpret_cast<const load_command*>(header + 1);
        for (std::uint32_t c = 0; c < header->ncmds; ++c) {
            if (command->cmd == LC_SEGMENT_64) {
                const auto* segment = reinterpret_cast<const segment_command_64*>(command);
                if (std::strncmp(segment->segname, SEG_TEXT, sizeof(segment->segname)) == 0) {
                    base = reinterpret_cast<const unsigned char*>(segment->vmaddr + slide);
                    size = static_cast<std::size_t>(segment->vmsize);
                    return true;
                }
            }
            command = reinterpret_cast<const load_command*>(reinterpret_cast<const std::byte*>(command) + command->cmdsize);
        }
        return false;
    }
    return false;
}

}  // namespace galaxy::host
