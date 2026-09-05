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
    src/worker.cpp
    src/gpu1_context.cpp
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
        file(DOWNLOAD
             "https://raw.githubusercontent.com/ocornut/imgui/${MGPU_IMGUI_SHA}/${_h}"
             "${MGPU_IMGUI_DIR}/${_h}"
             STATUS _mgpu_dl TLS_VERIFY ON)
        list(GET _mgpu_dl 0 _mgpu_code)
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
)

target_compile_definitions(mgpu_bridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

# dxgi: factory + EnumAdapters1 (T2). d3d12: the private device (T3).
target_link_libraries(mgpu_bridge PRIVATE
    dxgi
    d3d12
)

# P0: warnings visible, not fatal. A compile error costs the same round
# trip as a logic error and teaches less - keep the first-try bar low.
target_compile_options(mgpu_bridge PRIVATE /W3)
