/*
 * Copyright 2016 Michael Müller
 * Copyright 2017 Andrey Gusev
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

#define COBJMACROS

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windows.h"
#include "appmodel.h"
#include "shlwapi.h"
#include "perflib.h"
#include "winternl.h"
#include "winperf.h"

#include "wine/debug.h"
#include "kernelbase.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(kernelbase);


BOOL is_wow64 = FALSE;

#ifdef __x86_64__
static BOOL death_stranding_environment_effects_enabled(void)
{
    static const WCHAR nameW[] = L"PROTON_DEATH_STRANDING_FORCE_ENVIRONMENT_EFFECTS";
    WCHAR buffer[2];
    UNICODE_STRING name, value;

    RtlInitUnicodeString( &name, nameW );
    value.Buffer = buffer;
    value.Length = 0;
    value.MaximumLength = sizeof(buffer);
    return !RtlQueryEnvironmentVariable_U( NULL, &name, &value ) &&
            value.Length == sizeof(WCHAR) && buffer[0] == '1';
}

typedef ULONG (WINAPI *death_stranding_set_rtpc_value_t)(ULONG id, float value,
        ULONGLONG game_object, ULONG transition_duration, ULONG fade_curve,
        ULONG bypass_interpolation, ULONG unknown);
typedef ULONG (WINAPI *death_stranding_set_state_t)(ULONG group, ULONG state);
typedef ULONG (WINAPI *death_stranding_post_event_t)(ULONG id, void *game_object,
        ULONG_PTR arg3, ULONG_PTR arg4, ULONG_PTR arg5, ULONG_PTR arg6,
        ULONG_PTR arg7, ULONG_PTR arg8, ULONG_PTR arg9, ULONG_PTR arg10,
        ULONG_PTR arg11, ULONG_PTR arg12, ULONG_PTR arg13, ULONG_PTR arg14);

struct death_stranding_aux_send_value
{
    ULONGLONG listener;
    ULONG bus;
    float volume;
};

typedef ULONG (WINAPI *death_stranding_set_aux_sends_t)(ULONGLONG game_object,
        struct death_stranding_aux_send_value *values, ULONG count);
typedef ULONG (WINAPI *death_stranding_set_output_bus_volume_t)(ULONGLONG emitter,
        ULONGLONG listener, float volume);
typedef ULONG (WINAPI *death_stranding_set_output_volume_t)(ULONGLONG output, float volume);
typedef ULONG (WINAPI *death_stranding_add_output_t)(const ULONG *settings,
        ULONGLONG *output, const ULONGLONG *listeners, ULONG count);
typedef ULONG (WINAPI *death_stranding_remove_output_t)(ULONGLONG output);
typedef ULONG (WINAPI *death_stranding_set_bus_device_t)(ULONG bus, ULONG device);

struct death_stranding_rtpc_trace_entry
{
    LONG id;
    LONG calls;
};

struct death_stranding_event_trace_entry
{
    LONG id;
    LONG calls;
};

struct death_stranding_routing_trace_entry
{
    LONGLONG game_object;
    LONG controller_bus;
    LONG aux_calls;
    LONG output_calls;
};

struct death_stranding_output_trace_entry
{
    LONGLONG output;
    LONG calls;
};

static death_stranding_set_rtpc_value_t death_stranding_set_rtpc_value_original;
static death_stranding_set_state_t death_stranding_set_state_original;
static death_stranding_post_event_t death_stranding_post_event_original;
static death_stranding_set_aux_sends_t death_stranding_set_aux_sends_original;
static death_stranding_set_output_bus_volume_t death_stranding_set_output_bus_volume_original;
static death_stranding_set_output_volume_t death_stranding_set_output_volume_original;
static death_stranding_add_output_t death_stranding_add_output_original;
static death_stranding_remove_output_t death_stranding_remove_output_original;
static death_stranding_set_bus_device_t death_stranding_set_bus_device_original;
static struct death_stranding_rtpc_trace_entry death_stranding_rtpc_trace_entries[256];
static struct death_stranding_event_trace_entry death_stranding_event_trace_entries[2048];
static struct death_stranding_routing_trace_entry death_stranding_routing_trace_entries[4096];
static struct death_stranding_output_trace_entry death_stranding_output_trace_entries[64];
static LONGLONG death_stranding_environment_objects[64];
static LONGLONG death_stranding_state_trace_entries[512];
static LONG death_stranding_controller_state_initialized;
static LONG death_stranding_add_output_calls;
static LONG death_stranding_remove_output_calls;
static LONG death_stranding_set_bus_device_calls;

static BOOL WINAPI death_stranding_environment_output_enabled(void)
{
    static LONG logged;

    if (!InterlockedExchange(&logged, TRUE))
        WARN("Death Stranding evaluated its environmental controller-speaker capability; "
                "returning true.\n");
    return TRUE;
}

static ULONG WINAPI death_stranding_trace_set_rtpc_value(ULONG id, float value,
        ULONGLONG game_object, ULONG transition_duration, ULONG fade_curve,
        ULONG bypass_interpolation, ULONG unknown)
{
    ULONG value_bits;
    LONG key = id;
    unsigned int i;

    memcpy(&value_bits, &value, sizeof(value_bits));
    for (i = 0; i < ARRAY_SIZE(death_stranding_rtpc_trace_entries); ++i)
    {
        LONG entry_id = InterlockedCompareExchange(
                &death_stranding_rtpc_trace_entries[i].id, key, 0);

        if (entry_id && entry_id != key) continue;
        if (InterlockedIncrement(&death_stranding_rtpc_trace_entries[i].calls) <= 4)
            WARN("Death Stranding set global RTPC %#lx to float bits %#lx.\n",
                    id, value_bits);
        break;
    }
    return death_stranding_set_rtpc_value_original(id, value, game_object,
            transition_duration, fade_curve, bypass_interpolation, unknown);
}

static ULONG WINAPI death_stranding_trace_set_state(ULONG group, ULONG state)
{
    static const ULONG sound_update_graph_group = 0x7a3867ba;
    static const ULONG controller_speaker_group = 0xd517feeb;
    static const ULONG controller_speaker_enabled = 0x2b6e617d;
    LONGLONG pair = ((ULONGLONG)group << 32) | state;
    ULONG controller_rtpc_result, controller_state_result, result;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(death_stranding_state_trace_entries); ++i)
    {
        LONGLONG entry = InterlockedCompareExchange64(
                &death_stranding_state_trace_entries[i], pair, 0);

        if (entry == pair) break;
        if (entry) continue;
        WARN("Death Stranding set new Wwise state pair %#lx -> %#lx.\n", group, state);
        break;
    }

    result = death_stranding_set_state_original(group, state);
    if (group == sound_update_graph_group &&
            !InterlockedCompareExchange(&death_stranding_controller_state_initialized, TRUE, FALSE))
    {
        controller_state_result = death_stranding_set_state_original(controller_speaker_group,
                controller_speaker_enabled);
        controller_rtpc_result = death_stranding_set_rtpc_value_original(controller_speaker_group,
                1.0f, ~(ULONGLONG)0, 0, 4, FALSE, 0);
        WARN("Death Stranding enabled environmental controller-speaker state %#lx -> %#lx: "
                "%#lx; RTPC %#lx -> 1: %#lx.\n", controller_speaker_group,
                controller_speaker_enabled, controller_state_result,
                controller_speaker_group, controller_rtpc_result);
        if (controller_state_result != 1 || controller_rtpc_result != 1)
            InterlockedExchange(&death_stranding_controller_state_initialized, FALSE);
    }
    return result;
}

static ULONG death_stranding_controller_bus_for_event(ULONG id)
{
    switch (id)
    {
        /* Stone and gravel footstep events use sam_fs_stone_debris. */
        case 0x3ca60f68: case 0x8af12b8f:
        case 0x3ca60f6b: case 0x8af12b8c:
        case 0x3ca60f6d: case 0x8af12b8a:
        case 0x39a60a91: case 0x89f12a1a:
        case 0x36a3c762: case 0x10f43d75:
            return 0x31a8bcd4;

        /* Grass reuses the textured sam_fs_snow_thick controller layer. */
        case 0x39a60a95: case 0x89f12a1e:
        case 0x44a61bc2: case 0x82f11f15:
        case 0xf2a69342: case 0x4da0fc9e:
        case 0x311fc17b: case 0x5e7d0c92: case 0xfa90dbab:
            return 0x746a8010;

        /* Water footstep and movement events use sam_fs_water. */
        case 0x39a60a97: case 0x0a2bfefe:
        case 0xb82046fc: case 0xfb1b3b60: case 0x5aef2d94:
            return 0x8065800c;
    }
    return 0;
}

static void death_stranding_set_controller_bus(ULONGLONG game_object, ULONG bus)
{
    unsigned int i;

    if (!game_object || !bus) return;
    for (i = 0; i < ARRAY_SIZE(death_stranding_routing_trace_entries); ++i)
    {
        struct death_stranding_routing_trace_entry *entry =
                &death_stranding_routing_trace_entries[i];
        LONGLONG object = InterlockedCompareExchange64(
                &entry->game_object, game_object, 0);

        if (object && object != game_object) continue;
        InterlockedExchange(&entry->controller_bus, bus);
        return;
    }
}

static ULONG WINAPI death_stranding_trace_post_event(ULONG id, void *game_object,
        ULONG_PTR arg3, ULONG_PTR arg4, ULONG_PTR arg5, ULONG_PTR arg6,
        ULONG_PTR arg7, ULONG_PTR arg8, ULONG_PTR arg9, ULONG_PTR arg10,
        ULONG_PTR arg11, ULONG_PTR arg12, ULONG_PTR arg13, ULONG_PTR arg14)
{
    static const ULONG environment_event_ids[] =
    {
        0xc5c64b7f, 0x1ef78956, 0x9c541536, 0xb8b66b56,
        0x5b58a574, 0x7941a8b8, 0x1f85777b, 0x51c0962c,
        0x9952b771, 0x84f685eb, 0xc6564efe, 0x029c58ff,
    };
    static LONG environment_event_calls[ARRAY_SIZE(environment_event_ids)];
    ULONG controller_bus, result;
    unsigned int i;

    controller_bus = death_stranding_controller_bus_for_event(id);
    if (controller_bus)
        death_stranding_set_controller_bus((ULONG_PTR)game_object, controller_bus);
    result = death_stranding_post_event_original(id, game_object, arg3, arg4,
            arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12, arg13, arg14);

    for (i = 0; i < ARRAY_SIZE(environment_event_ids); ++i)
    {
        LONG calls;
        unsigned int j;

        if (id != environment_event_ids[i]) continue;
        for (j = 0; j < ARRAY_SIZE(death_stranding_environment_objects); ++j)
        {
            LONGLONG object = InterlockedCompareExchange64(
                    &death_stranding_environment_objects[j], (ULONG_PTR)game_object, 0);

            if (!object || object == (ULONG_PTR)game_object) break;
        }
        calls = InterlockedIncrement(&environment_event_calls[i]);
        if (calls <= 16)
            WARN("Death Stranding posted candidate environment event %#lx on Wwise object %p "
                    "(result %#lx, flags %#Ix/%#Ix, call %ld).\n",
                    id, game_object, result, arg3, arg4, calls);
        return result;
    }

    if (id)
    {
        LONG key = id;

        for (i = 0; i < ARRAY_SIZE(death_stranding_event_trace_entries); ++i)
        {
            LONG entry_id = InterlockedCompareExchange(
                    &death_stranding_event_trace_entries[i].id, key, 0);

            if (entry_id && entry_id != key) continue;
            if (InterlockedIncrement(&death_stranding_event_trace_entries[i].calls) == 1)
                WARN("Death Stranding posted new Wwise event %#lx on object %p "
                        "(result %#lx, flags %#Ix/%#Ix).\n",
                        id, game_object, result, arg3, arg4);
            break;
        }
    }
    return result;
}

static BOOL death_stranding_is_environment_object(ULONGLONG game_object)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(death_stranding_environment_objects); ++i)
    {
        LONGLONG object = InterlockedCompareExchange64(
                &death_stranding_environment_objects[i], 0, 0);

        if (object == game_object) return TRUE;
        if (!object) return FALSE;
    }
    return FALSE;
}

static struct death_stranding_routing_trace_entry *death_stranding_get_routing_entry(
        ULONGLONG game_object)
{
    unsigned int i;

    if (!game_object) return NULL;
    for (i = 0; i < ARRAY_SIZE(death_stranding_routing_trace_entries); ++i)
    {
        struct death_stranding_routing_trace_entry *entry =
                &death_stranding_routing_trace_entries[i];
        LONGLONG object = InterlockedCompareExchange64(
                &entry->game_object, game_object, 0);

        if (!object || object == game_object) return entry;
    }
    return NULL;
}

static ULONG WINAPI death_stranding_trace_set_aux_sends(ULONGLONG game_object,
        struct death_stranding_aux_send_value *values, ULONG count)
{
    static const ULONG controller_speaker_bus = 0x5db4f0ba;
    struct death_stranding_aux_send_value routed_values[10];
    struct death_stranding_aux_send_value *submitted_values = values;
    struct death_stranding_routing_trace_entry *entry;
    ULONG buses[2], result, i, j, calls = 0, controller_bus = 0, submitted_count = count;
    BOOL environment_object;

    entry = death_stranding_get_routing_entry(game_object);
    if (entry)
        controller_bus = InterlockedCompareExchange(&entry->controller_bus, 0, 0);
    if (controller_bus && count + ARRAY_SIZE(buses) <= ARRAY_SIZE(routed_values) &&
            (!count || values))
    {
        if (count) memcpy(routed_values, values, count * sizeof(*values));
        submitted_values = routed_values;
        buses[0] = controller_bus;
        buses[1] = controller_speaker_bus;

        for (j = 0; j < ARRAY_SIZE(buses); ++j)
        {
            for (i = 0; i < submitted_count; ++i)
                if (routed_values[i].bus == buses[j]) break;
            if (i != submitted_count) continue;

            routed_values[submitted_count].listener = count ? values[0].listener : 0;
            routed_values[submitted_count].bus = buses[j];
            routed_values[submitted_count].volume = 1.0f;
            ++submitted_count;
        }
    }
    result = death_stranding_set_aux_sends_original(game_object,
            submitted_values, submitted_count);
    environment_object = death_stranding_is_environment_object(game_object);
    if (entry) calls = InterlockedIncrement(&entry->aux_calls);
    if (calls == 1 || (environment_object && calls <= 32))
    {
        WARN("Death Stranding set %lu Wwise aux sends on object %#I64x "
                "(result %#lx, call %lu, environment %u, material bus %#lx, "
                "speaker bus %#lx).\n",
                submitted_count, game_object, result, calls, environment_object,
                controller_bus, controller_bus ? controller_speaker_bus : 0);
        for (i = 0; submitted_values && i < submitted_count && i < 8; ++i)
        {
            ULONG volume_bits;

            memcpy(&volume_bits, &submitted_values[i].volume, sizeof(volume_bits));
            WARN("Death Stranding aux send %lu: listener %#I64x, bus %#lx, "
                    "volume bits %#lx.\n", i, submitted_values[i].listener,
                    submitted_values[i].bus, volume_bits);
        }
    }
    return result;
}

static ULONG WINAPI death_stranding_trace_set_output_bus_volume(ULONGLONG emitter,
        ULONGLONG listener, float volume)
{
    struct death_stranding_routing_trace_entry *entry;
    ULONG result, volume_bits, calls = 0;
    BOOL environment_object;

    result = death_stranding_set_output_bus_volume_original(emitter, listener, volume);
    entry = death_stranding_get_routing_entry(emitter);
    environment_object = death_stranding_is_environment_object(emitter);
    if (entry) calls = InterlockedIncrement(&entry->output_calls);
    if (calls == 1 || (environment_object && calls <= 32))
    {
        memcpy(&volume_bits, &volume, sizeof(volume_bits));
        WARN("Death Stranding set Wwise output-bus volume on object %#I64x to listener "
                "%#I64x, volume bits %#lx (result %#lx, call %lu, environment %u).\n",
                emitter, listener, volume_bits, result, calls, environment_object);
    }
    return result;
}

static ULONG WINAPI death_stranding_trace_set_output_volume(ULONGLONG output, float volume)
{
    ULONG result, volume_bits;
    unsigned int i;

    result = death_stranding_set_output_volume_original(output, volume);
    memcpy(&volume_bits, &volume, sizeof(volume_bits));
    for (i = 0; i < ARRAY_SIZE(death_stranding_output_trace_entries); ++i)
    {
        struct death_stranding_output_trace_entry *entry =
                &death_stranding_output_trace_entries[i];
        LONGLONG output_id = InterlockedCompareExchange64(&entry->output, output, 0);

        if (output_id && output_id != output) continue;
        if (InterlockedIncrement(&entry->calls) <= 16)
            WARN("Death Stranding set Wwise output %#I64x volume bits %#lx "
                    "(result %#lx).\n", output, volume_bits, result);
        break;
    }
    return result;
}

static ULONG WINAPI death_stranding_trace_add_output(const ULONG *settings,
        ULONGLONG *output, const ULONGLONG *listeners, ULONG count)
{
    ULONG values[4] = {0}, result, calls, i;
    ULONGLONG output_id = 0;

    if (settings) memcpy(values, settings, sizeof(values));
    result = death_stranding_add_output_original(settings, output, listeners, count);
    if (output) output_id = *output;
    calls = InterlockedIncrement(&death_stranding_add_output_calls);
    if (calls <= 32)
    {
        WARN("Death Stranding added Wwise output %#I64x with settings "
                "%#lx/%#lx/%#lx/%#lx and %lu listeners (result %#lx, call %lu).\n",
                output_id, values[0], values[1], values[2], values[3],
                count, result, calls);
        for (i = 0; listeners && i < count && i < 8; ++i)
            WARN("Death Stranding Wwise output listener %lu: %#I64x.\n", i, listeners[i]);
    }
    return result;
}

static ULONG WINAPI death_stranding_trace_remove_output(ULONGLONG output)
{
    ULONG result, calls;

    result = death_stranding_remove_output_original(output);
    calls = InterlockedIncrement(&death_stranding_remove_output_calls);
    if (calls <= 32)
        WARN("Death Stranding removed Wwise output %#I64x (result %#lx, call %lu).\n",
                output, result, calls);
    return result;
}

static ULONG WINAPI death_stranding_trace_set_bus_device(ULONG bus, ULONG device)
{
    ULONG result, calls;

    result = death_stranding_set_bus_device_original(bus, device);
    calls = InterlockedIncrement(&death_stranding_set_bus_device_calls);
    if (calls <= 64)
        WARN("Death Stranding routed Wwise bus %#lx to device shareset %#lx "
                "(result %#lx, call %lu).\n", bus, device, result, calls);
    return result;
}

static BYTE *death_stranding_find_code_pattern(BYTE *base, IMAGE_NT_HEADERS *nt,
        const BYTE *pattern, SIZE_T pattern_size, const char *description)
{
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    BYTE *match = NULL;
    unsigned int i, count = 0;

    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        BYTE *start;
        SIZE_T size, j;

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                section[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage)
            continue;
        start = base + section[i].VirtualAddress;
        size = min(section[i].Misc.VirtualSize,
                nt->OptionalHeader.SizeOfImage - section[i].VirtualAddress);
        if (size < pattern_size) continue;

        for (j = 0; j <= size - pattern_size; ++j)
        {
            if (memcmp(start + j, pattern, pattern_size)) continue;
            match = start + j;
            ++count;
        }
    }

    if (count != 1)
    {
        WARN("Found %u copies of Death Stranding %s signature.\n", count, description);
        return NULL;
    }
    return match;
}

static BYTE *death_stranding_find_registration_callback(BYTE *base, IMAGE_NT_HEADERS *nt,
        const char *registration_name, BYTE **callback_load_out)
{
    static const BYTE registration_tail[] =
    {
        0xc7, 0x44, 0x24, 0x28, 0x03, 0x00, 0x00, 0x00,
    };
    IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
    BYTE *name = NULL, *reference = NULL, *callback_load;
    unsigned int i, name_count = 0, reference_count = 0;
    SIZE_T registration_name_size = strlen(registration_name) + 1;
    LONG displacement;

    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        BYTE *start;
        SIZE_T size, j;

        if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                section[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage)
            continue;
        start = base + section[i].VirtualAddress;
        size = min(section[i].Misc.VirtualSize,
                nt->OptionalHeader.SizeOfImage - section[i].VirtualAddress);
        if (size < registration_name_size) continue;
        for (j = 0; j <= size - registration_name_size; ++j)
        {
            if (memcmp(start + j, registration_name, registration_name_size)) continue;
            name = start + j;
            ++name_count;
        }
    }

    if (name_count != 1)
    {
        WARN("Found %u copies of Death Stranding registration %s.\n",
                name_count, registration_name);
        return NULL;
    }

    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        BYTE *start;
        SIZE_T size, j;

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                section[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage)
            continue;
        start = base + section[i].VirtualAddress;
        size = min(section[i].Misc.VirtualSize,
                nt->OptionalHeader.SizeOfImage - section[i].VirtualAddress);
        if (size < 7) continue;
        for (j = 0; j <= size - 7; ++j)
        {
            BYTE *p = start + j;

            if (p[0] != 0x4c || p[1] != 0x8d || p[2] != 0x05) continue;
            memcpy(&displacement, p + 3, sizeof(displacement));
            if (p + 7 + displacement != name) continue;
            reference = p;
            ++reference_count;
        }
    }

    if (reference_count != 1 || reference < base + 15)
    {
        WARN("Found %u references to Death Stranding registration %s.\n",
                reference_count, registration_name);
        return NULL;
    }

    callback_load = reference - 15;
    if (callback_load[0] != 0x48 || callback_load[1] != 0x8d || callback_load[2] != 0x05 ||
            memcmp(callback_load + 7, registration_tail, sizeof(registration_tail)))
    {
        WARN("Death Stranding registration %s has an unknown layout.\n", registration_name);
        return NULL;
    }

    memcpy(&displacement, callback_load + 3, sizeof(displacement));
    if (callback_load_out) *callback_load_out = callback_load;
    return callback_load + 7 + displacement;
}

static BOOL death_stranding_redirect_registration_callback(BYTE *callback_load,
        const void *target)
{
    INT_PTR relative_offset = (INT_PTR)target - (INT_PTR)(callback_load + 7);
    LONG displacement = relative_offset;
    void *protect_base = callback_load + 3;
    SIZE_T protect_size = sizeof(displacement);
    ULONG old_protect;
    NTSTATUS status;

    if ((INT_PTR)displacement != relative_offset)
    {
        WARN("Death Stranding diagnostic callback is outside relative-call range.\n");
        return FALSE;
    }
    if ((status = NtProtectVirtualMemory(NtCurrentProcess(), &protect_base, &protect_size,
            PAGE_EXECUTE_READWRITE, &old_protect)))
    {
        WARN("Could not make Death Stranding registration %p writable, status %#lx.\n",
                callback_load, status);
        return FALSE;
    }

    memcpy(callback_load + 3, &displacement, sizeof(displacement));
    NtFlushInstructionCache(NtCurrentProcess(), callback_load, 7);

    protect_base = callback_load + 3;
    protect_size = sizeof(displacement);
    if ((status = NtProtectVirtualMemory(NtCurrentProcess(), &protect_base, &protect_size,
            old_protect, &old_protect)))
        WARN("Could not restore Death Stranding registration %p protection, status %#lx.\n",
                callback_load, status);
    return TRUE;
}

static BOOL death_stranding_install_absolute_jump(BYTE *address, const void *target)
{
    BYTE jump[] = {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    void *protect_base = address;
    SIZE_T protect_size = sizeof(jump);
    ULONG old_protect;
    NTSTATUS status;

    memcpy(jump + 2, &target, sizeof(target));
    if ((status = NtProtectVirtualMemory(NtCurrentProcess(), &protect_base, &protect_size,
            PAGE_EXECUTE_READWRITE, &old_protect)))
    {
        WARN("Could not make Death Stranding callback %p writable, status %#lx.\n",
                address, status);
        return FALSE;
    }

    memcpy(address, jump, sizeof(jump));
    NtFlushInstructionCache(NtCurrentProcess(), address, sizeof(jump));

    protect_base = address;
    protect_size = sizeof(jump);
    if ((status = NtProtectVirtualMemory(NtCurrentProcess(), &protect_base, &protect_size,
            old_protect, &old_protect)))
        WARN("Could not restore Death Stranding callback %p protection, status %#lx.\n",
                address, status);
    return TRUE;
}

static BOOL death_stranding_install_trampoline(BYTE *address, const void *target,
        SIZE_T copied_size, void **original)
{
    BYTE jump[] =
    {
        0xff, 0x25, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    BYTE *trampoline = NULL;
    const void *resume = address + copied_size;
    SIZE_T trampoline_size = copied_size + sizeof(jump);
    void *protect_base;
    SIZE_T protect_size;
    ULONG old_protect;
    NTSTATUS status;

    if (copied_size < sizeof(jump)) return FALSE;
    if ((status = NtAllocateVirtualMemory(NtCurrentProcess(), (void **)&trampoline, 0,
            &trampoline_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
    {
        WARN("Could not allocate Death Stranding diagnostic trampoline, status %#lx.\n", status);
        return FALSE;
    }

    memcpy(trampoline, address, copied_size);
    memcpy(jump + 6, &resume, sizeof(resume));
    memcpy(trampoline + copied_size, jump, sizeof(jump));
    NtFlushInstructionCache(NtCurrentProcess(), trampoline, copied_size + sizeof(jump));

    memcpy(jump + 6, &target, sizeof(target));
    protect_base = address;
    protect_size = copied_size;
    if ((status = NtProtectVirtualMemory(NtCurrentProcess(), &protect_base, &protect_size,
            PAGE_EXECUTE_READWRITE, &old_protect)))
    {
        WARN("Could not make Death Stranding Wwise function %p writable, status %#lx.\n",
                address, status);
        return FALSE;
    }

    memcpy(address, jump, sizeof(jump));
    memset(address + sizeof(jump), 0x90, copied_size - sizeof(jump));
    NtFlushInstructionCache(NtCurrentProcess(), address, copied_size);

    protect_base = address;
    protect_size = copied_size;
    if ((status = NtProtectVirtualMemory(NtCurrentProcess(), &protect_base, &protect_size,
            old_protect, &old_protect)))
        WARN("Could not restore Death Stranding Wwise function protection, status %#lx.\n",
                status);

    *original = trampoline;
    return TRUE;
}

static void trace_death_stranding_environment_audio(void)
{
    static const BYTE set_global_rtpc_pattern[] =
    {
        0x48, 0x83, 0xec, 0x38, 0x45, 0x33, 0xc9, 0xc6,
        0x44, 0x24, 0x28, 0x00, 0xc7, 0x44, 0x24, 0x20,
        0x04, 0x00, 0x00, 0x00, 0x4d, 0x8d, 0x41, 0xff,
        0xe8,
    };
    static const BYTE set_rtpc_wrapper_pattern[] =
    {
        0x48, 0x83, 0xec, 0x48,
        0x0f, 0xb6, 0x44, 0x24, 0x78,
        0x88, 0x44, 0x24, 0x30,
        0x8b, 0x44, 0x24, 0x70,
        0x89, 0x44, 0x24, 0x28,
        0x44, 0x89, 0x4c, 0x24, 0x20,
        0x45, 0x33, 0xc9,
        0xe8,
    };
    static const BYTE set_rtpc_value_pattern[] =
    {
        0x48, 0x89, 0x5c, 0x24, 0x08,
        0x48, 0x89, 0x6c, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18,
    };
    static const BYTE simple_post_event_pattern[] =
    {
        0x48, 0x83, 0xec, 0x78, 0x33, 0xc0,
        0x48, 0x89, 0x44, 0x24, 0x68,
        0x89, 0x44, 0x24, 0x60,
        0x48, 0x89, 0x44, 0x24, 0x58,
        0x48, 0x89, 0x44, 0x24, 0x50,
        0x48, 0x89, 0x44, 0x24, 0x48,
        0xc7, 0x44, 0x24, 0x40, 0x04, 0x00, 0x00, 0x00,
        0x89, 0x44, 0x24, 0x38,
        0x89, 0x44, 0x24, 0x30,
        0x0f, 0xb6, 0x84, 0x24, 0xa8, 0x00, 0x00, 0x00,
        0x88, 0x44, 0x24, 0x28,
        0x0f, 0xb6, 0x84, 0x24, 0xa0, 0x00, 0x00, 0x00,
        0x88, 0x44, 0x24, 0x20,
        0xe8,
    };
    static const BYTE post_event_pattern[] =
    {
        0x40, 0x55, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41,
        0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x68,
    };
    static const BYTE set_aux_sends_pattern[] =
    {
        0x48, 0x89, 0x5c, 0x24, 0x10,
        0x48, 0x89, 0x6c, 0x24, 0x18,
        0x57, 0x48, 0x83, 0xec, 0x30, 0x45, 0x33, 0xd2,
    };
    static const BYTE set_output_bus_volume_pattern[] =
    {
        0x48, 0x89, 0x5c, 0x24, 0x08,
        0x57, 0x48, 0x83, 0xec, 0x30,
        0x0f, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8b, 0xda,
    };
    static const BYTE set_output_volume_pattern[] =
    {
        0x40, 0x53, 0x48, 0x83, 0xec, 0x30,
        0x0f, 0x29, 0x74, 0x24, 0x20,
        0x48, 0x8b, 0xd9, 0x0f, 0x28, 0xf1,
    };
    static const BYTE add_output_pattern[] =
    {
        0x48, 0x89, 0x5c, 0x24, 0x08,
        0x48, 0x89, 0x6c, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18,
        0x48, 0x89, 0x7c, 0x24, 0x20,
        0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xec, 0x20,
        0x8b, 0x31, 0x41, 0x8b, 0xf9, 0x4d, 0x8b, 0xf8,
        0x4c, 0x8b, 0xe2, 0x4c, 0x8b, 0xf1,
        0xbd, 0x07, 0x00, 0xae, 0x00, 0x85, 0xf6, 0x74, 0x6f,
    };
    enum { add_output_trampoline_size = 30 };
    static const BYTE remove_output_pattern[] =
    {
        0x40, 0x53,
        0x48, 0x83, 0xec, 0x20,
        0xba, 0x1d, 0x00, 0x00, 0x00,
        0x48, 0x8b, 0xd9,
    };
    static const BYTE set_bus_device_pattern[] =
    {
        0x48, 0x89, 0x5c, 0x24, 0x08,
        0x48, 0x89, 0x6c, 0x24, 0x10,
        0x48, 0x89, 0x74, 0x24, 0x18,
        0x57,
        0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00,
    };
    static const char environment_name[] = "Wwise::sGetFootSoundToControllerSpeaker";
    static const char set_global_rtpc_name[] = "WwiseGameObject::sSetGlobalRTPCExport";
    static const char set_state_name[] = "WwiseGameObject::sSetStateExport";
    HMODULE module = NtCurrentTeb()->Peb->ImageBaseAddress;
    BYTE *base = (BYTE *)module;
    BYTE *environment_callback, *set_global_rtpc_callback, *set_rtpc_wrapper_callback;
    BYTE *set_rtpc_value_callback, *simple_post_event_callback, *post_event_callback;
    BYTE *set_aux_sends_callback, *set_output_bus_volume_callback, *set_output_volume_callback;
    BYTE *add_output_callback, *remove_output_callback, *set_bus_device_callback;
    BYTE *set_state_callback, *set_state_load = NULL;
    IMAGE_NT_HEADERS *nt;
    LONG displacement;
    unsigned int i;

    if (!death_stranding_environment_effects_enabled() || !(nt = RtlImageNtHeader(module)) ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return;

    environment_callback = death_stranding_find_registration_callback(base, nt,
            environment_name, NULL);
    set_global_rtpc_callback = death_stranding_find_registration_callback(base, nt,
            set_global_rtpc_name, NULL);
    set_state_callback = death_stranding_find_registration_callback(base, nt,
            set_state_name, &set_state_load);
    simple_post_event_callback = death_stranding_find_code_pattern(base, nt,
            simple_post_event_pattern, sizeof(simple_post_event_pattern),
            "Wwise simple PostEvent callback");
    set_aux_sends_callback = death_stranding_find_code_pattern(base, nt,
            set_aux_sends_pattern, sizeof(set_aux_sends_pattern),
            "Wwise SetGameObjectAuxSendValues function");
    set_output_bus_volume_callback = death_stranding_find_code_pattern(base, nt,
            set_output_bus_volume_pattern, sizeof(set_output_bus_volume_pattern),
            "Wwise SetGameObjectOutputBusVolume function");
    set_output_volume_callback = death_stranding_find_code_pattern(base, nt,
            set_output_volume_pattern, sizeof(set_output_volume_pattern),
            "Wwise SetOutputVolume function");
    add_output_callback = death_stranding_find_code_pattern(base, nt,
            add_output_pattern, sizeof(add_output_pattern),
            "Wwise AddOutput function");
    remove_output_callback = death_stranding_find_code_pattern(base, nt,
            remove_output_pattern, sizeof(remove_output_pattern),
            "Wwise RemoveOutput function");
    set_bus_device_callback = death_stranding_find_code_pattern(base, nt,
            set_bus_device_pattern, sizeof(set_bus_device_pattern),
            "Wwise SetBusDevice function");
    if (!environment_callback || !set_global_rtpc_callback || !set_state_callback ||
            !set_state_load || !simple_post_event_callback || !set_aux_sends_callback ||
            !set_output_bus_volume_callback || !set_output_volume_callback ||
            !add_output_callback || !remove_output_callback || !set_bus_device_callback)
        return;

    if (environment_callback < base || environment_callback + 12 > base + nt->OptionalHeader.SizeOfImage ||
            environment_callback[0] != 0xb0 || environment_callback[1] != 0x01 ||
            environment_callback[2] != 0xc3)
    {
        WARN("Death Stranding environmental callback %p cannot be traced safely.\n",
                environment_callback);
        return;
    }
    for (i = 3; i < 12; ++i)
    {
        if (environment_callback[i] == 0xcc) continue;
        WARN("Death Stranding environmental callback %p has no trampoline space.\n",
                environment_callback);
        return;
    }

    if (set_global_rtpc_callback < base ||
            set_global_rtpc_callback + sizeof(set_global_rtpc_pattern) + sizeof(displacement) >
            base + nt->OptionalHeader.SizeOfImage ||
            memcmp(set_global_rtpc_callback, set_global_rtpc_pattern,
            sizeof(set_global_rtpc_pattern)))
    {
        WARN("Death Stranding Wwise global RTPC callback %p has an unknown layout.\n",
                set_global_rtpc_callback);
        return;
    }
    memcpy(&displacement, set_global_rtpc_callback + sizeof(set_global_rtpc_pattern),
            sizeof(displacement));
    set_rtpc_wrapper_callback = set_global_rtpc_callback + sizeof(set_global_rtpc_pattern) +
            sizeof(displacement) + displacement;
    if (set_rtpc_wrapper_callback < base ||
            set_rtpc_wrapper_callback + sizeof(set_rtpc_wrapper_pattern) + sizeof(displacement) >
            base + nt->OptionalHeader.SizeOfImage ||
            memcmp(set_rtpc_wrapper_callback, set_rtpc_wrapper_pattern,
            sizeof(set_rtpc_wrapper_pattern)))
    {
        WARN("Death Stranding Wwise RTPC wrapper %p has an unknown layout.\n",
                set_rtpc_wrapper_callback);
        return;
    }
    memcpy(&displacement, set_rtpc_wrapper_callback + sizeof(set_rtpc_wrapper_pattern),
            sizeof(displacement));
    set_rtpc_value_callback = set_rtpc_wrapper_callback + sizeof(set_rtpc_wrapper_pattern) +
            sizeof(displacement) + displacement;
    if (set_rtpc_value_callback < base ||
            set_rtpc_value_callback + sizeof(set_rtpc_value_pattern) >
            base + nt->OptionalHeader.SizeOfImage ||
            memcmp(set_rtpc_value_callback, set_rtpc_value_pattern,
            sizeof(set_rtpc_value_pattern)))
    {
        WARN("Death Stranding Wwise RTPC submission function %p has an unknown layout.\n",
                set_rtpc_value_callback);
        return;
    }
    if (set_state_callback < base || set_state_callback >= base + nt->OptionalHeader.SizeOfImage)
    {
        WARN("Death Stranding Wwise state callback %p is outside the executable.\n",
                set_state_callback);
        return;
    }
    if (simple_post_event_callback < base ||
            simple_post_event_callback + sizeof(simple_post_event_pattern) + sizeof(displacement) >
            base + nt->OptionalHeader.SizeOfImage ||
            memcmp(simple_post_event_callback, simple_post_event_pattern,
            sizeof(simple_post_event_pattern)))
    {
        WARN("Death Stranding Wwise simple PostEvent callback %p has an unknown layout.\n",
                simple_post_event_callback);
        return;
    }
    memcpy(&displacement, simple_post_event_callback + sizeof(simple_post_event_pattern),
            sizeof(displacement));
    post_event_callback = simple_post_event_callback + sizeof(simple_post_event_pattern) +
            sizeof(displacement) + displacement;
    if (post_event_callback < base ||
            post_event_callback + sizeof(post_event_pattern) >
            base + nt->OptionalHeader.SizeOfImage ||
            memcmp(post_event_callback, post_event_pattern, sizeof(post_event_pattern)))
    {
        WARN("Death Stranding Wwise PostEvent submission function %p has an unknown layout.\n",
                post_event_callback);
        return;
    }
    death_stranding_set_state_original = (death_stranding_set_state_t)set_state_callback;

    if (!death_stranding_install_absolute_jump(environment_callback,
            (const void *)death_stranding_environment_output_enabled) ||
            !death_stranding_install_trampoline(set_rtpc_value_callback,
            (const void *)death_stranding_trace_set_rtpc_value,
            sizeof(set_rtpc_value_pattern),
            (void **)&death_stranding_set_rtpc_value_original) ||
            !death_stranding_install_trampoline(post_event_callback,
            (const void *)death_stranding_trace_post_event,
            sizeof(post_event_pattern),
            (void **)&death_stranding_post_event_original) ||
            !death_stranding_install_trampoline(set_aux_sends_callback,
            (const void *)death_stranding_trace_set_aux_sends,
            sizeof(set_aux_sends_pattern),
            (void **)&death_stranding_set_aux_sends_original) ||
            !death_stranding_install_trampoline(set_output_bus_volume_callback,
            (const void *)death_stranding_trace_set_output_bus_volume,
            sizeof(set_output_bus_volume_pattern),
            (void **)&death_stranding_set_output_bus_volume_original) ||
            !death_stranding_install_trampoline(set_output_volume_callback,
            (const void *)death_stranding_trace_set_output_volume,
            sizeof(set_output_volume_pattern),
            (void **)&death_stranding_set_output_volume_original) ||
            !death_stranding_install_trampoline(add_output_callback,
            (const void *)death_stranding_trace_add_output,
            add_output_trampoline_size,
            (void **)&death_stranding_add_output_original) ||
            !death_stranding_install_trampoline(remove_output_callback,
            (const void *)death_stranding_trace_remove_output,
            sizeof(remove_output_pattern),
            (void **)&death_stranding_remove_output_original) ||
            !death_stranding_install_trampoline(set_bus_device_callback,
            (const void *)death_stranding_trace_set_bus_device,
            sizeof(set_bus_device_pattern),
            (void **)&death_stranding_set_bus_device_original) ||
            !death_stranding_redirect_registration_callback(set_state_load,
            (const void *)death_stranding_trace_set_state))
        return;

    TRACE("Tracing Death Stranding environmental callback %p, Wwise RTPC submission %p, "
            "Wwise state callback %p, Wwise PostEvent submission %p, aux sends %p, "
            "output-bus volume %p, output volume %p, output creation %p, output removal %p, "
            "and bus-device routing %p.\n",
            environment_callback, set_rtpc_value_callback, set_state_callback,
            post_event_callback, set_aux_sends_callback,
            set_output_bus_volume_callback, set_output_volume_callback,
            add_output_callback, remove_output_callback, set_bus_device_callback);
}

static void enable_death_stranding_environment_audio(void)
{
    static const char setting_name[] = "Wwise::sGetFootSoundToControllerSpeaker";
    static const BYTE registration_tail[] =
    {
        0xc7, 0x44, 0x24, 0x28, 0x03, 0x00, 0x00, 0x00,
    };
    static const BYTE true_stub_pattern[] =
    {
        0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
        0xb0, 0x01, 0xc3, 0xcc,
    };
    HMODULE module = NtCurrentTeb()->Peb->ImageBaseAddress;
    BYTE *base = (BYTE *)module;
    BYTE *name = NULL, *reference = NULL, *callback_load, *callback, *true_callback = NULL;
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *section;
    unsigned int i, name_count = 0, reference_count = 0, true_callback_count = 0;
    INT_PTR relative_offset;
    LONG displacement;
    void *protect_base;
    SIZE_T protect_size;
    ULONG old_protect;
    NTSTATUS status;

    if (!death_stranding_environment_effects_enabled() || !(nt = RtlImageNtHeader( module )) ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return;

    section = IMAGE_FIRST_SECTION( nt );
    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        BYTE *start;
        SIZE_T size, j;

        if (section[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage) continue;
        start = base + section[i].VirtualAddress;
        size = min( section[i].Misc.VirtualSize,
                nt->OptionalHeader.SizeOfImage - section[i].VirtualAddress );

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
        {
            if (size < sizeof(setting_name)) continue;
            for (j = 0; j <= size - sizeof(setting_name); ++j)
            {
                if (memcmp( start + j, setting_name, sizeof(setting_name) )) continue;
                name = start + j;
                ++name_count;
            }
            continue;
        }

        if (size >= sizeof(true_stub_pattern))
        {
            for (j = 0; j <= size - sizeof(true_stub_pattern); ++j)
            {
                if (memcmp( start + j, true_stub_pattern, sizeof(true_stub_pattern) )) continue;
                true_callback = start + j + 8;
                ++true_callback_count;
            }
        }
    }

    if (name_count != 1 || true_callback_count != 1)
    {
        WARN( "Death Stranding environmental-audio signatures are ambiguous "
                "(name %u, true callback %u).\n", name_count, true_callback_count );
        return;
    }

    for (i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        BYTE *start;
        SIZE_T size, j;

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
                section[i].VirtualAddress >= nt->OptionalHeader.SizeOfImage)
            continue;
        start = base + section[i].VirtualAddress;
        size = min( section[i].Misc.VirtualSize,
                nt->OptionalHeader.SizeOfImage - section[i].VirtualAddress );
        if (size < 7) continue;

        for (j = 0; j <= size - 7; ++j)
        {
            BYTE *p = start + j;

            if (p[0] != 0x4c || p[1] != 0x8d || p[2] != 0x05) continue;
            memcpy( &displacement, p + 3, sizeof(displacement) );
            if (p + 7 + displacement != name) continue;
            reference = p;
            ++reference_count;
        }
    }

    if (reference_count != 1 || reference < base + 15)
    {
        WARN( "Found %u Death Stranding environmental registration references.\n",
                reference_count );
        return;
    }

    callback_load = reference - 15;
    if (callback_load[0] != 0x48 || callback_load[1] != 0x8d || callback_load[2] != 0x05 ||
            memcmp( callback_load + 7, registration_tail, sizeof(registration_tail) ))
    {
        WARN( "Death Stranding environmental registration has an unknown layout.\n" );
        return;
    }

    memcpy( &displacement, callback_load + 3, sizeof(displacement) );
    callback = callback_load + 7 + displacement;
    if (callback < base || callback + 5 > base + nt->OptionalHeader.SizeOfImage ||
            memcmp( callback, "\x32\xc0\xc3\xcc\xcc", 5 ))
    {
        WARN( "Death Stranding environmental registration has an unknown callback %p.\n", callback );
        return;
    }

    relative_offset = true_callback - (callback_load + 7);
    displacement = relative_offset;
    if ((INT_PTR)displacement != relative_offset)
    {
        WARN( "Death Stranding environmental callback is outside relative-call range.\n" );
        return;
    }

    protect_base = callback_load + 3;
    protect_size = sizeof(displacement);
    if ((status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &protect_size,
            PAGE_EXECUTE_READWRITE, &old_protect )))
    {
        WARN( "Could not make Death Stranding environmental registration writable, status %#lx.\n",
                status );
        return;
    }

    memcpy( callback_load + 3, &displacement, sizeof(displacement) );
    NtFlushInstructionCache( NtCurrentProcess(), callback_load, 7 );

    protect_base = callback_load + 3;
    protect_size = sizeof(displacement);
    if ((status = NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &protect_size,
            old_protect, &old_protect )))
        WARN( "Could not restore Death Stranding environmental registration protection, status %#lx.\n",
                status );

    TRACE( "Redirected Death Stranding environmental registration %p from shared false callback "
            "%p to in-game true callback %p before game initialization.\n",
            callback_load, callback, true_callback );
}
#else
static void enable_death_stranding_environment_audio(void)
{
}

static void trace_death_stranding_environment_audio(void)
{
}
#endif

/***********************************************************************
 *           DllMain
 */
BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls( hinst );
        IsWow64Process( GetCurrentProcess(), &is_wow64 );
        init_global_data();
        init_locale( hinst );
        init_startup_info( NtCurrentTeb()->Peb->ProcessParameters );
        init_console();
        enable_death_stranding_environment_audio();
        trace_death_stranding_environment_audio();
    }
    return TRUE;
}


/***********************************************************************
 *           MulDiv   (kernelbase.@)
 */
INT WINAPI MulDiv( INT a, INT b, INT c )
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

/***********************************************************************
 *          AppPolicyGetMediaFoundationCodecLoading (KERNELBASE.@)
 */

LONG WINAPI AppPolicyGetMediaFoundationCodecLoading(HANDLE token, AppPolicyMediaFoundationCodecLoading *policy)
{
    FIXME("%p, %p\n", token, policy);

    if(policy)
        *policy = AppPolicyMediaFoundationCodecLoading_All;

    return ERROR_SUCCESS;
}

/***********************************************************************
 *          AppPolicyGetProcessTerminationMethod (KERNELBASE.@)
 */
LONG WINAPI AppPolicyGetProcessTerminationMethod(HANDLE token, AppPolicyProcessTerminationMethod *policy)
{
    FIXME("%p, %p\n", token, policy);

    if(policy)
        *policy = AppPolicyProcessTerminationMethod_ExitProcess;

    return ERROR_SUCCESS;
}

/***********************************************************************
 *          AppPolicyGetThreadInitializationType (KERNELBASE.@)
 */
LONG WINAPI AppPolicyGetThreadInitializationType(HANDLE token, AppPolicyThreadInitializationType *policy)
{
    FIXME("%p, %p\n", token, policy);

    if(policy)
        *policy = AppPolicyThreadInitializationType_None;

    return ERROR_SUCCESS;
}

/***********************************************************************
 *          AppPolicyGetShowDeveloperDiagnostic (KERNELBASE.@)
 */
LONG WINAPI AppPolicyGetShowDeveloperDiagnostic(HANDLE token, AppPolicyShowDeveloperDiagnostic *policy)
{
    FIXME("%p, %p\n", token, policy);

    if(policy)
        *policy = AppPolicyShowDeveloperDiagnostic_ShowUI;

    return ERROR_SUCCESS;
}

/***********************************************************************
 *          AppPolicyGetWindowingModel (KERNELBASE.@)
 */
LONG WINAPI AppPolicyGetWindowingModel(HANDLE token, AppPolicyWindowingModel *policy)
{
    static int once;

    if(!once++)
        FIXME("%p, %p\n", token, policy);

    if(policy)
        *policy = AppPolicyWindowingModel_ClassicDesktop;

    return ERROR_SUCCESS;
}

struct counterset_template
{
    PERF_COUNTERSET_INFO counterset;
    PERF_COUNTER_INFO counter[1];
};

struct counterset_instance
{
    struct list entry;
    struct counterset_template *template;
    PERF_COUNTERSET_INSTANCE instance;
};

struct perf_provider
{
    GUID guid;
    PERFLIBREQUEST callback;
    struct counterset_template **countersets;
    unsigned int counterset_count;

    struct list instance_list;
};

static struct perf_provider *perf_provider_from_handle(HANDLE prov)
{
    return (struct perf_provider *)prov;
}

/***********************************************************************
 *           PerfCreateInstance   (KERNELBASE.@)
 */
PERF_COUNTERSET_INSTANCE WINAPI *PerfCreateInstance( HANDLE handle, const GUID *guid,
                                                     const WCHAR *name, ULONG id )
{
    struct perf_provider *prov = perf_provider_from_handle( handle );
    struct counterset_template *template;
    struct counterset_instance *inst;
    unsigned int i;
    ULONG size;

    FIXME( "handle %p, guid %s, name %s, id %lu semi-stub.\n", handle, debugstr_guid(guid), debugstr_w(name), id );

    if (!prov || !guid || !name)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return NULL;
    }

    for (i = 0; i < prov->counterset_count; ++i)
        if (IsEqualGUID(guid, &prov->countersets[i]->counterset.CounterSetGuid)) break;

    if (i == prov->counterset_count)
    {
        SetLastError( ERROR_NOT_FOUND );
        return NULL;
    }

    template = prov->countersets[i];

    LIST_FOR_EACH_ENTRY(inst, &prov->instance_list, struct counterset_instance, entry)
    {
        if (inst->template == template && inst->instance.InstanceId == id)
        {
            SetLastError( ERROR_ALREADY_EXISTS );
            return NULL;
        }
    }

    size = (sizeof(PERF_COUNTERSET_INSTANCE) + template->counterset.NumCounters * sizeof(UINT64)
            + (lstrlenW( name ) + 1) * sizeof(WCHAR) + 7) & ~7;
    inst = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                      offsetof(struct counterset_instance, instance) + size );
    if (!inst)
    {
        SetLastError( ERROR_OUTOFMEMORY );
        return NULL;
    }

    inst->template = template;
    inst->instance.CounterSetGuid = *guid;
    inst->instance.dwSize = size;
    inst->instance.InstanceId = id;
    inst->instance.InstanceNameOffset = sizeof(PERF_COUNTERSET_INSTANCE)
                                        + template->counterset.NumCounters * sizeof(UINT64);
    inst->instance.InstanceNameSize = (lstrlenW( name ) + 1) * sizeof(WCHAR);
    memcpy( (BYTE *)&inst->instance + inst->instance.InstanceNameOffset, name, inst->instance.InstanceNameSize );
    list_add_tail( &prov->instance_list, &inst->entry );

    return &inst->instance;
}

/***********************************************************************
 *           PerfDeleteInstance   (KERNELBASE.@)
 */
ULONG WINAPI PerfDeleteInstance(HANDLE provider, PERF_COUNTERSET_INSTANCE *block)
{
    struct perf_provider *prov = perf_provider_from_handle( provider );
    struct counterset_instance *inst;

    TRACE( "provider %p, block %p.\n", provider, block );

    if (!prov || !block) return ERROR_INVALID_PARAMETER;

    inst = CONTAINING_RECORD(block, struct counterset_instance, instance);
    list_remove( &inst->entry );
    HeapFree( GetProcessHeap(), 0, inst );

    return ERROR_SUCCESS;
}

/***********************************************************************
 *           PerfSetCounterSetInfo   (KERNELBASE.@)
 */
ULONG WINAPI PerfSetCounterSetInfo( HANDLE handle, PERF_COUNTERSET_INFO *template, ULONG size )
{
    struct perf_provider *prov = perf_provider_from_handle( handle );
    struct counterset_template **new_array;
    struct counterset_template *new;
    unsigned int i;

    FIXME( "handle %p, template %p, size %lu semi-stub.\n", handle, template, size );

    if (!prov || !template) return ERROR_INVALID_PARAMETER;
    if (!template->NumCounters) return ERROR_INVALID_PARAMETER;
    if (size < sizeof(*template) || (size - (sizeof(*template))) / sizeof(PERF_COUNTER_INFO) < template->NumCounters)
        return ERROR_INVALID_PARAMETER;

    for (i = 0; i < prov->counterset_count; ++i)
    {
        if (IsEqualGUID( &template->CounterSetGuid, &prov->countersets[i]->counterset.CounterSetGuid ))
            return ERROR_ALREADY_EXISTS;
    }

    size = offsetof( struct counterset_template, counter[template->NumCounters] );
    if (!(new = HeapAlloc( GetProcessHeap(), 0, size ))) return ERROR_OUTOFMEMORY;

    if (prov->counterset_count)
        new_array = HeapReAlloc( GetProcessHeap(), 0, prov->countersets,
                                 sizeof(*prov->countersets) * (prov->counterset_count + 1) );
    else
        new_array = HeapAlloc( GetProcessHeap(), 0, sizeof(*prov->countersets) );

    if (!new_array)
    {
        HeapFree( GetProcessHeap(), 0, new );
        return ERROR_OUTOFMEMORY;
    }
    memcpy( new, template, size );
    for (i = 0; i < template->NumCounters; ++i)
        new->counter[i].Offset = i * sizeof(UINT64);
    new_array[prov->counterset_count++] = new;
    prov->countersets = new_array;

    return STATUS_SUCCESS;
}

static PERF_COUNTER_INFO* get_performance_counter_info(PERF_COUNTERSET_INSTANCE *instance, ULONG counter_id)
{
    unsigned int i;
    struct counterset_template *template;
    struct counterset_instance *inst;

    inst = CONTAINING_RECORD(instance, struct counterset_instance, instance);
    template = inst->template;

    for (i = 0; i < template->counterset.NumCounters; ++i)
        if (template->counter[i].CounterId == counter_id) return  &template->counter[i];

    return NULL;
}

/***********************************************************************
 *           PerfSetCounterRefValue   (KERNELBASE.@)
 */
ULONG WINAPI PerfSetCounterRefValue(HANDLE provider, PERF_COUNTERSET_INSTANCE *instance,
                                    ULONG counterid, void *address)
{
    struct perf_provider *prov = perf_provider_from_handle( provider );
    PERF_COUNTER_INFO* counter;

    FIXME( "provider %p, instance %p, counterid %lu, address %p semi-stub.\n",
           provider, instance, counterid, address );

    if (!prov || !instance || !address) return ERROR_INVALID_PARAMETER;

    counter = get_performance_counter_info(instance, counterid);

    if (counter == NULL) return ERROR_NOT_FOUND;
    if (!(counter->Attrib & PERF_ATTRIB_BY_REFERENCE)) return ERROR_INVALID_PARAMETER;

    *(void **)((BYTE *)(instance + 1) + counter->Offset) = address;

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           PerfSetULongCounterValue   (KERNELBASE.@)
 */
ULONG WINAPI PerfSetULongCounterValue(HANDLE provider, PERF_COUNTERSET_INSTANCE *instance,
                                      ULONG counterid, ULONG value)
{
    struct perf_provider *prov = perf_provider_from_handle( provider );
    PERF_COUNTER_INFO* counter;

    TRACE( "provider %p, instance %p, counterid %lu, address %lu semi-stub.\n",
           provider, instance, counterid, value );

    if (!prov || !instance) return ERROR_INVALID_PARAMETER;

    counter = get_performance_counter_info(instance, counterid);

    if (counter == NULL) return ERROR_NOT_FOUND;
    if (counter->Attrib & PERF_ATTRIB_BY_REFERENCE) return ERROR_INVALID_PARAMETER;
    if (counter->Type & PERF_SIZE_LARGE) return ERROR_INVALID_PARAMETER;

    *(ULONG*)((BYTE *)(instance + 1) + counter->Offset) = value;

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           PerfSetULongLongCounterValue   (KERNELBASE.@)
 */
ULONG WINAPI PerfSetULongLongCounterValue(HANDLE provider, PERF_COUNTERSET_INSTANCE *instance,
                                          ULONG counterid, ULONGLONG value)
{
    struct perf_provider *prov = perf_provider_from_handle( provider );
    PERF_COUNTER_INFO* counter;

    TRACE( "provider %p, instance %p, counterid %lu, address %I64u semi-stub.\n",
           provider, instance, counterid, value );

    if (!prov || !instance) return ERROR_INVALID_PARAMETER;

    counter = get_performance_counter_info(instance, counterid);

    if (counter == NULL) return ERROR_NOT_FOUND;
    if (counter->Attrib & PERF_ATTRIB_BY_REFERENCE) return ERROR_INVALID_PARAMETER;
    if (!(counter->Type & PERF_SIZE_LARGE)) return ERROR_INVALID_PARAMETER;

    *(ULONGLONG*)((BYTE *)(instance + 1) + counter->Offset) = value;

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           PerfStartProvider   (KERNELBASE.@)
 */
ULONG WINAPI PerfStartProvider( GUID *guid, PERFLIBREQUEST callback, HANDLE *provider )
{
    PERF_PROVIDER_CONTEXT ctx;

    FIXME( "guid %s, callback %p, provider %p semi-stub.\n", debugstr_guid(guid), callback, provider );

    memset( &ctx, 0, sizeof(ctx) );
    ctx.ContextSize = sizeof(ctx);
    ctx.ControlCallback = callback;

    return PerfStartProviderEx( guid, &ctx, provider );
}

/***********************************************************************
 *           PerfStartProviderEx   (KERNELBASE.@)
 */
ULONG WINAPI PerfStartProviderEx( GUID *guid, PERF_PROVIDER_CONTEXT *context, HANDLE *provider )
{
    struct perf_provider *prov;

    FIXME( "guid %s, context %p, provider %p semi-stub.\n", debugstr_guid(guid), context, provider );

    if (!guid || !context || !provider) return ERROR_INVALID_PARAMETER;
    if (context->ContextSize < sizeof(*context)) return ERROR_INVALID_PARAMETER;

    if (context->MemAllocRoutine || context->MemFreeRoutine)
        FIXME("Memory allocation routine is not supported.\n");

    if (!(prov = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*prov) ))) return ERROR_OUTOFMEMORY;
    list_init( &prov->instance_list );
    memcpy( &prov->guid, guid, sizeof(prov->guid) );
    prov->callback = context->ControlCallback;
    *provider = prov;

    return STATUS_SUCCESS;
}

/***********************************************************************
 *           PerfStopProvider   (KERNELBASE.@)
 */
ULONG WINAPI PerfStopProvider(HANDLE handle)
{
    struct perf_provider *prov = perf_provider_from_handle( handle );
    struct counterset_instance *inst, *next;
    unsigned int i;

    TRACE( "handle %p.\n", handle );

    if (!list_empty( &prov->instance_list ))
        WARN( "Stopping provider with active counter instances.\n" );

    LIST_FOR_EACH_ENTRY_SAFE(inst, next, &prov->instance_list, struct counterset_instance, entry)
    {
        list_remove( &inst->entry );
        HeapFree( GetProcessHeap(), 0, inst );
    }

    for (i = 0; i < prov->counterset_count; ++i)
        HeapFree( GetProcessHeap(), 0, prov->countersets[i] );
    HeapFree( GetProcessHeap(), 0, prov->countersets );
    HeapFree( GetProcessHeap(), 0, prov );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           QuirkIsEnabled   (KERNELBASE.@)
 */
BOOL WINAPI QuirkIsEnabled(void *arg)
{
    FIXME("(%p): stub\n", arg);
    return FALSE;
}

/***********************************************************************
 *          QuirkIsEnabled3 (KERNELBASE.@)
 */
BOOL WINAPI QuirkIsEnabled3(void *unk1, void *unk2)
{
    static int once;

    if (!once++)
        FIXME("(%p, %p) stub!\n", unk1, unk2);

    return FALSE;
}

HRESULT WINAPI QISearch(void *base, const QITAB *table, REFIID riid, void **obj)
{
    const QITAB *ptr;
    IUnknown *unk;

    TRACE("%p, %p, %s, %p\n", base, table, debugstr_guid(riid), obj);

    if (!obj)
        return E_POINTER;

    for (ptr = table; ptr->piid; ++ptr)
    {
        TRACE("trying (offset %ld) %s\n", ptr->dwOffset, debugstr_guid(ptr->piid));
        if (IsEqualIID(riid, ptr->piid))
        {
            unk = (IUnknown *)((BYTE *)base + ptr->dwOffset);
            TRACE("matched, returning (%p)\n", unk);
            *obj = unk;
            IUnknown_AddRef(unk);
            return S_OK;
        }
    }

    if (IsEqualIID(riid, &IID_IUnknown))
    {
        unk = (IUnknown *)((BYTE *)base + table->dwOffset);
        TRACE("returning first for IUnknown (%p)\n", unk);
        *obj = unk;
        IUnknown_AddRef(unk);
        return S_OK;
    }

    WARN("Not found %s.\n", debugstr_guid(riid));
    *obj = NULL;
    return E_NOINTERFACE;
}

HRESULT WINAPI GetAcceptLanguagesA(LPSTR langbuf, DWORD *buflen)
{
    DWORD buflenW, convlen;
    WCHAR *langbufW;
    HRESULT hr;

    TRACE("%p, %p, *%p: %ld\n", langbuf, buflen, buflen, buflen ? *buflen : -1);

    if (!langbuf || !buflen || !*buflen)
        return E_FAIL;

    buflenW = *buflen;
    langbufW = HeapAlloc(GetProcessHeap(), 0, sizeof(WCHAR) * buflenW);
    hr = GetAcceptLanguagesW(langbufW, &buflenW);

    if (hr == S_OK)
    {
        convlen = WideCharToMultiByte(CP_ACP, 0, langbufW, -1, langbuf, *buflen, NULL, NULL);
        convlen--;  /* do not count the terminating 0 */
    }
    else  /* copy partial string anyway */
    {
        convlen = WideCharToMultiByte(CP_ACP, 0, langbufW, *buflen, langbuf, *buflen, NULL, NULL);
        if (convlen < *buflen)
        {
            langbuf[convlen] = 0;
            convlen--;  /* do not count the terminating 0 */
        }
        else
        {
            convlen = *buflen;
        }
    }
    *buflen = buflenW ? convlen : 0;

    HeapFree(GetProcessHeap(), 0, langbufW);
    return hr;
}

static HRESULT lcid_to_rfc1766(LCID lcid, WCHAR *rfc1766, INT len)
{
    WCHAR buffer[6 /* MAX_RFC1766_NAME */];
    INT n = GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, buffer, ARRAY_SIZE(buffer));
    INT i;

    if (n)
    {
        i = PRIMARYLANGID(lcid);
        if ((((i == LANG_ENGLISH) || (i == LANG_CHINESE) || (i == LANG_ARABIC)) &&
            (SUBLANGID(lcid) == SUBLANG_DEFAULT)) ||
            (SUBLANGID(lcid) > SUBLANG_DEFAULT)) {

            buffer[n - 1] = '-';
            i = GetLocaleInfoW(lcid, LOCALE_SISO3166CTRYNAME, buffer + n, ARRAY_SIZE(buffer) - n);
            if (!i)
                buffer[n - 1] = '\0';
        }
        else
            i = 0;

        LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_LOWERCASE, buffer, n + i, rfc1766, len);
        return ((n + i) > len) ? E_INVALIDARG : S_OK;
    }
    return E_FAIL;
}

HRESULT WINAPI GetAcceptLanguagesW(WCHAR *langbuf, DWORD *buflen)
{
    DWORD mystrlen, mytype;
    WCHAR *mystr;
    LCID mylcid;
    HKEY mykey;
    LONG lres;
    DWORD len;

    TRACE("%p, %p, *%p: %ld\n", langbuf, buflen, buflen, buflen ? *buflen : -1);

    if (!langbuf || !buflen || !*buflen)
        return E_FAIL;

    mystrlen = (*buflen > 20) ? *buflen : 20 ;
    len = mystrlen * sizeof(WCHAR);
    mystr = HeapAlloc(GetProcessHeap(), 0, len);
    mystr[0] = 0;
    RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Internet Explorer\\International",
                  0, KEY_QUERY_VALUE, &mykey);
    lres = RegQueryValueExW(mykey, L"AcceptLanguage", 0, &mytype, (PBYTE)mystr, &len);
    RegCloseKey(mykey);
    len = lstrlenW(mystr);

    if (!lres && (*buflen > len))
    {
        lstrcpyW(langbuf, mystr);
        *buflen = len;
        HeapFree(GetProcessHeap(), 0, mystr);
        return S_OK;
    }

    /* Did not find a value in the registry or the user buffer is too small */
    mylcid = GetUserDefaultLCID();
    lcid_to_rfc1766(mylcid, mystr, mystrlen);
    len = lstrlenW(mystr);

    memcpy(langbuf, mystr, min(*buflen, len + 1)*sizeof(WCHAR));
    HeapFree(GetProcessHeap(), 0, mystr);

    if (*buflen > len)
    {
        *buflen = len;
        return S_OK;
    }

    *buflen = 0;
    return E_NOT_SUFFICIENT_BUFFER;
}

HRESULT WINAPI GetIntegratedDisplaySize( double *sz_inches )
{
    FIXME( "%p stub.\n", sz_inches );
    return E_NOTIMPL;
}
