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
      --without-wayland \
      --without-xcomposite \
      --without-xfixes \
      --without-xinerama \
      --with-xrandr \
      --with-xrender \
      --without-xshape \
      --with-xshm \
      --without-xxf86vm

    echo "Applying patches..."

    PATCHES=(
      # core patches
      "dlls_advapi32_advapi.c.patch"
      "dlls_amd_ags_x64_unixlib.c.patch"

      # dns
      "dlls_dnsapi_libresolv.c.patch"
      "dlls_dnsapi_record.c.patch"

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

      # unixlib load-by-name (MemoryWineLoadUnixLibByName) for FEX companion unixlib
      "dlls_ntdll_unix_unix_private.h.patch"
      "dlls_wow64_virtual.c.patch"
      "include_wine_unixlib.h.patch"
      "include_winternl.h.patch"
	  "dlls_ntdll_unix_signal_x86_64.c.patch"
	  
	  # opengl32
	  "dlls_opengl32_unix_wgl.c.patch"

      # shell32: guard bare drive-root (D:\ / D:) FO_COPY between roots
      "dlls_shell32_shlfileop.c.patch"

      # winegstreamer: skip the Proton audio converter when mediaconv is off
      # (symmetric to the video-converter skip) so native avdec_wmav2 decodes WMA
      "dlls_winegstreamer_wg_parser.c.patch"

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
    )

    for patch in "${PATCHES[@]}"; do
      echo "----------------------------------------"
      echo "Applying: $patch"

      if git apply --check "./android/patches/$patch" 2>/dev/null; then
        if git apply "./android/patches/$patch"; then
          echo "SUCCESS: $patch applied"
        else
          echo "FAILED: error applying $patch"
        fi
      else
        echo "SKIPPED: $patch does not apply cleanly"
      fi
    done

    echo "----------------------------------------"
    echo "Done applying patches."

    # ---------------------------------------------------------------------
    # HARD post-apply verification.
    #
    # The apply loop above is fail-SOFT: a patch whose context has drifted is
    # reported as "SKIPPED" and the build continues GREEN. That is how a
    # critical Android fix can silently no-op (e.g. GE-11.0-5 shipped without
    # the noexec/force_anon fix). git-apply success is ALSO not proof for a
    # graft that lives inside a larger multi-hunk patch, since a single hunk
    # can fuzz away while the file still "applies".
    #
    # So we grep the ACTUAL post-apply source for a code token unique to each
    # of the three Android fixes and abort the build if any is missing.
    # ---------------------------------------------------------------------
    echo "Verifying Android bug-fixes actually landed in the tree..."
    verify_fail=0

    # Fix #1: noexec / force_anon (Dragon Age SD-card boot). Grafted into
    # dlls_ntdll_unix_virtual.c.patch; the symbol only exists once applied.
    if ! grep -q 'force_anon' dlls/ntdll/unix/virtual.c; then
      echo "FATAL: force_anon not present in dlls/ntdll/unix/virtual.c (Fix #1 noexec/force_anon did NOT apply)"
      verify_fail=1
    fi

    # Fix #2: shell32 bare drive-root FO_COPY guard. 'dir_len' is a code token
    # introduced only by dlls_shell32_shlfileop.c.patch.
    if ! grep -q 'dir_len' dlls/shell32/shlfileop.c; then
      echo "FATAL: dir_len guard not present in dlls/shell32/shlfileop.c (Fix #2 drive-root copy guard did NOT apply)"
      verify_fail=1
    fi

    # Fix #3: LC_ALL=C.UTF-8 bionic locale default.
    if ! grep -q '"C.UTF-8"' dlls/ntdll/unix/env.c; then
      echo "FATAL: LC_ALL=C.UTF-8 default not present in dlls/ntdll/unix/env.c (Fix #3 locale bring-up did NOT apply)"
      verify_fail=1
    fi

    # DirectAudio v1.3.1: the vendored driver source must be the 1.3.1 build.
    # BANNER_AUDIO_DIRECT_RUNTIME (live in-game config mailbox) exists only in
    # the >=1.3 driver; the old v1 driver we used to ship lacks it.
    if ! grep -q 'BANNER_AUDIO_DIRECT_RUNTIME' dlls/winedirectaudio.drv/directaudio.c; then
      echo "FATAL: BANNER_AUDIO_DIRECT_RUNTIME not present in dlls/winedirectaudio.drv/directaudio.c (DirectAudio is NOT the v1.3.1 build)"
      verify_fail=1
    fi

    # winegstreamer FMV: the symmetric audio-converter skip must be present so
    # WMA cutscene audio routes to native avdec_wmav2 instead of the failing
    # Proton audio converter (whole pipeline would otherwise fail -> skipped FMV).
    if ! grep -q 'Proton audio converter with rate fixup' dlls/winegstreamer/wg_parser.c; then
      echo "FATAL: audio-converter skip not present in dlls/winegstreamer/wg_parser.c (FMV audio fix did NOT apply)"
      verify_fail=1
    fi

    if [ "$verify_fail" != "0" ]; then
      echo "FATAL: one or more Android bug-fixes failed to apply; refusing to build a silently-broken layer."
      exit 1
    fi
    echo "All three Android bug-fixes verified present in the tree."
    echo "----------------------------------------"

    # GE-Proton game-fixes tier, layered AFTER the bionic patches (verified to
    # apply cleanly on the bionic-patched tree in this order). Hard-fails on any
    # reject so CI surfaces the conflict.
    echo "Applying GE-Proton patches..."
    ./build-scripts/apply-ge-patches.sh
  fi

  if [ "$arg" == "--build" ]
  then
    echo "Building..."
    rm -rf $OUTPUT_DIR/bin
    rm -rf $OUTPUT_DIR/lib
    rm -rf $OUTPUT_DIR/share
    rm -rf $install_dir
    make -j$(nproc)
  fi

  if [ "$arg" == "--install" ]
  then
    echo "Installing..."
    mkdir -p $OUTPUT_DIR/bin
    mkdir -p $OUTPUT_DIR/lib
    mkdir -p $OUTPUT_DIR/share
    mkdir -p $install_dir
    make install -j$(nproc)
    echo "Copying files..."
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
	# symlinking wine binaries to $install_dir/bin
    ln -sf ../lib/wine/aarch64-unix/wine "$install_dir/bin/wine"
    ln -sf ../lib/wine/aarch64-unix/wine "$OUTPUT_DIR/bin/wine"
    ln -sf ../lib/wine/aarch64-unix/wine-preloader "$OUTPUT_DIR/bin/wine-preloader"
    ln -sf ../lib/wine/aarch64-unix/wine-preloader "$install_dir/bin/wine-preloader"
    echo "Wine loader symlinks:"
    ls -la "$OUTPUT_DIR/bin/wine" "$OUTPUT_DIR/bin/wine-preloader"
  fi
done
