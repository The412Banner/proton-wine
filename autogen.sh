#!/bin/sh
set -e

APPLY_ANDROID_PATCH=0

while [ $# -gt 0 ]; do
    case "$1" in
        --aarch64)
            APPLY_ANDROID_PATCH=1
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--aarch64]"
            exit 1
            ;;
    esac
done

tools/make_requests
tools/make_specfiles
dlls/winevulkan/make_vulkan -x vk.xml -X video.xml

if [ $APPLY_ANDROID_PATCH -eq 1 ]; then
    echo "Applying Android patch to configure.ac..."
    # vanilla wine-10.6 already carries most arm64ec configure bits, so this only adds
    # ,aarch64 to a couple of enable_* lists and lands with a line offset -> fuzz-tolerant,
    # non-fatal (redundant on trees that already have it).
    patch -p1 --fuzz=3 --forward --no-backup-if-mismatch < android/patches/test-bylaws/configure_ac.patch || true
fi

autoreconf -ifv
rm -rf autom4te.cache

echo "Now run ./configure"
