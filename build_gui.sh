#!/bin/sh
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

# Build LumiPaint with the Dear ImGui GUI (Windows).
#
#   sh build_gui.sh
#
# Expects, in this directory:
#   src/  clap-src/  imgui/  rtmidi/
#
# Everything is checked before anything is compiled, and each command is
# echoed, so a failure names itself instead of scrolling past.

CROSS=${CROSS:-}
CXX=${CROSS}g++
OPT=${OPT:--O2}
EXTRA=${EXTRA:-}

fail() { echo; echo "FAILED: $1"; exit 1; }

# ---- 1. everything present? ------------------------------------------------
missing=
for f in src/lumipaint.cpp src/lumipaint.h src/gui.cpp src/imgui_host.h \
         src/imgui_host_win32.cpp src/keybed_capture.cpp src/keybed_capture.h \
         src/device_claim.cpp src/device_claim.h src/preset.cpp src/preset.h \
         clap-src/include/clap/clap.h imgui/imgui.cpp \
         imgui/backends/imgui_impl_win32.cpp \
         imgui/backends/imgui_impl_opengl3.cpp \
         rtmidi/RtMidi.cpp rtmidi/RtMidi.h; do
  [ -f "$f" ] || missing="$missing  $f"
done
if [ -n "$missing" ]; then
  echo "missing files:"; echo "$missing"
  echo
  echo "  clap-src:  git clone --depth 1 https://github.com/free-audio/clap.git clap-src"
  echo "  imgui:     git clone --depth 1 -b v1.91.5 https://github.com/ocornut/imgui.git imgui"
  echo "  rtmidi:    git clone --depth 1 -b 6.0.0 https://github.com/thestk/rtmidi.git rtmidi"
  echo
  echo "  RtMidi master moved its classes into rt::midi. The pimpl in lumipaint.h"
  echo "  copes with both, but pinning the release tag keeps this predictable."
  exit 1
fi

# ---- 2. compiler present? --------------------------------------------------
command -v "$CXX" >/dev/null 2>&1 || fail "$CXX not found. pacman -S mingw-w64-x86_64-gcc"

# ---- 3. can the compiler reach its assembler? ------------------------------
# On some machines the x86_64-w64-mingw32/bin directory is blocked from
# executing, and gcc then fails with no message at all. Detect it and point
# gcc at the copy in /mingw64/bin instead.
if [ -z "$EXTRA" ]; then
  echo 'int probe(void){return 0;}' > .probe.cpp
  if ! $CXX -c .probe.cpp -o .probe.o >/dev/null 2>&1; then
    if $CXX -B/mingw64/bin -c .probe.cpp -o .probe.o >/dev/null 2>&1; then
      EXTRA="-B/mingw64/bin"
      echo "note: using $EXTRA (the default assembler path is not executable)"
    else
      rm -f .probe.cpp .probe.o
      fail "$CXX cannot compile even a trivial file. Try: $CXX -v -c yourfile.cpp"
    fi
  fi
  rm -f .probe.cpp .probe.o
fi

mkdir -p obj

# Objects are reused between runs, so record which compiler built them. Without
# this, switching toolchain silently links yesterday's objects against today's
# compiler and the errors point at ImGui rather than at the real cause.
STAMP="obj/.toolchain"
WANT="${CROSS}g++"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$WANT" ]; then
    rm -f obj/*.o
    printf '%s' "$WANT" > "$STAMP"
fi

INC="-Iimgui -Iimgui/backends -Iclap-src/include -Irtmidi -Isrc"

# ---- 4. build --------------------------------------------------------------
echo "[1/5] imgui (slow the first time only)"
for f in imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp \
         imgui/imgui_widgets.cpp imgui/backends/imgui_impl_win32.cpp \
         imgui/backends/imgui_impl_opengl3.cpp; do
  o="obj/$(basename "${f%.cpp}").o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
    echo "  $f"
    $CXX $OPT $EXTRA -Iimgui -Iimgui/backends -c "$f" -o "$o" || fail "$f"
  fi
done

# __WINDOWS_MM__ selects the WinMM backend. Without an API define RtMidi
# refuses to compile at all, with an error about no enabled APIs that reads
# like a missing header.
echo "[2/5] rtmidi"
o="obj/RtMidi.o"
if [ ! -f "$o" ] || [ rtmidi/RtMidi.cpp -nt "$o" ]; then
  $CXX $OPT $EXTRA -D__WINDOWS_MM__ -Irtmidi -c rtmidi/RtMidi.cpp -o "$o" || fail "RtMidi.cpp"
fi

echo "[3/5] gui.cpp + imgui_host_win32.cpp"
$CXX $OPT $EXTRA $INC -c src/gui.cpp -o obj/gui.o || fail "gui.cpp"
$CXX $OPT $EXTRA $INC -c src/imgui_host_win32.cpp -o obj/imgui_host_win32.o \
  || fail "imgui_host_win32.cpp"
$CXX -std=c++17 $OPT $EXTRA $INC -c src/keybed_capture.cpp -o obj/keybed_capture.o \
  || fail "keybed_capture.cpp"
$CXX -std=c++17 $OPT $EXTRA $INC -c src/device_claim.cpp -o obj/device_claim.o \
  || fail "device_claim.cpp"
$CXX -std=c++17 $OPT $EXTRA $INC -c src/preset.cpp -o obj/preset.o \
  || fail "preset.cpp"

echo "[4/5] lumipaint.cpp"
$CXX -std=c++17 $OPT $EXTRA -Wall -D__WINDOWS_MM__ $INC \
    -c src/lumipaint.cpp -o obj/lumipaint.o || fail "lumipaint.cpp"

echo "[5/5] link"
# -static matters more than it looks. MSYS2's gcc is built with posix threads,
# so libstdc++ pulls in libwinpthread-1.dll; that DLL lives inside MSYS2 and is
# NOT on the search path when a host loads a plugin. Windows then refuses the
# plugin with no message at all - it builds perfectly and simply never appears.
# -static folds all of it in.
#
# One list, used by both attempts. Written twice, they drift.
#
# -lwinmm is RtMidi's WinMM backend. Without it the link fails with undefined
# references to midiOutOpen and friends, which name the symbols but not the
# library. Note that WinMM has no virtual ports, so the "LumiPaint In" port
# the plugin opens on macOS and Linux simply does not appear here; the code
# already catches that and carries on.
LIBS="-lopengl32 -lgdi32 -limm32 -ldwmapi -lwinmm -lcomdlg32 -lole32 -lshell32 -lm"

$CXX -shared -static -static-libgcc -static-libstdc++ $EXTRA \
     -o LumiPaint.clap obj/*.o $LIBS 2>/dev/null \
  || $CXX -shared -static-libgcc -static-libstdc++ $EXTRA \
     -o LumiPaint.clap obj/*.o $LIBS || fail "link"

# ---- 5. verify -------------------------------------------------------------
echo
${CROSS}objdump -p LumiPaint.clap 2>/dev/null | grep -q clap_entry \
  || fail "LumiPaint.clap has no clap_entry symbol; the host would not load it"

# Any dependency outside the Windows system set will not be found when a host
# loads the plugin, and the failure is silent. Catch it here instead.
bad=$(${CROSS}objdump -p LumiPaint.clap 2>/dev/null | sed -n 's/^\tDLL Name: //p' \
      | grep -iv -e '^KERNEL32' -e '^msvcrt' -e '^USER32' -e '^GDI32' \
                 -e '^OPENGL32' -e '^SHELL32' -e '^dwmapi' -e '^ADVAPI32' \
                 -e '^ole32' -e '^OLEAUT32' -e '^IMM32' -e '^api-ms-win' \
                 -e '^COMDLG32' -e '^COMCTL32' \
                 -e '^ucrtbase' -e '^VERSION' -e '^SETUPAPI' -e '^CFGMGR32' \
                 -e '^WINMM')

echo "built LumiPaint.clap"
echo "  clap_entry exported: yes"
if [ -n "$bad" ]; then
  echo
  echo "  PROBLEM - depends on DLLs the host will not find:"
  echo "$bad" | sed 's/^/    /'
  echo
  echo "  These live inside MSYS2. Bitwig will fail to load the plugin and say"
  echo "  nothing. Rebuild with a fully static link, or copy those DLLs next to"
  echo "  the .clap file."
  echo
  echo "  If one of them is in fact a Windows system DLL - a new API being used"
  echo "  for the first time - add it to the list above instead."
  exit 1
fi
echo "  dependencies: Windows system DLLs only"
echo
echo "device program: drag device/lumi_paint.littlefoot onto the keyboard in ROLI Dashboard"
echo
echo "install:  cp LumiPaint.clap \"$LOCALAPPDATA/Programs/Common/CLAP/\""

# ==============================================================================
#  VST3, from the same sources
#
#  Built every time, so the two formats cannot drift apart - a fix that lands in
#  the CLAP and not the VST3 is the kind of thing nobody notices for weeks.
#
#  It comes from free-audio/clap-wrapper, which is a CMake project, so this step
#  hands over to CMake rather than driving the wrapper from a shell script. That
#  build compiles the sources a second time; slower than reusing the objects above,
#  but it keeps one description of how to build a VST3 rather than a second one
#  here that would fall out of step with it.
#
#  LUMIPAINT_NO_VST3=1 skips it, for when you are iterating on the CLAP and do not
#  want to wait.
# ==============================================================================

if [ -z "$LUMIPAINT_NO_VST3" ]; then
    if ! command -v cmake >/dev/null 2>&1; then
        echo
        echo "VST3 skipped: cmake is not on the path."
        echo "  install cmake, or set LUMIPAINT_NO_VST3=1 to stop asking"
        exit 0
    fi

    echo
    echo "[vst3] configuring"

    # A path with a space in it breaks the wrapper build long before it says why, so
    # it is worth refusing early with a reason rather than failing obscurely.
    case "$PWD" in
        *\ *)
            echo
            echo "VST3 skipped: this path contains a space."
            echo "  $PWD"
            echo "  the wrapper build cannot handle it - move the project somewhere like"
            echo "  C:/msys64/home/dev/lumipaint and run again"
            exit 0
            ;;
    esac

    # Ninja first, make second.
    #
    # g++ compiles fine by hand while ninja reports "FAILED: [code=1]" with no compiler
    # output at all - which is ninja failing to spawn the process, not the compiler
    # failing to compile. It happens on MSYS2 with a mismatched ninja package or with
    # antivirus blocking spawned processes, and there is no fixing it from here. So
    # ninja is tried, and if configuring fails the whole thing is retried with plain
    # makefiles, which spawn the same compiler by a different route.
    #
    # The compiler is named explicitly either way: the same one this script just used
    # successfully, rather than whatever CMake goes looking for.
    # $EXTRA carries the assembler fix, and CMake needs it as much as the build above.
    #
    # This was the whole problem. On an install where gcc cannot reach its own
    # assembler, the check near the top of this script finds that out and compiles
    # everything with -B/mingw64/bin. CMake was handed the compiler but not the flag,
    # so its very first test - compiling an empty main - failed with no diagnostic at
    # all, because the failure is gcc being unable to run "as" rather than anything
    # about the code. Three generators reported the same thing for the same reason.
    vst3_configure()
    {
        # CLAP_SDK_ROOT points at the copy this script already cloned. The wrapper
        # looks for the SDK in a handful of places, none of which is "next to the
        # project", and without it the configure fails after everything else has gone
        # right. Downloading dependencies is also allowed, so a first run without
        # clap-src present still works.
        cmake -B build-vst3 -G "$1" -DLUMIPAINT_BUILD_VST3=ON \
              -DCMAKE_C_COMPILER="${CROSS}gcc" -DCMAKE_CXX_COMPILER="${CROSS}g++" \
              -DCMAKE_C_FLAGS="$EXTRA" -DCMAKE_CXX_FLAGS="$EXTRA" \
              -DCLAP_SDK_ROOT="$PWD/clap-src" \
              -DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=TRUE \
              -DLUMIPAINT_HOST_SOURCES=src/imgui_host_win32.cpp \
              -DCMAKE_BUILD_TYPE=Release >build-vst3.log 2>&1
    }

    configured=""

    for gen in "Ninja" "MinGW Makefiles" "Unix Makefiles"; do
        rm -rf build-vst3
        echo "[vst3] configuring with $gen"

        if vst3_configure "$gen"; then
            configured="$gen"
            break
        fi
    done

    if [ -z "$configured" ]; then
        echo "VST3 configure failed with every generator - see build-vst3.log"
        echo
        echo "  The CLAP above built with: ${CROSS}g++ $EXTRA"
        echo "  and CMake was given the same compiler and the same flags, so if this"
        echo "  still fails the log holds a real error rather than a setup problem."
        echo
        echo "  LUMIPAINT_NO_VST3=1 builds the CLAP alone."
        exit 1
    fi

    echo "[vst3] building with $configured"

    if ! cmake --build build-vst3 --config Release >>build-vst3.log 2>&1; then
        echo "VST3 build failed - see build-vst3.log"
        echo "  configure succeeded with $configured, so the toolchain works and this"
        echo "  is a real compile error rather than a generator problem"
        echo "  LUMIPAINT_NO_VST3=1 builds the CLAP alone"
        exit 1
    fi

    VST3=$(find build-vst3 -name "*.vst3" -print -quit 2>/dev/null)

    if [ -n "$VST3" ]; then
        echo "built $VST3"
        echo "install: copy it to %COMMONPROGRAMFILES%\\VST3\\"
    else
        echo "VST3 build reported success but produced no bundle - see build-vst3.log"
    fi
fi
