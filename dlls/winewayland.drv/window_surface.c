/*
 * Wayland window surface implementation
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

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "waylanddrv.h"
#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#define GDI_OVERLAY_RING_SIZE 3

RGNDATA *get_region_data(HRGN region);

struct wayland_buffer_queue
{
    struct wl_event_queue *wl_event_queue;
    struct wl_list buffer_list;
    int width;
    int height;
    uint32_t format;
};

struct wayland_gdi_overlay_slot
{
    HANDLE section;
    void *bits;
    int fd;
    RECT damage;
    UINT64 release_token;
    unsigned int image_id;
    BOOL busy, initialized, has_damage;
};

struct wayland_gdi_overlay_producer
{
    int channel_fd;
    UINT64 producer_unique_id;
    UINT64 next_release_token;
    unsigned int frame_seq;
    unsigned int ring_generation;
    int width, height, stride, size;
    void *master_bits;
    HRGN region;
    HRGN pending_region;
    BOOL lost, slots_created;
    struct wayland_gdi_overlay_slot slots[GDI_OVERLAY_RING_SIZE];
};

struct wayland_window_surface
{
    struct window_surface header;
    struct wayland_buffer_queue *wayland_buffer_queue;
    struct wayland_gdi_overlay_producer gdi_overlay;
    BOOL layered;
    BOOL occlusion_clipped;
};

static LONGLONG volatile gdi_overlay_next_producer_id;

static struct wayland_window_surface *wayland_window_surface_cast(
    struct window_surface *window_surface)
{
    return (struct wayland_window_surface *)window_surface;
}

static void wayland_gdi_overlay_slot_destroy(struct wayland_gdi_overlay_slot *slot)
{
    if (slot->fd >= 0) close(slot->fd);
    if (slot->bits) NtUnmapViewOfSection(GetCurrentProcess(), slot->bits);
    if (slot->section) NtClose(slot->section);
    memset(slot, 0, sizeof(*slot));
    slot->fd = -1;
}

static void wayland_gdi_overlay_destroy_slots(struct wayland_gdi_overlay_producer *producer)
{
    unsigned int i;

    for (i = 0; i < GDI_OVERLAY_RING_SIZE; i++)
        wayland_gdi_overlay_slot_destroy(&producer->slots[i]);
    free(producer->master_bits);
    producer->master_bits = NULL;
    if (producer->pending_region)
    {
        NtGdiDeleteObjectApp(producer->pending_region);
        producer->pending_region = 0;
    }
    producer->slots_created = FALSE;
}

static void wayland_gdi_overlay_clear_region(struct wayland_gdi_overlay_producer *producer)
{
    if (producer->region)
    {
        NtGdiDeleteObjectApp(producer->region);
        producer->region = 0;
    }
}

static BOOL wayland_gdi_overlay_slot_create(struct wayland_gdi_overlay_slot *slot,
                                            unsigned int image_id, int size)
{
    LARGE_INTEGER section_size;
    SIZE_T view_size = 0;
    NTSTATUS status;

    memset(slot, 0, sizeof(*slot));
    slot->fd = -1;
    slot->image_id = image_id;

    section_size.QuadPart = size;
    status = NtCreateSection(&slot->section, GENERIC_READ | SECTION_MAP_READ | SECTION_MAP_WRITE,
                             NULL, &section_size, PAGE_READWRITE, SEC_COMMIT, 0);
    if (status)
    {
        WARN("failed to create GDI overlay shm section status %#x\n", status);
        return FALSE;
    }

    status = NtMapViewOfSection(slot->section, GetCurrentProcess(), &slot->bits, 0, 0, NULL,
                                &view_size, ViewUnmap, 0, PAGE_READWRITE);
    if (status)
    {
        WARN("failed to map GDI overlay shm section status %#x\n", status);
        wayland_gdi_overlay_slot_destroy(slot);
        return FALSE;
    }

    status = wine_server_handle_to_fd(slot->section, FILE_READ_DATA, &slot->fd, NULL);
    if (status)
    {
        WARN("failed to export GDI overlay shm section status %#x\n", status);
        wayland_gdi_overlay_slot_destroy(slot);
        return FALSE;
    }

    return TRUE;
}

static BOOL wayland_gdi_overlay_ensure_slots(struct wayland_gdi_overlay_producer *producer,
                                             int width, int height)
{
    unsigned int i;

    if (width <= 0 || height <= 0) return FALSE;
    if (width > INT_MAX / 4 || height > INT_MAX / (width * 4)) return FALSE;

    if (producer->slots_created && producer->width == width && producer->height == height)
        return TRUE;

    wayland_gdi_overlay_destroy_slots(producer);
    producer->width = width;
    producer->height = height;
    producer->stride = width * 4;
    producer->size = producer->stride * height;
    producer->ring_generation++;
    if (!producer->ring_generation) producer->ring_generation++;

    if (!(producer->master_bits = calloc(1, producer->size)))
        return FALSE;

    for (i = 0; i < GDI_OVERLAY_RING_SIZE; i++)
        if (!wayland_gdi_overlay_slot_create(&producer->slots[i], i, producer->size))
        {
            while (i--) wayland_gdi_overlay_slot_destroy(&producer->slots[i]);
            free(producer->master_bits);
            producer->master_bits = NULL;
            return FALSE;
        }

    producer->slots_created = TRUE;
    TRACE("gdi_overlay allocated %u shm slots size=%d generation=%u\n",
          GDI_OVERLAY_RING_SIZE, producer->size, producer->ring_generation);
    return TRUE;
}

static void wayland_gdi_overlay_close_channel(HWND hwnd, struct wayland_gdi_overlay_producer *producer)
{
    if (producer->channel_fd >= 0)
    {
        close(producer->channel_fd);
        producer->channel_fd = -1;
        wine_hwnd_dmabuf_release_gdi_overlay_channel(hwnd);
    }
}

static BOOL wayland_gdi_overlay_ensure_channel(HWND hwnd, struct wayland_gdi_overlay_producer *producer)
{
    HANDLE handle = 0;
    LONGLONG producer_id;
    int fd = -1;

    if (producer->lost) return FALSE;
    if (producer->channel_fd >= 0) return TRUE;

    if (wine_hwnd_dmabuf_get_gdi_overlay_channel(hwnd, &handle) != HWND_DMABUF_OK || !handle)
    {
        producer->lost = TRUE;
        TRACE("gdi_overlay hwnd=%p failed to open producer channel\n", hwnd);
        return FALSE;
    }

    if (wine_server_handle_to_fd(handle, FILE_READ_DATA | FILE_WRITE_DATA, &fd, NULL))
        fd = -1;
    NtClose(handle);
    if (fd < 0)
    {
        wine_hwnd_dmabuf_release_gdi_overlay_channel(hwnd);
        producer->lost = TRUE;
        return FALSE;
    }

    producer_id = InterlockedIncrement64(&gdi_overlay_next_producer_id);
    if (!producer_id) producer_id = InterlockedIncrement64(&gdi_overlay_next_producer_id);
    producer->producer_unique_id = producer_id;
    producer->channel_fd = fd;
    TRACE("gdi_overlay hwnd=%p opened producer channel fd=%d producer=%s\n",
          hwnd, fd, wine_dbgstr_longlong(producer->producer_unique_id));
    return TRUE;
}

static int wayland_gdi_overlay_channel_result_from_errno(int err)
{
    switch (err)
    {
    case EPIPE:
    case ECONNRESET:
    case ENOTCONN:
    case ECONNABORTED:
#ifdef ESHUTDOWN
    case ESHUTDOWN:
#endif
    case EBADF:
        return HWND_DMABUF_CHANNEL_CLOSED;
    default:
        return HWND_DMABUF_CHANNEL_ERROR;
    }
}

static int wayland_gdi_overlay_channel_send(int channel_fd, const hwnd_dmabuf_frame_desc_t *desc, int fd)
{
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {0};
    struct iovec iov;
    ssize_t n;
    int send_fd;

    if ((send_fd = dup(fd)) < 0)
        return wayland_gdi_overlay_channel_result_from_errno(errno);

    iov.iov_base = (void *)desc;
    iov.iov_len = sizeof(*desc);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    {
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &send_fd, sizeof(int));
    }

    do n = sendmsg(channel_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    while (n < 0 && errno == EINTR);
    close(send_fd);
    return n == sizeof(*desc) ? HWND_DMABUF_CHANNEL_OK :
           n < 0 ? wayland_gdi_overlay_channel_result_from_errno(errno) : HWND_DMABUF_CHANNEL_ERROR;
}

static int wayland_gdi_overlay_channel_recv_release(int channel_fd, hwnd_dmabuf_release_t *release)
{
    ssize_t n;

    do n = recv(channel_fd, release, sizeof(*release), MSG_DONTWAIT);
    while (n < 0 && errno == EINTR);

    if (n == sizeof(*release)) return HWND_DMABUF_CHANNEL_OK;
#if EAGAIN == EWOULDBLOCK
    if (n < 0 && errno == EAGAIN) return HWND_DMABUF_CHANNEL_EMPTY;
#else
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return HWND_DMABUF_CHANNEL_EMPTY;
#endif
    if (!n) return HWND_DMABUF_CHANNEL_CLOSED;
    return n < 0 ? wayland_gdi_overlay_channel_result_from_errno(errno) : HWND_DMABUF_CHANNEL_ERROR;
}

static void wayland_gdi_overlay_drain_releases(HWND hwnd, struct wayland_gdi_overlay_producer *producer)
{
    hwnd_dmabuf_release_t rel;
    int ret;

    if (producer->channel_fd < 0) return;
    while ((ret = wayland_gdi_overlay_channel_recv_release(producer->channel_fd, &rel)) == HWND_DMABUF_CHANNEL_OK)
    {
        struct wayland_gdi_overlay_slot *slot;

        if (rel.producer_unique_id != producer->producer_unique_id) continue;
        if (rel.ring_generation != producer->ring_generation) continue;
        if (rel.image_id >= GDI_OVERLAY_RING_SIZE) continue;

        slot = &producer->slots[rel.image_id];
        if (!slot->release_token || slot->release_token != rel.release_token) continue;

        slot->busy = FALSE;
        slot->release_token = 0;
        TRACE("gdi_overlay hwnd=%p released slot=%u flags=%#x\n", hwnd, rel.image_id, rel.flags);
    }

    if (ret == HWND_DMABUF_CHANNEL_CLOSED)
    {
        producer->lost = TRUE;
        wayland_gdi_overlay_close_channel(hwnd, producer);
    }
}

static struct wayland_gdi_overlay_slot *wayland_gdi_overlay_get_free_slot(
        HWND hwnd, struct wayland_gdi_overlay_producer *producer)
{
    unsigned int i;

    wayland_gdi_overlay_drain_releases(hwnd, producer);
    for (i = 0; i < GDI_OVERLAY_RING_SIZE; i++)
        if (!producer->slots[i].busy)
            return &producer->slots[i];
    return NULL;
}

static UINT64 wayland_gdi_overlay_next_release_token(struct wayland_gdi_overlay_producer *producer)
{
    UINT64 token = ++producer->next_release_token;

    if (!token) token = ++producer->next_release_token;
    return token;
}

static BOOL wayland_gdi_overlay_master_copy_region(struct wayland_gdi_overlay_producer *producer,
                                                   struct wayland_shm_buffer *src, HRGN region)
{
    RECT buffer_rect = {0, 0, src->width, src->height};
    RECT *rgn_rect, *rgn_rect_end;
    RGNDATA *rgndata;
    BOOL copied = FALSE;

    if (!producer->master_bits || !region) return FALSE;
    if (!(rgndata = get_region_data(region))) return FALSE;

    rgn_rect = (RECT *)rgndata->Buffer;
    rgn_rect_end = rgn_rect + rgndata->rdh.nCount;

    for (; rgn_rect < rgn_rect_end; rgn_rect++)
    {
        RECT rect;
        int y;

        if (!intersect_rect(&rect, rgn_rect, &buffer_rect)) continue;
        for (y = rect.top; y < rect.bottom; y++)
        {
            const char *src_row = (const char *)src->map_data + (size_t)y * src->width * 4;
            char *dst_row = (char *)producer->master_bits + (size_t)y * producer->stride;

            memcpy(dst_row + (size_t)rect.left * 4, src_row + (size_t)rect.left * 4,
                   (size_t)(rect.right - rect.left) * 4);
            copied = TRUE;
        }
    }
    free(rgndata);
    return copied;
}

static void wayland_gdi_overlay_slot_add_damage(struct wayland_gdi_overlay_slot *slot,
                                                const RECT *damage)
{
    /* Stable slots may lag behind while the consumer owns them. */
    if (!slot->initialized) return;
    if (!slot->has_damage)
    {
        slot->damage = *damage;
        slot->has_damage = TRUE;
        return;
    }

    slot->damage.left = min(slot->damage.left, damage->left);
    slot->damage.top = min(slot->damage.top, damage->top);
    slot->damage.right = max(slot->damage.right, damage->right);
    slot->damage.bottom = max(slot->damage.bottom, damage->bottom);
}

static void wayland_gdi_overlay_add_slot_damage(struct wayland_gdi_overlay_producer *producer,
                                                HRGN region)
{
    RECT bounds = {0, 0, producer->width, producer->height}, damage;
    unsigned int i;
    int type;

    if (!region || (type = NtGdiGetRgnBox(region, &damage)) == ERROR)
        damage = bounds;
    else if (type == NULLREGION || !intersect_rect(&damage, &damage, &bounds))
        return;

    for (i = 0; i < GDI_OVERLAY_RING_SIZE; i++)
        wayland_gdi_overlay_slot_add_damage(&producer->slots[i], &damage);
}

static void wayland_gdi_overlay_copy_slot(struct wayland_gdi_overlay_producer *producer,
                                          struct wayland_gdi_overlay_slot *slot,
                                          const RECT *damage)
{
    RECT bounds = {0, 0, producer->width, producer->height}, rect;
    int y;

    if (!intersect_rect(&rect, damage, &bounds)) return;
    for (y = rect.top; y < rect.bottom; y++)
        memcpy((char *)slot->bits + (size_t)y * producer->stride + (size_t)rect.left * 4,
               (char *)producer->master_bits + (size_t)y * producer->stride + (size_t)rect.left * 4,
               (size_t)(rect.right - rect.left) * 4);
}

static void wayland_gdi_overlay_master_clear_region(struct wayland_gdi_overlay_producer *producer,
                                                    HRGN region)
{
    RECT buffer_rect = {0, 0, producer->width, producer->height};
    RECT *rgn_rect, *rgn_rect_end;
    RGNDATA *rgndata;

    if (!producer->master_bits || !region) return;
    if (!(rgndata = get_region_data(region))) return;

    rgn_rect = (RECT *)rgndata->Buffer;
    rgn_rect_end = rgn_rect + rgndata->rdh.nCount;

    for (; rgn_rect < rgn_rect_end; rgn_rect++)
    {
        RECT rect;
        int y;

        if (!intersect_rect(&rect, rgn_rect, &buffer_rect)) continue;
        for (y = rect.top; y < rect.bottom; y++)
            memset((char *)producer->master_bits + (size_t)y * producer->stride + (size_t)rect.left * 4,
                   0, (size_t)(rect.right - rect.left) * 4);
    }

    free(rgndata);
}

static void wayland_gdi_overlay_union_pending(struct wayland_gdi_overlay_producer *producer,
                                              HRGN region)
{
    if (!region) return;
    if (!producer->pending_region)
        producer->pending_region = NtGdiCreateRectRgn(0, 0, 0, 0);
    if (producer->pending_region)
        NtGdiCombineRgn(producer->pending_region, producer->pending_region, region, RGN_OR);
}

static BOOL wayland_gdi_overlay_update_region(struct wayland_gdi_overlay_producer *producer,
                                              HRGN region)
{
    HRGN old_region = 0, removed_region = 0;
    int type;

    if (!region)
    {
        wayland_gdi_overlay_clear_region(producer);
        return FALSE;
    }

    if (producer->region && NtGdiEqualRgn(region, producer->region)) return FALSE;

    if (!producer->region)
        producer->region = NtGdiCreateRectRgn(0, 0, 0, 0);
    if (!producer->region) return FALSE;

    if ((old_region = NtGdiCreateRectRgn(0, 0, 0, 0)) &&
        (removed_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        NtGdiCombineRgn(old_region, producer->region, 0, RGN_COPY);
        type = NtGdiCombineRgn(removed_region, old_region, region, RGN_DIFF);
        if (type != ERROR && type != NULLREGION && producer->master_bits)
        {
            wayland_gdi_overlay_master_clear_region(producer, removed_region);
            wayland_gdi_overlay_union_pending(producer, removed_region);
        }
    }
    if (old_region) NtGdiDeleteObjectApp(old_region);
    if (removed_region) NtGdiDeleteObjectApp(removed_region);

    NtGdiCombineRgn(producer->region, region, 0, RGN_COPY);
    return TRUE;
}

static void wayland_gdi_overlay_fill_full_dirty(hwnd_dmabuf_frame_desc_t *desc)
{
    desc->dirty_count = 1;
    desc->dirty_rects[0][0] = 0;
    desc->dirty_rects[0][1] = 0;
    desc->dirty_rects[0][2] = min(desc->width, 0xffff);
    desc->dirty_rects[0][3] = min(desc->height, 0xffff);
}

static void wayland_gdi_overlay_fill_region_dirty(hwnd_dmabuf_frame_desc_t *desc, HRGN region)
{
    RGNDATA *rgndata;
    RECT *rects;
    unsigned int i, count;

    if (!region)
    {
        wayland_gdi_overlay_fill_full_dirty(desc);
        return;
    }

    if (!(rgndata = get_region_data(region)))
    {
        wayland_gdi_overlay_fill_full_dirty(desc);
        return;
    }

    if (rgndata->rdh.nCount > HWND_DMABUF_MAX_DIRTY_RECTS)
    {
        wayland_gdi_overlay_fill_full_dirty(desc);
        free(rgndata);
        return;
    }

    rects = (RECT *)rgndata->Buffer;
    count = rgndata->rdh.nCount;
    for (i = 0; i < count; i++)
    {
        RECT rect = rects[i];

        rect.left = max(0, min(rect.left, (LONG)desc->width));
        rect.top = max(0, min(rect.top, (LONG)desc->height));
        rect.right = max(rect.left, min(rect.right, (LONG)desc->width));
        rect.bottom = max(rect.top, min(rect.bottom, (LONG)desc->height));
        if (rect.left >= rect.right || rect.top >= rect.bottom) continue;

        desc->dirty_rects[desc->dirty_count][0] = min(rect.left, 0xffff);
        desc->dirty_rects[desc->dirty_count][1] = min(rect.top, 0xffff);
        desc->dirty_rects[desc->dirty_count][2] = min(rect.right, 0xffff);
        desc->dirty_rects[desc->dirty_count][3] = min(rect.bottom, 0xffff);
        desc->dirty_count++;
    }
    if (!desc->dirty_count) wayland_gdi_overlay_fill_full_dirty(desc);
    free(rgndata);
}

static BOOL wayland_gdi_overlay_publish(HWND hwnd, struct wayland_gdi_overlay_producer *producer,
                                        HRGN dirty_region)
{
    hwnd_dmabuf_frame_desc_t desc;
    struct wayland_gdi_overlay_slot *slot;
    int ret;

    if (!producer->slots_created) return FALSE;
    wayland_gdi_overlay_add_slot_damage(producer, dirty_region);
    if (!wayland_gdi_overlay_ensure_channel(hwnd, producer)) return TRUE;
    if (!(slot = wayland_gdi_overlay_get_free_slot(hwnd, producer)))
    {
        TRACE("gdi_overlay hwnd=%p no free shm slot\n", hwnd);
        return FALSE;
    }

    if (!slot->initialized)
    {
        RECT full = {0, 0, producer->width, producer->height};
        wayland_gdi_overlay_copy_slot(producer, slot, &full);
    }
    else if (slot->has_damage)
    {
        wayland_gdi_overlay_copy_slot(producer, slot, &slot->damage);
    }
    slot->initialized = TRUE;
    slot->has_damage = FALSE;

    memset(&desc, 0, sizeof(desc));
    desc.version = HWND_DMABUF_DESC_VERSION_V1;
    desc.flags = HWND_DMABUF_FLAG_SHM | HWND_DMABUF_FLAG_STABLE_SLOT | HWND_DMABUF_FLAG_GDI_OVERLAY;
    desc.width = producer->width;
    desc.height = producer->height;
    desc.fourcc = HWND_DMABUF_SHM_FORMAT_ARGB8888;
    desc.stride = producer->stride;
    desc.frame_seq = ++producer->frame_seq;
    desc.ring_generation = producer->ring_generation;
    desc.image_id = slot->image_id;
    desc.modifier = HWND_DMABUF_MOD_LINEAR;
    desc.producer_unique_id = producer->producer_unique_id;
    desc.release_token = wayland_gdi_overlay_next_release_token(producer);
    desc.alpha_mode = HWND_DMABUF_ALPHA_MODE_UNSPECIFIED;
    desc.plane_count = 1;
    desc.plane_offsets[0] = 0;
    desc.plane_strides[0] = producer->stride;
    wayland_gdi_overlay_fill_region_dirty(&desc, dirty_region);

    slot->busy = TRUE;
    slot->release_token = desc.release_token;
    ret = wayland_gdi_overlay_channel_send(producer->channel_fd, &desc, slot->fd);
    if (ret == HWND_DMABUF_CHANNEL_OK)
    {
        RECT box = {0};
        if (dirty_region) NtGdiGetRgnBox(dirty_region, &box);
        TRACE("gdi_overlay hwnd=%p published shm slot=%u seq=%u size=%ux%u region=%s token=%s\n",
              hwnd, slot->image_id, desc.frame_seq, desc.width, desc.height,
              dirty_region ? wine_dbgstr_rect(&box) : "(clear)",
              wine_dbgstr_longlong(desc.release_token));
        return TRUE;
    }

    slot->busy = FALSE;
    slot->release_token = 0;
    WARN("gdi_overlay hwnd=%p failed to publish shm slot=%u ret=%d\n", hwnd, slot->image_id, ret);
    if (ret == HWND_DMABUF_CHANNEL_CLOSED)
    {
        producer->lost = TRUE;
        wayland_gdi_overlay_close_channel(hwnd, producer);
        return TRUE;
    }
    return FALSE;
}

static BOOL wayland_gdi_overlay_update(HWND hwnd, struct wayland_gdi_overlay_producer *producer,
                                       struct wayland_shm_buffer *src, const RECT *dirty,
                                       HRGN gdi_over_producer_region, HRGN gdi_over_paint_region)
{
    HRGN dirty_region, copy_region;
    int type;

    if (!gdi_over_producer_region)
    {
        if (producer->channel_fd >= 0)
            wayland_gdi_overlay_close_channel(hwnd, producer);
        wayland_gdi_overlay_destroy_slots(producer);
        wayland_gdi_overlay_clear_region(producer);
        producer->lost = FALSE;
        return TRUE;
    }

    if ((gdi_over_paint_region || producer->pending_region || producer->slots_created) &&
        !wayland_gdi_overlay_ensure_slots(producer, src->width, src->height))
        return !gdi_over_paint_region;
    wayland_gdi_overlay_update_region(producer, gdi_over_producer_region);
    if (!gdi_over_paint_region && !producer->pending_region) return TRUE;

    if (!(dirty_region = NtGdiCreateRectRgn(dirty->left, dirty->top, dirty->right, dirty->bottom)))
        return !gdi_over_paint_region;
    if (!(copy_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        NtGdiDeleteObjectApp(dirty_region);
        return !gdi_over_paint_region;
    }

    if (gdi_over_paint_region)
    {
        type = NtGdiCombineRgn(copy_region, dirty_region, gdi_over_producer_region, RGN_AND);
        if (type != ERROR && type != NULLREGION)
            type = NtGdiCombineRgn(copy_region, copy_region, gdi_over_paint_region, RGN_AND);
        if (type != ERROR && type != NULLREGION &&
            wayland_gdi_overlay_master_copy_region(producer, src, copy_region))
            wayland_gdi_overlay_union_pending(producer, copy_region);
    }

    if (producer->pending_region)
    {
        if (!wayland_gdi_overlay_publish(hwnd, producer, producer->pending_region))
        {
            NtGdiDeleteObjectApp(copy_region);
            NtGdiDeleteObjectApp(dirty_region);
            return FALSE;
        }
        NtGdiDeleteObjectApp(producer->pending_region);
        producer->pending_region = 0;
    }

    NtGdiDeleteObjectApp(copy_region);
    NtGdiDeleteObjectApp(dirty_region);
    return TRUE;
}

static BOOL window_surface_has_occlusion_clip(struct window_surface *window_surface)
{
    if (!window_surface->clip_region) return FALSE;
    if (!window_surface->shape_region) return TRUE;
    return !NtGdiEqualRgn(window_surface->clip_region, window_surface->shape_region);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_shm_buffer *shm_buffer = data;
    TRACE("shm_buffer=%p\n", shm_buffer);
    shm_buffer->busy = FALSE;
    wayland_shm_buffer_unref(shm_buffer);
}

static const struct wl_buffer_listener buffer_listener = { buffer_release };

/**********************************************************************
 *          wayland_buffer_queue_destroy
 *
 * Destroys a buffer queue and any contained buffers.
 */
static void wayland_buffer_queue_destroy(struct wayland_buffer_queue *queue)
{
    struct wayland_shm_buffer *shm_buffer, *next;

    wl_list_for_each_safe(shm_buffer, next, &queue->buffer_list, link)
    {
        wl_list_remove(&shm_buffer->link);
        wl_list_init(&shm_buffer->link);
        /* Since this buffer may still be busy, attach it to the per-process
         * wl_event_queue to handle any future buffer release events. */
        wl_proxy_set_queue((struct wl_proxy *)shm_buffer->wl_buffer,
                           process_wayland.wl_event_queue);
        wayland_shm_buffer_unref(shm_buffer);
    }

    if (queue->wl_event_queue)
    {
        /* Dispatch the event queue before destruction to process any
         * pending buffer release events. This is required after changing
         * the buffer proxy event queue in the previous step, to avoid
         * missing any events. */
        wl_display_dispatch_queue_pending(process_wayland.wl_display,
                                          queue->wl_event_queue);
        wl_event_queue_destroy(queue->wl_event_queue);
    }

    free(queue);
}

/**********************************************************************
 *          wayland_buffer_queue_create
 *
 * Creates a buffer queue containing buffers with the specified width and height.
 */
static struct wayland_buffer_queue *wayland_buffer_queue_create(int width, int height,
                                                                uint32_t format)
{
    struct wayland_buffer_queue *queue;

    queue = calloc(1, sizeof(*queue));
    if (!queue) goto err;

#if (WAYLAND_VERSION_MAJOR == 1 && WAYLAND_VERSION_MINOR >= 23)
    {
        char buffer[1024];
        snprintf(buffer, ARRAY_SIZE(buffer), "%s buffer queue",
                 process_name ? process_name : "winewayland");
        queue->wl_event_queue =
            wl_display_create_queue_with_name(process_wayland.wl_display, buffer);
    }
#else
    queue->wl_event_queue = wl_display_create_queue(process_wayland.wl_display);
#endif
    if (!queue->wl_event_queue) goto err;
    queue->width = width;
    queue->height = height;
    queue->format = format;

    wl_list_init(&queue->buffer_list);

    return queue;

err:
    if (queue) wayland_buffer_queue_destroy(queue);
    return NULL;
}

/**********************************************************************
 *          wayland_buffer_queue_get_free_buffer
 *
 * Gets a free buffer from the buffer queue. If no free buffers
 * are available this function blocks until it can provide one.
 */
static struct wayland_shm_buffer *wayland_buffer_queue_get_free_buffer(struct wayland_buffer_queue *queue)
{
    struct wayland_shm_buffer *shm_buffer;

    TRACE("queue=%p\n", queue);

    while (TRUE)
    {
        int nbuffers = 0;

        /* Dispatch any pending buffer release events. */
        wl_display_dispatch_queue_pending(process_wayland.wl_display,
                                          queue->wl_event_queue);

        /* Search through our buffers to find an available one. */
        wl_list_for_each(shm_buffer, &queue->buffer_list, link)
        {
            if (!shm_buffer->busy) goto out;
            nbuffers++;
        }

        /* Dynamically create up to 3 buffers. */
        if (nbuffers < 3)
        {
            shm_buffer = wayland_shm_buffer_create(queue->width, queue->height,
                                                   queue->format);
            if (shm_buffer)
            {
                /* Buffer events go to their own queue so that we can dispatch
                 * them independently. */
                wl_proxy_set_queue((struct wl_proxy *) shm_buffer->wl_buffer,
                                   queue->wl_event_queue);
                wl_buffer_add_listener(shm_buffer->wl_buffer, &buffer_listener,
                                       shm_buffer);
                wl_list_insert(&queue->buffer_list, &shm_buffer->link);
                goto out;
            }
            else if (nbuffers < 2)
            {
                /* If we failed to allocate a new buffer, but we have at least two
                 * buffers busy, there is a good chance the compositor will
                 * eventually release one of them, so dispatch events and wait
                 * below. Otherwise, give up and return a NULL buffer. */
                ERR(" => failed to acquire buffer\n");
                return NULL;
            }
        }

        /* We don't have any buffers available, so block waiting for a buffer
         * release event. */
        if (wl_display_dispatch_queue(process_wayland.wl_display,
                                      queue->wl_event_queue) == -1)
        {
            return NULL;
        }
    }

out:
    TRACE(" => %p %dx%d map=[%p, %p)\n",
          shm_buffer, shm_buffer->width, shm_buffer->height, shm_buffer->map_data,
          (unsigned char*)shm_buffer->map_data + shm_buffer->map_size);

    return shm_buffer;
}

/**********************************************************************
 *          wayland_buffer_queue_add_damage
 */
static void wayland_buffer_queue_add_damage(struct wayland_buffer_queue *queue, HRGN damage)
{
    struct wayland_shm_buffer *shm_buffer;

    wl_list_for_each(shm_buffer, &queue->buffer_list, link)
    {
        NtGdiCombineRgn(shm_buffer->damage_region, shm_buffer->damage_region,
                        damage, RGN_OR);
    }
}

/***********************************************************************
 *           wayland_window_surface_set_clip
 */
static void wayland_window_surface_set_clip(struct window_surface *window_surface,
                                            const RECT *rects, UINT count)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    BOOL occlusion_clipped;

    TRACE("hwnd=%p rects=%p count=%u\n", window_surface->hwnd, rects, count);

    occlusion_clipped = window_surface_has_occlusion_clip(window_surface);

    if (wws->occlusion_clipped || occlusion_clipped)
    {
        /* Repaint the full surface when the compositor-visible region changes. */
        window_surface->bounds = window_surface->rect;
        NtUserPostMessage(window_surface->hwnd, WM_WAYLAND_EXPOSE, 0, 0);
    }
    wws->occlusion_clipped = occlusion_clipped;

    if (window_surface->hwnd)
        NtUserPostMessage(window_surface->hwnd, WM_WINE_UPDATEWINDOWSTATE, 0, 0);
}

/**********************************************************************
 *          get_region_data
 */
RGNDATA *get_region_data(HRGN region)
{
    RGNDATA *data;
    DWORD size;

    if (!region) return NULL;
    if (!(size = NtGdiGetRegionData(region, 0, NULL))) return NULL;
    if (!(data = malloc(size))) return NULL;
    if (!NtGdiGetRegionData(region, size, data))
    {
        free(data);
        return NULL;
    }

    return data;
}

static void wayland_window_surface_sync_regions(struct window_surface *window_surface)
{
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(window_surface->hwnd)))
        return;

    if (data->wayland_surface)
        wayland_surface_sync_window_regions(data->wayland_surface, window_surface);

    wayland_win_data_release(data);
}

/**********************************************************************
 *          copy_pixel_region
 */
static void copy_pixel_region(const char *src_pixels, RECT *src_rect,
                              char *dst_pixels, RECT *dst_rect,
                              HRGN region, BOOL force_opaque)
{
    static const int bpp = WINEWAYLAND_BYTES_PER_PIXEL;
    RGNDATA *rgndata = get_region_data(region);
    RECT *rgn_rect;
    RECT *rgn_rect_end;
    int src_stride, dst_stride;

    if (!rgndata) return;

    src_stride = (src_rect->right - src_rect->left) * bpp;
    dst_stride = (dst_rect->right - dst_rect->left) * bpp;

    rgn_rect = (RECT *)rgndata->Buffer;
    rgn_rect_end = rgn_rect + rgndata->rdh.nCount;

    for (;rgn_rect < rgn_rect_end; rgn_rect++)
    {
        const char *src;
        char *dst;
        int x, y, width, height;
        RECT rc;

        TRACE("rect %s\n", wine_dbgstr_rect(rgn_rect));

        if (!intersect_rect(&rc, rgn_rect, src_rect)) continue;
        if (!intersect_rect(&rc, &rc, dst_rect)) continue;

        src = src_pixels + (rc.top - src_rect->top) * src_stride + (rc.left - src_rect->left) * bpp;
        dst = dst_pixels + (rc.top - dst_rect->top) * dst_stride + (rc.left - dst_rect->left) * bpp;
        width = rc.right - rc.left;
        height = rc.bottom - rc.top;

        /* Fast path for full width rectangles. */
        if (width * bpp == src_stride && src_stride == dst_stride)
        {
            if (force_opaque)
            {
                for (x = 0; x < height * width; ++x)
                    ((UINT32 *)dst)[x] = ((UINT32 *)src)[x] | 0xff000000;
            }
            else memcpy(dst, src, height * width * 4);
            continue;
        }

        if (force_opaque)
        {
            for (y = 0; y < height; y++)
            {
                for (x = 0; x < width; ++x)
                    ((UINT32 *)dst)[x] = ((UINT32 *)src)[x] | 0xff000000;
                src += src_stride;
                dst += dst_stride;
            }
        }
        else
        {
            for (y = 0; y < height; y++)
            {
                memcpy(dst, src, width * 4);
                src += src_stride;
                dst += dst_stride;
            }
        }
    }

    free(rgndata);
}

static void clear_pixel_region(struct wayland_shm_buffer *buffer, HRGN region)
{
    RGNDATA *rgndata = get_region_data(region);
    RECT buffer_rect = {0, 0, buffer->width, buffer->height};
    RECT *rgn_rect, *rgn_rect_end;

    if (!rgndata) return;

    rgn_rect = (RECT *)rgndata->Buffer;
    rgn_rect_end = rgn_rect + rgndata->rdh.nCount;

    for (; rgn_rect < rgn_rect_end; rgn_rect++)
    {
        RECT rect;
        int y, width;

        if (!intersect_rect(&rect, rgn_rect, &buffer_rect)) continue;
        width = rect.right - rect.left;

        for (y = rect.top; y < rect.bottom; y++)
            memset((char *)buffer->map_data + ((size_t)y * buffer->width + rect.left) * 4,
                   0, (size_t)width * 4);
    }

    free(rgndata);
}

static void wayland_shm_buffer_clear_outside_clip(struct wayland_shm_buffer *buffer,
                                                  const RECT *dirty, HRGN clip_region)
{
    HRGN dirty_region, clear_region;

    if (!clip_region) return;

    if (!(dirty_region = NtGdiCreateRectRgn(dirty->left, dirty->top, dirty->right, dirty->bottom)))
        return;
    if (!(clear_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        NtGdiDeleteObjectApp(dirty_region);
        return;
    }

    if (NtGdiCombineRgn(clear_region, dirty_region, clip_region, RGN_DIFF) != ERROR)
        clear_pixel_region(buffer, clear_region);

    NtGdiDeleteObjectApp(clear_region);
    NtGdiDeleteObjectApp(dirty_region);
}

static HRGN create_occluded_region(const RECT *surface_rect, HRGN clip_region)
{
    HRGN surface_region, occluded_region;
    int type;

    if (!clip_region) return 0;
    if (!(surface_region = NtGdiCreateRectRgn(surface_rect->left, surface_rect->top,
                                              surface_rect->right, surface_rect->bottom)))
        return 0;
    if (!(occluded_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        NtGdiDeleteObjectApp(surface_region);
        return 0;
    }

    type = NtGdiCombineRgn(occluded_region, surface_region, clip_region, RGN_DIFF);
    NtGdiDeleteObjectApp(surface_region);
    if (type == ERROR || type == NULLREGION)
    {
        NtGdiDeleteObjectApp(occluded_region);
        return 0;
    }
    return occluded_region;
}

static HRGN union_regions(HRGN a, HRGN b)
{
    HRGN region;

    if (!a) return b;
    if (!b) return a;
    if (!(region = NtGdiCreateRectRgn(0, 0, 0, 0))) return a;
    if (NtGdiCombineRgn(region, a, b, RGN_OR) == ERROR)
    {
        NtGdiDeleteObjectApp(region);
        return a;
    }
    return region;
}

/**********************************************************************
 *          wayland_shm_buffer_copy_data
 */
static void wayland_shm_buffer_copy_data(struct wayland_shm_buffer *buffer,
                                         const char *bits, RECT *rect,
                                         HRGN region, BOOL force_opaque)
{
    RECT buffer_rect = {0, 0, buffer->width, buffer->height};
    TRACE("buffer=%p bits=%p rect=%s\n", buffer, bits, wine_dbgstr_rect(rect));
    copy_pixel_region(bits, rect, buffer->map_data, &buffer_rect, region, force_opaque);
}

static void wayland_shm_buffer_copy(struct wayland_shm_buffer *src,
                                    struct wayland_shm_buffer *dst,
                                    HRGN region)
{
    RECT src_rect = {0, 0, src->width, src->height};
    RECT dst_rect = {0, 0, dst->width, dst->height};
    TRACE("src=%p dst=%p\n", src, dst);
    copy_pixel_region(src->map_data, &src_rect, dst->map_data, &dst_rect, region,
                      src->format == WL_SHM_FORMAT_XRGB8888 && dst->format == WL_SHM_FORMAT_ARGB8888);
}

/**********************************************************************
 *          wayland_shm_buffer_copy_shape
 */
static void wayland_shm_buffer_copy_shape(struct wayland_shm_buffer *buffer, const RECT *dirty,
                                          const BITMAPINFO *shape_info, const void *shape_bits)
{
    RECT dst_rect = {0, 0, buffer->width, buffer->height};
    UINT32 *color, shape_stride, color_stride, x, y;
    const BYTE *shape;
    RECT rect;

    shape_stride = shape_info->bmiHeader.biSizeImage / abs(shape_info->bmiHeader.biHeight);
    color_stride = dst_rect.right - dst_rect.left;

    if (!intersect_rect(&rect, &dst_rect, dirty)) return;

    color = (UINT32 *)buffer->map_data + rect.top * color_stride;
    shape = (const BYTE *)shape_bits + rect.top * shape_stride;

    for (y = rect.top; y < rect.bottom; y++, color += color_stride, shape += shape_stride)
    {
        for (x = rect.left; x < rect.right; x++)
        {
            if (!(shape[x / 8] & (1 << (7 - (x & 7))))) color[x] = 0;
        }
    }
}

/***********************************************************************
 *           wayland_window_surface_flush
 */
static BOOL wayland_window_surface_flush(struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                                         const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                         const BITMAPINFO *shape_info, const void *shape_bits)
{
    RECT surface_rect = {.right = color_info->bmiHeader.biWidth, .bottom = abs(color_info->bmiHeader.biHeight)};
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
    struct wayland_shm_buffer *shm_buffer = NULL, *latest_buffer = NULL;
    BOOL flushed = FALSE;
    BOOL overlay_flushed;
    BOOL content_over_producer;
    HRGN surface_damage_region = NULL;
    HRGN occluded_region = NULL;
    HRGN merged_gdi_over_region = NULL;
    HRGN gdi_over_region, gdi_over_paint_region;
    HRGN copy_from_window_region = NULL;
    HRGN self_paint_clip = NULL, clip_region = window_surface->clip_region;
    uint32_t buffer_format;

    if (!window_surface->app_painted_full && !window_surface->app_painted_region)
    {
        if (shape_changed) wayland_window_surface_sync_regions(window_surface);
        flushed = set_window_surface_contents(window_surface->hwnd, NULL, NULL, FALSE);
        wl_display_flush(process_wayland.wl_display);
        goto done;
    }

    surface_damage_region = NtGdiCreateRectRgn(rect->left + dirty->left, rect->top + dirty->top,
                                               rect->left + dirty->right, rect->top + dirty->bottom);
    if (!surface_damage_region)
    {
        ERR("failed to create surface damage region\n");
        goto done;
    }
    if (window_surface->app_painted_full)
        copy_from_window_region = surface_damage_region;
    else if (!(copy_from_window_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
    {
        ERR("failed to create copy_from_window region\n");
        goto done;
    }
    if (!window_surface->app_painted_full &&
        NtGdiCombineRgn(copy_from_window_region, surface_damage_region,
                        window_surface->app_painted_region, RGN_AND) == ERROR)
        goto done;

    /* Same-window GDI presents are newer than the retained native image.
     * Preserve their pixels even when the native client punched out SHM. */
    if (clip_region && window_surface->self_painted_region)
    {
        if (!(self_paint_clip = NtGdiCreateRectRgn(0, 0, 0, 0))) goto done;
        if (NtGdiCombineRgn(self_paint_clip, clip_region,
                           window_surface->self_painted_region, RGN_OR) == ERROR) goto done;
        clip_region = self_paint_clip;
    }

    buffer_format = (shape_bits || wws->occlusion_clipped || wws->layered) ?
                    WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888;
    if (wws->wayland_buffer_queue->format != buffer_format)
    {
        int width = wws->wayland_buffer_queue->width;
        int height = wws->wayland_buffer_queue->height;
        TRACE("recreating buffer queue with format %d\n", buffer_format);
        wayland_buffer_queue_destroy(wws->wayland_buffer_queue);
        wws->wayland_buffer_queue = wayland_buffer_queue_create(width, height, buffer_format);
    }

    wayland_buffer_queue_add_damage(wws->wayland_buffer_queue, surface_damage_region);

    shm_buffer = wayland_buffer_queue_get_free_buffer(wws->wayland_buffer_queue);
    if (!shm_buffer)
    {
        ERR("failed to acquire Wayland SHM buffer, returning\n");
        goto done;
    }

    if ((latest_buffer = get_window_surface_contents(window_surface->hwnd)))
    {
        TRACE("latest_window_buffer=%p\n", latest_buffer);
        /* If we have a latest buffer, use it as the source of all pixel
         * data that are not contained in the bounds of the flush... */
        if (latest_buffer != shm_buffer)
        {
            HRGN copy_from_latest_region = NtGdiCreateRectRgn(0, 0, 0, 0);
            if (!copy_from_latest_region)
            {
                ERR("failed to create copy_from_latest region\n");
                goto done;
            }
            NtGdiCombineRgn(copy_from_latest_region, shm_buffer->damage_region,
                            copy_from_window_region, RGN_DIFF);
            wayland_shm_buffer_copy(latest_buffer,
                                    shm_buffer, copy_from_latest_region);
            NtGdiDeleteObjectApp(copy_from_latest_region);
        }
        wayland_shm_buffer_unref(latest_buffer);
    }
    else
    {
        HRGN clear_region;

        TRACE("latest_window_buffer=NULL\n");

        if ((clear_region = NtGdiCreateRectRgn(0, 0, 0, 0)))
        {
            NtGdiCombineRgn(clear_region, surface_damage_region, copy_from_window_region, RGN_DIFF);
            clear_pixel_region(shm_buffer, clear_region);
            NtGdiDeleteObjectApp(clear_region);
        }
    }

    wayland_shm_buffer_copy_data(shm_buffer, color_bits, &surface_rect, copy_from_window_region,
                                 (shape_bits || wws->occlusion_clipped) && !wws->layered);
    if (shape_bits) wayland_shm_buffer_copy_shape(shm_buffer, rect, shape_info, shape_bits);

    gdi_over_region = window_surface->gdi_over_producer_region;
    gdi_over_paint_region = window_surface->gdi_over_paint_region;
    if (wws->occlusion_clipped && !wws->layered)
    {
        occluded_region = create_occluded_region(&surface_rect, window_surface->clip_region);
        merged_gdi_over_region = union_regions(window_surface->gdi_over_producer_region, occluded_region);
        gdi_over_region = merged_gdi_over_region;
    }

    overlay_flushed = wayland_gdi_overlay_update(window_surface->hwnd, &wws->gdi_overlay, shm_buffer, dirty,
                                                 gdi_over_region, gdi_over_paint_region);
    /* Child overlays and same-window GDI both take precedence over native pixels. */
    content_over_producer = wws->gdi_overlay.slots_created || window_surface->self_painted_region;
    if (wws->occlusion_clipped)
        wayland_shm_buffer_clear_outside_clip(shm_buffer, dirty, clip_region);

    NtGdiSetRectRgn(shm_buffer->damage_region, 0, 0, 0, 0);

    if (shape_changed) wayland_window_surface_sync_regions(window_surface);

    /* Keep the parent surface and GDI overlay in lock-step. If the overlay
     * cannot publish yet, leave the surface dirty so the next idle flush
     * retries both together. */
    if (overlay_flushed)
        flushed = set_window_surface_contents(window_surface->hwnd, shm_buffer,
                                              surface_damage_region, content_over_producer);
    wl_display_flush(process_wayland.wl_display);

done:
    if (self_paint_clip) NtGdiDeleteObjectApp(self_paint_clip);
    if (merged_gdi_over_region && merged_gdi_over_region != window_surface->gdi_over_producer_region &&
        merged_gdi_over_region != occluded_region)
        NtGdiDeleteObjectApp(merged_gdi_over_region);
    if (occluded_region) NtGdiDeleteObjectApp(occluded_region);
    if (copy_from_window_region && copy_from_window_region != surface_damage_region)
        NtGdiDeleteObjectApp(copy_from_window_region);
    if (surface_damage_region) NtGdiDeleteObjectApp(surface_damage_region);
    return flushed;
}

/***********************************************************************
 *           wayland_window_surface_destroy
 */
static void wayland_window_surface_destroy(struct window_surface *window_surface)
{
    struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);

    TRACE("surface=%p\n", wws);

    wayland_gdi_overlay_close_channel(window_surface->hwnd, &wws->gdi_overlay);
    wayland_gdi_overlay_destroy_slots(&wws->gdi_overlay);
    wayland_gdi_overlay_clear_region(&wws->gdi_overlay);
    wayland_buffer_queue_destroy(wws->wayland_buffer_queue);
}

static const struct window_surface_funcs wayland_window_surface_funcs =
{
    wayland_window_surface_set_clip,
    wayland_window_surface_flush,
    wayland_window_surface_destroy
};

/***********************************************************************
 *           wayland_window_surface_create
 */
static struct window_surface *wayland_window_surface_create(HWND hwnd, const RECT *rect,
                                                            BOOL layered)
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct wayland_window_surface *wws;
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;
    struct window_surface *window_surface;

    TRACE("hwnd %p rect %s\n", hwnd, wine_dbgstr_rect(rect));

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = width * height * 4;
    info->bmiHeader.biCompression = BI_RGB;

    if ((window_surface = window_surface_create(sizeof(*wws), &wayland_window_surface_funcs, hwnd, rect, info, 0)))
    {
        struct wayland_window_surface *wws = wayland_window_surface_cast(window_surface);
        unsigned int i;

        wws->wayland_buffer_queue =
            wayland_buffer_queue_create(width, height,
                                        layered ? WL_SHM_FORMAT_ARGB8888 :
                                                  WL_SHM_FORMAT_XRGB8888);
        wws->gdi_overlay.channel_fd = -1;
        for (i = 0; i < GDI_OVERLAY_RING_SIZE; i++)
            wws->gdi_overlay.slots[i].fd = -1;
        wws->layered = layered;
    }

    return window_surface;
}

/***********************************************************************
 *           WAYLAND_CreateWindowSurface
 */
BOOL WAYLAND_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface)
{
    struct window_surface *previous;
    struct wayland_win_data *data;

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n", hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

    if ((previous = *surface) && previous->funcs == &wayland_window_surface_funcs) return TRUE;
    if (!(data = wayland_win_data_get(hwnd))) return TRUE; /* use default surface */
    if (previous) window_surface_release(previous);

    if (layered) data->layered_attribs_set = TRUE;
    *surface = wayland_window_surface_create(data->hwnd, surface_rect, layered);

    wayland_win_data_release(data);
    return TRUE;
}
