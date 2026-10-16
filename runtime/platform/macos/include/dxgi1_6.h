// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// DXGI on macOS: the factory, adapter, output and swap-chain interfaces the renderer uses,
// implemented by the Metal translation layer (runtime/platform/macos/src/d3d12). A swap chain
// presents into the CAMetalLayer of the window's content view. Methods outside the runtime's use
// are omitted; interface IDs are DXGI's.

#include <windows.h>
#include <dxgicommon.h>
#include <dxgiformat.h>

#define DXGI_ERROR_INVALID_CALL (static_cast<HRESULT>(0x887A0001u))
#define DXGI_ERROR_NOT_FOUND (static_cast<HRESULT>(0x887A0002u))
#define DXGI_ERROR_MORE_DATA (static_cast<HRESULT>(0x887A0003u))
#define DXGI_ERROR_UNSUPPORTED (static_cast<HRESULT>(0x887A0004u))
#define DXGI_ERROR_DEVICE_REMOVED (static_cast<HRESULT>(0x887A0005u))
#define DXGI_ERROR_DEVICE_HUNG (static_cast<HRESULT>(0x887A0006u))
#define DXGI_ERROR_DEVICE_RESET (static_cast<HRESULT>(0x887A0007u))
#define DXGI_ERROR_WAS_STILL_DRAWING (static_cast<HRESULT>(0x887A000Au))
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT (static_cast<HRESULT>(0x887A000Bu))
#define DXGI_ERROR_DRIVER_INTERNAL_ERROR (static_cast<HRESULT>(0x887A0020u))
#define DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (static_cast<HRESULT>(0x887A0022u))
#define DXGI_STATUS_OCCLUDED (static_cast<HRESULT>(0x087A0001))

#define DXGI_CREATE_FACTORY_DEBUG 0x01u
#define DXGI_MWA_NO_WINDOW_CHANGES 0x1u
#define DXGI_MWA_NO_ALT_ENTER 0x2u
#define DXGI_MWA_NO_PRINT_SCREEN 0x4u
#define DXGI_PRESENT_TEST 0x00000001u
#define DXGI_PRESENT_DO_NOT_WAIT 0x00000008u
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200u
#define DXGI_USAGE_SHADER_INPUT 0x00000010u
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020u
#define DXGI_USAGE_BACK_BUFFER 0x00000040u
using DXGI_USAGE = UINT;

enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4,
};
enum DXGI_SCALING { DXGI_SCALING_STRETCH = 0, DXGI_SCALING_NONE = 1, DXGI_SCALING_ASPECT_RATIO_STRETCH = 2 };
enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
};
enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048,
};
enum DXGI_MODE_SCANLINE_ORDER { DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0, DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1 };
enum DXGI_MODE_SCALING { DXGI_MODE_SCALING_UNSPECIFIED = 0, DXGI_MODE_SCALING_CENTERED = 1, DXGI_MODE_SCALING_STRETCHED = 2 };
enum DXGI_MODE_ROTATION { DXGI_MODE_ROTATION_UNSPECIFIED = 0, DXGI_MODE_ROTATION_IDENTITY = 1 };
enum DXGI_GPU_PREFERENCE {
    DXGI_GPU_PREFERENCE_UNSPECIFIED = 0,
    DXGI_GPU_PREFERENCE_MINIMUM_POWER = 1,
    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE = 2,
};
enum DXGI_FEATURE { DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0 };
enum DXGI_MEMORY_SEGMENT_GROUP { DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1 };
enum DXGI_ADAPTER_FLAG { DXGI_ADAPTER_FLAG_NONE = 0, DXGI_ADAPTER_FLAG_REMOTE = 1, DXGI_ADAPTER_FLAG_SOFTWARE = 2 };

struct DXGI_MODE_DESC {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
};

struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
};

struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
};

struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC {
    DXGI_RATIONAL RefreshRate;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
    BOOL Windowed;
};

struct DXGI_FRAME_STATISTICS {
    UINT PresentCount;
    UINT PresentRefreshCount;
    UINT SyncRefreshCount;
    LARGE_INTEGER SyncQPCTime;
    LARGE_INTEGER SyncGPUTime;
};

struct DXGI_ADAPTER_DESC {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
};

struct DXGI_ADAPTER_DESC1 {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
    UINT Flags;
};

struct DXGI_OUTPUT_DESC {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
};

struct DXGI_QUERY_VIDEO_MEMORY_INFO {
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 AvailableForReservation;
    UINT64 CurrentReservation;
};

struct IDXGIObject : IUnknown {
    virtual HRESULT GetParent(REFIID riid, void** parent) = 0;
};

struct IDXGIOutput : IDXGIObject {
    virtual HRESULT GetDesc(DXGI_OUTPUT_DESC* desc) = 0;
    virtual HRESULT FindClosestMatchingMode(const DXGI_MODE_DESC* mode, DXGI_MODE_DESC* closest, IUnknown* device) = 0;
    virtual HRESULT WaitForVBlank() = 0;
    virtual HRESULT GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) = 0;
};

struct IDXGIAdapter : IDXGIObject {
    virtual HRESULT EnumOutputs(UINT output, IDXGIOutput** result) = 0;
    virtual HRESULT GetDesc(DXGI_ADAPTER_DESC* desc) = 0;
};
struct IDXGIAdapter1 : IDXGIAdapter {
    virtual HRESULT GetDesc1(DXGI_ADAPTER_DESC1* desc) = 0;
};
struct IDXGIAdapter2 : IDXGIAdapter1 {};
struct IDXGIAdapter3 : IDXGIAdapter2 {
    virtual HRESULT QueryVideoMemoryInfo(UINT node, DXGI_MEMORY_SEGMENT_GROUP group,
        DXGI_QUERY_VIDEO_MEMORY_INFO* info) = 0;
};

struct IDXGISwapChain : IDXGIObject {
    virtual HRESULT Present(UINT sync_interval, UINT flags) = 0;
    virtual HRESULT GetBuffer(UINT buffer, REFIID riid, void** surface) = 0;
    virtual HRESULT SetFullscreenState(BOOL fullscreen, IDXGIOutput* target) = 0;
    virtual HRESULT GetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) = 0;
    virtual HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* desc) = 0;
    virtual HRESULT ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) = 0;
    virtual HRESULT ResizeTarget(const DXGI_MODE_DESC* target) = 0;
    virtual HRESULT GetContainingOutput(IDXGIOutput** output) = 0;
    virtual HRESULT GetFrameStatistics(DXGI_FRAME_STATISTICS* statistics) = 0;
    virtual HRESULT GetLastPresentCount(UINT* count) = 0;
};
struct IDXGISwapChain1 : IDXGISwapChain {
    virtual HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) = 0;
};
struct IDXGISwapChain2 : IDXGISwapChain1 {
    virtual HRESULT SetMaximumFrameLatency(UINT latency) = 0;
    virtual HRESULT GetMaximumFrameLatency(UINT* latency) = 0;
    virtual HANDLE GetFrameLatencyWaitableObject() = 0;
};
struct IDXGISwapChain3 : IDXGISwapChain2 {
    virtual UINT GetCurrentBackBufferIndex() = 0;
};
struct IDXGISwapChain4 : IDXGISwapChain3 {};

struct IDXGIFactory : IDXGIObject {
    virtual HRESULT EnumAdapters(UINT adapter, IDXGIAdapter** result) = 0;
    virtual HRESULT MakeWindowAssociation(HWND window, UINT flags) = 0;
};
struct IDXGIFactory1 : IDXGIFactory {
    virtual HRESULT EnumAdapters1(UINT adapter, IDXGIAdapter1** result) = 0;
    virtual BOOL IsCurrent() = 0;
};
struct IDXGIFactory2 : IDXGIFactory1 {
    virtual HRESULT CreateSwapChainForHwnd(IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* restrict_to, IDXGISwapChain1** swap_chain) = 0;
};
struct IDXGIFactory3 : IDXGIFactory2 {};
struct IDXGIFactory4 : IDXGIFactory3 {
    virtual HRESULT EnumAdapterByLuid(LUID luid, REFIID riid, void** adapter) = 0;
    // macOS has no WARP: the system GPU stands in for the software rasterizer (GPU tests).
    virtual HRESULT EnumWarpAdapter(REFIID riid, void** adapter) = 0;
};
struct IDXGIFactory5 : IDXGIFactory4 {
    virtual HRESULT CheckFeatureSupport(DXGI_FEATURE feature, void* data, UINT size) = 0;
};
struct IDXGIFactory6 : IDXGIFactory5 {
    virtual HRESULT EnumAdapterByGpuPreference(UINT adapter, DXGI_GPU_PREFERENCE preference, REFIID riid,
        void** result) = 0;
};

__CRT_UUID_DECL(IDXGIObject, 0xaec22fb8, 0x76f3, 0x4639, 0x9b, 0xe0, 0x28, 0xeb, 0x43, 0xa6, 0x7a, 0x2e)
__CRT_UUID_DECL(IDXGIOutput, 0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa)
__CRT_UUID_DECL(IDXGIAdapter, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0)
__CRT_UUID_DECL(IDXGIAdapter1, 0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05)
__CRT_UUID_DECL(IDXGIAdapter2, 0x0aa1ae0a, 0xfa0e, 0x4b84, 0x86, 0x44, 0xe0, 0x5f, 0xf8, 0xe5, 0xac, 0xb5)
__CRT_UUID_DECL(IDXGIAdapter3, 0x645967a4, 0x1392, 0x4310, 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd)
__CRT_UUID_DECL(IDXGISwapChain, 0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a)
__CRT_UUID_DECL(IDXGISwapChain1, 0x790a45f7, 0x0d42, 0x4876, 0x98, 0x3a, 0x0a, 0x55, 0xcf, 0xe6, 0xf4, 0xaa)
__CRT_UUID_DECL(IDXGISwapChain2, 0xa8be2ac4, 0x199f, 0x4946, 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7)
__CRT_UUID_DECL(IDXGISwapChain3, 0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1)
__CRT_UUID_DECL(IDXGISwapChain4, 0x3d585d5a, 0xbd4a, 0x489e, 0xb1, 0xf4, 0x3d, 0xbc, 0xb6, 0x45, 0x2f, 0xfb)
__CRT_UUID_DECL(IDXGIFactory, 0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69)
__CRT_UUID_DECL(IDXGIFactory1, 0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87)
__CRT_UUID_DECL(IDXGIFactory2, 0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0)
__CRT_UUID_DECL(IDXGIFactory3, 0x25483823, 0xcd46, 0x4c7d, 0x86, 0xca, 0x47, 0xaa, 0x95, 0xb8, 0x37, 0xbd)
__CRT_UUID_DECL(IDXGIFactory4, 0x1bc6ea02, 0xef36, 0x464f, 0xbf, 0x0c, 0x21, 0xca, 0x39, 0xe5, 0x16, 0x8a)
__CRT_UUID_DECL(IDXGIFactory5, 0x7632e1f5, 0xee65, 0x4dca, 0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d)
__CRT_UUID_DECL(IDXGIFactory6, 0xc1b6694f, 0xff09, 0x44a9, 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17)

HRESULT CreateDXGIFactory1(REFIID riid, void** factory) noexcept;
HRESULT CreateDXGIFactory2(UINT flags, REFIID riid, void** factory) noexcept;
