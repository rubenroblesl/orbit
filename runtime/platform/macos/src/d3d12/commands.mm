// SPDX-License-Identifier: GPL-3.0-only
//
// Command lists and queues. A list records commands on the CPU; ExecuteCommandLists encodes them
// into one Metal command buffer: render passes open at the first draw after the render targets
// change, full clears become the load actions of the next pass on that target (or a clear-only
// pass), rectangle clears draw a quad, and copies go through blit encoders. Metal tracks hazards
// between encoders, so resource barriers need no encoding.

#include "metal12.h"

#include <mach/mach_time.h>

#include <cstring>
#include <variant>

namespace galaxy::metal12 {

// --- Recorded commands ---------------------------------------------------------------------------

namespace cmd {
struct Draw {
    UINT count, instances, first, first_instance;
    INT base_vertex;
    bool indexed;
};
struct CopyBuffer {
    Resource* destination;
    UINT64 destination_offset;
    Resource* source;
    UINT64 source_offset;
    UINT64 bytes;
};
struct CopyTexture {
    D3D12_TEXTURE_COPY_LOCATION destination, source;
    UINT x, y, z;
    bool has_box;
    D3D12_BOX box;
};
struct CopyResource {
    Resource* destination;
    Resource* source;
};
struct Resolve {
    Resource* destination;
    UINT destination_subresource;
    Resource* source;
    UINT source_subresource;
};
struct Topology {
    D3D12_PRIMITIVE_TOPOLOGY topology;
};
struct Viewport {
    D3D12_VIEWPORT viewport;
};
struct Scissor {
    D3D12_RECT rect;
};
struct BlendFactor {
    float factor[4];
};
struct StencilRef {
    UINT reference;
};
struct Pipeline {
    PipelineState* pipeline;
};
struct ClearState {
    PipelineState* pipeline;
};
struct Root {
    RootSignature* root;
};
struct RootTable {
    UINT index;
    Descriptor* base;
};
struct RootConstants {
    UINT index;
    UINT offset;
    UINT count;
    std::size_t pool_offset;
};
struct RootAddress {
    UINT index;
    D3D12_GPU_VIRTUAL_ADDRESS address;
};
struct IndexBuffer {
    D3D12_INDEX_BUFFER_VIEW view;
};
struct VertexBuffers {
    UINT start, count;
    std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexBuffers> views;
};
struct Targets {
    RenderTargets targets;
};
struct Clear {
    Descriptor view;
    bool depth_view;
    D3D12_CLEAR_FLAGS flags;
    float color[4];
    float depth;
    UINT8 stencil;
    std::vector<D3D12_RECT> rects;
};
struct Timestamp {
    QueryHeap* heap;
    UINT index;
};
struct ResolveQueries {
    QueryHeap* heap;
    UINT first, count;
    Resource* destination;
    UINT64 offset;
};
}  // namespace cmd

struct Command {
    std::variant<cmd::Draw, cmd::CopyBuffer, cmd::CopyTexture, cmd::CopyResource, cmd::Resolve, cmd::Topology,
        cmd::Viewport, cmd::Scissor, cmd::BlendFactor, cmd::StencilRef, cmd::Pipeline, cmd::ClearState, cmd::Root,
        cmd::RootTable, cmd::RootConstants, cmd::RootAddress, cmd::IndexBuffer, cmd::VertexBuffers, cmd::Targets,
        cmd::Clear, cmd::Timestamp, cmd::ResolveQueries>
        payload;
};

namespace {

std::uint64_t now_ns() {
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
}

MTLPrimitiveType primitive_type(D3D12_PRIMITIVE_TOPOLOGY topology) {
    switch (topology) {
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return MTLPrimitiveTypePoint;
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return MTLPrimitiveTypeLine;
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return MTLPrimitiveTypeLineStrip;
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle;
    }
}

struct Subresource {
    unsigned mip = 0;
    unsigned slice = 0;
    unsigned plane = 0;
};

Subresource split_subresource(const Resource& resource, UINT index) {
    const unsigned mips = std::max<unsigned>(1u, resource.desc.MipLevels);
    const unsigned slices = resource.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
        ? 1u : std::max<unsigned>(1u, resource.desc.DepthOrArraySize);
    return {index % mips, (index / mips) % slices, index / (mips * slices)};
}

MTLSize mip_size(id<MTLTexture> texture, unsigned mip) {
    return MTLSizeMake(std::max<NSUInteger>(1u, texture.width >> mip), std::max<NSUInteger>(1u, texture.height >> mip),
        texture.textureType == MTLTextureType3D ? std::max<NSUInteger>(1u, texture.depth >> mip) : 1u);
}

// --- Internal pipelines: rectangle clears ---------------------------------------------------------

constexpr const char* kClearSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct ClearOut { float4 position [[position]]; };
vertex ClearOut clear_vertex(uint id [[vertex_id]], constant float4& values [[buffer(0)]]) {
    const float2 uv = float2((id << 1u) & 2u, id & 2u);
    ClearOut out;
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), values.x, 1.0);
    return out;
}
fragment float4 clear_color(constant float4& color [[buffer(0)]]) { return color; }
fragment void clear_depth() {}
)MSL";

struct ClearPipelines {
    std::mutex mutex;
    id<MTLLibrary> library = nil;
    std::map<std::uint64_t, id<MTLRenderPipelineState>> pipelines;
    std::map<unsigned, id<MTLDepthStencilState>> depth_states;
};

ClearPipelines& clear_pipelines() {
    static ClearPipelines pipelines;
    return pipelines;
}

id<MTLRenderPipelineState> clear_pipeline(Device& device, MTLPixelFormat color, MTLPixelFormat depth, bool stencil,
    NSUInteger samples) {
    ClearPipelines& cache = clear_pipelines();
    std::lock_guard lock(cache.mutex);
    if (cache.library == nil) {
        NSError* error = nil;
        cache.library = [device.mtl newLibraryWithSource:[NSString stringWithUTF8String:kClearSource] options:nil error:&error];
        if (cache.library == nil) return nil;
    }
    const std::uint64_t key = (std::uint64_t{color} << 32u) | (std::uint64_t{depth} << 8u) | (stencil ? 0x80u : 0u) | samples;
    if (const auto it = cache.pipelines.find(key); it != cache.pipelines.end()) return it->second;
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [cache.library newFunctionWithName:@"clear_vertex"];
    descriptor.fragmentFunction = [cache.library newFunctionWithName:color != MTLPixelFormatInvalid ? @"clear_color" : @"clear_depth"];
    descriptor.colorAttachments[0].pixelFormat = color;
    if (depth != MTLPixelFormatInvalid) {
        descriptor.depthAttachmentPixelFormat = depth;
        if (stencil) descriptor.stencilAttachmentPixelFormat = depth;
    }
    descriptor.rasterSampleCount = samples;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [device.mtl newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline != nil) cache.pipelines.emplace(key, pipeline);
    return pipeline;
}

id<MTLDepthStencilState> clear_depth_state(Device& device, bool depth, bool stencil) {
    ClearPipelines& cache = clear_pipelines();
    std::lock_guard lock(cache.mutex);
    const unsigned key = (depth ? 1u : 0u) | (stencil ? 2u : 0u);
    if (const auto it = cache.depth_states.find(key); it != cache.depth_states.end()) return it->second;
    MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
    descriptor.depthCompareFunction = MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = depth;
    if (stencil) {
        MTLStencilDescriptor* face = [[MTLStencilDescriptor alloc] init];
        face.stencilCompareFunction = MTLCompareFunctionAlways;
        face.depthStencilPassOperation = MTLStencilOperationReplace;
        face.writeMask = 0xFFu;
        descriptor.frontFaceStencil = face;
        descriptor.backFaceStencil = face;
    }
    id<MTLDepthStencilState> state = [device.mtl newDepthStencilStateWithDescriptor:descriptor];
    cache.depth_states.emplace(key, state);
    return state;
}

// --- Encoder ---------------------------------------------------------------------------------------

struct PendingClear {
    id<MTLTexture> texture = nil;
    bool color = false;
    float color_value[4]{};
    bool depth = false;
    float depth_value = 0.0f;
    bool stencil = false;
    std::uint8_t stencil_value = 0;
};

constexpr unsigned kMaxRootParameters = 64;

class Encoder {
public:
    Encoder(Device& device, id<MTLCommandBuffer> command_buffer, const std::vector<std::uint32_t>& constant_pool)
        : device_(device), command_buffer_(command_buffer), constant_pool_(constant_pool) {}

    void run(const std::vector<Command>& commands) {
        for (const Command& command : commands) {
            std::visit([this](const auto& payload) { execute(payload); }, command.payload);
        }
        flush_clears();
        end_encoders();
    }

private:
    // --- Encoders ---

    void end_render() {
        if (render_ != nil) {
            [render_ endEncoding];
            render_ = nil;
        }
    }
    void end_blit() {
        if (blit_ != nil) {
            [blit_ endEncoding];
            blit_ = nil;
        }
    }
    void end_encoders() {
        end_render();
        end_blit();
    }
    id<MTLBlitCommandEncoder> blit() {
        end_render();
        if (blit_ == nil) blit_ = [command_buffer_ blitCommandEncoder];
        return blit_;
    }

    static bool format_has_depth(MTLPixelFormat format) {
        return format == MTLPixelFormatDepth32Float || format == MTLPixelFormatDepth16Unorm ||
            format == MTLPixelFormatDepth32Float_Stencil8;
    }
    static bool format_has_stencil(MTLPixelFormat format) {
        return format == MTLPixelFormatDepth32Float_Stencil8 || format == MTLPixelFormatX32_Stencil8 ||
            format == MTLPixelFormatStencil8;
    }

    PendingClear* pending_for(id<MTLTexture> texture) {
        for (PendingClear& clear : pending_) {
            if (clear.texture == texture) return &clear;
        }
        return nullptr;
    }

    void flush_clears() {
        if (pending_.empty()) return;
        std::vector<PendingClear> pending;
        pending.swap(pending_);
        end_encoders();
        for (const PendingClear& clear : pending) {
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            if (clear.color) {
                pass.colorAttachments[0].texture = clear.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor =
                    MTLClearColorMake(clear.color_value[0], clear.color_value[1], clear.color_value[2], clear.color_value[3]);
            } else {
                attach_depth(pass, clear.texture, &clear);
            }
            [[command_buffer_ renderCommandEncoderWithDescriptor:pass] endEncoding];
        }
    }

    void attach_depth(MTLRenderPassDescriptor* pass, id<MTLTexture> texture, const PendingClear* clear) {
        const MTLPixelFormat format = texture.pixelFormat;
        if (format_has_depth(format)) {
            pass.depthAttachment.texture = texture;
            pass.depthAttachment.loadAction = clear != nullptr && clear->depth ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            if (clear != nullptr) pass.depthAttachment.clearDepth = clear->depth_value;
        }
        if (format_has_stencil(format)) {
            pass.stencilAttachment.texture = texture;
            pass.stencilAttachment.loadAction = clear != nullptr && clear->stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            if (clear != nullptr) pass.stencilAttachment.clearStencil = clear->stencil_value;
        }
    }

    bool begin_render_pass() {
        end_blit();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        NSUInteger width = 0;
        NSUInteger height = 0;
        bool any = false;
        for (unsigned i = 0; i < targets_.color_count; ++i) {
            id<MTLTexture> texture = targets_.colors[i].texture;
            if (texture == nil) continue;
            pass.colorAttachments[i].texture = texture;
            pass.colorAttachments[i].storeAction = MTLStoreActionStore;
            pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
            if (PendingClear* clear = pending_for(texture); clear != nullptr && clear->color) {
                pass.colorAttachments[i].loadAction = MTLLoadActionClear;
                pass.colorAttachments[i].clearColor =
                    MTLClearColorMake(clear->color_value[0], clear->color_value[1], clear->color_value[2], clear->color_value[3]);
                clear->texture = nil;
            }
            width = texture.width;
            height = texture.height;
            any = true;
        }
        if (targets_.has_depth && targets_.depth.texture != nil) {
            id<MTLTexture> texture = targets_.depth.texture;
            PendingClear* clear = pending_for(texture);
            attach_depth(pass, texture, clear);
            if (clear != nullptr) clear->texture = nil;
            width = texture.width;
            height = texture.height;
            any = true;
        }
        std::erase_if(pending_, [](const PendingClear& clear) { return clear.texture == nil; });
        if (!any) {
            if (!has_viewport_) return false;
            pass.renderTargetWidth = static_cast<NSUInteger>(std::max(1.0f, viewport_.Width));
            pass.renderTargetHeight = static_cast<NSUInteger>(std::max(1.0f, viewport_.Height));
            pass.defaultRasterSampleCount = 1;
            width = pass.renderTargetWidth;
            height = pass.renderTargetHeight;
        }
        // Clears of other targets happen before this pass.
        flush_clears();
        render_ = [command_buffer_ renderCommandEncoderWithDescriptor:pass];
        pass_width_ = width;
        pass_height_ = height;
        pipeline_dirty_ = viewport_dirty_ = scissor_dirty_ = blend_dirty_ = stencil_dirty_ = true;
        vertex_buffers_dirty_ = ~0u;
        root_dirty_ = ~std::uint64_t{0};
        static_samplers_dirty_ = true;
        return true;
    }

    // --- State application ---

    void bind_buffer(std::uint8_t stages, unsigned index, D3D12_GPU_VIRTUAL_ADDRESS address) {
        if (address == 0u) return;
        const Device::BufferSpan span = device_.resolve_address(address);
        if (span.buffer == nil) return;
        if (stages & static_cast<std::uint8_t>(StageMask::Vertex)) [render_ setVertexBuffer:span.buffer offset:span.offset atIndex:index];
        if (stages & static_cast<std::uint8_t>(StageMask::Fragment)) [render_ setFragmentBuffer:span.buffer offset:span.offset atIndex:index];
    }
    void bind_texture(std::uint8_t stages, unsigned index, id<MTLTexture> texture) {
        if (texture == nil) texture = device_.null_texture;
        if (stages & static_cast<std::uint8_t>(StageMask::Vertex)) [render_ setVertexTexture:texture atIndex:index];
        if (stages & static_cast<std::uint8_t>(StageMask::Fragment)) [render_ setFragmentTexture:texture atIndex:index];
    }
    void bind_sampler(std::uint8_t stages, unsigned index, id<MTLSamplerState> sampler) {
        if (sampler == nil) sampler = device_.default_sampler;
        if (stages & static_cast<std::uint8_t>(StageMask::Vertex)) [render_ setVertexSamplerState:sampler atIndex:index];
        if (stages & static_cast<std::uint8_t>(StageMask::Fragment)) [render_ setFragmentSamplerState:sampler atIndex:index];
    }

    void apply_root() {
        if (root_ == nullptr) return;
        if (static_samplers_dirty_) {
            for (const StaticSampler& sampler : root_->static_samplers) bind_sampler(sampler.stages, sampler.metal_index, sampler.sampler);
            static_samplers_dirty_ = false;
        }
        const unsigned count = std::min<unsigned>(static_cast<unsigned>(root_->parameters.size()), kMaxRootParameters);
        for (unsigned p = 0; p < count; ++p) {
            if ((root_dirty_ & (std::uint64_t{1} << p)) == 0u) continue;
            const RootParameter& parameter = root_->parameters[p];
            const RootValue& value = root_values_[p];
            switch (parameter.type) {
            case D3D12_ROOT_PARAMETER_TYPE_CBV:
            case D3D12_ROOT_PARAMETER_TYPE_SRV:
            case D3D12_ROOT_PARAMETER_TYPE_UAV:
                bind_buffer(parameter.stages, parameter.metal_buffer, value.address);
                break;
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
                const NSUInteger length = std::max<NSUInteger>(16u, (parameter.constants * 4u + 15u) & ~NSUInteger{15u});
                if (parameter.stages & static_cast<std::uint8_t>(StageMask::Vertex)) {
                    [render_ setVertexBytes:value.constants.data() length:length atIndex:parameter.metal_buffer];
                }
                if (parameter.stages & static_cast<std::uint8_t>(StageMask::Fragment)) {
                    [render_ setFragmentBytes:value.constants.data() length:length atIndex:parameter.metal_buffer];
                }
                break;
            }
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                if (value.table == nullptr) break;
                for (const RootRange& range : parameter.ranges) {
                    for (unsigned i = 0; i < range.count; ++i) {
                        const Descriptor& descriptor = value.table[range.table_offset + i];
                        switch (range.type) {
                        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
                        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
                            bind_texture(parameter.stages, range.metal_index + i, descriptor.texture);
                            break;
                        case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
                            bind_sampler(parameter.stages, range.metal_index + i, descriptor.sampler);
                            break;
                        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
                            bind_buffer(parameter.stages, range.metal_index + i, descriptor.address);
                            break;
                        }
                    }
                }
                break;
            }
            }
        }
        root_dirty_ = 0u;
    }

    bool apply_state() {
        if (pipeline_dirty_) {
            [render_ setRenderPipelineState:pipeline_->pipeline];
            [render_ setDepthStencilState:pipeline_->depth_stencil];
            [render_ setCullMode:pipeline_->cull];
            [render_ setFrontFacingWinding:pipeline_->winding];
            [render_ setTriangleFillMode:pipeline_->fill];
            [render_ setDepthClipMode:pipeline_->depth_clip];
            [render_ setDepthBias:pipeline_->depth_bias slopeScale:pipeline_->slope_scaled_depth_bias clamp:pipeline_->depth_bias_clamp];
            pipeline_dirty_ = false;
        }
        if (viewport_dirty_ && has_viewport_) {
            [render_ setViewport:MTLViewport{viewport_.TopLeftX, viewport_.TopLeftY, viewport_.Width, viewport_.Height,
                                     viewport_.MinDepth, viewport_.MaxDepth}];
            viewport_dirty_ = false;
        }
        if (scissor_dirty_) {
            const LONG right = std::clamp<LONG>(scissor_.right, 0, static_cast<LONG>(pass_width_));
            const LONG bottom = std::clamp<LONG>(scissor_.bottom, 0, static_cast<LONG>(pass_height_));
            const LONG left = std::clamp<LONG>(scissor_.left, 0, right);
            const LONG top = std::clamp<LONG>(scissor_.top, 0, bottom);
            scissor_empty_ = right <= left || bottom <= top;
            if (!scissor_empty_) {
                [render_ setScissorRect:MTLScissorRect{static_cast<NSUInteger>(left), static_cast<NSUInteger>(top),
                                            static_cast<NSUInteger>(right - left), static_cast<NSUInteger>(bottom - top)}];
            }
            scissor_dirty_ = false;
        }
        if (scissor_empty_) return false;
        if (blend_dirty_) {
            [render_ setBlendColorRed:blend_[0] green:blend_[1] blue:blend_[2] alpha:blend_[3]];
            blend_dirty_ = false;
        }
        if (stencil_dirty_) {
            [render_ setStencilReferenceValue:stencil_reference_];
            stencil_dirty_ = false;
        }
        if (vertex_buffers_dirty_ != 0u) {
            for (unsigned slot = 0; slot < kMaxVertexBuffers; ++slot) {
                if ((vertex_buffers_dirty_ & (1u << slot)) == 0u) continue;
                const D3D12_VERTEX_BUFFER_VIEW& view = vertex_buffers_[slot];
                if (view.BufferLocation == 0u) continue;
                const Device::BufferSpan span = device_.resolve_address(view.BufferLocation);
                if (span.buffer == nil) continue;
                [render_ setVertexBuffer:span.buffer
                                  offset:span.offset
                         attributeStride:std::max<UINT>(4u, view.StrideInBytes)
                                 atIndex:kFirstVertexBufferIndex - slot];
            }
            vertex_buffers_dirty_ = 0u;
        }
        apply_root();
        return true;
    }

    // --- Commands ---

    void execute(const cmd::Draw& draw) {
        if (pipeline_ == nullptr || pipeline_->pipeline == nil) return;
        if (render_ == nil && !begin_render_pass()) return;
        if (!apply_state()) return;
        if (pipeline_->expansion != GeometryExpansion::None) {
            draw_expanded(draw);
            return;
        }
        const MTLPrimitiveType type = primitive_type(topology_);
        if (!draw.indexed) {
            [render_ drawPrimitives:type
                        vertexStart:draw.first
                        vertexCount:draw.count
                      instanceCount:draw.instances
                       baseInstance:draw.first_instance];
            return;
        }
        if (index_buffer_.BufferLocation == 0u) return;
        const Device::BufferSpan span = device_.resolve_address(index_buffer_.BufferLocation);
        if (span.buffer == nil) return;
        const bool wide = index_buffer_.Format == DXGI_FORMAT_R32_UINT;
        const NSUInteger offset = span.offset + NSUInteger{draw.first} * (wide ? 4u : 2u);
        [render_ drawIndexedPrimitives:type
                            indexCount:draw.count
                             indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                           indexBuffer:span.buffer
                     indexBufferOffset:offset
                         instanceCount:draw.instances
                            baseVertex:draw.base_vertex
                          baseInstance:draw.first_instance];
    }

    // Lines and points of a geometry-expansion pipeline: six vertices per primitive, fetched by the
    // expansion function from slot 0 and the index buffer (see GeometryExpansion).
    void draw_expanded(const cmd::Draw& draw) {
        const D3D12_VERTEX_BUFFER_VIEW& view = vertex_buffers_[0];
        const Device::BufferSpan vertices = device_.resolve_address(view.BufferLocation);
        if (vertices.buffer == nil) return;
        ExpansionParams params;
        params.first = draw.first;
        params.base_vertex = draw.base_vertex;
        params.indexed = draw.indexed ? 1u : 0u;
        params.stride = view.StrideInBytes;
        if (draw.indexed) {
            const Device::BufferSpan indices = device_.resolve_address(index_buffer_.BufferLocation);
            if (indices.buffer == nil) return;
            params.index32 = index_buffer_.Format == DXGI_FORMAT_R32_UINT ? 1u : 0u;
            [render_ setVertexBuffer:indices.buffer offset:indices.offset atIndex:kExpansionIndexBuffer];
        } else {
            // The function declares the index buffer; any buffer satisfies the binding.
            [render_ setVertexBuffer:vertices.buffer offset:vertices.offset atIndex:kExpansionIndexBuffer];
        }
        [render_ setVertexBuffer:vertices.buffer offset:vertices.offset atIndex:kExpansionVertexBuffer];
        [render_ setVertexBytes:&params length:sizeof(params) atIndex:kExpansionParamsBuffer];
        const unsigned per_primitive = pipeline_->expansion == GeometryExpansion::Lines ? 2u : 1u;
        const NSUInteger primitives = draw.count / per_primitive;
        if (primitives == 0u) return;
        [render_ drawPrimitives:MTLPrimitiveTypeTriangle
                    vertexStart:0
                    vertexCount:primitives * 6u
                  instanceCount:draw.instances
                   baseInstance:draw.first_instance];
    }

    void execute(const cmd::CopyBuffer& copy) {
        if (copy.destination == nullptr || copy.source == nullptr || copy.bytes == 0u) return;
        flush_clears();
        [blit() copyFromBuffer:copy.source->buffer
                  sourceOffset:copy.source_offset
                      toBuffer:copy.destination->buffer
             destinationOffset:copy.destination_offset
                          size:copy.bytes];
    }

    void execute(const cmd::CopyTexture& copy) {
        auto* destination = static_cast<Resource*>(copy.destination.pResource);
        auto* source = static_cast<Resource*>(copy.source.pResource);
        if (destination == nullptr || source == nullptr) return;
        flush_clears();
        if (copy.source.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
            copy.destination.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
            if (destination->texture == nil) return;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = copy.source.PlacedFootprint;
            const Subresource sub = split_subresource(*destination, copy.destination.SubresourceIndex);
            const FormatInfo& info = destination->format;
            D3D12_BOX box = copy.has_box ? copy.box
                : D3D12_BOX{0, 0, 0, footprint.Footprint.Width, footprint.Footprint.Height, footprint.Footprint.Depth};
            const MTLSize limit = mip_size(destination->texture, sub.mip);
            const NSUInteger width = std::min<NSUInteger>(box.right - box.left, limit.width - std::min<NSUInteger>(copy.x, limit.width));
            const NSUInteger height = std::min<NSUInteger>(box.bottom - box.top, limit.height - std::min<NSUInteger>(copy.y, limit.height));
            const NSUInteger depth = std::max<NSUInteger>(1u, box.back - box.front);
            if (width == 0u || height == 0u) return;
            const NSUInteger block = std::max(1u, info.block);
            const NSUInteger rows = (footprint.Footprint.Height + block - 1u) / block;
            const NSUInteger offset = footprint.Offset + NSUInteger{box.front} * footprint.Footprint.RowPitch * rows +
                (box.top / block) * footprint.Footprint.RowPitch + (box.left / block) * info.bytes_per_block;
            MTLBlitOption options = MTLBlitOptionNone;
            if (info.depth && info.stencil) options = sub.plane == 0u ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionStencilFromDepthStencil;
            [blit() copyFromBuffer:source->buffer
                      sourceOffset:offset
                 sourceBytesPerRow:footprint.Footprint.RowPitch
               sourceBytesPerImage:footprint.Footprint.RowPitch * rows
                        sourceSize:MTLSizeMake(width, height, depth)
                         toTexture:destination->texture
                  destinationSlice:sub.slice
                  destinationLevel:sub.mip
                 destinationOrigin:MTLOriginMake(copy.x, copy.y, copy.z)
                           options:options];
            return;
        }
        if (copy.source.Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
            copy.destination.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
            if (source->texture == nil) return;
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = copy.destination.PlacedFootprint;
            const Subresource sub = split_subresource(*source, copy.source.SubresourceIndex);
            const FormatInfo& info = source->format;
            const MTLSize limit = mip_size(source->texture, sub.mip);
            D3D12_BOX box = copy.has_box ? copy.box
                : D3D12_BOX{0, 0, 0, static_cast<UINT>(limit.width), static_cast<UINT>(limit.height), static_cast<UINT>(limit.depth)};
            box.right = std::min<UINT>(box.right, static_cast<UINT>(limit.width));
            box.bottom = std::min<UINT>(box.bottom, static_cast<UINT>(limit.height));
            if (box.right <= box.left || box.bottom <= box.top) return;
            const NSUInteger block = std::max(1u, info.block);
            const NSUInteger rows = (footprint.Footprint.Height + block - 1u) / block;
            const NSUInteger offset = footprint.Offset + NSUInteger{copy.z} * footprint.Footprint.RowPitch * rows +
                (copy.y / block) * footprint.Footprint.RowPitch + (copy.x / block) * info.bytes_per_block;
            MTLBlitOption options = MTLBlitOptionNone;
            if (info.depth && info.stencil) options = sub.plane == 0u ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionStencilFromDepthStencil;
            [blit() copyFromTexture:source->texture
                         sourceSlice:sub.slice
                         sourceLevel:sub.mip
                        sourceOrigin:MTLOriginMake(box.left, box.top, box.front)
                          sourceSize:MTLSizeMake(box.right - box.left, box.bottom - box.top, std::max<UINT>(1u, box.back - box.front))
                            toBuffer:destination->buffer
                   destinationOffset:offset
              destinationBytesPerRow:footprint.Footprint.RowPitch
            destinationBytesPerImage:footprint.Footprint.RowPitch * rows
                             options:options];
            return;
        }
        if (source->texture == nil || destination->texture == nil) return;
        const Subresource from = split_subresource(*source, copy.source.SubresourceIndex);
        const Subresource to = split_subresource(*destination, copy.destination.SubresourceIndex);
        const MTLSize source_limit = mip_size(source->texture, from.mip);
        const MTLSize destination_limit = mip_size(destination->texture, to.mip);
        D3D12_BOX box = copy.has_box ? copy.box
            : D3D12_BOX{0, 0, 0, static_cast<UINT>(source_limit.width), static_cast<UINT>(source_limit.height),
                  static_cast<UINT>(source_limit.depth)};
        const NSUInteger width = std::min<NSUInteger>(std::min<NSUInteger>(box.right, source_limit.width) - box.left,
            destination_limit.width - std::min<NSUInteger>(copy.x, destination_limit.width));
        const NSUInteger height = std::min<NSUInteger>(std::min<NSUInteger>(box.bottom, source_limit.height) - box.top,
            destination_limit.height - std::min<NSUInteger>(copy.y, destination_limit.height));
        if (width == 0u || height == 0u || width > source_limit.width || height > source_limit.height) return;
        [blit() copyFromTexture:source->texture
                    sourceSlice:from.slice
                    sourceLevel:from.mip
                   sourceOrigin:MTLOriginMake(box.left, box.top, box.front)
                     sourceSize:MTLSizeMake(width, height, std::max<UINT>(1u, box.back - box.front))
                      toTexture:destination->texture
               destinationSlice:to.slice
               destinationLevel:to.mip
              destinationOrigin:MTLOriginMake(copy.x, copy.y, copy.z)];
    }

    void execute(const cmd::CopyResource& copy) {
        if (copy.destination == nullptr || copy.source == nullptr) return;
        flush_clears();
        if (copy.source->buffer != nil && copy.destination->buffer != nil) {
            [blit() copyFromBuffer:copy.source->buffer
                      sourceOffset:0
                          toBuffer:copy.destination->buffer
                 destinationOffset:0
                              size:std::min(copy.source->buffer.length, copy.destination->buffer.length)];
        } else if (copy.source->texture != nil && copy.destination->texture != nil) {
            [blit() copyFromTexture:copy.source->texture toTexture:copy.destination->texture];
        }
    }

    void execute(const cmd::Resolve& resolve) {
        if (resolve.destination == nullptr || resolve.source == nullptr || resolve.source->texture == nil ||
            resolve.destination->texture == nil) {
            return;
        }
        flush_clears();
        end_encoders();
        const Subresource from = split_subresource(*resolve.source, resolve.source_subresource);
        const Subresource to = split_subresource(*resolve.destination, resolve.destination_subresource);
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = resolve.source->texture;
        pass.colorAttachments[0].slice = from.slice;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
        pass.colorAttachments[0].resolveTexture = resolve.destination->texture;
        pass.colorAttachments[0].resolveLevel = to.mip;
        pass.colorAttachments[0].resolveSlice = to.slice;
        [[command_buffer_ renderCommandEncoderWithDescriptor:pass] endEncoding];
    }

    void execute(const cmd::Topology& topology) { topology_ = topology.topology; }
    void execute(const cmd::Viewport& viewport) {
        viewport_ = viewport.viewport;
        has_viewport_ = true;
        viewport_dirty_ = true;
    }
    void execute(const cmd::Scissor& scissor) {
        scissor_ = scissor.rect;
        scissor_dirty_ = true;
    }
    void execute(const cmd::BlendFactor& factor) {
        std::memcpy(blend_, factor.factor, sizeof(blend_));
        blend_dirty_ = true;
    }
    void execute(const cmd::StencilRef& reference) {
        stencil_reference_ = reference.reference;
        stencil_dirty_ = true;
    }
    void execute(const cmd::Pipeline& pipeline) {
        if (pipeline.pipeline == pipeline_) return;
        pipeline_ = pipeline.pipeline;
        pipeline_dirty_ = true;
    }
    void execute(const cmd::ClearState& state) {
        pipeline_ = state.pipeline;
        pipeline_dirty_ = true;
        root_ = nullptr;
        topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        index_buffer_ = {};
        vertex_buffers_ = {};
        if (targets_.color_count != 0u || targets_.has_depth) end_render();
        targets_ = {};
    }
    void execute(const cmd::Root& root) {
        if (root.root == root_) return;
        root_ = root.root;
        root_dirty_ = ~std::uint64_t{0};
        static_samplers_dirty_ = true;
    }
    void execute(const cmd::RootTable& table) {
        if (table.index >= kMaxRootParameters) return;
        root_values_[table.index].table = table.base;
        root_dirty_ |= std::uint64_t{1} << table.index;
    }
    void execute(const cmd::RootConstants& constants) {
        if (constants.index >= kMaxRootParameters) return;
        auto& values = root_values_[constants.index].constants;
        for (UINT i = 0; i < constants.count && constants.offset + i < values.size(); ++i) {
            values[constants.offset + i] = constant_pool_[constants.pool_offset + i];
        }
        root_dirty_ |= std::uint64_t{1} << constants.index;
    }
    void execute(const cmd::RootAddress& address) {
        if (address.index >= kMaxRootParameters) return;
        root_values_[address.index].address = address.address;
        root_dirty_ |= std::uint64_t{1} << address.index;
    }
    void execute(const cmd::IndexBuffer& buffer) { index_buffer_ = buffer.view; }
    void execute(const cmd::VertexBuffers& buffers) {
        for (UINT i = 0; i < buffers.count && buffers.start + i < kMaxVertexBuffers; ++i) {
            vertex_buffers_[buffers.start + i] = buffers.views[i];
            vertex_buffers_dirty_ |= 1u << (buffers.start + i);
        }
    }
    void execute(const cmd::Targets& targets) {
        bool same = targets.targets.color_count == targets_.color_count && targets.targets.has_depth == targets_.has_depth;
        for (unsigned i = 0; same && i < targets_.color_count; ++i) {
            same = targets.targets.colors[i].texture == targets_.colors[i].texture;
        }
        if (same && targets_.has_depth) same = targets.targets.depth.texture == targets_.depth.texture;
        targets_ = targets.targets;
        if (!same) end_render();
    }

    bool target_bound(id<MTLTexture> texture) const {
        for (unsigned i = 0; i < targets_.color_count; ++i) {
            if (targets_.colors[i].texture == texture) return true;
        }
        return targets_.has_depth && targets_.depth.texture == texture;
    }

    void execute(const cmd::Clear& clear) {
        id<MTLTexture> texture = clear.view.texture;
        if (texture == nil) return;
        bool whole = clear.rects.empty();
        for (const D3D12_RECT& rect : clear.rects) {
            if (rect.left <= 0 && rect.top <= 0 && rect.right >= static_cast<LONG>(texture.width) &&
                rect.bottom >= static_cast<LONG>(texture.height)) {
                whole = true;
            }
        }
        if (whole) {
            if (render_ != nil && target_bound(texture)) end_render();
            PendingClear* pending = pending_for(texture);
            if (pending == nullptr) {
                pending_.push_back(PendingClear{});
                pending = &pending_.back();
                pending->texture = texture;
            }
            if (!clear.depth_view) {
                pending->color = true;
                std::memcpy(pending->color_value, clear.color, sizeof(pending->color_value));
            } else {
                if ((clear.flags & D3D12_CLEAR_FLAG_DEPTH) != 0) {
                    pending->depth = true;
                    pending->depth_value = clear.depth;
                }
                if ((clear.flags & D3D12_CLEAR_FLAG_STENCIL) != 0) {
                    pending->stencil = true;
                    pending->stencil_value = clear.stencil;
                }
            }
            return;
        }
        clear_rectangles(clear);
    }

    // Rectangle clears draw a full-target triangle through the scissor of each rectangle.
    void clear_rectangles(const cmd::Clear& clear) {
        id<MTLTexture> texture = clear.view.texture;
        flush_clears();
        end_encoders();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLPixelFormat color = MTLPixelFormatInvalid;
        MTLPixelFormat depth = MTLPixelFormatInvalid;
        bool clear_depth = false;
        bool clear_stencil = false;
        if (!clear.depth_view) {
            pass.colorAttachments[0].texture = texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            color = texture.pixelFormat;
        } else {
            attach_depth(pass, texture, nullptr);
            depth = texture.pixelFormat;
            clear_depth = (clear.flags & D3D12_CLEAR_FLAG_DEPTH) != 0 && format_has_depth(depth);
            clear_stencil = (clear.flags & D3D12_CLEAR_FLAG_STENCIL) != 0 && format_has_stencil(depth);
        }
        id<MTLRenderPipelineState> pipeline =
            clear_pipeline(device_, color, depth, format_has_stencil(depth), texture.sampleCount);
        if (pipeline == nil) {
            unimplemented("rectangle clear pipeline");
            return;
        }
        id<MTLRenderCommandEncoder> encoder = [command_buffer_ renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        if (clear.depth_view) {
            [encoder setDepthStencilState:clear_depth_state(device_, clear_depth, clear_stencil)];
            [encoder setStencilReferenceValue:clear.stencil];
        }
        const float vertex_values[4] = {clear.depth, 0.0f, 0.0f, 0.0f};
        [encoder setVertexBytes:vertex_values length:sizeof(vertex_values) atIndex:0];
        if (!clear.depth_view) [encoder setFragmentBytes:clear.color length:sizeof(clear.color) atIndex:0];
        for (const D3D12_RECT& rect : clear.rects) {
            const LONG right = std::clamp<LONG>(rect.right, 0, static_cast<LONG>(texture.width));
            const LONG bottom = std::clamp<LONG>(rect.bottom, 0, static_cast<LONG>(texture.height));
            const LONG left = std::clamp<LONG>(rect.left, 0, right);
            const LONG top = std::clamp<LONG>(rect.top, 0, bottom);
            if (right <= left || bottom <= top) continue;
            [encoder setScissorRect:MTLScissorRect{static_cast<NSUInteger>(left), static_cast<NSUInteger>(top),
                                        static_cast<NSUInteger>(right - left), static_cast<NSUInteger>(bottom - top)}];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [encoder endEncoding];
    }

    void execute(const cmd::Timestamp& timestamp) {
        if (timestamp.heap != nullptr && timestamp.index < timestamp.heap->values.size()) {
            timestamp.heap->values[timestamp.index] = now_ns();
        }
    }
    void execute(const cmd::ResolveQueries& resolve) {
        if (resolve.heap == nullptr || resolve.destination == nullptr || resolve.destination->buffer == nil) return;
        const std::size_t count = std::min<std::size_t>(resolve.count, resolve.heap->values.size() - std::min<std::size_t>(resolve.first, resolve.heap->values.size()));
        if (resolve.offset + count * 8u > resolve.destination->buffer.length) return;
        std::memcpy(static_cast<std::uint8_t*>(resolve.destination->buffer.contents) + resolve.offset,
            resolve.heap->values.data() + resolve.first, count * 8u);
    }

    struct RootValue {
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;
        Descriptor* table = nullptr;
        std::array<std::uint32_t, 64> constants{};
    };

    Device& device_;
    id<MTLCommandBuffer> command_buffer_;
    const std::vector<std::uint32_t>& constant_pool_;
    id<MTLRenderCommandEncoder> render_ = nil;
    id<MTLBlitCommandEncoder> blit_ = nil;
    std::vector<PendingClear> pending_;
    RenderTargets targets_{};
    NSUInteger pass_width_ = 0;
    NSUInteger pass_height_ = 0;
    PipelineState* pipeline_ = nullptr;
    RootSignature* root_ = nullptr;
    std::array<RootValue, kMaxRootParameters> root_values_{};
    std::uint64_t root_dirty_ = 0;
    bool static_samplers_dirty_ = true;
    D3D12_PRIMITIVE_TOPOLOGY topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_VIEWPORT viewport_{};
    bool has_viewport_ = false;
    D3D12_RECT scissor_{};
    bool scissor_empty_ = true;
    float blend_[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    UINT stencil_reference_ = 0;
    D3D12_INDEX_BUFFER_VIEW index_buffer_{};
    std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexBuffers> vertex_buffers_{};
    unsigned vertex_buffers_dirty_ = 0;
    bool pipeline_dirty_ = true;
    bool viewport_dirty_ = true;
    bool scissor_dirty_ = true;
    bool blend_dirty_ = true;
    bool stencil_dirty_ = true;
};

}  // namespace

// --- Command list recording -------------------------------------------------------------------------

CommandList::CommandList() = default;
CommandList::~CommandList() = default;

HRESULT CommandList::Close() {
    if (closed) return E_FAIL;
    closed = true;
    return S_OK;
}

HRESULT CommandList::Reset(ID3D12CommandAllocator*, ID3D12PipelineState* initial) {
    commands.clear();
    keep_alive.clear();
    constant_pool.clear();
    closed = false;
    if (initial != nullptr) commands.push_back({cmd::ClearState{static_cast<PipelineState*>(initial)}});
    return S_OK;
}

void CommandList::ClearState(ID3D12PipelineState* pipeline) {
    commands.push_back({cmd::ClearState{static_cast<PipelineState*>(pipeline)}});
}

void CommandList::DrawInstanced(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
    if (vertices == 0u || instances == 0u) return;
    commands.push_back({cmd::Draw{vertices, instances, first_vertex, first_instance, 0, false}});
}

void CommandList::DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex, UINT first_instance) {
    if (indices == 0u || instances == 0u) return;
    commands.push_back({cmd::Draw{indices, instances, first_index, first_instance, base_vertex, true}});
}

void CommandList::Dispatch(UINT, UINT, UINT) { unimplemented("Dispatch"); }

void CommandList::CopyBufferRegion(ID3D12Resource* destination, UINT64 destination_offset, ID3D12Resource* source,
    UINT64 source_offset, UINT64 bytes) {
    commands.push_back({cmd::CopyBuffer{static_cast<Resource*>(destination), destination_offset,
        static_cast<Resource*>(source), source_offset, bytes}});
}

void CommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* destination, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* source, const D3D12_BOX* box) {
    if (destination == nullptr || source == nullptr) return;
    cmd::CopyTexture copy{*destination, *source, x, y, z, box != nullptr, box != nullptr ? *box : D3D12_BOX{}};
    commands.push_back({copy});
}

void CommandList::CopyResource(ID3D12Resource* destination, ID3D12Resource* source) {
    commands.push_back({cmd::CopyResource{static_cast<Resource*>(destination), static_cast<Resource*>(source)}});
}

void CommandList::CopyTiles(ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
    ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS) {
    unimplemented("CopyTiles");
}

void CommandList::ResolveSubresource(ID3D12Resource* destination, UINT destination_subresource, ID3D12Resource* source,
    UINT source_subresource, DXGI_FORMAT) {
    commands.push_back({cmd::Resolve{static_cast<Resource*>(destination), destination_subresource,
        static_cast<Resource*>(source), source_subresource}});
}

void CommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    commands.push_back({cmd::Topology{topology}});
}

void CommandList::RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
    if (count == 0u || viewports == nullptr) return;
    commands.push_back({cmd::Viewport{viewports[0]}});
}

void CommandList::RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
    if (count == 0u || rects == nullptr) return;
    commands.push_back({cmd::Scissor{rects[0]}});
}

void CommandList::OMSetBlendFactor(const FLOAT factor[4]) {
    cmd::BlendFactor blend{{1.0f, 1.0f, 1.0f, 1.0f}};
    if (factor != nullptr) std::memcpy(blend.factor, factor, sizeof(blend.factor));
    commands.push_back({blend});
}

void CommandList::OMSetStencilRef(UINT reference) { commands.push_back({cmd::StencilRef{reference}}); }

void CommandList::SetPipelineState(ID3D12PipelineState* pipeline) {
    commands.push_back({cmd::Pipeline{static_cast<PipelineState*>(pipeline)}});
}

void CommandList::ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER*) {}

void CommandList::ExecuteBundle(ID3D12GraphicsCommandList*) { unimplemented("ExecuteBundle"); }

void CommandList::SetComputeRootSignature(ID3D12RootSignature*) {}
void CommandList::SetComputeRootDescriptorTable(UINT, D3D12_GPU_DESCRIPTOR_HANDLE) {}
void CommandList::SetComputeRoot32BitConstant(UINT, UINT, UINT) {}
void CommandList::SetComputeRoot32BitConstants(UINT, UINT, const void*, UINT) {}
void CommandList::SetComputeRootConstantBufferView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}
void CommandList::SetComputeRootShaderResourceView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}
void CommandList::SetComputeRootUnorderedAccessView(UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}

void CommandList::SetGraphicsRootSignature(ID3D12RootSignature* root) {
    commands.push_back({cmd::Root{static_cast<RootSignature*>(root)}});
}

void CommandList::SetGraphicsRootDescriptorTable(UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base) {
    commands.push_back({cmd::RootTable{index, descriptor_at(base)}});
}

void CommandList::SetGraphicsRoot32BitConstant(UINT index, UINT value, UINT offset) {
    SetGraphicsRoot32BitConstants(index, 1, &value, offset);
}

void CommandList::SetGraphicsRoot32BitConstants(UINT index, UINT count, const void* data, UINT offset) {
    if (count == 0u || data == nullptr) return;
    std::vector<std::uint32_t>& pool = constant_pool;
    const std::size_t at = pool.size();
    pool.resize(at + count);
    std::memcpy(pool.data() + at, data, std::size_t{count} * 4u);
    commands.push_back({cmd::RootConstants{index, offset, count, at}});
}

void CommandList::SetGraphicsRootConstantBufferView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    commands.push_back({cmd::RootAddress{index, address}});
}
void CommandList::SetGraphicsRootShaderResourceView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    commands.push_back({cmd::RootAddress{index, address}});
}
void CommandList::SetGraphicsRootUnorderedAccessView(UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    commands.push_back({cmd::RootAddress{index, address}});
}

void CommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
    commands.push_back({cmd::IndexBuffer{view != nullptr ? *view : D3D12_INDEX_BUFFER_VIEW{}}});
}

void CommandList::IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
    cmd::VertexBuffers buffers{start, std::min<UINT>(count, kMaxVertexBuffers), {}};
    for (UINT i = 0; i < buffers.count; ++i) buffers.views[i] = views != nullptr ? views[i] : D3D12_VERTEX_BUFFER_VIEW{};
    commands.push_back({buffers});
}

void CommandList::SOSetTargets(UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*) { unimplemented("stream output"); }

void CommandList::OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* colors, BOOL single_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    cmd::Targets targets{};
    targets.targets.color_count = std::min<UINT>(count, 8u);
    for (UINT i = 0; i < targets.targets.color_count; ++i) {
        const Descriptor* descriptor = single_range ? descriptor_at(colors[0]) + i : descriptor_at(colors[i]);
        targets.targets.colors[i] = *descriptor;
    }
    if (depth != nullptr && depth->ptr != 0u) {
        targets.targets.depth = *descriptor_at(*depth);
        targets.targets.has_depth = true;
    }
    commands.push_back({targets});
}

void CommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE view, D3D12_CLEAR_FLAGS flags, FLOAT depth,
    UINT8 stencil, UINT rect_count, const D3D12_RECT* rects) {
    cmd::Clear clear{*descriptor_at(view), true, flags, {}, depth, stencil, {}};
    if (rects != nullptr) clear.rects.assign(rects, rects + rect_count);
    commands.push_back({std::move(clear)});
}

void CommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE view, const FLOAT color[4], UINT rect_count,
    const D3D12_RECT* rects) {
    cmd::Clear clear{*descriptor_at(view), false, D3D12_CLEAR_FLAGS{}, {color[0], color[1], color[2], color[3]}, 0.0f, 0, {}};
    if (rects != nullptr) clear.rects.assign(rects, rects + rect_count);
    commands.push_back({std::move(clear)});
}

void CommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*,
    const UINT[4], UINT, const D3D12_RECT*) {
    unimplemented("ClearUnorderedAccessViewUint");
}

void CommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12Resource*,
    const FLOAT[4], UINT, const D3D12_RECT*) {
    unimplemented("ClearUnorderedAccessViewFloat");
}

void CommandList::EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
    if (type != D3D12_QUERY_TYPE_TIMESTAMP) {
        unimplemented("queries other than timestamps");
        return;
    }
    commands.push_back({cmd::Timestamp{static_cast<QueryHeap*>(heap), index}});
}

void CommandList::ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE, UINT first, UINT count,
    ID3D12Resource* destination, UINT64 offset) {
    commands.push_back({cmd::ResolveQueries{static_cast<QueryHeap*>(heap), first, count,
        static_cast<Resource*>(destination), offset}});
}

void CommandList::ExecuteIndirect(ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64) {
    unimplemented("ExecuteIndirect");
}

void encode_command_list(CommandQueue& queue, CommandList& list, id<MTLCommandBuffer> command_buffer) {
    Encoder encoder(*queue.device, command_buffer, list.constant_pool);
    encoder.run(list.commands);
}

// --- Command queue ------------------------------------------------------------------------------------------

void CommandQueue::ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists) {
    std::lock_guard lock(submit_mutex);
    @autoreleasepool {
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        for (UINT i = 0; i < count; ++i) {
            auto* list = static_cast<CommandList*>(static_cast<ID3D12GraphicsCommandList*>(lists[i]));
            if (list != nullptr) encode_command_list(*this, *list, command_buffer);
        }
        [command_buffer commit];
    }
}

HRESULT CommandQueue::Signal(ID3D12Fence* fence, UINT64 value) {
    if (fence == nullptr) return E_INVALIDARG;
    std::lock_guard lock(submit_mutex);
    @autoreleasepool {
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        [command_buffer encodeSignalEvent:static_cast<Fence*>(fence)->event value:value];
        [command_buffer commit];
    }
    return S_OK;
}

HRESULT CommandQueue::Wait(ID3D12Fence* fence, UINT64 value) {
    if (fence == nullptr) return E_INVALIDARG;
    std::lock_guard lock(submit_mutex);
    @autoreleasepool {
        id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
        [command_buffer encodeWaitForEvent:static_cast<Fence*>(fence)->event value:value];
        [command_buffer commit];
    }
    return S_OK;
}

HRESULT CommandQueue::GetTimestampFrequency(UINT64* frequency) {
    if (frequency == nullptr) return E_INVALIDARG;
    *frequency = 1000000000ull;
    return S_OK;
}

HRESULT CommandQueue::GetClockCalibration(UINT64* gpu, UINT64* cpu) {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    if (gpu != nullptr) *gpu = now_ns();
    if (cpu != nullptr) *cpu = static_cast<UINT64>(counter.QuadPart);
    return S_OK;
}

}  // namespace galaxy::metal12
