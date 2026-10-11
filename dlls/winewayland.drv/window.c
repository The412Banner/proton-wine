/*
 * Wayland window handling
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
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"
#include "wine/hwnd_dmabuf.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);


static int wayland_win_data_cmp_rb(const void *key,
                                   const struct rb_entry *entry)
{
    HWND key_hwnd = (HWND)key; /* cast to work around const */
    const struct wayland_win_data *entry_win_data =
        RB_ENTRY_VALUE(entry, const struct wayland_win_data, entry);

    if (key_hwnd < entry_win_data->hwnd) return -1;
    if (key_hwnd > entry_win_data->hwnd) return 1;
    return 0;
}

static pthread_mutex_t win_data_mutex;
static struct rb_tree win_data_rb = { wayland_win_data_cmp_rb };
static BOOL window_surface_configure_blocks_dmabuf(HWND hwnd);
static BOOL window_surface_has_requested_configure(HWND hwnd);
static BOOL window_surface_has_hwnd_dmabuf_content(HWND hwnd);

static const WCHAR frameless_window_prop[] =
    {'_','_','w','i','n','e','_','w','i','n','3','2','u','_','f','r','a','m','e','l','e','s','s',0};
static const WCHAR layer_menu_hwnd_prop[] =
    {'_','_','w','i','n','e','_','w','a','y','l','a','n','d','_','l','a','y','e','r','_','m','e','n','u',0};
static const WCHAR layer_menu_restore_hwnd_prop[] =
    {'_','_','w','i','n','e','_','w','a','y','l','a','n','d','_','l','a','y','e','r','_','m','e','n','u','_','r','e','s','t','o','r','e',0};

/***********************************************************************
 *           wayland_win_data_create
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *wayland_win_data_create(HWND hwnd, const struct window_rects *rects)
{
    struct wayland_win_data *data;
    struct rb_entry *rb_entry;
    HWND parent;

    /* Don't create win data for desktop or HWND_MESSAGE windows. */
    if (!(parent = NtUserGetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != NtUserGetDesktopWindow() && !NtUserGetAncestor(parent, GA_PARENT))
        return NULL;

    if (!(data = calloc(1, sizeof(*data)))) return NULL;

    data->hwnd = hwnd;
    data->rects = *rects;
    data->ime_enabled = FALSE;
    data->num_ime_children = 0;
    data->alpha_multiplier = UINT32_MAX;
    list_init(&data->client_surfaces);

    pthread_mutex_lock(&win_data_mutex);

    /* Check that another thread hasn't already created the wayland_win_data. */
    if ((rb_entry = rb_get(&win_data_rb, hwnd)))
    {
        free(data);
        return RB_ENTRY_VALUE(rb_entry, struct wayland_win_data, entry);
    }

    rb_put(&win_data_rb, hwnd, &data->entry);

    TRACE("hwnd=%p\n", data->hwnd);

    return data;
}

/***********************************************************************
 *           wayland_win_data_destroy
 */
static void wayland_win_data_destroy(struct wayland_win_data *data)
{
    struct wayland_client_surface *client, *next;

    TRACE("hwnd=%p\n", data->hwnd);

    LIST_FOR_EACH_ENTRY_SAFE(client, next, &data->client_surfaces,
                             struct wayland_client_surface, hwnd_entry)
    {
        wayland_client_surface_attach(client, NULL);
        list_remove(&client->hwnd_entry);
        list_init(&client->hwnd_entry);
        InterlockedExchange(&client->client.presentation_owner, FALSE);
    }
    data->client_surface = NULL;
    rb_remove(&win_data_rb, &data->entry);

    pthread_mutex_unlock(&win_data_mutex);

    if (data->stashed_client) client_surface_release(&data->stashed_client->client);
    if (data->wayland_surface) wayland_surface_destroy(data->wayland_surface);
    if (data->window_contents) wayland_shm_buffer_unref(data->window_contents);
    free(data);
}

/***********************************************************************
 *           wayland_win_data_get
 *
 * Lock and return the data structure associated with a window.
 */
struct wayland_win_data *wayland_win_data_get_nolock(HWND hwnd)
{
    struct rb_entry *entry;

    if ((entry = rb_get(&win_data_rb, hwnd)))
        return RB_ENTRY_VALUE(entry, struct wayland_win_data, entry);
    return NULL;
}

struct wayland_win_data *wayland_win_data_get(HWND hwnd)
{
    struct wayland_win_data *data;

    pthread_mutex_lock(&win_data_mutex);
    if ((data = wayland_win_data_get_nolock(hwnd))) return data;
    pthread_mutex_unlock(&win_data_mutex);
    return NULL;
}

/***********************************************************************
 *           wayland_win_data_release
 *
 * Release the data returned by wayland_win_data_get.
 */
void wayland_win_data_release(struct wayland_win_data *data)
{
    assert(data);
    pthread_mutex_unlock(&win_data_mutex);
}

static void wayland_win_data_queue_state_update(struct wayland_win_data *data,
                                                UINT state_cmd, const RECT *rect)
{
    data->state_update_cmd = state_cmd;
    data->state_update_swp_flags = 0;
    data->state_update_rect = *rect;
    data->state_update_foreground = NULL;
}

static BOOL wayland_win_data_is_fullscreen(struct wayland_win_data *data, DWORD style)
{
    const RECT *client = &data->rects.client;
    MONITORINFO mi;
    HMONITOR monitor;
    RECT rect;

    if (!data->is_fullscreen) return FALSE;
    if (!(style & WS_POPUP) &&
        (style & (WS_MAXIMIZE | WS_THICKFRAME)) == (WS_MAXIMIZE | WS_THICKFRAME))
        return FALSE;
    if (NtUserGetPresentRect(data->hwnd, &rect, -1)) return TRUE;
    if (!(style & (WS_CAPTION | WS_THICKFRAME))) return TRUE;

    /* Some games retain a thin border outside a monitor-sized client area
     * in windowed fullscreen mode (e.g. Dragon Age: Inquisition). */
    if ((style & (WS_CHILD | WS_POPUP | WS_CAPTION | WS_THICKFRAME)) !=
        (WS_POPUP | WS_BORDER))
        return FALSE;

    mi.cbSize = sizeof(mi);
    if (!(monitor = NtUserMonitorFromRect(&data->rects.window, MONITOR_DEFAULTTONEAREST)) ||
        !NtUserGetMonitorInfo(monitor, &mi))
        return FALSE;

    return client->right - client->left == mi.rcMonitor.right - mi.rcMonitor.left &&
           client->bottom - client->top == mi.rcMonitor.bottom - mi.rcMonitor.top;
}

static BOOL wayland_win_data_get_pending_config_state(struct wayland_win_data *data,
                                                      enum wayland_surface_config_state *state)
{
    struct wayland_surface *surface = data->wayland_surface;

    if (!surface || surface->role != WAYLAND_SURFACE_ROLE_TOPLEVEL ||
        !surface->xdg_surface || !surface->processing.serial)
        return FALSE;

    *state = surface->processing.state &
             (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
              WAYLAND_SURFACE_CONFIG_STATE_TILED |
              WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN);
    return TRUE;
}

static void wayland_win_data_get_config(struct wayland_win_data *data,
                                        struct wayland_window_config *conf)
{
    enum wayland_surface_config_state window_state = 0;
    enum wayland_surface_config_state pending_state;
    DWORD style, exstyle;
    BOOL fullscreen;

    conf->rect = data->rects.visible;
    conf->window_rect = data->rects.window;
    conf->client_rect = data->rects.client;
    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);
    exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);
    conf->exstyle = exstyle;
    fullscreen = wayland_win_data_is_fullscreen(data, style);

    TRACE("window=%s style=%#x exstyle=%#x\n", wine_dbgstr_rect(&conf->rect), style, exstyle);

    conf->minimized = FALSE;

    if (style & WS_MINIMIZE)
    {
        conf->minimized = TRUE;
    }
    else if (wayland_win_data_get_pending_config_state(data, &pending_state))
    {
        window_state |= pending_state;
    }
    /* The fullscreen state is implied by the window position and style. */
    else if (fullscreen)
    {
        if ((style & WS_MAXIMIZE) && (style & WS_CAPTION) == WS_CAPTION)
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
        else if (!(style & WS_MINIMIZE))
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
    }
    else if (style & WS_MAXIMIZE)
    {
        window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
    }

    /* A frame removal and fullscreen resize may arrive in separate WindowPos
     * updates, leaving the cached client rect with the old frame insets. */
    if ((window_state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        !(style & (WS_CAPTION | WS_THICKFRAME)))
        conf->client_rect = conf->rect;

    conf->resizeable = (style & WS_THICKFRAME) && !fullscreen;
    conf->state = window_state;
    conf->managed = data->managed;
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect)) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static BOOL rect_intersects_virtual_screen(const RECT *rect)
{
    RECT virtual_rect, intersect;

    virtual_rect.left = NtUserGetSystemMetrics(SM_XVIRTUALSCREEN);
    virtual_rect.top = NtUserGetSystemMetrics(SM_YVIRTUALSCREEN);
    virtual_rect.right = virtual_rect.left + NtUserGetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtual_rect.bottom = virtual_rect.top + NtUserGetSystemMetrics(SM_CYVIRTUALSCREEN);

    return intersect_rect(&intersect, rect, &virtual_rect);
}

static BOOL should_keep_minimized_toplevel_mapped(struct wayland_surface *surface,
                                                  DWORD style)
{
    if (!surface || surface->role != WAYLAND_SURFACE_ROLE_TOPLEVEL) return FALSE;
    if (!(style & WS_MINIMIZE)) return FALSE;

    return !surface->current.caps ||
           (surface->current.caps & WAYLAND_SURFACE_WM_CAPS_MINIMIZE);
}

static BOOL window_or_root_minimized(HWND hwnd)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);

    if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE) return TRUE;
    return root && root != hwnd && (NtUserGetWindowLongW(root, GWL_STYLE) & WS_MINIMIZE);
}

static void detach_client_surfaces_for_toplevel(HWND toplevel)
{
    struct wayland_win_data *data;

    RB_FOR_EACH_ENTRY(data, &win_data_rb, struct wayland_win_data, entry)
    {
        struct wayland_client_surface *client;

        LIST_FOR_EACH_ENTRY(client, &data->client_surfaces,
                            struct wayland_client_surface, hwnd_entry)
            if (client->toplevel == toplevel)
                wayland_client_surface_attach(client, NULL);
    }
}

static BOOL is_menu_popup_candidate_style(DWORD style, DWORD exstyle)
{
    if (!(style & WS_POPUP)) return FALSE;
    if ((style & WS_CAPTION) == WS_CAPTION) return FALSE;
    if (style & WS_SYSMENU) return FALSE;
    if (exstyle & WS_EX_APPWINDOW) return FALSE;
    return TRUE;
}

static BOOL should_defer_unanchored_menu_popup(DWORD style, DWORD exstyle,
                                               BOOL fullscreen, BOOL has_owner)
{
    if (!has_owner) return FALSE;
    if (!(exstyle & WS_EX_TOOLWINDOW)) return FALSE;
    if (fullscreen) return FALSE;
    return is_menu_popup_candidate_style(style, exstyle);
}

/* Some games create an inert fullscreen backing window and place it directly
 * behind their render window. It must share the render window's surface tree:
 * an independent toplevel has no controllable cross-toplevel z-order, while an
 * xdg_popup is required by the protocol to remain above its parent. */
static HWND get_fullscreen_backing_parent(HWND hwnd, HWND insert_after, BOOL fullscreen)
{
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    DWORD exstyle = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    HWND parent = insert_after;

    if (!fullscreen || !(style & WS_POPUP) || !(style & WS_DISABLED) ||
        !(exstyle & WS_EX_TOOLWINDOW) || (exstyle & WS_EX_APPWINDOW))
        return NULL;

    if (!parent || !NtUserIsWindow(parent))
        parent = NtUserGetWindowRelative(hwnd, GW_HWNDPREV);
    if (!parent || parent == hwnd || parent == NtUserGetDesktopWindow() ||
        !NtUserIsWindow(parent))
        return NULL;

    parent = NtUserGetAncestor(parent, GA_ROOT);
    if (!parent || parent == hwnd || parent == NtUserGetDesktopWindow() ||
        (DWORD)(ULONG_PTR)NtUserQueryWindow(parent, WindowProcess) != GetCurrentProcessId())
        return NULL;

    return parent;
}

static BOOL wayland_win_data_create_wayland_surface(struct wayland_win_data *data,
                                                    struct wayland_surface *toplevel_surface,
                                                    struct wayland_surface *owner_surface,
                                                    BOOL use_layer_shell,
                                                    BOOL layer_menu,
                                                    struct window_surface *window_surface,
                                                    UINT swp_flags,
                                                    BOOL has_menu_popup_owner,
                                                    BOOL stack_below_parent)
{
    struct wayland_client_surface *client = data->client_surface;
    struct wayland_surface *surface;
    enum wayland_surface_role role;
    BOOL visible, layer_set, server_decor = FALSE;
    DWORD exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);
    DWORD style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);

    TRACE("hwnd=%p\n", data->hwnd);

    surface = data->wayland_surface;

    layer_set = !(exstyle & WS_EX_LAYERED) || data->layered_attribs_set;
    visible = ((style & WS_VISIBLE) == WS_VISIBLE);

    /* if a window is layered and visible but doesn't have attributes set,
     * that only delays when it gets mapped: it doesn't cause the window to get unmapped. */
    if (!surface || !surface->window.visible)
        visible = visible && layer_set;

    /* A fullscreen renderer may hide and show the same HWND synchronously while
     * focus changes. Preserve its established presentation tree, but continue to
     * unmap minimized windows and non-rendering windows such as launchers. */
    if ((swp_flags & SWP_HIDEWINDOW) && data->is_fullscreen && !(style & WS_MINIMIZE) &&
        (client || wayland_surface_has_hwnd_dmabuf_content(surface)))
        visible = TRUE;

    if (visible && !owner_surface && !use_layer_shell && !toplevel_surface &&
        !rect_intersects_virtual_screen(&data->rects.window) &&
        !should_keep_minimized_toplevel_mapped(surface, style))
        visible = FALSE;

    if (!visible || (stack_below_parent && !toplevel_surface)) role = WAYLAND_SURFACE_ROLE_NONE;
    else if (owner_surface) role = WAYLAND_SURFACE_ROLE_POPUP;
    else if (use_layer_shell && !IsRectEmpty(&data->rects.window)) role = WAYLAND_SURFACE_ROLE_LAYER;
    else if (toplevel_surface) role = WAYLAND_SURFACE_ROLE_SUBSURFACE;
    else if (!client && should_defer_unanchored_menu_popup(style, exstyle,
                                                           data->is_fullscreen,
                                                           has_menu_popup_owner))
        role = WAYLAND_SURFACE_ROLE_NONE;
    else if (!IsRectEmpty(&data->rects.window)) role = WAYLAND_SURFACE_ROLE_TOPLEVEL;
    else role = WAYLAND_SURFACE_ROLE_NONE;

    if (surface && role == WAYLAND_SURFACE_ROLE_LAYER &&
        (surface->role == WAYLAND_SURFACE_ROLE_NONE ||
         (surface->role == WAYLAND_SURFACE_ROLE_LAYER && !surface->zwlr_layer_surface_v1)))
    {
        wayland_client_surfaces_attach(data, NULL);
        wayland_surface_destroy(data->wayland_surface);
        data->wayland_surface = NULL;
        surface = NULL;
    }

    /* we can temporarily clear the role of a surface but cannot assign a different one after it's set */
    if (surface && role && surface->role && surface->role != role)
    {
        /* Make sure any attached client surface is detached before we destroy the surface.
         * They will be reattached when win32u updates them again after WindowPosChanged.
         */
        data->wayland_surface = NULL;
        update_client_surfaces(data->hwnd);
        wayland_surface_destroy(surface);
    }

    if (!(surface = data->wayland_surface) && !(surface = wayland_surface_create(data->hwnd))) return FALSE;
    /* Client attachment resolves the parent through window data. Publish a
     * newly created carrier before refreshing every registered presenter. */
    data->wayland_surface = surface;

    surface->ensured_contents = WAYLAND_SURFACE_NOT_ENSURED;

    if (!EqualRect(&data->rects.visible, &data->rects.window)
        && !NtUserGetProp(data->hwnd, frameless_window_prop)
        && is_decoration_enabled(style, exstyle))
    {
        server_decor = TRUE;
    }

    surface->window.visible = visible;
    wayland_win_data_get_config(data, &surface->window);

    /* If the window is a visible toplevel make it a wayland
     * xdg_toplevel. Otherwise keep it role-less to avoid polluting the
     * compositor with empty xdg_toplevels. */
    switch (role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        if (surface->role) wayland_surface_clear_role(surface);
        break;
    case WAYLAND_SURFACE_ROLE_POPUP:
        wayland_surface_make_popup(surface, owner_surface);
        break;
    case WAYLAND_SURFACE_ROLE_LAYER:
        wayland_surface_make_layer(surface, &data->rects.window, layer_menu);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        wayland_surface_make_subsurface(surface, toplevel_surface,
                                        stack_below_parent);
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        wayland_surface_make_toplevel(surface, server_decor);
        break;
    }

    if (role != WAYLAND_SURFACE_ROLE_NONE && !window_or_root_minimized(data->hwnd))
        wayland_client_surfaces_attach(data, data->hwnd);
    else
        wayland_client_surfaces_attach(data, NULL);
    if (surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->window.minimized)
        detach_client_surfaces_for_toplevel(data->hwnd);
    wayland_surface_sync_window_regions(surface, window_surface);

    /* Size/position changes affect the effective pointer constraint, so update
     * it as needed. */
    if (data->hwnd == NtUserGetForegroundWindow()) reapply_cursor_clipping();

    TRACE("hwnd=%p surface=%p\n", data->hwnd, surface);
    return TRUE;
}

static BOOL wayland_surface_has_pending_state(struct wayland_surface *surface,
                                              enum wayland_surface_config_state state)
{
    if (surface->requested.serial && (surface->requested.state & state)) return TRUE;
    if (surface->processing.serial && (surface->processing.state & state)) return TRUE;
    return FALSE;
}

static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{
    const RECT *rect = &surface->window.rect;
    BOOL processing_config = surface->processing.serial;

    TRACE("hwnd=%p window_state=%#x %s->state=%#x\n",
          surface->hwnd, surface->window.state,
          processing_config ? "processing" : "current",
          processing_config ? surface->processing.state : surface->current.state);

    /* update the parent here as well to ensure that its not stale if the owner is updated
     * with no new contents comitted or state change */
    wayland_surface_update_toplevel_parent(surface);

    /* If we are not processing a compositor requested config, use the
     * window state to determine and update the Wayland state. */
    if (!processing_config)
    {
         /* First do all state unsettings, before setting new state. Some
          * Wayland compositors misbehave if the order is reversed. */
        if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
        {
            xdg_toplevel_unset_maximized(surface->xdg_toplevel);
        }
        if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        {
            xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
            wayland_surface_shortcut_control(surface, FALSE);
            surface->requested_output = NULL;
        }

        if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            !wayland_surface_has_pending_state(surface,
                                               WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
        {
            xdg_toplevel_set_maximized(surface->xdg_toplevel);
        }
        if (surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
        {
            struct wayland_output *output;
            struct wl_output *wl_output = NULL;

            if ((output = wayland_output_for_rect(rect)))
                wl_output = output->wl_output;

            if (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN)
            {
                if (surface->requested_output != wl_output)
                {
                    xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
                    wl_display_flush(process_wayland.wl_display);
                }
                else
                    goto skip_fullscreen;
            }
            else if (surface->requested_output == wl_output &&
                     wayland_surface_has_pending_state(surface,
                                                       WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
            {
                goto skip_fullscreen;
            }

            xdg_toplevel_set_fullscreen(surface->xdg_toplevel, wl_output);
            wayland_surface_shortcut_control(surface, TRUE);
            surface->requested_output = wl_output;

        skip_fullscreen:
            if (output) wayland_output_release(output);
        }
        if (surface->window.minimized && !surface->comitted.minimized)
        {
            xdg_toplevel_set_minimized(surface->xdg_toplevel);
        }

        /* Reset the size hint on state changes, and invalidate the cache so
         * fixed-size windows restore their limits on the next geometry update. */
        if (surface->comitted.state != surface->window.state)
        {
            xdg_toplevel_set_min_size(surface->xdg_toplevel, 0, 0);
            xdg_toplevel_set_max_size(surface->xdg_toplevel, 0, 0);
            memset(&surface->toplevel_size_limits, 0, sizeof(surface->toplevel_size_limits));
            wl_surface_commit(surface->wl_surface);
        }

        surface->comitted.minimized = surface->window.minimized;
        surface->comitted.state = surface->window.state;
    }
    else
    {
        /* Keep compositor configures authoritative until promotion. */
        surface->processing.processed = TRUE;
    }
}

static void wayland_win_data_update_wayland_state(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
    /* popups do not have any state to update */
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
    case WAYLAND_SURFACE_ROLE_POPUP:
    case WAYLAND_SURFACE_ROLE_LAYER:
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        wayland_surface_update_state_toplevel(surface);
        break;
    }

    wl_display_flush(process_wayland.wl_display);
}

static BOOL is_managed(HWND hwnd)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    BOOL ret = data && data->managed;
    if (data) wayland_win_data_release(data);
    return ret;
}

static HWND *build_hwnd_list(void)
{
    NTSTATUS status;
    HWND *list;
    ULONG count = 128;

    for (;;)
    {
        if (!(list = malloc(count * sizeof(*list)))) return NULL;
        status = NtUserBuildHwndList(0, 0, 0, 0, 0, count, list, &count);
        if (!status) return list;
        free(list);
        if (status != STATUS_BUFFER_TOO_SMALL) return NULL;
    }
}

static BOOL has_owned_popups(HWND hwnd)
{
    HWND *list;
    UINT i;
    BOOL ret = FALSE;

    if (!(list = build_hwnd_list())) return FALSE;

    for (i = 0; list[i] != HWND_BOTTOM; i++)
    {
        if (list[i] == hwnd) break;  /* popups are always above owner */
        if (NtUserGetWindowRelative(list[i], GW_OWNER) != hwnd) continue;
        if ((ret = is_managed(list[i]))) break;
    }

    free(list);
    return ret;
}

BOOL wayland_is_popup_menu_class(HWND hwnd)
{
    static const WCHAR popup_menu_class[] = {'#','3','2','7','6','8',0};
    WCHAR buffer[16];
    UNICODE_STRING name;
    INT len;

    name.Buffer = buffer;
    name.MaximumLength = sizeof(buffer);
    if (!(len = NtUserGetClassName(hwnd, FALSE, &name))) return FALSE;
    return len == ARRAY_SIZE(popup_menu_class) - 1 &&
           !memcmp(name.Buffer, popup_menu_class, len * sizeof(WCHAR));
}

static HWND get_layer_menu_hwnd(void)
{
    return NtUserGetProp(NtUserGetDesktopWindow(), layer_menu_hwnd_prop);
}

static BOOL same_window_process(HWND hwnd, HWND other)
{
    DWORD pid = 0, other_pid = 0;

    if (!hwnd || !other) return FALSE;
    NtUserGetWindowThread(hwnd, &pid);
    NtUserGetWindowThread(other, &other_pid);
    return pid && pid == other_pid;
}

void wayland_set_layer_menu_hwnd(HWND hwnd)
{
    HWND desktop = NtUserGetDesktopWindow();
    HWND foreground = NtUserGetForegroundWindow();

    /* Updating the layer geometry must not replace the pre-menu foreground. */
    if (NtUserGetProp(desktop, layer_menu_hwnd_prop) == hwnd) return;

    NtUserSetProp(desktop, layer_menu_hwnd_prop, hwnd);
    if (foreground && foreground != hwnd && NtUserIsWindow(foreground) &&
        same_window_process(foreground, hwnd))
        NtUserSetProp(desktop, layer_menu_restore_hwnd_prop, foreground);
    else
        NtUserRemoveProp(desktop, layer_menu_restore_hwnd_prop);
}

void wayland_clear_layer_menu_hwnd(HWND hwnd)
{
    HWND desktop = NtUserGetDesktopWindow();

    if (!hwnd || NtUserGetProp(desktop, layer_menu_hwnd_prop) == hwnd)
    {
        NtUserRemoveProp(desktop, layer_menu_hwnd_prop);
        NtUserRemoveProp(desktop, layer_menu_restore_hwnd_prop);
    }
}

BOOL wayland_is_layer_menu_hwnd(HWND hwnd)
{
    return hwnd && get_layer_menu_hwnd() == hwnd;
}

static HWND get_layer_menu_restore_hwnd(HWND menu)
{
    HWND desktop = NtUserGetDesktopWindow();
    HWND restore;

    if (NtUserGetProp(desktop, layer_menu_hwnd_prop) != menu) return NULL;
    restore = NtUserGetProp(desktop, layer_menu_restore_hwnd_prop);
    return restore && NtUserIsWindow(restore) ? restore : desktop;
}

void wayland_cancel_layer_menu(HWND hwnd)
{
    HWND menu = hwnd ? hwnd : get_layer_menu_hwnd();
    HWND restore;

    if (!menu) return;
    restore = get_layer_menu_restore_hwnd(menu);
    TRACE("dismissing layer menu %p restore %p\n", menu, restore);
    NtUserPostMessage(menu, WM_WAYLAND_SET_FOREGROUND, TRUE, (LPARAM)restore);
}

void wayland_cancel_layer_menu_if_needed(HWND hwnd)
{
    HWND menu = get_layer_menu_hwnd();

    if (!menu) return;
    if (hwnd && (hwnd == menu || NtUserGetAncestor(hwnd, GA_ROOT) == menu ||
                 NtUserGetAncestor(hwnd, GA_ROOTOWNER) == menu))
        return;

    wayland_cancel_layer_menu(menu);
}

static inline HWND get_active_window(void)
{
    GUITHREADINFO info;
    info.cbSize = sizeof(info);
    return NtUserGetGUIThreadInfo(GetCurrentThreadId(), &info) ? info.hwndActive : 0;
}

/***********************************************************************
 *		is_window_managed
 *
 * Check if a given window should be managed
 */
/* Owned, caption-less popups (menus, dropdowns, tooltips) become xdg_popups
 * anchored to their owner. A Wayland toplevel has no client-set position. */
/* Reject owner cycles that can briefly appear during transition states. */
static BOOL has_owner_cycle(HWND hwnd, struct wayland_surface *owner)
{
    struct wayland_win_data *grandparent_data;
    struct wayland_surface *grandparent;

    if (!wayland_surface_is_popup(owner)) return FALSE;
    if (owner->owner_hwnd == hwnd) return TRUE;

    if (!(grandparent_data = wayland_win_data_get_nolock(owner->owner_hwnd))) return FALSE;
    if (!(grandparent = grandparent_data->wayland_surface)) return FALSE;
    return has_owner_cycle(hwnd, grandparent);
}

/* Same for a subsurface's parent (toplevel_hwnd) chain. */
static BOOL has_parent_cycle(HWND hwnd, struct wayland_surface *parent)
{
    struct wayland_win_data *grandparent_data;
    struct wayland_surface *grandparent;

    if (parent->role != WAYLAND_SURFACE_ROLE_SUBSURFACE) return FALSE;
    if (parent->toplevel_hwnd == hwnd) return TRUE;

    if (!(grandparent_data = wayland_win_data_get_nolock(parent->toplevel_hwnd))) return FALSE;
    if (!(grandparent = grandparent_data->wayland_surface)) return FALSE;
    return has_parent_cycle(hwnd, grandparent);
}

/* Caption-less, sysmenu-less popups are menu-like transient windows. When they
 * have an owner, force them unmanaged so they become xdg_popups, not
 * compositor-positioned toplevels. */
BOOL wayland_is_menu_popup_candidate(HWND hwnd)
{
    DWORD style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    DWORD exstyle = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);

    return is_menu_popup_candidate_style(style, exstyle);
}

static HWND get_menu_popup_owner(HWND hwnd, HWND owner_hint)
{
    HWND owner = NtUserGetWindowRelative(hwnd, GW_OWNER);

    if (!owner) owner = owner_hint;
    if (!owner || owner == hwnd || owner == NtUserGetDesktopWindow() ||
        !NtUserIsWindow(owner))
        return NULL;

    owner = NtUserGetAncestor(owner, GA_ROOT);
    if (!owner || owner == hwnd || owner == NtUserGetDesktopWindow() ||
        !NtUserIsWindow(owner))
        return NULL;
    if ((DWORD)(ULONG_PTR)NtUserQueryWindow(owner, WindowProcess) != GetCurrentProcessId())
        return NULL;

    return owner;
}

BOOL wayland_is_menu_popup(HWND hwnd)
{
    if (!wayland_is_menu_popup_candidate(hwnd)) return FALSE;
    return get_menu_popup_owner(hwnd, NULL) != NULL;
}

static BOOL is_window_managed(HWND hwnd, HWND menu_popup_owner, UINT swp_flags, BOOL fullscreen)
{
    DWORD style, ex_style;

    /* child windows are not managed */
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style & (WS_CHILD|WS_POPUP)) == WS_CHILD) return FALSE;
    if (wayland_is_menu_popup_candidate(hwnd) && menu_popup_owner) return FALSE;
    /* activated windows are managed */
    if (!(swp_flags & (SWP_NOACTIVATE|SWP_HIDEWINDOW))) return TRUE;
    if (hwnd == get_active_window()) return TRUE;
    /* windows with caption are managed */
    if ((style & WS_CAPTION) == WS_CAPTION) return TRUE;
    /* windows with thick frame are managed */
    if (style & WS_THICKFRAME) return TRUE;
    if (style & WS_POPUP)
    {
        /* popup with sysmenu == caption are managed */
        if (style & WS_SYSMENU) return TRUE;
        /* full-screen popup windows are managed */
        if (fullscreen) return TRUE;
    }
    /* application windows are managed */
    ex_style = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    /* windows that own popups are managed */
    if (has_owned_popups(hwnd)) return TRUE;
    /* default: not managed */
    return FALSE;
}

static BOOL is_notification_window(HWND hwnd, UINT swp_flags,
                                   const struct window_rects *rects)
{
    const DWORD required_ex_style = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    struct wayland_output *output;
    const struct wayland_output_state *state;
    int width, height, output_width, output_height;
    RECT rect = rects->window, output_rect;
    DWORD style, ex_style;
    BOOL at_edge;

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    ex_style = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);

    if ((swp_flags & SWP_HIDEWINDOW) || !(style & WS_VISIBLE)) return FALSE;
    if (!(style & WS_POPUP) || (style & (WS_CHILD | WS_CAPTION | WS_THICKFRAME))) return FALSE;
    if ((ex_style & required_ex_style) != required_ex_style || (ex_style & WS_EX_APPWINDOW))
        return FALSE;
    if (wayland_is_popup_menu_class(hwnd)) return FALSE;

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return FALSE;

    if (!(output = wayland_output_for_rect(&rect))) return FALSE;
    state = &output->current;
    if (!state->current_mode)
    {
        wayland_output_release(output);
        return FALSE;
    }

    SetRect(&output_rect, state->physical_x, state->physical_y,
            state->physical_x + state->current_mode->width,
            state->physical_y + state->current_mode->height);
    output_width = output_rect.right - output_rect.left;
    output_height = output_rect.bottom - output_rect.top;

    /* Desktop notification windows are small, borderless surfaces deliberately
     * placed against an output edge. This excludes centered overlays, splash
     * screens, and ordinary layered game windows with the same extended styles. */
    at_edge = abs(rect.left - output_rect.left) <= 64 ||
              abs(rect.right - output_rect.right) <= 64 ||
              abs(rect.top - output_rect.top) <= 64 ||
              abs(rect.bottom - output_rect.bottom) <= 64;
    wayland_output_release(output);

    return at_edge && width <= output_width / 2 && height <= output_height / 2;
}

/***********************************************************************
 *           WAYLAND_DestroyWindow
 */
void WAYLAND_DestroyWindow(HWND hwnd)
{
    struct wayland_win_data *data;

    TRACE("%p\n", hwnd);

    if (!(data = wayland_win_data_get(hwnd))) return;
    wayland_win_data_destroy(data);
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
BOOL WAYLAND_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);

    TRACE("hwnd %p, swp_flags %04x, shaped %u, rects %s\n", hwnd, swp_flags, shaped, debugstr_window_rects(rects));

    if (!data && !(data = wayland_win_data_create(hwnd, rects))) return FALSE;

    wayland_win_data_release(data);

    return TRUE;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanged
 */
void WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface)
{
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT), owner = NULL, menu_popup_owner = NULL;
    HWND backing_parent, ancestor;
    struct wayland_surface *toplevel_surface = NULL, *owner_surface = NULL;
    struct wayland_win_data *data, *toplevel_data, *owner_data;
    BOOL managed, fullscreen = swp_flags & WINE_SWP_FULLSCREEN;
    BOOL tray_menu = swp_flags & WINE_SWP_TRAY_MENU;
    BOOL notification = FALSE;
    BOOL use_layer_shell = FALSE;

    /* A hit-test owner hint may be one of our own dialogs. Do not invert
     * the Win32 owner chain by turning its owner into an xdg_popup above it. */
    for (ancestor = owner_hint; ancestor; ancestor = NtUserGetWindowRelative(ancestor, GW_OWNER))
    {
        if (ancestor != hwnd) continue;
        owner_hint = NULL;
        break;
    }

    backing_parent = get_fullscreen_backing_parent(hwnd, insert_after, fullscreen);

    /* Get the managed state with win_data unlocked, as is_window_managed
     * may need to query win_data information about other HWNDs and thus
     * acquire the lock itself internally. */
    menu_popup_owner = wayland_is_menu_popup_candidate(hwnd) ? get_menu_popup_owner(hwnd, owner_hint) : NULL;
    managed = is_window_managed(hwnd, menu_popup_owner, swp_flags, fullscreen);
    if (!tray_menu && surface && process_wayland.zwlr_layer_shell_v1)
        notification = is_notification_window(hwnd, swp_flags, new_rects);
    if (backing_parent)
    {
        managed = FALSE;
        toplevel = backing_parent;
        owner = NULL;
    }
    else if ((tray_menu || notification) && surface && process_wayland.zwlr_layer_shell_v1)
    {
        managed = FALSE;
        toplevel = NULL;
        owner = NULL;
        use_layer_shell = TRUE;
    }
    else if (!managed && surface)
    {
        toplevel = NULL;
        owner = menu_popup_owner ? menu_popup_owner : owner_hint;
    }

    TRACE("hwnd %p toplevel %p owner %p owner_hint %p menu_owner %p new_rects %s after %p flags %08x\n",
          hwnd, toplevel, owner, owner_hint, menu_popup_owner, debugstr_window_rects(new_rects),
          insert_after, swp_flags);

    if (!(data = wayland_win_data_get(hwnd))) return;
    toplevel_data = toplevel && toplevel != hwnd ? wayland_win_data_get_nolock(toplevel) : NULL;
    toplevel_surface = toplevel_data ? toplevel_data->wayland_surface : NULL;
    owner_data = owner && owner != hwnd ? wayland_win_data_get_nolock(owner) : NULL;
    owner_surface = owner_data ? owner_data->wayland_surface : NULL;
    /* for it to be a popup, we need a valid xdg surface.
     * Demote to subsurface instead if this condition is not met. */
    if (owner_surface && !owner_surface->xdg_surface)
    {
        /* if the window is unmanaged and the owner
         * is not visible, we cannot use a subsurface. */
        if (owner_surface->role != WAYLAND_SURFACE_ROLE_NONE)
            toplevel_surface = owner_surface;
        owner_surface = NULL;
    }
    if (owner_surface) use_layer_shell = FALSE;
    /* Cycles can occur during some transition states.
     * They can be corrected the next time their position updates (after the toplevel gets its role)
     * otherwise these windows will remain as toplevels. */
    if (owner_surface && has_owner_cycle(hwnd, owner_surface))
    {
        ERR("hwnd=%p owner=%p forms a cycle!\n", hwnd, owner);
        owner_surface = NULL;
    }
    if (toplevel_surface && has_parent_cycle(hwnd, toplevel_surface))
    {
        ERR("hwnd=%p parent=%p forms a cycle!\n", hwnd, toplevel);
        toplevel_surface = NULL;
    }

    data->rects = *new_rects;
    data->is_fullscreen = fullscreen;
    data->managed = managed;

    if (!surface)
    {
        if (toplevel && NtUserIsWindowVisible(hwnd) && !window_or_root_minimized(hwnd))
            wayland_client_surfaces_attach(data, toplevel);
        else
            wayland_client_surfaces_attach(data, NULL);
        if (data->wayland_surface)
        {
            wayland_surface_destroy(data->wayland_surface);
            data->wayland_surface = NULL;
        }
    }
    else if (wayland_win_data_create_wayland_surface(data, toplevel_surface, owner_surface,
                                                     use_layer_shell, tray_menu, surface, swp_flags,
                                                     menu_popup_owner != NULL,
                                                     backing_parent != NULL))
    {
        wayland_win_data_update_wayland_state(data);
    }

    if (owner_data) wayland_win_data_release(owner_data);
    wayland_win_data_release(data);
}

static void wayland_configure_window(HWND hwnd)
{
    struct wayland_surface *surface;
    INT width, height, window_width, window_height;
    UINT flags = 0;
    UINT state_cmd = 0;
    UINT resize_edge;
    uint32_t state;
    DWORD style;
    BOOL needs_enter_size_move = FALSE;
    BOOL needs_exit_size_move = FALSE;
    struct wayland_win_data *data;
    RECT rect;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    if (!surface->xdg_surface)
    {
        TRACE("missing xdg_surface, returning\n");
        wayland_win_data_release(data);
        return;
    }

    if (!surface->requested.serial)
    {
        TRACE("hwnd=%p requested configure event already handled, returning\n", hwnd);
        wayland_win_data_release(data);
        return;
    }

    surface->processing = surface->requested;
    memset(&surface->requested, 0, sizeof(surface->requested));

    state = surface->processing.state;
    /* Ignore size hints if we don't have a state that requires strict
     * size adherence, in order to avoid spurious resizes.
     * The tiled and maximized states have a strict size adherance, so
     * their sizes cannot change while we are still processing the new config.
     * This also preserves the final size on leaving an interactive resize or
     * transitioning from maximized/tiled to a stateless regular window. */
    if ((state & ~WAYLAND_SURFACE_CONFIG_STATE_SUSPENDED) || surface->resizing ||
        (surface->current.state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                                   WAYLAND_SURFACE_CONFIG_STATE_TILED)))
    {
        width = surface->processing.rect.right - surface->processing.rect.left;
        height = surface->processing.rect.bottom - surface->processing.rect.top;
    }
    else
    {
        width = height = 0;
    }

    if ((state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && !surface->resizing)
    {
        surface->resizing = TRUE;
        needs_enter_size_move = TRUE;
    }

    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && surface->resizing)
    {
        surface->resizing = FALSE;
        needs_exit_size_move = TRUE;
    }
    resize_edge = surface->resize_edge;
    if (needs_exit_size_move) surface->resize_edge = 0;

    /* Only Win32 maximize and decoration transitions change non-client metrics.
     * Fullscreen is a Wayland presentation state, not a Win32 window style. */
    if (((state ^ surface->current.state) & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) ||
        surface->processing.decor != surface->current.decor)
    {
        flags |= SWP_FRAMECHANGED;
    }

    /* If the window is already fullscreen and its size is compatible with what
     * the compositor is requesting, don't force a resize, since some applications
     * are very insistent on a particular fullscreen size (which may not match
     * the monitor size). */
    if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        wayland_surface_config_is_compatible(&surface->processing, surface->window.rect,
                                             surface->window.state))
    {
        flags |= SWP_NOSIZE;
    }

    window_width = width;
    window_height = height;

    flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE;
    if (window_width == 0 || window_height == 0) flags |= SWP_NOSIZE;
    SetRect(&rect, 0, 0, window_width, window_height);
    OffsetRect(&rect, data->rects.window.left, data->rects.window.top);
    if (!IsRectEmpty(&rect)) rect = window_rect_from_visible(&data->rects, rect);
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    /* Compositors may report fixed-size windows as maximized without enlarging
     * them. Apply the configured geometry directly: SC_MAXIMIZE would instead
     * recalculate a monitor-sized rectangle and spuriously resize the window. */
    if ((style & WS_THICKFRAME) && !(style & WS_MINIMIZE) &&
        !(state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        if ((state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) && !(style & WS_MAXIMIZE))
            state_cmd = SC_MAXIMIZE;
        else if (!(state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                            WAYLAND_SURFACE_CONFIG_STATE_TILED)) &&
                 (style & WS_MAXIMIZE))
            state_cmd = SC_RESTORE;
    }
    if (state_cmd) wayland_win_data_queue_state_update(data, state_cmd, &rect);

    wayland_win_data_release(data);

    TRACE("hwnd=%p processing=%s,%#x\n", hwnd, wine_dbgstr_rect(&rect), state);

    if (needs_enter_size_move) send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);

    if (state_cmd)
    {
        if (needs_exit_size_move) send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);
        TRACE("hwnd=%p queueing state update %#x rect=%s\n", hwnd, state_cmd,
              wine_dbgstr_rect(&rect));
        NtUserPostMessage(hwnd, WM_WINE_WINDOW_STATE_CHANGED, 0, 0);
        return;
    }

    flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE;
    if (IsRectEmpty(&rect)) flags |= SWP_NOSIZE;

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) != !(style & WS_MAXIMIZE)
        && !(state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        NtUserSetWindowLong(hwnd, GWL_STYLE, style ^ WS_MAXIMIZE, FALSE);

    /* The Wayland maximized and fullscreen states are very strict about
     * surface size, so don't let the application override it. The tiled state
     * is not as strict, but it indicates a strong size preference, so try to
     * respect it. */
    if (state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                 WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                 WAYLAND_SURFACE_CONFIG_STATE_TILED))
    {
        flags |= SWP_NOSENDCHANGING;
    }

    /* Mark pending configures processed before rawpos can flush. */
    if ((data = wayland_win_data_get(hwnd)))
    {
        surface = data->wayland_surface;
        if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            surface->xdg_surface && surface->processing.serial &&
            !surface->processing.processed)
            wayland_win_data_update_wayland_state(data);
        wayland_win_data_release(data);
    }

    if (((state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) || needs_exit_size_move) &&
        !(flags & (SWP_NOSIZE | SWP_NOSENDCHANGING)))
    {
        RECT proposed = rect;
        NtUserSendSizingMessage(hwnd, resize_edge, &rect);
        if (rect.left != proposed.left || rect.top != proposed.top) flags &= ~SWP_NOMOVE;
    }

    NtUserSetRawWindowPos(hwnd, rect, flags, FALSE);
    if (needs_exit_size_move) send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);

    /* Ack/promote the processed configure if rawpos did not flush it. */
    if ((data = wayland_win_data_get(hwnd)))
    {
        surface = data->wayland_surface;
        if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            surface->xdg_surface && surface->processing.serial &&
            surface->processing.processed)
        {
            wayland_win_data_release(data);
            /* Preserve flush lock order: surface before win_data. */
            ensure_window_surface_contents(hwnd);
        }
        else wayland_win_data_release(data);
    }
}

/**********************************************************************
 *           WAYLAND_WindowMessage
 */
LRESULT WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_WAYLAND_INIT_DISPLAY_DEVICES:
        NtUserCallNoParam(NtUserCallNoParam_DisplayModeChanged);
        return 0;
    case WM_WAYLAND_CONFIGURE:
        wayland_configure_window(hwnd);
        return 0;
    case WM_WAYLAND_RECALC_CLIENT_RECT:
    {
        struct wayland_win_data *data;
        struct wayland_surface *surface;
        RECT window, expected, client;
        static const RECT empty_rect;
        DWORD style;
        UINT dpi;
        BOOL fullscreen = FALSE;

        if ((data = wayland_win_data_get(hwnd)))
        {
            surface = data->wayland_surface;
            fullscreen = surface && wayland_surface_is_toplevel(surface) &&
                         (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN);
            wayland_win_data_release(data);
        }
        if (!fullscreen) return 0;

        /* This repair is for borderless fullscreen windows. In particular,
         * do not send a synthetic non-client transition to ordinary
         * maximized launchers. */
        style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
        if (style & (WS_MINIMIZE | WS_CAPTION | WS_THICKFRAME)) return 0;

        /* Use live Win32 geometry rather than the cached Wayland window rect:
         * the latter is populated from the same transition whose stale
         * non-client metrics this message is repairing. */
        dpi = NtUserGetDpiForWindow(hwnd);
        if (!NtUserGetWindowRect(hwnd, &window, dpi) ||
            !NtUserGetClientRect(hwnd, &client, dpi))
            return 0;
        SetRect(&expected, 0, 0, window.right - window.left,
                window.bottom - window.top);
        if (IsRectEmpty(&expected)) return 0;
        if (EqualRect(&client, &expected)) return 0;

        /* The posted message may outlive the configure that queued it. Do not
         * repair a window that left fullscreen while it was pending. */
        if (!(data = wayland_win_data_get(hwnd))) return 0;
        surface = data->wayland_surface;
        fullscreen = surface && wayland_surface_is_toplevel(surface) &&
                     (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN);
        wayland_win_data_release(data);
        if (!fullscreen) return 0;

        TRACE("hwnd=%p client %s -> %s\n", hwnd, wine_dbgstr_rect(&client),
              wine_dbgstr_rect(&expected));
        NtUserSetRawWindowPos(hwnd, empty_rect, SWP_NOSIZE | SWP_NOMOVE |
                              SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                              SWP_FRAMECHANGED | SWP_NOSENDCHANGING, FALSE);
        return 0;
    }
    case WM_WAYLAND_SET_FOREGROUND:
    {
        HWND focused, restore;

        pthread_mutex_lock(&process_wayland.keyboard.mutex);
        focused = process_wayland.keyboard.focused_hwnd;
        pthread_mutex_unlock(&process_wayland.keyboard.mutex);

        if (!wayland_is_layer_menu_hwnd(hwnd)) return 0;

        if (wp)
        {
            /* On host focus loss, do not reactivate the menu's owner over the
             * application the user just selected. Only restore it when we
             * still have keyboard focus (e.g. Escape). */
            restore = focused == hwnd ? (HWND)lp : focused;
            if (!restore || !NtUserIsWindow(restore)) restore = NtUserGetDesktopWindow();
            wayland_clear_layer_menu_hwnd(hwnd);
            if (NtUserGetForegroundWindow() == hwnd)
                NtUserSetForegroundWindowInternal(restore);
            send_message(hwnd, WM_CANCELMODE, 0, 0);
            if (wayland_is_popup_menu_class(hwnd)) NtUserEndMenu();
        }
        else if (focused == hwnd)
        {
            if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE)
                NtUserPostMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            NtUserSetForegroundWindowInternal(hwnd);
        }
        return 0;
    }
    case WM_WINE_MAP_NOTIFY_ICON_POINT:
        WAYLAND_MapNotifyIconPoint((POINT *)lp);
        return 0;
    case WM_WAYLAND_DMABUF_FRAME:
    {
        BOOL had_dmabuf_content = FALSE;
        struct wayland_win_data *data;

        if (window_surface_has_requested_configure(hwnd))
            wayland_configure_window(hwnd);

        if ((wp & HWND_DMABUF_WAKE_REANNOUNCE) &&
            (data = wayland_win_data_get(hwnd)))
        {
            if (data->wayland_surface)
                wayland_surface_reannounce_hwnd_dmabuf_consumers(data->wayland_surface);
            wayland_win_data_release(data);
        }

        if (window_surface_configure_blocks_dmabuf(hwnd))
            return 0;

        /* A producer published a frame for a descendant of this toplevel.
         * Import it in the process that owns the toplevel's wayland surface. */
        had_dmabuf_content = window_surface_has_hwnd_dmabuf_content(hwnd);
        ensure_window_surface_contents(hwnd);
        if (had_dmabuf_content != window_surface_has_hwnd_dmabuf_content(hwnd))
            NtUserPostMessage(hwnd, WM_WINE_UPDATEWINDOWSTATE, 0, 0);
        return 0;
    }
    case WM_WAYLAND_EXPOSE:
        /* Event-thread callbacks use this for exposes that must follow the
         * window-thread lock order. */
        NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
        return 0;
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, (long)wp, lp);
        return 0;
    }
}

/**********************************************************************
 *           WAYLAND_DesktopWindowProc
 */
LRESULT WAYLAND_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return NtUserMessageCall(hwnd, msg, wp, lp, 0, NtUserDefWindowProc, FALSE);
}

/*****************************************************************
 *		WAYLAND_SetLayeredWindowAttributes
 */
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("hwnd=%p key=%x alpha=%u flags=%x\n", hwnd, key, alpha, flags);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
        wayland_surface_set_opacity(surface, alpha, flags);
    data->layered_attribs_set = TRUE;

    wayland_win_data_release(data);
}

static enum xdg_toplevel_resize_edge hittest_to_resize_edge(WPARAM hittest)
{
    switch (hittest)
    {
    case WMSZ_LEFT:        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case WMSZ_RIGHT:       return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case WMSZ_TOP:         return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case WMSZ_TOPLEFT:     return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case WMSZ_TOPRIGHT:    return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case WMSZ_BOTTOM:      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case WMSZ_BOTTOMLEFT:  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case WMSZ_BOTTOMRIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    default:               return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

/*****************************************************************
 *		WAYLAND_SetWindowIcons
 */
void WAYLAND_SetWindowIcons(HWND hwnd, HICON icon, const ICONINFO *ii, HICON icon_small, const ICONINFO *ii_small)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    if (!process_wayland.xdg_toplevel_icon_manager_v1) return;

    TRACE("hwnd=%p icon=%p ii=%p icon_small=%p ii_small=%p\n", hwnd, icon, ii, icon_small, ii_small);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
    {
        wayland_surface_set_icon_buffer(surface, ICON_BIG, ii);
        if (icon_small) wayland_surface_set_icon_buffer(surface, ICON_SMALL, ii_small);
        if (wayland_surface_is_toplevel(surface)) wayland_surface_assign_icon(surface);
    }

    wayland_win_data_release(data);
}

/***********************************************************************
 *		WAYLAND_SetWindowStyle
 */
void WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    DWORD changed = style->styleNew ^ style->styleOld;

    TRACE("hwnd=%p offset=%d\n", hwnd, offset);

    if (hwnd == NtUserGetDesktopWindow()) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Changing WS_EX_LAYERED resets attributes */
    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
    {
        if ((surface = data->wayland_surface))
            wayland_surface_set_opacity(surface, 0, 0);
        data->layered_attribs_set = FALSE;
    }

    wayland_win_data_release(data);
}

/*****************************************************************
 *		WAYLAND_SetWindowText
 */
void WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p text=%s\n", hwnd, wine_dbgstr_w(text));

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
            wayland_surface_set_title(surface, text);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *          WAYLAND_SysCommand
 */
LRESULT WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos)
{
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;
    uint32_t button_serial;
    struct wl_seat *wl_seat;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("cmd=%lx hwnd=%p, %lx, %lx\n",
          (long)command, hwnd, (long)wparam, lparam);

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == hwnd)
        button_serial = process_wayland.pointer.button_serial;
    else
        button_serial = 0;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    if (command == SC_MOVE || command == SC_SIZE)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            pthread_mutex_lock(&process_wayland.seat.mutex);
            wl_seat = process_wayland.seat.wl_seat;
            if (wl_seat && (surface = data->wayland_surface) &&
                wayland_surface_is_toplevel(surface) && button_serial)
            {
                if (command == SC_MOVE)
                {
                    xdg_toplevel_move(surface->xdg_toplevel, wl_seat, button_serial);
                }
                else if (command == SC_SIZE)
                {
                    surface->resize_edge = wparam & 0x0f;
                    xdg_toplevel_resize(surface->xdg_toplevel, wl_seat, button_serial,
                                        hittest_to_resize_edge(wparam & 0x0f));
                }
            }
            pthread_mutex_unlock(&process_wayland.seat.mutex);
            wayland_win_data_release(data);
            ret = 0;
        }
    }

    wl_display_flush(process_wayland.wl_display);
    return ret;
}

/***********************************************************************
 *          WAYLAND_UpdateLayeredWindow
 */
void WAYLAND_UpdateLayeredWindow(HWND hwnd, BYTE alpha, UINT flags)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("hwnd=%p alpha=%u flags=%x\n", hwnd, alpha, flags);

    if (!(data = wayland_win_data_get(hwnd))) return;

    if ((surface = data->wayland_surface))
        wayland_surface_set_opacity(surface, alpha, flags);

    wayland_win_data_release(data);
}

/***********************************************************************
 *          WAYLAND_HasWindowManager
 */
BOOL WAYLAND_HasWindowManager(const char *name)
{
    static int once;
    const char *env = getenv("XDG_CURRENT_DESKTOP");

    if (!once++) TRACE("DE: %s\n", debugstr_a(env));

    if (!strcmp("waylanddrv", name)) return TRUE;
    if (env && !strcmp(env, name)) return TRUE;

    return FALSE;
}

/**********************************************************************
 *          WAYLAND_FlashWindowEx
 */
void WAYLAND_FlashWindowEx(FLASHWINFO *info)
{
    struct wayland_win_data *data;

    TRACE("hwnd %p flags %u\n", info->hwnd, info->dwFlags);

    if (!info->dwFlags || !(data = wayland_win_data_get(info->hwnd))) return;
    if (data->wayland_surface) wayland_surface_flash_window(data->wayland_surface);
    wayland_win_data_release(data);

    wl_display_flush(process_wayland.wl_display);
}

/***********************************************************************
 *           WAYLAND_ActivateWindow
 */
void WAYLAND_ActivateWindow(HWND hwnd, HWND previous)
{
    struct wayland_win_data *data;

    TRACE("hwnd=%p previous=%p\n", hwnd, previous);

    if (hwnd == previous) return;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (data->wayland_surface) wayland_surface_activate(data->wayland_surface);
    wayland_win_data_release(data);

    wl_display_flush(process_wayland.wl_display);
}

/***********************************************************************
 *           WAYLAND_GetWindowStyleMasks
 */
BOOL WAYLAND_GetWindowStyleMasks(HWND hwnd, UINT style, UINT ex_style,
                                 UINT *style_mask, UINT *ex_style_mask)
{
    BOOL ret = TRUE;
    struct wayland_win_data *data;
    struct wayland_surface *surface;

    TRACE("%p %x %x %p %p\n", hwnd, style, ex_style, style_mask, ex_style_mask);

    *style_mask = *ex_style_mask = 0;

    if (!process_wayland.zxdg_decoration_manager_v1) return FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
    {
        if (!data->managed) ret = FALSE;
        else if (surface->current.decor == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
            ret = FALSE;
    }

    wayland_win_data_release(data);

    if (ret && (ret = is_decoration_enabled(style, ex_style)))
    {
        *style_mask |= WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
        *ex_style_mask |= WS_EX_DLGMODALFRAME;
    }

    return ret;
}

/***********************************************************************
 *           WAYLAND_GetWindowStateUpdates
 */
BOOL WAYLAND_GetWindowStateUpdates(HWND hwnd, UINT *state_cmd, UINT *swp_flags,
                                   RECT *rect, HWND *foreground)
{
    struct wayland_surface *surface;
    struct wayland_keyboard *keyboard = &process_wayland.keyboard;
    struct wayland_win_data *data;
    DWORD style;
    HWND focused_hwnd, old_foreground;
    BOOL ret;

    /* win32u uses a second call with NULL outputs to release any host state
     * lock acquired by the first call. Wine-Wayland does not acquire one. */
    if (!state_cmd)
        return FALSE;

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    old_foreground = NtUserGetForegroundWindow();

    *state_cmd = *swp_flags = 0;
    *foreground = NULL;
    SetRectEmpty(rect);

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    *state_cmd = data->state_update_cmd;
    *swp_flags = data->state_update_swp_flags;
    *rect = data->state_update_rect;
    *foreground = data->state_update_foreground;
    ret = *state_cmd || *swp_flags || *foreground;

    data->state_update_cmd = 0;
    data->state_update_swp_flags = 0;
    SetRectEmpty(&data->state_update_rect);
    data->state_update_foreground = NULL;

    wayland_win_data_release(data);

    /* in these cases we dont need to update the window focus, borrowed from winemac */
    if (!(style & WS_VISIBLE)) return ret;
    if ((style & (WS_POPUP | WS_CHILD)) == WS_CHILD) return ret;
    if (style & WS_DISABLED) return ret;

    pthread_mutex_lock(&keyboard->mutex);
    focused_hwnd = keyboard->focused_hwnd;
    pthread_mutex_unlock(&keyboard->mutex);

    /* if the foreground window is not the hwnd then this is a stale focus loss */
    if (!focused_hwnd && old_foreground == hwnd)
        focused_hwnd = NtUserGetDesktopWindow();
    else if (focused_hwnd != hwnd) focused_hwnd = NULL;

    if (!*foreground && old_foreground != focused_hwnd) *foreground = focused_hwnd;

    /* we can't track if the host window is minimized or unminimized, but
     * if we have keyboard focus on this window we can treat it as restored. */
    if (!*state_cmd && (style & WS_MINIMIZE) && focused_hwnd == hwnd && old_foreground != hwnd)
        *state_cmd = MAKELONG(SC_RESTORE, 1);
    else if (!*state_cmd && !*swp_flags && (data = wayland_win_data_get(hwnd)))
    {
        struct surface_output_entry *output_entry;

        if (!(surface = data->wayland_surface)) goto skip;
        if (!wayland_surface_is_toplevel(surface)) goto skip;
        if (wl_list_empty(&surface->output_list)) goto skip;
        /* the output hint syncs the win32u position to the toplevel position */
        if (surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) goto skip;

        *swp_flags = SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_NOACTIVATE;
        SetRect(rect, 0, 0, 1, 1);

        output_entry = wl_container_of(surface->output_list.next, output_entry, entry);
        OffsetRect(rect, output_entry->output->current.physical_x, output_entry->output->current.physical_y);

        TRACE("Moving hwnd=%p to %s\n", hwnd, wine_dbgstr_rect(rect));

    skip:
        wayland_win_data_release(data);
    }

    TRACE("hwnd=%p foreground=%p state_cmd=%#x swp_flags=%#x rect=%s\n",
          hwnd, *foreground, *state_cmd, *swp_flags, wine_dbgstr_rect(rect));

    return TRUE;
}

BOOL WAYLAND_GetWindowMaxTrackSize(HWND hwnd, SIZE *size)
{
    struct wayland_win_data *data;
    RECT rect;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    /* The cached Wayland config is refreshed from WindowPosChanged, but this
     * query runs during calc_winpos. Recheck the live style so a borderless
     * fullscreen transition cannot be clamped by stale configure bounds. */
    if ((NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_THICKFRAME) && data->wayland_surface)
        ret = wayland_surface_get_max_track_size(data->wayland_surface, size);

    if (ret)
    {
        /* xdg-shell constrains visible geometry, not the hidden Win32 frame. */
        SetRect(&rect, 0, 0, size->cx, size->cy);
        rect = window_rect_from_visible(&data->rects, rect);
        size->cx = rect.right - rect.left;
        size->cy = rect.bottom - rect.top;
    }

    wayland_win_data_release(data);
    return ret;
}

/* True when the client surface occludes this window's GDI buffer. A detached
 * or reparented client does not count. */
static BOOL window_client_surface_attached(struct wayland_win_data *data)
{
    return data->client_surface && data->client_surface->wl_subsurface &&
           data->client_surface->toplevel == data->hwnd;
}

static BOOL window_client_surface_pending_first_frame(struct wayland_win_data *data)
{
    return data->client_surface && !data->client_surface->has_presented;
}

static BOOL window_surface_has_requested_configure(HWND hwnd)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    surface = data->wayland_surface;
    if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
        surface->xdg_surface && surface->requested.serial)
        ret = TRUE;
    wayland_win_data_release(data);

    return ret;
}

static BOOL window_surface_configure_blocks_dmabuf(HWND hwnd)
{
    struct wayland_win_data *data;
    struct wayland_surface *surface;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    surface = data->wayland_surface;
    if (surface && surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL && surface->xdg_surface)
    {
        /* Keep producer frames flowing while a maximize/restore SysCommand is
         * pending. Only an actual unconfigured parent blocks dmabuf commits. */
        ret = surface->requested.serial ||
              (surface->processing.serial && !surface->processing.processed &&
               !surface->current.serial);
    }
    wayland_win_data_release(data);

    return ret;
}

static BOOL window_surface_has_hwnd_dmabuf_content(HWND hwnd)
{
    struct wayland_win_data *data;
    BOOL ret = FALSE;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    ret = data->wayland_surface && wayland_surface_has_hwnd_dmabuf_content(data->wayland_surface);
    wayland_win_data_release(data);

    return ret;
}

BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer,
                                 HRGN damage_region, BOOL content_over_producer)
{
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    BOOL committed = FALSE;
    BOOL content_over_changed;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    content_over_changed = data->content_over_producer != content_over_producer;
    data->content_over_producer = content_over_producer;

    if ((wayland_surface = data->wayland_surface))
    {
        if (wayland_surface->role == WAYLAND_SURFACE_ROLE_TOPLEVEL &&
            wayland_surface->window.minimized)
        {
            wayland_win_data_release(data);
            return TRUE;
        }

        if (wayland_surface_reconfigure(wayland_surface))
        {
            /* sync the alpha multiplier if it has changed due to SLWA/ULW */
            if (data->alpha_multiplier != wayland_surface->alpha_multiplier)
            {
                wayland_surface->alpha_multiplier = data->alpha_multiplier;
                wayland_surface_sync_alpha(wayland_surface);
            }

            if (shm_buffer)
            {
                wayland_surface_prepare_direct_dmabuf_shm_commit(wayland_surface);
                wl_surface_set_opaque_region(wayland_surface->wl_surface, NULL);
                wayland_surface_attach_shm(wayland_surface, shm_buffer, damage_region);
                wl_surface_commit(wayland_surface->wl_surface);
                wayland_surface_finish_direct_dmabuf_shm_commit(wayland_surface);
                wayland_surface->ensured_contents = WAYLAND_SURFACE_ENSURED_FLUSH;
                wayland_surface_update_hwnd_dmabufs(wayland_surface);
                committed = TRUE;
            }
            else if (data->client_surface && window_client_surface_attached(data) &&
                     !window_client_surface_pending_first_frame(data))
                committed = TRUE;
            else
                committed = wayland_surface_attach_transparent_carrier(wayland_surface);
        }
        else
        {
            TRACE("Wayland surface not configured yet, not flushing\n");
        }
    }

    /* Update the latest window buffer for the wayland surface. Note that we
     * only care whether the buffer contains the latest window contents,
     * it's irrelevant if it was actually committed or not. */
    if (data->window_contents)
        wayland_shm_buffer_unref(data->window_contents);
    data->window_contents = NULL;
    if (shm_buffer)
    {
        wayland_shm_buffer_ref(shm_buffer);
        data->window_contents = shm_buffer;
    }

    /* Reclassify the client whenever GDI contents appear or disappear. The
     * client attachment selects the matching opaque or transparent carrier. */
    if (committed && data->client_surface &&
        !window_client_surface_pending_first_frame(data))
    {
        if (window_client_surface_attached(data))
        {
            if (!shm_buffer || content_over_changed ||
                (content_over_producer && data->client_surface->stack_above_parent))
                wayland_client_surface_attach(data->client_surface,
                                              data->client_surface->toplevel);
        }
        else if (shm_buffer)
            wayland_client_surface_attach(data->client_surface, NULL);
    }

    wayland_win_data_release(data);

    return committed;
}

struct wayland_shm_buffer *get_window_surface_contents(HWND hwnd)
{
    struct wayland_shm_buffer *shm_buffer;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return NULL;
    if ((shm_buffer = data->window_contents)) wayland_shm_buffer_ref(shm_buffer);
    wayland_win_data_release(data);

    return shm_buffer;
}

void wayland_window_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&win_data_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}

void ensure_window_surface_contents(HWND hwnd)
{
    BOOL has_dmabuf_content = FALSE, expose = FALSE;
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    uint32_t processing_serial;

    if (!(data = wayland_win_data_get(hwnd)))
    {
        TRACE("hwnd=%p no wayland data for ensure contents\n", hwnd);
        return;
    }

    TRACE("hwnd=%p wayland_surface=%p\n", hwnd, data->wayland_surface);

    if ((wayland_surface = data->wayland_surface))
    {
        wayland_surface_ensure_contents(wayland_surface, data->client_surface);
        has_dmabuf_content = wayland_surface_has_hwnd_dmabuf_content(wayland_surface);

        if (wayland_surface->window.visible)
        {
            /* Toplevel configure state is independent of its pixel source. */
            processing_serial = wayland_surface->processing.serial;
            if (processing_serial && wayland_surface->processing.processed &&
                wayland_surface_reconfigure(wayland_surface))
            {
                /* A client request may have superseded the compositor state
                 * while its configure was being processed. Re-evaluate it
                 * immediately after promoting that configure. */
                if (wayland_surface->processing.serial != processing_serial)
                    wayland_win_data_update_wayland_state(data);
                wl_surface_commit(wayland_surface->wl_surface);
            }

            /* Producer content already visible: do not create a fallback
             * window surface that could replace its carrier with default pixels. */
            if (!data->window_contents && !has_dmabuf_content) expose = TRUE;
        }

        /* Flush queued commits now: the dmabuf present path has no other flush
         * point. They would otherwise sit in the output buffer until the event
         * thread next wakes, throttling presentation to that rate. */
        wl_display_flush(process_wayland.wl_display);
    }

    wayland_win_data_release(data);

    if (expose) NtUserExposeWindowSurface(hwnd, 0, NULL, 0);
}
