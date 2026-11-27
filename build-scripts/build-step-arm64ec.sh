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
export FONTCONFIG_LIBS="-L$deps/lib -lfontconfig -lfreetype -lexpat"
export X_CFLAGS="-I$deps/include/X11"
export X_LIBS="-landroid-sysvshm"
export GSTREAMER_CFLAGS="-I$deps/include/gstreamer-1.0 -I$deps/include/glib-2.0 -I$deps/lib/glib-2.0/include -I$deps/glib-2.0/include -I$deps/lib/gstreamer-1.0/include"
export GSTREAMER_LIBS="-L$deps/lib -lgstgl-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0"
export FFMPEG_CFLAGS="-I$deps/include"
export FFMPEG_LIBS="-L$deps/lib -lavutil -lavcodec -lavformat"

# Wayland driver deps (winewayland.drv). The bionic aarch64 libs+headers are vendored in
# android/wayland-deps and staged into $deps by the workflow. Like the other deps above we
# set *_CFLAGS/_LIBS explicitly so configure uses them directly instead of pkg-config (whose
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
    NTSYNC_TRIPLE=aarch64-linux-android
    if [ ! -f "$NTSYNC_DIR/Cargo.toml" ]; then
        echo "FATAL: ntsync-android not found at $NTSYNC_DIR"
        exit 1
    fi
    echo "Building libntsync_android.a ($NTSYNC_TRIPLE) from $NTSYNC_DIR @ $(git -C "$NTSYNC_DIR" rev-parse HEAD 2>/dev/null)"
    rustup target add "$NTSYNC_TRIPLE" || exit $?
    # Build from the crate root so its .cargo/config.toml applies (it only
    # matters for the cdylib; the static archive takes the page alignment of
    # ntdll.so/wineserver, i.e. this script's LDFLAGS).
    ( cd "$NTSYNC_DIR" && env CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$TOOLCHAIN/$TARGET-clang" AR="$TOOLCHAIN/llvm-ar" \
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

      # unixlib load-by-name (MemoryWineLoadUnixLibByName, FEX companion unixlib) is
      # upstream in Valve 981e2f0b9007 (fb596168268, fbadeed3c8c, c24a38ac3d1): the old
      # unix_private.h / wow64 virtual.c / unixlib.h / winternl.h patches are dropped;
      # loader.c keeps only the PREFIX search dir + the load_unixlib_by_name traces.
      # signal_x86_64.c "jmp *%rcx" is upstream too (e33ce1f2396): dropped.
	  
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
      # kernelbase: report AMD64 from GetNativeSystemInfo() in x64 processes (#94, upstream Wine c5a96a0b6c, Brendan Shanks)
      "dlls_kernelbase_memory.c_native_system_info.patch"
      # crypt32: CRYPT_E_NOT_FOUND for an empty enhanced key usage (#46, Cole McCarren)
      "dlls_crypt32_cert.c_enhkey_usage.patch"
      # winex11: Vulkan other-process rendering off by default on Android (#82, Dmage22 via Utkarsh Dalal;
      # WINE_DISABLE_VULKAN_OPWR=0 turns it back on)
      "dlls_winex11.drv_init.c_opwr.patch"
      # winex11: drop WGL_CONTEXT_OPENGL_NO_ERROR_ARB when GLX lacks it (#66, Utkarsh Dalal)
      "dlls_winex11.drv_opengl.c_no_error.patch"
      # ntdll: release the arm64ec emulator cpu area from its view base (#42, David Luo)
      "dlls_ntdll_unix_virtual.c_emu_stack_release.patch"

      # v17 (2026-11-24)
      # ws2_32: DiRT 3 / DiRT Showdown keep calling sendto() on their closed telemetry socket; report 0 bytes
      # sent instead of WSAENOTSOCK (error 10038) for those executables only (from our DroidDeck-Proton
      # 11-12.x layers; same rule as DiRTTelemetryErrorFix by RoccoC, MIT, after Carsten Gottschling)
      "dlls_ws2_32_socket.c_dirt_telemetry.patch"
      # winex11: needs_offscreen_rendering at most every WINE_OFFSCREEN_CHECK_MS ms on the present path;
      # unset or 0 = every call (proton-wine ff9db6f6 + bab12909, the file our DroidDeck-Proton 11-11.9+
      # layers carry; opt-in)
      "dlls_winex11.drv_init.c_offscreen_check_cache.patch"
      # winegstreamer: skip the Proton audio converter when mediaconv is off (symmetric to the video-converter
      # skip) so native avdec_wmav2 decodes WMA cutscene audio (our 8956410a, feat/fmv-audio-converter-skip)
      "dlls_winegstreamer_wg_parser.c.patch"
      # From GameNative/proton-wine (proton_11.0-2), credited per patch:
      # ntdll: throttle Sleep(0) / SwitchToThread storms (DOOM 2016 idle worker, Cult of the Lamb dispatch thread);
      # WINE_SLEEP0_THROTTLE=0 disables it (#119, Utkarsh Dalal; thread-data hunk re-anchored on next_killed)
      "dlls_ntdll_unix_sync_c_sleep0_yield_throttle.patch"
      # ntdll, server: fail would-block receives on nonblocking sockets without a server round trip (DOOM 2016
      # DTLS poll thread); WINE_SOCKET_POLL_FASTPATH=0 disables it (#119, Utkarsh Dalal)
      "dlls_ntdll_unix_socket_c_recv_fastpath.patch"
      # ntdll: userspace ntsync wait-any accepts the same object more than once, like the kernel driver (OMORI
      # spin; #128, Utkarsh Dalal; ported onto our ntsync_userspace.patch, must follow it)
      "dlls_ntdll_unix_sync.c_ntsync_wait_any_duplicates.patch"
      # ntdll: a system call writing into an RWX page the emulator write-protected for self-modifying-code
      # tracking succeeds, as on Windows (GameNative #131 + #133, Utkarsh Dalal; Hogwarts Legacy)
      "dlls_ntdll_unix_virtual_c_emulated_code_write.patch"
      # ...and is reported to the emulator as cross-process memory-write work; that work is applied inside a
      # syscall callback so the emulator's own allocations while it invalidates code are not reported back
      # (GameNative #133, Utkarsh Dalal). arm64ec only.
      "dlls_ntdll_unix_virtual_c_report_emulated_code_write.patch"
      "dlls_ntdll_signal_arm64ec_c_cross_process_work_callback.patch"
      # ntdll: gated emulator notifications for protected launchers (Battle.net): non-image section views are
      # reported only to an emulator exporting BTCpu64NotifyNonImageViews (GameNative's FEX), and EFlags PF/AF
      # ride in cpsr bits 26/27, announced by __wine_arm64ec_extended_eflags (#67, Utkarsh Dalal, fixes by
      # Dmage22). Its sechost part is not taken: sechost already imports rpcrt4 directly here. arm64ec only.
      "dlls_ntdll_ntdll.spec_arm64ec_extended_eflags.patch"
      "dlls_ntdll_signal_arm64ec.c_emulator_notifications.patch"
      "dlls_ntdll_unwind.h_emulator_notifications.patch"
      # ntdll: fill the ARM64EC dispatch pointers in a second image mapping of a loaded hybrid DLL (Call of Duty:
      # Black Ops III / Arxan maps gdi32 again and calls into the copy; DroidDeck-Proton #34, FryMcNugget,
      # patch by McDonaldsVibecoder). arm64ec only.
      "dlls_ntdll_unix_virtual.c_arm64ec_hybrid_pointers_image_copies.patch"
      # ntdll: pthread_exit_wrapper() closes alert_fd only when it is an fd; userspace ntsync caches its alert
      # object index there and every thread exit closed an unrelated fd (Android fdsan double-close aborts).
      # The fsync index is not cached there (Valve 707bb01fbfc, Paul Gofman, in the base). Must follow ntsync_userspace.patch.
      "dlls_ntdll_unix_thread.c_alert_fd_close.patch"
      # ntdll: the Sleep(0) throttle starts a new run after > 500 us of own CPU work, or two > 50 us work gaps < 2 ms apart, between
      # yields (frame loops faster than 2 ms per frame were capped: AIO DirectDraw 2785 -> 1460 fps).
      "dlls_ntdll_unix_sync.c_sleep0_work_reset.patch"

      # DroidDeck-Proton fixes carried by the 11.0-12.8 layer (DroidDeck-Proton 11-11.x .. 11-12.8 and main)
      # 11-11.9 (2026-10-30): DroidDeck-Proton 11-11.3..11-11.9 Wine fixes (Droid-Deck/DroidDeck-Proton
      # f7d00335 patches/wine-hotfixes/pending, unchanged) + the proton-wine banner-proton/11.11 winex11 commit.
      # server: give the user ownership of files on Android shared storage (FUSE/sdcardfs owner is the
      # media provider; Blizzard Agent error 2123) (DroidDeck-Proton #4, maxjivi05)
      "server_file.c_android_shared_storage_owner.patch"
      # ntdll: recover a lost wake in the i386 RtlWaitOnAddress() under FEX (Battle.net UI freeze) (#5, maxjivi05)
      "dlls_ntdll_sync.c_i386_lost_wake.patch"
      # ntdll: don't report Wine's own installed files as read-only (MapleStory) (#6, maxjivi05)
      "dlls_ntdll_unix_file.c_wine_files_not_readonly.patch"
      # ntdll: treat Android's case-folding shared storage as case-insensitive (no per-lookup directory scan) (#7, maxjivi05)
      "dlls_ntdll_unix_file.c_android_case_insensitive.patch"
      # ntdll: World of Warcraft Forever ARM64EC syscall dispatch, NLS and map-time fixes (DroidDeck-Proton #3,
      # arusiasotto). arm64ec only: its start_thread hunk calls an __aarch64__-only function.
      "dlls_ntdll_wow_forever_arm64ec.patch"
      # winebuild, ntdll: ARM64X import thunks get the x86_64 hotpatch prologue, so x86-64 hooks on system DLL
      # exports land and ARM64EC callers still skip the emulator (11-12.2, maxjivi05). arm64ec only.
      "dlls_ntdll_signal_arm64ec.c_arm64x_import_thunk_hotpatch.patch"
      # 11-12.8 (2026-11-16): DroidDeck-Proton 11-12.4..11-12.8 Wine fixes plus one from its main after 11-12.8
      # (Droid-Deck/DroidDeck-Proton 9956783f + d03ba52e patches).
      # server: directories grant Users read access, as on Windows; Steam's DRM helper looks for it
      # (11-12.4, xXJSONDeruloXx)
      "server_file.c_directory_users_ace.patch"
      # ntdll: no extended attribute queries on a filesystem that reported ENOTSUP once (11-12.6, maxjivi05;
      # context rebased)
      "dlls_ntdll_unix_file.c_xattr_less_filesystems.patch"
      # ntdll: no directory listing for a missing file on a case-folding volume unless the name can be a hashed
      # short name (11-12.7, maxjivi05)
      "dlls_ntdll_unix_file.c_case_folding_missing_file.patch"
      # ntdll: RtlWaitOnAddress() queue entries from a static pool, so a thread killed in a wait leaves no
      # dangling stack entry (Forza Horizon 6 loading stall; main d03ba52e, l4nr3; one line ported)
      "dlls_ntdll_sync.c_futex_entry_pool.patch"
      # ntdll: the ARM64EC process breakpoint handler steps over the brk at Rip, not at the ARM64 Pc offset
      # (MGS V Ground Zeroes splash loop; 11-12.4, xXJSONDeruloXx). arm64ec only.
      "dlls_ntdll_signal_arm64ec.c_process_breakpoint_rip.patch"
      # ntdll: NtGetContextThread on a NULL context returns STATUS_ACCESS_VIOLATION on ARM64EC (Crash Bandicoot
      # N. Sane Trilogy; main 92b57e73, 66sam). arm64ec only.
      "dlls_ntdll_signal_arm64ec.c_get_context_null.patch"
      # ntdll: RtlWow64GetCpuAreaInfo() looks up the context length without a trace line; since 03487a0dda3
      # every RtlWow64GetCurrentMachine() wrote one with +seh (Counter-Strike 60 -> 9 fps with BL Wine debug on)
      "dlls_ntdll_process.c_cpu_area_no_trace.patch"
      # GameNative fixes (2026-11-26, see the Source line in each patch; LGPL-2.1, credit kept)
      # group B, GameNative AI-help PRs (butilly): #144 DNS canonname, #117 registry flush tmp,
      # #63 WMA audio-converter skip, #107 icu backend check, #100 IsBorderRequired, #72 stale x64
      # .sys, #88 gamma pass on GLSL 1.40 (#139 wmic is a configure.ac commit)
      "dlls_ws2_32_unixlib.c_gn144_dns_canonname.patch"
      "dlls_ntdll_unix_registry.c_gn117_flush_unique_tmp.patch"
      "dlls_winegstreamer_unixlib.c_gn63_audio_transcode_skip.patch"
      "dlls_icu_main.c_gn107_backend_check.patch"
      "dlls_wintypes_main.c_gn100_border_required.patch"
      "dlls_ntdll_loader.c_gn72_stale_x64_drivers.patch"
      "dlls_opengl32_unix_wgl.c_gn88_gamma_glsl140.patch"
      # GameNative #86: ProcessorInformation power query cached for 500 ms (Wine bug 60462)
      "dlls_ntdll_unix_system_c_power_cache.patch"
      # GameNative #85 ntdll side: arm64x_check_call follows the -import fast-forward thunk (winebuild
      # side = TOOLS_PATCHES in build-step0.sh; Wine bug 60463). Adapted next to our .Lhotpatch check.
      "dlls_ntdll_signal_arm64ec_c_ffwd_import.patch"
      # GameNative #86: NtContinue to x64 code resumes in the emulator without a syscall. Adapted.
      "dlls_ntdll_signal_arm64ec_c_fast_continue.patch"
      # GameNative #122 (part): native ARM64 unwind stops when it makes no progress. The WoW Forever
      # ntdll gaps of #122 are already covered by dlls_ntdll_wow_forever_arm64ec.patch (same report).
      "dlls_ntdll_signal_arm64_c_no_unwind_progress.patch"
      # GameNative Android clipboard (user32 side, app socket 127.0.0.1:20000 = BL WineRequestHandler)
      # and X11 pointer clip re-grab / grab on the foreground window (#82)
      "dlls_user32_clipboard_c.patch"
      "dlls_winex11_drv_mouse_c_clip_regrab.patch"
      "dlls_winex11_drv_mouse_c_clip_grab_foreground.patch"
      # GameNative #122 Battle.net-style launcher GPU drawing (other process Vulkan readback), OFF by default:
      # WINE_DISABLE_VULKAN_OPWR=0 turns it on (user decision 2026-11-26). Both adapted.
      "dlls_win32u_vulkan_c_other_process_readback.patch"
      "dlls_winex11.drv_init.c_opwr_readback.patch"
      # V8 sandbox (Chrome / CEF 128+, EA app 13.836): report a 41-bit user address space to Chromium
      # child processes, Android gives 64-bit processes only 39 bits and V8 crashes below 1 TB
      "dlls_ntdll_unix_virtual_c_v8_sandbox_address_space.patch"
      # Wayland: frames a process draws into another process's window (Chromium/CEF GPU process ->
      # browser window: EA app, Windows Steam) are read back and shown by the window's owner
      "dlls_winewayland_drv_other_process_frames.patch"
      # GameNative #133 extended to writable image sections (Denuvo code in an RWX exe section, NFS Heat)
      "dlls_ntdll_unix_virtual_c_emulated_code_write_image.patch"
      # EA app 13.836 Steam/Epic launchers cut launch env values to 1024 chars under Wine (EA token
      # for Origin-era games, NFS Most Wanted OOA activation): hide wine_get_* from those launchers
      "dlls_ntdll_loader_c_hide_wine_exports_ea_launchers.patch"
      # Android: fixed MAC for interfaces Android randomizes (per-network Wi-Fi MAC, per-boot dummy0), per
      # interface name + prefix; EA OOA licences (NFS Most Wanted) went stale on every network change
      "dlls_nsiproxy.sys_ndis.c_stable_mac.patch"
      # A service's CreateProcessAsUser for the user (GOG Galaxy /deelevated) asks for WinSta0\\Default explicitly:
      # map it to the prefix's configured desktop (Bannerlator Wayland: "shell") so it gets a display driver
      "dlls_win32u_winstation_c_default_desktop_alias.patch"
      # GOG Galaxy: QtWebEngine software compositing, set inside GalaxyClient.exe (its service relaunch drops
      # outside env); its D3D11 texture sharing by NT handle fails under DXVK on Android -> all-white window
      "dlls_ntdll_loader_c_app_environment_hacks.patch"
      # Electron main processes (resources\\app.asar next to the exe, e.g. Vortex) run V8 too: give them the
      # same 41-bit address space report as Chromium children (EXCEPTION_BREAKPOINT at start otherwise)
      "dlls_ntdll_unix_virtual_c_v8_electron_main.patch"
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
      "dlls/sechost/Makefile.in|IMPORTS   = kernelbase rpcrt4|sechost imports rpcrt4 directly (no variadic exit thunk; DOOM 2016 GOG / EA app service crash)"
      "dlls/advapi32/Makefile.in|IMPORTS   = kernelbase sechost msvcrt rpcrt4|advapi32 imports rpcrt4 directly (no variadic exit thunk)"
      "dlls/ntdll/unix/virtual.c|force_anon|noexec/force_anon SD-card boot (Dragon Age)"
      "dlls/shell32/shlfileop.c|dir_len|drive-root FO_COPY guard"
      "dlls/ntdll/unix/env.c|C.UTF-8|LC_ALL=C.UTF-8 bionic locale bring-up"
      "dlls/winedirectaudio.drv/directaudio.c|BANNER_AUDIO_DIRECT_MIC|DirectAudio driver is the v1.3.2 build (mic capture)"
      "dlls/xinput1_3/main.c|transient wait failure in the update thread|xinput WAIT_FAILED retry (controller-dies fix)"
      "dlls/winepulse.drv/pulse.c|Failed to create timer event, timer callbacks disabled|winepulse rttime_new guard (re-ported onto the 46b29104 period timer)"
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
      "dlls/kernelbase/memory.c|if (!is_wow64 || native_machine != IMAGE_FILE_MACHINE_AMD64)|GetNativeSystemInfo AMD64 in x64 processes (Wine c5a96a0b6c)"
      "dlls/ntdll/unix/virtual.c|void *base = (char *)teb->ChpeV2CpuAreaInfo - kernel_stack_guard_size;|arm64ec emulator cpu area released (GameNative #42)"

      "dlls/amd_ags_x64/unixlib.c|__ANDROID__|AGS unixlib Android guard (builtin AGS on bionic)"
      "dlls/amd_ags_x64/unixlib.c|STATUS_NOT_IMPLEMENTED|AGS Android unix_call table populated (not empty)"
      # GE-Proton 11.0-14 (proton_11.14-GE): Valve a47c73f52fd7 merge + GE-Proton11-14 picks
      "dlls/ntdll/unix/sync.c|if (do_fsync()) fd = reply->sync_shm_idx;|fsync shm index kept out of alert_fd (Valve 707bb01fbfc, android hunk re-anchored)"
      "dlls/winegstreamer/wg_transform.c|set_sample_flags_from_buffer(sample, buffer, total_size, src_video_info)|MF cutscene frame-interval fix kept across Valve h264 edge replication (48a3542ec3b)"
      "dlls/xinput1_3/main.c|RegNotifyChangeKeyValue(deviface_key|xinput rescans controllers only when DeviceClasses changes (Valve 575131f3)"
      "dlls/winebus.sys/main.c|overlong report %#x length|winebus truncates overlong input reports (Valve 674d3cdd)"
      "dlls/ntdll/sync.c|if (sem) NtReleaseSemaphore( sem, 1, NULL );|RtlpUnWaitCriticalSection ignores semaphore errors (Valve d5c4b850)"
      "programs/cmd/builtins.c|Console handles are synchronous, so the overlapped read below would block|cmd CHOICE /T times out on a console (GE 11-14)"
      "dlls/win32u/vulkan.c|sort_surface_formats2( formats, *format_count )|B8G8R8A8_UNORM reported as the first surface format (GE 11-14)"
      "dlls/winewayland.drv/wayland_pointer.c|cursor_animation_once|winewayland animated cursors (GE 11-14)"
      "dlls/ntdll/unix/thread.c|queue_killed_thread|TEB and stacks of killed threads freed (GE 11-14, Helldivers 2)"
      "dlls/ntdll/heap.c|InterlockedIncrement( &bin->group_freed );|LFH reuses free blocks of partially used groups (GE 11-14, Wine MR 12472)"
      "programs/lsass/main.c|SamSs|lsass.exe SamSs service stub (GE 11-14 ACE)"
      "programs/wineboot/wineboot.c|start_samss_service|wineboot starts SamSs (GE 11-14 ACE)"
      # v17
      "dlls/ws2_32/socket.c|ignore_sendto_on_closed_socket|DiRT 3 / DiRT Showdown telemetry sendto on a closed socket (v17)"
      "dlls/winex11.drv/init.c|WINE_OFFSCREEN_CHECK_MS|winex11 offscreen check cache (opt-in, v17)"
      "dlls/winegstreamer/wg_parser.c|Proton audio converter with rate fixup|WMA cutscene audio: Proton audio converter skipped when mediaconv is off (v17)"
      "dlls/ntdll/unix/sync.c|WINE_SLEEP0_THROTTLE|Sleep(0) / SwitchToThread storm throttle (GameNative #119)"
      "dlls/ntdll/unix/sync.c|sleep0_work_last|Sleep(0) throttle frame-loop work reset (two work gaps < 2 ms apart)"
      "dlls/ntdll/unix/sync.c|WINE_SLEEP0_THROTTLE_SKIP|Sleep(0) throttle off per graphics API (default DirectX 7)"
      "dlls/ntdll/unix/virtual.c|sleep0_note_image( nt_name );|image map notes graphics DLLs for the per-API throttle switch"
      "dlls/ntdll/unix/socket.c|WINE_SOCKET_POLL_FASTPATH|would-block receive fast path on nonblocking sockets (GameNative #119)"
      "dlls/ntdll/unix/sync.c|first_index[distinct++] = i;|userspace ntsync wait-any with a repeated object (GameNative #128)"
      "dlls/ntdll/unix/virtual.c|BOOL virtual_handle_emulated_code_write( void *addr )|system call writes into emulator write-protected RWX pages succeed (GameNative #131/#133)"
      "dlls/ntdll/unix/virtual.c|static void queue_emulated_code_write( void *addr, SIZE_T size )|those writes reported to the emulator (GameNative #133)"
      "dlls/ntdll/signal_arm64ec.c|callback = enter_syscall_callback();|cross-process emulator work applied inside a syscall callback (GameNative #133)"
      "dlls/ntdll/signal_arm64ec.c|pBTCpu64NotifyNonImageViews = RtlFindExportedRoutineByName|non-image view notifications gated on the emulator (GameNative #67)"
      "dlls/ntdll/ntdll.spec|__wine_arm64ec_extended_eflags|EFlags PF/AF in cpsr bits 26/27 announced to the emulator (GameNative #67)"
      "dlls/ntdll/unwind.h|parity and aux carry have no AArch64 equivalent|EFlags PF/AF carried through cpsr bits 26/27 (GameNative #67)"
      "dlls/ntdll/unix/virtual.c|copy_arm64ec_hybrid_pointers( view, nt, dir, image_info->map_addr );|ARM64EC dispatch pointers in second image mappings (DroidDeck-Proton #34, Black Ops III)"
      "dlls/ntdll/unix/virtual.c|ULONG_PTR limit = max( (ULONG_PTR)address_space_limit, (ULONG_PTR)1 << 47 );|ARM64EC code map sized for 47-bit addresses on a 39-bit host (v17, d4c9ffa7)"
      "dlls/ntdll/signal_arm64ec.c|unwind made no progress at pc|ARM64EC exception search / unwind stops on a frame that does not move (v17, fecf7376)"
      "dlls/ntdll/signal_arm64ec.c|if (!is_valid_arm64ec_frame( new_context.Sp ))|ARM64EC unwind checks the stack pointer before each step (v17, a188791d)"
      "dlls/ntdll/unix/thread.c|if (inproc_alert_fd_is_fd()) close( ntdll_get_thread_data()->alert_fd );|thread exit closes alert_fd only when it is an fd (userspace ntsync index, fdsan double-close)"
      "dlls/ntdll/unix/sync.c|if (do_fsync()) fd = reply->sync_shm_idx;|fsync alert index not cached in alert_fd (Valve 707bb01fbfc, in the base)"

      "server/file.c|security_unix_uid_to_sid( file_sd_owner( unix_fd, &st ) )|Android shared storage files owned by the user (DroidDeck-Proton #4)"
      "dlls/ntdll/sync.c|static NTSTATUS wait_for_wake( struct futex_entry *entry, const LARGE_INTEGER *timeout )|i386 RtlWaitOnAddress lost-wake recovery (DroidDeck-Proton #5)"
      "dlls/ntdll/unix/file.c|is_wine_install_fd( fd )|Wine's own files not read-only (DroidDeck-Proton #6)"
      "dlls/ntdll/unix/file.c|else if (dir_folds_case( root_fd, dir, fd ))|Android shared storage case-insensitive (DroidDeck-Proton #7)"
      "include/wine/asm.h|ldr x16, [x18, #0x370]|WoW Forever ARM64EC syscall stubs read the TEB syscall table (DroidDeck-Proton #3)"
      "dlls/ntdll/unix/syscall.c|ntdll_arm64ec_sync_syscall_dispatch|WoW Forever ARM64EC KSDT dispatcher sync (DroidDeck-Proton #3)"
      "dlls/ntdll/unix/env.c|map_nls_file_as_view|WoW Forever NLS section fallback (DroidDeck-Proton #3)"
      "dlls/ntdll/unix/virtual.c|map_image_init_ntdll_dispatchers|WoW Forever mapped-ntdll dispatcher init (DroidDeck-Proton #3)"
      "dlls/ntdll/signal_arm64ec.c|.Lhotpatch_seq|ARM64X import thunks with the x86_64 hotpatch prologue (DroidDeck-Proton 11-12.2, maxjivi05)"
      "server/file.c|Windows grants Users read access to system directories|directories grant Users read access (DroidDeck-Proton 11-12.4, xXJSONDeruloXx)"
      "dlls/ntdll/unix/file.c|if (xattr_less( st )) return ret;|no xattr queries on filesystems without them (DroidDeck-Proton 11-12.6, maxjivi05)"
      "dlls/ntdll/unix/file.c|static BOOLEAN is_hashed_short_name( const WCHAR *name, int length )|no directory listing for a missing file on case-folding volumes (DroidDeck-Proton 11-12.7, maxjivi05)"
      "dlls/ntdll/sync.c|static struct futex_entry futex_entries[4096];|RtlWaitOnAddress entries off the waiter's stack (DroidDeck-Proton main d03ba52e, l4nr3)"
      "dlls/ntdll/process.c|get_extended_context_length( data[i].flag, &ext_size, ~(ULONG64)0 )|RtlWow64GetCpuAreaInfo context length without trace (+seh log flood)"
      "dlls/ws2_32/unixlib.c|dst->ai_canonname = NULL;|ws2_32 DNS canonname cleared (GameNative #144)"
      "dlls/ntdll/unix/registry.c|static const char temp_fn[] = |registry flush unique tmp file (GameNative #117)"
      "dlls/winegstreamer/unixlib.c|Ignoring protonaudioconverterbin without an audio dump database|audio-converter skip without dump db (GameNative #63)"
      "dlls/icu/main.c|BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )|icu backend check (GameNative #107)"
      "dlls/wintypes/main.c|Graphics capture border control is not implemented|IsBorderRequired absent (GameNative #100)"
      "dlls/ntdll/loader.c|if (NtReadFile( file, 0, NULL, NULL, &io, signature, sizeof(signature), &offset, NULL )) return FALSE;|stale x64 builtin drivers (GameNative #72)"
      "dlls/win32u/opengl.c|USE_GL_FUNC(glBindFragDataLocation)|gamma pass on GLSL 1.40 (GameNative #88)"
      "dlls/ntdll/unix/system.c|if (cached_power && cached_cpus == out_cpus && now - cached_time < 500)|power info cache (GameNative #86)"
      "dlls/user32/clipboard.c|static int send_android_clipboard_data(android_clipboard_data_t clipboard_data) {|Android clipboard user32 side (GameNative)"
      "dlls/winex11.drv/mouse.c|if (data->clipping_cursor && clipping_cursor && EqualRect( clip, &clip_rect )) return TRUE;|X11 clip re-grab skip (GameNative #82)"
      "dlls/winex11.drv/mouse.c|if (fg_data->whole_window && fg_data->display == data->display) grab_window = fg_data->whole_window;|X11 grab on foreground window (GameNative #82)"
      "configure.ac|enable_wmic=|wmic built for ARM64EC (GameNative #139)"
      "dlls/win32u/vulkan.c|static BOOL readback_wanted( struct surface *surface )|other process Vulkan readback (GameNative #122, opt-in)"
      "dlls/win32u/vulkan.c|winewayland hands the read-back|readback on by default (winex11 still refuses other process surfaces)"
      "dlls/winex11.drv/init.c|static void android_opwr_position( HWND hwnd, UINT dpi, int *x, int *y )|other process surfaces kept behind their window (GameNative #122)"
      "dlls/winex11.drv/init.c|disable = !e || atoi( e );|other process rendering off by default on Android"
      "dlls/ntdll/signal_arm64ec.c|Lffwd_imp|arm64x_check_call follows -import fast-forward thunks (GameNative #85)"
      "dlls/ntdll/signal_arm64ec.c|if (!alertable && can_continue_in_emulator( context, (ULONG_PTR)__builtin_frame_address(0) ))|fast NtContinue in the emulator (GameNative #86)"
      "dlls/ntdll/signal_arm64.c|no unwind progress at pc|native ARM64 unwind progress guard (GameNative #122)"
      "dlls/ntdll/signal_arm64ec.c|ldr x4, [x2, #0xf8]|ARM64EC process breakpoint stepped at Rip (DroidDeck-Proton 11-12.4, xXJSONDeruloXx)"
      "dlls/ntdll/signal_arm64ec.c|arm_ctx = (ARM64_NT_CONTEXT){ .ContextFlags = ctx_flags_x64_to_arm( context->ContextFlags ) };|ARM64EC NtGetContextThread NULL context (DroidDeck-Proton main 92b57e73, 66sam)"
      "dlls/ntdll/unix/virtual.c|copy_arm64ec_hybrid_pointers( view, nt, dir, image_info->map_addr );|ARM64EC dispatch pointers in second mappings of a loaded DLL (DroidDeck-Proton #34, Black Ops III)"
      "dlls/ntdll/unix/virtual.c|static BOOL android_large_address_report(void)|V8 sandbox address space report for Chromium children (EA app 13.836)"
      "dlls/ntdll/unix/system.c|virtual_get_reported_system_info( &sbi, FALSE );|reported system info used for SystemBasicInformation"
      "dlls/winewayland.drv/wayland_surface.c|BOOL wayland_xproc_send(HWND hwnd, HDC hdc)|Wayland: other process frames sent to the window owner"
      "dlls/ntdll/loader.c|static BOOL hide_wine_exports_from_app(void)|ntdll hides wine_get_* from EA Steam/Epic launchers (EA token cut to 1024)"
      "dlls/nsiproxy.sys/ndis.c|static void stabilize_phys_addr( const char *name, IF_PHYSICAL_ADDRESS *phys_addr )|nsiproxy fixed MAC for Android random-MAC interfaces (EA OOA licence)"
      "dlls/win32u/winstation.c|if (desktop && !wcsicmp( desktop, defaultW ) && (!winstation|explicit WinSta0 Default desktop follows the configured desktop (GOG Galaxy)"
      "dlls/ntdll/loader.c|static void apply_app_environment_hacks(void)|per-app environment hacks (GOG Galaxy QtWebEngine software compositing)"
      "dlls/ntdll/unix/virtual.c|static BOOL main_exe_is_electron(void)|V8 sandbox address space report for Electron main processes (Vortex)"
      "dlls/winewayland.drv/wayland_surface.c|void wayland_xproc_frame(HWND hwnd, WPARAM wp, LPARAM lp)|Wayland: owner shows other process frames in a subsurface"
      "dlls/winewayland.drv/window.c|case WM_WAYLAND_XPROC_FRAME:|Wayland: WM_WAYLAND_XPROC_FRAME handled"
      "dlls/ntdll/unix/virtual.c|static BOOL is_writable_image_section( const struct file_view *view, const char *page )|system call writes into emulator-protected writable image sections (NFS Heat)"
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

    # A layer without amd_ags_x64.dll leaves every game that links AGS on its own bundled copy,
    # which on a non-AMD GPU reports no display at all - and with it no HDR10. The module is
    # built as x64 code under arm64ec (#pragma makedep arm64ec_x64), so it lands in the aarch64
    # PE dir like the rest. Fail here rather than ship a layer that silently lost it again.
    if ! ls "$OUTPUT_DIR"/lib/wine/*/amd_ags_x64.dll >/dev/null 2>&1; then
      echo "FATAL: amd_ags_x64.dll is not in the built layer (the builtin AGS did not build)" >&2
      exit 1
    fi
    echo "amd_ags_x64.dll present: $(ls "$OUTPUT_DIR"/lib/wine/*/amd_ags_x64.dll)"

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
	# symlinking wine binaries to $install_dir/bin
    ln -sf ../lib/wine/aarch64-unix/wine "$install_dir/bin/wine"
    ln -sf ../lib/wine/aarch64-unix/wine "$OUTPUT_DIR/bin/wine"
    ln -sf ../lib/wine/aarch64-unix/wine-preloader "$OUTPUT_DIR/bin/wine-preloader"
    ln -sf ../lib/wine/aarch64-unix/wine-preloader "$install_dir/bin/wine-preloader"
    echo "Wine loader symlinks:"
    ls -la "$OUTPUT_DIR/bin/wine" "$OUTPUT_DIR/bin/wine-preloader"
  fi
done
