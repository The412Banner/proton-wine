/*
 * Wayland pointer handling
 *
 * Copyright (c) 2020 Alexandros Frantzis for Collabora Ltd
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

#include <errno.h>
#include <linux/input.h>
#undef SW_MAX /* Also defined in winuser.rh */
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>

#define OEMRESOURCE

#include "waylanddrv.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* The cursor-shape-v1 protocol file references the zwp_tablet_tool_v2
 * interface object. Since we don't currently use the tablet protocol,
 * provide a dummy object here to avoid linking errors. */
void *zwp_tablet_tool_v2_interface = NULL;

struct system_cursors
{
    WORD id;
    enum wp_cursor_shape_device_v1_shape shape;
};

static const struct system_cursors user32_cursors[] =
{
    {OCR_NORMAL,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {OCR_IBEAM,       WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
    {OCR_WAIT,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
    {OCR_CROSS,       WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
    {OCR_SIZE,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
    {OCR_SIZENWSE,    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},
    {OCR_SIZENESW,    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
    {OCR_SIZEWE,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
    {OCR_SIZENS,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
    {OCR_SIZEALL,     WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_RESIZE},
    {OCR_NO,          WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
    {OCR_HAND,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {OCR_APPSTARTING, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
    {OCR_HELP,        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
    {OCR_RDR2DIM,     WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},
    {0}
};

static const struct system_cursors comctl32_cursors[] =
{
    {102, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {104, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {105, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {106, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {107, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {108, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {135, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
    {0}
};

static const struct system_cursors ole32_cursors[] =
{
    {1, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {2, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {3, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {4, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
    {0}
};

static const struct system_cursors riched20_cursors[] =
{
    {105, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {109, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {110, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {111, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {0}
};

static const struct
{
    const struct system_cursors *cursors;
    WCHAR name[16];
} module_cursors[] =
{
    {user32_cursors, {'u','s','e','r','3','2','.','d','l','l',0}},
    {comctl32_cursors, {'c','o','m','c','t','l','3','2','.','d','l','l',0}},
    {ole32_cursors, {'o','l','e','3','2','.','d','l','l',0}},
    {riched20_cursors, {'r','i','c','h','e','d','2','0','.','d','l','l',0}}
};

static HWND wayland_pointer_get_focused_hwnd(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    pthread_mutex_lock(&pointer->mutex);
    hwnd = pointer->focused_hwnd;
    pthread_mutex_unlock(&pointer->mutex);

    return hwnd;
}

static void pointer_handle_motion_internal(wl_fixed_t sx, wl_fixed_t sy)
{
    INPUT input = {0};
    RECT *window_rect;
    HWND hwnd;
    POINT screen;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;
    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    window_rect = &surface->window.rect;

    wayland_surface_coords_to_window(surface,
                                     wl_fixed_to_double(sx),
                                     wl_fixed_to_double(sy),
                                     (int *)&screen.x, (int *)&screen.y);
    screen.x += window_rect->left;
    screen.y += window_rect->top;
    /* Sometimes, due to rounding, we may end up with pointer coordinates
     * slightly outside the target window, so bring them within bounds. */
    if (screen.x >= window_rect->right) screen.x = window_rect->right - 1;
    else if (screen.x < window_rect->left) screen.x = window_rect->left;
    if (screen.y >= window_rect->bottom) screen.y = window_rect->bottom - 1;
    else if (screen.y < window_rect->top) screen.y = window_rect->top;

    wayland_win_data_release(data);

    input.type = INPUT_MOUSE;
    input.mi.dx = screen.x;
    input.mi.dy = screen.y;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    TRACE("hwnd=%p wayland_xy=%.2f,%.2f screen_xy=%d,%d\n",
          hwnd, wl_fixed_to_double(sx), wl_fixed_to_double(sy),
          screen.x, screen.y);

    NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    /* Ignore absolute motion events if in relative mode. */
    if (pointer->zwp_relative_pointer_v1) return;

    pointer_handle_motion_internal(sx, sy);
}

static void wayland_set_cursor(HWND hwnd, HCURSOR hcursor, BOOL use_hcursor);

static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *wl_surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;
    /* The wl_surface user data remains valid and immutable for the whole
     * lifetime of the object, so it's safe to access without locking. */
    hwnd = wl_surface_get_user_data(wl_surface);

    TRACE("hwnd=%p\n", hwnd);

    pthread_mutex_lock(&pointer->mutex);
    pointer->focused_hwnd = hwnd;
    pointer->enter_serial = serial;
    pthread_mutex_unlock(&pointer->mutex);

    /* The cursor is undefined at every enter, so we set it again with
     * the latest information we have. */
    wayland_set_cursor(hwnd, NULL, FALSE);

    /* Handle the enter as a motion, to account for cases where the
     * window first appears beneath the pointer and won't get a separate
     * motion event. */
    pointer_handle_motion_internal(sx, sy);
}

static void pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t serial, struct wl_surface *wl_surface)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!wl_surface) return;

    TRACE("hwnd=%p\n", wl_surface_get_user_data(wl_surface));

    pthread_mutex_lock(&pointer->mutex);
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    pthread_mutex_unlock(&pointer->mutex);
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    INPUT input = {0};
    HWND hwnd;

    InterlockedExchange(&process_wayland.input_serial, serial);

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    input.type = INPUT_MOUSE;

    switch (button)
    {
    case BTN_LEFT: input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
    case BTN_RIGHT: input.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
    case BTN_MIDDLE: input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
    case BTN_SIDE:
    case BTN_BACK:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = XBUTTON1;
        break;
    case BTN_EXTRA:
    case BTN_FORWARD:
        input.mi.dwFlags = MOUSEEVENTF_XDOWN;
        input.mi.mouseData = XBUTTON2;
        break;
    default: break;
    }

    if (state == WL_POINTER_BUTTON_STATE_RELEASED) input.mi.dwFlags <<= 1;

    pthread_mutex_lock(&pointer->mutex);
    pointer->button_serial = state == WL_POINTER_BUTTON_STATE_PRESSED ?
                             serial : 0;
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd=%p button=%#x state=%u\n", hwnd, button, state);

    NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
                                       uint32_t axis_source)
{
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t time, uint32_t axis)
{
}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                         uint32_t axis, int32_t discrete)
{
    INPUT input = {0};
    HWND hwnd;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;

    input.type = INPUT_MOUSE;

    switch (axis)
    {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = -WHEEL_DELTA * discrete;
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = WHEEL_DELTA * discrete;
        break;
    default: break;
    }

    TRACE("hwnd=%p axis=%u discrete=%d\n", hwnd, axis, discrete);

    NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

static const struct wl_pointer_listener pointer_listener =
{
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete
};

/**********************************************************************
 *          wayland_motion_delta_to_window
 *
 * Converts the surface-local delta to window (logical) coordinate delta.
 */
static void wayland_motion_delta_to_window(struct wayland_surface *surface,
                                           double surface_x, double surface_y,
                                           double *window_x, double *window_y)
{
    *window_x = surface_x * surface->window.scale;
    *window_y = surface_y * surface->window.scale;
}

static void relative_pointer_v1_relative_motion(void *private,
                                                struct zwp_relative_pointer_v1 *zwp_relative_pointer_v1,
                                                uint32_t utime_hi, uint32_t utime_lo,
                                                wl_fixed_t dx, wl_fixed_t dy,
                                                wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    INPUT input = {0};
    HWND hwnd;
    struct wayland_win_data *data;
    double screen_x = 0.0, screen_y = 0.0;
    struct wayland_pointer *pointer = &process_wayland.pointer;

    if (!(hwnd = wayland_pointer_get_focused_hwnd())) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    wayland_motion_delta_to_window(data->wayland_surface,
                                   wl_fixed_to_double(dx),
                                   wl_fixed_to_double(dy),
                                   &screen_x, &screen_y);
    wayland_win_data_release(data);

    pthread_mutex_lock(&pointer->mutex);

    pointer->accum_x += screen_x;
    pointer->accum_y += screen_y;

    input.type = INPUT_MOUSE;
    input.mi.dx = round(pointer->accum_x);
    input.mi.dy = round(pointer->accum_y);
    input.mi.dwFlags = MOUSEEVENTF_MOVE;

    pointer->accum_x -= input.mi.dx;
    pointer->accum_y -= input.mi.dy;

    pthread_mutex_unlock(&pointer->mutex);

    TRACE("hwnd=%p wayland_dxdy=%.2f,%.2f accum_dxdy=%d,%d\n",
          hwnd, wl_fixed_to_double(dx), wl_fixed_to_double(dy),
          input.mi.dx, input.mi.dy);

    NtUserSendHardwareInput(hwnd, 0, &input, 0);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_v1_listener =
{
    relative_pointer_v1_relative_motion
};

void wayland_pointer_init(struct wl_pointer *wl_pointer)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    pointer->wl_pointer = wl_pointer;
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    pthread_mutex_unlock(&pointer->mutex);
    wl_pointer_add_listener(pointer->wl_pointer, &pointer_listener, NULL);
}

void wayland_pointer_deinit(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_confined_pointer_v1)
    {
        zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
        pointer->zwp_confined_pointer_v1 = NULL;
    }
    if (pointer->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
        pointer->zwp_locked_pointer_v1 = NULL;
    }
    if (pointer->zwp_relative_pointer_v1)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
    }
    if (pointer->wp_cursor_shape_device_v1)
    {
        wp_cursor_shape_device_v1_destroy(pointer->wp_cursor_shape_device_v1);
        pointer->wp_cursor_shape_device_v1 = NULL;
    }
    wl_pointer_release(pointer->wl_pointer);
    pointer->wl_pointer = NULL;
    pointer->focused_hwnd = NULL;
    pointer->enter_serial = 0;
    pthread_mutex_unlock(&pointer->mutex);
}

/***********************************************************************
 *           create_mono_cursor_buffer
 *
 * Create a wayland_shm_buffer for a monochrome cursor bitmap.
 *
 * Adapted from wineandroid.drv code.
 */
static struct wayland_shm_buffer *create_mono_cursor_buffer(HBITMAP bmp)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    BITMAP bm;
    char *mask = NULL;
    unsigned int i, j, stride, mask_size, *ptr;

    if (!NtGdiExtGetObjectW(bmp, sizeof(bm), &bm)) goto done;
    stride = ((bm.bmWidth + 15) >> 3) & ~1;
    mask_size = stride * bm.bmHeight;
    if (!(mask = malloc(mask_size))) goto done;
    if (!NtGdiGetBitmapBits(bmp, mask_size, mask)) goto done;

    bm.bmHeight /= 2;
    shm_buffer = wayland_shm_buffer_create(bm.bmWidth, bm.bmHeight,
                                           WL_SHM_FORMAT_ARGB8888);
    if (!shm_buffer) goto done;

    ptr = shm_buffer->map_data;
    for (i = 0; i < bm.bmHeight; i++)
    {
        for (j = 0; j < bm.bmWidth; j++, ptr++)
        {
            int and = ((mask[i * stride + j / 8] << (j % 8)) & 0x80);
            int xor = ((mask[(i + bm.bmHeight) * stride + j / 8] << (j % 8)) & 0x80);
            if (!xor && and)
                *ptr = 0;
            else if (xor && !and)
                *ptr = 0xffffffff;
            else
                /* we can't draw "invert" pixels, so render them as black instead */
                *ptr = 0xff000000;
        }
    }

done:
    free(mask);
    return shm_buffer;
}

/***********************************************************************
 *           get_icon_info
 *
 * Local GetIconInfoExW helper implementation.
 */
static BOOL get_icon_info(HICON handle, ICONINFOEXW *ret)
{
    UNICODE_STRING module, res_name;
    ICONINFO info;

    module.Buffer = ret->szModName;
    module.MaximumLength = sizeof(ret->szModName) - sizeof(WCHAR);
    res_name.Buffer = ret->szResName;
    res_name.MaximumLength = sizeof(ret->szResName) - sizeof(WCHAR);
    if (!NtUserGetIconInfo(handle, &info, &module, &res_name, NULL, 0)) return FALSE;
    ret->fIcon = info.fIcon;
    ret->xHotspot = info.xHotspot;
    ret->yHotspot = info.yHotspot;
    ret->hbmColor = info.hbmColor;
    ret->hbmMask = info.hbmMask;
    ret->wResID = res_name.Length ? 0 : LOWORD(res_name.Buffer);
    ret->szModName[module.Length] = 0;
    ret->szResName[res_name.Length] = 0;
    return TRUE;
}

static BOOL cursor_buffer_is_transparent(struct wayland_shm_buffer *shm_buffer)
{
    uint32_t *pixel = shm_buffer->map_data;
    uint32_t *end = pixel + shm_buffer->map_size / WINEWAYLAND_BYTES_PER_PIXEL;

    for (; pixel < end; ++pixel)
        if ((*pixel & 0xff000000) != 0) return FALSE;

    return TRUE;
}

/* Create the buffer and the scaled hotspot for one cursor image. */
static struct wayland_shm_buffer *create_cursor_image_buffer(HCURSOR hcursor, double scale,
                                                             int *hotspot_x, int *hotspot_y)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    ICONINFOEXW info = {0};
    int x, y;

    if (!get_icon_info(hcursor, &info))
    {
        ERR("Failed to get icon info for cursor=%p\n", hcursor);
        return NULL;
    }

    if (info.hbmColor)
    {
        HDC hdc = NtGdiCreateCompatibleDC(0);
        shm_buffer = wayland_shm_buffer_from_color_bitmaps(hdc, info.hbmColor, info.hbmMask);
        NtGdiDeleteObjectApp(hdc);
    }
    else
    {
        shm_buffer = create_mono_cursor_buffer(info.hbmMask);
    }

    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);

    if (!shm_buffer)
    {
        ERR("Failed to create shm_buffer for cursor=%p\n", hcursor);
        return NULL;
    }

    x = info.xHotspot;
    y = info.yHotspot;

    /* Make sure the hotspot is valid. */
    if (x >= shm_buffer->width || y >= shm_buffer->height)
    {
        x = shm_buffer->width / 2;
        y = shm_buffer->height / 2;
    }

    *hotspot_x = round(x / scale);
    *hotspot_y = round(y / scale);

    return shm_buffer;
}

static void wayland_cursor_free_frames(struct wayland_cursor *cursor)
{
    UINT i;

    for (i = 0; i < cursor->num_frames; i++)
        if (cursor->frames[i].shm_buffer)
            wayland_shm_buffer_unref(cursor->frames[i].shm_buffer);
    free(cursor->frames);
    cursor->frames = NULL;
    cursor->num_frames = 0;
    cursor->current_frame = 0;
}

/* Render every step of an animated cursor. Returns FALSE for a static
 * cursor, or if a frame can't be rendered, so that the caller falls back to
 * showing a single image. */
static BOOL wayland_cursor_create_frames(struct wayland_cursor *cursor,
                                         HCURSOR hcursor, double scale)
{
    struct wayland_cursor_frame *frames;
    DWORD delay_jiffies, num_steps;
    UINT i;

    if (!NtUserGetCursorFrameInfo(hcursor, 0, &delay_jiffies, &num_steps)) return FALSE;
    /* win32u reports ~0 steps for an animation with a single step. */
    if (num_steps <= 1 || num_steps == ~0u) return FALSE;
    if (num_steps > 256) num_steps = 256;

    if (!(frames = calloc(num_steps, sizeof(*frames)))) return FALSE;

    for (i = 0; i < num_steps; i++)
    {
        HCURSOR frame = NtUserGetCursorFrameInfo(hcursor, i, &delay_jiffies, &num_steps);

        if (!frame ||
            !(frames[i].shm_buffer = create_cursor_image_buffer(frame, scale,
                                                                &frames[i].hotspot_x,
                                                                &frames[i].hotspot_y)))
        {
            WARN("Failed to create frame %u of cursor=%p, not animating\n", i, hcursor);
            cursor->frames = frames;
            cursor->num_frames = i;
            wayland_cursor_free_frames(cursor);
            return FALSE;
        }

        /* The delay is in jiffies (1/60 s). */
        frames[i].delay_ms = delay_jiffies ? max(delay_jiffies * 100 / 6, 10) : 100;
    }

    TRACE("cursor=%p has %u frames\n", hcursor, i);

    cursor->frames = frames;
    cursor->num_frames = i;
    cursor->current_frame = 0;
    cursor->shm_buffer = frames[0].shm_buffer;
    wayland_shm_buffer_ref(cursor->shm_buffer);
    cursor->hotspot_x = frames[0].hotspot_x;
    cursor->hotspot_y = frames[0].hotspot_y;
    return TRUE;
}

static void wayland_pointer_update_cursor_buffer(HCURSOR hcursor, double scale)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    /* Create new buffers for the specified cursor. */
    wayland_cursor_free_frames(cursor);
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }

    if (!hcursor) return;

    if (wayland_cursor_create_frames(cursor, hcursor, scale)) return;

    cursor->shm_buffer = create_cursor_image_buffer(hcursor, scale,
                                                    &cursor->hotspot_x,
                                                    &cursor->hotspot_y);

    if (cursor->shm_buffer && cursor_buffer_is_transparent(cursor->shm_buffer))
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }
}

static void wayland_pointer_clear_cursor_surface(void)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (cursor->wp_viewport)
    {
        wp_viewport_destroy(cursor->wp_viewport);
        cursor->wp_viewport = NULL;
    }
    if (cursor->wl_surface)
    {
        wl_surface_destroy(cursor->wl_surface);
        cursor->wl_surface = NULL;
    }
    if (cursor->shm_buffer)
    {
        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = NULL;
    }
    wayland_cursor_free_frames(cursor);
}

static void wayland_pointer_update_cursor_surface(double scale)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (!cursor->shm_buffer) goto clear_cursor;

    cursor->scale = scale;

    if (!cursor->wl_surface)
    {
        cursor->wl_surface =
            wl_compositor_create_surface(process_wayland.wl_compositor);
        if (!cursor->wl_surface)
        {
            ERR("Failed to create wl_surface for cursor\n");
            goto clear_cursor;
        }
    }

    if (!cursor->wp_viewport)
    {
        cursor->wp_viewport =
            wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                       cursor->wl_surface);
        if (!cursor->wp_viewport)
        {
            ERR("Failed to create wp_viewport for cursor\n");
            goto clear_cursor;
        }
    }

    /* Commit the cursor buffer to the cursor surface. */
    wl_surface_attach(cursor->wl_surface,
                      cursor->shm_buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(cursor->wl_surface, 0, 0,
                             cursor->shm_buffer->width,
                             cursor->shm_buffer->height);
    /* Setting only the viewport is enough, but some compositors don't
     * support wp_viewport for cursor surfaces, so also set the buffer
     * scale. Note that setting the viewport destination overrides
     * the buffer scale, so it's fine to set both. */
    wl_surface_set_buffer_scale(cursor->wl_surface, round(scale));
    wp_viewport_set_destination(cursor->wp_viewport,
                                round(cursor->shm_buffer->width / scale),
                                round(cursor->shm_buffer->height / scale));
    wl_surface_commit(cursor->wl_surface);

    return;

clear_cursor:
    wayland_pointer_clear_cursor_surface();
}

/* Animated cursors: the frames are rendered when the cursor is set, and this
 * thread attaches them to the cursor surface in turn. It only uses Wayland
 * objects and the prerendered buffers, all under the pointer mutex. */
static pthread_once_t cursor_animation_once = PTHREAD_ONCE_INIT;
static pthread_cond_t cursor_animation_cond;
static BOOL cursor_animation_ok;
static struct timespec cursor_animation_deadline;

static void timespec_add_ms(struct timespec *ts, UINT ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
}

static BOOL cursor_animation_active(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    return pointer->cursor.num_frames > 1 && pointer->cursor.wl_surface &&
           pointer->wl_pointer && pointer->focused_hwnd;
}

static void *cursor_animation_thread(void *arg)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_cursor *cursor = &pointer->cursor;
    struct wayland_cursor_frame *frame;
    struct timespec now;
    sigset_t sigset;
    UINT delay_ms;

    /* Leave signals to the Wine threads. */
    sigfillset(&sigset);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    pthread_mutex_lock(&pointer->mutex);
    for (;;)
    {
        if (!cursor_animation_active())
        {
            pthread_cond_wait(&cursor_animation_cond, &pointer->mutex);
            continue;
        }

        /* Woken early when the cursor changed: start over with the new state. */
        if (pthread_cond_timedwait(&cursor_animation_cond, &pointer->mutex,
                                   &cursor_animation_deadline) != ETIMEDOUT)
            continue;
        if (!cursor_animation_active()) continue;

        cursor->current_frame = (cursor->current_frame + 1) % cursor->num_frames;
        frame = &cursor->frames[cursor->current_frame];
        /* Updating the surface frees the frames if it fails. */
        delay_ms = frame->delay_ms;

        wayland_shm_buffer_unref(cursor->shm_buffer);
        cursor->shm_buffer = frame->shm_buffer;
        wayland_shm_buffer_ref(cursor->shm_buffer);

        if (frame->hotspot_x != cursor->hotspot_x || frame->hotspot_y != cursor->hotspot_y)
        {
            cursor->hotspot_x = frame->hotspot_x;
            cursor->hotspot_y = frame->hotspot_y;
            wl_pointer_set_cursor(pointer->wl_pointer, pointer->enter_serial,
                                  cursor->wl_surface, cursor->hotspot_x,
                                  cursor->hotspot_y);
        }
        wayland_pointer_update_cursor_surface(cursor->scale);
        wl_display_flush(process_wayland.wl_display);

        /* Don't try to catch up on frames we were too late for. */
        clock_gettime(CLOCK_MONOTONIC, &now);
        timespec_add_ms(&cursor_animation_deadline, delay_ms);
        if (cursor_animation_deadline.tv_sec < now.tv_sec ||
            (cursor_animation_deadline.tv_sec == now.tv_sec &&
             cursor_animation_deadline.tv_nsec < now.tv_nsec))
        {
            cursor_animation_deadline = now;
            timespec_add_ms(&cursor_animation_deadline, delay_ms);
        }
    }

    return NULL;
}

static void cursor_animation_init(void)
{
    pthread_condattr_t attr;
    pthread_attr_t thread_attr;
    pthread_t thread;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cursor_animation_cond, &attr);
    pthread_condattr_destroy(&attr);

    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&thread, &thread_attr, cursor_animation_thread, NULL))
        cursor_animation_ok = TRUE;
    else
        ERR("Failed to create the cursor animation thread\n");
    pthread_attr_destroy(&thread_attr);
}

/* Called with the pointer mutex held, after the cursor surface has been set. */
static void wayland_pointer_restart_cursor_animation(void)
{
    struct wayland_cursor *cursor = &process_wayland.pointer.cursor;

    if (cursor->num_frames <= 1) return;

    pthread_once(&cursor_animation_once, cursor_animation_init);
    if (!cursor_animation_ok) return;

    clock_gettime(CLOCK_MONOTONIC, &cursor_animation_deadline);
    timespec_add_ms(&cursor_animation_deadline,
                    cursor->frames[cursor->current_frame].delay_ms);
    pthread_cond_signal(&cursor_animation_cond);
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect)) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static enum wp_cursor_shape_device_v1_shape cursor_shape_from_info(ICONINFOEXW *info,
                                                                   uint32_t proto_version)
{
    const struct system_cursors *cursors;
    const WCHAR *module;
    unsigned int i;
    enum wp_cursor_shape_device_v1_shape shape = 0;

    if (!info->szModName[0]) return 0;
    if ((module = wcsrchr(info->szModName, '\\'))) module++;
    else module = info->szModName;
    for (i = 0; i < ARRAY_SIZE(module_cursors); i++)
        if (!wcsicmp(module, module_cursors[i].name)) break;
    if (i == ARRAY_SIZE(module_cursors)) return 0;

    cursors = module_cursors[i].cursors;
    for (i = 0; cursors[i].id; i++)
    {
        if (cursors[i].id == info->wResID)
        {
            shape = cursors[i].shape;
            break;
        }
    }

    if (shape >= WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DND_ASK && proto_version < 2)
        shape = 0;

    return shape;
}

static BOOL wayland_pointer_set_cursor_shape(HCURSOR hcursor)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    ICONINFOEXW info = {0};
    enum wp_cursor_shape_device_v1_shape shape = 0;
    uint32_t proto_version;

    if (!process_wayland.wp_cursor_shape_manager_v1) return FALSE;
    if (!hcursor) return FALSE;
    if (!get_icon_info(hcursor, &info)) return FALSE;
    proto_version = wp_cursor_shape_manager_v1_get_version(
        process_wayland.wp_cursor_shape_manager_v1);
    shape = cursor_shape_from_info(&info, proto_version);

    if (info.hbmColor) NtGdiDeleteObjectApp(info.hbmColor);
    if (info.hbmMask) NtGdiDeleteObjectApp(info.hbmMask);

    if (!shape) return FALSE;

    if (!pointer->wp_cursor_shape_device_v1)
    {
        pointer->wp_cursor_shape_device_v1 =
            wp_cursor_shape_manager_v1_get_pointer(
                process_wayland.wp_cursor_shape_manager_v1, pointer->wl_pointer);
        if (!pointer->wp_cursor_shape_device_v1) return FALSE;
    }

    wp_cursor_shape_device_v1_set_shape(pointer->wp_cursor_shape_device_v1,
                                        pointer->enter_serial, shape);

    return TRUE;
}

static void wayland_pointer_clear_cursor_shape(void)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    if (pointer->wp_cursor_shape_device_v1)
    {
        wp_cursor_shape_device_v1_destroy(pointer->wp_cursor_shape_device_v1);
        pointer->wp_cursor_shape_device_v1 = NULL;
    }
}

static void wayland_set_cursor(HWND hwnd, HCURSOR hcursor, BOOL use_hcursor)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    double scale;
    BOOL reapply_clip = FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (!(surface = data->wayland_surface))
        {
            wayland_win_data_release(data);
            return;
        }
        scale = surface->window.scale;
        if (use_hcursor) surface->hcursor = hcursor;
        else hcursor = surface->hcursor;
        use_hcursor = TRUE;
        wayland_win_data_release(data);
    }
    else
    {
        scale = 1.0;
    }

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->focused_hwnd == hwnd)
    {
        if ((!use_hcursor && pointer->wp_cursor_shape_device_v1) ||
            (use_hcursor && hcursor && wayland_pointer_set_cursor_shape(hcursor)))
        {
            wayland_pointer_clear_cursor_surface();
        }
        else
        {
            if (use_hcursor) wayland_pointer_update_cursor_buffer(hcursor, scale);
            wayland_pointer_update_cursor_surface(scale);
            wl_pointer_set_cursor(pointer->wl_pointer,
                                  pointer->enter_serial,
                                  pointer->cursor.wl_surface,
                                  pointer->cursor.hotspot_x,
                                  pointer->cursor.hotspot_y);
            wayland_pointer_clear_cursor_shape();
            wayland_pointer_restart_cursor_animation();
        }
        wl_display_flush(process_wayland.wl_display);
        reapply_clip = TRUE;
    }
    pthread_mutex_unlock(&pointer->mutex);

    /* Reapply cursor clip since cursor visibility affects pointer constraint
     * behavior. */
    if (reapply_clip) reapply_cursor_clipping();
}

/**********************************************************************
 *          wayland_surface_calc_confine
 *
 * Calculates the pointer confine rect (in surface-local coords)
 * for the specified clip rectangle (in screen coords using thread dpi).
 */
static void wayland_surface_calc_confine(struct wayland_surface *surface,
                                         const RECT *clip, RECT *confine)
{
    RECT window_clip;

    TRACE("hwnd=%p clip=%s window=%s\n",
          surface->hwnd, wine_dbgstr_rect(clip),
          wine_dbgstr_rect(&surface->window.rect));

    /* FIXME: surface->window.(client_)rect is in window dpi, whereas
     * clip is in thread dpi. */

    if (!intersect_rect(&window_clip, clip, &surface->window.rect))
    {
        SetRectEmpty(confine);
        return;
    }

    OffsetRect(&window_clip,
               -surface->window.rect.left,
               -surface->window.rect.top);
    wayland_surface_coords_from_window(surface,
                                       window_clip.left, window_clip.top,
                                       (int *)&confine->left, (int *)&confine->top);
    wayland_surface_coords_from_window(surface,
                                       window_clip.right, window_clip.bottom,
                                       (int *)&confine->right, (int *)&confine->bottom);
}

/**********************************************************************
 *          wayland_surface_client_covers_vscreen
 *
 * Whether a surface window client area covers the whole virtual screen.
 */
static BOOL wayland_surface_client_covers_vscreen(struct wayland_surface *surface)
{
    RECT vscreen_rect, rect;

    /* Get individual system metrics to get coords in thread dpi
     * (NtUserGetVirtualScreenRect would return values in system dpi). */
    vscreen_rect.left = NtUserGetSystemMetrics(SM_XVIRTUALSCREEN);
    vscreen_rect.top = NtUserGetSystemMetrics(SM_YVIRTUALSCREEN);
    vscreen_rect.right = vscreen_rect.left +
                         NtUserGetSystemMetrics(SM_CXVIRTUALSCREEN);
    vscreen_rect.bottom = vscreen_rect.top +
                          NtUserGetSystemMetrics(SM_CYVIRTUALSCREEN);

    /* FIXME: surface->window.client_rect is in window dpi, whereas
     * vscreen_rect is in thread dpi. */
    intersect_rect(&rect, &surface->window.client_rect, &vscreen_rect);

    return EqualRect(&vscreen_rect, &rect);
}

/***********************************************************************
 *           wayland_pointer_update_constraint
 *
 *  Enables/disables pointer confinement.
 */
static void wayland_pointer_update_constraint(struct wl_surface *wl_surface,
                                              RECT *confine_rect,
                                              BOOL covers_vscreen,
                                              BOOL force_lock)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    BOOL needs_relative, needs_lock, needs_confine, is_visible;
    static unsigned int once;

    if (!process_wayland.zwp_pointer_constraints_v1)
    {
        if (!once++)
            ERR("This function requires zwp_pointer_constraints_v1\n");
        return;
    }

    is_visible = pointer->cursor.wl_surface || pointer->wp_cursor_shape_device_v1;
    needs_lock = wl_surface &&
                 (((confine_rect || covers_vscreen) && !is_visible) || force_lock) &&
                 pointer->wl_pointer;
    needs_confine = wl_surface && confine_rect && is_visible && !force_lock &&
                    pointer->wl_pointer;

    if (!needs_confine && pointer->zwp_confined_pointer_v1)
    {
        TRACE("Unconfining from hwnd=%p\n", pointer->constraint_hwnd);
        zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
        pointer->zwp_confined_pointer_v1 = NULL;
        pointer->constraint_hwnd = NULL;
    }

    if (!needs_lock && pointer->zwp_locked_pointer_v1)
    {
        TRACE("Unlocking from hwnd=%p\n", pointer->constraint_hwnd);
        zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
        pointer->zwp_locked_pointer_v1 = NULL;
        pointer->constraint_hwnd = NULL;
    }

    if (needs_confine)
    {
        HWND hwnd = wl_surface_get_user_data(wl_surface);
        struct wl_region *region;

        region = wl_compositor_create_region(process_wayland.wl_compositor);
        wl_region_add(region, confine_rect->left, confine_rect->top,
                      confine_rect->right - confine_rect->left,
                      confine_rect->bottom - confine_rect->top);

        if (!pointer->zwp_confined_pointer_v1 || pointer->constraint_hwnd != hwnd)
        {
            if (pointer->zwp_confined_pointer_v1)
                zwp_confined_pointer_v1_destroy(pointer->zwp_confined_pointer_v1);
            pointer->zwp_confined_pointer_v1 =
                zwp_pointer_constraints_v1_confine_pointer(
                    process_wayland.zwp_pointer_constraints_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    region,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            pointer->constraint_hwnd = hwnd;
        }
        else
        {
            zwp_confined_pointer_v1_set_region(pointer->zwp_confined_pointer_v1,
                                               region);
        }

        TRACE("Confining to hwnd=%p wayland=%d,%d+%d,%d\n",
              pointer->constraint_hwnd,
              confine_rect->left, confine_rect->top,
              confine_rect->right - confine_rect->left,
              confine_rect->bottom - confine_rect->top);

        wl_region_destroy(region);
    }
    else if (needs_lock)
    {
        HWND hwnd = wl_surface_get_user_data(wl_surface);

        if (!pointer->zwp_locked_pointer_v1 || pointer->constraint_hwnd != hwnd)
        {
            if (pointer->zwp_locked_pointer_v1)
                zwp_locked_pointer_v1_destroy(pointer->zwp_locked_pointer_v1);
            pointer->zwp_locked_pointer_v1 =
                zwp_pointer_constraints_v1_lock_pointer(
                    process_wayland.zwp_pointer_constraints_v1,
                    wl_surface,
                    pointer->wl_pointer,
                    NULL,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            pointer->constraint_hwnd = hwnd;
            TRACE("Locking to hwnd=%p\n", pointer->constraint_hwnd);
        }
    }

    if (!process_wayland.zwp_relative_pointer_manager_v1)
    {
        if (!once++)
            ERR("zwp_relative_pointer_manager_v1 isn't supported, skipping relative motion\n");
        return;
    }

    needs_relative = !is_visible && pointer->constraint_hwnd &&
                     pointer->constraint_hwnd == pointer->focused_hwnd;

    if (needs_relative && !pointer->zwp_relative_pointer_v1)
    {
        pointer->accum_x = pointer->accum_y = 0;
        pointer->zwp_relative_pointer_v1 =
            zwp_relative_pointer_manager_v1_get_relative_pointer(
                process_wayland.zwp_relative_pointer_manager_v1,
                pointer->wl_pointer);
        zwp_relative_pointer_v1_add_listener(pointer->zwp_relative_pointer_v1,
                                             &relative_pointer_v1_listener, NULL);
        TRACE("Enabling relative motion\n");
    }
    else if (!needs_relative && pointer->zwp_relative_pointer_v1)
    {
        zwp_relative_pointer_v1_destroy(pointer->zwp_relative_pointer_v1);
        pointer->zwp_relative_pointer_v1 = NULL;
        TRACE("Disabling relative motion\n");
    }
}

void wayland_pointer_clear_constraint(void)
{
    wayland_pointer_update_constraint(NULL, NULL, FALSE, FALSE);
}

/***********************************************************************
 *           WAYLAND_SetCursor
 */
void WAYLAND_SetCursor(HWND hwnd, HCURSOR hcursor)
{
    TRACE("hwnd=%p hcursor=%p\n", hwnd, hcursor);

    wayland_set_cursor(hwnd, hcursor, TRUE);
}

/***********************************************************************
 *           WAYLAND_SetCursorPos
 */
BOOL WAYLAND_SetCursorPos(INT x, INT y)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;

    pthread_mutex_lock(&pointer->mutex);
    if (pointer->zwp_relative_pointer_v1)
    {
        pthread_mutex_unlock(&pointer->mutex);
        return FALSE;
    }
    pointer->pending_warp = TRUE;
    pthread_mutex_unlock(&pointer->mutex);

    TRACE("warping to %d,%d\n", x, y);
    reapply_cursor_clipping();
    return TRUE;
}

/***********************************************************************
 *	     WAYLAND_ClipCursor
 */
BOOL WAYLAND_ClipCursor(const RECT *clip, BOOL reset)
{
    struct wayland_pointer *pointer = &process_wayland.pointer;
    HWND hwnd;
    struct wl_surface *wl_surface = NULL;
    struct wayland_surface *surface = NULL;
    struct wayland_win_data *data;
    BOOL covers_vscreen = FALSE;
    RECT confine_rect;
    POINT cursor_pos;
    int warp_x, warp_y;

    TRACE("clip=%s reset=%d\n", wine_dbgstr_rect(clip), reset);

    NtUserGetCursorPos(&cursor_pos);
    hwnd = NtUserGetForegroundWindow();

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    if ((surface = data->wayland_surface))
    {
        wl_surface = surface->wl_surface;
        if (clip) wayland_surface_calc_confine(surface, clip, &confine_rect);
        covers_vscreen = wayland_surface_client_covers_vscreen(surface);
        wayland_surface_coords_from_window(surface,
                cursor_pos.x - surface->window.rect.left,
                cursor_pos.y - surface->window.rect.top,
                &warp_x, &warp_y);
    }
    wayland_win_data_release(data);

    pthread_mutex_lock(&pointer->mutex);
    if (wl_surface && pointer->pending_warp)
    {
        wayland_pointer_update_constraint(wl_surface, NULL, FALSE, TRUE);
        pointer->pending_warp = FALSE;
    }

    if (wl_surface && hwnd == pointer->constraint_hwnd && pointer->zwp_locked_pointer_v1)
    {
        zwp_locked_pointer_v1_set_cursor_position_hint(
                pointer->zwp_locked_pointer_v1,
                wl_fixed_from_int(warp_x),
                wl_fixed_from_int(warp_y));
        pthread_mutex_unlock(&pointer->mutex);

        data = wayland_win_data_get(hwnd);
        wl_surface_commit(wl_surface);
        wayland_win_data_release(data);
        TRACE("position hint hwnd=%p wayland_xy=%d,%d screen_xy=%d,%d\n",
                hwnd, warp_x, warp_y, cursor_pos.x, cursor_pos.y);
        pthread_mutex_lock(&pointer->mutex);
    }

   /* Since we are running in the context of the foreground thread we know
    * that the wl_surface of the foreground HWND will not be invalidated,
    * so we can access it without having the win data lock. */
    wayland_pointer_update_constraint(wl_surface,
                                      (clip && wl_surface) ? &confine_rect : NULL,
                                      covers_vscreen,
                                      FALSE);
    pthread_mutex_unlock(&pointer->mutex);

    /* Constraint state belongs to the parent surface, not the independently
     * presented client. Flush alone does not apply double-buffered state.
     * Keep the earlier hint commit before unlocking the pointer as well. */
    if (wl_surface && (data = wayland_win_data_get(hwnd)))
    {
        if (data->wayland_surface && data->wayland_surface->wl_surface == wl_surface)
            wl_surface_commit(wl_surface);
        wayland_win_data_release(data);
    }

    wl_display_flush(process_wayland.wl_display);

    return TRUE;
}
