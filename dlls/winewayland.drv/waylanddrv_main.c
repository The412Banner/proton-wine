/*
 * WAYLANDDRV initialization code
 *
 * Copyright 2020 Alexandre Frantzis for Collabora Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"

char *process_name = NULL;

static const struct user_driver_funcs waylanddrv_funcs =
{
    .pClipboardWindowProc = WAYLAND_ClipboardWindowProc,
    .pClipCursor = WAYLAND_ClipCursor,
    .pDesktopWindowProc = WAYLAND_DesktopWindowProc,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pSetIMECompositionRect = WAYLAND_SetIMECompositionRect,
    .pKbdLayerDescriptor = WAYLAND_KbdLayerDescriptor,
    .pReleaseKbdTables = WAYLAND_ReleaseKbdTables,
    .pSetCursor = WAYLAND_SetCursor,
    .pSetCursorPos = WAYLAND_SetCursorPos,
    .pSetDesktopWindow = WAYLAND_SetDesktopWindow,
    .pSetLayeredWindowAttributes = WAYLAND_SetLayeredWindowAttributes,
    .pSetWindowIcons = WAYLAND_SetWindowIcons,
    .pSetWindowStyle = WAYLAND_SetWindowStyle,
    .pSetWindowText = WAYLAND_SetWindowText,
    .pSysCommand = WAYLAND_SysCommand,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pClipClientSurfaces = WAYLAND_ClipClientSurfaces,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
    .pCreateWindowSurface = WAYLAND_CreateWindowSurface,
    .pVulkanInit = WAYLAND_VulkanInit,
    .pOpenGLInit = WAYLAND_OpenGLInit,
};

static void wayland_init_process_name(void)
{
    WCHAR *p, *appname;
    WCHAR appname_lower[MAX_PATH];
    DWORD appname_len;
    DWORD appnamez_size;
    DWORD utf8_size;
    int i;

    appname = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr(appname, '/'))) appname = p + 1;
    if ((p = wcsrchr(appname, '\\'))) appname = p + 1;
    appname_len = lstrlenW(appname);

    if (appname_len == 0 || appname_len >= MAX_PATH) return;

    for (i = 0; appname[i]; i++) appname_lower[i] = RtlDowncaseUnicodeChar(appname[i]);
    appname_lower[i] = 0;

    appnamez_size = (appname_len + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_size, appname_lower, appnamez_size) &&
        (process_name = malloc(utf8_size)))
    {
        RtlUnicodeToUTF8N(process_name, utf8_size, &utf8_size, appname_lower, appnamez_size);
    }
}

/* Containers on the Bannerlator compositor point VK_ICD_FILENAMES at a wrapper driver that
 * can only present to X11, and their OpenGL is GLX-only. This build ships a Wayland-capable
 * Turnip and Mesa's EGL + Zink next to Wine, so use those when we're on that compositor. */
static void use_bundled_drivers(void)
{
    static const char json[] = "/share/vulkan/icd.d/banner_wayland_turnip.json";
    static const char egl[] = "/lib/libEGL.so.1";
    char wine[PATH_MAX], path[PATH_MAX], *p;
    const char *env;
    Dl_info info;
    int i;

    if (!dladdr((void *)use_bundled_drivers, &info) || !info.dli_fname) return;
    if (strlen(info.dli_fname) >= sizeof(wine) - sizeof(json)) return;
    strcpy(wine, info.dli_fname);
    /* <wine>/lib/wine/aarch64-unix/winewayland.so -> <wine> */
    for (i = 0; i < 4; i++)
    {
        if (!(p = strrchr(wine, '/'))) return;
        *p = 0;
    }

    strcpy(path, wine);
    strcat(path, json);
    if (!access(path, R_OK))
    {
        setenv("VK_ICD_FILENAMES", path, 1);
        MESSAGE("winewayland: Vulkan driver %s\n", path);

        /* The Vulkan loader unloads the driver when a program destroys its last instance,
         * and a later call still reaching into it then jumps into unmapped code (DiRT Rally
         * 2.0 probes a device, releases it, and spins on that fault). Keep the library
         * resident for the life of the process. */
        strcpy(path, wine);
        strcat(path, "/lib/libvulkan_freedreno_wayland.so");
        if (!dlopen(path, RTLD_NOW | RTLD_NODELETE))
            MESSAGE("winewayland: could not pin %s: %s\n", path, dlerror());
        /* Keeping the driver resident leaves its globals alive across a winevulkan unload, so
         * winevulkan pins itself too when we are the active driver and the two stay in step
         * (see dlls/winevulkan/loader.c). */
    }

    /* OpenGL through EGL on Zink, opt-in for now (BANNER_WAYLAND_GL=1): with WINE_USE_EGL set,
     * win32u probes the GPU at every process start, and that probe in the desktop process
     * deadlocks other processes opening a display DC. */
    strcpy(path, wine);
    strcat(path, egl);
    if (!access(path, R_OK) && (env = getenv("BANNER_WAYLAND_GL")) && atoi(env))
    {
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        /* NOT LIBGL_ALWAYS_SOFTWARE: that makes Zink demand a CPU Vulkan device. Our bundled Mesa
         * takes the kopper (Zink) path for a Wayland display without a DRM device on its own. */
        setenv("WINE_USE_EGL", "1", 1);
        MESSAGE("winewayland: OpenGL through %s (Zink)\n", path);
    }
}

static NTSTATUS waylanddrv_unix_init(void *arg)
{
    /* Set the user driver functions now so that they are available during
     * our initialization. We clear them on error. */
    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    wayland_init_process_name();

    if (!wayland_process_init()) goto err;

    if (process_wayland.banner_desktop_v1) use_bundled_drivers();

    return 0;

err:
    __wine_set_user_driver(NULL, WINE_GDI_DRIVER_VERSION);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_read_events(void *arg)
{
    while (wl_display_dispatch_queue(process_wayland.wl_display,
                                     process_wayland.wl_event_queue) != -1)
        continue;
    /* This function only returns on a fatal error, e.g., if our connection
     * to the Wayland server is lost. */
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_init_clipboard(void *arg)
{
    /* If the compositor supports zwlr_data_control_manager_v1, we don't need
     * per-process clipboard window and handling, we can use the default clipboard
     * window from the desktop process. */
    if (process_wayland.zwlr_data_control_manager_v1) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == waylanddrv_unix_func_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == waylanddrv_unix_func_count);

#endif /* _WIN64 */
