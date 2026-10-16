// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Audio endpoint enumeration. macOS exposes one endpoint: the system default output device,
// which the compat XAudio2 engine follows. Implemented in xaudio2_coreaudio.cpp.

#include <windows.h>
#include <mmreg.h>

enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 };
enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 };
#define DEVICE_STATE_ACTIVE 0x00000001u
#define DEVICE_STATE_DISABLED 0x00000002u
#define DEVICE_STATE_NOTPRESENT 0x00000004u
#define DEVICE_STATE_UNPLUGGED 0x00000008u
#define DEVICE_STATEMASK_ALL 0x0000000Fu

struct IPropertyStore;

struct IMMDevice : IUnknown {
    virtual HRESULT Activate(REFIID iid, DWORD context, void* parameters, void** interface_pointer) noexcept = 0;
    virtual HRESULT OpenPropertyStore(DWORD access, IPropertyStore** store) noexcept = 0;
    virtual HRESULT GetId(LPWSTR* id) noexcept = 0;
    virtual HRESULT GetState(DWORD* state) noexcept = 0;
};

struct IMMDeviceCollection : IUnknown {
    virtual HRESULT GetCount(UINT* count) noexcept = 0;
    virtual HRESULT Item(UINT index, IMMDevice** device) noexcept = 0;
};

struct IMMNotificationClient;

struct IMMDeviceEnumerator : IUnknown {
    virtual HRESULT EnumAudioEndpoints(EDataFlow flow, DWORD state_mask, IMMDeviceCollection** devices) noexcept = 0;
    virtual HRESULT GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** device) noexcept = 0;
    virtual HRESULT GetDevice(LPCWSTR id, IMMDevice** device) noexcept = 0;
    virtual HRESULT RegisterEndpointNotificationCallback(IMMNotificationClient* client) noexcept = 0;
    virtual HRESULT UnregisterEndpointNotificationCallback(IMMNotificationClient* client) noexcept = 0;
};

struct MMDeviceEnumerator;
__CRT_UUID_DECL(MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E)
__CRT_UUID_DECL(IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6)
__CRT_UUID_DECL(IMMDevice, 0xD666063F, 0x1587, 0x4E43, 0x81, 0xF1, 0xB9, 0x48, 0xE8, 0x07, 0x36, 0x3F)
__CRT_UUID_DECL(IMMDeviceCollection, 0x0BD7A1BE, 0x7A1A, 0x44DB, 0x83, 0x97, 0xCC, 0x53, 0x92, 0x38, 0x7B, 0x5E)

HRESULT CoCreateInstance(REFCLSID clsid, IUnknown* outer, DWORD context, REFIID iid, void** object) noexcept;
