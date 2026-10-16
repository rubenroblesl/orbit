// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The XAudio2 subset the runtime's audio sink uses, implemented over a Core Audio output unit
// (runtime/platform/macos/src/xaudio2_coreaudio.cpp).
//
// One engine owns one default-output AudioUnit. Source voices accept 16-bit PCM buffers that
// stay owned by the caller until OnBufferEnd, resample them to the device rate (times the
// voice's frequency ratio) and are mixed in the output unit's render callback. Voice and engine
// callbacks run on that real-time thread, as XAudio2's callbacks run on its engine thread.

#include <windows.h>
#include <mmreg.h>

#define XAUDIO2_COMMIT_NOW 0u
#define XAUDIO2_COMMIT_ALL 0u
#define XAUDIO2_DEFAULT_CHANNELS 0u
#define XAUDIO2_DEFAULT_SAMPLERATE 0u
#define XAUDIO2_DEFAULT_PROCESSOR 0x00000001u
#define XAUDIO2_DEFAULT_FREQ_RATIO 2.0f
#define XAUDIO2_MAX_FREQ_RATIO 1024.0f
#define XAUDIO2_MAX_QUEUED_BUFFERS 64u
#define XAUDIO2_MAX_BUFFER_BYTES 0x80000000u
#define XAUDIO2_VOICE_NOPITCH 0x0002u
#define XAUDIO2_VOICE_NOSRC 0x0004u
#define XAUDIO2_VOICE_NOSAMPLESPLAYED 0x0100u
#define XAUDIO2_END_OF_STREAM 0x0040u
#define XAUDIO2_PLAY_TAILS 0x0020u
#define XAUDIO2_NO_LOOP_REGION 0u
#define XAUDIO2_E_INVALID_CALL (static_cast<HRESULT>(0x88960001u))
#define XAUDIO2_E_XMA_DECODER_ERROR (static_cast<HRESULT>(0x88960002u))
#define XAUDIO2_E_DEVICE_INVALIDATED (static_cast<HRESULT>(0x88960004u))
#define AUDCLNT_E_DEVICE_INVALIDATED (static_cast<HRESULT>(0x88890004u))
#define AUDCLNT_E_SERVICE_NOT_RUNNING (static_cast<HRESULT>(0x88890010u))
#define AUDCLNT_E_ENDPOINT_CREATE_FAILED (static_cast<HRESULT>(0x8889000Fu))

enum AUDIO_STREAM_CATEGORY { AudioCategory_Other = 0, AudioCategory_GameEffects = 6, AudioCategory_GameMedia = 11 };

struct XAUDIO2_BUFFER {
    UINT32 Flags;
    UINT32 AudioBytes;
    const BYTE* pAudioData;
    UINT32 PlayBegin;
    UINT32 PlayLength;
    UINT32 LoopBegin;
    UINT32 LoopLength;
    UINT32 LoopCount;
    void* pContext;
};

struct XAUDIO2_BUFFER_WMA {
    const UINT32* pDecodedPacketCumulativeBytes;
    UINT32 PacketCount;
};

struct XAUDIO2_VOICE_STATE {
    void* pCurrentBufferContext;
    UINT32 BuffersQueued;
    UINT64 SamplesPlayed;
};

struct XAUDIO2_PERFORMANCE_DATA {
    UINT64 AudioCyclesSinceLastQuery;
    UINT64 TotalCyclesSinceLastQuery;
    UINT32 MinimumCyclesPerQuantum;
    UINT32 MaximumCyclesPerQuantum;
    UINT32 MemoryUsageInBytes;
    UINT32 CurrentLatencyInSamples;
    UINT32 GlitchesSinceEngineStarted;
    UINT32 ActiveSourceVoiceCount;
    UINT32 TotalSourceVoiceCount;
    UINT32 ActiveSubmixVoiceCount;
    UINT32 ActiveResamplerCount;
    UINT32 ActiveMatrixMixCount;
    UINT32 ActiveXmaSourceVoices;
    UINT32 ActiveXmaStreams;
};

struct XAUDIO2_VOICE_DETAILS {
    UINT32 CreationFlags;
    UINT32 ActiveFlags;
    UINT32 InputChannels;
    UINT32 InputSampleRate;
};

struct XAUDIO2_VOICE_SENDS;
struct XAUDIO2_EFFECT_CHAIN;

struct IXAudio2VoiceCallback {
    virtual void OnVoiceProcessingPassStart(UINT32 bytes_required) noexcept = 0;
    virtual void OnVoiceProcessingPassEnd() noexcept = 0;
    virtual void OnStreamEnd() noexcept = 0;
    virtual void OnBufferStart(void* context) noexcept = 0;
    virtual void OnBufferEnd(void* context) noexcept = 0;
    virtual void OnLoopEnd(void* context) noexcept = 0;
    virtual void OnVoiceError(void* context, HRESULT error) noexcept = 0;

protected:
    ~IXAudio2VoiceCallback() = default;
};

struct IXAudio2EngineCallback {
    virtual void OnProcessingPassStart() noexcept = 0;
    virtual void OnProcessingPassEnd() noexcept = 0;
    virtual void OnCriticalError(HRESULT error) noexcept = 0;

protected:
    ~IXAudio2EngineCallback() = default;
};

struct IXAudio2Voice {
    virtual void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* details) noexcept = 0;
    virtual HRESULT SetVolume(float volume, UINT32 operation_set = XAUDIO2_COMMIT_NOW) noexcept = 0;
    virtual void GetVolume(float* volume) noexcept = 0;
    virtual void DestroyVoice() noexcept = 0;

protected:
    ~IXAudio2Voice() = default;
};

struct IXAudio2SourceVoice : IXAudio2Voice {
    virtual HRESULT Start(UINT32 flags = 0, UINT32 operation_set = XAUDIO2_COMMIT_NOW) noexcept = 0;
    virtual HRESULT Stop(UINT32 flags = 0, UINT32 operation_set = XAUDIO2_COMMIT_NOW) noexcept = 0;
    virtual HRESULT SubmitSourceBuffer(
        const XAUDIO2_BUFFER* buffer, const XAUDIO2_BUFFER_WMA* wma = nullptr) noexcept = 0;
    virtual HRESULT FlushSourceBuffers() noexcept = 0;
    virtual HRESULT Discontinuity() noexcept = 0;
    virtual void GetState(XAUDIO2_VOICE_STATE* state, UINT32 flags = 0) noexcept = 0;
    virtual HRESULT SetFrequencyRatio(float ratio, UINT32 operation_set = XAUDIO2_COMMIT_NOW) noexcept = 0;
    virtual void GetFrequencyRatio(float* ratio) noexcept = 0;

protected:
    ~IXAudio2SourceVoice() = default;
};

struct IXAudio2MasteringVoice : IXAudio2Voice {
    virtual HRESULT GetChannelMask(DWORD* mask) noexcept = 0;

protected:
    ~IXAudio2MasteringVoice() = default;
};

struct IXAudio2 : IUnknown {
    virtual HRESULT RegisterForCallbacks(IXAudio2EngineCallback* callback) noexcept = 0;
    virtual void UnregisterForCallbacks(IXAudio2EngineCallback* callback) noexcept = 0;
    virtual HRESULT CreateSourceVoice(IXAudio2SourceVoice** voice, const WAVEFORMATEX* format,
        UINT32 flags = 0, float max_frequency_ratio = XAUDIO2_DEFAULT_FREQ_RATIO,
        IXAudio2VoiceCallback* callback = nullptr, const XAUDIO2_VOICE_SENDS* sends = nullptr,
        const XAUDIO2_EFFECT_CHAIN* effects = nullptr) noexcept = 0;
    virtual HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** voice,
        UINT32 channels = XAUDIO2_DEFAULT_CHANNELS, UINT32 sample_rate = XAUDIO2_DEFAULT_SAMPLERATE,
        UINT32 flags = 0, LPCWSTR device_id = nullptr, const XAUDIO2_EFFECT_CHAIN* effects = nullptr,
        AUDIO_STREAM_CATEGORY category = AudioCategory_GameEffects) noexcept = 0;
    virtual HRESULT StartEngine() noexcept = 0;
    virtual void StopEngine() noexcept = 0;
    virtual HRESULT CommitChanges(UINT32 operation_set) noexcept = 0;
    virtual void GetPerformanceData(XAUDIO2_PERFORMANCE_DATA* data) noexcept = 0;
};

HRESULT XAudio2Create(IXAudio2** engine, UINT32 flags = 0,
    UINT32 processor = XAUDIO2_DEFAULT_PROCESSOR) noexcept;
