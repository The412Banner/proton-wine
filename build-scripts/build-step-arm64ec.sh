#!/bin/bash

export ARCH="aarch64"
export WIN_ARCH="arm64ec,aarch64,i386"
export OUTPUT_DIR="$HOME/compiled-files-aarch64"

export deps="$HOME/termuxfs/aarch64/data/data/com.termux/files/usr"
export RUNTIME_PATH="/data/data/com.termux/files/usr"
export install_dir=$deps/../opt/wine

#export TOOLCHAIN="$HOME/Android/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
export TOOLCHAIN="$HOME/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin"
export LLVM_MINGW_TOOLCHAIN="$HOME/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin"
export TARGET=aarch64-linux-android28
export PATH=$LLVM_MINGW_TOOLCHAIN:$PATH

export CC=$TOOLCHAIN/$TARGET-clang
export AS=$CC
export CXX=$TOOLCHAIN/$TARGET-clang++
export AR=$TOOLCHAIN/llvm-ar
export LD=$TOOLCHAIN/ld
export RANLIB=$TOOLCHAIN/llvm-ranlib
export STRIP=$TOOLCHAIN/llvm-strip
export DLLTOOL=$LLVM_MINGW_TOOLCHAIN/llvm-dlltool

export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:$deps/share/pkgconfig
export ACLOCAL_PATH=$deps/lib/aclocal:$deps/share/aclocal
export CPPFLAGS="--sysroot=$TOOLCHAIN/../sysroot -idirafter $deps/include"

export C_OPTS="-Wno-declaration-after-statement -Wno-implicit-function-declaration -Wno-int-conversion"
export CFLAGS=$C_OPTS
export CXXFLAGS=$C_OPTS
export LDFLAGS="-L$deps/lib -Wl,-rpath=$RUNTIME_PATH/lib"

export FREETYPE_CFLAGS="-I$deps/include/freetype2"
export FONTCONFIG_LIBS="-L$deps/lib -lfontconfig -lfreetype -lexpat"
export PULSE_CFLAGS="-I$deps/include/pulse"
export PULSE_LIBS="-L$deps/lib/pulseaudio -lpulse"
export SDL2_CFLAGS="-I$deps/include/SDL2"
export SDL2_LIBS="-L$deps/lib -lSDL2"
export X_CFLAGS="-I$deps/include/X11"
export X_LIBS="-landroid-sysvshm"
export GSTREAMER_CFLAGS="-I$deps/include/gstreamer-1.0 -I$deps/include/glib-2.0 -I$deps/lib/glib-2.0/include -I$deps/glib-2.0/include -I$deps/lib/gstreamer-1.0/include"
export GSTREAMER_LIBS="-L$deps/lib -lgstgl-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0"
export FFMPEG_CFLAGS="-I$deps/include/libavutil -I$deps/include/libavcodec -I$deps/include/libavformat"
export FFMPEG_LIBS="-L$deps/lib -lavutil -lavcodec -lavformat"

for arg in "$@"
do
  if [ "$arg" == "--enable-16kb-pages" ];
  then
    echo "Enabling 16KB page size support..."
    export TARGET=aarch64-linux-android35
    export C_OPTS="$C_OPTS -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES"
    export CFLAGS="$C_OPTS"
    export CXXFLAGS="$C_OPTS"
    export LDFLAGS="$LDFLAGS -Wl,-z,max-page-size=16384"
    echo "16KB page size support enabled"
  fi

  if [ "$arg" == "--build-sysvshm" ];
  then
    # Build android_sysvshm library
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

    if [ -d "$PROJECT_ROOT/android/android_sysvshm" ]; then
        echo "Building android_sysvshm library..."
        cd "$PROJECT_ROOT/android/android_sysvshm"
        ./build-aarch64.sh
        if [ $? -eq 0 ]; then
            echo "android_sysvshm built successfully"
            # Copy the library to deps/lib for linking
            mkdir -p "$deps/lib"
            cp build-aarch64/libandroid-sysvshm.so "$deps/lib/"
            echo "Copied libandroid-sysvshm.so to $deps/lib/"
        else
            echo "Warning: android_sysvshm build failed"
        fi
        cd "$PROJECT_ROOT"
    fi
  fi

  if [ "$arg" == "--configure" ];
  then
    ./configure \
      --enable-archs=$WIN_ARCH \
      --host=$TARGET \
      --prefix $install_dir \
      --bindir $install_dir/bin \
      --libdir $install_dir/lib \
      --exec-prefix $install_dir \
      --with-mingw=clang \
      --with-wine-tools=./wine-tools \
      --enable-win64 \
      --disable-win16 \
      --enable-nls \
      --disable-amd_ags_x64 \
      --enable-wineandroid_drv=no \
      --disable-tests \
      --without-alsa \
      --without-capi \
      --without-coreaudio \
      --without-cups \
      --without-dbus \
      --without-ffmpeg \
      --with-fontconfig \
      --with-freetype \
      --without-gcrypt \
      --without-gettext \
      --with-gettextpo=no \
      --without-gphoto \
      --without-gnutls \
      --without-gssapi \
      --without-gstreamer \
      --without-inotify \
      --without-krb5 \
      --without-netapi \
      --without-opencl \
      --without-opengl \
      --without-osmesa \
      --without-oss \
      --without-pcap \
      --without-pcsclite \
      --without-piper \
      --with-pthread \
      --without-pulse \
      --without-sane \
      --with-sdl \
      --without-udev \
      --without-unwind \
      --without-usb \
      --without-v4l2 \
      --without-vosk \
      --with-vulkan \
      --without-wayland \
      --without-xcomposite \
      --without-xcursor \
      --without-xfixes \
      --without-xinerama \
      --with-xrandr \
      --with-xrender \
      --without-xshape \
      --without-xshm \
      --without-xxf86vm

    echo "Applying patches..."

    PATCHES=(
      # android network patch
      "dlls_dnsapi_libresolv_c.patch"
      "dlls_dnsapi_record_c.patch"
      "dlls_nsiproxy_sys_ip_c.patch"
      "dlls_nsiproxy_sys_ndis_c.patch"
      "dlls_nsiproxy_sys_nsi_common_h.patch"
      "dlls_ws2_32_socket_c.patch"
      "server_token_c.patch"
      "server_unicode_c.patch"

      # midi support
      "midi_support.patch"

      # sdl patch
      "dlls_winebus_sys_bus_sdl_c.patch"

      # shm_utils
      "dlls_ntdll_unix_esync_c.patch"
      "dlls_ntdll_unix_fsync_c.patch"
      "server_esync_c.patch"
      "server_fsync_c.patch"

      # winex11
      "dlls_winex11_drv_x11drv_h.patch"
      "dlls_winex11_drv_bitblt_c.patch"
      "dlls_winex11_drv_desktop_c.patch"
      "dlls_winex11_drv_mouse_c.patch"
      "dlls_winex11_drv_window_c.patch"
      "dlls_winex11_drv_keyboard_c.patch"
      "dlls_winex11_drv_x11drv_main_c.patch"

      # address space patches
      "arm64ec/dlls_ntdll_unix_virtual_c.patch"
      "loader_preloader_c.patch"

      # syscall Patches
      "dlls_ntdll_unix_signal_x86_64_c.patch"

      # pulse Patches
      "dlls_winepulse_drv_pulse_c.patch"

      # desktop patches
      "programs_explorer_desktop_c.patch"

      # path patches
      "dlls_ntdll_unix_server_c.patch"

      # winlator patches
      "dlls_amd_ags_x64_unixlib_c.patch"
      "dlls_winex11_drv_opengl_c.patch"

      # shortcut patch
      "programs_winemenubuilder_winemenubuilder_c.patch"

      # advapi32 patches
      "dlls_advapi32_advapi_c.patch"

      # browser patches
      "programs_winebrowser_makefile_in.patch"
      "programs_winebrowser_main_c.patch"

      # clipboard patches
      "dlls_user32_clipboard_c.patch"
      "dlls_win32u_clipboard_c.patch"

      # user32 patches
      "dlls_user32_makefile_in.patch"

      # fexcore patch
      "dlls_ntdll_loader_c.patch"
      "dlls_ntdll_unix_loader_c.patch"
      "dlls_wow64_syscall_c.patch"
      "loader_wine_inf_in.patch"

      # fix build
      "programs_wineboot_wineboot_c.patch"
      "dlls_wdscore_wdscore_spec.patch"

      # 1. Extended State (XSTATE/YMM) Support Patches
      "test-bylaws/dlls_ntdll_unwind_h.patch"
      "test-bylaws/include_winnt_h.patch"

      # 2. Thread Suspension Patches
      "test-bylaws/dlls_ntdll_signal_arm64_c.patch"
      "test-bylaws/dlls_ntdll_signal_arm64ec_c.patch"
      "test-bylaws/dlls_ntdll_signal_x86_64_c.patch"
      "test-bylaws/dlls_ntdll_ntdll_spec.patch"
      "test-bylaws/dlls_ntdll_ntdll_misc_h.patch"
      "test-bylaws/dlls_wow64_process_c.patch"
      "test-bylaws/dlls_wow64_wow64_spec.patch"

      # 3. Process and Virtual Memory Management
      "test-bylaws/dlls_wow64_virtual_c.patch"
      "test-bylaws/server_process_c.patch"
      "test-bylaws/dlls_ntdll_unix_process_c.patch"

      # 4. Server and Threading Infrastructure
      "test-bylaws/server_thread_h.patch"
      "test-bylaws/server_thread_c.patch"
      "test-bylaws/dlls_ntdll_unix_thread_c.patch"

      # 5. Internal Headers
      "test-bylaws/include_winternl_h.patch"

      # 6. Build System (Optional)
#      "test-bylaws/tools_makedep_c.patch"
    )

    for patch in "${PATCHES[@]}"; do
#      if git apply --check ./android/patches/$patch 2>/dev/null; then
        git apply ./android/patches/$patch
#      fi
    done

    # ------------------------------------------------------------------
    # Arihany-parity source-presence guards.
    #
    # NOTE: the PATCHES array above targets ./android/patches/, which does
    # NOT exist on this branch, so every git apply above is a fail-soft
    # no-op. The Wine-9 arm64ec tree is instead PRE-PATCHED at the source
    # level. The five arihany-parity changes are therefore baked directly
    # into the source (not applied as patches). These guards hard-fail the
    # build if any of those edits is missing, so a bad rebase/merge can
    # never silently ship an unpatched .wcp.
    # ------------------------------------------------------------------
    echo "Verifying arihany-parity source edits are present..."
    guard_fail=0
    check_marker() {
        # $1 = human label, $2 = file, $3 = grep -E pattern
        if grep -qE "$3" "$2"; then
            echo "  OK: $1"
        else
            echo "GUARD FAILED: $1 -- marker missing in $2" >&2
            guard_fail=1
        fi
    }
    # 1. Arihany #1 force_anon / noexec (dlls/ntdll/unix/virtual.c)
    check_marker "force_anon (virtual.c)" dlls/ntdll/unix/virtual.c 'BOOL force_anon'
    # 2. Arihany #2 shlfileop bare-drive-root guard (dlls/shell32/shlfileop.c)
    check_marker "drive-root guard (shlfileop.c)" dlls/shell32/shlfileop.c 'bare drive root'
    # 3. Arihany #3 LC_ALL=C.UTF-8 default (dlls/ntdll/unix/env.c)
    check_marker "C.UTF-8 locale default (env.c)" dlls/ntdll/unix/env.c 'setenv\( "LC_ALL", "C.UTF-8", 0 \)'
    # 5. fast-yield gate (dlls/ntdll/unix/sync.c)
    check_marker "WINE_FAST_YIELD gate (sync.c)" dlls/ntdll/unix/sync.c 'WINE_FAST_YIELD'
    # 6. Controller input: xinput must be the stock HID (winexinput.sys) reader,
    # NOT the legacy winlator UDP-socket variant (SERVER_PORT 7949 / GET_GAMEPAD),
    # whose Java-side endpoint Bannerlator does not serve -> zero controller input.
    check_marker "stock HID xinput (main.c opens device)" dlls/xinput1_3/main.c 'CreateFileW'
    if grep -qE 'SERVER_PORT|REQUEST_CODE_GET_GAMEPAD|recvfrom' dlls/xinput1_3/main.c; then
        echo "GUARD FAILED: xinput1_3/main.c still has the winlator UDP-socket input path" >&2
        guard_fail=1
    else
        echo "  OK: no legacy UDP-socket input path in xinput1_3/main.c"
    fi
    # 4. XRandR/XRender enabled in this build script (self-check)
    check_marker "--with-xrandr flag" build-scripts/build-step-arm64ec.sh '^\s*--with-xrandr \\'
    check_marker "--with-xrender flag" build-scripts/build-step-arm64ec.sh '^\s*--with-xrender \\'
    # 4b. Confirm configure actually DETECTED the XRandR/XRender sonames, i.e.
    # winex11 will compile the real xrandr backend (not the "XRandR support not
    # compiled in" stub). --with-xrandr alone is a silent no-op if the Xrender
    # soname is not found, so this catches a missing/renamed sysroot lib.
    if [ -f include/config.h ]; then
        check_marker "SONAME_LIBXRANDR detected (config.h)" include/config.h '^#define[[:space:]]+SONAME_LIBXRANDR[[:space:]]'
        check_marker "SONAME_LIBXRENDER detected (config.h)" include/config.h '^#define[[:space:]]+SONAME_LIBXRENDER[[:space:]]'
        # Controller input: winebus needs the SDL2 enumeration backend (Android has
        # no udev). --with-sdl is a silent no-op if configure can't find the SDL2
        # soname, which would ship a winebus.so with no backend and zero gamepads.
        check_marker "SONAME_LIBSDL2 detected (config.h)" include/config.h '^#define[[:space:]]+SONAME_LIBSDL2[[:space:]]'
    else
        echo "GUARD FAILED: include/config.h not found after configure" >&2
        guard_fail=1
    fi
    if [ "$guard_fail" -ne 0 ]; then
        echo "ERROR: one or more arihany-parity source edits are missing; refusing to build." >&2
        exit 1
    fi
    echo "All arihany-parity source edits verified."
  fi

  if [ "$arg" == "--build" ]
  then
    echo "Building..."
    rm -rf $OUTPUT_DIR/bin
    rm -rf $OUTPUT_DIR/lib
    rm -rf $OUTPUT_DIR/share
    rm -rf $install_dir
    if ! make -j$(nproc); then
      echo "ERROR: 'make' failed for Proton 9 arm64ec build; aborting." >&2
      exit 1
    fi
  fi

  if [ "$arg" == "--install" ]
  then
    echo "Installing..."
    mkdir -p $OUTPUT_DIR/bin
    mkdir -p $OUTPUT_DIR/lib
    mkdir -p $OUTPUT_DIR/share
    mkdir -p $install_dir
    if ! make install -j$(nproc); then
      echo "ERROR: 'make install' failed for Proton 9 arm64ec build; aborting." >&2
      exit 1
    fi
    cp -r $install_dir/bin/wine* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/reg* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/msi* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/notepad $OUTPUT_DIR/bin
    cp -r $install_dir/lib/wine  $OUTPUT_DIR/lib
    cp -r $install_dir/share/wine  $OUTPUT_DIR/share
  fi
done
