/*
 * Wayland surfaces
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

#include "waylanddrv.h"
#include "dxgi1_2.h"
#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

static LONG wayland_surface_serial_counter;

static void wayland_surface_clear_direct_dmabuf(struct wayland_surface *surface,
                                                struct wayland_win_data *data);
void wayland_surface_update_hwnd_dmabufs(struct wayland_surface *surface);
static BOOL wayland_surface_try_direct_dmabuf(HWND hwnd);
static void wayland_client_surface_stack(struct wayland_surface *surface,
                                         struct wayland_client_surface *client,
                                         BOOL stack_above_parent);

struct wayland_child_visibility_info
{
    enum wayland_child_visibility visibility;
    RECT rect;
    unsigned int rect_count;
};

static HRGN create_child_region(HRGN shape_region)
{
    HRGN region;

    if (!shape_region) return 0;
    if (!(region = NtGdiCreateRectRgn(0, 0, 0, 0))) return 0;

    if (NtGdiCombineRgn(region, shape_region, 0, RGN_COPY) == ERROR)
    {
        NtGdiDeleteObjectApp(region);
        return 0;
    }

    return region;
}

static void wayland_surface_set_region_constraints(struct wayland_surface *surface,
                                                   HRGN shape_region, HRGN clip_region)
{
    if (surface->child_region) NtGdiDeleteObjectApp(surface->child_region);
    surface->child_region = create_child_region(shape_region);
    surface->shaped = shape_region != 0;
    surface->occlusion_clipped = clip_region != 0 &&
        (!shape_region || !NtGdiEqualRgn(clip_region, shape_region));
}

static struct wl_region *wayland_surface_create_shape_input_region(struct wayland_surface *surface,
                                                                   HRGN shape_region)
{
    struct wl_region *region;
    RGNDATA *data;
    RECT *rect, *end;

    if (!(region = wl_compositor_create_region(process_wayland.wl_compositor)))
        return NULL;

    if (!(data = get_region_data(shape_region)))
    {
        wl_region_destroy(region);
        return NULL;
    }

    rect = (RECT *)data->Buffer;
    end = rect + data->rdh.nCount;
    for (; rect < end; rect++)
    {
        int left, top, right, bottom;

        wayland_surface_coords_from_window(surface, rect->left, rect->top, &left, &top);
        wayland_surface_coords_from_window(surface, rect->right, rect->bottom, &right, &bottom);
        if (right > left && bottom > top)
            wl_region_add(region, left, top, right - left, bottom - top);
    }

    free(data);
    return region;
}

static void wayland_surface_sync_shape_input_region(struct wayland_surface *surface, HRGN shape_region)
{
    DWORD exstyle = NtUserGetWindowLongW(surface->hwnd, GWL_EXSTYLE);
    BOOL transparent = (exstyle & WS_EX_TRANSPARENT) && (exstyle & WS_EX_LAYERED);
    struct wl_region *region = NULL;

    if (transparent)
    {
        region = wl_compositor_create_region(process_wayland.wl_compositor);
        if (region) wl_surface_set_input_region(surface->wl_surface, region);
    }
    else if (shape_region)
    {
        region = wayland_surface_create_shape_input_region(surface, shape_region);
        if (region) wl_surface_set_input_region(surface->wl_surface, region);
    }
    else wl_surface_set_input_region(surface->wl_surface, NULL);

    if (region) wl_region_destroy(region);
}

static HRGN wayland_surface_get_window_shape_region(HWND hwnd)
{
    HRGN shape_region = NtGdiCreateRectRgn(0, 0, 0, 0);

    if (!shape_region) return 0;
    if (NtUserGetWindowRgnEx(hwnd, shape_region, 0) == ERROR)
    {
        NtGdiDeleteObjectApp(shape_region);
        return 0;
    }

    return shape_region;
}

void wayland_surface_sync_window_regions(struct wayland_surface *surface,
                                         struct window_surface *window_surface)
{
    HRGN shape_region, owned_shape_region = 0;

    if (window_surface && window_surface->shape_region)
        shape_region = window_surface->shape_region;
    else
        shape_region = owned_shape_region =
            wayland_surface_get_window_shape_region(surface->hwnd);

    wayland_surface_sync_shape_input_region(surface, shape_region);
    wayland_surface_set_region_constraints(surface, shape_region,
                                           window_surface ? window_surface->clip_region : 0);

    if (owned_shape_region) NtGdiDeleteObjectApp(owned_shape_region);
}

static void request_window_surface_expose(HWND hwnd, BOOL allow_inline)
{
    /* The direct dmabuf fast path may run inline from event callbacks because
     * it only commits Wayland state under win_data_mutex. It must not enter the
     * win32u window_surface flush path, which has its own lock ordering. */
    if (wayland_surface_try_direct_dmabuf(hwnd)) return;

    /* Inline exposes preserve the initial-configure path for windows that draw
     * without pumping messages. */
    if (allow_inline)
    {
        NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
        return;
    }

    NtUserPostMessage(hwnd, WM_WAYLAND_EXPOSE, 0, 0);
}

struct wayland_hwnd_dmabuf_surface;

struct wayland_hwnd_dmabuf_buffer
{
    struct wl_list link;
    struct wayland_hwnd_dmabuf_surface *surface;
    struct wl_buffer *wl_buffer;
    UINT64 producer_unique_id;
    UINT64 release_token;
    hwnd_dmabuf_frame_desc_t desc;
    UINT64 modifier;              /* cached-slot layout identity */
    unsigned int image_id;        /* producer ring slot. Cache key (stable-slot path) */
    unsigned int ring_generation; /* swapchain-rebuild counter. Invalidates a cached slot */
    unsigned int fourcc;
    unsigned int stride;
    unsigned int offset;
    unsigned int alpha_mode;
    unsigned int dirty_count;
    unsigned short dirty_rects[HWND_DMABUF_MAX_DIRTY_RECTS][4];
    int width;
    int height;
    LONG ref;          /* owner ref + one ref per outstanding compositor commit */
    LONG commit_refs;  /* outstanding compositor commits holding this buffer */
    LONG released;     /* uncached path: set by wl_buffer.release, reaped next pass */
    LONG cache_valid;  /* stable-slot cache entry is still retained by this surface */
    BOOL stable_slot;  /* cached and reused per slot. Release token sent on release */
    unsigned int release_flags;
    int channel_fd;    /* dup of surface->channel_fd for sending release tokens */
    int data_fd;       /* dup of the producer backing fd, for per-slice wrappers */
};

struct wayland_hwnd_dmabuf_slice_buffer
{
    struct wayland_hwnd_dmabuf_buffer *buffer;
};

struct wayland_hwnd_dmabuf_slice_geometry
{
    int x, y;
    int width, height;
    wl_fixed_t source_x, source_y;
    wl_fixed_t source_width, source_height;
};

struct wayland_hwnd_dmabuf_slice
{
    struct wl_list link;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct wp_viewport *wp_viewport;
    struct wayland_hwnd_dmabuf_slice_geometry geometry;
    BOOL seen;
    BOOL geometry_valid;
};

enum wayland_hwnd_dmabuf_consumer_state
{
    WAYLAND_HWNDDMABUF_CONSUMER_UNKNOWN,
    WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE,
    WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED,
};

struct wayland_hwnd_dmabuf_surface
{
    struct wl_list link;
    HWND hwnd;
    struct wayland_surface *parent;
    struct wl_surface *wl_surface;
    struct wl_subsurface *wl_subsurface;
    struct wp_viewport *wp_viewport;
    struct wayland_hwnd_dmabuf_buffer *current;
    struct wl_list buffers;
    struct wl_list slices;
    UINT frame_seq;
    UINT current_attach_count;
    UINT slice_count;
    BOOL current_committed;
    BOOL logged_first_attach;
    BOOL logged_first_import;
    BOOL seen;
    BOOL gdi_overlay;
    BOOL direct;
    BOOL linked;
    BOOL sliced;
    unsigned long long last_seen_ms; /* tick when last present in the producer list */
    int committed_width, committed_height;
    int channel_fd;                 /* consumer end of the producer socket, or -1 */
    enum wayland_hwnd_dmabuf_consumer_state consumer_state;
    struct wl_surface *stack_bottom;
    struct wl_surface *slice_layout_sibling;
    struct wayland_visual_constraint visual_constraint;
};

enum wayland_hwnd_dmabuf_configure_result
{
    WAYLAND_HWNDDMABUF_CONFIGURE_FAILED,
    WAYLAND_HWNDDMABUF_CONFIGURE_NOOP,
    WAYLAND_HWNDDMABUF_CONFIGURE_UPDATED,
};

/* A child may briefly drop out of the descendant list between frames. Tearing its surface
 * (and dmabuf cache) down on a single miss churns the cache and, with fd-once, strands slots
 * the producer thinks are still cached. Keep an unseen surface for a grace window first. */
#define WAYLAND_DMABUF_SURFACE_GRACE_MS 1000
#define WAYLAND_DMABUF_MAX_SLICES 512
#define POPUP_GRAB_SERIAL_TIMEOUT_MS 1000

unsigned long long wayland_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static unsigned int wayland_hwnd_dmabuf_buffer_cache_flags(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    if (buffer->stable_slot && InterlockedCompareExchange(&buffer->cache_valid, FALSE, FALSE))
        return HWND_DMABUF_RELEASE_CACHED;
    return 0;
}

static UINT64 wayland_hwnd_dmabuf_buffer_exchange_release_token(struct wayland_hwnd_dmabuf_buffer *buffer,
                                                                UINT64 release_token)
{
    LONGLONG volatile *token = (LONGLONG volatile *)&buffer->release_token;
    LONGLONG old;

    do old = *token;
    while (InterlockedCompareExchange64(token, release_token, old) != old);
    return old;
}

/* Send a release record, retrying EINTR. */
static int wayland_hwnd_dmabuf_channel_send_release(int channel_fd, const hwnd_dmabuf_release_t *rel)
{
    ssize_t n;

    do n = send(channel_fd, rel, sizeof(*rel), MSG_DONTWAIT | MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == sizeof(*rel) ? 0 : n < 0 ? errno : EMSGSIZE;
}

static int wayland_hwnd_dmabuf_surface_send_consumer_state(
        struct wayland_hwnd_dmabuf_surface *surface, unsigned int flag)
{
    hwnd_dmabuf_release_t rel = { 0, 0, flag, 0, 0, 0 };

    if (surface->channel_fd < 0) return ENOTCONN;
    return wayland_hwnd_dmabuf_channel_send_release(surface->channel_fd, &rel);
}

static void wayland_hwnd_dmabuf_buffer_send_release(struct wayland_hwnd_dmabuf_buffer *buffer,
                                                    unsigned int flags, BOOL keep_token)
{
    UINT64 release_token;

    if (buffer->channel_fd < 0) return;
    release_token = wayland_hwnd_dmabuf_buffer_exchange_release_token(buffer, 0);
    if (release_token)
    {
        hwnd_dmabuf_release_t rel = { buffer->producer_unique_id, release_token,
                                      flags | wayland_hwnd_dmabuf_buffer_cache_flags(buffer),
                                      buffer->image_id, buffer->ring_generation, 0 };
        if (keep_token && wayland_hwnd_dmabuf_channel_send_release(buffer->channel_fd, &rel))
            InterlockedCompareExchange64((LONGLONG volatile *)&buffer->release_token, release_token, 0);
        else if (!keep_token)
            wayland_hwnd_dmabuf_channel_send_release(buffer->channel_fd, &rel);
    }
}

/* Drop a buffer reference. The last unref destroys the wl_buffer and frees.
 * Lock-free, may run on the present thread or the event thread (inside the
 * wl_buffer.release handler). wl_buffer_destroy never races a release dispatch. */
static void wayland_hwnd_dmabuf_buffer_unref(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    if (InterlockedDecrement(&buffer->ref) > 0) return;
    wayland_hwnd_dmabuf_buffer_send_release(buffer, buffer->release_flags ?
                                            buffer->release_flags : HWND_DMABUF_RELEASE_ORPHANED, FALSE);
    if (buffer->channel_fd >= 0) close(buffer->channel_fd);
    if (buffer->data_fd >= 0) close(buffer->data_fd);
    if (buffer->wl_buffer) wl_buffer_destroy(buffer->wl_buffer);
    free(buffer);
}

/* Return a release token to the producer. */
static void wayland_hwnd_dmabuf_send_release(struct wayland_hwnd_dmabuf_surface *surface,
                                             UINT64 producer_unique_id, UINT64 release_token,
                                             unsigned int flags, unsigned int image_id,
                                             unsigned int ring_generation)
{
    hwnd_dmabuf_release_t rel = { producer_unique_id, release_token, flags, image_id, ring_generation, 0 };

    /* One-shot reject/orphan releases are best effort. */
    if (release_token && surface->channel_fd >= 0)
        wayland_hwnd_dmabuf_channel_send_release(surface->channel_fd, &rel);
}

static void wayland_hwnd_dmabuf_surface_clear_slices(struct wayland_hwnd_dmabuf_surface *surface);
static struct wayland_hwnd_dmabuf_slice *wayland_hwnd_dmabuf_surface_get_slice(
        struct wayland_hwnd_dmabuf_surface *surface);

/* Present-thread teardown: detach from the surface and drop the owner ref.
 * Must be called with win_data_mutex held.
 * If the compositor still holds the buffer it survives as an orphan until its
 * release handler drops the last ref. */
static void wayland_hwnd_dmabuf_buffer_reap(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    struct wayland_hwnd_dmabuf_surface *surface = buffer->surface;

    if (surface && surface->current == buffer)
    {
        surface->current = NULL;
        surface->current_committed = FALSE;
        wayland_hwnd_dmabuf_surface_clear_slices(surface);
    }
    InterlockedExchange(&buffer->cache_valid, FALSE);
    wl_list_remove(&buffer->link);
    buffer->surface = NULL;
    wayland_hwnd_dmabuf_buffer_unref(buffer);
}

/* wl_buffer.release runs on the event thread: lock-free only, no surface/list
 * mutation. Cached stable-slot buffers stay cached for the slot's next frame.
 * Send the release token now to recycle the slot. The busy gate blocks the
 * producer from overwriting it until then. Uncached buffers are flagged for the
 * present thread to reap next pass (token sent on destroy). Either way drop the
 * per-commit ref. */
static void wayland_hwnd_dmabuf_buffer_handle_release(void *data, struct wl_buffer *wl_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = data;
    LONG old, remaining;

    do
    {
        old = InterlockedCompareExchange(&buffer->commit_refs, 0, 0);
        if (old <= 0) return;
        remaining = old - 1;
    } while (InterlockedCompareExchange(&buffer->commit_refs, remaining, old) != old);

    if (buffer->stable_slot && remaining == 0)
        wayland_hwnd_dmabuf_buffer_send_release(buffer, HWND_DMABUF_RELEASE_PRESENTED, TRUE);
    else if (!buffer->stable_slot)
    {
        buffer->release_flags = HWND_DMABUF_RELEASE_PRESENTED;
        InterlockedExchange(&buffer->released, TRUE);
    }
    wayland_hwnd_dmabuf_buffer_unref(buffer);
}

static const struct wl_buffer_listener wayland_hwnd_dmabuf_buffer_listener =
{
    wayland_hwnd_dmabuf_buffer_handle_release
};

static void wayland_hwnd_dmabuf_slice_buffer_handle_release(void *data, struct wl_buffer *wl_buffer)
{
    struct wayland_hwnd_dmabuf_slice_buffer *slice_buffer = data;

    wayland_hwnd_dmabuf_buffer_handle_release(slice_buffer->buffer, wl_buffer);
    wl_buffer_destroy(wl_buffer);
    free(slice_buffer);
}

static const struct wl_buffer_listener wayland_hwnd_dmabuf_slice_buffer_listener =
{
    wayland_hwnd_dmabuf_slice_buffer_handle_release
};

static BOOL wayland_hwnd_dmabuf_desc_is_shm(const hwnd_dmabuf_frame_desc_t *desc);
static struct wl_buffer *wayland_hwnd_dmabuf_create_shm_wl_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc, int fd);
static struct wl_buffer *wayland_hwnd_dmabuf_create_dmabuf_wl_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc, int fd);

static void wayland_hwnd_dmabuf_buffer_add_commit_ref(struct wayland_hwnd_dmabuf_buffer *buffer)
{
    InterlockedIncrement(&buffer->ref);
    InterlockedIncrement(&buffer->commit_refs);
}

static struct wl_buffer *wayland_hwnd_dmabuf_buffer_create_slice_wl_buffer(
        struct wayland_hwnd_dmabuf_buffer *buffer)
{
    struct wl_buffer *wl_buffer;
    int fd;

    if (buffer->data_fd < 0) return NULL;
    if ((fd = dup(buffer->data_fd)) < 0) return NULL;

    if (wayland_hwnd_dmabuf_desc_is_shm(&buffer->desc))
        wl_buffer = wayland_hwnd_dmabuf_create_shm_wl_buffer(buffer->surface, &buffer->desc, fd);
    else
        wl_buffer = wayland_hwnd_dmabuf_create_dmabuf_wl_buffer(buffer->surface, &buffer->desc, fd);
    close(fd);
    return wl_buffer;
}

static BOOL wayland_hwnd_dmabuf_slice_attach_buffer(struct wayland_hwnd_dmabuf_slice *slice,
                                                    struct wayland_hwnd_dmabuf_buffer *buffer)
{
    struct wayland_hwnd_dmabuf_slice_buffer *slice_buffer;
    struct wl_buffer *wl_buffer;

    if (!(slice_buffer = calloc(1, sizeof(*slice_buffer)))) return FALSE;
    if (!(wl_buffer = wayland_hwnd_dmabuf_buffer_create_slice_wl_buffer(buffer)))
    {
        free(slice_buffer);
        return FALSE;
    }

    slice_buffer->buffer = buffer;
    wayland_hwnd_dmabuf_buffer_add_commit_ref(buffer);
    wl_buffer_add_listener(wl_buffer, &wayland_hwnd_dmabuf_slice_buffer_listener, slice_buffer);
    wl_surface_attach(slice->wl_surface, wl_buffer, 0, 0);
    return TRUE;
}

static void wayland_hwnd_dmabuf_slice_destroy(struct wayland_hwnd_dmabuf_slice *slice)
{
    wl_list_remove(&slice->link);
    if (slice->wl_subsurface) wl_subsurface_destroy(slice->wl_subsurface);
    if (slice->wp_viewport) wp_viewport_destroy(slice->wp_viewport);
    if (slice->wl_surface) wl_surface_destroy(slice->wl_surface);
    free(slice);
}

static void wayland_hwnd_dmabuf_surface_assert_slice_cache(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_slice *slice;
    unsigned int count = 0;

    wl_list_for_each(slice, &surface->slices, link)
        count++;

    assert(count == surface->slice_count);
    assert(surface->sliced == !!surface->slice_count);
    if (!surface->sliced) assert(!surface->slice_layout_sibling);
}

static void wayland_hwnd_dmabuf_surface_clear_slices(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_slice *slice, *next;

    wl_list_for_each_safe(slice, next, &surface->slices, link)
        wayland_hwnd_dmabuf_slice_destroy(slice);
    surface->sliced = FALSE;
    surface->slice_count = 0;
    surface->stack_bottom = surface->wl_surface;
    surface->slice_layout_sibling = NULL;
    wayland_hwnd_dmabuf_surface_assert_slice_cache(surface);
}

static BOOL wayland_hwnd_dmabuf_surface_slice_layout_matches(
        struct wayland_hwnd_dmabuf_surface *surface, struct wl_surface *sibling,
        const struct wayland_hwnd_dmabuf_slice_geometry *layout, unsigned int count)
{
    struct wayland_hwnd_dmabuf_slice *slice;
    unsigned int i = 0;

    if (!surface->sliced || surface->slice_count != count ||
        surface->slice_layout_sibling != sibling)
        return FALSE;

    wl_list_for_each(slice, &surface->slices, link)
    {
        if (i >= count || !slice->geometry_valid ||
            memcmp(&slice->geometry, &layout[i], sizeof(layout[i])))
            return FALSE;
        i++;
    }

    return i == count;
}

static struct wl_surface *wayland_hwnd_dmabuf_surface_slice_stack_bottom(
        struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_slice *slice;
    struct wl_surface *bottom = surface->wl_surface;

    wl_list_for_each(slice, &surface->slices, link)
        bottom = slice->wl_surface;
    return bottom;
}

#define WAYLAND_DMABUF_COVER_MAX_RECTS 1024
#define WAYLAND_DMABUF_COVER_MAX_CELLS (1024 * 1024)

static int compare_long(const void *a, const void *b)
{
    LONG x = *(const LONG *)a, y = *(const LONG *)b;

    return (x > y) - (x < y);
}

static unsigned int sort_unique_longs(LONG *values, unsigned int count)
{
    unsigned int i, out = 0;

    qsort(values, count, sizeof(*values), compare_long);
    for (i = 0; i < count; i++)
    {
        if (out && values[out - 1] == values[i]) continue;
        values[out++] = values[i];
    }
    return out;
}

static int find_long_index(const LONG *values, unsigned int count, LONG value)
{
    unsigned int low = 0, high = count;

    while (low < high)
    {
        unsigned int mid = low + (high - low) / 2;

        if (values[mid] == value) return mid;
        if (values[mid] < value) low = mid + 1;
        else high = mid;
    }
    return -1;
}

static RECT *wayland_hwnd_dmabuf_cover_slice_rects(const RECT *rects, unsigned int count,
                                                   unsigned int *covered_count)
{
    unsigned int i, j, x_count, y_count, x_cells, y_cells, out_count = 0;
    unsigned int remaining = 0, edge_count = count * 2;
    LONG *x_edges = NULL, *y_edges = NULL;
    RECT *out = NULL;
    unsigned char *cells = NULL;
    size_t cell_count;

    *covered_count = 0;
    if (!count || count > WAYLAND_DMABUF_COVER_MAX_RECTS) return NULL;
    if (!(x_edges = malloc(edge_count * sizeof(*x_edges)))) goto failed;
    if (!(y_edges = malloc(edge_count * sizeof(*y_edges)))) goto failed;

    for (i = 0; i < count; i++)
    {
        x_edges[i * 2] = rects[i].left;
        x_edges[i * 2 + 1] = rects[i].right;
        y_edges[i * 2] = rects[i].top;
        y_edges[i * 2 + 1] = rects[i].bottom;
    }
    x_count = sort_unique_longs(x_edges, edge_count);
    y_count = sort_unique_longs(y_edges, edge_count);
    if (x_count < 2 || y_count < 2) goto failed;

    x_cells = x_count - 1;
    y_cells = y_count - 1;
    if (x_cells && y_cells > ~(size_t)0 / x_cells) goto failed;
    cell_count = (size_t)x_cells * y_cells;
    if (cell_count > WAYLAND_DMABUF_COVER_MAX_CELLS) goto failed;
    if (!(cells = calloc(cell_count, sizeof(*cells)))) goto failed;
    if (!(out = calloc(WAYLAND_DMABUF_MAX_SLICES, sizeof(*out)))) goto failed;

    for (i = 0; i < count; i++)
    {
        int left = find_long_index(x_edges, x_count, rects[i].left);
        int right = find_long_index(x_edges, x_count, rects[i].right);
        int top = find_long_index(y_edges, y_count, rects[i].top);
        int bottom = find_long_index(y_edges, y_count, rects[i].bottom);
        int x, y;

        if (left < 0 || right < 0 || top < 0 || bottom < 0) goto failed;
        for (y = top; y < bottom; y++)
            for (x = left; x < right; x++)
            {
                size_t idx = (size_t)y * x_cells + x;

                if (cells[idx]) continue;
                cells[idx] = 1;
                remaining++;
            }
    }

    while (remaining)
    {
        unsigned int start_x = 0, start_y = 0, width, best_width = 1, best_height = 1;
        unsigned int best_area = 1;
        BOOL found = FALSE;

        for (i = 0; i < y_cells && !found; i++)
            for (j = 0; j < x_cells; j++)
                if (cells[(size_t)i * x_cells + j])
                {
                    start_y = i;
                    start_x = j;
                    found = TRUE;
                    break;
                }
        if (!found) break;

        for (width = 0; start_x + width < x_cells &&
             cells[(size_t)start_y * x_cells + start_x + width]; width++)
            ;

        for (i = start_y; i < y_cells && width; i++)
        {
            unsigned int row_width = 0, height = i - start_y + 1;
            unsigned int area;

            while (row_width < width && cells[(size_t)i * x_cells + start_x + row_width])
                row_width++;
            width = row_width;
            if (!width) break;
            area = width * height;
            if (area > best_area)
            {
                best_area = area;
                best_width = width;
                best_height = height;
            }
        }

        if (out_count == WAYLAND_DMABUF_MAX_SLICES) goto failed;
        out[out_count].left = x_edges[start_x];
        out[out_count].right = x_edges[start_x + best_width];
        out[out_count].top = y_edges[start_y];
        out[out_count].bottom = y_edges[start_y + best_height];
        out_count++;

        for (i = start_y; i < start_y + best_height; i++)
            for (j = start_x; j < start_x + best_width; j++)
            {
                size_t idx = (size_t)i * x_cells + j;

                if (!cells[idx]) continue;
                cells[idx] = 0;
                remaining--;
            }
    }

    free(cells);
    free(x_edges);
    free(y_edges);
    *covered_count = out_count;
    return out;

failed:
    free(out);
    free(cells);
    free(x_edges);
    free(y_edges);
    return NULL;
}

static void wayland_hwnd_dmabuf_try_cover_slice_rects(RECT **rects, unsigned int *count)
{
    RECT *covered_rects;
    unsigned int covered_count;

    if (!(covered_rects = wayland_hwnd_dmabuf_cover_slice_rects(*rects, *count, &covered_count)))
        return;
    if (covered_count && covered_count < *count)
    {
        free(*rects);
        *rects = covered_rects;
        *count = covered_count;
    }
    else
    {
        free(covered_rects);
    }
}

static BOOL wayland_hwnd_dmabuf_surface_apply_slice_layout(
        struct wayland_hwnd_dmabuf_surface *surface,
        const struct wayland_hwnd_dmabuf_slice_geometry *layout, unsigned int count,
        struct wl_surface *sibling)
{
    struct wayland_hwnd_dmabuf_slice *slice, *next;
    struct wl_surface *initial_sibling = sibling;
    unsigned int i;

    wl_list_for_each(slice, &surface->slices, link) slice->seen = FALSE;

    for (i = 0; i < count; i++)
    {
        if (!(slice = wayland_hwnd_dmabuf_surface_get_slice(surface))) return FALSE;

        wl_subsurface_set_position(slice->wl_subsurface, layout[i].x, layout[i].y);
        wl_subsurface_place_below(slice->wl_subsurface, sibling);
        wp_viewport_set_source(slice->wp_viewport, layout[i].source_x, layout[i].source_y,
                               layout[i].source_width, layout[i].source_height);
        wp_viewport_set_destination(slice->wp_viewport, layout[i].width, layout[i].height);
        slice->geometry = layout[i];
        slice->geometry_valid = TRUE;
        sibling = slice->wl_surface;
    }

    wl_list_for_each_safe(slice, next, &surface->slices, link)
    {
        if (slice->seen) continue;
        wayland_hwnd_dmabuf_slice_destroy(slice);
    }

    surface->sliced = TRUE;
    surface->slice_count = count;
    surface->slice_layout_sibling = initial_sibling;
    surface->stack_bottom = sibling;
    wayland_hwnd_dmabuf_surface_assert_slice_cache(surface);
    return TRUE;
}

static struct wayland_hwnd_dmabuf_slice *wayland_hwnd_dmabuf_surface_get_slice(
        struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_slice *slice;
    struct wl_region *empty_region;

    wl_list_for_each(slice, &surface->slices, link)
        if (!slice->seen)
        {
            slice->seen = TRUE;
            return slice;
        }

    if (!(slice = calloc(1, sizeof(*slice)))) return NULL;
    wl_list_init(&slice->link);
    if (!(slice->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    if (!(empty_region = wl_compositor_create_region(process_wayland.wl_compositor))) goto err;
    wl_surface_set_input_region(slice->wl_surface, empty_region);
    wl_region_destroy(empty_region);
    if (!(slice->wp_viewport = wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                                          slice->wl_surface))) goto err;
    if (!(slice->wl_subsurface = wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                                                 slice->wl_surface,
                                                                 surface->parent->wl_surface))) goto err;
    wl_subsurface_set_desync(slice->wl_subsurface);
    slice->seen = TRUE;
    wl_list_insert(surface->slices.prev, &slice->link);
    return slice;

err:
    if (slice->wl_subsurface) wl_subsurface_destroy(slice->wl_subsurface);
    if (slice->wp_viewport) wp_viewport_destroy(slice->wp_viewport);
    if (slice->wl_surface) wl_surface_destroy(slice->wl_surface);
    free(slice);
    return NULL;
}

static BOOL wayland_hwnd_dmabuf_surface_attach_slices(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;
    struct wayland_hwnd_dmabuf_slice *slice;
    unsigned int count = 0;

    if (!buffer) return FALSE;

    wl_list_for_each(slice, &surface->slices, link)
    {
        if (!wayland_hwnd_dmabuf_slice_attach_buffer(slice, buffer))
            return FALSE;
        wl_surface_damage_buffer(slice->wl_surface, 0, 0, buffer->width, buffer->height);
        wl_surface_commit(slice->wl_surface);
        count++;
    }

    return count == surface->slice_count && count;
}

static void wayland_hwnd_dmabuf_surface_destroy(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;

    wl_list_for_each_safe(buffer, buffer_next, &surface->buffers, link)
        wayland_hwnd_dmabuf_buffer_reap(buffer);
    wayland_hwnd_dmabuf_surface_clear_slices(surface);

    if (surface->parent && surface->parent->direct_dmabuf_surface == surface)
        surface->parent->direct_dmabuf_surface = NULL;
    if (surface->linked) wl_list_remove(&surface->link);
    if (surface->wl_subsurface) wl_subsurface_destroy(surface->wl_subsurface);
    if (!surface->direct && surface->wp_viewport) wp_viewport_destroy(surface->wp_viewport);
    if (!surface->direct && surface->wl_surface) wl_surface_destroy(surface->wl_surface);
    if (surface->consumer_state != WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED &&
        !wayland_hwnd_dmabuf_surface_send_consumer_state(surface,
                HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED))
        surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED;
    if (surface->channel_fd >= 0) close(surface->channel_fd);
    free(surface);
}

/* Roundtrip through WindowPosChanged to refresh the host window state. */
static void update_window_state(HWND hwnd)
{
    static const UINT swp_flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE |
                                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;
    static const RECT rect;

    NtUserSetRawWindowPos(hwnd, rect, swp_flags, FALSE);
}

static void wayland_surface_handle_enter(void *private, struct wl_surface *wl_surface,
                                         struct wl_output *wl_output)
{
    HWND hwnd = private;
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    struct surface_output_entry *output_entry;
    struct wayland_output *output = wl_output_get_user_data(wl_output);

    TRACE("hwnd=%p name=%s\n", hwnd, debugstr_a(output->current.name));

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface)) goto err;
    if (surface->wl_surface != wl_surface) goto err;
    if (!wayland_surface_is_toplevel(surface)) goto err;
    wl_list_for_each(output_entry, &surface->output_list, entry)
        if (output_entry->output == output) goto err;
    if (!(output_entry = calloc(1, sizeof(*output_entry)))) goto err;

    wl_list_init(&output_entry->entry);
    output_entry->output = output;
    wayland_output_add_ref(output_entry->output);
    wl_list_insert(&surface->output_list, &output_entry->entry);

    wayland_win_data_release(data);
    NtUserPostMessage(hwnd, WM_WINE_WINDOW_STATE_CHANGED, 0, 0);
    return;
err:
    wayland_win_data_release(data);
}

static void wayland_surface_handle_leave(void *private, struct wl_surface *wl_surface,
                                         struct wl_output *wl_output)
{
    HWND hwnd = private;
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    struct surface_output_entry *pos, *tmp;
    struct wayland_output *output = wl_output_get_user_data(wl_output);

    TRACE("hwnd=%p name=%s\n", hwnd, debugstr_a(output->current.name));

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface)) goto err;
    if (surface->wl_surface != wl_surface) goto err;

    wl_list_for_each_safe(pos, tmp, &surface->output_list, entry)
    {
        if (output == pos->output)
        {
            wl_list_remove(&pos->entry);
            wayland_output_release(pos->output);
            free(pos);
        }
    }

    wayland_win_data_release(data);
    NtUserPostMessage(hwnd, WM_WINE_WINDOW_STATE_CHANGED, 0, 0);
    return;
err:
    wayland_win_data_release(data);
}

static const struct wl_surface_listener wayland_surface_listener = {
    wayland_surface_handle_enter,
    wayland_surface_handle_leave,
};

static BOOL wayland_surface_config_has_bounds(const struct wayland_surface_config *config)
{
    return config->bounds_set && config->bounds_width > 0 && config->bounds_height > 0;
}

static const struct wayland_surface_config *wayland_surface_latest_config(struct wayland_surface *surface)
{
    if (surface->requested.serial) return &surface->requested;
    if (surface->processing.serial) return &surface->processing;
    if (surface->current.serial) return &surface->current;
    return NULL;
}

static BOOL wayland_surface_repaint_suspended(struct wayland_surface *surface)
{
    const struct wayland_surface_config *config = wayland_surface_latest_config(surface);

    return config && (config->state & WAYLAND_SURFACE_CONFIG_STATE_SUSPENDED);
}

static void wayland_surface_config_inherit_caps_and_bounds(struct wayland_surface_config *config,
                                                           struct wayland_surface *surface)
{
    const struct wayland_surface_config *prev = wayland_surface_latest_config(surface);

    if (!prev) return;

    if (!config->caps && prev->caps) config->caps = prev->caps;
    if (!config->bounds_set)
    {
        config->bounds_set = prev->bounds_set;
        config->bounds_width = prev->bounds_width;
        config->bounds_height = prev->bounds_height;
    }
}

static void xdg_surface_handle_configure(void *private, struct xdg_surface *xdg_surface,
                                         uint32_t serial)
{
    struct wayland_surface *surface;
    BOOL should_post = FALSE, should_expose = FALSE;
    struct wayland_win_data *data;
    HWND hwnd = private;

    TRACE("hwnd=%p serial=%u\n", hwnd, serial);

    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Handle this event only if wayland_surface is still associated with
     * the target xdg_surface. */
    if (!(surface = data->wayland_surface) || surface->xdg_surface != xdg_surface)
    {
        wayland_win_data_release(data);
        return;
    }

    if (wayland_surface_is_toplevel(surface))
    {
        BOOL was_suspended = wayland_surface_repaint_suspended(surface);

        /* If we have a previously requested config, we have already sent a
         * WM_WAYLAND_CONFIGURE which hasn't been handled yet. In that case,
         * avoid sending another message to reduce message queue traffic. */
        should_post = surface->requested.serial == 0;
        should_expose = surface->current.serial == 0;
        surface->pending.serial = serial;
        wayland_surface_config_inherit_caps_and_bounds(&surface->pending, surface);
        if (!surface->pending.decor && surface->current.decor)
            surface->pending.decor = surface->current.decor;
        else if (!surface->pending.decor && surface->requested.decor)
            surface->pending.decor = surface->requested.decor;
        if (surface->pending.decor &&
            surface->pending.decor != surface->current.decor)
        {
            should_post = TRUE;
            should_expose = TRUE;
        }
        surface->requested = surface->pending;
        memset(&surface->pending, 0, sizeof(surface->pending));

        if (was_suspended != wayland_surface_repaint_suspended(surface))
        {
            /* Re-evaluate every native presenter when workspace visibility
             * changes, then restore the selected presenter last. */
            wayland_client_surfaces_attach(data, surface->hwnd);
            should_expose = TRUE;
        }
    }
    else if (wayland_surface_is_popup(surface))
    {
        /* We cannot check if the owner moved so we must expose every configure. */
        should_expose = TRUE;
        surface->pending.serial = serial;
        surface->processing = surface->pending;
        surface->processing.processed = 1;
        memset(&surface->pending, 0, sizeof(surface->pending));
    }

    wayland_win_data_release(data);

    if (should_post) NtUserPostMessage(hwnd, WM_WAYLAND_CONFIGURE, 0, 0);

    /* Flush the window surface in case there is content that we weren't
     * able to flush before due to the lack of the initial configure. */
    if (should_expose) request_window_surface_expose(hwnd, TRUE);
}

static const struct xdg_surface_listener xdg_surface_listener =
{
    xdg_surface_handle_configure
};

static void xdg_toplevel_handle_configure(void *private,
                                          struct xdg_toplevel *xdg_toplevel,
                                          int32_t width, int32_t height,
                                          struct wl_array *states)
{
    struct wayland_surface *surface;
    HWND hwnd = private;
    uint32_t *state;
    enum wayland_surface_config_state config_state = 0;
    struct wayland_win_data *data;
    RECT rect;

    SetRect(&rect, 0, 0, width, height);

    wl_array_for_each(state, states)
    {
        switch(*state)
        {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_RESIZING;
            break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_TILED;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
            break;
        case XDG_TOPLEVEL_STATE_SUSPENDED:
            config_state |= WAYLAND_SURFACE_CONFIG_STATE_SUSPENDED;
            break;
        default:
            break;
        }
    }

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        surface->pending.rect = rect = map_rect_from_surface(surface, rect);
        surface->pending.state = config_state;
        if (!surface->pending.decor)
            surface->pending.decor = surface->current.decor;
    }

    wayland_win_data_release(data);

    TRACE("hwnd=%p %s,%#x suspended=%u\n", hwnd, wine_dbgstr_rect(&rect), config_state,
          !!(config_state & WAYLAND_SURFACE_CONFIG_STATE_SUSPENDED));
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    NtUserPostMessage((HWND)data, WM_SYSCOMMAND, SC_CLOSE, 0);
}

static void xdg_toplevel_handle_configure_bounds(void *private,
                                                 struct xdg_toplevel *xdg_toplevel,
                                                 int width, int height)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    HWND hwnd = private;

    TRACE("hwnd=%p bounds=%dx%d\n", hwnd, width, height);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        surface->pending.bounds_width = width;
        surface->pending.bounds_height = height;
        surface->pending.bounds_set = TRUE;
    }

    wayland_win_data_release(data);
}

static void xdg_toplevel_handle_wm_caps(void *private, struct xdg_toplevel *xdg_toplevel, struct wl_array *caps)
{
    int *state;
    HWND hwnd = private;
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    enum wayland_surface_wm_caps cap = WAYLAND_SURFACE_WM_CAPS_CHANGED;

    wl_array_for_each(state, caps)
    {
        switch (*state)
        {
            case XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN:
                cap |= WAYLAND_SURFACE_WM_CAPS_FULLSCREEN;
                break;
            case XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE:
                cap |= WAYLAND_SURFACE_WM_CAPS_MINIMIZE;
                break;
            case XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE:
                cap |= WAYLAND_SURFACE_WM_CAPS_MAXIMIZE;
                break;
            case XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU:
                cap |= WAYLAND_SURFACE_WM_CAPS_SHOW_WINDOW;
                break;
            default: break;
        }
    }

    TRACE("hwnd %p caps %x\n", hwnd, cap);

    if (!(cap & WAYLAND_SURFACE_WM_CAPS_FULLSCREEN))
        WARN("Compositor does not support fullscreen!\n");
    if (!(cap & WAYLAND_SURFACE_WM_CAPS_MAXIMIZE))
        WARN("Compositor does not support maximize!\n");
    if (!(cap & WAYLAND_SURFACE_WM_CAPS_MINIMIZE))
        WARN("Compositor does not support minimize, cannot implement window focus loss!\n");

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        surface->pending.caps = cap;
    }

    wayland_win_data_release(data);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener =
{
    xdg_toplevel_handle_configure,
    xdg_toplevel_handle_close,
    xdg_toplevel_handle_configure_bounds,
    xdg_toplevel_handle_wm_caps
};

static void xdg_popup_handle_configure(void *private, struct xdg_popup *xdg_popup,
                                       int32_t x, int32_t y, int32_t width, int32_t height)
{
    HWND hwnd = private;
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_popup(surface))
    {
        SetRect(&surface->pending.rect, x, y, x + width, y + height);
        surface->pending.rect = map_rect_from_surface(surface, surface->pending.rect);

        TRACE("hwnd=%p rect=%s\n", hwnd, wine_dbgstr_rect(&surface->pending.rect));
        surface->pending.state = 0;
    }

    wayland_win_data_release(data);
}

static void xdg_popup_handle_done(void *private, struct xdg_popup *xdg_popup)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    HWND hwnd = private;

    /* Recreate the popup if the compositor dismissed it for some reason.
     * The protocol does not explicitly prohibit this from occuring on ungrabbed popups. */
    WARN("Compositor dismissed popup hwnd=%p\n", hwnd);

    /* the protocol requires us to destroy the xdg_popup */
    xdg_popup_destroy(xdg_popup);

    if (!(data = wayland_win_data_get(hwnd))) return;
    if ((surface = data->wayland_surface) && surface->xdg_popup == xdg_popup)
    {
        surface->xdg_popup = NULL;
        wayland_surface_clear_role(surface);
    }
    wayland_win_data_release(data);

    NtUserPostMessage(hwnd, WM_WINE_UPDATEWINDOWSTATE, 0, 0);
}

static void xdg_popup_handle_reposition(void *private, struct xdg_popup *xdg_popup, uint32_t token)
{
    /* we also get a configure event in this case */
    TRACE("hwnd=%p\n", private);
}

static const struct xdg_popup_listener xdg_popup_listener =
{
    xdg_popup_handle_configure,
    xdg_popup_handle_done,
    xdg_popup_handle_reposition,
};

static void zwlr_layer_surface_v1_handle_configure(void *private,
                                                   struct zwlr_layer_surface_v1 *layer_surface,
                                                   uint32_t serial, uint32_t width, uint32_t height)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    HWND hwnd = private;

    TRACE("hwnd=%p %ux%u serial=%u\n", hwnd, width, height, serial);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface) && wayland_surface_is_layer(surface) &&
        surface->zwlr_layer_surface_v1 == layer_surface)
    {
        RECT rect = map_rect_to_surface(surface, surface->window.rect);

        if (!width) width = max(1, rect.right - rect.left);
        if (!height) height = max(1, rect.bottom - rect.top);

        SetRect(&surface->processing.rect, 0, 0, width, height);
        surface->processing.serial = serial;
        surface->processing.processed = TRUE;
    }

    wayland_win_data_release(data);
    request_window_surface_expose(hwnd, FALSE);
}

static void zwlr_layer_surface_v1_handle_closed(void *private,
                                                struct zwlr_layer_surface_v1 *layer_surface)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;
    BOOL menu = FALSE;
    HWND hwnd = private;

    TRACE("hwnd=%p\n", hwnd);

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && surface->zwlr_layer_surface_v1 == layer_surface)
            menu = surface->layer_menu;
        wayland_win_data_release(data);
    }
    if (menu) wayland_cancel_layer_menu(hwnd);
}

static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_v1_listener =
{
    zwlr_layer_surface_v1_handle_configure,
    zwlr_layer_surface_v1_handle_closed,
};

void wp_fractional_scale_handle_scale(void* user_data,
                                      struct wp_fractional_scale_v1 *fractional_scale_v1,
                                      uint32_t scale_fixed)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    double scale = scale_fixed / 120.0;
    HWND hwnd = user_data;

    TRACE("hwnd=%p scale=%lf\n", hwnd, scale);

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface) || scale == surface->window.scale)
    {
        wayland_win_data_release(data);
        return;
    }

    surface->window.scale = scale;

    wayland_win_data_release(data);

    request_window_surface_expose(hwnd, FALSE);
    update_window_state(hwnd);
}

static const struct wp_fractional_scale_v1_listener wp_fractional_scale_listener =
{
    wp_fractional_scale_handle_scale
};

static void zxdg_toplevel_decoration_v1_configure(void *user_data,
                                                  struct zxdg_toplevel_decoration_v1 *decoration,
                                                  uint32_t mode)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    HWND hwnd = user_data;

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
        {
            TRACE("hwnd=%p mode=%u\n", hwnd, mode);
            surface->pending.decor = mode;
        }
        wayland_win_data_release(data);
    }
}

static const struct zxdg_toplevel_decoration_v1_listener zxdg_toplevel_decoration_listener =
{
    zxdg_toplevel_decoration_v1_configure
};

static void wayland_surface_clear_child_surfaces(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;

    wayland_surface_clear_direct_dmabuf(surface, NULL);

    wl_list_for_each_safe(dmabuf_surface, next, &surface->hwnd_dmabuf_surfaces, link)
        wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);

    surface->dmabuf_bottom = NULL;
}

/* Clear cached input state when the wl_surface is destroyed. Role changes are
 * not input events; compositor enter/leave handles focus while the surface lives. */
static void wayland_surface_clear_input_state(struct wayland_surface *surface)
{
    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == surface->hwnd)
    {
        process_wayland.pointer.focused_hwnd = NULL;
        process_wayland.pointer.enter_serial = 0;
    }
    if (process_wayland.pointer.constraint_hwnd == surface->hwnd)
        wayland_pointer_clear_constraint();
    if (process_wayland.pointer.popup_serial_hwnd == surface->hwnd)
    {
        process_wayland.pointer.popup_serial = 0;
        process_wayland.pointer.popup_serial_hwnd = NULL;
        process_wayland.pointer.popup_serial_time = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    pthread_mutex_lock(&process_wayland.keyboard.mutex);
    if (process_wayland.keyboard.focused_hwnd == surface->hwnd)
        process_wayland.keyboard.focused_hwnd = NULL;
    pthread_mutex_unlock(&process_wayland.keyboard.mutex);

    pthread_mutex_lock(&process_wayland.text_input.mutex);
    if (process_wayland.text_input.focused_hwnd == surface->hwnd)
        process_wayland.text_input.focused_hwnd = NULL;
    pthread_mutex_unlock(&process_wayland.text_input.mutex);
}

static struct wl_surface *wayland_surface_dmabuf_stack_bottom(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;

    if (!surface->dmabuf_bottom) return NULL;

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
    {
        if (dmabuf_surface->stack_bottom == surface->dmabuf_bottom && dmabuf_surface->current)
            return surface->dmabuf_bottom;
    }

    surface->dmabuf_bottom = NULL;
    return NULL;
}

static struct wl_surface *wayland_surface_client_stack_anchor(struct wayland_surface *surface)
{
    struct wl_surface *bottom = wayland_surface_dmabuf_stack_bottom(surface);

    return bottom ? bottom : surface->wl_surface;
}

/**********************************************************************
 *          wayland_surface_create
 *
 * Creates a role-less wayland surface.
 */
struct wayland_surface *wayland_surface_create(HWND hwnd)
{
    struct wayland_surface *surface;

    surface = calloc(1, sizeof(*surface));
    if (!surface)
    {
        ERR("Failed to allocate space for Wayland surface\n");
        goto err;
    }

    TRACE("surface=%p\n", surface);

    surface->hwnd = hwnd;
    surface->serial = InterlockedIncrement(&wayland_surface_serial_counter);
    wl_list_init(&surface->hwnd_dmabuf_surfaces);
    wl_list_init(&surface->output_list);

    surface->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor);
    if (!surface->wl_surface)
    {
        ERR("Failed to create wl_surface Wayland surface\n");
        goto err;
    }
    wl_surface_add_listener(surface->wl_surface, &wayland_surface_listener, hwnd);

    surface->wp_viewport =
        wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                   surface->wl_surface);
    if (!surface->wp_viewport)
    {
        ERR("Failed to create wp_viewport Wayland surface\n");
        goto err;
    }
    if (process_wayland.wp_alpha_modifier_v1)
    {
        COLORREF key;
        DWORD flags;
        BYTE alpha;

        surface->wp_alpha_modifier_surface_v1 =
            wp_alpha_modifier_v1_get_surface(process_wayland.wp_alpha_modifier_v1, surface->wl_surface);

        if (!NtUserGetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) flags = 0;
        wayland_surface_set_opacity(surface, alpha, flags);
    }

    surface->window.scale = 1.0;
    surface->comitted.scale = 0.0;
    surface->ensured_contents = WAYLAND_SURFACE_NOT_ENSURED;
    surface->alpha_multiplier = UINT32_MAX;

    return surface;

err:
    if (surface) wayland_surface_destroy(surface);
    return NULL;
}

/**********************************************************************
 *          wayland_surface_destroy
 *
 * Destroys a wayland surface.
 */
void wayland_surface_destroy(struct wayland_surface *surface)
{
    struct surface_output_entry *output, *next;

    wayland_surface_clear_input_state(surface);
    wayland_surface_clear_role(surface);

    /* Destroying a wl_surface need not generate leave events. */
    wl_list_for_each_safe(output, next, &surface->output_list, entry)
    {
        wl_list_remove(&output->entry);
        wayland_output_release(output->output);
        free(output);
    }

    if (surface->wp_alpha_modifier_surface_v1)
    {
        wp_alpha_modifier_surface_v1_destroy(surface->wp_alpha_modifier_surface_v1);
        surface->wp_alpha_modifier_surface_v1 = NULL;
    }

    if (surface->wp_viewport)
    {
        wp_viewport_destroy(surface->wp_viewport);
        surface->wp_viewport = NULL;
        surface->configured_wp_viewport = NULL;
    }

    if (surface->wl_surface)
    {
        wl_surface_destroy(surface->wl_surface);
        surface->wl_surface = NULL;
    }

    if (surface->big_icon_buffer)
    {
        wayland_shm_buffer_unref(surface->big_icon_buffer);
        surface->big_icon_buffer = NULL;
    }

    if (surface->small_icon_buffer)
    {
        wayland_shm_buffer_unref(surface->small_icon_buffer);
        surface->small_icon_buffer = NULL;
    }

    if (surface->child_region)
    {
        NtGdiDeleteObjectApp(surface->child_region);
        surface->child_region = 0;
    }

    wl_display_flush(process_wayland.wl_display);

    free(surface);
}

static void wayland_surface_init_fractional_scale(struct wayland_surface *surface,
                                                  double initial_scale)
{
    surface->window.scale = initial_scale;

    if (!process_wayland.wp_fractional_scale_manager_v1) return;

    surface->wp_fractional_scale_v1 =
        wp_fractional_scale_manager_v1_get_fractional_scale(
            process_wayland.wp_fractional_scale_manager_v1,
            surface->wl_surface);
    if (!surface->wp_fractional_scale_v1)
    {
        ERR("Failed to create wp_fractional_scale_v1\n");
        return;
    }
    wp_fractional_scale_v1_add_listener(
        surface->wp_fractional_scale_v1,
        &wp_fractional_scale_listener,
        surface->hwnd);
}

static void wayland_surface_init_decoration(struct wayland_surface *surface)
{
    if (!process_wayland.zxdg_decoration_manager_v1) return;

    TRACE("surface %p\n", surface);

    surface->current.decor = 0;
    surface->zxdg_toplevel_decoration_v1 =
    zxdg_decoration_manager_v1_get_toplevel_decoration(
        process_wayland.zxdg_decoration_manager_v1,
        surface->xdg_toplevel);

    if (!surface->zxdg_toplevel_decoration_v1)
    {
        ERR("Failed to create toplevel zxdg_toplevel_decoration_v1\n");
        return;
    }

    zxdg_toplevel_decoration_v1_set_mode(
        surface->zxdg_toplevel_decoration_v1,
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    zxdg_toplevel_decoration_v1_add_listener(
        surface->zxdg_toplevel_decoration_v1,
        &zxdg_toplevel_decoration_listener,
        surface->hwnd);
}

void wayland_surface_sync_alpha(struct wayland_surface *surface)
{
    if (!process_wayland.wp_alpha_modifier_v1) return;

    if (surface->alpha_multiplier != UINT32_MAX)
    {
        if (!surface->wp_alpha_modifier_surface_v1)
        {
            surface->wp_alpha_modifier_surface_v1 =
                wp_alpha_modifier_v1_get_surface(process_wayland.wp_alpha_modifier_v1,
                                                 surface->wl_surface);
        }
        if (!surface->wp_alpha_modifier_surface_v1)
        {
            ERR("Failed to create alpha modifier surface\n");
            return;
        }
        wp_alpha_modifier_surface_v1_set_multiplier(surface->wp_alpha_modifier_surface_v1,
                                                    surface->alpha_multiplier);
    }
    else if (surface->wp_alpha_modifier_surface_v1)
    {
        wp_alpha_modifier_surface_v1_destroy(surface->wp_alpha_modifier_surface_v1);
        surface->wp_alpha_modifier_surface_v1 = NULL;
    }
}

/**********************************************************************
 *          wayland_surface_make_toplevel
 *
 * Gives the toplevel role to a plain wayland surface.
 */
void wayland_surface_make_toplevel(struct wayland_surface *surface, BOOL server_decor)
{
    WCHAR text[1024];

    TRACE("surface=%p\n", surface);

    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL);
    surface->owner_hwnd = NtUserGetWindowRelative(surface->hwnd, GW_OWNER);

    if (surface->xdg_surface && surface->xdg_toplevel)
    {
        if (!process_wayland.zxdg_decoration_manager_v1) return;

        if (!server_decor && surface->zxdg_toplevel_decoration_v1)
        {
            zxdg_toplevel_decoration_v1_destroy(surface->zxdg_toplevel_decoration_v1);
            surface->zxdg_toplevel_decoration_v1 = NULL;
            surface->pending.decor = surface->current.decor = 0;
        }
        else if (server_decor && !surface->zxdg_toplevel_decoration_v1)
            wayland_surface_init_decoration(surface);

        wl_surface_commit(surface->wl_surface);
        wl_display_flush(process_wayland.wl_display);

        return;
    }

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_TOPLEVEL;

    surface->xdg_surface =
        xdg_wm_base_get_xdg_surface(process_wayland.xdg_wm_base, surface->wl_surface);
    if (!surface->xdg_surface) goto err;
    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface->hwnd);

    surface->xdg_toplevel = xdg_surface_get_toplevel(surface->xdg_surface);
    if (!surface->xdg_toplevel) goto err;
    xdg_toplevel_add_listener(surface->xdg_toplevel, &xdg_toplevel_listener, surface->hwnd);

    if (process_name)
        xdg_toplevel_set_app_id(surface->xdg_toplevel, process_name);

    if (process_wayland.xdg_toplevel_tag_manager_v1)
    {
        xdg_toplevel_tag_manager_v1_set_toplevel_tag(
            process_wayland.xdg_toplevel_tag_manager_v1, surface->xdg_toplevel,
            "proton-game"
        );
        xdg_toplevel_tag_manager_v1_set_toplevel_description(
            process_wayland.xdg_toplevel_tag_manager_v1, surface->xdg_toplevel,
            "This is a game running through proton"
        );
    }

    if (!NtUserInternalGetWindowText(surface->hwnd, text, ARRAY_SIZE(text)))
        text[0] = 0;
    wayland_surface_set_title(surface, text);

    wayland_surface_assign_icon(surface);

    wayland_surface_init_fractional_scale(surface, 1.0);

    wayland_surface_sync_alpha(surface);

    if (server_decor) wayland_surface_init_decoration(surface);

    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign toplevel role to wayland surface\n");
}

/**********************************************************************
 *          wayland_surface_make_subsurface
 *
 * Gives the subsurface role to a plain Wayland surface.
 */
void wayland_surface_make_subsurface(struct wayland_surface *surface,
                                     struct wayland_surface *parent,
                                     BOOL stack_below_parent)
{
    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_SUBSURFACE);
    if (surface->wl_subsurface && surface->toplevel_hwnd == parent->hwnd)
    {
        surface->stack_below_parent = stack_below_parent;
        if (surface->parent_serial == parent->serial) return;

        TRACE("hwnd=%p parent_hwnd=%p serial changed %u -> %u; recreating subsurface\n",
              surface->hwnd, parent->hwnd, surface->parent_serial, parent->serial);
    }

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_SUBSURFACE;

    TRACE("surface=%p parent=%p\n", surface, parent);

    surface->wl_subsurface =
        wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                        surface->wl_surface, parent->wl_surface);
    if (!surface->wl_subsurface) goto err;

    wayland_surface_init_fractional_scale(surface, parent->window.scale);
    wayland_surface_sync_alpha(surface);
    surface->toplevel_hwnd = parent->hwnd;
    surface->parent_serial = parent->serial;
    surface->stack_below_parent = stack_below_parent;

    /* Present contents independently of the parent surface. */
    wl_subsurface_set_desync(surface->wl_subsurface);
    wl_display_flush(process_wayland.wl_display);
    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign subsurface role to Wayland surface\n");
}

static struct xdg_positioner *create_xdg_positioner(RECT rect)
{
    int width, height;
    struct xdg_positioner *xdg_positioner =
        xdg_wm_base_create_positioner(process_wayland.xdg_wm_base);

    if (!xdg_positioner) return NULL;

    width = max(1, rect.right - rect.left);
    height = max(1, rect.bottom - rect.top);

    /* this anchor rect is always valid, then we offset by the requested amount */
    xdg_positioner_set_anchor_rect(xdg_positioner, 0, 0, 1, 1);
    xdg_positioner_set_anchor(xdg_positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(xdg_positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_constraint_adjustment(xdg_positioner,
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                                             XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
    xdg_positioner_set_offset(xdg_positioner, rect.left, rect.top);
    xdg_positioner_set_size(xdg_positioner, width, height);
    xdg_positioner_set_constraint_adjustment(
        xdg_positioner,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);

    return xdg_positioner;
}

static BOOL hwnd_matches_popup_owner(HWND hwnd, HWND owner)
{
    return hwnd == owner || NtUserGetAncestor(hwnd, GA_ROOT) == owner;
}

static void clear_pointer_popup_serial(uint32_t serial, HWND hwnd)
{
    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.popup_serial == serial &&
        (!hwnd || process_wayland.pointer.popup_serial_hwnd == hwnd))
    {
        process_wayland.pointer.popup_serial = 0;
        process_wayland.pointer.popup_serial_hwnd = NULL;
        process_wayland.pointer.popup_serial_time = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);
}

static uint32_t popup_grab_serial_for_owner(struct wayland_surface *owner)
{
    uint32_t button_serial, popup_serial, serial = 0;
    unsigned long long popup_serial_time, now;
    HWND focused, popup_hwnd;

    if (wayland_surface_is_popup(owner) && !owner->xdg_popup_grabbed) return 0;

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    focused = process_wayland.pointer.focused_hwnd;
    button_serial = process_wayland.pointer.button_serial;
    popup_serial = process_wayland.pointer.popup_serial;
    popup_hwnd = process_wayland.pointer.popup_serial_hwnd;
    popup_serial_time = process_wayland.pointer.popup_serial_time;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    if (button_serial && focused && hwnd_matches_popup_owner(focused, owner->hwnd))
    {
        clear_pointer_popup_serial(button_serial, focused);
        return button_serial;
    }

    if (!popup_serial || !popup_hwnd) return 0;

    now = wayland_time_ms();
    if (now - popup_serial_time > POPUP_GRAB_SERIAL_TIMEOUT_MS ||
        !hwnd_matches_popup_owner(popup_hwnd, owner->hwnd))
    {
        if (now - popup_serial_time > POPUP_GRAB_SERIAL_TIMEOUT_MS)
        {
            clear_pointer_popup_serial(popup_serial, NULL);
        }
        return 0;
    }

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.popup_serial == popup_serial &&
        process_wayland.pointer.popup_serial_hwnd == popup_hwnd)
    {
        serial = process_wayland.pointer.popup_serial;
        process_wayland.pointer.popup_serial = 0;
        process_wayland.pointer.popup_serial_hwnd = NULL;
        process_wayland.pointer.popup_serial_time = 0;
    }
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    return serial;
}

/**********************************************************************
 *          wayland_surface_make_popup
 *
 * Gives the popup role to a plain wayland surface.
 */
void wayland_surface_make_popup(struct wayland_surface *surface,
                                struct wayland_surface *owner)
{
    struct xdg_positioner *xdg_positioner = NULL;
    RECT rect = surface->window.rect;
    uint32_t grab_serial = 0;

    OffsetRect(&rect, -owner->window.rect.left, -owner->window.rect.top);

    assert(owner->xdg_surface);
    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_POPUP);

    if (surface->xdg_popup && surface->owner_hwnd == owner->hwnd)
    {
        if (!surface->current.serial) return;

        rect = map_rect_to_surface(surface, rect);

        /* reposition the popup if needed */
        if (EqualRect(&surface->current.rect, &rect)) return;

        xdg_positioner = create_xdg_positioner(rect);
        if (!xdg_positioner)
        {
            ERR("Failed to create positioner!\n");
            return;
        }

        xdg_popup_reposition(surface->xdg_popup, xdg_positioner, 0);
        xdg_positioner_destroy(xdg_positioner);
        wl_surface_commit(surface->wl_surface);
        wl_display_flush(process_wayland.wl_display);
        return;
    }

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_POPUP;
    surface->owner_hwnd = NULL;
    surface->xdg_popup_grabbed = FALSE;

    wayland_surface_init_fractional_scale(surface, owner->window.scale);
    rect = map_rect_to_surface(surface, rect);

    surface->xdg_surface = xdg_wm_base_get_xdg_surface(process_wayland.xdg_wm_base,
                                                       surface->wl_surface);
    if (!surface->xdg_surface) goto err;
    xdg_surface_add_listener(surface->xdg_surface, &xdg_surface_listener, surface->hwnd);

    xdg_positioner = create_xdg_positioner(rect);
    if (!xdg_positioner) goto err;

    surface->xdg_popup = xdg_surface_get_popup(surface->xdg_surface, owner->xdg_surface,
                                               xdg_positioner);
    xdg_positioner_destroy(xdg_positioner);
    if (!surface->xdg_popup) goto err;
    xdg_popup_add_listener(surface->xdg_popup, &xdg_popup_listener, surface->hwnd);

    if (wayland_is_popup_menu_class(surface->hwnd))
        grab_serial = popup_grab_serial_for_owner(owner);
    if (grab_serial)
    {
        pthread_mutex_lock(&process_wayland.seat.mutex);
        if (process_wayland.seat.wl_seat)
        {
            TRACE("grabbing popup hwnd=%p owner=%p serial=%u\n",
                  surface->hwnd, owner->hwnd, grab_serial);
            xdg_popup_grab(surface->xdg_popup, process_wayland.seat.wl_seat, grab_serial);
            surface->xdg_popup_grabbed = TRUE;
        }
        pthread_mutex_unlock(&process_wayland.seat.mutex);
    }

    wayland_surface_sync_alpha(surface);
    surface->owner_hwnd = owner->hwnd;
    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);

    return;
err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign popup role to wayland surface\n");
}

static struct wl_output *layer_surface_get_output(const RECT *rect, RECT *output_rect,
                                                  double *output_scale)
{
    struct wayland_output *output;
    struct wl_output *wl_output = NULL;

    SetRectEmpty(output_rect);
    *output_scale = 1.0;

    if ((output = wayland_output_for_rect(rect)))
    {
        const struct wayland_output_state *state = &output->current;

        wl_output = output->wl_output;
        if (state->current_mode)
        {
            SetRect(output_rect, state->physical_x, state->physical_y,
                    state->physical_x + state->current_mode->width,
                    state->physical_y + state->current_mode->height);
            if (state->logical_w > 0)
                *output_scale = round(120.0 * state->current_mode->width /
                                      state->logical_w) / 120.0;
        }
        wayland_output_release(output);
    }

    return wl_output;
}

static void wayland_surface_update_layer_config(struct wayland_surface *surface,
                                                const RECT *rect,
                                                const RECT *output_rect)
{
    POINT position = {rect->left, rect->top};
    RECT size = {0, 0, rect->right - rect->left, rect->bottom - rect->top};

    if (surface->layer_output && !IsRectEmpty(output_rect))
    {
        position.x -= output_rect->left;
        position.y -= output_rect->top;
    }

    position = map_point_to_surface(surface, position);
    size = map_rect_to_surface(surface, size);

    zwlr_layer_surface_v1_set_size(surface->zwlr_layer_surface_v1,
                                   max(1, size.right), max(1, size.bottom));
    zwlr_layer_surface_v1_set_anchor(surface->zwlr_layer_surface_v1,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_margin(surface->zwlr_layer_surface_v1,
                                     position.y, 0, 0, position.x);
    zwlr_layer_surface_v1_set_exclusive_zone(surface->zwlr_layer_surface_v1, -1);
}

/**********************************************************************
 *          wayland_surface_make_layer
 *
 * Gives the layer-shell role to a tray menu or desktop-notification surface.
 * Unlike an xdg_toplevel, layer-shell preserves the application's requested
 * screen-edge position.
 */
void wayland_surface_make_layer(struct wayland_surface *surface, const RECT *rect, BOOL menu)
{
    uint32_t keyboard_interactivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    struct wl_output *output;
    double initial_scale;
    RECT output_rect;

    TRACE("surface=%p rect=%s\n", surface, wine_dbgstr_rect(rect));

    assert(!surface->role || surface->role == WAYLAND_SURFACE_ROLE_LAYER);
    output = layer_surface_get_output(rect, &output_rect, &initial_scale);

    if (surface->zwlr_layer_surface_v1 &&
        (surface->layer_output != output || surface->layer_menu != menu))
        wayland_surface_clear_role(surface);

    if (surface->zwlr_layer_surface_v1)
    {
        wayland_surface_update_layer_config(surface, rect, &output_rect);
        if (surface->layer_menu) wayland_set_layer_menu_hwnd(surface->hwnd);
        wl_surface_commit(surface->wl_surface);
        wl_display_flush(process_wayland.wl_display);
        return;
    }

    wayland_surface_clear_role(surface);
    surface->role = WAYLAND_SURFACE_ROLE_LAYER;
    surface->layer_output = output;
    surface->layer_menu = menu;
    /* Tray menus must be above top-layer panels with exclusive focus.
     * Keep their keyboard interactivity non-exclusive so outside clicks can
     * still move focus away and dismiss the menu. */
    surface->zwlr_layer_surface_v1 =
        zwlr_layer_shell_v1_get_layer_surface(process_wayland.zwlr_layer_shell_v1,
                                              surface->wl_surface, output,
                                              menu ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY :
                                                     ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                              menu ? "wine-menu" : "wine-notification");
    if (!surface->zwlr_layer_surface_v1) goto err;

    /* Exclusive layer focus prevents other applications from receiving keys,
     * even after a click outside the menu. Older layer-shell versions cannot
     * provide non-exclusive keyboard focus. Set this only when creating the
     * role: repeating ON_DEMAND on a geometry update may relinquish focus. */
    if (menu && zwlr_layer_surface_v1_get_version(surface->zwlr_layer_surface_v1) >=
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION)
        keyboard_interactivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
    zwlr_layer_surface_v1_set_keyboard_interactivity(surface->zwlr_layer_surface_v1,
                                                    keyboard_interactivity);

    zwlr_layer_surface_v1_add_listener(surface->zwlr_layer_surface_v1,
                                       &zwlr_layer_surface_v1_listener,
                                       surface->hwnd);
    wayland_surface_init_fractional_scale(surface, initial_scale);
    wayland_surface_update_layer_config(surface, rect, &output_rect);
    if (surface->layer_menu) wayland_set_layer_menu_hwnd(surface->hwnd);

    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);
    return;

err:
    wayland_surface_clear_role(surface);
    ERR("Failed to assign layer role to wayland surface\n");
}

static void wayland_surface_clear_carrier(struct wayland_surface *surface)
{
    /* wl_surface state survives buffer replacement. */
    if (surface->carrier_attached && surface->carrier_opaque)
        wl_surface_set_opaque_region(surface->wl_surface, NULL);

    surface->carrier_attached = FALSE;
    surface->carrier_opaque = FALSE;
    surface->carrier_single_pixel = FALSE;
    surface->carrier_width = surface->carrier_height = 0;
}

/**********************************************************************
 *          wayland_surface_clear_role
 *
 * Clears the role related Wayland objects of a Wayland surface, making it a
 * plain surface again. We can later assign the same role (but not a
 * different one!) to the surface.
 */
void wayland_surface_clear_role(struct wayland_surface *surface)
{
    TRACE("surface=%p\n", surface);

    /* Keep input state across role churn; it follows wl_surface enter/leave. */
    wayland_surface_clear_child_surfaces(surface);
    wayland_surface_clear_carrier(surface);

    /* some objects are shared between several roles */

    if (surface->wp_fractional_scale_v1)
    {
        wp_fractional_scale_v1_destroy(surface->wp_fractional_scale_v1);
        surface->wp_fractional_scale_v1 = NULL;
    }

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        break;

    case WAYLAND_SURFACE_ROLE_POPUP:

        if (surface->xdg_popup)
        {
            xdg_popup_destroy(surface->xdg_popup);
            surface->xdg_popup = NULL;
        }

        if (surface->xdg_surface)
        {
            xdg_surface_destroy(surface->xdg_surface);
            surface->xdg_surface = NULL;
        }

        surface->owner_hwnd = NULL;
        surface->xdg_popup_grabbed = FALSE;
        break;

    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (surface->xdg_toplevel_icon)
        {
            xdg_toplevel_icon_manager_v1_set_icon(
                process_wayland.xdg_toplevel_icon_manager_v1,
                surface->xdg_toplevel, NULL);
            xdg_toplevel_icon_v1_destroy(surface->xdg_toplevel_icon);
            surface->xdg_toplevel_icon = NULL;
        }

        if (surface->zwp_keyboard_shortcuts_inhibitor_v1)
        {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(
                surface->zwp_keyboard_shortcuts_inhibitor_v1);
            surface->zwp_keyboard_shortcuts_inhibitor_v1 = NULL;
        }

        if (surface->zxdg_toplevel_decoration_v1)
        {
            zxdg_toplevel_decoration_v1_destroy(surface->zxdg_toplevel_decoration_v1);
            surface->zxdg_toplevel_decoration_v1 = NULL;
        }

        if (surface->xdg_toplevel)
        {
            xdg_toplevel_destroy(surface->xdg_toplevel);
            surface->xdg_toplevel = NULL;
        }

        if (surface->xdg_surface)
        {
            xdg_surface_destroy(surface->xdg_surface);
            surface->xdg_surface = NULL;
        }

        surface->requested_output = NULL;
        break;

    case WAYLAND_SURFACE_ROLE_LAYER:
        if (surface->layer_menu) wayland_clear_layer_menu_hwnd(surface->hwnd);
        if (surface->zwlr_layer_surface_v1)
        {
            zwlr_layer_surface_v1_destroy(surface->zwlr_layer_surface_v1);
            surface->zwlr_layer_surface_v1 = NULL;
        }
        surface->layer_output = NULL;
        surface->layer_menu = FALSE;
        break;

    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        if (surface->wl_subsurface)
        {
            wl_subsurface_destroy(surface->wl_subsurface);
            surface->wl_subsurface = NULL;
        }

        surface->toplevel_hwnd = 0;
        surface->parent_serial = 0;
        surface->stack_below_parent = FALSE;
        break;
    }

    memset(&surface->pending, 0, sizeof(surface->pending));
    memset(&surface->requested, 0, sizeof(surface->requested));
    memset(&surface->processing, 0, sizeof(surface->processing));
    memset(&surface->current, 0, sizeof(surface->current));

    memset(&surface->comitted, 0, sizeof(surface->comitted));
    surface->comitted.scale = 0.0;
    memset(&surface->toplevel_size_limits, 0, sizeof(surface->toplevel_size_limits));

    /* Ensure no buffer is attached, otherwise future role assignments may fail. */
    wl_surface_attach(surface->wl_surface, NULL, 0, 0);
    wl_surface_commit(surface->wl_surface);

    surface->content_width = 0;
    surface->content_height = 0;
    surface->ensured_contents = WAYLAND_SURFACE_NOT_ENSURED;
    SetRect(&surface->geometry, 0, 0, 0, 0);

    wl_display_flush(process_wayland.wl_display);
}

/**********************************************************************
 *          wayland_surface_attach_shm_buffer
 *
 * Attaches a SHM buffer to a wayland surface.
 *
 * The buffer is marked as unavailable until committed and subsequently
 * released by the compositor.
 */
static void wayland_surface_attach_shm_buffer(struct wayland_surface *surface,
                                              struct wayland_shm_buffer *shm_buffer,
                                              HRGN surface_damage_region)
{
    RGNDATA *surface_damage;
    int win_width, win_height;

    TRACE("surface=%p shm_buffer=%p (%dx%d)\n",
          surface, shm_buffer, shm_buffer->width, shm_buffer->height);

    shm_buffer->busy = TRUE;
    wayland_shm_buffer_ref(shm_buffer);

    wl_surface_attach(surface->wl_surface, shm_buffer->wl_buffer, 0, 0);

    /* Add surface damage, i.e., which parts of the surface have changed since
     * the last surface commit. Note that this is different from the buffer
     * damage region. */
    surface_damage = get_region_data(surface_damage_region);
    if (surface_damage)
    {
        RECT *rgn_rect = (RECT *)surface_damage->Buffer;
        RECT *rgn_rect_end = rgn_rect + surface_damage->rdh.nCount;

        for (;rgn_rect < rgn_rect_end; rgn_rect++)
        {
            wl_surface_damage_buffer(surface->wl_surface,
                                     rgn_rect->left, rgn_rect->top,
                                     rgn_rect->right - rgn_rect->left,
                                     rgn_rect->bottom - rgn_rect->top);
        }
        free(surface_damage);
    }

    win_width = surface->window.rect.right - surface->window.rect.left;
    win_height = surface->window.rect.bottom - surface->window.rect.top;

    /* It is an error to specify a wp_viewporter source rectangle that
     * is partially or completely outside of the wl_buffe.
     * 0 is also an invalid width / height value so use 1x1 instead.
     */
    win_width = max(1, min(win_width, shm_buffer->width));
    win_height = max(1, min(win_height, shm_buffer->height));

    wp_viewport_set_source(surface->wp_viewport, 0, 0,
                           wl_fixed_from_int(win_width),
                           wl_fixed_from_int(win_height));

    surface->content_width = win_width;
    surface->content_height = win_height;
}

void wayland_surface_attach_shm(struct wayland_surface *surface,
                                struct wayland_shm_buffer *shm_buffer,
                                HRGN surface_damage_region)
{
    wayland_surface_clear_carrier(surface);
    wayland_surface_attach_shm_buffer(surface, shm_buffer, surface_damage_region);
}

/**********************************************************************
 *          wayland_surface_config_is_compatible
 *
 * Checks whether a wayland_surface_config object is compatible with the
 * the provided arguments.
 */
BOOL wayland_surface_config_is_compatible(struct wayland_surface_config *conf, RECT rect,
                                          enum wayland_surface_config_state state)
{
    static enum wayland_surface_config_state mask =
        WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;

    /* The fullscreen state requires a size smaller or equal to the configured
     * size. If we have a larger size, we can use surface geometry during
     * surface reconfiguration to provide the smaller size, so we are always
     * compatible with a fullscreen state.
     * NOTE: Fullscreen combined with maximized is the same as fullscreen. */
    if (conf->state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
        return TRUE;

    /* We require the same state. */
    if ((state & mask) != (conf->state & mask)) return FALSE;

    /* The maximized state requires the configured size. During surface
     * reconfiguration we can use surface geometry to provide smaller areas
     * from larger sizes, so only smaller sizes are incompatible. */
    if ((conf->state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
        (rect.right - rect.left < conf->rect.right - conf->rect.left ||
         rect.bottom - rect.top < conf->rect.bottom - conf->rect.top))
    {
        return FALSE;
    }

    return TRUE;
}

static void wayland_surface_reconfigure_subsurface(struct wayland_surface *surface)
{
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *toplevel_surface;
    POINT point;

    if (!(toplevel_data = wayland_win_data_get_nolock(surface->toplevel_hwnd)) ||
        !(toplevel_surface = toplevel_data->wayland_surface))
        return;

    if (surface->parent_serial != toplevel_surface->serial)
    {
        TRACE("hwnd=%p parent_hwnd=%p serial changed %u -> %u; recreating subsurface\n",
              surface->hwnd, toplevel_surface->hwnd, surface->parent_serial,
              toplevel_surface->serial);
        wayland_surface_make_subsurface(surface, toplevel_surface,
                                        surface->stack_below_parent);
        return;
    }

    point.x = surface->window.rect.left - toplevel_surface->window.rect.left;
    point.y = surface->window.rect.top - toplevel_surface->window.rect.top;
    point = map_point_to_surface(surface, point);

    TRACE("hwnd=%p pos=%d,%d\n", surface->hwnd, point.x, point.y);
    wl_subsurface_set_position(surface->wl_subsurface, point.x, point.y);
    if (surface->stack_below_parent)
        wl_subsurface_place_below(surface->wl_subsurface,
                                  toplevel_surface->wl_surface);
    else if (toplevel_data->client_surface && toplevel_data->client_surface->wl_subsurface)
        wl_subsurface_place_above(surface->wl_subsurface,
                                  toplevel_data->client_surface->wl_surface);
    else
        wl_subsurface_place_above(surface->wl_subsurface,
                                  toplevel_surface->wl_surface);
    wl_surface_commit(toplevel_surface->wl_surface);
    memset(&surface->processing, 0, sizeof(surface->processing));
}

/**********************************************************************
 *          wayland_surface_get_rect_in_monitor
 *
 * Gets the largest rectangle within a surface's window (in window coordinates)
 * that is visible in a monitor.
 */
static void wayland_surface_get_rect_in_monitor(struct wayland_surface *surface,
                                                RECT *rect)
{
    HMONITOR hmonitor;
    MONITORINFO mi;

    mi.cbSize = sizeof(mi);
    if (!(hmonitor = NtUserMonitorFromRect(&surface->window.rect, 0)) ||
        !NtUserGetMonitorInfo(hmonitor, (MONITORINFO *)&mi))
    {
        SetRectEmpty(rect);
        return;
    }

    intersect_rect(rect, &mi.rcMonitor, &surface->window.rect);
    OffsetRect(rect, -surface->window.rect.left, -surface->window.rect.top);
}

void wayland_surface_update_toplevel_parent(struct wayland_surface *surface)
{
    struct wayland_win_data *owner_data;
    struct wayland_surface *owner_surface = NULL;

    if (!wayland_surface_is_toplevel(surface)) return;

    TRACE("hwnd=%p owner=%p\n", surface->hwnd, surface->owner_hwnd);

    if ((owner_data = wayland_win_data_get(surface->owner_hwnd)))
    {
        if (!(owner_surface = owner_data->wayland_surface) ||
            !wayland_surface_is_toplevel(owner_surface))
            owner_surface = NULL;

        wayland_win_data_release(owner_data);
    }

    xdg_toplevel_set_parent(surface->xdg_toplevel, owner_surface ? owner_surface->xdg_toplevel : NULL);
}

static BOOL wayland_surface_config_is_managed(const struct wayland_surface_config *config)
{
    return config->state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                            WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                            WAYLAND_SURFACE_CONFIG_STATE_TILED);
}

BOOL wayland_surface_get_max_track_size(struct wayland_surface *surface, SIZE *size)
{
    const struct wayland_surface_config *config = &surface->current;
    int width, height;

    if (!surface->window.resizeable || !wayland_surface_is_toplevel(surface) ||
        !surface->xdg_toplevel)
        return FALSE;

    /* A maximize/restore command runs before the new configure is committed.
     * Do not constrain it with the previous floating window's bounds. */
    if (surface->processing.serial) config = &surface->processing;
    else if (surface->requested.serial) config = &surface->requested;
    if (config->state & (WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                         WAYLAND_SURFACE_CONFIG_STATE_TILED))
        return FALSE;
    if (config->serial && (config->state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
    {
        /* Configure rects are already in window coordinates. */
        width = config->rect.right - config->rect.left;
        height = config->rect.bottom - config->rect.top;
    }
    else
    {
        config = &surface->current;
        if (!config->serial || wayland_surface_config_is_managed(config) ||
            !wayland_surface_config_has_bounds(config))
            return FALSE;

        wayland_surface_coords_to_window(surface, config->bounds_width,
                                         config->bounds_height, &width, &height);
    }
    if (width <= 0 || height <= 0) return FALSE;

    size->cx = width;
    size->cy = height;
    return TRUE;
}

static void wayland_surface_apply_toplevel_size_limits(struct wayland_surface *surface,
                                                       int width, int height)
{
    struct wayland_toplevel_size_limits limits = {0};

    if (!wayland_surface_is_toplevel(surface)) return;

    if (surface->window.resizeable)
    {
        if (!wayland_surface_config_is_managed(&surface->current) &&
            wayland_surface_config_has_bounds(&surface->current))
        {
            limits.max_width = surface->current.bounds_width;
            limits.max_height = surface->current.bounds_height;
        }
    }
    else
    {
        limits.min_width = limits.max_width = width;
        limits.min_height = limits.max_height = height;
    }

    if (surface->toplevel_size_limits.valid &&
        surface->toplevel_size_limits.min_width == limits.min_width &&
        surface->toplevel_size_limits.min_height == limits.min_height &&
        surface->toplevel_size_limits.max_width == limits.max_width &&
        surface->toplevel_size_limits.max_height == limits.max_height)
        return;

    xdg_toplevel_set_min_size(surface->xdg_toplevel, limits.min_width,
                              limits.min_height);
    xdg_toplevel_set_max_size(surface->xdg_toplevel, limits.max_width,
                              limits.max_height);

    limits.valid = TRUE;
    surface->toplevel_size_limits = limits;
}

/**********************************************************************
 *          wayland_surface_reconfigure_geometry
 *
 * Sets the xdg_surface geometry
 */
static void wayland_surface_reconfigure_geometry(struct wayland_surface *surface, RECT rect)
{
    const RECT *current = &surface->current.rect;
    const RECT *client = &surface->window.client_rect;
    BOOL fullscreen_client = (surface->current.state & surface->window.state &
                             WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
                            !IsRectEmpty(client) &&
                            client->right - client->left == current->right - current->left &&
                            client->bottom - client->top == current->bottom - current->top;

    /* Keep a fullscreen client's thin frame outside the visible geometry.
     * Preserve surface-local coordinates for both buffers and pointer input. */
    if (fullscreen_client)
    {
        rect = *client;
        OffsetRect(&rect, -surface->window.rect.left, -surface->window.rect.top);
    }
    /* If the window size is bigger than the current state accepts, use the
     * largest visible (from Windows' perspective) subregion of the window. */
    else if ((surface->current.state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                                       WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)) &&
             (rect.right - rect.left > current->right - current->left ||
              rect.bottom - rect.top > current->bottom - current->top))
    {
        wayland_surface_get_rect_in_monitor(surface, &rect);

        /* If the window rect in the monitor is smaller than required,
         * fall back to an appropriately sized rect at the top-left. */
        if ((surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            (rect.right - rect.left < current->right - current->left ||
             rect.bottom - rect.top < current->bottom - current->top))
        {
            SetRect(&rect, 0, 0, current->right - current->left, current->bottom - current->top);
        }
        else
        {
            rect.right = min(rect.right, rect.left + current->right - current->left);
            rect.bottom = min(rect.bottom, rect.top + current->bottom - current->top);
        }
        TRACE("Window is too large for Wayland state, using subregion\n");
    }

    rect = map_rect_to_surface(surface, rect);

    TRACE("hwnd=%p geometry=%s\n", surface->hwnd, wine_dbgstr_rect(&rect));

    if (!IsRectEmpty(&rect))
    {
        int width = rect.right - rect.left, height = rect.bottom - rect.top;

        xdg_surface_set_window_geometry(surface->xdg_surface,
                                        fullscreen_client ? rect.left : 0,
                                        fullscreen_client ? rect.top : 0, width, height);
        surface->geometry = rect;
        /* min/max size are toplevel-only. xdg_popup aliases xdg_toplevel in the union. */
        if (!wayland_surface_is_toplevel(surface)) return;

        wayland_surface_apply_toplevel_size_limits(surface, width, height);
        wayland_surface_update_toplevel_parent(surface);
    }
}

/**********************************************************************
 *          wayland_surface_reconfigure_size
 *
 * Sets the surface size with viewporter
 */
static void wayland_surface_reconfigure_size(struct wayland_surface *surface,
                                             int width, int height)
{
    int dest_width = width, dest_height = height;

    if (width <= 0 || height <= 0)
        dest_width = dest_height = -1;

    if (surface->configured_wp_viewport == surface->wp_viewport &&
        surface->viewport_dest_width == dest_width &&
        surface->viewport_dest_height == dest_height)
        return;

    TRACE("hwnd=%p size=%dx%d\n", surface->hwnd, dest_width, dest_height);

    if (dest_width > 0 && dest_height > 0)
        wp_viewport_set_destination(surface->wp_viewport, dest_width, dest_height);
    else
        wp_viewport_set_destination(surface->wp_viewport, -1, -1);

    surface->configured_wp_viewport = surface->wp_viewport;
    surface->viewport_dest_width = dest_width;
    surface->viewport_dest_height = dest_height;
}

/**********************************************************************
 *          wayland_surface_reconfigure_client
 *
 * Reconfigures the subsurface covering the client area.
 */
static BOOL wayland_client_surface_should_stack_above_parent(
        struct wayland_surface *surface, struct wayland_client_surface *client,
        HWND hwnd, HWND toplevel, BOOL content_over_producer)
{
    if (!ReadAcquire(&client->client.presentation_owner)) return FALSE;
    /* A single opaque client surface covering the whole toplevel can sit above
     * an opaque parent carrier without using transparent punch-through. */
    if (hwnd != toplevel) return FALSE;
    if (content_over_producer || ReadAcquire(&client->has_alpha)) return FALSE;
    if (surface->window.exstyle & WS_EX_LAYERED) return FALSE;
    if (!wayland_surface_is_toplevel(surface) || surface->window.minimized) return FALSE;
    /* The outer Win32 rect may retain stale frame extents across a fullscreen
     * transition; classify coverage against the Wayland presentation rect. */
    if (!EqualRect(&surface->window.rect, &surface->window.client_rect)) return FALSE;
    if (surface->shaped) return FALSE;
    if (wayland_surface_dmabuf_stack_bottom(surface)) return FALSE;
    return TRUE;
}

static void wayland_client_surface_stack(struct wayland_surface *surface,
                                         struct wayland_client_surface *client,
                                         BOOL stack_above_parent)
{
    BOOL effective_stack_above_parent;

    if (!client->wl_subsurface) return;

    effective_stack_above_parent = stack_above_parent &&
                                    !wayland_surface_repaint_suspended(surface);
    if (effective_stack_above_parent)
        wl_subsurface_place_above(client->wl_subsurface, surface->wl_surface);
    else
        wl_subsurface_place_below(client->wl_subsurface,
                                  wayland_surface_client_stack_anchor(surface));
    client->stack_above_parent = stack_above_parent;
}

static BOOL wayland_surface_reconfigure_client(struct wayland_surface *surface,
                                               struct wayland_client_surface *client,
                                               const RECT *client_rect,
                                               BOOL stack_above_parent)
{
    struct wayland_window_config *window = &surface->window;
    BOOL rect_changed, stack_changed;
    RECT rect = *client_rect;

    /* The offset of the client area origin relatively to the window origin. */
    OffsetRect(&rect, window->client_rect.left - window->rect.left,
               window->client_rect.top - window->rect.top);
    rect = map_rect_to_surface(surface, rect);

    TRACE("hwnd=%p rect=%s\n", surface->hwnd, wine_dbgstr_rect(&rect));

    rect_changed = !EqualRect(&client->rect, &rect);
    stack_changed = client->stack_above_parent != stack_above_parent;

    if (rect_changed || stack_changed)
    {
        if (client->wl_subsurface)
        {
            if (rect_changed)
                wl_subsurface_set_position(client->wl_subsurface, rect.left, rect.top);
            wayland_client_surface_stack(surface, client, stack_above_parent);
        }

        if (rect_changed)
        {
            client->rect = rect;
            if (rect.left != rect.right && rect.top != rect.bottom)
                wp_viewport_set_destination(client->wp_viewport,
                                            rect.right - rect.left, rect.bottom - rect.top);
            else /* We can't have a 0x0 destination, use 1x1 instead. */
                wp_viewport_set_destination(client->wp_viewport, 1, 1);
        }
    }

    return rect_changed || stack_changed;
}

static struct wayland_hwnd_dmabuf_surface *wayland_hwnd_dmabuf_surface_get(struct wayland_surface *parent,
                                                                           HWND hwnd, BOOL gdi_overlay)
{
    struct wayland_hwnd_dmabuf_surface *surface;

    wl_list_for_each(surface, &parent->hwnd_dmabuf_surfaces, link)
        if (surface->hwnd == hwnd && surface->gdi_overlay == gdi_overlay) return surface;
    return NULL;
}

static struct wayland_hwnd_dmabuf_surface *wayland_hwnd_dmabuf_surface_create(struct wayland_surface *parent,
                                                                              HWND hwnd, BOOL gdi_overlay)
{
    struct wayland_hwnd_dmabuf_surface *surface;
    struct wl_region *empty_region;

    if (!(surface = calloc(1, sizeof(*surface)))) return NULL;
    surface->hwnd = hwnd;
    surface->parent = parent;
    surface->gdi_overlay = gdi_overlay;
    surface->channel_fd = -1;
    wl_list_init(&surface->buffers);
    wl_list_init(&surface->slices);
    wl_list_init(&surface->link);

    if (!(surface->wl_surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    wl_surface_set_user_data(surface->wl_surface, hwnd);

    empty_region = wl_compositor_create_region(process_wayland.wl_compositor);
    if (!empty_region) goto err;
    wl_surface_set_input_region(surface->wl_surface, empty_region);
    wl_region_destroy(empty_region);

    if (!(surface->wp_viewport = wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                                            surface->wl_surface))) goto err;

    if (!(surface->wl_subsurface = wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                                                   surface->wl_surface,
                                                                   parent->wl_surface))) goto err;
    wl_subsurface_set_desync(surface->wl_subsurface);

    wl_list_insert(parent->hwnd_dmabuf_surfaces.prev, &surface->link);
    surface->linked = TRUE;
    return surface;

err:
    if (surface->wl_subsurface) wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->wp_viewport) wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_surface) wl_surface_destroy(surface->wl_surface);
    free(surface);
    return NULL;
}

static struct wayland_hwnd_dmabuf_surface *wayland_hwnd_dmabuf_surface_create_direct(
        struct wayland_surface *parent, HWND hwnd)
{
    struct wayland_hwnd_dmabuf_surface *surface;

    if (!(surface = calloc(1, sizeof(*surface)))) return NULL;
    surface->hwnd = hwnd;
    surface->parent = parent;
    surface->wl_surface = parent->wl_surface;
    surface->wp_viewport = parent->wp_viewport;
    surface->channel_fd = -1;
    surface->direct = TRUE;
    wl_list_init(&surface->buffers);
    wl_list_init(&surface->slices);
    wl_list_init(&surface->link);

    parent->direct_dmabuf_surface = surface;
    TRACE("hwnd=%p using direct parent dmabuf\n", hwnd);
    return surface;
}

static void wayland_hwnd_dmabuf_surface_set_opaque(struct wayland_hwnd_dmabuf_surface *surface,
                                                   int width, int height)
{
    struct wl_region *region = NULL;

    if (surface->current && surface->current->alpha_mode == HWND_DMABUF_ALPHA_MODE_IGNORE &&
        (region = wl_compositor_create_region(process_wayland.wl_compositor)))
    {
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(surface->wl_surface, region);
        wl_region_destroy(region);
    }
    else wl_surface_set_opaque_region(surface->wl_surface, NULL);
}

struct wayland_hwnd_dmabuf_geometry
{
    double source_x, source_y;
    double source_width, source_height;
    int x, y;
    int width, height;
};

static void wayland_surface_classify_child_visibility(struct wayland_surface *surface,
        const RECT *dst, struct wayland_child_visibility_info *info)
{
    HRGN dst_region, visible_region;
    RGNDATA *data;

    info->visibility = WAYLAND_CHILD_VISIBILITY_AS_IS;
    info->rect = *dst;
    info->rect_count = 0;
    if (!surface->child_region) return;

    info->visibility = WAYLAND_CHILD_VISIBILITY_UNMASKABLE;

    if (!(dst_region = NtGdiCreateRectRgn(dst->left, dst->top, dst->right, dst->bottom)))
        return;
    if (!(visible_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        NtGdiDeleteObjectApp(dst_region);
        return;
    }

    if (NtGdiCombineRgn(visible_region, dst_region, surface->child_region, RGN_AND) != ERROR)
    {
        if (NtGdiEqualRgn(visible_region, dst_region))
        {
            info->visibility = WAYLAND_CHILD_VISIBILITY_AS_IS;
        }
        else if ((data = get_region_data(visible_region)))
        {
            info->rect = data->rdh.rcBound;
            info->rect_count = data->rdh.nCount;
            if (data->rdh.nCount == 1)
            {
                info->rect = *(RECT *)data->Buffer;
                info->visibility = WAYLAND_CHILD_VISIBILITY_CROPPED;
            }
            free(data);
        }
    }

    NtGdiDeleteObjectApp(visible_region);
    NtGdiDeleteObjectApp(dst_region);
}

BOOL wayland_surface_client_is_unmaskable(struct wayland_surface *surface)
{
    struct wayland_child_visibility_info visibility;
    RECT dst;

    dst = surface->window.client_rect;
    OffsetRect(&dst, -surface->window.rect.left, -surface->window.rect.top);
    wayland_surface_classify_child_visibility(surface, &dst, &visibility);
    return visibility.visibility == WAYLAND_CHILD_VISIBILITY_UNMASKABLE;
}

static void wayland_surface_update_child_visibility(const RECT *dst,
        const struct wayland_child_visibility_info *info, struct wayland_visual_constraint *cache)
{
    if (info->visibility == WAYLAND_CHILD_VISIBILITY_AS_IS)
    {
        cache->valid = FALSE;
        return;
    }

    if (cache->valid && cache->visibility == info->visibility &&
        EqualRect(&cache->dst, dst) && EqualRect(&cache->rect, &info->rect) &&
        cache->rect_count == info->rect_count)
        return;

    cache->valid = TRUE;
    cache->visibility = info->visibility;
    cache->dst = *dst;
    cache->rect = info->rect;
    cache->rect_count = info->rect_count;
}

static BOOL wayland_hwnd_dmabuf_surface_compute_geometry(struct wayland_surface *parent,
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_info_t *info,
        struct wayland_hwnd_dmabuf_geometry *geometry)
{
    RECT rect = wine_server_get_rect(info->client), clipped, client;
    RECT dst;
    struct wayland_child_visibility_info visibility;
    int rect_width, rect_height;

    if (surface->gdi_overlay)
    {
        int width = parent->window.rect.right - parent->window.rect.left;
        int height = parent->window.rect.bottom - parent->window.rect.top;

        if (width <= 0 || height <= 0) return FALSE;
        geometry->source_x = 0;
        geometry->source_y = 0;
        geometry->source_width = max(1, min(width, surface->current->width));
        geometry->source_height = max(1, min(height, surface->current->height));
        wayland_surface_coords_from_window(parent, 0, 0, &geometry->x, &geometry->y);
        wayland_surface_coords_from_window(parent, width, height,
                                           &geometry->width, &geometry->height);
        geometry->width = max(1, geometry->width);
        geometry->height = max(1, geometry->height);
        return TRUE;
    }

    rect_width = rect.right - rect.left;
    rect_height = rect.bottom - rect.top;
    if (rect_width <= 0 || rect_height <= 0) return FALSE;

    client.left = client.top = 0;
    client.right = parent->window.client_rect.right - parent->window.client_rect.left;
    client.bottom = parent->window.client_rect.bottom - parent->window.client_rect.top;

    clipped.left = max(rect.left, client.left);
    clipped.top = max(rect.top, client.top);
    clipped.right = min(rect.right, client.right);
    clipped.bottom = min(rect.bottom, client.bottom);
    if (IsRectEmpty(&clipped)) return FALSE;

    dst = clipped;
    OffsetRect(&dst, parent->window.client_rect.left - parent->window.rect.left,
               parent->window.client_rect.top - parent->window.rect.top);

    wayland_surface_classify_child_visibility(parent, &dst, &visibility);
    wayland_surface_update_child_visibility(&dst, &visibility, &surface->visual_constraint);

    if (visibility.visibility == WAYLAND_CHILD_VISIBILITY_CROPPED)
        dst = visibility.rect;

    clipped = dst;
    OffsetRect(&clipped, parent->window.rect.left - parent->window.client_rect.left,
               parent->window.rect.top - parent->window.client_rect.top);

    geometry->source_x = (double)(clipped.left - rect.left) * surface->current->width / rect_width;
    geometry->source_y = (double)(clipped.top - rect.top) * surface->current->height / rect_height;
    geometry->source_width = (double)(clipped.right - clipped.left) * surface->current->width / rect_width;
    geometry->source_height = (double)(clipped.bottom - clipped.top) * surface->current->height / rect_height;

    wayland_surface_coords_from_window(parent, dst.left, dst.top,
                                       &geometry->x, &geometry->y);
    wayland_surface_coords_from_window(parent, dst.right - dst.left,
                                       dst.bottom - dst.top,
                                       &geometry->width, &geometry->height);
    geometry->width = max(1, geometry->width);
    geometry->height = max(1, geometry->height);

    return TRUE;
}

static void wayland_hwnd_dmabuf_surface_apply_geometry(struct wayland_hwnd_dmabuf_surface *surface,
                                                       const struct wayland_hwnd_dmabuf_geometry *geometry,
                                                       struct wl_surface *sibling)
{
    wayland_hwnd_dmabuf_surface_clear_slices(surface);
    wl_subsurface_set_position(surface->wl_subsurface, geometry->x, geometry->y);
    wl_subsurface_place_below(surface->wl_subsurface, sibling);
    wp_viewport_set_source(surface->wp_viewport,
                           wl_fixed_from_double(geometry->source_x),
                           wl_fixed_from_double(geometry->source_y),
                           wl_fixed_from_double(geometry->source_width),
                           wl_fixed_from_double(geometry->source_height));
    wp_viewport_set_destination(surface->wp_viewport, geometry->width, geometry->height);
    wayland_hwnd_dmabuf_surface_set_opaque(surface, geometry->width, geometry->height);
    surface->stack_bottom = surface->wl_surface;
}

static enum wayland_hwnd_dmabuf_configure_result wayland_hwnd_dmabuf_surface_configure_slices(
        struct wayland_surface *parent, struct wayland_hwnd_dmabuf_surface *surface,
        const hwnd_dmabuf_frame_info_t *info, struct wl_surface *sibling, BOOL attach_frame)
{
    RECT child_rect = wine_server_get_rect(info->client), clipped, client, dst;
    struct wayland_hwnd_dmabuf_slice_geometry layout[WAYLAND_DMABUF_MAX_SLICES];
    RECT *visible_rects;
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;
    struct wayland_child_visibility_info visibility;
    BOOL layout_matches;
    RGNDATA *data;
    RECT *rect, *end;
    int child_width, child_height;
    unsigned int count = 0, region_count, i;

    if (surface->gdi_overlay || surface->direct || !buffer || !parent->child_region)
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;

    child_width = child_rect.right - child_rect.left;
    child_height = child_rect.bottom - child_rect.top;
    if (child_width <= 0 || child_height <= 0) return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;

    client.left = client.top = 0;
    client.right = parent->window.client_rect.right - parent->window.client_rect.left;
    client.bottom = parent->window.client_rect.bottom - parent->window.client_rect.top;

    clipped.left = max(child_rect.left, client.left);
    clipped.top = max(child_rect.top, client.top);
    clipped.right = min(child_rect.right, client.right);
    clipped.bottom = min(child_rect.bottom, client.bottom);
    if (IsRectEmpty(&clipped)) return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;

    dst = clipped;
    OffsetRect(&dst, parent->window.client_rect.left - parent->window.rect.left,
               parent->window.client_rect.top - parent->window.rect.top);

    wayland_surface_classify_child_visibility(parent, &dst, &visibility);
    wayland_surface_update_child_visibility(&dst, &visibility, &surface->visual_constraint);
    if (visibility.visibility != WAYLAND_CHILD_VISIBILITY_UNMASKABLE)
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;

    if (!(data = get_region_data(parent->child_region)))
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;

    region_count = data->rdh.nCount;
    if (!(visible_rects = calloc(region_count ? region_count : 1, sizeof(*visible_rects))))
    {
        free(data);
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;
    }
    rect = (RECT *)data->Buffer;
    end = rect + data->rdh.nCount;
    for (; rect < end; rect++)
    {
        RECT visible = *rect;

        visible.left = max(visible.left, dst.left);
        visible.top = max(visible.top, dst.top);
        visible.right = min(visible.right, dst.right);
        visible.bottom = min(visible.bottom, dst.bottom);
        if (IsRectEmpty(&visible)) continue;
        visible_rects[count++] = visible;
    }
    free(data);

    if (!count)
    {
        free(visible_rects);
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;
    }
    wayland_hwnd_dmabuf_try_cover_slice_rects(&visible_rects, &count);
    if (count > WAYLAND_DMABUF_MAX_SLICES)
    {
        free(visible_rects);
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;
    }

    for (i = 0; i < count; i++)
    {
        RECT *visible = &visible_rects[i], source_rect = *visible;
        double sx, sy, sw, sh;

        OffsetRect(&source_rect, parent->window.rect.left - parent->window.client_rect.left,
                   parent->window.rect.top - parent->window.client_rect.top);

        sx = (double)(source_rect.left - child_rect.left) * buffer->width / child_width;
        sy = (double)(source_rect.top - child_rect.top) * buffer->height / child_height;
        sw = (double)(source_rect.right - source_rect.left) * buffer->width / child_width;
        sh = (double)(source_rect.bottom - source_rect.top) * buffer->height / child_height;

        wayland_surface_coords_from_window(parent, visible->left, visible->top,
                                           &layout[i].x, &layout[i].y);
        wayland_surface_coords_from_window(parent, visible->right - visible->left,
                                           visible->bottom - visible->top,
                                           &layout[i].width, &layout[i].height);
        layout[i].width = max(1, layout[i].width);
        layout[i].height = max(1, layout[i].height);
        layout[i].source_x = wl_fixed_from_double(sx);
        layout[i].source_y = wl_fixed_from_double(sy);
        layout[i].source_width = wl_fixed_from_double(sw);
        layout[i].source_height = wl_fixed_from_double(sh);
    }
    free(visible_rects);

    layout_matches = wayland_hwnd_dmabuf_surface_slice_layout_matches(surface, sibling, layout, count);

    if (layout_matches)
        surface->stack_bottom = wayland_hwnd_dmabuf_surface_slice_stack_bottom(surface);

    if (layout_matches && !attach_frame)
        return WAYLAND_HWNDDMABUF_CONFIGURE_NOOP;

    if (!layout_matches)
    {
        if (!wayland_hwnd_dmabuf_surface_apply_slice_layout(surface, layout, count, sibling))
        {
            wayland_hwnd_dmabuf_surface_clear_slices(surface);
            return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;
        }
    }

    if (attach_frame || !layout_matches)
    {
        wl_surface_attach(surface->wl_surface, NULL, 0, 0);
        wl_surface_commit(surface->wl_surface);
        surface->current_attach_count = 0;
    }

    if ((attach_frame || !layout_matches) && !wayland_hwnd_dmabuf_surface_attach_slices(surface))
    {
        wayland_hwnd_dmabuf_surface_clear_slices(surface);
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;
    }

    return WAYLAND_HWNDDMABUF_CONFIGURE_UPDATED;
}

static enum wayland_hwnd_dmabuf_configure_result wayland_hwnd_dmabuf_surface_configure(
        struct wayland_surface *parent, struct wayland_hwnd_dmabuf_surface *surface,
        const hwnd_dmabuf_frame_info_t *info, struct wl_surface *sibling, BOOL attach_frame)
{
    struct wayland_hwnd_dmabuf_geometry geometry;
    enum wayland_hwnd_dmabuf_configure_result ret;

    ret = wayland_hwnd_dmabuf_surface_configure_slices(parent, surface, info, sibling, attach_frame);
    if (ret != WAYLAND_HWNDDMABUF_CONFIGURE_FAILED)
        return ret;

    if (!wayland_hwnd_dmabuf_surface_compute_geometry(parent, surface, info, &geometry))
        return WAYLAND_HWNDDMABUF_CONFIGURE_FAILED;

    wayland_hwnd_dmabuf_surface_apply_geometry(surface, &geometry, sibling);
    return WAYLAND_HWNDDMABUF_CONFIGURE_UPDATED;
}

/* Claim this child's consumer channel end once. The producer mints it lazily. Retry. */
static void wayland_hwnd_dmabuf_surface_claim_channel(struct wayland_hwnd_dmabuf_surface *surface)
{
    HANDLE handle = 0;
    int fd = -1;

    if (surface->channel_fd >= 0) return;
    if (surface->gdi_overlay)
    {
        if (wine_hwnd_dmabuf_claim_gdi_overlay_channel(surface->hwnd, &handle) != HWND_DMABUF_OK || !handle)
            return;
    }
    else if (wine_hwnd_dmabuf_claim_channel(surface->hwnd, &handle) != HWND_DMABUF_OK || !handle)
        return;
    if (wine_server_handle_to_fd(handle, FILE_READ_DATA | FILE_WRITE_DATA, &fd, NULL))
        fd = -1;
    NtClose(handle);
    surface->channel_fd = fd;
    if (fd >= 0) TRACE("hwnd=%p claimed %s socket channel fd %d\n", surface->hwnd,
                       surface->gdi_overlay ? "gdi overlay" : "dmabuf", fd);
}

/* Receive one frame. Returns 1 on success, 0 if empty, -1 on EOF. */
static int wayland_hwnd_dmabuf_channel_recv_one(int channel_fd, hwnd_dmabuf_frame_desc_t *desc,
                                                int *out_fd)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    struct cmsghdr *cmsg;
    struct iovec iov;
    int fd = -1;
    ssize_t n;

    iov.iov_base = desc;
    iov.iov_len = sizeof(*desc);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    n = recvmsg(channel_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n == 0) return -1;                       /* peer closed the channel */
    if (n != (ssize_t)sizeof(*desc)) return 0;   /* EAGAIN or short read: nothing usable now */

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    *out_fd = fd;
    return 1;
}

static BOOL wayland_hwnd_dmabuf_desc_is_shm(const hwnd_dmabuf_frame_desc_t *desc)
{
    return (desc->flags & HWND_DMABUF_FLAG_SHM) != 0;
}

static BOOL wayland_hwnd_shm_format_from_fourcc(unsigned int fourcc, uint32_t *format)
{
    switch (fourcc)
    {
    case HWND_DMABUF_SHM_FORMAT_XRGB8888:
        if (format) *format = WL_SHM_FORMAT_XRGB8888;
        return TRUE;
    case HWND_DMABUF_SHM_FORMAT_ARGB8888:
        if (format) *format = WL_SHM_FORMAT_ARGB8888;
        return TRUE;
    default:
        return FALSE;
    }
}

static struct wl_buffer *wayland_hwnd_dmabuf_create_shm_wl_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc, int fd)
{
    struct wl_shm_pool *pool;
    struct wl_buffer *wl_buffer;
    uint32_t format;
    UINT64 size;

    if (!process_wayland.wl_shm || fd < 0) return NULL;
    if (!wayland_hwnd_shm_format_from_fourcc(desc->fourcc, &format)) return NULL;
    if (!desc->stride || desc->height > (UINT_MAX - desc->offset) / desc->stride) return NULL;
    size = desc->offset + (UINT64)desc->stride * desc->height;
    if (!size || size > INT_MAX) return NULL;

    if (!(pool = wl_shm_create_pool(process_wayland.wl_shm, fd, (int)size)))
        return NULL;
    wl_buffer = wl_shm_pool_create_buffer(pool, desc->offset, desc->width, desc->height,
                                          desc->stride, format);
    wl_shm_pool_destroy(pool);
    if (!wl_buffer)
        WARN("hwnd=%p failed to create shm buffer size=%ux%u stride=%u fourcc=%#x format=%#x\n",
             surface->hwnd, desc->width, desc->height, desc->stride, desc->fourcc, format);
    return wl_buffer;
}

static struct wl_buffer *wayland_hwnd_dmabuf_create_dmabuf_wl_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc, int fd)
{
    struct zwp_linux_buffer_params_v1 *params;
    unsigned int i, plane_count = desc->plane_count;
    struct wl_buffer *wl_buffer;

    if (!process_wayland.zwp_linux_dmabuf_v1 || fd < 0) return NULL;

    if (plane_count < 1) plane_count = 1;
    if (plane_count > HWND_DMABUF_MAX_PLANES) plane_count = HWND_DMABUF_MAX_PLANES;

    if (!(params = zwp_linux_dmabuf_v1_create_params(process_wayland.zwp_linux_dmabuf_v1)))
        return NULL;
    /* all planes share the one exported memory fd */
    for (i = 0; i < plane_count; i++)
        zwp_linux_buffer_params_v1_add(params, fd, i, desc->plane_offsets[i], desc->plane_strides[i],
                                       desc->modifier >> 32, desc->modifier & 0xffffffff);
    wl_buffer = zwp_linux_buffer_params_v1_create_immed(params, desc->width, desc->height,
                                                        desc->fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!wl_buffer)
        WARN("hwnd=%p failed to create dmabuf buffer size=%ux%u fourcc=%#x "
             "modifier=0x%08x%08x stride=%u alpha=%u\n",
             surface->hwnd, desc->width, desc->height, desc->fourcc,
             (unsigned int)(desc->modifier >> 32), (unsigned int)desc->modifier,
             desc->stride, desc->alpha_mode);
    return wl_buffer;
}

/* Build a wl_buffer wrapping the producer fd plus its tracking buffer, cached on the
 * surface. Does not close fd (caller owns it). Returns NULL on failure. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_create_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc,
        int fd, BOOL stable_slot)
{
    struct wayland_hwnd_dmabuf_buffer *buffer;
    struct wl_buffer *wl_buffer;
    int data_fd;

    if (fd < 0) return NULL;
    if ((data_fd = dup(fd)) < 0) return NULL;

    if (wayland_hwnd_dmabuf_desc_is_shm(desc))
        wl_buffer = wayland_hwnd_dmabuf_create_shm_wl_buffer(surface, desc, fd);
    else
        wl_buffer = wayland_hwnd_dmabuf_create_dmabuf_wl_buffer(surface, desc, fd);
    if (!wl_buffer)
    {
        close(data_fd);
        return NULL;
    }

    if (!(buffer = calloc(1, sizeof(*buffer))))
    {
        close(data_fd);
        wl_buffer_destroy(wl_buffer);
        return NULL;
    }
    buffer->channel_fd = -1;
    buffer->data_fd = data_fd;
    buffer->wl_buffer = wl_buffer;
    buffer->ref = 1;  /* owner ref. A compositor ref is added per attach in the update loop */
    buffer->surface = surface;
    buffer->producer_unique_id = desc->producer_unique_id;
    buffer->image_id = desc->image_id;
    buffer->ring_generation = desc->ring_generation;
    buffer->desc = *desc;
    buffer->fourcc = desc->fourcc;
    buffer->stride = desc->stride;
    buffer->offset = desc->offset;
    buffer->modifier = desc->modifier;
    buffer->width = desc->width;
    buffer->height = desc->height;
    buffer->stable_slot = stable_slot;
    buffer->cache_valid = stable_slot ? TRUE : FALSE;
    buffer->release_flags = HWND_DMABUF_RELEASE_ORPHANED;
    buffer->channel_fd = surface->channel_fd >= 0 ? dup(surface->channel_fd) : -1;
    wl_list_insert(surface->buffers.prev, &buffer->link);
    wl_buffer_add_listener(buffer->wl_buffer, &wayland_hwnd_dmabuf_buffer_listener, buffer);
    return buffer;
}

/* Make buffer the surface's current frame. Publishing is separate so direct
 * parent mode can validate the frame before it attaches anything. */
static void wayland_hwnd_dmabuf_set_frame(struct wayland_hwnd_dmabuf_surface *surface,
        struct wayland_hwnd_dmabuf_buffer *buffer, const hwnd_dmabuf_frame_desc_t *desc)
{
    if (wayland_hwnd_dmabuf_buffer_exchange_release_token(buffer, desc->release_token))
        WARN("hwnd=%p slot=%u overwrote an unreleased token\n", surface->hwnd, desc->image_id);
    buffer->alpha_mode = desc->alpha_mode;
    buffer->dirty_count = min(desc->dirty_count, HWND_DMABUF_MAX_DIRTY_RECTS);
    memcpy(buffer->dirty_rects, desc->dirty_rects, sizeof(buffer->dirty_rects));
    buffer->release_flags = HWND_DMABUF_RELEASE_ORPHANED;
    surface->current = buffer;
    surface->frame_seq = desc->frame_seq;
    surface->current_committed = FALSE;
}

static void wayland_hwnd_dmabuf_attach_current(struct wayland_hwnd_dmabuf_surface *surface)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;

    if (!buffer) return;
    if (!surface->logged_first_attach)
    {
        TRACE("hwnd=%p attaching producer slot=%u size=%dx%d fourcc=%#x alpha=%u\n",
              surface->hwnd, buffer->image_id, buffer->width, buffer->height,
              buffer->fourcc, buffer->alpha_mode);
        surface->logged_first_attach = TRUE;
    }
    buffer->release_flags = HWND_DMABUF_RELEASE_PRESENTED;
    surface->current_attach_count = 1;
    surface->stack_bottom = surface->wl_surface;
    wl_surface_attach(surface->wl_surface, buffer->wl_buffer, 0, 0);
    if (surface->gdi_overlay && buffer->dirty_count)
    {
        unsigned int i;

        for (i = 0; i < buffer->dirty_count; i++)
        {
            int left = buffer->dirty_rects[i][0], top = buffer->dirty_rects[i][1];
            int right = buffer->dirty_rects[i][2], bottom = buffer->dirty_rects[i][3];

            if (right <= left || bottom <= top) continue;
            wl_surface_damage_buffer(surface->wl_surface, left, top, right - left, bottom - top);
        }
    }
    else
        wl_surface_damage_buffer(surface->wl_surface, 0, 0, buffer->width, buffer->height);
    if (!surface->gdi_overlay)
        wp_viewport_set_source(surface->wp_viewport, 0, 0,
                               wl_fixed_from_int(buffer->width), wl_fixed_from_int(buffer->height));
}

static void wayland_hwnd_dmabuf_drop_current_frame(struct wayland_hwnd_dmabuf_surface *surface,
                                                   unsigned int flags)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = surface->current;

    if (!buffer) return;
    wayland_hwnd_dmabuf_buffer_send_release(buffer, flags, buffer->stable_slot);
    surface->current = NULL;
    surface->current_committed = FALSE;
    wayland_hwnd_dmabuf_surface_clear_slices(surface);
    if (!buffer->stable_slot) wayland_hwnd_dmabuf_buffer_reap(buffer);
}

/* Stable-slot path (HWND_DMABUF_FLAG_STABLE_SLOT): ensure this slot is imported and cached
 * (no attach). The producer may send a slot fd once, then fd-less references:
 *  - cache hit (exact layout match): reuse the cached wl_buffer.
 *  - fd-bearing miss: import and cache it.
 *  - fd-less miss: the slot is not cached -> return NULL. The failed release clears the
 *    producer's cache state so it resends the fd.
 * Stale slots (new producer/ring generation or a changed layout) are reaped first. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_cache_slot(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_desc_t *desc,
        int fd, BOOL *created_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL, *it, *it_next;

    wl_list_for_each_safe(it, it_next, &surface->buffers, link)
        if (it->producer_unique_id != desc->producer_unique_id ||
            it->ring_generation != desc->ring_generation)
        {
            TRACE("hwnd=%p cache-drop stale producer/gen slot=%u gen %u->%u\n",
                  surface->hwnd, it->image_id, it->ring_generation, desc->ring_generation);
            wayland_hwnd_dmabuf_buffer_reap(it);
        }

    wl_list_for_each_safe(it, it_next, &surface->buffers, link)
    {
        if (it->image_id != desc->image_id) continue;
        if (it->width == (int)desc->width && it->height == (int)desc->height &&
            it->fourcc == desc->fourcc && it->modifier == desc->modifier &&
            it->stride == desc->stride && it->offset == desc->offset)
            buffer = it;
        else
        {
            TRACE("hwnd=%p cache-drop layout-mismatch slot=%u\n", surface->hwnd, it->image_id);
            wayland_hwnd_dmabuf_buffer_reap(it);
        }
        break;
    }

    if (buffer)
        return buffer;
    if (fd < 0)
    {
        TRACE("hwnd=%p cache-miss-no-fd slot=%u (awaiting fd resend)\n",
              surface->hwnd, desc->image_id);
        return NULL;
    }
    if (!(buffer = wayland_hwnd_dmabuf_create_buffer(surface, desc, fd, TRUE))) return NULL;
    TRACE("hwnd=%p cache-miss import slot=%u gen=%u frame_seq=%u\n",
          surface->hwnd, desc->image_id, desc->ring_generation, desc->frame_seq);
    *created_buffer = TRUE;
    return buffer;
}

/* A queued frame superseded by a newer one. For a stable slot, still import/cache it so a
 * later reference resolves. Then release the present so the producer can recycle the slot.
 * Uncached frames are simply dropped (each is a fresh dmabuf, nothing to keep). */
static void wayland_hwnd_dmabuf_retire_frame(struct wayland_hwnd_dmabuf_surface *surface,
        const hwnd_dmabuf_frame_desc_t *desc, int fd, BOOL *created_buffer)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL;
    unsigned int flags = HWND_DMABUF_RELEASE_DROPPED;

    if (desc->flags & HWND_DMABUF_FLAG_STABLE_SLOT)
    {
        buffer = wayland_hwnd_dmabuf_cache_slot(surface, desc, fd, created_buffer);
        if (buffer) flags |= HWND_DMABUF_RELEASE_CACHED;
        else flags = HWND_DMABUF_RELEASE_FAILED;
    }
    if (fd >= 0) close(fd);
    wayland_hwnd_dmabuf_send_release(surface, desc->producer_unique_id, desc->release_token,
                                     flags, desc->image_id, desc->ring_generation);
}

static BOOL wayland_hwnd_dmabuf_desc_is_valid(const hwnd_dmabuf_frame_desc_t *desc)
{
    return desc->version == HWND_DMABUF_DESC_VERSION_V1 && desc->width && desc->height &&
           desc->stride && desc->fourcc && desc->release_token;
}

static BOOL wayland_hwnd_dmabuf_desc_format_supported(const hwnd_dmabuf_frame_desc_t *desc)
{
    if (wayland_hwnd_dmabuf_desc_is_shm(desc))
        return process_wayland.wl_shm && wayland_hwnd_shm_format_from_fourcc(desc->fourcc, NULL);
    return process_wayland.zwp_linux_dmabuf_v1 &&
           wayland_dmabuf_format_supported(desc->fourcc, desc->modifier);
}

/* Drain the channel, importing every fd-bearing slot so none is lost, then show the newest
 * frame. Stable-slot producers send a slot's fd once and fd-less references after. The fd
 * is dup'd + passed via SCM_RIGHTS once per slot, not once per frame. created_buffer reports
 * a freshly imported wl_buffer. attached_frame reports a frame was published. */
static struct wayland_hwnd_dmabuf_buffer *wayland_hwnd_dmabuf_surface_import_buffer(
        struct wayland_hwnd_dmabuf_surface *surface, const hwnd_dmabuf_frame_info_t *info,
        BOOL *created_buffer, BOOL *attached_frame)
{
    struct wayland_hwnd_dmabuf_buffer *buffer = NULL;
    hwnd_dmabuf_frame_desc_t pdesc, desc;
    BOOL have_pending = FALSE;
    BOOL retried = FALSE;
    int pfd = -1, fd, r;

    *created_buffer = FALSE;
    *attached_frame = FALSE;
    memset(&pdesc, 0, sizeof(pdesc));

retry:
    wayland_hwnd_dmabuf_surface_claim_channel(surface);
    if (surface->channel_fd < 0) return surface->current;
    if (surface->consumer_state != WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE &&
        !wayland_hwnd_dmabuf_surface_send_consumer_state(surface,
                HWND_DMABUF_RELEASE_CONSUMER_ACTIVE))
        surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE;

    while ((r = wayland_hwnd_dmabuf_channel_recv_one(surface->channel_fd, &desc, &fd)) > 0)
    {
        BOOL desc_valid = wayland_hwnd_dmabuf_desc_is_valid(&desc);
        BOOL format_supported = desc_valid &&
                wayland_hwnd_dmabuf_desc_format_supported(&desc);
        BOOL overlay_frame = (desc.flags & HWND_DMABUF_FLAG_GDI_OVERLAY) != 0;

        if (!desc_valid || !format_supported || overlay_frame != surface->gdi_overlay)
        {
            WARN("hwnd=%p import rejected info_seq=%u desc_seq=%u version=%u "
                 "size=%ux%u stride=%u offset=%u fourcc=%#x (%c%c%c%c) "
                 "modifier=0x%08x%08x token=0x%08x%08x supported=%u\n",
                 surface->hwnd, info->frame_seq, desc.frame_seq, desc.version,
                 desc.width, desc.height, desc.stride, desc.offset, desc.fourcc,
                 desc.fourcc & 0xff, (desc.fourcc >> 8) & 0xff,
                 (desc.fourcc >> 16) & 0xff, (desc.fourcc >> 24) & 0xff,
                 (unsigned int)(desc.modifier >> 32), (unsigned int)desc.modifier,
                 (unsigned int)(desc.release_token >> 32),
                 (unsigned int)desc.release_token, format_supported);
            if (fd >= 0) close(fd);
            if (desc.release_token)
                wayland_hwnd_dmabuf_send_release(surface, desc.producer_unique_id, desc.release_token,
                                                 HWND_DMABUF_RELEASE_FAILED,
                                                 desc.image_id, desc.ring_generation);
            continue;
        }
        if (have_pending)
            wayland_hwnd_dmabuf_retire_frame(surface, &pdesc, pfd, created_buffer);
        pdesc = desc;
        pfd = fd;
        have_pending = TRUE;
    }
    if (r < 0)
    {
        close(surface->channel_fd);
        surface->channel_fd = -1;
        surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_UNKNOWN;
        if (!have_pending && !retried)
        {
            retried = TRUE;
            goto retry;
        }
    }

    if (!have_pending) return surface->current;

    /* Show the newest drained frame. */
    if (pdesc.flags & HWND_DMABUF_FLAG_STABLE_SLOT)
        buffer = wayland_hwnd_dmabuf_cache_slot(surface, &pdesc, pfd, created_buffer);
    else if ((buffer = wayland_hwnd_dmabuf_create_buffer(surface, &pdesc, pfd, FALSE)))
    {
        *created_buffer = TRUE;
        if (!surface->logged_first_import)
        {
            TRACE("hwnd=%p imported %s seq=%u slot=%u size=%ux%u "
                  "fourcc=%#x modifier=0x%08x%08x alpha=%u flags=%#x\n",
                  surface->hwnd, wayland_hwnd_dmabuf_desc_is_shm(&pdesc) ? "shm" : "dmabuf",
                  pdesc.frame_seq, pdesc.image_id,
                  pdesc.width, pdesc.height, pdesc.fourcc,
                  (unsigned int)(pdesc.modifier >> 32),
                  (unsigned int)pdesc.modifier, pdesc.alpha_mode, pdesc.flags);
            surface->logged_first_import = TRUE;
        }
    }

    if (pfd >= 0) close(pfd);

    if (!buffer)
    {
        wayland_hwnd_dmabuf_send_release(surface, pdesc.producer_unique_id, pdesc.release_token,
                                         HWND_DMABUF_RELEASE_FAILED,
                                         pdesc.image_id, pdesc.ring_generation);
        return surface->current;
    }
    wayland_hwnd_dmabuf_set_frame(surface, buffer, &pdesc);
    *attached_frame = TRUE;
    return buffer;
}

static BOOL wayland_surface_client_fills_window(struct wayland_surface *surface)
{
    return EqualRect(&surface->window.rect, &surface->window.client_rect);
}

static BOOL wayland_hwnd_dmabuf_frame_covers_client(struct wayland_surface *surface,
                                                    const hwnd_dmabuf_frame_info_t *info)
{
    RECT rect = wine_server_get_rect(info->client);
    int width = surface->window.client_rect.right - surface->window.client_rect.left;
    int height = surface->window.client_rect.bottom - surface->window.client_rect.top;

    return width > 0 && height > 0 &&
           rect.left <= 0 && rect.top <= 0 &&
           rect.right >= width && rect.bottom >= height;
}

static BOOL wayland_surface_has_region_constraints(struct wayland_surface *surface)
{
    return surface->shaped || surface->occlusion_clipped;
}

static BOOL wayland_surface_direct_dmabuf_candidate(struct wayland_surface *surface,
                                                    struct wayland_win_data *data,
                                                    const hwnd_dmabuf_frame_info_t *frames,
                                                    unsigned int count)
{
    if (count != 1) return FALSE;
    if (!wayland_surface_is_toplevel(surface)) return FALSE;
    if (frames[0].opened & HWND_DMABUF_FRAME_GDI_OVERLAY) return FALSE;
    if (!data || data->client_surface) return FALSE;
    if (data->window_contents) return FALSE;
    if (wayland_surface_has_region_constraints(surface))
    {
        TRACE("hwnd=%p direct parent dmabuf rejected for visual constraints\n", surface->hwnd);
        return FALSE;
    }
    if (!wayland_surface_client_fills_window(surface)) return FALSE;
    return wayland_hwnd_dmabuf_frame_covers_client(surface, &frames[0]);
}

static BOOL wayland_surface_replace_direct_dmabuf_with_shm(struct wayland_surface *surface,
                                                           struct wayland_win_data *data)
{
    struct wayland_hwnd_dmabuf_surface *direct = surface->direct_dmabuf_surface;

    if (!direct || !data || !data->window_contents || !wayland_surface_reconfigure(surface))
        return FALSE;

    wl_surface_set_opaque_region(surface->wl_surface, NULL);
    wayland_surface_attach_shm(surface, data->window_contents,
                               data->window_contents->damage_region);
    wl_surface_commit(surface->wl_surface);
    wayland_hwnd_dmabuf_surface_destroy(direct);
    return TRUE;
}

static void carrier_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;

    TRACE("shm_buffer=%p\n", shm_buffer);
    shm_buffer->busy = FALSE;
    wayland_shm_buffer_unref(shm_buffer);
}

static const struct wl_buffer_listener carrier_buffer_listener =
{
    carrier_buffer_release
};

static BOOL wayland_surface_attach_carrier(struct wayland_surface *surface, BOOL opaque,
                                           BOOL single_pixel)
{
    struct wayland_shm_buffer *shm_buffer;
    struct wl_region *region = NULL;
    enum wl_shm_format format;
    int width, height, buffer_width, buffer_height;

    width = max(1, surface->window.rect.right - surface->window.rect.left);
    height = max(1, surface->window.rect.bottom - surface->window.rect.top);
    /* The viewport stretches a single black pixel over the parent. */
    buffer_width = single_pixel ? 1 : width;
    buffer_height = single_pixel ? 1 : height;

    if (surface->carrier_attached && surface->carrier_opaque == opaque &&
        surface->carrier_single_pixel == single_pixel &&
        surface->carrier_width == width && surface->carrier_height == height)
        return TRUE;

    if (!wayland_surface_reconfigure(surface)) return FALSE;

    format = opaque ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888;
    if (!(shm_buffer = wayland_shm_buffer_create(buffer_width, buffer_height, format)))
        return FALSE;

    if (opaque && !(region = wl_compositor_create_region(process_wayland.wl_compositor)))
    {
        wayland_shm_buffer_unref(shm_buffer);
        return FALSE;
    }

    memset(shm_buffer->map_data, 0, shm_buffer->map_size);
    wl_buffer_add_listener(shm_buffer->wl_buffer, &carrier_buffer_listener, shm_buffer);
    if (region)
    {
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(surface->wl_surface, region);
        wl_region_destroy(region);
    }
    else wl_surface_set_opaque_region(surface->wl_surface, NULL);
    wayland_surface_attach_shm_buffer(surface, shm_buffer, shm_buffer->damage_region);
    wl_surface_commit(surface->wl_surface);
    wayland_shm_buffer_unref(shm_buffer);
    surface->carrier_attached = TRUE;
    surface->carrier_opaque = opaque;
    surface->carrier_single_pixel = single_pixel;
    /* Cache the viewport destination, not the backing buffer size. */
    surface->carrier_width = width;
    surface->carrier_height = height;
    return TRUE;
}

BOOL wayland_surface_attach_transparent_carrier(struct wayland_surface *surface)
{
    return wayland_surface_attach_carrier(surface, FALSE, FALSE);
}

static BOOL wayland_surface_attach_opaque_carrier(struct wayland_surface *surface)
{
    return wayland_surface_attach_carrier(surface, TRUE, TRUE);
}

static BOOL wayland_surface_replace_direct_dmabuf_with_transparent_shm(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *direct = surface->direct_dmabuf_surface;

    if (!direct) return FALSE;

    /* Keep a pure-dmabuf toplevel mapped when it leaves direct mode before any
     * real GDI buffer exists; do not leave the stale producer frame as base. */
    if (!wayland_surface_attach_transparent_carrier(surface)) return FALSE;

    wayland_hwnd_dmabuf_surface_destroy(direct);
    return TRUE;
}

static void wayland_surface_clear_direct_dmabuf(struct wayland_surface *surface,
                                                struct wayland_win_data *data)
{
    if (surface->direct_dmabuf_surface)
    {
        struct wayland_hwnd_dmabuf_surface *direct = surface->direct_dmabuf_surface;

        if (wayland_surface_replace_direct_dmabuf_with_shm(surface, data))
            return;
        if (data && direct->current_committed &&
            wayland_surface_replace_direct_dmabuf_with_transparent_shm(surface))
            return;

        wl_surface_set_opaque_region(surface->wl_surface, NULL);
        wayland_hwnd_dmabuf_surface_destroy(direct);
    }
}

void wayland_surface_prepare_direct_dmabuf_shm_commit(struct wayland_surface *surface)
{
    if (surface->direct_dmabuf_surface)
        wl_surface_set_opaque_region(surface->wl_surface, NULL);
}

void wayland_surface_finish_direct_dmabuf_shm_commit(struct wayland_surface *surface)
{
    if (surface->direct_dmabuf_surface)
        wayland_hwnd_dmabuf_surface_destroy(surface->direct_dmabuf_surface);
}

static void wayland_surface_clear_dmabuf_children(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;

    wl_list_for_each_safe(dmabuf_surface, next, &surface->hwnd_dmabuf_surfaces, link)
        wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);
    surface->dmabuf_bottom = NULL;
}

static BOOL wayland_surface_update_direct_dmabuf(struct wayland_surface *surface,
                                                 struct wayland_win_data *data,
                                                 const hwnd_dmabuf_frame_info_t *frames,
                                                 unsigned int count,
                                                 unsigned long long now)
{
    struct wayland_hwnd_dmabuf_surface *direct;
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;
    BOOL created_buffer, attached_frame = FALSE;
    int width, height;

    if (!process_wayland.zwp_linux_dmabuf_v1)
    {
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    if (!wayland_surface_direct_dmabuf_candidate(surface, data, frames, count))
    {
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    if (!frames[0].opened)
    {
        if ((direct = surface->direct_dmabuf_surface) && direct->current)
        {
            direct->seen = TRUE;
            direct->last_seen_ms = now;
            goto commit_current;
        }
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    wayland_surface_clear_dmabuf_children(surface);

    if (!(direct = surface->direct_dmabuf_surface) &&
        !(direct = wayland_hwnd_dmabuf_surface_create_direct(surface, (HWND)(UINT_PTR)frames[0].hwnd)))
        return FALSE;

    direct->seen = TRUE;
    direct->last_seen_ms = now;

    wl_list_for_each_safe(buffer, buffer_next, &direct->buffers, link)
        if (buffer->released)
            wayland_hwnd_dmabuf_buffer_reap(buffer);

    wayland_hwnd_dmabuf_surface_import_buffer(direct, &frames[0], &created_buffer, &attached_frame);
    (void)created_buffer;
    assert(!attached_frame || !direct->current_committed);
    if (!direct->current) return TRUE;

commit_current:
    if (direct->current->alpha_mode != HWND_DMABUF_ALPHA_MODE_IGNORE)
    {
        wayland_hwnd_dmabuf_drop_current_frame(direct, HWND_DMABUF_RELEASE_DROPPED);
        wayland_surface_clear_direct_dmabuf(surface, data);
        return FALSE;
    }

    if (!wayland_surface_reconfigure(surface))
    {
        if (!direct->current_committed)
        {
            wayland_surface_clear_direct_dmabuf(surface, data);
            return FALSE;
        }
        return TRUE;
    }

    wayland_surface_coords_from_window(surface,
                                       surface->window.rect.right - surface->window.rect.left,
                                       surface->window.rect.bottom - surface->window.rect.top,
                                       &width, &height);
    width = max(1, width);
    height = max(1, height);

    if (!attached_frame && direct->current_committed &&
        direct->committed_width == width && direct->committed_height == height)
        return TRUE;

    if (!direct->current_committed)
        wayland_hwnd_dmabuf_attach_current(direct);
    wayland_hwnd_dmabuf_surface_set_opaque(direct, width, height);
    wl_surface_commit(surface->wl_surface);
    if (!direct->current_committed)
    {
        wayland_hwnd_dmabuf_buffer_add_commit_ref(direct->current);
        direct->current_committed = TRUE;
    }
    direct->committed_width = width;
    direct->committed_height = height;
    surface->content_width = surface->window.rect.right - surface->window.rect.left;
    surface->content_height = surface->window.rect.bottom - surface->window.rect.top;
    return TRUE;
}

BOOL wayland_surface_has_hwnd_dmabuf_content(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;

    if (surface->direct_dmabuf_surface && surface->direct_dmabuf_surface->current_committed)
        return TRUE;
    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        if (!dmabuf_surface->gdi_overlay && dmabuf_surface->current_committed)
            return TRUE;
    return FALSE;
}

static BOOL wayland_surface_try_direct_dmabuf(HWND hwnd)
{
    hwnd_dmabuf_frame_info_t stack_frames[16], *frames = stack_frames;
    unsigned int total = 0, count = 0;
    enum hwnd_dmabuf_status status;
    struct wayland_win_data *data;
    BOOL had_direct = FALSE, may_try = FALSE, ret = FALSE;

    if (!process_wayland.zwp_linux_dmabuf_v1) return FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        struct wayland_surface *surface = data->wayland_surface;

        if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            surface->window.minimized)
        {
            wayland_win_data_release(data);
            return FALSE;
        }

        had_direct = surface && surface->direct_dmabuf_surface;
        may_try = surface && !wayland_surface_repaint_suspended(surface) &&
                  (had_direct ||
                   (wayland_surface_is_toplevel(surface) &&
                    !data->window_contents &&
                    !data->client_surface &&
                    wayland_surface_client_fills_window(surface)));
        wayland_win_data_release(data);
    }
    if (!may_try) return FALSE;

    if (had_direct)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            if (data->wayland_surface)
            {
                wayland_surface_update_hwnd_dmabufs(data->wayland_surface);
                ret = wayland_surface_has_hwnd_dmabuf_content(data->wayland_surface);
            }
            wayland_win_data_release(data);
        }
        if (ret) wl_display_flush(process_wayland.wl_display);
        return ret;
    }

    status = wine_hwnd_dmabuf_list(hwnd, frames, ARRAY_SIZE(stack_frames), &total, &count);
    if (status != HWND_DMABUF_OK) return FALSE;
    if (total > count)
    {
        if (!(frames = calloc(total, sizeof(*frames)))) return FALSE;
        status = wine_hwnd_dmabuf_list(hwnd, frames, total, &total, &count);
        if (status != HWND_DMABUF_OK) count = 0;
    }

    if ((data = wayland_win_data_get(hwnd)))
    {
        if (data->wayland_surface)
            ret = wayland_surface_update_direct_dmabuf(data->wayland_surface, data, frames, count,
                                                       wayland_time_ms());
        wayland_win_data_release(data);
    }
    if (frames != stack_frames) free(frames);
    if (ret) wl_display_flush(process_wayland.wl_display);
    return ret;
}

void wayland_surface_reannounce_hwnd_dmabuf_consumers(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        dmabuf_surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_UNKNOWN;
    if (surface->direct_dmabuf_surface)
        surface->direct_dmabuf_surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_UNKNOWN;
}

/* Stop producer updates while new frames cannot be displayed. */
static void wayland_surface_suspend_hwnd_dmabuf_consumers(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        if (dmabuf_surface->consumer_state != WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED &&
            !wayland_hwnd_dmabuf_surface_send_consumer_state(dmabuf_surface,
                    HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED))
            dmabuf_surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED;

    if ((dmabuf_surface = surface->direct_dmabuf_surface) &&
        dmabuf_surface->consumer_state != WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED &&
        !wayland_hwnd_dmabuf_surface_send_consumer_state(dmabuf_surface,
                HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED))
        dmabuf_surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_SUSPENDED;
}

static void wayland_surface_activate_hwnd_dmabuf_consumers(struct wayland_surface *surface)
{
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface;

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        if (dmabuf_surface->consumer_state != WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE &&
            !wayland_hwnd_dmabuf_surface_send_consumer_state(dmabuf_surface,
                    HWND_DMABUF_RELEASE_CONSUMER_ACTIVE))
            dmabuf_surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE;

    if ((dmabuf_surface = surface->direct_dmabuf_surface) &&
        dmabuf_surface->consumer_state != WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE &&
        !wayland_hwnd_dmabuf_surface_send_consumer_state(dmabuf_surface,
                HWND_DMABUF_RELEASE_CONSUMER_ACTIVE))
        dmabuf_surface->consumer_state = WAYLAND_HWNDDMABUF_CONSUMER_ACTIVE;
}

void wayland_surface_update_hwnd_dmabufs(struct wayland_surface *surface)
{
    hwnd_dmabuf_frame_info_t stack_frames[16], *frames = stack_frames;
    unsigned int total = 0, count = 0, i;
    struct wayland_hwnd_dmabuf_buffer *buffer, *buffer_next;
    struct wayland_hwnd_dmabuf_surface *dmabuf_surface, *next;
    enum hwnd_dmabuf_status status;
    struct wayland_win_data *data;
    struct wl_surface *sibling, *bottom = NULL;
    unsigned long long now = wayland_time_ms();
    BOOL any_new = FALSE;

    /* Producer children are composited for primary surfaces. */
    if (!wayland_surface_is_toplevel(surface) && !wayland_surface_is_popup(surface) &&
        !wayland_surface_is_layer(surface))
    {
        wayland_surface_suspend_hwnd_dmabuf_consumers(surface);
        return;
    }
    if (surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->window.minimized)
        return;

    /* Import is driven by producer wakes (WM_WAYLAND_DMABUF_FRAME) at the producer's
     * self-paced rate, regardless of whether our toplevel is presented. We do not pace
     * to a frame callback on our surface: the compositor stops firing it when the surface
     * is not in front, stalling delivery while another window (e.g. a game) is active. */

    wl_list_for_each(dmabuf_surface, &surface->hwnd_dmabuf_surfaces, link)
        dmabuf_surface->seen = FALSE;

    status = wine_hwnd_dmabuf_list(surface->hwnd, frames, ARRAY_SIZE(stack_frames), &total, &count);
    if (status != HWND_DMABUF_OK)
    {
        TRACE("hwnd=%p wine_hwnd_dmabuf_list status=%u\n", surface->hwnd, status);
        return;
    }
    if (total > count)
    {
        if (!(frames = calloc(total, sizeof(*frames)))) return;
        status = wine_hwnd_dmabuf_list(surface->hwnd, frames, total, &total, &count);
        if (status != HWND_DMABUF_OK) count = 0;
    }
    if (surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL)
    {
        if (surface->window.minimized || wayland_surface_repaint_suspended(surface))
        {
            wayland_surface_suspend_hwnd_dmabuf_consumers(surface);
            return;
        }
        wayland_surface_activate_hwnd_dmabuf_consumers(surface);
    }

    data = wayland_win_data_get_nolock(surface->hwnd);
    if (wayland_surface_update_direct_dmabuf(surface, data, frames, count, now))
    {
        if (frames != stack_frames) free(frames);
        return;
    }

    /* Self-presenting children are stacked below the GDI surface; server order
     * is top-to-bottom, matching place_below chaining. */
    sibling = surface->wl_surface;

    for (i = 0; i < count; i++)
    {
        HWND hwnd = (HWND)(UINT_PTR)frames[i].hwnd;
        BOOL gdi_overlay = (frames[i].opened & HWND_DMABUF_FRAME_GDI_OVERLAY) != 0;
        BOOL created_buffer, attached_frame;
        enum wayland_hwnd_dmabuf_configure_result configure_result;

        if (!frames[i].opened)
        {
            if ((dmabuf_surface = wayland_hwnd_dmabuf_surface_get(surface, hwnd, FALSE)))
            {
                dmabuf_surface->seen = TRUE;
                dmabuf_surface->last_seen_ms = now;
                if (dmabuf_surface->current &&
                    (configure_result = wayland_hwnd_dmabuf_surface_configure(
                            surface, dmabuf_surface, &frames[i], sibling, FALSE)) !=
                    WAYLAND_HWNDDMABUF_CONFIGURE_FAILED)
                {
                    if (configure_result == WAYLAND_HWNDDMABUF_CONFIGURE_UPDATED)
                    {
                        wl_surface_commit(dmabuf_surface->wl_surface);
                        any_new = TRUE;
                    }
                    bottom = sibling = dmabuf_surface->stack_bottom;
                }
            }
            continue;
        }

        if (!(dmabuf_surface = wayland_hwnd_dmabuf_surface_get(surface, hwnd, gdi_overlay)) &&
            !(dmabuf_surface = wayland_hwnd_dmabuf_surface_create(surface, hwnd, gdi_overlay)))
            continue;

        dmabuf_surface->seen = TRUE;
        dmabuf_surface->last_seen_ms = now;
        /* Reap uncached buffers the compositor has released. Cached stable-slot
         * buffers set no released flag and stay cached for reuse. */
        wl_list_for_each_safe(buffer, buffer_next, &dmabuf_surface->buffers, link)
            if (buffer->released)
                wayland_hwnd_dmabuf_buffer_reap(buffer);

        wayland_hwnd_dmabuf_surface_import_buffer(dmabuf_surface, &frames[i],
                                                  &created_buffer, &attached_frame);
        assert(!attached_frame || !dmabuf_surface->current_committed);
        if (attached_frame) any_new = TRUE;
        if (!dmabuf_surface->current) continue;
        if (attached_frame) wayland_hwnd_dmabuf_attach_current(dmabuf_surface);
        if ((configure_result = wayland_hwnd_dmabuf_surface_configure(
                surface, dmabuf_surface, &frames[i], sibling, attached_frame)) ==
            WAYLAND_HWNDDMABUF_CONFIGURE_FAILED)
        {
            TRACE("hwnd=%p child=%p configure failed frame_seq=%u\n",
                  surface->hwnd, hwnd, frames[i].frame_seq);
            wayland_hwnd_dmabuf_drop_current_frame(dmabuf_surface, HWND_DMABUF_RELEASE_DROPPED);
            if (dmabuf_surface->wl_surface)
            {
                wl_surface_attach(dmabuf_surface->wl_surface, NULL, 0, 0);
                wl_surface_commit(dmabuf_surface->wl_surface);
            }
            continue;
        }

        if (configure_result == WAYLAND_HWNDDMABUF_CONFIGURE_UPDATED)
        {
            wl_surface_commit(dmabuf_surface->wl_surface);
            any_new = TRUE;
        }
        /* One compositor ref per attached frame. The wl_buffer.release handler drops
         * it. Cached slots re-attach the same wl_buffer each new frame, hence the ref
         * is per-attach. The busy gate keeps at most one attach of a slot outstanding. */
        if (attached_frame)
        {
            unsigned int j;

            if (!dmabuf_surface->sliced)
                for (j = 0; j < max(1, dmabuf_surface->current_attach_count); j++)
                    wayland_hwnd_dmabuf_buffer_add_commit_ref(dmabuf_surface->current);
            dmabuf_surface->current_committed = TRUE;
        }
        bottom = sibling = dmabuf_surface->stack_bottom;
    }

    surface->dmabuf_bottom = bottom;

    /* A child absent from the descendant list: hide its stale dmabuf subsurface
     * at once so the content behind it (e.g. the client surface) is not
     * occluded. Keep the dmabuf cache for a grace window so a brief gap does
     * not force a re-import. Reap the cache only once the grace expires. */
    wl_list_for_each_safe(dmabuf_surface, next, &surface->hwnd_dmabuf_surfaces, link)
    {
        if (dmabuf_surface->seen) continue;
        if (dmabuf_surface->gdi_overlay ||
            now - dmabuf_surface->last_seen_ms > WAYLAND_DMABUF_SURFACE_GRACE_MS)
        {
            wayland_hwnd_dmabuf_surface_destroy(dmabuf_surface);
            any_new = TRUE;
        }
        else if (dmabuf_surface->current)
        {
            wayland_hwnd_dmabuf_surface_clear_slices(dmabuf_surface);
            wl_surface_attach(dmabuf_surface->wl_surface, NULL, 0, 0);
            wl_surface_commit(dmabuf_surface->wl_surface);
            dmabuf_surface->current = NULL;
            dmabuf_surface->current_committed = FALSE;
            any_new = TRUE;
        }
    }

    if (any_new && data && data->client_surface && data->client_surface->wl_subsurface &&
        data->client_surface->toplevel == surface->hwnd)
    {
        BOOL stack_above_parent = wayland_client_surface_should_stack_above_parent(
                surface, data->client_surface, data->client_surface->client.hwnd,
                surface->hwnd, data->content_over_producer);

        wayland_client_surface_stack(surface, data->client_surface, stack_above_parent);
        any_new = TRUE;
    }

    if (data && !data->window_contents)
    {
        if (data->client_surface && data->client_surface->stack_above_parent)
            wayland_surface_attach_opaque_carrier(surface);
        else if (wayland_surface_has_hwnd_dmabuf_content(surface))
            wayland_surface_attach_transparent_carrier(surface);
    }

    /* Commit and arm the next vsync only when something changed. An idle window costs nothing. */
    /* Commit the parent to apply subsurface stacking. No frame callback: import is
     * paced by the producer, not by our toplevel being presented. */
    if (any_new)
        wl_surface_commit(surface->wl_surface);
    if (frames != stack_frames) free(frames);
}

/**********************************************************************
 *          wayland_surface_reconfigure_xdg
 *
 * Reconfigures the xdg surface as needed to match the latest requested
 * state.
 */
static BOOL wayland_surface_reconfigure_xdg(struct wayland_surface *surface, RECT rect)
{
    struct wayland_window_config *window = &surface->window;

    /* Acknowledge any compatible processed config. */
    if (surface->processing.serial && surface->processing.processed &&
        wayland_surface_config_is_compatible(&surface->processing, rect,
                                             window->state))
    {
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
        xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);
    }
    /* Initial configure, or a fullscreen/maximized transition: ack from the
     * requested state so a dmabuf-only toplevel finalizes without the win32
     * resize round-trip. Decoration changes must go through the message loop. */
    else if (surface->requested.serial &&
             (!surface->current.serial ||
              (surface->requested.state &
               (WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))) &&
             surface->current.decor == surface->requested.decor &&
             wayland_surface_config_is_compatible(&surface->requested, rect,
                                                  window->state))
    {
        surface->current = surface->requested;
        memset(&surface->processing, 0, sizeof(surface->processing));
        memset(&surface->requested, 0, sizeof(surface->requested));
        xdg_surface_ack_configure(surface->xdg_surface, surface->current.serial);

        /* Acking a fullscreen configure from the requested state bypasses
         * wayland_configure_window(), so the Win32 client
         * rect is not recomputed for the transition. Applications that
         * removed their frame styles before entering the state keep the
         * stale frame insets in the client rect, and exclusive-fullscreen
         * apps size their swapchain against a framed client (Black Desert
         * Online, GE issue 721). Ask the window thread to refresh it with
         * a geometry-neutral frame-changed update; it stays quiet when the
         * client rect already matches.
         */
        if (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
            NtUserPostMessage(surface->hwnd, WM_WAYLAND_RECALC_CLIENT_RECT, 0, 0);
    }
    else if (!surface->current.serial ||
             !wayland_surface_config_is_compatible(&surface->current, rect,
                                                   window->state))
    {
        return FALSE;
    }

    wayland_surface_reconfigure_geometry(surface, rect);

    return TRUE;
}

static BOOL wayland_surface_reconfigure_layer(struct wayland_surface *surface)
{
    if (surface->processing.serial && surface->processing.processed)
    {
        if (surface->processing.serial != surface->current.serial)
            zwlr_layer_surface_v1_ack_configure(surface->zwlr_layer_surface_v1,
                                                surface->processing.serial);
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
    }
    else if (!surface->current.serial)
    {
        return FALSE;
    }

    return TRUE;
}

/**********************************************************************
 *          wayland_surface_reconfigure
 *
 * Reconfigures the wayland surface as needed to match the latest requested
 * state.
 */
BOOL wayland_surface_reconfigure(struct wayland_surface *surface)
{
    struct wayland_window_config *window = &surface->window;
    RECT rect = surface->window.rect;

    TRACE("hwnd=%p window=%s,%#x processing=%s,%#x current=%s,%#x\n",
          surface->hwnd, wine_dbgstr_rect(&rect), window->state,
          wine_dbgstr_rect(&surface->processing.rect), surface->processing.state,
          wine_dbgstr_rect(&surface->current.rect), surface->current.state);

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        break;
    case WAYLAND_SURFACE_ROLE_POPUP:
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        if (!wayland_surface_reconfigure_xdg(surface, rect)) return FALSE;
        break;
    case WAYLAND_SURFACE_ROLE_LAYER:
        if (!surface->zwlr_layer_surface_v1) break;
        if (!wayland_surface_reconfigure_layer(surface)) return FALSE;
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        if (!surface->wl_subsurface) break;
        wayland_surface_reconfigure_subsurface(surface);
        break;
    }

    rect = map_rect_to_surface(surface, rect);
    wayland_surface_reconfigure_size(surface, rect.right - rect.left, rect.bottom - rect.top);

    return TRUE;
}

/**********************************************************************
 *          wayland_shm_buffer_ref
 *
 * Increases the reference count of a SHM buffer.
 */
void wayland_shm_buffer_ref(struct wayland_shm_buffer *shm_buffer)
{
    InterlockedIncrement(&shm_buffer->ref);
}

/**********************************************************************
 *          wayland_shm_buffer_unref
 *
 * Decreases the reference count of a SHM buffer (and may destroy it).
 */
void wayland_shm_buffer_unref(struct wayland_shm_buffer *shm_buffer)
{
    if (InterlockedDecrement(&shm_buffer->ref) > 0) return;

    TRACE("destroying %p map=%p\n", shm_buffer, shm_buffer->map_data);

    if (shm_buffer->wl_buffer)
        wl_buffer_destroy(shm_buffer->wl_buffer);
    if (shm_buffer->map_data)
        NtUnmapViewOfSection(GetCurrentProcess(), shm_buffer->map_data);
    if (shm_buffer->damage_region)
        NtGdiDeleteObjectApp(shm_buffer->damage_region);

    free(shm_buffer);
}

/**********************************************************************
 *          wayland_shm_buffer_create
 *
 * Creates a SHM buffer with the specified width, height and format.
 */
struct wayland_shm_buffer *wayland_shm_buffer_create(int width, int height,
                                                     enum wl_shm_format format)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    HANDLE handle = 0;
    int fd = -1;
    SIZE_T view_size = 0;
    LARGE_INTEGER section_size;
    NTSTATUS status;
    struct wl_shm_pool *pool;
    int stride, size;

    stride = width * WINEWAYLAND_BYTES_PER_PIXEL;
    size = stride * height;
    if (size == 0)
    {
        ERR("Invalid shm_buffer size %dx%d\n", width, height);
        goto err;
    }

    shm_buffer = calloc(1, sizeof(*shm_buffer));
    if (!shm_buffer)
    {
        ERR("Failed to allocate space for SHM buffer\n");
        goto err;
    }

    TRACE("%p %dx%d format=%d size=%d\n", shm_buffer, width, height, format, size);

    shm_buffer->ref = 1;
    shm_buffer->width = width;
    shm_buffer->height = height;
    shm_buffer->format = format;
    shm_buffer->map_size = size;

    shm_buffer->damage_region = NtGdiCreateRectRgn(0, 0, width, height);
    if (!shm_buffer->damage_region)
    {
        ERR("Failed to create buffer damage region\n");
        goto err;
    }

    section_size.QuadPart = size;
    status = NtCreateSection(&handle,
                             GENERIC_READ | SECTION_MAP_READ | SECTION_MAP_WRITE,
                             NULL, &section_size, PAGE_READWRITE, SEC_COMMIT, 0);
    if (status)
    {
        ERR("Failed to create SHM section status=0x%x\n", status);
        goto err;
    }

    status = NtMapViewOfSection(handle, GetCurrentProcess(),
                                (PVOID)&shm_buffer->map_data, 0, 0, NULL,
                                &view_size, ViewUnmap, 0, PAGE_READWRITE);
    if (status)
    {
        shm_buffer->map_data = NULL;
        ERR("Failed to create map SHM handle status=0x%x\n", status);
        goto err;
    }

    status = wine_server_handle_to_fd(handle, FILE_READ_DATA, &fd, NULL);
    if (status)
    {
        ERR("Failed to get fd from SHM handle status=0x%x\n", status);
        goto err;
    }

    pool = wl_shm_create_pool(process_wayland.wl_shm, fd, size);
    if (!pool)
    {
        ERR("Failed to create SHM pool fd=%d size=%d\n", fd, size);
        goto err;
    }
    shm_buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                                      stride, format);
    wl_shm_pool_destroy(pool);
    if (!shm_buffer->wl_buffer)
    {
        ERR("Failed to create SHM buffer %dx%d\n", width, height);
        goto err;
    }

    close(fd);
    NtClose(handle);

    TRACE("=> map=%p\n", shm_buffer->map_data);

    return shm_buffer;

err:
    if (fd >= 0) close(fd);
    if (handle) NtClose(handle);
    if (shm_buffer) wayland_shm_buffer_unref(shm_buffer);
    return NULL;
}

/***********************************************************************
 *           copy_rectangle_into_center_of_square
 *
 * Copies non-square rectangle src to the center of square dest.
 */
static void copy_rectangle_into_center_of_square(const unsigned int *src,
                                                 int src_w, int src_h,
                                                 unsigned int *dest)
{
    int dest_length;

    if (src_w > src_h)
    {
        dest += src_w * (src_w - src_h) / 2;
        dest_length = src_w;
    }
    else
    {
        dest += (src_h - src_w) / 2;
        dest_length = src_h;
    }

    for (int h = 0; h < src_h; h++, dest += dest_length, src += src_w)
        memcpy(dest, src, src_w * 4);
}

static BOOL get_color_bitmap_bits(HDC hdc, HBITMAP bitmap, const BITMAP *bm,
                                  BITMAPINFO *info, unsigned int *bits)
{
    HBITMAP dib = 0, old_dst = 0, old_src = 0;
    HDC src_hdc = 0;
    void *dib_bits = NULL;
    BOOL ret = FALSE;

    if (NtGdiGetDIBitsInternal(hdc, bitmap, 0, bm->bmHeight, bits, info,
                               DIB_RGB_COLORS, 0, 0))
        return TRUE;

    /* Windows rejects GetDIBits for DDBs whose native depth is neither 1 nor
     * 32 bits. Render other DDB formats into a 32-bit DIB section instead of
     * weakening that application-visible behavior. */
    if (!(src_hdc = NtGdiCreateCompatibleDC(0))) goto done;
    if (!(dib = NtGdiCreateDIBSection(hdc, NULL, 0, info, DIB_RGB_COLORS,
                                      0, 0, 0, &dib_bits)))
        goto done;
    if (!(old_src = NtGdiSelectBitmap(src_hdc, bitmap))) goto done;
    if (!(old_dst = NtGdiSelectBitmap(hdc, dib))) goto done;
    if (!NtGdiBitBlt(hdc, 0, 0, bm->bmWidth, bm->bmHeight,
                     src_hdc, 0, 0, SRCCOPY, 0, 0))
        goto done;

    memcpy(bits, dib_bits, bm->bmWidth * bm->bmHeight * sizeof(*bits));
    ret = TRUE;

done:
    if (old_dst) NtGdiSelectBitmap(hdc, old_dst);
    if (old_src) NtGdiSelectBitmap(src_hdc, old_src);
    if (dib) NtGdiDeleteObjectApp(dib);
    if (src_hdc) NtGdiDeleteObjectApp(src_hdc);
    if (!ret)
        ERR("Failed to convert %dx%d color bitmap, planes %u, bpp %u\n",
            bm->bmWidth, bm->bmHeight, (unsigned int)bm->bmPlanes,
            (unsigned int)bm->bmBitsPixel);
    return ret;
}

/***********************************************************************
 *           wayland_shm_buffer_from_color_bitmaps
 *
 * Create a wayland_shm_buffer for a color bitmap.
 *
 * Adapted from wineandroid.drv code.
 */
struct wayland_shm_buffer *wayland_shm_buffer_from_color_bitmaps(HDC hdc, HBITMAP color,
                                                                 HBITMAP mask,
                                                                 BOOL allow_padding)
{
    struct wayland_shm_buffer *shm_buffer = NULL;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    BITMAP bm;
    unsigned int *ptr, *bits = NULL;
    unsigned char *mask_bits = NULL;
    int i, j, square_length;
    BOOL has_alpha = FALSE, use_padding = FALSE;

    if (!NtGdiExtGetObjectW(color, sizeof(bm), &bm)) goto failed;

    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = bm.bmWidth;
    info->bmiHeader.biHeight = -bm.bmHeight;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = bm.bmWidth * bm.bmHeight * 4;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;

    use_padding = allow_padding && bm.bmWidth != bm.bmHeight;

    if (use_padding)
    {
        square_length = max(bm.bmWidth, bm.bmHeight);
        shm_buffer = wayland_shm_buffer_create(square_length, square_length,
                                               WL_SHM_FORMAT_ARGB8888);
        if (!shm_buffer) goto failed;
        if (!(bits = malloc(info->bmiHeader.biSizeImage))) goto failed;
    }
    else
    {
        shm_buffer = wayland_shm_buffer_create(bm.bmWidth, bm.bmHeight,
                                               WL_SHM_FORMAT_ARGB8888);
        if (!shm_buffer) goto failed;
        bits = shm_buffer->map_data;
    }

    if (!get_color_bitmap_bits(hdc, color, &bm, info, bits))
        goto failed;

    for (i = 0; i < bm.bmWidth * bm.bmHeight; i++)
        if ((has_alpha = (bits[i] & 0xff000000) != 0)) break;

    if (!has_alpha)
    {
        unsigned int width_bytes = (bm.bmWidth + 31) / 32 * 4;
        /* generate alpha channel from the mask */
        info->bmiHeader.biBitCount = 1;
        info->bmiHeader.biSizeImage = width_bytes * bm.bmHeight;
        if (!(mask_bits = malloc(info->bmiHeader.biSizeImage))) goto failed;
        if (!NtGdiGetDIBitsInternal(hdc, mask, 0, bm.bmHeight, mask_bits,
                                    info, DIB_RGB_COLORS, 0, 0))
            goto failed;
        ptr = bits;
        for (i = 0; i < bm.bmHeight; i++)
        {
            for (j = 0; j < bm.bmWidth; j++, ptr++)
            {
                if (!((mask_bits[i * width_bytes + j / 8] << (j % 8)) & 0x80))
                    *ptr |= 0xff000000;
            }
        }
        free(mask_bits);
    }

    if (use_padding)
    {
        copy_rectangle_into_center_of_square(bits, bm.bmWidth,
                                             bm.bmHeight, shm_buffer->map_data);
        free(bits);
        bits = shm_buffer->map_data;
    }

    /* Wayland requires pre-multiplied alpha values */
    for (ptr = bits, i = 0; i < shm_buffer->width * shm_buffer->height; ptr++, i++)
    {
        unsigned char alpha = *ptr >> 24;
        if (alpha == 0)
        {
            *ptr = 0;
        }
        else if (alpha != 255)
        {
            *ptr = (alpha << 24) |
                   (((BYTE)(*ptr >> 16) * alpha / 255) << 16) |
                   (((BYTE)(*ptr >> 8) * alpha / 255) << 8) |
                   (((BYTE)*ptr * alpha / 255));
        }
    }

    return shm_buffer;

failed:
    if (shm_buffer) wayland_shm_buffer_unref(shm_buffer);
    if (use_padding) free(bits);
    free(mask_bits);
    return NULL;
}

/**********************************************************************
 *          map_rect_to_surface
 *
 * Converts the window (logical) coordinates to wayland surface-local coordinates.
 */
RECT map_rect_to_surface(struct wayland_surface *surface, RECT rect)
{
    rect.left = round(rect.left / surface->window.scale);
    rect.top  = round(rect.top / surface->window.scale);
    rect.right = round(rect.right / surface->window.scale);
    rect.bottom  = round(rect.bottom / surface->window.scale);
    return rect;
}

/**********************************************************************
 *          map_point_to_surface
 *
 * Converts the window (logical) coordinates to wayland surface-local coordinates.
 */
POINT map_point_to_surface(struct wayland_surface *surface, POINT point)
{
    point.x = round(point.x / surface->window.scale);
    point.y  = round(point.y / surface->window.scale);
    return point;
}

/**********************************************************************
 *          map_rect_from_surface
 *
 * Converts the surface-local coordinates to window (logical) coordinates.
 */
RECT map_rect_from_surface(struct wayland_surface *surface, RECT rect)
{
    rect.left = round(rect.left * surface->window.scale);
    rect.top  = round(rect.top * surface->window.scale);
    rect.right = round(rect.right * surface->window.scale);
    rect.bottom  = round(rect.bottom * surface->window.scale);
    return rect;
}

/**********************************************************************
 *          map_point_from_surface
 *
 * Converts the surface-local coordinates to window (logical) coordinates.
 */
POINT map_point_from_surface(struct wayland_surface *surface, POINT point)
{
    point.x = round(point.x * surface->window.scale);
    point.y = round(point.y * surface->window.scale);
    return point;
}

void wayland_surface_coords_from_window(struct wayland_surface *surface,
                                        int window_x, int window_y,
                                        int *surface_x, int *surface_y)
{
    POINT point = {window_x, window_y};

    point = map_point_to_surface(surface, point);
    *surface_x = point.x;
    *surface_y = point.y;
}

void wayland_surface_coords_to_window(struct wayland_surface *surface,
                                      double surface_x, double surface_y,
                                      int *window_x, int *window_y)
{
    POINT point = {round(surface_x), round(surface_y)};

    point = map_point_from_surface(surface, point);
    *window_x = point.x;
    *window_y = point.y;
}

struct wayland_client_surface *impl_from_client_surface(struct client_surface *client)
{
    return CONTAINING_RECORD(client, struct wayland_client_surface, client);
}

static BOOL is_client_visible(HWND hwnd);

static void wayland_client_surface_register(struct wayland_win_data *data,
                                            struct wayland_client_surface *surface)
{
    if (list_empty(&surface->hwnd_entry))
        list_add_tail(&data->client_surfaces, &surface->hwnd_entry);
}

static void wayland_client_surface_unregister(struct wayland_client_surface *surface)
{
    if (list_empty(&surface->hwnd_entry)) return;
    list_remove(&surface->hwnd_entry);
    list_init(&surface->hwnd_entry);
}

static void wayland_client_surface_register_latest(
        struct wayland_win_data *data, struct wayland_client_surface *surface)
{
    wayland_client_surface_unregister(surface);
    list_add_tail(&data->client_surfaces, &surface->hwnd_entry);
}

static struct wayland_client_surface *wayland_client_surface_find_owner(
        struct wayland_win_data *data, struct wayland_client_surface *exclude,
        BOOL active_only)
{
    struct wayland_client_surface *surface;

    LIST_FOR_EACH_ENTRY_REV(surface, &data->client_surfaces,
                            struct wayland_client_surface, hwnd_entry)
    {
        if (surface == exclude) continue;
        if (active_only && !ReadAcquire(&surface->client.active_ref)) continue;
        return surface;
    }
    return NULL;
}

static BOOL wayland_client_surface_is_presentation_candidate(
        struct wayland_win_data *data, struct wayland_client_surface *surface)
{
    struct wayland_client_surface *candidate;

    /* Predict find_owner(active_only) after activating this surface. Newer
     * inactive surfaces, including graphics probes, must not block it. */
    LIST_FOR_EACH_ENTRY_REV(candidate, &data->client_surfaces,
                            struct wayland_client_surface, hwnd_entry)
    {
        if (candidate == surface) return TRUE;
        if (ReadAcquire(&candidate->client.active_ref)) return FALSE;
    }
    return FALSE;
}

/* Presentation ownership follows graphics-surface creation order. Replacement
 * flip-model presenters are newer than the legacy presenter they supersede. */
static BOOL wayland_client_surface_set_owner(struct wayland_win_data *data,
                                             struct wayland_client_surface *surface)
{
    struct wayland_client_surface *client;
    struct wayland_client_surface *previous = data->client_surface;

    if (previous == surface) return FALSE;

    if (previous)
    {
        InterlockedExchange(&previous->client.presentation_owner, FALSE);
        if (ReadAcquire(&previous->client.active_ref))
            client_surface_invalidate_presentation(&previous->client);
    }
    data->client_surface = surface;
    if (surface)
    {
        InterlockedExchange(&surface->client.presentation_owner, TRUE);
        if (ReadAcquire(&surface->client.active_ref))
            client_surface_invalidate_presentation(&surface->client);
    }

    LIST_FOR_EACH_ENTRY(client, &data->client_surfaces,
                        struct wayland_client_surface, hwnd_entry)
    {
        if (client == previous || client == surface ||
            !ReadAcquire(&client->client.active_ref))
            continue;
        client_surface_invalidate_presentation(&client->client);
    }

    TRACE("hwnd=%p presentation owner %s -> %s\n", data->hwnd,
          previous ? debugstr_client_surface(&previous->client) : "(none)",
          surface ? debugstr_client_surface(&surface->client) : "(none)");
    return TRUE;
}

static void wayland_client_surface_restore_owner(
        struct wayland_win_data *data, struct wayland_client_surface *surface)
{
    struct wayland_client_surface *replacement;

    replacement = wayland_client_surface_find_owner(data, surface, TRUE);
    if (!replacement)
        replacement = wayland_client_surface_find_owner(data, surface, FALSE);
    wayland_client_surface_set_owner(data, replacement);
}

void wayland_client_surfaces_attach(struct wayland_win_data *data, HWND toplevel)
{
    struct wayland_client_surface *surface;
    struct wayland_client_surface *owner = data->client_surface;
    BOOL visible = toplevel && is_client_visible(data->hwnd);

    /* Only native background presenters need compositor service. A pending
     * native presenter must also be attached so host WSI can finish creating
     * its swapchain; locally retired background swapchains remain detached. */
    LIST_FOR_EACH_ENTRY(surface, &data->client_surfaces,
                        struct wayland_client_surface, hwnd_entry)
    {
        if (surface == owner) continue;

        if ((ReadAcquire(&surface->client.native_ref) ||
             ReadAcquire(&surface->client.native_pending_ref)) && visible)
            wayland_client_surface_attach(surface, toplevel);
        else
            wayland_client_surface_attach(surface, NULL);
    }

    if (owner)
    {
        if (visible) wayland_client_surface_attach(owner, toplevel);
        else wayland_client_surface_attach(owner, NULL);
    }
}

static void wayland_client_surface_update_owner(struct wayland_win_data *data)
{
    struct wayland_client_surface *owner;

    owner = wayland_client_surface_find_owner(data, NULL, TRUE);
    if (!owner && data->client_surface &&
        !list_empty(&data->client_surface->hwnd_entry))
        owner = data->client_surface;
    if (!owner) owner = wayland_client_surface_find_owner(data, NULL, FALSE);
    wayland_client_surface_set_owner(data, owner);
    wayland_client_surfaces_attach(data, NtUserGetAncestor(data->hwnd, GA_ROOT));
}

static void wayland_client_surface_destroy(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_win_data *data;
    struct wl_callback *callback;

    TRACE("%s\n", debugstr_client_surface(client));

    if ((data = wayland_win_data_get(client->hwnd)))
    {
        wayland_client_surface_attach(surface, NULL);
        if (data->client_surface == surface)
            wayland_client_surface_restore_owner(data, surface);
        wayland_client_surface_unregister(surface);
        wayland_client_surface_update_owner(data);
        wayland_win_data_release(data);
    }
    else
        wayland_client_surface_unregister(surface);
    InterlockedExchange(&client->presentation_owner, FALSE);

    if ((callback = InterlockedExchangePointer((void **)&surface->wl_callback, NULL)))
        wl_callback_destroy(callback);
    if (surface->throttle)
        NtClose(surface->throttle);
    if (surface->wp_color_management_surface_v1)
        wp_color_management_surface_v1_destroy(surface->wp_color_management_surface_v1);
    if (surface->wp_content_type_v1)
        wp_content_type_v1_destroy(surface->wp_content_type_v1);
    if (surface->wp_viewport)
        wp_viewport_destroy(surface->wp_viewport);
    if (surface->wl_subsurface)
        wl_subsurface_destroy(surface->wl_subsurface);
    if (surface->dmabuf_feedback)
        zwp_linux_dmabuf_feedback_v1_destroy(surface->dmabuf_feedback);
    if (surface->wl_surface)
        wl_surface_destroy(surface->wl_surface);
}

static void wayland_client_surface_detach(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_win_data *data;

    TRACE("%s\n", debugstr_client_surface(client));

    if ((data = wayland_win_data_get(client->hwnd)))
    {
        wayland_client_surface_attach(surface, NULL);
        if (data->client_surface == surface)
            wayland_client_surface_restore_owner(data, surface);
        wayland_client_surface_unregister(surface);
        wayland_client_surface_update_owner(data);
        wayland_win_data_release(data);
    }
    InterlockedExchange(&client->presentation_owner, FALSE);
}

static BOOL is_client_visible(HWND hwnd)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    RECT dummy;

    if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE) return FALSE;
    if (root && root != hwnd && (NtUserGetWindowLongW(root, GWL_STYLE) & WS_MINIMIZE))
        return FALSE;
    return NtUserIsWindowVisible(hwnd) || NtUserGetPresentRect(hwnd, &dummy, -1);
}

static void wayland_client_surface_update(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_win_data *data;

    TRACE("%s\n", debugstr_client_surface(client));

    if (!(data = wayland_win_data_get(client->hwnd))) return;

    /* Preserve graphics-surface creation order while updating activity. */
    wayland_client_surface_register(data, surface);
    wayland_client_surface_update_owner(data);
    wayland_win_data_release(data);
}

static BOOL wayland_surface_has_live_role(struct wayland_surface *surface);

static BOOL wayland_client_surface_update_visual_constraint(struct wayland_client_surface *client,
                                                            HWND toplevel)
{
    struct wayland_child_visibility_info visibility;
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *surface;
    HWND hwnd = client->client.hwnd;
    RECT client_rect, dst;

    if (!(toplevel_data = wayland_win_data_get(toplevel)) ||
        !(surface = toplevel_data->wayland_surface) ||
        !wayland_surface_has_live_role(surface))
    {
        if (toplevel_data) wayland_win_data_release(toplevel_data);
        client->visual_constraint.valid = FALSE;
        return FALSE;
    }

    if (hwnd == toplevel)
    {
        SetRect(&client_rect, 0, 0,
                surface->window.client_rect.right - surface->window.client_rect.left,
                surface->window.client_rect.bottom - surface->window.client_rect.top);
    }
    else
    {
        NtUserGetClientRect(hwnd, &client_rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
        NtUserMapWindowPoints(hwnd, toplevel, (POINT *)&client_rect, 2, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
    }

    dst = client_rect;
    OffsetRect(&dst, surface->window.client_rect.left - surface->window.rect.left,
               surface->window.client_rect.top - surface->window.rect.top);
    wayland_surface_classify_child_visibility(surface, &dst, &visibility);
    wayland_surface_update_child_visibility(&dst, &visibility, &client->visual_constraint);
    wayland_win_data_release(toplevel_data);
    return TRUE;
}

static BOOL wayland_client_surface_is_hwnd_dmabuf_producer(struct wayland_client_surface *surface)
{
    struct wayland_win_data *data;
    HANDLE handle = 0;
    HWND hwnd = surface->client.hwnd;
    BOOL local_data = FALSE, local_unmaskable = FALSE;

    if ((data = wayland_win_data_get(hwnd)))
    {
        local_data = TRUE;
        local_unmaskable = surface->visual_constraint.valid &&
                           surface->visual_constraint.visibility == WAYLAND_CHILD_VISIBILITY_UNMASKABLE;
        wayland_win_data_release(data);
        if (surface->hwnd_dmabuf_producer && !local_unmaskable)
        {
            surface->hwnd_dmabuf_producer = FALSE;
        }
        if (!local_unmaskable) return FALSE;
        if (surface->hwnd_dmabuf_producer) return TRUE;
    }
    else if (surface->hwnd_dmabuf_producer) return TRUE;

    if (wine_hwnd_dmabuf_claim_channel(hwnd, &handle) != HWND_DMABUF_OK || !handle)
        return FALSE;

    NtClose(handle);
    surface->hwnd_dmabuf_producer = TRUE;
    if (local_data) wayland_client_surface_attach(surface, NULL);
    TRACE("hwnd=%p is an HWND dmabuf producer %s; skipping client surface presents\n",
          hwnd, local_data ? "for an unmaskable local client surface" : "without local wayland data");
    return TRUE;
}

static void wayland_client_surface_present(struct client_surface *client, HDC hdc)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    HWND hwnd = client->hwnd, toplevel = NtUserGetAncestor(hwnd, GA_ROOT);

    TRACE("%s hdc=%p toplevel=%p\n", debugstr_client_surface(client), hdc, toplevel);
    wayland_client_surface_update_visual_constraint(surface, toplevel);

    /* A dmabuf producer presents through the hwnd-dmabuf path, not this
     * client subsurface. Still mark it presented for lifecycle state. */
    if (wayland_client_surface_is_hwnd_dmabuf_producer(surface))
    {
        surface->has_presented = TRUE;
        return;
    }

    surface->has_presented = TRUE;
    wayland_client_surface_update(client);
    ensure_window_surface_contents(toplevel);
}

void set_client_surface(HWND hwnd, struct wayland_client_surface *new_client)
{
    struct wayland_client_surface *owner;
    struct wayland_win_data *data;

    /* ownership is shared with the callers, the last caller to release
     * its reference will also destroy it and clear our pointer. */

    if (!(data = wayland_win_data_get(hwnd))) return;

    if (new_client)
        wayland_client_surface_register_latest(data, new_client);
    owner = wayland_client_surface_find_owner(data, NULL, TRUE);
    if (!owner) owner = data->client_surface ? data->client_surface : new_client;
    wayland_client_surface_set_owner(data, owner);
    wayland_client_surfaces_attach(data, NtUserGetAncestor(hwnd, GA_ROOT));
    wayland_win_data_release(data);
}

static struct client_surface *wayland_client_surface_activate(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_client_surface *owner;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(client->hwnd))) return client;

    wayland_client_surface_register(data, surface);
    owner = wayland_client_surface_find_owner(data, NULL, TRUE);
    wayland_client_surface_set_owner(data, owner);
    wayland_client_surfaces_attach(data, NtUserGetAncestor(client->hwnd, GA_ROOT));
    TRACE("activated %s as %s\n", debugstr_client_surface(client),
          owner == surface ? "presentation owner" : "background presenter");
    wayland_win_data_release(data);
    return owner ? &owner->client : NULL;
}

static BOOL wayland_client_surface_is_candidate(struct client_surface *client)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wayland_win_data *data;
    BOOL ret = TRUE;

    if ((data = wayland_win_data_get(client->hwnd)))
    {
        wayland_client_surface_register(data, surface);
        ret = wayland_client_surface_is_presentation_candidate(data, surface);
        wayland_win_data_release(data);
    }
    return ret;
}

static void wayland_client_surface_deactivate(struct client_surface *client)
{
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(client->hwnd))) return;

    /* Keep the last frame attached across swapchain recreation. Only another
     * active presenter, or actual surface detachment/destruction, replaces it. */
    wayland_client_surface_update_owner(data);
    TRACE("deactivated %s\n", debugstr_client_surface(client));
    wayland_win_data_release(data);
}

static const struct client_surface_funcs wayland_client_surface_funcs =
{
    .destroy = wayland_client_surface_destroy,
    .detach = wayland_client_surface_detach,
    .update = wayland_client_surface_update,
    .is_presentation_candidate = wayland_client_surface_is_candidate,
    .activate = wayland_client_surface_activate,
    .deactivate = wayland_client_surface_deactivate,
    .present = wayland_client_surface_present,
};

static void client_dmabuf_feedback_handle_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
}

static void client_dmabuf_feedback_handle_format_table(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, int fd, uint32_t size)
{
    /* WSI consumes its own feedback; this subscription only preserves its lifetime. */
    close(fd);
}

static void client_dmabuf_feedback_handle_device(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, struct wl_array *device)
{
}

static void client_dmabuf_feedback_handle_tranche_formats(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, struct wl_array *indices)
{
}

static void client_dmabuf_feedback_handle_tranche_flags(void *data,
        struct zwp_linux_dmabuf_feedback_v1 *feedback, uint32_t flags)
{
    TRACE("client dmabuf feedback %p tranche flags %#x\n", feedback, flags);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener client_dmabuf_feedback_listener =
{
    client_dmabuf_feedback_handle_done,
    client_dmabuf_feedback_handle_format_table,
    client_dmabuf_feedback_handle_device,
    client_dmabuf_feedback_handle_done,
    client_dmabuf_feedback_handle_device,
    client_dmabuf_feedback_handle_tranche_formats,
    client_dmabuf_feedback_handle_tranche_flags,
};

struct wayland_client_surface *wayland_client_surface_create(HWND hwnd)
{
    struct wayland_client_surface *client;
    struct wl_region *empty_region;

    if (!(client = client_surface_create(sizeof(*client), &wayland_client_surface_funcs, hwnd))) return NULL;
    InterlockedExchange(&client->client.presentation_owner, FALSE);
    list_init(&client->hwnd_entry);

    client->wl_surface =
        wl_compositor_create_surface(process_wayland.wl_compositor);
    if (!client->wl_surface)
    {
        ERR("Failed to create client wl_surface\n");
        goto err;
    }
    wl_surface_set_user_data(client->wl_surface, hwnd);

    /* Keep surface feedback alive across WSI swapchain recreation. Mutter drops
     * its cached scanout tranche when the last feedback subscription is destroyed. */
    if (process_wayland.zwp_linux_dmabuf_v1 &&
        zwp_linux_dmabuf_v1_get_version(process_wayland.zwp_linux_dmabuf_v1) >=
            ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION)
    {
        client->dmabuf_feedback = zwp_linux_dmabuf_v1_get_surface_feedback(
            process_wayland.zwp_linux_dmabuf_v1, client->wl_surface);
        if (!client->dmabuf_feedback)
        {
            ERR("Failed to create client dmabuf feedback\n");
            goto err;
        }
        zwp_linux_dmabuf_feedback_v1_add_listener(client->dmabuf_feedback,
                                                 &client_dmabuf_feedback_listener, NULL);
    }

    /* Let parent handle all pointer events. */
    empty_region = wl_compositor_create_region(process_wayland.wl_compositor);
    if (!empty_region)
    {
        ERR("Failed to create wl_region\n");
        goto err;
    }
    wl_surface_set_input_region(client->wl_surface, empty_region);
    wl_region_destroy(empty_region);

    client->wp_viewport =
        wp_viewporter_get_viewport(process_wayland.wp_viewporter,
                                    client->wl_surface);
    if (!client->wp_viewport)
    {
        ERR("Failed to create client wp_viewport\n");
        goto err;
    }

    if (process_wayland.wp_content_type_manager_v1)
    {
        client->wp_content_type_v1 =
            wp_content_type_manager_v1_get_surface_content_type(
                process_wayland.wp_content_type_manager_v1,
                client->wl_surface
            );
        if (client->wp_content_type_v1)
        {
            wp_content_type_v1_set_content_type(client->wp_content_type_v1,
                                                WP_CONTENT_TYPE_V1_TYPE_GAME);
            TRACE("set game content on client surface!\n");
        }
    }

    SetRect(&client->viewport_rect, -INT32_MAX, 0, -INT32_MAX, 0);

    return client;

err:
    client_surface_release(&client->client);
    return NULL;
}

static BOOL wayland_surface_has_live_role(struct wayland_surface *surface)
{
    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        return surface->xdg_surface != NULL && surface->xdg_toplevel != NULL;
    case WAYLAND_SURFACE_ROLE_POPUP:
        return surface->xdg_surface != NULL && surface->xdg_popup != NULL;
    case WAYLAND_SURFACE_ROLE_LAYER:
        return surface->zwlr_layer_surface_v1 != NULL;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        return surface->wl_subsurface != NULL;
    case WAYLAND_SURFACE_ROLE_NONE:
        return FALSE;
    }

    return FALSE;
}

static void wayland_client_surface_cancel_frame_callback(struct wayland_client_surface *client)
{
    struct wl_callback *callback;

    if ((callback = InterlockedExchangePointer((void **)&client->wl_callback, NULL)))
        wl_callback_destroy(callback);
    if (client->throttle) NtSetEvent(client->throttle, NULL);
}

void wayland_client_surface_attach(struct wayland_client_surface *client, HWND toplevel)
{
    struct wayland_win_data *toplevel_data;
    struct wayland_surface *surface = NULL;
    HWND hwnd = client->client.hwnd;
    RECT client_rect, dst;
    struct wayland_child_visibility_info visibility;
    BOOL stack_above_parent;

    if (!toplevel)
    {
        HWND previous_toplevel = client->toplevel;

        wayland_client_surface_cancel_frame_callback(client);

        if (client->wl_subsurface)
        {
            client_surface_invalidate_presentation(&client->client);
            wl_subsurface_destroy(client->wl_subsurface);
            client->wl_subsurface = NULL;
            client->toplevel_wl_surface = NULL;
            InterlockedIncrement(&client->attachment_generation);
        }

        SetRect(&client->viewport_rect, -INT32_MAX, 0, -INT32_MAX, 0);
        client->toplevel = 0;
        client->stack_above_parent = FALSE;

        /* An opaque carrier is only valid while its full-size client covers it. */
        if (previous_toplevel && (toplevel_data = wayland_win_data_get(previous_toplevel)))
        {
            surface = toplevel_data->wayland_surface;
            if (surface && surface->carrier_attached && surface->carrier_opaque)
                wayland_surface_attach_transparent_carrier(surface);
            wayland_win_data_release(toplevel_data);
        }
        return;
    }

    if (client->hwnd_dmabuf_producer)
    {
        wayland_client_surface_attach(client, NULL);
        return;
    }

    if (!(toplevel_data = wayland_win_data_get(toplevel)) ||
        !(surface = toplevel_data->wayland_surface) ||
        !wayland_surface_has_live_role(surface))
    {
        if (toplevel_data) wayland_win_data_release(toplevel_data);
        return wayland_client_surface_attach(client, NULL);
    }
    if (surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->window.minimized)
    {
        wayland_client_surface_attach(client, NULL);
        return;
    }

    stack_above_parent = wayland_client_surface_should_stack_above_parent(
            surface, client, hwnd, toplevel, toplevel_data->content_over_producer);

    if (client->toplevel != toplevel ||
        client->toplevel_wl_surface != surface->wl_surface)
    {
        wayland_client_surface_attach(client, NULL);

        client->wl_subsurface =
            wl_subcompositor_get_subsurface(process_wayland.wl_subcompositor,
                                            client->wl_surface,
                                            surface->wl_surface);
        if (!client->wl_subsurface) goto done;

        InterlockedIncrement(&client->attachment_generation);

        /* Present contents independently of the parent surface. */
        wl_subsurface_set_desync(client->wl_subsurface);
        wayland_client_surface_stack(surface, client, stack_above_parent);

        client->toplevel = toplevel;
        client->toplevel_wl_surface = surface->wl_surface;
        SetRect(&client->rect, 0, 0, -1, -1);

        TRACE("Created subsurface for toplevel=%p\n", toplevel);
    }

    if (hwnd == toplevel)
    {
        /* Keep own-toplevel client surfaces in the same geometry generation as
         * the parent surface during xdg configure / rawpos transitions. */
        SetRect(&client_rect, 0, 0,
                surface->window.client_rect.right - surface->window.client_rect.left,
                surface->window.client_rect.bottom - surface->window.client_rect.top);
    }
    else
    {
        NtUserGetClientRect(hwnd, &client_rect, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
        NtUserMapWindowPoints(hwnd, toplevel, (POINT *)&client_rect, 2, NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI));
    }

    dst = client_rect;
    OffsetRect(&dst, surface->window.client_rect.left - surface->window.rect.left,
               surface->window.client_rect.top - surface->window.rect.top);
    wayland_surface_classify_child_visibility(surface, &dst, &visibility);
    wayland_surface_update_child_visibility(&dst, &visibility, &client->visual_constraint);

    /* Subsurface position and stacking are double-buffered on the parent.
     * Avoid a no-op parent commit after every client present. */
    if (wayland_surface_reconfigure_client(surface, client, &client_rect,
                                           stack_above_parent))
        wl_surface_commit(surface->wl_surface);

    if (!toplevel_data->window_contents)
    {
        if (stack_above_parent)
            wayland_surface_attach_opaque_carrier(surface);
        else
            wayland_surface_attach_transparent_carrier(surface);
    }

done:
    wayland_win_data_release(toplevel_data);
}

static void wayland_image_description_v1_failed(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t cause, const char *msg)
{
    struct wayland_client_surface *surface = user_data;
    ERR("cause=%u msg=%s\n", cause, debugstr_a(msg));

    wp_image_description_v1_destroy(wp_image_description_v1);
    client_surface_release(&surface->client);
}

static void wayland_image_description_v1_ready2(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t identity_hi, uint32_t identity_lo)
{
    struct wayland_client_surface *surface = user_data;
    TRACE("id=%#x%x\n", identity_hi, identity_lo);

    if (!surface->wp_color_management_surface_v1)
    {
        surface->wp_color_management_surface_v1 =
            wp_color_manager_v1_get_surface(process_wayland.wp_color_manager_v1,
                                            surface->wl_surface);
        if (!surface->wp_color_management_surface_v1)
        {
            ERR("Failed to create color management surface for client surface!\n");
            return;
        }
    }
    wp_color_management_surface_v1_set_image_description(
        surface->wp_color_management_surface_v1,
        wp_image_description_v1,
        WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wp_image_description_v1_destroy(wp_image_description_v1);
    client_surface_release(&surface->client);
}

static void wayland_image_description_v1_ready(void *user_data,
                    struct wp_image_description_v1 *wp_image_description_v1,
                    uint32_t identity)
{
    wayland_image_description_v1_ready2(user_data, wp_image_description_v1, 0, identity);
}

static const struct wp_image_description_v1_listener image_description_listener = {
    wayland_image_description_v1_failed,
    wayland_image_description_v1_ready,
    wayland_image_description_v1_ready2
};

void wayland_client_surface_attach_image_description(struct wayland_client_surface *surface,
                                                     struct wp_image_description_v1 *image_desc)
{
    if (!image_desc)
    {
        if (surface->wp_color_management_surface_v1)
        {
            wp_color_management_surface_v1_destroy(surface->wp_color_management_surface_v1);
            surface->wp_color_management_surface_v1 = NULL;
        }
        return;
    }

    client_surface_add_ref(&surface->client);
    wp_image_description_v1_add_listener(image_desc, &image_description_listener, surface);
}

void wayland_client_surface_set_alpha(struct client_surface *client, BOOL alpha)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    struct wl_region *region = NULL;

    surface->has_alpha = alpha;

    if (!alpha)
    {
        if (!(region = wl_compositor_create_region(process_wayland.wl_compositor)))
        {
            ERR("Failed to create client opaque region\n");
            return;
        }
        /* Surface-local regions are clipped by the compositor. Cover future
         * resizes as well, independently of buffer scale and viewport size. */
        wl_region_add(region, 0, 0, INT32_MAX, INT32_MAX);
    }

    /* Let the next WSI buffer commit apply the pending opacity state. */
    wl_surface_set_opaque_region(surface->wl_surface, region);
    if (region) wl_region_destroy(region);
}

/**********************************************************************
 *          wayland_surface_ensure_contents
 *
 * Import any direct or child dmabuf content. Parent SHM contents normally come
 * from the real window surface expose path; punch-through mode uses a
 * transparent carrier when below-main content has no real parent buffer yet.
 */
void wayland_surface_ensure_contents(struct wayland_surface *surface,
                                     struct wayland_client_surface *client)
{
    (void)client;
    wayland_surface_update_hwnd_dmabufs(surface);
}

/**********************************************************************
 *          wayland_surface_set_title
 */
void wayland_surface_set_title(struct wayland_surface *surface, LPCWSTR text)
{
    DWORD text_len;
    DWORD utf8_count;
    char *utf8 = NULL;

    assert(wayland_surface_is_toplevel(surface));

    TRACE("surface=%p hwnd=%p text='%s'\n",
          surface, surface->hwnd, wine_dbgstr_w(text));

    text_len = (lstrlenW(text) + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_count, text, text_len) &&
        (utf8 = malloc(utf8_count)))
    {
        RtlUnicodeToUTF8N(utf8, utf8_count, &utf8_count, text, text_len);
        xdg_toplevel_set_title(surface->xdg_toplevel, utf8);
    }

    free(utf8);
}

/**********************************************************************
 *          wayland_surface_set_icon_buffer
 */
void wayland_surface_set_icon_buffer(struct wayland_surface *surface, UINT type, const ICONINFO *ii)
{
    struct wayland_shm_buffer *icon_buf;
    HDC hDC;

    if (!process_wayland.xdg_toplevel_icon_manager_v1) return;

    assert(ii);

    TRACE("surface=%p type=%x ii=%p\n", surface, type, ii);

    hDC = NtGdiCreateCompatibleDC(0);
    icon_buf = wayland_shm_buffer_from_color_bitmaps(hDC, ii->hbmColor, ii->hbmMask, TRUE);
    NtGdiDeleteObjectApp(hDC);

    if (surface->big_icon_buffer && type == ICON_BIG)
    {
        wayland_shm_buffer_unref(surface->big_icon_buffer);
        surface->big_icon_buffer = NULL;
    }
    else if (surface->small_icon_buffer && type != ICON_BIG)
    {
        wayland_shm_buffer_unref(surface->small_icon_buffer);
        surface->small_icon_buffer = NULL;
    }

    if (icon_buf)
    {
        if (type == ICON_BIG) surface->big_icon_buffer = icon_buf;
        else surface->small_icon_buffer = icon_buf;
    }
}

/**********************************************************************
 *          wayland_surface_assign_icon
 */
void wayland_surface_assign_icon(struct wayland_surface *surface)
{
    if (!process_wayland.xdg_toplevel_icon_manager_v1) return;

    assert(wayland_surface_is_toplevel(surface));

    TRACE("surface=%p\n", surface);

    if (surface->xdg_toplevel_icon)
    {
        xdg_toplevel_icon_manager_v1_set_icon(process_wayland.xdg_toplevel_icon_manager_v1,
                                              surface->xdg_toplevel, NULL);
        xdg_toplevel_icon_v1_destroy(surface->xdg_toplevel_icon);
        surface->xdg_toplevel_icon = NULL;
    }

    if (surface->big_icon_buffer)
    {
        surface->xdg_toplevel_icon =
            xdg_toplevel_icon_manager_v1_create_icon(process_wayland.xdg_toplevel_icon_manager_v1);

        /* FIXME: what to do with scale ? */
        xdg_toplevel_icon_v1_add_buffer(surface->xdg_toplevel_icon,
                                        surface->big_icon_buffer->wl_buffer, 1);
        if (surface->small_icon_buffer)
        {
            xdg_toplevel_icon_v1_add_buffer(surface->xdg_toplevel_icon,
                                            surface->small_icon_buffer->wl_buffer, 1);
        }

        xdg_toplevel_icon_v1_set_name(surface->xdg_toplevel_icon, "");

        xdg_toplevel_icon_manager_v1_set_icon(process_wayland.xdg_toplevel_icon_manager_v1,
                                              surface->xdg_toplevel, surface->xdg_toplevel_icon);
    }
}

void wayland_surface_set_opacity(struct wayland_surface *surface, BYTE alpha, UINT flags)
{
    uint32_t opacity;

    if (!surface->wp_alpha_modifier_surface_v1) return;

    opacity = (flags & LWA_ALPHA) ? (UINT32_MAX / 0xff) * alpha : UINT32_MAX;
    wp_alpha_modifier_surface_v1_set_multiplier(surface->wp_alpha_modifier_surface_v1, opacity);
    wl_surface_commit(surface->wl_surface);
    wl_display_flush(process_wayland.wl_display);
}

static void activate_token_handle_done(void *user_data,
                                       struct xdg_activation_token_v1 *xdg_activation_token_v1,
                                       const char *token)
{
    HWND hwnd = user_data;
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    if (!(data = wayland_win_data_get(hwnd))) goto done;

    if ((surface = data->wayland_surface))
        xdg_activation_v1_activate(process_wayland.xdg_activation_v1, token, surface->wl_surface);
    wayland_win_data_release(data);

done:
    xdg_activation_token_v1_destroy(xdg_activation_token_v1);
}

const static struct xdg_activation_token_v1_listener activate_listener = {
    activate_token_handle_done,
};

static void save_token_handle_done(void *user_data,
                                   struct xdg_activation_token_v1 *xdg_activation_token_v1,
                                   const char *token)
{
    /* TODO: move this token to wineserver (or a shared memory) */
    char *temp = InterlockedExchangePointer((void **)&process_activate_token, strdup(token));
    xdg_activation_token_v1_destroy(xdg_activation_token_v1);
    if (temp) free(temp);
}

const static struct xdg_activation_token_v1_listener save_token_listener = {
    save_token_handle_done,
};

static struct xdg_activation_token_v1 *wayland_surface_create_token(
                                        struct wayland_surface *surface,
                                        BOOL activate)
{
    struct wayland_seat *seat = &process_wayland.seat;
    struct xdg_activation_token_v1 *token;
    uint32_t serial = ReadAcquire(&process_wayland.input_serial);

    if (!process_wayland.xdg_activation_v1) return NULL;
    if (!wayland_surface_is_toplevel(surface)) return NULL;

    if (!(token = xdg_activation_v1_get_activation_token(process_wayland.xdg_activation_v1)))
    {
        ERR("Failed to create activation token!\n");
        return NULL;
    }

    xdg_activation_token_v1_set_surface(token, surface->wl_surface);
    if (process_name && activate) xdg_activation_token_v1_set_app_id(token, process_name);

    pthread_mutex_lock(&seat->mutex);
    if (activate && seat->wl_seat) xdg_activation_token_v1_set_serial(token, serial, seat->wl_seat);
    pthread_mutex_unlock(&seat->mutex);

    return token;
}

void wayland_surface_flash_window(struct wayland_surface *surface)
{
    struct xdg_activation_token_v1 *token;

    if (!process_wayland.xdg_activation_v1) return;
    if (!wayland_surface_is_toplevel(surface)) return;

    if (!(token = wayland_surface_create_token(surface, FALSE))) return;
    xdg_activation_token_v1_add_listener(token, &activate_listener, surface->hwnd);
    xdg_activation_token_v1_commit(token);
}

void wayland_surface_refresh_token(struct wayland_surface *surface)
{
    struct xdg_activation_token_v1 *token;

    if (!process_wayland.xdg_activation_v1) return;
    if (!wayland_surface_is_toplevel(surface)) return;

    if (!(token = wayland_surface_create_token(surface, TRUE))) return;
    xdg_activation_token_v1_add_listener(token, &save_token_listener, NULL);
    xdg_activation_token_v1_commit(token);
}

void wayland_surface_activate(struct wayland_surface *surface)
{
    struct xdg_activation_token_v1 *token;
    char *token_str;

    if (!process_wayland.xdg_activation_v1) return;
    if (!wayland_surface_is_toplevel(surface)) return;

    token_str = InterlockedExchangePointer((void **)&process_activate_token, NULL);
    if (token_str)
    {
        xdg_activation_v1_activate(process_wayland.xdg_activation_v1, token_str, surface->wl_surface);
        free(token_str);
    }
    else
    {
        if (!(token = wayland_surface_create_token(surface, TRUE))) return;
        xdg_activation_token_v1_add_listener(token, &activate_listener, surface->hwnd);
        xdg_activation_token_v1_commit(token);
    }

    /* The previous activation token is now consumed. The window that (hopefully) gets
     * activated will create a new token for us to use in the future. */
}

static BOOL use_inhibit(void)
{
    static int enabled = -1;

    if (enabled == -1)
    {
        const char *env = getenv("WAYLANDDRV_SHORTCUT_INHIBIT");
        enabled = env && atoi(env);
    }

    return enabled;
}

void wayland_surface_shortcut_control(struct wayland_surface *surface, BOOL inhibit)
{
    BOOL should_inhibit = inhibit && use_inhibit();

    if (!process_wayland.zwp_keyboard_shortcuts_inhibit_manager_v1) return;

    if (should_inhibit)
    {
        if (!surface->zwp_keyboard_shortcuts_inhibitor_v1)
        {
            surface->zwp_keyboard_shortcuts_inhibitor_v1 =
                zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                    process_wayland.zwp_keyboard_shortcuts_inhibit_manager_v1,
                    surface->wl_surface, process_wayland.seat.wl_seat);
            /* dont create a listener since we dont care
             * if the shortcuts are actually inhibited or not */
        }
    }
    else if (surface->zwp_keyboard_shortcuts_inhibitor_v1)
    {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(
            surface->zwp_keyboard_shortcuts_inhibitor_v1);
        surface->zwp_keyboard_shortcuts_inhibitor_v1 = NULL;
    }
}
