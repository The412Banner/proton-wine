/*
 * Wayland OpenGL functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
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

#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "wine/debug.h"

#ifdef HAVE_LIBWAYLAND_EGL

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#include <wayland-egl.h>

#include "wine/opengl_driver.h"

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs wayland_drawable_funcs;
static EGLContext egl_fallback_context;

/* Original (win32u) driver proc-address resolver, chained from
 * wayland_get_proc_address so that names other than the WINE dmabuf-export
 * extension keep their previous behavior. */
static void *(*prev_get_proc_address)(const char *);

/* dmabuf export leaf (GL texture/renderbuffer -> Linux dmabuf via
 * EGL_MESA_image_dma_buf_export). Resolved lazily and gated behind the
 * runtime extension check below. */
static PFN_eglCreateImageKHR pfn_eglCreateImageKHR;
static PFN_eglDestroyImageKHR pfn_eglDestroyImageKHR;
static PFN_eglExportDMABUFImageMESA pfn_eglExportDMABUFImageMESA;
static PFN_eglExportDMABUFImageQueryMESA pfn_eglExportDMABUFImageQueryMESA;

static pthread_once_t dmabuf_export_init_once = PTHREAD_ONCE_INIT;
static BOOL dmabuf_export_supported;

static BOOL dmabuf_export_type_supported(GLenum type)
{
    if (!egl) return FALSE;
    switch (type)
    {
    case GL_TEXTURE_2D:
    case GL_RENDERBUFFER:
        return TRUE;
    default:
        return FALSE;
    }
}

static void dmabuf_export_init_support(void)
{
    const char *extensions;

    if (!egl || !funcs || !funcs->p_eglGetProcAddress || !funcs->p_eglQueryString ||
        !funcs->p_eglGetCurrentContext)
        return;
    if (!(extensions = funcs->p_eglQueryString(egl->display, EGL_EXTENSIONS))) return;
    if (!strstr(extensions, "EGL_KHR_image_base") && !strstr(extensions, "EGL_KHR_image"))
        return;
    if (!strstr(extensions, "EGL_MESA_image_dma_buf_export"))
        return;
    if (!dmabuf_export_type_supported(GL_TEXTURE_2D))
        return;

    pfn_eglCreateImageKHR = (void *)funcs->p_eglGetProcAddress("eglCreateImageKHR");
    pfn_eglDestroyImageKHR = (void *)funcs->p_eglGetProcAddress("eglDestroyImageKHR");
    pfn_eglExportDMABUFImageMESA = (void *)funcs->p_eglGetProcAddress("eglExportDMABUFImageMESA");
    pfn_eglExportDMABUFImageQueryMESA = (void *)funcs->p_eglGetProcAddress("eglExportDMABUFImageQueryMESA");

    if (!pfn_eglCreateImageKHR || !pfn_eglDestroyImageKHR || !pfn_eglExportDMABUFImageMESA ||
        !pfn_eglExportDMABUFImageQueryMESA)
        return;

    dmabuf_export_supported = TRUE;
}

static BOOL dmabuf_export_bridge_supported(void)
{
    pthread_once(&dmabuf_export_init_once, dmabuf_export_init_support);
    return dmabuf_export_supported;
}

struct wayland_gl_drawable
{
    struct opengl_drawable base;
    struct wl_egl_window *wl_egl_window;
    EGLConfig config;
    LONG attachment_generation;
    BOOL present_opaque;
    BOOL swap_interval_initialized;
    BOOL manual_frame_throttle;
};

static struct wayland_gl_drawable *impl_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_gl_drawable, base);
}

static void wayland_drawable_destroy(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);
    if (gl->wl_egl_window) wl_egl_window_destroy(gl->wl_egl_window);
}

static EGLConfig egl_config_for_format(int format)
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void wayland_gl_drawable_sync_size(struct wayland_gl_drawable *gl)
{
    int client_width, client_height;
    RECT client_rect = {0};

    NtUserGetClientRect(gl->base.client->hwnd, &client_rect, NtUserGetDpiForWindow(gl->base.client->hwnd));
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;
    if (client_width == 0 || client_height == 0) client_width = client_height = 1;

    wl_egl_window_resize(gl->wl_egl_window, client_width, client_height, 0, 0);
}

static EGLSurface wayland_gl_drawable_create_egl_surface(struct wayland_gl_drawable *gl)
{
    EGLint attribs[4], *attrib = attribs;

    if (gl->present_opaque)
    {
        *attrib++ = EGL_PRESENT_OPAQUE_EXT;
        *attrib++ = EGL_TRUE;
    }
    *attrib++ = EGL_NONE;

    return funcs->p_eglCreateWindowSurface(egl->display, gl->config,
                                           gl->wl_egl_window, attribs);
}

static BOOL wayland_gl_drawable_recreate_egl_surface(struct wayland_gl_drawable *gl)
{
    EGLSurface old_surface = gl->base.surface;
    EGLSurface draw_surface = EGL_NO_SURFACE, read_surface = EGL_NO_SURFACE;
    EGLContext context = funcs->p_eglGetCurrentContext();
    BOOL rebind = FALSE;

    if (context != EGL_NO_CONTEXT)
    {
        draw_surface = funcs->p_eglGetCurrentSurface(EGL_DRAW);
        read_surface = funcs->p_eglGetCurrentSurface(EGL_READ);
        rebind = draw_surface == old_surface || read_surface == old_surface;
        if (rebind && !funcs->p_eglMakeCurrent(egl->display, EGL_NO_SURFACE,
                                              EGL_NO_SURFACE, EGL_NO_CONTEXT))
        {
            ERR("Failed to release EGL surface %p with error %#x.\n",
                old_surface, funcs->p_eglGetError());
            return FALSE;
        }
    }

    if (old_surface != EGL_NO_SURFACE &&
        !funcs->p_eglDestroySurface(egl->display, old_surface))
    {
        ERR("Failed to destroy stale EGL surface %p with error %#x.\n",
            old_surface, funcs->p_eglGetError());
        return FALSE;
    }

    gl->base.surface = wayland_gl_drawable_create_egl_surface(gl);
    if (gl->base.surface == EGL_NO_SURFACE)
    {
        ERR("Failed to recreate EGL window surface with error %#x.\n",
            funcs->p_eglGetError());
        return FALSE;
    }

    if (rebind)
    {
        if (draw_surface == old_surface) draw_surface = gl->base.surface;
        if (read_surface == old_surface) read_surface = gl->base.surface;
        if (!funcs->p_eglMakeCurrent(egl->display, draw_surface, read_surface, context))
        {
            ERR("Failed to bind recreated EGL surface %p with error %#x.\n",
                gl->base.surface, funcs->p_eglGetError());
            return FALSE;
        }
    }

    gl->swap_interval_initialized = FALSE;
    TRACE("Recreated EGL surface %p as %p after a client role transition.\n",
          old_surface, gl->base.surface);
    return TRUE;
}

static BOOL wayland_opengl_surface_create(HWND hwnd, BOOL raw, int format, struct opengl_drawable **drawable)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_client_surface *client;
    struct opengl_drawable *previous;
    struct wayland_gl_drawable *gl;
    RECT rect;

    TRACE("hwnd=%p format=%d\n", hwnd, format);

    if ((previous = *drawable) && previous->format == format) return TRUE;

    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    if (rect.right == rect.left) rect.right = rect.left + 1;
    if (rect.bottom == rect.top) rect.bottom = rect.top + 1;

    if (!egl->has_EGL_EXT_present_opaque)
        WARN("Missing EGL_EXT_present_opaque extension\n");

    if (!(client = wayland_client_surface_create(hwnd))) return FALSE;
    gl = opengl_drawable_create(sizeof(*gl), &wayland_drawable_funcs, format, &client->client);
    client_surface_release(&client->client);
    if (!gl) return FALSE;
    gl->base.buffer_map[0] = GL_BACK_LEFT;
    gl->base.buffer_map[1] = GL_BACK_RIGHT;
    gl->base.buffer_map[GL_FRONT - GL_FRONT_LEFT] = GL_BACK;
    gl->base.buffer_map[GL_FRONT_AND_BACK - GL_FRONT_LEFT] = GL_BACK;
    gl->config = config;
    gl->present_opaque = egl->has_EGL_EXT_present_opaque;
    gl->attachment_generation = ReadAcquire(&client->attachment_generation);

    NtCreateEvent(&client->throttle, EVENT_ALL_ACCESS, NULL, SynchronizationEvent, TRUE);

    if (!(gl->wl_egl_window = wl_egl_window_create(client->wl_surface, rect.right, rect.bottom))) goto err;
    if (!(gl->base.surface = wayland_gl_drawable_create_egl_surface(gl))) goto err;
    set_client_surface(hwnd, client);

    TRACE("Created drawable %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);

    if (previous) opengl_drawable_release( previous );
    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static void wayland_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_WAYLAND_KHR;
    platform->native_display = process_wayland.wl_display;
    platform->force_pbuffer_formats = TRUE;
    egl = platform;
}

static void wayland_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);

    TRACE("drawable %s, flags %#x\n", debugstr_opengl_drawable(base), flags);

    /* Since context_flush is called from operations that may latch the native size,
     * perform any pending resizes before calling them. */
    if (flags & GL_FLUSH_UPDATED) wayland_gl_drawable_sync_size(gl);
}

static void wayland_gl_frame_done(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct client_surface *client = data;
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    BOOL current;

    current = InterlockedCompareExchangePointer((void **)&surface->wl_callback, NULL,
                                                wl_callback) == wl_callback;
    if (!current) return;
    wl_callback_destroy(wl_callback);

    TRACE("hwnd=%p\n", client->hwnd);

    if (surface->throttle) NtSetEvent(surface->throttle, NULL);
}

static const struct wl_callback_listener gl_throttle_listener =
{
    wayland_gl_frame_done,
};

static BOOL wayland_drawable_swap(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);
    struct wayland_client_surface *surface = impl_from_client_surface(base->client);
    struct wl_callback *callback;
    LONG attachment_generation;
    BOOL fallback_bound = FALSE;
    EGLint error;

    client_surface_present(base->client);
    if (!surface->toplevel && !surface->hwnd_dmabuf_producer)
        return TRUE;

    attachment_generation = ReadAcquire(&surface->attachment_generation);
    if (gl->attachment_generation != attachment_generation)
    {
        if (!wayland_gl_drawable_recreate_egl_surface(gl)) return TRUE;
        gl->attachment_generation = attachment_generation;
    }

    if (!funcs->p_eglGetCurrentContext())
    {
        if (!egl_fallback_context)
        {
            egl_fallback_context = funcs->p_eglCreateContext(egl->display, EGL_NO_CONFIG_KHR,
                                                             EGL_NO_CONTEXT, NULL);
            if (!egl_fallback_context)
            {
                ERR("Failed to create fallback EGL context with error %#x.\n",
                    funcs->p_eglGetError());
                return TRUE;
            }
        }

        if (!funcs->p_eglMakeCurrent(egl->display, gl->base.surface,
                                     gl->base.surface, egl_fallback_context))
        {
            ERR("Failed to bind fallback EGL context with error %#x.\n",
                funcs->p_eglGetError());
            return TRUE;
        }
        fallback_bound = TRUE;
    }

    if (!gl->swap_interval_initialized)
    {
        gl->manual_frame_throttle = funcs->p_eglSwapInterval(egl->display, 0);
        gl->swap_interval_initialized = TRUE;
        if (!gl->manual_frame_throttle)
            WARN("Failed to disable the EGL swap interval; retaining native throttling (error %#x).\n",
                 funcs->p_eglGetError());
    }

    if (gl->manual_frame_throttle && abs(base->interval))
    {
        const LARGE_INTEGER timeout = { .QuadPart = -10000 * 1000 };
        NTSTATUS status;

        if (surface->throttle)
        {
            status = NtWaitForSingleObject(surface->throttle, FALSE, &timeout);
            if (status == STATUS_TIMEOUT) WARN("Present timed out!\n");
        }

        /* wp_fifo with commit timing could support intervals greater than one. */
        if (!ReadPointerAcquire((void * const volatile *)&surface->wl_callback))
        {
            if (surface->throttle) NtResetEvent(surface->throttle, NULL);
            callback = wl_surface_frame(surface->wl_surface);
            wl_callback_add_listener(callback, &gl_throttle_listener, base->client);
            InterlockedExchangePointer((void **)&surface->wl_callback, callback);
        }
    }
    if (!funcs->p_eglSwapBuffers(egl->display, gl->base.surface))
    {
        error = funcs->p_eglGetError();
        if (error == EGL_BAD_SURFACE && wayland_gl_drawable_recreate_egl_surface(gl))
        {
            gl->attachment_generation = ReadAcquire(&surface->attachment_generation);
            if (funcs->p_eglSwapBuffers(egl->display, gl->base.surface)) goto done;
            error = funcs->p_eglGetError();
        }
        ERR("eglSwapBuffers failed with error %#x.\n", error);
    }

done:
    if (fallback_bound &&
        !funcs->p_eglMakeCurrent(egl->display, EGL_NO_SURFACE,
                                 EGL_NO_SURFACE, EGL_NO_CONTEXT))
    {
        WARN("Failed to release fallback EGL context with error %#x.\n",
             funcs->p_eglGetError());
    }

    return TRUE;
}

struct wayland_pbuffer
{
    struct opengl_drawable base;
    struct wl_surface *surface;
    struct wl_egl_window *window;
};

static struct wayland_pbuffer *pbuffer_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_pbuffer, base);
}

static void wayland_pbuffer_destroy(struct opengl_drawable *base)
{
    struct wayland_pbuffer *gl = pbuffer_from_opengl_drawable(base);

    TRACE("%s\n", debugstr_opengl_drawable(base));

    if (gl->window)
        wl_egl_window_destroy(gl->window);
    if (gl->surface)
        wl_surface_destroy(gl->surface);
}

static const struct opengl_drawable_funcs wayland_pbuffer_funcs =
{
    .destroy = wayland_pbuffer_destroy,
};

static BOOL wayland_pbuffer_create(HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                   GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **surface)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_pbuffer *gl;

    TRACE("hdc %p, format %d, largest %u, texture_format %#x, texture_target %#x, max_level %#x, width %d, height %d, private %p\n",
          hdc, format, largest, texture_format, texture_target, max_level, *width, *height, surface);

    if (!(gl = opengl_drawable_create(sizeof(*gl), &wayland_pbuffer_funcs, format, NULL))) return FALSE;
    /* Wayland EGL doesn't support pixmap or pbuffer, create a dummy window surface to act as the target render surface. */
    if (!(gl->surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    if (!(gl->window = wl_egl_window_create(gl->surface, *width, *height))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->window, NULL))) goto err;

    TRACE("Created pbuffer %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);
    *surface = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static BOOL wayland_pbuffer_updated(HDC hdc, struct opengl_drawable *base, GLenum cube_face, GLint mipmap_level)
{
    return GL_TRUE;
}

static UINT wayland_pbuffer_bind(HDC hdc, struct opengl_drawable *base, GLenum buffer)
{
    return -1; /* use default implementation */
}

static BOOL GLAPIENTRY wayland_wglWineExportDmaBufWINE(GLuint texture, GLenum target,
        struct wgl_dmabuf_desc *desc, int *fd)
{
    const EGLint image_attribs[] = {EGL_NONE};
    EGLuint64KHR modifiers[4] = {0};
    EGLint offsets[4] = {0}, strides[4] = {0};
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    EGLClientBuffer client;
    int export_fd = -1;
    int fourcc = 0, planes = 0;
    EGLenum egl_target;

    TRACE("texture %u, target %#x, desc %p, fd %p.\n", texture, target, desc, fd);

    if (!desc || !fd) return FALSE;
    memset(desc, 0, sizeof(*desc));
    *fd = -1;

    if (!texture || !dmabuf_export_bridge_supported() || !dmabuf_export_type_supported(target))
        return FALSE;

    switch (target)
    {
    case GL_TEXTURE_2D:
        egl_target = EGL_GL_TEXTURE_2D;
        break;
    case GL_RENDERBUFFER:
        egl_target = EGL_GL_RENDERBUFFER;
        break;
    default:
        return FALSE;
    }

    client = (EGLClientBuffer)(uintptr_t)texture;
    image = pfn_eglCreateImageKHR(egl->display, funcs->p_eglGetCurrentContext(),
            egl_target, client, image_attribs);
    if (image == EGL_NO_IMAGE_KHR)
        return FALSE;

    /* The export path currently supports only single-plane RGB buffers. */
    if (!pfn_eglExportDMABUFImageQueryMESA(egl->display, image, &fourcc, &planes, modifiers)
            || planes != 1)
        goto done;

    if (!pfn_eglExportDMABUFImageMESA(egl->display, image, &export_fd, strides, offsets)
            || export_fd < 0)
        goto done;

    desc->fourcc = fourcc;
    desc->stride = strides[0];
    desc->offset = offsets[0];
    desc->modifier = modifiers[0];
    *fd = export_fd;
    export_fd = -1;

done:
    if (image != EGL_NO_IMAGE_KHR)
        pfn_eglDestroyImageKHR(egl->display, image);
    if (export_fd >= 0)
        close(export_fd);
    return *fd >= 0;
}

static BOOL GLAPIENTRY wayland_wglWineDmaBufExportSupportedWINE(void)
{
    return dmabuf_export_bridge_supported();
}

static void *wayland_get_proc_address(const char *name)
{
    if (!strcmp(name, "wglWineDmaBufExportSupportedWINE"))
        return wayland_wglWineDmaBufExportSupportedWINE;
    if (!strcmp(name, "wglWineExportDmaBufWINE"))
        return dmabuf_export_bridge_supported() ? (void *)wayland_wglWineExportDmaBufWINE : NULL;

    return prev_get_proc_address ? prev_get_proc_address(name) : NULL;
}

static struct opengl_driver_funcs wayland_driver_funcs =
{
    .p_init_egl_platform = wayland_init_egl_platform,
    .p_surface_create = wayland_opengl_surface_create,
    .p_pbuffer_create = wayland_pbuffer_create,
    .p_pbuffer_updated = wayland_pbuffer_updated,
    .p_pbuffer_bind = wayland_pbuffer_bind,
};

static const struct opengl_drawable_funcs wayland_drawable_funcs =
{
    .destroy = wayland_drawable_destroy,
    .flush = wayland_drawable_flush,
    .swap = wayland_drawable_swap,
};

/**********************************************************************
 *           WAYLAND_OpenGLInit
 */
UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR("Version mismatch, opengl32 wants %u but driver has %u\n",
            version, WINE_OPENGL_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    prev_get_proc_address = (*driver_funcs)->p_get_proc_address;
    wayland_driver_funcs.p_get_proc_address = wayland_get_proc_address;
    wayland_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    wayland_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    wayland_driver_funcs.p_init_wgl_extensions = (*driver_funcs)->p_init_wgl_extensions;
    wayland_driver_funcs.p_context_create = (*driver_funcs)->p_context_create;
    wayland_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    wayland_driver_funcs.p_make_current = (*driver_funcs)->p_make_current;

    *driver_funcs = &wayland_driver_funcs;
    return STATUS_SUCCESS;
}

#else /* No GL */

UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif
