// SPDX-License-Identifier: GPL-3.0-only
//
// XAudio2 over Core Audio.
//
// The engine owns a default-output AudioUnit, which follows the system output device. Its render
// callback runs one processing pass: every started source voice consumes its queued 16-bit PCM at
// (input rate x frequency ratio) and is resampled to the device rate with a four-point Hermite
// interpolator, then the voices are summed into the float output.
//
// Voice callbacks follow XAudio2's order on the real-time thread: OnVoiceProcessingPassStart,
// OnBufferStart before a buffer's first sample, OnBufferEnd after its last (the buffer has left
// the queue by then, so GetState inside OnBufferEnd counts only its successors), and
// OnVoiceProcessingPassEnd. The voice lock is recursive because callbacks call back into the voice
// (SetFrequencyRatio, GetState), as XAudio2 permits.

#include <xaudio2.h>
#include <mmdeviceapi.h>

#include "win32_internal.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace {

constexpr HRESULT kAudioEngineError = static_cast<HRESULT>(0x88960003u);  // XAUDIO2_E_DEVICE_INVALIDATED family

class CoreAudioEngine;

class SourceVoice final : public IXAudio2SourceVoice {
public:
    SourceVoice(CoreAudioEngine* engine, const WAVEFORMATEX& format, float max_ratio,
        IXAudio2VoiceCallback* callback) noexcept
        : engine_(engine), channels_(format.nChannels), sample_rate_(format.nSamplesPerSec),
          max_ratio_(max_ratio), callback_(callback) {}

    // IXAudio2Voice
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* details) noexcept override {
        details->CreationFlags = 0;
        details->ActiveFlags = 0;
        details->InputChannels = channels_;
        details->InputSampleRate = sample_rate_;
    }
    HRESULT SetVolume(float volume, UINT32) noexcept override {
        std::lock_guard lock(mutex_);
        volume_ = volume;
        return S_OK;
    }
    void GetVolume(float* volume) noexcept override {
        std::lock_guard lock(mutex_);
        *volume = volume_;
    }
    void DestroyVoice() noexcept override;

    // IXAudio2SourceVoice
    HRESULT Start(UINT32, UINT32) noexcept override {
        std::lock_guard lock(mutex_);
        started_ = true;
        return S_OK;
    }
    HRESULT Stop(UINT32, UINT32) noexcept override {
        std::lock_guard lock(mutex_);
        started_ = false;
        return S_OK;
    }
    HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER* buffer, const XAUDIO2_BUFFER_WMA*) noexcept override {
        if (buffer == nullptr || buffer->pAudioData == nullptr || buffer->AudioBytes == 0u ||
            buffer->LoopCount != 0u) {
            return XAUDIO2_E_INVALID_CALL;
        }
        std::lock_guard lock(mutex_);
        if (queue_.size() >= XAUDIO2_MAX_QUEUED_BUFFERS) {
            return XAUDIO2_E_INVALID_CALL;
        }
        const std::uint32_t frame_bytes = 2u * channels_;
        Queued queued{};
        queued.data = reinterpret_cast<const std::int16_t*>(buffer->pAudioData);
        const std::uint32_t total = buffer->AudioBytes / frame_bytes;
        queued.first = std::min(buffer->PlayBegin, total);
        queued.frames = buffer->PlayLength != 0u ? std::min(buffer->PlayLength, total - queued.first) : total - queued.first;
        queued.context = buffer->pContext;
        queued.end_of_stream = (buffer->Flags & XAUDIO2_END_OF_STREAM) != 0u;
        queue_.push_back(queued);
        return S_OK;
    }
    HRESULT FlushSourceBuffers() noexcept override {
        std::vector<void*> ended;
        {
            std::lock_guard lock(mutex_);
            // A started voice keeps the buffer it is playing; everything after it is removed.
            const std::size_t keep = (started_ && !queue_.empty() && queue_.front().began) ? 1u : 0u;
            while (queue_.size() > keep) {
                ended.push_back(queue_.back().context);
                queue_.pop_back();
            }
            std::reverse(ended.begin(), ended.end());
            if (keep == 0u) position_ = 0.0;
        }
        if (callback_ != nullptr) {
            for (void* context : ended) callback_->OnBufferEnd(context);
        }
        return S_OK;
    }
    HRESULT Discontinuity() noexcept override { return S_OK; }
    void GetState(XAUDIO2_VOICE_STATE* state, UINT32 flags) noexcept override {
        std::lock_guard lock(mutex_);
        state->pCurrentBufferContext = queue_.empty() ? nullptr : queue_.front().context;
        state->BuffersQueued = static_cast<UINT32>(queue_.size());
        state->SamplesPlayed = (flags & XAUDIO2_VOICE_NOSAMPLESPLAYED) != 0u ? 0u : samples_played_;
    }
    HRESULT SetFrequencyRatio(float ratio, UINT32) noexcept override {
        if (!(ratio >= 1.0f / 1024.0f) || ratio > max_ratio_) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        ratio_ = ratio;
        return S_OK;
    }
    void GetFrequencyRatio(float* ratio) noexcept override {
        std::lock_guard lock(mutex_);
        *ratio = ratio_;
    }

    // Mixes `frames` device frames into `out` (interleaved stereo float). Real-time thread.
    void render(float* out, std::uint32_t frames, double device_rate) noexcept {
        std::lock_guard lock(mutex_);
        if (callback_ != nullptr) {
            callback_->OnVoiceProcessingPassStart(static_cast<UINT32>(frames * 2u * channels_));
        }
        if (started_) {
            for (std::uint32_t i = 0; i < frames; ++i) {
                if (!ensure_frames(3u)) break;  // underrun: the rest of the pass is silent
                const double t = position_ - std::floor(position_);
                float left = 0.0f;
                float right = 0.0f;
                sample(t, left, right);
                out[2u * i] += left * volume_;
                out[2u * i + 1u] += right * volume_;
                const double step = static_cast<double>(sample_rate_) * ratio_ / device_rate;
                advance(step);
            }
        }
        if (callback_ != nullptr) callback_->OnVoiceProcessingPassEnd();
    }

private:
    struct Queued {
        const std::int16_t* data = nullptr;
        std::uint32_t first = 0;
        std::uint32_t frames = 0;
        void* context = nullptr;
        bool began = false;
        bool end_of_stream = false;
    };

    // Frame `index` relative to the current read point of the head buffer (index 0 is the frame
    // at floor(position_)); index -1 is the history frame before it.
    void frame_at(int index, float& left, float& right) const noexcept {
        if (index < 0) {
            left = history_left_;
            right = history_right_;
            return;
        }
        auto offset = static_cast<std::uint64_t>(position_) + static_cast<std::uint64_t>(index);
        for (const Queued& queued : queue_) {
            if (offset < queued.frames) {
                const std::int16_t* frame = queued.data + (queued.first + offset) * channels_;
                left = static_cast<float>(frame[0]) / 32768.0f;
                right = channels_ >= 2u ? static_cast<float>(frame[1]) / 32768.0f : left;
                return;
            }
            offset -= queued.frames;
        }
        left = right = 0.0f;
    }

    void sample(double t, float& left, float& right) const noexcept {
        float l[4];
        float r[4];
        for (int k = 0; k < 4; ++k) frame_at(k - 1, l[k], r[k]);
        const auto hermite = [t](const float* y) {
            const double c0 = y[1];
            const double c1 = 0.5 * (y[2] - y[0]);
            const double c2 = y[0] - 2.5 * y[1] + 2.0 * y[2] - 0.5 * y[3];
            const double c3 = 0.5 * (y[3] - y[0]) + 1.5 * (y[1] - y[2]);
            return static_cast<float>(((c3 * t + c2) * t + c1) * t + c0);
        };
        left = hermite(l);
        right = hermite(r);
    }

    // True when at least `needed` frames from the read point are queued; begins the head buffer.
    bool ensure_frames(std::uint32_t needed) noexcept {
        if (queue_.empty()) return false;
        begin_head();
        std::uint64_t available = 0;
        for (const Queued& queued : queue_) available += queued.frames;
        available -= static_cast<std::uint64_t>(position_);
        // Fewer than `needed` frames still play: frame_at pads the interpolation window with silence.
        (void)needed;
        return available >= 1u;
    }

    void begin_head() noexcept {
        Queued& head = queue_.front();
        if (!head.began) {
            head.began = true;
            if (callback_ != nullptr) callback_->OnBufferStart(head.context);
        }
    }

    void advance(double step) noexcept {
        const auto before = static_cast<std::uint64_t>(position_);
        position_ += step;
        auto after = static_cast<std::uint64_t>(position_);
        for (std::uint64_t frame = before; frame < after; ++frame) {
            // Keep the frame being left as interpolation history.
            float left = 0.0f;
            float right = 0.0f;
            const double saved = position_;
            position_ = static_cast<double>(frame);
            frame_at(0, left, right);
            position_ = saved;
            history_left_ = left;
            history_right_ = right;
        }
        samples_played_ += after - before;
        while (!queue_.empty() && after >= queue_.front().frames) {
            Queued finished = queue_.front();
            queue_.pop_front();
            after -= finished.frames;
            position_ -= static_cast<double>(finished.frames);
            if (callback_ != nullptr) {
                if (!finished.began) callback_->OnBufferStart(finished.context);
                callback_->OnBufferEnd(finished.context);
                if (finished.end_of_stream) callback_->OnStreamEnd();
            }
            if (!queue_.empty()) begin_head();
        }
        if (queue_.empty() && position_ > 0.0) {
            position_ = 0.0;  // drained: the next buffer starts at its first frame
        }
    }

    CoreAudioEngine* engine_;
    std::uint32_t channels_;
    std::uint32_t sample_rate_;
    float max_ratio_;
    IXAudio2VoiceCallback* callback_;
    std::recursive_mutex mutex_;
    std::deque<Queued> queue_;
    double position_ = 0.0;
    float history_left_ = 0.0f;
    float history_right_ = 0.0f;
    float ratio_ = 1.0f;
    float volume_ = 1.0f;
    bool started_ = false;
    std::uint64_t samples_played_ = 0;
};

class MasteringVoice final : public IXAudio2MasteringVoice {
public:
    explicit MasteringVoice(CoreAudioEngine* engine, std::uint32_t channels, std::uint32_t rate) noexcept
        : engine_(engine), channels_(channels), rate_(rate) {}
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* details) noexcept override {
        details->CreationFlags = 0;
        details->ActiveFlags = 0;
        details->InputChannels = channels_;
        details->InputSampleRate = rate_;
    }
    HRESULT SetVolume(float volume, UINT32) noexcept override;
    void GetVolume(float* volume) noexcept override;
    void DestroyVoice() noexcept override;
    HRESULT GetChannelMask(DWORD* mask) noexcept override {
        *mask = 0x3u;  // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
        return S_OK;
    }

private:
    CoreAudioEngine* engine_;
    std::uint32_t channels_;
    std::uint32_t rate_;
};

class CoreAudioEngine final : public IXAudio2 {
public:
    ~CoreAudioEngine() { stop_unit(); }

    HRESULT QueryInterface(REFIID, void** object) override {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++references_; }
    ULONG Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }

    HRESULT RegisterForCallbacks(IXAudio2EngineCallback* callback) noexcept override {
        std::lock_guard lock(mutex_);
        engine_callbacks_.push_back(callback);
        return S_OK;
    }
    void UnregisterForCallbacks(IXAudio2EngineCallback* callback) noexcept override {
        std::lock_guard lock(mutex_);
        engine_callbacks_.erase(std::remove(engine_callbacks_.begin(), engine_callbacks_.end(), callback),
            engine_callbacks_.end());
    }

    HRESULT CreateSourceVoice(IXAudio2SourceVoice** voice, const WAVEFORMATEX* format, UINT32, float max_ratio,
        IXAudio2VoiceCallback* callback, const XAUDIO2_VOICE_SENDS*, const XAUDIO2_EFFECT_CHAIN*) noexcept override {
        if (voice == nullptr || format == nullptr || format->wFormatTag != WAVE_FORMAT_PCM ||
            format->wBitsPerSample != 16u || format->nChannels == 0u || format->nChannels > 2u) {
            return XAUDIO2_E_INVALID_CALL;
        }
        if (mastering_ == nullptr) return XAUDIO2_E_INVALID_CALL;
        auto* created = new SourceVoice(this, *format, max_ratio, callback);
        std::lock_guard lock(mutex_);
        voices_.push_back(created);
        *voice = created;
        return S_OK;
    }

    HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** voice, UINT32, UINT32, UINT32, LPCWSTR,
        const XAUDIO2_EFFECT_CHAIN*, AUDIO_STREAM_CATEGORY) noexcept override {
        if (voice == nullptr || mastering_ != nullptr) return XAUDIO2_E_INVALID_CALL;
        const HRESULT result = create_unit();
        if (FAILED(result)) return result;
        mastering_ = new MasteringVoice(this, 2u, static_cast<std::uint32_t>(device_rate_));
        *voice = mastering_;
        return S_OK;
    }

    HRESULT StartEngine() noexcept override {
        if (unit_ == nullptr) return XAUDIO2_E_INVALID_CALL;
        if (running_) return S_OK;
        const OSStatus status = AudioOutputUnitStart(unit_);
        if (status != noErr) {
            std::fprintf(stderr, "[audio] AudioOutputUnitStart failed status=%d\n", static_cast<int>(status));
            return kAudioEngineError;
        }
        running_ = true;
        return S_OK;
    }

    void StopEngine() noexcept override {
        if (unit_ != nullptr && running_) {
            AudioOutputUnitStop(unit_);
            running_ = false;
        }
    }

    HRESULT CommitChanges(UINT32) noexcept override { return S_OK; }

    void GetPerformanceData(XAUDIO2_PERFORMANCE_DATA* data) noexcept override {
        *data = {};
        data->GlitchesSinceEngineStarted = glitches_.load(std::memory_order_relaxed);
        data->CurrentLatencyInSamples = latency_frames_;
        std::lock_guard lock(mutex_);
        data->TotalSourceVoiceCount = static_cast<UINT32>(voices_.size());
        data->ActiveSourceVoiceCount = data->TotalSourceVoiceCount;
    }

    void destroy_voice(SourceVoice* voice) noexcept {
        {
            std::lock_guard lock(mutex_);
            voices_.erase(std::remove(voices_.begin(), voices_.end(), voice), voices_.end());
        }
        // The render callback holds mutex_ while mixing, so no pass still uses the voice.
        delete voice;
    }

    void destroy_mastering_voice() noexcept {
        stop_unit();
        delete mastering_;
        mastering_ = nullptr;
    }

    float master_volume = 1.0f;

private:
    HRESULT create_unit() noexcept {
        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_DefaultOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr || AudioComponentInstanceNew(component, &unit_) != noErr) {
            unit_ = nullptr;
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        AudioStreamBasicDescription device{};
        UInt32 size = sizeof(device);
        if (AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size) ==
                noErr &&
            device.mSampleRate > 0.0) {
            device_rate_ = device.mSampleRate;
        }
        AudioStreamBasicDescription format{};
        format.mSampleRate = device_rate_;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 8;
        format.mBytesPerPacket = 8;
        AURenderCallbackStruct render{};
        render.inputProc = &CoreAudioEngine::render_callback;
        render.inputProcRefCon = this;
        if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format,
                sizeof(format)) != noErr ||
            AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &render,
                sizeof(render)) != noErr ||
            AudioUnitInitialize(unit_) != noErr) {
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            return kAudioEngineError;
        }
        UInt32 frames = 0;
        size = sizeof(frames);
        if (AudioUnitGetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &frames,
                &size) == noErr) {
            latency_frames_ = frames;
        }
        return S_OK;
    }

    void stop_unit() noexcept {
        if (unit_ != nullptr) {
            if (running_) AudioOutputUnitStop(unit_);
            running_ = false;
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
    }

    static OSStatus render_callback(void* user, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32,
        UInt32 frames, AudioBufferList* buffers) {
        auto* engine = static_cast<CoreAudioEngine*>(user);
        auto* out = static_cast<float*>(buffers->mBuffers[0].mData);
        std::memset(out, 0, buffers->mBuffers[0].mDataByteSize);
        std::lock_guard lock(engine->mutex_);
        for (IXAudio2EngineCallback* callback : engine->engine_callbacks_) callback->OnProcessingPassStart();
        bool any = false;
        for (SourceVoice* voice : engine->voices_) {
            voice->render(out, frames, engine->device_rate_);
            any = true;
        }
        const float gain = engine->master_volume;
        for (UInt32 i = 0; i < frames * 2u; ++i) out[i] = std::clamp(out[i] * gain, -1.0f, 1.0f);
        for (IXAudio2EngineCallback* callback : engine->engine_callbacks_) callback->OnProcessingPassEnd();
        if (!any && flags != nullptr) *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    std::atomic<ULONG> references_{1};
    std::recursive_mutex mutex_;
    std::vector<IXAudio2EngineCallback*> engine_callbacks_;
    std::vector<SourceVoice*> voices_;
    MasteringVoice* mastering_ = nullptr;
    AudioUnit unit_ = nullptr;
    double device_rate_ = 48000.0;
    UInt32 latency_frames_ = 0;
    bool running_ = false;
    std::atomic<UINT32> glitches_{0};
};

void SourceVoice::DestroyVoice() noexcept { engine_->destroy_voice(this); }
HRESULT MasteringVoice::SetVolume(float volume, UINT32) noexcept {
    engine_->master_volume = volume;
    return S_OK;
}
void MasteringVoice::GetVolume(float* volume) noexcept { *volume = engine_->master_volume; }
void MasteringVoice::DestroyVoice() noexcept { engine_->destroy_mastering_voice(); }

// --- Endpoint enumeration: the system default output is the one endpoint. ---

class DefaultEndpoint final : public IMMDevice {
public:
    HRESULT QueryInterface(REFIID, void** object) override {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++references_; }
    ULONG Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }
    HRESULT Activate(REFIID, DWORD, void*, void**) noexcept override { return E_NOTIMPL; }
    HRESULT OpenPropertyStore(DWORD, IPropertyStore**) noexcept override { return E_NOTIMPL; }
    HRESULT GetId(LPWSTR* id) noexcept override {
        static constexpr wchar_t kId[] = L"{macos.default-output}";
        auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc(sizeof(kId)));
        if (copy == nullptr) return E_OUTOFMEMORY;
        std::memcpy(copy, kId, sizeof(kId));
        *id = copy;
        return S_OK;
    }
    HRESULT GetState(DWORD* state) noexcept override {
        *state = DEVICE_STATE_ACTIVE;
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
};

class EndpointCollection final : public IMMDeviceCollection {
public:
    HRESULT QueryInterface(REFIID, void** object) override {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++references_; }
    ULONG Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }
    HRESULT GetCount(UINT* count) noexcept override {
        *count = 1u;
        return S_OK;
    }
    HRESULT Item(UINT index, IMMDevice** device) noexcept override {
        if (index != 0u) return E_INVALIDARG;
        *device = new DefaultEndpoint();
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
};

class DeviceEnumerator final : public IMMDeviceEnumerator {
public:
    HRESULT QueryInterface(REFIID, void** object) override {
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG AddRef() override { return ++references_; }
    ULONG Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0u) delete this;
        return remaining;
    }
    HRESULT EnumAudioEndpoints(EDataFlow flow, DWORD, IMMDeviceCollection** devices) noexcept override {
        if (flow != eRender) return E_INVALIDARG;
        *devices = new EndpointCollection();
        return S_OK;
    }
    HRESULT GetDefaultAudioEndpoint(EDataFlow flow, ERole, IMMDevice** device) noexcept override {
        if (flow != eRender) return E_INVALIDARG;
        *device = new DefaultEndpoint();
        return S_OK;
    }
    HRESULT GetDevice(LPCWSTR, IMMDevice** device) noexcept override {
        *device = new DefaultEndpoint();
        return S_OK;
    }
    HRESULT RegisterEndpointNotificationCallback(IMMNotificationClient*) noexcept override { return S_OK; }
    HRESULT UnregisterEndpointNotificationCallback(IMMNotificationClient*) noexcept override { return S_OK; }

private:
    std::atomic<ULONG> references_{1};
};

}  // namespace

HRESULT XAudio2Create(IXAudio2** engine, UINT32, UINT32) noexcept {
    if (engine == nullptr) return E_POINTER;
    *engine = new CoreAudioEngine();
    return S_OK;
}

HRESULT CoCreateInstance(REFCLSID clsid, IUnknown*, DWORD, REFIID, void** object) noexcept {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (clsid == __uuidof(MMDeviceEnumerator)) {
        *object = static_cast<IMMDeviceEnumerator*>(new DeviceEnumerator());
        return S_OK;
    }
    return static_cast<HRESULT>(0x80040154u);  // REGDB_E_CLASSNOTREG
}
