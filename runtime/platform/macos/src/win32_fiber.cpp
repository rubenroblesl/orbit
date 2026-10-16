// SPDX-License-Identifier: GPL-3.0-only
//
// Win32 fibers on macOS/AArch64.
//
// A switch stores the AAPCS64 callee-saved state (x19-x28, fp, lr, d8-d15) and FPCR on the
// outgoing stack, records that stack pointer in the outgoing fiber and resumes the incoming one
// from its saved stack pointer. A new fiber's stack holds a frame whose saved lr is the entry
// trampoline, which calls the start routine with x19 = the fiber.
//
// Fiber stacks are anonymous mappings (address space only until touched) with an inaccessible
// guard page below them, so an overflow faults instead of corrupting a neighbour.

#include <windows.h>

#include "win32_internal.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if !defined(__aarch64__)
#error "win32_fiber.cpp implements the AArch64 context switch"
#endif

namespace {

struct Fiber {
    void* stack_pointer = nullptr;  // saved context while not running
    void* mapping = nullptr;        // stack mapping including the guard page (null for a thread fiber)
    std::size_t mapping_size = 0;
    LPFIBER_START_ROUTINE start = nullptr;
    void* parameter = nullptr;
    bool thread_fiber = false;
};

thread_local Fiber* t_current = nullptr;

constexpr std::size_t kContextFrameSize = 176;  // see the layout in galaxy_fiber_switch

}  // namespace

extern "C" void galaxy_fiber_switch(void** save_stack_pointer, void* load_stack_pointer);
extern "C" void galaxy_fiber_trampoline();

// Frame layout (offsets from the saved sp):
//   0 x19 x20, 16 x21 x22, 32 x23 x24, 48 x25 x26, 64 x27 x28, 80 x29 x30,
//   96 d8 d9, 112 d10 d11, 128 d12 d13, 144 d14 d15, 160 fpcr, 168 padding
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _galaxy_fiber_switch\n"
    "_galaxy_fiber_switch:\n"
    "    sub sp, sp, #176\n"
    "    stp x19, x20, [sp, #0]\n"
    "    stp x21, x22, [sp, #16]\n"
    "    stp x23, x24, [sp, #32]\n"
    "    stp x25, x26, [sp, #48]\n"
    "    stp x27, x28, [sp, #64]\n"
    "    stp x29, x30, [sp, #80]\n"
    "    stp d8, d9, [sp, #96]\n"
    "    stp d10, d11, [sp, #112]\n"
    "    stp d12, d13, [sp, #128]\n"
    "    stp d14, d15, [sp, #144]\n"
    "    mrs x9, fpcr\n"
    "    str x9, [sp, #160]\n"
    "    mov x9, sp\n"
    "    str x9, [x0]\n"
    "    mov sp, x1\n"
    "    ldr x9, [sp, #160]\n"
    "    msr fpcr, x9\n"
    "    ldp x19, x20, [sp, #0]\n"
    "    ldp x21, x22, [sp, #16]\n"
    "    ldp x23, x24, [sp, #32]\n"
    "    ldp x25, x26, [sp, #48]\n"
    "    ldp x27, x28, [sp, #64]\n"
    "    ldp x29, x30, [sp, #80]\n"
    "    ldp d8, d9, [sp, #96]\n"
    "    ldp d10, d11, [sp, #112]\n"
    "    ldp d12, d13, [sp, #128]\n"
    "    ldp d14, d15, [sp, #144]\n"
    "    add sp, sp, #176\n"
    "    ret\n"
    ".p2align 4\n"
    ".globl _galaxy_fiber_trampoline\n"
    "_galaxy_fiber_trampoline:\n"
    "    mov x29, #0\n"
    "    mov x0, x19\n"
    "    bl _galaxy_fiber_run\n"
    "    brk #0x1\n");

extern "C" [[noreturn]] void galaxy_fiber_run(Fiber* fiber) {
    fiber->start(fiber->parameter);
    // A start routine that returns has no caller to return to: Windows exits the thread.
    std::fprintf(stderr, "[macos-compat] fiber start routine returned\n");
    std::abort();
}

LPVOID ConvertThreadToFiberEx(LPVOID parameter, DWORD) noexcept {
    if (t_current != nullptr) {
        SetLastError(1280u);  // ERROR_ALREADY_FIBER
        return nullptr;
    }
    auto* fiber = new Fiber();
    fiber->thread_fiber = true;
    fiber->parameter = parameter;
    t_current = fiber;
    return fiber;
}

LPVOID ConvertThreadToFiber(LPVOID parameter) noexcept { return ConvertThreadToFiberEx(parameter, 0u); }

BOOL ConvertFiberToThread() noexcept {
    if (t_current == nullptr || !t_current->thread_fiber) {
        SetLastError(1281u);  // ERROR_ALREADY_THREAD
        return FALSE;
    }
    delete t_current;
    t_current = nullptr;
    return TRUE;
}

LPVOID CreateFiberEx(SIZE_T, SIZE_T reserve_size, DWORD, LPFIBER_START_ROUTINE start, LPVOID parameter) noexcept {
    const std::size_t page = static_cast<std::size_t>(getpagesize());
    std::size_t stack_size = reserve_size != 0 ? reserve_size : 1024u * 1024u;
    stack_size = (stack_size + page - 1u) & ~(page - 1u);
    const std::size_t mapping_size = stack_size + page;
    void* const mapping = mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        galaxy::compat::set_last_error_from_errno(errno);
        return nullptr;
    }
    (void)mprotect(mapping, page, PROT_NONE);  // guard page below the stack
    auto* fiber = new Fiber();
    fiber->mapping = mapping;
    fiber->mapping_size = mapping_size;
    fiber->start = start;
    fiber->parameter = parameter;
    // Initial context: x19 = fiber, lr = trampoline, FPCR = the creating thread's.
    auto* top = static_cast<std::byte*>(mapping) + mapping_size;
    auto* frame = reinterpret_cast<std::uint64_t*>(top - kContextFrameSize);
    for (std::size_t i = 0; i < kContextFrameSize / sizeof(std::uint64_t); ++i) frame[i] = 0;
    frame[0] = reinterpret_cast<std::uint64_t>(fiber);
    frame[11] = reinterpret_cast<std::uint64_t>(&galaxy_fiber_trampoline);
    std::uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    frame[20] = fpcr;
    fiber->stack_pointer = frame;
    return fiber;
}

LPVOID CreateFiber(SIZE_T stack_size, LPFIBER_START_ROUTINE start, LPVOID parameter) noexcept {
    return CreateFiberEx(0, stack_size, 0u, start, parameter);
}

void SwitchToFiber(LPVOID target) noexcept {
    Fiber* const current = t_current;
    auto* next = static_cast<Fiber*>(target);
    if (current == nullptr || next == nullptr || next == current) {
        if (next != current) {
            std::fprintf(stderr, "[macos-compat] SwitchToFiber outside a fiber\n");
            std::abort();
        }
        return;
    }
    t_current = next;
    galaxy_fiber_switch(&current->stack_pointer, next->stack_pointer);
    // Resumed: another fiber switched back to this one on this thread.
    t_current = current;
}

void DeleteFiber(LPVOID target) noexcept {
    auto* fiber = static_cast<Fiber*>(target);
    if (fiber == nullptr) return;
    if (fiber == t_current) {
        // Deleting the running fiber exits the thread on Windows; the runtime never does it.
        std::fprintf(stderr, "[macos-compat] DeleteFiber on the running fiber\n");
        std::abort();
    }
    if (fiber->mapping != nullptr) munmap(fiber->mapping, fiber->mapping_size);
    delete fiber;
}

LPVOID GetCurrentFiber() noexcept { return t_current; }
LPVOID GetFiberData() noexcept { return t_current != nullptr ? t_current->parameter : nullptr; }
BOOL IsThreadAFiber() noexcept { return t_current != nullptr ? TRUE : FALSE; }
