#!/bin/sh
# Build the host wine-tools (winebuild/winegcc/wrc/etc.) used by the cross build
# via --with-wine-tools=./wine-tools. Native (glibc) host compiler, no X / no
# gstreamer / no vulkan / no wayland needed for the tool deps.
set -e
mkdir -p wine-tools
cd wine-tools
../configure --without-x --without-gstreamer --without-vulkan --without-wayland
make -j$(nproc) __tooldeps__ nls/all
