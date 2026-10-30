#!/bin/bash

# Fail hard on any command error. Note: `set -e` does NOT cover commands inside
# `if` bodies below, so the critical steps (configure / git apply / make) also
# carry explicit `|| exit $?` — without this a failing `make` used to be masked
# by the trailing `if [ "$arg" == "--install" ]; then ... fi` returning 0, so
# CI shipped a broken (skeleton) wcp while reporting success.
set -eo pipefail

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

# ccache: cache compiled objects so re-runs with unchanged Wine source skip recompilation. Unix side:
# wrap the full-path NDK clang. PE side (--with-mingw=clang, resolved via PATH): masquerade clang/clang++
# with ccache symlinks placed first on PATH, so Wine's cross-compiler calls go through ccache too.
if command -v ccache >/dev/null 2>&1; then
  export CCACHE_DIR="${CCACHE_DIR:-$HOME/.ccache}"
  ccache -M 3G >/dev/null 2>&1 || true
  mkdir -p "$HOME/ccache-bin"
  ln -sf "$(command -v ccache)" "$HOME/ccache-bin/clang"
  ln -sf "$(command -v ccache)" "$HOME/ccache-bin/clang++"
  export PATH="$HOME/ccache-bin:$PATH"
  export CC="ccache $TOOLCHAIN/$TARGET-clang"
  export CXX="ccache $TOOLCHAIN/$TARGET-clang++"
else
  export CC=$TOOLCHAIN/$TARGET-clang
  export CXX=$TOOLCHAIN/$TARGET-clang++
fi
export AS=$TOOLCHAIN/$TARGET-clang
export AR=$TOOLCHAIN/llvm-ar
export LD=$TOOLCHAIN/ld
export RANLIB=$TOOLCHAIN/llvm-ranlib
export STRIP=$TOOLCHAIN/llvm-strip
export DLLTOOL=$LLVM_MINGW_TOOLCHAIN/llvm-dlltool

export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:$deps/share/pkgconfig
export ACLOCAL_PATH=$deps/lib/aclocal:$deps/share/aclocal
export CPPFLAGS="-I$deps/include --sysroot=$TOOLCHAIN/../sysroot"

# -g0 = don't emit debug info (the bulk of the tree size); -O2 = normal release optimisation.
# Applied to the ELF/unix side via CFLAGS below and to the arm64ec PE side via CROSSCFLAGS.
# (A post-install llvm-strip pass in --install trims the remaining symbol tables.)
export C_OPTS="-g0 -O2 -Wno-declaration-after-statement -Wno-implicit-function-declaration -Wno-int-conversion"
export CFLAGS=$C_OPTS
export CXXFLAGS=$C_OPTS
export CROSSCFLAGS="-g0 -O2"
export LDFLAGS="-L$deps/lib -Wl,-rpath=$RUNTIME_PATH/lib"

export FREETYPE_CFLAGS="-I$deps/include/freetype2"
export PULSE_CFLAGS="-I$deps/include/pulse"
export PULSE_LIBS="-L$deps/lib/pulseaudio -lpulse"
export SDL2_CFLAGS="-I$deps/include/SDL2"
export SDL2_LIBS="-L$deps/lib -lSDL2"
export X_CFLAGS="-I$deps/include/X11"
export X_LIBS="-landroid-sysvshm"
export GSTREAMER_CFLAGS="-I$deps/include/gstreamer-1.0 -I$deps/include/glib-2.0 -I$deps/lib/glib-2.0/include -I$deps/glib-2.0/include -I$deps/lib/gstreamer-1.0/include"
export GSTREAMER_LIBS="-L$deps/lib -lgstgl-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0"
export FFMPEG_CFLAGS="-I$deps/include"
export FFMPEG_LIBS="-L$deps/lib -lavutil -lavcodec -lavformat"

# Wayland driver deps (winewayland.drv). The bionic aarch64 libs+headers are vendored in
# android/wayland-deps and staged into $deps here. Like the other deps above we set
# *_CFLAGS/_LIBS explicitly so configure uses them directly instead of pkg-config (whose
# .pc prefix points at an absolute Termux path that doesn't exist on the CI host). The host
# wayland-scanner (x86_64) is found on PATH via AC_PATH_PROG.
export WAYLAND_CLIENT_CFLAGS="-I$deps/include"
export WAYLAND_CLIENT_LIBS="-L$deps/lib -lwayland-client"
export WAYLAND_EGL_CFLAGS="-I$deps/include"
export WAYLAND_EGL_LIBS="-L$deps/lib -lwayland-egl"
export XKBCOMMON_CFLAGS="-I$deps/include"
export XKBCOMMON_LIBS="-L$deps/lib -lxkbcommon"
export XKBREGISTRY_CFLAGS="-I$deps/include"
export XKBREGISTRY_LIBS="-L$deps/lib -lxkbregistry"

# Stage the vendored wayland/xkb bionic deps into the sysroot (idempotent; only if present so
# a stripped checkout still builds the non-wayland path).
_WLD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android/wayland-deps/usr"
if [ -d "$_WLD" ]; then
  mkdir -p "$deps/lib/pkgconfig" "$deps/include"
  cp -rn "$_WLD/lib/." "$deps/lib/" 2>/dev/null || true
  cp -rn "$_WLD/include/." "$deps/include/" 2>/dev/null || true
  echo "Staged vendored wayland/xkb deps into $deps"
fi

for arg in "$@"
do
  if [ "$arg" == "--enable-16kb-pages" ];
  then
    echo "Enabling 16KB page size support..."
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
        if ./build-aarch64.sh; then
            echo "android_sysvshm built successfully"
            # Copy the library to deps/lib for linking
            mkdir -p "$deps/lib"
            cp build-aarch64/libandroid-sysvshm.so "$deps/lib/"
            echo "Copied libandroid-sysvshm.so to $deps/lib/"
        else
            # X_LIBS links -landroid-sysvshm: without it the X11 driver silently loses XShm.
            echo "FATAL: android_sysvshm build failed"
            exit 1
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
      --with-alsa \
      --without-capi \
      --without-coreaudio \
      --without-cups \
      --without-dbus \
      --with-ffmpeg \
      --with-fontconfig \
      --with-freetype \
      --without-gcrypt \
      --without-gettext \
      --with-gettextpo=no \
      --without-gphoto \
      --with-gnutls \
      --without-gssapi \
      --with-gstreamer \
      --without-inotify \
      --without-krb5 \
      --without-netapi \
      --without-opencl \
      --with-opengl \
      --without-osmesa \
      --without-oss \
      --without-pcap \
      --without-pcsclite \
      --without-piper \
      --with-pthread \
      --with-pulse \
      --without-sane \
      --with-sdl \
      --without-udev \
      --without-unwind \
      --without-usb \
      --without-v4l2 \
      --without-vosk \
      --with-vulkan \
      --with-wayland \
      --without-xcomposite \
      --without-xfixes \
      --without-xinerama \
      --with-xrandr \
      --with-xrender \
      --without-xshape \
      --with-xshm \
      --without-xxf86vm \
      || exit $?

    echo "Applying patches..."

    PATCHES=(
      # android network patch
      "common/dlls_dnsapi_libresolv_c.patch"
      "common/dlls_dnsapi_record_c.patch"
      "common/dlls_nsiproxy_sys_ip_c.patch"
      "common/dlls_nsiproxy_sys_ndis_c.patch"
      "common/dlls_nsiproxy_sys_nsi_common_h.patch"
      "common/dlls_user32_makefile_in.patch"
      "common/dlls_ws2_32_socket_c.patch"
      # ws2_32: bionic rejects AI_V4MAPPED/AI_ALL -> emulate (EA DirtySDK / dual-stack DNS)
      "common/dlls_ws2_32_unixlib_c.patch"

      # xinput: a transient WAIT_FAILED (esync ppoll EAGAIN) killed the update thread for good,
      # taking every pad AND the on-screen controller with it until the game was relaunched.
      "common/dlls_xinput1_3_main_c.patch"
      "common/server_token_c.patch"
      "common/server_unicode_c.patch"

      # midi support
      "common/midi_support.patch"

      # sdl patch
      "common/dlls_winebus_sys_bus_sdl_c.patch"

      # shm_utils
      "common/dlls_ntdll_unix_esync_c.patch"
      "common/dlls_ntdll_unix_fsync_c.patch"
      "common/server_esync_c.patch"
      "common/server_fsync_c.patch"

      # winex11
      "common/dlls_winex11_drv_bitblt_c.patch"
      "common/dlls_winex11_drv_desktop_c.patch"
      "common/dlls_winex11_drv_keyboard_c.patch"
      "common/dlls_winex11_drv_mouse_c.patch"
      "common/dlls_winex11_drv_opengl_c.patch"
      "common/dlls_winex11_drv_window_c.patch"
      "common/dlls_winex11_drv_x11drv_h.patch"
      "common/dlls_winex11_drv_x11drv_main_c.patch"

      # address space patches
      "common/loader_preloader_c.patch"
      "arm64ec/dlls_ntdll_unix_virtual_c.patch"

      # Android bionic bug-fixes (shell32 drive-root copy guard;
      # LC_ALL=C.UTF-8 locale bring-up)
      "common/dlls_shell32_shlfileop_c.patch"
      "common/dlls_ntdll_unix_env_c.patch"

      # syscall Patches (use test-bylaws below)
      # "arm64ec/dlls_wow64_syscall_c.patch"

      # pulse Patches
      "common/dlls_winepulse_drv_pulse_c.patch"

      # desktop patches
      "common/programs_explorer_desktop_c.patch"

      # path patches
      "common/dlls_ntdll_unix_server_c.patch"

      # winlator patches
      "common/dlls_amd_ags_x64_unixlib_c.patch"

      # shortcut patch
      "common/programs_winemenubuilder_winemenubuilder_c.patch"

      # xuser patches
      "common/dlls_advapi32_advapi_c.patch"

      # browser patches
      "common/programs_winebrowser_makefile_in.patch"
      "common/programs_winebrowser_main_c.patch"

      # clipboard patches
      "common/dlls_user32_clipboard_c.patch"
      "common/dlls_win32u_clipboard_c.patch"

      # fexcore patch
      "arm64ec/dlls_ntdll_loader_c.patch"
      "arm64ec/dlls_ntdll_unix_loader_c.patch"
      "arm64ec/loader_wine_inf_in.patch"
      "test-bylaws/programs_services_services_c.patch"
      "test-bylaws/dlls_winecrt0_arm64ec_c.patch"

      # fix build
      "arm64ec/dlls_wdscore_wdscore_spec.patch"
      "arm64ec/programs_wineboot_wineboot_c.patch"

      # 1. Extended State (XSTATE/YMM) Support Patches
      "test-bylaws/dlls_ntdll_unwind_h.patch"
      "test-bylaws/include_winnt_h.patch"

      # 2. Thread Suspension Patches
      "test-bylaws/dlls_ntdll_signal_arm64_c.patch"
      "test-bylaws/dlls_ntdll_signal_arm64ec_c.patch"
      "test-bylaws/dlls_ntdll_signal_x86_64_c.patch"
      "test-bylaws/dlls_ntdll_unix_debug_c.patch"
      "test-bylaws/dlls_ntdll_unix_signal_arm64_c.patch"
      "test-bylaws/dlls_ntdll_unix_signal_arm_c.patch"
      "test-bylaws/dlls_ntdll_unix_signal_i386_c.patch"
      "test-bylaws/dlls_ntdll_unix_unix_private_h.patch"
      "test-bylaws/dlls_ntdll_ntdll_spec.patch"
      "test-bylaws/dlls_ntdll_ntdll_misc_h.patch"
      "test-bylaws/dlls_wow64_process_c.patch"
      "test-bylaws/dlls_wow64_syscall_c.patch"
      "test-bylaws/dlls_wow64_wow64_spec.patch"

      # 3. Process and Virtual Memory Management
      "test-bylaws/dlls_wow64_virtual_c.patch"
      "test-bylaws/dlls_ntdll_unix_process_c.patch"

      # 4. Server and Threading Infrastructure
      "test-bylaws/dlls_ntdll_unix_thread_c.patch"
      "test-bylaws/server_process_c.patch"
      "test-bylaws/server_thread_h.patch"
      "test-bylaws/server_thread_c.patch"
      "test-bylaws/server_mapping_c.patch"

      # 5. Internal Headers
      "test-bylaws/include_winternl_h.patch"

      # 5a. FEX unixlib load-by-name (MemoryWineLoadUnixLibByName = 1002)
      "test-bylaws/include_wine_unixlib_h.patch"

      # 6. build vcruntime140_1 with aarch64
      "test-bylaws/dlls_vcruntime140_1_vcruntime140_1_spec.patch"

      # 7. Build System (Optional)
#      "test-bylaws/tools_makedep_c.patch"

      # 8. v15 (2026-10-23). From GameNative/proton-wine (proton_11.0-2), credited per patch;
      #    #58 and #73 re-ported onto this Wine 10 tree (#69, #82 and #66 do not apply here:
      #    no image pool, no other-process rendering, no_error only advertised when GLX has it)
      # kernelbase: report AMD64 from GetNativeSystemInfo() in x64 processes (#94, upstream Wine c5a96a0b6c, Brendan Shanks)
      "common/dlls_kernelbase_memory_c_native_system_info.patch"
      # crypt32: CRYPT_E_NOT_FOUND for an empty enhanced key usage (#46, Cole McCarren)
      "common/dlls_crypt32_cert_c_enhkey_usage.patch"
      # ntdll: release the arm64ec emulator cpu area from its view base (#42, David Luo)
      "common/dlls_ntdll_unix_virtual_c_emu_stack_release.patch"
      # win32u: keep the full mode list after a mode change in virtual desktop mode (#58, Utkarsh Dalal)
      "common/dlls_win32u_sysparams_c_modelist.patch"
      # ntdll: relocate fixed allocations above the host address space limit (#73, Utkarsh Dalal)
      "common/dlls_ntdll_unix_virtual_c_relocate_high_allocs.patch"
    )

    for patch in "${PATCHES[@]}"; do
#      if git apply --check ./android/patches/$patch 2>/dev/null; then
        git apply ./android/patches/$patch || exit $?
#      fi
    done

    # ---------------------------------------------------------------------
    # HARD post-apply verification.
    #
    # The apply loop above is fail-hard, but it cannot notice a patch that was
    # dropped from the PATCHES array, a graft that a later upstream change made
    # a no-op, or an in-tree feature lost in a merge. So grep the ACTUAL
    # post-apply source for one token per shipped feature and refuse to build a
    # silently-degraded layer if any is missing. (build-scripts/verify-layer.py
    # repeats the same idea on the COMPILED binaries after --install.)
    # ---------------------------------------------------------------------
    echo "Verifying shipped features are present in the source tree..."
    verify_fail=0
    MARKERS=(
      "dlls/sechost/Makefile.in|IMPORTS   = kernelbase rpcrt4|sechost imports rpcrt4 directly (no variadic exit thunk; DOOM 2016 GOG / EA app service crash)"
      "dlls/advapi32/Makefile.in|IMPORTS   = kernelbase sechost msvcrt rpcrt4|advapi32 imports rpcrt4 directly (no variadic exit thunk)"
      "dlls/ntdll/unix/virtual.c|force_anon|noexec/force_anon SD-card boot (Dragon Age)"
      "dlls/shell32/shlfileop.c|dir_len|drive-root FO_COPY guard"
      "dlls/ntdll/unix/env.c|C.UTF-8|LC_ALL=C.UTF-8 bionic locale bring-up"
      "dlls/winedirectaudio.drv/directaudio.c|BANNER_AUDIO_DIRECT_MIC|DirectAudio driver is the v1.3.2 build (mic capture)"
      "dlls/xinput1_3/main.c|transient wait failure in the update thread|xinput WAIT_FAILED retry (controller-dies fix)"
      "dlls/ws2_32/unixlib.c|EMULATE_V4MAPPED|ws2_32 AI_V4MAPPED emulation (EA DirtySDK DNS)"
      "dlls/nsiproxy.sys/ip.c|WINE_ANDROID_GATEWAY|nsiproxy default-route fix (EA offline latch)"
      "dlls/dnsapi/libresolv.c|LIBANDROID_HANDLE|dnsapi Android resolver"
      "dlls/win32u/clipboard.c|WINE_FROM_ANDROID_CLIPBOARD|Android clipboard bridge (win32u)"
      "server/fsync.c|!defined(__ANDROID__)|fsync compiled out on Android (seccomp blocks futex_waitv)"
      "dlls/ntdll/unix/sync.c|WINE_FAST_YIELD|fast-yield gate (in-tree)"
      "dlls/ntdll/unix/virtual.c|WINEVMEMMAXSIZE|WINEVMEMMAXSIZE address-space cap (in-tree)"
      "dlls/win32u/font.c|MAX_FONT_HANDLES  32768|realized-font-handle cap 32768 (in-tree)"
      "dlls/ntdll/signal_arm64ec.c|if (ptr >> 47) return FALSE;|RtlIsEcCode bounds guard (Denuvo / NFS Heat, in-tree)"
      "programs/explorer/systray.c|WINE_TASKBAR_STYLE|XP taskbar (in-tree)"
      "dlls/win32u/defwnd.c|WINE_XP_FRAMES|XP window frames (in-tree)"
      "dlls/ntdll/unix/esync.c|shm_utils.h|esync Android shm glue"
      "server/esync.c|esync: up and running|esync server side (in-tree on Wine-10)"
      "dlls/user32/clipboard.c|WINE_FROM_ANDROID_CLIPBOARD|Android clipboard bridge (user32 half, Wine-10 only)"
      "server/token.c|machine-id|per-prefix machine-id fallback (Wine-10 only)"
      "dlls/ntdll/unix/loader.c|load_unixlib_by_name|FEX unixlib load-by-name loader"
      "dlls/kernelbase/memory.c|if (!is_wow64 || native_machine != IMAGE_FILE_MACHINE_AMD64)|GetNativeSystemInfo AMD64 in x64 processes (Wine c5a96a0b6c)"
      "dlls/crypt32/cert.c|Windows sets the last error to CRYPT_E_NOT_FOUND|crypt32 empty EKU last error (GameNative #46)"
      "dlls/ntdll/unix/virtual.c|void *base = (char *)teb->ChpeV2CpuAreaInfo - kernel_stack_guard_size;|arm64ec emulator cpu area released (GameNative #42)"
      "dlls/win32u/sysparams.c|bound the mode list by the desktop size, not the current mode|virtual desktop keeps its full mode list (GameNative #58)"
      "dlls/ntdll/unix/virtual.c|relocating fixed allocation|fixed allocations beyond the host address limit relocated (GameNative #73)"
      "dlls/winewayland.drv/wayland.c|banner_desktop_v1|Wayland virtual desktop (banner_desktop_v1)"
      "dlls/winewayland.drv/wayland_touch.c|touch_lparam|Wayland touch (wl_touch -> WM_POINTER*, normalized lparam)"
      "dlls/winewayland.drv/wayland_edid.c|wayland_edid_build|Wayland HDR10 monitor EDID"
    )
    for row in "${MARKERS[@]}"; do
      m_file="${row%%|*}"; rest="${row#*|}"; m_token="${rest%%|*}"; m_what="${rest#*|}"
      if [ -f "$m_file" ] && grep -qF -- "$m_token" "$m_file"; then
        echo "  ok    $m_what"
      else
        echo "  FATAL $m_what -- '$m_token' not found in $m_file"
        verify_fail=1
      fi
    done
    if [ "$verify_fail" != "0" ]; then
      echo "FATAL: one or more shipped features are missing from the source tree; refusing to build a silently-broken layer."
      exit 1
    fi
    echo "All shipped features verified present in the source tree."
    echo "----------------------------------------"

    # GE-Proton game-fixes tier, layered AFTER the bionic patches (verified to
    # apply cleanly on the bionic-patched tree in this order). apply-ge-patches.sh
    # hard-fails on any reject AND checks one source marker per patch.
    if [ -d ./android/ge-patches/game-fixes ]; then
      echo "Applying GE-Proton patches..."
      ./build-scripts/apply-ge-patches.sh || exit $?
    fi

  fi

  if [ "$arg" == "--build" ]
  then
    echo "Building..."
    rm -rf $OUTPUT_DIR/bin
    rm -rf $OUTPUT_DIR/lib
    rm -rf $OUTPUT_DIR/share
    rm -rf $install_dir
    make -j$(nproc) || exit $?
  fi

  if [ "$arg" == "--install" ]
  then
    echo "Installing..."
    mkdir -p $OUTPUT_DIR/bin
    mkdir -p $OUTPUT_DIR/lib
    mkdir -p $OUTPUT_DIR/share
    mkdir -p $install_dir
    make install -j$(nproc) || exit $?
    cp -r $install_dir/bin/wine* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/reg* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/msi* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/notepad $OUTPUT_DIR/bin
    cp -r $install_dir/lib/wine  $OUTPUT_DIR/lib
    cp -r $install_dir/share/wine  $OUTPUT_DIR/share

    # Bundle winewayland.so's runtime deps into the wcp lib/ so the driver can load even where
    # the imagefs doesn't (yet) ship them. Vendored bionic aarch64 libs from android/wayland-deps.
    _WLD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android/wayland-deps/usr/lib"
    if [ -d "$_WLD" ]; then
      cp -n "$_WLD"/libwayland-client.so "$_WLD"/libwayland-egl.so \
            "$_WLD"/libxkbcommon.so "$_WLD"/libxkbregistry.so "$OUTPUT_DIR/lib/" 2>/dev/null || true
      # Our Wayland Turnip links libdrm (its Wayland WSI needs the DRM image path); the imagefs has
      # one, but ship the copy it was built against so the ICD never depends on that.
      [ -f "$_WLD"/libdrm.so ] && cp -n "$_WLD"/libdrm.so "$OUTPUT_DIR/lib/" 2>/dev/null || true
      echo "Bundled wayland/xkb runtime libs into wcp lib/"
      # Wayland-capable Turnips (our Banners-Turnip `wayland` build, see android/wayland-deps/TURNIP.md):
      # the plain driver plus the Adreno 7xx (710/720/722), the two WN-Turnip 8xx tunings (Balanced,
      # Performance), the gen8 8xx build, StevenMXZ's Gen8 V36, whitebelyash's Mainline v31 and pure
      # upstream main, each with its ICD manifest. winewayland picks one on the Bannerlator compositor
      # (BANNER_WAYLAND_VK_VARIANT / BANNER_WAYLAND_VK_ICD); lib/libvulkan_freedreno_wayland.so is
      # what the app checks for. All eight ship or the build fails: a wcp missing a variant would
      # silently render 710/720 or 8xx devices on the plain driver, which cannot create a device there.
      if [ -f "$_WLD"/libvulkan_freedreno_wayland.so ]; then
        mkdir -p "$OUTPUT_DIR/share/vulkan/icd.d"
        for v in "" _a7xx _a8xx _a8xx_perf _a8xx_gen8 _a8xx_smxz _a8xx_white _a8xx_upstream; do
          [ -f "$_WLD/libvulkan_freedreno_wayland$v.so" ] && [ -f "$_WLD/../share/vulkan/icd.d/banner_wayland_turnip$v.json" ] \
            || { echo "ERROR: Wayland Turnip variant '$v' (libvulkan_freedreno_wayland$v.so + banner_wayland_turnip$v.json) missing from android/wayland-deps" >&2; exit 1; }
          cp "$_WLD/libvulkan_freedreno_wayland$v.so" "$OUTPUT_DIR/lib/"
          cp "$_WLD/../share/vulkan/icd.d/banner_wayland_turnip$v.json" "$OUTPUT_DIR/share/vulkan/icd.d/"
        done
        echo "Bundled the Wayland Turnip ICDs (plain, a7xx, a8xx, a8xx_perf, a8xx_gen8, a8xx_smxz, a8xx_white, a8xx_upstream) into wcp"
      fi
      # xkeyboard-config data for the bundled libxkbregistry/libxkbcommon (XKB-SOURCE.md next to it):
      # winewayland sets XKB_CONFIG_ROOT to it so layouts get their real names.
      if [ -f "$_WLD"/../share/X11/xkb/rules/evdev.xml ]; then
        mkdir -p "$OUTPUT_DIR/share/X11"
        cp -r "$_WLD"/../share/X11/xkb "$OUTPUT_DIR/share/X11/"
        echo "Bundled xkeyboard-config into wcp share/X11/xkb ($(du -sh "$OUTPUT_DIR/share/X11/xkb" | cut -f1))"
      fi
      # Mesa's EGL (Wayland platform) + Zink for OpenGL, from the same build as that Turnip,
      # with the libwayland-server its EGL links.
      if [ -f "$_WLD"/libEGL.so.1 ]; then
        cp "$_WLD"/libEGL.so.1 "$_WLD"/libGLESv2.so.2 "$_WLD"/libgallium-*.so \
           "$OUTPUT_DIR/lib/"
        echo "Bundled Mesa EGL + Zink into wcp"
      fi
    fi

    # Strip the packaged binaries to shrink the tree. llvm-strip ($STRIP) is arm64ec/COFF-aware AND
    # handles ELF, so it strips both the PE DLLs/EXEs and the unix .so loaders. --strip-all keeps the
    # PE export directory + ELF .dynsym (so DLLs still resolve and .so still loads); falls back to
    # --strip-debug. Non-fatal per file so an unexpected format can never fail the build.
    echo "Stripping binaries with llvm-strip to shrink the tree..."
    before_mb=$(du -sm "$OUTPUT_DIR" 2>/dev/null | cut -f1)
    find "$OUTPUT_DIR/lib" "$OUTPUT_DIR/bin" -type f \
      \( -name '*.dll' -o -name '*.exe' -o -name '*.drv' -o -name '*.so' -o -name 'wine' -o -name 'wine-preloader' \) \
      -print0 2>/dev/null | while IFS= read -r -d '' f; do
        "$STRIP" --strip-all "$f" 2>/dev/null || "$STRIP" --strip-debug "$f" 2>/dev/null || true
      done
    after_mb=$(du -sm "$OUTPUT_DIR" 2>/dev/null | cut -f1)
    echo "OUTPUT tree: ${before_mb}MB -> ${after_mb}MB after strip."

    # A Wayland layer that lost its driver or its Turnips would install and then silently fall back
    # to X11 (or render on nothing). Refuse to package one.
    wl_fail=0
    for f in lib/wine/aarch64-unix/winewayland.so lib/wine/aarch64-windows/winewayland.drv \
             lib/wine/i386-windows/winewayland.drv; do
      if [ -f "$OUTPUT_DIR/$f" ]; then echo "  ok    $f"; else echo "  FATAL $f is missing from the layer" >&2; wl_fail=1; fi
    done
    for v in "" _a7xx _a8xx _a8xx_perf _a8xx_gen8 _a8xx_smxz _a8xx_white _a8xx_upstream; do
      if [ -f "$OUTPUT_DIR/lib/libvulkan_freedreno_wayland$v.so" ] && [ -f "$OUTPUT_DIR/share/vulkan/icd.d/banner_wayland_turnip$v.json" ]; then
        echo "  ok    Wayland Turnip '$v'"
      else
        echo "  FATAL Wayland Turnip '$v' is missing from the layer" >&2; wl_fail=1
      fi
    done
    if [ -f "$OUTPUT_DIR/lib/wine/aarch64-unix/winewayland.so" ] && grep -qa banner_desktop_v1 "$OUTPUT_DIR/lib/wine/aarch64-unix/winewayland.so"; then
      echo "  ok    winewayland.so speaks banner_desktop_v1"
    else
      echo "  FATAL winewayland.so does not carry banner_desktop_v1 (the virtual desktop protocol)" >&2; wl_fail=1
    fi
    if [ "$wl_fail" != "0" ]; then
      echo "FATAL: the Wayland driver is incomplete in this layer; refusing to package it." >&2
      exit 1
    fi
    echo "Wayland driver and the eight Wayland Turnips verified in the layer."
  fi
done
