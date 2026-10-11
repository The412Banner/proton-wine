/*
 * Copyright 2011-2012 Maarten Lankhorst
 * Copyright 2010-2011 Maarten Lankhorst for CodeWeavers
 * Copyright 2011 Andrew Eikum for CodeWeavers
 * Copyright 2022 Huw Davies
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
#undef _TIME_BITS /* libpulse uses default time bitness convention. */
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef HAVE_LINUX_HIDRAW_H
# include <linux/hidraw.h>
#endif
#include <alsa/asoundlib.h>
#include <pulse/pulseaudio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"

#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"

#include "wine/debug.h"
#include "wine/list.h"
#include "wine/server.h"
#include "wine/unixlib.h"

#include "initguid.h"
#include "devpkey.h"
DEFINE_GUID(GUID_NULL,0,0,0,0,0,0,0,0,0,0,0);

#include "../mmdevapi/unixlib.h"

#ifdef HAVE_LIBUDEV_H
#include <libudev.h>
#endif

#include "mult.h"

WINE_DEFAULT_DEBUG_CHANNEL(pulse);

C_ASSERT((sizeof(void *) == 8 && sizeof(struct timeval) == 16) || (sizeof(void *) == 4 && sizeof(struct timeval) == 8));

enum phys_device_bus_type {
    phys_device_bus_invalid = -1,
    phys_device_bus_pci,
    phys_device_bus_usb
};

struct pulse_period
{
    struct list entry;
    char *device;
    pa_usec_t period;
    pa_usec_t timer_last_time, stream_time;
    int64_t adjust;
    struct list streams;
    pa_time_event *time_event;
    struct pulse_stream *timer_stream;
};

static struct list active_periods = LIST_INIT(active_periods);
static struct list dualsense_mono_streams = LIST_INIT(dualsense_mono_streams);
static struct list dualsense_haptic_streams = LIST_INIT(dualsense_haptic_streams);

struct pulse_stream
{
    EDataFlow dataflow;

    char *device;
    pa_stream *stream;
    pa_stream *speaker_stream;
    char *speaker_device;
    snd_pcm_t *haptic_pcm;
    char *haptic_alsa_path;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_buffer_attr attr;

    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;
    float vol[PA_CHANNELS_MAX];

    REFERENCE_TIME def_period;
    REFERENCE_TIME duration;

    INT32 locked;
    BOOL started;
    SIZE_T bufsize_frames, real_bufsize_bytes, period_bytes;
    SIZE_T peek_ofs, read_offs_bytes, lcl_offs_bytes, pa_offs_bytes;
    SIZE_T tmp_buffer_bytes, held_bytes, peek_len, peek_buffer_len, pa_held_bytes, max_pa_held_bytes;
    BYTE *local_buffer, *tmp_buffer, *peek_buffer;
    int16_t *haptic_buffer;
    SIZE_T haptic_buffer_frames;
    BYTE *speaker_buffer;
    SIZE_T speaker_buffer_bytes, speaker_buffer_held, speaker_dropped_bytes;
    float speaker_peak;
    pa_usec_t speaker_trace_time;
    unsigned int haptic_channel_peak[4];
    pa_usec_t haptic_channel_trace_time;
    pa_usec_t haptic_reconnect_time;
    pa_usec_t haptic_path_check_time;
    unsigned int haptic_hotplug_generation;
    void *locked_ptr;
    BOOL just_underran, pa_started, update_timing_info_pending, rebase_write_index;
    pa_usec_t mmdev_period_usec;
    pa_usec_t timeline_start_stream_time, timeline_start_period_time;

    INT64 clock_lastpos, clock_written;

    struct list packet_free_head;
    struct list dualsense_mono_entry;
    struct list dualsense_haptic_entry;
    struct list packet_filled_head;
    struct list period_entry;
    struct pulse_period *period;
    unsigned int dualsense_mono_hotplug_generation;
    BOOL dualsense_mono_registered;
    BOOL dualsense_haptic_registered;
    GUID sony_controller_container_id;
    BOOL sony_controller_container_valid;
    BOOL sony_speaker_source;
    BOOL sony_actuator_source;
    pa_usec_t sony_speaker_signal_time;
    pa_usec_t sony_actuator_signal_time;
    BOOL speaker_route_selected;
};

static void pulse_write(struct pulse_stream *stream);

typedef struct _ACPacket
{
    struct list entry;
    UINT64 qpcpos;
    BYTE *data;
    UINT32 discont;
} ACPacket;

typedef struct _PhysDevice {
    struct list entry;
    WCHAR *name;
    enum phys_device_bus_type bus_type;
    USHORT vendor_id, product_id;

    /* ready vars */
    BOOL ready;
    pthread_mutex_t ready_mutex;
    pthread_cond_t ready_cond;

    /* probe vars */
    pa_channel_map map;
    pa_sample_spec ss;
    unsigned int length;
    int probe_status;

    EDataFlow flow;
    EndpointFormFactor form;
    UINT channel_mask;
    UINT index;
    int alsa_card;
    REFERENCE_TIME min_period, def_period;
    WAVEFORMATEXTENSIBLE fmt;
    GUID container_id;
    char *raw_haptic_target;
    char *raw_haptic_alsa_path;
    char *endpoint_id;
    char pulse_name[0];
} PhysDevice;

static pa_context *pulse_ctx;
static pa_mainloop *pulse_ml;

static pthread_mutex_t g_phys_mutex;
static pthread_cond_t g_phys_cond = PTHREAD_COND_INITIALIZER;
static unsigned int g_haptic_hotplug_generation;
static unsigned int g_dualsense_endpoint_generation;
static LONG g_sony_windows_audio_mode;
static unsigned int g_dualsense_mono_speaker_add_generation;
static char *g_dualsense_mono_preferred_sink;
static unsigned int g_phys_event_mask;
static unsigned int g_phys_event_stale_render;
static unsigned int g_phys_event_stale_capture;
static struct list g_phys_speakers = LIST_INIT(g_phys_speakers);
static struct list g_phys_sources = LIST_INIT(g_phys_sources);
static struct list g_phys_speakers_added = LIST_INIT(g_phys_speakers_added);
static struct list g_phys_sources_added = LIST_INIT(g_phys_sources_added);
static struct list g_phys_speakers_removed = LIST_INIT(g_phys_speakers_removed);
static struct list g_phys_sources_removed = LIST_INIT(g_phys_sources_removed);
/* separate list for holding removed devices that were just added */
/* to avoid race-condition/deadlock. */
static struct list g_phys_added_removed = LIST_INIT(g_phys_added_removed);

static pthread_mutex_t pulse_mutex;
static pthread_cond_t pulse_cond = PTHREAD_COND_INITIALIZER;

static ULONG_PTR zero_bits = 0;

static UINT32 silence_buf_size = 1048576;
static BYTE *silence_buf;

#define PHYS_EVENT_RENDER 0x1
#define PHYS_EVENT_CAPTURE 0x2

static BOOL device_list_has_dualsense_audio_device(struct list *devices)
{
    PhysDevice *dev;

    LIST_FOR_EACH_ENTRY(dev, devices, PhysDevice, entry)
    {
        if (dev->bus_type == phys_device_bus_usb && dev->vendor_id == 0x054c
                && (dev->product_id == 0x05c4 || dev->product_id == 0x09cc ||
                    dev->product_id == 0x0ce6 || dev->product_id == 0x0df2))
            return TRUE;
    }

    return FALSE;
}

static BOOL device_list_has_missing_dualsense_render_profile(struct list *capture_devices)
{
    static const char monitor_suffix[] = ".monitor";
    PhysDevice *capture, *render;

    LIST_FOR_EACH_ENTRY(capture, capture_devices, PhysDevice, entry)
    {
        size_t capture_length, render_length;
        BOOL found = FALSE;

        if (capture->bus_type != phys_device_bus_usb || capture->vendor_id != 0x054c ||
                (capture->product_id != 0x05c4 && capture->product_id != 0x09cc &&
                 capture->product_id != 0x0ce6 && capture->product_id != 0x0df2))
            continue;

        capture_length = strlen(capture->pulse_name);
        if (capture_length <= sizeof(monitor_suffix) - 1 ||
                strcmp(capture->pulse_name + capture_length -
                (sizeof(monitor_suffix) - 1), monitor_suffix))
            continue;
        render_length = capture_length - (sizeof(monitor_suffix) - 1);

        LIST_FOR_EACH_ENTRY(render, &g_phys_speakers, PhysDevice, entry)
        {
            if (strlen(render->pulse_name) == render_length &&
                    !memcmp(render->pulse_name, capture->pulse_name, render_length))
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return TRUE;
    }

    return FALSE;
}

static NTSTATUS pulse_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

#define DUALSENSE_HAPTIC_REFRESH_USEC 1000000

static int16_t float_to_s16(float sample)
{
    if (sample > 1.0f) sample = 1.0f;
    else if (sample < -1.0f) sample = -1.0f;

    return sample < 0.0f ? sample * 32768.0f : sample * 32767.0f;
}


static void g_phys_lock(void)
{
    pthread_mutex_lock(&g_phys_mutex);
}

static void g_phys_unlock(void)
{
    pthread_mutex_unlock(&g_phys_mutex);
}

static void pulse_lock(void)
{
    pthread_mutex_lock(&pulse_mutex);
}

static void pulse_unlock(void)
{
    pthread_mutex_unlock(&pulse_mutex);
}

static int pulse_cond_wait(void)
{
    return pthread_cond_wait(&pulse_cond, &pulse_mutex);
}

static int pulse_cond_timedwait_ms(unsigned int timeout_ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    return pthread_cond_timedwait(&pulse_cond, &pulse_mutex, &ts);
}

static int g_phys_cond_timedwait_ms(unsigned int timeout_ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    return pthread_cond_timedwait(&g_phys_cond, &g_phys_mutex, &ts);
}

static void pulse_broadcast(void)
{
    pthread_cond_broadcast(&pulse_cond);
}

static void pulse_dev_free(PhysDevice *dev)
{
    pthread_mutex_destroy(&dev->ready_mutex);
    pthread_cond_destroy(&dev->ready_cond);
    free(dev->name);
    free(dev->raw_haptic_target);
    free(dev->raw_haptic_alsa_path);
    free(dev->endpoint_id);
    free(dev);
}

static void pulse_dev_wait_ready(PhysDevice *dev)
{
    if (dev->ready)
        return;
    pthread_mutex_lock(&dev->ready_mutex);
    while (!dev->ready)
        pthread_cond_wait(&dev->ready_cond, &dev->ready_mutex);
    pthread_mutex_unlock(&dev->ready_mutex);
}

static struct pulse_stream *handle_get_stream(stream_handle h)
{
    return (struct pulse_stream *)(UINT_PTR)h;
}

static void remove_stream_from_period(struct pulse_stream *stream);
static void pulse_add_stream_to_period(struct pulse_stream *stream);

static const char *pulse_endpoint_id(const PhysDevice *dev)
{
    return dev->endpoint_id ? dev->endpoint_id : dev->pulse_name;
}

static BOOL pulse_device_matches(const PhysDevice *dev, const char *name)
{
    return name && (!strcmp(name, dev->pulse_name) || !strcmp(name, pulse_endpoint_id(dev)));
}

static BOOL pulse_get_device_container_id(const char *name, GUID *container_id)
{
    static struct list *const lists[] =
    {
        &g_phys_speakers_added,
        &g_phys_speakers,
        &g_phys_sources_added,
        &g_phys_sources,
        NULL
    };
    struct list *const *list;
    PhysDevice *dev;
    BOOL found = FALSE;

    if (!name)
        return FALSE;

    g_phys_lock();
    for (list = lists; *list && !found; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            if (!pulse_device_matches(dev, name) || IsEqualGUID(&dev->container_id, &GUID_NULL))
                continue;

            *container_id = dev->container_id;
            found = TRUE;
            break;
        }
    }
    g_phys_unlock();
    return found;
}

static BOOL pulse_get_haptic_target_container_id(const char *target, BOOL alsa, GUID *container_id)
{
    static struct list *const lists[] = { &g_phys_speakers_added, &g_phys_speakers, NULL };
    struct list *const *list;
    PhysDevice *dev;
    BOOL found = FALSE;

    if (!target)
        return FALSE;

    g_phys_lock();
    for (list = lists; *list && !found; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            const char *device_target = alsa ? dev->raw_haptic_alsa_path : dev->raw_haptic_target;

            if (!device_target || strcmp(device_target, target) ||
                    IsEqualGUID(&dev->container_id, &GUID_NULL))
                continue;

            *container_id = dev->container_id;
            found = TRUE;
            break;
        }
    }
    g_phys_unlock();
    return found;
}

static char *make_pipewire_dualsense_haptic_path(const char *target);

static char *pulse_get_haptic_path_for_container(const GUID *container_id)
{
    static struct list *const lists[] = { &g_phys_speakers_added, &g_phys_speakers, NULL };
    struct list *const *list;
    PhysDevice *dev;
    char *path = NULL;

    g_phys_lock();
    for (list = lists; *list && !path; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            if (!IsEqualGUID(&dev->container_id, container_id))
                continue;

            if (dev->raw_haptic_target && dev->raw_haptic_target[0])
                path = make_pipewire_dualsense_haptic_path(dev->raw_haptic_target);
            else if (dev->raw_haptic_alsa_path && dev->raw_haptic_alsa_path[0])
                path = strdup(dev->raw_haptic_alsa_path);
            if (path)
                break;
        }
    }
    g_phys_unlock();
    return path;
}

static BOOL pulse_get_active_sony_controller(GUID *container_id)
{
    unsigned int data[4];
    BOOL valid = FALSE;

    C_ASSERT(sizeof(data) == sizeof(*container_id));
    SERVER_START_REQ(get_sony_active_controller)
    {
        if (!wine_server_call(req) && reply->valid)
        {
            data[0] = reply->container0;
            data[1] = reply->container1;
            data[2] = reply->container2;
            data[3] = reply->container3;
            valid = TRUE;
        }
    }
    SERVER_END_REQ;

    if (valid)
        memcpy(container_id, data, sizeof(data));
    return valid;
}

static BOOL pulse_container_is_present(const GUID *container_id)
{
    static struct list *const lists[] = { &g_phys_speakers_added, &g_phys_speakers, NULL };
    struct list *const *list;
    PhysDevice *dev;
    BOOL found = FALSE;

    g_phys_lock();
    for (list = lists; *list && !found; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            if (!IsEqualGUID(&dev->container_id, container_id))
                continue;

            found = TRUE;
            break;
        }
    }
    g_phys_unlock();
    return found;
}

static void dump_attr(const pa_buffer_attr *attr)
{
    TRACE("maxlength: %u\n", attr->maxlength);
    TRACE("minreq: %u\n", attr->minreq);
    TRACE("fragsize: %u\n", attr->fragsize);
    TRACE("tlength: %u\n", attr->tlength);
    TRACE("prebuf: %u\n", attr->prebuf);
}

static void free_phys_device_lists(void)
{
    static struct list *const lists[] = {
        &g_phys_speakers,
        &g_phys_sources,
        &g_phys_speakers_added,
        &g_phys_sources_added,
        &g_phys_speakers_removed,
        &g_phys_sources_removed,
        &g_phys_added_removed,
        NULL
    };
    struct list *const *list = lists;
    PhysDevice *dev, *dev_next;

    g_phys_lock();
    do {
        LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, *list, PhysDevice, entry)
            pulse_dev_free(dev);
        list_init(*list);
    } while (*(++list));
    g_phys_unlock();
}

/* copied from kernelbase */
static int muldiv(int a, int b, int c)
{
    LONGLONG ret;

    if (!c) return -1;

    /* We want to deal with a positive divisor to simplify the logic. */
    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    /* If the result is positive, we "add" to round. else, we subtract to round. */
    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static char *wstr_to_str(const WCHAR *wstr)
{
    const int len = wcslen(wstr);
    char *str = malloc(len * 3 + 1);
    ntdll_wcstoumbs(wstr, len + 1, str, len * 3 + 1, FALSE);
    return str;
}

static BOOL wait_pa_operation_complete(pa_operation *o)
{
    if (!o)
        return FALSE;

    while (pulse_ml && pa_operation_get_state(o) == PA_OPERATION_RUNNING)
        pulse_cond_wait();
    pa_operation_unref(o);
    return !!pulse_ml;
}

/* Following pulseaudio design here, mainloop has the lock taken whenever
 * it is handling something for pulse, and the lock is required whenever
 * doing any pa_* call that can affect the state in any way
 *
 * pa_cond_wait is used when waiting on results, because the mainloop needs
 * the same lock taken to affect the state
 *
 * This is basically the same as the pa_threaded_mainloop implementation,
 * but that cannot be used because it uses pthread_create directly
 *
 * pa_threaded_mainloop_(un)lock -> pthread_mutex_(un)lock
 * pa_threaded_mainloop_signal -> pthread_cond_broadcast
 * pa_threaded_mainloop_wait -> pthread_cond_wait
 */
static int pulse_poll_func(struct pollfd *ufds, unsigned long nfds, int timeout, void *userdata)
{
    int r;
    pulse_unlock();
    r = poll(ufds, nfds, timeout);
    pulse_lock();
    return r;
}

static NTSTATUS pulse_process_attach(void *args)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);

    if (pthread_mutex_init(&pulse_mutex, &attr) != 0)
        pthread_mutex_init(&pulse_mutex, NULL);

    if (pthread_mutex_init(&g_phys_mutex, &attr) != 0)
        pthread_mutex_init(&g_phys_mutex, NULL);

#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset)
    {
        SYSTEM_BASIC_INFORMATION info;

        NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
        zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    }
#endif

    return STATUS_SUCCESS;
}

static NTSTATUS pulse_process_detach(void *args)
{
    free_phys_device_lists();
    if (pulse_ctx)
    {
        pa_context_disconnect(pulse_ctx);
        pa_context_unref(pulse_ctx);
    }
    if (pulse_ml)
        pa_mainloop_quit(pulse_ml, 0);

    free( silence_buf );
    silence_buf = NULL;
    free(g_dualsense_mono_preferred_sink);
    g_dualsense_mono_preferred_sink = NULL;

    pthread_mutex_destroy(&pulse_mutex);
    pthread_mutex_destroy(&g_phys_mutex);

    return STATUS_SUCCESS;
}

static void pulse_main_loop_thread_cleanup(void *context)
{
    TRACE("Main loop thread is being aborted.\n");
    pulse_broadcast();
}

static pa_mainloop *pulse_main_loop_new(void)
{
    if (pulse_ml)
        return pulse_ml;
    pulse_ml = pa_mainloop_new();
    if (!pulse_ml)
        return NULL;
    pa_mainloop_set_poll_func(pulse_ml, pulse_poll_func, NULL);
    return pulse_ml;
}

static void pulse_main_loop_free(void)
{
    if (!pulse_ml)
        return;
    pa_mainloop_free(pulse_ml);
    pulse_ml = NULL;
}

static NTSTATUS pulse_main_loop(void *args)
{
    struct main_loop_params *params = args;
    int ret;
    pulse_lock();
    if (!pulse_main_loop_new()) {
        NtSetEvent(params->event, NULL);
        pulse_unlock();
        ERR("Failed to create main loop\n");
        return STATUS_SUCCESS;
    }
    NtSetEvent(params->event, NULL);
    pthread_cleanup_push(pulse_main_loop_thread_cleanup, NULL);
    pa_mainloop_run(pulse_ml, &ret);
    pthread_cleanup_pop(0);
    pulse_main_loop_free();
    pulse_unlock();

    pthread_cond_broadcast(&g_phys_cond);

    return STATUS_SUCCESS;
}

static BOOL pulse_get_endpoints_size_needed(struct list *list, size_t *needed, unsigned int size)
{
    PhysDevice *dev;
    size_t len, name_len;

    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry) {
        name_len = lstrlenW(dev->name) + 1;
        len = strlen(pulse_endpoint_id(dev)) + 1;
        *needed += name_len * sizeof(WCHAR) + ((len + 1) & ~1);

        if (*needed > size)
            return FALSE;
    }
    return TRUE;
}

static void pulse_get_endpoints(struct list *list, struct endpoint **endpoint, unsigned int *offset, void *names)
{
    PhysDevice *dev;
    size_t len, name_len;

    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry) {
        name_len = lstrlenW(dev->name) + 1;
        len = strlen(pulse_endpoint_id(dev)) + 1;

        (*endpoint)->name = *offset;
        memcpy((char *)names + *offset, dev->name, name_len * sizeof(WCHAR));
        *offset += name_len * sizeof(WCHAR);
        (*endpoint)->device = *offset;
        memcpy((char *)names + *offset, pulse_endpoint_id(dev), len);
        *offset += (len + 1) & ~1;
        (*endpoint)++;
    }
}

static void pulse_wait_devices(struct list *list)
{
    PhysDevice *dev;
again:
    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry) {
        if (dev->ready)
            continue;

        g_phys_unlock();
        pulse_dev_wait_ready(dev);
        g_phys_lock();
        goto again;
    }
}

static BOOL pulse_refresh_devices(EDataFlow flow);
static BOOL is_dualsense_speaker_sink(const PhysDevice *dev);
static char *pulse_find_shared_sony_speaker_survivor(const PhysDevice *removed,
        struct list *active, struct list *added);
static void pulse_retarget_dualsense_mono_streams(const char *preferred);
static BOOL use_pipewire_dualsense_haptic_target(void);
static BOOL use_dualsense_split_audio(void);
static void pulse_retarget_dualsense_haptic_streams(const char *target);
static void pulse_retarget_dualsense_haptic_streams_to_alsa(const char *path);
static char *find_dualsense_haptic_alsa_path(void);

/* for winepulse, this function can be called in a separate thread from other functions. */
/* this function cannot be called from multiple threads at the same time however. */
/* for the list variable, all devices in it must be ready by the end of the lock. */
static NTSTATUS pulse_get_endpoint_ids(void *args)
{
    struct get_endpoint_ids_params *params = args;
    struct list *list;
    struct list *list_added;
    struct list *list_removed;
    struct list *other_list_added;
    struct list *other_list_removed;
    struct endpoint *endpoint = params->endpoints;
    size_t needed = 0;
    unsigned int offset;
    unsigned int retry;
    unsigned int event_mask;
    unsigned int other_event_mask;
    unsigned int *event_stale;
    BOOL refreshed;
    PhysDevice *dev, *dev_next;
    BOOL delta = params->delta;
    BOOL find_dualsense_haptic_alsa = FALSE;
    BOOL retarget_haptic = use_pipewire_dualsense_haptic_target();
    BOOL refresh_render_after_capture = FALSE;
    char *dualsense_mono_target = NULL;
    char *dualsense_haptic_target = NULL;
    char *dualsense_haptic_alsa_path = NULL;

    TRACE("flow %d, delta %d\n", (int)params->flow, (int)params->delta);

    if (params->flow == eRender)
    {
        list = &g_phys_speakers;
        list_added = &g_phys_speakers_added;
        list_removed = &g_phys_speakers_removed;
        event_mask = PHYS_EVENT_RENDER;
        other_event_mask = PHYS_EVENT_CAPTURE;
        event_stale = &g_phys_event_stale_render;

        other_list_added = &g_phys_sources_added;
        other_list_removed = &g_phys_sources_removed;
    }
    else
    {
        list = &g_phys_sources;
        list_added = &g_phys_sources_added;
        list_removed = &g_phys_sources_removed;
        event_mask = PHYS_EVENT_CAPTURE;
        other_event_mask = PHYS_EVENT_RENDER;
        event_stale = &g_phys_event_stale_capture;

        other_list_added = &g_phys_speakers_added;
        other_list_removed = &g_phys_speakers_removed;
    }

    params->num = params->num_removed = 0;
    params->more_data = FALSE;

    g_phys_lock();

    pulse_wait_devices(&g_phys_added_removed);
    LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, &g_phys_added_removed, PhysDevice, entry)
        pulse_dev_free(dev);
    list_init(&g_phys_added_removed);

    if (delta)
    {
        /* also check if we are still running */
        if (!pulse_ml)
        {
            g_phys_cond_timedwait_ms(1000);
            goto done;
        }
        if (!list_count(list_added) && !list_count(list_removed))
        {
            if (!(g_phys_event_mask & event_mask)
                    && (list_count(other_list_added) + list_count(other_list_removed)
                    || (g_phys_event_mask & other_event_mask)))
            {
                params->more_data = TRUE;
                goto done;
            }
            while (!(g_phys_event_mask & event_mask))
            {
                if (!pulse_ml)
                {
                    g_phys_cond_timedwait_ms(1000);
                    goto done;
                }
                if (g_phys_event_mask & ~event_mask)
                {
                    params->more_data = TRUE;
                    goto done;
                }
                if (g_phys_cond_timedwait_ms(1000) == ETIMEDOUT)
                    goto done;
            }
            for (retry = 0; retry < 4 && !list_count(list_added) && !list_count(list_removed); ++retry)
            {
                g_phys_unlock();
                if (retry)
                    poll(NULL, 0, 250);
                refreshed = pulse_refresh_devices(params->flow);
                g_phys_lock();
                if (!refreshed)
                    break;
            }
            if (!list_count(list_added) && !list_count(list_removed))
            {
                if (++*event_stale >= 8)
                {
                    TRACE("Dropping stale PulseAudio endpoint event for flow %d after %u refresh passes.\n",
                            (int)params->flow, *event_stale);
                    g_phys_event_mask &= ~event_mask;
                    *event_stale = 0;
                }
                goto done;
            }
            g_phys_event_mask &= ~event_mask;
            *event_stale = 0;
        }

        params->default_idx = -1;

        if (params->flow == eCapture && (g_phys_event_mask & PHYS_EVENT_RENDER)
                && (device_list_has_dualsense_audio_device(list_added)
                    || device_list_has_dualsense_audio_device(list_removed)))
        {
            TRACE("Deferring DualSense capture endpoint delta until render endpoints are refreshed.\n");
            params->more_data = TRUE;
            goto done;
        }

        pulse_wait_devices(list_added);
        pulse_wait_devices(list_removed);

        if (params->flow == eRender)
        {
            LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, list_removed, PhysDevice, entry)
            {
                char *survivor = pulse_find_shared_sony_speaker_survivor(dev,
                        list, list_added);

                if (!survivor)
                    continue;

                TRACE("Keeping shared Sony speaker endpoint %s while physical sink %s survives.\n",
                        debugstr_a(pulse_endpoint_id(dev)), debugstr_a(survivor));
                free(dualsense_mono_target);
                dualsense_mono_target = survivor;
                list_remove(&dev->entry);
                pulse_dev_free(dev);
            }
        }

        params->num = list_count(list_added);
        params->num_removed = list_count(list_removed);
        offset = needed = (params->num + params->num_removed) * sizeof(*params->endpoints);

        if (!pulse_get_endpoints_size_needed(list_added, &needed, params->size))
            goto done;
        if (!pulse_get_endpoints_size_needed(list_removed, &needed, params->size))
            goto done;

        pulse_get_endpoints(list_added, &endpoint, &offset, params->endpoints);
        pulse_get_endpoints(list_removed, &endpoint, &offset, params->endpoints);

        refresh_render_after_capture = params->flow == eCapture &&
                device_list_has_dualsense_audio_device(list_added) &&
                device_list_has_dualsense_audio_device(list_removed) &&
                device_list_has_missing_dualsense_render_profile(list_added);

        LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, list_added, PhysDevice, entry) {
            if (params->flow == eRender && is_dualsense_speaker_sink(dev))
            {
                char *target = strdup(dev->pulse_name);

                if (target)
                {
                    free(dualsense_mono_target);
                    dualsense_mono_target = target;
                }
                if (retarget_haptic && !dev->raw_haptic_target)
                    find_dualsense_haptic_alsa = TRUE;
            }
            if (retarget_haptic && params->flow == eRender && dev->raw_haptic_target)
            {
                char *target = strdup(dev->raw_haptic_target);

                if (target)
                {
                    free(dualsense_haptic_target);
                    dualsense_haptic_target = target;
                }
            }
            list_remove(&dev->entry);
            list_add_tail(list, &dev->entry);
        }
        LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, list_removed, PhysDevice, entry) {
            list_remove(&dev->entry);
            pulse_dev_free(dev);
        }
        params->more_data = list_count(other_list_added) + list_count(other_list_removed)
                || (g_phys_event_mask & other_event_mask);
        if (refresh_render_after_capture)
        {
            TRACE("Scheduling a render refresh after Sony capture profile replacement.\n");
            g_phys_event_mask |= PHYS_EVENT_RENDER;
            params->more_data = TRUE;
        }
    }
    else
    {
        pulse_wait_devices(list);
        pulse_wait_devices(list_added);

        params->default_idx = 0;
        params->num = list_count(list) + list_count(list_added);
        params->num_removed = 0;
        offset = needed = params->num * sizeof(*params->endpoints);

        if (!pulse_get_endpoints_size_needed(list, &needed, params->size))
            goto done;
        if (!pulse_get_endpoints_size_needed(list_added, &needed, params->size))
            goto done;

        pulse_get_endpoints(list, &endpoint, &offset, params->endpoints);
        pulse_get_endpoints(list_added, &endpoint, &offset, params->endpoints);

        LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, list_added, PhysDevice, entry) {
            if (params->flow == eRender && is_dualsense_speaker_sink(dev))
            {
                char *target = strdup(dev->pulse_name);

                if (target)
                {
                    free(dualsense_mono_target);
                    dualsense_mono_target = target;
                }
                if (retarget_haptic && !dev->raw_haptic_target)
                    find_dualsense_haptic_alsa = TRUE;
            }
            if (retarget_haptic && params->flow == eRender && dev->raw_haptic_target)
            {
                char *target = strdup(dev->raw_haptic_target);

                if (target)
                {
                    free(dualsense_haptic_target);
                    dualsense_haptic_target = target;
                }
            }
            list_remove(&dev->entry);
            list_add_tail(list, &dev->entry);
        }
    }

done:
    g_phys_unlock();

    /* DualShock 4 publishes only its public stereo PipeWire sink. Discover
     * its four-channel ALSA PCM after releasing the physical-device lock so
     * an existing DualSense effects stream can move to the newly added pad. */
    if (!dualsense_haptic_target && find_dualsense_haptic_alsa)
        dualsense_haptic_alsa_path = find_dualsense_haptic_alsa_path();

    if (dualsense_mono_target || dualsense_haptic_target || dualsense_haptic_alsa_path)
    {
        pulse_lock();
        if (dualsense_mono_target)
            pulse_retarget_dualsense_mono_streams(dualsense_mono_target);
        if (dualsense_haptic_target)
            pulse_retarget_dualsense_haptic_streams(dualsense_haptic_target);
        else if (dualsense_haptic_alsa_path)
            pulse_retarget_dualsense_haptic_streams_to_alsa(dualsense_haptic_alsa_path);
        pulse_unlock();
        free(dualsense_mono_target);
        free(dualsense_haptic_target);
        free(dualsense_haptic_alsa_path);
    }

    if (needed > params->size) {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    } else {
        params->delta = TRUE;
        params->result = S_OK;
    }
    TRACE("returning flow %d, result %#x, added %u, removed %u, more_data %d, event_mask %#x.\n",
            (int)params->flow, (unsigned int)params->result, params->num, params->num_removed,
            params->more_data, g_phys_event_mask);
    return STATUS_SUCCESS;
}

static void pulse_stream_state(pa_stream *s, void *user)
{
    struct pulse_stream *stream = user;
    pa_stream_state_t state = pa_stream_get_state(s);
    TRACE("%p: Stream state changed to %i\n", user, state);
    if (stream && stream->dataflow == eRender && state == PA_STREAM_READY)
    {
        const pa_buffer_attr *attr = pa_stream_get_buffer_attr(s);
        pa_operation *op;

        if (attr)
            stream->attr = *attr;
        if (stream->started)
        {
            pulse_write(stream);
            if (pa_stream_is_corked(s) && (op = pa_stream_cork(s, 0, NULL, NULL)))
                pa_operation_unref(op);
        }
    }
    pulse_broadcast();
}

static void pulse_split_speaker_state(pa_stream *s, void *user)
{
    TRACE("%p: Split speaker stream state changed to %i\n", user, pa_stream_get_state(s));
    pulse_broadcast();
}

static void pulse_attr_update(pa_stream *s, void *user) {
    struct pulse_stream *stream = user;
    const pa_buffer_attr *attr = pa_stream_get_buffer_attr(s);
    TRACE("%p: New attributes or device moved:\n", user);
    if (stream && attr)
        stream->attr = *attr;
    dump_attr(attr);
}

static void pulse_underflow_callback(pa_stream *s, void *userdata)
{
    struct pulse_stream *stream = userdata;
    WARN("%p: Underflow\n", userdata);
    stream->just_underran = TRUE;
    stream->pa_started = FALSE;
    stream->timeline_start_period_time = 0;
    stream->timeline_start_stream_time = 0;

    if (stream->period && stream->period->timer_stream == stream)
    {
        stream->period->timer_stream = NULL;
        stream->period->adjust = 0;
    }
}

static void pulse_started_callback(pa_stream *s, void *userdata)
{
    struct pulse_stream *stream = userdata;

    TRACE("%p: (Re)started playing\n", userdata);
    stream->pa_started = TRUE;
}

static void pulse_op_cb(pa_stream *s, int success, void *user)
{
    TRACE("Success: %i\n", success);
    *(int*)user = success;
    pulse_broadcast();
}

static void pulse_context_op_cb(pa_context *c, int success, void *user)
{
    TRACE("Success: %i\n", success);
    *(int*)user = success;
    pulse_broadcast();
}

static void silence_buffer(pa_sample_format_t format, BYTE *buffer, UINT32 bytes)
{
    memset(buffer, format == PA_SAMPLE_U8 ? 0x80 : 0, bytes);
}

static BOOL pulse_stream_valid(struct pulse_stream *stream)
{
    pa_stream_state_t state;

    if (stream->dualsense_mono_registered)
        return TRUE;

    if (use_pipewire_dualsense_haptic_target() && !stream->stream &&
            stream->dualsense_haptic_registered)
        return stream->dataflow == eRender;

    if (!stream->stream)
        return stream->dataflow == eRender && stream->haptic_pcm && stream->haptic_alsa_path
                && stream->haptic_alsa_path[0];

    state = pa_stream_get_state(stream->stream);

    return state == PA_STREAM_READY || (stream->dataflow == eRender && state == PA_STREAM_CREATING);
}

static BOOL pulse_stream_ready(struct pulse_stream *stream)
{
    if (!stream->stream)
        return FALSE;

    return pa_stream_get_state(stream->stream) == PA_STREAM_READY;
}

static BOOL pulse_stream_haptic(struct pulse_stream *stream)
{
    return stream->dataflow == eRender && stream->haptic_alsa_path;
}

static void pulse_probe_settings(pa_mainloop *ml, pa_context *ctx, int render, const char *pulse_name,
                                 WAVEFORMATEXTENSIBLE *fmt, REFERENCE_TIME *def_period, REFERENCE_TIME *min_period);

static void pulse_contextcallback(pa_context *c, void *userdata)
{
    switch (pa_context_get_state(c))
    {
    default:
        FIXME("Unhandled state: %i\n", pa_context_get_state(c));
        break;
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
    case PA_CONTEXT_TERMINATED:
        TRACE("State change to %i\n", pa_context_get_state(c));
        break;
    case PA_CONTEXT_READY:
        TRACE("Ready\n");
        break;
    case PA_CONTEXT_FAILED:
        WARN("Context failed: %s\n", pa_strerror(pa_context_errno(c)));
        break;
    }
    pulse_broadcast();
}

static void pulse_subscribe_callback(pa_context *c, pa_subscription_event_type_t type, uint32_t index, void *userdata)
{
    pa_subscription_event_type_t facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_subscription_event_type_t event = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    unsigned int event_mask = 0;

    if (event != PA_SUBSCRIPTION_EVENT_NEW && event != PA_SUBSCRIPTION_EVENT_REMOVE)
        return;

    if (facility == PA_SUBSCRIPTION_EVENT_SINK)
        event_mask = PHYS_EVENT_RENDER;
    else if (facility == PA_SUBSCRIPTION_EVENT_SOURCE)
        event_mask = PHYS_EVENT_CAPTURE;
    else if (facility == PA_SUBSCRIPTION_EVENT_CARD)
        event_mask = PHYS_EVENT_RENDER | PHYS_EVENT_CAPTURE;
    else
        return;

    TRACE("PulseAudio endpoint event facility %#x event %#x index %u.\n", facility, event, index);

    g_phys_lock();
    if (event_mask & PHYS_EVENT_RENDER)
    {
        ++g_haptic_hotplug_generation;
        g_phys_event_stale_render = 0;
    }
    if (event_mask & PHYS_EVENT_CAPTURE)
        g_phys_event_stale_capture = 0;
    g_phys_event_mask |= event_mask;
    pthread_cond_broadcast(&g_phys_cond);
    g_phys_unlock();
}

static HRESULT pulse_connect(const char *name)
{
    pa_context_state_t state;
    pa_operation *op;
    int success = 0;

    if (pulse_ctx && PA_CONTEXT_IS_GOOD(pa_context_get_state(pulse_ctx)))
        return S_OK;
    if (pulse_ctx)
        pa_context_unref(pulse_ctx);

    pulse_ctx = pa_context_new(pa_mainloop_get_api(pulse_ml), name);
    setenv("PULSE_PROP_application.name", name, 1);
    if (!pulse_ctx) {
        ERR("Failed to create context\n");
        return E_FAIL;
    }

    pa_context_set_state_callback(pulse_ctx, pulse_contextcallback, NULL);
    pa_context_set_subscribe_callback(pulse_ctx, pulse_subscribe_callback, NULL);

    TRACE("libpulse protocol version: %u. API Version %u\n", pa_context_get_protocol_version(pulse_ctx), PA_API_VERSION);
    if (pa_context_connect(pulse_ctx, NULL, 0, NULL) < 0)
        goto fail;

    /* Wait for connection */
    while ((state = pa_context_get_state(pulse_ctx)) != PA_CONTEXT_READY &&
           state != PA_CONTEXT_FAILED && state != PA_CONTEXT_TERMINATED)
        pulse_cond_wait();

    if (state != PA_CONTEXT_READY)
        goto fail;

    TRACE("Connected to server %s with protocol version: %i.\n",
        pa_context_get_server(pulse_ctx),
        pa_context_get_server_protocol_version(pulse_ctx));

    if ((op = pa_context_subscribe(pulse_ctx, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
            | PA_SUBSCRIPTION_MASK_CARD,
            pulse_context_op_cb, &success)))
    {
        wait_pa_operation_complete(op);
        if (!success)
            WARN("Failed to subscribe to PulseAudio endpoint events.\n");
    }

    return S_OK;

fail:
    pa_context_unref(pulse_ctx);
    pulse_ctx = NULL;
    return E_FAIL;
}

static UINT pulse_channel_map_to_channel_mask(const pa_channel_map *map)
{
    int i;
    UINT mask = 0;

    for (i = 0; i < map->channels; ++i) {
        switch (map->map[i]) {
            default: FIXME("Unhandled channel %s\n", pa_channel_position_to_string(map->map[i])); break;
            case PA_CHANNEL_POSITION_AUX0:
            case PA_CHANNEL_POSITION_FRONT_LEFT: mask |= SPEAKER_FRONT_LEFT; break;
            case PA_CHANNEL_POSITION_MONO:
            case PA_CHANNEL_POSITION_FRONT_CENTER: mask |= SPEAKER_FRONT_CENTER; break;
            case PA_CHANNEL_POSITION_AUX1:
            case PA_CHANNEL_POSITION_FRONT_RIGHT: mask |= SPEAKER_FRONT_RIGHT; break;
            case PA_CHANNEL_POSITION_REAR_LEFT: mask |= SPEAKER_BACK_LEFT; break;
            case PA_CHANNEL_POSITION_REAR_CENTER: mask |= SPEAKER_BACK_CENTER; break;
            case PA_CHANNEL_POSITION_REAR_RIGHT: mask |= SPEAKER_BACK_RIGHT; break;
            case PA_CHANNEL_POSITION_LFE: mask |= SPEAKER_LOW_FREQUENCY; break;
            case PA_CHANNEL_POSITION_SIDE_LEFT: mask |= SPEAKER_SIDE_LEFT; break;
            case PA_CHANNEL_POSITION_SIDE_RIGHT: mask |= SPEAKER_SIDE_RIGHT; break;
            case PA_CHANNEL_POSITION_TOP_CENTER: mask |= SPEAKER_TOP_CENTER; break;
            case PA_CHANNEL_POSITION_TOP_FRONT_LEFT: mask |= SPEAKER_TOP_FRONT_LEFT; break;
            case PA_CHANNEL_POSITION_TOP_FRONT_CENTER: mask |= SPEAKER_TOP_FRONT_CENTER; break;
            case PA_CHANNEL_POSITION_TOP_FRONT_RIGHT: mask |= SPEAKER_TOP_FRONT_RIGHT; break;
            case PA_CHANNEL_POSITION_TOP_REAR_LEFT: mask |= SPEAKER_TOP_BACK_LEFT; break;
            case PA_CHANNEL_POSITION_TOP_REAR_CENTER: mask |= SPEAKER_TOP_BACK_CENTER; break;
            case PA_CHANNEL_POSITION_TOP_REAR_RIGHT: mask |= SPEAKER_TOP_BACK_RIGHT; break;
            case PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER: mask |= SPEAKER_FRONT_LEFT_OF_CENTER; break;
            case PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER: mask |= SPEAKER_FRONT_RIGHT_OF_CENTER; break;
        }
    }

    return mask;
}

#define MAX_DEVICE_NAME_LEN 62

static WCHAR *get_device_name(const char *desc, pa_proplist *proplist)
{
    /*
       Some broken apps (e.g. Split/Second with fmodex) can't handle names that
       are too long and crash even on native. If the device desc is too long,
       we'll attempt to incrementally build it to try to stay under the limit.
       ( + 1 is to check against truncated buffer after ntdll_umbstowcs )
    */
    WCHAR buf[MAX_DEVICE_NAME_LEN + 1];

    /* For monitors of sinks; this does not seem to be localized in PA either */
    static const WCHAR monitor_of[] = {'M','o','n','i','t','o','r',' ','o','f',' '};

    size_t len = strlen(desc);
    WCHAR *name, *tmp;

    if (!(name = malloc((len + 1) * sizeof(WCHAR))))
        return NULL;
    if (!(len = ntdll_umbstowcs(desc, len, name, len))) {
        free(name);
        return NULL;
    }

    if (len > MAX_DEVICE_NAME_LEN && proplist) {
        const char *prop = pa_proplist_gets(proplist, PA_PROP_DEVICE_CLASS);
        unsigned prop_len, rem = ARRAY_SIZE(buf);
        BOOL monitor = FALSE;

        if (prop && !strcmp(prop, "monitor")) {
            rem -= ARRAY_SIZE(monitor_of);
            monitor = TRUE;
        }

        prop = pa_proplist_gets(proplist, PA_PROP_DEVICE_PRODUCT_NAME);
        if (!prop || !prop[0] ||
            !(prop_len = ntdll_umbstowcs(prop, strlen(prop), buf, rem)) || prop_len == rem) {
            prop = pa_proplist_gets(proplist, "alsa.card_name");
            if (!prop || !prop[0] ||
                !(prop_len = ntdll_umbstowcs(prop, strlen(prop), buf, rem)) || prop_len == rem)
                prop = NULL;
        }

        if (prop) {
            /* We know we have a name that fits within the limit now */
            WCHAR *p = name;

            if (monitor) {
                memcpy(p, monitor_of, sizeof(monitor_of));
                p += ARRAY_SIZE(monitor_of);
            }
            len = ntdll_umbstowcs(prop, strlen(prop), p, rem);
            rem -= len;
            p += len;

            if (rem > 2) {
                rem--;  /* space */

                prop = pa_proplist_gets(proplist, PA_PROP_DEVICE_PROFILE_DESCRIPTION);
                if (prop && prop[0] && (len = ntdll_umbstowcs(prop, strlen(prop), p + 1, rem)) && len != rem) {
                    *p++ = ' ';
                    p += len;
                }
            }
            len = p - name;
        }
    }
    name[len] = '\0';

    if ((tmp = realloc(name, (len + 1) * sizeof(WCHAR))))
        name = tmp;
    return name;
}

#ifdef HAVE_UDEV
static void create_usb_dev_container_id(uint64_t usec_init, uint16_t vid, uint16_t pid, uint8_t bus_num, uint8_t dev_num,
        GUID *out)
{
    out->Data1 = MAKELONG(vid, pid);
    out->Data2 = bus_num;
    out->Data3 = dev_num;
    memcpy(out->Data4, &usec_init, sizeof(out->Data4));
}

static GUID get_container_id(const char *sysfs_path)
{
    struct udev_device *audio_dev, *usb_dev;
    struct udev *udev = udev_new();
    char buffer[4096] = "/sys";
    uint32_t vid, pid, version;
    uint8_t bus_num, dev_num;
    uint64_t init_time;
    const char *tmp;
    GUID tmp_guid;

    tmp_guid = GUID_NULL;
    strcat(buffer, sysfs_path);
    audio_dev = usb_dev = NULL;
    if (!udev)
    {
        ERR("Failed to get udev!\n");
        goto exit;
    }

    audio_dev = udev_device_new_from_syspath(udev, buffer);
    if (!audio_dev)
        goto exit;

    usb_dev = udev_device_get_parent_with_subsystem_devtype(audio_dev, "usb", "usb_device");
    TRACE("usb dev %p, udev %p.\n", usb_dev, udev);
    if (!usb_dev)
        goto exit;

    init_time = 0;
    bus_num = dev_num = 0;
    vid = pid = version = 0;
    if ((tmp = udev_device_get_property_value(usb_dev, "PRODUCT")))
        sscanf(tmp, "%x/%x/%x", &vid, &pid, &version);
    if ((tmp = udev_device_get_property_value(usb_dev, "USEC_INITIALIZED")))
        init_time = strtoull(tmp, NULL, 10);
    if ((tmp = udev_device_get_property_value(usb_dev, "BUSNUM")))
        bus_num = strtol(tmp, NULL, 10);
    if ((tmp = udev_device_get_property_value(usb_dev, "DEVNUM")))
        dev_num = strtol(tmp, NULL, 10);

    create_usb_dev_container_id(init_time, vid, pid, bus_num, dev_num, &tmp_guid);

exit:
    if (audio_dev)
        udev_device_unref(audio_dev);
    if (udev)
        udev_unref(udev);

    TRACE("Returning %s.\n", debugstr_guid(&tmp_guid));
    return tmp_guid;
}
#else
static GUID get_container_id(const char *sysfs_path)
{
    FIXME("No udev, can't get device data.\n");
    return GUID_NULL;
}
#endif


static void fill_device_info(PhysDevice *dev, pa_proplist *p)
{
    const char *buffer;

    dev->bus_type = phys_device_bus_invalid;
    dev->vendor_id = 0;
    dev->product_id = 0;
    dev->alsa_card = -1;
    memset(&dev->container_id, 0, sizeof(dev->container_id));

    if (!p)
        return;

    if ((buffer = pa_proplist_gets(p, PA_PROP_DEVICE_BUS))) {
        if (!strcmp(buffer, "usb"))
            dev->bus_type = phys_device_bus_usb;
        else if (!strcmp(buffer, "pci"))
            dev->bus_type = phys_device_bus_pci;
    }

    if ((buffer = pa_proplist_gets(p, PA_PROP_DEVICE_VENDOR_ID)))
        dev->vendor_id = strtol(buffer, NULL, 16);

    if ((buffer = pa_proplist_gets(p, PA_PROP_DEVICE_PRODUCT_ID)))
        dev->product_id = strtol(buffer, NULL, 16);

    if ((buffer = pa_proplist_gets(p, "alsa.card")) ||
            (buffer = pa_proplist_gets(p, "api.alsa.card")))
        dev->alsa_card = strtol(buffer, NULL, 10);

    if ((buffer = pa_proplist_gets(p, "sysfs.path")))
        dev->container_id = get_container_id(buffer);
}

static BOOL is_dualsense_audio_device(const PhysDevice *dev)
{
    return dev->bus_type == phys_device_bus_usb && dev->vendor_id == 0x054c
            && (dev->product_id == 0x05c4 || dev->product_id == 0x09cc ||
                dev->product_id == 0x0ce6 || dev->product_id == 0x0df2);
}

static BOOL is_dualshock_audio_device(const PhysDevice *dev)
{
    return dev->bus_type == phys_device_bus_usb && dev->vendor_id == 0x054c
            && (dev->product_id == 0x05c4 || dev->product_id == 0x09cc);
}

static BOOL is_dualshock_speaker_sink(const PhysDevice *dev)
{
    return dev->flow == eRender && is_dualshock_audio_device(dev) &&
            (strstr(dev->pulse_name, "Speaker__sink") || strstr(dev->pulse_name, "analog-stereo"));
}

static BOOL is_dualsense_speaker_sink(const PhysDevice *dev)
{
    return is_dualshock_speaker_sink(dev) ||
            (dev->flow == eRender && strstr(dev->pulse_name, "Speaker__sink")
            && (is_dualsense_audio_device(dev)
                || (strstr(dev->pulse_name, "alsa_output.usb-Sony_Interactive_Entertainment_")
                    && strstr(dev->pulse_name, "Wireless_Controller"))));
}

static BOOL use_death_stranding_controller_effects(void)
{
    const char *env = getenv("PROTON_DEATH_STRANDING_CONTROLLER_EFFECTS");

    return env && env[0] == '1' && !env[1];
}

static BOOL use_windows_sony_controller_names(void)
{
    const char *env = getenv("PROTON_SONY_WINDOWS_DEVICE_NAMES");

    return (env && env[0] == '1' && !env[1]) ||
            use_death_stranding_controller_effects() ||
            InterlockedCompareExchange(&g_sony_windows_audio_mode, 0, 0);
}

static BOOL use_pipewire_dualsense_haptic_target(void)
{
    const char *env = getenv("PROTON_DUALSENSE_HAPTICS_PREFER_NON_EVENT");

    return (env && env[0] == '1' && !env[1]) ||
            use_death_stranding_controller_effects() ||
            InterlockedCompareExchange(&g_sony_windows_audio_mode, 0, 0);
}

static BOOL use_dualsense_split_audio(void)
{
    const char *env = getenv("PROTON_DUALSENSE_SPLIT_AUDIO");

    return (env && env[0] == '1' && !env[1]) || use_death_stranding_controller_effects();
}

static char *make_pipewire_dualsense_haptic_path(const char *target)
{
    static const char prefix[] = "pipewire:NODE=";
    char *path;
    size_t len;

    if (!target || !target[0])
        return NULL;

    len = strlen(target) + sizeof(prefix);
    if ((path = malloc(len)))
        snprintf(path, len, "%s%s", prefix, target);
    return path;
}

static void apply_windows_sony_audio_format(PhysDevice *dev)
{
    WAVEFORMATEX *wfx = &dev->fmt.Format;

    if (!use_windows_sony_controller_names() || !is_dualsense_speaker_sink(dev))
        return;

    /* DS4 USB audio is stereo, not a split DualSense haptic endpoint. */
    if (is_dualshock_audio_device(dev))
        return;

    /* Windows exposes the DualSense USB audio function as a four-channel
     * endpoint. PipeWire's UCM profile splits out the controller speaker as a
     * mono sink, but games still use this endpoint for channels 3 and 4 of the
     * raw haptic stream. Keep the probed sample representation and restore the
     * Windows channel layout advertised to applications. */
    wfx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->nChannels = 4;
    wfx->nSamplesPerSec = 48000;
    wfx->nBlockAlign = wfx->nChannels * wfx->wBitsPerSample / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    dev->fmt.dwChannelMask = KSAUDIO_SPEAKER_QUAD;
}

static BOOL pulse_name_looks_like_dualsense_speaker_sink(const char *name)
{
    return name && strstr(name, "alsa_output.usb-Sony_Interactive_Entertainment_")
            && strstr(name, "Wireless_Controller") &&
            (strstr(name, "Speaker__sink") || strstr(name, "analog-stereo"));
}

static BOOL pulse_name_is_dualsense_speaker_sink(const char *name)
{
    PhysDevice *dev;
    BOOL ret = FALSE;

    if (!name)
        return FALSE;

    g_phys_lock();
    LIST_FOR_EACH_ENTRY(dev, &g_phys_speakers, PhysDevice, entry)
    {
        if (!pulse_device_matches(dev, name))
            continue;

        ret = is_dualsense_speaker_sink(dev);
        break;
    }
    g_phys_unlock();

    return ret || pulse_name_looks_like_dualsense_speaker_sink(name);
}

static BOOL is_dualsense_haptic_format(const struct pulse_stream *stream)
{
    return stream->dataflow == eRender && stream->ss.rate == 48000 && stream->ss.channels == 4 &&
            (stream->ss.format == PA_SAMPLE_FLOAT32LE || stream->ss.format == PA_SAMPLE_S16LE);
}

static BOOL string_contains_dualsense_name(const char *name)
{
    return name && (strstr(name, "DualSense") || strstr(name, "Wireless_Controller")
            || strstr(name, "Wireless Controller") || strstr(name, "Sony_Interactive_Entertainment"));
}

static BOOL pulse_stream_dualsense_mono_format(const struct pulse_stream *stream)
{
    return stream->dataflow == eRender && stream->ss.channels == 1;
}

#define DUALSENSE_MONO_ENDPOINT_ID \
    "alsa_output.usb-Sony_Interactive_Entertainment_Wireless_Controller-00.HiFi__Speaker__sink"
#define DUALSENSE_EDGE_MONO_ENDPOINT_ID \
    "alsa_output.usb-Sony_Interactive_Entertainment_DualSense_Edge_Wireless_Controller-00.Default__Speaker__sink"

static const char *sony_render_endpoint_base(const PhysDevice *dev)
{
    if (dev->flow != eRender || !is_dualsense_audio_device(dev))
        return NULL;

    if (dev->product_id == 0x0df2 ||
            strstr(dev->pulse_name, "DualSense_Edge_Wireless_Controller"))
        return DUALSENSE_EDGE_MONO_ENDPOINT_ID;

    return DUALSENSE_MONO_ENDPOINT_ID;
}

static BOOL sony_render_device_is_same_physical(const PhysDevice *a, const PhysDevice *b)
{
    if (!IsEqualGUID(&a->container_id, &GUID_NULL) &&
            !IsEqualGUID(&b->container_id, &GUID_NULL))
        return IsEqualGUID(&a->container_id, &b->container_id);

    return !strcmp(a->pulse_name, b->pulse_name);
}

static const char *find_sony_endpoint_for_physical_in_list(const PhysDevice *dev,
        const char *base, struct list *devices)
{
    PhysDevice *other;

    LIST_FOR_EACH_ENTRY(other, devices, PhysDevice, entry)
    {
        const char *other_base = sony_render_endpoint_base(other);

        if (other_base && !strcmp(other_base, base) && other->endpoint_id &&
                sony_render_device_is_same_physical(dev, other))
            return other->endpoint_id;
    }

    return NULL;
}

static BOOL sony_endpoint_conflicts_in_list(const PhysDevice *dev, const char *base,
        struct list *devices)
{
    PhysDevice *other;

    LIST_FOR_EACH_ENTRY(other, devices, PhysDevice, entry)
    {
        const char *other_base = sony_render_endpoint_base(other);

        if (other_base && !strcmp(other_base, base) &&
                !sony_render_device_is_same_physical(dev, other))
            return TRUE;
    }

    return FALSE;
}

static char *find_existing_sony_physical_endpoint(const PhysDevice *dev,
        const char *base, struct list *current)
{
    const char *endpoint;
    char *ret = NULL;

    if ((endpoint = find_sony_endpoint_for_physical_in_list(dev, base, current)))
        return strdup(endpoint);

    g_phys_lock();
    if (current != &g_phys_speakers &&
            (endpoint = find_sony_endpoint_for_physical_in_list(dev, base,
            &g_phys_speakers)))
        ret = strdup(endpoint);
    if (!ret && current != &g_phys_speakers_added &&
            (endpoint = find_sony_endpoint_for_physical_in_list(dev, base,
            &g_phys_speakers_added)))
        ret = strdup(endpoint);
    g_phys_unlock();

    return ret;
}

static BOOL sony_endpoint_has_physical_conflict(const PhysDevice *dev,
        const char *base, struct list *current)
{
    BOOL conflict;

    if (sony_endpoint_conflicts_in_list(dev, base, current))
        return TRUE;

    g_phys_lock();
    conflict = (current != &g_phys_speakers &&
            sony_endpoint_conflicts_in_list(dev, base, &g_phys_speakers)) ||
            (current != &g_phys_speakers_added &&
            sony_endpoint_conflicts_in_list(dev, base, &g_phys_speakers_added));
    g_phys_unlock();

    return conflict;
}

static char *make_physical_sony_endpoint_id(const PhysDevice *dev, const char *base)
{
    char *endpoint_id;
    size_t len;

    if (IsEqualGUID(&dev->container_id, &GUID_NULL))
        return strdup(dev->pulse_name);

    len = strlen(base) + sizeof("#wine-sony-physical-") + 36;
    if (!(endpoint_id = malloc(len)))
        return NULL;

    snprintf(endpoint_id, len,
            "%s#wine-sony-physical-%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            base, (unsigned int)dev->container_id.Data1,
            (unsigned int)dev->container_id.Data2,
            (unsigned int)dev->container_id.Data3,
            (unsigned int)dev->container_id.Data4[0],
            (unsigned int)dev->container_id.Data4[1],
            (unsigned int)dev->container_id.Data4[2],
            (unsigned int)dev->container_id.Data4[3],
            (unsigned int)dev->container_id.Data4[4],
            (unsigned int)dev->container_id.Data4[5],
            (unsigned int)dev->container_id.Data4[6],
            (unsigned int)dev->container_id.Data4[7]);
    return endpoint_id;
}

static PhysDevice *find_canonical_sony_endpoint_in_list(const PhysDevice *dev,
        const char *base, struct list *devices)
{
    PhysDevice *other;

    LIST_FOR_EACH_ENTRY(other, devices, PhysDevice, entry)
    {
        const char *other_base = sony_render_endpoint_base(other);

        if (other_base && !strcmp(other_base, base) && other->endpoint_id &&
                !strcmp(other->endpoint_id, base) &&
                !sony_render_device_is_same_physical(dev, other))
            return other;
    }

    return NULL;
}

static BOOL sony_audio_device_precedes(const PhysDevice *dev, const PhysDevice *other)
{
    return dev->alsa_card >= 0 &&
            (other->alsa_card < 0 || dev->alsa_card < other->alsa_card);
}

static BOOL pulse_set_endpoint_id(PhysDevice *dev, struct list *current)
{
    PhysDevice *canonical;
    const char *sony_base;
    char *existing;
    unsigned int generation;
    size_t len;

    if ((sony_base = sony_render_endpoint_base(dev)))
    {
        if ((existing = find_existing_sony_physical_endpoint(dev, sony_base, current)))
        {
            dev->endpoint_id = existing;
            return TRUE;
        }

        if (is_dualsense_speaker_sink(dev))
        {
            if ((canonical = find_canonical_sony_endpoint_in_list(dev, sony_base, current)) &&
                    sony_audio_device_precedes(dev, canonical))
            {
                char *canonical_id = strdup(sony_base);
                char *physical_id = make_physical_sony_endpoint_id(canonical, sony_base);

                if (!canonical_id || !physical_id)
                {
                    free(canonical_id);
                    free(physical_id);
                    return FALSE;
                }

                TRACE("Assigning canonical Sony endpoint %s to ALSA card %d instead of card %d.\n",
                        debugstr_a(sony_base), dev->alsa_card, canonical->alsa_card);
                free(canonical->endpoint_id);
                canonical->endpoint_id = physical_id;
                dev->endpoint_id = canonical_id;
                return TRUE;
            }

            if (sony_endpoint_has_physical_conflict(dev, sony_base, current))
                return !!(dev->endpoint_id = make_physical_sony_endpoint_id(dev, sony_base));

            return !!(dev->endpoint_id = strdup(sony_base));
        }
    }

    if (!is_dualsense_audio_device(dev) || (dev->flow == eRender && !dev->raw_haptic_target
            && strstr(dev->pulse_name, "Speaker__sink")))
        return !!(dev->endpoint_id = strdup(dev->pulse_name));

    if (!use_windows_sony_controller_names())
    {
        generation = ++g_dualsense_endpoint_generation;
        len = strlen(dev->pulse_name) + sizeof("#wine-dualsense-hotplug-") + 10;
        if (!(dev->endpoint_id = malloc(len)))
            return FALSE;

        snprintf(dev->endpoint_id, len, "%s#wine-dualsense-hotplug-%u",
                dev->pulse_name, generation);
        return TRUE;
    }

    len = strlen(dev->pulse_name) + sizeof("#wine-sony-hotplug-") + 36;
    if (!(dev->endpoint_id = malloc(len)))
        return FALSE;

    snprintf(dev->endpoint_id, len,
            "%s#wine-sony-hotplug-%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            dev->pulse_name, (unsigned int)dev->container_id.Data1,
            (unsigned int)dev->container_id.Data2, (unsigned int)dev->container_id.Data3,
            (unsigned int)dev->container_id.Data4[0], (unsigned int)dev->container_id.Data4[1],
            (unsigned int)dev->container_id.Data4[2], (unsigned int)dev->container_id.Data4[3],
            (unsigned int)dev->container_id.Data4[4], (unsigned int)dev->container_id.Data4[5],
            (unsigned int)dev->container_id.Data4[6], (unsigned int)dev->container_id.Data4[7]);
    return TRUE;
}

static void pulse_add_device(struct list *list, EDataFlow flow, pa_proplist *proplist, int index, EndpointFormFactor form,
                             UINT channel_mask, const char *pulse_name, const char *desc)
{
    size_t len = strlen(pulse_name);
    PhysDevice *dev = malloc(FIELD_OFFSET(PhysDevice, pulse_name[len + 1]));

    if (!dev)
        return;

    if (!(dev->name = get_device_name(desc, proplist))) {
        free(dev);
        return;
    }
    dev->flow = flow;
    dev->form = form;
    dev->index = index;
    dev->channel_mask = channel_mask;
    dev->def_period = 0;
    dev->min_period = 0;
    dev->raw_haptic_target = NULL;
    dev->raw_haptic_alsa_path = NULL;
    dev->endpoint_id = NULL;
    dev->ready = TRUE;
    dev->length = 0;
    dev->probe_status = -1;
    pthread_mutex_init(&dev->ready_mutex, NULL);
    pthread_cond_init(&dev->ready_cond, NULL);
    fill_device_info(dev, proplist);
    if (dev->flow == eRender && is_dualsense_audio_device(dev) && proplist)
    {
        const char *target = pa_proplist_gets(proplist, "api.alsa.split.name");
        const char *alsa_path = pa_proplist_gets(proplist, "api.alsa.path");

        if (target && target[0])
        {
            size_t target_len = strlen(target) + 1;

            if ((dev->raw_haptic_target = malloc(target_len)))
                memcpy(dev->raw_haptic_target, target, target_len);
        }
        else if (strstr(pulse_name, "Direct__Direct__sink") &&
                (channel_mask & KSAUDIO_SPEAKER_QUAD) == KSAUDIO_SPEAKER_QUAD)
        {
            size_t target_len = strlen(pulse_name) + 1;

            /* The Direct profile exposes the physical four-channel PCM as
             * this public PipeWire node rather than as a hidden split parent.
             * Use the real Pulse name, not Wine's generated endpoint id. */
            if ((dev->raw_haptic_target = malloc(target_len)))
                memcpy(dev->raw_haptic_target, pulse_name, target_len);
        }
        if (alsa_path && alsa_path[0])
        {
            size_t alsa_path_len = strlen(alsa_path) + 1;

            if ((dev->raw_haptic_alsa_path = malloc(alsa_path_len)))
                memcpy(dev->raw_haptic_alsa_path, alsa_path, alsa_path_len);
        }
    }
    memcpy(dev->pulse_name, pulse_name, len + 1);
    if (!pulse_set_endpoint_id(dev, list))
    {
        pulse_dev_free(dev);
        return;
    }

    list_add_tail(list, &dev->entry);

    TRACE("%s\n", debugstr_w(dev->name));
}

static void pulse_resolve_dualsense_split_targets(struct list *devices)
{
    PhysDevice *quad, *dev;
    char *parent, *target;

    LIST_FOR_EACH_ENTRY(quad, devices, PhysDevice, entry)
    {
        if (!is_dualsense_audio_device(quad) || !quad->raw_haptic_target ||
                (quad->channel_mask & KSAUDIO_SPEAKER_QUAD) != KSAUDIO_SPEAKER_QUAD ||
                !strcmp(quad->raw_haptic_target, quad->pulse_name))
            continue;
        if (!(parent = strdup(quad->raw_haptic_target)))
            continue;

        /* UCM split parents may be Audio/Sink/Internal. Prefer the public
         * quad sink for every endpoint of this exact split group, but leave
         * profiles without a quad sink on their existing raw path. */
        LIST_FOR_EACH_ENTRY(dev, devices, PhysDevice, entry)
        {
            if (!is_dualsense_audio_device(dev) || !dev->raw_haptic_target ||
                    strcmp(dev->raw_haptic_target, parent))
                continue;
            if (!(target = strdup(quad->pulse_name)))
                continue;
            TRACE("Routing Sony split endpoint %s through public quad sink %s instead of %s.\n",
                    debugstr_a(dev->pulse_name), debugstr_a(target), debugstr_a(parent));
            free(dev->raw_haptic_target);
            dev->raw_haptic_target = target;
        }
        free(parent);
    }
}

static void pulse_phys_speakers_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    struct list *speaker;
    UINT channel_mask;

    if (eol > 0)
    {
        pulse_resolve_dualsense_split_targets(userdata ? userdata : &g_phys_speakers);
        return;
    }
    if (!i || !i->name || !i->name[0])
        return;
    channel_mask = pulse_channel_map_to_channel_mask(&i->channel_map);

    /* For default PulseAudio render device, OR together all of the
     * PKEY_AudioEndpoint_PhysicalSpeakers values of the sinks. */
    speaker = list_head(userdata ? userdata : &g_phys_speakers);
    if (speaker)
        LIST_ENTRY(speaker, PhysDevice, entry)->channel_mask |= channel_mask;

    pulse_add_device(userdata ? userdata : &g_phys_speakers, eRender, i->proplist, i->index, Speakers, channel_mask,
            i->name, i->description);
}

static void pulse_phys_sources_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata)
{
    if (!i || !i->name || !i->name[0])
        return;
    pulse_add_device(userdata ? userdata : &g_phys_sources, eCapture, i->proplist, i->index,
        (i->monitor_of_sink == PA_INVALID_INDEX) ? Microphone : LineLevel, 0, i->name, i->description);
}

static PhysDevice *pulse_find_device(struct list *list, const char *pulse_name)
{
    PhysDevice *dev;

    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry)
        if (!strcmp(dev->pulse_name, pulse_name))
            return dev;

    return NULL;
}

static char *pulse_resolve_device_name(const char *device)
{
    static struct list *const lists[] =
    {
        &g_phys_speakers_added,
        &g_phys_speakers,
        &g_phys_sources_added,
        &g_phys_sources,
        NULL
    };
    struct list *const *list;
    PhysDevice *dev;
    char *ret = NULL;

    if (!device || !device[0])
        return NULL;

    g_phys_lock();
    for (list = lists; *list; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            if (!pulse_device_matches(dev, device))
                continue;
            ret = strdup(dev->pulse_name);
            break;
        }
        if (ret)
            break;
    }
    g_phys_unlock();
    return ret;
}

static void pulse_probe_device_list(pa_mainloop *ml, pa_context *ctx, struct list *list, BOOL render)
{
    PhysDevice *dev;

    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry)
    {
        pulse_probe_settings(ml, ctx, render, dev->pulse_name, &dev->fmt, &dev->def_period, &dev->min_period);
        apply_windows_sony_audio_format(dev);
    }
}

static BOOL pulse_context_wait_ready(pa_mainloop *ml, pa_context *ctx, const char *what)
{
    pa_usec_t deadline = pa_rtclock_now() + DUALSENSE_HAPTIC_REFRESH_USEC;
    int ret;

    while (pa_rtclock_now() < deadline)
    {
        pa_context_state_t state = pa_context_get_state(ctx);

        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            return FALSE;
        if (state == PA_CONTEXT_READY)
            return TRUE;

        if (pa_mainloop_iterate(ml, 0, &ret) < 0)
            return FALSE;
        poll(NULL, 0, 10);
    }

    TRACE("Timed out waiting for PulseAudio %s.\n", what);
    return FALSE;
}

static BOOL pulse_operation_wait_done(pa_mainloop *ml, pa_operation *op, const char *what)
{
    pa_usec_t deadline = pa_rtclock_now() + DUALSENSE_HAPTIC_REFRESH_USEC;
    int ret;

    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING && pa_rtclock_now() < deadline)
    {
        if (pa_mainloop_iterate(ml, 0, &ret) < 0)
            return FALSE;
        poll(NULL, 0, 10);
    }

    if (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
    {
        TRACE("Timed out waiting for PulseAudio %s.\n", what);
        return FALSE;
    }
    return TRUE;
}

static BOOL pulse_stream_wait_until_not(pa_mainloop *ml, pa_stream *stream, pa_stream_state_t wait_state, const char *what)
{
    pa_usec_t deadline = pa_rtclock_now() + DUALSENSE_HAPTIC_REFRESH_USEC;
    int ret;

    while (pa_stream_get_state(stream) == wait_state && pa_rtclock_now() < deadline)
    {
        if (pa_mainloop_iterate(ml, 0, &ret) < 0)
            return FALSE;
        poll(NULL, 0, 10);
    }

    if (pa_stream_get_state(stream) == wait_state)
    {
        TRACE("Timed out waiting for PulseAudio %s.\n", what);
        return FALSE;
    }
    return TRUE;
}

static BOOL pulse_refresh_devices(EDataFlow flow)
{
    struct list current = LIST_INIT(current);
    struct list *active = flow == eRender ? &g_phys_speakers : &g_phys_sources;
    struct list *added = flow == eRender ? &g_phys_speakers_added : &g_phys_sources_added;
    struct list *removed = flow == eRender ? &g_phys_speakers_removed : &g_phys_sources_removed;
    PhysDevice *dev, *next, *existing;
    pa_mainloop *ml;
    pa_context *ctx;
    pa_operation *o;
    BOOL find_dualsense_haptic_alsa = FALSE;
    BOOL retarget_haptic = flow == eRender && use_pipewire_dualsense_haptic_target();
    char *dualsense_mono_target = NULL;
    char *dualsense_haptic_target = NULL;
    char *dualsense_haptic_alsa_path = NULL;

    if (!(ml = pa_mainloop_new()))
        return FALSE;

    if (!(ctx = pa_context_new(pa_mainloop_get_api(ml), "winepulse device refresh")))
    {
        pa_mainloop_free(ml);
        return FALSE;
    }

    pa_context_set_state_callback(ctx, pulse_contextcallback, NULL);
    if (pa_context_connect(ctx, NULL, 0, NULL) < 0)
        goto fail;

    if (!pulse_context_wait_ready(ml, ctx, "device refresh context"))
        goto fail;

    if (flow == eRender)
        pulse_add_device(&current, eRender, NULL, 0, Speakers, 0, "", "PulseAudio Output");
    else
        pulse_add_device(&current, eCapture, NULL, 0, Microphone, 0, "", "PulseAudio Input");

    if (flow == eRender)
        o = pa_context_get_sink_info_list(ctx, pulse_phys_speakers_cb, &current);
    else
        o = pa_context_get_source_info_list(ctx, pulse_phys_sources_cb, &current);

    if (o)
    {
        if (!pulse_operation_wait_done(ml, o, "device list"))
        {
            pa_operation_unref(o);
            goto fail;
        }
        pa_operation_unref(o);
    }

    pulse_probe_device_list(ml, ctx, &current, flow == eRender);

    g_phys_lock();

    LIST_FOR_EACH_ENTRY_SAFE(dev, next, active, PhysDevice, entry)
    {
        if (pulse_find_device(&current, dev->pulse_name))
            continue;
        TRACE("PulseAudio endpoint %s was removed.\n", debugstr_a(dev->pulse_name));
        if (flow == eRender && (is_dualsense_audio_device(dev) || is_dualsense_speaker_sink(dev)))
        {
            ++g_haptic_hotplug_generation;
            if (is_dualsense_speaker_sink(dev))
            {
                ++g_dualsense_mono_speaker_add_generation;
            }
        }
        list_remove(&dev->entry);
        list_add_tail(removed, &dev->entry);
    }

    LIST_FOR_EACH_ENTRY_SAFE(dev, next, &current, PhysDevice, entry)
    {
        if ((existing = pulse_find_device(active, dev->pulse_name)) ||
                (existing = pulse_find_device(added, dev->pulse_name)))
        {
            /* A public quad sink can appear after its mono sibling without
             * changing that sibling's endpoint name or identity. */
            if (flow == eRender && is_dualsense_audio_device(dev))
            {
                free(existing->raw_haptic_target);
                free(existing->raw_haptic_alsa_path);
                existing->raw_haptic_target = dev->raw_haptic_target;
                existing->raw_haptic_alsa_path = dev->raw_haptic_alsa_path;
                dev->raw_haptic_target = NULL;
                dev->raw_haptic_alsa_path = NULL;
            }
            list_remove(&dev->entry);
            pulse_dev_free(dev);
            continue;
        }

        TRACE("PulseAudio endpoint %s was added.\n", debugstr_a(dev->pulse_name));
        if (flow == eRender && (is_dualsense_audio_device(dev) || is_dualsense_speaker_sink(dev)))
        {
            ++g_haptic_hotplug_generation;
            if (is_dualsense_speaker_sink(dev))
            {
                char *target = strdup(dev->pulse_name);

                ++g_dualsense_mono_speaker_add_generation;
                if (target)
                {
                    free(dualsense_mono_target);
                    dualsense_mono_target = target;
                }
            }
            if (retarget_haptic && dev->raw_haptic_target)
            {
                char *target = strdup(dev->raw_haptic_target);

                if (target)
                {
                    free(dualsense_haptic_target);
                    dualsense_haptic_target = target;
                }
            }
            else if (retarget_haptic && is_dualsense_speaker_sink(dev))
                find_dualsense_haptic_alsa = TRUE;
        }
        list_remove(&dev->entry);
        list_add_tail(added, &dev->entry);
    }

    g_phys_unlock();

    if (!dualsense_haptic_target && find_dualsense_haptic_alsa)
        dualsense_haptic_alsa_path = find_dualsense_haptic_alsa_path();

    if (dualsense_mono_target || dualsense_haptic_target || dualsense_haptic_alsa_path)
    {
        pulse_lock();
        if (dualsense_mono_target)
            pulse_retarget_dualsense_mono_streams(dualsense_mono_target);
        if (dualsense_haptic_target)
            pulse_retarget_dualsense_haptic_streams(dualsense_haptic_target);
        else if (dualsense_haptic_alsa_path)
            pulse_retarget_dualsense_haptic_streams_to_alsa(dualsense_haptic_alsa_path);
        pulse_unlock();
        free(dualsense_mono_target);
        free(dualsense_haptic_target);
        free(dualsense_haptic_alsa_path);
    }

    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    LIST_FOR_EACH_ENTRY_SAFE(dev, next, &current, PhysDevice, entry)
        pulse_dev_free(dev);

    pthread_cond_broadcast(&g_phys_cond);
    return TRUE;

fail:
    free(dualsense_mono_target);
    free(dualsense_haptic_target);
    free(dualsense_haptic_alsa_path);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
    LIST_FOR_EACH_ENTRY_SAFE(dev, next, &current, PhysDevice, entry)
        pulse_dev_free(dev);
    return FALSE;
}

/* For most hardware on Windows, users must choose a configuration with an even
 * number of channels (stereo, quad, 5.1, 7.1). Users can then disable
 * channels, but those channels are still reported to applications from
 * GetMixFormat! Some applications behave badly if given an odd number of
 * channels (e.g. 2.1).  Here, we find the nearest configuration that Windows
 * would report for a given channel layout. */
static void convert_channel_map(const pa_channel_map *pa_map, WAVEFORMATEXTENSIBLE *fmt)
{
    UINT pa_mask = pulse_channel_map_to_channel_mask(pa_map);

    TRACE("got mask for PA: 0x%x\n", pa_mask);

    if (pa_map->channels == 1)
    {
        fmt->Format.nChannels = 1;
        fmt->dwChannelMask = pa_mask;
        return;
    }

    /* compare against known configurations and find smallest configuration
     * which is a superset of the given speakers */

    if (pa_map->channels <= 2 &&
            (pa_mask & ~KSAUDIO_SPEAKER_STEREO) == 0)
    {
        fmt->Format.nChannels = 2;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        return;
    }

    if (pa_map->channels <= 4 &&
            (pa_mask & ~KSAUDIO_SPEAKER_QUAD) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_QUAD;
        return;
    }

    if (pa_map->channels <= 4 &&
            (pa_mask & ~KSAUDIO_SPEAKER_SURROUND) == 0)
    {
        fmt->Format.nChannels = 4;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_SURROUND;
        return;
    }

    if (pa_map->channels <= 6 &&
            (pa_mask & ~KSAUDIO_SPEAKER_5POINT1) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1;
        return;
    }

    if (pa_map->channels <= 6 &&
            (pa_mask & ~KSAUDIO_SPEAKER_5POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 6;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
        return;
    }

    if (pa_map->channels <= 8 &&
            (pa_mask & ~KSAUDIO_SPEAKER_7POINT1) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
        return;
    }

    if (pa_map->channels <= 8 &&
            (pa_mask & ~KSAUDIO_SPEAKER_7POINT1_SURROUND) == 0)
    {
        fmt->Format.nChannels = 8;
        fmt->dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
        return;
    }

    /* oddball format, report truthfully */
    fmt->Format.nChannels = pa_map->channels;
    fmt->dwChannelMask = pa_mask;
}

static void pulse_probe_settings(pa_mainloop *ml, pa_context *ctx, int render, const char *pulse_name,
                                 WAVEFORMATEXTENSIBLE *fmt, REFERENCE_TIME *def_period, REFERENCE_TIME *min_period)
{
    WAVEFORMATEX *wfx = &fmt->Format;
    pa_stream *stream;
    pa_channel_map map;
    pa_sample_spec ss;
    pa_buffer_attr attr;
    int ret;
    unsigned int length = 0;

    if (pulse_name && !pulse_name[0])
        pulse_name = NULL;

    pa_channel_map_init_auto(&map, 2, PA_CHANNEL_MAP_ALSA);
    ss.rate = 48000;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.channels = map.channels;

    attr.maxlength = -1;
    attr.tlength = -1;
    attr.minreq = attr.fragsize = pa_frame_size(&ss);
    attr.prebuf = 0;

    stream = pa_stream_new(ctx, "format test stream", &ss, &map);
    if (stream)
        pa_stream_set_state_callback(stream, pulse_stream_state, NULL);
    if (!stream)
        ret = -1;
    else if (render)
        ret = pa_stream_connect_playback(stream, pulse_name, &attr,
        PA_STREAM_START_CORKED|PA_STREAM_FIX_RATE|PA_STREAM_FIX_CHANNELS|PA_STREAM_EARLY_REQUESTS, NULL, NULL);
    else
        ret = pa_stream_connect_record(stream, pulse_name, &attr, PA_STREAM_START_CORKED|PA_STREAM_FIX_RATE|PA_STREAM_FIX_CHANNELS|PA_STREAM_EARLY_REQUESTS);
    if (ret >= 0) {
        pulse_stream_wait_until_not(ml, stream, PA_STREAM_CREATING, "format probe stream");
        if (pa_stream_get_state(stream) == PA_STREAM_READY) {
            ss = *pa_stream_get_sample_spec(stream);
            map = *pa_stream_get_channel_map(stream);
            if (render)
                length = pa_stream_get_buffer_attr(stream)->minreq;
            else
                length = pa_stream_get_buffer_attr(stream)->fragsize;
            pa_stream_disconnect(stream);
            pulse_stream_wait_until_not(ml, stream, PA_STREAM_READY, "format probe disconnect");
        }
        else if (pa_stream_get_state(stream) == PA_STREAM_CREATING)
            pa_stream_disconnect(stream);
    }

    if (stream)
        pa_stream_unref(stream);

    if (length)
        *def_period = *min_period = pa_bytes_to_usec(10 * length, &ss);

    wfx->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx->cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    convert_channel_map(&map, fmt);

    wfx->wBitsPerSample = 8 * pa_sample_size_of_format(ss.format);
    wfx->nSamplesPerSec = ss.rate;
    wfx->nBlockAlign = wfx->nChannels * wfx->wBitsPerSample / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    if (ss.format != PA_SAMPLE_S24_32LE)
        fmt->Samples.wValidBitsPerSample = wfx->wBitsPerSample;
    else
        fmt->Samples.wValidBitsPerSample = 24;
    if (ss.format == PA_SAMPLE_FLOAT32LE)
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    else
        fmt->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

/* some poorly-behaved applications call audio functions during DllMain, so we
 * have to do as much as possible without creating a new thread. this function
 * sets up a synchronous connection to verify the server is running and query
 * static data. */
static NTSTATUS pulse_test_connect(void *args)
{
    struct test_connect_params *params = args;
    PhysDevice *dev;
    pa_operation *o;
    int ret;
    char *name = wstr_to_str(params->name);
    pa_mainloop *ml;
    pa_context *ctx;

    pulse_lock();
    ml = pa_mainloop_new();

    pa_mainloop_set_poll_func(ml, pulse_poll_func, NULL);

    ctx = pa_context_new(pa_mainloop_get_api(ml), name);
    free(name);
    if (!ctx) {
        ERR("Failed to create context\n");
        pa_mainloop_free(ml);
        pulse_unlock();
        params->priority = Priority_Unavailable;
        return STATUS_SUCCESS;
    }

    pa_context_set_state_callback(ctx, pulse_contextcallback, NULL);

    TRACE("libpulse protocol version: %u. API Version %u\n", pa_context_get_protocol_version(ctx), PA_API_VERSION);
    if (pa_context_connect(ctx, NULL, 0, NULL) < 0)
        goto fail;

    /* Wait for connection */
    while (pa_mainloop_iterate(ml, 1, &ret) >= 0) {
        pa_context_state_t state = pa_context_get_state(ctx);

        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            goto fail;

        if (state == PA_CONTEXT_READY)
            break;
    }

    if (pa_context_get_state(ctx) != PA_CONTEXT_READY)
        goto fail;

    TRACE("Test-connected to server %s with protocol version: %i.\n",
        pa_context_get_server(ctx),
        pa_context_get_server_protocol_version(ctx));

    free_phys_device_lists();
    list_init(&g_phys_speakers);
    list_init(&g_phys_sources);

    /* Burnout Paradise Remastered expects device name to have a space. */
    pulse_add_device(&g_phys_speakers, eRender, NULL, 0, Speakers, 0, "", "PulseAudio Output");
    pulse_add_device(&g_phys_sources, eCapture, NULL, 0, Microphone, 0, "", "PulseAudio Input");

    o = pa_context_get_sink_info_list(ctx, &pulse_phys_speakers_cb, NULL);
    if (o) {
        while (pa_mainloop_iterate(ml, 1, &ret) >= 0 &&
                pa_operation_get_state(o) == PA_OPERATION_RUNNING)
        {}
        pa_operation_unref(o);
    }

    o = pa_context_get_source_info_list(ctx, &pulse_phys_sources_cb, NULL);
    if (o) {
        while (pa_mainloop_iterate(ml, 1, &ret) >= 0 &&
                pa_operation_get_state(o) == PA_OPERATION_RUNNING)
        {}
        pa_operation_unref(o);
    }

    LIST_FOR_EACH_ENTRY(dev, &g_phys_speakers, PhysDevice, entry) {
        pulse_probe_settings(ml, ctx, 1, dev->pulse_name, &dev->fmt, &dev->def_period, &dev->min_period);
        apply_windows_sony_audio_format(dev);
    }

    LIST_FOR_EACH_ENTRY(dev, &g_phys_sources, PhysDevice, entry) {
        pulse_probe_settings(ml, ctx, 0, dev->pulse_name, &dev->fmt, &dev->def_period, &dev->min_period);
    }

    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    pulse_unlock();

    params->priority = Priority_Preferred;
    return STATUS_SUCCESS;

fail:
    pa_context_unref(ctx);
    pa_mainloop_free(ml);
    pulse_unlock();
    params->priority = Priority_Unavailable;
    return STATUS_SUCCESS;
}

static UINT get_channel_mask(unsigned int channels)
{
    switch(channels) {
    case 0:
        return 0;
    case 1:
        return KSAUDIO_SPEAKER_MONO;
    case 2:
        return KSAUDIO_SPEAKER_STEREO;
    case 3:
        return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:
        return KSAUDIO_SPEAKER_QUAD;    /* not _SURROUND */
    case 5:
        return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:
        return KSAUDIO_SPEAKER_5POINT1; /* not 5POINT1_SURROUND */
    case 7:
        return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:
        return KSAUDIO_SPEAKER_7POINT1_SURROUND; /* Vista deprecates 7POINT1 */
    }
    FIXME("Unknown speaker configuration: %u\n", channels);
    return 0;
}

static const enum pa_channel_position pulse_pos_from_wfx[] = {
    PA_CHANNEL_POSITION_FRONT_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER,
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
    PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
    PA_CHANNEL_POSITION_REAR_CENTER,
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,
    PA_CHANNEL_POSITION_TOP_CENTER,
    PA_CHANNEL_POSITION_TOP_FRONT_LEFT,
    PA_CHANNEL_POSITION_TOP_FRONT_CENTER,
    PA_CHANNEL_POSITION_TOP_FRONT_RIGHT,
    PA_CHANNEL_POSITION_TOP_REAR_LEFT,
    PA_CHANNEL_POSITION_TOP_REAR_CENTER,
    PA_CHANNEL_POSITION_TOP_REAR_RIGHT,
    PA_CHANNEL_POSITION_AUX0,
    PA_CHANNEL_POSITION_AUX1
};

static HRESULT pulse_spec_from_waveformat(struct pulse_stream *stream, const WAVEFORMATEX *fmt)
{
    pa_channel_map_init(&stream->map);
    stream->ss.rate = fmt->nSamplesPerSec;
    stream->ss.format = PA_SAMPLE_INVALID;

    switch(fmt->wFormatTag) {
    case WAVE_FORMAT_IEEE_FLOAT:
        if (!fmt->nChannels || fmt->nChannels > 2 || fmt->wBitsPerSample != 32)
            break;
        stream->ss.format = PA_SAMPLE_FLOAT32LE;
        pa_channel_map_init_auto(&stream->map, fmt->nChannels, PA_CHANNEL_MAP_ALSA);
        break;
    case WAVE_FORMAT_PCM:
        if (!fmt->nChannels || fmt->nChannels > 2)
            break;
        if (fmt->wBitsPerSample == 8)
            stream->ss.format = PA_SAMPLE_U8;
        else if (fmt->wBitsPerSample == 16)
            stream->ss.format = PA_SAMPLE_S16LE;
        else if (fmt->wBitsPerSample == 32)
            stream->ss.format = PA_SAMPLE_S32LE;
        else
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        pa_channel_map_init_auto(&stream->map, fmt->nChannels, PA_CHANNEL_MAP_ALSA);
        break;
    case WAVE_FORMAT_EXTENSIBLE: {
        WAVEFORMATEXTENSIBLE *wfe = (WAVEFORMATEXTENSIBLE*)fmt;
        UINT mask = wfe->dwChannelMask;
        unsigned i = 0, j;
        if (fmt->cbSize != (sizeof(*wfe) - sizeof(*fmt)) && fmt->cbSize != sizeof(*wfe))
            break;
        if (IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
            (!wfe->Samples.wValidBitsPerSample || wfe->Samples.wValidBitsPerSample == 32) &&
            fmt->wBitsPerSample == 32)
            stream->ss.format = PA_SAMPLE_FLOAT32LE;
        else if (IsEqualGUID(&wfe->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM)) {
            DWORD valid = wfe->Samples.wValidBitsPerSample;
            if (!valid)
                valid = fmt->wBitsPerSample;
            if (!valid || valid > fmt->wBitsPerSample)
                break;
            switch (fmt->wBitsPerSample) {
                case 8:
                    if (valid == 8)
                        stream->ss.format = PA_SAMPLE_U8;
                    break;
                case 16:
                    if (valid == 16)
                        stream->ss.format = PA_SAMPLE_S16LE;
                    break;
                case 24:
                    if (valid == 24)
                        stream->ss.format = PA_SAMPLE_S24LE;
                    break;
                case 32:
                    if (valid == 24)
                        stream->ss.format = PA_SAMPLE_S24_32LE;
                    else if (valid == 32)
                        stream->ss.format = PA_SAMPLE_S32LE;
                    break;
                default:
                    return AUDCLNT_E_UNSUPPORTED_FORMAT;
            }
        }
        stream->map.channels = fmt->nChannels;
        if (!mask || (mask & (SPEAKER_ALL|SPEAKER_RESERVED)))
            mask = get_channel_mask(fmt->nChannels);
        for (j = 0; j < ARRAY_SIZE(pulse_pos_from_wfx) && i < fmt->nChannels; ++j) {
            if (mask & (1 << j))
                stream->map.map[i++] = pulse_pos_from_wfx[j];
        }

        /* Special case for mono since pulse appears to map it differently */
        if (mask == SPEAKER_FRONT_CENTER)
            stream->map.map[0] = PA_CHANNEL_POSITION_MONO;

        if (i < fmt->nChannels || (mask & SPEAKER_RESERVED)) {
            stream->map.channels = 0;
            ERR("Invalid channel mask: %i/%i and %x(%x)\n", i, fmt->nChannels, mask, (unsigned)wfe->dwChannelMask);
            break;
        }
        break;
        }
    case WAVE_FORMAT_ALAW:
    case WAVE_FORMAT_MULAW:
        if (fmt->wBitsPerSample != 8) {
            FIXME("Unsupported bpp %u for LAW\n", fmt->wBitsPerSample);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        if (fmt->nChannels != 1 && fmt->nChannels != 2) {
            FIXME("Unsupported channels %u for LAW\n", fmt->nChannels);
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        stream->ss.format = fmt->wFormatTag == WAVE_FORMAT_MULAW ? PA_SAMPLE_ULAW : PA_SAMPLE_ALAW;
        pa_channel_map_init_auto(&stream->map, fmt->nChannels, PA_CHANNEL_MAP_ALSA);
        break;
    default:
        WARN("Unhandled tag %x\n", fmt->wFormatTag);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    stream->ss.channels = stream->map.channels;
    if (!pa_channel_map_valid(&stream->map) || stream->ss.format == PA_SAMPLE_INVALID) {
        ERR("Invalid format! Channel spec valid: %i, format: %i\n",
            pa_channel_map_valid(&stream->map), stream->ss.format);
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    return S_OK;
}

static HRESULT pulse_stream_connect_timeout(struct pulse_stream *stream, const char *pulse_name, const char *target_object,
        UINT32 period_bytes, unsigned int timeout_ms, BOOL allow_local_render_fallback)
{
    pa_stream_flags_t flags = PA_STREAM_START_CORKED | PA_STREAM_START_UNMUTED | PA_STREAM_ADJUST_LATENCY;
    pa_proplist *proplist = NULL;
    int ret;
    char buffer[64];
    static LONG number;
    pa_buffer_attr attr;

    ret = InterlockedIncrement(&number);
    sprintf(buffer, "audio stream #%i", ret);

    if (target_object && target_object[0])
    {
        if ((proplist = pa_proplist_new()))
        {
            pa_proplist_sets(proplist, "target.object", target_object);
            pa_proplist_sets(proplist, "node.target", target_object);
            pa_proplist_sets(proplist, "media.class", "Stream/Output/Audio/Internal");
            pa_proplist_sets(proplist, "node.dont-fallback", "true");
            pa_proplist_sets(proplist, "stream.dont-remix", "true");
        }
    }

    stream->stream = proplist
            ? pa_stream_new_with_proplist(pulse_ctx, buffer, &stream->ss, &stream->map, proplist)
            : pa_stream_new(pulse_ctx, buffer, &stream->ss, &stream->map);
    if (proplist)
        pa_proplist_free(proplist);

    if (!stream->stream) {
        WARN("pa_stream_new returned error %i\n", pa_context_errno(pulse_ctx));
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    pa_stream_set_state_callback(stream->stream, pulse_stream_state, stream);
    pa_stream_set_buffer_attr_callback(stream->stream, pulse_attr_update, stream);
    pa_stream_set_moved_callback(stream->stream, pulse_attr_update, stream);

    /* PulseAudio will fill in correct values */
    attr.minreq = attr.fragsize = period_bytes;
    attr.tlength = period_bytes * 3;
    attr.maxlength = stream->bufsize_frames * pa_frame_size(&stream->ss);
    attr.prebuf = 0;
    stream->attr = attr;
    dump_attr(&attr);

    /* If specific device was requested, use it exactly. For PipeWire raw
     * haptic targets, keep the public DualSense sink as the Pulse device and
     * let target.object select the hidden split ALSA node. */
    if (pulse_name && pulse_name[0])
        flags |= PA_STREAM_DONT_MOVE;
    else
        pulse_name = NULL;  /* use default */

    if (stream->dataflow == eRender)
        ret = pa_stream_connect_playback(stream->stream, pulse_name, &attr, flags|PA_STREAM_VARIABLE_RATE, NULL, NULL);
    else
        ret = pa_stream_connect_record(stream->stream, pulse_name, &attr, flags);
    if (ret < 0) {
        WARN("Returns %i\n", ret);
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }
    while (stream->stream && pa_stream_get_state(stream->stream) == PA_STREAM_CREATING)
    {
        if (pulse_cond_timedwait_ms(timeout_ms) == ETIMEDOUT)
            break;
    }
    if (!stream->stream)
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    if (pa_stream_get_state(stream->stream) != PA_STREAM_READY)
    {
        if (allow_local_render_fallback && stream->dataflow == eRender
                && pa_stream_get_state(stream->stream) == PA_STREAM_CREATING &&
                !(target_object && target_object[0]))
        {
            WARN("Render stream still creating; disconnecting Pulse stream and continuing locally.\n");
            pa_stream_disconnect(stream->stream);
            pa_stream_unref(stream->stream);
            stream->stream = NULL;
            return S_OK;
        }
        WARN("Stream failed to become ready, state %i\n", pa_stream_get_state(stream->stream));
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    if (stream->dataflow == eRender) {
        pa_stream_set_underflow_callback(stream->stream, pulse_underflow_callback, stream);
        pa_stream_set_started_callback(stream->stream, pulse_started_callback, stream);
    }
    return S_OK;
}

static HRESULT pulse_stream_connect(struct pulse_stream *stream, const char *pulse_name, const char *target_object,
        UINT32 period_bytes)
{
    return pulse_stream_connect_timeout(stream, pulse_name, target_object, period_bytes,
            target_object && target_object[0] ? 5000 : 500, TRUE);
}

static HRESULT pulse_stream_connect_dualsense_speaker(struct pulse_stream *stream, const char *pulse_name,
        UINT32 period_bytes)
{
    return pulse_stream_connect_timeout(stream, pulse_name, NULL, period_bytes, 5000, FALSE);
}

static BOOL pulse_split_speaker_ready(const struct pulse_stream *stream)
{
    return stream->speaker_stream && pa_stream_get_state(stream->speaker_stream) == PA_STREAM_READY;
}

static void pulse_split_speaker_drain(struct pulse_stream *stream)
{
    SIZE_T bytes, sample_size, writable;

    if (!pulse_split_speaker_ready(stream) || !stream->speaker_buffer_held)
        return;

    sample_size = pa_sample_size_of_format(stream->ss.format);
    writable = pa_stream_writable_size(stream->speaker_stream);
    if (!sample_size || writable == (SIZE_T)-1)
        return;

    bytes = min(stream->speaker_buffer_held, writable);
    bytes -= bytes % sample_size;
    if (!bytes)
        return;

    if (pa_stream_write(stream->speaker_stream, stream->speaker_buffer, bytes,
            NULL, 0, PA_SEEK_RELATIVE) < 0)
    {
        WARN("Failed to write queued split DualSense speaker audio: %d.\n",
                pa_context_errno(pulse_ctx));
        return;
    }

    stream->speaker_buffer_held -= bytes;
    if (stream->speaker_buffer_held)
        memmove(stream->speaker_buffer, stream->speaker_buffer + bytes,
                stream->speaker_buffer_held);
}

static void pulse_split_speaker_write_callback(pa_stream *s, size_t bytes, void *user)
{
    struct pulse_stream *stream = user;

    if (stream && s == stream->speaker_stream && bytes)
        pulse_split_speaker_drain(stream);
}

static void pulse_split_speaker_disconnect(struct pulse_stream *stream)
{
    stream->speaker_buffer_held = 0;
    stream->speaker_dropped_bytes = 0;
    stream->speaker_peak = 0.0f;
    stream->speaker_trace_time = 0;
    stream->speaker_route_selected = FALSE;

    if (!stream->speaker_stream)
        return;

    if (PA_STREAM_IS_GOOD(pa_stream_get_state(stream->speaker_stream)))
        pa_stream_disconnect(stream->speaker_stream);
    pa_stream_unref(stream->speaker_stream);
    stream->speaker_stream = NULL;
}

static HRESULT pulse_split_speaker_connect(struct pulse_stream *stream, const char *pulse_name)
{
    pa_stream_flags_t flags = PA_STREAM_START_CORKED | PA_STREAM_START_UNMUTED |
            PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE;
    pa_sample_spec ss = stream->ss;
    pa_channel_map map;
    pa_cvolume volume;
    pa_buffer_attr attr;
    UINT32 source_frame_size, speaker_frame_size, period_frames;
    char name[64];
    static LONG number;
    char *device;
    int ret;

    if (!pulse_name || !pulse_name[0])
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;

    source_frame_size = pa_frame_size(&stream->ss);
    ss.channels = 1;
    pa_channel_map_init_mono(&map);
    speaker_frame_size = pa_frame_size(&ss);
    if (!source_frame_size || !speaker_frame_size)
        return AUDCLNT_E_UNSUPPORTED_FORMAT;

    period_frames = stream->period_bytes / source_frame_size;
    attr.minreq = attr.fragsize = period_frames * speaker_frame_size;
    attr.tlength = attr.minreq * 3;
    attr.maxlength = stream->bufsize_frames * speaker_frame_size;
    attr.prebuf = 0;

    if (!(device = strdup(pulse_name)))
        return E_OUTOFMEMORY;

    pulse_split_speaker_disconnect(stream);
    snprintf(name, sizeof(name), "DualSense speaker #%d", InterlockedIncrement(&number));
    if (!(stream->speaker_stream = pa_stream_new(pulse_ctx, name, &ss, &map)))
    {
        free(device);
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    pa_stream_set_state_callback(stream->speaker_stream, pulse_split_speaker_state, stream);
    pa_stream_set_write_callback(stream->speaker_stream,
            pulse_split_speaker_write_callback, stream);
    pa_cvolume_set(&volume, ss.channels, PA_VOLUME_NORM);
    ret = pa_stream_connect_playback(stream->speaker_stream, pulse_name, &attr, flags, &volume, NULL);
    if (ret < 0)
        goto failed;

    while (pa_stream_get_state(stream->speaker_stream) == PA_STREAM_CREATING)
    {
        if (pulse_cond_timedwait_ms(5000) == ETIMEDOUT)
            break;
    }
    if (!pulse_split_speaker_ready(stream))
        goto failed;

    free(stream->speaker_device);
    stream->speaker_device = device;
    TRACE("Connected split DualSense speaker stream %p to %s.\n",
            stream, debugstr_a(stream->speaker_device));
    return S_OK;

failed:
    WARN("Failed to connect split DualSense speaker stream to %s: %d.\n",
            debugstr_a(pulse_name), pa_context_errno(pulse_ctx));
    pulse_split_speaker_disconnect(stream);
    free(device);
    return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

static void pulse_split_speaker_set_corked(struct pulse_stream *stream, BOOL corked)
{
    pa_operation *op;

    if (!pulse_split_speaker_ready(stream) || pa_stream_is_corked(stream->speaker_stream) == corked)
        return;

    if ((op = pa_stream_cork(stream->speaker_stream, corked, NULL, NULL)))
        pa_operation_unref(op);
}

static HRESULT get_device_period_helper(EDataFlow flow, const char *pulse_name, REFERENCE_TIME *def, REFERENCE_TIME *min)
{
    struct list *list = (flow == eRender) ? &g_phys_speakers : &g_phys_sources;
    PhysDevice *dev;

    if (!def && !min) {
        return E_POINTER;
    }

    g_phys_lock();
    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry) {
        if (!pulse_device_matches(dev, pulse_name))
            continue;

        if (def)
            *def = dev->def_period;
        if (min)
            *min = dev->min_period;

        g_phys_unlock();
        return S_OK;
    }
    g_phys_unlock();

    return E_FAIL;
}

static char *get_dualsense_haptic_target(const char *pulse_name, const struct pulse_stream *stream)
{
    PhysDevice *dev;

    if (stream->dataflow != eRender || (stream->ss.format != PA_SAMPLE_FLOAT32LE
            && (!use_dualsense_split_audio() || stream->ss.format != PA_SAMPLE_S16LE))
            || stream->ss.rate != 48000 || stream->ss.channels != 4 || !pulse_name || !pulse_name[0])
        return NULL;

    g_phys_lock();
    LIST_FOR_EACH_ENTRY(dev, &g_phys_speakers, PhysDevice, entry)
    {
        if (!pulse_device_matches(dev, pulse_name))
            continue;

        if (dev->raw_haptic_target)
        {
            size_t len = strlen(dev->raw_haptic_target) + 1;
            char *target = malloc(len);

            if (target)
                memcpy(target, dev->raw_haptic_target, len);

            g_phys_unlock();
            return target;
        }
        break;
    }
    g_phys_unlock();
    return NULL;
}

static BOOL dualsense_haptic_pcm_path_works(const char *path);

static char *get_dualsense_haptic_alsa_path(const char *pulse_name, const struct pulse_stream *stream)
{
    PhysDevice *dev, *fallback = NULL;
    char *path = NULL;

    if (stream->dataflow != eRender || stream->ss.format != PA_SAMPLE_FLOAT32LE
            || stream->ss.rate != 48000 || stream->ss.channels != 4 || !pulse_name || !pulse_name[0])
        return NULL;

    g_phys_lock();
    LIST_FOR_EACH_ENTRY(dev, &g_phys_speakers, PhysDevice, entry)
    {
        if (!pulse_device_matches(dev, pulse_name))
        {
            if (!fallback && is_dualsense_audio_device(dev) && dev->raw_haptic_alsa_path)
                fallback = dev;
            continue;
        }

        if (dev->raw_haptic_alsa_path && dualsense_haptic_pcm_path_works(dev->raw_haptic_alsa_path))
        {
            size_t len = strlen(dev->raw_haptic_alsa_path) + 1;

            if (path)
                free(path);
            if ((path = malloc(len)))
                memcpy(path, dev->raw_haptic_alsa_path, len);
        }
        break;
    }
    if (!path && fallback && dualsense_haptic_pcm_path_works(fallback->raw_haptic_alsa_path))
    {
        size_t len = strlen(fallback->raw_haptic_alsa_path) + 1;

        if ((path = malloc(len)))
            memcpy(path, fallback->raw_haptic_alsa_path, len);
    }
    g_phys_unlock();
    return path;
}

static char *get_dualsense_speaker_sink(const char *preferred, BOOL require_speaker)
{
    static struct list *const lists[] = { &g_phys_speakers_added, &g_phys_speakers, NULL };
    struct list *const *list;
    PhysDevice *dev;
    char *fallback = NULL, *direct_fallback = NULL;

    g_phys_lock();
    for (list = lists; *list; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            BOOL direct, speaker;

            speaker = is_dualsense_speaker_sink(dev);
            if (!speaker && !is_dualsense_audio_device(dev))
                continue;

            direct = strstr(dev->pulse_name, "Direct__Direct__sink") || (dev->raw_haptic_target && !speaker);
            if (require_speaker && !speaker)
                continue;

            if (preferred && pulse_device_matches(dev, preferred) && !direct)
            {
                char *ret = strdup(dev->pulse_name);

                g_phys_unlock();
                free(fallback);
                free(direct_fallback);
                return ret;
            }

            if (!direct && (!fallback || speaker))
            {
                free(fallback);
                fallback = strdup(dev->pulse_name);
            }
            else if (!require_speaker && direct && !direct_fallback)
                direct_fallback = strdup(dev->pulse_name);
        }
    }
    g_phys_unlock();
    if (preferred)
    {
        free(fallback);
        free(direct_fallback);
        return NULL;
    }
    if (fallback)
    {
        free(direct_fallback);
        return fallback;
    }
    return direct_fallback;
}

static char *pulse_find_shared_sony_speaker_survivor(const PhysDevice *removed,
        struct list *active, struct list *added)
{
    struct list *const lists[] = { added, active, NULL };
    struct list *const *list;
    PhysDevice *dev;

    if (!is_dualsense_speaker_sink(removed) ||
            IsEqualGUID(&removed->container_id, &GUID_NULL))
        return NULL;

    for (list = lists; *list; ++list)
    {
        LIST_FOR_EACH_ENTRY(dev, *list, PhysDevice, entry)
        {
            if (!is_dualsense_speaker_sink(dev) ||
                    IsEqualGUID(&dev->container_id, &GUID_NULL) ||
                    IsEqualGUID(&dev->container_id, &removed->container_id) ||
                    strcmp(pulse_endpoint_id(dev), pulse_endpoint_id(removed)))
                continue;

            return strdup(dev->pulse_name);
        }
    }

    return NULL;
}

static BOOL pulse_stream_dualsense_mono(const struct pulse_stream *stream)
{
    return pulse_stream_dualsense_mono_format(stream) && string_contains_dualsense_name(stream->device);
}

static void pulse_stream_disconnect(struct pulse_stream *stream)
{
    if (!stream->stream)
        return;

    if (PA_STREAM_IS_GOOD(pa_stream_get_state(stream->stream)))
        pa_stream_disconnect(stream->stream);
    pa_stream_unref(stream->stream);
    stream->stream = NULL;
    stream->pa_started = FALSE;
    stream->update_timing_info_pending = FALSE;
}

static void pulse_set_dualsense_mono_preferred_sink(const char *preferred)
{
    char *copy;

    if (!preferred)
    {
        free(g_dualsense_mono_preferred_sink);
        g_dualsense_mono_preferred_sink = NULL;
        return;
    }

    if (!(copy = strdup(preferred)))
    {
        WARN("Failed to remember preferred Sony controller speaker sink %s.\n", debugstr_a(preferred));
        return;
    }

    free(g_dualsense_mono_preferred_sink);
    g_dualsense_mono_preferred_sink = copy;
}

static HRESULT pulse_retarget_dualsense_mono_stream(struct pulse_stream *stream, const char *preferred)
{
    GUID target_container_id;
    char *dualsense_speaker;
    const pa_buffer_attr *attr;
    BOOL target_container_valid;
    HRESULT hr;
    int success;

    if (!stream->dualsense_mono_registered && !pulse_stream_dualsense_mono(stream))
        return S_FALSE;

    if (!(dualsense_speaker = get_dualsense_speaker_sink(preferred, TRUE)))
        if (!(dualsense_speaker = get_dualsense_speaker_sink(NULL, TRUE)))
            return E_FAIL;

    target_container_valid = pulse_get_device_container_id(dualsense_speaker,
            &target_container_id);

    pulse_set_dualsense_mono_preferred_sink(dualsense_speaker);

    if (target_container_valid && stream->sony_controller_container_valid &&
            !IsEqualGUID(&stream->sony_controller_container_id, &target_container_id) &&
            pulse_container_is_present(&stream->sony_controller_container_id))
    {
        TRACE("Keeping Sony controller mono stream %p on live container %s instead of %s.\n",
                stream, debugstr_guid(&stream->sony_controller_container_id),
                debugstr_guid(&target_container_id));
        stream->dualsense_mono_hotplug_generation = g_dualsense_mono_speaker_add_generation;
        stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
        free(dualsense_speaker);
        return S_FALSE;
    }

    if (stream->device && !strcmp(stream->device, dualsense_speaker) && pulse_stream_ready(stream))
    {
        if (target_container_valid)
        {
            stream->sony_controller_container_id = target_container_id;
            stream->sony_controller_container_valid = TRUE;
        }
        stream->dualsense_mono_hotplug_generation = g_dualsense_mono_speaker_add_generation;
        stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
        free(dualsense_speaker);
        return S_FALSE;
    }

    TRACE("Retargeting Sony controller mono stream %p from %s to %s.\n",
            stream, debugstr_a(stream->device), debugstr_a(dualsense_speaker));

    pulse_stream_disconnect(stream);
    free(stream->device);
    stream->device = dualsense_speaker;
    stream->timeline_start_period_time = 0;
    stream->timeline_start_stream_time = 0;
    stream->update_timing_info_pending = FALSE;

    hr = pulse_stream_connect_dualsense_speaker(stream, stream->device, stream->period_bytes);
    if (FAILED(hr))
    {
        WARN("Failed to retarget Sony controller mono stream %p to %s: %#x.\n",
                stream, debugstr_a(stream->device), (unsigned int)hr);
        return hr;
    }

    if ((attr = pa_stream_get_buffer_attr(stream->stream)))
        stream->attr = *attr;

    if (target_container_valid)
    {
        stream->sony_controller_container_id = target_container_id;
        stream->sony_controller_container_valid = TRUE;
    }

    stream->dualsense_mono_hotplug_generation = g_dualsense_mono_speaker_add_generation;
    stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
    stream->just_underran = TRUE;

    if (!stream->period)
        pulse_add_stream_to_period(stream);
    if (stream->started && pulse_stream_ready(stream) && pa_stream_is_corked(stream->stream))
    {
        success = 0;
        if (!wait_pa_operation_complete(pa_stream_cork(stream->stream, 0, pulse_op_cb, &success)))
            success = 0;
        if (!success)
            hr = E_FAIL;
    }

    return hr;
}

static void pulse_retarget_dualsense_mono_streams(const char *preferred)
{
    struct pulse_stream *stream, *next;
    BOOL found = FALSE;

    if (preferred)
    {
        TRACE("New Sony controller speaker sink %s is preferred for live mono streams.\n",
                debugstr_a(preferred));
        pulse_set_dualsense_mono_preferred_sink(preferred);
    }

    LIST_FOR_EACH_ENTRY_SAFE(stream, next, &dualsense_mono_streams, struct pulse_stream, dualsense_mono_entry)
    {
        found = TRUE;
        if (!preferred && stream->dualsense_mono_hotplug_generation == g_dualsense_mono_speaker_add_generation
                && stream->haptic_hotplug_generation == g_haptic_hotplug_generation
                && pulse_stream_ready(stream))
            continue;

        pulse_retarget_dualsense_mono_stream(stream, preferred);
    }

    if (preferred && !found)
        TRACE("No live Sony controller mono streams were registered for preferred speaker %s.\n",
                debugstr_a(preferred));
}

static char *find_dualsense_haptic_alsa_path(void)
{
    snd_pcm_info_t *info;
    char *card_name = NULL, *card_longname = NULL, *path = NULL;
    void **hints = NULL, **hint;
    int card = -1, err;

    if (snd_device_name_hint(-1, "pcm", &hints) >= 0)
    {
        for (hint = hints; *hint; ++hint)
        {
            char *name = snd_device_name_get_hint(*hint, "NAME");
            char *desc = snd_device_name_get_hint(*hint, "DESC");
            char *ioid = snd_device_name_get_hint(*hint, "IOID");

            if (name && (!ioid || strcmp(ioid, "Input")) &&
                    (string_contains_dualsense_name(name) || string_contains_dualsense_name(desc)))
            {
                TRACE("Checking DualSense haptic PCM hint %s: %s\n", debugstr_a(name), debugstr_a(desc));
                if (dualsense_haptic_pcm_path_works(name))
                {
                    path = strdup(name);
                    TRACE("Selected DualSense haptic PCM hint %s.\n", debugstr_a(path));
                }
            }

            free(name);
            free(desc);
            free(ioid);
            if (path) break;
        }
        snd_device_name_free_hint(hints);
        if (path) return path;
    }

    snd_pcm_info_alloca(&info);
    while ((err = snd_card_next(&card)) >= 0 && card >= 0)
    {
        char card_path[32];
        snd_ctl_t *ctl;
        int device = -1;
        BOOL card_match;

        free(card_name);
        free(card_longname);
        card_name = card_longname = NULL;
        snd_card_get_name(card, &card_name);
        snd_card_get_longname(card, &card_longname);
        card_match = string_contains_dualsense_name(card_name) || string_contains_dualsense_name(card_longname);

        TRACE("Checking ALSA card %d for DualSense haptics: %s / %s\n",
                card, debugstr_a(card_name), debugstr_a(card_longname));

        snprintf(card_path, sizeof(card_path), "hw:%d", card);
        if ((err = snd_ctl_open(&ctl, card_path, 0)) < 0)
        {
            TRACE("Unable to open ALSA control %s while finding DualSense haptics: %d (%s)\n",
                    card_path, err, snd_strerror(err));
            continue;
        }

        while ((err = snd_ctl_pcm_next_device(ctl, &device)) >= 0 && device >= 0)
        {
            const char *pcm_name;

            snd_pcm_info_set_device(info, device);
            snd_pcm_info_set_subdevice(info, 0);
            snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
            if (snd_ctl_pcm_info(ctl, info) < 0)
                continue;

            pcm_name = snd_pcm_info_get_name(info);
            if (!card_match && !string_contains_dualsense_name(pcm_name))
                continue;

            if ((path = malloc(32)))
            {
                snprintf(path, 32, "hw:%d,%d", card, device);
                TRACE("Checking DualSense haptic PCM candidate %s: %s\n",
                        debugstr_a(path), debugstr_a(pcm_name));
                if (dualsense_haptic_pcm_path_works(path))
                {
                    TRACE("Selected DualSense haptic PCM candidate %s.\n", debugstr_a(path));
                    break;
                }
                free(path);
                path = NULL;
            }
        }
        snd_ctl_close(ctl);
        if (path)
            break;
    }

    free(card_name);
    free(card_longname);
    return path;
}

static BOOL dualsense_haptic_pcm_path_works(const char *path)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_t *pcm;
    unsigned int rate = 48000;
    int err;

    if ((err = snd_pcm_open(&pcm, path, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0)
    {
        TRACE("DualSense haptic PCM candidate %s rejected: open failed %d (%s)\n",
                debugstr_a(path), err, snd_strerror(err));
        return FALSE;
    }

    hw_params = malloc(snd_pcm_hw_params_sizeof());
    if (!hw_params)
    {
        snd_pcm_close(pcm);
        return FALSE;
    }

    err = snd_pcm_hw_params_any(pcm, hw_params);
    if (err >= 0)
        err = snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err >= 0)
        err = snd_pcm_hw_params_set_format(pcm, hw_params, SND_PCM_FORMAT_S16_LE);
    if (err >= 0)
        err = snd_pcm_hw_params_set_rate_near(pcm, hw_params, &rate, NULL);
    if (err >= 0 && rate != 48000)
        err = -EINVAL;
    if (err >= 0)
        err = snd_pcm_hw_params_set_channels(pcm, hw_params, 4);

    free(hw_params);
    snd_pcm_close(pcm);
    if (err < 0)
        TRACE("DualSense haptic PCM candidate %s rejected: unsupported 48 kHz S16 4-channel playback (%d: %s)\n",
                debugstr_a(path), err, snd_strerror(err));
    return err >= 0;
}

static int alsa_config_add_compound(snd_config_t *parent, const char *name, snd_config_t **node)
{
    int err;

    if ((err = snd_config_make_compound(node, name, 0)) < 0)
        return err;
    if ((err = snd_config_add(parent, *node)) < 0)
    {
        snd_config_delete(*node);
        *node = NULL;
    }
    return err;
}

static int alsa_config_add_string(snd_config_t *parent, const char *name, const char *value)
{
    snd_config_t *node;
    int err;

    if ((err = snd_config_imake_string(&node, name, value)) < 0)
        return err;
    if ((err = snd_config_add(parent, node)) < 0)
        snd_config_delete(node);
    return err;
}

static int alsa_config_add_integer(snd_config_t *parent, const char *name, long value)
{
    snd_config_t *node;
    int err;

    if ((err = snd_config_imake_integer(&node, name, value)) < 0)
        return err;
    if ((err = snd_config_add(parent, node)) < 0)
        snd_config_delete(node);
    return err;
}

static int open_dualsense_haptic_pcm(snd_pcm_t **pcm, const char *alsa_path)
{
    static const char pipewire_prefix[] = "pipewire:NODE=";
    snd_config_t *config = NULL, *pcm_types, *pipewire_type, *pcms, *dualsense;
    const char *plugin, *node;
    int err;

    if (strncmp(alsa_path, pipewire_prefix, sizeof(pipewire_prefix) - 1))
        return snd_pcm_open(pcm, alsa_path, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);

    node = alsa_path + sizeof(pipewire_prefix) - 1;
    plugin = getenv("PROTON_PIPEWIRE_ALSA_PLUGIN");
    if (!plugin || !plugin[0] || !node[0])
        return -ENOENT;

    if ((err = snd_config_top(&config)) < 0 ||
            (err = alsa_config_add_compound(config, "pcm_type", &pcm_types)) < 0 ||
            (err = alsa_config_add_compound(pcm_types, "pipewire", &pipewire_type)) < 0 ||
            (err = alsa_config_add_string(pipewire_type, "lib", plugin)) < 0 ||
            (err = alsa_config_add_compound(config, "pcm", &pcms)) < 0 ||
            (err = alsa_config_add_compound(pcms, "ge_dualsense", &dualsense)) < 0 ||
            (err = alsa_config_add_string(dualsense, "type", "pipewire")) < 0 ||
            (err = alsa_config_add_string(dualsense, "playback_node", node)) < 0 ||
            (err = alsa_config_add_integer(dualsense, "aux_channels", 1)) < 0)
        goto done;

    err = snd_pcm_open_lconf(pcm, "ge_dualsense", SND_PCM_STREAM_PLAYBACK,
            SND_PCM_NONBLOCK, config);

done:
    if (config)
        snd_config_delete(config);
    return err;
}

static BOOL pulse_select_dualsense_usb_speaker(void)
{
#ifdef HAVE_LINUX_HIDRAW_H
    static const BYTE report[63] =
    {
        [0] = 0x02, /* USB output report. */
        [1] = 0xa0, /* Speaker volume and audio routing are valid. */
        [2] = 0x80, /* Speaker pre-gain is valid. */
        [6] = 0x64, /* Speaker volume. */
        [8] = 0x30, /* Route audio to the internal speaker. */
        [38] = 0x02, /* Speaker pre-gain. */
    };
    struct hidraw_devinfo info;
    char path[32];
    ssize_t written;
    unsigned int i;
    BOOL found = FALSE, selected = FALSE;
    int fd;

    if (!use_pipewire_dualsense_haptic_target())
        return FALSE;

    for (i = 0; i < 64; ++i)
    {
        snprintf(path, sizeof(path), "/dev/hidraw%u", i);
        if ((fd = open(path, O_RDWR)) == -1)
        {
            if (errno != ENOENT && errno != ENODEV)
                WARN("Unable to open %s while selecting the DualSense USB speaker: %d (%s).\n",
                        path, errno, strerror(errno));
            continue;
        }

        fcntl(fd, F_SETFD, FD_CLOEXEC);
        memset(&info, 0, sizeof(info));
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == -1 || info.bustype != 0x03 ||
                info.vendor != 0x054c || (info.product != 0x0ce6 && info.product != 0x0df2))
        {
            close(fd);
            continue;
        }

        found = TRUE;
        written = write(fd, report, sizeof(report));
        if (written == sizeof(report))
        {
            TRACE("Selected the USB internal speaker on Sony controller %04x:%04x at %s.\n",
                    (unsigned int)(unsigned short)info.vendor,
                    (unsigned int)(unsigned short)info.product, path);
            selected = TRUE;
        }
        else
            WARN("Failed to select the USB internal speaker on Sony controller %04x:%04x at %s: "
                    "wrote %ld of %u bytes, errno %d (%s).\n",
                    (unsigned int)(unsigned short)info.vendor,
                    (unsigned int)(unsigned short)info.product, path,
                    (long)written, (unsigned int)sizeof(report), errno, strerror(errno));
        close(fd);
    }

    if (!found)
        WARN("No writable USB DualSense hidraw device was found while selecting the internal speaker.\n");
    return selected;
#else
    return FALSE;
#endif
}

static HRESULT pulse_haptic_stream_connect(struct pulse_stream *stream, const char *alsa_path)
{
    snd_pcm_hw_params_t *hw_params;
    snd_pcm_sw_params_t *sw_params;
    snd_pcm_uframes_t buffer_frames;
    snd_pcm_uframes_t period_frames;
    unsigned int rate = stream->ss.rate;
    int err;

    stream->attr.minreq = stream->period_bytes;
    stream->attr.fragsize = stream->period_bytes;
    stream->attr.tlength = stream->period_bytes * 3;
    stream->attr.maxlength = stream->bufsize_frames * pa_frame_size(&stream->ss);
    stream->attr.prebuf = 0;

    if ((err = open_dualsense_haptic_pcm(&stream->haptic_pcm, alsa_path)) < 0)
    {
        WARN("Unable to open DualSense haptic PCM \"%s\": %d (%s)\n", alsa_path, err, snd_strerror(err));
        return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
    }

    hw_params = malloc(snd_pcm_hw_params_sizeof());
    sw_params = malloc(snd_pcm_sw_params_sizeof());
    if (!hw_params || !sw_params)
    {
        free(hw_params);
        free(sw_params);
        snd_pcm_close(stream->haptic_pcm);
        stream->haptic_pcm = NULL;
        return E_OUTOFMEMORY;
    }

    if ((err = snd_pcm_hw_params_any(stream->haptic_pcm, hw_params)) < 0 ||
            (err = snd_pcm_hw_params_set_access(stream->haptic_pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
            (err = snd_pcm_hw_params_set_format(stream->haptic_pcm, hw_params, SND_PCM_FORMAT_S16_LE)) < 0 ||
            (err = snd_pcm_hw_params_set_rate_near(stream->haptic_pcm, hw_params, &rate, NULL)) < 0 ||
            (err = snd_pcm_hw_params_set_channels(stream->haptic_pcm, hw_params, 4)) < 0)
    {
        WARN("Unable to configure DualSense haptic PCM \"%s\": %d (%s)\n", alsa_path, err, snd_strerror(err));
        goto failed;
    }

    period_frames = stream->period_bytes / pa_frame_size(&stream->ss);
    if ((err = snd_pcm_hw_params_set_period_size_near(stream->haptic_pcm, hw_params, &period_frames, NULL)) < 0)
        WARN("Unable to set DualSense haptic period to %lu frames: %d (%s)\n",
                period_frames, err, snd_strerror(err));

    buffer_frames = stream->bufsize_frames > period_frames * 4 ? stream->bufsize_frames : period_frames * 4;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(stream->haptic_pcm, hw_params, &buffer_frames)) < 0)
        WARN("Unable to set DualSense haptic buffer to %lu frames: %d (%s)\n",
                buffer_frames, err, snd_strerror(err));

    if ((err = snd_pcm_hw_params(stream->haptic_pcm, hw_params)) < 0)
    {
        WARN("Unable to apply DualSense haptic hw params: %d (%s)\n", err, snd_strerror(err));
        goto failed;
    }

    if ((err = snd_pcm_sw_params_current(stream->haptic_pcm, sw_params)) < 0 ||
            (err = snd_pcm_sw_params_set_start_threshold(stream->haptic_pcm, sw_params, 1)) < 0 ||
            (err = snd_pcm_sw_params_set_stop_threshold(stream->haptic_pcm, sw_params, buffer_frames)) < 0 ||
            (err = snd_pcm_sw_params(stream->haptic_pcm, sw_params)) < 0)
    {
        WARN("Unable to apply DualSense haptic sw params: %d (%s)\n", err, snd_strerror(err));
        goto failed;
    }

    if ((err = snd_pcm_prepare(stream->haptic_pcm)) < 0)
    {
        WARN("Unable to prepare DualSense haptic PCM: %d (%s)\n", err, snd_strerror(err));
        goto failed;
    }

    free(hw_params);
    free(sw_params);
    TRACE("Opened DualSense haptic PCM \"%s\" at %u Hz, %u channels.\n", alsa_path, rate, stream->ss.channels);
    stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
    pulse_select_dualsense_usb_speaker();
    return S_OK;

failed:
    free(hw_params);
    free(sw_params);
    snd_pcm_close(stream->haptic_pcm);
    stream->haptic_pcm = NULL;
    return AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

static void pulse_haptic_drop_held(struct pulse_stream *stream)
{
    TRACE("Dropping %lu bytes of pending DualSense haptic data.\n", (unsigned long)stream->held_bytes);
    stream->pa_offs_bytes += stream->held_bytes;
    stream->pa_offs_bytes %= stream->real_bufsize_bytes;
    stream->lcl_offs_bytes += stream->held_bytes;
    stream->lcl_offs_bytes %= stream->real_bufsize_bytes;
    stream->pa_held_bytes = 0;
    stream->held_bytes = 0;
}

static HRESULT pulse_retarget_dualsense_haptic_stream_path_internal(struct pulse_stream *stream,
        const char *target, BOOL drop_held)
{
    snd_pcm_t *old_pcm;
    snd_pcm_state_t state;
    char *old_path, *path;
    HRESULT hr;
    BOOL target_changed;

    if (!stream->dualsense_haptic_registered)
        return S_FALSE;
    target_changed = !stream->haptic_alsa_path || strcmp(stream->haptic_alsa_path, target);
    if (stream->haptic_pcm &&
            (state = snd_pcm_state(stream->haptic_pcm)) != SND_PCM_STATE_DISCONNECTED &&
            !target_changed)
    {
        TRACE("Keeping connected Sony controller haptic stream %p on %s during endpoint churn "
                "(PCM state %s).\n", stream, debugstr_a(stream->haptic_alsa_path),
                snd_pcm_state_name(state));
        return S_FALSE;
    }
    if (!(path = strdup(target)))
        return E_OUTOFMEMORY;

    TRACE("Retargeting Sony controller haptic stream %p from %s to %s.\n",
            stream, debugstr_a(stream->haptic_alsa_path), debugstr_a(path));

    /* Input-directed routing happens while processing the block which caused
     * the switch. Keep the current PCM alive until its replacement is ready;
     * otherwise a transient busy target leaves the write path with NULL. */
    if (!drop_held)
    {
        old_pcm = stream->haptic_pcm;
        old_path = stream->haptic_alsa_path;
        stream->haptic_pcm = NULL;
        stream->haptic_alsa_path = path;

        if (FAILED(hr = pulse_haptic_stream_connect(stream, stream->haptic_alsa_path)))
        {
            if (stream->haptic_pcm)
            {
                snd_pcm_close(stream->haptic_pcm);
                stream->haptic_pcm = NULL;
            }
            free(stream->haptic_alsa_path);
            stream->haptic_pcm = old_pcm;
            stream->haptic_alsa_path = old_path;
            WARN("Failed to retarget Sony controller haptic stream %p to %s: %#x.\n",
                    stream, debugstr_a(target), (unsigned int)hr);
            return hr;
        }

        pulse_stream_disconnect(stream);
        if (old_pcm)
        {
            snd_pcm_drop(old_pcm);
            snd_pcm_close(old_pcm);
        }
        free(old_path);
        stream->haptic_reconnect_time = 0;
        stream->haptic_path_check_time = 0;
        stream->just_underran = TRUE;
        stream->timeline_start_period_time = 0;
        stream->timeline_start_stream_time = 0;
        return S_OK;
    }

    /* DS4 does not expose the hidden PipeWire split node used by DualSense,
     * so its effects stream may currently be routed through Pulse. Drop that
     * backend before converting the live Windows stream to the new device's
     * raw PipeWire target. */
    pulse_stream_disconnect(stream);
    if (stream->haptic_pcm)
    {
        snd_pcm_drop(stream->haptic_pcm);
        snd_pcm_close(stream->haptic_pcm);
        stream->haptic_pcm = NULL;
    }
    free(stream->haptic_alsa_path);
    stream->haptic_alsa_path = path;
    stream->haptic_reconnect_time = 0;
    stream->haptic_path_check_time = 0;
    if (drop_held)
        pulse_haptic_drop_held(stream);

    if (FAILED(hr = pulse_haptic_stream_connect(stream, stream->haptic_alsa_path)))
    {
        if (!drop_held)
            pulse_haptic_drop_held(stream);
        stream->haptic_reconnect_time = pa_rtclock_now() + 250000;
        WARN("Failed to retarget Sony controller haptic stream %p to %s: %#x.\n",
                stream, debugstr_a(stream->haptic_alsa_path), (unsigned int)hr);
        return hr;
    }

    stream->just_underran = TRUE;
    stream->timeline_start_period_time = 0;
    stream->timeline_start_stream_time = 0;
    return S_OK;
}

static HRESULT pulse_retarget_dualsense_haptic_stream_path(struct pulse_stream *stream,
        const char *target)
{
    return pulse_retarget_dualsense_haptic_stream_path_internal(stream, target, TRUE);
}

static BOOL pulse_route_sony_effect_to_active_controller(struct pulse_stream *stream,
        BOOL *ready)
{
    GUID container_id;
    HRESULT hr = S_FALSE;
    char *path;

    *ready = FALSE;
    if (!pulse_get_active_sony_controller(&container_id) ||
            !(path = pulse_get_haptic_path_for_container(&container_id)))
        return FALSE;

    if (!stream->haptic_alsa_path || strcmp(stream->haptic_alsa_path, path))
    {
        TRACE("Routing active Sony effect stream %p from %s to input container %s on %s.\n",
                stream, debugstr_a(stream->haptic_alsa_path), debugstr_guid(&container_id),
                debugstr_a(path));
        hr = pulse_retarget_dualsense_haptic_stream_path_internal(stream, path, FALSE);
    }

    if (SUCCEEDED(hr) && stream->haptic_pcm)
    {
        stream->sony_controller_container_id = container_id;
        stream->sony_controller_container_valid = TRUE;
        *ready = TRUE;
    }
    free(path);

    /* A physical HID match is authoritative even when the PCM was already
     * correct. Do not let the content-correlation fallback undo it. */
    return TRUE;
}

static BOOL pulse_sony_effect_signals_correlate(pa_usec_t first, pa_usec_t second)
{
    pa_usec_t delta;

    if (!first || !second)
        return FALSE;

    delta = first > second ? first - second : second - first;
    return delta <= 500000;
}

static void pulse_bind_sony_effect_stream(struct pulse_stream *stream,
        const struct pulse_stream *source)
{
    stream->sony_controller_container_valid = source->sony_controller_container_valid;
    if (source->sony_controller_container_valid)
        stream->sony_controller_container_id = source->sony_controller_container_id;
}

static BOOL pulse_align_sony_controller_stream(struct pulse_stream *stream,
        const int16_t *samples, UINT32 frames)
{
    struct pulse_stream *active = NULL, *other, *next;
    unsigned int speaker_peak = 0, actuator_peak = 0;
    BOOL speaker_signal = FALSE, actuator_signal = FALSE;
    BOOL speaker_only_signal = FALSE, actuator_only_signal = FALSE, route_ready;
    pa_usec_t now;
    UINT32 frame;

    if (stream->ss.channels < 4 || !stream->dualsense_haptic_registered)
        return FALSE;

    for (frame = 0; frame < frames; ++frame)
    {
        int speaker = samples[frame * stream->ss.channels + 1];
        int left = samples[frame * stream->ss.channels + 2];
        int right = samples[frame * stream->ss.channels + 3];
        unsigned int speaker_amplitude = speaker < 0 ? -speaker : speaker;
        unsigned int left_amplitude = left < 0 ? -left : left;
        unsigned int right_amplitude = right < 0 ? -right : right;

        speaker_peak = max(speaker_peak, speaker_amplitude);
        actuator_peak = max(actuator_peak, max(left_amplitude, right_amplitude));
    }

    now = pa_rtclock_now();
    speaker_signal = speaker_peak >= 128;
    actuator_signal = actuator_peak >= 512;
    if (speaker_signal && speaker_peak > actuator_peak * 2)
    {
        stream->sony_speaker_source = TRUE;
        stream->sony_speaker_signal_time = now;
        speaker_only_signal = TRUE;
    }
    if (actuator_signal && actuator_peak > speaker_peak * 2)
    {
        stream->sony_actuator_source = TRUE;
        stream->sony_actuator_signal_time = now;
        actuator_only_signal = TRUE;
    }

    if ((speaker_signal || actuator_signal) &&
            pulse_route_sony_effect_to_active_controller(stream, &route_ready))
        return !route_ready;

    if (speaker_only_signal && stream->sony_speaker_source && !stream->sony_actuator_source)
    {
        LIST_FOR_EACH_ENTRY(other, &dualsense_haptic_streams,
                struct pulse_stream, dualsense_haptic_entry)
        {
            if (!other->sony_actuator_source || other->sony_speaker_source ||
                    !other->haptic_alsa_path || !other->haptic_alsa_path[0] ||
                    !pulse_sony_effect_signals_correlate(
                            stream->sony_speaker_signal_time,
                            other->sony_actuator_signal_time))
                continue;

            if (!active || other->sony_actuator_signal_time > active->sony_actuator_signal_time)
                active = other;
        }

        if (active && (!stream->haptic_alsa_path ||
                strcmp(stream->haptic_alsa_path, active->haptic_alsa_path)))
        {
            TRACE("Pairing Sony speaker stream %p with active actuator stream %p on %s.\n",
                    stream, active, debugstr_a(active->haptic_alsa_path));
            if (pulse_retarget_dualsense_haptic_stream_path(stream,
                    active->haptic_alsa_path) == S_OK)
            {
                pulse_bind_sony_effect_stream(stream, active);
                return TRUE;
            }
        }
    }

    if (actuator_only_signal && stream->sony_actuator_source &&
            !stream->sony_speaker_source && stream->haptic_alsa_path &&
            stream->haptic_alsa_path[0])
    {
        LIST_FOR_EACH_ENTRY_SAFE(other, next, &dualsense_haptic_streams,
                struct pulse_stream, dualsense_haptic_entry)
        {
            if (other == stream || !other->sony_speaker_source ||
                    other->sony_actuator_source ||
                    !pulse_sony_effect_signals_correlate(
                            stream->sony_actuator_signal_time,
                            other->sony_speaker_signal_time) ||
                    (other->haptic_alsa_path &&
                     !strcmp(other->haptic_alsa_path, stream->haptic_alsa_path)))
                continue;

            TRACE("Pairing Sony speaker stream %p with active actuator stream %p on %s.\n",
                    other, stream, debugstr_a(stream->haptic_alsa_path));
            if (pulse_retarget_dualsense_haptic_stream_path(other,
                    stream->haptic_alsa_path) == S_OK)
                pulse_bind_sony_effect_stream(other, stream);
        }
    }

    return FALSE;
}

static void pulse_retarget_dualsense_haptic_streams(const char *target)
{
    struct pulse_stream *stream, *next;
    GUID target_container_id;
    BOOL target_container_valid;
    char *path;

    target_container_valid = pulse_get_haptic_target_container_id(target, FALSE, &target_container_id);
    if (!(path = make_pipewire_dualsense_haptic_path(target)))
        return;

    LIST_FOR_EACH_ENTRY_SAFE(stream, next, &dualsense_haptic_streams,
            struct pulse_stream, dualsense_haptic_entry)
    {
        HRESULT hr;

        if (target_container_valid && stream->sony_controller_container_valid &&
                !IsEqualGUID(&stream->sony_controller_container_id, &target_container_id) &&
                pulse_container_is_present(&stream->sony_controller_container_id))
        {
            TRACE("Keeping Sony controller stream %p on container %s while adding target for %s.\n",
                    stream, debugstr_guid(&stream->sony_controller_container_id),
                    debugstr_guid(&target_container_id));
            continue;
        }

        hr = pulse_retarget_dualsense_haptic_stream_path(stream, path);
        if (target_container_valid && SUCCEEDED(hr))
        {
            stream->sony_controller_container_id = target_container_id;
            stream->sony_controller_container_valid = TRUE;
        }
    }
    free(path);
}

static void pulse_retarget_dualsense_haptic_streams_to_alsa(const char *path)
{
    struct pulse_stream *stream, *next;
    GUID target_container_id;
    BOOL target_container_valid;

    TRACE("Retargeting live Sony controller effect streams to ALSA PCM %s.\n", debugstr_a(path));
    target_container_valid = pulse_get_haptic_target_container_id(path, TRUE, &target_container_id);
    LIST_FOR_EACH_ENTRY_SAFE(stream, next, &dualsense_haptic_streams,
            struct pulse_stream, dualsense_haptic_entry)
    {
        HRESULT hr;

        if (target_container_valid && stream->sony_controller_container_valid &&
                !IsEqualGUID(&stream->sony_controller_container_id, &target_container_id) &&
                pulse_container_is_present(&stream->sony_controller_container_id))
        {
            TRACE("Keeping Sony controller stream %p on container %s while adding ALSA target for %s.\n",
                    stream, debugstr_guid(&stream->sony_controller_container_id),
                    debugstr_guid(&target_container_id));
            continue;
        }

        hr = pulse_retarget_dualsense_haptic_stream_path(stream, path);
        if (target_container_valid && SUCCEEDED(hr))
        {
            stream->sony_controller_container_id = target_container_id;
            stream->sony_controller_container_valid = TRUE;
        }
    }
}

static void pulse_haptic_invalidate(struct pulse_stream *stream, const char *reason)
{
    TRACE("Invalidating DualSense haptic PCM after %s.\n", reason);
    if (stream->haptic_pcm)
    {
        snd_pcm_close(stream->haptic_pcm);
        stream->haptic_pcm = NULL;
    }
    /* The unplugged controller is gone. Keep the old stream invalid instead of
     * retargeting it to a later controller that may be a different device. */
    if (stream->haptic_alsa_path)
        stream->haptic_alsa_path[0] = 0;
    stream->haptic_reconnect_time = 0;
    stream->haptic_path_check_time = 0;
    stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
    pulse_haptic_drop_held(stream);
}

static BOOL pulse_haptic_refresh_path(struct pulse_stream *stream)
{
    if (!use_pipewire_dualsense_haptic_target())
    {
        if (stream->haptic_pcm && stream->haptic_hotplug_generation == g_haptic_hotplug_generation)
            return TRUE;
        TRACE("DualSense haptic stream no longer matches the current device generation.\n");
        return FALSE;
    }

    if (!stream->haptic_pcm)
    {
        pa_usec_t now;

        if (stream->dualsense_haptic_registered && stream->haptic_alsa_path &&
                stream->haptic_alsa_path[0])
        {
            now = pa_rtclock_now();
            if (now >= stream->haptic_reconnect_time)
            {
                if (SUCCEEDED(pulse_haptic_stream_connect(stream, stream->haptic_alsa_path)))
                    return TRUE;
                stream->haptic_reconnect_time = now + 250000;
            }
        }
        TRACE("DualSense haptic stream has no open raw PCM.\n");
        return FALSE;
    }

    if (stream->haptic_hotplug_generation != g_haptic_hotplug_generation)
    {
        TRACE("Keeping open DualSense haptic PCM across device generation %u -> %u.\n",
                stream->haptic_hotplug_generation, g_haptic_hotplug_generation);
        stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
    }
    return TRUE;
}

static void pulse_haptic_advance(struct pulse_stream *stream, UINT32 frames, UINT32 frame_size)
{
    SIZE_T bytes = (SIZE_T)frames * frame_size;

    stream->pa_offs_bytes += bytes;
    stream->pa_offs_bytes %= stream->real_bufsize_bytes;
    stream->pa_held_bytes -= min(stream->pa_held_bytes, bytes);
    if (pulse_split_speaker_ready(stream))
        return;

    stream->lcl_offs_bytes += bytes;
    stream->lcl_offs_bytes %= stream->real_bufsize_bytes;
    stream->held_bytes -= min(stream->held_bytes, bytes);
}

static void pulse_haptic_trace_channels(struct pulse_stream *stream, const int16_t *samples,
        UINT32 frames)
{
    pa_usec_t now;
    UINT32 frame, channel;

    if (!TRACE_ON(pulse))
        return;

    for (frame = 0; frame < frames; ++frame)
    {
        for (channel = 0; channel < ARRAY_SIZE(stream->haptic_channel_peak); ++channel)
        {
            int value = samples[frame * stream->ss.channels + channel];
            unsigned int amplitude = value < 0 ? -value : value;

            stream->haptic_channel_peak[channel] =
                    max(stream->haptic_channel_peak[channel], amplitude);
        }
    }

    now = pa_rtclock_now();
    if (!stream->haptic_channel_trace_time)
    {
        stream->haptic_channel_trace_time = now + 1000000;
        return;
    }
    if (now < stream->haptic_channel_trace_time)
        return;

    TRACE("DualSense raw PCM channel peaks: %u, %u, %u, %u.\n",
            stream->haptic_channel_peak[0], stream->haptic_channel_peak[1],
            stream->haptic_channel_peak[2], stream->haptic_channel_peak[3]);
    memset(stream->haptic_channel_peak, 0, sizeof(stream->haptic_channel_peak));
    stream->haptic_channel_trace_time = now + 1000000;
}

static void pulse_split_speaker_write(struct pulse_stream *stream, const BYTE *src, UINT32 frames)
{
    SIZE_T sample_size, bytes, max_queue, required, source_offset = 0;
    BOOL has_signal = FALSE;
    pa_usec_t now;
    UINT32 frame;

    if (!pulse_split_speaker_ready(stream))
        return;

    sample_size = pa_sample_size_of_format(stream->ss.format);
    if (!sample_size || stream->ss.channels < 2)
        return;

    bytes = (SIZE_T)frames * sample_size;
    if (!bytes)
        return;

    pulse_split_speaker_drain(stream);

    /* Keep at most 250 ms locally. If the server stops consuming, retain
     * current effects rather than replaying stale controller audio later. */
    max_queue = max((SIZE_T)stream->ss.rate * sample_size / 4,
            (SIZE_T)stream->period_bytes / stream->ss.channels * 3);
    if (bytes > max_queue)
    {
        source_offset = bytes - max_queue;
        source_offset -= source_offset % sample_size;
        stream->speaker_dropped_bytes += source_offset;
        bytes -= source_offset;
        frames = bytes / sample_size;
    }

    if (stream->speaker_buffer_held + bytes > max_queue)
    {
        SIZE_T drop = min(stream->speaker_buffer_held,
                stream->speaker_buffer_held + bytes - max_queue);

        drop -= drop % sample_size;
        stream->speaker_buffer_held -= drop;
        stream->speaker_dropped_bytes += drop;
        if (stream->speaker_buffer_held)
            memmove(stream->speaker_buffer, stream->speaker_buffer + drop,
                    stream->speaker_buffer_held);
    }

    required = stream->speaker_buffer_held + bytes;
    if (required > stream->speaker_buffer_bytes)
    {
        SIZE_T capacity = max(required, min(max_queue,
                max(stream->speaker_buffer_bytes * 2, (SIZE_T)4096)));
        BYTE *buffer = realloc(stream->speaker_buffer, capacity);

        if (!buffer)
            return;
        stream->speaker_buffer = buffer;
        stream->speaker_buffer_bytes = capacity;
    }

    /* The DualSense UCM speaker split is hardware channel 1. Preserve that
     * Windows endpoint layout instead of applying a software downmix. */
    for (frame = 0; frame < frames; ++frame)
    {
        const BYTE *sample = src + source_offset * stream->ss.channels +
                (frame * stream->ss.channels + 1) * sample_size;
        SIZE_T byte;

        memcpy(stream->speaker_buffer + stream->speaker_buffer_held +
                frame * sample_size, sample, sample_size);
        if (!stream->speaker_route_selected)
            for (byte = 0; byte < sample_size; ++byte)
                has_signal |= sample[byte] != 0;
        if (TRACE_ON(pulse) && stream->ss.format == PA_SAMPLE_FLOAT32LE)
        {
            float value;

            memcpy(&value, sample, sizeof(value));
            if (value < 0.0f) value = -value;
            if (value > stream->speaker_peak) stream->speaker_peak = value;
        }
    }
    stream->speaker_buffer_held += bytes;

    if (!stream->speaker_route_selected && has_signal)
        stream->speaker_route_selected = pulse_select_dualsense_usb_speaker();

    pulse_split_speaker_drain(stream);

    if (!TRACE_ON(pulse))
        return;
    now = pa_rtclock_now();
    if (!stream->speaker_trace_time)
        stream->speaker_trace_time = now + 1000000;
    else if (now >= stream->speaker_trace_time)
    {
        TRACE("%p split speaker peak %.9g, pending %zu bytes, dropped %zu bytes, corked %d.\n",
                stream, stream->speaker_peak, (size_t)stream->speaker_buffer_held,
                (size_t)stream->speaker_dropped_bytes, pa_stream_is_corked(stream->speaker_stream));
        stream->speaker_peak = 0.0f;
        stream->speaker_dropped_bytes = 0;
        stream->speaker_trace_time = now + 1000000;
    }
}

static void pulse_haptic_write(struct pulse_stream *stream)
{
    snd_pcm_state_t state;
    snd_pcm_sframes_t avail, written;
    UINT32 frame_size = pa_frame_size(&stream->ss);
    UINT32 held_frames = stream->held_bytes / frame_size;
    UINT32 frames, contiguous_frames, i, c;
    const BYTE *src;
    const void *write_data;
    BOOL raw_speaker, split_speaker;
    int err;

    if (!pulse_stream_haptic(stream) || !held_frames)
        return;
    if (!pulse_haptic_refresh_path(stream))
        return;
    if (!stream->haptic_pcm)
        return;

    state = snd_pcm_state(stream->haptic_pcm);
    if (state == SND_PCM_STATE_DISCONNECTED)
    {
        WARN("DualSense haptic PCM disconnected; invalidating stream.\n");
        pulse_haptic_invalidate(stream, "disconnect");
        return;
    }
    else if (state == SND_PCM_STATE_XRUN || state == SND_PCM_STATE_SUSPENDED)
    {
        if ((err = snd_pcm_recover(stream->haptic_pcm, state == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE, 1)) < 0)
        {
            WARN("DualSense haptic PCM state recover failed from %s: %d (%s)\n",
                    snd_pcm_state_name(state), err, snd_strerror(err));
            pulse_haptic_invalidate(stream, "state recovery failure");
            return;
        }
    }

    avail = snd_pcm_avail_update(stream->haptic_pcm);
    if (avail < 0)
    {
        if ((err = snd_pcm_recover(stream->haptic_pcm, avail, 1)) < 0)
        {
            WARN("DualSense haptic PCM recover failed: %d (%s)\n", err, snd_strerror(err));
            pulse_haptic_invalidate(stream, "availability recovery failure");
            return;
        }
        avail = snd_pcm_avail_update(stream->haptic_pcm);
    }
    if (avail <= 0)
        return;

    contiguous_frames = (stream->real_bufsize_bytes - stream->pa_offs_bytes) / frame_size;
    frames = min(min((UINT32)avail, held_frames), contiguous_frames);
    if (!frames)
        return;
    src = stream->local_buffer + stream->pa_offs_bytes;
    split_speaker = use_dualsense_split_audio() && pulse_split_speaker_ready(stream);
    /* Automatic mode can be selected after winebus initialized the HID, so
     * retain and route AUX1 here when no private speaker stream owns it. */
    raw_speaker = use_pipewire_dualsense_haptic_target() && !split_speaker &&
            stream->ss.channels >= 2;
    if (stream->ss.format == PA_SAMPLE_FLOAT32LE)
    {
        const float *float_src = (const float *)src;

        if (frames > stream->haptic_buffer_frames)
        {
            int16_t *buffer = realloc(stream->haptic_buffer,
                    frames * stream->ss.channels * sizeof(*buffer));

            if (!buffer)
                return;
            stream->haptic_buffer = buffer;
            stream->haptic_buffer_frames = frames;
        }

        for (i = 0; i < frames; ++i)
            for (c = 0; c < stream->ss.channels; ++c)
                stream->haptic_buffer[i * stream->ss.channels + c] =
                        float_to_s16(float_src[i * stream->ss.channels + c]);
        write_data = stream->haptic_buffer;
    }
    else if (split_speaker || raw_speaker)
    {
        if (frames > stream->haptic_buffer_frames)
        {
            int16_t *buffer = realloc(stream->haptic_buffer,
                    frames * stream->ss.channels * sizeof(*buffer));

            if (!buffer)
                return;
            stream->haptic_buffer = buffer;
            stream->haptic_buffer_frames = frames;
        }
        memcpy(stream->haptic_buffer, src,
                frames * stream->ss.channels * sizeof(*stream->haptic_buffer));
        write_data = stream->haptic_buffer;
    }
    else
        write_data = src;

    if (split_speaker || raw_speaker)
    {
        int16_t *samples = stream->haptic_buffer;
        BOOL has_speaker_signal = FALSE;

        for (i = 0; i < frames; ++i)
        {
            samples[i * stream->ss.channels] = 0;
            if (raw_speaker)
                has_speaker_signal |= samples[i * stream->ss.channels + 1] != 0;
            else
                samples[i * stream->ss.channels + 1] = 0;
        }

        if (raw_speaker && has_speaker_signal && !stream->speaker_route_selected)
            stream->speaker_route_selected = pulse_select_dualsense_usb_speaker();
    }

    if (pulse_align_sony_controller_stream(stream, write_data, frames))
        return;

    pulse_haptic_trace_channels(stream, write_data, frames);

    written = snd_pcm_writei(stream->haptic_pcm, write_data, frames);
    if (written < 0)
    {
        if ((err = snd_pcm_recover(stream->haptic_pcm, written, 1)) < 0)
        {
            WARN("DualSense haptic PCM write failed: %ld/%d (%s)\n", written, err, snd_strerror(err));
            pulse_haptic_invalidate(stream, "write recovery failure");
            return;
        }
        written = snd_pcm_writei(stream->haptic_pcm, write_data, frames);
        if (written < 0)
        {
            WARN("DualSense haptic PCM write failed after recover: %ld (%s)\n",
                    written, snd_strerror(written));
            pulse_haptic_invalidate(stream, "write failure");
            return;
        }
    }

    pulse_haptic_advance(stream, written, frame_size);
}
static NTSTATUS pulse_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct pulse_stream *stream;
    unsigned int i, bufsize_bytes;
    HRESULT hr;
    char *name, *haptic_target = NULL, *haptic_alsa_path = NULL, *dualsense_speaker = NULL;
    char *split_speaker = NULL;
    char *resolved_device = NULL;
    const char *connect_device;
    BOOL haptic_candidate, dualsense_mono_candidate, dualsense_pulse_haptic_candidate = FALSE;

    if (params->sony_windows_audio_mode &&
            !InterlockedExchange(&g_sony_windows_audio_mode, 1))
        TRACE("Selected Windows Sony audio mode from stream initialization.\n");

    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE) {
        params->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
        return STATUS_SUCCESS;
    }

    pulse_lock();

    name = wstr_to_str(params->name);
    params->result = pulse_connect(name);
    free(name);

    if (FAILED(params->result))
    {
        pulse_unlock();
        return STATUS_SUCCESS;
    }

    if (!(stream = calloc(1, sizeof(*stream))))
    {
        pulse_unlock();
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }

    stream->dataflow = params->flow;
    list_init(&stream->dualsense_mono_entry);
    list_init(&stream->dualsense_haptic_entry);
    for (i = 0; i < ARRAY_SIZE(stream->vol); ++i)
        stream->vol[i] = 1.f;

    hr = pulse_spec_from_waveformat(stream, params->fmt);
    TRACE("Obtaining format returns %08x\n", (unsigned)hr);

    if (FAILED(hr))
        goto exit;

    haptic_candidate = is_dualsense_haptic_format(stream)
            && (string_contains_dualsense_name(params->device)
                    || (haptic_target = get_dualsense_haptic_target(params->device, stream)));
    /* A Sony endpoint name satisfies the first side of the expression above,
     * so retrieve its hidden PipeWire parent separately instead of losing it
     * to short-circuit evaluation. */
    if (use_pipewire_dualsense_haptic_target() && haptic_candidate && !haptic_target)
        haptic_target = get_dualsense_haptic_target(params->device, stream);
    if (haptic_candidate && pulse_get_device_container_id(params->device,
            &stream->sony_controller_container_id))
    {
        stream->sony_controller_container_valid = TRUE;
        TRACE("Bound Sony controller stream %p to container %s.\n", stream,
                debugstr_guid(&stream->sony_controller_container_id));
    }
    dualsense_mono_candidate = !haptic_candidate && pulse_stream_dualsense_mono_format(stream)
            && string_contains_dualsense_name(params->device);
    if (haptic_candidate)
    {
        stream->map.map[0] = PA_CHANNEL_POSITION_AUX0;
        stream->map.map[1] = PA_CHANNEL_POSITION_AUX1;
        stream->map.map[2] = PA_CHANNEL_POSITION_AUX2;
        stream->map.map[3] = PA_CHANNEL_POSITION_AUX3;
        if (use_pipewire_dualsense_haptic_target() && haptic_target)
        {
            haptic_alsa_path = make_pipewire_dualsense_haptic_path(haptic_target);
            TRACE("Routing DualSense stream from %s through ALSA PipeWire target %s.\n",
                    params->device, haptic_alsa_path ? haptic_alsa_path : haptic_target);
        }
        else if (!use_dualsense_split_audio())
        {
            haptic_alsa_path = get_dualsense_haptic_alsa_path(params->device, stream);
            if (!haptic_alsa_path)
                haptic_alsa_path = find_dualsense_haptic_alsa_path();
            TRACE("Routing DualSense haptic stream from %s through %s%s.\n",
                    params->device, haptic_alsa_path ? "raw ALSA PCM " : "Pulse device ",
                    haptic_alsa_path ? haptic_alsa_path : params->device);
        }
        else
            WARN("No PipeWire parent was found for the DualSense haptic stream from %s.\n",
                    params->device);

        if (use_dualsense_split_audio() && !use_death_stranding_controller_effects() &&
                stream->ss.format == PA_SAMPLE_FLOAT32LE)
            split_speaker = get_dualsense_speaker_sink(params->device, TRUE);
    }

    stream->def_period = params->period;
    stream->duration = params->duration;

    stream->period_bytes = pa_frame_size(&stream->ss) * muldiv(params->period,
                                                               stream->ss.rate,
                                                               10000000);

    stream->bufsize_frames = ceil((params->duration / 10000000.) * params->fmt->nSamplesPerSec);
    bufsize_bytes = stream->bufsize_frames * pa_frame_size(&stream->ss);
    stream->mmdev_period_usec = params->period / 10;

    stream->share = params->share;
    stream->flags = params->flags;
    connect_device = params->device;
    if (dualsense_mono_candidate)
    {
        dualsense_speaker = get_dualsense_speaker_sink(params->device, TRUE);
        if (!dualsense_speaker && g_dualsense_mono_preferred_sink &&
                strcmp(g_dualsense_mono_preferred_sink, params->device))
            dualsense_speaker = get_dualsense_speaker_sink(g_dualsense_mono_preferred_sink, TRUE);
        if (!dualsense_speaker)
            dualsense_speaker = get_dualsense_speaker_sink(NULL, TRUE);
        if (dualsense_speaker)
            connect_device = dualsense_speaker;
    }
    else if ((resolved_device = pulse_resolve_device_name(connect_device)))
        connect_device = resolved_device;

    if (!dualsense_mono_candidate && !haptic_candidate && pulse_name_is_dualsense_speaker_sink(connect_device))
    {
        TRACE("Tracking DualSense speaker stream for selected sink %s, channels %u.\n",
                debugstr_a(connect_device), stream->ss.channels);
        dualsense_mono_candidate = TRUE;
    }

    if (haptic_candidate)
    {
        stream->haptic_alsa_path = strdup(haptic_alsa_path ? haptic_alsa_path : "");
        if (stream->haptic_alsa_path)
        {
            if (stream->haptic_alsa_path[0])
                hr = pulse_haptic_stream_connect(stream, stream->haptic_alsa_path);
            else
                hr = AUDCLNT_E_ENDPOINT_CREATE_FAILED;
            if (FAILED(hr))
            {
                TRACE("Falling back to Pulse routing for DualSense haptic stream.\n");
                free(stream->haptic_alsa_path);
                stream->haptic_alsa_path = NULL;
                if ((dualsense_speaker = get_dualsense_speaker_sink(connect_device, FALSE)))
                    connect_device = dualsense_speaker;
                /* The hidden haptic target is only for the raw ALSA split path. If raw
                 * access is busy, connect directly to the controller speaker sink so controller
                 * speaker/effect audio works before any hotplug reconnect happens. */
                hr = pulse_stream_connect_dualsense_speaker(stream, connect_device, stream->period_bytes);
            }
        }
        else
            hr = E_OUTOFMEMORY;
        if (SUCCEEDED(hr) && !stream->haptic_alsa_path)
            dualsense_pulse_haptic_candidate = TRUE;
    }
    else
    {
        if (dualsense_mono_candidate)
            hr = pulse_stream_connect_dualsense_speaker(stream, connect_device, stream->period_bytes);
        else
            hr = pulse_stream_connect(stream, connect_device, haptic_target, stream->period_bytes);
    }
    if (SUCCEEDED(hr)) {
        UINT32 unalign;
        const pa_buffer_attr *attr = pulse_stream_ready(stream) ? pa_stream_get_buffer_attr(stream->stream) : &stream->attr;
        SIZE_T size;

        stream->attr = *attr;
        /* Update frames according to new size */
        dump_attr(attr);
        if (stream->dataflow == eRender) {
            size = stream->real_bufsize_bytes =
                stream->bufsize_frames * 2 * pa_frame_size(&stream->ss);
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                        zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
                hr = E_OUTOFMEMORY;
        } else {
            UINT32 i, capture_packets;

            if ((unalign = bufsize_bytes % stream->period_bytes))
                bufsize_bytes += stream->period_bytes - unalign;
            stream->bufsize_frames = bufsize_bytes / pa_frame_size(&stream->ss);
            stream->real_bufsize_bytes = bufsize_bytes;

            capture_packets = stream->real_bufsize_bytes / stream->period_bytes;

            size = stream->real_bufsize_bytes + capture_packets * sizeof(ACPacket);
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                        zero_bits, &size, MEM_COMMIT, PAGE_READWRITE))
                hr = E_OUTOFMEMORY;
            else {
                ACPacket *cur_packet = (ACPacket*)((char*)stream->local_buffer + stream->real_bufsize_bytes);
                BYTE *data = stream->local_buffer;
                silence_buffer(stream->ss.format, stream->local_buffer, stream->real_bufsize_bytes);
                list_init(&stream->packet_free_head);
                list_init(&stream->packet_filled_head);
                for (i = 0; i < capture_packets; ++i, ++cur_packet) {
                    list_add_tail(&stream->packet_free_head, &cur_packet->entry);
                    cur_packet->data = data;
                    data += stream->period_bytes;
                }
            }
        }
        stream->device = strdup(connect_device);
        if (haptic_candidate && use_dualsense_split_audio() &&
                !use_death_stranding_controller_effects() && haptic_alsa_path &&
                stream->ss.format == PA_SAMPLE_FLOAT32LE)
        {
            if (!split_speaker)
                WARN("No public DualSense speaker sink was found for split stream %s.\n",
                        debugstr_a(params->device));
            else if (FAILED(pulse_split_speaker_connect(stream, split_speaker)))
                WARN("Could not create split speaker stream for %s; retaining raw channel routing.\n",
                        debugstr_a(split_speaker));
        }
        if (dualsense_pulse_haptic_candidate || dualsense_mono_candidate)
            stream->haptic_hotplug_generation = g_haptic_hotplug_generation;
        if (dualsense_mono_candidate)
        {
            if (pulse_get_device_container_id(connect_device,
                    &stream->sony_controller_container_id))
            {
                stream->sony_controller_container_valid = TRUE;
                TRACE("Bound Sony controller mono stream %p to container %s.\n", stream,
                        debugstr_guid(&stream->sony_controller_container_id));
            }
            pulse_set_dualsense_mono_preferred_sink(connect_device);

            stream->dualsense_mono_hotplug_generation = g_dualsense_mono_speaker_add_generation;
            list_add_tail(&dualsense_mono_streams, &stream->dualsense_mono_entry);
            stream->dualsense_mono_registered = TRUE;
        }
        if (SUCCEEDED(hr) && haptic_candidate &&
                use_pipewire_dualsense_haptic_target())
        {
            list_add_tail(&dualsense_haptic_streams, &stream->dualsense_haptic_entry);
            stream->dualsense_haptic_registered = TRUE;
        }
    }

    *params->channel_count = stream->ss.channels;
    *params->stream = (stream_handle)(UINT_PTR)stream;
    TRACE("created stream %p.\n", stream);
exit:
    if (FAILED(params->result = hr)) {
        free(stream->local_buffer);
        if (stream->stream) {
            pa_stream_disconnect(stream->stream);
            pa_stream_unref(stream->stream);
        }
        free(stream->device);
        free(stream->haptic_alsa_path);
        pulse_split_speaker_disconnect(stream);
        free(stream->speaker_device);
        free(stream->speaker_buffer);
        free(stream);
    }

    free(haptic_target);
    free(haptic_alsa_path);
    free(dualsense_speaker);
    free(split_speaker);
    free(resolved_device);
    pulse_unlock();
    return STATUS_SUCCESS;
}

static int write_buffer(struct pulse_stream *stream, BYTE *buffer, UINT32 bytes)
{
    const float *vol = stream->vol;
    pa_seek_mode_t seek;
    int ret;
    UINT32 i, channels, mute = 0;
    BOOL adjust = FALSE;
    BYTE *end;

    if (!bytes) return 0;

    /* Adjust the buffer based on the volume for each channel */
    channels = stream->ss.channels;
    for (i = 0; i < channels; i++)
    {
        adjust |= vol[i] != 1.0f;
        if (vol[i] == 0.0f)
            mute++;
    }
    if (mute == channels)
    {
        silence_buffer(stream->ss.format, buffer, bytes);
        goto write;
    }
    if (!adjust) goto write;

    end = buffer + bytes;
    switch (stream->ss.format)
    {
#ifndef WORDS_BIGENDIAN
#define PROCESS_BUFFER(type) do         \
{                                       \
    type *p = (type*)buffer;            \
    do                                  \
    {                                   \
        for (i = 0; i < channels; i++)  \
            p[i] = p[i] * vol[i];       \
        p += i;                         \
    } while ((BYTE*)p != end);          \
} while (0)
    case PA_SAMPLE_S16LE:
        PROCESS_BUFFER(INT16);
        break;
    case PA_SAMPLE_S32LE:
        PROCESS_BUFFER(INT32);
        break;
    case PA_SAMPLE_FLOAT32LE:
        PROCESS_BUFFER(float);
        break;
#undef PROCESS_BUFFER
    case PA_SAMPLE_S24_32LE:
    {
        UINT32 *p = (UINT32*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
            {
                p[i] = (INT32)((INT32)(p[i] << 8) * vol[i]);
                p[i] >>= 8;
            }
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    case PA_SAMPLE_S24LE:
    {
        /* do it 12 bytes at a time until it is no longer possible */
        UINT32 *q = (UINT32*)buffer;
        BYTE *p;

        i = 0;
        while (end - (BYTE*)q >= 12)
        {
            UINT32 v[4], k;
            v[0] = q[0] << 8;
            v[1] = q[1] << 16 | (q[0] >> 16 & ~0xff);
            v[2] = q[2] << 24 | (q[1] >> 8  & ~0xff);
            v[3] = q[2] & ~0xff;
            for (k = 0; k < 4; k++)
            {
                v[k] = (INT32)((INT32)v[k] * vol[i]);
                if (++i == channels) i = 0;
            }
            *q++ = v[0] >> 8  | (v[1] & ~0xff) << 16;
            *q++ = v[1] >> 16 | (v[2] & ~0xff) << 8;
            *q++ = v[2] >> 24 | (v[3] & ~0xff);
        }
        p = (BYTE*)q;
        while (p != end)
        {
            UINT32 v = (INT32)((INT32)(p[0] << 8 | p[1] << 16 | p[2] << 24) * vol[i]);
            *p++ = v >> 8  & 0xff;
            *p++ = v >> 16 & 0xff;
            *p++ = v >> 24;
            if (++i == channels) i = 0;
        }
        break;
    }
#endif
    case PA_SAMPLE_U8:
    {
        UINT8 *p = (UINT8*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = (int)((p[i] - 128) * vol[i]) + 128;
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    case PA_SAMPLE_ALAW:
    {
        UINT8 *p = (UINT8*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = mult_alaw_sample(p[i], vol[i]);
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    case PA_SAMPLE_ULAW:
    {
        UINT8 *p = (UINT8*)buffer;
        do
        {
            for (i = 0; i < channels; i++)
                p[i] = mult_ulaw_sample(p[i], vol[i]);
            p += i;
        } while ((BYTE*)p != end);
        break;
    }
    default:
        TRACE("Unhandled format %i, not adjusting volume.\n", stream->ss.format);
        break;
    }

write:
    if (!bytes)
        return 0;

    seek = stream->rebase_write_index ? PA_SEEK_RELATIVE_ON_READ : PA_SEEK_RELATIVE;
    ret = pa_stream_write(stream->stream, buffer, bytes, NULL, 0, seek);
    if (!ret && stream->rebase_write_index)
    {
        TRACE("Rebased stream %p write index on Pulse read index.\n", stream);
        stream->rebase_write_index = FALSE;
    }
    return ret;
}

static void pulse_write_index_catchup(struct pulse_stream *stream)
{
    const pa_timing_info *ti;
    size_t writable;
    UINT32 frame_size, to_write, max_write;
    uint64_t gap_delta, max_gap, sane_gap;
    int64_t gap, write_index;

    if (!pulse_stream_ready(stream))
        return;

    ti = pa_stream_get_timing_info(stream->stream);
    if (!ti || ti->read_index <= ti->write_index) return;
    if (ti->read_index_corrupt || ti->write_index_corrupt)
    {
        WARN("index corrupt %d / %d.\n", ti->read_index_corrupt, ti->write_index_corrupt);
        return;
    }

    if (!silence_buf) silence_buf = calloc(1, silence_buf_size);
    frame_size = pa_frame_size(&stream->ss);
    if (!frame_size) return;

    while (ti && ti->read_index > ti->write_index)
    {
        write_index = ti->write_index;
        gap = ti->read_index - write_index;
        TRACE("stream %p, running %d, read is ahead of write %lld bytes.\n", stream, stream->started,
              (long long)gap);

        max_gap = max(stream->period_bytes * 8, stream->real_bufsize_bytes);
        if (stream->attr.maxlength != (uint32_t)-1)
            max_gap = max(max_gap, (uint64_t)stream->attr.maxlength);
        if (!max_gap)
            max_gap = silence_buf_size;
        sane_gap = max(max_gap, (uint64_t)stream->ss.rate * frame_size * 300);
        if (gap <= 0)
            break;

        if ((uint64_t)gap > max_gap)
        {
            writable = pa_stream_writable_size(stream->stream);
            gap_delta = (uint64_t)-1;
            if (writable != (size_t)-1)
                gap_delta = (uint64_t)gap > (uint64_t)writable
                        ? (uint64_t)gap - (uint64_t)writable
                        : (uint64_t)writable - (uint64_t)gap;

            if (gap_delta <= max_gap || (uint64_t)gap <= sane_gap)
            {
                WARN("Recovering Pulse stream %p from %lld-byte starvation gap "
                        "(writable %zu, tolerance %llu, sane limit %llu).\n", stream,
                        (long long)gap, writable, (unsigned long long)max_gap,
                        (unsigned long long)sane_gap);
                stream->rebase_write_index = TRUE;
                stream->just_underran = FALSE;
                stream->pa_started = FALSE;
                stream->timeline_start_period_time = 0;
                stream->timeline_start_stream_time = 0;
                if (stream->period && stream->period->timer_stream == stream)
                {
                    stream->period->timer_stream = NULL;
                    stream->period->adjust = 0;
                }
                break;
            }

            WARN("Ignoring implausible Pulse timing gap %lld bytes, limit %llu.\n",
                    (long long)gap, (unsigned long long)max_gap);
            break;
        }

        max_write = min(stream->period_bytes, silence_buf_size);
        to_write = min((uint64_t)gap, (uint64_t)max_write) / frame_size * frame_size;
        if (!to_write) break;
        if (!pulse_stream_ready(stream))
            break;
        pa_stream_write(stream->stream, silence_buf, to_write, NULL, 0, PA_SEEK_RELATIVE);
        if (!pulse_stream_ready(stream))
            break;
        ti = pa_stream_get_timing_info(stream->stream);
        if (ti && ti->write_index <= write_index)
        {
            WARN("write index did not advance.\n");
            break;
        }
    }
}

static void pulse_write(struct pulse_stream *stream)
{
    /* write as much data to PA as we can */
    UINT32 to_write;
    BYTE *buf = stream->local_buffer + stream->pa_offs_bytes;
    UINT32 bytes;

    if (pulse_stream_haptic(stream))
    {
        pulse_haptic_write(stream);
        return;
    }

    if (stream->dualsense_mono_registered
            && (stream->dualsense_mono_hotplug_generation != g_dualsense_mono_speaker_add_generation
                || stream->haptic_hotplug_generation != g_haptic_hotplug_generation
                || !pulse_stream_ready(stream))
            && FAILED(pulse_retarget_dualsense_mono_stream(stream, g_dualsense_mono_preferred_sink)))
        return;

    if (pulse_stream_dualsense_mono(stream) && !pulse_stream_ready(stream))
        return;

    bytes = pa_stream_writable_size(stream->stream);

    if (stream->just_underran)
    {
        /* Do not fill an accumulated starvation request before rebasing it. */
        if (!stream->rebase_write_index && stream->pa_held_bytes < bytes)
        {
            to_write = bytes - stream->pa_held_bytes;
            TRACE("prebuffering %u frames of silence\n",
                    (int)(to_write / pa_frame_size(&stream->ss)));
            if (silence_buf && to_write > silence_buf_size)
            {
                free(silence_buf);
                silence_buf = NULL;
            }
            if (!silence_buf)
            {
                silence_buf_size = max(silence_buf_size, to_write);
                silence_buf = calloc(1, silence_buf_size);
            }
            pa_stream_write(stream->stream, silence_buf, to_write, NULL, 0, PA_SEEK_RELATIVE);
        }

        stream->just_underran = FALSE;
    }

    buf = stream->local_buffer + stream->pa_offs_bytes;
    TRACE("held: %lu, avail: %u\n", stream->pa_held_bytes, bytes);
    bytes = min(stream->pa_held_bytes, bytes);

    if (stream->pa_offs_bytes + bytes > stream->real_bufsize_bytes)
    {
        to_write = stream->real_bufsize_bytes - stream->pa_offs_bytes;
        TRACE("writing small chunk of %u bytes\n", to_write);
        write_buffer(stream, buf, to_write);
        stream->pa_held_bytes -= to_write;
        to_write = bytes - to_write;
        stream->pa_offs_bytes = 0;
        buf = stream->local_buffer;
    }
    else
        to_write = bytes;

    TRACE("writing main chunk of %u bytes\n", to_write);
    write_buffer(stream, buf, to_write);
    stream->pa_offs_bytes += to_write;
    stream->pa_offs_bytes %= stream->real_bufsize_bytes;
    stream->pa_held_bytes -= to_write;
}

static void pulse_read(struct pulse_stream *stream)
{
    size_t bytes = pa_stream_readable_size(stream->stream);

    TRACE("Readable total: %zu, fragsize: %u\n", bytes, pa_stream_get_buffer_attr(stream->stream)->fragsize);

    bytes += stream->peek_len - stream->peek_ofs;

    while (bytes >= stream->period_bytes)
    {
        BYTE *dst = NULL, *src;
        size_t src_len, copy, rem = stream->period_bytes;

        if (stream->started)
        {
            LARGE_INTEGER stamp, freq;
            ACPacket *p, *next;

            if (!(p = (ACPacket*)list_head(&stream->packet_free_head)))
            {
                p = (ACPacket*)list_head(&stream->packet_filled_head);
                if (!p) return;
                if (!p->discont) {
                    next = (ACPacket*)p->entry.next;
                    next->discont = 1;
                } else
                    p = (ACPacket*)list_tail(&stream->packet_filled_head);
            }
            else
            {
                stream->held_bytes += stream->period_bytes;
            }
            NtQueryPerformanceCounter(&stamp, &freq);
            p->qpcpos = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
            p->discont = 0;
            list_remove(&p->entry);
            list_add_tail(&stream->packet_filled_head, &p->entry);

            dst = p->data;
        }

        while (rem)
        {
            if (stream->peek_len)
            {
                copy = min(rem, stream->peek_len - stream->peek_ofs);

                if (dst)
                {
                    memcpy(dst, stream->peek_buffer + stream->peek_ofs, copy);
                    dst += copy;
                }

                rem -= copy;
                stream->peek_ofs += copy;
                if(stream->peek_len == stream->peek_ofs)
                    stream->peek_len = stream->peek_ofs = 0;

            }
            else if (pa_stream_peek(stream->stream, (const void**)&src, &src_len) == 0 && src_len)
            {
                copy = min(rem, src_len);

                if (dst) {
                    if(src)
                        memcpy(dst, src, copy);
                    else
                        silence_buffer(stream->ss.format, dst, copy);

                    dst += copy;
                }

                rem -= copy;

                if (copy < src_len)
                {
                    if (src_len > stream->peek_buffer_len)
                    {
                        free(stream->peek_buffer);
                        stream->peek_buffer = malloc(src_len);
                        stream->peek_buffer_len = src_len;
                    }

                    if(src)
                        memcpy(stream->peek_buffer, src + copy, src_len - copy);
                    else
                        silence_buffer(stream->ss.format, stream->peek_buffer, src_len - copy);

                    stream->peek_len = src_len - copy;
                    stream->peek_ofs = 0;
                }

                pa_stream_drop(stream->stream);
            }
        }

        bytes -= stream->period_bytes;
    }
}

static NTSTATUS pulse_timer_loop(void *args)
{
    /* Stream's data are read and written from the main loop timer callback. */
    return STATUS_SUCCESS;
}

#define TIMER_ADJUST_DELAY (5 * PA_USEC_PER_SEC)

static void pulse_update_timing_cb(pa_stream *s, int success, void *user)
{
    struct pulse_stream *stream = user;
    struct pulse_period *period = stream->period;
    pa_usec_t period_stream_time, stream_time;
    int err;

    stream->update_timing_info_pending = FALSE;
    if (!success)
    {
        WARN("failed.\n");
        return;
    }

    period_stream_time = period->stream_time;
    if (!stream->pa_started)
    {
        pulse_write_index_catchup(stream);
        return;
    }

    if (period->timer_stream && (!period->timer_stream->pa_started || !period->timer_stream->timeline_start_period_time
                                 || period_stream_time - period->timer_stream->timeline_start_period_time < TIMER_ADJUST_DELAY))
    {
        TRACE("stream %p is no longer timer stream, started %d, period_stream_time %lld, last_time %lld.\n",
            period->timer_stream, period->timer_stream->pa_started, (long long)period_stream_time, (long long)period->timer_stream->timeline_start_period_time);
        period->timer_stream->timeline_start_period_time = 0;
        period->timer_stream = NULL;
    }

    if ((err = pa_stream_get_time(stream->stream, &stream_time)))
    {
        WARN("pa_stream_get_time failed with %d.\n", err);
        return;
    }

    if (!stream->timeline_start_period_time)
    {
        stream->timeline_start_period_time = period_stream_time;
        stream->timeline_start_stream_time = stream_time;
        TRACE("started stream %p timing at rt %lld, stream %lld.\n", stream, (long long)period_stream_time, (long long)stream_time);
    }
    else if (!period->timer_stream && period_stream_time - stream->timeline_start_period_time > TIMER_ADJUST_DELAY)
    {
        period->timer_stream = stream;
        TRACE("stream %p is now timer stream.\n", stream);
    }
    if (period->timer_stream == stream)
    {
        period->adjust = (int64_t)((period_stream_time - stream->timeline_start_period_time) - (stream_time - stream->timeline_start_stream_time)) / 10;
        TRACE("stream %p, peropd diff %lld, stream diff %lld.\n", stream,
              (long long)(period_stream_time - stream->timeline_start_period_time),
              (long long)(stream_time - stream->timeline_start_stream_time));
        if (period->adjust < -(int64_t)(5 * period->period) || period->adjust > (int64_t)(5 * period->period))
        {
            WARN("stream %p, resetting period timing (adjust %lld).\n", stream, (long long)period->adjust);
            period->adjust = 0;
            period->timer_stream = NULL;
            stream->timeline_start_period_time = 0;
            stream->timeline_start_stream_time = 0;
        }
    }
    pulse_write_index_catchup(stream);
}


static void pa_streams_timer_cb(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv, void *userdata)
{
    struct pulse_period *period = userdata;
    struct pulse_stream *stream, *next;
    BOOL reset_timeline = FALSE;
    pa_usec_t next_timer;
    int64_t adjust = 0;
    UINT32 adv_bytes;
    pa_operation *o;
    pa_usec_t now;

    period->stream_time += period->period;
    now = pa_rtclock_now();
    if (period->timer_last_time + period->period < now)
    {
        WARN("Next period is in the past, resetting timeline.\n");
        period->timer_last_time = now;
        period->adjust = 0;
        reset_timeline = TRUE;
    }

    LIST_FOR_EACH_ENTRY_SAFE(stream, next, &period->streams, struct pulse_stream, period_entry)
    {
        if (stream->started)
        {
            if (reset_timeline)
            {
                stream->timeline_start_period_time = 0;
                stream->timeline_start_stream_time = 0;
            }
            if (!pulse_stream_haptic(stream) && !pulse_stream_ready(stream))
                continue;
            if (!pulse_stream_haptic(stream) && !stream->update_timing_info_pending && (o = pa_stream_update_timing_info(stream->stream, pulse_update_timing_cb, stream)))
            {
                pa_operation_unref(o);
                stream->update_timing_info_pending = TRUE;
            }
            else if (!pulse_stream_haptic(stream) && stream->update_timing_info_pending)
            {
                TRACE("pa_stream_update_timing_info is still pending.\n");
            }
            else if (!pulse_stream_haptic(stream))
            {
                ERR("pa_stream_update_timing_info err %d.\n", pa_context_errno(pulse_ctx));
            }
            if (stream->dataflow == eRender && stream->held_bytes)
            {
                pulse_write(stream);

                if (!pulse_stream_haptic(stream) || pulse_split_speaker_ready(stream))
                {
                    /* Pace normal Pulse and split speaker streams by one logical period. */
                    adv_bytes = min(stream->period_bytes, stream->held_bytes);
                    stream->lcl_offs_bytes += adv_bytes;
                    stream->lcl_offs_bytes %= stream->real_bufsize_bytes;
                    stream->held_bytes -= adv_bytes;
                }
            }
            else if (stream->dataflow == eCapture)
            {
                pulse_read(stream);
            }
        }
        if (stream->event)
            NtSetEvent(stream->event, NULL);
    }

    adjust = period->adjust;
    if (adjust > (int64_t)(period->period / 3))
        adjust = period->period / 3;
    else if (adjust < -(int64_t)(period->period / 3))
        adjust = -(int64_t)period->period / 3;
    next_timer = period->timer_last_time + period->period + adjust;
    TRACE("period %p, timer_last_time %llu, next_timer %llu, adjust %lld.\n",
            period, (long long)period->timer_last_time, (long long)next_timer, (long long)period->adjust);
    period->timer_last_time = next_timer;
    period->adjust = 0;
    pa_context_rttime_restart(pulse_ctx, e, next_timer);
}

static void pa_streams_timer_cb_destroy(pa_mainloop_api *api, pa_time_event *e, void *userdata)
{
    struct pulse_period *period = userdata;

    TRACE("period %p.\n", period);

    list_remove(&period->entry);
    free(period->device);
    free(period);
}

static void remove_stream_from_period(struct pulse_stream *stream)
{
    if (!stream->period)
        return;

    if (stream->period->timer_stream == stream)
        stream->period->timer_stream = NULL;

    list_remove(&stream->period_entry);
    if (list_empty(&stream->period->streams) && pulse_ml)
    {
        pa_mainloop_api *api = pa_mainloop_get_api(pulse_ml);

        TRACE("freeing time event for period %p.\n", stream->period);
        api->time_free(stream->period->time_event);
        stream->period->time_event = NULL;
    }
}

static void pulse_add_stream_to_period(struct pulse_stream *stream)
{
    struct pulse_period *period;
    pa_mainloop_api *api;

    if ((period = stream->period))
    {
        assert(stream->mmdev_period_usec == period->period);
        assert(!strcmp(stream->device, period->device));
        /* */
        list_remove(&stream->period_entry);
        list_add_tail(&period->streams, &stream->period_entry);
        return;
    }

    LIST_FOR_EACH_ENTRY(period, &active_periods, struct pulse_period, entry)
    {
        if (!period->time_event)
        {
            /* Period is being removed but pa_streams_timer_cb_destroy was not called yet. */
            continue;
        }
        if (period->period == stream->mmdev_period_usec && !strcmp(period->device, stream->device))
        {
            TRACE("Using period %p.\n", period);
            stream->period = period;
            list_add_tail(&period->streams, &stream->period_entry);
            return;
        }
    }

    period = calloc(1, sizeof(*period));
    period->period = stream->mmdev_period_usec;
    period->device = strdup(stream->device);
    list_init(&period->streams);
    stream->period = period;
    list_add_tail(&period->streams, &stream->period_entry);
    list_add_tail(&active_periods, &period->entry);
    period->timer_last_time = pa_rtclock_now() + period->period;
    period->time_event = pa_context_rttime_new(pulse_ctx, period->timer_last_time,
            pa_streams_timer_cb, period);
    api = pa_mainloop_get_api(pulse_ml);
    api->time_set_destroy(period->time_event, pa_streams_timer_cb_destroy);
    TRACE("Created period %p, %s, %lld.\n", period, debugstr_a(period->device), (long long)period->period);
}

static NTSTATUS pulse_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    if(params->timer_thread) {
        NtWaitForSingleObject(params->timer_thread, FALSE, NULL);
        NtClose(params->timer_thread);
    }

    pulse_lock();
    remove_stream_from_period(stream);
    if (stream->dualsense_mono_registered)
    {
        list_remove(&stream->dualsense_mono_entry);
        stream->dualsense_mono_registered = FALSE;
    }
    if (stream->dualsense_haptic_registered)
    {
        list_remove(&stream->dualsense_haptic_entry);
        stream->dualsense_haptic_registered = FALSE;
    }
    if (stream->stream)
    {
        if (PA_STREAM_IS_GOOD(pa_stream_get_state(stream->stream))) {
            pa_stream_disconnect(stream->stream);
            while (pulse_ml && PA_STREAM_IS_GOOD(pa_stream_get_state(stream->stream)))
                pulse_cond_wait();
        }
        pa_stream_unref(stream->stream);
    }
    pulse_split_speaker_disconnect(stream);
    if (stream->haptic_pcm)
    {
        snd_pcm_drop(stream->haptic_pcm);
        snd_pcm_close(stream->haptic_pcm);
    }
    pulse_unlock();

    if (stream->tmp_buffer) {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                            &size, MEM_RELEASE);
    }
    if (stream->local_buffer) {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                            &size, MEM_RELEASE);
    }
    free(stream->peek_buffer);
    free(stream->haptic_buffer);
    free(stream->speaker_buffer);
    free(stream->device);
    free(stream->speaker_device);
    free(stream->haptic_alsa_path);
    free(stream);
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_start(void *args)
{
    struct start_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    int success;

    params->result = S_OK;
    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
        return STATUS_SUCCESS;
    }

    if (stream->started)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_NOT_STOPPED;
        return STATUS_SUCCESS;
    }

    if (pulse_stream_haptic(stream))
    {
        if (pulse_haptic_refresh_path(stream))
        {
            snd_pcm_prepare(stream->haptic_pcm);
            pulse_write(stream);
        }
        else
            params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        if (SUCCEEDED(params->result))
            pulse_split_speaker_set_corked(stream, FALSE);
    }
    else
    {
        if (pulse_stream_ready(stream))
        {
            pulse_write(stream);

            if (pa_stream_is_corked(stream->stream))
            {
                if (!wait_pa_operation_complete(pa_stream_cork(stream->stream, 0, pulse_op_cb, &success)))
                    success = 0;
                if (!success)
                    params->result = E_FAIL;
            }
        }
    }

    if (SUCCEEDED(params->result))
    {
        stream->started = TRUE;
        pulse_add_stream_to_period(stream);
    }
    pulse_unlock();
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_stop(void *args)
{
    struct stop_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    int success;

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (!stream->started)
    {
        pulse_unlock();
        params->result = S_FALSE;
        return STATUS_SUCCESS;
    }

    params->result = S_OK;
    if (pulse_stream_haptic(stream))
    {
        if (stream->haptic_pcm)
        {
            snd_pcm_drop(stream->haptic_pcm);
            snd_pcm_prepare(stream->haptic_pcm);
        }
        pulse_split_speaker_set_corked(stream, TRUE);
    }
    else if (stream->dataflow == eRender && pulse_stream_ready(stream))
    {
        if (!wait_pa_operation_complete(pa_stream_cork(stream->stream, 1, pulse_op_cb, &success)))
            success = 0;
        if (!success)
            params->result = E_FAIL;
    }
    if (SUCCEEDED(params->result))
    {
        stream->started = FALSE;
        stream->pa_started = FALSE;
        stream->timeline_start_period_time = 0;
        stream->timeline_start_stream_time = 0;
    }
    pulse_unlock();
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_reset(void *args)
{
    struct reset_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (stream->started)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_NOT_STOPPED;
        return STATUS_SUCCESS;
    }

    if (stream->locked)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
        return STATUS_SUCCESS;
    }

    if (stream->dataflow == eRender)
    {
        /* If there is still data in the render buffer it needs to be removed from the server */
        int success = 0;
        if ((stream->held_bytes || stream->speaker_buffer_held) && pulse_stream_haptic(stream))
        {
            if (stream->haptic_pcm)
            {
                snd_pcm_drop(stream->haptic_pcm);
                snd_pcm_prepare(stream->haptic_pcm);
            }
            if (pulse_split_speaker_ready(stream))
            {
                int speaker_success = 0;

                wait_pa_operation_complete(pa_stream_flush(stream->speaker_stream,
                        pulse_op_cb, &speaker_success));
            }
            success = 1;
        }
        else if (stream->held_bytes && pulse_stream_ready(stream))
            wait_pa_operation_complete(pa_stream_flush(stream->stream, pulse_op_cb, &success));

        if (success || !stream->held_bytes)
        {
            stream->clock_lastpos = stream->clock_written = 0;
            stream->pa_offs_bytes = stream->lcl_offs_bytes = 0;
            stream->held_bytes = stream->pa_held_bytes = 0;
            stream->speaker_buffer_held = 0;
        }
    }
    else
    {
        ACPacket *p;
        stream->clock_written += stream->held_bytes;
        stream->held_bytes = 0;

        if ((p = stream->locked_ptr))
        {
            stream->locked_ptr = NULL;
            list_add_tail(&stream->packet_free_head, &p->entry);
        }
        list_move_tail(&stream->packet_free_head, &stream->packet_filled_head);
    }
    pulse_unlock();
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static BOOL alloc_tmp_buffer(struct pulse_stream *stream, SIZE_T bytes)
{
    SIZE_T size;

    if (stream->tmp_buffer_bytes >= bytes)
        return TRUE;

    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                            &size, MEM_RELEASE);
        stream->tmp_buffer = NULL;
        stream->tmp_buffer_bytes = 0;
    }
    if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                                zero_bits, &bytes, MEM_COMMIT, PAGE_READWRITE))
        return FALSE;

    stream->tmp_buffer_bytes = bytes;
    return TRUE;
}

static UINT32 pulse_render_padding(struct pulse_stream *stream)
{
    return stream->held_bytes / pa_frame_size(&stream->ss);
}

static UINT32 pulse_capture_padding(struct pulse_stream *stream)
{
    ACPacket *packet = stream->locked_ptr;
    if (!packet && !list_empty(&stream->packet_filled_head))
    {
        packet = (ACPacket*)list_head(&stream->packet_filled_head);
        stream->locked_ptr = packet;
        list_remove(&packet->entry);
    }
    return stream->held_bytes / pa_frame_size(&stream->ss);
}

static NTSTATUS pulse_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    size_t bytes;
    UINT32 wri_offs_bytes;

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (stream->locked)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        return STATUS_SUCCESS;
    }

    if (!params->frames)
    {
        pulse_unlock();
        *params->data = NULL;
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    if (stream->held_bytes / pa_frame_size(&stream->ss) + params->frames > stream->bufsize_frames)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        return STATUS_SUCCESS;
    }

    bytes = params->frames * pa_frame_size(&stream->ss);
    wri_offs_bytes = (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    if (wri_offs_bytes + bytes > stream->real_bufsize_bytes)
    {
        if (!alloc_tmp_buffer(stream, bytes))
        {
            pulse_unlock();
            params->result = E_OUTOFMEMORY;
            return STATUS_SUCCESS;
        }
        *params->data = stream->tmp_buffer;
        stream->locked = -bytes;
    }
    else
    {
        *params->data = stream->local_buffer + wri_offs_bytes;
        stream->locked = bytes;
    }

    silence_buffer(stream->ss.format, *params->data, bytes);

    pulse_unlock();
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static void pulse_wrap_buffer(struct pulse_stream *stream, BYTE *buffer, UINT32 written_bytes)
{
    UINT32 wri_offs_bytes = (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    UINT32 chunk_bytes = stream->real_bufsize_bytes - wri_offs_bytes;

    if (written_bytes <= chunk_bytes)
    {
        memcpy(stream->local_buffer + wri_offs_bytes, buffer, written_bytes);
    }
    else
    {
        memcpy(stream->local_buffer + wri_offs_bytes, buffer, chunk_bytes);
        memcpy(stream->local_buffer, buffer + chunk_bytes, written_bytes - chunk_bytes);
    }
}

static NTSTATUS pulse_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    UINT32 written_bytes;
    BYTE *buffer;

    pulse_lock();
    if (!stream->locked || !params->written_frames)
    {
        stream->locked = 0;
        pulse_unlock();
        params->result = params->written_frames ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
        return STATUS_SUCCESS;
    }

    if (params->written_frames * pa_frame_size(&stream->ss) >
        (stream->locked >= 0 ? stream->locked : -stream->locked))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_INVALID_SIZE;
        return STATUS_SUCCESS;
    }

    if (stream->locked >= 0)
        buffer = stream->local_buffer + (stream->lcl_offs_bytes + stream->held_bytes) % stream->real_bufsize_bytes;
    else
        buffer = stream->tmp_buffer;

    written_bytes = params->written_frames * pa_frame_size(&stream->ss);
    if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
        silence_buffer(stream->ss.format, buffer, written_bytes);

    /* Keep controller-speaker delivery on the application's render cadence.
     * The raw actuator PCM may temporarily report no writable frames, but it
     * must not stall or burst the independent mono speaker stream. */
    if (pulse_split_speaker_ready(stream))
        pulse_split_speaker_write(stream, buffer, params->written_frames);

    if (stream->locked < 0)
        pulse_wrap_buffer(stream, buffer, written_bytes);

    stream->held_bytes += written_bytes;
    stream->pa_held_bytes += written_bytes;
    if (stream->pa_held_bytes > stream->max_pa_held_bytes)
    {
        stream->max_pa_held_bytes = stream->pa_held_bytes;
        TRACE("%p max_pa_held_bytes %lld.\n", stream, (long long)stream->max_pa_held_bytes);
    }
    if (stream->pa_held_bytes > stream->real_bufsize_bytes)
    {
        WARN("%p PA buffer overflow.\n", stream);
        stream->max_pa_held_bytes = 0;
        stream->pa_offs_bytes = stream->lcl_offs_bytes;
        stream->pa_held_bytes = stream->held_bytes;
    }
    stream->clock_written += written_bytes;
    stream->locked = 0;

    /* push as much data as we can to pulseaudio too */
    pulse_write(stream);

    TRACE("Released %u, held %lu\n", params->written_frames, stream->held_bytes / pa_frame_size(&stream->ss));

    pulse_unlock();
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    ACPacket *packet;

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    if (stream->locked)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        return STATUS_SUCCESS;
    }

    pulse_capture_padding(stream);
    if ((packet = stream->locked_ptr))
    {
        *params->frames = stream->period_bytes / pa_frame_size(&stream->ss);
        *params->flags = 0;
        if (packet->discont)
            *params->flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        if (params->devpos)
        {
            if (packet->discont)
                *params->devpos = (stream->clock_written + stream->period_bytes) / pa_frame_size(&stream->ss);
            else
                *params->devpos = stream->clock_written / pa_frame_size(&stream->ss);
        }
        if (params->qpcpos)
            *params->qpcpos = packet->qpcpos;
        *params->data = packet->data;
    }
    else
        *params->frames = 0;
    stream->locked = *params->frames;
    pulse_unlock();
    params->result =  *params->frames ? S_OK : AUDCLNT_S_BUFFER_EMPTY;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    if (!stream->locked && params->done)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        return STATUS_SUCCESS;
    }
    if (params->done && stream->locked != params->done)
    {
        pulse_unlock();
        params->result = AUDCLNT_E_INVALID_SIZE;
        return STATUS_SUCCESS;
    }
    if (params->done)
    {
        ACPacket *packet = stream->locked_ptr;
        stream->locked_ptr = NULL;
        stream->held_bytes -= stream->period_bytes;
        if (packet->discont)
            stream->clock_written += 2 * stream->period_bytes;
        else
            stream->clock_written += stream->period_bytes;
        list_add_tail(&stream->packet_free_head, &packet->entry);
    }
    stream->locked = 0;
    pulse_unlock();
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;

    if (params->sony_windows_audio_mode &&
            !InterlockedExchange(&g_sony_windows_audio_mode, 1))
        TRACE("Selected automatically detected Windows Sony audio mode.\n");

    /* This driver does not support exclusive mode. */
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE) {
        WARN("Exclusive mode requested but winepulse.drv does not support exclusive mode.\n");
        params->result = AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED;
    } else {
        params->result = S_OK;
    }

    return STATUS_SUCCESS;
}

static void sink_name_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
    uint32_t *current_device_index = userdata;
    pulse_broadcast();

    if (!i || !i->name || !i->name[0])
        return;
    *current_device_index = i->index;
}

struct find_monitor_of_sink_cb_param
{
    struct get_loopback_capture_device_params *params;
    uint32_t current_device_index;
};

static void find_monitor_of_sink_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata)
{
    struct find_monitor_of_sink_cb_param *p = userdata;
    unsigned int len;

    pulse_broadcast();

    if (!i || !i->name || !i->name[0])
        return;
    if (i->monitor_of_sink != p->current_device_index)
        return;

    len = strlen(i->name) + 1;
    if (len <= p->params->ret_device_len)
    {
        memcpy(p->params->ret_device, i->name, len);
        p->params->result = STATUS_SUCCESS;
        return;
    }
    p->params->ret_device_len = len;
    p->params->result = STATUS_BUFFER_TOO_SMALL;
}

static NTSTATUS pulse_get_loopback_capture_device(void *args)
{
    struct get_loopback_capture_device_params *params = args;
    uint32_t current_device_index = PA_INVALID_INDEX;
    struct find_monitor_of_sink_cb_param p;
    const char *device_name;
    char *resolved_device = NULL;
    char *name;

    pulse_lock();

    if (!pulse_ml)
    {
        pulse_unlock();
        ERR("Called without main loop running.\n");
        params->result = E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    name = wstr_to_str(params->name);
    params->result = pulse_connect(name);
    free(name);

    if (FAILED(params->result))
    {
        pulse_unlock();
        return STATUS_SUCCESS;
    }

    device_name = params->device;
    if (device_name && !device_name[0]) device_name = NULL;
    if ((resolved_device = pulse_resolve_device_name(device_name)))
        device_name = resolved_device;

    params->result = E_FAIL;
    wait_pa_operation_complete(pa_context_get_sink_info_by_name(pulse_ctx, device_name, &sink_name_info_cb, &current_device_index));
    if (current_device_index != PA_INVALID_INDEX)
    {
        p.current_device_index = current_device_index;
        p.params = params;
        wait_pa_operation_complete(pa_context_get_source_info_list(pulse_ctx, &find_monitor_of_sink_cb, &p));
    }

    pulse_unlock();
    free(resolved_device);
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;
    struct list *list = (params->flow == eRender) ? &g_phys_speakers : &g_phys_sources;
    PhysDevice *dev;

    if (params->sony_windows_audio_mode &&
            !InterlockedExchange(&g_sony_windows_audio_mode, 1))
        TRACE("Selected Windows Sony audio mode before returning the mix format.\n");

    g_phys_lock();
    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry) {
        if (!pulse_device_matches(dev, params->device))
            continue;

        apply_windows_sony_audio_format(dev);
        *params->fmt = dev->fmt;
        g_phys_unlock();
        params->result = S_OK;
        return STATUS_SUCCESS;
    }
    g_phys_unlock();

    params->result = E_FAIL;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    params->result = get_device_period_helper(params->flow, params->device, params->def_period, params->min_period);
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    params->result = S_OK;

    pulse_lock();
    if (!pulse_stream_valid(stream))
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    else
        *params->frames = stream->bufsize_frames;
    pulse_unlock();

    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    const pa_buffer_attr *attr;
    REFERENCE_TIME lat;

    pulse_lock();
    if (!pulse_stream_valid(stream)) {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }
    attr = pulse_stream_ready(stream) ? pa_stream_get_buffer_attr(stream->stream) : &stream->attr;
    if (stream->dataflow == eRender)
        lat = attr->minreq / pa_frame_size(&stream->ss);
    else
        lat = attr->fragsize / pa_frame_size(&stream->ss);
    *params->latency = (lat * 10000000) / stream->ss.rate + stream->def_period;
    pulse_unlock();
    TRACE("Latency: %u ms\n", (unsigned)(*params->latency / 10000));
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    if (stream->dataflow == eRender)
        *params->padding = pulse_render_padding(stream);
    else
        *params->padding = pulse_capture_padding(stream);
    pulse_unlock();

    TRACE("%p Pad: %u ms (%u)\n", stream, muldiv(*params->padding, 1000, stream->ss.rate),
          *params->padding);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    pulse_capture_padding(stream);
    if (stream->locked_ptr)
        *params->frames = stream->period_bytes / pa_frame_size(&stream->ss);
    else
        *params->frames = 0;
    pulse_unlock();
    params->result = S_OK;

    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    *params->freq = stream->ss.rate;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq *= pa_frame_size(&stream->ss);
    pulse_unlock();
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_get_position(void *args)
{
    struct get_position_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    if (!pulse_stream_valid(stream))
    {
        pulse_unlock();
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        return STATUS_SUCCESS;
    }

    *params->pos = stream->clock_written - stream->held_bytes;

    if (stream->share == AUDCLNT_SHAREMODE_EXCLUSIVE || params->device)
        *params->pos /= pa_frame_size(&stream->ss);

    /* Make time never go backwards */
    if (*params->pos < stream->clock_lastpos)
        *params->pos = stream->clock_lastpos;
    else
        stream->clock_lastpos = *params->pos;
    pulse_unlock();

    TRACE("%p Position: %u\n", stream, (unsigned)*params->pos);

    if (params->qpctime)
    {
        LARGE_INTEGER stamp, freq;
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    unsigned int i;

    for (i = 0; i < stream->ss.channels; i++)
        stream->vol[i] = params->volumes[i] * params->master_volume * params->session_volumes[i];

    return STATUS_SUCCESS;
}

static NTSTATUS pulse_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;

    pulse_lock();
    if (!pulse_stream_valid(stream))
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = params->event;
    pulse_unlock();

    params->result = hr;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_set_sample_rate(void *args)
{
    struct set_sample_rate_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;
    int success;
    pa_sample_spec new_ss;

    pulse_lock();
    if (!pulse_stream_valid(stream)) {
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
        goto exit;
    }
    if (stream->dataflow != eRender) {
        hr = E_NOTIMPL;
        goto exit;
    }

    new_ss = stream->ss;
    new_ss.rate = params->rate;

    if (pulse_stream_ready(stream))
    {
        if (!wait_pa_operation_complete(pa_stream_update_sample_rate(stream->stream, params->rate, pulse_op_cb, &success)))
            success = 0;

        if (!success) {
            hr = E_OUTOFMEMORY;
            goto exit;
        }
    }

    if (stream->held_bytes && pulse_stream_ready(stream))
        wait_pa_operation_complete(pa_stream_flush(stream->stream, pulse_op_cb, &success));

    stream->clock_lastpos = stream->clock_written = 0;
    stream->pa_offs_bytes = stream->lcl_offs_bytes = 0;
    stream->held_bytes = stream->pa_held_bytes = 0;
    stream->period_bytes = pa_frame_size(&new_ss) * muldiv(stream->mmdev_period_usec, new_ss.rate, 1000000);
    stream->ss = new_ss;

    silence_buffer(new_ss.format, stream->local_buffer, stream->real_bufsize_bytes);

exit:
    pulse_unlock();

    params->result = hr;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_is_started(void *args)
{
    struct is_started_params *params = args;
    struct pulse_stream *stream = handle_get_stream(params->stream);

    pulse_lock();
    params->result = pulse_stream_valid(stream) && stream->started ? S_OK : S_FALSE;
    pulse_unlock();

    return STATUS_SUCCESS;
}

static BOOL get_device_path(PhysDevice *dev, struct get_prop_value_params *params)
{
    const GUID *guid = params->guid;
    PROPVARIANT *out = params->value;
    UINT serial_number;
    char path[128];
    int len;

    /* As hardly any audio devices have serial numbers, Windows instead
       appears to use a persistent random number. We emulate this here
       by instead using the last 8 hex digits of the GUID. */
    serial_number = (guid->Data4[4] << 24) | (guid->Data4[5] << 16) | (guid->Data4[6] << 8) | guid->Data4[7];

    switch (dev->bus_type) {
    case phys_device_bus_pci:
        len = sprintf(path, "{1}.HDAUDIO\\FUNC_01&VEN_%04X&DEV_%04X\\%u&%08X", dev->vendor_id, dev->product_id, dev->index, serial_number);
        break;
    case phys_device_bus_usb:
        len = sprintf(path, "{1}.USB\\VID_%04X&PID_%04X\\%u&%08X", dev->vendor_id, dev->product_id, dev->index, serial_number);
        break;
    default:
        len = sprintf(path, "{1}.ROOT\\MEDIA\\%04u", dev->index);
        break;
    }

    if (*params->buffer_size < ++len * sizeof(WCHAR)) {
        params->result = E_NOT_SUFFICIENT_BUFFER;
        *params->buffer_size = len * sizeof(WCHAR);
        return FALSE;
    }

    out->vt = VT_LPWSTR;
    out->pwszVal = params->buffer;

    ntdll_umbstowcs(path, len, out->pwszVal, len);

    params->result = S_OK;

    return TRUE;
}

static NTSTATUS pulse_get_prop_value(void *args)
{
    static const GUID PKEY_AudioEndpoint_GUID = {
        0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}
    };
    static const PROPERTYKEY devicepath_key = { /* undocumented? - {b3f8fa53-0004-438e-9003-51a46e139bfc},2 */
        {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2
    };
    struct get_prop_value_params *params = args;
    struct list *list = (params->flow == eRender) ? &g_phys_speakers : &g_phys_sources;
    PhysDevice *dev;

    g_phys_lock();
    LIST_FOR_EACH_ENTRY(dev, list, PhysDevice, entry) {
        if (!pulse_device_matches(dev, params->device))
            continue;
        if (IsEqualPropertyKey(*params->prop, devicepath_key)) {
            get_device_path(dev, params);

            g_phys_unlock();
            return STATUS_SUCCESS;
        } else if (IsEqualGUID(&params->prop->fmtid, &PKEY_AudioEndpoint_GUID)) {
            switch (params->prop->pid) {
            case 0:   /* FormFactor */
                params->value->vt = VT_UI4;
                params->value->ulVal = dev->form;

                g_phys_unlock();
                params->result = S_OK;
                return STATUS_SUCCESS;
            case 3:   /* PhysicalSpeakers */
                if (!dev->channel_mask)
                    goto fail;
                params->value->vt = VT_UI4;
                params->value->ulVal = dev->channel_mask;

                g_phys_unlock();
                params->result = S_OK;
                return STATUS_SUCCESS;
            }
        } else if (IsEqualGUID(&params->prop->fmtid, &DEVPKEY_Device_ContainerId)) {
            if (!params->buffer || *params->buffer_size < sizeof(*params->value->puuid)) {
                *params->buffer_size = sizeof(*params->value->puuid);
                params->result = E_NOT_SUFFICIENT_BUFFER;
            } else {
                params->value->vt = VT_CLSID;
                params->value->puuid = params->buffer;
                *params->value->puuid = dev->container_id;
                params->result = S_OK;
            }

            g_phys_unlock();
            return STATUS_SUCCESS;
        }

        g_phys_unlock();
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

fail:
    g_phys_unlock();
    params->result = E_FAIL;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_midi_get_driver(void *args)
{
    static const WCHAR driver[] = {'a','l','s','a',0};

    memcpy( args, driver, sizeof(driver) );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    pulse_process_attach,
    pulse_process_detach,
    pulse_main_loop,
    pulse_get_endpoint_ids,
    pulse_create_stream,
    pulse_release_stream,
    pulse_start,
    pulse_stop,
    pulse_reset,
    pulse_timer_loop,
    pulse_get_render_buffer,
    pulse_release_render_buffer,
    pulse_get_capture_buffer,
    pulse_release_capture_buffer,
    pulse_is_format_supported,
    pulse_get_loopback_capture_device,
    pulse_get_mix_format,
    pulse_get_device_period,
    pulse_get_buffer_size,
    pulse_get_latency,
    pulse_get_current_padding,
    pulse_get_next_packet_size,
    pulse_get_frequency,
    pulse_get_position,
    pulse_set_volumes,
    pulse_set_event_handle,
    pulse_set_sample_rate,
    pulse_test_connect,
    pulse_is_started,
    pulse_get_prop_value,
    pulse_midi_get_driver,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS pulse_wow64_main_loop(void *args)
{
    struct
    {
        PTR32 event;
    } *params32 = args;
    struct main_loop_params params =
    {
        .event = ULongToHandle(params32->event)
    };
    return pulse_main_loop(&params);
}

static NTSTATUS pulse_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    pulse_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
        BOOL sony_windows_audio_mode;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream),
        .sony_windows_audio_mode = params32->sony_windows_audio_mode,
    };
    pulse_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 timer_thread;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
        .timer_thread = ULongToHandle(params32->timer_thread)
    };
    pulse_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    pulse_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .data = &data,
        .frames = ULongToPtr(params32->frames),
        .flags = ULongToPtr(params32->flags),
        .devpos = ULongToPtr(params32->devpos),
        .qpcpos = ULongToPtr(params32->qpcpos)
    };
    pulse_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
};

static NTSTATUS pulse_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
        BOOL sony_windows_audio_mode;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
        .sony_windows_audio_mode = params32->sony_windows_audio_mode,
    };
    pulse_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_loopback_capture_device(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        PTR32 ret_device;
        UINT32 ret_device_len;
        HRESULT result;
    } *params32 = args;

    struct get_loopback_capture_device_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .ret_device = ULongToPtr(params32->ret_device),
        .ret_device_len = params32->ret_device_len,
    };

    pulse_get_loopback_capture_device(&params);
    params32->result = params.result;
    params32->ret_device_len = params.ret_device_len;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
        BOOL sony_windows_audio_mode;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt),
        .sony_windows_audio_mode = params32->sony_windows_audio_mode,
    };
    pulse_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    pulse_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    pulse_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    pulse_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    pulse_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    pulse_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    pulse_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    pulse_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return pulse_set_volumes(&params);
}

static NTSTATUS pulse_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    pulse_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name),
    };
    pulse_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS pulse_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer; /* caller allocated buffer to hold value's strings */
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    pulse_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
        case VT_CLSID:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    pulse_process_attach,
    pulse_process_detach,
    pulse_wow64_main_loop,
    pulse_wow64_get_endpoint_ids,
    pulse_wow64_create_stream,
    pulse_wow64_release_stream,
    pulse_start,
    pulse_stop,
    pulse_reset,
    pulse_timer_loop,
    pulse_wow64_get_render_buffer,
    pulse_release_render_buffer,
    pulse_wow64_get_capture_buffer,
    pulse_release_capture_buffer,
    pulse_wow64_is_format_supported,
    pulse_wow64_get_loopback_capture_device,
    pulse_wow64_get_mix_format,
    pulse_wow64_get_device_period,
    pulse_wow64_get_buffer_size,
    pulse_wow64_get_latency,
    pulse_wow64_get_current_padding,
    pulse_wow64_get_next_packet_size,
    pulse_wow64_get_frequency,
    pulse_wow64_get_position,
    pulse_wow64_set_volumes,
    pulse_wow64_set_event_handle,
    pulse_set_sample_rate,
    pulse_wow64_test_connect,
    pulse_is_started,
    pulse_wow64_get_prop_value,
    pulse_midi_get_driver,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
    pulse_not_implemented,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
