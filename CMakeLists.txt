# MGPU Bridge - Cross-Adapter Bridge, Milestone 0 (Gate P0)
#
# Target: mgpu_bridge.addon64  (a ReShade 64-bit add-on, API 20)
#
# The .addon64 extension REPLACES .dll - the output is NOT
# mgpu_bridge.addon64.dll. ReShade loads *.addon64 files; a file with the
# wrong name goes green in CI and silently never loads on the rig. CI
# verifies the exact output name for this reason.

cmake_minimum_required(VERSION 3.21)

project(mgpu_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source list follows the closed manifest (P0_RECORD.md, section 01):
# T2 adds diag/adapter, T3 adds worker/gpu1_context. runtime_probe lands
# with its own task; assets/ is deployed by hand and never compiled.
add_library(mgpu_bridge SHARED
    src/dllmain.cpp
    src/diag.cpp
    src/adapter.cpp
    src/adapter_selection.cpp
    src/worker.cpp
    src/gpu1_context.cpp
    # P9.1: the lateral acquisition probe. Self-contained - it holds every
    # ReShade event callback the probe needs and nothing else, so
    # gpu1_context.cpp keeps its rule of holding no ReShade types and is
    # not touched by this feature at all.
    src/probe.cpp
    # R101: the calibrator. Reads the parameter block the game already hands
    # DLSS every frame, so acquisition stops being inferred. Self-contained
    # like probe.cpp - it holds the import-table interception and the reader
    # and nothing else, so neither gpu1_context nor probe gains a dependency.
    src/calibrator.cpp
    # R108: the idle screen. Draws with scissored clears only - no shader,
    # no font file, no ImGui - so it renders in exactly the situation where
    # nothing else can: an empty reshade-shaders folder.
    src/screen.cpp
    # SL1: the Streamline interposer probe. Self-contained like the three
    # above - it holds no ReShade types and no gpu1_context types, every
    # interface arrives as a void*, and the whole instrument is one
    # documented QueryInterface. No new link library: version.dll is
    # resolved at run time, the way d3dcompiler_47 already is.
    src/sl_probe.cpp
    # SLT1: the Streamline tag tap. Self-contained like the four above - no
    # ReShade type, no gpu1_context type, no Streamline header and no link
    # library; every Streamline struct is read by byte offset, and every
    # offset is quoted from the public header beside it. OFF unless
    # SLTags= is set in mgpu.ini, so this source compiles into every build
    # and touches nothing in any of them until it is asked for.
    src/sl_tags.cpp
    # R208: the experimental FP16 input path. No gpu1_context type;
    # gpu1_context hands it D3D12 objects. OFF unless NRInput16=4 is in
    # mgpu.ini: no texture, no kernel, no extra pass.
    src/nr16.cpp
    # R316: the residual pass module (private test). No gpu1_context type;
    # gpu1_context hands it D3D12 objects and the NGX parameter block. OFF
    # unless ResidualPass=1|2 is in mgpu.ini: no texture, no kernel, no extra pass.
    src/residual.cpp
    src/gpu0_stage.cpp
    src/chain_colour_space.cpp
    src/fg_gpu1.cpp
    # D1 (0.3.0 R&D): the discovery calibrator. OFF unless Discovery=1 in
    # mgpu.ini. Holds no ReShade type and no gpu1_context type.
    src/discovery.cpp
    # D1.5a (0.3.0 R&D): the read-only latency probe. OFF unless
    # LatencyProbe=1 in mgpu.ini. Holds no ReShade type and no gpu1_context type.
    src/latency_probe.cpp
    # D2.0 (0.3.0 R&D): log queue, journal, stall watch, our Reflex for
    # case-B titles. Each OFF unless its key is set (Journal, StallWatch,
    # OwnReflex). No ReShade type and no gpu1_context type in any of them.
    src/log_queue.cpp
    src/journal.cpp
    src/stall_watch.cpp
    src/startup_trace.cpp         # R313: StartupTrace + the stop/death record; DcompSafeMode keys (off by default)
    src/own_reflex.cpp
    # D2.0 NR step one: read-only frame-gen vector map, OFF unless NRFrameFilter=1.
    src/fg_map.cpp
    # DX11 transport-hop probe (DX11_CONTRACT_LEDGER.md). OFF unless Dx11Probe=1.
    # Holds no ReShade type and no gpu1_context type; nothing on GPU 1.
    src/dx11_probe.cpp
    # DX11 producer (DX11_CONTRACT_LEDGER.md section 7, R227). OFF unless DX11=1
    # and only ever called from a D3D11 game device; owns a D3D12 device on the
    # game adapter and feeds the existing stream producer. No ReShade type.
    src/dx11_producer.cpp
    # R253: motion-vector census for D3D11 titles (DX11_CONTRACT_LEDGER.md section 13).
    # Observe-only, OFF unless MVecCensus=1; registers its own ReShade events.
    src/mvec_census.cpp
    # R279: TexCensus=1 instrument (remove with the file pair when done)
    src/tex_census.cpp
    # R280: the virtual velocity target for D3D12 titles without an NGX contract (MVEC_D3D12_LEDGER.md)
    src/mvec_extract.cpp
    # PanelOverPicture (N01950): the live panel over the neural frame. OFF unless
    # PanelOverPicture=1; registers its own ReShade event only then.
    src/live_panel.cpp
    src/panel_simple.cpp   # the player-facing panel; its layout lives in panel_simple.txt
    # R300 - T1.19 (Dom / LordZaxxon), docs/R300_T119.md. Every path OFF unless its mgpu.ini key is set:
    # t119_keys.cpp is the one place the keys are read (from probe's ini read).
    src/t119_keys.cpp
    src/temporal_diag.cpp         # R300a1-R300a8: temporal record + gated policy (TemporalDiag, TemporalMV, ...)
    src/perf_diag.cpp             # R300e1-R300e6: consumer timing + T1.12 prefetch (Perf11, PerfPrefetch)
    src/feature_diag.cpp          # R300f1: scene-feature silence detector (FeatureDiag)
    src/producer_diag.cpp         # R300g1-R300g6: producer (GPU 0) cost bookkeeping (ProducerDiag)
    src/producer_gpu_timers.cpp   # R300h: GPU 0 timestamps (Gpu0Timers)

    # [PR] contract capture, the XeSS-FG provider adapter, and their vendored deps.
    src/ngx_hook.cpp
    src/ffx_hook.cpp
    src/display_ref.cpp
    src/b1_loader_stub.cpp
    src/xess_fg.cpp
    third_party/safetyhook/safetyhook.cpp
    third_party/safetyhook/Zydis.c
)

target_include_directories(mgpu_bridge PRIVATE
    "ext/reshade/include"
)

# ---------------------------------------------------------------------------
# P7.1. The overlay panel's only dependency: two ImGui headers.
#
# "ext/reshade/deps/imgui" used to be listed above and did nothing, because CMake
# silently ignores an include directory that does not exist and __has_include
# then just says no - which is how a correct-looking CMakeLists edit produced no
# change at all in the log. ext/reshade is created by CI from the pinned SHA and
# is not in the repo, so nothing was ever there to find.
#
# We do not vendor ReShade and we compile no ImGui source. reshade.hpp supplies
# the whole ImGui API as a table of function pointers the ReShade runtime fills
# in at register_addon time; imgui.h is needed only for the declarations,
# IMGUI_VERSION_NUM and the struct layouts. So the dependency is imgui.h +
# imconfig.h, fetched into the BUILD tree - no submodule, no build step, and
# nothing third-party committed to this repo.
#
# The version is not negotiable: reshade_overlay.hpp #errors on anything but
# 19250 (ImGui 1.92.5), because the function table is a version-numbered struct
# of raw pointers and a near-miss is a silent ABI mismatch. 3912b3d9 is the
# commit ReShade 18deaa52de0c425a78b329e9cb3c497281cd00ec pins at deps/imgui.
#
# If the fetch fails the build still succeeds - it logs NO OVERLAY PANEL IN THIS
# BUILD and the CTRL+ALT+F8/F9/F11 hotkeys remain the control. The configure
# output below says which of the two you are getting, BEFORE you build.
set(MGPU_IMGUI_SHA "3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c")
set(MGPU_IMGUI_DIR "${CMAKE_BINARY_DIR}/ext/imgui")
foreach(_h imgui.h imconfig.h)
    if(NOT EXISTS "${MGPU_IMGUI_DIR}/${_h}")
        # R299c: three attempts (N01950 lost the panel to one failed fetch). Still green
        # on failure by design; CI fails the job instead (build.yml, Configure step).
        foreach(_try 1 2 3)
            file(DOWNLOAD
                 "https://raw.githubusercontent.com/ocornut/imgui/${MGPU_IMGUI_SHA}/${_h}"
                 "${MGPU_IMGUI_DIR}/${_h}"
                 STATUS _mgpu_dl TLS_VERIFY ON)
            list(GET _mgpu_dl 0 _mgpu_code)
            if(_mgpu_code EQUAL 0)
                break()
            endif()
            message(STATUS "MGPU: ${_h} fetch attempt ${_try}/3 failed")
            execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 2)
        endforeach()
        if(NOT _mgpu_code EQUAL 0)
            list(GET _mgpu_dl 1 _mgpu_msg)
            message(WARNING "MGPU: could not fetch ${_h} (${_mgpu_msg}) - building without the overlay panel.")
            file(REMOVE "${MGPU_IMGUI_DIR}/${_h}")
        endif()
    endif()
endforeach()

if(EXISTS "${MGPU_IMGUI_DIR}/imgui.h" AND EXISTS "${MGPU_IMGUI_DIR}/imconfig.h")
    target_include_directories(mgpu_bridge PRIVATE "${MGPU_IMGUI_DIR}")
    message(STATUS "MGPU: overlay panel ENABLED - ImGui headers at ${MGPU_IMGUI_DIR}")
else()
    message(STATUS "MGPU: overlay panel DISABLED - no ImGui headers; hotkeys only.")
endif()
# ---------------------------------------------------------------------------

set_target_properties(mgpu_bridge PROPERTIES
    PREFIX ""
    SUFFIX ".addon64"
    # $<1:...> suppresses the per-configuration subdirectory that
    # multi-config generators (including "Visual Studio 17 2022", which
    # CI uses) would otherwise append to RUNTIME_OUTPUT_DIRECTORY: the
    # output is build/mgpu_bridge.addon64, not build/Release/...
    # CI locates the artifact by name regardless; this makes the stated
    # path true.
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"
)

target_include_directories(mgpu_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # CI creates ext/reshade/ from crosire/reshade at the pinned SHA
    # (see .github/workflows/build.yml and VENDOR_LOCK.md).
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/reshade
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/safetyhook
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xess_fg
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/ngx
)

target_compile_definitions(mgpu_bridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
    # 2026-10-06, Rise of the Tomb Raider (and a reporter's Shadow of the Tomb
    # Raider): the game folder ships msvcp140.dll 14.28.29910 and the process
    # loads that copy, not the system's. Visual Studio 2022 17.10+ made
    # std::mutex's constructor constexpr; a binary built that way crashes at
    # its first mutex use (0xC0000005) against an msvcp140 older than 14.40.
    # The add-on takes a std::mutex in its first device event. This is
    # Microsoft's documented switch for exactly that case. The alternative, a
    # static CRT, is the broader answer and a later milestone.
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
    ZYDIS_STATIC_BUILD
)

# dxgi: factory + EnumAdapters1 (T2). d3d12: the private device (T3).
target_link_libraries(mgpu_bridge PRIVATE
    dxgi
    d3d12
    # V49/V51: the ghost mode. DirectComposition, and D3D11 solely to name an
    # adapter to it - nothing is ever drawn with the D3D11 device. Both are
    # also declared with #pragma comment(lib, ...) inside gpu1_context.cpp, so
    # a build made from src/* without this file still links.
    dcomp
    d3d11
)

# P0: warnings visible, not fatal. A compile error costs the same round
# trip as a logic error and teaches less - keep the first-try bar low.
target_compile_options(mgpu_bridge PRIVATE /W3)
# R330 (F7): panel_simple.cpp carries UTF-8 text, so it is read AND emitted as UTF-8 (ImGui expects
# UTF-8). Only that file: every other source compiles exactly as before.
if (MSVC)
    set_source_files_properties(src/panel_simple.cpp PROPERTIES COMPILE_OPTIONS "/utf-8")
    # [PR] the two hook TUs and SafetyHook need C++latest; the rest of the add-on stays C++17.
    set_source_files_properties(
        src/ngx_hook.cpp src/ffx_hook.cpp third_party/safetyhook/safetyhook.cpp
        PROPERTIES COMPILE_OPTIONS "/std:c++latest")
endif()

option(MGPU_BUILD_TESTS "Build CPU-only adapter policy tests" OFF)
if(MGPU_BUILD_TESTS)
    enable_testing()
    add_executable(mgpu_adapter_selection_test
        tests/adapter_selection_test.cpp
        src/adapter_selection.cpp
    )
    target_include_directories(mgpu_adapter_selection_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME mgpu_adapter_selection_test
             COMMAND mgpu_adapter_selection_test)
    # T1.19 (Dom): the instruments' state machines, off-device
    add_executable(mgpu_temporal_diag_test tests/temporal_diag_test.cpp src/temporal_diag.cpp)
    add_test(NAME mgpu_temporal_diag_test COMMAND mgpu_temporal_diag_test)
    add_executable(mgpu_perf_diag_test tests/perf_diag_test.cpp src/perf_diag.cpp)
    add_test(NAME mgpu_perf_diag_test COMMAND mgpu_perf_diag_test)
    add_executable(mgpu_feature_diag_test tests/feature_diag_test.cpp src/feature_diag.cpp)
    add_test(NAME mgpu_feature_diag_test COMMAND mgpu_feature_diag_test)
    add_executable(mgpu_producer_diag_test tests/producer_diag_test.cpp src/producer_diag.cpp)
    add_test(NAME mgpu_producer_diag_test COMMAND mgpu_producer_diag_test)
endif()
