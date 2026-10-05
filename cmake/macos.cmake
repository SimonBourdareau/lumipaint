# ==============================================================================
#  Everything the build does differently on a Mac.
#
#  Gathered here because it used to be in six places spread through a 314-line
#  CMakeLists: toolchain defaults near the top, an ImGui patch step, ARC flags after
#  the target, frameworks after that, bundle properties further down, and two nearly
#  identical wrapper blocks at the end. Nothing platform-neutral read cleanly between
#  them.
#
#  Included once from the main file, after the LumiPaint target exists.
# ==============================================================================

if(NOT APPLE)
    return()
endif()

# --- Toolchain ----------------------------------------------------------------
#
#  Called before the target is defined, from the top of the main file.
#
#  enable_language(OBJCXX) is NOT here, deliberately.
#
#  It has to run at directory scope. Called from inside a function it looks like it
#  worked - CMake reports "The OBJCXX compiler identification is AppleClang" and
#  configure finishes - but the compile rules it defines stay in the function's scope,
#  so generate then fails with "required internal CMake variable not set:
#  CMAKE_OBJCXX_COMPILE_OBJECT", once per target that has a .mm in it. The message
#  blames CMake's own installation, which is the last place worth looking.
#
#  The main CMakeLists calls it directly instead, next to the include of this file.
#
function(lumipaint_macos_toolchain)
    set(CMAKE_OBJCXX_STANDARD 17 PARENT_SCOPE)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON PARENT_SCOPE)

    # NSOpenGLView and NSModalResponse need a floor somewhere, and the default is
    # whatever SDK happens to be installed. Universal so one build serves Intel and
    # Apple silicon; override either on the command line.
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
    endif()

    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
endfunction()

# --- ImGui's Cocoa backend ----------------------------------------------------
#
#  Patched into a build-local copy, leaving the downloaded SDK untouched.
#
#  Worth knowing why this exists, because it is the most fragile thing in the build:
#  imgui_impl_osx installs a process-wide NSEvent monitor and application-wide
#  notification observers, both of which assume one ImGui context per process. A plugin
#  is N instances and N contexts inside a host process it does not own, so every hunk of
#  the patch is the same fix in a different place - carry a context, scope it, bail if
#  it is null.
#
#  The honest long-term answer is to stop using this backend and feed input from
#  LumiPaintView directly, the way the X11 host already does. That deletes the patch,
#  the version pin, the patch(1) requirement and the GameController framework, which is
#  linked only because this backend wants gamepad support.
function(lumipaint_macos_patch_imgui imgui_dir out_var)
    set(backend "${CMAKE_CURRENT_BINARY_DIR}/imgui-backend/imgui_impl_osx.mm")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/imgui-backend")
    configure_file("${imgui_dir}/backends/imgui_impl_osx.mm" "${backend}" COPYONLY)

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui-osx-context.patch")

    execute_process(COMMAND /usr/bin/patch --silent --fuzz=0
        "${backend}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui-osx-context.patch"
        RESULT_VARIABLE patch_result)

    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Unable to apply the Cocoa context fix. Use ImGui v1.91.5.")
    endif()

    set(${out_var} "${backend}" PARENT_SCOPE)
endfunction()

# --- The target ---------------------------------------------------------------

function(lumipaint_macos_configure_target target)
    # ARC, which the sources already assume. Every __bridge cast means nothing without
    # it, and the window host holds NSWindow, NSOpenGLView and NSTimer as strong
    # references it never releases by hand - so without ARC those casts are meaningless
    # and every one of those references leaks.
    #
    # imgui_impl_osx.mm is left alone deliberately: it guards on
    # __has_feature(objc_arc) and works either way, and ARC is per translation unit.
    set_source_files_properties(
        src/platform/macos/imgui_host_macos.mm
        src/platform/macos/preset_macos.mm
        src/platform/macos/keybed_capture_macos.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

    target_compile_definitions(${target} PRIVATE __MACOSX_CORE__)

    # CoreMIDI and friends for RtMidi; Cocoa for the window and the dialogs;
    # CoreGraphics for reading another application's window; OpenGL for the editor;
    # GameController only because imgui_impl_osx wants it.
    target_link_libraries(${target} PRIVATE
        "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation"
        "-framework Cocoa" "-framework CoreGraphics" "-framework OpenGL"
        "-framework GameController")

    # A CLAP is one file on Windows and Linux and a bundle here. PREFIX/SUFFIX alone
    # produced a bare Mach-O called LumiPaint.clap, which no host will load: the format
    # wants LumiPaint.clap/Contents/MacOS/LumiPaint beside an Info.plist saying BNDL.
    set_target_properties(${target} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        OUTPUT_NAME "LumiPaint"
        MACOSX_BUNDLE_BUNDLE_NAME "LumiPaint"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.lumipaint.clap"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.2"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.2"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2026 Simon Bourdareau"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/clap-bundle.plist.in")

    lumipaint_macos_codesign(${target})
endfunction()

# Ad-hoc signing, so the bundle has a signature the installer can verify. Was written
# out four times.
function(lumipaint_macos_codesign target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:${target}>"
        VERBATIM)
endfunction()

# --- Wrappers -----------------------------------------------------------------
#
#  Each wrapper is its own bundle and loads the CLAP embedded inside it. Applying both
#  wrappers to LumiPaint itself overwrites its bundle format, which is why these are
#  separate targets here and not on Windows or Linux.
#
#  The VST3 and AU blocks were the same eight steps written twice. The difference
#  between them is the call in the middle, so that is the only part left out here.
function(lumipaint_macos_add_wrapper name identifier)
    add_library(${name} MODULE)
    set_target_properties(${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(${name} LumiPaint)

    # Repackage when the embedded plugin changes, even if the wrapper's own sources did
    # not. A target dependency alone only orders builds.
    set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:LumiPaint>")
endfunction()

function(lumipaint_macos_add_vst3)
    lumipaint_macos_add_wrapper(LumiPaint_VST3 "com.lumipaint.vst3")
    target_add_vst3_wrapper(TARGET LumiPaint_VST3
        OUTPUT_NAME "LumiPaint"
        BUNDLE_IDENTIFIER "com.lumipaint.vst3"
        BUNDLE_VERSION "1.0.2"
        SUPPORTS_ALL_NOTE_EXPRESSIONS TRUE
        MACOS_EMBEDDED_CLAP_LOCATION "$<TARGET_BUNDLE_DIR:LumiPaint>")
    lumipaint_macos_codesign(LumiPaint_VST3)
endfunction()

function(lumipaint_macos_add_au)
    lumipaint_macos_add_wrapper(LumiPaint_AU "com.lumipaint.au")
    target_add_auv2_wrapper(TARGET LumiPaint_AU
        OUTPUT_NAME "LumiPaint"
        BUNDLE_IDENTIFIER "com.lumipaint.au"
        BUNDLE_VERSION "1.0.2"
        MANUFACTURER_NAME "LumiPaint"
        MANUFACTURER_CODE "Lmpt"
        SUBTYPE_CODE "Lmp1"
        INSTRUMENT_TYPE "aumf"
        PREFER_CMAKE_AUV2_CONFIGURATION TRUE
        MACOS_EMBEDDED_CLAP_LOCATION "$<TARGET_BUNDLE_DIR:LumiPaint>")
    lumipaint_macos_codesign(LumiPaint_AU)
endfunction()
