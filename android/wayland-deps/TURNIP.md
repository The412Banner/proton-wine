# Wayland Turnip

`usr/lib/libvulkan_freedreno_wayland.so` is `libvulkan_freedreno.so` from Termux's
`mesa-vulkan-icd-freedreno` 26.0.6-3 (aarch64): Turnip built with the KGSL backend and the
Wayland WSI. The containers' own Vulkan drivers (the wrapper, adrenotools Turnip builds) have
no Wayland WSI, so winewayland points `VK_ICD_FILENAMES` at
`share/vulkan/icd.d/banner_wayland_turnip.json` when it runs on the Bannerlator compositor.

Its libraries (libdrm, libxcb*, libX11-xcb, libxshmfence, libandroid-shmem, libc++_shared,
libz, libzstd) come from the imagefs; libwayland-client is bundled next to it.

Source: https://packages-cf.termux.dev/apt/termux-main/pool/main/m/mesa-vulkan-icd-freedreno/mesa-vulkan-icd-freedreno_26.0.6-3_aarch64.deb

# OpenGL (EGL + Zink)

`usr/lib/libEGL.so.1`, `libGLESv2.so.2` and `libgallium-26.3.0-devel.so` are Mesa 26.3.0-devel at
7cda7850edd103ace21aac37d416d2fdf7a282e1 (the Banners-Turnip release commit), built by the
Banners-Turnip `wayland` branch (`build_wayland.sh`): EGL on the Wayland platform with Zink, no LLVM,
no GLX, as a Linux-style build on bionic like Termux's Mesa. winewayland points Mesa at Zink
(`MESA_LOADER_DRIVER_OVERRIDE=zink`, `LIBGL_ALWAYS_SOFTWARE=1`: without a DRM device EGL only reaches
Zink through its software path, which then draws with Vulkan through kopper) when these are present.
Zink opens `libvulkan.so.1`, the imagefs Vulkan loader, which follows `VK_ICD_FILENAMES` to the
Wayland Turnip above.

`libwayland-client.so` and `libwayland-egl.so` are Termux's libwayland 1.25.0-1, the version these
libraries were linked against.
