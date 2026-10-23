#!/bin/bash

# Fail hard on any command error. Note: `set -e` does NOT cover commands inside
# `if` bodies below, so the critical steps (configure / git apply / make) also
# carry explicit `|| exit $?` — without this a failing `make` used to be masked
# by the trailing `if [ "$arg" == "--install" ]; then ... fi` returning 0, so
# CI shipped a broken (skeleton) wcp while reporting success.
set -eo pipefail

export ARCH="x86_64"
export WIN_ARCH="x86_64,i386"
export OUTPUT_DIR="$HOME/compiled-files-x86_64"

export deps="$HOME/termuxfs/x86_64/data/data/com.termux/files/usr"
export RUNTIME_PATH="/data/data/com.termux/files/usr"
export install_dir=$deps/../opt/wine

#export TOOLCHAIN="$HOME/Android/android-ndk-r27d/toolchains/llvm/prebuilt/linux-x86_64/bin"
export TOOLCHAIN="$HOME/Android/Sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin"
export LLVM_MINGW_TOOLCHAIN="$HOME/toolchains/llvm-mingw-20250920-ucrt-ubuntu-22.04-x86_64/bin"
export TARGET=x86_64-linux-android28
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
# Applied to the unix side via CFLAGS and to the x86_64 PE side via CROSSCFLAGS.
# (A post-install llvm-strip pass in --install trims the remaining symbol tables.)
export C_OPTS="-march=x86-64 -mtune=generic -g0 -O2 -Wno-declaration-after-statement -Wno-implicit-function-declaration -Wno-int-conversion"
export CFLAGS=$C_OPTS
export CXXFLAGS=$C_OPTS
export CROSSCFLAGS="-g0 -O2"
export LDFLAGS="-L$deps/lib -Wl,-rpath=$RUNTIME_PATH/lib"

export FREETYPE_CFLAGS="-I$deps/include/freetype2"
export PULSE_CFLAGS="-I$deps/include/pulse"
export PULSE_LIBS="-L$deps/lib/pulseaudio -lpulse"
export SDL2_CFLAGS="-I$deps/include/SDL2"
export SDL2_LIBS="-L$deps/lib -lSDL2"
export FONTCONFIG_LIBS="-L$deps/lib -lfontconfig -lfreetype -lexpat"
export X_CFLAGS="-I$deps/include/X11"
export X_LIBS=""
export GSTREAMER_CFLAGS="-I$deps/include/gstreamer-1.0 -I$deps/include/glib-2.0 -I$deps/lib/glib-2.0/include -I$deps/glib-2.0/include -I$deps/lib/gstreamer-1.0/include"
export GSTREAMER_LIBS="-L$deps/lib -lgstgl-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0"
export FFMPEG_CFLAGS="-I$deps/include"
export FFMPEG_LIBS="-L$deps/lib -lavutil -lavcodec -lavformat"

for arg in "$@"
do
  if [ "$arg" == "--enable-16kb-pages" ];
  then
    echo "Enabling 16KB page size support..."
    # NOTE: this block used to `export TARGET=x86_64-linux-android35`, but CC/CXX/AS were
    # already bound to the android28 clang above, so every "sdk35" x86_64 build was in
    # fact an android28 build with 16 KB linker alignment (verified via the ELF
    # .note.android.ident). Keep that honest: 16 KB alignment on the android28 target,
    # the same shape as the arm64ec builds.
    export C_OPTS="$C_OPTS -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES"
    export CFLAGS="$C_OPTS"
    export CXXFLAGS="$C_OPTS"
    export LDFLAGS="$LDFLAGS -Wl,-z,max-page-size=16384"
    echo "16KB page size support enabled"
  fi

  if [ "$arg" == "--build-ntsync-android" ];
  then
    # Userspace ntsync backend, used ONLY when WINENTSYNC=1 is set at runtime
    # (esync stays the default). ntsync-android by Joshua Tam (joshuatam,
    # GameNative), https://github.com/GameNative/ntsync-android, LGPL-3.0-only;
    # the workflow checks it out pinned to 7ce6435. Statically linked into
    # ntdll.so and wineserver (-lntsync_android in both UNIX_LIBS), so the
    # link is unconditional and a failed build here must stop the layer.
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
    NTSYNC_DIR="${NTSYNC_ANDROID_DIR:-$PROJECT_ROOT/../ntsync-android}"
    NTSYNC_TRIPLE=x86_64-linux-android
    if [ ! -f "$NTSYNC_DIR/Cargo.toml" ]; then
        echo "FATAL: ntsync-android not found at $NTSYNC_DIR"
        exit 1
    fi
    echo "Building libntsync_android.a ($NTSYNC_TRIPLE) from $NTSYNC_DIR @ $(git -C "$NTSYNC_DIR" rev-parse HEAD 2>/dev/null)"
    rustup target add "$NTSYNC_TRIPLE" || exit $?
    # Build from the crate root so its .cargo/config.toml applies (it only
    # matters for the cdylib; the static archive takes the page alignment of
    # ntdll.so/wineserver, i.e. this script's LDFLAGS).
    ( cd "$NTSYNC_DIR" && env CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER="$TOOLCHAIN/$TARGET-clang" AR="$TOOLCHAIN/llvm-ar" \
        cargo build --release --locked --target "$NTSYNC_TRIPLE" ) || { echo "FATAL: ntsync-android build failed"; exit 1; }
    mkdir -p "$deps/lib"
    cp "$NTSYNC_DIR/target/$NTSYNC_TRIPLE/release/libntsync_android.a" "$deps/lib/" || { echo "FATAL: libntsync_android.a missing"; exit 1; }
    rm -f "$deps/lib/libntsync_android.so"
    echo "Copied libntsync_android.a ($NTSYNC_TRIPLE) to $deps/lib/"
  fi

  if [ "$arg" == "--build-sysvshm" ];
  then
    # Build android_sysvshm library
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

    if [ -d "$PROJECT_ROOT/android/android_sysvshm" ]; then
        echo "Building android_sysvshm library..."
        cd "$PROJECT_ROOT/android/android_sysvshm"
        if ./build-x86_64.sh; then
            echo "android_sysvshm built successfully"
            # Copy the library to deps/lib for linking
            mkdir -p "$deps/lib"
            cp build-x86_64/libandroid-sysvshm.so "$deps/lib/"
            echo "Copied libandroid-sysvshm.so to $deps/lib/"
        else
            # X_LIBS links -landroid-sysvshm: without it the X11 driver silently loses XShm.
            echo "FATAL: android_sysvshm build failed"
            exit 1
        fi
        cd "$PROJECT_ROOT"
    fi
  fi

  # lsteamclient (the Steam bridge to a native host client) is arm64ec-only: an x86_64 unix side
  # cannot dlopen the host's aarch64 libsteamclient.so, so it is not built for this layer.
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
      --disable-lsteamclient \
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
      --without-oss \
      --without-pcap \
      --without-pcsclite \
      --without-piper \
      --with-pthread \
      --with-pulse \
      --without-pipewire \
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
      --without-xfixes \
      --without-xinerama \
      --with-xrandr \
      --with-xrender \
      --without-xshape \
      --without-xshm \
      --without-xxf86vm \
      || exit $?

    echo "Applying patches..."

    PATCHES=(
      # core patches
      "dlls_advapi32_advapi.c.patch"
      "dlls_amd_ags_x64_unixlib.c.patch"

      # dns
      "dlls_dnsapi_libresolv.c.patch"
      "dlls_dnsapi_record.c.patch"

      # ws2_32: bionic rejects AI_V4MAPPED/AI_ALL -> emulate (EA DirtySDK / dual-stack DNS)
      "dlls_ws2_32_unixlib.c.patch"

      # gdiplus: clamp degenerate spans instead of assert()/abort (EA app installer wizard)
      "dlls_gdiplus_region.c.patch"

      # xinput: a transient WAIT_FAILED (esync ppoll EAGAIN) killed the update thread for good,
      # taking every pad AND the on-screen controller with it until the game was relaunched.
      "dlls_xinput1_3_main.c.patch"

      # midi
      "dlls_midimap_Makefile.in.patch"
      "dlls_midimap_midimap.c.patch"

      # nsiproxy
	  "dlls_nsiproxy.sys_nsi_common.h.patch"
      "dlls_nsiproxy.sys_ip.c.patch"
      "dlls_nsiproxy.sys_ndis.c.patch"

      # ntdll
      "dlls_ntdll_Makefile.in.patch"
      "dlls_ntdll_unix_fsync.c.patch"
      "dlls_ntdll_unix_loader.c.patch"
      "dlls_ntdll_unix_server.c.patch"
      "dlls_ntdll_unix_sync.c.patch"
      "dlls_ntdll_unix_virtual.c.patch"

      # Android bionic locale bring-up: force LC_ALL=C.UTF-8 before locale init
      # (bionic ships no locale data beyond C/C.UTF-8).
      "dlls_ntdll_unix_env.c.patch"

      # unixlib load-by-name (MemoryWineLoadUnixLibByName, used by the FEX companion
      # unixlib) and the dump_syscall_fault_return `jmp *%rcx` fix are already in the
      # Proton-CachyOS tree (upstream Wine, same enum values 1002-1004), so their
      # patches (unix_private.h, wow64 virtual.c, wine/unixlib.h, winternl.h,
      # signal_x86_64.c) are gone; loader.c/virtual.c keep only the Android parts.
	  
	  # opengl32
	  "dlls_opengl32_unix_wgl.c.patch"

      # shell32: guard bare drive-root (D:\ / D:) FO_COPY between roots
      "dlls_shell32_shlfileop.c.patch"

      # user32 / clipboard
      "dlls_user32_Makefile.in.patch"
      "dlls_win32u_clipboard.c.patch"

      # drivers
      "dlls_winebus.sys_bus_sdl.c.patch"
      "dlls_winepulse.drv_pulse.c.patch"

      # winex11
      "dlls_winex11.drv_bitblt.c.patch"
      "dlls_winex11.drv_keyboard.c.patch"
      "dlls_winex11.drv_mouse.c.patch"
      "dlls_winex11.drv_opengl.c.patch"
      "dlls_winex11.drv_window.c.patch"
      "dlls_winex11.drv_x11drv.h.patch"
      "dlls_winex11.drv_x11drv_main.c.patch"

      # wow64
      # "dlls_wow64_process.c.patch"
      "dlls_wow64_syscall.c.patch"

      # loader
      "loader_preloader.c.patch"

      # programs
      "programs_explorer_desktop.c.patch"
      "programs_wineboot_wineboot.c.patch"
      "programs_winebrowser_Makefile.in.patch"
      "programs_winebrowser_main.c.patch"
      "programs_winemenubuilder_winemenubuilder.c.patch"

      # server
      "server_Makefile.in.patch"
      "server_fsync.c.patch"
      "server_inproc_sync.c.patch"
      "server_main.c.patch"
      # "server_protocol.def.patch"
      "server_thread.c.patch"
      "server_unicode.c.patch"
	  
	  # esync
	  "dlls_ntdll_unix_esync.c.patch"
	  "dlls_ntdll_unix_esync.h.patch"
	  "server_esync.c.patch"
	  "server_esync.h.patch"
	  # userspace ntsync, OPT-IN at runtime (WINENTSYNC=1); must follow esync
	  "ntsync_userspace.patch"

      # v15 (2026-10-23)
      # FEX unixlib: Valve's load_unixlib_by_name set the extension one byte early for an
      # extensionless name, so it looked for libarm64ecfe.so / libwow64fe.so and FEX ran
      # without its unixlib (same bug our proton_11.0 branch fixed in f3ec0cb6)
      "dlls_ntdll_unix_loader.c_unixlib_name.patch"
      # From GameNative/proton-wine (proton_11.0-2), credited per patch:
      # win32u: keep the full mode list after a mode change in virtual desktop mode (#58, Utkarsh Dalal)
      "dlls_win32u_sysparams.c_modelist.patch"
      # server: keep the 64-bit image map pool below bionic's randomised mappings (#69, Utkarsh Dalal)
      "server_mapping.c_image_pool.patch"
      # ntdll: relocate fixed allocations above the host address space limit (#73, Utkarsh Dalal)
      "dlls_ntdll_unix_virtual.c_relocate_high_allocs.patch"
      # crypt32: CRYPT_E_NOT_FOUND for an empty enhanced key usage (#46, Cole McCarren)
      "dlls_crypt32_cert.c_enhkey_usage.patch"
      # winex11: Vulkan other-process rendering off by default on Android (#82, Dmage22 via Utkarsh Dalal;
      # WINE_DISABLE_VULKAN_OPWR=0 turns it back on)
      "dlls_winex11.drv_init.c_opwr.patch"
      # winex11: drop WGL_CONTEXT_OPENGL_NO_ERROR_ARB when GLX lacks it (#66, Utkarsh Dalal)
      "dlls_winex11.drv_opengl.c_no_error.patch"
    )

    # Fail-HARD apply loop. The old loop reported a drifted patch as "SKIPPED" and
    # let the build continue GREEN — that is how GE-11.0-5 once shipped without the
    # noexec/force_anon fix. `git apply` is atomic (all hunks or none, no fuzz), so a
    # non-zero exit here means the patch is NOT in the tree: stop the build.
    for patch in "${PATCHES[@]}"; do
      echo "----------------------------------------"
      echo "Applying: $patch"
      if [ ! -f "./android/patches/$patch" ]; then
        echo "FATAL: ./android/patches/$patch does not exist"
        exit 1
      fi
      if git apply "./android/patches/$patch"; then
        echo "SUCCESS: $patch applied"
      else
        echo "FATAL: $patch does not apply cleanly; refusing to build a layer without it"
        git apply --check "./android/patches/$patch" || true
        exit 1
      fi
    done

    echo "----------------------------------------"
    echo "Done applying patches."

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
      "dlls/ntdll/unix/virtual.c|force_anon|noexec/force_anon SD-card boot (Dragon Age)"
      "dlls/shell32/shlfileop.c|dir_len|drive-root FO_COPY guard"
      "dlls/ntdll/unix/env.c|C.UTF-8|LC_ALL=C.UTF-8 bionic locale bring-up"
      "dlls/winedirectaudio.drv/directaudio.c|BANNER_AUDIO_DIRECT_MIC|DirectAudio driver is the v1.3.2 build (mic capture)"
      "dlls/xinput1_3/main.c|transient wait failure in the update thread|xinput WAIT_FAILED retry (controller-dies fix)"
      "dlls/winepulse.drv/pulse.c|Failed to create timer event, timer callbacks disabled|winepulse rttime_new guard (re-ported onto the CachyOS pulse main loop)"
      "include/config.h|#define WINE_MMDEVAPI_SYSTEM_THREADS 1|DirectAudio built for the system-thread mmdevapi ABI (CachyOS/upstream mmdevapi)"
      "dlls/winedirectaudio.drv/directaudio.c|static void unix_timer_loop(void *args);|DirectAudio driver carries the system-thread mmdevapi ABI port"
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
      "dlls/ntdll/unix/esync.c|ESYNC_AUTO_EVENT|esync re-added to Wine-11 (Proton 11 dropped it upstream)"
      "server/esync.c|esync: up and running|esync server side re-added to Wine-11"
      "server/inproc_sync.c|WINENTSYNC set, no usable /dev/ntsync, using userspace ntsync|userspace ntsync, opt-in via WINENTSYNC=1 (server)"
      "dlls/ntdll/unix/esync.c|if (ntsync_opt_in_active) return 0;|esync steps aside only under the WINENTSYNC opt-in (client)"
      "dlls/ntdll/unix/sync.c|userspace_wait_objs|userspace ntsync wait path (client)"
      "dlls/gdiplus/region.c|if (x1_min <= x) x1_min = x + 1;|gdiplus degenerate-span clamp (EA installer wizard)"
      "dlls/ntdll/unix/loader.c|load_unixlib_by_name|FEX unixlib load-by-name loader"
      "dlls/ntdll/unix/loader.c|ends at pos + 1 + len, not pos + len|FEX unixlib full name (load_unixlib_by_name extension offset)"
      "dlls/win32u/sysparams.c|bound the mode list by the desktop size, not the current mode|virtual desktop keeps its full mode list (GameNative #58)"
      "server/mapping.c|free_available_high_map_addr( 0x5000000000, 0x1000000000 );|64-bit image map pool below bionic mappings (GameNative #69)"
      "dlls/ntdll/unix/virtual.c|relocating fixed allocation|fixed allocations beyond the host address limit relocated (GameNative #73)"
      "dlls/crypt32/cert.c|Windows sets the last error to CRYPT_E_NOT_FOUND|crypt32 empty EKU last error (GameNative #46)"
      "dlls/winex11.drv/init.c|Failing it makes Chromium-based launchers fall back to software compositing|Vulkan OPWR off by default on Android (GameNative #82)"
      "dlls/winex11.drv/opengl.c|GLX has no create_context_no_error|WGL no_error attribute dropped without GLX support (GameNative #66)"
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
    echo "Copying files..."
    cp -r $install_dir/bin/wine* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/reg* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/msi* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/notepad $OUTPUT_DIR/bin
    cp -r $install_dir/lib/wine  $OUTPUT_DIR/lib
    cp -r $install_dir/share/wine  $OUTPUT_DIR/share
    # ntsync-android (statically linked, LGPL-3.0-only): ship its licence + provenance.
    mkdir -p "$OUTPUT_DIR/share/licenses/ntsync-android"
    _NTS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/android/ntsync_android"
    cp "$_NTS/LICENSE" "$_NTS/NOTICE" "$OUTPUT_DIR/share/licenses/ntsync-android/" || exit $?

    # Strip the packaged binaries to shrink the tree. llvm-strip ($STRIP) handles PE (x86_64/i386) +
    # ELF. --strip-all keeps the PE export directory + ELF .dynsym (so DLLs still resolve and .so still
    # loads); falls back to --strip-debug. Non-fatal per file so an unexpected format can't fail the build.
    echo "Stripping binaries with llvm-strip to shrink the tree..."
    before_mb=$(du -sm "$OUTPUT_DIR" 2>/dev/null | cut -f1)
    find "$OUTPUT_DIR/lib" "$OUTPUT_DIR/bin" -type f \
      \( -name '*.dll' -o -name '*.exe' -o -name '*.drv' -o -name '*.so' -o -name 'wine' -o -name 'wine-preloader' \) \
      -print0 2>/dev/null | while IFS= read -r -d '' f; do
        "$STRIP" --strip-all "$f" 2>/dev/null || "$STRIP" --strip-debug "$f" 2>/dev/null || true
      done
    after_mb=$(du -sm "$OUTPUT_DIR" 2>/dev/null | cut -f1)
    echo "OUTPUT tree: ${before_mb}MB -> ${after_mb}MB after strip."
	# symlinking wine binaries to $install_dir/bin
    ln -sf ../lib/wine/x86_64-unix/wine "$install_dir/bin/wine"
    ln -sf ../lib/wine/x86_64-unix/wine "$OUTPUT_DIR/bin/wine"
    ln -sf ../lib/wine/x86_64-unix/wine-preloader "$OUTPUT_DIR/bin/wine-preloader"
    ln -sf ../lib/wine/x86_64-unix/wine-preloader "$install_dir/bin/wine-preloader"
    echo "Wine loader symlinks:"
    ls -la "$OUTPUT_DIR/bin/wine" "$OUTPUT_DIR/bin/wine-preloader"
  fi
done
