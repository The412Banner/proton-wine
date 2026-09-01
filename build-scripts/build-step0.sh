#!/bin/sh

set -e

mkdir -p wine-tools
cd wine-tools
../configure --without-x --without-gstreamer --without-vulkan --without-wayland
make -j$(nproc) __tooldeps__ nls/all