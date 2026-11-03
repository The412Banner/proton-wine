/*
 * Copyright 2009 Maarten Lankhorst
 * Copyright 2011 Andrew Eikum for CodeWeavers
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

#include <stdarg.h>
#include <wchar.h>

#include "ntstatus.h"
#define COBJMACROS
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"

#include "ole2.h"
#include "olectl.h"
#include "rpcproxy.h"
#include "propsys.h"
#include "propkeydef.h"
#include "mmdeviceapi.h"
#include "mmsystem.h"
#include "dsound.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "devpkey.h"
#include "winreg.h"
#include "spatialaudioclient.h"
#include "mmddk.h"

#include "mmdevapi_private.h"
#include "wine/asm.h"
#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

DriverFuncs drvs;
static DriverFuncs midi_driver;

#define MIDI_CALL(code,args)  __wine_unix_call( midi_driver.module_unixlib, code, args )

const WCHAR drv_keyW[] = L"Software\\Wine\\Drivers";

typedef BOOL (WINAPI *death_stranding_output_selector_t)(DWORD output_id);
typedef void (WINAPI *death_stranding_output_switch_t)(void);

#define WM_DEATH_STRANDING_OUTPUT_CHANGED (WM_APP + 1)

static death_stranding_output_selector_t death_stranding_output_selector;
static death_stranding_output_switch_t death_stranding_output_switch;
static LONG death_stranding_default_output_id = -1;
static LONG death_stranding_follow_default_output;
static LONG death_stranding_output_message_pending;
static DWORD death_stranding_output_thread;
static HWND death_stranding_output_window;
static DWORD *death_stranding_requested_output;
static void **death_stranding_audio_system;
static BYTE *death_stranding_environment_name;
static void *death_stranding_environment_original_callback;
static LONG death_stranding_environment_callback_updated;
static LONG death_stranding_environment_scan_in_progress;

static BOOL force_death_stranding_environment_effects(void)
{
    WCHAR value[2];

    return GetEnvironmentVariableW(L"PROTON_DEATH_STRANDING_FORCE_ENVIRONMENT_EFFECTS",
            value, ARRAY_SIZE(value)) == 1 && value[0] == '1';
}

static BOOL WINAPI death_stranding_environment_output_enabled(void)
{
    static LONG logged;

    if (!InterlockedExchange(&logged, TRUE))
        TRACE("Death Stranding requested environmental controller-speaker audio.\n");
    return TRUE;
}

static BOOL death_stranding_readable_range(const void *address, SIZE_T size)
{
    MEMORY_BASIC_INFORMATION info;
    const BYTE *start = address;
    const BYTE *region;
    SIZE_T offset;

    if (!address || !size || !VirtualQuery(address, &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return FALSE;

    region = info.BaseAddress;
    if (start < region) return FALSE;
    offset = start - region;
    return offset <= info.RegionSize && size <= info.RegionSize - offset;
}

static BOOL death_stranding_writable_protection(DWORD protect)
{
    switch (protect & 0xff)
    {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return TRUE;
        default:
            return FALSE;
    }
}

static BYTE *death_stranding_find_environment_entry(void)
{
    static const SIZE_T active_callback_offsets[] = {0x28, 0x68};
    SYSTEM_INFO system_info;
    BYTE *address, *limit, *found = NULL;

    GetSystemInfo(&system_info);
    address = system_info.lpMinimumApplicationAddress;
    limit = system_info.lpMaximumApplicationAddress;

    while (address < limit)
    {
        MEMORY_BASIC_INFORMATION info;
        BYTE *start, *end, *next, *p;

        if (!VirtualQuery(address, &info, sizeof(info))) break;
        start = info.BaseAddress;
        if (info.RegionSize > (SIZE_T)(limit - start)) next = limit;
        else next = start + info.RegionSize;

        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                death_stranding_writable_protection(info.Protect) &&
                info.RegionSize >= 0xe8)
        {
            end = next;
            p = (BYTE *)(((UINT_PTR)start + sizeof(void *) - 1) & ~(sizeof(void *) - 1));
            __TRY
            {
                for (; p + sizeof(void *) <= end; p += sizeof(void *))
                {
                    void *value;
                    BYTE *entry;
                    unsigned int i;

                    memcpy(&value, p, sizeof(value));
                    if (value != death_stranding_environment_name ||
                            p < start + 0x18)
                        continue;

                    entry = p - 0x18;
                    if (!death_stranding_readable_range(entry, 0xe8)) continue;
                    for (i = 0; i < ARRAY_SIZE(active_callback_offsets); ++i)
                    {
                        memcpy(&value, entry + active_callback_offsets[i], sizeof(value));
                        if (value != death_stranding_environment_original_callback &&
                                value != (void *)death_stranding_environment_output_enabled)
                            break;
                    }
                    if (i == ARRAY_SIZE(active_callback_offsets))
                    {
                        found = entry;
                        break;
                    }
                }
            }
            __EXCEPT_PAGE_FAULT
            {
                found = NULL;
            }
            __ENDTRY

            if (found) return found;
        }

        if (next <= address) break;
        address = next;
    }
    return NULL;
}

static BOOL death_stranding_update_environment_callback(void)
{
    static const SIZE_T callback_offsets[] = {0x28, 0x68, 0xa8};
    BYTE *entry;
    unsigned int i;

    if (!force_death_stranding_environment_effects())
        return FALSE;
    if (InterlockedCompareExchange(&death_stranding_environment_callback_updated, 0, 0))
        return TRUE;
    if (!death_stranding_environment_name ||
            !death_stranding_environment_original_callback ||
            InterlockedExchange(&death_stranding_environment_scan_in_progress, TRUE))
        return FALSE;

    entry = death_stranding_find_environment_entry();
    if (entry)
    {
        for (i = 0; i < ARRAY_SIZE(callback_offsets); ++i)
            InterlockedCompareExchangePointer(
                    (void * volatile *)(entry + callback_offsets[i]),
                    (void *)death_stranding_environment_output_enabled,
                    death_stranding_environment_original_callback);
        InterlockedExchange(&death_stranding_environment_callback_updated, TRUE);
        TRACE("Updated Death Stranding's live environmental callback entry at %p.\n", entry);
    }
    InterlockedExchange(&death_stranding_environment_scan_in_progress, FALSE);
    return entry != NULL;
}

static void death_stranding_queue_controller_output_update(void)
{
    HWND window = InterlockedCompareExchangePointer((void **)&death_stranding_output_window, NULL, NULL);

    if (!window || !InterlockedCompareExchange(&death_stranding_follow_default_output, 0, 0) ||
            InterlockedExchange(&death_stranding_output_message_pending, TRUE))
        return;

    if (!PostMessageW(window, WM_DEATH_STRANDING_OUTPUT_CHANGED, 0, 0))
    {
        InterlockedExchange(&death_stranding_output_message_pending, FALSE);
        WARN("Could not queue Death Stranding controller output refresh, error %lu.\n", GetLastError());
    }
}

static LRESULT CALLBACK death_stranding_output_wndproc(HWND window, UINT msg, WPARAM wparam, LPARAM lparam)
{
    static BOOL refreshing;
    LONG hash;

    if (msg == WM_DEATH_STRANDING_OUTPUT_CHANGED)
    {
        InterlockedExchange(&death_stranding_output_message_pending, FALSE);
        if (refreshing || !InterlockedCompareExchange(&death_stranding_follow_default_output, 0, 0) ||
                !death_stranding_output_switch || !*death_stranding_audio_system)
            return 0;

        hash = InterlockedCompareExchange(&death_stranding_default_output_id, -1, -1);
        if (hash == -1 || *death_stranding_requested_output == (DWORD)hash) return 0;

        /* Re-enter Wwise only on the settings thread, never under MMDevAPI's device locks. */
        refreshing = TRUE;
        if (death_stranding_output_selector(hash))
        {
            death_stranding_output_switch();
            TRACE("Refreshed Death Stranding default controller output to %#lx on settings thread %04lx.\n",
                    (DWORD)hash, GetCurrentThreadId());
        }
        else
            WARN("Death Stranding rejected replacement controller output %#lx.\n", (DWORD)hash);
        refreshing = FALSE;
        if (hash != InterlockedCompareExchange(&death_stranding_default_output_id, -1, -1))
            death_stranding_queue_controller_output_update();
        return 0;
    }
    if (msg == WM_NCDESTROY)
    {
        InterlockedCompareExchangePointer((void **)&death_stranding_output_window, NULL, window);
        InterlockedExchange(&death_stranding_output_message_pending, FALSE);
        InterlockedExchange((LONG *)&death_stranding_output_thread, 0);
    }
    return DefWindowProcW(window, msg, wparam, lparam);
}

static void death_stranding_init_output_window(void)
{
    static const WCHAR class_name[] = L"WineDeathStrandingControllerOutput";
    DWORD thread = GetCurrentThreadId(), owner;
    WNDCLASSW class = {0};
    HMODULE module;
    HWND window;

    if (!death_stranding_output_switch) return;
    owner = InterlockedCompareExchange((LONG *)&death_stranding_output_thread, thread, 0);
    if (owner && owner != thread) return;
    if (InterlockedCompareExchangePointer((void **)&death_stranding_output_window, NULL, NULL)) return;

    /* The existing game hooks and this window must remain valid until process exit. */
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            (const WCHAR *)death_stranding_output_wndproc, &module)) return;
    class.lpfnWndProc = death_stranding_output_wndproc;
    class.hInstance = module;
    class.lpszClassName = class_name;
    if (!RegisterClassW(&class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        WARN("Could not register Death Stranding output window, error %lu.\n", GetLastError());
        return;
    }
    if (!(window = CreateWindowExW(0, class_name, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, module, NULL)))
    {
        WARN("Could not create Death Stranding output window, error %lu.\n", GetLastError());
        return;
    }
    InterlockedExchangePointer((void **)&death_stranding_output_window, window);
    TRACE("Created Death Stranding output notification window %p on settings thread %04lx.\n", window, thread);
}

BOOL WINAPI death_stranding_select_controller_output(DWORD output_id)
{
    DWORD requested_output = output_id;
    LONG hash;
    BOOL ret;

    InterlockedExchange(&death_stranding_follow_default_output, output_id == ~0u);
    if (output_id == ~0u)
    {
        hash = InterlockedCompareExchange(&death_stranding_default_output_id, -1, -1);
        if (hash != -1) output_id = hash;
    }
    ret = death_stranding_output_selector(output_id);
    TRACE("Death Stranding selected output %#lx for setting %#lx: %d.\n", output_id, requested_output, ret);
    return ret;
}

void * WINAPI death_stranding_prepare_controller_output(void *settings, BYTE setting)
{
    DWORD *selected_output = (DWORD *)((BYTE *)settings + 0xdc);
    BOOL ret;

    death_stranding_update_environment_callback();

    if (setting == 0x7b || setting == 0x7c || setting == 0xff)
    {
        death_stranding_init_output_window();
        ret = death_stranding_select_controller_output(*selected_output);
        death_stranding_queue_controller_output_update();

        TRACE("Death Stranding preselected setting output %#lx before applying setting %#x: %d.\n",
                *selected_output, setting, ret);
        if (!ret)
            WARN("Death Stranding rejected output %#lx before its audio settings transaction.\n",
                    *selected_output);
    }

    return *death_stranding_audio_system;
}

#if defined(__x86_64__) && !defined(__arm64ec__)
extern void death_stranding_prepare_controller_output_thunk(void);
__ASM_GLOBAL_FUNC( death_stranding_prepare_controller_output_thunk,
                   "subq $0xb8,%rsp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 0xb8\n\t")
                   "movq %rax,0x20(%rsp)\n\t"
                   "movq %rcx,0x28(%rsp)\n\t"
                   "movq %rdx,0x30(%rsp)\n\t"
                   "movq %r9,0x38(%rsp)\n\t"
                   "movq %r10,0x40(%rsp)\n\t"
                   "movq %r11,0x48(%rsp)\n\t"
                   "movdqu %xmm0,0x50(%rsp)\n\t"
                   "movdqu %xmm1,0x60(%rsp)\n\t"
                   "movdqu %xmm2,0x70(%rsp)\n\t"
                   "movdqu %xmm3,0x80(%rsp)\n\t"
                   "movdqu %xmm4,0x90(%rsp)\n\t"
                   "movdqu %xmm5,0xa0(%rsp)\n\t"
                   "call " __ASM_NAME("death_stranding_prepare_controller_output") "\n\t"
                   "movq %rax,%r8\n\t"
                   "movdqu 0x50(%rsp),%xmm0\n\t"
                   "movdqu 0x60(%rsp),%xmm1\n\t"
                   "movdqu 0x70(%rsp),%xmm2\n\t"
                   "movdqu 0x80(%rsp),%xmm3\n\t"
                   "movdqu 0x90(%rsp),%xmm4\n\t"
                   "movdqu 0xa0(%rsp),%xmm5\n\t"
                   "movq 0x20(%rsp),%rax\n\t"
                   "movq 0x28(%rsp),%rcx\n\t"
                   "movq 0x30(%rsp),%rdx\n\t"
                   "movq 0x38(%rsp),%r9\n\t"
                   "movq 0x40(%rsp),%r10\n\t"
                   "movq 0x48(%rsp),%r11\n\t"
                   "addq $0xb8,%rsp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset -0xb8\n\t")
                   "ret" )

extern void death_stranding_store_controller_output_thunk(void);
__ASM_GLOBAL_FUNC( death_stranding_store_controller_output_thunk,
                   "movl %edx,0xdc(%rbx)\n\t"
                   "pushfq\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 8\n\t")
                   "subq $0xc0,%rsp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset 0xc0\n\t")
                   "movq %rax,0x20(%rsp)\n\t"
                   "movq %rcx,0x28(%rsp)\n\t"
                   "movq %rdx,0x30(%rsp)\n\t"
                   "movq %r8,0x38(%rsp)\n\t"
                   "movq %r9,0x40(%rsp)\n\t"
                   "movq %r10,0x48(%rsp)\n\t"
                   "movq %r11,0x50(%rsp)\n\t"
                   "movdqu %xmm0,0x60(%rsp)\n\t"
                   "movdqu %xmm1,0x70(%rsp)\n\t"
                   "movdqu %xmm2,0x80(%rsp)\n\t"
                   "movdqu %xmm3,0x90(%rsp)\n\t"
                   "movdqu %xmm4,0xa0(%rsp)\n\t"
                   "movdqu %xmm5,0xb0(%rsp)\n\t"
                   "movl %edx,%ecx\n\t"
                   "call " __ASM_NAME("death_stranding_select_controller_output") "\n\t"
                   "movdqu 0x60(%rsp),%xmm0\n\t"
                   "movdqu 0x70(%rsp),%xmm1\n\t"
                   "movdqu 0x80(%rsp),%xmm2\n\t"
                   "movdqu 0x90(%rsp),%xmm3\n\t"
                   "movdqu 0xa0(%rsp),%xmm4\n\t"
                   "movdqu 0xb0(%rsp),%xmm5\n\t"
                   "movq 0x20(%rsp),%rax\n\t"
                   "movq 0x28(%rsp),%rcx\n\t"
                   "movq 0x30(%rsp),%rdx\n\t"
                   "movq 0x38(%rsp),%r8\n\t"
                   "movq 0x40(%rsp),%r9\n\t"
                   "movq 0x48(%rsp),%r10\n\t"
                   "movq 0x50(%rsp),%r11\n\t"
                   "addq $0xc0,%rsp\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset -0xc0\n\t")
                   "popfq\n\t"
                   __ASM_CFI(".cfi_adjust_cfa_offset -8\n\t")
                   "ret" )
#else
#define death_stranding_prepare_controller_output_thunk death_stranding_prepare_controller_output
#define death_stranding_store_controller_output_thunk death_stranding_select_controller_output
#endif

static const char *get_priority_string(int prio)
{
    switch(prio){
    case Priority_Unavailable:
        return "Unavailable";
    case Priority_Low:
        return "Low";
    case Priority_Neutral:
        return "Neutral";
    case Priority_Preferred:
        return "Preferred";
    }
    return "Invalid";
}

static BOOL load_driver(const WCHAR *name, DriverFuncs *driver)
{
    NTSTATUS status;
    WCHAR driver_module[264], path[MAX_PATH];
    struct test_connect_params params;

    lstrcpyW(driver_module, L"wine");
    lstrcatW(driver_module, name);
    lstrcatW(driver_module, L".drv");

    TRACE("Attempting to load %s\n", wine_dbgstr_w(driver_module));

    driver->module = LoadLibraryW(driver_module);
    if(!driver->module){
        TRACE("Unable to load %s: %lu\n", wine_dbgstr_w(driver_module),
                GetLastError());
        return FALSE;
    }

    if ((status = NtQueryVirtualMemory(GetCurrentProcess(), driver->module, MemoryWineLoadUnixLib,
        &driver->module_unixlib, sizeof(driver->module_unixlib), NULL))) {
        ERR("Unable to load UNIX functions: %lx\n", status);
        goto fail;
    }

    if ((status = __wine_unix_call(driver->module_unixlib, process_attach, NULL))) {
        ERR("Unable to initialize library: %lx\n", status);
        goto fail;
    }

    GetModuleFileNameW(NULL, path, ARRAY_SIZE(path));
    params.name     = wcsrchr(path, '\\');
    params.name     = params.name ? params.name + 1 : path;
    params.priority = Priority_Neutral;

    if ((status = __wine_unix_call(driver->module_unixlib, test_connect, &params))) {
        ERR("Unable to retrieve driver priority: %lx\n", status);
        goto fail;
    }

    driver->priority = params.priority;

    lstrcpyW(driver->module_name, driver_module);

    TRACE("Successfully loaded %s with priority %s\n",
            wine_dbgstr_w(driver_module), get_priority_string(driver->priority));

    return TRUE;
fail:
    FreeLibrary(driver->module);
    return FALSE;
}

static BOOL enable_death_stranding_controller_effects(void)
{
    static const char *const callback_names[] =
    {
        "Wwise::sGetFootSoundToControllerSpeaker",
        "Wwise::sGetControllerSpeakerEnable",
    };
    HMODULE module = GetModuleHandleW(NULL);
    BYTE *base = (BYTE *)module;
    BYTE *names[ARRAY_SIZE(callback_names)] = {0};
    BYTE *references[ARRAY_SIZE(callback_names)] = {0};
    BYTE *callback_loads[ARRAY_SIZE(callback_names)] = {0};
    BYTE *callbacks[ARRAY_SIZE(callback_names)] = {0};
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *section;
    static const BYTE settings_prefix[] =
    {
        0x48, 0x8b, 0xc4, 0x53, 0x57, 0x48, 0x83, 0xec, 0x78, 0x4c, 0x8b, 0x05,
    };
    static const BYTE settings_suffix[] =
    {
        0x0f, 0xb6, 0xfa, 0x48, 0x8b, 0xd9, 0x4d, 0x85, 0xc0,
    };
    static const BYTE output_switch_old[] =
    {
        0x45, 0x33, 0xc9,                         /* xor r9d, r9d */
        0xc7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, /* mov [rsp+0x20], 0 */
    };
    static const BYTE output_switch_new[] =
    {
        0x45, 0x33, 0xc9,                         /* xor r9d, r9d */
        0x44, 0x8b, 0xc3,                         /* mov r8d, ebx */
        0x44, 0x89, 0x4c, 0x24, 0x20,             /* mov [rsp+0x20], r9d */
    };
    BYTE *output_selector = NULL, *selector_call = NULL, *settings_apply;
    BYTE *ui_settings_call = NULL, *output_stores[2], *output_switch, *requested_output;
    BOOL environment_registered_early = FALSE, registered_callback_updated = FALSE;
    DWORD old_protect;
    WCHAR value[2];
    INT32 jump_offset;
    INT_PTR relative_offset;
    unsigned int i, k;

    if (GetEnvironmentVariableW(L"PROTON_DEATH_STRANDING_CONTROLLER_EFFECTS", value, ARRAY_SIZE(value)) != 1
            || value[0] != '1')
        return FALSE;
    if (!(nt = RtlImageNtHeader(module))) return FALSE;

    section = IMAGE_FIRST_SECTION(nt);
    for (k = 0; k < ARRAY_SIZE(callback_names); ++k)
    {
        SIZE_T name_size = strlen(callback_names[k]) + 1;

        for (i = 0; i < nt->FileHeader.NumberOfSections && !names[k]; ++i)
        {
            BYTE *start = base + section[i].VirtualAddress;
            SIZE_T size = section[i].Misc.VirtualSize;
            SIZE_T j;

            if (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE || size < name_size) continue;
            for (j = 0; j <= size - name_size; ++j)
            {
                if (!memcmp(start + j, callback_names[k], name_size))
                {
                    names[k] = start + j;
                    break;
                }
            }
        }
        if (!names[k]) return FALSE;
    }

    for (k = 0; k < ARRAY_SIZE(callback_names); ++k)
    {
        for (i = 0; i < nt->FileHeader.NumberOfSections && !references[k]; ++i)
        {
            BYTE *start = base + section[i].VirtualAddress;
            SIZE_T size = section[i].Misc.VirtualSize;
            SIZE_T j;

            if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < 7) continue;
            for (j = 0; j <= size - 7; ++j)
            {
                BYTE *p = start + j;
                INT32 displacement;

                if (p[0] != 0x4c || p[1] != 0x8d || p[2] != 0x05) continue;
                memcpy(&displacement, p + 3, sizeof(displacement));
                if (p + 7 + displacement == names[k])
                {
                    references[k] = p;
                    break;
                }
            }
        }
        if (!references[k]) return FALSE;
    }

    for (k = 0; k < ARRAY_SIZE(callback_names); ++k)
    {
        BYTE *p;

        for (p = references[k]; p >= references[k] - 32; --p)
        {
            INT32 displacement;
            BYTE *target;

            if (p[0] != 0x48 || p[1] != 0x8d || p[2] != 0x05) continue;
            memcpy(&displacement, p + 3, sizeof(displacement));
            target = p + 7 + displacement;
            if (target < base || target + 11 > base + nt->OptionalHeader.SizeOfImage) continue;
            callback_loads[k] = p;
            callbacks[k] = target;
            break;
        }
        if (!callbacks[k]) return FALSE;
    }

    environment_registered_early =
            (callbacks[0][0] == 0xb0 && callbacks[0][1] == 0x01 &&
             callbacks[0][2] == 0xc3 && callbacks[0][3] == 0xcc && callbacks[0][4] == 0xcc) ||
            (callbacks[0][0] == 0x48 && callbacks[0][1] == 0xb8 &&
             callbacks[0][10] == 0xff && callbacks[0][11] == 0xe0);
    if (!environment_registered_early &&
            (callbacks[0][0] != 0x32 || callbacks[0][1] != 0xc0 || callbacks[0][2] != 0xc3 ||
             callbacks[0][3] != 0xcc || callbacks[0][4] != 0xcc))
        return FALSE;
    if (callbacks[1][0] != 0x83 || callbacks[1][1] != 0x3d || callbacks[1][6] != 0xff
            || callbacks[1][7] != 0x0f || callbacks[1][8] != 0x95 || callbacks[1][9] != 0xc0
            || callbacks[1][10] != 0xc3)
        return FALSE;

    for (i = 0; i < nt->FileHeader.NumberOfSections && !selector_call; ++i)
    {
        BYTE *start = base + section[i].VirtualAddress;
        SIZE_T size = section[i].Misc.VirtualSize;
        SIZE_T j;

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < 24) continue;
        for (j = 0; j <= size - 24; ++j)
        {
            BYTE *p = start + j;
            INT32 displacement;
            BYTE *target;

            if (p[0] != 0x8b || p[1] != 0x8b || p[2] != 0xdc ||
                    p[3] != 0x00 || p[4] != 0x00 || p[5] != 0x00 ||
                    p[6] != 0xc5 || p[7] != 0xf8 || p[8] != 0x77 || p[9] != 0xe8 ||
                    p[14] != 0xc5 || p[15] != 0xf8 || p[16] != 0x77 ||
                    p[17] != 0x48 || p[18] != 0x83 || p[19] != 0xc4 || p[20] != 0x78 ||
                    p[21] != 0x5f || p[22] != 0x5b || p[23] != 0xc3)
                continue;

            memcpy(&displacement, p + 10, sizeof(displacement));
            target = p + 14 + displacement;
            if (target < base || target + 10 > base + nt->OptionalHeader.SizeOfImage ||
                    target[0] != 0x48 || target[1] != 0x89 || target[2] != 0x5c ||
                    target[3] != 0x24 || target[4] != 0x10 || target[5] != 0x56 ||
                    target[6] != 0x48 || target[7] != 0x83 || target[8] != 0xec || target[9] != 0x40)
                continue;

            selector_call = p + 9;
            output_selector = target;
            break;
        }
    }
    if (!selector_call) return FALSE;

    if (output_selector + 0xdb31 > base + nt->OptionalHeader.SizeOfImage ||
            output_selector[0xaf] != 0x89 || output_selector[0xb0] != 0x1d)
        return FALSE;

    output_switch = output_selector + 0xda40;
    if (memcmp(output_switch, "\x48\x83\xec\x58\x48\x8b\x05", 7) ||
            output_switch[0x2c] != 0x44 || output_switch[0x2d] != 0x8b ||
            output_switch[0x2e] != 0x05 || output_switch[0x33] != 0x8b ||
            output_switch[0x34] != 0x1d ||
            memcmp(output_switch + 0xec, "\x48\x83\xc4\x58\xc3", 5) ||
            memcmp(output_switch + 0xa0, output_switch_old, sizeof(output_switch_old)))
        return FALSE;

    memcpy(&jump_offset, output_selector + 0xb1, sizeof(jump_offset));
    requested_output = output_selector + 0xb5 + jump_offset;
    memcpy(&jump_offset, output_switch + 0x35, sizeof(jump_offset));
    if (output_switch + 0x39 + jump_offset != requested_output || requested_output < base ||
            requested_output + sizeof(DWORD) > base + nt->OptionalHeader.SizeOfImage)
        return FALSE;

    if ((SIZE_T)(selector_call - base) < 0x579) return FALSE;
    settings_apply = selector_call - 0x579;
    if (settings_apply + 25 > base + nt->OptionalHeader.SizeOfImage ||
            memcmp(settings_apply, settings_prefix, ARRAY_SIZE(settings_prefix)) ||
            memcmp(settings_apply + 16, settings_suffix, ARRAY_SIZE(settings_suffix)))
        return FALSE;

    memcpy(&jump_offset, settings_apply + 12, sizeof(jump_offset));
    death_stranding_audio_system = (void **)(settings_apply + 16 + jump_offset);
    if ((BYTE *)death_stranding_audio_system < base ||
            (BYTE *)(death_stranding_audio_system + 1) > base + nt->OptionalHeader.SizeOfImage)
        return FALSE;

    for (i = 0; i < nt->FileHeader.NumberOfSections && !ui_settings_call; ++i)
    {
        BYTE *start = base + section[i].VirtualAddress;
        SIZE_T size = section[i].Misc.VirtualSize;
        SIZE_T j;

        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || size < 22) continue;
        for (j = 0; j <= size - 22; ++j)
        {
            BYTE *p = start + j;
            INT32 displacement;

            if (p[0] != 0x41 || p[1] != 0x0f || p[2] != 0xb6 || p[3] != 0xd6 ||
                    p[4] != 0x48 || p[5] != 0x8b || p[6] != 0xcb || p[7] != 0xe8 ||
                    p[12] != 0x80 || p[13] != 0x7f || p[14] != 0x30 || p[15] != 0x06 ||
                    p[16] != 0x74 || p[18] != 0x41 || p[19] != 0x80 ||
                    p[20] != 0xfe || p[21] != 0x21)
                continue;

            memcpy(&displacement, p + 8, sizeof(displacement));
            if (p + 12 + displacement == settings_apply)
                ui_settings_call = p + 7;
        }
    }
    if (!ui_settings_call || (SIZE_T)(ui_settings_call - base) < 0x6b) return FALSE;

    output_stores[0] = ui_settings_call - 0x6b;
    output_stores[1] = ui_settings_call - 0x5e;
    if (memcmp(output_stores[0] - 4, "\x8b\x54\xc8\x08\x89\x93\xdc\x00\x00\x00\xeb\x32", 12) ||
            memcmp(output_stores[1] - 5, "\xba\xff\xff\xff\xff\x89\x93\xdc\x00\x00\x00\xeb\x25", 13))
        return FALSE;

    death_stranding_output_selector = (death_stranding_output_selector_t)output_selector;
    relative_offset = (BYTE *)death_stranding_prepare_controller_output_thunk - (settings_apply + 14);
    jump_offset = (INT32)relative_offset;
    if ((INT_PTR)jump_offset != relative_offset) return FALSE;
    if (!VirtualProtect(settings_apply + 9, 7, PAGE_EXECUTE_READWRITE, &old_protect))
        return FALSE;
    settings_apply[9] = 0xe8;
    memcpy(settings_apply + 10, &jump_offset, sizeof(jump_offset));
    settings_apply[14] = settings_apply[15] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), settings_apply + 9, 7);
    VirtualProtect(settings_apply + 9, 7, old_protect, &old_protect);

    for (i = 0; i < ARRAY_SIZE(output_stores); ++i)
    {
        relative_offset = (BYTE *)death_stranding_store_controller_output_thunk - (output_stores[i] + 5);
        jump_offset = (INT32)relative_offset;
        if ((INT_PTR)jump_offset != relative_offset) return FALSE;
        if (!VirtualProtect(output_stores[i], 6, PAGE_EXECUTE_READWRITE, &old_protect)) return FALSE;
        output_stores[i][0] = 0xe8;
        memcpy(output_stores[i] + 1, &jump_offset, sizeof(jump_offset));
        output_stores[i][5] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), output_stores[i], 6);
        VirtualProtect(output_stores[i], 6, old_protect, &old_protect);
    }

    relative_offset = (BYTE *)death_stranding_select_controller_output - (selector_call + 5);
    jump_offset = (INT32)relative_offset;
    if ((INT_PTR)jump_offset != relative_offset) return FALSE;
    if (!VirtualProtect(selector_call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) return FALSE;
    selector_call[0] = 0xe8;
    memcpy(selector_call + 1, &jump_offset, sizeof(jump_offset));
    FlushInstructionCache(GetCurrentProcess(), selector_call, 5);
    VirtualProtect(selector_call, 5, old_protect, &old_protect);

    if (VirtualProtect(callbacks[1], 11, PAGE_EXECUTE_READWRITE, &old_protect))
    {
        callbacks[1][0] = 0xb0; /* mov al, 1 */
        callbacks[1][1] = 0x01;
        callbacks[1][2] = 0xc3; /* ret */
        memset(callbacks[1] + 3, 0x90, 8);
        FlushInstructionCache(GetCurrentProcess(), callbacks[1], 11);
        VirtualProtect(callbacks[1], 11, old_protect, &old_protect);
        TRACE("Enabled Death Stranding's native BB controller-speaker capability at %p.\n",
                callbacks[1]);
    }
    else
        WARN("Could not enable Death Stranding's native BB controller-speaker capability.\n");

    if (!VirtualProtect(output_switch + 0xa0, sizeof(output_switch_old),
            PAGE_EXECUTE_READWRITE, &old_protect))
        return FALSE;
    memcpy(output_switch + 0xa0, output_switch_new, sizeof(output_switch_new));
    FlushInstructionCache(GetCurrentProcess(), output_switch + 0xa0, sizeof(output_switch_new));
    VirtualProtect(output_switch + 0xa0, sizeof(output_switch_old), old_protect, &old_protect);
    death_stranding_requested_output = (DWORD *)requested_output;
    death_stranding_output_switch = (death_stranding_output_switch_t)output_switch;

    if (force_death_stranding_environment_effects() && !environment_registered_early)
    {
        relative_offset = (BYTE *)death_stranding_environment_output_enabled - (callback_loads[0] + 7);
        jump_offset = (INT32)relative_offset;
        if ((INT_PTR)jump_offset != relative_offset) return FALSE;
        if (!VirtualProtect(callback_loads[0] + 3, sizeof(jump_offset),
                PAGE_EXECUTE_READWRITE, &old_protect))
            return FALSE;
        memcpy(callback_loads[0] + 3, &jump_offset, sizeof(jump_offset));
        FlushInstructionCache(GetCurrentProcess(), callback_loads[0], 7);
        VirtualProtect(callback_loads[0] + 3, sizeof(jump_offset), old_protect, &old_protect);

        death_stranding_environment_name = names[0];
        death_stranding_environment_original_callback = callbacks[0];
        registered_callback_updated = death_stranding_update_environment_callback();
    }

    TRACE("Enabled Death Stranding controller output; environmental speaker override %d "
            "at registration %p using callback %p; early registration enabled %d, "
            "existing runtime entry updated %d, "
            "pre-applied output selection through %p and UI stores %p/%p, "
            "redirected its final selector call %p through %p, and corrected its Wwise "
            "output transition at %p.\n",
            force_death_stranding_environment_effects(), callback_loads[0], callbacks[0],
            environment_registered_early,
            registered_callback_updated,
            (void *)death_stranding_prepare_controller_output_thunk,
            output_stores[0], output_stores[1], selector_call,
            (void *)death_stranding_select_controller_output, output_switch + 0xa0);
    return TRUE;
}

void death_stranding_controller_endpoint_seen(const WCHAR *id)
{
    const WCHAR *p;
    DWORD hash = 0x811c9dc5;

    death_stranding_update_environment_callback();
    if (!death_stranding_output_selector || !id) return;

    for (p = id; *p; ++p)
    {
        WCHAR ch = *p;

        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch > 0x7f)
        {
            WARN("Cannot select non-ASCII Death Stranding controller endpoint %s.\n", wine_dbgstr_w(id));
            return;
        }
        hash *= 0x01000193;
        hash ^= ch;
    }

    if (hash == ~0u || (DWORD)InterlockedExchange(&death_stranding_default_output_id, hash) == hash)
        return;

    TRACE("Discovered Death Stranding default controller endpoint %s (Wwise device %#lx).\n",
            wine_dbgstr_w(id), hash);
    death_stranding_queue_controller_output_update();
}

BOOL death_stranding_controller_output_hook_active(void)
{
    return death_stranding_output_selector != NULL;
}

static BOOL WINAPI init_driver(INIT_ONCE *once, void *param, void **context)
{
    static WCHAR default_list[] = L"pulse,alsa,oss,coreaudio";
    DriverFuncs driver;
    HKEY key;
    WCHAR reg_list[256], *p, *next, *driver_list = default_list;

    if (GetEnvironmentVariableW(L"PROTON_DEATH_STRANDING_CONTROLLER_EFFECTS", reg_list, ARRAY_SIZE(reg_list))
            && !enable_death_stranding_controller_effects())
        WARN("Could not configure Death Stranding's native controller-audio output.\n");

    if(RegOpenKeyW(HKEY_CURRENT_USER, drv_keyW, &key) == ERROR_SUCCESS){
        DWORD size = sizeof(reg_list);

        if(RegQueryValueExW(key, L"Audio", 0, NULL, (BYTE*)reg_list, &size) == ERROR_SUCCESS){
            if(reg_list[0] == '\0'){
                TRACE("User explicitly chose no driver\n");
                RegCloseKey(key);
                return TRUE;
            }

            driver_list = reg_list;
        }

        RegCloseKey(key);
    }

    TRACE("Loading driver list %s\n", wine_dbgstr_w(driver_list));
    for(next = p = driver_list; next; p = next + 1){
        next = wcschr(p, ',');
        if(next)
            *next = '\0';

        driver.priority = Priority_Unavailable;
        if(load_driver(p, &driver)){
            if(driver.priority == Priority_Unavailable)
                FreeLibrary(driver.module);
            else if(!drvs.module || driver.priority > drvs.priority){
                TRACE("Selecting driver %s with priority %s\n",
                        wine_dbgstr_w(p), get_priority_string(driver.priority));
                if(drvs.module)
                    FreeLibrary(drvs.module);
                drvs = driver;
            }else
                FreeLibrary(driver.module);
        }else
            TRACE("Failed to load driver %s\n", wine_dbgstr_w(p));

        if(next)
            *next = ',';
    }

    if (drvs.module != 0)
    {
        WCHAR midi_drvname[64];

        midi_drvname[0] = 0;
        wine_unix_call( midi_get_driver, midi_drvname );
        if (midi_drvname[0])
        {
            if (load_driver( midi_drvname, &midi_driver ))
                TRACE( "loaded %s as MIDI driver\n", debugstr_w(midi_driver.module_name) );
            else
               TRACE( "failed to load MIDI driver %s\n", wine_dbgstr_w(midi_drvname) );
        }
        else midi_driver = drvs;

        load_devices_from_reg();
        load_driver_devices(eRender);
        load_driver_devices(eCapture);
    }

    if (drvs.module == 0)
        ERR("No driver from %s could be initialized. "
            "Maybe check dependencies with WINEDEBUG=warn+module.\n",
            wine_dbgstr_w(driver_list));

    return drvs.module != 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    TRACE("(0x%p, %ld, %p)\n", hinstDLL, fdwReason, lpvReserved);

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hinstDLL);
            break;
        case DLL_PROCESS_DETACH:
            if (drvs.module_unixlib)
            {
                wine_unix_call( process_detach, NULL );
                FreeLibrary( drvs.module );
                if (midi_driver.module != drvs.module)
                {
                    MIDI_CALL( process_detach, NULL );
                    FreeLibrary( midi_driver.module );
                }
            }
            main_loop_stop();
            stop_update_thread();

            if (!lpvReserved)
                MMDevEnum_Free();
            break;
    }

    return TRUE;
}

typedef HRESULT (*FnCreateInstance)(REFIID riid, LPVOID *ppobj);

typedef struct {
    IClassFactory IClassFactory_iface;
    REFCLSID rclsid;
    FnCreateInstance pfnCreateInstance;
} IClassFactoryImpl;

static inline IClassFactoryImpl *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, IClassFactoryImpl, IClassFactory_iface);
}

static HRESULT WINAPI
MMCF_QueryInterface(IClassFactory *iface, REFIID riid, void **ppobj)
{
    IClassFactoryImpl *This = impl_from_IClassFactory(iface);
    TRACE("(%p, %s, %p)\n", This, debugstr_guid(riid), ppobj);
    if (ppobj == NULL)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppobj = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }
    *ppobj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI MMCF_AddRef(LPCLASSFACTORY iface)
{
    return 2;
}

static ULONG WINAPI MMCF_Release(LPCLASSFACTORY iface)
{
    /* static class, won't be freed */
    return 1;
}

static HRESULT WINAPI MMCF_CreateInstance(
    LPCLASSFACTORY iface,
    LPUNKNOWN pOuter,
    REFIID riid,
    LPVOID *ppobj)
{
    IClassFactoryImpl *This = impl_from_IClassFactory(iface);
    TRACE("(%p, %p, %s, %p)\n", This, pOuter, debugstr_guid(riid), ppobj);

    if (pOuter)
        return CLASS_E_NOAGGREGATION;

    if (ppobj == NULL) {
        WARN("invalid parameter\n");
        return E_POINTER;
    }
    *ppobj = NULL;
    return This->pfnCreateInstance(riid, ppobj);
}

static HRESULT WINAPI MMCF_LockServer(LPCLASSFACTORY iface, BOOL dolock)
{
    IClassFactoryImpl *This = impl_from_IClassFactory(iface);
    FIXME("(%p, %d) stub!\n", This, dolock);
    return S_OK;
}

static const IClassFactoryVtbl MMCF_Vtbl = {
    MMCF_QueryInterface,
    MMCF_AddRef,
    MMCF_Release,
    MMCF_CreateInstance,
    MMCF_LockServer
};

static IClassFactoryImpl MMDEVAPI_CF[] = {
    { { &MMCF_Vtbl }, &CLSID_MMDeviceEnumerator, (FnCreateInstance)MMDevEnum_Create }
};

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static HANDLE notify_thread_handle;

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    unsigned int i = 0;
    TRACE("(%s, %s, %p)\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);

    InitOnceExecuteOnce(&init_once, init_driver, NULL, NULL);

    if (ppv == NULL) {
        WARN("invalid parameter\n");
        return E_INVALIDARG;
    }

    *ppv = NULL;

    if (!IsEqualIID(riid, &IID_IClassFactory) &&
        !IsEqualIID(riid, &IID_IUnknown)) {
        WARN("no interface for %s\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }

    for (i = 0; i < ARRAY_SIZE(MMDEVAPI_CF); ++i)
    {
        if (IsEqualGUID(rclsid, MMDEVAPI_CF[i].rclsid)) {
            IClassFactory_AddRef(&MMDEVAPI_CF[i].IClassFactory_iface);
            *ppv = &MMDEVAPI_CF[i];
            return S_OK;
        }
    }

    WARN("(%s, %s, %p): no class found.\n", debugstr_guid(rclsid),
         debugstr_guid(riid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

static void notify_client(struct notify_context *notify)
{
    TRACE( "dev %u msg %x param1 %Ix param2 %0Ix\n",
           notify->dev_id, notify->msg, notify->param_1, notify->param_2);

    DriverCallback( notify->callback, notify->flags, notify->device, notify->msg,
                    notify->instance, notify->param_1, notify->param_2 );
}

static DWORD WINAPI notify_thread( void *p )
{
    struct midi_notify_wait_params params;
    struct notify_context notify;
    BOOL quit;

    SetThreadDescription( GetCurrentThread(), L"mmdevapi_midi_notify" );
    params.notify = &notify;
    params.quit = &quit;

    while (1)
    {
        MIDI_CALL( midi_notify_wait, &params );
        if (quit) break;
        if (notify.send_notify) notify_client(&notify);
    }
    return 0;
}

LRESULT WINAPI DriverProc( DWORD_PTR id, HANDLE driver, UINT msg, LPARAM param1, LPARAM param2 )
{
    InitOnceExecuteOnce( &init_once, init_driver, NULL, NULL );
    if (!midi_driver.module_unixlib) return 0;

    switch(msg)
    {
    case DRV_LOAD:
    {
        struct midi_init_params params;
        UINT err = DRV_SUCCESS;

        params.err = &err;
        MIDI_CALL( midi_init, &params );
        if (err == DRV_SUCCESS) notify_thread_handle = CreateThread( NULL, 0, notify_thread, NULL, 0, NULL );
        return err;
    }
    case DRV_FREE:
        MIDI_CALL( midi_release, NULL );
        WaitForSingleObject( notify_thread_handle, INFINITE );
        CloseHandle( notify_thread_handle );
        notify_thread_handle = NULL;
        return 1;
    case DRV_OPEN:
    case DRV_CLOSE:
    case DRV_QUERYCONFIGURE:
    case DRV_CONFIGURE:
        return 1;
    }
    return DefDriverProc( id, driver, msg, param1, param2 );
}

DWORD WINAPI midMessage( UINT id, UINT msg, DWORD_PTR user, DWORD_PTR param1, DWORD_PTR param2 )
{
    struct midi_in_message_params params;
    struct notify_context notify;
    UINT err = 0;

    TRACE( "%04x %04x %08Ix %08Ix %08Ix\n", id, msg, user, param1, param2 );

    params.dev_id  = id;
    params.msg     = msg;
    params.user    = user;
    params.param_1 = param1;
    params.param_2 = param2;
    params.err     = &err;
    params.notify  = &notify;

    do
    {
        MIDI_CALL( midi_in_message, &params );
        if ((!err || err == ERROR_RETRY) && notify.send_notify) notify_client( &notify );
    } while (err == ERROR_RETRY);

    return err;
}

DWORD WINAPI modMessage( UINT id, UINT msg, DWORD_PTR user, DWORD_PTR param1, DWORD_PTR param2 )
{
    struct midi_out_message_params params;
    struct notify_context notify;
    UINT err = 0;

    TRACE( "%04x %04x %08Ix %08Ix %08Ix\n", id, msg, user, param1, param2 );

    params.dev_id  = id;
    params.msg     = msg;
    params.user    = user;
    params.param_1 = param1;
    params.param_2 = param2;
    params.err     = &err;
    params.notify  = &notify;

    MIDI_CALL( midi_out_message, &params );
    if (!err && notify.send_notify) notify_client( &notify );
    return err;
}

DWORD WINAPI auxMessage( UINT id, UINT msg, DWORD_PTR user, DWORD_PTR param1, DWORD_PTR param2 )
{
    struct aux_message_params params;
    UINT err = 0;

    TRACE( "%04x %04x %08Ix %08Ix %08Ix\n", id, msg, user, param1, param2 );

    params.dev_id  = id;
    params.msg     = msg;
    params.user    = user;
    params.param_1 = param1;
    params.param_2 = param2;
    params.err     = &err;
    wine_unix_call( aux_message, &params );
    return err;
}

struct activate_async_op {
    IActivateAudioInterfaceAsyncOperation IActivateAudioInterfaceAsyncOperation_iface;
    LONG ref;

    IActivateAudioInterfaceCompletionHandler *callback;
    HRESULT result_hr;
    IUnknown *result_iface;
};

static struct activate_async_op *impl_from_IActivateAudioInterfaceAsyncOperation(IActivateAudioInterfaceAsyncOperation *iface)
{
    return CONTAINING_RECORD(iface, struct activate_async_op, IActivateAudioInterfaceAsyncOperation_iface);
}

static HRESULT WINAPI activate_async_op_QueryInterface(IActivateAudioInterfaceAsyncOperation *iface,
        REFIID riid, void **ppv)
{
    struct activate_async_op *This = impl_from_IActivateAudioInterfaceAsyncOperation(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) ||
            IsEqualIID(riid, &IID_IActivateAudioInterfaceAsyncOperation)) {
        *ppv = &This->IActivateAudioInterfaceAsyncOperation_iface;
    } else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI activate_async_op_AddRef(IActivateAudioInterfaceAsyncOperation *iface)
{
    struct activate_async_op *This = impl_from_IActivateAudioInterfaceAsyncOperation(iface);
    LONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) refcount now %li\n", This, ref);
    return ref;
}

static ULONG WINAPI activate_async_op_Release(IActivateAudioInterfaceAsyncOperation *iface)
{
    struct activate_async_op *This = impl_from_IActivateAudioInterfaceAsyncOperation(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) refcount now %li\n", This, ref);
    if (!ref) {
        if(This->result_iface)
            IUnknown_Release(This->result_iface);
        IActivateAudioInterfaceCompletionHandler_Release(This->callback);
        free(This);
    }
    return ref;
}

static HRESULT WINAPI activate_async_op_GetActivateResult(IActivateAudioInterfaceAsyncOperation *iface,
        HRESULT *result_hr, IUnknown **result_iface)
{
    struct activate_async_op *This = impl_from_IActivateAudioInterfaceAsyncOperation(iface);

    TRACE("(%p)->(%p, %p)\n", This, result_hr, result_iface);

    *result_hr = This->result_hr;

    if(This->result_hr == S_OK){
        *result_iface = This->result_iface;
        IUnknown_AddRef(*result_iface);
    }

    return S_OK;
}

static IActivateAudioInterfaceAsyncOperationVtbl IActivateAudioInterfaceAsyncOperation_vtbl = {
    activate_async_op_QueryInterface,
    activate_async_op_AddRef,
    activate_async_op_Release,
    activate_async_op_GetActivateResult,
};

static DWORD WINAPI activate_async_threadproc(void *user)
{
    struct activate_async_op *op = user;

    SetThreadDescription(GetCurrentThread(), L"wine_mmdevapi_activate_async");

    IActivateAudioInterfaceCompletionHandler_ActivateCompleted(op->callback, &op->IActivateAudioInterfaceAsyncOperation_iface);

    IActivateAudioInterfaceAsyncOperation_Release(&op->IActivateAudioInterfaceAsyncOperation_iface);

    return 0;
}

#define MMDEV_ID_FLOW_IDX 5
/* strlen("{0.0.1.00000000}.{fd47d9cc-4218-4135-9ce2-0c195c87405b}") + 1 */
#define MMDEV_ID_LEN 56
/* ARRAY_SIZE(MMDEV_PATH_PREFIX) */
#define MMDEV_PREFIX_LEN 18
/* (MMDEV_PREFIX_LEN - 1) + (MMDEV_ID_LEN - 1) + 1 + (ARRAY_SIZE(DEVINTERFACE_AUDIO_RENDER_WSTR) - 1) + 1 */
#define MMDEV_PATH_LEN 112
static HRESULT get_mmdevice_by_activatepath(const WCHAR *path, IMMDevice **mmdev)
{
    IMMDeviceEnumerator *devenum;
    HRESULT hr;

    static const WCHAR DEVINTERFACE_AUDIO_RENDER_WSTR[] = L"{E6327CAD-DCEC-4949-AE8A-991E976A79D2}";
    static const WCHAR DEVINTERFACE_AUDIO_CAPTURE_WSTR[] = L"{2EEF81BE-33FA-4800-9670-1CD474972C3F}";
    static const WCHAR MMDEV_PATH_PREFIX[] = L"\\\\?\\SWD#MMDEVAPI#";

    hr = MMDevEnum_Create(&IID_IMMDeviceEnumerator, (void**)&devenum);
    if (FAILED(hr)) {
        WARN("Failed to create MMDeviceEnumerator: %08lx\n", hr);
        return hr;
    }

    if (!lstrcmpiW(path, DEVINTERFACE_AUDIO_RENDER_WSTR)) {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(devenum, eRender, eMultimedia, mmdev);
    } else if (!lstrcmpiW(path, DEVINTERFACE_AUDIO_CAPTURE_WSTR)) {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(devenum, eCapture, eMultimedia, mmdev);
    } else if (wcslen(path) == MMDEV_PATH_LEN - 1) {
        WCHAR path_prefix[MMDEV_PREFIX_LEN];
        memcpy(path_prefix, path, (MMDEV_PREFIX_LEN - 1) * sizeof(WCHAR));
        path_prefix[MMDEV_PREFIX_LEN - 1] = 0;

        if (
            !lstrcmpiW(path_prefix, MMDEV_PATH_PREFIX) &&
            path[(MMDEV_PREFIX_LEN - 1) + (MMDEV_ID_LEN - 1)] == L'#'
        ) {
            const WCHAR *path_suffix = path + (MMDEV_PREFIX_LEN - 1) + (MMDEV_ID_LEN - 1) + 1;
            WCHAR device_id[MMDEV_ID_LEN];
            lstrcpynW(device_id, path + (MMDEV_PREFIX_LEN - 1), MMDEV_ID_LEN);

            if (
                (device_id[MMDEV_ID_FLOW_IDX] == L'0' && !lstrcmpiW(path_suffix, DEVINTERFACE_AUDIO_RENDER_WSTR)) ||
                (device_id[MMDEV_ID_FLOW_IDX] == L'1' && !lstrcmpiW(path_suffix, DEVINTERFACE_AUDIO_CAPTURE_WSTR))
            )
                hr = IMMDeviceEnumerator_GetDevice(devenum, device_id, mmdev);
        }
    } else {
        FIXME("Unrecognized device id format: %s\n", debugstr_w(path));
        hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    if (FAILED(hr)) {
        WARN("Failed to get requested device (%s): %08lx\n", debugstr_w(path), hr);
        *mmdev = NULL;
        hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    IMMDeviceEnumerator_Release(devenum);

    return hr;
}

/***********************************************************************
 *		ActivateAudioInterfaceAsync (MMDEVAPI.17)
 */
HRESULT WINAPI ActivateAudioInterfaceAsync(const WCHAR *path, REFIID riid,
        PROPVARIANT *params, IActivateAudioInterfaceCompletionHandler *done_handler,
        IActivateAudioInterfaceAsyncOperation **op_out)
{
    struct activate_async_op *op;
    HANDLE ht;
    IMMDevice *mmdev;

    TRACE("(%s, %s, %p, %p, %p)\n", debugstr_w(path), debugstr_guid(riid),
            params, done_handler, op_out);

    op = malloc(sizeof(*op));
    if (!op)
        return E_OUTOFMEMORY;

    op->ref = 2; /* returned ref and threadproc ref */
    op->IActivateAudioInterfaceAsyncOperation_iface.lpVtbl = &IActivateAudioInterfaceAsyncOperation_vtbl;
    op->callback = done_handler;
    IActivateAudioInterfaceCompletionHandler_AddRef(done_handler);

    op->result_hr = get_mmdevice_by_activatepath(path, &mmdev);
    if (SUCCEEDED(op->result_hr)) {
        op->result_hr = IMMDevice_Activate(mmdev, riid, CLSCTX_INPROC_SERVER, params, (void**)&op->result_iface);
        IMMDevice_Release(mmdev);
    }else
        op->result_iface = NULL;

    ht = CreateThread(NULL, 0, &activate_async_threadproc, op, 0, NULL);
    CloseHandle(ht);

    *op_out = &op->IActivateAudioInterfaceAsyncOperation_iface;

    return S_OK;
}
