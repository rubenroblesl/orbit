// SPDX-License-Identifier: GPL-3.0-only
//
// Direct3D 12 on Metal: device, resources, descriptors, root signatures, pipeline states, fences
// and query heaps.

#include "metal12.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <set>

namespace galaxy::metal12 {

void unimplemented(const char* what) noexcept {
    static std::mutex mutex;
    static std::set<std::string> reported;
    std::lock_guard lock(mutex);
    if (reported.insert(what).second) std::fprintf(stderr, "[metal12] %s is not implemented\n", what);
}

namespace {

NSString* ns_string(const std::wstring& text) {
    return [NSString stringWithUTF8String:galaxy::compat::utf8_from_wide(text).c_str()];
}

template <typename T>
HRESULT hand_out(T* object, REFIID riid, void** result) {
    if (result == nullptr) {
        object->Release();
        return S_FALSE;
    }
    const HRESULT hr = object->QueryInterface(riid, result);
    object->Release();
    return hr;
}

MTLCompareFunction compare_function(D3D12_COMPARISON_FUNC function) {
    switch (function) {
    case D3D12_COMPARISON_FUNC_NEVER: return MTLCompareFunctionNever;
    case D3D12_COMPARISON_FUNC_LESS: return MTLCompareFunctionLess;
    case D3D12_COMPARISON_FUNC_EQUAL: return MTLCompareFunctionEqual;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL: return MTLCompareFunctionLessEqual;
    case D3D12_COMPARISON_FUNC_GREATER: return MTLCompareFunctionGreater;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLSamplerAddressMode address_mode(D3D12_TEXTURE_ADDRESS_MODE mode) {
    switch (mode) {
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
    case D3D12_TEXTURE_ADDRESS_MODE_CLAMP: return MTLSamplerAddressModeClampToEdge;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

MTLBlendFactor blend_factor(D3D12_BLEND blend) {
    switch (blend) {
    case D3D12_BLEND_ZERO: return MTLBlendFactorZero;
    case D3D12_BLEND_ONE: return MTLBlendFactorOne;
    case D3D12_BLEND_SRC_COLOR: return MTLBlendFactorSourceColor;
    case D3D12_BLEND_INV_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case D3D12_BLEND_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case D3D12_BLEND_INV_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case D3D12_BLEND_DEST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case D3D12_BLEND_INV_DEST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case D3D12_BLEND_DEST_COLOR: return MTLBlendFactorDestinationColor;
    case D3D12_BLEND_INV_DEST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case D3D12_BLEND_SRC_ALPHA_SAT: return MTLBlendFactorSourceAlphaSaturated;
    case D3D12_BLEND_BLEND_FACTOR: return MTLBlendFactorBlendColor;
    case D3D12_BLEND_INV_BLEND_FACTOR: return MTLBlendFactorOneMinusBlendColor;
    case D3D12_BLEND_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case D3D12_BLEND_INV_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case D3D12_BLEND_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case D3D12_BLEND_INV_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    case D3D12_BLEND_ALPHA_FACTOR: return MTLBlendFactorBlendAlpha;
    case D3D12_BLEND_INV_ALPHA_FACTOR: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}

bool is_dual_source(D3D12_BLEND blend) {
    return blend == D3D12_BLEND_SRC1_COLOR || blend == D3D12_BLEND_INV_SRC1_COLOR ||
        blend == D3D12_BLEND_SRC1_ALPHA || blend == D3D12_BLEND_INV_SRC1_ALPHA;
}

MTLBlendOperation blend_operation(D3D12_BLEND_OP operation) {
    switch (operation) {
    case D3D12_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
    case D3D12_BLEND_OP_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case D3D12_BLEND_OP_MIN: return MTLBlendOperationMin;
    case D3D12_BLEND_OP_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLStencilOperation stencil_operation(D3D12_STENCIL_OP operation) {
    switch (operation) {
    case D3D12_STENCIL_OP_ZERO: return MTLStencilOperationZero;
    case D3D12_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
    case D3D12_STENCIL_OP_INCR_SAT: return MTLStencilOperationIncrementClamp;
    case D3D12_STENCIL_OP_DECR_SAT: return MTLStencilOperationDecrementClamp;
    case D3D12_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
    case D3D12_STENCIL_OP_INCR: return MTLStencilOperationIncrementWrap;
    case D3D12_STENCIL_OP_DECR: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLStencilDescriptor* stencil_descriptor(const D3D12_DEPTH_STENCILOP_DESC& face, UINT8 read_mask, UINT8 write_mask) {
    MTLStencilDescriptor* descriptor = [[MTLStencilDescriptor alloc] init];
    descriptor.stencilCompareFunction = compare_function(face.StencilFunc);
    descriptor.stencilFailureOperation = stencil_operation(face.StencilFailOp);
    descriptor.depthFailureOperation = stencil_operation(face.StencilDepthFailOp);
    descriptor.depthStencilPassOperation = stencil_operation(face.StencilPassOp);
    descriptor.readMask = read_mask;
    descriptor.writeMask = write_mask;
    return descriptor;
}

std::string semantic_key(const char* name, unsigned index) {
    std::string key = name != nullptr ? name : "";
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return key + std::to_string(index);
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Device children
// ------------------------------------------------------------------------------------------------

#define METAL12_GET_DEVICE(Class)                                                     \
    HRESULT STDMETHODCALLTYPE Class::GetDevice(REFIID riid, void** result) {        \
        if (device == nullptr) return E_FAIL;                                      \
        return device->QueryInterface(riid, result);                               \
    }
METAL12_GET_DEVICE(Heap)
METAL12_GET_DEVICE(Resource)
METAL12_GET_DEVICE(DescriptorHeap)
METAL12_GET_DEVICE(RootSignature)
METAL12_GET_DEVICE(PipelineState)
METAL12_GET_DEVICE(Fence)
METAL12_GET_DEVICE(CommandAllocator)
METAL12_GET_DEVICE(QueryHeap)
METAL12_GET_DEVICE(CommandList)
METAL12_GET_DEVICE(CommandQueue)

// ------------------------------------------------------------------------------------------------
// Device
// ------------------------------------------------------------------------------------------------

Device::Device() {
    mtl = MTLCreateSystemDefaultDevice();
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    null_texture = [mtl newTextureWithDescriptor:descriptor];
    const std::uint32_t black = 0;
    [null_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
    MTLSamplerDescriptor* sampler = [[MTLSamplerDescriptor alloc] init];
    default_sampler = [mtl newSamplerStateWithDescriptor:sampler];
}

Device::~Device() = default;

D3D12_GPU_VIRTUAL_ADDRESS Device::reserve_address(std::uint64_t size, Resource* resource) {
    std::lock_guard lock(address_mutex_);
    const D3D12_GPU_VIRTUAL_ADDRESS address = next_address_;
    next_address_ += (std::max<std::uint64_t>(size, 1u) + 0xFFFFu) & ~std::uint64_t{0xFFFFu};
    address_map_[address] = {size, resource};
    return address;
}

void Device::release_address(D3D12_GPU_VIRTUAL_ADDRESS address) {
    std::lock_guard lock(address_mutex_);
    address_map_.erase(address);
}

Device::BufferSpan Device::resolve_address(D3D12_GPU_VIRTUAL_ADDRESS address) {
    std::lock_guard lock(address_mutex_);
    auto it = address_map_.upper_bound(address);
    if (it == address_map_.begin()) return {};
    --it;
    const std::uint64_t offset = address - it->first;
    if (offset > it->second.first) return {};
    return {it->second.second->buffer, offset};
}

id<MTLSamplerState> Device::sampler_for(const D3D12_SAMPLER_DESC& desc) {
    const std::string key(reinterpret_cast<const char*>(&desc), sizeof(desc));
    std::lock_guard lock(sampler_mutex_);
    if (const auto it = samplers_.find(key); it != samplers_.end()) return it->second;
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    const unsigned filter = static_cast<unsigned>(desc.Filter);
    const bool anisotropic = (filter & D3D12_ANISOTROPIC_FILTERING_BIT) != 0u;
    descriptor.minFilter = (anisotropic || ((filter >> D3D12_MIN_FILTER_SHIFT) & 1u) != 0u)
        ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = (anisotropic || ((filter >> D3D12_MAG_FILTER_SHIFT) & 1u) != 0u)
        ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = (anisotropic || ((filter >> D3D12_MIP_FILTER_SHIFT) & 1u) != 0u)
        ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    if (anisotropic) descriptor.maxAnisotropy = std::clamp<NSUInteger>(desc.MaxAnisotropy, 1u, 16u);
    descriptor.sAddressMode = address_mode(desc.AddressU);
    descriptor.tAddressMode = address_mode(desc.AddressV);
    descriptor.rAddressMode = address_mode(desc.AddressW);
    if (desc.BorderColor[3] == 0.0f) {
        descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
    } else if (desc.BorderColor[0] >= 1.0f && desc.BorderColor[1] >= 1.0f && desc.BorderColor[2] >= 1.0f) {
        descriptor.borderColor = MTLSamplerBorderColorOpaqueWhite;
    } else {
        descriptor.borderColor = MTLSamplerBorderColorOpaqueBlack;
    }
    const unsigned reduction = (filter >> D3D12_FILTER_REDUCTION_TYPE_SHIFT) & D3D12_FILTER_REDUCTION_TYPE_MASK;
    if (reduction == D3D12_FILTER_REDUCTION_TYPE_COMPARISON) descriptor.compareFunction = compare_function(desc.ComparisonFunc);
    descriptor.lodMinClamp = std::max(0.0f, desc.MinLOD);
    descriptor.lodMaxClamp = desc.MaxLOD > 1000.0f ? FLT_MAX : std::max(descriptor.lodMinClamp, desc.MaxLOD);
    // Sampler LOD bias needs macOS 26; earlier systems sample without it.
    if (@available(macOS 26.0, *)) descriptor.lodBias = desc.MipLODBias;
    id<MTLSamplerState> sampler = [mtl newSamplerStateWithDescriptor:descriptor];
    samplers_.emplace(key, sampler);
    return sampler;
}

HRESULT Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** result) {
    if (desc == nullptr) return E_INVALIDARG;
    auto* queue = new CommandQueue();
    queue->device = this;
    queue->desc = *desc;
    queue->queue = [mtl newCommandQueueWithMaxCommandBufferCount:64];
    return hand_out(queue, riid, result);
}

HRESULT Device::CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** result) {
    auto* allocator = new CommandAllocator();
    allocator->device = this;
    allocator->type = type;
    return hand_out(allocator, riid, result);
}

HRESULT Device::CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
    ID3D12PipelineState* initial, REFIID riid, void** result) {
    auto* list = new CommandList();
    list->device = this;
    list->type = type;
    list->Reset(allocator, initial);
    return hand_out(list, riid, result);
}

HRESULT Device::CreateFence(UINT64 initial, D3D12_FENCE_FLAGS, REFIID riid, void** result) {
    auto* fence = new Fence();
    fence->device = this;
    fence->event = [mtl newSharedEvent];
    fence->event.signaledValue = initial;
    return hand_out(fence, riid, result);
}

HRESULT Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** result) {
    if (desc == nullptr || desc->NumDescriptors == 0u) return E_INVALIDARG;
    auto* heap = new DescriptorHeap();
    heap->device = this;
    heap->desc = *desc;
    heap->descriptors.resize(desc->NumDescriptors);
    return hand_out(heap, riid, result);
}

HRESULT Device::CreateRootSignature(UINT, const void* blob, SIZE_T size, REFIID riid, void** result) {
    auto* root = new RootSignature();
    root->device = this;
    const HRESULT hr = parse_root_signature(*this, blob, size, *root);
    if (FAILED(hr)) {
        root->Release();
        return hr;
    }
    return hand_out(root, riid, result);
}

HRESULT Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** result) {
    if (desc == nullptr || desc->pRootSignature == nullptr || desc->VS.pShaderBytecode == nullptr) return E_INVALIDARG;
    auto* root = static_cast<RootSignature*>(desc->pRootSignature);
    ShaderModule vertex;
    if (!parse_shader_blob(desc->VS.pShaderBytecode, desc->VS.BytecodeLength, vertex)) {
        std::fprintf(stderr, "[metal12] pipeline vertex shader is not a translated shader blob\n");
        return E_INVALIDARG;
    }
    ShaderModule pixel;
    const bool has_pixel = desc->PS.pShaderBytecode != nullptr &&
        parse_shader_blob(desc->PS.pShaderBytecode, desc->PS.BytecodeLength, pixel);
    GeometryExpansion expansion = GeometryExpansion::None;
    if (desc->GS.pShaderBytecode != nullptr) {
        ShaderModule geometry;
        if (parse_shader_blob(desc->GS.pShaderBytecode, desc->GS.BytecodeLength, geometry)) {
            expansion = geometry_expansion_of(geometry);
        }
        if (expansion == GeometryExpansion::None) {
            unimplemented("geometry shaders other than the GX line/point expansion");
        }
    }

    std::vector<InputElement> inputs;
    std::array<unsigned, kMaxVertexBuffers> slot_offsets{};
    for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
        const D3D12_INPUT_ELEMENT_DESC& element = desc->InputLayout.pInputElementDescs[i];
        InputElement input;
        input.semantic = semantic_key(element.SemanticName, element.SemanticIndex);
        input.format = element.Format;
        input.slot = element.InputSlot;
        const unsigned slot = std::min<unsigned>(element.InputSlot, kMaxVertexBuffers - 1u);
        input.offset = element.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT ? slot_offsets[slot] : element.AlignedByteOffset;
        slot_offsets[slot] = input.offset + std::max(1u, format_info(element.Format).bytes_per_block);
        if (vertex_format(element.Format) != MTLVertexFormatInvalid) {
            const MTLVertexFormat vf = vertex_format(element.Format);
            const unsigned size = vf == MTLVertexFormatFloat4 || vf == MTLVertexFormatUInt4 || vf == MTLVertexFormatInt4 ? 16u
                : vf == MTLVertexFormatFloat3 || vf == MTLVertexFormatUInt3 ? 12u
                : vf == MTLVertexFormatFloat2 || vf == MTLVertexFormatUInt2 || vf == MTLVertexFormatInt2 ||
                        vf == MTLVertexFormatHalf4 || vf == MTLVertexFormatUShort4Normalized || vf == MTLVertexFormatShort4Normalized ||
                        vf == MTLVertexFormatUShort4 || vf == MTLVertexFormatShort4 ? 8u
                : 4u;
            slot_offsets[slot] = input.offset + size;
        }
        inputs.push_back(input);
    }

    bool dual_source = false;
    for (unsigned i = 0; i < 8; ++i) {
        const auto& target = desc->BlendState.RenderTarget[desc->BlendState.IndependentBlendEnable ? i : 0u];
        if (target.BlendEnable && (is_dual_source(target.SrcBlend) || is_dual_source(target.DestBlend) ||
                                      is_dual_source(target.SrcBlendAlpha) || is_dual_source(target.DestBlendAlpha))) {
            dual_source = true;
        }
    }

    const MetalProgram program = build_program(*this, vertex, has_pixel ? &pixel : nullptr, *root, inputs, dual_source,
        expansion == GeometryExpansion::None && desc->PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,
        expansion);
    if (program.vertex == nil) {
        std::fprintf(stderr, "[metal12] pipeline shader translation failed: %s\n", program.error.c_str());
        return E_FAIL;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = program.vertex;
    descriptor.fragmentFunction = program.fragment;
    MTLVertexDescriptor* vertex_descriptor = [MTLVertexDescriptor vertexDescriptor];
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const int location = i < program.attribute_locations.size() ? program.attribute_locations[i] : -1;
        if (location < 0) continue;
        const D3D12_INPUT_ELEMENT_DESC& element = desc->InputLayout.pInputElementDescs[i];
        const unsigned buffer_index = kFirstVertexBufferIndex - std::min<unsigned>(element.InputSlot, kMaxVertexBuffers - 1u);
        vertex_descriptor.attributes[location].format = vertex_format(element.Format);
        vertex_descriptor.attributes[location].offset = inputs[i].offset;
        vertex_descriptor.attributes[location].bufferIndex = buffer_index;
        vertex_descriptor.layouts[buffer_index].stride = MTLBufferLayoutStrideDynamic;
        if (element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            vertex_descriptor.layouts[buffer_index].stepFunction = MTLVertexStepFunctionPerInstance;
            vertex_descriptor.layouts[buffer_index].stepRate = std::max(1u, element.InstanceDataStepRate);
        }
    }
    // An expansion fetches its vertices itself.
    if (expansion == GeometryExpansion::None) descriptor.vertexDescriptor = vertex_descriptor;

    auto* pipeline = new PipelineState();
    pipeline->device = this;
    for (UINT i = 0; i < desc->NumRenderTargets && i < 8u; ++i) {
        const MTLPixelFormat format = format_info(desc->RTVFormats[i]).pixel;
        pipeline->color_formats[i] = format;
        descriptor.colorAttachments[i].pixelFormat = format;
        const auto& target = desc->BlendState.RenderTarget[desc->BlendState.IndependentBlendEnable ? i : 0u];
        MTLRenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[i];
        attachment.blendingEnabled = target.BlendEnable;
        attachment.sourceRGBBlendFactor = blend_factor(target.SrcBlend);
        attachment.destinationRGBBlendFactor = blend_factor(target.DestBlend);
        attachment.rgbBlendOperation = blend_operation(target.BlendOp);
        attachment.sourceAlphaBlendFactor = blend_factor(target.SrcBlendAlpha);
        attachment.destinationAlphaBlendFactor = blend_factor(target.DestBlendAlpha);
        attachment.alphaBlendOperation = blend_operation(target.BlendOpAlpha);
        MTLColorWriteMask mask = MTLColorWriteMaskNone;
        if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_RED) mask |= MTLColorWriteMaskRed;
        if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_GREEN) mask |= MTLColorWriteMaskGreen;
        if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_BLUE) mask |= MTLColorWriteMaskBlue;
        if (target.RenderTargetWriteMask & D3D12_COLOR_WRITE_ENABLE_ALPHA) mask |= MTLColorWriteMaskAlpha;
        attachment.writeMask = mask;
        if (target.LogicOpEnable) unimplemented("blend logic operations");
    }
    if (desc->DSVFormat != DXGI_FORMAT_UNKNOWN) {
        const FormatInfo depth = format_info(desc->DSVFormat, true);
        pipeline->depth_format = depth.pixel;
        if (depth.depth) descriptor.depthAttachmentPixelFormat = depth.pixel;
        if (depth.stencil) descriptor.stencilAttachmentPixelFormat = depth.pixel;
    }
    descriptor.rasterSampleCount = std::max(1u, desc->SampleDesc.Count);
    pipeline->sample_count = static_cast<unsigned>(descriptor.rasterSampleCount);
    descriptor.alphaToCoverageEnabled = desc->BlendState.AlphaToCoverageEnable;
    switch (expansion != GeometryExpansion::None ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : desc->PrimitiveTopologyType) {
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
    case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
    default: descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; break;
    }
    pipeline->topology_type = desc->PrimitiveTopologyType;
    pipeline->expansion = expansion;
    if (!debug_name.empty()) descriptor.label = ns_string(debug_name);

    NSError* error = nil;
    pipeline->pipeline = [mtl newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline->pipeline == nil) {
        std::fprintf(stderr, "[metal12] render pipeline creation failed: %s\n",
            error != nil ? error.localizedDescription.UTF8String : "unknown error");
        pipeline->Release();
        return E_FAIL;
    }

    MTLDepthStencilDescriptor* depth = [[MTLDepthStencilDescriptor alloc] init];
    if (desc->DepthStencilState.DepthEnable) {
        depth.depthCompareFunction = compare_function(desc->DepthStencilState.DepthFunc);
        depth.depthWriteEnabled = desc->DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
    } else {
        depth.depthCompareFunction = MTLCompareFunctionAlways;
        depth.depthWriteEnabled = NO;
    }
    if (desc->DepthStencilState.StencilEnable) {
        depth.frontFaceStencil = stencil_descriptor(desc->DepthStencilState.FrontFace,
            desc->DepthStencilState.StencilReadMask, desc->DepthStencilState.StencilWriteMask);
        depth.backFaceStencil = stencil_descriptor(desc->DepthStencilState.BackFace,
            desc->DepthStencilState.StencilReadMask, desc->DepthStencilState.StencilWriteMask);
    }
    pipeline->depth_stencil = [mtl newDepthStencilStateWithDescriptor:depth];

    const D3D12_RASTERIZER_DESC& raster = desc->RasterizerState;
    pipeline->cull = raster.CullMode == D3D12_CULL_MODE_FRONT ? MTLCullModeFront
        : raster.CullMode == D3D12_CULL_MODE_BACK ? MTLCullModeBack : MTLCullModeNone;
    pipeline->winding = raster.FrontCounterClockwise ? MTLWindingCounterClockwise : MTLWindingClockwise;
    pipeline->fill = raster.FillMode == D3D12_FILL_MODE_WIREFRAME ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    pipeline->depth_clip = raster.DepthClipEnable ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
    pipeline->depth_bias = static_cast<float>(raster.DepthBias);
    pipeline->slope_scaled_depth_bias = raster.SlopeScaledDepthBias;
    pipeline->depth_bias_clamp = raster.DepthBiasClamp;
    pipeline->root = root;
    pipeline->root_reference = desc->pRootSignature;
    return hand_out(pipeline, riid, result);
}

HRESULT Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) {
    unimplemented("compute pipelines");
    return E_NOTIMPL;
}

HRESULT Device::CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size) {
    if (data == nullptr) return E_INVALIDARG;
    switch (feature) {
    case D3D12_FEATURE_FEATURE_LEVELS: {
        if (size < sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) return E_INVALIDARG;
        auto* levels = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(data);
        D3D_FEATURE_LEVEL best = D3D_FEATURE_LEVEL_11_0;
        for (UINT i = 0; i < levels->NumFeatureLevels; ++i) {
            const D3D_FEATURE_LEVEL requested = levels->pFeatureLevelsRequested[i];
            if (requested <= D3D_FEATURE_LEVEL_12_0 && requested > best) best = requested;
        }
        levels->MaxSupportedFeatureLevel = best;
        return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
        if (size < sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT)) return E_INVALIDARG;
        auto* support = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
        const bool known = format_info(support->Format).pixel != MTLPixelFormatInvalid;
        support->Support1 = known ? static_cast<D3D12_FORMAT_SUPPORT1>(D3D12_FORMAT_SUPPORT1_TEXTURE2D |
            D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE)
                                  : D3D12_FORMAT_SUPPORT1_NONE;
        support->Support2 = D3D12_FORMAT_SUPPORT2_NONE;
        return S_OK;
    }
    default:
        // Optional features (ray tracing, shader caches, tiled resources, ...) report none.
        std::memset(data, 0, size);
        if (feature == D3D12_FEATURE_D3D12_OPTIONS && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS)) {
            auto* options = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data);
            options->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_2;
            options->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
        }
        return S_OK;
    }
}

// --- Views ------------------------------------------------------------------------------------------

void Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    Descriptor* descriptor = descriptor_at(destination);
    *descriptor = Descriptor{};
    if (desc == nullptr) return;
    descriptor->kind = Descriptor::Kind::Cbv;
    descriptor->address = desc->BufferLocation;
    descriptor->size = desc->SizeInBytes;
}

void Device::CreateShaderResourceView(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    Descriptor* descriptor = descriptor_at(destination);
    *descriptor = Descriptor{};
    descriptor->kind = Descriptor::Kind::Srv;
    auto* source = static_cast<Resource*>(resource);
    if (source == nullptr) {
        descriptor->texture = null_texture;
        return;
    }
    descriptor->resource = source;
    if (source->is_buffer()) {
        const UINT64 stride = desc != nullptr && desc->Buffer.StructureByteStride != 0u ? desc->Buffer.StructureByteStride
            : std::max<UINT64>(1u, format_info(desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN).bytes_per_block);
        descriptor->address = source->gpu_address + (desc != nullptr ? desc->Buffer.FirstElement * stride : 0u);
        descriptor->size = static_cast<std::uint32_t>(desc != nullptr ? desc->Buffer.NumElements * stride : source->desc.Width);
        return;
    }
    const DXGI_FORMAT format = desc != nullptr ? desc->Format : source->desc.Format;
    unsigned first_mip = 0;
    unsigned mips = source->texture != nil ? static_cast<unsigned>(source->texture.mipmapLevelCount) : 1u;
    if (desc != nullptr && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
        first_mip = desc->Texture2D.MostDetailedMip;
        if (desc->Texture2D.MipLevels != static_cast<UINT>(-1)) mips = desc->Texture2D.MipLevels;
        else mips -= std::min(mips, first_mip);
    }
    descriptor->format = format;
    descriptor->texture = source->view(format, first_mip, mips);
    if (descriptor->texture == nil) descriptor->texture = null_texture;
}

void Device::CreateUnorderedAccessView(ID3D12Resource* resource, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    Descriptor* descriptor = descriptor_at(destination);
    *descriptor = Descriptor{};
    descriptor->kind = Descriptor::Kind::Uav;
    auto* source = static_cast<Resource*>(resource);
    if (source == nullptr) return;
    descriptor->resource = source;
    if (source->is_buffer()) {
        descriptor->address = source->gpu_address;
        descriptor->size = static_cast<std::uint32_t>(source->desc.Width);
    } else {
        descriptor->texture = source->view(desc != nullptr ? desc->Format : source->desc.Format,
            desc != nullptr && desc->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D ? desc->Texture2D.MipSlice : 0u, 1u);
    }
}

void Device::CreateRenderTargetView(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    Descriptor* descriptor = descriptor_at(destination);
    *descriptor = Descriptor{};
    descriptor->kind = Descriptor::Kind::Rtv;
    auto* source = static_cast<Resource*>(resource);
    if (source == nullptr) return;
    descriptor->resource = source;
    const DXGI_FORMAT format = desc != nullptr && desc->Format != DXGI_FORMAT_UNKNOWN ? desc->Format : source->desc.Format;
    const unsigned mip = desc != nullptr && desc->ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2D ? desc->Texture2D.MipSlice : 0u;
    descriptor->format = format;
    descriptor->texture = source->view(format, mip, 1u);
}

void Device::CreateDepthStencilView(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    Descriptor* descriptor = descriptor_at(destination);
    *descriptor = Descriptor{};
    descriptor->kind = Descriptor::Kind::Dsv;
    auto* source = static_cast<Resource*>(resource);
    if (source == nullptr) return;
    descriptor->resource = source;
    descriptor->format = desc != nullptr ? desc->Format : source->desc.Format;
    const unsigned mip = desc != nullptr && desc->ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2D ? desc->Texture2D.MipSlice : 0u;
    descriptor->texture = mip == 0u ? source->texture : source->view(source->desc.Format, mip, 1u);
    descriptor->read_only_depth = desc != nullptr && (desc->Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH) != 0;
}

void Device::CreateSampler(const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    Descriptor* descriptor = descriptor_at(destination);
    *descriptor = Descriptor{};
    descriptor->kind = Descriptor::Kind::Sampler;
    descriptor->sampler = desc != nullptr ? sampler_for(*desc) : default_sampler;
}

void Device::CopyDescriptors(UINT destination_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* destination_starts,
    const UINT* destination_sizes, UINT source_ranges, const D3D12_CPU_DESCRIPTOR_HANDLE* source_starts,
    const UINT* source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE) {
    UINT destination_range = 0;
    UINT destination_index = 0;
    for (UINT source_range = 0; source_range < source_ranges; ++source_range) {
        const UINT count = source_sizes != nullptr ? source_sizes[source_range] : 1u;
        for (UINT i = 0; i < count; ++i) {
            while (destination_range < destination_ranges &&
                   destination_index >= (destination_sizes != nullptr ? destination_sizes[destination_range] : 1u)) {
                ++destination_range;
                destination_index = 0;
            }
            if (destination_range >= destination_ranges) return;
            const Descriptor* source = descriptor_at(source_starts[source_range]) + i;
            Descriptor* destination = descriptor_at(destination_starts[destination_range]) + destination_index;
            *destination = *source;
            ++destination_index;
        }
    }
}

void Device::CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE destination, D3D12_CPU_DESCRIPTOR_HANDLE source,
    D3D12_DESCRIPTOR_HEAP_TYPE) {
    const Descriptor* from = descriptor_at(source);
    Descriptor* to = descriptor_at(destination);
    if (from == to) return;
    for (UINT i = 0; i < count; ++i) to[i] = from[i];
}

// --- Resources --------------------------------------------------------------------------------------

D3D12_RESOURCE_ALLOCATION_INFO Device::GetResourceAllocationInfo(UINT, UINT count, const D3D12_RESOURCE_DESC* descs) {
    D3D12_RESOURCE_ALLOCATION_INFO info{0, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
    for (UINT i = 0; i < count; ++i) {
        UINT64 total = 0;
        GetCopyableFootprints(&descs[i], 0, std::max<UINT>(1u, descs[i].MipLevels) *
            (descs[i].Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : descs[i].DepthOrArraySize), 0, nullptr,
            nullptr, nullptr, &total);
        if (descs[i].Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) total = descs[i].Width;
        info.SizeInBytes += (total + D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1u) &
            ~UINT64{D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1u};
    }
    return info;
}

D3D12_HEAP_PROPERTIES Device::GetCustomHeapProperties(UINT, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    properties.CPUPageProperty = type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE
        : type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE : D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    return properties;
}

namespace {

HRESULT create_resource(Device& device, const D3D12_HEAP_PROPERTIES& heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC& desc, Resource*& out) {
    auto* resource = new Resource();
    resource->device = &device;
    resource->desc = desc;
    resource->heap_properties = heap;
    resource->heap_flags = flags;
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        const MTLResourceOptions options = MTLResourceStorageModeShared |
            (heap.Type == D3D12_HEAP_TYPE_UPLOAD ? MTLResourceCPUCacheModeWriteCombined : MTLResourceCPUCacheModeDefaultCache);
        resource->buffer = [device.mtl newBufferWithLength:std::max<UINT64>(desc.Width, 4u) options:options];
        if (resource->buffer == nil) {
            resource->Release();
            return E_OUTOFMEMORY;
        }
        resource->gpu_address = device.reserve_address(desc.Width, resource);
        out = resource;
        return S_OK;
    }
    const bool depth_resource = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    resource->format = format_info(desc.Format, depth_resource);
    if (resource->format.pixel == MTLPixelFormatInvalid) {
        std::fprintf(stderr, "[metal12] unsupported texture format %d\n", static_cast<int>(desc.Format));
        resource->Release();
        return E_INVALIDARG;
    }
    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    const unsigned array_size = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : std::max<unsigned>(1u, desc.DepthOrArraySize);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
        descriptor.textureType = MTLTextureType3D;
        descriptor.depth = desc.DepthOrArraySize;
    } else if (desc.SampleDesc.Count > 1u) {
        descriptor.textureType = array_size > 1u ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
        descriptor.sampleCount = desc.SampleDesc.Count;
    } else {
        descriptor.textureType = array_size > 1u ? MTLTextureType2DArray : MTLTextureType2D;
    }
    descriptor.arrayLength = array_size;
    descriptor.pixelFormat = resource->format.pixel;
    descriptor.width = static_cast<NSUInteger>(desc.Width);
    descriptor.height = std::max<NSUInteger>(1u, desc.Height);
    unsigned mips = desc.MipLevels;
    if (mips == 0u) {
        mips = 1u;
        for (UINT64 size = std::max<UINT64>(desc.Width, desc.Height); size > 1u; size >>= 1u) ++mips;
    }
    resource->desc.MipLevels = static_cast<UINT16>(mips);
    descriptor.mipmapLevelCount = mips;
    descriptor.storageMode = MTLStorageModePrivate;
    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0) {
        usage |= MTLTextureUsageRenderTarget;
    }
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) usage |= MTLTextureUsageShaderWrite;
    descriptor.usage = usage;
    resource->texture = [device.mtl newTextureWithDescriptor:descriptor];
    if (resource->texture == nil) {
        resource->Release();
        return E_OUTOFMEMORY;
    }
    out = resource;
    return S_OK;
}

}  // namespace

HRESULT Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID riid, void** result) {
    if (heap == nullptr || desc == nullptr) return E_INVALIDARG;
    Resource* resource = nullptr;
    const HRESULT hr = create_resource(*this, *heap, flags, *desc, resource);
    if (FAILED(hr)) return hr;
    return hand_out(resource, riid, result);
}

HRESULT Device::CreateHeap(const D3D12_HEAP_DESC* desc, REFIID riid, void** result) {
    if (desc == nullptr) return E_INVALIDARG;
    auto* heap = new Heap();
    heap->device = this;
    heap->desc = *desc;
    return hand_out(heap, riid, result);
}

// Placed resources get their own allocation: Metal tracks hazards per resource, and the renderer
// never aliases two live resources in one heap range.
HRESULT Device::CreatePlacedResource(ID3D12Heap* heap, UINT64, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** result) {
    if (heap == nullptr || desc == nullptr) return E_INVALIDARG;
    const D3D12_HEAP_DESC heap_desc = static_cast<Heap*>(heap)->desc;
    return CreateCommittedResource(&heap_desc.Properties, heap_desc.Flags, desc, state, clear, riid, result);
}

HRESULT Device::CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
    void**) {
    unimplemented("reserved (tiled) resources");
    return E_NOTIMPL;
}

HRESULT Device::CreateSharedHandle(ID3D12DeviceChild*, const SECURITY_ATTRIBUTES*, DWORD, LPCWSTR, HANDLE*) {
    return E_NOTIMPL;
}
HRESULT Device::OpenSharedHandle(HANDLE, REFIID, void**) { return E_NOTIMPL; }
HRESULT Device::OpenSharedHandleByName(LPCWSTR, DWORD, HANDLE*) { return E_NOTIMPL; }

void Device::GetCopyableFootprints(const D3D12_RESOURCE_DESC* desc, UINT first, UINT count, UINT64 base,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* layouts, UINT* rows, UINT64* row_sizes, UINT64* total) {
    if (desc == nullptr) return;
    UINT64 offset = base;
    UINT64 end = base;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (layouts != nullptr) {
            layouts[0].Offset = base;
            layouts[0].Footprint = {DXGI_FORMAT_UNKNOWN, static_cast<UINT>(desc->Width), 1, 1,
                static_cast<UINT>((desc->Width + 255u) & ~UINT64{255u})};
        }
        if (rows != nullptr) rows[0] = 1;
        if (row_sizes != nullptr) row_sizes[0] = desc->Width;
        if (total != nullptr) *total = desc->Width;
        return;
    }
    const FormatInfo info = format_info(desc->Format, (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
    const unsigned mips = std::max<unsigned>(1u, desc->MipLevels);
    for (UINT i = 0; i < count; ++i) {
        const unsigned subresource = first + i;
        const unsigned mip = subresource % mips;
        const UINT width = std::max<UINT>(1u, static_cast<UINT>(desc->Width >> mip));
        const UINT height = std::max<UINT>(1u, desc->Height >> mip);
        const UINT block_columns = (width + info.block - 1u) / info.block;
        const UINT block_rows = (height + info.block - 1u) / info.block;
        const UINT64 row_size = UINT64{block_columns} * std::max(1u, info.bytes_per_block);
        const UINT pitch = static_cast<UINT>((row_size + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~UINT64{D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u});
        offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u) & ~UINT64{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u};
        if (layouts != nullptr) {
            layouts[i].Offset = offset;
            layouts[i].Footprint = {desc->Format, width, height, 1u, pitch};
        }
        if (rows != nullptr) rows[i] = block_rows;
        if (row_sizes != nullptr) row_sizes[i] = row_size;
        end = offset + UINT64{pitch} * (block_rows - 1u) + row_size;
        offset += UINT64{pitch} * block_rows;
    }
    if (total != nullptr) *total = end - base;
}

HRESULT Device::CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, REFIID riid, void** result) {
    if (desc == nullptr) return E_INVALIDARG;
    auto* heap = new QueryHeap();
    heap->device = this;
    heap->desc = *desc;
    heap->values.assign(desc->Count, 0u);
    return hand_out(heap, riid, result);
}

HRESULT Device::CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC*, ID3D12RootSignature*, REFIID, void**) {
    unimplemented("command signatures");
    return E_NOTIMPL;
}

// ------------------------------------------------------------------------------------------------
// Resource
// ------------------------------------------------------------------------------------------------

Resource::~Resource() {
    if (gpu_address != 0u && device != nullptr) device->release_address(gpu_address);
}

HRESULT Resource::SetName(LPCWSTR name) {
    debug_name = name != nullptr ? name : L"";
    NSString* label = ns_string(debug_name);
    if (buffer != nil) buffer.label = label;
    if (texture != nil) texture.label = label;
    return S_OK;
}

HRESULT Resource::Map(UINT, const D3D12_RANGE*, void** data) {
    if (buffer == nil) {
        unimplemented("Map on textures");
        return E_INVALIDARG;
    }
    if (data != nullptr) *data = buffer.contents;
    return S_OK;
}

void Resource::Unmap(UINT, const D3D12_RANGE*) {}

HRESULT Resource::WriteToSubresource(UINT subresource, const D3D12_BOX* box, const void* data, UINT row_pitch, UINT depth_pitch) {
    if (texture == nil) return E_INVALIDARG;
    const unsigned mip = subresource % std::max<unsigned>(1u, desc.MipLevels);
    MTLRegion region = MTLRegionMake2D(0, 0, std::max<NSUInteger>(1u, texture.width >> mip), std::max<NSUInteger>(1u, texture.height >> mip));
    if (box != nullptr) region = MTLRegionMake2D(box->left, box->top, box->right - box->left, box->bottom - box->top);
    [texture replaceRegion:region mipmapLevel:mip slice:0 withBytes:data bytesPerRow:row_pitch bytesPerImage:depth_pitch];
    return S_OK;
}

HRESULT Resource::ReadFromSubresource(void*, UINT, UINT, UINT, const D3D12_BOX*) {
    unimplemented("ReadFromSubresource");
    return E_NOTIMPL;
}

HRESULT Resource::GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) {
    if (properties != nullptr) *properties = heap_properties;
    if (flags != nullptr) *flags = heap_flags;
    return S_OK;
}

id<MTLTexture> Resource::view(DXGI_FORMAT view_format, unsigned first_mip, unsigned mip_count, unsigned first_slice,
    unsigned slice_count) {
    if (texture == nil) return nil;
    const unsigned mips = static_cast<unsigned>(texture.mipmapLevelCount);
    first_mip = std::min(first_mip, mips - 1u);
    mip_count = std::clamp(mip_count, 1u, mips - first_mip);
    MTLPixelFormat pixel = format_info(view_format, format.depth).pixel;
    // Depth formats are sampled through the texture itself (the depth plane).
    if (pixel == MTLPixelFormatInvalid || format.depth) pixel = texture.pixelFormat;
    if (view_format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || view_format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) {
        pixel = MTLPixelFormatX32_Stencil8;
    }
    const bool whole = pixel == texture.pixelFormat && first_mip == 0u && mip_count == mips && first_slice == 0u &&
        slice_count >= texture.arrayLength;
    if (whole) return texture;
    const std::uint64_t key = (std::uint64_t{static_cast<std::uint32_t>(pixel)} << 32u) | (first_mip << 24u) |
        (mip_count << 16u) | (first_slice << 8u) | slice_count;
    std::lock_guard lock(views_mutex);
    if (const auto it = views.find(key); it != views.end()) return it->second;
    MTLTextureType type = texture.textureType;
    if (type == MTLTextureType2DArray && slice_count == 1u) type = MTLTextureType2D;
    id<MTLTexture> created = [texture newTextureViewWithPixelFormat:pixel
                                                        textureType:type
                                                             levels:NSMakeRange(first_mip, mip_count)
                                                             slices:NSMakeRange(first_slice, std::max(1u, slice_count))];
    if (created == nil) created = texture;
    views.emplace(key, created);
    return created;
}

// ------------------------------------------------------------------------------------------------
// Fence, pipeline, misc
// ------------------------------------------------------------------------------------------------

namespace {
MTLSharedEventListener* fence_listener() {
    static MTLSharedEventListener* listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_queue_create("nebula.metal12.fence", DISPATCH_QUEUE_CONCURRENT)];
    return listener;
}
}  // namespace

HRESULT Fence::SetEventOnCompletion(UINT64 value, HANDLE event_handle) {
    if (event.signaledValue >= value) {
        if (event_handle != nullptr) SetEvent(event_handle);
        return S_OK;
    }
    if (event_handle != nullptr) {
        [event notifyListener:fence_listener() atValue:value block:^(id<MTLSharedEvent>, uint64_t) { SetEvent(event_handle); }];
        return S_OK;
    }
    // A null event blocks until the fence reaches the value.
    auto state = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
    auto done = std::make_shared<bool>(false);
    [event notifyListener:fence_listener()
                  atValue:value
                    block:^(id<MTLSharedEvent>, uint64_t) {
                        std::lock_guard lock(state->first);
                        *done = true;
                        state->second.notify_all();
                    }];
    std::unique_lock lock(state->first);
    state->second.wait(lock, [&] { return *done; });
    return S_OK;
}

HRESULT Fence::Signal(UINT64 value) {
    event.signaledValue = value;
    return S_OK;
}

HRESULT PipelineState::GetCachedBlob(ID3DBlob** blob) {
    if (blob != nullptr) *blob = nullptr;
    return E_NOTIMPL;
}

// ------------------------------------------------------------------------------------------------
// Root signatures
// ------------------------------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kRootMagic = 0x5352424Eu;  // "NBRS"

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u32(std::uint32_t value) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
        bytes.insert(bytes.end(), data, data + 4);
    }
    void raw(const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }
};

struct Reader {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset = 0;
    bool ok = true;
    std::uint32_t u32() {
        if (offset + 4 > size) {
            ok = false;
            return 0;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, data + offset, 4);
        offset += 4;
        return value;
    }
    void raw(void* destination, std::size_t length) {
        if (offset + length > size) {
            ok = false;
            return;
        }
        std::memcpy(destination, data + offset, length);
        offset += length;
    }
};

std::uint8_t stage_mask(D3D12_SHADER_VISIBILITY visibility) {
    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_VERTEX: return static_cast<std::uint8_t>(StageMask::Vertex);
    case D3D12_SHADER_VISIBILITY_PIXEL: return static_cast<std::uint8_t>(StageMask::Fragment);
    case D3D12_SHADER_VISIBILITY_ALL: return static_cast<std::uint8_t>(StageMask::All);
    default: return static_cast<std::uint8_t>(StageMask::All);  // geometry-visible data reaches the vertex stage
    }
}

}  // namespace

HRESULT parse_root_signature(Device& device, const void* data, std::size_t size, RootSignature& out) {
    Reader reader{static_cast<const std::uint8_t*>(data), size};
    if (reader.u32() != kRootMagic) return E_INVALIDARG;
    const std::uint32_t parameter_count = reader.u32();
    const std::uint32_t sampler_count = reader.u32();
    (void)reader.u32();  // flags
    unsigned next_buffer = 0;
    unsigned next_texture = 0;
    unsigned next_sampler = 0;
    for (std::uint32_t p = 0; p < parameter_count && reader.ok; ++p) {
        RootParameter parameter;
        parameter.type = static_cast<D3D12_ROOT_PARAMETER_TYPE>(reader.u32());
        parameter.stages = stage_mask(static_cast<D3D12_SHADER_VISIBILITY>(reader.u32()));
        parameter.shader_register = reader.u32();
        parameter.space = reader.u32();
        parameter.constants = reader.u32();
        const std::uint32_t range_count = reader.u32();
        switch (parameter.type) {
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            parameter.metal_buffer = next_buffer++;
            out.bindings[{'b', parameter.space, parameter.shader_register}] = parameter.metal_buffer;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            parameter.metal_buffer = next_buffer++;
            out.bindings[{'B', parameter.space, parameter.shader_register}] = parameter.metal_buffer;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            parameter.metal_buffer = next_buffer++;
            out.bindings[{'u', parameter.space, parameter.shader_register}] = parameter.metal_buffer;
            break;
        default:
            break;
        }
        unsigned table_offset = 0;
        for (std::uint32_t r = 0; r < range_count && reader.ok; ++r) {
            RootRange range;
            range.type = static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(reader.u32());
            range.count = reader.u32();
            range.base_register = reader.u32();
            range.space = reader.u32();
            const std::uint32_t offset = reader.u32();
            range.table_offset = offset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND ? table_offset : offset;
            const unsigned count = range.count == static_cast<unsigned>(-1) ? 1u : range.count;
            range.count = count;
            table_offset = range.table_offset + count;
            char kind = 't';
            switch (range.type) {
            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                range.metal_index = next_texture;
                next_texture += count;
                kind = 't';
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
                range.metal_index = next_texture;
                next_texture += count;
                kind = 'u';
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                range.metal_index = next_buffer;
                next_buffer += count;
                kind = 'b';
                break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                range.metal_index = next_sampler;
                next_sampler += count;
                kind = 's';
                break;
            }
            for (unsigned i = 0; i < count; ++i) {
                out.bindings[{kind, range.space, range.base_register + i}] = range.metal_index + i;
            }
            parameter.ranges.push_back(range);
        }
        out.parameters.push_back(std::move(parameter));
    }
    for (std::uint32_t s = 0; s < sampler_count && reader.ok; ++s) {
        D3D12_STATIC_SAMPLER_DESC desc{};
        reader.raw(&desc, sizeof(desc));
        StaticSampler sampler;
        sampler.shader_register = desc.ShaderRegister;
        sampler.space = desc.RegisterSpace;
        sampler.stages = stage_mask(desc.ShaderVisibility);
        sampler.metal_index = next_sampler++;
        D3D12_SAMPLER_DESC dynamic{};
        dynamic.Filter = desc.Filter;
        dynamic.AddressU = desc.AddressU;
        dynamic.AddressV = desc.AddressV;
        dynamic.AddressW = desc.AddressW;
        dynamic.MipLODBias = desc.MipLODBias;
        dynamic.MaxAnisotropy = desc.MaxAnisotropy;
        dynamic.ComparisonFunc = desc.ComparisonFunc;
        if (desc.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK) {
            dynamic.BorderColor[3] = 1.0f;
        } else if (desc.BorderColor == D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE) {
            dynamic.BorderColor[0] = dynamic.BorderColor[1] = dynamic.BorderColor[2] = dynamic.BorderColor[3] = 1.0f;
        }
        dynamic.MinLOD = desc.MinLOD;
        dynamic.MaxLOD = desc.MaxLOD;
        sampler.sampler = device.sampler_for(dynamic);
        out.bindings[{'s', sampler.space, sampler.shader_register}] = sampler.metal_index;
        out.static_samplers.push_back(sampler);
    }
    if (!reader.ok || next_buffer > kFirstReservedBuffer || next_sampler > 16u ||
        next_texture > 128u) {
        return E_INVALIDARG;
    }
    out.hash = fnv1a64(data, size);
    return S_OK;
}

}  // namespace galaxy::metal12

using namespace galaxy::metal12;

HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob** blob, ID3DBlob** error) {
    if (error != nullptr) *error = nullptr;
    if (desc == nullptr || blob == nullptr) return E_INVALIDARG;
    Writer writer;
    writer.u32(kRootMagic);
    writer.u32(desc->NumParameters);
    writer.u32(desc->NumStaticSamplers);
    writer.u32(desc->Flags);
    for (UINT p = 0; p < desc->NumParameters; ++p) {
        const D3D12_ROOT_PARAMETER& parameter = desc->pParameters[p];
        writer.u32(parameter.ParameterType);
        writer.u32(parameter.ShaderVisibility);
        switch (parameter.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            writer.u32(0);
            writer.u32(0);
            writer.u32(0);
            writer.u32(parameter.DescriptorTable.NumDescriptorRanges);
            for (UINT r = 0; r < parameter.DescriptorTable.NumDescriptorRanges; ++r) {
                const D3D12_DESCRIPTOR_RANGE& range = parameter.DescriptorTable.pDescriptorRanges[r];
                writer.u32(range.RangeType);
                writer.u32(range.NumDescriptors);
                writer.u32(range.BaseShaderRegister);
                writer.u32(range.RegisterSpace);
                writer.u32(range.OffsetInDescriptorsFromTableStart);
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            writer.u32(parameter.Constants.ShaderRegister);
            writer.u32(parameter.Constants.RegisterSpace);
            writer.u32(parameter.Constants.Num32BitValues);
            writer.u32(0);
            break;
        default:
            writer.u32(parameter.Descriptor.ShaderRegister);
            writer.u32(parameter.Descriptor.RegisterSpace);
            writer.u32(0);
            writer.u32(0);
            break;
        }
    }
    for (UINT s = 0; s < desc->NumStaticSamplers; ++s) writer.raw(&desc->pStaticSamplers[s], sizeof(D3D12_STATIC_SAMPLER_DESC));
    *blob = new Blob(std::move(writer.bytes));
    return S_OK;
}

HRESULT WINAPI D3D12CreateDevice(IUnknown*, D3D_FEATURE_LEVEL minimum, REFIID riid, void** result) {
    if (minimum > D3D_FEATURE_LEVEL_12_0) return E_FAIL;
    if (MTLCreateSystemDefaultDevice() == nil) return E_FAIL;
    if (result == nullptr) return S_FALSE;
    auto* device = new Device();
    const HRESULT hr = device->QueryInterface(riid, result);
    device->Release();
    return hr;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID, void** result) {
    if (result != nullptr) *result = nullptr;
    return E_NOINTERFACE;
}
