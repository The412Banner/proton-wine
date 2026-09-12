# Wayland Turnip

`usr/lib/libvulkan_freedreno_wayland.so` is the Banners-Turnip **combined** driver: the release's
A6xx/A7xx Turnip (Android platform, KGSL, Mesa at the release commit) built with the Wayland WSI
as well (`build_turnip_wayland_wsi.sh` on the Banners-Turnip `wayland` branch, `-Dplatforms=
android,wayland`). The same file, zipped with `meta.json`, is the AdrenoTools driver for X11, so one
driver serves both paths. It links Termux's libwayland-client (bundled next to it) and Android's
libhardware/libnativewindow/libsync from /system/lib64. The containers' own Vulkan drivers (the
wrapper, plain adrenotools builds) have no Wayland WSI, so winewayland points `VK_ICD_FILENAMES` at
`share/vulkan/icd.d/banner_wayland_turnip.json` when it runs on the Bannerlator compositor.

Before 2026-09-12 15:25 this was Termux's `mesa-vulkan-icd-freedreno` 26.0.6-3 Turnip.

# OpenGL (EGL + Zink)

`usr/lib/libEGL.so.1`, `libGLESv2.so.2` and `libgallium-26.3.0-devel.so` are Mesa 26.3.0-devel at
7cda7850edd103ace21aac37d416d2fdf7a282e1 (the Banners-Turnip release commit), built by the
Banners-Turnip `wayland` branch (`build_wayland.sh`): EGL on the Wayland platform with Zink, no LLVM,
no GLX, as a Linux-style build on bionic like Termux's Mesa, with EGL patched to take its kopper (Zink)
path for a Wayland display without a DRM device. winewayland points Mesa at Zink
(`MESA_LOADER_DRIVER_OVERRIDE=zink`, `WINE_USE_EGL=1`; NOT `LIBGL_ALWAYS_SOFTWARE`, which makes
Zink demand a CPU Vulkan device) when these are present.
Zink opens `libvulkan.so.1`, the imagefs Vulkan loader, which follows `VK_ICD_FILENAMES` to the
Wayland Turnip above.

`libwayland-client.so` and `libwayland-egl.so` are Termux's libwayland 1.25.0-1, the version these
libraries were linked against.
