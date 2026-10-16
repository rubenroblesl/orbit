// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Shader blobs and their translation to the Metal Shading Language. Plain C++: glslang and
// SPIRV-Cross stay out of the Objective-C++ translation units.
//
// D3DCompile produces a blob holding the stage, the entry point, the SPIR-V module and the HLSL
// semantic of every stage input and output location (read from the module before optimization,
// which removes unused interface variables). Pipelines map vertex attributes and the varyings
// between stages by those semantics, as Direct3D does.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace galaxy::metal12 {

enum class ShaderStage : std::uint32_t { Vertex = 0, Pixel = 1, Geometry = 2, Compute = 3 };

struct IoSemantic {
    bool output = false;
    unsigned location = 0;
    std::string semantic;  // upper-case name with its index appended ("TEXCOORD3", "COLOR0")
};

struct ShaderModule {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    std::vector<std::uint32_t> spirv;
    std::vector<IoSemantic> io;
    std::uint64_t hash = 0;
};

[[nodiscard]] std::vector<std::uint8_t> serialize_shader_blob(const ShaderModule& module);
[[nodiscard]] bool parse_shader_blob(const void* data, std::size_t size, ShaderModule& out);

[[nodiscard]] std::uint64_t fnv1a64(const void* data, std::size_t size,
    std::uint64_t hash = 14695981039346656037ull) noexcept;

// Where a shader resource (by register class, space, register) lives in Metal.
struct BindingKey {
    char kind;           // 'b' buffer (CBV), 't' texture SRV, 'B' buffer SRV, 's' sampler, 'u' UAV
    unsigned space;
    unsigned shader_register;
    bool operator<(const BindingKey& other) const {
        if (kind != other.kind) return kind < other.kind;
        if (space != other.space) return space < other.space;
        return shader_register < other.shader_register;
    }
};

// Metal has no geometry stage. The GX renderer's two geometry shaders expand each line or point
// into a quad of the BP line width / point size (shader_gen.cpp kLineGeometryShader and
// kPointGeometryShader). Pipelines with one of them draw six vertices per primitive instead: an
// expansion vertex function fetches the primitive's vertices from the bound buffers, runs the
// pipeline's vertex shader on each and emits the quad corner with the same arithmetic.
enum class GeometryExpansion : std::uint32_t { None = 0, Lines = 1, Points = 2 };

// The expansion kind of a geometry shader, from its input primitive.
[[nodiscard]] GeometryExpansion geometry_expansion_of(const ShaderModule& geometry);

// Buffers an expansion draw binds besides the root signature's.
inline constexpr unsigned kExpansionVertexBuffer = 19;
inline constexpr unsigned kExpansionIndexBuffer = 20;
inline constexpr unsigned kExpansionParamsBuffer = 21;
inline constexpr unsigned kFirstReservedBuffer = 19;   // root signature buffers stay below
struct ExpansionParams {
    std::uint32_t first = 0;         // first index (indexed) or first vertex
    std::int32_t base_vertex = 0;
    std::uint32_t indexed = 0;
    std::uint32_t index32 = 0;
    std::uint32_t stride = 0;        // vertex buffer stride in bytes
    std::uint32_t reserved = 0;
};

// How the expansion function reads one input element: an MSL expression of the element's value
// where `P` is a `const device uchar*` at the element.
struct VertexFetch {
    std::string semantic;
    std::string load;
    unsigned offset = 0;
};

struct MslRequest {
    const ShaderModule* vertex = nullptr;
    const ShaderModule* pixel = nullptr;               // optional
    const std::map<BindingKey, unsigned>* bindings = nullptr;
    std::vector<std::string> input_semantics;          // one per input layout element
    bool dual_source_blend = false;
    bool points = false;                               // point topology: the vertex stage writes a point size
    GeometryExpansion expansion = GeometryExpansion::None;
    std::vector<VertexFetch> vertex_fetches;           // per input element, for an expansion
    int expansion_constants_buffer = -1;               // Metal index of the geometry shader's b0
};

struct MslProgram {
    std::string vertex_source;
    std::string vertex_entry;
    std::string fragment_source;
    std::string fragment_entry;
    std::vector<int> attribute_locations;               // per input element; -1 when the shader ignores it
    std::string error;
};

[[nodiscard]] MslProgram translate_to_msl(const MslRequest& request);

}  // namespace galaxy::metal12
