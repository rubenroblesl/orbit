// SPDX-License-Identifier: GPL-3.0-only
//
// DXGI on Metal: one adapter (the system GPU) with one output (the main display), and swap chains
// that present into the CAMetalLayer of the window's content view. Back buffers are ordinary
// render-target textures; Present draws the current one into the next drawable (scaling when the
// drawable and buffer sizes differ, converting RGBA to the layer's BGRA) and presents it. The
// frame-latency waitable object is a semaphore released as each present completes.

#include "metal12.h"

#include <CoreGraphics/CoreGraphics.h>

#include <cstring>
#include <thread>

namespace galaxy::metal12 {

namespace {

template <typename Interface, typename... Bases>
class DxgiObject : public Unknown<Interface, Bases...> {
public:
    HRESULT GetParent(REFIID riid, void** parent) override {
        if (parent_ == nullptr) {
            if (parent != nullptr) *parent = nullptr;
            return E_NOINTERFACE;
        }
        return parent_->QueryInterface(riid, parent);
    }

protected:
    Microsoft::WRL::ComPtr<IUnknown> parent_;
};

// --- Output -----------------------------------------------------------------------------------------

class Output final : public DxgiObject<IDXGIOutput, IDXGIObject> {
public:
    explicit Output(IUnknown* adapter) { parent_ = adapter; }

    HRESULT GetDesc(DXGI_OUTPUT_DESC* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        const HMONITOR monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
            desc->DesktopCoordinates = info.rcMonitor;
            std::wcsncpy(desc->DeviceName, info.szDevice, 31);
        } else {
            std::wcsncpy(desc->DeviceName, L"\\\\.\\DISPLAY1", 31);
        }
        desc->AttachedToDesktop = TRUE;
        desc->Rotation = DXGI_MODE_ROTATION_IDENTITY;
        desc->Monitor = monitor;
        return S_OK;
    }

    HRESULT FindClosestMatchingMode(const DXGI_MODE_DESC* mode, DXGI_MODE_DESC* closest, IUnknown*) override {
        if (mode == nullptr || closest == nullptr) return E_INVALIDARG;
        *closest = *mode;
        const double refresh = refresh_hz();
        closest->RefreshRate = {static_cast<UINT>(std::lround(refresh * 1000.0)), 1000u};
        if (closest->Format == DXGI_FORMAT_UNKNOWN) closest->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return S_OK;
    }

    HRESULT WaitForVBlank() override {
        const double period = 1.0 / std::max(1.0, refresh_hz());
        const double now = CACurrentMediaTime();
        const double next = (std::floor(now / period) + 1.0) * period;
        std::this_thread::sleep_for(std::chrono::duration<double>(next - now));
        return S_OK;
    }

    HRESULT GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) override {
        if (statistics != nullptr) *statistics = {};
        return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    }

    static double refresh_hz() {
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
        double hz = mode != nullptr ? CGDisplayModeGetRefreshRate(mode) : 0.0;
        if (mode != nullptr) CGDisplayModeRelease(mode);
        return hz > 0.0 ? hz : 60.0;
    }
};

// --- Adapter ----------------------------------------------------------------------------------------

constexpr LUID kAdapterLuid{0x4D45544Cu, 0};

class Adapter final : public DxgiObject<IDXGIAdapter3, IDXGIAdapter2, IDXGIAdapter1, IDXGIAdapter, IDXGIObject> {
public:
    explicit Adapter(IUnknown* factory) : device_(MTLCreateSystemDefaultDevice()) { parent_ = factory; }

    HRESULT EnumOutputs(UINT index, IDXGIOutput** output) override {
        if (output == nullptr) return E_INVALIDARG;
        *output = nullptr;
        if (index != 0u) return DXGI_ERROR_NOT_FOUND;
        *output = new Output(static_cast<IDXGIAdapter3*>(this));
        return S_OK;
    }

    HRESULT GetDesc(DXGI_ADAPTER_DESC* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        DXGI_ADAPTER_DESC1 full{};
        GetDesc1(&full);
        std::memcpy(desc, &full, sizeof(*desc));
        return S_OK;
    }

    HRESULT GetDesc1(DXGI_ADAPTER_DESC1* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        const std::wstring name = galaxy::compat::wide_from_utf8(device_.name.UTF8String);
        std::wcsncpy(desc->Description, name.c_str(), 127);
        desc->VendorId = 0x106Bu;  // Apple
        // Unified memory: the working-set budget is both the "dedicated" and the shared pool.
        desc->DedicatedVideoMemory = static_cast<SIZE_T>(device_.recommendedMaxWorkingSetSize);
        desc->SharedSystemMemory = static_cast<SIZE_T>(device_.recommendedMaxWorkingSetSize);
        desc->AdapterLuid = kAdapterLuid;
        desc->Flags = DXGI_ADAPTER_FLAG_NONE;
        return S_OK;
    }

    HRESULT QueryVideoMemoryInfo(UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO* info) override {
        if (info == nullptr) return E_INVALIDARG;
        *info = {};
        info->Budget = device_.recommendedMaxWorkingSetSize;
        info->CurrentUsage = device_.currentAllocatedSize;
        return S_OK;
    }

private:
    id<MTLDevice> device_;
};

// --- Swap chain -------------------------------------------------------------------------------------

constexpr const char* kPresentSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct PresentOut { float4 position [[position]]; float2 uv; };
vertex PresentOut present_vertex(uint id [[vertex_id]]) {
    const float2 uv = float2((id << 1u) & 2u, id & 2u);
    PresentOut out;
    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    out.uv = uv;
    return out;
}
fragment float4 present_fragment(PresentOut in [[stage_in]], texture2d<float> source [[texture(0)]],
                                 sampler linear [[sampler(0)]]) {
    return float4(source.sample(linear, in.uv).rgb, 1.0);
}
)MSL";

class SwapChain final : public DxgiObject<IDXGISwapChain4, IDXGISwapChain3, IDXGISwapChain2, IDXGISwapChain1,
                            IDXGISwapChain, IDXGIObject> {
public:
    SwapChain(IUnknown* factory, CommandQueue* queue, HWND window, CAMetalLayer* layer, const DXGI_SWAP_CHAIN_DESC1& desc)
        : queue_(queue), window_(window), layer_(layer), desc_(desc) {
        parent_ = factory;
        queue_reference_ = queue;
        device_ = queue->device;
        latency_semaphore_ = CreateSemaphoreW(nullptr, 1, 64, nullptr);
    }

    ~SwapChain() override {
        if (last_present_ != nil) [last_present_ waitUntilCompleted];
    }

    HRESULT initialize() {
        NSError* error = nil;
        id<MTLLibrary> library = [device_->mtl newLibraryWithSource:[NSString stringWithUTF8String:kPresentSource]
                                                            options:nil
                                                              error:&error];
        if (library == nil) return E_FAIL;
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"present_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"present_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline_ = [device_->mtl newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (pipeline_ == nil) return E_FAIL;
        MTLSamplerDescriptor* sampler = [[MTLSamplerDescriptor alloc] init];
        sampler.minFilter = MTLSamplerMinMagFilterLinear;
        sampler.magFilter = MTLSamplerMinMagFilterLinear;
        sampler_ = [device_->mtl newSamplerStateWithDescriptor:sampler];

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer_.device = device_->mtl;
        layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer_.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        layer_.maximumDrawableCount = 3;
        layer_.displaySyncEnabled = YES;
        layer_.allowsNextDrawableTimeout = YES;
        [CATransaction commit];
        return create_buffers();
    }

    // IDXGISwapChain
    HRESULT Present(UINT sync_interval, UINT flags) override {
        if ((flags & DXGI_PRESENT_TEST) != 0u) return S_OK;
        std::lock_guard lock(queue_->submit_mutex);
        @autoreleasepool {
            const bool sync = sync_interval > 0u;
            if (sync != display_sync_) {
                layer_.displaySyncEnabled = sync;
                display_sync_ = sync;
            }
            id<MTLCommandBuffer> command_buffer = [queue_->queue commandBuffer];
            id<CAMetalDrawable> drawable = [layer_ nextDrawable];
            Resource* buffer = buffers_[current_].Get() != nullptr ? static_cast<Resource*>(buffers_[current_].Get()) : nullptr;
            if (drawable != nil && buffer != nullptr) {
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
                [encoder setRenderPipelineState:pipeline_];
                [encoder setFragmentTexture:buffer->texture atIndex:0];
                [encoder setFragmentSamplerState:sampler_ atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [encoder endEncoding];
                [command_buffer presentDrawable:drawable];
            }
            HANDLE semaphore = latency_semaphore_;
            std::atomic<UINT>* completed = &completed_presents_;
            std::atomic<LONGLONG>* completed_qpc = &completed_qpc_;
            [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                completed_qpc->store(now.QuadPart);
                completed->fetch_add(1u);
                ReleaseSemaphore(semaphore, 1, nullptr);
            }];
            [command_buffer commit];
            last_present_ = command_buffer;
        }
        current_ = (current_ + 1u) % std::max(1u, desc_.BufferCount);
        ++present_count_;
        return S_OK;
    }

    HRESULT GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (surface == nullptr) return E_INVALIDARG;
        *surface = nullptr;
        if (index >= buffers_.size()) return DXGI_ERROR_INVALID_CALL;
        return buffers_[index]->QueryInterface(riid, surface);
    }

    HRESULT SetFullscreenState(BOOL fullscreen, IDXGIOutput*) override {
        // Exclusive fullscreen does not exist on macOS; the renderer falls back to a borderless
        // window covering the screen.
        return fullscreen ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : S_OK;
    }

    HRESULT GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) override {
        if (fullscreen != nullptr) *fullscreen = FALSE;
        if (target != nullptr) *target = nullptr;
        return S_OK;
    }

    HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = {};
        desc->BufferDesc.Width = desc_.Width;
        desc->BufferDesc.Height = desc_.Height;
        desc->BufferDesc.Format = desc_.Format;
        desc->SampleDesc = desc_.SampleDesc;
        desc->BufferUsage = desc_.BufferUsage;
        desc->BufferCount = desc_.BufferCount;
        desc->OutputWindow = window_;
        desc->Windowed = TRUE;
        desc->SwapEffect = desc_.SwapEffect;
        desc->Flags = desc_.Flags;
        return S_OK;
    }

    HRESULT ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override {
        for (const auto& buffer : buffers_) {
            // Every outside reference must be gone, as DXGI requires.
            buffer->AddRef();
            if (buffer->Release() > 1u) return DXGI_ERROR_INVALID_CALL;
        }
        if (last_present_ != nil) [last_present_ waitUntilCompleted];
        if (count != 0u) desc_.BufferCount = count;
        if (format != DXGI_FORMAT_UNKNOWN) desc_.Format = format;
        if (width == 0u || height == 0u) {
            int client_width = 0;
            int client_height = 0;
            double scale = 1.0;
            if (galaxy::macos::window_client_pixels(window_, client_width, client_height, scale)) {
                if (width == 0u) width = static_cast<UINT>(std::max(1, client_width));
                if (height == 0u) height = static_cast<UINT>(std::max(1, client_height));
            }
        }
        if (width != 0u) desc_.Width = width;
        if (height != 0u) desc_.Height = height;
        desc_.Flags = flags;
        current_ = 0;
        return create_buffers();
    }

    HRESULT ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }

    HRESULT GetContainingOutput(IDXGIOutput** output) override {
        if (output == nullptr) return E_INVALIDARG;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        if (FAILED(GetParent(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters(0, &adapter))) return E_FAIL;
        return adapter->EnumOutputs(0, output);
    }

    HRESULT GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) override {
        if (statistics == nullptr) return E_INVALIDARG;
        *statistics = {};
        const UINT completed = completed_presents_.load();
        if (completed == 0u) return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
        statistics->PresentCount = completed;
        statistics->PresentRefreshCount = completed;
        statistics->SyncRefreshCount = completed;
        statistics->SyncQPCTime.QuadPart = completed_qpc_.load();
        return S_OK;
    }

    HRESULT GetLastPresentCount(UINT* count) override {
        if (count == nullptr) return E_INVALIDARG;
        *count = present_count_;
        return S_OK;
    }

    // IDXGISwapChain1..3
    HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        if (desc == nullptr) return E_INVALIDARG;
        *desc = desc_;
        return S_OK;
    }

    HRESULT SetMaximumFrameLatency(UINT latency) override {
        if (latency == 0u || latency > 16u) return DXGI_ERROR_INVALID_CALL;
        if (latency > latency_) ReleaseSemaphore(latency_semaphore_, static_cast<LONG>(latency - latency_), nullptr);
        latency_ = latency;
        return S_OK;
    }

    HRESULT GetMaximumFrameLatency(UINT* latency) override {
        if (latency == nullptr) return E_INVALIDARG;
        *latency = latency_;
        return S_OK;
    }

    HANDLE GetFrameLatencyWaitableObject() override { return latency_semaphore_; }

    UINT GetCurrentBackBufferIndex() override { return current_; }

private:
    HRESULT create_buffers() {
        buffers_.clear();
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer_.drawableSize = CGSizeMake(desc_.Width, desc_.Height);
        [CATransaction commit];
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = std::max(1u, desc_.Width);
        desc.Height = std::max(1u, desc_.Height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = desc_.Format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (UINT i = 0; i < std::max(1u, desc_.BufferCount); ++i) {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&buffer));
            if (FAILED(hr)) return hr;
            buffers_.push_back(buffer);
        }
        return S_OK;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_reference_;
    CommandQueue* queue_;
    Device* device_ = nullptr;
    HWND window_;
    CAMetalLayer* layer_;
    DXGI_SWAP_CHAIN_DESC1 desc_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> buffers_;
    UINT current_ = 0;
    UINT present_count_ = 0;
    std::atomic<UINT> completed_presents_{0};
    std::atomic<LONGLONG> completed_qpc_{0};
    UINT latency_ = 1;
    HANDLE latency_semaphore_ = nullptr;
    bool display_sync_ = true;
    id<MTLRenderPipelineState> pipeline_ = nil;
    id<MTLSamplerState> sampler_ = nil;
    id<MTLCommandBuffer> last_present_ = nil;
};

// --- Factory ----------------------------------------------------------------------------------------

class Factory final : public DxgiObject<IDXGIFactory6, IDXGIFactory5, IDXGIFactory4, IDXGIFactory3, IDXGIFactory2,
                          IDXGIFactory1, IDXGIFactory, IDXGIObject> {
public:
    HRESULT EnumAdapters(UINT index, IDXGIAdapter** adapter) override {
        if (adapter == nullptr) return E_INVALIDARG;
        *adapter = nullptr;
        if (index != 0u || MTLCreateSystemDefaultDevice() == nil) return DXGI_ERROR_NOT_FOUND;
        *adapter = new Adapter(static_cast<IDXGIFactory6*>(this));
        return S_OK;
    }

    HRESULT MakeWindowAssociation(HWND, UINT) override { return S_OK; }

    HRESULT EnumAdapters1(UINT index, IDXGIAdapter1** adapter) override {
        if (adapter == nullptr) return E_INVALIDARG;
        IDXGIAdapter* base = nullptr;
        const HRESULT hr = EnumAdapters(index, &base);
        *adapter = static_cast<IDXGIAdapter1*>(static_cast<Adapter*>(base));
        return hr;
    }

    BOOL IsCurrent() override { return TRUE; }

    HRESULT CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1** swap_chain) override {
        if (device == nullptr || desc == nullptr || swap_chain == nullptr) return E_INVALIDARG;
        *swap_chain = nullptr;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return DXGI_ERROR_INVALID_CALL;
        auto* layer = (__bridge CAMetalLayer*)galaxy::macos::window_metal_layer(window);
        if (layer == nil) return DXGI_ERROR_INVALID_CALL;
        DXGI_SWAP_CHAIN_DESC1 effective = *desc;
        if (effective.Width == 0u || effective.Height == 0u) {
            int width = 0;
            int height = 0;
            double scale = 1.0;
            galaxy::macos::window_client_pixels(window, width, height, scale);
            if (effective.Width == 0u) effective.Width = static_cast<UINT>(std::max(1, width));
            if (effective.Height == 0u) effective.Height = static_cast<UINT>(std::max(1, height));
        }
        auto* chain = new SwapChain(static_cast<IDXGIFactory6*>(this), static_cast<CommandQueue*>(queue.Get()), window,
            layer, effective);
        const HRESULT hr = chain->initialize();
        if (FAILED(hr)) {
            chain->Release();
            return hr;
        }
        *swap_chain = chain;
        return S_OK;
    }

    HRESULT EnumAdapterByLuid(LUID, REFIID riid, void** adapter) override {
        return EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, riid, adapter);
    }

    HRESULT EnumWarpAdapter(REFIID riid, void** adapter) override {
        return EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_UNSPECIFIED, riid, adapter);
    }

    HRESULT CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) override {
        if (feature != DXGI_FEATURE_PRESENT_ALLOW_TEARING || data == nullptr || size < sizeof(BOOL)) return E_INVALIDARG;
        // Presents without display sync replace the drawable immediately.
        *static_cast<BOOL*>(data) = TRUE;
        return S_OK;
    }

    HRESULT EnumAdapterByGpuPreference(UINT index, DXGI_GPU_PREFERENCE, REFIID riid, void** result) override {
        if (result == nullptr) return E_INVALIDARG;
        *result = nullptr;
        IDXGIAdapter* adapter = nullptr;
        const HRESULT hr = EnumAdapters(index, &adapter);
        if (FAILED(hr)) return hr;
        const HRESULT query = adapter->QueryInterface(riid, result);
        adapter->Release();
        return query;
    }
};

}  // namespace

}  // namespace galaxy::metal12

HRESULT CreateDXGIFactory1(REFIID riid, void** factory) noexcept {
    return CreateDXGIFactory2(0, riid, factory);
}

HRESULT CreateDXGIFactory2(UINT, REFIID riid, void** factory) noexcept {
    if (factory == nullptr) return E_INVALIDARG;
    auto* created = new galaxy::metal12::Factory();
    const HRESULT hr = created->QueryInterface(riid, factory);
    created->Release();
    return hr;
}
