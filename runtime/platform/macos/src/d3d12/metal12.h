// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Direct3D 12 on Metal: internal declarations of the translation layer.
//
// Objects implement the DirectX-Headers interfaces. The layer covers the D3D12 subset the GX
// renderer uses: committed and placed resources (buffers in shared memory with emulated GPU
// virtual addresses, textures in private memory), CPU-addressed descriptor heaps, root
// signatures, graphics pipelines (HLSL compiled to SPIR-V by D3DCompile, converted to MSL here
// for the pipeline's root signature and input layout), command lists recorded on the CPU and
// encoded into Metal command buffers at ExecuteCommandLists, fences over MTLSharedEvent, and a
// DXGI swap chain that presents through a CAMetalLayer.
//
// Descriptor handles are the addresses of Descriptor records. OMSetRenderTargets copies the
// records at record time; descriptor tables are read when the list executes, as on D3D12.

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#undef interface

#include "../win32_internal.h"
#include "shader_translate.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace galaxy::metal12 {

class Device;
class Resource;

// --- COM plumbing --------------------------------------------------------------------------------

template <typename Interface, typename... Bases>
class Unknown : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Interface) || ((riid == __uuidof(Bases)) || ...)) {
            *object = static_cast<Interface*>(this);
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
    virtual ~Unknown() = default;

private:
    std::atomic<ULONG> references_{1};
};

// ID3D12Object and ID3D12DeviceChild for every device object.
#define METAL12_PRIVATE_DATA_METHODS                                                                  \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* size, void*) override {                   \
        if (size != nullptr) *size = 0;                                                              \
        return DXGI_ERROR_NOT_FOUND;                                                                 \
    }                                                                                                \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }   \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; } \
    std::wstring debug_name;

#define METAL12_OBJECT_METHODS                                                                        \
    METAL12_PRIVATE_DATA_METHODS                                                                     \
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {                                       \
        debug_name = name != nullptr ? name : L"";                                                   \
        return S_OK;                                                                                 \
    }

#define METAL12_CHILD_METHODS                                                                         \
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override;                        \
    Device* device = nullptr;

// Logs an unimplemented call once.
void unimplemented(const char* what) noexcept;

// --- Formats -------------------------------------------------------------------------------------

struct FormatInfo {
    MTLPixelFormat pixel = MTLPixelFormatInvalid;
    std::uint32_t bytes_per_block = 0;  // per pixel, or per 4x4 block when compressed
    std::uint32_t block = 1;            // 1, or 4 for block-compressed formats
    bool depth = false;
    bool stencil = false;
};
[[nodiscard]] FormatInfo format_info(DXGI_FORMAT format, bool depth_resource = false);
[[nodiscard]] MTLVertexFormat vertex_format(DXGI_FORMAT format);
// MSL expression reading one vertex element of `format` (see VertexFetch).
[[nodiscard]] std::string msl_fetch_expression(DXGI_FORMAT format);

// --- Resources -----------------------------------------------------------------------------------

class Heap final : public Unknown<ID3D12Heap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_HEAP_DESC desc{};
};

class Resource final : public Unknown<ID3D12Resource, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_PRIVATE_DATA_METHODS
    METAL12_CHILD_METHODS
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override;  // labels the Metal object
    ~Resource() override;
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* read_range, void** data) override;
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE* written_range) override;
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return gpu_address; }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT, const D3D12_BOX*, const void*, UINT, UINT) override;
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) override;
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override;

    [[nodiscard]] bool is_buffer() const noexcept { return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER; }
    // Texture view of one mip range in `format` (typeless resources are viewed typed).
    [[nodiscard]] id<MTLTexture> view(DXGI_FORMAT format, unsigned first_mip, unsigned mip_count,
        unsigned first_slice = 0, unsigned slice_count = 1);

    D3D12_RESOURCE_DESC desc{};
    D3D12_HEAP_PROPERTIES heap_properties{};
    D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
    id<MTLBuffer> buffer = nil;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;
    id<MTLTexture> texture = nil;
    FormatInfo format{};
    std::mutex views_mutex;
    std::map<std::uint64_t, id<MTLTexture>> views;
};

// --- Descriptors ----------------------------------------------------------------------------------

struct Descriptor {
    enum class Kind : std::uint8_t { Empty, Cbv, Srv, Uav, Rtv, Dsv, Sampler };
    Kind kind = Kind::Empty;
    Resource* resource = nullptr;      // not owned: D3D12 descriptors hold no reference
    id<MTLTexture> texture = nil;      // SRV/RTV/DSV view
    id<MTLSamplerState> sampler = nil;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;  // CBV and buffer SRVs
    std::uint32_t size = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool read_only_depth = false;
};

class DescriptorHeap final
    : public Unknown<ID3D12DescriptorHeap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }
    D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetCPUDescriptorHandleForHeapStart() override {
        return D3D12_CPU_DESCRIPTOR_HANDLE{reinterpret_cast<SIZE_T>(descriptors.data())};
    }
    D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE GetGPUDescriptorHandleForHeapStart() override {
        return D3D12_GPU_DESCRIPTOR_HANDLE{reinterpret_cast<UINT64>(descriptors.data())};
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    std::vector<Descriptor> descriptors;
};

inline Descriptor* descriptor_at(D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept {
    return reinterpret_cast<Descriptor*>(handle.ptr);
}
inline Descriptor* descriptor_at(D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
    return reinterpret_cast<Descriptor*>(handle.ptr);
}

// --- Root signatures --------------------------------------------------------------------------------

enum class StageMask : std::uint8_t { None = 0, Vertex = 1, Fragment = 2, All = 3 };

struct RootRange {
    D3D12_DESCRIPTOR_RANGE_TYPE type{};
    unsigned count = 0;
    unsigned base_register = 0;
    unsigned space = 0;
    unsigned table_offset = 0;   // descriptors from the table start
    unsigned metal_index = 0;    // first Metal texture/sampler/buffer index
};

struct RootParameter {
    D3D12_ROOT_PARAMETER_TYPE type{};
    std::uint8_t stages = 0;     // StageMask bits
    unsigned shader_register = 0;
    unsigned space = 0;
    unsigned constants = 0;      // 32-bit values for root constants
    unsigned metal_buffer = 0;   // root CBV/SRV/UAV/constants
    std::vector<RootRange> ranges;
};

struct StaticSampler {
    unsigned shader_register = 0;
    unsigned space = 0;
    std::uint8_t stages = 0;
    unsigned metal_index = 0;
    id<MTLSamplerState> sampler = nil;
};

class RootSignature final : public Unknown<ID3D12RootSignature, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    std::vector<RootParameter> parameters;
    std::vector<StaticSampler> static_samplers;
    std::map<BindingKey, unsigned> bindings;   // Metal index per register
    std::uint64_t hash = 0;
};

// Builds the layout from a serialized root signature (D3D12SerializeRootSignature blob).
[[nodiscard]] HRESULT parse_root_signature(Device& device, const void* data, std::size_t size, RootSignature& out);

// --- Shaders --------------------------------------------------------------------------------------

struct InputElement {
    std::string semantic;   // upper-case semantic name with index appended ("TEXCOORD3")
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned slot = 0;
    unsigned offset = 0;
};

struct MetalProgram {
    id<MTLFunction> vertex = nil;
    id<MTLFunction> fragment = nil;
    // Vertex attribute location of each input element (same order as the input layout).
    std::vector<int> attribute_locations;
    std::string error;
};

// Converts the stages to MSL for this root signature and compiles them.
[[nodiscard]] MetalProgram build_program(Device& device, const ShaderModule& vertex, const ShaderModule* pixel,
    const RootSignature& root, const std::vector<InputElement>& inputs, bool dual_source_blend, bool points,
    GeometryExpansion expansion = GeometryExpansion::None);

// --- Pipelines ------------------------------------------------------------------------------------

inline constexpr unsigned kMaxVertexBuffers = 8;
inline constexpr unsigned kFirstVertexBufferIndex = 30;   // slot s -> Metal buffer 30 - s

class PipelineState final : public Unknown<ID3D12PipelineState, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob** blob) override;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLDepthStencilState> depth_stencil = nil;
    MTLCullMode cull = MTLCullModeNone;
    MTLWinding winding = MTLWindingClockwise;
    MTLTriangleFillMode fill = MTLTriangleFillModeFill;
    MTLDepthClipMode depth_clip = MTLDepthClipModeClip;
    float depth_bias = 0.0f;
    float slope_scaled_depth_bias = 0.0f;
    float depth_bias_clamp = 0.0f;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    GeometryExpansion expansion = GeometryExpansion::None;  // draws six vertices per line or point
    std::array<MTLPixelFormat, 8> color_formats{};
    MTLPixelFormat depth_format = MTLPixelFormatInvalid;
    unsigned sample_count = 1;
    RootSignature* root = nullptr;  // held by root_reference
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_reference;
};

// --- Synchronization --------------------------------------------------------------------------------

class Fence final : public Unknown<ID3D12Fence, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return event.signaledValue; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event_handle) override;
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override;
    id<MTLSharedEvent> event = nil;
};

class CommandAllocator final
    : public Unknown<ID3D12CommandAllocator, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
};

class QueryHeap final : public Unknown<ID3D12QueryHeap, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    D3D12_QUERY_HEAP_DESC desc{};
    std::vector<std::uint64_t> values;  // written when the command buffer that ends them completes
};

// --- Command lists ------------------------------------------------------------------------------------

struct RenderTargets {
    std::array<Descriptor, 8> colors{};
    unsigned color_count = 0;
    Descriptor depth{};
    bool has_depth = false;
};

struct Command;  // defined in commands.mm

class CommandList final : public Unknown<ID3D12GraphicsCommandList, ID3D12CommandList, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    CommandList();
    ~CommandList() override;
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return type; }
    HRESULT STDMETHODCALLTYPE Close() override;
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial) override;
    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState* pipeline) override;
    void STDMETHODCALLTYPE DrawInstanced(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) override;
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex,
        UINT first_instance) override;
    void STDMETHODCALLTYPE Dispatch(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* destination, UINT64 destination_offset,
        ID3D12Resource* source, UINT64 source_offset, UINT64 bytes) override;
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z,
        const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) override;
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource* destination, ID3D12Resource* source) override;
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
        const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS) override;
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource* destination, UINT destination_subresource,
        ID3D12Resource* source, UINT source_subresource, DXGI_FORMAT format) override;
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) override;
    void STDMETHODCALLTYPE RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) override;
    void STDMETHODCALLTYPE RSSetScissorRects(UINT count, const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override;
    void STDMETHODCALLTYPE OMSetStencilRef(UINT reference) override;
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pipeline) override;
    void STDMETHODCALLTYPE ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) override;
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList*) override;
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT, ID3D12DescriptorHeap* const*) override {}
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature*) override;
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* root) override;
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) override;
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT, UINT, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) override;
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) override;
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) override;
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override;
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override;
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) override;
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) override;
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) override;
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) override;
    void STDMETHODCALLTYPE SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) override;
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* colors, BOOL single_range,
        const D3D12_CPU_DESCRIPTOR_HANDLE* depth) override;
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth,
        UINT8 stencil, UINT rect_count, const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4],
        UINT rect_count, const D3D12_RECT* rects) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
        ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
        ID3D12Resource*, const FLOAT[4], UINT, const D3D12_RECT*) override;
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource*, const D3D12_DISCARD_REGION*) override {}
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT) override {}
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) override;
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT first, UINT count,
        ID3D12Resource* destination, UINT64 offset) override;
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource*, UINT64, D3D12_PREDICATION_OP) override {}
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*,
        UINT64) override;

    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    std::vector<Command> commands;
    std::vector<std::uint32_t> constant_pool;  // root constant values the commands index
    bool closed = false;
    // Objects the recorded commands reference stay alive until the list is reset.
    std::vector<Microsoft::WRL::ComPtr<IUnknown>> keep_alive;
};

class CommandQueue final : public Unknown<ID3D12CommandQueue, ID3D12Pageable, ID3D12DeviceChild, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    METAL12_CHILD_METHODS
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*,
        const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*, const UINT*, const UINT*,
        D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, ID3D12Resource*,
        const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*, D3D12_TILE_MAPPING_FLAGS) override {}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) override;
    void STDMETHODCALLTYPE SetMarker(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE BeginEvent(UINT, const void*, UINT) override {}
    void STDMETHODCALLTYPE EndEvent() override {}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* fence, UINT64 value) override;
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* frequency) override;
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* gpu, UINT64* cpu) override;
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return desc; }

    D3D12_COMMAND_QUEUE_DESC desc{};
    id<MTLCommandQueue> queue = nil;
    std::mutex submit_mutex;  // ExecuteCommandLists, Signal and Present stay in submission order
};

// --- Device --------------------------------------------------------------------------------------------

class Device final : public Unknown<ID3D12Device, ID3D12Object> {
public:
    METAL12_OBJECT_METHODS
    Device();
    ~Device() override;

    UINT STDMETHODCALLTYPE GetNodeCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** queue) override;
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** allocator) override;
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid,
        void** pipeline) override;
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid,
        void** pipeline) override;
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initial, REFIID riid, void** list) override;
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) override;
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** heap) override;
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) override {
        return static_cast<UINT>(sizeof(Descriptor));
    }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node, const void* blob, SIZE_T size, REFIID riid, void** root) override;
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource* counter,
        const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) override;
    void STDMETHODCALLTYPE CopyDescriptors(UINT destination_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* destination_starts,
        const UINT* destination_sizes, UINT source_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* source_starts,
        const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) override;
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination,
        D3D12_CPU_DESCRIPTOR_HANDLE source, D3D12_DESCRIPTOR_HEAP_TYPE type) override;
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT visible, UINT count,
        const D3D12_RESOURCE_DESC* descs) override;
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT node, D3D12_HEAP_TYPE type) override;
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid,
        void** resource) override;
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** heap) override;
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc,
        D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** resource) override;
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE*, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR,
        HANDLE*) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE, REFIID, void**) override;
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE MakeResident(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Evict(UINT, ID3D12Pageable* const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial, D3D12_FENCE_FLAGS flags, REFIID riid, void** fence) override;
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* row_sizes, UINT64* total) override;
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** heap) override;
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID,
        void**) override;
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource*, UINT*, D3D12_PACKED_MIP_INFO*, D3D12_TILE_SHAPE*, UINT*,
        UINT, D3D12_SUBRESOURCE_TILING*) override {}
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return LUID{0x4D45544Cu, 0}; }

    // Translation state.
    id<MTLDevice> mtl = nil;
    id<MTLTexture> null_texture = nil;          // bound for null SRVs
    id<MTLSamplerState> default_sampler = nil;
    [[nodiscard]] id<MTLSamplerState> sampler_for(const D3D12_SAMPLER_DESC& desc);

    // Emulated GPU virtual addresses of buffers.
    D3D12_GPU_VIRTUAL_ADDRESS reserve_address(std::uint64_t size, Resource* resource);
    void release_address(D3D12_GPU_VIRTUAL_ADDRESS address);
    struct BufferSpan {
        id<MTLBuffer> buffer = nil;
        std::uint64_t offset = 0;
    };
    [[nodiscard]] BufferSpan resolve_address(D3D12_GPU_VIRTUAL_ADDRESS address);

private:
    std::mutex address_mutex_;
    std::map<D3D12_GPU_VIRTUAL_ADDRESS, std::pair<std::uint64_t, Resource*>> address_map_;
    D3D12_GPU_VIRTUAL_ADDRESS next_address_ = 0x0000000100000000ull;
    std::mutex sampler_mutex_;
    std::unordered_map<std::string, id<MTLSamplerState>> samplers_;
};

// Executes recorded lists into a command buffer (commands.mm).
void encode_command_list(CommandQueue& queue, CommandList& list, id<MTLCommandBuffer> command_buffer);

// Blob (ID3DBlob) over a byte vector.
class Blob final : public Unknown<ID3DBlob> {
public:
    explicit Blob(std::vector<std::uint8_t> bytes) : data(std::move(bytes)) {}
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data.size(); }
    std::vector<std::uint8_t> data;
};

}  // namespace galaxy::metal12
