// SPDX-License-Identifier: GPL-3.0-only
//
// The HLSL compiler (d3dcompiler_47) on macOS: glslang's HLSL front end compiles shader model 5.x
// to SPIR-V, SPIRV-Tools legalizes it, and the result is wrapped with the interface semantics in
// the blob format of shader_translate.h. D3DReflect reports the stage of such a blob.

#include <windows.h>
#include <d3dcompiler.h>
#include <d3d12shader.h>

#include "shader_translate.h"

// The HLSL entry points of glslang are declared only with ENABLE_HLSL (Homebrew builds it in).
#define ENABLE_HLSL 1
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#if __has_include(<glslang/SPIRV/GlslangToSpv.h>)
#include <glslang/SPIRV/GlslangToSpv.h>  // installed layout
#else
#include <SPIRV/GlslangToSpv.h>          // source tree (static build)
#endif
#include <spirv-tools/optimizer.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace galaxy::metal12 {

std::uint64_t fnv1a64(const void* data, std::size_t size, std::uint64_t hash) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

namespace {

constexpr std::uint32_t kBlobMagic = 0x4853424Eu;  // "NBSH"
constexpr std::uint32_t kBlobVersion = 1;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + 4);
}

void put_string(std::vector<std::uint8_t>& out, const std::string& text) {
    put_u32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    while (out.size() % 4u != 0u) out.push_back(0);
}

struct BlobReader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (offset + 4 > size) {
            ok = false;
            return 0;
        }
        std::uint32_t value;
        std::memcpy(&value, data + offset, 4);
        offset += 4;
        return value;
    }
    std::string string() {
        const std::uint32_t length = u32();
        if (!ok || offset + length > size) {
            ok = false;
            return {};
        }
        std::string text(reinterpret_cast<const char*>(data + offset), length);
        offset = (offset + length + 3u) & ~std::size_t{3u};
        return text;
    }
};

}  // namespace

std::vector<std::uint8_t> serialize_shader_blob(const ShaderModule& module) {
    std::vector<std::uint8_t> out;
    put_u32(out, kBlobMagic);
    put_u32(out, kBlobVersion);
    put_u32(out, static_cast<std::uint32_t>(module.stage));
    put_string(out, module.entry);
    put_u32(out, static_cast<std::uint32_t>(module.io.size()));
    for (const IoSemantic& io : module.io) {
        put_u32(out, io.output ? 1u : 0u);
        put_u32(out, io.location);
        put_string(out, io.semantic);
    }
    put_u32(out, static_cast<std::uint32_t>(module.spirv.size()));
    const auto* words = reinterpret_cast<const std::uint8_t*>(module.spirv.data());
    out.insert(out.end(), words, words + module.spirv.size() * 4u);
    return out;
}

bool parse_shader_blob(const void* data, std::size_t size, ShaderModule& out) {
    BlobReader reader{static_cast<const std::uint8_t*>(data), size};
    if (data == nullptr || reader.u32() != kBlobMagic || reader.u32() != kBlobVersion) return false;
    out.stage = static_cast<ShaderStage>(reader.u32());
    out.entry = reader.string();
    const std::uint32_t io_count = reader.u32();
    out.io.clear();
    for (std::uint32_t i = 0; i < io_count && reader.ok; ++i) {
        IoSemantic io;
        io.output = reader.u32() != 0u;
        io.location = reader.u32();
        io.semantic = reader.string();
        out.io.push_back(std::move(io));
    }
    const std::uint32_t words = reader.u32();
    if (!reader.ok || reader.offset + std::size_t{words} * 4u > size) return false;
    out.spirv.resize(words);
    std::memcpy(out.spirv.data(), reader.data + reader.offset, std::size_t{words} * 4u);
    out.hash = fnv1a64(data, size);
    return true;
}

}  // namespace galaxy::metal12

namespace {

using galaxy::metal12::IoSemantic;
using galaxy::metal12::ShaderModule;
using galaxy::metal12::ShaderStage;

class BytesBlob final : public ID3DBlob {
public:
    explicit BytesBlob(std::size_t size) : bytes_(size) {}
    explicit BytesBlob(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
            *object = static_cast<ID3DBlob*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes_.size(); }

private:
    std::vector<std::uint8_t> bytes_;
    std::atomic<ULONG> references_{1};
};

ID3DBlob* text_blob(const std::string& text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return new BytesBlob(std::move(bytes));
}

// Includes through an ID3DInclude handler, or the standard file include (relative to the
// including file, then to the source name).
class Includer final : public glslang::TShader::Includer {
public:
    Includer(ID3DInclude* handler, std::string source_name) : handler_(handler), source_name_(std::move(source_name)) {}

    IncludeResult* includeLocal(const char* name, const char* includer, std::size_t) override {
        return open(D3D_INCLUDE_LOCAL, name, includer);
    }
    IncludeResult* includeSystem(const char* name, const char* includer, std::size_t) override {
        return open(D3D_INCLUDE_SYSTEM, name, includer);
    }
    void releaseInclude(IncludeResult* result) override {
        if (result == nullptr) return;
        auto* data = static_cast<IncludeData*>(result->userData);
        if (data->handler_data != nullptr) handler_->Close(data->handler_data);
        delete data;
        delete result;
    }

private:
    struct IncludeData {
        std::string text;
        LPCVOID handler_data = nullptr;
    };

    IncludeResult* open(D3D_INCLUDE_TYPE type, const char* name, const char* includer) {
        if (handler_ == nullptr) return nullptr;
        if (handler_ == D3D_COMPILE_STANDARD_FILE_INCLUDE) {
            namespace fs = std::filesystem;
            std::vector<fs::path> candidates;
            if (includer != nullptr && *includer != '\0') candidates.push_back(fs::path(includer).parent_path() / name);
            if (!source_name_.empty()) candidates.push_back(fs::path(source_name_).parent_path() / name);
            candidates.emplace_back(name);
            for (const fs::path& path : candidates) {
                std::ifstream in(path, std::ios::binary);
                if (!in) continue;
                auto* data = new IncludeData();
                std::ostringstream text;
                text << in.rdbuf();
                data->text = text.str();
                return new IncludeResult(path.string(), data->text.data(), data->text.size(), data);
            }
            return nullptr;
        }
        LPCVOID bytes = nullptr;
        UINT size = 0;
        if (FAILED(handler_->Open(type, name, nullptr, &bytes, &size))) return nullptr;
        auto* data = new IncludeData();
        data->handler_data = bytes;
        return new IncludeResult(name, static_cast<const char*>(bytes), size, data);
    }

    ID3DInclude* handler_;
    std::string source_name_;
};

// Semantics of the interface variables of an unoptimized module. An array declared with one
// semantic ("float2 uv[8] : TEXCOORD") is split into one variable per element, all carrying that
// semantic in location order; element k takes the semantic index plus k.
std::vector<IoSemantic> interface_semantics(const std::vector<std::uint32_t>& spirv) {
    constexpr std::uint32_t kOpVariable = 59;
    constexpr std::uint32_t kOpDecorate = 71;
    constexpr std::uint32_t kOpDecorateString = 5632;
    constexpr std::uint32_t kDecorationLocation = 30;
    constexpr std::uint32_t kDecorationUserSemantic = 5635;
    constexpr std::uint32_t kStorageInput = 1;
    constexpr std::uint32_t kStorageOutput = 3;
    std::map<std::uint32_t, unsigned> locations;
    std::map<std::uint32_t, std::string> semantics;
    std::map<std::uint32_t, bool> storage;  // id -> output
    for (std::size_t i = 5; i < spirv.size();) {
        const std::uint32_t word = spirv[i];
        const std::uint32_t count = word >> 16u;
        const std::uint32_t opcode = word & 0xFFFFu;
        if (count == 0u || i + count > spirv.size()) break;
        if (opcode == kOpDecorate && count >= 4 && spirv[i + 2] == kDecorationLocation) {
            locations[spirv[i + 1]] = spirv[i + 3];
        } else if (opcode == kOpDecorateString && count >= 4 && spirv[i + 2] == kDecorationUserSemantic) {
            const char* text = reinterpret_cast<const char*>(&spirv[i + 3]);
            semantics[spirv[i + 1]] = std::string(text, strnlen(text, (count - 3u) * 4u));
        } else if (opcode == kOpVariable && count >= 4 &&
                   (spirv[i + 3] == kStorageInput || spirv[i + 3] == kStorageOutput)) {
            storage[spirv[i + 2]] = spirv[i + 3] == kStorageOutput;
        }
        i += count;
    }
    std::vector<IoSemantic> result;
    for (const bool output : {false, true}) {
        std::map<unsigned, std::string> by_location;
        for (const auto& [id, is_output] : storage) {
            if (is_output != output) continue;
            const auto location = locations.find(id);
            const auto semantic = semantics.find(id);
            if (location == locations.end() || semantic == semantics.end()) continue;
            by_location[location->second] = semantic->second;
        }
        std::map<std::string, unsigned> occurrences;
        for (const auto& [location, raw] : by_location) {
            std::string name = raw;
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            std::size_t digits = name.size();
            while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1]))) --digits;
            const unsigned base = digits < name.size() ? static_cast<unsigned>(std::stoul(name.substr(digits))) : 0u;
            const unsigned index = base + occurrences[name]++;
            result.push_back(IoSemantic{output, location, name.substr(0, digits) + std::to_string(index)});
        }
    }
    return result;
}

std::once_flag g_glslang_once;

}  // namespace

HRESULT D3DCompile(LPCVOID source, SIZE_T source_size, LPCSTR source_name, const D3D_SHADER_MACRO* defines,
    ID3DInclude* include, LPCSTR entry_point, LPCSTR target, UINT flags1, UINT, ID3DBlob** code,
    ID3DBlob** errors) noexcept {
    if (code != nullptr) *code = nullptr;
    if (errors != nullptr) *errors = nullptr;
    if (source == nullptr || target == nullptr || code == nullptr) return E_INVALIDARG;
    try {
        std::call_once(g_glslang_once, [] { glslang::InitializeProcess(); });

        ShaderModule module;
        EShLanguage language;
        switch (target[0]) {
        case 'v': language = EShLangVertex; module.stage = ShaderStage::Vertex; break;
        case 'p': language = EShLangFragment; module.stage = ShaderStage::Pixel; break;
        case 'g': language = EShLangGeometry; module.stage = ShaderStage::Geometry; break;
        case 'c': language = EShLangCompute; module.stage = ShaderStage::Compute; break;
        default:
            if (errors != nullptr) *errors = text_blob(std::string("unsupported target ") + target);
            return E_INVALIDARG;
        }
        module.entry = entry_point != nullptr ? entry_point : "main";

        std::string preamble;
        if ((flags1 & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR) != 0u) preamble += "#pragma pack_matrix(row_major)\n";
        for (const D3D_SHADER_MACRO* define = defines; define != nullptr && define->Name != nullptr; ++define) {
            preamble += std::string("#define ") + define->Name + " " + (define->Definition != nullptr ? define->Definition : "1") + "\n";
        }

        glslang::TShader shader(language);
        const char* text = static_cast<const char*>(source);
        const int length = static_cast<int>(source_size);
        const char* name = source_name != nullptr ? source_name : "shader.hlsl";
        shader.setStringsWithLengthsAndNames(&text, &length, &name, 1);
        shader.setPreamble(preamble.c_str());
        shader.setEntryPoint(module.entry.c_str());
        shader.setSourceEntryPoint(module.entry.c_str());
        shader.setEnvInput(glslang::EShSourceHlsl, language, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
        shader.setEnvTargetHlslFunctionality1();
        shader.setHlslIoMapping(true);
        shader.setAutoMapLocations(true);
        shader.setAutoMapBindings(true);
        shader.setDxPositionW(true);
        // Registers keep their numbers in four binding ranges; spaces become descriptor sets.
        shader.setShiftBinding(glslang::EResUbo, 0);
        shader.setShiftBinding(glslang::EResTexture, 1000);
        shader.setShiftBinding(glslang::EResSampler, 2000);
        shader.setShiftBinding(glslang::EResUav, 3000);
        shader.setShiftBinding(glslang::EResImage, 3000);
        shader.setShiftBinding(glslang::EResSsbo, 3000);
        const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl |
            EShMsgHlslOffsets);
        Includer includer(include, source_name != nullptr ? source_name : "");
        if (!shader.parse(GetDefaultResources(), 100, false, messages, includer)) {
            if (errors != nullptr) *errors = text_blob(shader.getInfoLog());
            return E_FAIL;
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages) || !program.mapIO()) {
            if (errors != nullptr) *errors = text_blob(program.getInfoLog());
            return E_FAIL;
        }
        std::vector<std::uint32_t> spirv;
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.disableOptimizer = true;
        glslang::GlslangToSpv(*program.getIntermediate(language), spirv, &logger, &options);
        module.io = interface_semantics(spirv);

        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_0);
        std::string optimizer_errors;
        optimizer.SetMessageConsumer([&](spv_message_level_t level, const char*, const spv_position_t&, const char* message) {
            if (level <= SPV_MSG_ERROR) optimizer_errors += std::string(message) + "\n";
        });
        optimizer.RegisterLegalizationPasses();
        spvtools::OptimizerOptions optimizer_options;
        optimizer_options.set_run_validator(false);
        if (!optimizer.Run(spirv.data(), spirv.size(), &module.spirv, optimizer_options)) {
            if (errors != nullptr) *errors = text_blob("SPIR-V legalization failed:\n" + optimizer_errors);
            return E_FAIL;
        }
        const std::string warnings = shader.getInfoLog();
        if (errors != nullptr && !warnings.empty()) *errors = text_blob(warnings);
        *code = new BytesBlob(galaxy::metal12::serialize_shader_blob(module));
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::exception& error) {
        if (errors != nullptr) *errors = text_blob(error.what());
        return E_FAIL;
    }
}

namespace {

class ShaderReflection final : public ID3D12ShaderReflection {
public:
    explicit ShaderReflection(ShaderStage stage) : stage_(stage) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12ShaderReflection)) {
            *object = static_cast<ID3D12ShaderReflection*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3D12_SHADER_DESC* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        UINT type = D3D12_SHVER_VERTEX_SHADER;
        switch (stage_) {
        case ShaderStage::Pixel: type = D3D12_SHVER_PIXEL_SHADER; break;
        case ShaderStage::Geometry: type = D3D12_SHVER_GEOMETRY_SHADER; break;
        case ShaderStage::Compute: type = D3D12_SHVER_COMPUTE_SHADER; break;
        default: break;
        }
        desc->Version = (type << 16u) | (5u << 4u) | 1u;
        desc->Creator = "glslang";
        return S_OK;
    }
    ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByIndex(UINT) override { return nullptr; }
    ID3D12ShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR) override { return nullptr; }
    HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT, D3D12_SHADER_INPUT_BIND_DESC*) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC*) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC*) override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT, D3D12_SIGNATURE_PARAMETER_DESC*) override {
        return E_INVALIDARG;
    }
    ID3D12ShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(LPCSTR) override { return nullptr; }
    HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR, D3D12_SHADER_INPUT_BIND_DESC*) override {
        return E_INVALIDARG;
    }
    UINT STDMETHODCALLTYPE GetMovInstructionCount() override { return 0; }
    UINT STDMETHODCALLTYPE GetMovcInstructionCount() override { return 0; }
    UINT STDMETHODCALLTYPE GetConversionInstructionCount() override { return 0; }
    UINT STDMETHODCALLTYPE GetBitwiseInstructionCount() override { return 0; }
    D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive() override { return D3D_PRIMITIVE_UNDEFINED; }
    BOOL STDMETHODCALLTYPE IsSampleFrequencyShader() override { return FALSE; }
    UINT STDMETHODCALLTYPE GetNumInterfaceSlots() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetMinFeatureLevel(D3D_FEATURE_LEVEL* level) override {
        if (level != nullptr) *level = D3D_FEATURE_LEVEL_11_0;
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetThreadGroupSize(UINT* x, UINT* y, UINT* z) override {
        if (x != nullptr) *x = 0;
        if (y != nullptr) *y = 0;
        if (z != nullptr) *z = 0;
        return 0;
    }
    UINT64 STDMETHODCALLTYPE GetRequiresFlags() override { return 0; }

private:
    ShaderStage stage_;
    std::atomic<ULONG> references_{1};
};

}  // namespace

HRESULT D3DReflect(LPCVOID data, SIZE_T size, REFIID iid, void** reflector) noexcept {
    if (reflector == nullptr) return E_INVALIDARG;
    *reflector = nullptr;
    ShaderModule module;
    if (!galaxy::metal12::parse_shader_blob(data, size, module)) return E_INVALIDARG;
    auto* reflection = new (std::nothrow) ShaderReflection(module.stage);
    if (reflection == nullptr) return E_OUTOFMEMORY;
    const HRESULT hr = reflection->QueryInterface(iid, reflector);
    reflection->Release();
    return hr;
}

HRESULT D3DCreateBlob(SIZE_T size, ID3DBlob** blob) noexcept {
    if (blob == nullptr) return E_INVALIDARG;
    *blob = new (std::nothrow) BytesBlob(size);
    return *blob != nullptr ? S_OK : E_OUTOFMEMORY;
}
