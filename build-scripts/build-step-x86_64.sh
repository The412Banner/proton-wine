#!/bin/bash
#
# Proton 9.0 x86_64 (bionic) build step — box64 runtime path.
#
# Source tree = Pipetto-crypto/wine `proton-9.0-x86_64`, which is ALREADY
# pre-patched at the source level with the Winlator/Proton edits (there is no
# ./android/patches/ directory on this branch, unlike the arm64ec tree). So
# this script does NOT apply any patches — it only configures, builds, strips
# and installs. The five arch-agnostic "arihany-parity" edits (force_anon,
# drive-root, C.UTF-8, fast-yield, XRandR/XRender enablement) are baked into
# the tree / this script and hard-verified by the guard block in --configure.

export ARCH="x86_64"
export WIN_ARCH="x86_64,i386"
export OUTPUT_DIR="$HOME/compiled-files-x86_64"

export deps="$HOME/termuxfs/x86_64/data/data/com.termux/files/usr"
export RUNTIME_PATH="/data/data/com.termux/files/usr"
export install_dir=$deps/../opt/wine

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
# Applied to the unix side via CFLAGS and to the x86_64/i386 PE side via CROSSCFLAGS.
# (A post-install llvm-strip pass in --install trims the remaining symbol tables, so the
# shipped .so/.dll/.exe carry no .debug/.symtab sections.)
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
export X_LIBS="-L$deps/lib"
export GSTREAMER_CFLAGS="-I$deps/include/gstreamer-1.0 -I$deps/include/glib-2.0 -I$deps/lib/glib-2.0/include -I$deps/glib-2.0/include -I$deps/lib/gstreamer-1.0/include"
export GSTREAMER_LIBS="-L$deps/lib -lgstgl-1.0 -lgstapp-1.0 -lgstvideo-1.0 -lgstaudio-1.0 -lglib-2.0 -lgobject-2.0 -lgio-2.0 -lgsttag-1.0 -lgstbase-1.0 -lgstreamer-1.0"
export FFMPEG_CFLAGS="-I$deps/include/libavutil -I$deps/include/libavcodec -I$deps/include/libavformat"
export FFMPEG_LIBS="-L$deps/lib -lavutil -lavcodec -lavformat"

for arg in "$@"
do
  if [ "$arg" == "--enable-16kb-pages" ];
  then
    echo "Enabling 16KB page size support..."
    export TARGET=x86_64-linux-android35
    export C_OPTS="$C_OPTS -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES"
    export CFLAGS="$C_OPTS"
    export CXXFLAGS="$C_OPTS"
    export LDFLAGS="$LDFLAGS -Wl,-z,max-page-size=16384"
    # ccache-wrapped CC/CXX embed $TARGET; refresh them for the android35 target.
    if command -v ccache >/dev/null 2>&1; then
      export CC="ccache $TOOLCHAIN/$TARGET-clang"
      export CXX="ccache $TOOLCHAIN/$TARGET-clang++"
    else
      export CC=$TOOLCHAIN/$TARGET-clang
      export CXX=$TOOLCHAIN/$TARGET-clang++
    fi
    export AS=$TOOLCHAIN/$TARGET-clang
    echo "16KB page size support enabled"
  fi

  if [ "$arg" == "--build-sysvshm" ];
  then
    # x86_64 (box64) does not use the android_sysvshm SysV-shm shim (that shim
    # is an aarch64/arm64ec-only build input). This branch's tree has no
    # android/ directory either, so this is a deliberate no-op kept only for
    # call-site symmetry with build-step-arm64ec.sh.
    echo "--build-sysvshm: no-op for x86_64 (box64 path, no android_sysvshm on this tree)."
  fi

  if [ "$arg" == "--configure" ];
  then
    # NOTE: XRandR/XRender are ENABLED (--with-xrandr --with-xrender) for the
    # RandR refresh-rate / XRender path. Best-effort: the guard block below
    # hard-fails if configure did NOT actually detect the sonames, so a missing
    # sysroot lib surfaces loudly instead of silently shipping the stub.
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
      --without-xshm \
      --without-xxf86vm

    # ------------------------------------------------------------------
    # Arihany-parity / feature source-presence guards.
    #
    # This tree is pre-patched (no ./android/patches/), and the five
    # arch-agnostic arihany-parity edits are baked directly into the source
    # (force_anon, drive-root, C.UTF-8, fast-yield) or into this script
    # (XRandR/XRender). These guards HARD-FAIL the build if any edit is
    # missing, so a bad rebase/merge can never silently ship an unpatched or
    # feature-stripped .wcp.
    # ------------------------------------------------------------------
    echo "Verifying feature source edits are present..."
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
    # 1. force_anon / noexec (dlls/ntdll/unix/virtual.c)
    check_marker "force_anon (virtual.c)" dlls/ntdll/unix/virtual.c 'BOOL force_anon'
    # 2. shlfileop bare-drive-root guard (dlls/shell32/shlfileop.c)
    check_marker "drive-root guard (shlfileop.c)" dlls/shell32/shlfileop.c 'bare drive root'
    # 3. LC_ALL=C.UTF-8 default (dlls/ntdll/unix/env.c)
    check_marker "C.UTF-8 locale default (env.c)" dlls/ntdll/unix/env.c 'setenv\( "LC_ALL", "C.UTF-8", 0 \)'
    # 4. fast-yield gate (dlls/ntdll/unix/sync.c)
    check_marker "WINE_FAST_YIELD gate (sync.c)" dlls/ntdll/unix/sync.c 'WINE_FAST_YIELD'
    # 4b. Controller input: xinput must be the stock HID (winexinput.sys) reader,
    # NOT the legacy winlator UDP-socket variant (SERVER_PORT / GET_GAMEPAD), whose
    # Java-side endpoint Bannerlator does not serve -> zero controller input.
    check_marker "stock HID xinput (main.c opens device)" dlls/xinput1_3/main.c 'CreateFileW'
    if grep -qE 'SERVER_PORT|REQUEST_CODE_GET_GAMEPAD|recvfrom' dlls/xinput1_3/main.c; then
        echo "GUARD FAILED: xinput1_3/main.c still has the winlator UDP-socket input path" >&2
        guard_fail=1
    else
        echo "  OK: no legacy UDP-socket input path in xinput1_3/main.c"
    fi
    # 5. XRandR/XRender enabled in this build script (self-check)
    check_marker "--with-xrandr flag" build-scripts/build-step-x86_64.sh '^\s*--with-xrandr \\'
    check_marker "--with-xrender flag" build-scripts/build-step-x86_64.sh '^\s*--with-xrender \\'
    # 5b. Confirm configure actually DETECTED the XRandR/XRender sonames, i.e.
    # winex11 compiles the real xrandr backend (not the "XRandR support not
    # compiled in" stub). --with-xrandr alone is a silent no-op if the soname
    # is not found in the sysroot.
    if [ -f include/config.h ]; then
        check_marker "SONAME_LIBXRANDR detected (config.h)" include/config.h '^#define[[:space:]]+SONAME_LIBXRANDR[[:space:]]'
        check_marker "SONAME_LIBXRENDER detected (config.h)" include/config.h '^#define[[:space:]]+SONAME_LIBXRENDER[[:space:]]'
    else
        echo "GUARD FAILED: include/config.h not found after configure" >&2
        guard_fail=1
    fi
    if [ "$guard_fail" -ne 0 ]; then
        echo "ERROR: one or more feature source edits are missing; refusing to build." >&2
        exit 1
    fi
    echo "All feature source edits verified."
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
    # -L dereferences: if make install placed bin/wine as a symlink into
    # lib/wine/x86_64-unix/, copy the real loader ELF here (never a symlink).
    cp -rL $install_dir/bin/wine* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/reg* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/msi* $OUTPUT_DIR/bin
    cp -r $install_dir/bin/notepad $OUTPUT_DIR/bin
    cp -r $install_dir/lib/wine  $OUTPUT_DIR/lib
    cp -r $install_dir/share/wine  $OUTPUT_DIR/share

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

    # The unix loader (bin/wine) and its preloader (bin/wine-preloader) MUST ship
    # as REAL ELF binaries in bin/, exactly like build-step-arm64ec.sh keeps them
    # (working arm64ec wcp: bin/wine is a real ~10KB loader, no lib/wine/*-unix/wine).
    # A prior P11-derived version symlinked bin/wine -> lib/wine/x86_64-unix/wine,
    # but make install never places the loader there, so the link dangled and box64
    # reported: "Error: File is not found. (wine)". No symlinking here; hard-fail if
    # either loader is missing or is a symlink so an unlaunchable .wcp can't ship.
    echo "Verifying unix loader binaries are real files in bin/..."
    loader_fail=0
    for l in wine wine-preloader; do
      if [ -L "$OUTPUT_DIR/bin/$l" ]; then
        echo "ERROR: $OUTPUT_DIR/bin/$l is a symlink; must be a real loader ELF" >&2
        ls -la "$OUTPUT_DIR/bin/$l" >&2
        loader_fail=1
      elif [ ! -f "$OUTPUT_DIR/bin/$l" ]; then
        echo "ERROR: $OUTPUT_DIR/bin/$l is missing after install" >&2
        loader_fail=1
      else
        echo "  OK: bin/$l is a real file ($(stat -c%s "$OUTPUT_DIR/bin/$l") bytes)"
      fi
    done
    if [ "$loader_fail" -ne 0 ]; then
      echo "Loader binaries found in the install tree (for diagnosis):" >&2
      find "$install_dir" -type f \( -name wine -o -name wine-preloader -o -name wine64 \) -exec ls -la {} + >&2 2>/dev/null || true
      echo "ERROR: unix loader not packaged as real bin/ ELFs; refusing to ship an unlaunchable .wcp." >&2
      exit 1
    fi
    echo "Unix loader binaries present as real files in bin/."
  fi
done
