/*
 * Copyright 2009 Maarten Lankhorst
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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "initguid.h"
#include "ole2.h"
#include "mmdeviceapi.h"
#include "dshow.h"
#include "dsound.h"
#include "audioclient.h"
#include "endpointvolume.h"
#include "audiopolicy.h"
#include "spatialaudioclient.h"
#include "setupapi.h"
#include "wine/plugplay.h"

#include "mmdevapi_private.h"
#include "devpkey.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);

DEFINE_GUID(GUID_NULL,0,0,0,0,0,0,0,0,0,0,0);

#define WINE_REG_PROP_MAGIC 0xbeef
struct reg_prop_serialized {
    VARTYPE vt;
    WORD unk; /* Uninitialized memory on native, we store a magic value here. */
    ULONG elems;
    BYTE data[];
};

static HKEY key_render;
static HKEY key_capture;

typedef struct MMDevPropStoreImpl
{
    IPropertyStore IPropertyStore_iface;
    LONG ref;
    MMDevice *parent;
    DWORD access;
} MMDevPropStore;

typedef struct MMDevEnumImpl
{
    IMMDeviceEnumerator IMMDeviceEnumerator_iface;
    LONG ref;
} MMDevEnumImpl;

static MMDevice *MMDevice_def_rec, *MMDevice_def_play;
static const IMMDeviceEnumeratorVtbl MMDevEnumVtbl;
static const IMMDeviceCollectionVtbl MMDevColVtbl;
static const IMMDeviceVtbl MMDeviceVtbl;
static const IPropertyStoreVtbl MMDevPropVtbl;
static const IMMEndpointVtbl MMEndpointVtbl;

static MMDevEnumImpl enumerator;
static struct list device_list = LIST_INIT(device_list);
static CRITICAL_SECTION device_list_cs;
static CRITICAL_SECTION_DEBUG device_list_cs_debug =
{
    0, 0, &device_list_cs,
    { &device_list_cs_debug.ProcessLocksList, &device_list_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": device_list_cs") }
};
static CRITICAL_SECTION device_list_cs = { &device_list_cs_debug, -1, 0, 0, 0, 0 };

struct NotificationClientWrapper {
    IMMNotificationClient *client;
    struct list entry;
};

static struct list g_notif_clients = LIST_INIT(g_notif_clients);

/* forward declare */
static CRITICAL_SECTION g_notif_lock;
static HRESULT set_format(MMDevice *dev);

static const WCHAR devid_formatW[] = L"{0.0.%u.00000000}.{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}";

static BOOL is_dualsense_endpoint_name( const WCHAR *name )
{
    return name && (wcsstr( name, L"DualSense" ) || wcsstr( name, L"DualShock" ) ||
            wcsstr( name, L"Wireless Controller" ));
}

static BOOL is_dualsense_backend_name( const char *name )
{
    return name && (strstr( name, "DualSense" ) || strstr( name, "DualShock" ) ||
            strstr( name, "Wireless Controller" ) || strstr( name, "Wireless_Controller" ) ||
            strstr( name, "Sony_Interactive_Entertainment" ));
}

static BOOL use_death_stranding_controller_effects(void)
{
    const char *env = getenv("PROTON_DEATH_STRANDING_CONTROLLER_EFFECTS");

    return env && env[0] == '1' && !env[1];
}

BOOL sony_windows_audio_mode_selected(void);

static BOOL use_stable_dualsense_device_ids(void)
{
    const char *env = getenv("PROTON_DUALSENSE_SPLIT_AUDIO");

    return (env && env[0] == '1' && !env[1]) ||
            use_death_stranding_controller_effects() || sony_windows_audio_mode_selected();
}

static DWORD hash_dualsense_device_name(const char *name, size_t length,
        EDataFlow flow, DWORD seed)
{
    const unsigned char *p = (const unsigned char *)name;
    DWORD hash = seed;

    while (length--)
    {
        hash ^= *p++;
        hash *= 0x01000193;
    }
    hash ^= flow;
    hash *= 0x01000193;
    return hash;
}

enum dualsense_audio_profile
{
    DUALSENSE_AUDIO_PROFILE_UNKNOWN,
    DUALSENSE_AUDIO_PROFILE_DEFAULT,
    DUALSENSE_AUDIO_PROFILE_DIRECT,
};

enum dualsense_audio_endpoint_role
{
    DUALSENSE_AUDIO_ENDPOINT_RENDER,
    DUALSENSE_AUDIO_ENDPOINT_MONITOR,
    DUALSENSE_AUDIO_ENDPOINT_CAPTURE,
};

static enum dualsense_audio_profile get_dualsense_audio_profile(const char *name)
{
    if (strstr(name, ".Default__") || strstr(name, ".HiFi__"))
        return DUALSENSE_AUDIO_PROFILE_DEFAULT;
    if (strstr(name, ".Direct__"))
        return DUALSENSE_AUDIO_PROFILE_DIRECT;
    return DUALSENSE_AUDIO_PROFILE_UNKNOWN;
}

static enum dualsense_audio_endpoint_role get_dualsense_audio_endpoint_role(
        const char *name, EDataFlow flow)
{
    if (flow == eRender)
        return DUALSENSE_AUDIO_ENDPOINT_RENDER;
    if (strstr(name, ".monitor"))
        return DUALSENSE_AUDIO_ENDPOINT_MONITOR;
    return DUALSENSE_AUDIO_ENDPOINT_CAPTURE;
}

static size_t get_dualsense_physical_identity_length(const char *name)
{
    const char *default_profile = strstr(name, ".Default__");
    const char *hifi_profile = strstr(name, ".HiFi__");
    const char *direct_profile = strstr(name, ".Direct__");
    size_t length = strlen(name);

    if (!default_profile || (hifi_profile && hifi_profile < default_profile))
        default_profile = hifi_profile;
    if (default_profile && (!direct_profile || default_profile < direct_profile))
        return default_profile - name;
    if (direct_profile)
        return direct_profile - name;
    return length;
}

static size_t get_dualsense_device_identity_length(const char *name, EDataFlow flow)
{
    const char *hotplug_suffix;
    size_t length = strlen(name);

    /* A physical suffix deliberately distinguishes simultaneous controllers
     * which otherwise expose the same Pulse/UCM endpoint name.  Include it in
     * the stable identity instead of folding the endpoints back together at
     * the profile separator below. */
    if (strstr(name, "#wine-sony-physical-"))
        return length;

    if (flow == eRender && get_dualsense_audio_profile(name) != DUALSENSE_AUDIO_PROFILE_UNKNOWN)
        return get_dualsense_physical_identity_length(name);
    if ((hotplug_suffix = strstr(name, "#wine-sony-hotplug-")))
        return hotplug_suffix - name;

    return length;
}

static void get_stable_dualsense_device_guid(EDataFlow flow, const char *name, GUID *guid)
{
    size_t identity_length = get_dualsense_device_identity_length(name, flow);
    DWORD hashes[4];

    hashes[0] = hash_dualsense_device_name(name, identity_length, flow, 0x811c9dc5);
    hashes[1] = hash_dualsense_device_name(name, identity_length, flow, 0x9e3779b9);
    hashes[2] = hash_dualsense_device_name(name, identity_length, flow, 0x85ebca6b);
    hashes[3] = hash_dualsense_device_name(name, identity_length, flow, 0xc2b2ae35);

    guid->Data1 = hashes[0];
    guid->Data2 = hashes[1];
    guid->Data3 = (hashes[1] >> 16 & 0x0fff) | 0x5000;
    memcpy(guid->Data4, &hashes[2], sizeof(hashes[2]));
    memcpy(guid->Data4 + sizeof(hashes[2]), &hashes[3], sizeof(hashes[3]));
    guid->Data4[0] = (guid->Data4[0] & 0x3f) | 0x80;
}

static BOOL device_is_published(const GUID *devguid, EDataFlow flow)
{
    MMDevice *device;
    BOOL found = FALSE;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, MMDevice, entry)
    {
        if (device->flow == flow && IsEqualGUID(&device->devguid, devguid))
        {
            found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&device_list_cs);

    return found;
}

typedef struct MMDevColImpl
{
    IMMDeviceCollection IMMDeviceCollection_iface;
    LONG ref;
    IMMDevice **devices;
    UINT devices_count;
    EDataFlow flow;
    DWORD state;
    MMDevice *dualsense_mono;
} MMDevColImpl;

typedef struct IPropertyBagImpl {
    IPropertyBag IPropertyBag_iface;
    GUID devguid;
} IPropertyBagImpl;

static const IPropertyBagVtbl PB_Vtbl;

typedef struct IConnectorImpl {
    IConnector IConnector_iface;
    LONG ref;
} IConnectorImpl;

typedef struct IDeviceTopologyImpl {
    IDeviceTopology IDeviceTopology_iface;
    LONG ref;
} IDeviceTopologyImpl;

static HRESULT MMDevPropStore_Create(MMDevice *This, DWORD access, IPropertyStore **ppv);

static inline MMDevPropStore *impl_from_IPropertyStore(IPropertyStore *iface)
{
    return CONTAINING_RECORD(iface, MMDevPropStore, IPropertyStore_iface);
}

static inline MMDevEnumImpl *impl_from_IMMDeviceEnumerator(IMMDeviceEnumerator *iface)
{
    return CONTAINING_RECORD(iface, MMDevEnumImpl, IMMDeviceEnumerator_iface);
}

static inline MMDevColImpl *impl_from_IMMDeviceCollection(IMMDeviceCollection *iface)
{
    return CONTAINING_RECORD(iface, MMDevColImpl, IMMDeviceCollection_iface);
}

static inline IPropertyBagImpl *impl_from_IPropertyBag(IPropertyBag *iface)
{
    return CONTAINING_RECORD(iface, IPropertyBagImpl, IPropertyBag_iface);
}

static HRESULT DeviceTopology_Create(IMMDevice *device, IDeviceTopology **ppv);

static inline IConnectorImpl *impl_from_IConnector(IConnector *iface)
{
    return CONTAINING_RECORD(iface, IConnectorImpl, IConnector_iface);
}

static inline IDeviceTopologyImpl *impl_from_IDeviceTopology(IDeviceTopology *iface)
{
    return CONTAINING_RECORD(iface, IDeviceTopologyImpl, IDeviceTopology_iface);
}

static const WCHAR propkey_formatW[] = L"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x},%d";

struct device
{
    struct list entry;
    GUID        guid;
    EDataFlow   flow;
    char        name[];
};

static CRITICAL_SECTION devices_cache_cs;
static CRITICAL_SECTION_DEBUG devices_cache_cs_debug =
{
    0, 0, &devices_cache_cs,
    { &devices_cache_cs_debug.ProcessLocksList, &devices_cache_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": devices_cache_cs") }
};
static CRITICAL_SECTION devices_cache_cs = { &devices_cache_cs_debug, -1, 0, 0, 0, 0 };
static struct list devices_cache = LIST_INIT( devices_cache );

static void add_device_to_cache( const GUID *guid, const char *name, EDataFlow flow )
{
    struct device *dev;

    if (!(dev = malloc( offsetof( struct device, name[strlen(name) + 1] )))) return;
    dev->guid = *guid;
    dev->flow = flow;
    strcpy( dev->name, name );
    list_add_tail( &devices_cache, &dev->entry );
}

static struct device *find_device_in_cache( const GUID *guid )
{
    struct device *dev;

    LIST_FOR_EACH_ENTRY( dev, &devices_cache, struct device, entry )
        if (IsEqualGUID( guid, &dev->guid )) return dev;
    return NULL;
}

static struct device *find_device_name_in_cache( const char *name, EDataFlow flow )
{
    struct device *dev;

    LIST_FOR_EACH_ENTRY( dev, &devices_cache, struct device, entry )
        if (dev->flow == flow && !strcmp( dev->name, name )) return dev;
    return NULL;
}

static BOOL dualsense_device_names_share_identity(const char *left, const char *right,
        EDataFlow flow)
{
    size_t left_length = get_dualsense_device_identity_length(left, flow);
    size_t right_length = get_dualsense_device_identity_length(right, flow);

    return left_length == right_length && !memcmp(left, right, left_length);
}

static BOOL dualsense_device_names_share_physical_identity(const char *left, const char *right)
{
    size_t left_length = get_dualsense_physical_identity_length(left);
    size_t right_length = get_dualsense_physical_identity_length(right);

    return left_length == right_length && !memcmp(left, right, left_length);
}

static BOOL dualsense_device_names_replace_physical_profile(const char *left, const char *right)
{
    enum dualsense_audio_profile left_profile = get_dualsense_audio_profile(left);
    enum dualsense_audio_profile right_profile = get_dualsense_audio_profile(right);

    return left_profile != DUALSENSE_AUDIO_PROFILE_UNKNOWN &&
            right_profile != DUALSENSE_AUDIO_PROFILE_UNKNOWN &&
            left_profile != right_profile &&
            dualsense_device_names_share_physical_identity(left, right);
}

static BOOL dualsense_device_names_replace_profile(const char *left, const char *right,
        EDataFlow flow)
{
    return dualsense_device_names_replace_physical_profile(left, right) &&
            get_dualsense_audio_endpoint_role(left, flow) ==
            get_dualsense_audio_endpoint_role(right, flow);
}

static struct device *find_dualsense_identity_in_cache(const char *name, EDataFlow flow)
{
    struct device *dev;

    LIST_FOR_EACH_ENTRY(dev, &devices_cache, struct device, entry)
        if (dev->flow == flow && dualsense_device_names_share_identity(dev->name, name, flow))
            return dev;
    return NULL;
}

static void alias_dualsense_identity_in_cache(const char *name, EDataFlow flow, const GUID *guid)
{
    struct device *dev;

    LIST_FOR_EACH_ENTRY(dev, &devices_cache, struct device, entry)
        if (dev->flow == flow && dualsense_device_names_share_identity(dev->name, name, flow))
            dev->guid = *guid;
}

static void remove_device_from_cache( const GUID *guid )
{
    struct device *dev, *next;

    EnterCriticalSection( &devices_cache_cs );
    LIST_FOR_EACH_ENTRY_SAFE( dev, next, &devices_cache, struct device, entry )
    {
        if (!IsEqualGUID( guid, &dev->guid )) continue;
        list_remove( &dev->entry );
        free( dev );
    }
    LeaveCriticalSection( &devices_cache_cs );
}

BOOL get_device_name_from_guid( const GUID *guid, char **name, EDataFlow *flow )
{
    struct device *dev;
    WCHAR key_name[MAX_PATH];
    DWORD index = 0;
    HKEY key;

    EnterCriticalSection( &devices_cache_cs );
    if ((dev = find_device_in_cache( guid )))
    {
        *name = strdup(dev->name);
        *flow = dev->flow;

        LeaveCriticalSection( &devices_cache_cs );
        return TRUE;
    }

    swprintf( key_name, ARRAY_SIZE(key_name), L"Software\\Wine\\Drivers\\%s\\devices", drvs.module_name );
    if (RegOpenKeyExW( HKEY_CURRENT_USER, key_name, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &key )) {
        LeaveCriticalSection( &devices_cache_cs );
        return FALSE;
    }

    for (;;)
    {
        DWORD size, type;
        LSTATUS status;
        GUID reg_guid;
        HKEY dev_key;

        size = ARRAY_SIZE(key_name);
        if (RegEnumKeyExW( key, index++, key_name, &size, NULL, NULL, NULL, NULL )) break;
        if (RegOpenKeyExW( key, key_name, 0, KEY_READ | KEY_WOW64_64KEY, &dev_key )) continue;
        size = sizeof(reg_guid);
        status = RegQueryValueExW( dev_key, L"guid", 0, &type, (BYTE *)&reg_guid, &size );
        RegCloseKey(dev_key);
        if (status || type != REG_BINARY || size != sizeof(reg_guid)) continue;
        if (!IsEqualGUID( &reg_guid, guid )) continue;
        if (key_name[0] == '0') *flow = eRender;
        else if (key_name[0] == '1') *flow = eCapture;
        else continue;

        TRACE( "Found matching device key %s for %s\n", wine_dbgstr_w(key_name), debugstr_guid(guid) );
        size = WideCharToMultiByte( CP_UNIXCP, 0, key_name + 2, -1, NULL, 0, NULL, NULL );
        if (!(*name = malloc( size ))) {
            RegCloseKey( key );
            LeaveCriticalSection( &devices_cache_cs );
            return FALSE;
        }
        WideCharToMultiByte( CP_UNIXCP, 0, key_name + 2, -1, *name, size, NULL, NULL );
        if (is_dualsense_backend_name( *name ))
        {
            TRACE( "Dropping persisted Sony controller device key %s for %s\n",
                   debugstr_a(*name), debugstr_guid(guid) );
            RegDeleteTreeW( key, key_name );
            RegCloseKey( key );
            free( *name );
            *name = NULL;
            LeaveCriticalSection( &devices_cache_cs );
            return FALSE;
        }
        add_device_to_cache( guid, *name, *flow );

        RegCloseKey( key );
        LeaveCriticalSection( &devices_cache_cs );
        return TRUE;
    }
    RegCloseKey( key );
    WARN( "No matching device in registry for %s\n", debugstr_guid(guid) );

    LeaveCriticalSection( &devices_cache_cs );
    return FALSE;
}

static void get_device_guid( EDataFlow flow, const char *dev_name, BOOL create, GUID *guid )
{
    WCHAR name[512];
    DWORD type, size = sizeof(*guid);
    HKEY key;
    LSTATUS status;
    int len;

    if (is_dualsense_backend_name( dev_name ))
    {
        struct device *dev;

        EnterCriticalSection( &devices_cache_cs );
        if (strstr( dev_name, "#wine-sony-physical-" ))
        {
            if ((dev = find_device_name_in_cache( dev_name, flow )))
                *guid = dev->guid;
            else if (create)
            {
                get_stable_dualsense_device_guid( flow, dev_name, guid );
                add_device_to_cache( guid, dev_name, flow );
            }
            else
                memset( guid, 0, sizeof(*guid) );
        }
        else if (use_stable_dualsense_device_ids())
        {
            if ((dev = find_device_name_in_cache( dev_name, flow )))
            {
                *guid = dev->guid;
                alias_dualsense_identity_in_cache( dev_name, flow, guid );
            }
            else if ((dev = find_dualsense_identity_in_cache( dev_name, flow )))
            {
                *guid = dev->guid;
                if (create) add_device_to_cache( guid, dev_name, flow );
            }
            else
            {
                get_stable_dualsense_device_guid( flow, dev_name, guid );
                if (create) add_device_to_cache( guid, dev_name, flow );
            }
        }
        else if ((dev = find_device_name_in_cache( dev_name, flow )))
            *guid = dev->guid;
        else if (create)
        {
            CoCreateGuid( guid );
            add_device_to_cache( guid, dev_name, flow );
        }
        else
            memset( guid, 0, sizeof(*guid) );
        LeaveCriticalSection( &devices_cache_cs );
        return;
    }

    len = swprintf( name, ARRAY_SIZE(name), L"Software\\Wine\\Drivers\\%s\\devices\\%u,",
                    drvs.module_name, flow == eCapture );
    MultiByteToWideChar( CP_UNIXCP, 0, dev_name, -1, name + len, ARRAY_SIZE(name) - len );
    status = RegCreateKeyExW( HKEY_CURRENT_USER, name, 0, NULL, 0,
                              KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, NULL, &key, NULL);
    if (status)
    {
        ERR( "Failed to create key %s: %lu\n", debugstr_w(name), status );
        return;
    }
    status = RegQueryValueExW( key, L"guid", 0, &type, (BYTE *)guid, &size );
    if (status != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(*guid))
    {
        CoCreateGuid( guid );
        RegSetValueExW( key, L"guid", 0, REG_BINARY, (BYTE *)guid, sizeof(*guid) );
    }
    RegCloseKey( key );

    EnterCriticalSection( &devices_cache_cs );
    if (!find_device_in_cache( guid )) add_device_to_cache( guid, dev_name, flow );
    LeaveCriticalSection( &devices_cache_cs );
}

static HRESULT MMDevPropStore_OpenPropKey(const GUID *guid, DWORD flow, HKEY *propkey)
{
    WCHAR buffer[39];
    LONG ret;
    HKEY key;
    StringFromGUID2(guid, buffer, 39);
    if ((ret = RegOpenKeyExW(flow == eRender ? key_render : key_capture, buffer, 0, KEY_READ|KEY_WRITE|KEY_WOW64_64KEY, &key)) != ERROR_SUCCESS)
    {
        WARN("Opening key %s failed with %lu\n", debugstr_w(buffer), ret);
        return E_FAIL;
    }
    ret = RegOpenKeyExW(key, L"Properties", 0, KEY_READ|KEY_WRITE|KEY_WOW64_64KEY, propkey);
    RegCloseKey(key);
    if (ret != ERROR_SUCCESS)
    {
        WARN("Opening key Properties failed with %lu\n", ret);
        return E_FAIL;
    }
    return S_OK;
}

static BOOL is_vector_vt(VARTYPE vt)
{
    return !!(vt & VT_VECTOR);
}

static unsigned int get_vt_elem_size(VARTYPE vt)
{
    switch (vt & VT_TYPEMASK)
    {
        case VT_BOOL:
            return sizeof(VARIANT_BOOL);

        case VT_CLSID:
            return sizeof(GUID);

        default:
            return 0;
    }
}

static BOOL is_valid_serialized_reg_prop(BYTE *data, DWORD data_size)
{
    struct reg_prop_serialized *reg_prop;
    unsigned int elem_size;

    if (data_size <= sizeof(*reg_prop))
        return FALSE;

    reg_prop = (struct reg_prop_serialized *)data;
    if (reg_prop->unk != WINE_REG_PROP_MAGIC)
        return FALSE;

    if (((reg_prop->vt & VT_TYPEMASK) > VT_CLSID))
        return FALSE;

    if (!!(reg_prop->vt & ~VT_TYPEMASK) && !is_vector_vt(reg_prop->vt))
        return FALSE;

    if (!reg_prop->elems || ((reg_prop->elems > 1) && !is_vector_vt(reg_prop->vt)))
        return FALSE;

    elem_size = get_vt_elem_size(reg_prop->vt);
    if (elem_size && (((elem_size * reg_prop->elems) + sizeof(*reg_prop)) > data_size))
        return FALSE;

    return TRUE;
}

static void deserialize_reg_prop(BYTE *data, DWORD data_size, PROPVARIANT *pv)
{
    struct reg_prop_serialized *reg_prop = (struct reg_prop_serialized *)data;
    unsigned int elems_size;

    switch (reg_prop->vt)
    {
        case VT_BOOL:
            pv->vt = reg_prop->vt;
            pv->boolVal = ((VARIANT_BOOL *)reg_prop->data)[0];
            break;

        case VT_BOOL | VT_VECTOR:
            pv->vt = reg_prop->vt;
            pv->cabool.cElems = reg_prop->elems;
            elems_size = sizeof(*pv->cabool.pElems) * reg_prop->elems;
            pv->cabool.pElems = CoTaskMemAlloc(elems_size);
            memcpy(pv->cabool.pElems, reg_prop->data, elems_size);
            break;

        case VT_CLSID:
            pv->vt = reg_prop->vt;
            pv->puuid = CoTaskMemAlloc(sizeof(*pv->puuid));
            *pv->puuid = ((GUID *)reg_prop->data)[0];
            break;

        case VT_CLSID | VT_VECTOR:
            pv->vt = reg_prop->vt;
            pv->cauuid.cElems = reg_prop->elems;
            elems_size = sizeof(*pv->cauuid.pElems) * reg_prop->elems;
            pv->cauuid.pElems = CoTaskMemAlloc(elems_size);
            memcpy(pv->cauuid.pElems, reg_prop->data, elems_size);
            break;

        default:
            break;
    }
}

static HRESULT serialize_reg_prop(HKEY reg_key, const WCHAR *prop_id, PROPVARIANT *pv)
{
    const struct reg_prop_serialized reg_prop_init = { pv->vt, WINE_REG_PROP_MAGIC };
    struct reg_prop_serialized *reg_prop = NULL;
    unsigned int size = sizeof(reg_prop_init);
    unsigned int elems_size = 0, elem_count = 1;
    void *elems_val = NULL;
    LONG ret;

    switch (pv->vt)
    {
        case VT_BOOL:
            elems_size = get_vt_elem_size(pv->vt);
            elems_val = &pv->boolVal;
            break;

        case VT_BOOL | VT_VECTOR:
            elem_count = pv->cabool.cElems;
            elems_size = elem_count * get_vt_elem_size(pv->vt);
            elems_val = pv->cabool.pElems;
            break;

        case VT_CLSID:
            elems_size = get_vt_elem_size(pv->vt);
            elems_val = pv->puuid;
            break;

        case VT_CLSID | VT_VECTOR:
            elem_count = pv->cauuid.cElems;
            elems_size = elem_count * get_vt_elem_size(pv->vt);
            elems_val = pv->cauuid.pElems;
            break;

        default:
            assert(0);
            break;
    }

    size += elems_size;
    if (!(reg_prop = malloc(size)))
        return E_OUTOFMEMORY;

    *reg_prop = reg_prop_init;
    reg_prop->elems = elem_count;
    memcpy(reg_prop->data, elems_val, elems_size);
    ret = RegSetValueExW(reg_key, prop_id, 0, REG_BINARY, (BYTE *)reg_prop, size);
    free(reg_prop);
    return !ret ? S_OK : E_FAIL;
}

static HRESULT MMDevice_GetPropValue(const GUID *devguid, DWORD flow, REFPROPERTYKEY key, PROPVARIANT *pv)
{
    WCHAR buffer[80];
    const GUID *id = &key->fmtid;
    DWORD type, size;
    HRESULT hr = S_OK;
    HKEY regkey;
    LONG ret;

    hr = MMDevPropStore_OpenPropKey(devguid, flow, &regkey);
    if (FAILED(hr))
        return hr;
    wsprintfW( buffer, propkey_formatW, id->Data1, id->Data2, id->Data3,
               id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
               id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7], key->pid );
    ret = RegGetValueW(regkey, NULL, buffer, RRF_RT_ANY, &type, NULL, &size);
    if (ret != ERROR_SUCCESS)
    {
        WARN("Reading %s returned %ld\n", debugstr_w(buffer), ret);
        RegCloseKey(regkey);
        pv->vt = VT_EMPTY;
        return S_OK;
    }

    switch (type)
    {
        case REG_SZ:
        {
            pv->vt = VT_LPWSTR;
            pv->pwszVal = CoTaskMemAlloc(size);
            if (!pv->pwszVal)
                hr = E_OUTOFMEMORY;
            else
                RegGetValueW(regkey, NULL, buffer, RRF_RT_REG_SZ, NULL, (BYTE*)pv->pwszVal, &size);
            break;
        }
        case REG_DWORD:
        {
            pv->vt = VT_UI4;
            RegGetValueW(regkey, NULL, buffer, RRF_RT_REG_DWORD, NULL, (BYTE*)&pv->ulVal, &size);
            break;
        }
        case REG_BINARY:
        {
            BYTE *data = CoTaskMemAlloc(size);

            if (!data)
            {
                hr = E_OUTOFMEMORY;
                break;
            }

            RegGetValueW(regkey, NULL, buffer, RRF_RT_REG_BINARY, NULL, data, &size);
            if (is_valid_serialized_reg_prop(data, size))
            {
                TRACE("do_deserialize().\n");
                deserialize_reg_prop(data, size, pv);
                CoTaskMemFree(data);
            }
            else
            {
                pv->vt = VT_BLOB;
                pv->blob.cbSize = size;
                pv->blob.pBlobData = data;
            }
            break;
        }
        default:
            ERR("Unknown/unhandled type: %lu\n", type);
            PropVariantClear(pv);
            break;
    }
    RegCloseKey(regkey);
    return hr;
}

static HRESULT MMDevice_SetPropValue(const GUID *devguid, DWORD flow, REFPROPERTYKEY key, REFPROPVARIANT pv)
{
    WCHAR buffer[80];
    const GUID *id = &key->fmtid;
    HRESULT hr;
    HKEY regkey;
    LONG ret;

    hr = MMDevPropStore_OpenPropKey(devguid, flow, &regkey);
    if (FAILED(hr))
        return hr;
    wsprintfW( buffer, propkey_formatW, id->Data1, id->Data2, id->Data3,
               id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
               id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7], key->pid );
    switch (pv->vt)
    {
        case VT_UI4:
        {
            ret = RegSetValueExW(regkey, buffer, 0, REG_DWORD, (const BYTE*)&pv->ulVal, sizeof(DWORD));
            break;
        }

        case VT_BOOL:
        case VT_BOOL | VT_VECTOR:
        case VT_CLSID:
        case VT_CLSID | VT_VECTOR:
        {
            hr = serialize_reg_prop(regkey, buffer, (PROPVARIANT *)pv);
            ret = 0;
            break;
        }

        case VT_BLOB:
        {
            ret = RegSetValueExW(regkey, buffer, 0, REG_BINARY, pv->blob.pBlobData, pv->blob.cbSize);
            TRACE("Blob %p %lu\n", pv->blob.pBlobData, pv->blob.cbSize);

            break;
        }
        case VT_LPWSTR:
        {
            ret = RegSetValueExW(regkey, buffer, 0, REG_SZ, (const BYTE*)pv->pwszVal, sizeof(WCHAR)*(1+lstrlenW(pv->pwszVal)));
            break;
        }
        default:
            ret = 0;
            FIXME("Unhandled type %u\n", pv->vt);
            hr = E_INVALIDARG;
            break;
    }
    RegCloseKey(regkey);
    TRACE("Writing %s returned %lu\n", debugstr_w(buffer), ret);

    if (SUCCEEDED(hr) && device_is_published(devguid, flow))
    {
        struct NotificationClientWrapper *wrapper;
        WCHAR devid[56];
        const GUID *id = devguid;
        swprintf(devid, 56, devid_formatW,
            flow, id->Data1, id->Data2, id->Data3,
            id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
            id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);

        EnterCriticalSection(&g_notif_lock);
        LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients, struct NotificationClientWrapper, entry)
            IMMNotificationClient_OnPropertyValueChanged(wrapper->client, devid, *key);
        LeaveCriticalSection(&g_notif_lock);
    }
    return hr;
}

/* pv will be overwritten, make sure it doesn't have allocated memory before. */
/* On return it will be valid (or empty). */
static HRESULT set_get_driver_prop_value(GUID *id, const EDataFlow flow, const PROPERTYKEY *prop, PROPVARIANT *pv)
{
    struct get_prop_value_params params;
    char *dev_name;
    unsigned int size = 0;

    TRACE("%s, (%s,%lu)\n", wine_dbgstr_guid(id), wine_dbgstr_guid(&prop->fmtid), prop->pid);

    if (!get_device_name_from_guid(id, &dev_name, &params.flow))
        return E_FAIL;

    params.device      = dev_name;
    params.guid        = id;
    params.prop        = prop;
    params.value       = pv;
    params.buffer      = NULL;
    params.buffer_size = &size;

    while (1) {
        __wine_unix_call(drvs.module_unixlib, get_prop_value, &params);

        if (params.result != E_NOT_SUFFICIENT_BUFFER)
            break;

        CoTaskMemFree(params.buffer);
        params.buffer = CoTaskMemAlloc(*params.buffer_size);
        if (!params.buffer) {
            free(dev_name);
            return E_OUTOFMEMORY;
        }
    }

    if (FAILED(params.result)) {
        CoTaskMemFree(params.buffer);
        PropVariantInit(pv);
    } else {
        MMDevice_SetPropValue(id, flow, prop, pv);
    }

    free(dev_name);
    return params.result;
}

static HRESULT set_driver_prop_value(GUID *id, const EDataFlow flow, const PROPERTYKEY *prop)
{
    PROPVARIANT pv;
    HRESULT hr;

    PropVariantInit(&pv);
    hr = set_get_driver_prop_value(id, flow, prop, &pv);

    PropVariantClear(&pv);
    return hr;
}

struct product_name_overrides
{
    const WCHAR *id;
    const WCHAR *product;
};

static const struct product_name_overrides product_name_overrides[] =
{
    /* Sony controllers */
    { .id = L"VID_054C&PID_05C4", .product = L"DualShock 4 Wireless Controller" },
    { .id = L"VID_054C&PID_09CC", .product = L"DualShock 4 Wireless Controller" },
    { .id = L"VID_054C&PID_0CE6", .product = L"DualSense Wireless Controller" },
    { .id = L"VID_054C&PID_0DF2", .product = L"DualSense Edge Wireless Controller" },
};

static const WCHAR *find_product_name_override(const WCHAR *device_id)
{
    const WCHAR *match_id = wcschr( device_id, '\\' ) + 1;
    DWORD i;

    for (i = 0; i < ARRAY_SIZE(product_name_overrides); ++i)
        if (!wcsnicmp( product_name_overrides[i].id, match_id, 17 ))
            return product_name_overrides[i].product;

    return NULL;
}

/* len of SWD\MMDEVAPI\{0.0.x.00000000}.{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} + nul */
#define DEVICE_ID_LEN 69
#define FRIENDLY_NAME_MAX 200

/* Create fake entries for an AudioEndpoint driver even though we are not actually one. */
/* Fix DualSense Wireless Controller haptic detection for some. */
/* This is way less code than even a skeleton pnp driver, not to mention the difficulty of setting friendly name */
/* from within ntoskrnl. */
static BOOL is_dualsense_device_path(const WCHAR *device_path)
{
    return device_path && (wcsstr(device_path, L"VID_054C&PID_05C4") ||
            wcsstr(device_path, L"VID_054C&PID_09CC") ||
            wcsstr(device_path, L"VID_054C&PID_0CE6") ||
            wcsstr(device_path, L"VID_054C&PID_0DF2"));
}

static const WCHAR *sony_controller_speaker_name(const WCHAR *device_path)
{
    if (!device_path)
        return NULL;
    if (wcsstr(device_path, L"VID_054C&PID_05C4") ||
            wcsstr(device_path, L"VID_054C&PID_09CC"))
        return L"DualShock 4 Wireless Controller Speaker";
    if (wcsstr(device_path, L"VID_054C&PID_0CE6"))
        return L"DualSense wireless controller (PS5) Internal Mono Speaker";
    if (wcsstr(device_path, L"VID_054C&PID_0DF2"))
        return L"DualSense Edge wireless controller (PS5) Internal Mono Speaker";
    return NULL;
}

static BOOL is_sony_direct_render_name(const WCHAR *name)
{
    return is_dualsense_endpoint_name(name) &&
            wcsstr(name, L"Direct Wireless Controller");
}

static BOOL enable_mhwilds_usb_audio(void)
{
    const char *env = getenv("PROTON_ENABLE_MHWILDS_USB_AUDIO");

    return env && env[0] == '1' && !env[1];
}

#define SONY_WINDOWS_AUDIO_MODE_EVENT L"__wine_sony_windows_audio_mode"
#define SONY_AUDIO_CAPABILITY_PROBE_THRESHOLD 8

static LONG sony_windows_audio_mode;
static LONG sony_windows_controller_names;
static HANDLE sony_windows_audio_mode_event;

static BOOL sony_windows_audio_mode_event_enabled(void)
{
    HANDLE event;
    BOOL enabled;

    if (!(event = OpenEventW(SYNCHRONIZE, FALSE, SONY_WINDOWS_AUDIO_MODE_EVENT)))
        return FALSE;
    enabled = WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    CloseHandle(event);
    return enabled;
}

BOOL sony_windows_audio_mode_selected(void)
{
    return InterlockedCompareExchange(&sony_windows_audio_mode, 0, 0) ||
            sony_windows_audio_mode_event_enabled();
}

static BOOL keep_sony_audio_endpoint_visible(void)
{
    const char *env = getenv("PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE");

    return (env && env[0] == '1' && !env[1]) ||
            use_death_stranding_controller_effects() ||
            InterlockedCompareExchange(&sony_windows_audio_mode, 0, 0) ||
            sony_windows_audio_mode_event_enabled();
}

static BOOL use_windows_sony_controller_names(void)
{
    const char *env = getenv("PROTON_SONY_WINDOWS_DEVICE_NAMES");

    return (env && env[0] == '1' && !env[1]) ||
            use_death_stranding_controller_effects() ||
            InterlockedCompareExchange(&sony_windows_controller_names, 0, 0);
}

static BOOL is_dualsense_mono_endpoint_name(const WCHAR *name)
{
    if (!name)
        return FALSE;

    if (wcsstr(name, L"DualShock") && (wcsstr(name, L"Internal Mono Speaker") ||
            wcsstr(name, L"Analog Stereo")))
        return TRUE;

    return (wcsstr(name, L"DualSense") || wcsstr(name, L"Wireless Controller")) &&
            wcsstr(name, L"Internal Mono Speaker");
}

enum dualsense_mono_model
{
    DUALSENSE_MONO_MODEL_UNKNOWN,
    DUALSENSE_MONO_MODEL_STANDARD,
    DUALSENSE_MONO_MODEL_DUALSHOCK,
    DUALSENSE_MONO_MODEL_EDGE,
};

static enum dualsense_mono_model dualsense_mono_endpoint_model_name(const WCHAR *name)
{
    if (!is_dualsense_mono_endpoint_name(name))
        return DUALSENSE_MONO_MODEL_UNKNOWN;

    if (wcsstr(name, L"Edge"))
        return DUALSENSE_MONO_MODEL_EDGE;

    if (wcsstr(name, L"DualShock"))
        return DUALSENSE_MONO_MODEL_DUALSHOCK;

    return DUALSENSE_MONO_MODEL_STANDARD;
}

static BOOL is_dualsense_mono_endpoint(const MMDevice *device)
{
    return device->flow == eRender && is_dualsense_mono_endpoint_name(device->drv_id);
}

static enum dualsense_mono_model dualsense_mono_endpoint_model(const MMDevice *device)
{
    return device->flow == eRender ? dualsense_mono_endpoint_model_name(device->drv_id) :
            DUALSENSE_MONO_MODEL_UNKNOWN;
}

static BOOL should_hide_from_endpoint_collection(const MMDevice *device)
{
    return is_dualsense_mono_endpoint(device) &&
            (!(device->state & DEVICE_STATE_ACTIVE) || device->hide_from_collection);
}

static BOOL should_prioritize_dualsense_mono_endpoint(const MMDevice *device)
{
    return is_dualsense_mono_endpoint(device) && (device->state & DEVICE_STATE_ACTIVE);
}

static BOOL collection_device_visible(const MMDevice *device, EDataFlow flow, DWORD state)
{
    return (device->flow == flow || flow == eAll) && (device->state & state) &&
            !should_hide_from_endpoint_collection(device);
}

struct endpoint_notification
{
    EDataFlow flow;
    GUID guid;
    GUID container_id;
    DWORD state;
    enum dualsense_mono_model model;
};

static void notify_endpoint_added_state(EDataFlow flow, const GUID *id, DWORD state)
{
    struct NotificationClientWrapper *wrapper;
    WCHAR devid[56];

    swprintf(devid, 56, devid_formatW,
            flow, id->Data1, id->Data2, id->Data3,
            id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
            id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);

    EnterCriticalSection(&g_notif_lock);
    LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients, struct NotificationClientWrapper, entry)
    {
        IMMNotificationClient_OnDeviceAdded(wrapper->client, devid);
        IMMNotificationClient_OnDeviceStateChanged(wrapper->client, devid, state);
    }
    LeaveCriticalSection(&g_notif_lock);
}

static void notify_endpoint_removed(EDataFlow flow, const GUID *id)
{
    struct NotificationClientWrapper *wrapper;
    WCHAR devid[56];

    swprintf(devid, 56, devid_formatW,
            flow, id->Data1, id->Data2, id->Data3,
            id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
            id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);

    EnterCriticalSection(&g_notif_lock);
    LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients, struct NotificationClientWrapper, entry)
    {
        IMMNotificationClient_OnDeviceStateChanged(wrapper->client, devid, DEVICE_STATE_NOTPRESENT);
        IMMNotificationClient_OnDeviceRemoved(wrapper->client, devid);
    }
    LeaveCriticalSection(&g_notif_lock);
}

static void delete_device_state_key(MMDevice *device)
{
    HKEY root = device->flow == eRender ? key_render : key_capture;
    WCHAR guidstr[39];

    StringFromGUID2(&device->devguid, guidstr, ARRAY_SIZE(guidstr));
    RegDeleteTreeW(root, guidstr);
}

static BOOL add_endpoint_notification(struct endpoint_notification **notifications,
        unsigned int *count, unsigned int *capacity, const MMDevice *device)
{
    struct endpoint_notification *new_notifications;
    PROPVARIANT container_id;
    unsigned int new_capacity;

    if (*count == *capacity)
    {
        new_capacity = *capacity ? *capacity * 2 : 2;
        new_notifications = realloc(*notifications, new_capacity * sizeof(**notifications));
        if (!new_notifications)
        {
            WARN("Failed to allocate DualSense mono endpoint hotplug notifications.\n");
            return FALSE;
        }

        *notifications = new_notifications;
        *capacity = new_capacity;
    }

    (*notifications)[*count].flow = device->flow;
    (*notifications)[*count].guid = device->devguid;
    (*notifications)[*count].container_id = GUID_NULL;
    (*notifications)[*count].state = device->state;
    (*notifications)[*count].model = dualsense_mono_endpoint_model(device);

    PropVariantInit(&container_id);
    if (SUCCEEDED(MMDevice_GetPropValue(&device->devguid, device->flow,
            (const PROPERTYKEY *)&DEVPKEY_Device_ContainerId, &container_id)) &&
            container_id.vt == VT_CLSID)
        (*notifications)[*count].container_id = *container_id.puuid;
    PropVariantClear(&container_id);

    (*count)++;
    return TRUE;
}

static BOOL endpoint_notification_is_current_dualsense_mono(
        const struct endpoint_notification *notification, BOOL require_visible)
{
    MMDevice *device;
    BOOL found = FALSE;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, MMDevice, entry)
    {
        if (device->flow != notification->flow ||
                !IsEqualGUID(&device->devguid, &notification->guid) ||
                !is_dualsense_mono_endpoint(device) ||
                device->removed || !(device->state & DEVICE_STATE_ACTIVE) ||
                (require_visible && should_hide_from_endpoint_collection(device)))
            continue;

        found = TRUE;
        break;
    }
    LeaveCriticalSection(&device_list_cs);

    return found;
}

static BOOL endpoint_notification_exists(const struct endpoint_notification *notifications,
        unsigned int count, const MMDevice *device)
{
    unsigned int i;

    for (i = 0; i < count; ++i)
    {
        if (notifications[i].flow == device->flow &&
                IsEqualGUID(&notifications[i].guid, &device->devguid))
            return TRUE;
    }

    return FALSE;
}

static unsigned int collect_visible_dualsense_mono_endpoints(enum dualsense_mono_model model,
        struct endpoint_notification **notifications, unsigned int *count, unsigned int *capacity)
{
    unsigned int start = *count;
    MMDevice *device;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, MMDevice, entry)
    {
        if (!is_dualsense_mono_endpoint(device) || should_hide_from_endpoint_collection(device))
            continue;
        if (model != DUALSENSE_MONO_MODEL_UNKNOWN && dualsense_mono_endpoint_model(device) != model)
            continue;
        if (endpoint_notification_exists(*notifications, *count, device))
            continue;

        if (!add_endpoint_notification(notifications, count, capacity, device))
            break;
    }
    LeaveCriticalSection(&device_list_cs);

    return start;
}

static void notify_dualsense_mono_endpoints(const struct endpoint_notification *notifications,
        unsigned int start, unsigned int count, BOOL release_hid)
{
    unsigned int i;

    for (i = start; i < count; ++i)
    {
        if (release_hid && !endpoint_notification_is_current_dualsense_mono(&notifications[i], FALSE))
            continue;

        if (release_hid && !IsEqualGUID(&notifications[i].container_id, &GUID_NULL))
            __wine_sechost_signal_sony_audio_endpoint_ready(&notifications[i].container_id);
        if (release_hid && !endpoint_notification_is_current_dualsense_mono(&notifications[i], TRUE))
            continue;

        TRACE("Re-signaling DualSense mono endpoint add %s %s.\n",
                debugstr_guid(&notifications[i].guid),
                release_hid ? "after complete audio hotplug and HID arrival dispatch" :
                "before completing the audio hotplug batch");
        notify_endpoint_added_state(notifications[i].flow, &notifications[i].guid,
                notifications[i].state);
    }
}

static void remove_dualsense_media_devices(const GUID *remove_container_id)
{
    static const GUID MEDIA_ClassGUID = { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    HDEVINFO device_set;
    SP_DEVINFO_DATA device_info_data;
    BOOL mhwilds = enable_mhwilds_usb_audio();
    DWORD index = 0;

    /* MH Wilds needs these entries for every connected controller. Do not
     * sweep them during registration; remove only a known disconnected one. */
    if (mhwilds && (!remove_container_id || IsEqualGUID(remove_container_id, &GUID_NULL)))
        return;

    device_set = SetupDiGetClassDevsW(&MEDIA_ClassGUID, NULL, NULL, 0);
    if (device_set == INVALID_HANDLE_VALUE)
        return;

    for (;;)
    {
        WCHAR instance_id[256];

        memset(&device_info_data, 0, sizeof(device_info_data));
        device_info_data.cbSize = sizeof(device_info_data);

        if (!SetupDiEnumDeviceInfo(device_set, index, &device_info_data))
            break;

        if (SetupDiGetDeviceInstanceIdW(device_set, &device_info_data, instance_id, ARRAY_SIZE(instance_id), NULL) &&
                is_dualsense_device_path(instance_id))
        {
            if (mhwilds)
            {
                WCHAR container_id_string[39];
                GUID container_id;

                if (!SetupDiGetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_BASE_CONTAINERID,
                        NULL, (BYTE *)container_id_string, sizeof(container_id_string), NULL) ||
                        FAILED(CLSIDFromString(container_id_string, &container_id)) ||
                        !IsEqualGUID(remove_container_id, &container_id))
                {
                    index++;
                    continue;
                }
            }

            TRACE("Removing stale DualSense MEDIA device %s.\n", debugstr_w(instance_id));
            if (SetupDiRemoveDevice(device_set, &device_info_data))
                continue;
            WARN("Failed to remove stale DualSense MEDIA device %s: 0x%lu.\n", debugstr_w(instance_id), GetLastError());
        }

        index++;
    }

    SetupDiDestroyDeviceInfoList(device_set);
}

static BOOL is_dualsense_audioendpoint_name(const WCHAR *name)
{
    return is_dualsense_mono_endpoint_name(name) ||
            (name && (wcsstr(name, L"Wireless Controller Speaker") ||
            wcsstr(name, L"DualShock 4 Wireless Controller Speaker") ||
            wcsstr(name, L"DualSense Wireless Controller Speaker") ||
            wcsstr(name, L"DualSense Edge Wireless Controller Speaker")));
}

static BOOL is_persistent_sony_audioendpoint_name(const WCHAR *name)
{
    return is_dualsense_audioendpoint_name(name) ||
            (name && (wcsstr(name, L"DualSense") || wcsstr(name, L"DualShock")));
}

static BOOL is_dualsense_audioendpoint_container(const GUID *container_id)
{
    WORD vendor = LOWORD(container_id->Data1);
    WORD product = HIWORD(container_id->Data1);

    return vendor == 0x054c && (product == 0x05c4 || product == 0x09cc ||
            product == 0x0ce6 || product == 0x0df2);
}

static void remove_dualsense_audioendpoint_devices(void)
{
    static const GUID AudioEndpoint_ClassGUID = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    HDEVINFO device_set;
    SP_DEVINFO_DATA device_info_data;
    DWORD index = 0;

    device_set = SetupDiGetClassDevsW(&AudioEndpoint_ClassGUID, NULL, NULL, 0);
    if (device_set == INVALID_HANDLE_VALUE)
        return;

    for (;;)
    {
        WCHAR friendly_name[256];

        memset(&device_info_data, 0, sizeof(device_info_data));
        device_info_data.cbSize = sizeof(device_info_data);

        if (!SetupDiEnumDeviceInfo(device_set, index, &device_info_data))
            break;

        if (SetupDiGetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_FRIENDLYNAME, NULL,
                (BYTE *)friendly_name, sizeof(friendly_name), NULL) &&
                is_dualsense_audioendpoint_name(friendly_name))
        {
            TRACE("Removing stale DualSense AudioEndpoint device %s.\n", debugstr_w(friendly_name));
            if (SetupDiRemoveDevice(device_set, &device_info_data))
                continue;
            WARN("Failed to remove stale DualSense AudioEndpoint device %s: 0x%lu.\n",
                    debugstr_w(friendly_name), GetLastError());
        }

        index++;
    }

    SetupDiDestroyDeviceInfoList(device_set);
}

static BOOL active_sony_audioendpoint_container(const GUID *container_id)
{
    MMDevice *device;
    BOOL found = FALSE;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, MMDevice, entry)
    {
        PROPVARIANT value = {VT_EMPTY};

        if (!(device->state & DEVICE_STATE_ACTIVE) ||
                !is_dualsense_endpoint_name(device->drv_id))
            continue;

        if (SUCCEEDED(MMDevice_GetPropValue(&device->devguid, device->flow,
                (const PROPERTYKEY *)&DEVPKEY_Device_ContainerId, &value)) &&
                value.vt == VT_CLSID && IsEqualGUID(value.puuid, container_id))
            found = TRUE;
        PropVariantClear(&value);
        if (found)
            break;
    }
    LeaveCriticalSection(&device_list_cs);

    return found;
}

static void remove_persistent_sony_audioendpoint_devices(const GUID *keep_container_id,
        const GUID *remove_container_id)
{
    static const GUID AudioEndpoint_ClassGUID = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    HDEVINFO device_set;
    SP_DEVINFO_DATA device_info_data;
    DWORD index = 0;

    device_set = SetupDiGetClassDevsW(&AudioEndpoint_ClassGUID, NULL, NULL, 0);
    if (device_set == INVALID_HANDLE_VALUE)
        return;

    for (;;)
    {
        WCHAR container_id_string[39], friendly_name[256], instance_id[256];
        GUID container_id;
        BOOL keep = FALSE, remove = FALSE, sony = FALSE;

        memset(&device_info_data, 0, sizeof(device_info_data));
        device_info_data.cbSize = sizeof(device_info_data);

        if (!SetupDiEnumDeviceInfo(device_set, index, &device_info_data))
            break;

        friendly_name[0] = 0;
        instance_id[0] = 0;
        SetupDiGetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_FRIENDLYNAME, NULL,
                (BYTE *)friendly_name, sizeof(friendly_name), NULL);
        SetupDiGetDeviceInstanceIdW(device_set, &device_info_data, instance_id,
                ARRAY_SIZE(instance_id), NULL);

        if (SetupDiGetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_BASE_CONTAINERID,
                NULL, (BYTE *)container_id_string, sizeof(container_id_string), NULL) &&
                SUCCEEDED(CLSIDFromString(container_id_string, &container_id)) &&
                is_dualsense_audioendpoint_container(&container_id))
        {
            sony = TRUE;
            if (remove_container_id)
                remove = IsEqualGUID(remove_container_id, &container_id);
            else
                keep = (keep_container_id && IsEqualGUID(keep_container_id, &container_id)) ||
                        active_sony_audioendpoint_container(&container_id);
        }
        else if (is_persistent_sony_audioendpoint_name(friendly_name))
            sony = TRUE;

        if (sony && (remove_container_id ? remove : !keep))
        {
            TRACE("Removing stale Sony AudioEndpoint device %s (%s).\n",
                    debugstr_w(friendly_name), debugstr_w(instance_id));
            if (SetupDiRemoveDevice(device_set, &device_info_data))
                continue;
            WARN("Failed to remove stale Sony AudioEndpoint device %s (%s): 0x%lu.\n",
                    debugstr_w(friendly_name), debugstr_w(instance_id), GetLastError());
        }

        index++;
    }

    SetupDiDestroyDeviceInfoList(device_set);
}

static void MMDevice_Register(const WCHAR *instguid, const WCHAR *friendly_name, const WCHAR *device_iname, const WCHAR *device_path,
        const GUID *container_id, BOOL skip_audioendpoint, EDataFlow flow)
{
    static const WCHAR ControlClass[] = L"System\\CurrentControlSet\\Control\\Class";
    static const WCHAR AudioEndpoint_Class[] = L"AudioEndpoint";
    static const GUID AudioEndpoint_ClassGUID = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };

    static const WCHAR MEDIA_Class[] = L"MEDIA";
    static const GUID MEDIA_ClassGUID = { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

    HDEVINFO device_set;
    SP_DEVINFO_DATA device_info_data;
    SP_DEVICE_INTERFACE_DATA device_interface_data;

    // TODO move to wine.inf?
    {
        /* len of ControlClass + '\\' + guidstr */
        WCHAR buf[78];
        WCHAR guidstr[39];
        HKEY key;

        StringFromGUID2(&AudioEndpoint_ClassGUID, guidstr, 39);
        _wcslwr(guidstr);
        lstrcpyW(buf, ControlClass);
        lstrcatW(buf, L"\\");
        lstrcatW(buf, guidstr);
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, buf, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, NULL) == ERROR_SUCCESS)
        {
            /* setupapi uses this to set other things later. */
            RegSetValueExW(key, L"Class", 0, REG_SZ, (LPBYTE)AudioEndpoint_Class, ARRAY_SIZE(AudioEndpoint_Class) * sizeof(WCHAR));
            RegCloseKey(key);
        }

        StringFromGUID2(&MEDIA_ClassGUID, guidstr, 39);
        _wcslwr(guidstr);
        lstrcpyW(buf, ControlClass);
        lstrcatW(buf, L"\\");
        lstrcatW(buf, guidstr);
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, buf, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, NULL) == ERROR_SUCCESS)
        {
            /* setupapi uses this to set other things later. */
            RegSetValueExW(key, L"Class", 0, REG_SZ, (LPBYTE)MEDIA_Class, ARRAY_SIZE(MEDIA_Class) * sizeof(WCHAR));
            RegCloseKey(key);
        }
    }

    /* pretend to be pnp */
    if ((device_set = SetupDiCreateDeviceInfoList(NULL, NULL)))
    {
        WCHAR device_name[DEVICE_ID_LEN];

        if (skip_audioendpoint && is_dualsense_device_path(device_path))
        {
            remove_persistent_sony_audioendpoint_devices(
                    IsEqualGUID(container_id, &GUID_NULL) ? NULL : container_id, NULL);
            skip_audioendpoint = FALSE;
        }

        if (skip_audioendpoint)
        {
            TRACE("Skipping fake PnP AudioEndpoint registration for %s.\n", debugstr_w(friendly_name));
        }
        else if (swprintf(device_name, ARRAY_SIZE(device_name), L"SWD\\MMDEVAPI\\{0.0.%u.00000000}.%s", flow, instguid) != -1)
        {
            memset(&device_info_data, 0, sizeof(device_info_data));
            device_info_data.cbSize = sizeof(device_info_data);

            if (
                SetupDiCreateDeviceInfoW(device_set, device_name, &AudioEndpoint_ClassGUID, device_name, NULL, 0, &device_info_data) ||
                (
                    GetLastError() == ERROR_DEVINST_ALREADY_EXISTS &&
                    SetupDiOpenDeviceInfoW(device_set, device_name, NULL, 0, &device_info_data)
                )
            )
            {
                SetupDiRegisterDeviceInfo(device_set, &device_info_data, 0, NULL, NULL, NULL);

                {
                    HKEY key;
                    key = SetupDiOpenDevRegKey(device_set, &device_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, 0);
                    if (key == INVALID_HANDLE_VALUE)
                        key = SetupDiCreateDevRegKeyW(device_set, &device_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, NULL, NULL);

                    if (key != INVALID_HANDLE_VALUE)
                        RegCloseKey(key);
                    else
                        WARN("SetupDiOpenDevRegKey and SetupDiCreateDevRegKeyW failed.");
                }

                {
                    WCHAR buf[39];
                    if (StringFromGUID2(container_id, buf, 39))
                    {
                        _wcslwr(buf);
                        TRACE("Container id: %S\n", buf);
                        if (!SetupDiSetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_BASE_CONTAINERID, (BYTE *)buf, 39 * sizeof(WCHAR)))
                            WARN("Set container id failed: 0x%lu\n", GetLastError());
                    }
                    else
                        WARN("StringFromGUID2 failed.\n");
                }

                if (!SetupDiSetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_FRIENDLYNAME, (BYTE *)friendly_name, (wcslen(friendly_name) + 1) * sizeof(WCHAR)))
                    WARN("Set friendly name failed: 0x%lu\n", GetLastError());

                memset(&device_interface_data, 0, sizeof(device_interface_data));
                device_interface_data.cbSize = sizeof(device_interface_data);

                {
                    const GUID *interface_class = flow == eRender ?
                            &DEVINTERFACE_AUDIO_RENDER : &DEVINTERFACE_AUDIO_CAPTURE;
                    HDEVINFO interface_set;

                    if (!SetupDiCreateDeviceInterfaceW(device_set, &device_info_data,
                            interface_class, NULL, 0, &device_interface_data))
                        WARN("SetupDiCreateDeviceInterfaceW failed.");
                    else if (is_dualsense_device_path(device_path))
                    {
                        interface_set = SetupDiGetClassDevsW(interface_class, NULL, NULL,
                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
                        if (interface_set == INVALID_HANDLE_VALUE)
                            WARN("Failed to publish fresh Sony controller speaker interface: %lu.\n", GetLastError());
                        else
                            SetupDiDestroyDeviceInfoList(interface_set);
                    }
                }
            }
            else
                WARN("SetupDiCreateDeviceInfoW failed.");
        }
        else
            WARN("swprintf failed.");

        if (is_dualsense_device_path(device_path))
            remove_dualsense_media_devices(NULL);

        // HACK on top of hack, create entries that MHWilds expects.
        if (enable_mhwilds_usb_audio() && device_path) {
            device_path = wcsstr(device_path, L"USB\\VID_");
            if (device_path) {
                memset(&device_info_data, 0, sizeof(device_info_data));
                device_info_data.cbSize = sizeof(device_info_data);

                if (
                    SetupDiCreateDeviceInfoW(device_set, device_path, &MEDIA_ClassGUID, device_path, NULL, 0, &device_info_data) ||
                    (
                        GetLastError() == ERROR_DEVINST_ALREADY_EXISTS &&
                        SetupDiOpenDeviceInfoW(device_set, device_path, NULL, 0, &device_info_data)
                    )
                )
                {
                    SetupDiRegisterDeviceInfo(device_set, &device_info_data, 0, NULL, NULL, NULL);

                    {
                        HKEY key;
                        key = SetupDiOpenDevRegKey(device_set, &device_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, 0);
                        if (key == INVALID_HANDLE_VALUE)
                            key = SetupDiCreateDevRegKeyW(device_set, &device_info_data, DICS_FLAG_GLOBAL, 0, DIREG_DRV, NULL, NULL);

                        if (key != INVALID_HANDLE_VALUE)
                            RegCloseKey(key);
                        else
                            WARN("SetupDiOpenDevRegKey and SetupDiCreateDevRegKeyW failed.");
                    }

                    {
                        WCHAR buf[39];
                        if (StringFromGUID2(container_id, buf, 39))
                        {
                            _wcslwr(buf);
                            TRACE("Container id: %S\n", buf);
                            if (!SetupDiSetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_BASE_CONTAINERID, (BYTE *)buf, ARRAY_SIZE(buf) * sizeof(WCHAR)))
                                WARN("Set container id failed: 0x%lu\n", GetLastError());
                        }
                        else
                            WARN("StringFromGUID2 failed.\n");
                    }

                    if (!SetupDiSetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_FRIENDLYNAME, (BYTE *)device_iname, (wcslen(device_iname) + 1) * sizeof(WCHAR)))
                        WARN("Set friendly name failed: 0x%lu\n", GetLastError());

                    {
                        static const SIZE_T base_id_len = 21; /* len of USB\VID_XXXX&PID_XXXX */
                        WCHAR buf[256];
                        const WCHAR *id_end;
                        SIZE_T full_id_len;
                        SIZE_T pos = 0;

                        id_end = wcschr(device_path + 4, '\\');
                        full_id_len = id_end ? id_end - device_path : wcslen(device_path);

                        if (full_id_len >= ARRAY_SIZE(buf))
                            full_id_len = ARRAY_SIZE(buf) - 1;

                        memcpy(buf + pos, device_path, full_id_len * sizeof(WCHAR));
                        pos += full_id_len;
                        buf[pos++] = 0;

                        if (full_id_len != base_id_len && base_id_len + pos < ARRAY_SIZE(buf))
                        {
                            memcpy(buf + pos, device_path, base_id_len * sizeof(WCHAR));
                            pos += base_id_len;
                            buf[pos++] = 0;
                        }

                        buf[pos++] = 0;
                        if (!SetupDiSetDeviceRegistryPropertyW(device_set, &device_info_data, SPDRP_HARDWAREID, (BYTE *)buf, pos * sizeof(WCHAR)))
                            WARN("Set hardware id failed: 0x%lu\n", GetLastError());
                    }

                    memset(&device_interface_data, 0, sizeof(device_interface_data));
                    device_interface_data.cbSize = sizeof(device_interface_data);

                    if (!SetupDiCreateDeviceInterfaceW(device_set, &device_info_data, &AM_KSCATEGORY_AUDIO, NULL, 0, &device_interface_data))
                        WARN("SetupDiCreateDeviceInterfaceW failed.");
                }
                else
                    WARN("SetupDiCreateDeviceInfoW failed.");
            }
        }

        SetupDiDestroyDeviceInfoList(device_set);
    }
    else
        WARN("SetupDiCreateDeviceInfoList failed.");
}

/* Creates or updates the state of a device
 * If GUID is null, a random guid will be assigned
 * and the device will be created
 */
static MMDevice *MMDevice_Create(const WCHAR *name, GUID *id, EDataFlow flow, DWORD state, BOOL setdefault)
{
    HKEY key, root;
    MMDevice *device, *cur = NULL;
    WCHAR guidstr[39];
    PROPVARIANT device_path, container_id;
    BOOL update_add = FALSE;
    BOOL update_readd = FALSE;
    BOOL update_state = FALSE;
    WCHAR friendly_name[FRIENDLY_NAME_MAX];
    const WCHAR *device_iname;
    const WCHAR *device_desc = flow == eRender ? L"Speakers" : L"Microphone";
    const WCHAR *friendly_name_fmt = L"%ls (%ls)";
    BOOL dualsense_mono_endpoint;
    BOOL sony_direct_endpoint;
    const WCHAR *sony_direct_public_name = NULL;

    static const PROPERTYKEY deviceinterface_key = {
        {0x233164c8, 0x1b2c, 0x4c7d, {0xbc, 0x68, 0xb6, 0x71, 0x68, 0x7a, 0x25, 0x67}}, 1
    };

    static const PROPERTYKEY devicepath_key = {
        {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2
    };

    PropVariantInit(&device_path);
    PropVariantInit(&container_id);
    dualsense_mono_endpoint = flow == eRender && is_dualsense_mono_endpoint_name(name);
    sony_direct_endpoint = flow == eRender && is_sony_direct_render_name(name);

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, MMDevice, entry)
    {
        if (device->flow == flow && IsEqualGUID(&device->devguid, id)){
            cur = device;
            break;
        }
    }
    LeaveCriticalSection(&device_list_cs);

    if (!cur) {
        /* No device found, allocate new one */
        cur = calloc(1, sizeof(*cur));
        if (!cur)
            return NULL;
        cur->IMMDevice_iface.lpVtbl = &MMDeviceVtbl;
        cur->IMMEndpoint_iface.lpVtbl = &MMEndpointVtbl;
        list_init(&cur->entry);
        update_add = TRUE;
    } else {
        if (cur->ref > 0)
            WARN("Modifying an MMDevice with positive reference count!\n");
        if (cur->state != state) {
            if (cur->state == DEVICE_STATE_NOTPRESENT && state == DEVICE_STATE_ACTIVE)
                update_readd = TRUE;
            update_state = TRUE;
        }
    }

    free(cur->drv_id);
    cur->drv_id = wcsdup(name);
    device_iname = cur->drv_id;
    if (dualsense_mono_endpoint &&
            dualsense_mono_endpoint_model_name(name) == DUALSENSE_MONO_MODEL_DUALSHOCK)
        device_iname = L"DualShock 4 Wireless Controller Speaker";

    swprintf(friendly_name, FRIENDLY_NAME_MAX, friendly_name_fmt, device_desc, device_iname);
    cur->removed = FALSE;
    cur->hide_from_collection = FALSE;

    cur->flow = flow;
    cur->state = state;
    cur->devguid = *id;

    StringFromGUID2(&cur->devguid, guidstr, ARRAY_SIZE(guidstr));

    if (flow == eRender)
        root = key_render;
    else
        root = key_capture;

    if (RegCreateKeyExW(root, guidstr, 0, NULL, 0, KEY_WRITE|KEY_READ|KEY_WOW64_64KEY, NULL, &key, NULL) == ERROR_SUCCESS)
    {
        HKEY keyprop;
        RegSetValueExW(key, L"DeviceState", 0, REG_DWORD, (const BYTE*)&state, sizeof(DWORD));
        if (!RegCreateKeyExW(key, L"Properties", 0, NULL, 0, KEY_WRITE|KEY_READ|KEY_WOW64_64KEY, NULL, &keyprop, NULL))
        {
            PROPVARIANT pv;
            const WCHAR *public_device_iname;
            WCHAR public_friendly_name[FRIENDLY_NAME_MAX];

            if (SUCCEEDED(set_driver_prop_value(id, flow, &devicepath_key))) {
                if (SUCCEEDED(MMDevice_GetPropValue(id, flow, &devicepath_key, &device_path)) && device_path.vt == VT_LPWSTR) {
                    const WCHAR *override;

                    if (sony_direct_endpoint)
                        sony_direct_public_name = sony_controller_speaker_name(device_path.pwszVal);
                    if (!dualsense_mono_endpoint &&
                            (override = find_product_name_override(device_path.pwszVal)) != NULL) {
                        device_iname = override;
                        swprintf(friendly_name, FRIENDLY_NAME_MAX, friendly_name_fmt, device_desc, device_iname);
                    }
                }
            }

            pv.vt = VT_LPWSTR;

            if (sony_direct_public_name)
                public_device_iname = sony_direct_public_name;
            else if (dualsense_mono_endpoint && use_windows_sony_controller_names())
                public_device_iname = L"Wireless Controller";
            else
                public_device_iname = device_iname;
            swprintf(public_friendly_name, ARRAY_SIZE(public_friendly_name), friendly_name_fmt,
                    device_desc, public_device_iname);

            pv.pwszVal = public_friendly_name;
            MMDevice_SetPropValue(id, flow, (const PROPERTYKEY*)&DEVPKEY_Device_FriendlyName, &pv);

            pv.pwszVal = (LPWSTR)public_device_iname;
            MMDevice_SetPropValue(id, flow, (const PROPERTYKEY*)&DEVPKEY_DeviceInterface_FriendlyName, &pv);

            pv.pwszVal = (LPWSTR)device_desc;
            MMDevice_SetPropValue(id, flow, (const PROPERTYKEY*)&DEVPKEY_Device_DeviceDesc, &pv);

            pv.pwszVal = guidstr;
            MMDevice_SetPropValue(id, flow, &deviceinterface_key, &pv);

            if (FAILED(set_get_driver_prop_value(id, flow, (const PROPERTYKEY*)&DEVPKEY_Device_ContainerId, &container_id)) && state == DEVICE_STATE_ACTIVE)
                WARN("Failed to get and set container id\n");

            if (FAILED(set_driver_prop_value(id, flow, &PKEY_AudioEndpoint_FormFactor)))
            {
                pv.vt = VT_UI4;
                pv.ulVal = (flow == eCapture) ? Microphone : Speakers;

                MMDevice_SetPropValue(id, flow, &PKEY_AudioEndpoint_FormFactor, &pv);
            }

            if (flow != eCapture)
            {
                PROPVARIANT pv2;

                PropVariantInit(&pv2);

                /* make read-write by not overwriting if already set */
                if (FAILED(MMDevice_GetPropValue(id, flow, &PKEY_AudioEndpoint_PhysicalSpeakers, &pv2)) || pv2.vt != VT_UI4)
                    set_driver_prop_value(id, flow, &PKEY_AudioEndpoint_PhysicalSpeakers);

                PropVariantClear(&pv2);
            }

            pv.vt = VT_LPWSTR;
            pv.pwszVal = drvs.module_name;

            MMDevice_SetPropValue(id, flow, (const PROPERTYKEY*)&DEVPKEY_Device_Driver, &pv);

            RegCloseKey(keyprop);
        }
        RegCloseKey(key);
    }

    MMDevice_Register(guidstr, friendly_name, device_iname,
        device_path.vt == VT_LPWSTR ? device_path.pwszVal : NULL,
        container_id.vt == VT_CLSID ? container_id.puuid : &GUID_NULL,
        dualsense_mono_endpoint, flow);
    PropVariantClear(&device_path);
    PropVariantClear(&container_id);

    if (state == DEVICE_STATE_ACTIVE)
        set_format(cur);

    if (update_add)
    {
        EnterCriticalSection(&device_list_cs);
        list_add_tail(&device_list, &cur->entry);
        LeaveCriticalSection(&device_list_cs);
    }

    if (update_add || update_readd || update_state)
    {
        struct NotificationClientWrapper *wrapper;
        WCHAR devid[56];
        swprintf(devid, 56, devid_formatW,
            flow, id->Data1, id->Data2, id->Data3,
            id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
            id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);

        EnterCriticalSection(&g_notif_lock);
        LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients, struct NotificationClientWrapper, entry)
        {
            if (update_add || update_readd)
                IMMNotificationClient_OnDeviceAdded(wrapper->client, devid);
            if (update_add || (update_state && !update_readd))
                IMMNotificationClient_OnDeviceStateChanged(wrapper->client, devid, state);
        }
        LeaveCriticalSection(&g_notif_lock);
    }

    if (setdefault)
    {
        EnterCriticalSection(&g_notif_lock);
        if (flow == eRender)
            MMDevice_def_play = cur;
        else
            MMDevice_def_rec = cur;
        LeaveCriticalSection(&g_notif_lock);
    }
    return cur;
}

HRESULT load_devices_from_reg(void)
{
    DWORD i = 0;
    HKEY root, cur;
    LONG ret;
    DWORD curflow;

    ret = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio", 0, NULL, 0,
            KEY_WRITE|KEY_READ|KEY_WOW64_64KEY, NULL, &root, NULL);
    if (ret == ERROR_SUCCESS)
        ret = RegCreateKeyExW(root, L"Capture", 0, NULL, 0, KEY_READ|KEY_WRITE|KEY_WOW64_64KEY, NULL, &key_capture, NULL);
    if (ret == ERROR_SUCCESS)
        ret = RegCreateKeyExW(root, L"Render", 0, NULL, 0, KEY_READ|KEY_WRITE|KEY_WOW64_64KEY, NULL, &key_render, NULL);
    RegCloseKey(root);
    cur = key_capture;
    curflow = eCapture;
    if (ret != ERROR_SUCCESS)
    {
        if (key_render)
            RegCloseKey(key_render);
        if (key_capture)
            RegCloseKey(key_capture);
        key_render = key_capture = NULL;
        WARN("Couldn't create key: %lu\n", ret);
        return E_FAIL;
    }

    do {
        WCHAR guidvalue[39];
        GUID guid;
        DWORD len;
        PROPVARIANT pv = { VT_EMPTY };

        len = ARRAY_SIZE(guidvalue);
        ret = RegEnumKeyExW(cur, i++, guidvalue, &len, NULL, NULL, NULL, NULL);
        if (ret == ERROR_NO_MORE_ITEMS)
        {
            if (cur == key_capture)
            {
                cur = key_render;
                curflow = eRender;
                i = 0;
                continue;
            }
            break;
        }
        if (ret != ERROR_SUCCESS)
            continue;
        if (SUCCEEDED(CLSIDFromString(guidvalue, &guid))
            && SUCCEEDED(MMDevice_GetPropValue(&guid, curflow, (const PROPERTYKEY*)&DEVPKEY_DeviceInterface_FriendlyName, &pv))
            && pv.vt == VT_LPWSTR)
        {
            if (is_dualsense_endpoint_name( pv.pwszVal ))
            {
                TRACE("Removing persisted DualSense endpoint %s.\n", debugstr_w(pv.pwszVal));
                if (RegDeleteTreeW(cur, guidvalue) == ERROR_SUCCESS)
                    i--;
            }
            else
                MMDevice_Create(pv.pwszVal, &guid, curflow,
                        DEVICE_STATE_NOTPRESENT, FALSE);
            CoTaskMemFree(pv.pwszVal);
        }
    } while (1);

    return S_OK;
}

static HRESULT set_format(MMDevice *dev)
{
    HRESULT hr;
    IAudioClient *client;
    WAVEFORMATEX *fmt;
    WAVEFORMATEXTENSIBLE *fmtex;
    PROPVARIANT pv = { VT_EMPTY };

    hr = AudioClient_Create(&dev->devguid, &dev->IMMDevice_iface, &client);
    if(FAILED(hr))
        return hr;

    hr = IAudioClient_GetMixFormat(client, &fmt);
    if(FAILED(hr)){
        IAudioClient_Release(client);
        return hr;
    }

    IAudioClient_Release(client);

    /* for most devices, native Windows only allows PCM formats for
     * DeviceFormat. GetMixFormat often returns float. */
    if(fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE){
        fmtex = (WAVEFORMATEXTENSIBLE *)fmt;
        if(IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)){
            fmt->wBitsPerSample = 16;
            fmt->nBlockAlign = fmt->wBitsPerSample * fmt->nChannels / 8;
            fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
            fmtex->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            fmtex->Samples.wValidBitsPerSample = fmt->wBitsPerSample;
        }
    }else if(fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT){
        fmt->wFormatTag = WAVE_FORMAT_PCM;
        fmt->wBitsPerSample = 16;
        fmt->nBlockAlign = fmt->wBitsPerSample * fmt->nChannels / 8;
        fmt->nAvgBytesPerSec = fmt->nSamplesPerSec * fmt->nBlockAlign;
    }

    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(WAVEFORMATEX) + fmt->cbSize;
    pv.blob.pBlobData = (BYTE*)fmt;
    MMDevice_SetPropValue(&dev->devguid, dev->flow,
            &PKEY_AudioEngine_DeviceFormat, &pv);
    MMDevice_SetPropValue(&dev->devguid, dev->flow,
            &PKEY_AudioEngine_OEMFormat, &pv);
    CoTaskMemFree(fmt);

    return S_OK;
}

static LONG dualsense_mono_render_add_pending;
static LONG dualsense_mono_activation_serial;
static enum dualsense_mono_model dualsense_mono_render_add_model;
static void promote_delayed_dualsense_profile_in_cache(const GUID *guid,
        const char *added_name, EDataFlow flow);

static void add_endpoints_from_params(struct get_endpoint_ids_params *params, BOOL hotplug)
{
    UINT i;

    for (i = 0; i < params->num; i++) {
        BOOL already_published;
        MMDevice *device;
        GUID guid;
        const WCHAR *name = (WCHAR *)((char *)params->endpoints + params->endpoints[i].name);
        const char *dev_name = (char *)params->endpoints + params->endpoints[i].device;
        get_device_guid(params->flow, dev_name, TRUE, &guid);
        promote_delayed_dualsense_profile_in_cache(&guid, dev_name, params->flow);
        already_published = device_is_published(&guid, params->flow);
        device = MMDevice_Create(name, &guid, params->flow, DEVICE_STATE_ACTIVE, params->default_idx == i);
        if (device && params->flow == eRender &&
                (is_dualsense_mono_endpoint(device) ||
                 (is_dualsense_backend_name(dev_name) && strstr(dev_name, "Direct__Direct__sink"))))
        {
            const GUID *id = &device->devguid;
            WCHAR devid[56];

            if ((device->state & DEVICE_STATE_ACTIVE) && !should_hide_from_endpoint_collection(device))
            {
                swprintf(devid, ARRAY_SIZE(devid), devid_formatW,
                        device->flow, id->Data1, id->Data2, id->Data3,
                        id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
                        id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);
                death_stranding_controller_endpoint_seen(devid);
            }

            if (!already_published && is_dualsense_mono_endpoint(device) &&
                    (hotplug || !death_stranding_controller_output_hook_active()))
            {
                dualsense_mono_render_add_model = dualsense_mono_endpoint_model(device);
                InterlockedExchange(&dualsense_mono_render_add_pending, 1);
            }
        }
    }
}

static void alias_dualsense_profile_replacement_in_cache(
        const struct get_endpoint_ids_params *params, const char *removed_name,
        const GUID *guid)
{
    UINT i;

    EnterCriticalSection(&devices_cache_cs);
    for (i = 0; i < params->num; ++i)
    {
        const char *added_name = (const char *)params->endpoints + params->endpoints[i].device;
        struct device *cached, *next;

        if (!is_dualsense_backend_name(added_name) ||
                !dualsense_device_names_replace_profile(added_name, removed_name, params->flow))
            continue;

        LIST_FOR_EACH_ENTRY_SAFE(cached, next, &devices_cache, struct device, entry)
        {
            if (cached->flow != params->flow || !IsEqualGUID(&cached->guid, guid))
                continue;
            list_remove(&cached->entry);
            free(cached);
        }
        add_device_to_cache(guid, added_name, params->flow);
    }
    LeaveCriticalSection(&devices_cache_cs);
}

static void promote_delayed_dualsense_profile_in_cache(const GUID *guid,
        const char *added_name, EDataFlow flow)
{
    struct device *cached, *next, *current = NULL;

    if (!use_stable_dualsense_device_ids() || !is_dualsense_backend_name(added_name))
        return;

    EnterCriticalSection(&devices_cache_cs);
    LIST_FOR_EACH_ENTRY(cached, &devices_cache, struct device, entry)
    {
        if (cached->flow == flow && IsEqualGUID(&cached->guid, guid))
        {
            current = cached;
            break;
        }
    }
    if (!current || !dualsense_device_names_replace_profile(added_name, current->name, flow))
    {
        LeaveCriticalSection(&devices_cache_cs);
        return;
    }

    TRACE("Promoting delayed Sony profile endpoint %s over stale backend %s for %s.\n",
            debugstr_a(added_name), debugstr_a(current->name), debugstr_guid(guid));
    LIST_FOR_EACH_ENTRY_SAFE(cached, next, &devices_cache, struct device, entry)
    {
        if (cached->flow != flow || !IsEqualGUID(&cached->guid, guid))
            continue;
        list_remove(&cached->entry);
        free(cached);
    }
    add_device_to_cache(guid, added_name, flow);
    LeaveCriticalSection(&devices_cache_cs);
}

static BOOL endpoint_batch_replaces_dualsense_profile(
        const struct get_endpoint_ids_params *params, const char *removed_name)
{
    UINT i;

    if (!use_stable_dualsense_device_ids() || !is_dualsense_backend_name(removed_name))
        return FALSE;

    for (i = 0; i < params->num; ++i)
    {
        const char *added_name = (const char *)params->endpoints + params->endpoints[i].device;

        if (is_dualsense_backend_name(added_name) &&
                dualsense_device_names_replace_profile(added_name, removed_name, params->flow))
            return TRUE;
    }

    return FALSE;
}

struct pending_sony_endpoint_removal
{
    struct list entry;
    GUID guid;
    EDataFlow flow;
    char backend_name[];
};

static BOOL queue_pending_sony_endpoint_removal(struct list *pending_removals,
        EDataFlow flow, const GUID *guid, const char *backend_name)
{
    struct pending_sony_endpoint_removal *pending;
    size_t name_length = strlen(backend_name) + 1;

    LIST_FOR_EACH_ENTRY(pending, pending_removals, struct pending_sony_endpoint_removal, entry)
        if (pending->flow == flow && IsEqualGUID(&pending->guid, guid))
            return TRUE;

    if (!(pending = malloc(offsetof(struct pending_sony_endpoint_removal,
            backend_name[name_length]))))
        return FALSE;

    pending->guid = *guid;
    pending->flow = flow;
    memcpy(pending->backend_name, backend_name, name_length);
    list_add_tail(pending_removals, &pending->entry);
    return TRUE;
}

static BOOL endpoint_batch_completes_pending_sony_profile_change(
        const struct get_endpoint_ids_params *params,
        const struct pending_sony_endpoint_removal *pending)
{
    UINT i;

    if (params->flow != eCapture)
        return FALSE;

    for (i = 0; i < params->num; ++i)
    {
        const char *added_name = (const char *)params->endpoints + params->endpoints[i].device;

        if (is_dualsense_backend_name(added_name) &&
                dualsense_device_names_replace_physical_profile(added_name,
                pending->backend_name))
            return TRUE;
    }

    return FALSE;
}

static void MMDevice_Destroy(MMDevice *This);

static void remove_sony_controller_endpoint(MMDevice *dev)
{
    BOOL dualsense_mono_endpoint = is_dualsense_mono_endpoint(dev);
    PROPVARIANT container_id = {VT_EMPTY};
    GUID id = dev->devguid;

    TRACE("Removing disconnected Sony controller endpoint %s from active device list.\n",
            debugstr_guid(&id));
    if (dualsense_mono_endpoint)
    {
        if (SUCCEEDED(MMDevice_GetPropValue(&dev->devguid, dev->flow,
                (const PROPERTYKEY *)&DEVPKEY_Device_ContainerId, &container_id)) &&
                container_id.vt == VT_CLSID)
            remove_persistent_sony_audioendpoint_devices(NULL, container_id.puuid);
        else if (!keep_sony_audio_endpoint_visible())
            remove_dualsense_audioendpoint_devices();
        remove_dualsense_media_devices(container_id.vt == VT_CLSID ? container_id.puuid : NULL);
    }
    PropVariantClear(&container_id);
    notify_endpoint_removed(dev->flow, &id);
    remove_device_from_cache(&id);
    delete_device_state_key(dev);
    dev->removed = TRUE;
    dev->state = DEVICE_STATE_NOTPRESENT;
    list_remove(&dev->entry);
    list_init(&dev->entry);
    if (!dev->ref)
        MMDevice_Destroy(dev);
}

static void finish_pending_sony_endpoint_removals(struct list *pending_removals,
        const struct get_endpoint_ids_params *params)
{
    struct pending_sony_endpoint_removal *pending, *next;

    LIST_FOR_EACH_ENTRY_SAFE(pending, next, pending_removals,
            struct pending_sony_endpoint_removal, entry)
    {
        MMDevice *dev;

        list_remove(&pending->entry);
        if (endpoint_batch_completes_pending_sony_profile_change(params, pending))
        {
            TRACE("Keeping Sony controller endpoint %s active across asynchronous audio profile replacement.\n",
                    debugstr_guid(&pending->guid));
            free(pending);
            continue;
        }

        EnterCriticalSection(&device_list_cs);
        LIST_FOR_EACH_ENTRY(dev, &device_list, MMDevice, entry)
        {
            if (dev->flow == pending->flow && IsEqualGUID(&dev->devguid, &pending->guid))
            {
                remove_sony_controller_endpoint(dev);
                break;
            }
        }
        LeaveCriticalSection(&device_list_cs);
        free(pending);
    }
}

static void free_pending_sony_endpoint_removals(struct list *pending_removals)
{
    struct pending_sony_endpoint_removal *pending, *next;

    LIST_FOR_EACH_ENTRY_SAFE(pending, next, pending_removals,
            struct pending_sony_endpoint_removal, entry)
    {
        list_remove(&pending->entry);
        free(pending);
    }
}

static HANDLE g_update_thread;
static BOOL g_update_thread_running;
static LONG render_update_in_progress;

static void wait_for_render_update_if_needed(EDataFlow flow)
{
    unsigned int i;

    if (flow == eCapture)
        return;

    if (!InterlockedCompareExchange(&render_update_in_progress, 0, 0))
        return;

    TRACE("Waiting for pending render endpoint update before enumerating flow %d.\n", flow);
    for (i = 0; i < 200 && InterlockedCompareExchange(&render_update_in_progress, 0, 0); ++i)
        Sleep(10);
    if (i == 200)
        WARN("Timed out waiting for pending render endpoint update.\n");
}

static DWORD WINAPI update_thread_proc(void *user)
{
    struct get_endpoint_ids_params params;
    struct list pending_sony_removals = LIST_INIT(pending_sony_removals);
    struct endpoint_notification *pending_hid_notifications = NULL;
    unsigned int pending_hid_count = 0, pending_hid_capacity = 0;
    UINT i;

    params.flow = eRender;
    params.size = 1024;
    params.endpoints = malloc(params.size);
    params.delta = TRUE;

    for (;;) {
        for (;;) {
            if (!g_update_thread_running)
                goto end;
            params.more_data = FALSE;
            __wine_unix_call(drvs.module_unixlib, get_endpoint_ids, &params);

            if (params.result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
                free(params.endpoints);
                params.endpoints = malloc(params.size);
            }
            else
                break;
        }

        if (FAILED(params.result))
            goto end;

        if (params.more_data && params.flow == eCapture)
            InterlockedExchange(&render_update_in_progress, 1);
        if (params.flow == eRender)
            InterlockedExchange(&render_update_in_progress, 1);

        /* FIXME: update default device when removed, though currently only core audio backend could potentially */
        /* change the default device, and it doesn't have hotplug implemented. */
        for (i = 0; i < params.num_removed; i++) {
            GUID guid;
            MMDevice *dev;
            UINT i_removed = params.num + i;
            const WCHAR *name = (WCHAR *)((char *)params.endpoints + params.endpoints[i_removed].name);
            const char *dev_name = (char *)params.endpoints + params.endpoints[i_removed].device;
            get_device_guid( params.flow, dev_name, FALSE, &guid );

            EnterCriticalSection(&device_list_cs);
            LIST_FOR_EACH_ENTRY(dev, &device_list, MMDevice, entry) {
                WCHAR guidstr[39];
                HKEY key, root;
                if (dev->flow != params.flow)
                    continue;
                if (IsEqualGUID(&dev->devguid, &guid) ||
                        (!strstr(dev_name, "#wine-sony-physical-") && dev->drv_id &&
                         is_dualsense_mono_endpoint_name(name) && !wcscmp(dev->drv_id, name))) {
                    struct NotificationClientWrapper *wrapper;
                    const GUID *id = &dev->devguid;
                    BOOL sony_controller_endpoint = is_dualsense_endpoint_name(dev->drv_id);
                    BOOL profile_replacement = endpoint_batch_replaces_dualsense_profile(
                            &params, dev_name);
                    WCHAR devid[56];

                    if (sony_controller_endpoint)
                    {
                        if (profile_replacement)
                        {
                            alias_dualsense_profile_replacement_in_cache(&params, dev_name, id);
                            TRACE("Keeping Sony controller endpoint %s active across audio profile replacement.\n",
                                    debugstr_guid(id));
                            break;
                        }

                        if (params.flow == eRender && use_stable_dualsense_device_ids() &&
                                params.more_data && queue_pending_sony_endpoint_removal(
                                &pending_sony_removals, dev->flow, id, dev_name))
                        {
                            TRACE("Deferring Sony controller endpoint removal %s until the paired capture update.\n",
                                    debugstr_guid(id));
                            break;
                        }

                        remove_sony_controller_endpoint(dev);
                        break;
                    }

                    dev->state = DEVICE_STATE_NOTPRESENT;
                    StringFromGUID2(&dev->devguid, guidstr, ARRAY_SIZE(guidstr));
                    if (dev->flow == eRender)
                        root = key_render;
                    else
                        root = key_capture;
                    if (RegCreateKeyExW(root, guidstr, 0, NULL, 0, KEY_WRITE|KEY_READ|KEY_WOW64_64KEY, NULL, &key, NULL) == ERROR_SUCCESS)
                    {
                        RegSetValueExW(key, L"DeviceState", 0, REG_DWORD, (const BYTE*)&dev->state, sizeof(DWORD));
                        RegCloseKey(key);
                    }

                    swprintf(devid, 56, devid_formatW,
                        dev->flow, id->Data1, id->Data2, id->Data3,
                        id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
                        id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);
                    EnterCriticalSection(&g_notif_lock);
                    LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients, struct NotificationClientWrapper, entry)
                    {
                        IMMNotificationClient_OnDeviceStateChanged(wrapper->client, devid, dev->state);
                    }
                    LeaveCriticalSection(&g_notif_lock);

                    break;
                }
            }
            LeaveCriticalSection(&device_list_cs);
        }

        /* add devices only after removing devices */
        add_endpoints_from_params(&params, TRUE);

        if (params.flow == eCapture && !list_empty(&pending_sony_removals))
            finish_pending_sony_endpoint_removals(&pending_sony_removals, &params);

        if (params.flow == eRender)
        {
            enum dualsense_mono_model model = DUALSENSE_MONO_MODEL_UNKNOWN;
            BOOL resignal = FALSE;

            if (InterlockedExchange(&dualsense_mono_render_add_pending, 0))
            {
                unsigned int start;

                model = dualsense_mono_render_add_model;
                dualsense_mono_render_add_model = DUALSENSE_MONO_MODEL_UNKNOWN;
                resignal = TRUE;
                start = collect_visible_dualsense_mono_endpoints(model, &pending_hid_notifications,
                        &pending_hid_count, &pending_hid_capacity);
                notify_dualsense_mono_endpoints(pending_hid_notifications, start,
                        pending_hid_count, FALSE);
            }
            InterlockedExchange(&render_update_in_progress, 0);
            if (resignal && !pending_hid_count)
                WARN("No visible Sony controller speaker endpoint was available for HID release.\n");
        }

        if (!params.more_data && pending_hid_count)
        {
            notify_dualsense_mono_endpoints(pending_hid_notifications, 0,
                    pending_hid_count, TRUE);
            free(pending_hid_notifications);
            pending_hid_notifications = NULL;
            pending_hid_count = pending_hid_capacity = 0;
        }

        if (params.more_data)
            params.flow = params.flow == eRender ? eCapture : eRender;
    }

end:
    InterlockedExchange(&render_update_in_progress, 0);
    InterlockedExchange(&dualsense_mono_render_add_pending, 0);
    dualsense_mono_render_add_model = DUALSENSE_MONO_MODEL_UNKNOWN;
    free_pending_sony_endpoint_removals(&pending_sony_removals);
    free(pending_hid_notifications);
    free(params.endpoints);
    return 0;
}

static BOOL need_update_thread;
void create_update_thread(void)
{
    if (!need_update_thread)
        return;

    if (g_update_thread)
        return;

    EnterCriticalSection(&device_list_cs);

    if (g_update_thread) {
        LeaveCriticalSection(&device_list_cs);
        return;
    }

    g_update_thread_running = TRUE;
    g_update_thread = CreateThread(NULL, 0, update_thread_proc, NULL, 0, NULL);
    if (!g_update_thread) {
        ERR("CreateThread failed: %lu\n", GetLastError());
        g_update_thread_running = FALSE;
    } else
        SetThreadPriority(g_update_thread, THREAD_PRIORITY_BELOW_NORMAL);

    LeaveCriticalSection(&device_list_cs);
}

void stop_update_thread(void)
{
    if (g_update_thread) {
        g_update_thread_running = FALSE;
        WaitForSingleObject(g_update_thread, INFINITE);
        CloseHandle(g_update_thread);
        g_update_thread = NULL;
    }
}

HRESULT load_driver_devices(EDataFlow flow)
{
    struct get_endpoint_ids_params params;

    params.flow = flow;
    params.size = 1024;
    params.endpoints = NULL;
    do {
        free(params.endpoints);
        params.endpoints = malloc(params.size);
        params.delta = FALSE;
        __wine_unix_call(drvs.module_unixlib, get_endpoint_ids, &params);
    } while (params.result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

    if (FAILED(params.result))
        goto end;

    if (params.delta)
        need_update_thread = TRUE;
    add_endpoints_from_params(&params, FALSE);

end:
    free(params.endpoints);

    return params.result;
}

static void MMDevice_Destroy(MMDevice *This)
{
    TRACE("Freeing %s\n", debugstr_w(This->drv_id));
    if (!list_empty(&This->entry))
        list_remove(&This->entry);
    free(This->drv_id);
    free(This);
}

static inline MMDevice *impl_from_IMMDevice(IMMDevice *iface)
{
    return CONTAINING_RECORD(iface, MMDevice, IMMDevice_iface);
}

static BOOL is_sony_four_channel_audio_format(const WAVEFORMATEX *format)
{
    const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;

    return format && format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            format->cbSize >= sizeof(*extensible) - sizeof(*format) &&
            format->nChannels == 4 && format->nSamplesPerSec == 48000 &&
            format->wBitsPerSample == 32 &&
            IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

static BOOL is_sony_four_channel_stream_format(const WAVEFORMATEX *format)
{
    const WAVEFORMATEXTENSIBLE *extensible = (const WAVEFORMATEXTENSIBLE *)format;

    if (is_sony_four_channel_audio_format(format))
        return TRUE;

    return format && format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            format->cbSize >= sizeof(*extensible) - sizeof(*format) &&
            format->nChannels == 4 && format->nSamplesPerSec == 48000 &&
            format->wBitsPerSample == 16 &&
            IsEqualGUID(&extensible->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM);
}

static BOOL is_sony_controller_render_endpoint(const MMDevice *device)
{
    PROPVARIANT container_id;
    BOOL ret = FALSE;

    if (device->flow != eRender)
        return FALSE;
    if (is_dualsense_mono_endpoint(device))
        return TRUE;

    PropVariantInit(&container_id);
    if (SUCCEEDED(MMDevice_GetPropValue(&device->devguid, device->flow,
            (const PROPERTYKEY *)&DEVPKEY_Device_ContainerId, &container_id)) &&
            container_id.vt == VT_CLSID && container_id.puuid)
        ret = is_dualsense_audioendpoint_container(container_id.puuid);
    PropVariantClear(&container_id);

    return ret;
}

static BOOL select_sony_windows_audio_mode(MMDevice *device,
        unsigned int probe_count, BOOL stream_init)
{
    BOOL selected;
    HANDLE event;

    selected = InterlockedCompareExchange(&sony_windows_audio_mode, 0, 0) ||
            sony_windows_audio_mode_event_enabled();

    if (!selected && !InterlockedCompareExchange(&sony_windows_audio_mode, 1, 0))
    {
        event = CreateEventW(NULL, TRUE, TRUE, SONY_WINDOWS_AUDIO_MODE_EVENT);
        if (!event)
            WARN("Failed to publish automatic Windows Sony audio mode, error %lu.\n", GetLastError());
        else
            sony_windows_audio_mode_event = event;
        if (stream_init)
            TRACE("Selected Windows Sony audio mode for a native four-channel stream.\n");
        else if (probe_count)
            TRACE("Selected Windows Sony audio mode after %u format probes.\n", probe_count);
        else
            TRACE("Selected Windows Sony audio mode during controller endpoint discovery.\n");
    }

    EnterCriticalSection(&device_list_cs);
    if (device->hide_from_collection)
    {
        TRACE("Restoring auto-detected Sony endpoint %s to endpoint collections.\n",
                debugstr_guid(&device->devguid));
        device->hide_from_collection = FALSE;
    }
    LeaveCriticalSection(&device_list_cs);
    return TRUE;
}

BOOL auto_select_sony_audio_mode(IMMDevice *iface, AUDCLNT_SHAREMODE mode,
        const WAVEFORMATEX *format, unsigned int *probe_count)
{
    MMDevice *device = impl_from_IMMDevice(iface);
    BOOL selected, select_windows_names;
    BOOL mono_speaker;

    if (mode != AUDCLNT_SHAREMODE_SHARED || !is_sony_controller_render_endpoint(device))
        return FALSE;

    mono_speaker = is_dualsense_mono_endpoint(device);
    selected = sony_windows_audio_mode_selected();
    select_windows_names = InterlockedCompareExchange(
            &sony_windows_controller_names, 0, 0);

    /* A capability sweep is how Windows-style Sony audio clients discover the
     * four-channel endpoint. A native client may already have selected the
     * same transport by activating the endpoint, but it keeps the model-specific
     * controller-speaker identity unless it performs the complete sweep. */
    if (!select_windows_names && !selected && mono_speaker &&
            is_sony_four_channel_audio_format(format))
        select_windows_names = TRUE;
    else if (!select_windows_names &&
            *probe_count < SONY_AUDIO_CAPABILITY_PROBE_THRESHOLD &&
            ++*probe_count >= SONY_AUDIO_CAPABILITY_PROBE_THRESHOLD)
        select_windows_names = TRUE;

    if (select_windows_names &&
            !InterlockedExchange(&sony_windows_controller_names, 1))
        TRACE("Selected canonical Windows Sony controller endpoint names.\n");

    if (!selected && !select_windows_names)
        return FALSE;

    return select_sony_windows_audio_mode(device, *probe_count, FALSE);
}

BOOL select_sony_audio_mode_for_stream(IMMDevice *iface, AUDCLNT_SHAREMODE mode,
        const WAVEFORMATEX *format)
{
    MMDevice *device = impl_from_IMMDevice(iface);

    if (mode != AUDCLNT_SHAREMODE_SHARED || !is_sony_controller_render_endpoint(device))
        return FALSE;

    if (!is_sony_four_channel_stream_format(format) &&
            !InterlockedCompareExchange(&sony_windows_audio_mode, 0, 0) &&
            !sony_windows_audio_mode_event_enabled())
        return FALSE;

    return select_sony_windows_audio_mode(device, 0, TRUE);
}

static BOOL select_sony_audio_mode_for_activation(IMMDevice *iface)
{
    MMDevice *device = impl_from_IMMDevice(iface);

    if (!is_sony_controller_render_endpoint(device))
        return FALSE;

    return select_sony_windows_audio_mode(device, 0, FALSE);
}

static BOOL MMDevCol_device_visible(IMMDevice *iface)
{
    return !should_hide_from_endpoint_collection(impl_from_IMMDevice(iface));
}

static HRESULT WINAPI MMDevice_QueryInterface(IMMDevice *iface, REFIID riid, void **ppv)
{
    MMDevice *This = impl_from_IMMDevice(iface);
    TRACE("(%p)->(%s,%p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_IMMDevice))
        *ppv = &This->IMMDevice_iface;
    else if (IsEqualIID(riid, &IID_IMMEndpoint))
        *ppv = &This->IMMEndpoint_iface;
    if (*ppv)
    {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }
    WARN("Unknown interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI MMDevice_AddRef(IMMDevice *iface)
{
    MMDevice *This = impl_from_IMMDevice(iface);
    LONG ref;

    ref = InterlockedIncrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    return ref;
}

static ULONG WINAPI MMDevice_Release(IMMDevice *iface)
{
    MMDevice *This = impl_from_IMMDevice(iface);
    LONG ref;

    ref = InterlockedDecrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    if (!ref && This->removed)
        MMDevice_Destroy(This);
    return ref;
}

static void retire_old_hidden_dualsense_mono_endpoints(MMDevice *active)
{
    MMDevice *device, *next;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY_SAFE(device, next, &device_list, MMDevice, entry)
    {
        if (device == active || !is_dualsense_mono_endpoint(device) || !device->hide_from_collection)
            continue;

        TRACE("Retiring old hidden DualSense mono endpoint %s after activating %s.\n",
                debugstr_guid(&device->devguid), debugstr_guid(&active->devguid));
        device->removed = TRUE;
        device->state = DEVICE_STATE_NOTPRESENT;
        list_remove(&device->entry);
        list_init(&device->entry);
        if (!device->ref)
            MMDevice_Destroy(device);
    }
    LeaveCriticalSection(&device_list_cs);
}

static HRESULT WINAPI MMDevice_Activate(IMMDevice *iface, REFIID riid, DWORD clsctx, PROPVARIANT *params, void **ppv)
{
    HRESULT hr = E_NOINTERFACE;
    MMDevice *This = impl_from_IMMDevice(iface);

    TRACE("(%p)->(%s, %lx, %p, %p)\n", iface, debugstr_guid(riid), clsctx, params, ppv);

    if (!ppv)
        return E_POINTER;

    if (This->removed || This->state == DEVICE_STATE_NOTPRESENT)
        return E_NOTFOUND;

    if (IsEqualIID(riid, &IID_IAudioClient) ||
            IsEqualIID(riid, &IID_IAudioClient2) ||
            IsEqualIID(riid, &IID_IAudioClient3))
    {
        hr = AudioClient_Create(&This->devguid, iface, (IAudioClient**)ppv);
        if (SUCCEEDED(hr))
            select_sony_audio_mode_for_activation(iface);
        if (SUCCEEDED(hr) && is_dualsense_mono_endpoint(This) &&
                !keep_sony_audio_endpoint_visible())
        {
            TRACE("Hiding activated DualSense mono endpoint %s from endpoint collections.\n",
                    debugstr_guid(&This->devguid));
            This->hide_from_collection = TRUE;
            This->dualsense_mono_activation_serial = InterlockedIncrement(&dualsense_mono_activation_serial);
            retire_old_hidden_dualsense_mono_endpoints(This);
        }
    }else if (IsEqualIID(riid, &IID_IAudioEndpointVolume) ||
            IsEqualIID(riid, &IID_IAudioEndpointVolumeEx))
        hr = AudioEndpointVolume_Create(This, (IAudioEndpointVolumeEx**)ppv);
    else if (IsEqualIID(riid, &IID_IAudioSessionManager)
             || IsEqualIID(riid, &IID_IAudioSessionManager2))
    {
        hr = AudioSessionManager_Create(iface, (IAudioSessionManager2**)ppv);
    }
    else if (IsEqualIID(riid, &IID_IBaseFilter))
    {
        if (This->flow == eRender)
            hr = CoCreateInstance(&CLSID_DSoundRender, NULL, clsctx, riid, ppv);
        else
            ERR("Not supported for recording?\n");
        if (SUCCEEDED(hr))
        {
            IPersistPropertyBag *ppb;
            hr = IUnknown_QueryInterface((IUnknown*)*ppv, &IID_IPersistPropertyBag, (void **)&ppb);
            if (SUCCEEDED(hr))
            {
                /* ::Load cannot assume the interface stays alive after the function returns,
                 * so just create the interface on the stack, saves a lot of complicated code */
                IPropertyBagImpl bag = { { &PB_Vtbl } };
                bag.devguid = This->devguid;
                hr = IPersistPropertyBag_Load(ppb, &bag.IPropertyBag_iface, NULL);
                IPersistPropertyBag_Release(ppb);
                if (FAILED(hr))
                    IBaseFilter_Release((IBaseFilter*)*ppv);
            }
            else
            {
                FIXME("Wine doesn't support IPersistPropertyBag on DSoundRender yet, ignoring..\n");
                hr = S_OK;
            }
        }
    }
    else if (IsEqualIID(riid, &IID_IDeviceTopology))
    {
        hr = DeviceTopology_Create(iface, (IDeviceTopology**)ppv);
    }
    else if (IsEqualIID(riid, &IID_IDirectSound)
             || IsEqualIID(riid, &IID_IDirectSound8))
    {
        if (This->flow == eRender)
            hr = CoCreateInstance(&CLSID_DirectSound8, NULL, clsctx, riid, ppv);
        if (SUCCEEDED(hr))
        {
            hr = IDirectSound_Initialize((IDirectSound*)*ppv, &This->devguid);
            if (FAILED(hr))
                IDirectSound_Release((IDirectSound*)*ppv);
        }
    }
    else if (IsEqualIID(riid, &IID_IDirectSoundCapture))
    {
        if (This->flow == eCapture)
            hr = CoCreateInstance(&CLSID_DirectSoundCapture8, NULL, clsctx, riid, ppv);
        if (SUCCEEDED(hr))
        {
            hr = IDirectSoundCapture_Initialize((IDirectSoundCapture*)*ppv, &This->devguid);
            if (FAILED(hr))
                IDirectSoundCapture_Release((IDirectSoundCapture*)*ppv);
        }
    }
    else if (IsEqualIID(riid, &IID_ISpatialAudioClient))
    {
        hr = SpatialAudioClient_Create(iface, (ISpatialAudioClient**)ppv);
    }
    else
        ERR("Invalid/unknown iid %s\n", debugstr_guid(riid));

    if (FAILED(hr))
        *ppv = NULL;

    TRACE("Returning %08lx\n", hr);
    return hr;
}

static HRESULT WINAPI MMDevice_OpenPropertyStore(IMMDevice *iface, DWORD access, IPropertyStore **ppv)
{
    MMDevice *This = impl_from_IMMDevice(iface);
    TRACE("(%p)->(%lx,%p)\n", This, access, ppv);

    if (!ppv)
        return E_POINTER;
    return MMDevPropStore_Create(This, access, ppv);
}

static HRESULT WINAPI MMDevice_GetId(IMMDevice *iface, WCHAR **itemid)
{
    MMDevice *This = impl_from_IMMDevice(iface);
    WCHAR *str;
    GUID *id = &This->devguid;

    TRACE("(%p)->(%p)\n", This, itemid);
    if (!itemid)
        return E_POINTER;
    *itemid = str = CoTaskMemAlloc(56 * sizeof(WCHAR));
    if (!str)
        return E_OUTOFMEMORY;
    wsprintfW(str, devid_formatW,
              This->flow, id->Data1, id->Data2, id->Data3,
              id->Data4[0], id->Data4[1], id->Data4[2], id->Data4[3],
              id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);
    TRACE("returning %s\n", wine_dbgstr_w(str));
    return S_OK;
}

static HRESULT WINAPI MMDevice_GetState(IMMDevice *iface, DWORD *state)
{
    MMDevice *This = impl_from_IMMDevice(iface);
    TRACE("(%p)->(%p)\n", iface, state);

    if (!state)
        return E_POINTER;
    *state = This->state;
    return S_OK;
}

static const IMMDeviceVtbl MMDeviceVtbl =
{
    MMDevice_QueryInterface,
    MMDevice_AddRef,
    MMDevice_Release,
    MMDevice_Activate,
    MMDevice_OpenPropertyStore,
    MMDevice_GetId,
    MMDevice_GetState
};

static inline MMDevice *impl_from_IMMEndpoint(IMMEndpoint *iface)
{
    return CONTAINING_RECORD(iface, MMDevice, IMMEndpoint_iface);
}

static HRESULT WINAPI MMEndpoint_QueryInterface(IMMEndpoint *iface, REFIID riid, void **ppv)
{
    MMDevice *This = impl_from_IMMEndpoint(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);
    return IMMDevice_QueryInterface(&This->IMMDevice_iface, riid, ppv);
}

static ULONG WINAPI MMEndpoint_AddRef(IMMEndpoint *iface)
{
    MMDevice *This = impl_from_IMMEndpoint(iface);
    TRACE("(%p)\n", This);
    return IMMDevice_AddRef(&This->IMMDevice_iface);
}

static ULONG WINAPI MMEndpoint_Release(IMMEndpoint *iface)
{
    MMDevice *This = impl_from_IMMEndpoint(iface);
    TRACE("(%p)\n", This);
    return IMMDevice_Release(&This->IMMDevice_iface);
}

static HRESULT WINAPI MMEndpoint_GetDataFlow(IMMEndpoint *iface, EDataFlow *flow)
{
    MMDevice *This = impl_from_IMMEndpoint(iface);
    TRACE("(%p)->(%p)\n", This, flow);
    if (!flow)
        return E_POINTER;
    *flow = This->flow;
    return S_OK;
}

static const IMMEndpointVtbl MMEndpointVtbl =
{
    MMEndpoint_QueryInterface,
    MMEndpoint_AddRef,
    MMEndpoint_Release,
    MMEndpoint_GetDataFlow
};

static HRESULT MMDevCol_Create(IMMDeviceCollection **ppv, EDataFlow flow, DWORD state)
{
    MMDevice *default_device = flow == eRender ? MMDevice_def_play : NULL;
    MMDevColImpl *This;
    MMDevice *cur;
    UINT i = 0;

    This = malloc(sizeof(*This));
    *ppv = NULL;
    if (!This)
        return E_OUTOFMEMORY;
    This->IMMDeviceCollection_iface.lpVtbl = &MMDevColVtbl;
    This->ref = 1;
    This->devices = NULL;
    This->devices_count = 0;
    This->flow = flow;
    This->state = state;
    This->dualsense_mono = NULL;
    *ppv = &This->IMMDeviceCollection_iface;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(cur, &device_list, MMDevice, entry)
    {
        if (collection_device_visible(cur, flow, state))
            This->devices_count++;
    }

    if (This->devices_count)
    {
        This->devices = malloc(This->devices_count * sizeof(IMMDevice *));
        if (!This->devices_count) {
            LeaveCriticalSection(&device_list_cs);
            return E_OUTOFMEMORY;
        }

        LIST_FOR_EACH_ENTRY(cur, &device_list, MMDevice, entry)
        {
            if (!collection_device_visible(cur, flow, state) ||
                    !should_prioritize_dualsense_mono_endpoint(cur))
                continue;
            This->devices[i] = &cur->IMMDevice_iface;
            IMMDevice_AddRef(This->devices[i]);
            i++;
        }

        if (default_device && collection_device_visible(default_device, flow, state) &&
                !should_prioritize_dualsense_mono_endpoint(default_device))
        {
            This->devices[i] = &default_device->IMMDevice_iface;
            IMMDevice_AddRef(This->devices[i]);
            i++;
        }

        LIST_FOR_EACH_ENTRY(cur, &device_list, MMDevice, entry)
        {
            if (cur == default_device || !collection_device_visible(cur, flow, state) ||
                    should_prioritize_dualsense_mono_endpoint(cur))
                continue;
            This->devices[i] = &cur->IMMDevice_iface;
            IMMDevice_AddRef(This->devices[i]);
            i++;
        }
    }
    LeaveCriticalSection(&device_list_cs);

    create_update_thread();

    return S_OK;
}

static void MMDevCol_Destroy(MMDevColImpl *This)
{
    UINT i;
    for (i = 0; i < This->devices_count; i++)
        IMMDevice_Release(This->devices[i]);
    if (This->dualsense_mono)
        IMMDevice_Release(&This->dualsense_mono->IMMDevice_iface);

    free(This->devices);
    free(This);
}

static HRESULT WINAPI MMDevCol_QueryInterface(IMMDeviceCollection *iface, REFIID riid, void **ppv)
{
    MMDevColImpl *This = impl_from_IMMDeviceCollection(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_IMMDeviceCollection))
        *ppv = &This->IMMDeviceCollection_iface;
    else
        *ppv = NULL;
    if (!*ppv)
        return E_NOINTERFACE;
    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI MMDevCol_AddRef(IMMDeviceCollection *iface)
{
    MMDevColImpl *This = impl_from_IMMDeviceCollection(iface);
    LONG ref = InterlockedIncrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    return ref;
}

static ULONG WINAPI MMDevCol_Release(IMMDeviceCollection *iface)
{
    MMDevColImpl *This = impl_from_IMMDeviceCollection(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    if (!ref)
        MMDevCol_Destroy(This);
    return ref;
}

static MMDevice *MMDevCol_current_dualsense_mono_endpoint(MMDevColImpl *This)
{
    MMDevice *device, *found = NULL;

    if (This->flow != eRender)
        return NULL;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(device, &device_list, MMDevice, entry)
    {
        if (!collection_device_visible(device, This->flow, This->state) ||
                !should_prioritize_dualsense_mono_endpoint(device))
            continue;

        found = device;
        IMMDevice_AddRef(&found->IMMDevice_iface);
        break;
    }
    LeaveCriticalSection(&device_list_cs);

    return found;
}

static void MMDevCol_refresh_dualsense_mono_endpoint(MMDevColImpl *This)
{
    MMDevice *dualsense_mono = MMDevCol_current_dualsense_mono_endpoint(This);

    if (This->dualsense_mono)
        IMMDevice_Release(&This->dualsense_mono->IMMDevice_iface);
    This->dualsense_mono = dualsense_mono;
}

static HRESULT WINAPI MMDevCol_GetCount(IMMDeviceCollection *iface, UINT *numdevs)
{
    MMDevColImpl *This = impl_from_IMMDeviceCollection(iface);
    UINT i;

    TRACE("(%p)->(%p)\n", This, numdevs);
    if (!numdevs)
        return E_POINTER;

    *numdevs = 0;
    MMDevCol_refresh_dualsense_mono_endpoint(This);
    if (This->dualsense_mono)
        (*numdevs)++;

    for (i = 0; i < This->devices_count; ++i)
    {
        if (MMDevCol_device_visible(This->devices[i]) &&
                (!This->dualsense_mono ||
                impl_from_IMMDevice(This->devices[i]) != This->dualsense_mono))
            (*numdevs)++;
    }

    return S_OK;
}

static HRESULT WINAPI MMDevCol_Item(IMMDeviceCollection *iface, UINT n, IMMDevice **dev)
{
    MMDevColImpl *This = impl_from_IMMDeviceCollection(iface);
    UINT i, index = 0;

    TRACE("(%p)->(%u, %p)\n", This, n, dev);
    if (!dev)
        return E_POINTER;

    *dev = NULL;
    if (This->dualsense_mono && n == 0)
    {
        TRACE("Returning current Sony controller speaker %s through stale collection.\n",
                debugstr_guid(&This->dualsense_mono->devguid));
        *dev = &This->dualsense_mono->IMMDevice_iface;
        IMMDevice_AddRef(*dev);
        return S_OK;
    }
    if (This->dualsense_mono)
        index = 1;

    for (i = 0; i < This->devices_count; ++i)
    {
        if (!MMDevCol_device_visible(This->devices[i]))
            continue;
        if (This->dualsense_mono &&
                impl_from_IMMDevice(This->devices[i]) == This->dualsense_mono)
            continue;
        if (index++ == n)
        {
            *dev = This->devices[i];
            IMMDevice_AddRef(*dev);
            return S_OK;
        }
    }

    WARN("Could not obtain item %u\n", n);
    return E_INVALIDARG;
}

static const IMMDeviceCollectionVtbl MMDevColVtbl =
{
    MMDevCol_QueryInterface,
    MMDevCol_AddRef,
    MMDevCol_Release,
    MMDevCol_GetCount,
    MMDevCol_Item
};

HRESULT MMDevEnum_Create(REFIID riid, void **ppv)
{
    return IMMDeviceEnumerator_QueryInterface(&enumerator.IMMDeviceEnumerator_iface, riid, ppv);
}

void MMDevEnum_Free(void)
{
    MMDevice *device, *next;
    struct device *dev, *dev_next;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY_SAFE(device, next, &device_list, MMDevice, entry)
        MMDevice_Destroy(device);
    list_init(&device_list);
    LeaveCriticalSection(&device_list_cs);
    RegCloseKey(key_render);
    RegCloseKey(key_capture);
    LIST_FOR_EACH_ENTRY_SAFE(dev, dev_next, &devices_cache, struct device, entry)
        free( dev );
}

static HRESULT WINAPI MMDevEnum_QueryInterface(IMMDeviceEnumerator *iface, REFIID riid, void **ppv)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_IMMDeviceEnumerator))
        *ppv = &This->IMMDeviceEnumerator_iface;
    else
        *ppv = NULL;
    if (!*ppv)
        return E_NOINTERFACE;
    IUnknown_AddRef((IUnknown*)*ppv);

    return S_OK;
}

static ULONG WINAPI MMDevEnum_AddRef(IMMDeviceEnumerator *iface)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    LONG ref = InterlockedIncrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    return ref;
}

static ULONG WINAPI MMDevEnum_Release(IMMDeviceEnumerator *iface)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    return ref;
}

static HRESULT WINAPI MMDevEnum_EnumAudioEndpoints(IMMDeviceEnumerator *iface, EDataFlow flow, DWORD mask, IMMDeviceCollection **devices)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    TRACE("(%p)->(%u,%lu,%p)\n", This, flow, mask, devices);
    if (!devices)
        return E_POINTER;
    *devices = NULL;
    if (flow >= EDataFlow_enum_count)
        return E_INVALIDARG;
    if (mask & ~DEVICE_STATEMASK_ALL)
        return E_INVALIDARG;
    wait_for_render_update_if_needed(flow);
    return MMDevCol_Create(devices, flow, mask);
}

static HRESULT WINAPI MMDevEnum_GetDefaultAudioEndpoint(IMMDeviceEnumerator *iface, EDataFlow flow, ERole role, IMMDevice **device)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    WCHAR reg_key[256];
    HKEY key;
    HRESULT hr;

    TRACE("(%p)->(%u,%u,%p)\n", This, flow, role, device);

    if (!device)
        return E_POINTER;

    if((flow != eRender && flow != eCapture) ||
            (role != eConsole && role != eMultimedia && role != eCommunications)){
        WARN("Unknown flow (%u) or role (%u)\n", flow, role);
        return E_INVALIDARG;
    }

    *device = NULL;

    if(!drvs.module_name[0])
        return E_NOTFOUND;

    lstrcpyW(reg_key, drv_keyW);
    lstrcatW(reg_key, L"\\");
    lstrcatW(reg_key, drvs.module_name);

    if(RegOpenKeyW(HKEY_CURRENT_USER, reg_key, &key) == ERROR_SUCCESS){
        const WCHAR *reg_x_name, *reg_vx_name;
        WCHAR def_id[256];
        DWORD size = sizeof(def_id), state;

        if(flow == eRender){
            reg_x_name = L"DefaultOutput";
            reg_vx_name = L"DefaultVoiceOutput";
        }else{
            reg_x_name = L"DefaultInput";
            reg_vx_name = L"DefaultVoiceInput";
        }

        if(role == eCommunications &&
                RegQueryValueExW(key, reg_vx_name, 0, NULL,
                    (BYTE*)def_id, &size) == ERROR_SUCCESS){
            hr = IMMDeviceEnumerator_GetDevice(iface, def_id, device);
            if(SUCCEEDED(hr)){
                if(SUCCEEDED(IMMDevice_GetState(*device, &state)) &&
                        state == DEVICE_STATE_ACTIVE){
                    RegCloseKey(key);
                    return S_OK;
                }
            }

            TRACE("Unable to find voice device %s\n", wine_dbgstr_w(def_id));
        }

        if(RegQueryValueExW(key, reg_x_name, 0, NULL,
                    (BYTE*)def_id, &size) == ERROR_SUCCESS){
            hr = IMMDeviceEnumerator_GetDevice(iface, def_id, device);
            if(SUCCEEDED(hr)){
                if(SUCCEEDED(IMMDevice_GetState(*device, &state)) &&
                        state == DEVICE_STATE_ACTIVE){
                    RegCloseKey(key);
                    return S_OK;
                }
            }

            TRACE("Unable to find device %s\n", wine_dbgstr_w(def_id));
        }

        RegCloseKey(key);
    }

    EnterCriticalSection(&g_notif_lock);
    if (flow == eRender)
        *device = &MMDevice_def_play->IMMDevice_iface;
    else
        *device = &MMDevice_def_rec->IMMDevice_iface;
    LeaveCriticalSection(&g_notif_lock);

    if (!*device)
        return E_NOTFOUND;
    IMMDevice_AddRef(*device);
    return S_OK;
}

static HRESULT WINAPI MMDevEnum_GetDevice(IMMDeviceEnumerator *iface, const WCHAR *name, IMMDevice **device)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    MMDevice *impl;
    IMMDevice *dev = NULL;

    TRACE("(%p)->(%s,%p)\n", This, debugstr_w(name), device);

    if(!name || !device)
        return E_POINTER;

    EnterCriticalSection(&device_list_cs);
    LIST_FOR_EACH_ENTRY(impl, &device_list, MMDevice, entry)
    {
        HRESULT hr;
        WCHAR *str;

        if (is_dualsense_mono_endpoint(impl) && !impl->hide_from_collection
                && !(impl->state & DEVICE_STATE_ACTIVE))
        {
            TRACE("Skipping disconnected visible DualSense mono endpoint %s.\n", debugstr_guid(&impl->devguid));
            continue;
        }

        dev = &impl->IMMDevice_iface;
        hr = IMMDevice_GetId(dev, &str);
        if (FAILED(hr))
        {
            WARN("GetId failed: %08lx\n", hr);
            continue;
        }

        if (str && !lstrcmpiW(str, name))
        {
            LeaveCriticalSection(&device_list_cs);
            CoTaskMemFree(str);
            IMMDevice_AddRef(dev);
            *device = dev;
            return S_OK;
        }
        CoTaskMemFree(str);
    }
    LeaveCriticalSection(&device_list_cs);
    TRACE("Could not find device %s\n", debugstr_w(name));
    return E_INVALIDARG;
}

static HANDLE g_notif_thread;

static CRITICAL_SECTION_DEBUG g_notif_lock_debug =
{
    0, 0, &g_notif_lock,
    { &g_notif_lock_debug.ProcessLocksList, &g_notif_lock_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": g_notif_lock") }
};
static CRITICAL_SECTION g_notif_lock = { &g_notif_lock_debug, -1, 0, 0, 0, 0 };

static void notify_clients(EDataFlow flow, ERole role, const WCHAR *id)
{
    struct NotificationClientWrapper *wrapper;
    LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients,
            struct NotificationClientWrapper, entry)
        IMMNotificationClient_OnDefaultDeviceChanged(wrapper->client, flow,
                role, id);

    /* Windows 7 treats changes to eConsole as changes to eMultimedia */
    if(role == eConsole)
        notify_clients(flow, eMultimedia, id);
}

static BOOL notify_if_changed(EDataFlow flow, ERole role, HKEY key,
                              const WCHAR *val_name, WCHAR *old_val, IMMDevice *def_dev)
{
    WCHAR new_val[64], *id;
    DWORD size;
    HRESULT hr;

    size = sizeof(new_val);
    if(RegQueryValueExW(key, val_name, 0, NULL,
                (BYTE*)new_val, &size) != ERROR_SUCCESS){
        if(old_val[0] != 0){
            /* set by user -> system default */
            if(def_dev){
                hr = IMMDevice_GetId(def_dev, &id);
                if(FAILED(hr)){
                    ERR("GetId failed: %08lx\n", hr);
                    return FALSE;
                }
            }else
                id = NULL;

            notify_clients(flow, role, id);
            old_val[0] = 0;
            CoTaskMemFree(id);

            return TRUE;
        }

        /* system default -> system default, noop */
        return FALSE;
    }

    if(!lstrcmpW(old_val, new_val)){
        /* set by user -> same value */
        return FALSE;
    }

    if(new_val[0] != 0){
        /* set by user -> different value */
        notify_clients(flow, role, new_val);
        memcpy(old_val, new_val, sizeof(new_val));
        return TRUE;
    }

    /* set by user -> system default */
    if(def_dev){
        hr = IMMDevice_GetId(def_dev, &id);
        if(FAILED(hr)){
            ERR("GetId failed: %08lx\n", hr);
            return FALSE;
        }
    }else
        id = NULL;

    notify_clients(flow, role, id);
    old_val[0] = 0;
    CoTaskMemFree(id);

    return TRUE;
}

static DWORD WINAPI notif_thread_proc(void *user)
{
    HKEY key;
    WCHAR reg_key[256];
    WCHAR out_name[64], vout_name[64], in_name[64], vin_name[64];
    DWORD size;

    SetThreadDescription(GetCurrentThread(), L"wine_mmdevapi_notification");

    lstrcpyW(reg_key, drv_keyW);
    lstrcatW(reg_key, L"\\");
    lstrcatW(reg_key, drvs.module_name);

    if(RegCreateKeyExW(HKEY_CURRENT_USER, reg_key, 0, NULL, 0,
                MAXIMUM_ALLOWED, NULL, &key, NULL) != ERROR_SUCCESS){
        ERR("RegCreateKeyEx failed: %lu\n", GetLastError());
        return 1;
    }

    size = sizeof(out_name);
    if(RegQueryValueExW(key, L"DefaultOutput", 0, NULL, (BYTE*)out_name, &size) != ERROR_SUCCESS)
        out_name[0] = 0;

    size = sizeof(vout_name);
    if(RegQueryValueExW(key, L"DefaultVoiceOutput", 0, NULL, (BYTE*)vout_name, &size) != ERROR_SUCCESS)
        vout_name[0] = 0;

    size = sizeof(in_name);
    if(RegQueryValueExW(key, L"DefaultInput", 0, NULL, (BYTE*)in_name, &size) != ERROR_SUCCESS)
        in_name[0] = 0;

    size = sizeof(vin_name);
    if(RegQueryValueExW(key, L"DefaultVoiceInput", 0, NULL, (BYTE*)vin_name, &size) != ERROR_SUCCESS)
        vin_name[0] = 0;

    while(1){
        if(RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                    NULL, FALSE) != ERROR_SUCCESS){
            ERR("RegNotifyChangeKeyValue failed: %lu\n", GetLastError());
            RegCloseKey(key);
            g_notif_thread = NULL;
            return 1;
        }

        EnterCriticalSection(&g_notif_lock);

        notify_if_changed(eRender, eConsole, key, L"DefaultOutput",
                out_name, &MMDevice_def_play->IMMDevice_iface);
        notify_if_changed(eRender, eCommunications, key, L"DefaultVoiceOutput",
                vout_name, &MMDevice_def_play->IMMDevice_iface);
        notify_if_changed(eCapture, eConsole, key, L"DefaultInput",
                in_name, &MMDevice_def_rec->IMMDevice_iface);
        notify_if_changed(eCapture, eCommunications, key, L"DefaultVoiceInput",
                vin_name, &MMDevice_def_rec->IMMDevice_iface);

        LeaveCriticalSection(&g_notif_lock);
    }

    RegCloseKey(key);

    g_notif_thread = NULL;

    return 0;
}

static HRESULT WINAPI MMDevEnum_RegisterEndpointNotificationCallback(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    struct NotificationClientWrapper *wrapper;

    TRACE("(%p)->(%p)\n", This, client);

    if(!client)
        return E_POINTER;

    wrapper = malloc(sizeof(*wrapper));
    if(!wrapper)
        return E_OUTOFMEMORY;

    wrapper->client = client;

    EnterCriticalSection(&g_notif_lock);

    list_add_tail(&g_notif_clients, &wrapper->entry);

    if(!g_notif_thread){
        g_notif_thread = CreateThread(NULL, 0, notif_thread_proc, NULL, 0, NULL);
        if(!g_notif_thread)
            ERR("CreateThread failed: %lu\n", GetLastError());
    }

    LeaveCriticalSection(&g_notif_lock);

    /* Delay create thread as much as possible. */
    create_update_thread();

    return S_OK;
}

static HRESULT WINAPI MMDevEnum_UnregisterEndpointNotificationCallback(IMMDeviceEnumerator *iface, IMMNotificationClient *client)
{
    MMDevEnumImpl *This = impl_from_IMMDeviceEnumerator(iface);
    struct NotificationClientWrapper *wrapper;

    TRACE("(%p)->(%p)\n", This, client);

    if(!client)
        return E_POINTER;

    EnterCriticalSection(&g_notif_lock);

    LIST_FOR_EACH_ENTRY(wrapper, &g_notif_clients, struct NotificationClientWrapper, entry){
        if(wrapper->client == client){
            list_remove(&wrapper->entry);
            free(wrapper);
            LeaveCriticalSection(&g_notif_lock);
            return S_OK;
        }
    }

    LeaveCriticalSection(&g_notif_lock);

    return E_NOTFOUND;
}

static const IMMDeviceEnumeratorVtbl MMDevEnumVtbl =
{
    MMDevEnum_QueryInterface,
    MMDevEnum_AddRef,
    MMDevEnum_Release,
    MMDevEnum_EnumAudioEndpoints,
    MMDevEnum_GetDefaultAudioEndpoint,
    MMDevEnum_GetDevice,
    MMDevEnum_RegisterEndpointNotificationCallback,
    MMDevEnum_UnregisterEndpointNotificationCallback
};

static MMDevEnumImpl enumerator =
{
    {&MMDevEnumVtbl},
    1,
};

static HRESULT MMDevPropStore_Create(MMDevice *parent, DWORD access, IPropertyStore **ppv)
{
    MMDevPropStore *This;
    if (access != STGM_READ
        && access != STGM_WRITE
        && access != STGM_READWRITE)
    {
        WARN("Invalid access %08lx\n", access);
        return E_INVALIDARG;
    }
    This = malloc(sizeof(*This));
    *ppv = &This->IPropertyStore_iface;
    if (!This)
        return E_OUTOFMEMORY;
    This->IPropertyStore_iface.lpVtbl = &MMDevPropVtbl;
    This->ref = 1;
    This->parent = parent;
    This->access = access;
    return S_OK;
}

static void MMDevPropStore_Destroy(MMDevPropStore *This)
{
    free(This);
}

static HRESULT WINAPI MMDevPropStore_QueryInterface(IPropertyStore *iface, REFIID riid, void **ppv)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_IPropertyStore))
        *ppv = &This->IPropertyStore_iface;
    else
        *ppv = NULL;
    if (!*ppv)
        return E_NOINTERFACE;
    IUnknown_AddRef((IUnknown*)*ppv);
    return S_OK;
}

static ULONG WINAPI MMDevPropStore_AddRef(IPropertyStore *iface)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    LONG ref = InterlockedIncrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    return ref;
}

static ULONG WINAPI MMDevPropStore_Release(IPropertyStore *iface)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    LONG ref = InterlockedDecrement(&This->ref);
    TRACE("Refcount now %li\n", ref);
    if (!ref)
        MMDevPropStore_Destroy(This);
    return ref;
}

static HRESULT WINAPI MMDevPropStore_GetCount(IPropertyStore *iface, DWORD *nprops)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    WCHAR buffer[50];
    DWORD i = 0;
    HKEY propkey;
    HRESULT hr;

    TRACE("(%p)->(%p)\n", iface, nprops);
    if (!nprops)
        return E_POINTER;
    hr = MMDevPropStore_OpenPropKey(&This->parent->devguid, This->parent->flow, &propkey);
    if (FAILED(hr))
        return hr;
    *nprops = 0;
    do {
        DWORD len = ARRAY_SIZE(buffer);
        if (RegEnumValueW(propkey, i, buffer, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        i++;
    } while (1);
    RegCloseKey(propkey);
    TRACE("Returning %li\n", i);
    *nprops = i;
    return S_OK;
}

static HRESULT WINAPI MMDevPropStore_GetAt(IPropertyStore *iface, DWORD prop, PROPERTYKEY *key)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    WCHAR buffer[50];
    DWORD len = ARRAY_SIZE(buffer);
    HRESULT hr;
    HKEY propkey;

    TRACE("(%p)->(%lu,%p)\n", iface, prop, key);
    if (!key)
        return E_POINTER;

    hr = MMDevPropStore_OpenPropKey(&This->parent->devguid, This->parent->flow, &propkey);
    if (FAILED(hr))
        return hr;

    if (RegEnumValueW(propkey, prop, buffer, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS
        || len <= 39)
    {
        WARN("GetAt %lu failed\n", prop);
        return E_INVALIDARG;
    }
    RegCloseKey(propkey);
    buffer[38] = 0;
    CLSIDFromString(buffer, &key->fmtid);
    key->pid = wcstol(&buffer[39], NULL, 10);
    return S_OK;
}

static HRESULT WINAPI MMDevPropStore_GetValue(IPropertyStore *iface, REFPROPERTYKEY key, PROPVARIANT *pv)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    HRESULT hres;
    TRACE("(%p)->(\"%s,%lu\", %p)\n", This, key ? debugstr_guid(&key->fmtid) : NULL, key ? key->pid : 0, pv);

    if (!key || !pv)
        return E_POINTER;
    if (This->access != STGM_READ
        && This->access != STGM_READWRITE)
        return STG_E_ACCESSDENIED;

    /* Special case */
    if (IsEqualPropertyKey(*key, PKEY_AudioEndpoint_GUID))
    {
        pv->vt = VT_LPWSTR;
        pv->pwszVal = CoTaskMemAlloc(39 * sizeof(WCHAR));
        if (!pv->pwszVal)
            return E_OUTOFMEMORY;
        StringFromGUID2(&This->parent->devguid, pv->pwszVal, 39);
        _wcslwr(pv->pwszVal);
        return S_OK;
    }

    hres = MMDevice_GetPropValue(&This->parent->devguid, This->parent->flow, key, pv);
    if (FAILED(hres))
        return hres;

    if ((IsEqualPropertyKey(*key, DEVPKEY_Device_FriendlyName) ||
         IsEqualPropertyKey(*key, DEVPKEY_DeviceInterface_FriendlyName) ||
         IsEqualPropertyKey(*key, DEVPKEY_Device_DeviceDesc)) &&
        pv->vt == VT_LPWSTR && wcslen(pv->pwszVal) > 62)
    {
        WARN("Returned name exceeds length limit of some broken apps/libs, might crash: %s\n", debugstr_w(pv->pwszVal));
    }
    return hres;
}

static HRESULT WINAPI MMDevPropStore_SetValue(IPropertyStore *iface, REFPROPERTYKEY key, REFPROPVARIANT pv)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    TRACE("(%p)->(\"%s,%lu\", %p)\n", This, key ? debugstr_guid(&key->fmtid) : NULL, key ? key->pid : 0, pv);

    if (!key || !pv)
        return E_POINTER;

    if (This->access != STGM_WRITE
        && This->access != STGM_READWRITE)
        return STG_E_ACCESSDENIED;
    return MMDevice_SetPropValue(&This->parent->devguid, This->parent->flow, key, pv);
}

static HRESULT WINAPI MMDevPropStore_Commit(IPropertyStore *iface)
{
    MMDevPropStore *This = impl_from_IPropertyStore(iface);
    TRACE("(%p)\n", iface);

    if (This->access != STGM_WRITE
        && This->access != STGM_READWRITE)
        return STG_E_ACCESSDENIED;

    /* Does nothing - for mmdevapi, the propstore values are written on SetValue,
     * not on Commit. */

    return S_OK;
}

static const IPropertyStoreVtbl MMDevPropVtbl =
{
    MMDevPropStore_QueryInterface,
    MMDevPropStore_AddRef,
    MMDevPropStore_Release,
    MMDevPropStore_GetCount,
    MMDevPropStore_GetAt,
    MMDevPropStore_GetValue,
    MMDevPropStore_SetValue,
    MMDevPropStore_Commit
};


/* Property bag for IBaseFilter activation */
static HRESULT WINAPI PB_QueryInterface(IPropertyBag *iface, REFIID riid, void **ppv)
{
    ERR("Should not be called\n");
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI PB_AddRef(IPropertyBag *iface)
{
    ERR("Should not be called\n");
    return 2;
}

static ULONG WINAPI PB_Release(IPropertyBag *iface)
{
    ERR("Should not be called\n");
    return 1;
}

static HRESULT WINAPI PB_Read(IPropertyBag *iface, LPCOLESTR name, VARIANT *var, IErrorLog *log)
{
    IPropertyBagImpl *This = impl_from_IPropertyBag(iface);
    TRACE("Trying to read %s, type %u\n", debugstr_w(name), var->vt);
    if (!lstrcmpW(name, L"DSGuid"))
    {
        WCHAR guidstr[39];
        StringFromGUID2(&This->devguid, guidstr,ARRAY_SIZE(guidstr));
        var->vt = VT_BSTR;
        var->bstrVal = SysAllocString(guidstr);
        return S_OK;
    }
    ERR("Unknown property '%s' queried\n", debugstr_w(name));
    return E_FAIL;
}

static HRESULT WINAPI PB_Write(IPropertyBag *iface, LPCOLESTR name, VARIANT *var)
{
    ERR("Should not be called\n");
    return E_FAIL;
}

static const IPropertyBagVtbl PB_Vtbl =
{
    PB_QueryInterface,
    PB_AddRef,
    PB_Release,
    PB_Read,
    PB_Write
};

static HRESULT WINAPI Connector_QueryInterface(IConnector *iface, REFIID riid, void **ppv)
{
    IConnectorImpl *This = impl_from_IConnector(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IConnector))
        *ppv = &This->IConnector_iface;
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI Connector_AddRef(IConnector *iface)
{
    IConnectorImpl *This = impl_from_IConnector(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI Connector_Release(IConnector *iface)
{
    IConnectorImpl *This = impl_from_IConnector(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);

    if (!ref)
        HeapFree(GetProcessHeap(), 0, This);

    return ref;
}

static HRESULT WINAPI Connector_GetType(
    IConnector *This,
    ConnectorType *pType)
{
    FIXME("(%p) - partial stub\n", This);
    *pType = Physical_Internal;
    return S_OK;
}

static HRESULT WINAPI Connector_GetDataFlow(
    IConnector *This,
    DataFlow *pFlow)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Connector_ConnectTo(
    IConnector *This,
    IConnector *pConnectTo)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Connector_Disconnect(
    IConnector *This)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Connector_IsConnected(
    IConnector *This,
    BOOL *pbConnected)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Connector_GetConnectedTo(
    IConnector *This,
    IConnector **ppConTo)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Connector_GetConnectorIdConnectedTo(
    IConnector *This,
    LPWSTR *ppwstrConnectorId)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static HRESULT WINAPI Connector_GetDeviceIdConnectedTo(
    IConnector *This,
    LPWSTR *ppwstrDeviceId)
{
    FIXME("(%p) - stub\n", This);
    return E_NOTIMPL;
}

static const IConnectorVtbl Connector_Vtbl =
{
    Connector_QueryInterface,
    Connector_AddRef,
    Connector_Release,
    Connector_GetType,
    Connector_GetDataFlow,
    Connector_ConnectTo,
    Connector_Disconnect,
    Connector_IsConnected,
    Connector_GetConnectedTo,
    Connector_GetConnectorIdConnectedTo,
    Connector_GetDeviceIdConnectedTo,
};

HRESULT Connector_Create(IConnector **ppv)
{
    IConnectorImpl *This;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IConnector_iface.lpVtbl = &Connector_Vtbl;
    This->ref = 1;

    *ppv = &This->IConnector_iface;

    return S_OK;
}

static HRESULT WINAPI DT_QueryInterface(IDeviceTopology *iface, REFIID riid, void **ppv)
{
    IDeviceTopologyImpl *This = impl_from_IDeviceTopology(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IDeviceTopology))
        *ppv = &This->IDeviceTopology_iface;
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI DT_AddRef(IDeviceTopology *iface)
{
    IDeviceTopologyImpl *This = impl_from_IDeviceTopology(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI DT_Release(IDeviceTopology *iface)
{
    IDeviceTopologyImpl *This = impl_from_IDeviceTopology(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);

    if (!ref)
        HeapFree(GetProcessHeap(), 0, This);

    return ref;
}

static HRESULT WINAPI DT_GetConnectorCount(IDeviceTopology *This,
                                           UINT *pCount)
{
    FIXME("(%p)->(%p) - partial stub\n", This, pCount);

    if (!pCount)
        return E_POINTER;

    *pCount = 1;
    return S_OK;
}

static HRESULT WINAPI DT_GetConnector(IDeviceTopology *This,
                                      UINT nIndex,
                                      IConnector **ppConnector)
{
    FIXME("(%p)->(%u, %p) - partial stub\n", This, nIndex, ppConnector);

    if (nIndex == 0)
    {
        return Connector_Create(ppConnector);
    }

    return E_INVALIDARG;
}

static HRESULT WINAPI DT_GetSubunitCount(IDeviceTopology *This,
                                         UINT *pCount)
{
    FIXME("(%p)->(%p) - stub\n", This, pCount);
    return E_NOTIMPL;
}

static HRESULT WINAPI DT_GetSubunit(IDeviceTopology *This,
                                    UINT nIndex,
                                    ISubUnit **ppConnector)
{
    FIXME("(%p)->(%u, %p) - stub\n", This, nIndex, ppConnector);
    return E_NOTIMPL;
}

static HRESULT WINAPI DT_GetPartById(IDeviceTopology *This,
                                     UINT nId,
                                     IPart **ppPart)
{
    FIXME("(%p)->(%u, %p) - stub\n", This, nId, ppPart);
    return E_NOTIMPL;
}

static HRESULT WINAPI DT_GetDeviceId(IDeviceTopology *This,
                                     LPWSTR *ppwstrDeviceId)
{
    FIXME("(%p)->(%p) - stub\n", This, ppwstrDeviceId);
    return E_NOTIMPL;
}

static HRESULT WINAPI DT_GetSignalPath(IDeviceTopology *This,
                                       IPart *pIPartFrom,
                                       IPart *pIPartTo,
                                       BOOL bRejectMixedPaths,
                                       IPartsList **ppParts)
{
    FIXME("(%p)->(%p, %p, %s, %p) - stub\n",
          This, pIPartFrom, pIPartTo, bRejectMixedPaths ? "TRUE" : "FALSE", ppParts);
    return E_NOTIMPL;
}

static const IDeviceTopologyVtbl DeviceTopology_Vtbl =
{
    DT_QueryInterface,
    DT_AddRef,
    DT_Release,
    DT_GetConnectorCount,
    DT_GetConnector,
    DT_GetSubunitCount,
    DT_GetSubunit,
    DT_GetPartById,
    DT_GetDeviceId,
    DT_GetSignalPath,
};

static HRESULT DeviceTopology_Create(IMMDevice *device, IDeviceTopology **ppv)
{
    IDeviceTopologyImpl *This;

    This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IDeviceTopology_iface.lpVtbl = &DeviceTopology_Vtbl;
    This->ref = 1;

    *ppv = &This->IDeviceTopology_iface;

    return S_OK;
}
