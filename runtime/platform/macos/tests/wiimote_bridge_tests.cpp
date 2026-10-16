// SPDX-License-Identifier: GPL-3.0-only
//
// The macOS Wii Remote bridge (galaxy_macos_wiimote.h) over a fake transport: discovery, report
// normalisation, output reports, retries, removal and reconnection, shutdown, alone and behind the
// runtime's NativeHidIoWorker. The last check runs the real IOKit transport on this Mac.

#include "galaxy_macos_wiimote.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace wiimote = galaxy::macos::wiimote;
using Bytes = std::vector<std::uint8_t>;
using std::chrono::milliseconds;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct FakeRemote {
    std::mutex mutex;
    wiimote::TransportSink* sink = nullptr;
    bool connect_on_start = true;
    const char* open_failure = nullptr;
    bool started = false;
    bool stop_requested = false;
    bool stopped = false;
    unsigned reject_writes = 0;  // refused before writes are accepted
    bool accept_writes = true;
    std::vector<Bytes> written;

    wiimote::TransportSink& live_sink() {
        std::lock_guard lock(mutex);
        if (sink == nullptr) throw std::logic_error("the fake remote's transport is not running");
        return *sink;
    }
    void send(Bytes report) { live_sink().input_report(report); }
    void remove() { live_sink().remote_removed(); }
    std::size_t write_count() {
        std::lock_guard lock(mutex);
        return written.size();
    }
};

class FakeTransport final : public wiimote::Transport {
public:
    explicit FakeTransport(std::shared_ptr<FakeRemote> remote) : remote_(std::move(remote)) {}

    void start(wiimote::TransportSink& sink) override {
        bool connect = false;
        const char* failure = nullptr;
        {
            std::lock_guard lock(remote_->mutex);
            remote_->sink = &sink;
            remote_->started = true;
            connect = remote_->connect_on_start;
            failure = remote_->open_failure;
        }
        if (failure != nullptr) sink.remote_open_failed(failure);
        if (connect) sink.remote_opened(wiimote::DeviceInfo{wiimote::kWiiRemoteProductId, true, 22});
    }

    void request_stop() noexcept override {
        std::lock_guard lock(remote_->mutex);
        remote_->stop_requested = true;
    }

    void stop() noexcept override {
        std::lock_guard lock(remote_->mutex);
        remote_->stopped = true;
        remote_->sink = nullptr;
    }

    bool write(std::span<const std::uint8_t> report) noexcept override {
        std::lock_guard lock(remote_->mutex);
        remote_->written.emplace_back(report.begin(), report.end());
        if (remote_->reject_writes > 0u) {
            --remote_->reject_writes;
            return false;
        }
        return remote_->accept_writes;
    }

private:
    std::shared_ptr<FakeRemote> remote_;
};

wiimote::BridgeOptions fast_options() {
    wiimote::BridgeOptions options;
    options.discovery_timeout = milliseconds(50);
    options.write_retry_interval = milliseconds(1);
    options.write_attempts = 5;
    return options;
}

std::unique_ptr<galaxy::input::NativeHidIoBackend> make(
    const std::shared_ptr<FakeRemote>& remote, wiimote::BridgeOptions options = fast_options()) {
    return wiimote::make_bridge(std::make_unique<FakeTransport>(remote), options);
}

std::vector<std::byte> framed(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> out;
    for (const std::uint8_t byte : bytes) out.push_back(static_cast<std::byte>(byte));
    return out;
}

Bytes plain(const std::vector<std::byte>& bytes) {
    Bytes out;
    for (const std::byte byte : bytes) out.push_back(std::to_integer<std::uint8_t>(byte));
    return out;
}

// The message of what `action` throws, or "" when it returns.
std::string thrown(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

bool wait_until(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

void open_fails_without_a_remote() {
    auto remote = std::make_shared<FakeRemote>();
    remote->connect_on_start = false;
    auto bridge = make(remote);
    const auto begin = std::chrono::steady_clock::now();
    const std::string error = thrown([&] { bridge->open(); });
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    check(contains(error, "no Wii Remote is connected"), "open without a remote fails with the pairing message");
    check(contains(error, "System Settings > Bluetooth"), "the failure says where to pair");
    check(elapsed >= milliseconds(45) && elapsed < std::chrono::seconds(2), "open waits for the discovery timeout");
    bridge->close();
    check(remote->stopped, "close stops the transport");
}

void open_failure_reason_is_reported() {
    auto remote = std::make_shared<FakeRemote>();
    remote->connect_on_start = false;
    remote->open_failure = "IOHIDDeviceOpen failed";
    auto bridge = make(remote);
    const std::string error = thrown([&] { bridge->open(); });
    check(contains(error, "could not be opened: IOHIDDeviceOpen failed"), "a remote that cannot be opened is named");
}

void input_reports_are_framed_for_the_game() {
    auto remote = std::make_shared<FakeRemote>();
    auto bridge = make(remote);
    bridge->open();
    std::vector<std::byte> report;
    check(!bridge->poll_input(report), "no report before the remote sends one");

    remote->send({0x30, 0x00, 0x08});
    check(bridge->poll_input(report) && plain(report) == Bytes{0xa1, 0x30, 0x00, 0x08}, "0x30 gets its A1 header");

    Bytes full{0x37, 0x00, 0x04, 0x7f, 0x81, 0x99, 0xa4, 0x7c, 0x52, 0xbc, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80,
        0x80, 0x7a, 0x7e, 0x9c, 0x9e};
    remote->send(full);
    Bytes expected = full;
    expected.insert(expected.begin(), 0xa1);
    check(bridge->poll_input(report) && plain(report) == expected, "0x37 passes through unchanged");

    // A buffer longer than the report (IOKit sizes it to the largest report) is cut to the report.
    Bytes padded{0x20, 0x00, 0x00, 0x12, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00};
    remote->send(padded);
    check(bridge->poll_input(report) && plain(report) == Bytes{0xa1, 0x20, 0x00, 0x00, 0x12, 0x00, 0x00, 0xc8},
        "status report cut to its size");
    check(!bridge->poll_input(report), "each report is delivered once");
    bridge->close();
}

void malformed_input_fails() {
    auto remote = std::make_shared<FakeRemote>();
    auto bridge = make(remote);
    bridge->open();
    std::vector<std::byte> report;
    remote->send({0x3e, 0x00, 0x00});
    check(contains(thrown([&] { (void)bridge->poll_input(report); }), "unsupported input report: 3e 00 00"),
        "an unsupported report fails like on Windows");
    remote->send({0x37, 0x00});
    check(contains(thrown([&] { (void)bridge->poll_input(report); }), "truncated input report"),
        "a truncated report fails like on Windows");
}

void output_reports_go_out_at_their_own_length() {
    auto remote = std::make_shared<FakeRemote>();
    auto bridge = make(remote);
    bridge->open();
    check(bridge->write_output(framed({0xa2, 0x12, 0x04, 0x37})), "report mode written");
    check(bridge->write_output(framed({0xa2, 0x11, 0x10})), "LEDs written");
    std::vector<std::byte> write_memory = framed({0xa2, 0x16, 0x04, 0xa4, 0x00, 0xf0, 0x01, 0x55});
    write_memory.resize(23, std::byte{0});
    check(bridge->write_output(write_memory), "register write written");
    {
        std::lock_guard lock(remote->mutex);
        check(remote->written.size() == 3u, "three output reports");
        check(remote->written[0] == Bytes({0x12, 0x04, 0x37}), "report ID first, no A2, no padding");
        check(remote->written[1] == Bytes({0x11, 0x10}), "LED report");
        check(remote->written[2].size() == 22u && remote->written[2][0] == 0x16 && remote->written[2][6] == 0x55,
            "register write report");
    }
    check(contains(thrown([&] { (void)bridge->write_output(framed({0x12, 0x04, 0x37})); }), "A2 output report"),
        "an unframed output report is refused");
    check(contains(thrown([&] { (void)bridge->write_output(framed({0xa2})); }), "A2 output report"),
        "an output report without an ID is refused");
}

void refused_writes_are_retried_then_fail() {
    auto remote = std::make_shared<FakeRemote>();
    remote->reject_writes = 2;
    auto bridge = make(remote);
    bridge->open();
    check(bridge->write_output(framed({0xa2, 0x11, 0x10})), "a write accepted on the third attempt succeeds");
    check(remote->write_count() == 3u, "two retries");

    remote->accept_writes = false;
    const std::string error = thrown([&] { (void)bridge->write_output(framed({0xa2, 0x15, 0x00})); });
    check(contains(error, "output report 0x15 was not accepted after 5 attempts"), "a refused write fails in the end");
    check(remote->write_count() == 8u, "five attempts");
}

bool same_link(const galaxy::input::NativeHidDeviceLink& link, std::uint64_t attachments, bool attached) {
    return link.attachments == attachments && link.attached == attached;
}

void a_removed_remote_is_waited_for() {
    auto remote = std::make_shared<FakeRemote>();
    auto bridge = make(remote);
    check(same_link(bridge->device_link(), 0, false), "no device link before open");
    bridge->open();
    check(same_link(bridge->device_link(), 1, true), "the first remote is attachment 1");

    remote->send({0x30, 0x00, 0x08});
    remote->remove();
    check(same_link(bridge->device_link(), 1, false), "the removal is reported");
    std::vector<std::byte> report;
    check(thrown([&] { check(!bridge->poll_input(report), "reports of the ended link are dropped"); }).empty(),
        "a removed remote does not fail the input");
    const std::size_t writes = remote->write_count();
    bool written = false;
    check(thrown([&] { written = bridge->write_output(framed({0xa2, 0x11, 0x10})); }).empty() && written,
        "an output report for a removed remote is consumed");
    check(remote->write_count() == writes, "and goes nowhere");

    // The remote comes back as a new HID device: the bridge uses it.
    remote->live_sink().remote_opened(wiimote::DeviceInfo{wiimote::kWiiRemoteProductId, true, 22});
    check(same_link(bridge->device_link(), 2, true), "the remote that comes back is attachment 2");
    remote->send({0x30, 0x00, 0x04});
    check(bridge->poll_input(report) && plain(report) == Bytes{0xa1, 0x30, 0x00, 0x04}, "its reports reach the game");
    check(bridge->write_output(framed({0xa2, 0x11, 0x20})) && remote->write_count() == writes + 1u,
        "and the game's output reaches it");
    {
        std::lock_guard lock(remote->mutex);
        check(remote->written.back() == Bytes({0x11, 0x20}), "the output report after the reconnection");
    }

    // A second open report for the same attachment changes nothing.
    remote->live_sink().remote_opened(wiimote::DeviceInfo{wiimote::kWiiRemoteProductId, true, 22});
    check(same_link(bridge->device_link(), 2, true), "a duplicate open is ignored");
}

void a_removal_ends_pending_retries() {
    auto remote = std::make_shared<FakeRemote>();
    remote->accept_writes = false;
    wiimote::BridgeOptions options = fast_options();
    options.write_attempts = 100000;
    options.write_retry_interval = milliseconds(10);
    auto bridge = make(remote, options);
    bridge->open();
    bool written = false;
    std::string error = "not run";
    std::thread writer([&] { error = thrown([&] { written = bridge->write_output(framed({0xa2, 0x11, 0x10})); }); });
    std::this_thread::sleep_for(milliseconds(40));
    const auto begin = std::chrono::steady_clock::now();
    remote->remove();
    writer.join();
    check(error.empty() && written, "a write retrying when the remote goes away is consumed");
    check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1), "without waiting out its retries");
}

void stop_cancels_discovery_and_retries() {
    {
        auto remote = std::make_shared<FakeRemote>();
        remote->connect_on_start = false;
        wiimote::BridgeOptions options = fast_options();
        options.discovery_timeout = std::chrono::seconds(10);
        auto bridge = make(remote, options);
        std::string error = "not run";
        const auto begin = std::chrono::steady_clock::now();
        std::thread opener([&] { error = thrown([&] { bridge->open(); }); });
        std::this_thread::sleep_for(milliseconds(30));
        bridge->request_stop();
        opener.join();
        check(error.empty(), "a stopped discovery returns without failing");
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2), "request_stop ends discovery at once");
        check(remote->stop_requested, "request_stop reaches the transport");
        bridge->close();
    }
    {
        auto remote = std::make_shared<FakeRemote>();
        remote->accept_writes = false;
        wiimote::BridgeOptions options = fast_options();
        options.write_attempts = 100000;
        options.write_retry_interval = milliseconds(10);
        auto bridge = make(remote, options);
        bridge->open();
        bool written = true;
        std::thread writer([&] { written = bridge->write_output(framed({0xa2, 0x11, 0x10})); });
        std::this_thread::sleep_for(milliseconds(40));
        const auto begin = std::chrono::steady_clock::now();
        bridge->request_stop();
        writer.join();
        check(!written, "a stopped write reports the cancellation");
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1), "request_stop ends the retries");
        std::vector<std::byte> report;
        check(!bridge->poll_input(report), "no input after stop");
    }
}

void input_backlog_is_bounded() {
    auto remote = std::make_shared<FakeRemote>();
    wiimote::BridgeOptions options = fast_options();
    options.max_pending_input = 4;
    auto bridge = make(remote, options);
    bridge->open();
    for (std::uint8_t i = 0; i < 10; ++i) remote->send({0x30, 0x00, i});
    std::vector<std::uint8_t> seen;
    std::vector<std::byte> report;
    while (bridge->poll_input(report)) seen.push_back(std::to_integer<std::uint8_t>(report[3]));
    check(seen == std::vector<std::uint8_t>({6, 7, 8, 9}), "the oldest reports are dropped past the bound");
}

void close_is_idempotent_and_final() {
    auto remote = std::make_shared<FakeRemote>();
    auto bridge = make(remote);
    bridge->open();
    bridge->close();
    bridge->close();
    check(remote->stopped && remote->sink == nullptr, "the transport is stopped once closed");
    std::vector<std::byte> report;
    check(!bridge->poll_input(report), "a closed bridge reads nothing");
    check(!bridge->write_output(framed({0xa2, 0x11, 0x10})), "a closed bridge writes nothing");
    auto never_opened = make(std::make_shared<FakeRemote>());
    never_opened->close();
}

void works_behind_the_runtime_worker() {
    auto remote = std::make_shared<FakeRemote>();
    galaxy::input::NativeHidIoWorker worker(make(remote));
    worker.start();
    check(wait_until([&] { return worker.stats().opened; }), "the worker opens the bridge");
    check(same_link(worker.device_link(), 1, true), "the worker reports the device link");

    remote->send({0x30, 0x00, 0x08});
    check(wait_until([&] { return worker.input_occupied(); }), "the worker polls the remote's report");
    const galaxy::input::NativeHidIoTakeResult taken = worker.take_latest_input(64);
    check(taken.status == galaxy::input::NativeHidIoTakeStatus::Ok &&
              plain(taken.report.packet) == Bytes{0xa1, 0x30, 0x00, 0x08},
        "the game receives the framed report");

    worker.enqueue_output(framed({0xa2, 0x11, 0x10}));
    check(wait_until([&] { return remote->write_count() == 1u; }), "the worker writes the game's output report");

    remote->remove();
    check(same_link(worker.device_link(), 1, false), "the worker reports the removal");
    worker.enqueue_output(framed({0xa2, 0x15, 0x00}));
    check(wait_until([&] { return worker.stats().output_reports_completed == 2u; }),
        "output for the removed remote is consumed");
    std::this_thread::sleep_for(milliseconds(20));
    check(!worker.stats().failed && thrown([&] { worker.throw_if_failed(); }).empty(),
        "a removed remote does not fail the worker");
    check(remote->write_count() == 1u, "nothing is written while no remote is attached");

    remote->live_sink().remote_opened(wiimote::DeviceInfo{wiimote::kWiiRemoteProductId, true, 22});
    check(same_link(worker.device_link(), 2, true), "the worker reports the remote that came back");
    remote->send({0x30, 0x00, 0x04});
    check(wait_until([&] { return worker.input_occupied(); }), "the worker polls the new remote");
    const galaxy::input::NativeHidIoTakeResult again = worker.take_latest_input(64);
    check(again.status == galaxy::input::NativeHidIoTakeStatus::Ok &&
              plain(again.report.packet) == Bytes{0xa1, 0x30, 0x00, 0x04},
        "the game receives the new remote's report");
    worker.enqueue_output(framed({0xa2, 0x11, 0x10}));
    check(wait_until([&] { return remote->write_count() == 2u; }), "and the new remote gets the game's output");
    worker.stop();
    check(remote->stopped, "the worker closes the bridge");
}

void iokit_transport_on_this_mac() {
    if (wiimote::wiimote_connected()) {
        std::printf("A Wii Remote is connected: checking that it opens\n");
        wiimote::BridgeOptions options;
        auto bridge = wiimote::make_bridge(wiimote::make_iokit_transport(), options);
        check(thrown([&] { bridge->open(); }).empty(), "the connected Wii Remote opens");
        check(bridge->write_output(framed({0xa2, 0x15, 0x00})), "a status request reaches the remote");
        bridge->close();
        return;
    }
    wiimote::BridgeOptions options;
    options.discovery_timeout = milliseconds(300);
    auto bridge = wiimote::make_bridge(wiimote::make_iokit_transport(), options);
    const auto begin = std::chrono::steady_clock::now();
    const std::string error = thrown([&] { bridge->open(); });
    bridge->close();
    check(contains(error, "no Wii Remote is connected"), "IOKit finds no Wii Remote on this Mac");
    check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3), "IOKit discovery and teardown are bounded");
}

}  // namespace

int main() {
    open_fails_without_a_remote();
    open_failure_reason_is_reported();
    input_reports_are_framed_for_the_game();
    malformed_input_fails();
    output_reports_go_out_at_their_own_length();
    refused_writes_are_retried_then_fail();
    a_removed_remote_is_waited_for();
    a_removal_ends_pending_retries();
    stop_cancels_discovery_and_retries();
    input_backlog_is_bounded();
    close_is_idempotent_and_final();
    works_behind_the_runtime_worker();
    iokit_transport_on_this_mac();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d Wii Remote bridge checks failed\n", g_failures);
        return 1;
    }
    std::printf("Wii Remote bridge checks passed\n");
    return 0;
}
