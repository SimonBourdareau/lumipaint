# LumiPaint - per-note colour control for ROLI LUMI Keys
# Copyright (C) 2026 Simon Bourdareau
#
# This program is free software: you can redistribute it and/or modify it under the
# terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
# PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this
# program. If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(LumiPaint LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Everything Apple-specific lives in cmake/macos.cmake, which returns immediately on
# any other platform. It was in six places through this file before.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos.cmake")

if(APPLE)
    # At directory scope, not inside the function below, and not optional. Every .mm in
    # the project and in clap-wrapper needs it, and a function cannot give it to them:
    # the language is detected either way, but the compile rules stay where they were
    # defined and generate fails on every target that has a .mm in it.
    enable_language(OBJCXX)
    lumipaint_macos_toolchain()
endif()

# Where build_gui.sh clones them: beside the project, not under extern/.
#
# These defaulted to extern/ and nothing ever put anything there. The CLAP build never
# noticed because it uses its own paths; only the VST3 path goes through CMake, and it
# failed with "cannot find source file" after ninety seconds of fetching the VST3 SDK.
# One list of directories now, matching what the script actually creates.
set(CLAP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/clap-src" CACHE PATH "clap headers")
set(RTMIDI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rtmidi" CACHE PATH "RtMidi source")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/imgui" CACHE PATH "Dear ImGui source")

foreach(dep CLAP_DIR RTMIDI_DIR IMGUI_DIR)
    if(NOT EXISTS "${${dep}}")
        message(FATAL_ERROR
            "${dep} is ${${dep}}, which does not exist.\n"
            "Run build_gui.sh, which clones the dependencies, or pass -D${dep}=/path/to/it")
    endif()
endforeach()

if(APPLE)
    lumipaint_macos_patch_imgui("${IMGUI_DIR}" LUMIPAINT_OSX_BACKEND)
endif()
set(LUMIPAINT_HOST_SOURCES "" CACHE STRING "Sources implementing imgui_host.h")

# Each platform has exactly one sensible answer, so pick it rather than making every
# build spell it out. Still overridable for a port in progress.
if(NOT LUMIPAINT_HOST_SOURCES)
    if(WIN32)
        set(LUMIPAINT_HOST_SOURCES src/platform/win32/imgui_host_win32.cpp)
    elseif(APPLE)
        set(LUMIPAINT_HOST_SOURCES src/platform/macos/imgui_host_macos.mm)
    elseif(UNIX)
        set(LUMIPAINT_HOST_SOURCES src/platform/x11/imgui_host_x11.cpp)
    endif()
endif()

if(NOT LUMIPAINT_HOST_SOURCES)
    message(WARNING "LUMIPAINT_HOST_SOURCES is empty; the plugin will build but the editor cannot open a window")
endif()

add_library(LumiPaint MODULE
    src/lumipaint.cpp
    src/gui.cpp
    src/keybed_capture.cpp
    src/device_claim.cpp
    src/preset.cpp
    $<$<PLATFORM_ID:Darwin>:src/platform/macos/preset_macos.mm>
    $<$<PLATFORM_ID:Darwin>:src/platform/macos/keybed_capture_macos.mm>
    ${RTMIDI_DIR}/RtMidi.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp

    # The backends, which build_gui.sh compiles and this did not. Without them the
    # link fails on every ImGui_Impl symbol the host layer calls.
    $<$<PLATFORM_ID:Windows>:${IMGUI_DIR}/backends/imgui_impl_win32.cpp>
    $<$<PLATFORM_ID:Windows>:${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp>
    $<$<PLATFORM_ID:Darwin>:${LUMIPAINT_OSX_BACKEND}>
    $<$<PLATFORM_ID:Darwin>:${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp>
    $<$<PLATFORM_ID:Linux>:${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp>

    ${LUMIPAINT_HOST_SOURCES}
)

target_include_directories(LumiPaint PRIVATE
    src
    ${CLAP_DIR}/include
    ${RTMIDI_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

find_package(Threads REQUIRED)
target_link_libraries(LumiPaint PRIVATE Threads::Threads)

if(WIN32)
    target_compile_definitions(LumiPaint PRIVATE __WINDOWS_MM__)

    # The same set build_gui.sh links: OpenGL and GDI for the editor, comdlg32 and
    # shell32 for the preset dialogs, winmm for RtMidi. Only winmm was here, so the
    # link failed on everything the window and the file dialogs need.
    target_link_libraries(LumiPaint PRIVATE
        opengl32 gdi32 imm32 dwmapi winmm comdlg32 ole32 shell32)

    # Static runtime, as build_gui.sh does for the CLAP.
    #
    # This is what stops a host loading the plugin at all. MSYS2's gcc links against
    # libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll, which live in
    # /mingw64/bin and are not on any DAW's search path. The bundle is perfectly formed,
    # the DLL is the right size and in the right folder, and the host silently ignores
    # it because a dependency cannot be resolved. Nothing reports why.
    #
    # Only for GCC and Clang: MSVC has its own runtime handling and clap-wrapper
    # already sets CMAKE_MSVC_RUNTIME_LIBRARY.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(LumiPaint PRIVATE
            -static-libgcc -static-libstdc++
            -Wl,-Bstatic,--whole-archive -lwinpthread
            -Wl,--no-whole-archive)
    endif()
# Linux only, and said so rather than assumed.
#
# This was a plain else() while the chain above still began with if(APPLE). Lifting the
# Apple branch into cmake/macos.cmake left this as the fallback for everything that is
# not Windows - so a Mac came in here and the build stopped looking for ALSA, which is
# the Linux MIDI backend and has never existed on macOS. RtMidi uses CoreMIDI there,
# which cmake/macos.cmake already links.
elseif(UNIX AND NOT APPLE)
    find_package(ALSA REQUIRED)
    target_compile_definitions(LumiPaint PRIVATE __LINUX_ALSA__)
    target_link_libraries(LumiPaint PRIVATE ALSA::ALSA)

    # X11 and GLX for the editor and the window capture, ALSA for MIDI. Nothing else:
    # no GTK, no Qt. Linking a toolkit into an audio plugin invites symbol clashes with
    # whatever the host has already loaded, and the only thing one would buy here is a
    # file dialog, which zenity or kdialog provide from outside the process.
    find_package(X11 REQUIRED)

    # Without this, a machine carrying both the GLVND and the legacy libGL emits a
    # CMP0072 policy warning in the middle of the configure step, which looks like a
    # problem and is not one.
    set(OpenGL_GL_PREFERENCE GLVND)
    find_package(OpenGL REQUIRED)
    target_link_libraries(LumiPaint PRIVATE ${X11_LIBRARIES} OpenGL::GL OpenGL::GLX)
    target_include_directories(LumiPaint PRIVATE ${X11_INCLUDE_DIR})
endif()

if(APPLE)
    lumipaint_macos_configure_target(LumiPaint)
else()
    set_target_properties(LumiPaint PROPERTIES
        PREFIX ""
        SUFFIX ".clap"
        OUTPUT_NAME "LumiPaint"
    )
endif()

# ==============================================================================
#  VST3, and AU on a Mac, without a second implementation
#
#  free-audio/clap-wrapper takes a finished CLAP and produces a VST3 (and an AU on
#  macOS) from it. No plugin code changes: the same lumipaint.cpp serves all of them,
#  so a fix cannot land in one format and miss another. Writing a VST3 by hand would
#  mean maintaining two plugins that have to behave identically, which is a promise
#  nobody keeps.
#
#      cmake -B build -DLUMIPAINT_BUILD_VST3=ON
#      cmake --build build
#
#  The wrapper is fetched at configure time, so the first run needs a network.
# ==============================================================================

option (LUMIPAINT_BUILD_VST3 "Also build a VST3 through clap-wrapper" OFF)
option (LUMIPAINT_BUILD_AU "Also build an Audio Unit on macOS" ${LUMIPAINT_BUILD_VST3})

if (LUMIPAINT_BUILD_VST3 OR (APPLE AND LUMIPAINT_BUILD_AU))
    include (FetchContent)
    set(CLAP_SDK_ROOT "${CLAP_DIR}" CACHE PATH "CLAP SDK for the wrapper")
    set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "Download wrapper SDKs")

    FetchContent_Declare (clap-wrapper
        GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
        GIT_TAG 1cca996e96f29ab2be7ae9f8cfe532bbc92e1dd6)
    FetchContent_MakeAvailable (clap-wrapper)

    if(APPLE)
        if(LUMIPAINT_BUILD_VST3)
            lumipaint_macos_add_vst3()
        endif()

        if(LUMIPAINT_BUILD_AU)
            lumipaint_macos_add_au()
        endif()
    elseif(LUMIPAINT_BUILD_VST3)
        target_add_vst3_wrapper (TARGET LumiPaint
                             OUTPUT_NAME "LumiPaint"
                             SUPPORTS_ALL_NOTE_EXPRESSIONS TRUE)
    endif()
endif()
