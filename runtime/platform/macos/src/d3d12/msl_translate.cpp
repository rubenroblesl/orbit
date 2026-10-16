// SPDX-License-Identifier: GPL-3.0-only
//
// SPIR-V to the Metal Shading Language with SPIRV-Cross, bound to a root signature's Metal
// indices and to a pipeline's input layout.

#include "shader_translate.h"

#if __has_include(<spirv_cross/spirv_msl.hpp>)
#include <spirv_cross/spirv_msl.hpp>  // installed layout
#else
#include <spirv_msl.hpp>              // source tree (static build)
#endif

#include <mutex>

namespace galaxy::metal12 {

namespace {

// Varyings meet by semantic: both stages place a semantic at the same location.
unsigned varying_location(const std::string& semantic) {
    std::size_t digits = semantic.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(semantic[digits - 1]))) --digits;
    const std::string name = semantic.substr(0, digits);
    const unsigned index = digits < semantic.size() ? static_cast<unsigned>(std::stoul(semantic.substr(digits))) : 0u;
    if (name == "TEXCOORD" && index < 32u) return index;
    if (name == "COLOR" && index < 8u) return 32u + index;
    static std::mutex mutex;
    static std::map<std::string, unsigned> others;
    std::lock_guard lock(mutex);
    const auto [it, inserted] = others.emplace(semantic, 40u + static_cast<unsigned>(others.size()));
    return it->second;
}

const std::string* semantic_at(const ShaderModule& module, bool output, unsigned location) {
    for (const IoSemantic& io : module.io) {
        if (io.output == output && io.location == location) return &io.semantic;
    }
    return nullptr;
}

struct StageResult {
    std::string source;
    std::string entry;
};

// --- Geometry expansion (see GeometryExpansion in shader_translate.h) ---------------------------

struct EntryParameter {
    std::string declaration;  // without attributes
    std::string attribute;    // inside [[ ]], empty when none
    std::string name;
};

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\n");
    return text.substr(first, last - first + 1u);
}

std::vector<EntryParameter> split_parameters(const std::string& list) {
    std::vector<EntryParameter> parameters;
    int depth = 0;
    std::string current;
    auto flush = [&] {
        std::string text = trim(current);
        current.clear();
        if (text.empty()) return;
        EntryParameter parameter;
        const auto open = text.find("[[");
        if (open != std::string::npos) {
            const auto close = text.find("]]", open);
            parameter.attribute = trim(text.substr(open + 2u, close - open - 2u));
            text = trim(text.substr(0, open));
        }
        parameter.declaration = text;
        std::size_t end = text.size();
        std::size_t begin = end;
        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(text[begin - 1])) || text[begin - 1] == '_')) --begin;
        parameter.name = text.substr(begin, end - begin);
        parameters.push_back(std::move(parameter));
    };
    for (const char c : list) {
        if (c == '(' || c == '<' || c == '[') ++depth;
        if (c == ')' || c == '>' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            flush();
            continue;
        }
        current.push_back(c);
    }
    flush();
    return parameters;
}

// Turns the vertex entry point into a callable function and appends the expansion entry point
// `nbl_expand`, which calls it for the primitive's vertices.
bool append_expansion(std::string& source, const std::string& entry, const MslRequest& request,
    const std::vector<std::pair<unsigned, std::string>>& inputs, const std::vector<unsigned>& texcoords,
    std::string& error) {
    const std::string out_type = entry + "_out";
    const std::string in_type = entry + "_in";
    const std::string header = "vertex " + out_type + " " + entry + "(";
    const auto start = source.find(header);
    if (start == std::string::npos) {
        error = "expansion: vertex entry point not found";
        return false;
    }
    std::size_t close = start + header.size();
    for (int depth = 1; close < source.size(); ++close) {
        if (source[close] == '(') ++depth;
        if (source[close] == ')' && --depth == 0) break;
    }
    const std::vector<EntryParameter> parameters =
        split_parameters(source.substr(start + header.size(), close - start - header.size()));

    std::string plain;
    std::string forwarded;
    std::string calls;
    std::string constants;
    for (const EntryParameter& parameter : parameters) {
        if (!plain.empty()) plain += ", ";
        plain += parameter.declaration;
        if (!calls.empty()) calls += ", ";
        const std::string& attribute = parameter.attribute;
        if (attribute == "stage_in") {
            calls += "nbl_fetch(nbl_vb, nbl_p, VERTEX)";
        } else if (attribute == "vertex_id") {
            calls += "VERTEX";
        } else if (attribute == "instance_id") {
            calls += "nbl_instance";
        } else if (attribute == "base_vertex") {
            calls += "uint(nbl_p.base_vertex)";
        } else if (attribute == "base_instance") {
            calls += "0u";
        } else {
            calls += parameter.name;
            forwarded += ", " + parameter.declaration + (attribute.empty() ? "" : " [[" + attribute + "]]");
            if (attribute == "buffer(" + std::to_string(request.expansion_constants_buffer) + ")") {
                constants = parameter.declaration.find('*') != std::string::npos
                    ? "reinterpret_cast<constant float4*>(" + parameter.name + ")"
                    : "reinterpret_cast<constant float4*>(&" + parameter.name + ")";
            }
        }
    }
    if (constants.empty()) {
        forwarded += ", constant float4* nbl_lp [[buffer(" + std::to_string(request.expansion_constants_buffer) + ")]]";
        constants = "nbl_lp";
    }
    source.replace(start, close + 1u - start, "static inline " + out_type + " nbl_vs(" + plain + ")");

    auto call = [&](const std::string& vertex) {
        std::string text = calls;
        for (std::size_t at; (at = text.find("VERTEX")) != std::string::npos;) text.replace(at, 6u, vertex);
        return "nbl_vs(" + text + ")";
    };

    std::string code;
    code += "\nstruct nbl_expand_params { uint first; int base_vertex; uint indexed; uint index32; uint stride; uint reserved; };\n";
    code += "static inline uint nbl_index(const device uchar* ib, constant nbl_expand_params& p, uint n) {\n"
            "    const uint at = p.first + n;\n"
            "    if (p.indexed == 0u) return at;\n"
            "    const uint value = p.index32 != 0u\n"
            "        ? (uint(ib[at * 4u]) | (uint(ib[at * 4u + 1u]) << 8u) | (uint(ib[at * 4u + 2u]) << 16u) |\n"
            "           (uint(ib[at * 4u + 3u]) << 24u))\n"
            "        : (uint(ib[at * 2u]) | (uint(ib[at * 2u + 1u]) << 8u));\n"
            "    return uint(int(value) + p.base_vertex);\n"
            "}\n";
    code += "static inline " + in_type + " nbl_fetch(const device uchar* vb, constant nbl_expand_params& p, uint element) {\n"
            "    const device uchar* base = vb + element * p.stride;\n"
            "    " + in_type + " in = {};\n";
    for (const auto& [location, semantic] : inputs) {
        const VertexFetch* fetch = nullptr;
        for (const VertexFetch& candidate : request.vertex_fetches) {
            if (candidate.semantic == semantic) fetch = &candidate;
        }
        if (fetch == nullptr || fetch->load.empty()) {
            error = "expansion: no fetch for vertex input " + semantic;
            return false;
        }
        std::string load = fetch->load;
        const std::string pointer = "(base + " + std::to_string(fetch->offset) + "u)";
        for (std::size_t at; (at = load.find('P')) != std::string::npos;) load.replace(at, 1u, pointer);
        const std::string member = "in.nbl_in_" + std::to_string(location);
        code += "    " + member + " = decltype(" + member + ")(" + load + ");\n";
    }
    code += "    return in;\n}\n";
    code += "constant uint nbl_corners[6] = {0u, 1u, 2u, 2u, 1u, 3u};\n";
    code += "vertex " + out_type + " nbl_expand(uint nbl_vertex [[vertex_id]], uint nbl_instance [[instance_id]]" + forwarded +
            ", const device uchar* nbl_vb [[buffer(" + std::to_string(kExpansionVertexBuffer) + ")]]"
            ", const device uchar* nbl_ib [[buffer(" + std::to_string(kExpansionIndexBuffer) + ")]]"
            ", constant nbl_expand_params& nbl_p [[buffer(" + std::to_string(kExpansionParamsBuffer) + ")]])\n{\n";
    code += "    const uint primitive = nbl_vertex / 6u;\n"
            "    const uint corner = nbl_corners[nbl_vertex % 6u];\n"
            "    constant float4* nbl_constants = " + constants + ";\n"
            "    const float4 raster = nbl_constants[65];\n"
            "    const uint4 offsets = as_type<uint4>(nbl_constants[66]);\n";
    if (request.expansion == GeometryExpansion::Lines) {
        code += "    const uint first = nbl_index(nbl_ib, nbl_p, primitive * 2u);\n"
                "    const uint second = nbl_index(nbl_ib, nbl_p, primitive * 2u + 1u);\n"
                "    const " + out_type + " start = " + call("first") + ";\n"
                "    const " + out_type + " end = " + call("second") + ";\n"
                "    const float2 delta = abs(end.gl_Position.xy / end.gl_Position.w - start.gl_Position.xy / start.gl_Position.w);\n"
                "    const bool tall = raster.y * delta.y > raster.x * delta.x;\n"
                "    const float2 offset = tall ? float2(raster.z / raster.x, 0.0) : float2(0.0, -raster.z / raster.y);\n"
                "    " + out_type + " out = corner < 2u ? start : end;\n"
                "    const bool plus = (corner & 1u) != 0u;\n"
                "    out.gl_Position.xy += (plus ? 1.0 : -1.0) * offset * out.gl_Position.w;\n"
                "    if (plus && offsets.z != 0u) {\n"
                "        const float tex_offset = 1.0 / float(offsets.z);\n";
        for (const unsigned i : texcoords) {
            code += "        if ((offsets.x & " + std::to_string(1u << i) + "u) != 0u) out.nbl_out_TEXCOORD" +
                std::to_string(i) + ".x += tex_offset;\n";
        }
        code += "    }\n    return out;\n}\n";
    } else {
        code += "    const uint vertex_index = nbl_index(nbl_ib, nbl_p, primitive);\n"
                "    " + out_type + " out = " + call("vertex_index") + ";\n"
                "    const bool right = (corner & 1u) != 0u;\n"
                "    const bool bottom = (corner & 2u) != 0u;\n"
                "    const float2 signs = float2(right ? 1.0 : -1.0, bottom ? -1.0 : 1.0);\n"
                "    out.gl_Position.xy += signs * raster.ww / raster.xy * out.gl_Position.w;\n"
                "    if (offsets.w != 0u) {\n"
                "        const float tex_offset = 1.0 / float(offsets.w);\n"
                "        const float2 shift = float2(right ? tex_offset : 0.0, bottom ? tex_offset : 0.0);\n";
        for (const unsigned i : texcoords) {
            code += "        if ((offsets.y & " + std::to_string(1u << i) + "u) != 0u) out.nbl_out_TEXCOORD" +
                std::to_string(i) + ".xy += shift;\n";
        }
        code += "    }\n    return out;\n}\n";
    }
    source += code;
    return true;
}

GeometryExpansion expansion_from_spirv(const std::vector<std::uint32_t>& spirv) {
    constexpr std::uint32_t kOpExecutionMode = 16;
    constexpr std::uint32_t kModeInputPoints = 19;
    constexpr std::uint32_t kModeInputLines = 20;
    for (std::size_t i = 5; i < spirv.size();) {
        const std::uint32_t count = spirv[i] >> 16u;
        if (count == 0u || i + count > spirv.size()) break;
        if ((spirv[i] & 0xFFFFu) == kOpExecutionMode && count >= 3u) {
            if (spirv[i + 2] == kModeInputLines) return GeometryExpansion::Lines;
            if (spirv[i + 2] == kModeInputPoints) return GeometryExpansion::Points;
        }
        i += count;
    }
    return GeometryExpansion::None;
}

bool translate_stage(const ShaderModule& module, spv::ExecutionModel model, const MslRequest& request,
    std::vector<int>* attribute_locations, StageResult& result, std::string& error) {
    spirv_cross::CompilerMSL msl(module.spirv);
    std::string entry_name;
    for (const spirv_cross::EntryPoint& entry : msl.get_entry_points_and_stages()) {
        if (entry.execution_model == model) {
            entry_name = entry.name;
            break;
        }
    }
    if (entry_name.empty()) {
        error = "the module has no entry point for this stage";
        return false;
    }
    msl.set_entry_point(entry_name, model);

    spirv_cross::CompilerMSL::Options options;
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.set_msl_version(3, 0);
    options.pad_fragment_output_components = true;
    options.enable_point_size_default = request.points && model == spv::ExecutionModelVertex;
    options.buffer_size_buffer_index = 22;
    msl.set_msl_options(options);

    const spirv_cross::ShaderResources resources = msl.get_shader_resources();
    const auto& bindings = *request.bindings;
    auto bind = [&](const spirv_cross::Resource& resource, bool buffer, bool sampler) -> bool {
        const unsigned set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
        const unsigned binding = msl.get_decoration(resource.id, spv::DecorationBinding);
        char kind = binding >= 3000u ? 'u' : binding >= 2000u ? 's' : binding >= 1000u ? 't' : 'b';
        const unsigned shader_register = binding % 1000u;
        auto it = bindings.end();
        if (kind == 't' && buffer) it = bindings.find({'B', set, shader_register});
        if (it == bindings.end()) it = bindings.find({kind, set, shader_register});
        if (it == bindings.end()) {
            error = std::string("register ") + kind + std::to_string(shader_register) + ", space" + std::to_string(set) +
                " is not in the root signature";
            return false;
        }
        spirv_cross::MSLResourceBinding entry;
        entry.stage = model;
        entry.desc_set = set;
        entry.binding = binding;
        if (buffer) entry.msl_buffer = it->second;
        else if (sampler) entry.msl_sampler = it->second;
        else entry.msl_texture = it->second;
        msl.add_msl_resource_binding(entry);
        return true;
    };
    for (const auto& resource : resources.uniform_buffers) if (!bind(resource, true, false)) return false;
    for (const auto& resource : resources.storage_buffers) if (!bind(resource, true, false)) return false;
    for (const auto& resource : resources.separate_images) if (!bind(resource, false, false)) return false;
    for (const auto& resource : resources.storage_images) if (!bind(resource, false, false)) return false;
    for (const auto& resource : resources.separate_samplers) if (!bind(resource, false, true)) return false;
    for (const auto& resource : resources.sampled_images) {
        // Combined image-samplers do not come out of HLSL; bind both halves to the register.
        if (!bind(resource, false, false)) return false;
    }

    const bool expand = model == spv::ExecutionModelVertex && request.expansion != GeometryExpansion::None;
    std::vector<std::pair<unsigned, std::string>> expansion_inputs;
    std::vector<unsigned> expansion_texcoords;
    for (const auto& input : resources.stage_inputs) {
        if (!msl.has_decoration(input.id, spv::DecorationLocation)) continue;
        const unsigned location = msl.get_decoration(input.id, spv::DecorationLocation);
        const std::string* semantic = semantic_at(module, false, location);
        if (semantic == nullptr) {
            error = "stage input at location " + std::to_string(location) + " has no semantic";
            return false;
        }
        if (expand) {
            msl.set_name(input.id, "nbl_in_" + std::to_string(location));
            expansion_inputs.emplace_back(location, *semantic);
        }
        if (model == spv::ExecutionModelVertex) {
            bool found = false;
            for (std::size_t i = 0; i < request.input_semantics.size(); ++i) {
                if (request.input_semantics[i] == *semantic) {
                    (*attribute_locations)[i] = static_cast<int>(location);
                    found = true;
                }
            }
            if (!found) {
                error = "vertex input " + *semantic + " is not in the input layout";
                return false;
            }
        } else {
            msl.set_decoration(input.id, spv::DecorationLocation, varying_location(*semantic));
        }
    }
    for (const auto& output : resources.stage_outputs) {
        if (!msl.has_decoration(output.id, spv::DecorationLocation)) continue;
        const unsigned location = msl.get_decoration(output.id, spv::DecorationLocation);
        if (model == spv::ExecutionModelVertex) {
            const std::string* semantic = semantic_at(module, true, location);
            if (semantic != nullptr) msl.set_decoration(output.id, spv::DecorationLocation, varying_location(*semantic));
            if (expand && semantic != nullptr) {
                msl.set_name(output.id, "nbl_out_" + *semantic);
                for (unsigned i = 0; i < 8u; ++i) {
                    if (*semantic == "TEXCOORD" + std::to_string(i)) expansion_texcoords.push_back(i);
                }
            }
        } else if (model == spv::ExecutionModelFragment && request.dual_source_blend && location == 1u) {
            msl.set_decoration(output.id, spv::DecorationLocation, 0u);
            msl.set_decoration(output.id, spv::DecorationIndex, 1u);
        }
    }

    result.source = msl.compile();
    if (msl.needs_buffer_size_buffer()) {
        error = "buffer size queries (GetDimensions on buffers) are not supported";
        return false;
    }
    result.entry = msl.get_cleansed_entry_point_name(entry_name, model);
    if (expand) {
        if (!append_expansion(result.source, result.entry, request, expansion_inputs, expansion_texcoords, error)) {
            return false;
        }
        result.entry = "nbl_expand";
    }
    return true;
}

}  // namespace

GeometryExpansion geometry_expansion_of(const ShaderModule& geometry) {
    return geometry.stage == ShaderStage::Geometry ? expansion_from_spirv(geometry.spirv) : GeometryExpansion::None;
}

MslProgram translate_to_msl(const MslRequest& request) {
    MslProgram program;
    program.attribute_locations.assign(request.input_semantics.size(), -1);
    try {
        StageResult vertex;
        if (!translate_stage(*request.vertex, spv::ExecutionModelVertex, request, &program.attribute_locations, vertex,
                program.error)) {
            program.error = "vertex shader: " + program.error;
            return program;
        }
        program.vertex_source = std::move(vertex.source);
        program.vertex_entry = std::move(vertex.entry);
        if (request.pixel != nullptr && !request.pixel->spirv.empty()) {
            StageResult fragment;
            if (!translate_stage(*request.pixel, spv::ExecutionModelFragment, request, nullptr, fragment, program.error)) {
                program.error = "pixel shader: " + program.error;
                return program;
            }
            program.fragment_source = std::move(fragment.source);
            program.fragment_entry = std::move(fragment.entry);
        }
    } catch (const std::exception& exception) {
        program.error = std::string("SPIRV-Cross: ") + exception.what();
    }
    return program;
}

}  // namespace galaxy::metal12
