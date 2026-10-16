// SPDX-License-Identifier: GPL-3.0-only
//
// Process entry on macOS. AppKit must own the main thread, so the runtime's wmain runs on a
// thread of its own (with the 256 MiB stack the Windows image reserves for its main thread:
// cooperative guest threads nest on the host stack) while
// the main thread runs the Cocoa event loop that the Win32 window emulation drives.

#import <Foundation/Foundation.h>

#include <windows.h>

#include "win32_internal.h"

#include <pthread.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv);

namespace {

struct RuntimeArguments {
    std::vector<std::wstring> storage;
    std::vector<wchar_t*> pointers;
    int exit_code = 0;
};

struct EnvironmentDefault {
    const char* name;
    const char* value;
};

// installer/runtime-env.json: what the Windows launcher sets for every game launch.
constexpr EnvironmentDefault kLauncherEnvironment[] = {
#include "runtime_env_defaults.inc"
};

// LaunchOptions.Create and ApplyRecording(RecordingMode.None) set these after the table.
constexpr EnvironmentDefault kLauncherOverrides[] = {
    {"GALAXY_NATIVE_THP_VIDEO_DECODE", "1"},
    {"GALAXY_DUSK_DSP_HLE", "0"},
    {"GALAXY_TRACE_PRESENT_STATS", "0"},
    {"GALAXY_MONITOR_POINTER_LATENCY", "0"},
    {"GALAXY_MONITOR_FRAME_TAILS", "0"},
    {"GALAXY_MONITOR_DISPLAY_LAYOUT", "0"},
    {"GALAXY_FRAME_TELEMETRY", "0"},
    {"GALAXY_TRACE_IOS_ANOMALIES", "0"},
    {"GALAXY_DIAGNOSTIC_IOS_COUNTERS", "0"},
    {"GALAXY_TRACE_NATIVE_THP_BOUNDARY", "0"},
    {"GALAXY_TRACE_GX_STALLS", "0"},
    {"GALAXY_SIM_PROFILE", "0"},
    {"GALAXY_FRAME_TIMELINE", "0"},
    {"GALAXY_FRAME_TIMELINE_EVENTS", "4000000"},
    {"GALAXY_GPU_TIMESTAMPS", "0"},
    {"GALAXY_TRACE_GX_MICROPROFILE", "0"},
    {"GALAXY_GX_FRAME_TIMING_SAMPLE", "0"},
    {"GALAXY_GX_PSO_CYCLES", "0"},
    {"GALAXY_TRACE_GX_STALL_US", "20000"},
};

// macOS has no launcher, so the entry point applies its environment. Values already in the
// environment win (development and diagnostic runs set their own); GALAXY_LAUNCHER_DEFAULTS=0
// starts the runtime with the bare runtime defaults instead.
void apply_launcher_environment() {
    const char* enabled = std::getenv("GALAXY_LAUNCHER_DEFAULTS");
    if (enabled != nullptr && std::strcmp(enabled, "0") == 0) return;
    for (const EnvironmentDefault& entry : kLauncherOverrides) setenv(entry.name, entry.value, 0);
    for (const EnvironmentDefault& entry : kLauncherEnvironment) setenv(entry.name, entry.value, 0);
}

void* runtime_thread(void* context) {
    auto* arguments = static_cast<RuntimeArguments*>(context);
    pthread_setname_np("Orbit Runtime");
    arguments->exit_code = wmain(static_cast<int>(arguments->storage.size()), arguments->pointers.data());
    galaxy::macos::stop_application();
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    // Among them GALAXY_LAYOUT_ASPECT_REPAIRS, which fits full-screen 2D backgrounds, cinema bars
    // and HUD edges to displays taller than 16:9 (Mac displays are 16:10 or taller).
    apply_launcher_environment();

    static RuntimeArguments arguments;
    for (int i = 0; i < argc; ++i) arguments.storage.push_back(galaxy::compat::wide_from_utf8(argv[i]));
    for (std::wstring& argument : arguments.storage) arguments.pointers.push_back(argument.data());
    arguments.pointers.push_back(nullptr);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 256u << 20u);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, runtime_thread, &arguments) != 0) return 1;
    pthread_attr_destroy(&attributes);

    galaxy::macos::run_application();
    pthread_join(thread, nullptr);
    return arguments.exit_code;
}
