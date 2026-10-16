# macOS on Apple silicon: the runtime with a Metal renderer, Core Audio output and Game
# Controller input. Included from the top-level CMakeLists.txt after the SoftFloat source
# lists; the Windows targets below that point are not defined on this platform.

if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    message(FATAL_ERROR "The macOS build targets Apple silicon (arm64).")
endif()

enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

# Guest floating-point results depend on every IEEE operation rounding once. Apple clang
# contracts a*b+c into a fused multiply-add by default on arm64, which MSVC's /fp:precise
# (the Windows build) never does: galaxy_ppc_float's TwoSum/TwoProduct error terms and the
# 25-bit multiplier rounding would silently change. The code was also written against MSVC's
# aliasing and signed-overflow behavior.
add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-ffp-contract=off>
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fno-strict-aliasing>
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fwrapv>)
# std::osyncstream is behind libc++'s experimental-library switch.
add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fexperimental-library>)
add_link_options(-fexperimental-library)

set(NEBULA_MACOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform/macos/src")

# ---------------------------------------------------------------------------------------------
# Arithmetic libraries shared with the generated modules
# ---------------------------------------------------------------------------------------------

add_library(
    galaxy_softfloat
    STATIC
    ${SOFTFLOAT_COMMON_NAMES}
    ${SOFTFLOAT_SPECIALIZE_NAMES}
)
target_include_directories(
    galaxy_softfloat
    PUBLIC
        "${SOFTFLOAT_ROOT}/source/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/softfloat"
        "${SOFTFLOAT_ROOT}/source/8086-SSE"
)
target_compile_definitions(
    galaxy_softfloat
    PRIVATE
        SOFTFLOAT_FAST_INT64=1
        SOFTFLOAT_FAST_DIV32TO16=1
        SOFTFLOAT_FAST_DIV64TO32=1
        INLINE_LEVEL=5
)
target_compile_options(galaxy_softfloat PRIVATE -w)

add_library(galaxy_ppc_float STATIC runtime/src/ppc_float.cpp)
target_compile_definitions(galaxy_ppc_float PRIVATE SOFTFLOAT_FAST_INT64=1)
target_include_directories(
    galaxy_ppc_float
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/softfloat"
)
target_link_libraries(galaxy_ppc_float PUBLIC galaxy_softfloat)

add_library(galaxy_dsp_alu STATIC runtime/src/dsp_alu.cpp)
target_include_directories(galaxy_dsp_alu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")

# ---------------------------------------------------------------------------------------------
# Win32 emulation and macOS platform services (runtime/platform/macos)
# ---------------------------------------------------------------------------------------------

add_library(galaxy_macos_platform STATIC
    ${NEBULA_MACOS_DIR}/win32_kernel.cpp
    ${NEBULA_MACOS_DIR}/win32_files.cpp
    ${NEBULA_MACOS_DIR}/win32_fiber.cpp
    ${NEBULA_MACOS_DIR}/win32_window.mm
    ${NEBULA_MACOS_DIR}/macos_platform.cpp
    ${NEBULA_MACOS_DIR}/xaudio2_coreaudio.cpp
    ${NEBULA_MACOS_DIR}/xinput_gamecontroller.mm
    ${NEBULA_MACOS_DIR}/controller_hid_macos.cpp
    ${NEBULA_MACOS_DIR}/iokit_hid.cpp
    ${NEBULA_MACOS_DIR}/iokit_usb.mm)
target_include_directories(galaxy_macos_platform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform/macos/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sse2neon")
target_compile_options(galaxy_macos_platform PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(galaxy_macos_platform PUBLIC
    "-framework AudioToolbox" "-framework CoreAudio" "-framework GameController"
    "-framework CoreHaptics" "-framework Foundation" "-framework AppKit" "-framework QuartzCore"
    "-framework Carbon" "-framework IOKit" "-framework IOUSBHost" "-framework CoreGraphics")

add_library(galaxy_runtime_abi INTERFACE)
target_include_directories(galaxy_runtime_abi INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform/macos/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sse2neon")
target_link_libraries(galaxy_runtime_abi INTERFACE galaxy_macos_platform)

add_library(galaxy_native_dsp STATIC runtime/src/native_dsp.cpp)
target_link_libraries(galaxy_native_dsp PUBLIC galaxy_dsp_alu galaxy_runtime_abi)
# The macOS runtime loads the generated DSP from RMGE01_dsp.dylib, like the Windows release.
target_compile_definitions(galaxy_native_dsp PUBLIC GALAXY_HAVE_NATIVE_DSP=1 GALAXY_NATIVE_DSP_MODULE=1)

# ---------------------------------------------------------------------------------------------
# Platform-neutral runtime libraries
# ---------------------------------------------------------------------------------------------

add_library(galaxy_checkpoint_gate STATIC runtime/src/checkpoint_gate.cpp)
target_link_libraries(galaxy_checkpoint_gate PUBLIC galaxy_runtime_abi)

add_library(galaxy_decrementer_deadline STATIC runtime/src/decrementer_deadline.cpp)
target_link_libraries(galaxy_decrementer_deadline PUBLIC galaxy_runtime_abi)

add_library(galaxy_frame_cadence_diagnostics STATIC runtime/src/frame_cadence_diagnostics.cpp)
target_link_libraries(galaxy_frame_cadence_diagnostics PUBLIC galaxy_runtime_abi)

add_library(galaxy_host_pointer_events STATIC runtime/src/host_pointer_events.cpp)
target_link_libraries(galaxy_host_pointer_events PUBLIC galaxy_runtime_abi)

add_library(galaxy_native_input STATIC
    runtime/src/native_input.cpp
    runtime/src/pointer_control.cpp
    runtime/src/rumble.cpp
    runtime/src/switch_controller.cpp
    runtime/src/tilt.cpp
    runtime/src/wiimote_report.cpp)
target_link_libraries(galaxy_native_input PUBLIC galaxy_runtime_abi)

# Real Wii Remotes over IOKit HID (input_mode=real_wiimote), the macOS counterpart of the Windows
# HID bridge in native_host.cpp.
add_library(galaxy_macos_wiimote STATIC ${NEBULA_MACOS_DIR}/wiimote_hid_macos.cpp)
target_link_libraries(galaxy_macos_wiimote PUBLIC galaxy_native_input galaxy_macos_platform)

add_library(galaxy_native_input_causality STATIC runtime/src/native_input_causality.cpp)
target_link_libraries(galaxy_native_input_causality PUBLIC galaxy_runtime_abi)

add_library(galaxy_ai_pcm_identity STATIC runtime/src/ai_pcm_identity.cpp)
target_link_libraries(galaxy_ai_pcm_identity PUBLIC galaxy_runtime_abi)

add_library(galaxy_jaudio_mixer STATIC runtime/src/jaudio_host_mixer.cpp)
target_include_directories(galaxy_jaudio_mixer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include")

add_library(galaxy_test_module SHARED runtime/src/test_module.cpp)
target_link_libraries(galaxy_test_module PRIVATE galaxy_runtime_abi)
target_compile_definitions(galaxy_test_module PRIVATE GALAXY_BUILDING_GAME_MODULE=1)
set_target_properties(galaxy_test_module PROPERTIES PREFIX "")

add_library(galaxy_atomic_file STATIC runtime/src/atomic_file.cpp)
target_link_libraries(galaxy_atomic_file PUBLIC galaxy_runtime_abi)

add_library(galaxy_runtime_instance STATIC runtime/src/runtime_instance.cpp)
target_link_libraries(galaxy_runtime_instance PUBLIC galaxy_runtime_abi)

add_library(galaxy_runtime_settings STATIC
    runtime/src/runtime_settings.cpp
    runtime/src/input_bindings.cpp)
target_link_libraries(galaxy_runtime_settings PUBLIC galaxy_runtime_abi galaxy_atomic_file)

add_library(galaxy_runtime_timeline STATIC runtime/src/runtime_timeline.cpp)
target_link_libraries(galaxy_runtime_timeline PUBLIC galaxy_runtime_abi)

add_library(galaxy_native_host STATIC
    runtime/src/native_host.cpp
    runtime/src/flat_guest_memory.cpp)
target_link_libraries(galaxy_native_host PUBLIC
    galaxy_runtime_abi
    galaxy_runtime_settings
    galaxy_native_input
    galaxy_macos_wiimote
    galaxy_native_dsp
    galaxy_frame_cadence_diagnostics
    galaxy_ai_pcm_identity)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------

enable_testing()

function(nebula_macos_test name target)
    add_test(NAME ${name} COMMAND ${target} ${ARGN})
endfunction()

add_executable(galaxy_ppc_float_tests runtime/src/ppc_float_tests.cpp)
target_link_libraries(galaxy_ppc_float_tests PRIVATE galaxy_ppc_float)
target_compile_definitions(galaxy_ppc_float_tests PRIVATE SOFTFLOAT_FAST_INT64=1)
target_include_directories(galaxy_ppc_float_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/softfloat")
nebula_macos_test(ppc_float_helpers galaxy_ppc_float_tests)

add_executable(galaxy_native_api_tests runtime/src/native_api_tests.cpp)
target_link_libraries(galaxy_native_api_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float)
nebula_macos_test(native_api_helpers galaxy_native_api_tests)

add_executable(galaxy_native_fast_float_tests runtime/src/native_fast_float_tests.cpp)
target_compile_definitions(galaxy_native_fast_float_tests PRIVATE GALAXY_FAST_FLOAT=1)
target_link_libraries(galaxy_native_fast_float_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float)
nebula_macos_test(native_fast_float galaxy_native_fast_float_tests)

add_executable(galaxy_native_hw_float_conversion_tests runtime/src/native_hw_float_conversion_tests.cpp)
target_compile_definitions(galaxy_native_hw_float_conversion_tests PRIVATE GALAXY_FAST_FLOAT=1)
target_link_libraries(galaxy_native_hw_float_conversion_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float)
nebula_macos_test(native_hw_float_conversion galaxy_native_hw_float_conversion_tests)

add_executable(galaxy_native_assume_paired_singles_tests runtime/src/native_assume_paired_singles_tests.cpp)
target_compile_definitions(galaxy_native_assume_paired_singles_tests PRIVATE
    GALAXY_FAST_FLOAT=1 GALAXY_ASSUME_PAIRED_SINGLES=1)
target_link_libraries(galaxy_native_assume_paired_singles_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float)
nebula_macos_test(native_assume_paired_singles galaxy_native_assume_paired_singles_tests)

add_executable(galaxy_native_single_float_arithmetic_tests runtime/src/native_single_float_arithmetic_tests.cpp)
target_compile_definitions(galaxy_native_single_float_arithmetic_tests PRIVATE
    GALAXY_FAST_FLOAT=1 GALAXY_SINGLE_FLOAT_ARITHMETIC=1 GALAXY_ASSUME_PAIRED_SINGLES=1)
target_link_libraries(galaxy_native_single_float_arithmetic_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float)
nebula_macos_test(native_single_float_arithmetic galaxy_native_single_float_arithmetic_tests)

add_executable(galaxy_native_flat_view_only_tests runtime/src/native_flat_view_only_tests.cpp)
target_compile_definitions(galaxy_native_flat_view_only_tests PRIVATE
    GALAXY_FAST_FLOAT=1 GALAXY_FASTMEM=1 GALAXY_FLAT_VIEW_ONLY=1)
target_link_libraries(galaxy_native_flat_view_only_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float)
nebula_macos_test(native_flat_view_only galaxy_native_flat_view_only_tests)

foreach(test_name IN ITEMS
        native_module_checkpoint_gate native_bctrl_site_cache native_ios_clock native_multiple_gpr
        native_checkpoint_policy native_cached_call native_zero native_yaz0_span same_context_rfi
        native_audio_queue thp_audio_worker diagnostic_stick_lease simulation_cache_affinity
        layout_aspect flat_dispatch opening_route_marker function_async_flight external_dispatch
        vi_boundary)
    add_executable(galaxy_${test_name}_tests runtime/src/${test_name}_tests.cpp)
    target_link_libraries(galaxy_${test_name}_tests PRIVATE galaxy_runtime_abi galaxy_ppc_float
        galaxy_checkpoint_gate)
    nebula_macos_test(${test_name} galaxy_${test_name}_tests)
endforeach()

add_executable(galaxy_owned_fifo_packet_tests runtime/src/gx/owned_fifo_packet_tests.cpp)
target_link_libraries(galaxy_owned_fifo_packet_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(owned_fifo_packet_ownership galaxy_owned_fifo_packet_tests)

add_executable(galaxy_dsp_alu_tests runtime/src/dsp_alu_tests.cpp)
target_link_libraries(galaxy_dsp_alu_tests PRIVATE galaxy_dsp_alu)
nebula_macos_test(dsp_alu_helpers galaxy_dsp_alu_tests)

add_executable(galaxy_checkpoint_gate_tests runtime/src/checkpoint_gate_tests.cpp)
target_link_libraries(galaxy_checkpoint_gate_tests PRIVATE galaxy_checkpoint_gate galaxy_runtime_abi)
nebula_macos_test(checkpoint_gate_policy galaxy_checkpoint_gate_tests)

add_executable(galaxy_decrementer_deadline_tests runtime/src/decrementer_deadline_tests.cpp)
target_link_libraries(galaxy_decrementer_deadline_tests PRIVATE galaxy_decrementer_deadline)
nebula_macos_test(decrementer_deadline_policy galaxy_decrementer_deadline_tests)

add_executable(galaxy_frame_cadence_diagnostics_tests runtime/src/frame_cadence_diagnostics_tests.cpp)
target_link_libraries(galaxy_frame_cadence_diagnostics_tests PRIVATE galaxy_frame_cadence_diagnostics)
nebula_macos_test(frame_cadence_diagnostics galaxy_frame_cadence_diagnostics_tests)

# Spawns child processes through CreateProcessW: Windows only.
# add_executable(galaxy_host_pointer_events_tests runtime/src/host_pointer_events_tests.cpp)
# target_link_libraries(galaxy_host_pointer_events_tests PRIVATE galaxy_host_pointer_events)
# nebula_macos_test(host_pointer_events galaxy_host_pointer_events_tests)

add_executable(galaxy_native_input_tests runtime/src/native_input_tests.cpp)
target_link_libraries(galaxy_native_input_tests PRIVATE galaxy_native_input)
nebula_macos_test(native_input_reports galaxy_native_input_tests)

add_executable(galaxy_wiimote_report_tests runtime/src/wiimote_report_tests.cpp)
target_link_libraries(galaxy_wiimote_report_tests PRIVATE galaxy_native_input)
nebula_macos_test(wiimote_report_decoding galaxy_wiimote_report_tests)

add_executable(galaxy_macos_wiimote_tests runtime/platform/macos/tests/wiimote_bridge_tests.cpp)
target_link_libraries(galaxy_macos_wiimote_tests PRIVATE galaxy_macos_wiimote)
nebula_macos_test(macos_wiimote_bridge galaxy_macos_wiimote_tests)

# Checks a paired Wii Remote without the game: buttons, pointer, motion, Nunchuk, LEDs, rumble.
add_executable(nebula_wiimote_probe runtime/platform/macos/tools/wiimote_probe.cpp)
target_link_libraries(nebula_wiimote_probe PRIVATE galaxy_macos_wiimote)

add_executable(galaxy_native_input_causality_tests runtime/src/native_input_causality_tests.cpp)
target_link_libraries(galaxy_native_input_causality_tests PRIVATE galaxy_native_input_causality)
nebula_macos_test(native_input_causality galaxy_native_input_causality_tests)

add_executable(galaxy_ai_pcm_identity_tests runtime/src/ai_pcm_identity_tests.cpp)
target_link_libraries(galaxy_ai_pcm_identity_tests PRIVATE galaxy_ai_pcm_identity)
nebula_macos_test(ai_pcm_identity galaxy_ai_pcm_identity_tests)

add_executable(galaxy_jaudio_mixer_tests runtime/src/jaudio_host_mixer_tests.cpp)
target_link_libraries(galaxy_jaudio_mixer_tests PRIVATE galaxy_jaudio_mixer)
nebula_macos_test(jaudio_host_mixer galaxy_jaudio_mixer_tests)

add_executable(galaxy_native_thp_video_tests runtime/src/native_thp_video_tests.cpp runtime/src/native_thp_video.cpp)
target_link_libraries(galaxy_native_thp_video_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(native_thp_video galaxy_native_thp_video_tests)

add_executable(galaxy_transform_history_tests runtime/src/gx/transform_history_tests.cpp
    runtime/src/gx/transform_history.cpp)
target_link_libraries(galaxy_transform_history_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(transform_history_matching galaxy_transform_history_tests)

add_executable(galaxy_game_asset_extract_tests
    runtime/src/gx/game_asset_extract_tests.cpp
    runtime/src/gx/game_asset_extract.cpp)
target_link_libraries(galaxy_game_asset_extract_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(cosmetic_game_asset_extract galaxy_game_asset_extract_tests)

foreach(test_name IN ITEMS atomic_file runtime_instance runtime_settings runtime_timeline)
    add_executable(galaxy_${test_name}_tests runtime/src/${test_name}_tests.cpp)
endforeach()
target_link_libraries(galaxy_atomic_file_tests PRIVATE galaxy_atomic_file)
target_link_libraries(galaxy_runtime_instance_tests PRIVATE galaxy_runtime_instance)
target_link_libraries(galaxy_runtime_settings_tests PRIVATE galaxy_runtime_settings)
target_link_libraries(galaxy_runtime_timeline_tests PRIVATE galaxy_runtime_timeline)
nebula_macos_test(atomic_settings_file galaxy_atomic_file_tests)
nebula_macos_test(runtime_instance_lease galaxy_runtime_instance_tests)
nebula_macos_test(runtime_settings_helpers galaxy_runtime_settings_tests)
nebula_macos_test(runtime_timeline_helpers galaxy_runtime_timeline_tests)

add_executable(galaxy_flat_guest_memory_tests
    runtime/src/flat_guest_memory.cpp
    runtime/src/flat_guest_memory_tests.cpp)
target_link_libraries(galaxy_flat_guest_memory_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(flat_guest_memory_aliases galaxy_flat_guest_memory_tests)

add_executable(galaxy_native_dsp_tests runtime/src/native_dsp_tests.cpp)
target_link_libraries(galaxy_native_dsp_tests PRIVATE galaxy_native_dsp galaxy_runtime_timeline)
nebula_macos_test(native_dsp_coprocessor galaxy_native_dsp_tests)

add_executable(galaxy_controls_tests runtime/src/controls_tests.cpp)
target_link_libraries(galaxy_controls_tests PRIVATE galaxy_runtime_settings galaxy_native_input)
nebula_macos_test(controls_bindings_switch_hid galaxy_controls_tests)

add_executable(galaxy_frame_telemetry_tests runtime/src/frame_telemetry_tests.cpp runtime/src/frame_telemetry.cpp)
target_link_libraries(galaxy_frame_telemetry_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(galaxy_frame_telemetry_tests galaxy_frame_telemetry_tests)

add_executable(galaxy_macos_compat_tests runtime/platform/macos/tests/macos_compat_tests.cpp)
target_link_libraries(galaxy_macos_compat_tests PRIVATE galaxy_runtime_abi)
nebula_macos_test(macos_compat galaxy_macos_compat_tests)

# ---------------------------------------------------------------------------------------------
# Direct3D 12 and DXGI on Metal (runtime/platform/macos/src/d3d12)
# ---------------------------------------------------------------------------------------------
# Interface definitions come from Microsoft's DirectX-Headers; HLSL compiles to SPIR-V with
# glslang and becomes MSL through SPIRV-Cross (Homebrew: directx-headers glslang spirv-tools
# spirv-cross).

# The shader compilers are linked statically, so NebulaRuntime needs nothing beyond macOS at run
# time. By default they are built from the pinned Khronos releases (the versions Homebrew ships);
# OFF uses installed packages instead (Homebrew's glslang and SPIRV-Tools are shared libraries).
option(NEBULA_MACOS_STATIC_SHADER_TOOLS "Build glslang, SPIRV-Tools and SPIRV-Cross from pinned sources" ON)
if(NEBULA_MACOS_STATIC_SHADER_TOOLS)
    include(FetchContent)
    set(BUILD_SHARED_LIBS OFF)
    set(SPIRV_HEADERS_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
    set(SPIRV_HEADERS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
    set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
    set(ENABLE_OPT ON CACHE BOOL "" FORCE)
    set(ALLOW_EXTERNAL_SPIRV_TOOLS ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nebula_spirv_headers
        URL https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
        URL_HASH SHA256=5b3a0d099f5c7649e29c0938476c462e71d66ad89a0bde42fa1ba3eb476956c9)
    FetchContent_Declare(nebula_spirv_tools
        URL https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
        URL_HASH SHA256=e6c83a215538fbfd265ccf398d3877985055df1273dbed73912bd05307664688)
    FetchContent_Declare(nebula_glslang
        URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
        URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2)
    FetchContent_Declare(nebula_spirv_cross
        URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
        URL_HASH SHA256=f708982e88b763ef5b0394ed468fc2c3628f68cdab89f2e7a946cd628c04e721)
    FetchContent_MakeAvailable(nebula_spirv_headers)
    set(SPIRV-Headers_SOURCE_DIR "${nebula_spirv_headers_SOURCE_DIR}")
    FetchContent_MakeAvailable(nebula_spirv_tools nebula_glslang nebula_spirv_cross)
    set(NEBULA_GLSLANG_LIBRARIES glslang glslang-default-resource-limits SPIRV-Tools-opt)
    set(NEBULA_SPIRV_CROSS_LIBRARIES spirv-cross-msl spirv-cross-glsl spirv-cross-core)
else()
    find_package(glslang CONFIG REQUIRED)
    find_package(SPIRV-Tools-opt CONFIG REQUIRED)
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
    find_package(spirv_cross_msl CONFIG REQUIRED)
    set(NEBULA_GLSLANG_LIBRARIES glslang::glslang glslang::glslang-default-resource-limits glslang::SPIRV SPIRV-Tools-opt)
    set(NEBULA_SPIRV_CROSS_LIBRARIES spirv-cross-msl spirv-cross-glsl spirv-cross-core)
endif()
find_path(NEBULA_DIRECTX_HEADERS_DIR d3d12.h PATH_SUFFIXES directx REQUIRED)
find_path(NEBULA_DXGUIDS_DIR dxguids.h PATH_SUFFIXES dxguids REQUIRED)

# The compat headers wrap DirectX-Headers with #include_next, so these directories follow them.
target_include_directories(galaxy_runtime_abi INTERFACE "${NEBULA_DIRECTX_HEADERS_DIR}" "${NEBULA_DXGUIDS_DIR}")
target_include_directories(galaxy_macos_platform PUBLIC "${NEBULA_DIRECTX_HEADERS_DIR}" "${NEBULA_DXGUIDS_DIR}")

set(NEBULA_D3D12_DIR "${NEBULA_MACOS_DIR}/d3d12")

add_library(galaxy_hlsl_compiler STATIC
    ${NEBULA_D3D12_DIR}/d3dcompiler.cpp
    ${NEBULA_D3D12_DIR}/msl_translate.cpp)
target_link_libraries(galaxy_hlsl_compiler
    PUBLIC galaxy_macos_platform
    PRIVATE ${NEBULA_GLSLANG_LIBRARIES} ${NEBULA_SPIRV_CROSS_LIBRARIES})

add_library(galaxy_d3d12_metal STATIC
    ${NEBULA_D3D12_DIR}/device.mm
    ${NEBULA_D3D12_DIR}/formats.mm
    ${NEBULA_D3D12_DIR}/shaders.mm
    ${NEBULA_D3D12_DIR}/commands.mm
    ${NEBULA_D3D12_DIR}/dxgi.mm)
target_compile_options(galaxy_d3d12_metal PRIVATE -fobjc-arc)
target_link_libraries(galaxy_d3d12_metal PUBLIC galaxy_hlsl_compiler "-framework Metal" "-framework QuartzCore")

# Build-time HLSL compiler: fxc's role for the presentation post-processing shaders.
add_executable(nebula_hlslc runtime/platform/macos/tools/hlslc.cpp)
target_link_libraries(nebula_hlslc PRIVATE galaxy_hlsl_compiler)

# ---------------------------------------------------------------------------------------------
# GX renderer
# ---------------------------------------------------------------------------------------------

set(galaxy_uber_embed_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/uber_shader_embed.cpp")
set(galaxy_uber_embed_body "")
foreach(galaxy_uber_stage vs ps)
    set(galaxy_uber_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gx/uber_${galaxy_uber_stage}.hlsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${galaxy_uber_hlsl}")
    file(READ "${galaxy_uber_hlsl}" galaxy_uber_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," galaxy_uber_hex "${galaxy_uber_hex}")
    string(APPEND galaxy_uber_embed_body
        "extern const char galaxy_uber_${galaxy_uber_stage}_hlsl[] = {${galaxy_uber_hex}0x00};\n")
endforeach()
file(CONFIGURE OUTPUT "${galaxy_uber_embed_cpp}"
    CONTENT "namespace galaxy::gx {\n${galaxy_uber_embed_body}}\n"
    @ONLY)

set(galaxy_post_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/post_shaders")
file(MAKE_DIRECTORY "${galaxy_post_dir}")
set(galaxy_post_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/gx/post_process.hlsl")
set(galaxy_smaa_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa/SMAA.hlsl")
set(galaxy_ffx_a_h "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1/ffx_a.h")
set(galaxy_ffx_fsr1_h "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fidelityfx-fsr1/ffx_fsr1.h")
foreach(galaxy_post_input
        "${galaxy_post_hlsl}" "${galaxy_smaa_hlsl}" "${galaxy_ffx_a_h}" "${galaxy_ffx_fsr1_h}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${galaxy_post_input}")
endforeach()
file(READ "${galaxy_post_hlsl}" galaxy_post_src)
file(READ "${galaxy_smaa_hlsl}" galaxy_smaa_src)
file(READ "${galaxy_ffx_a_h}" galaxy_ffx_a_src)
file(READ "${galaxy_ffx_fsr1_h}" galaxy_ffx_fsr1_src)
set(galaxy_post_headers "")
# The pass sources are composed as on Windows (see CMakeLists.txt); nebula_hlslc compiles them.
function(galaxy_post_shader name pass preset library stage entry)
    set(text "#define ${pass} 1\n")
    if(NOT preset STREQUAL "-")
        string(APPEND text "#define ${preset} 1\n")
    endif()
    string(APPEND text "#define POST_SECTION 1\n${galaxy_post_src}\n")
    if(library STREQUAL "smaa")
        string(APPEND text "${galaxy_smaa_src}\n")
    elseif(library STREQUAL "fsr")
        string(APPEND text "${galaxy_ffx_a_src}\n#undef POST_SECTION\n#define POST_SECTION 2\n"
            "${galaxy_post_src}\n${galaxy_ffx_fsr1_src}\n")
    endif()
    string(APPEND text "#undef POST_SECTION\n#define POST_SECTION 3\n${galaxy_post_src}\n")
    set(source "${galaxy_post_dir}/${name}.hlsl")
    file(WRITE "${source}.tmp" "${text}")
    configure_file("${source}.tmp" "${source}" COPYONLY)
    set(header "${galaxy_post_dir}/${name}.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND nebula_hlslc ${stage} ${entry} g_${name} "${source}" "${header}"
        DEPENDS "${source}" nebula_hlslc
        VERBATIM)
    set(galaxy_post_headers ${galaxy_post_headers} "${header}" PARENT_SCOPE)
endfunction()
galaxy_post_shader(post_fullscreen_vs PASS_RESAMPLE - none vs_5_0 VSMain)
galaxy_post_shader(post_resample_ps PASS_RESAMPLE - none ps_5_0 PSMain)
galaxy_post_shader(post_fxaa_ps PASS_FXAA - none ps_5_0 PSMain)
galaxy_post_shader(post_fsr_easu_ps PASS_FSR_EASU - fsr ps_5_0 PSMain)
galaxy_post_shader(post_fsr_rcas_ps PASS_FSR_RCAS - fsr ps_5_0 PSMain)
galaxy_post_shader(post_smaa_edges_vs PASS_SMAA_EDGES SMAA_PRESET_HIGH smaa vs_5_0 VSMain)
galaxy_post_shader(post_smaa_blend_vs PASS_SMAA_BLEND SMAA_PRESET_HIGH smaa vs_5_0 VSMain)
galaxy_post_shader(post_smaa_blend_ps PASS_SMAA_BLEND SMAA_PRESET_HIGH smaa ps_5_0 PSMain)
foreach(galaxy_smaa_preset LOW MEDIUM HIGH ULTRA)
    string(TOLOWER "${galaxy_smaa_preset}" galaxy_smaa_suffix)
    galaxy_post_shader(post_smaa_edges_ps_${galaxy_smaa_suffix}
        PASS_SMAA_EDGES SMAA_PRESET_${galaxy_smaa_preset} smaa ps_5_0 PSMain)
    galaxy_post_shader(post_smaa_weights_vs_${galaxy_smaa_suffix}
        PASS_SMAA_WEIGHTS SMAA_PRESET_${galaxy_smaa_preset} smaa vs_5_0 VSMain)
    galaxy_post_shader(post_smaa_weights_ps_${galaxy_smaa_suffix}
        PASS_SMAA_WEIGHTS SMAA_PRESET_${galaxy_smaa_preset} smaa ps_5_0 PSMain)
endforeach()

add_library(galaxy_gx STATIC
    runtime/src/gx/gx_shim.cpp
    runtime/src/gx/gx_headers_compile.cpp
    runtime/src/gx/gx_state.cpp
    runtime/src/gx/frame_completion.cpp
    runtime/src/gx/render_config.cpp
    runtime/src/gx/renderer_d3d12.cpp
    runtime/src/gx/window_owner.cpp
    runtime/src/gx/texture_cache.cpp
    runtime/src/gx/placed_texture_allocator.cpp
    runtime/src/gx/vertex_loader.cpp
    runtime/src/gx/shader_keys.cpp
    runtime/src/gx/efb_copy.cpp
    runtime/src/gx/line_point_raster.cpp
    runtime/src/gx/pipeline_cache.cpp
    runtime/src/gx/shader_gen.cpp
    runtime/src/gx/custom_textures.cpp
    runtime/src/gx/uber_shaders.cpp
    ${galaxy_uber_embed_cpp}
    runtime/src/gx/post_process.cpp
    ${galaxy_post_headers}
    runtime/src/gx/temporal_upscaler.cpp
    runtime/src/gx/transform_history.cpp
    runtime/src/gx/display_modes.cpp
    runtime/src/gx/fifo_parser.cpp
    runtime/src/gx/gx_backend.cpp
    runtime/src/frame_telemetry.cpp
    runtime/src/gx/game_asset_extract.cpp)
target_link_libraries(galaxy_gx PUBLIC
    galaxy_native_host
    galaxy_frame_cadence_diagnostics
    galaxy_runtime_abi
    galaxy_runtime_settings
    galaxy_host_pointer_events
    galaxy_d3d12_metal)
target_compile_definitions(galaxy_gx PRIVATE GALAXY_WITH_STREAMLINE_BRIDGE=0)
target_include_directories(galaxy_gx PRIVATE "${galaxy_post_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/smaa"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xxhash" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb")

# ---------------------------------------------------------------------------------------------
# NebulaRuntime
# ---------------------------------------------------------------------------------------------

# The Windows launcher starts the runtime with installer/runtime-env.json. macOS has no launcher:
# the entry point applies the same table (generated here, so it cannot drift) as defaults.
set(nebula_runtime_env_json "${CMAKE_CURRENT_SOURCE_DIR}/installer/runtime-env.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nebula_runtime_env_json}")
file(READ "${nebula_runtime_env_json}" nebula_runtime_env)
string(JSON nebula_runtime_env_count LENGTH "${nebula_runtime_env}")
math(EXPR nebula_runtime_env_last "${nebula_runtime_env_count} - 1")
set(nebula_runtime_env_body "")
foreach(nebula_runtime_env_index RANGE ${nebula_runtime_env_last})
    string(JSON nebula_runtime_env_name GET "${nebula_runtime_env}" ${nebula_runtime_env_index} name)
    string(JSON nebula_runtime_env_value GET "${nebula_runtime_env}" ${nebula_runtime_env_index} value)
    string(APPEND nebula_runtime_env_body "    {\"${nebula_runtime_env_name}\", \"${nebula_runtime_env_value}\"},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/runtime_env_defaults.inc"
    CONTENT "// Generated from installer/runtime-env.json.\n${nebula_runtime_env_body}"
    @ONLY)

add_executable(NebulaRuntime
    runtime/src/native_runtime.cpp
    runtime/src/native_thp_video.cpp
    runtime/src/coop_scheduler.cpp
    runtime/src/site_hooks.cpp
    ${NEBULA_MACOS_DIR}/macos_main.mm)
set(NEBULA_SOURCE_REVISION "" CACHE STRING "Revision of exported source")
set(NEBULA_SOURCE_REVISION_KIND "unknown" CACHE STRING "Exported revision object type (commit or tree)")
set(NEBULA_SOURCE_GIT_TREE "unknown" CACHE STRING "Git tree of exported source")
set(NEBULA_SOURCE_TREE_SHA256 "" CACHE STRING "Exported source manifest tree SHA256")
set(NEBULA_SOURCE_DIRTY "unknown" CACHE STRING "Exported source dirty state")
add_custom_target(nebula_runtime_identity
    COMMAND "${CMAKE_COMMAND}"
        "-DCONFIG_FILE=${CMAKE_CURRENT_BINARY_DIR}/runtime_identity_config.cmake"
        "-DBUILD_CONFIG=$<CONFIG>"
        "-DOUTPUT_HEADER=${CMAKE_CURRENT_BINARY_DIR}/nebula_build_identity.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/runtime_build_identity.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/nebula_build_identity.h"
    VERBATIM)
add_dependencies(NebulaRuntime nebula_runtime_identity)
target_include_directories(NebulaRuntime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(NebulaRuntime PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(NebulaRuntime PRIVATE
    galaxy_native_host
    galaxy_native_input_causality
    galaxy_gx
    galaxy_ppc_float
    galaxy_checkpoint_gate
    galaxy_decrementer_deadline
    galaxy_runtime_timeline
    galaxy_jaudio_mixer
    galaxy_runtime_instance)
# Generated DSP entry points are reached through the native host only.
set_property(TARGET NebulaRuntime PROPERTY LINK_LIBRARY_OVERRIDE_galaxy_native_dsp WHOLE_ARCHIVE)
set(galaxy_native_isa_effective "arm64")
set(GALAXY_NATIVE_ISA "arm64")
set(NEBULA_DSP_MODULE ON)
set(nebula_host_ipo_effective OFF)
get_target_property(NEBULA_ID_TARGET_OPTIONS NebulaRuntime COMPILE_OPTIONS)
if(NOT NEBULA_ID_TARGET_OPTIONS)
    set(NEBULA_ID_TARGET_OPTIONS "")
endif()
get_directory_property(NEBULA_ID_DIRECTORY_OPTIONS COMPILE_OPTIONS)
configure_file(cmake/runtime_identity_config.cmake.in runtime_identity_config.cmake @ONLY)

# GPU tests of the GX renderer, run on the Metal translation layer.
add_executable(galaxy_line_point_raster_tests runtime/src/gx/line_point_raster_tests.cpp)
target_link_libraries(galaxy_line_point_raster_tests PRIVATE galaxy_gx)
nebula_macos_test(line_point_raster galaxy_line_point_raster_tests)
