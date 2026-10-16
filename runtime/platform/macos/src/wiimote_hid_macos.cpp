// SPDX-License-Identifier: GPL-3.0-only
//
// Real Wii Remotes over IOKit HID (galaxy_macos_wiimote.h). IOKit is reached through iokit_hid.h,
// so no Win32 compat header meets the IOKit ones here.
//
// The bridge follows the Windows RealWiimoteHidBridge: open() fails when no remote is connected
// (an input source that was asked for never falls back to the keyboard), reports are normalised
// to the A1-framed form the game's Bluetooth stack expects, and an output report that is not
// accepted is retried for two seconds before the bridge fails. Where Windows keeps a HID handle
// across a dropped radio link, macOS removes the device and matches a new one when the remote
// comes back. device_link() reports both; native_host.cpp then ends the game's Bluetooth link
// and offers a new one once a remote is attached, so the game sets the remote up again. Output
// reports for a remote that is gone are dropped.

#include "galaxy_macos_wiimote.h"

#include "galaxy/wiimote_report.h"

#include "iokit_hid.h"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

namespace galaxy::macos::wiimote {

namespace {

namespace hid = galaxy::macos::hid;
using Clock = std::chrono::steady_clock;

constexpr std::uint16_t kProducts[] = {kWiiRemoteProductId, kWiiRemotePlusProductId};

// Matched by the launcher (GameRunner.swift) to explain why the game closed.
constexpr const char* kNotConnected =
    "no Wii Remote is connected to this Mac (HID 057E:0306 or 057E:0330); pair it in System Settings > "
    "Bluetooth (press 1+2 or the red SYNC button)";

bool env_flag(const char* name) noexcept {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return false;
    return !(std::strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "off") == 0 ||
             strcasecmp(value, "no") == 0);
}

std::string hex(std::span<const std::uint8_t> bytes) {
    std::string text;
    char digits[4];
    for (const std::uint8_t byte : bytes) {
        if (!text.empty()) text += ' ';
        std::snprintf(digits, sizeof(digits), "%02x", byte);
        text += digits;
    }
    return text;
}

const char* product_name(std::uint16_t product_id) noexcept {
    return product_id == kWiiRemotePlusProductId ? "RVL-CNT-01-TR" : "RVL-CNT-01";
}

class Bridge final : public input::NativeHidIoBackend, private TransportSink {
public:
    Bridge(std::unique_ptr<Transport> transport, BridgeOptions options)
        : transport_(std::move(transport)), options_(options) {
        if (transport_ == nullptr) throw std::invalid_argument("Wii Remote bridge requires a transport");
        options_.write_attempts = std::max(1u, options_.write_attempts);
        options_.max_pending_input = std::max<std::size_t>(1u, options_.max_pending_input);
    }

    ~Bridge() override { close(); }

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    void open() override {
        {
            std::lock_guard lock(mutex_);
            if (stop_requested_ || transport_started_) return;
            transport_started_ = true;
        }
        transport_->start(*this);
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, options_.discovery_timeout,
            [&] { return stop_requested_ || attachments_ != 0u || !failure_.empty(); });
        if (stop_requested_) return;
        throw_if_failed_locked();
        if (attachments_ == 0u) {
            std::string message = std::string("real Wii Remote HID: ") + kNotConnected;
            if (!open_failure_.empty()) {
                message += " (a Wii Remote was found but could not be opened: " + open_failure_ + ")";
            }
            throw std::runtime_error(message);
        }
    }

    bool poll_input(std::vector<std::byte>& report) override {
        std::vector<std::uint8_t> raw;
        {
            std::lock_guard lock(mutex_);
            if (stop_requested_) return false;
            if (pending_.empty()) {
                throw_if_failed_locked();
                return false;
            }
            raw = std::move(pending_.front());
            pending_.pop_front();
        }
        std::array<std::uint8_t, input::kMaxWiimoteInputReportBytes> normalized{};
        const input::WiimoteReportBuildResult result =
            input::normalize_wiimote_hid_input_report(raw, normalized);
        if (result.status == input::WiimoteReportBuildStatus::UnsupportedReportMode) {
            throw std::runtime_error("real Wiimote HID produced unsupported input report: " + hex(raw));
        }
        if (result.status != input::WiimoteReportBuildStatus::Ok) {
            throw std::runtime_error("real Wiimote HID produced truncated input report: " + hex(raw));
        }
        report.resize(result.size);
        std::transform(normalized.begin(), normalized.begin() + static_cast<std::ptrdiff_t>(result.size),
            report.begin(), [](std::uint8_t value) { return static_cast<std::byte>(value); });
        ++input_reports_;
        if (options_.trace) trace_input(std::span<const std::uint8_t>(normalized.data(), result.size));
        return true;
    }

    bool write_output(std::span<const std::byte> report) override {
        const std::span<const std::uint8_t> framed(reinterpret_cast<const std::uint8_t*>(report.data()), report.size());
        if (framed.size() < 2u || framed[0] != input::kWiimoteHidOutput) {
            throw std::runtime_error("real Wiimote HID output must be an A2 output report");
        }
        // IOKit takes the report ID first, at the report's own length.
        const std::span<const std::uint8_t> hid_report = framed.subspan(1);
        for (unsigned attempt = 1;; ++attempt) {
            {
                std::lock_guard lock(mutex_);
                if (stop_requested_) return false;
                throw_if_failed_locked();
                if (!device_open_) {
                    // Meant for a remote that went away: the game's link to it is ending.
                    ++dropped_outputs_;
                    if (options_.trace) std::cerr << "[wiimote] out " << hex(hid_report) << " dropped (no remote)\n";
                    return true;
                }
            }
            const Clock::time_point begin = Clock::now();
            const bool written = transport_->write(hid_report);
            note_write(hid_report, Clock::now() - begin, written, attempt);
            if (written) return true;
            if (attempt >= options_.write_attempts) break;
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, options_.write_retry_interval, [&] { return stop_requested_ || !device_open_; });
        }
        char message[128];
        std::snprintf(message, sizeof(message),
            "real Wii Remote HID output report 0x%02x was not accepted after %u attempts", hid_report[0],
            options_.write_attempts);
        throw std::runtime_error(message);
    }

    void request_stop() noexcept override {
        {
            std::lock_guard lock(mutex_);
            stop_requested_ = true;
        }
        wake_.notify_all();
        transport_->request_stop();
    }

    void close() noexcept override {
        request_stop();
        bool stop_transport = false;
        {
            std::lock_guard lock(mutex_);
            stop_transport = transport_started_ && !transport_stopped_;
            transport_stopped_ = true;
        }
        // Not under mutex_: the transport thread may be waiting for it in a sink call.
        if (!stop_transport) return;
        transport_->stop();
        if (input_reports_ != 0u || output_reports_ != 0u) {
            std::cerr << "[wiimote] closed: connections=" << attachments_ << " input-reports=" << input_reports_
                      << " output-reports=" << output_reports_ << " output-retries=" << output_retries_
                      << " output-avg-us=" << (output_reports_ != 0u ? output_total_us_ / output_reports_ : 0u)
                      << " output-max-us=" << output_max_us_ << " dropped-output=" << dropped_outputs_
                      << " dropped-input=" << dropped_inputs_ << '\n';
        }
    }

    input::NativeHidDeviceLink device_link() const noexcept override {
        std::lock_guard lock(mutex_);
        return input::NativeHidDeviceLink{attachments_, device_open_};
    }

private:
    void throw_if_failed_locked() const {
        if (!failure_.empty()) throw std::runtime_error("real Wii Remote HID failed: " + failure_);
    }

    // --- TransportSink (the transport's thread) ---

    void remote_opened(const DeviceInfo& info) noexcept override {
        std::uint64_t attachment = 0;
        {
            std::lock_guard lock(mutex_);
            if (device_open_) return;
            device_open_ = true;
            attachment = ++attachments_;
            pending_.clear();
        }
        std::cerr << "[wiimote] connected Nintendo " << product_name(info.product_id)
                  << (info.bluetooth ? " over Bluetooth" : "") << " max-input-report=" << info.max_input_report
                  << (attachment > 1u ? " (again)" : "") << '\n';
        wake_.notify_all();
    }

    void remote_open_failed(const char* reason) noexcept override {
        try {
            std::lock_guard lock(mutex_);
            open_failure_ = reason != nullptr ? reason : "unknown";
        } catch (...) {
        }
        std::cerr << "[wiimote] found a Wii Remote but could not open it: " << (reason != nullptr ? reason : "")
                  << '\n';
    }

    void remote_removed() noexcept override {
        {
            std::lock_guard lock(mutex_);
            if (!device_open_) return;
            device_open_ = false;
            pending_.clear();  // reports of a link that ended
        }
        std::cerr << "[wiimote] disconnected; waiting for it to connect again\n";
        wake_.notify_all();
    }

    void input_report(std::span<const std::uint8_t> report) noexcept override {
        try {
            std::lock_guard lock(mutex_);
            if (!device_open_ || stop_requested_) return;
            pending_.emplace_back(report.begin(), report.end());
            if (pending_.size() > options_.max_pending_input) {
                pending_.pop_front();
                ++dropped_inputs_;
            }
        } catch (...) {
            // Out of memory: this report is lost, as a full OS input buffer would drop it.
        }
    }

    void transport_failed(const char* reason) noexcept override {
        try {
            std::lock_guard lock(mutex_);
            if (failure_.empty()) failure_ = reason != nullptr ? reason : "unknown";
        } catch (...) {
        }
        wake_.notify_all();
    }

    // --- Statistics and trace ---

    void note_write(std::span<const std::uint8_t> report, Clock::duration elapsed, bool written, unsigned attempt) {
        const auto us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
        if (written) {
            ++output_reports_;
            output_total_us_ += us;
            output_max_us_ = std::max(output_max_us_, us);
        } else {
            ++output_retries_;
        }
        if (options_.trace) {
            std::cerr << "[wiimote] out " << hex(report) << (written ? "" : " NOT ACCEPTED") << " attempt=" << attempt
                      << " us=" << us << '\n';
        }
    }

    void trace_input(std::span<const std::uint8_t> report) {
        const std::optional<input::DecodedWiimoteReport> decoded = input::decode_wiimote_input_report(report);
        if (!decoded.has_value()) return;
        const std::string buttons = input::wiimote_buttons_text(decoded->buttons);
        const bool reply = decoded->report_id < 0x30u;
        // Every reply; data reports when their ID or buttons change, and one in 200 otherwise.
        if (reply || decoded->report_id != traced_report_id_ || buttons != traced_buttons_ ||
            ++traced_skipped_ >= 200u) {
            std::cerr << "[wiimote] in " << input::describe_wiimote_report(*decoded) << '\n';
            traced_report_id_ = decoded->report_id;
            traced_buttons_ = buttons;
            traced_skipped_ = 0;
        }
    }

    std::unique_ptr<Transport> transport_;
    BridgeOptions options_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool transport_started_ = false;
    bool transport_stopped_ = false;
    bool stop_requested_ = false;
    bool device_open_ = false;
    std::uint64_t attachments_ = 0;
    std::string failure_;
    std::string open_failure_;
    std::deque<std::vector<std::uint8_t>> pending_;
    std::uint64_t dropped_inputs_ = 0;

    // Coordinator thread (input) and output thread.
    std::atomic<std::uint64_t> input_reports_{0};
    std::atomic<std::uint64_t> output_reports_{0};
    std::atomic<std::uint64_t> output_retries_{0};
    std::atomic<std::uint64_t> dropped_outputs_{0};
    std::atomic<std::uint64_t> output_total_us_{0};
    std::uint64_t output_max_us_ = 0;
    std::uint8_t traced_report_id_ = 0;
    std::string traced_buttons_;
    unsigned traced_skipped_ = 0;
};

// IOHIDManager on a run loop thread of its own. The first matched remote that opens is the one
// used; others are left alone.
class IokitTransport final : public Transport {
public:
    ~IokitTransport() override { stop(); }

    void start(TransportSink& sink) override {
        sink_ = &sink;
        thread_ = std::thread([this] { run(); });
    }

    void request_stop() noexcept override {
        stopping_.store(true, std::memory_order_release);
        std::lock_guard lock(loop_mutex_);
        hid::wake_run_loop(loop_);
    }

    void stop() noexcept override {
        request_stop();
        if (thread_.joinable()) thread_.join();
    }

    bool write(std::span<const std::uint8_t> report) noexcept override {
        hid::Device device = nullptr;
        {
            std::lock_guard lock(device_mutex_);
            device = device_;
            if (device != nullptr) hid::retain(device);
        }
        if (device == nullptr) return false;
        // IOHIDDeviceSetReport may be called from any thread; the retain keeps a device removed
        // meanwhile alive until it returns (with an error).
        const bool written = hid::set_output_report(device, report.data(), report.size());
        hid::release(device);
        return written;
    }

private:
    void run() {
        pthread_setname_np("nebula-wiimote-hid");
        {
            std::lock_guard lock(loop_mutex_);
            loop_ = hid::current_run_loop();
        }
        int open_result = 0;
        hid::Manager manager = hid::create_manager(kNintendoVendorId, kProducts,
            hid::ManagerCallbacks{this, &IokitTransport::on_matched, &IokitTransport::on_removed}, open_result);
        if (manager == nullptr) {
            sink_->transport_failed("IOHIDManagerCreate failed");
        } else if (open_result != 0) {
            std::fprintf(stderr, "[wiimote] IOHIDManagerOpen returned 0x%x\n", static_cast<unsigned>(open_result));
        }
        while (!stopping_.load(std::memory_order_acquire)) {
            hid::run_loop_once(0.25);
            open_matched();
        }
        close_device(true);
        hid::destroy_manager(manager);
        for (hid::Device ref : matched_) hid::release(ref);
        matched_.clear();
        std::lock_guard lock(loop_mutex_);
        loop_ = nullptr;
    }

    static void on_matched(void* context, hid::Device ref) {
        // Retained for us; opened after the callback returns.
        static_cast<IokitTransport*>(context)->matched_.push_back(ref);
    }

    static void on_removed(void* context, hid::Device ref) {
        auto* self = static_cast<IokitTransport*>(context);
        const auto pending = std::find(self->matched_.begin(), self->matched_.end(), ref);
        if (pending != self->matched_.end()) {
            hid::release(*pending);
            self->matched_.erase(pending);
        }
        if (ref == self->opened_) {
            self->close_device(false);  // gone: nothing to close
            self->sink_->remote_removed();
        }
    }

    static void on_input(void* context, const std::uint8_t* report, std::size_t size) {
        static_cast<IokitTransport*>(context)->sink_->input_report(std::span<const std::uint8_t>(report, size));
    }

    void open_matched() {
        while (!matched_.empty()) {
            hid::Device ref = matched_.front();
            matched_.erase(matched_.begin());
            if (opened_ == nullptr && !stopping_.load(std::memory_order_acquire)) open_device(ref);
            hid::release(ref);
        }
    }

    void open_device(hid::Device ref) {
        if (!hid::open_device(ref)) {
            sink_->remote_open_failed("IOHIDDeviceOpen failed");
            return;
        }
        hid::retain(ref);
        opened_ = ref;
        buffer_.assign(static_cast<std::size_t>(std::max(hid::max_input_report_size(ref), 64L)), 0u);
        registration_ = hid::register_input(ref, buffer_.data(), buffer_.size(), &IokitTransport::on_input, this);
        {
            std::lock_guard lock(device_mutex_);
            device_ = ref;
        }
        DeviceInfo info;
        info.product_id = static_cast<std::uint16_t>(hid::product_id(ref));
        info.bluetooth = hid::is_bluetooth(ref);
        info.max_input_report = static_cast<std::size_t>(std::max(hid::max_input_report_size(ref), 0L));
        sink_->remote_opened(info);
    }

    void close_device(bool still_attached) {
        if (opened_ == nullptr) return;
        {
            std::lock_guard lock(device_mutex_);
            device_ = nullptr;
        }
        hid::unregister_input(opened_, buffer_.data(), buffer_.size(), registration_);
        registration_ = nullptr;
        if (still_attached) hid::close_device(opened_);
        hid::release(opened_);
        opened_ = nullptr;
    }

    TransportSink* sink_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex loop_mutex_;
    hid::RunLoop loop_ = nullptr;

    // Run loop thread only.
    std::vector<hid::Device> matched_;
    hid::Device opened_ = nullptr;
    std::vector<std::uint8_t> buffer_;
    void* registration_ = nullptr;

    // The open remote for write(), from the worker's output thread.
    std::mutex device_mutex_;
    hid::Device device_ = nullptr;
};

}  // namespace

std::unique_ptr<input::NativeHidIoBackend> make_bridge(std::unique_ptr<Transport> transport, BridgeOptions options) {
    return std::make_unique<Bridge>(std::move(transport), options);
}

std::unique_ptr<Transport> make_iokit_transport() { return std::make_unique<IokitTransport>(); }

std::unique_ptr<input::NativeHidIoBackend> make_wiimote_hid_backend() {
    BridgeOptions options;
    options.trace = env_flag("GALAXY_TRACE_WIIMOTE");
    return make_bridge(make_iokit_transport(), options);
}

bool wiimote_connected() { return hid::device_present(kNintendoVendorId, kProducts); }

}  // namespace galaxy::macos::wiimote
