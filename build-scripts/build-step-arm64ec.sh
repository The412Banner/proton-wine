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
export C_OPTS="-g0 -O2 -Wno-declaration-after-statement -Wno-implicit-function-declaration -Wno-int-conversion -DHAVE_SYS_EVENTFD_H"
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
      --with-alsa \
      --without-capi \
      --without-coreaudio \
      --without-cups \
      --without-dbus \
      --without-ffmpeg \
      --without-fontconfig \
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
      --without-wayland \
      --without-xcomposite \
      --without-xfixes \
      --without-xinerama \
      --without-xrandr \
      --without-xrender \
      --without-xshape \
      --with-xshm \
      --without-xxf86vm

    # HARD GATE: this script has no `set -e`, so a fatal ./configure (e.g. a missing --with dep
    # aborting via as_fn_error) would otherwise fall through to the patch loop, which returns 0,
    # and green-light a Makefile-less tree -> the --build step then dies with "No targets ... no
    # makefile found". Fail LOUDLY here instead. (build #1 masking postmortem, 2026-08-22)
    if [ ! -f Makefile ]; then
      echo "CONFIGURE FAILED: no top-level Makefile produced (configure aborted before generating it)"; exit 1
    fi

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
      "common/server_token_c.patch"
      "common/server_unicode_c.patch"

      # midi support
      "common/midi_support.patch"

      # sdl patch
      "common/dlls_winebus_sys_bus_sdl_c.patch"

      # FSYNC SYNC PORT (futex-based in-process sync onto 11.16's ntsync server).
      # The 4 orphaned bionic esync/fsync overlay patches (common/*_esync_c/*_fsync_c)
      # target a staging-created base that no longer exists -> DROPPED. Instead, the
      # sync/ set below creates fsync.c/h (ntdll + server) from the proton_11.0-2
      # lineage, wires the wineserver protocol (fsync_free_shm_idx + enum fsync_type,
      # regenerated via make_requests after the loop), and adds the do_fsync() client
      # dispatch + server object handling. esync is intentionally NOT ported (proton's
      # own esync is vestigial: it references an undefined create_esync request); fsync
      # is the backend that engages on kernels with futex_waitv. ntsync stays upstream.
      "sync/create_dlls_ntdll_unix_fsync_c.patch"   # ntdll client fsync backend (new file)
      "sync/create_dlls_ntdll_unix_fsync_h.patch"
      "sync/dlls_ntdll_unix_unix_private_h.patch"   # + fsync_apc_futex field on struct thread_data
      "sync/create_server_fsync_c.patch"            # wineserver fsync shm + handler (new file)
      "sync/create_server_fsync_h.patch"
      # ESYNC (eventfd, proven on-device) — 11.x-anchored inproc-integrated esync from
      # proton_11.0-2 (blob matches our overlay's bionic esync patches). Reuses the same
      # inproc_sync framework as fsync; the sync.c/inproc_sync.c/server integration patches
      # below now carry BOTH fsync + esync branches. Runtime prefers esync on this hardware.
      "sync/create_dlls_ntdll_unix_esync_c.patch"   # ntdll client esync backend (new file)
      "sync/create_dlls_ntdll_unix_esync_h.patch"
      "sync/create_server_esync_c.patch"            # wineserver esync shm (new file)
      "sync/create_server_esync_h.patch"
      "sync/server_protocol_def.patch"              # + fsync_free_shm_idx, enum fsync_type/esync_type, ESYNC_USED_BY_SERVER, sync_shm_idx
      "sync/dlls_ntdll_Makefile_in.patch"           # compile unix/fsync.c
      "sync/server_Makefile_in.patch"               # compile server/fsync.c
      "sync/dlls_ntdll_unix_sync_c.patch"           # do_fsync() dispatch in inproc_* + NtDelayExecution
      "sync/dlls_ntdll_unix_server_c.patch"         # init_first_thread: FSYNC_USED_BY_SERVER + fsync_init
      "sync/server_main_c.patch"                    # fsync_init() at server startup
      "sync/server_thread_c.patch"                  # init_first_thread + get_inproc_alert_fd fsync
      "sync/server_inproc_sync_c.patch"             # create/signal/reset/abandon fsync branches

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
      # bionic/arm64ec host-page-size fix for alloc_virtual_heap (from proton_11.0 delta + heap round)
      "common/dlls_ntdll_unix_virtual_bionic_c.patch"
      # restore Proton win32u display mode-emulation dropped by going vanilla (fixes desktop geometry)
      "common/dlls_win32u_sysparams_c.patch"
      # arm64ec x64 exception dispatch: extended-context (XSTATE) support - fixes SEH-heavy x64 games
      "common/dlls_ntdll_signal_arm64ec_xstate_c.patch"
      "common/dlls_ntdll_signal_x86_64_xstate_c.patch"
      # arm64ec: advertise x86 AVX xstate config (write via writable USD alias, not RO 0x7ffe0000)
      "common/dlls_ntdll_unix_virtual_xstate_config_c.patch"
      # DROPPED(FEX-cluster): "arm64ec/dlls_ntdll_unix_virtual_c.patch"

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
      # DROPPED(FEX-cluster): "arm64ec/dlls_ntdll_loader_c.patch"
      # DROPPED(vanilla): "arm64ec/dlls_ntdll_unix_loader_c.patch"
      "arm64ec/loader_wine_inf_in.patch"
      "test-bylaws/programs_services_services_c.patch"
      "test-bylaws/dlls_winecrt0_arm64ec_c.patch"

      # fix build
      "arm64ec/dlls_wdscore_wdscore_spec.patch"
      "arm64ec/programs_wineboot_wineboot_c.patch"

      # 1. Extended State (XSTATE/YMM) Support Patches
      # DEFERRED(FEX-AVX/YMM cluster, build#3 2026-08-22): unwind.h adds CONTEXT_ARM64_FEX_YMMSTATE
      #   uses (ctx_flags_*_to_* + YMM context_x64<->arm memcpy), but include_winnt_h.patch (the
      #   macro DEFINITION) does NOT apply to vanilla 11.16 -> SKIPPED -> undeclared-identifier
      #   compile error. Same kept-use/dropped-def coupling as the 10.6 FEX cluster. This is the
      #   AVX/YMM extended-context plumbing we're deferring anyway (only matters for x64 AVX-heavy
      #   SEH games); drop BOTH so the pair stays consistent (0 dangling CONTEXT_ARM64_FEX_YMMSTATE
      #   refs — verified nothing else in the applied set uses it). Re-anchor together with XSTATE
      #   hunk#2 when we take on x64-SEH-game compat post-boot.
      # DEFERRED: "test-bylaws/dlls_ntdll_unwind_h.patch"
      # DEFERRED: "test-bylaws/include_winnt_h.patch"

      # 2. Thread Suspension Patches
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_signal_arm64_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_signal_arm64ec_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_signal_x86_64_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_unix_debug_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_unix_signal_arm64_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_unix_signal_arm_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_unix_signal_i386_c.patch"
      # DROPPED(vanilla): "test-bylaws/dlls_ntdll_unix_unix_private_h.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_ntdll_spec.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_ntdll_misc_h.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_wow64_process_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_wow64_syscall_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_wow64_wow64_spec.patch"

      # 3. Process and Virtual Memory Management
      # DROPPED(FEX-cluster): "test-bylaws/dlls_wow64_virtual_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_unix_process_c.patch"

      # 4. Server and Threading Infrastructure
      # DROPPED(FEX-cluster): "test-bylaws/dlls_ntdll_unix_thread_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/server_process_c.patch"
      # DROPPED(vanilla): "test-bylaws/server_thread_h.patch"
      # DROPPED(FEX-cluster): "test-bylaws/server_thread_c.patch"
      # DROPPED(FEX-cluster): "test-bylaws/server_mapping_c.patch"

      # 5. Internal Headers
      # DROPPED(vanilla): "test-bylaws/include_winternl_h.patch"

      # 5a. FEX unixlib load-by-name (MemoryWineLoadUnixLibByName = 1002)
      # DROPPED(vanilla): "test-bylaws/include_wine_unixlib_h.patch"

      # 6. build vcruntime140_1 with aarch64
      "test-bylaws/dlls_vcruntime140_1_vcruntime140_1_spec.patch"

      # 7. Build System (Optional)
#      "test-bylaws/tools_makedep_c.patch"
    )

    # Vanilla wine-10.6 base: the proton_10.0 bionic patch set was authored against Valve's
    # proton_10.0 tree, so on pure upstream 10.6 some hunks land only with line offset/fuzz and a
    # number are already upstream (arm64ec was largely mainlined by 10.6). Apply tolerantly and
    # never fail the build on a single patch: git apply (exact) -> patch --fuzz=2 (offset/fuzz; fuzz=3 mis-placed inserts)
    # -> log SKIPPED. Load-bearing patches are verified separately in the build log.
    applied=0; fuzzed=0; skipped=0
    for patch in "${PATCHES[@]}"; do
      pf="./android/patches/$patch"
      if [ ! -f "$pf" ]; then echo "MISSING  $patch"; skipped=$((skipped+1)); continue; fi
      if git apply "$pf" 2>/dev/null; then
        # git apply is atomic (all-or-nothing) -> a clean exact apply, no partial state.
        echo "APPLIED  $patch"; applied=$((applied+1))
      elif patch -p1 --fuzz=2 --forward --dry-run <"$pf" >/dev/null 2>&1; then
        # Dry-run gate FIRST: only real-apply when every hunk succeeds, so a partially-applying
        # patch (some hunks land, some reject) never leaks a half-patched file into the build.
        patch -p1 --fuzz=2 --forward --no-backup-if-mismatch -r /dev/null <"$pf" >/dev/null 2>&1
        echo "FUZZED   $patch"; fuzzed=$((fuzzed+1))
      else
        echo "SKIPPED  $patch (does not apply cleanly to vanilla 11.16 - left untouched)"; skipped=$((skipped+1))
      fi
    done
    echo "----------------------------------------"
    echo "Patch summary: applied=$applied fuzzed=$fuzzed skipped=$skipped"

    # SYNC PORT: the fsync patches add a new wineserver protocol request
    # (fsync_free_shm_idx) + enum fsync_type to server/protocol.def. The build does
    # NOT auto-regenerate the protocol, so regenerate it explicitly from the patched
    # protocol.def. make_requests is a standalone perl script that rewrites
    # include/wine/server_protocol.h + server/request_handlers.h + server/trace.c
    # (appends the new request, bumps SERVER_PROTOCOL_VERSION). Idempotent; only runs
    # when the fsync request is actually present so a non-sync build is unaffected.
    if grep -q "fsync_free_shm_idx" server/protocol.def 2>/dev/null; then
      echo "Regenerating wineserver protocol (make_requests) after protocol.def fsync patch..."
      perl ./tools/make_requests || { echo "make_requests FAILED"; exit 1; }
    fi
  fi

  if [ "$arg" == "--build" ]
  then
    echo "Building..."
    rm -rf $OUTPUT_DIR/bin
    rm -rf $OUTPUT_DIR/lib
    rm -rf $OUTPUT_DIR/share
    rm -rf $install_dir
    make -j$(nproc) || { echo "BUILD FAILED (make=$?)"; exit 1; }
  fi

  if [ "$arg" == "--install" ]
  then
    echo "Installing..."
    mkdir -p $OUTPUT_DIR/bin
    mkdir -p $OUTPUT_DIR/lib
    mkdir -p $OUTPUT_DIR/share
    mkdir -p $install_dir
    make install -j$(nproc) || { echo "INSTALL FAILED (make install=$?)"; exit 1; }
    cp -r $install_dir/bin/wine* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/reg* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/msi* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/notepad $OUTPUT_DIR/bin
    cp -r $install_dir/lib/wine  $OUTPUT_DIR/lib
    cp -r $install_dir/share/wine  $OUTPUT_DIR/share

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

    # CRITICAL: the new-layout wine loader lives at lib/wine/aarch64-unix/{wine,wine-preloader};
    # bin/wine must be a symlink to it (all the bin/* tool stubs point at bin/wine). The proton_10.0
    # --install did NOT create these, so bin/wine was ABSENT -> every bin/* symlink dangled and the
    # app's `exec .../bin/wine` failed with ENOENT (looked like a boot crash but wine never ran).
    # Mirror the proton_11.0 --install and create them explicitly (idempotent).
    ln -sf ../lib/wine/aarch64-unix/wine           "$install_dir/bin/wine"
    ln -sf ../lib/wine/aarch64-unix/wine           "$OUTPUT_DIR/bin/wine"
    ln -sf ../lib/wine/aarch64-unix/wine-preloader "$install_dir/bin/wine-preloader"
    ln -sf ../lib/wine/aarch64-unix/wine-preloader "$OUTPUT_DIR/bin/wine-preloader"
    echo "Created bin/wine + bin/wine-preloader loader symlinks:"
    ls -la "$OUTPUT_DIR/bin/wine" "$OUTPUT_DIR/bin/wine-preloader"
  fi
done
