# Wayland Turnip

`usr/lib/libvulkan_freedreno_wayland.so` is `libvulkan_freedreno.so` from Termux's
`mesa-vulkan-icd-freedreno` 26.0.6-3 (aarch64): Turnip built with the KGSL backend and the
Wayland WSI. The containers' own Vulkan drivers (the wrapper, adrenotools Turnip builds) have
no Wayland WSI, so winewayland points `VK_ICD_FILENAMES` at
`share/vulkan/icd.d/banner_wayland_turnip.json` when it runs on the Bannerlator compositor.

Its libraries (libdrm, libxcb*, libX11-xcb, libxshmfence, libandroid-shmem, libc++_shared,
libz, libzstd) come from the imagefs; libwayland-client is bundled next to it.

Source: https://packages-cf.termux.dev/apt/termux-main/pool/main/m/mesa-vulkan-icd-freedreno/mesa-vulkan-icd-freedreno_26.0.6-3_aarch64.deb
