// SPDX-License-Identifier: GPL-3.0-only
//
// Pipeline shader programs: MSL translation (msl_translate.cpp) compiled by Metal, cached by the
// shader modules, root signature layout and input layout that produced them.

#include "metal12.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace galaxy::metal12 {

namespace {

std::mutex g_program_mutex;
std::unordered_map<std::uint64_t, MetalProgram> g_programs;

bool dump_msl_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("NEBULA_METAL_DUMP_MSL");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return enabled;
}

id<MTLFunction> compile_function(id<MTLDevice> device, const std::string& source, const std::string& entry,
    std::string& error) {
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    options.languageVersion = MTLLanguageVersion3_0;
    // Direct3D compilers keep IEEE behavior for NaN, infinity and signed zero.
    if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
    } else {
        options.fastMathEnabled = NO;
    }
    NSError* ns_error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                  options:options
                                                    error:&ns_error];
    if (library == nil) {
        error = ns_error != nil ? ns_error.localizedDescription.UTF8String : "Metal compilation failed";
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:entry.c_str()]];
    if (function == nil) error = "entry point " + entry + " is missing from the compiled library";
    return function;
}

}  // namespace

MetalProgram build_program(Device& device, const ShaderModule& vertex, const ShaderModule* pixel,
    const RootSignature& root, const std::vector<InputElement>& inputs, bool dual_source_blend, bool points,
    GeometryExpansion expansion) {
    std::uint64_t key = fnv1a64(&vertex.hash, sizeof(vertex.hash));
    const std::uint64_t pixel_hash = pixel != nullptr ? pixel->hash : 0u;
    key = fnv1a64(&pixel_hash, sizeof(pixel_hash), key);
    key = fnv1a64(&root.hash, sizeof(root.hash), key);
    for (const InputElement& input : inputs) key = fnv1a64(input.semantic.data(), input.semantic.size() + 1u, key);
    const std::uint8_t flags = (dual_source_blend ? 1u : 0u) | (points ? 2u : 0u) |
        static_cast<std::uint8_t>(static_cast<unsigned>(expansion) << 2u);
    key = fnv1a64(&flags, 1u, key);
    {
        std::lock_guard lock(g_program_mutex);
        if (const auto it = g_programs.find(key); it != g_programs.end()) return it->second;
    }

    MslRequest request;
    request.vertex = &vertex;
    request.pixel = pixel;
    request.bindings = &root.bindings;
    for (const InputElement& input : inputs) request.input_semantics.push_back(input.semantic);
    request.dual_source_blend = dual_source_blend;
    request.points = points;
    request.expansion = expansion;
    if (expansion != GeometryExpansion::None) {
        for (const InputElement& input : inputs) {
            request.vertex_fetches.push_back({input.semantic, msl_fetch_expression(input.format), input.offset});
        }
        // The GX line/point geometry shaders read their constants from b0 (space 0).
        const auto constants = root.bindings.find({'b', 0u, 0u});
        request.expansion_constants_buffer = constants != root.bindings.end() ? static_cast<int>(constants->second) : -1;
        if (request.expansion_constants_buffer < 0) {
            MetalProgram failed;
            failed.error = "geometry expansion: the root signature has no b0";
            return failed;
        }
    }
    const MslProgram msl = translate_to_msl(request);

    MetalProgram program;
    program.attribute_locations = msl.attribute_locations;
    if (!msl.error.empty()) {
        program.error = msl.error;
        return program;
    }
    if (dump_msl_enabled()) {
        char name[64];
        std::snprintf(name, sizeof(name), "nebula_%016llx.metal", static_cast<unsigned long long>(key));
        std::ofstream(name) << "// vertex\n" << msl.vertex_source << "\n// fragment\n" << msl.fragment_source;
    }
    std::string error;
    program.vertex = compile_function(device.mtl, msl.vertex_source, msl.vertex_entry, error);
    if (program.vertex != nil && !msl.fragment_source.empty()) {
        program.fragment = compile_function(device.mtl, msl.fragment_source, msl.fragment_entry, error);
        if (program.fragment == nil) program.vertex = nil;
    }
    if (program.vertex == nil) {
        program.error = error;
        return program;
    }
    std::lock_guard lock(g_program_mutex);
    g_programs.emplace(key, program);
    return program;
}

}  // namespace galaxy::metal12
