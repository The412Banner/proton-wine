/*
 * KWin output HDR state
 *
 * Copyright 2026 GloriousEggroll
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
#include "waylanddrv.h"
#include "kde-output-device-v2-client-protocol.h"
#include "wine/debug.h"

#include <stdlib.h>
#include <string.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

struct kde_output_registry
{
    struct list entry;
    struct kde_output_device_registry_v2 *proxy;
    uint32_t global_id;
    BOOL stopped;
};

struct kde_output
{
    struct list entry, modes;
    struct kde_output_device_v2 *proxy;
    struct kde_output_registry *registry;
    uint32_t global_id;
    char *name, *pending_name;
    BOOL ready, hdr, have_hdr, pending_hdr, pending_enabled;
    uint32_t pending_capabilities;
};

struct kde_output_mode
{
    struct list entry;
    struct kde_output_device_mode_v2 *proxy;
};

static struct list registries = LIST_INIT(registries);
/* Membership and committed state are protected by output_mutex. Pending state
 * and protocol object lifetimes are managed only by the Wayland event queue. */
static struct list outputs = LIST_INIT(outputs);

static void output_proxy_destroy(struct kde_output_device_v2 *proxy)
{
    if (kde_output_device_v2_get_version(proxy) >= KDE_OUTPUT_DEVICE_V2_RELEASE_SINCE_VERSION)
        kde_output_device_v2_release(proxy);
    else
        kde_output_device_v2_destroy(proxy);
}

static void mode_destroy(struct kde_output_mode *mode)
{
    /* We bind at most version 21; modes have no release request before v26. */
    kde_output_device_mode_v2_destroy(mode->proxy);
    list_remove(&mode->entry);
    free(mode);
}

/* Called with output_mutex held. */
static void output_destroy(struct kde_output *output)
{
    struct kde_output_mode *mode, *next;

    LIST_FOR_EACH_ENTRY_SAFE(mode, next, &output->modes, struct kde_output_mode, entry)
        mode_destroy(mode);
    output_proxy_destroy(output->proxy);
    list_remove(&output->entry);
    free(output->name);
    free(output->pending_name);
    free(output);
}

static void mode_handle_size(void *data, struct kde_output_device_mode_v2 *proxy,
                             int32_t width, int32_t height)
{
}

static void mode_handle_refresh(void *data, struct kde_output_device_mode_v2 *proxy, int32_t refresh)
{
}

static void mode_handle_preferred(void *data, struct kde_output_device_mode_v2 *proxy)
{
}

static void mode_handle_removed(void *data, struct kde_output_device_mode_v2 *proxy)
{
    mode_destroy(data);
}

static void mode_handle_flags(void *data, struct kde_output_device_mode_v2 *proxy, uint32_t flags)
{
}

static const struct kde_output_device_mode_v2_listener mode_listener =
{
    .size = mode_handle_size,
    .refresh = mode_handle_refresh,
    .preferred = mode_handle_preferred,
    .removed = mode_handle_removed,
    .flags = mode_handle_flags,
};

static void output_handle_mode(void *data, struct kde_output_device_v2 *proxy,
                               struct kde_output_device_mode_v2 *mode_proxy)
{
    struct kde_output *output = data;
    struct kde_output_mode *mode = calloc(1, sizeof(*mode));

    if (!mode)
    {
        kde_output_device_mode_v2_destroy(mode_proxy);
        return;
    }
    mode->proxy = mode_proxy;
    list_add_tail(&output->modes, &mode->entry);
    kde_output_device_mode_v2_add_listener(mode_proxy, &mode_listener, mode);
}

static void output_handle_name(void *data, struct kde_output_device_v2 *proxy, const char *name)
{
    struct kde_output *output = data;

    free(output->pending_name);
    output->pending_name = strdup(name);
}

static void output_handle_hdr(void *data, struct kde_output_device_v2 *proxy, uint32_t enabled)
{
    struct kde_output *output = data;

    output->pending_hdr = !!enabled;
    output->have_hdr = TRUE;
}

static void output_handle_enabled(void *data, struct kde_output_device_v2 *proxy, int32_t enabled)
{
    struct kde_output *output = data;

    output->pending_enabled = !!enabled;
}

static void output_handle_capabilities(void *data, struct kde_output_device_v2 *proxy, uint32_t flags)
{
    struct kde_output *output = data;

    output->pending_capabilities = flags;
}

static void output_handle_done(void *data, struct kde_output_device_v2 *proxy)
{
    struct kde_output *output = data;

    pthread_mutex_lock(&process_wayland.output_mutex);
    if (output->pending_name)
    {
        free(output->name);
        output->name = output->pending_name;
        output->pending_name = NULL;
    }
    output->ready = output->name && output->have_hdr;
    output->hdr = output->pending_enabled && output->pending_hdr &&
                  (output->pending_capabilities & KDE_OUTPUT_DEVICE_V2_CAPABILITY_HIGH_DYNAMIC_RANGE);
    TRACE("KDE output %s ready=%u hdr=%u\n", debugstr_a(output->name), output->ready, output->hdr);
    pthread_mutex_unlock(&process_wayland.output_mutex);

    /* KDE properties may arrive before or after wl_output's done event. */
    wayland_output_refresh_hdr_state();
}

static void output_handle_removed(void *data, struct kde_output_device_v2 *proxy)
{
    pthread_mutex_lock(&process_wayland.output_mutex);
    output_destroy(data);
    pthread_mutex_unlock(&process_wayland.output_mutex);
    wayland_output_refresh_hdr_state();
}

static void output_handle_geometry(void *data, struct kde_output_device_v2 *proxy,
                                   int32_t x, int32_t y, int32_t physical_width,
                                   int32_t physical_height, int32_t subpixel,
                                   const char *make, const char *model, int32_t transform)
{
}

static void output_handle_current_mode(void *data, struct kde_output_device_v2 *proxy,
                                       struct kde_output_device_mode_v2 *mode)
{
}

static void output_handle_scale(void *data, struct kde_output_device_v2 *proxy, wl_fixed_t scale)
{
}

static void output_ignore_string(void *data, struct kde_output_device_v2 *proxy, const char *value)
{
}

static void output_ignore_uint(void *data, struct kde_output_device_v2 *proxy, uint32_t value)
{
}

static void output_handle_brightness_metadata(void *data, struct kde_output_device_v2 *proxy,
                                              uint32_t peak, uint32_t average, uint32_t minimum)
{
}

static void output_handle_brightness_overrides(void *data, struct kde_output_device_v2 *proxy,
                                               int32_t peak, int32_t average, int32_t minimum)
{
}

static void output_handle_bpc_range(void *data, struct kde_output_device_v2 *proxy,
                                    uint32_t minimum, uint32_t maximum)
{
}

/* Only events through v21 can be sent at our negotiated versions. */
static const struct kde_output_device_v2_listener output_listener =
{
    .geometry = output_handle_geometry,
    .current_mode = output_handle_current_mode,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .edid = output_ignore_string,
    .enabled = output_handle_enabled,
    .uuid = output_ignore_string,
    .serial_number = output_ignore_string,
    .eisa_id = output_ignore_string,
    .capabilities = output_handle_capabilities,
    .overscan = output_ignore_uint,
    .vrr_policy = output_ignore_uint,
    .rgb_range = output_ignore_uint,
    .name = output_handle_name,
    .high_dynamic_range = output_handle_hdr,
    .sdr_brightness = output_ignore_uint,
    .wide_color_gamut = output_ignore_uint,
    .auto_rotate_policy = output_ignore_uint,
    .icc_profile_path = output_ignore_string,
    .brightness_metadata = output_handle_brightness_metadata,
    .brightness_overrides = output_handle_brightness_overrides,
    .sdr_gamut_wideness = output_ignore_uint,
    .color_profile_source = output_ignore_uint,
    .brightness = output_ignore_uint,
    .color_power_tradeoff = output_ignore_uint,
    .dimming = output_ignore_uint,
    .replication_source = output_ignore_string,
    .ddc_ci_allowed = output_ignore_uint,
    .max_bits_per_color = output_ignore_uint,
    .max_bits_per_color_range = output_handle_bpc_range,
    .automatic_max_bits_per_color_limit = output_ignore_uint,
    .edr_policy = output_ignore_uint,
    .sharpness = output_ignore_uint,
    .priority = output_ignore_uint,
    .auto_brightness = output_ignore_uint,
    .removed = output_handle_removed,
};

static void output_create(struct kde_output_device_v2 *proxy, struct kde_output_registry *registry,
                           uint32_t global_id)
{
    struct kde_output *output;

    if (!proxy) return;
    if (!(output = calloc(1, sizeof(*output))))
    {
        output_proxy_destroy(proxy);
        return;
    }
    output->proxy = proxy;
    output->registry = registry;
    output->global_id = global_id;
    list_init(&output->modes);
    pthread_mutex_lock(&process_wayland.output_mutex);
    list_add_tail(&outputs, &output->entry);
    pthread_mutex_unlock(&process_wayland.output_mutex);
    kde_output_device_v2_add_listener(proxy, &output_listener, output);
}

static void registry_handle_output(void *data, struct kde_output_device_registry_v2 *proxy,
                                   struct kde_output_device_v2 *output)
{
    struct kde_output_registry *registry = data;

    if (registry->stopped) output_proxy_destroy(output);
    else output_create(output, registry, 0);
}

static void registry_handle_finished(void *data, struct kde_output_device_registry_v2 *proxy)
{
    struct kde_output_registry *registry = data;
    struct kde_output *output, *next;

    pthread_mutex_lock(&process_wayland.output_mutex);
    LIST_FOR_EACH_ENTRY_SAFE(output, next, &outputs, struct kde_output, entry)
        if (output->registry == registry) output_destroy(output);
    pthread_mutex_unlock(&process_wayland.output_mutex);
    kde_output_device_registry_v2_destroy(proxy);
    list_remove(&registry->entry);
    free(registry);
    wayland_output_refresh_hdr_state();
}

static const struct kde_output_device_registry_v2_listener registry_listener =
{
    .finished = registry_handle_finished,
    .output = registry_handle_output,
};

BOOL wayland_kde_output_bind(struct wl_registry *wl_registry, uint32_t id,
                             const char *interface, uint32_t version)
{
    struct kde_output_registry *registry;

    if (!strcmp(interface, "kde_output_device_registry_v2"))
    {
        /* This global rejects versions below 21 with a fatal protocol error. */
        if (version < 21) return TRUE;
        if (!(registry = calloc(1, sizeof(*registry)))) return TRUE;
        registry->proxy = wl_registry_bind(wl_registry, id, &kde_output_device_registry_v2_interface, 21);
        if (!registry->proxy)
        {
            free(registry);
            return TRUE;
        }
        registry->global_id = id;
        list_add_tail(&registries, &registry->entry);
        kde_output_device_registry_v2_add_listener(registry->proxy, &registry_listener, registry);
        return TRUE;
    }
    if (!strcmp(interface, "kde_output_device_v2"))
    {
        /* Older KWin versions advertise each device directly. v3 adds HDR. */
        if (version >= 3)
            output_create(wl_registry_bind(wl_registry, id, &kde_output_device_v2_interface, 3), NULL, id);
        return TRUE;
    }
    return FALSE;
}

BOOL wayland_kde_output_remove(uint32_t id)
{
    struct kde_output_registry *registry;
    struct kde_output *output;

    LIST_FOR_EACH_ENTRY(registry, &registries, struct kde_output_registry, entry)
    {
        if (registry->global_id != id) continue;
        pthread_mutex_lock(&process_wayland.output_mutex);
        registry->stopped = TRUE;
        pthread_mutex_unlock(&process_wayland.output_mutex);
        kde_output_device_registry_v2_stop(registry->proxy);
        wayland_output_refresh_hdr_state();
        return TRUE;
    }

    pthread_mutex_lock(&process_wayland.output_mutex);
    LIST_FOR_EACH_ENTRY(output, &outputs, struct kde_output, entry)
    {
        if (output->registry || output->global_id != id) continue;
        output_destroy(output);
        pthread_mutex_unlock(&process_wayland.output_mutex);
        wayland_output_refresh_hdr_state();
        return TRUE;
    }
    pthread_mutex_unlock(&process_wayland.output_mutex);
    return FALSE;
}

/* Called with output_mutex held. Match only connector names, not position or
 * model: mixed HDR/SDR desktops must retain per-output state. */
BOOL wayland_kde_output_get_hdr_state(const char *name, BOOL *enabled)
{
    struct kde_output *output;

    if (!name) return FALSE;
    LIST_FOR_EACH_ENTRY(output, &outputs, struct kde_output, entry)
    {
        if (!output->ready || (output->registry && output->registry->stopped)) continue;
        if (strcmp(name, output->name)) continue;
        *enabled = output->hdr;
        return TRUE;
    }
    return FALSE;
}
