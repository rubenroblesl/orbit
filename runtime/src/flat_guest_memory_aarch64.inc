// SPDX-License-Identifier: GPL-3.0-only
//
// AArch64 memory-operand emulation for fastmem faults on macOS. Included by flat_guest_memory.cpp
// inside its anonymous namespace.
//
// AArch64 accesses memory only through load/store instructions, so the decoder covers the
// load/store classes clang emits for the generated access templates: single registers with an
// unsigned, unscaled, pre- or post-indexed immediate or a register offset (LDR/STR and the
// sign-extending forms, general-purpose and SIMD&FP), register pairs (LDP/STP/LDPSW), and the
// AdvSIMD single-register LD1/ST1 forms (whole register, one lane, LD1R). The value moves between
// the register and the guest memory image in little-endian order, exactly as the native
// instruction would have moved it; the module's own REV instructions do the byte swapping.
// Anything else is reported and left to the previous handler.

#if !defined(__aarch64__)
#error "flat_guest_memory_aarch64.inc decodes AArch64 instructions"
#endif

struct Aarch64Access {
    bool valid = false;
    bool load = false;
    bool vector = false;        // Rt names a SIMD&FP register
    bool pair = false;
    bool prefetch = false;      // PRFM: completes without an access
    bool sign_extend = false;
    unsigned extend_to = 8;     // sign-extension width in bytes (4 or 8)
    unsigned size = 0;          // bytes per register
    unsigned rt = 0;
    unsigned rt2 = 0;
    unsigned rn = 0;
    std::uint64_t address = 0;  // host effective address
    bool writeback = false;
    std::uint64_t writeback_value = 0;
    // AdvSIMD structure forms
    bool lane = false;
    bool replicate = false;
    bool q = true;
    unsigned lane_index = 0;
    unsigned vector_bytes = 16;  // register bytes the instruction writes (8 with Q=0)
};

#if __DARWIN_OPAQUE_ARM_THREAD_STATE64
#error "the fastmem decoder reads the arm64 thread state fields directly"
#endif

std::uint64_t aarch64_x(const _STRUCT_ARM_THREAD_STATE64& state, unsigned index, bool sp_for_31) noexcept {
    if (index < 29u) return state.__x[index];
    if (index == 29u) return state.__fp;
    if (index == 30u) return state.__lr;
    return sp_for_31 ? state.__sp : 0u;
}

void aarch64_set_x(_STRUCT_ARM_THREAD_STATE64& state, unsigned index, bool sp_for_31, std::uint64_t value) noexcept {
    if (index < 29u) state.__x[index] = value;
    else if (index == 29u) state.__fp = value;
    else if (index == 30u) state.__lr = value;
    else if (sp_for_31) state.__sp = value;
    // XZR/WZR discards the value.
}

constexpr std::int64_t aarch64_sign_extend(std::uint64_t value, unsigned bits) noexcept {
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1u);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

// size/V/opc of the single-register forms: access width, direction and extension.
bool aarch64_classify_single(unsigned size, unsigned vector, unsigned opc, Aarch64Access& access) noexcept {
    access.vector = vector != 0u;
    if (access.vector) {
        if ((opc & 2u) != 0u) {
            if (size != 0u) return false;
            access.size = 16u;
        } else {
            access.size = 1u << size;
        }
        access.load = (opc & 1u) != 0u;
        return true;
    }
    access.size = 1u << size;
    switch (opc) {
    case 0u:
        access.load = false;
        return true;
    case 1u:
        access.load = true;
        return true;
    case 2u:
        if (size == 3u) {
            access.prefetch = true;
            return true;
        }
        access.load = true;
        access.sign_extend = true;
        access.extend_to = 8u;
        return true;
    case 3u:
        if (size >= 2u) return false;
        access.load = true;
        access.sign_extend = true;
        access.extend_to = 4u;
        return true;
    default:
        return false;
    }
}

unsigned aarch64_log2(unsigned value) noexcept {
    return static_cast<unsigned>(__builtin_ctz(value));
}

Aarch64Access aarch64_decode(std::uint32_t insn, const _STRUCT_ARM_THREAD_STATE64& state) noexcept {
    Aarch64Access access{};
    const unsigned rn = (insn >> 5u) & 31u;
    const unsigned rt = insn & 31u;
    access.rn = rn;
    access.rt = rt;
    const std::uint64_t base = aarch64_x(state, rn, true);

    if ((insn & 0x3B000000u) == 0x39000000u) {
        // Load/store register, unsigned immediate.
        if (!aarch64_classify_single(insn >> 30u, (insn >> 26u) & 1u, (insn >> 22u) & 3u, access)) return access;
        const std::uint64_t imm12 = (insn >> 10u) & 0xFFFu;
        access.address = base + (imm12 << aarch64_log2(access.size));
        access.valid = true;
        return access;
    }
    if ((insn & 0x3B200000u) == 0x38000000u) {
        // Load/store register: unscaled immediate, post-index, unprivileged, pre-index.
        if (!aarch64_classify_single(insn >> 30u, (insn >> 26u) & 1u, (insn >> 22u) & 3u, access)) return access;
        const auto imm9 = static_cast<std::uint64_t>(aarch64_sign_extend((insn >> 12u) & 0x1FFu, 9u));
        switch ((insn >> 10u) & 3u) {
        case 1u:  // post-index
            access.address = base;
            access.writeback = true;
            access.writeback_value = base + imm9;
            break;
        case 3u:  // pre-index
            access.address = base + imm9;
            access.writeback = true;
            access.writeback_value = access.address;
            break;
        default:  // unscaled, unprivileged
            access.address = base + imm9;
            break;
        }
        access.valid = true;
        return access;
    }
    if ((insn & 0x3B200C00u) == 0x38200800u) {
        // Load/store register, register offset.
        if (!aarch64_classify_single(insn >> 30u, (insn >> 26u) & 1u, (insn >> 22u) & 3u, access)) return access;
        const unsigned rm = (insn >> 16u) & 31u;
        const unsigned option = (insn >> 13u) & 7u;
        const bool shift = ((insn >> 12u) & 1u) != 0u;
        std::uint64_t offset = aarch64_x(state, rm, false);
        switch (option) {
        case 2u: offset = static_cast<std::uint32_t>(offset); break;                               // UXTW
        case 6u: offset = static_cast<std::uint64_t>(aarch64_sign_extend(offset & 0xFFFFFFFFu, 32u)); break;  // SXTW
        case 3u:                                                                                    // LSL / UXTX
        case 7u: break;                                                                             // SXTX
        default: return access;
        }
        if (shift) offset <<= aarch64_log2(access.size);
        access.address = base + offset;
        access.valid = true;
        return access;
    }
    if ((insn & 0x3A000000u) == 0x28000000u) {
        // Load/store register pair.
        const unsigned opc = insn >> 30u;
        const bool vector = ((insn >> 26u) & 1u) != 0u;
        const unsigned mode = (insn >> 23u) & 3u;
        access.load = ((insn >> 22u) & 1u) != 0u;
        access.vector = vector;
        access.pair = true;
        access.rt2 = (insn >> 10u) & 31u;
        if (vector) {
            if (opc == 3u) return access;
            access.size = 4u << opc;
        } else if (opc == 0u) {
            access.size = 4u;
        } else if (opc == 2u) {
            access.size = 8u;
        } else if (opc == 1u && access.load) {
            access.size = 4u;  // LDPSW
            access.sign_extend = true;
            access.extend_to = 8u;
        } else {
            return access;
        }
        const auto offset = static_cast<std::uint64_t>(aarch64_sign_extend((insn >> 15u) & 0x7Fu, 7u) *
                                                       static_cast<std::int64_t>(access.size));
        switch (mode) {
        case 1u:  // post-index
            access.address = base;
            access.writeback = true;
            access.writeback_value = base + offset;
            break;
        case 3u:  // pre-index
            access.address = base + offset;
            access.writeback = true;
            access.writeback_value = access.address;
            break;
        default:  // signed offset, non-temporal
            access.address = base + offset;
            break;
        }
        access.valid = true;
        return access;
    }
    if ((insn & 0xBFBFF000u) == 0x0C007000u || (insn & 0xBFA0F000u) == 0x0C807000u) {
        // AdvSIMD LD1/ST1 (multiple structures), one register; the second form post-indexes.
        access.vector = true;
        access.load = ((insn >> 22u) & 1u) != 0u;
        access.q = ((insn >> 30u) & 1u) != 0u;
        access.size = access.q ? 16u : 8u;
        access.vector_bytes = access.size;
        access.address = base;
        if ((insn & 0x00800000u) != 0u) {
            const unsigned rm = (insn >> 16u) & 31u;
            access.writeback = true;
            access.writeback_value = base + (rm == 31u ? access.size : aarch64_x(state, rm, false));
        }
        access.valid = true;
        return access;
    }
    if ((insn & 0xBF9F0000u) == 0x0D000000u || (insn & 0xBF800000u) == 0x0D800000u) {
        // AdvSIMD LD1/ST1 (single structure) and LD1R, one register; the second form post-indexes.
        if (((insn >> 21u) & 1u) != 0u) return access;  // R=1: LD2/LD4 families
        access.vector = true;
        access.load = ((insn >> 22u) & 1u) != 0u;
        access.q = ((insn >> 30u) & 1u) != 0u;
        access.vector_bytes = access.q ? 16u : 8u;
        const unsigned opcode = (insn >> 13u) & 7u;
        const unsigned s = (insn >> 12u) & 1u;
        const unsigned size = (insn >> 10u) & 3u;
        const unsigned q = access.q ? 1u : 0u;
        switch (opcode) {
        case 0u:  // 8-bit lane
            access.size = 1u;
            access.lane_index = (q << 3u) | (s << 2u) | size;
            access.lane = true;
            break;
        case 2u:  // 16-bit lane
            if ((size & 1u) != 0u) return access;
            access.size = 2u;
            access.lane_index = (q << 2u) | (s << 1u) | (size >> 1u);
            access.lane = true;
            break;
        case 4u:  // 32- or 64-bit lane
            if (size == 0u) {
                access.size = 4u;
                access.lane_index = (q << 1u) | s;
            } else if (size == 1u && s == 0u) {
                access.size = 8u;
                access.lane_index = q;
            } else {
                return access;
            }
            access.lane = true;
            break;
        case 6u:  // LD1R
            if (!access.load || s != 0u) return access;
            access.size = 1u << size;
            access.replicate = true;
            break;
        default:
            return access;
        }
        access.address = base;
        if ((insn & 0x00800000u) != 0u) {
            const unsigned rm = (insn >> 16u) & 31u;
            access.writeback = true;
            access.writeback_value = base + (rm == 31u ? access.size : aarch64_x(state, rm, false));
        }
        access.valid = true;
        return access;
    }
    return access;
}

bool aarch64_guest_address(std::uint64_t host, std::uint32_t size, std::uint32_t& guest) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(g_guest_base);
    if (g_guest_base == nullptr || host < base || host - base + size > kGuestSpaceSize) return false;
    guest = static_cast<std::uint32_t>(host - base);
    return true;
}

// Moves one register's worth of data between the context and the guest image.
bool aarch64_transfer_register(
    _STRUCT_MCONTEXT64& context, const Aarch64Access& access, unsigned reg, std::uint32_t guest,
    const std::uint8_t* code) noexcept {
    alignas(16) std::byte image[16]{};
    if (access.load) {
        if (!fault_transfer(guest, access.size, false, image)) {
            note_unhandled(guest, code, "load not accepted");
            return false;
        }
        g_fault_reads.fetch_add(1u, std::memory_order_relaxed);
        if (access.vector) {
            auto* const vector = reinterpret_cast<std::uint8_t*>(&context.__ns.__v[reg]);
            std::uint8_t result[16]{};
            if (access.lane) {
                std::memcpy(result, vector, 16u);
                std::memcpy(result + access.lane_index * access.size, image, access.size);
                if (access.vector_bytes == 8u) std::memset(result + 8u, 0, 8u);
            } else if (access.replicate) {
                for (unsigned offset = 0; offset < access.vector_bytes; offset += access.size) {
                    std::memcpy(result + offset, image, access.size);
                }
            } else {
                std::memcpy(result, image, access.size);  // a scalar or whole-register load zeroes the rest
            }
            std::memcpy(vector, result, 16u);
            return true;
        }
        std::uint64_t value = 0;
        std::memcpy(&value, image, access.size);
        if (access.sign_extend) {
            value = static_cast<std::uint64_t>(aarch64_sign_extend(value, access.size * 8u));
            if (access.extend_to == 4u) value &= 0xFFFFFFFFu;
        }
        aarch64_set_x(context.__ss, reg, false, value);
        return true;
    }
    if (access.vector) {
        const auto* const vector = reinterpret_cast<const std::uint8_t*>(&context.__ns.__v[reg]);
        std::memcpy(image, vector + (access.lane ? access.lane_index * access.size : 0u), access.size);
    } else {
        const std::uint64_t value = aarch64_x(context.__ss, reg, false);
        std::memcpy(image, &value, access.size);
    }
    if (!fault_transfer(guest, access.size, true, image)) {
        note_unhandled(guest, code, "store not accepted");
        return false;
    }
    g_fault_writes.fetch_add(1u, std::memory_order_relaxed);
    return true;
}

struct sigaction g_previous_sigbus{};
struct sigaction g_previous_sigsegv{};

void chain_to_previous_handler(int signal_number) noexcept {
    // Restore the previous disposition; returning re-executes the faulting instruction, which now
    // reaches that handler (or the default action).
    (void)sigaction(signal_number, signal_number == SIGBUS ? &g_previous_sigbus : &g_previous_sigsegv, nullptr);
}

void fastmem_signal_handler(int signal_number, siginfo_t* info, void* raw_context) noexcept {
    auto* const ucontext = static_cast<ucontext_t*>(raw_context);
    if (ucontext == nullptr || ucontext->uc_mcontext == nullptr) {
        chain_to_previous_handler(signal_number);
        return;
    }
    _STRUCT_MCONTEXT64& context = *ucontext->uc_mcontext;
    std::uint32_t fault_guest = 0;
    const auto fault = reinterpret_cast<std::uint64_t>(info != nullptr ? info->si_addr : nullptr);
    std::uint64_t thread_id = 0;
    pthread_threadid_np(nullptr, &thread_id);
    if (!aarch64_guest_address(fault, 1u, fault_guest) ||
        static_cast<std::uint32_t>(thread_id) != static_cast<std::uint32_t>(g_fault_callbacks.thread_id)) {
        chain_to_previous_handler(signal_number);
        return;
    }
    const auto* code = reinterpret_cast<const std::uint8_t*>(context.__ss.__pc);
    std::uint32_t insn = 0;
    std::memcpy(&insn, code, sizeof(insn));
    const Aarch64Access access = aarch64_decode(insn, context.__ss);
    std::uint32_t guest = 0;
    const std::uint32_t span = access.pair ? access.size * 2u : access.size;
    if (!access.valid || !aarch64_guest_address(access.address, std::max(span, 1u), guest)) {
        note_unhandled(fault_guest, code, access.valid ? "effective address outside the guest view" : "unknown instruction form");
        chain_to_previous_handler(signal_number);
        return;
    }
    count_fault_page(guest);
    count_fault_site(static_cast<std::uintptr_t>(context.__ss.__pc), guest);
    if ((guest & 0xFFFF0000u) == 0xE0000000u) {
        g_fault_locked_cache.fetch_add(1u, std::memory_order_relaxed);
    } else if ((guest & 0xFF000000u) == 0xCC000000u || (guest & 0xFF000000u) == 0x0C000000u) {
        g_fault_mmio.fetch_add(1u, std::memory_order_relaxed);
    }
    if (!access.prefetch) {
        if (!aarch64_transfer_register(context, access, access.rt, guest, code) ||
            (access.pair && !aarch64_transfer_register(context, access, access.rt2, guest + access.size, code))) {
            chain_to_previous_handler(signal_number);
            return;
        }
    }
    if (access.writeback) {
        aarch64_set_x(context.__ss, access.rn, true, access.writeback_value);
    }
    context.__ss.__pc += 4u;
}

void install_fastmem_signal_handlers() {
    struct sigaction action{};
    action.sa_sigaction = &fastmem_signal_handler;
    // Runs on the faulting stack: device callbacks can be deep, and fiber stacks are large.
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, &g_previous_sigbus) != 0 ||
        sigaction(SIGSEGV, &action, &g_previous_sigsegv) != 0) {
        fail_posix("sigaction for fastmem");
    }
}
