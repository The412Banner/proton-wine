/*
 * Vulkan display driver loading
 *
 * Copyright (c) 2017 Roderick Colenbrander
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

#include <limits.h>
#include <math.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#ifdef __linux__
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#endif
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/hwnd_dmabuf.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;

static void *vulkan_handle;
static struct vulkan_funcs vulkan_funcs;

WINE_DECLARE_DEBUG_CHANNEL(fps);

static const struct vulkan_driver_funcs *driver_funcs;
static int fshack_enabled = -1;

static void vulkan_driver_load(void);

static const UINT EXTERNAL_MEMORY_WIN32_BITS = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT |
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
static const UINT EXTERNAL_MEMORY_FD_BITS = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT |
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
static const UINT EXTERNAL_SEMAPHORE_WIN32_BITS = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                  VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
static const UINT EXTERNAL_FENCE_WIN32_BITS = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT |
                                              VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;

/* Query the compositor's importable dmabuf caps. The NtUserGetHwndDmabufCaps shim. */
static UINT hwnd_dmabuf_get_caps( HWND hwnd, void *caps, void *format_modifiers,
                                  UINT max_format_modifiers, UINT *format_modifier_count )
{
    vulkan_driver_load();
    if (!driver_funcs->p_vulkan_get_hwnd_dmabuf_caps)
    {
        if (format_modifier_count) *format_modifier_count = 0;
        return HWND_DMABUF_NOT_FOUND;
    }
    return driver_funcs->p_vulkan_get_hwnd_dmabuf_caps( hwnd, caps, format_modifiers,
                                                        max_format_modifiers, format_modifier_count );
}

UINT WINAPI NtUserHwndDmaBufGetCaps( HWND hwnd, void *caps, void *format_modifiers,
                                     UINT max_format_modifiers, UINT *format_modifier_count )
{
    return hwnd_dmabuf_get_caps( hwnd, caps, format_modifiers,
                                 max_format_modifiers, format_modifier_count );
}

#define ROUND_SIZE(size, mask) ((((SIZE_T)(size) + (mask)) & ~(SIZE_T)(mask)))

static BOOL use_external_memory(void)
{
    return zero_bits != 0;
}

struct mempool
{
    struct mempool *next;
    size_t mem_used;
    char mem[2048];
};

static void mem_free( struct mempool *pool )
{
    struct mempool *iter, *next = pool->next;
    while ((iter = next)) { next = iter->next; free( iter ); }
    pool->mem_used = 0;
    pool->next = NULL;
}

static void *mem_alloc( struct mempool *pool, size_t size )
{
    struct mempool *next = pool;
    if (pool->mem_used + size > sizeof(pool->mem)) next = pool->next;
    if (next && next->mem_used <= sizeof(next->mem) && next->mem_used + size <= sizeof(next->mem))
    {
        void *ret = next->mem + next->mem_used;
        next->mem_used += ROUND_SIZE( size, sizeof(UINT64) - 1 );
        return ret;
    }
    if (!(next = malloc( max( sizeof(*next), offsetof(struct mempool, mem[size]) ) ))) return NULL;
    next->next = pool->next;
    next->mem_used = size;
    pool->next = next;
    return next->mem;
}

struct instance
{
    struct vulkan_instance obj;
    BOOL enable_win32_surface;

    struct list utils_messengers;
    struct list report_callbacks;

    struct rb_tree objects;
    pthread_rwlock_t objects_lock;
};

static struct instance *instance_from_handle( VkInstance handle )
{
    struct vulkan_instance *object = vulkan_instance_from_handle( handle );
    return CONTAINING_RECORD( object, struct instance, obj );
}

struct device_memory
{
    struct vulkan_device_memory obj;
    VkDeviceSize size;
    void *vm_map;

    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;

    D3DKMT_HANDLE sync;
    D3DKMT_HANDLE mutex;
    VkSemaphore semaphore;
    UINT64 semaphore_value;
};

static inline struct device_memory *device_memory_from_handle( VkDeviceMemory handle )
{
    struct vulkan_device_memory *obj = vulkan_device_memory_from_handle( handle );
    return CONTAINING_RECORD( obj, struct device_memory, obj );
}

struct surface
{
    struct vulkan_surface obj;
    struct client_surface *client;
    struct swapchain *swapchain;
    HWND hwnd;
};

static struct surface *surface_from_handle( VkSurfaceKHR handle )
{
    struct vulkan_surface *obj = vulkan_surface_from_handle( handle );
    return CONTAINING_RECORD( obj, struct surface, obj );
}

/* Return whether integer scaling is on */
static BOOL fs_hack_is_integer(void)
{
    static int is_int = -1;
    if (is_int < 0)
    {
        const char *e = getenv( "WINE_FULLSCREEN_INTEGER_SCALING" );
        is_int = e && strcmp( e, "0" );
        TRACE( "is_integer_scaling: %s\n", is_int ? "TRUE" : "FALSE" );
    }
    return is_int;
}

struct fs_hack_image
{
    uint32_t cmd_queue_idx;
    VkCommandBuffer cmd;
    VkImage swapchain_image;
    VkImage user_image;
    VkSemaphore blit_finished;
    VkImageView user_view, blit_view;
    VkDescriptorSet descriptor_set;
};

static const char *debugstr_vkextent2d( const VkExtent2D *ext )
{
    if (!ext) return "(null)";
    return wine_dbg_sprintf( "(%d,%d)", (int)ext->width, (int)ext->height );
}

/* Cross-process Vulkan-WSI producer.
 *
 * A windowed swapchain whose toplevel has no on-screen wl_surface is a
 * cross-process child and advertises HWND dmabuf caps. For those we skip the
 * host vkCreateSwapchainKHR and build a managed swapchain of exportable
 * DRM-modifier images. Each presented frame is exported as a dmabuf and
 * published through the per-HWND bridge. On-screen windows get no caps and use
 * the host swapchain path. Works with stock DXVK, native Vulkan and vkd3d. */

#define WINE_VK_DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull
#define WINE_VK_MANAGED_MAX_IMAGES     8
#define WINE_VK_MANAGED_MAX_MODIFIERS  64
#define WINE_VK_MANAGED_REANNOUNCE_MIN_MS  100
#define WINE_VK_MANAGED_REANNOUNCE_MAX_MS  4000
/* Allow brief pauses beyond the consumer's dmabuf grace period. */
#define WINE_VK_MANAGED_STALL_MS       2000
#define WINE_VK_ACQUIRE_WAIT_SLICE_NS  (100 * 1000000ull)

/* Serializes the producer's cross-thread queue submits and device idles. */
static pthread_mutex_t producer_device_lock = PTHREAD_MUTEX_INITIALIZER;
static LONGLONG managed_next_producer_id;

enum wine_managed_consumer_state
{
    WINE_MANAGED_CONSUMER_UNKNOWN,
    WINE_MANAGED_CONSUMER_ACTIVE,
    WINE_MANAGED_CONSUMER_SUSPENDED,
};

enum wine_managed_image_layout
{
    WINE_MANAGED_IMAGE_LAYOUT_MODIFIER,
    WINE_MANAGED_IMAGE_LAYOUT_LINEAR,
};

/* The server reuses one channel while swapchains overlap during recreation. */
struct wine_managed_consumer
{
    struct list entry;
    HWND hwnd;
    unsigned int refcount;
    LONG state;
    LONG reannounce_pending;
    LONG last_reannounce_ms;
    LONG reannounce_delay_ms;
};

static pthread_mutex_t managed_consumers_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list managed_consumers = LIST_INIT( managed_consumers );

struct wine_managed_image
{
    VkImage image;                  /* raw host VkImage handle (no client wrapper) */
    VkDeviceMemory memory;          /* dedicated host VkDeviceMemory */
    int dmabuf_fd;                  /* cached exported dmabuf fd, dup()'d on publish */
    hwnd_dmabuf_frame_desc_t desc;  /* cached per-image frame descriptor */
    UINT64 release_token;           /* token handed to the compositor for this frame */
    BOOL acquired;                  /* handed to the app, not yet presented */
    BOOL busy;                      /* published, compositor may still read it */
    BOOL valid;                     /* image+memory+fd are all live */
    BOOL consumer_cached;           /* consumer explicitly acked this slot's dmabuf cache */
};

struct wine_managed_swapchain
{
    struct wine_managed_image images[WINE_VK_MANAGED_MAX_IMAGES];
    uint32_t image_count;

    uint64_t realized_modifier;
    unsigned int fourcc;
    unsigned int alpha_mode;        /* DXGI_ALPHA_MODE_* hint for the compositor */
    VkFormat format;
    VkExtent2D extents;
    VkImageUsageFlags usage;
    enum wine_managed_image_layout layout;
    BOOL discard;                   /* retire frames without an external consumer */

    /* ring / publish state */
    uint32_t next_acquire;
    UINT64 next_release_token;
    UINT64 present_id;              /* monotonic frame_seq counter */
    UINT64 producer_unique_id;      /* unique across managed swapchain recreates */
    unsigned int ring_generation;
    DWORD ring_full_since_ms;

    VkQueue signal_queue;           /* host queue used for empty acquire-signal submits */
    int channel_fd;                 /* producer end of the per-hwnd socket or -1 */
    pthread_mutex_t lock;
    BOOL lost;                      /* consumer channel died, force swapchain recreate */
    struct wine_managed_consumer *consumer;
    VkFence present_fence;          /* per-frame render-complete fence (export gate) */
    PFN_vkWaitForFences p_vkWaitForFences;
    PFN_vkResetFences p_vkResetFences;
    HWND hwnd;                      /* server-visible producer HWND */
    BOOL pending_registered;        /* server pending reference is live */
    BOOL channel_registered;        /* server active producer reference is live */
};

struct swapchain
{
    struct vulkan_swapchain obj;
    struct surface *surface;
    LONG presentation_generation;
    VkExtent2D extents;
    struct wine_managed_swapchain *managed; /* non-NULL => Wine owns the swapchain images */
    BOOL client_active;             /* client-surface activation was committed */
    BOOL native_presenter;          /* swapchain presents through host WSI */

    /* fs hack data below */
    UINT fshack_dpi;
    VkExtent2D host_extents;
    VkCommandPool *cmd_pools; /* VkCommandPool[device->queue_count] */
    VkDeviceMemory user_image_memory;
    uint32_t n_images;
    struct fs_hack_image *fs_hack_images; /* struct fs_hack_image[n_images] */
    VkSampler sampler;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
};

static struct swapchain *swapchain_from_handle( VkSwapchainKHR handle )
{
    struct vulkan_swapchain *obj = vulkan_swapchain_from_handle( handle );
    return CONTAINING_RECORD( obj, struct swapchain, obj );
}

static BOOL swapchain_is_out_of_date( const struct swapchain *swapchain )
{
    if (!swapchain || !swapchain->surface) return FALSE;
    return swapchain->presentation_generation !=
           ReadAcquire( &swapchain->surface->client->presentation_generation );
}

struct d3d12_fence_timeline
{
    pthread_mutex_t mutex;
    UINT64 virtual_value, physical_value;
    uint64_t last_reset_physical;
    uint64_t last_dropped_reset_physical;
    struct
    {
        uint64_t physical_at_reset;
        uint64_t virtual_before_reset;
    }
    reset_backlog[16];
    uint32_t reset_backlog_count;
    volatile LONG ref;
};

static void d3d12_fence_timeline_addref( struct d3d12_fence_timeline *t )
{
    if (t) InterlockedIncrement( &t->ref );
}

static void d3d12_fence_timeline_release( struct d3d12_fence_timeline *t )
{
    if (t) TRACE("t %p, ref %d.\n", t, t->ref);
    if (t && !InterlockedDecrement( &t->ref ))
    {
        pthread_mutex_destroy( &t->mutex );
        free( t );
    }
}

struct pending_d3d12_fence_op
{
    /* Vulkan native local semaphore. */
    struct local_timeline_semaphore local_sem;

    /* Operation values. */
    struct list entry;
    uint64_t virtual_value;
    uint64_t shared_physical_value;
    struct semaphore *semaphore;
};

struct d3d12_fence_info
{
    uint32_t type;
    uint32_t pid;
    union
    {
        struct d3d12_fence_timeline *t;
        uint64_t pad;
    };
};

struct semaphore
{
    struct vulkan_semaphore obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
    struct d3d12_fence_info info;
    /* Used for d3d12 fence. */
    VkSemaphore shared_sem;
    struct list poll_entry;
    struct list pending_waits;
    struct list pending_signals;
};

static struct semaphore *semaphore_from_handle( VkSemaphore handle )
{
    struct vulkan_semaphore *obj = vulkan_semaphore_from_handle( handle );
    return CONTAINING_RECORD( obj, struct semaphore, obj );
}

struct fence
{
    struct vulkan_fence obj;
    D3DKMT_HANDLE local;
    D3DKMT_HANDLE global;
    HANDLE shared;
};

static struct fence *fence_from_handle( VkFence handle )
{
    struct vulkan_fence *obj = vulkan_fence_from_handle( handle );
    return CONTAINING_RECORD( obj, struct fence, obj );
}

static VkResult allocate_external_host_memory( struct vulkan_device *device, VkMemoryAllocateInfo *alloc_info, uint32_t mem_flags,
                                               VkImportMemoryHostPointerInfoEXT *import_info )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkMemoryHostPointerPropertiesEXT props =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    uint32_t i, align = physical_device->external_memory_align - 1;
    SIZE_T alloc_size = alloc_info->allocationSize;
    static int once;
    void *mapping = NULL;
    VkResult res;

    if (!once++) FIXME( "Using VK_EXT_external_memory_host\n" );

    if (NtAllocateVirtualMemory( GetCurrentProcess(), &mapping, zero_bits, &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
    {
        ERR( "NtAllocateVirtualMemory failed\n" );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    if ((res = device->p_vkGetMemoryHostPointerPropertiesEXT( device->host.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                              mapping, &props )))
    {
        ERR( "vkGetMemoryHostPointerPropertiesEXT failed: %d\n", res );
        return res;
    }

    if (!(props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex)))
    {
        /* If requested memory type is not allowed to use external memory, try to find a supported compatible type. */
        uint32_t mask = mem_flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
        {
            if (!(props.memoryTypeBits & (1u << i))) continue;
            if ((physical_device->memory_properties.memoryTypes[i].propertyFlags & mask) != mask) continue;

            TRACE( "Memory type not compatible with host memory, using %u instead\n", i );
            alloc_info->memoryTypeIndex = i;
            break;
        }
        if (i == physical_device->memory_properties.memoryTypeCount)
        {
            FIXME( "Not found compatible memory type\n" );
            alloc_size = 0;
            NtFreeVirtualMemory( GetCurrentProcess(), &mapping, &alloc_size, MEM_RELEASE );
        }
    }

    if (props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex))
    {
        import_info->sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        import_info->handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import_info->pHostPointer = mapping;
        import_info->pNext = alloc_info->pNext;
        alloc_info->pNext = import_info;
        alloc_info->allocationSize = (alloc_info->allocationSize + align) & ~align;
    }

    return VK_SUCCESS;
}

static VkExternalMemoryHandleTypeFlagBits get_host_external_memory_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_memory_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_fd) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (extensions.has_VK_EXT_external_memory_dma_buf) return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    return 0;
}

static UINT map_external_memory_handle_types( UINT handle_types )
{
    UINT host_handle_types = handle_types & EXTERNAL_MEMORY_FD_BITS;

    if (handle_types & EXTERNAL_MEMORY_WIN32_BITS)
        host_handle_types |= get_host_external_memory_type();
    if (handle_types & ~(EXTERNAL_MEMORY_WIN32_BITS | EXTERNAL_MEMORY_FD_BITS))
        FIXME( "Unsupported handle types %#x\n", handle_types );
    return host_handle_types;
}

static VkExternalSemaphoreHandleTypeFlagBits get_host_external_semaphore_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_semaphore_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_semaphore_fd) return VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static VkExternalFenceHandleTypeFlagBits get_host_external_fence_type(void)
{
    struct vulkan_device_extensions extensions = {.has_VK_KHR_external_fence_win32 = 1};
    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_fence_fd) return VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    return 0;
}

static void init_shared_resource_path( const WCHAR *name, UNICODE_STRING *str )
{
    UINT len = wcslen( name );
    char buffer[MAX_PATH];

    snprintf( buffer, ARRAY_SIZE(buffer), "\\Sessions\\%u\\BaseNamedObjects\\",
              NtCurrentTeb()->Peb->SessionId );
    str->MaximumLength = asciiz_to_unicode( str->Buffer, buffer );
    str->Length = str->MaximumLength - sizeof(WCHAR);

    memcpy( str->Buffer + str->Length / sizeof(WCHAR), name, (len + 1) * sizeof(WCHAR) );
    str->MaximumLength += len * sizeof(WCHAR);
    str->Length += len * sizeof(WCHAR);
}

static HANDLE create_shared_resource_handle( D3DKMT_HANDLE local, const VkExportMemoryWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_resource_from_name( const WCHAR *name )
{
    D3DKMT_OPENNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

/* shared d3d12 fences helpers. */
static void d3d12_semaphore_lock( struct semaphore *semaphore )
{
    if (semaphore->info.t) pthread_mutex_lock( &semaphore->info.t->mutex );
}

static void d3d12_semaphore_unlock( struct semaphore *semaphore )
{
    if (semaphore->info.t) pthread_mutex_unlock( &semaphore->info.t->mutex );
}

static void signal_timeline_sem( struct vulkan_device *device, VkSemaphore sem, UINT64 *value )
{
    /* May be called from native thread. */
    struct VkSemaphoreSignalInfo info = { 0 };
    VkResult res;

    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = sem;
    info.value = *value + 1;
    __atomic_store_n(value, info.value, __ATOMIC_RELEASE);
    if (device->physical_device->api_version < VK_API_VERSION_1_2 || device->physical_device->instance->api_version < VK_API_VERSION_1_2)
        res = device->p_vkSignalSemaphoreKHR( device->host.device, &info );
    else
        res = device->p_vkSignalSemaphore( device->host.device, &info );
    if (res != VK_SUCCESS)
        fprintf( stderr, "err:winevulkan:signal_timeline_sem vkSignalSemaphore failed, res=%d.\n", res );
}

static VkResult wait_host_semaphores( struct vulkan_device *device, const VkSemaphoreWaitInfo *wait_info, uint64_t timeout )
{
    if (device->physical_device->api_version < VK_API_VERSION_1_2 || device->physical_device->instance->api_version < VK_API_VERSION_1_2)
        return device->p_vkWaitSemaphoresKHR( device->host.device, wait_info, timeout );
    return device->p_vkWaitSemaphores( device->host.device, wait_info, timeout );
}

static VkResult get_semaphore_value( struct vulkan_device *device, VkSemaphore sem, uint64_t *value )
{
    if (device->physical_device->api_version < VK_API_VERSION_1_2 || device->physical_device->instance->api_version < VK_API_VERSION_1_2)
        return device->p_vkGetSemaphoreCounterValueKHR( device->host.device, sem, value );
    return device->p_vkGetSemaphoreCounterValue( device->host.device, sem, value );
}

static VkSemaphore create_timeline_semaphore(struct vulkan_device *device)
{
    VkSemaphoreTypeCreateInfo timeline_info = { 0 };
    VkSemaphoreCreateInfo create_info = { 0 };
    VkSemaphore sem = 0;
    VkResult res;

    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create_info.pNext = &timeline_info;

    res = device->p_vkCreateSemaphore( device->host.device, &create_info, NULL, &sem );
    if (res != VK_SUCCESS)
        ERR( "vkCreateSemaphore failed, res=%d\n", res );
    return sem;
}

static void release_fence_op( struct vulkan_device *device, struct pending_d3d12_fence_op *op )
{
    struct vulkan_instance *instance = device->physical_device->instance;

    list_remove( &op->entry );
    instance->p_remove_object( instance, &op->semaphore->obj.obj );
    vulkan_object_init( &op->semaphore->obj.obj, op->semaphore->shared_sem );
    instance->p_insert_object( instance, &op->semaphore->obj.obj );
    op->semaphore = NULL;
    list_add_head( &device->free_fence_ops_list, &op->entry );
}

static int wait_info_realloc( VkSemaphoreWaitInfo *wait_info, uint32_t *wait_alloc_count )
{
    VkSemaphore *new_sem;
    uint64_t *new_values;

    if (wait_info->semaphoreCount + 1 <= *wait_alloc_count)
        return 1;
    new_sem = realloc( (void *)wait_info->pSemaphores, *wait_alloc_count * 2 * sizeof(*new_sem) );
    if (!new_sem)
    {
        fprintf( stderr, "err:winevulkan:wait_info_realloc no memory.\n" );
        return 0;
    }
    new_values = realloc( (void *)wait_info->pValues, *wait_alloc_count * 2 * sizeof(*new_values) );
    if (!new_values)
    {
        fprintf( stderr, "err:winevulkan:wait_info_realloc no memory.\n" );
        return 0;
    }
    *wait_alloc_count *= 2;
    wait_info->pSemaphores = new_sem;
    wait_info->pValues = new_values;
    return 1;
}

static int add_sem_wait( VkSemaphoreWaitInfo *wait_info, uint32_t *wait_alloc_count, VkSemaphore sem, uint64_t value )
{
    if (!wait_info_realloc( wait_info, wait_alloc_count ))
        return 0;
    ((VkSemaphore *)wait_info->pSemaphores)[wait_info->semaphoreCount] = sem;
    ((uint64_t *)wait_info->pValues)[wait_info->semaphoreCount] = value;
    ++wait_info->semaphoreCount;
    return 1;
}

static int debug_level;

static int semaphore_process( struct vulkan_device *device, struct semaphore *sem,
                              VkSemaphoreWaitInfo *wait_info, uint32_t *wait_alloc_count )
{
    /* Called from native thread. */
    struct pending_d3d12_fence_op *op, *op2;
    uint64_t global_sem_wait_value;
    int virtual_value_updated = 0;
    uint64_t value, virtual_value;
    VkResult res;
    uint32_t i;

    /* Check local pending signal ops completion, update shared semaphore. */
    d3d12_semaphore_lock( sem );
    virtual_value = sem->info.t->virtual_value;
    LIST_FOR_EACH_ENTRY_SAFE(op, op2, &sem->pending_signals, struct pending_d3d12_fence_op, entry)
    {
        res = get_semaphore_value( device, op->local_sem.sem, &value );
        if (res != VK_SUCCESS)
        {
            fprintf( stderr, "err:winevulkan:semaphore_process vkGetSemaphoreCounterValue failed, res=%d.\n", res );
            goto signal_op_complete;
        }
        if (value <= op->local_sem.value)
        {
            if (!add_sem_wait( wait_info, wait_alloc_count, op->local_sem.sem, op->local_sem.value + 1 ))
            {
                d3d12_semaphore_unlock( sem );
                return 0;
            }
            continue;
        }

        virtual_value = max( sem->info.t->virtual_value, op->virtual_value );
        sem->info.t->virtual_value = op->virtual_value;
        virtual_value_updated = 1;
signal_op_complete:
        op->local_sem.value = value;
        release_fence_op(device, op);
    }

    if (sem->info.t->virtual_value < virtual_value)
    {
        uint32_t idx = sem->info.t->reset_backlog_count;

        if (debug_level >= 3)
            fprintf(stderr, "warn:winevulkan:semaphore_process resetting semaphore %p virtual value.\n", sem);
        if (idx == ARRAY_SIZE(sem->info.t->reset_backlog))
        {
            sem->info.t->last_dropped_reset_physical = sem->info.t->reset_backlog[0].physical_at_reset;
            --idx;
            memmove( &sem->info.t->reset_backlog[0], &sem->info.t->reset_backlog[1],
                     sizeof(*sem->info.t->reset_backlog) * (sem->info.t->reset_backlog_count - 1) );
        }
        else
        {
            ++sem->info.t->reset_backlog_count;
        }
        sem->info.t->last_reset_physical = sem->info.t->physical_value + 1;
        sem->info.t->reset_backlog[idx].physical_at_reset = sem->info.t->last_reset_physical;
        sem->info.t->reset_backlog[idx].virtual_before_reset = virtual_value;
    }
    if (virtual_value_updated)
        signal_timeline_sem( device, sem->shared_sem, &sem->info.t->physical_value );
    global_sem_wait_value = sem->info.t->physical_value + 1;

    /* Complete satisfied local waits. */
    LIST_FOR_EACH_ENTRY_SAFE(op, op2, &sem->pending_waits, struct pending_d3d12_fence_op, entry)
    {
        if (op->virtual_value > virtual_value)
        {
            if (op->shared_physical_value > sem->info.t->last_reset_physical)
                continue;
            for (i = 0; i < sem->info.t->reset_backlog_count; ++i)
            {
                if (sem->info.t->reset_backlog[i].physical_at_reset >= op->shared_physical_value
                    && sem->info.t->reset_backlog[i].virtual_before_reset >= op->virtual_value)
                    break;
            }
            if (i == sem->info.t->reset_backlog_count)
            {
                if (sem->info.t->last_dropped_reset_physical < op->shared_physical_value)
                    continue;
                fprintf( stderr, "err:winevulkan:semaphore_process wait needs reset backlog beyond cut off.\n" );
            }
        }

        signal_timeline_sem( device, op->local_sem.sem, &op->local_sem.value );
        release_fence_op( device, op );
    }
    d3d12_semaphore_unlock(sem);

    if (list_empty( &sem->pending_waits ))
        return 1;
    return add_sem_wait( wait_info, wait_alloc_count, sem->shared_sem, global_sem_wait_value );
}

#define SIGNALLER_INITIAL_WAIT_COUNT 256

static void *signaller_worker( void *arg )
{
    int unix_tid = gettid();
    struct vulkan_device *device = arg;
    struct semaphore *sem;
    VkSemaphoreWaitInfo wait_info = { 0 };
    uint32_t wait_alloc_count = 0;
    VkResult res;

    if (debug_level)
        fprintf( stderr, "[%d] msg:winevulkan:signaller_worker started.\n", unix_tid );

    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.flags = VK_SEMAPHORE_WAIT_ANY_BIT;
    wait_alloc_count = SIGNALLER_INITIAL_WAIT_COUNT;
    if (!(wait_info.pSemaphores = malloc( sizeof(*wait_info.pSemaphores) * wait_alloc_count) ))
    {
        fprintf( stderr, "err:winevulkan:signaller_worker no memory.\n" );
        return NULL;
    }
    if (!(wait_info.pValues = malloc( sizeof(*wait_info.pValues) * wait_alloc_count )))
    {
        fprintf( stderr, "err:winevulkan:signaller_worker no memory.\n" );
        free( (void *)wait_info.pSemaphores );
        return NULL;
    }

    for (;;)
    {
        pthread_mutex_lock( &device->signaller_mutex );
        if (device->stop)
        {
            pthread_mutex_unlock( &device->signaller_mutex );
            break;
        }
        wait_info.semaphoreCount = 1;
        *(VkSemaphore *)wait_info.pSemaphores = device->sem_poll_update.sem;
        *(uint64_t *)wait_info.pValues = device->sem_poll_update.value + 1;
        LIST_FOR_EACH_ENTRY(sem, &device->sem_poll_list, struct semaphore, poll_entry)
        {
            if (!semaphore_process( device, sem, &wait_info, &wait_alloc_count ))
            {
                pthread_mutex_unlock( &device->signaller_mutex );
                break;
            }
        }
        device->sem_poll_update_value = device->sem_poll_update.value;
        pthread_cond_signal( &device->sem_poll_updated_cond );
        pthread_mutex_unlock( &device->signaller_mutex );
        while ((res = wait_host_semaphores( device, &wait_info, 3000000000ull )) == VK_TIMEOUT)
        {
            if (wait_info.semaphoreCount > 1)
                fprintf( stderr, "err:winevulkan:signaller_worker wait timed out with non-empty poll list.\n" );
        }
        if (res != VK_SUCCESS)
        {
            fprintf( stderr, "err:winevulkan:signaller_worker error waiting for semaphores, vr %d.\n", res );
            break;
        }
    }

    free( (void *)wait_info.pSemaphores );
    free( (void *)wait_info.pValues );
    if (debug_level)
        fprintf( stderr, "[%d] msg:winevulkan:signaller_worker exiting.\n", unix_tid );

    return NULL;
}

static void register_sem_poll( struct vulkan_device *device, struct semaphore *semaphore )
{
    pthread_mutex_lock( &device->signaller_mutex );
    if (!device->signaller_thread)
    {
        device->sem_poll_update.sem = create_timeline_semaphore( device );
        device->sem_poll_update.value = 0;
        pthread_cond_init( &device->sem_poll_updated_cond, NULL );
        if (TRACE_ON(vulkan))
            debug_level = 4;
        else if (WARN_ON(vulkan))
            debug_level = 3;
        else if (FIXME_ON(vulkan))
            debug_level = 2;
        else if (ERR_ON(vulkan))
            debug_level = 1;
        else
            debug_level = 0;
        if (pthread_create( &device->signaller_thread, NULL, signaller_worker, device ))
            ERR( "Failed to create signaller_worker.\n" );
        WARN( "d3d12 fence used, created signaller worker.\n" );
    }
    assert( !semaphore->poll_entry.next );
    list_add_head( &device->sem_poll_list, &semaphore->poll_entry );
    signal_timeline_sem( device, device->sem_poll_update.sem, &device->sem_poll_update.value );
    pthread_mutex_unlock( &device->signaller_mutex );
}

static void update_sem_poll_wait_processed_locked( struct vulkan_device *device )
{
    uint64_t update_value;

    signal_timeline_sem( device, device->sem_poll_update.sem, &device->sem_poll_update.value );
    update_value = device->sem_poll_update.value;
    while ( device->sem_poll_update_value < update_value )
        pthread_cond_wait( &device->sem_poll_updated_cond, &device->signaller_mutex );
}

static void unregister_sem_poll( struct vulkan_device *device, struct semaphore *semaphore )
{
    struct list *entry;

    pthread_mutex_lock( &device->signaller_mutex );
    list_remove(&semaphore->poll_entry);
    semaphore->poll_entry.next = semaphore->poll_entry.prev = NULL;
    update_sem_poll_wait_processed_locked( device );
    pthread_mutex_unlock( &device->signaller_mutex );

    while ((entry = list_head( &semaphore->pending_waits )))
        release_fence_op( device, CONTAINING_RECORD(entry, struct pending_d3d12_fence_op, entry) );
    while ((entry = list_head( &semaphore->pending_signals)))
        release_fence_op( device, CONTAINING_RECORD(entry, struct pending_d3d12_fence_op, entry) );
}

static struct pending_d3d12_fence_op *get_free_fence_op( struct vulkan_device *device )
{
    struct pending_d3d12_fence_op *op;
    struct list *entry;

    if ((entry = list_head(&device->free_fence_ops_list)))
    {
        list_remove( entry );
        return CONTAINING_RECORD(entry, struct pending_d3d12_fence_op, entry);
    }

    if (!(op = malloc( sizeof(*op) )))
    {
        ERR( "No memory.\n" );
        return NULL;
    }
    op->local_sem.sem = create_timeline_semaphore( device );
    op->local_sem.value = 0;
    ++device->allocated_fence_ops_count;
    TRACE( "Total allocated fence ops %u.\n", device->allocated_fence_ops_count );
    return op;
}

static void add_sem_wait_op( struct vulkan_device *device, struct semaphore *semaphore, uint64_t virtual_value,
                             VkSemaphore *phys_semaphore, uint64_t *phys_wait_value )
{
    struct vulkan_instance *instance = device->physical_device->instance;
    struct pending_d3d12_fence_op *op;

    pthread_mutex_lock( &device->signaller_mutex );
    LIST_FOR_EACH_ENTRY(op, &semaphore->pending_waits, struct pending_d3d12_fence_op, entry)
    {
        if (op->virtual_value == virtual_value)
        {
            *phys_semaphore = op->local_sem.sem;
            *phys_wait_value = op->local_sem.value + 1;
            pthread_mutex_unlock( &device->signaller_mutex );
            return;
        }
    }
    if ((op = get_free_fence_op( device )))
    {
        op->virtual_value = virtual_value;
        op->shared_physical_value = __atomic_load_n( &semaphore->info.t->physical_value, __ATOMIC_ACQUIRE ) + 1;
        *phys_semaphore = op->local_sem.sem;
        *phys_wait_value = op->local_sem.value + 1;
        op->semaphore = semaphore;
        list_add_tail( &semaphore->pending_waits, &op->entry );
        instance->p_remove_object( instance, &semaphore->obj.obj );
        vulkan_object_init( &semaphore->obj.obj, op->local_sem.sem );
        instance->p_insert_object( instance, &semaphore->obj.obj );

        signal_timeline_sem( device, device->sem_poll_update.sem, &device->sem_poll_update.value );
        TRACE( "added wait op, semaphore %p, %s, temp sem %s, %s.\n", semaphore, wine_dbgstr_longlong(virtual_value),
                wine_dbgstr_longlong(op->local_sem.sem), wine_dbgstr_longlong(op->local_sem.value) );
    }
    else
    {
        *phys_semaphore = 0;
        *phys_wait_value = 0;
    }
    pthread_mutex_unlock( &device->signaller_mutex );
}

static void add_sem_signal_op( struct vulkan_device *device, struct semaphore *semaphore, uint64_t virtual_value,
                               VkSemaphore *phys_semaphore, uint64_t *phys_signal_value, BOOL signal_immediate )
{
    struct vulkan_instance *instance = device->physical_device->instance;
    struct pending_d3d12_fence_op *op;
    UINT64 value;

    pthread_mutex_lock( &device->signaller_mutex );
    if ((op = get_free_fence_op( device )))
    {
        op->virtual_value = virtual_value;
        *phys_semaphore = op->local_sem.sem;
        *phys_signal_value = op->local_sem.value + 1;
        op->semaphore = semaphore;
        list_add_tail( &semaphore->pending_signals, &op->entry );
        instance->p_remove_object( instance, &semaphore->obj.obj );
        vulkan_object_init( &semaphore->obj.obj, op->local_sem.sem );
        instance->p_insert_object( instance, &semaphore->obj.obj );

        if (signal_immediate)
        {
            value = op->local_sem.value;
            signal_timeline_sem( device, op->local_sem.sem, &value );
            update_sem_poll_wait_processed_locked( device );
            TRACE( "signal op %p, semaphore %p, %s, temp sem %s, %s.\n", op, semaphore, wine_dbgstr_longlong(virtual_value),
                   wine_dbgstr_longlong(op->local_sem.sem), wine_dbgstr_longlong(op->local_sem.value) );
        }
        else
        {
            signal_timeline_sem( device, device->sem_poll_update.sem, &device->sem_poll_update.value );
            TRACE( "added signal op, semaphore %p, %s, temp sem %s, %s.\n", semaphore, wine_dbgstr_longlong(virtual_value),
                   wine_dbgstr_longlong(op->local_sem.sem), wine_dbgstr_longlong(op->local_sem.value) );
        }
    }
    else
    {
        *phys_semaphore = 0;
        *phys_signal_value = 0;
    }
    pthread_mutex_unlock( &device->signaller_mutex );
}
/* end of d3d12 shared fences helpers. */

static const void *find_next_struct( const VkBaseInStructure *header, VkStructureType type )
{
    for (; header; header = header->pNext) if (header->sType == type) return header;
    return NULL;
}

static const void *pop_next_struct( VkBaseOutStructure **next, VkStructureType type )
{
    VkBaseOutStructure *ptr;
    while (*next && (*next)->sType != type) next = &(*next)->pNext;
    if ((ptr = *next)) *next = ptr->pNext;
    return ptr;
}

static int vulkan_object_compare( const void *key, const struct rb_entry *entry )
{
    struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
    const uint64_t *host_handle = key;
    if (*host_handle < object->host_handle) return -1;
    if (*host_handle > object->host_handle) return 1;
    return 0;
}

static uint64_t vulkan_instance_client_handle_from_host( struct vulkan_instance *instance, uint64_t host_handle )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    struct rb_entry *entry;
    uint64_t result = 0;

    pthread_rwlock_rdlock( &impl->objects_lock );
    if ((entry = rb_get( &impl->objects, &host_handle )))
    {
        struct vulkan_object *object = RB_ENTRY_VALUE( entry, struct vulkan_object, entry );
        result = object->client_handle;
    }
    pthread_rwlock_unlock( &impl->objects_lock );
    return result;
}

static void vulkan_instance_insert_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_put( &impl->objects, &obj->host_handle, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void vulkan_instance_remove_object( struct vulkan_instance *instance, struct vulkan_object *obj )
{
    struct instance *impl = CONTAINING_RECORD( instance, struct instance, obj );
    if (impl->objects.compare)
    {
        pthread_rwlock_wrlock( &impl->objects_lock );
        rb_remove( &impl->objects, &obj->entry );
        pthread_rwlock_unlock( &impl->objects_lock );
    }
}

static void free_debug_utils_messengers( struct list *messengers )
{
    struct vulkan_debug_utils_messenger *messenger, *next;

    LIST_FOR_EACH_ENTRY_SAFE( messenger, next, messengers, struct vulkan_debug_utils_messenger, entry )
    {
        list_remove( &messenger->entry );
        free( messenger );
    }
}

static void free_debug_report_callbacks( struct list *callbacks )
{
    struct vulkan_debug_report_callback *callback, *next;

    LIST_FOR_EACH_ENTRY_SAFE( callback, next, callbacks, struct vulkan_debug_report_callback, entry )
    {
        list_remove( &callback->entry );
        free( callback );
    }
}

static VkResult convert_instance_create_info( struct mempool *pool, VkInstanceCreateInfo *info, struct instance *instance )
{
    const VkBaseInStructure *header = (const VkBaseInStructure *)info;
    const VkDebugReportCallbackCreateInfoEXT *debug_report_callback;
    const char **extensions;
    uint32_t count = 0;

    while ((header = find_next_struct( header->pNext, VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT )))
    {
        const VkDebugUtilsMessengerCreateInfoEXT *debug_utils_messenger = (const VkDebugUtilsMessengerCreateInfoEXT *)header;
        struct vulkan_debug_utils_messenger *messenger = debug_utils_messenger->pUserData;

        list_remove( &messenger->entry );
        list_add_tail( &instance->utils_messengers, &messenger->entry );
        messenger->instance = &instance->obj;
    }

    if ((debug_report_callback = find_next_struct( info->pNext, VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT )))
    {
        struct vulkan_debug_report_callback *callback = debug_report_callback->pUserData;

        list_remove( &callback->entry );
        list_add_tail( &instance->report_callbacks, &callback->entry );
        callback->instance = &instance->obj;
    }

    if (info->enabledLayerCount)
    {
        FIXME( "Loading explicit layers is not supported!\n" );
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    driver_funcs->p_map_instance_extensions( &instance->obj.extensions );
    instance->obj.extensions.has_VK_KHR_win32_surface = 0;

    if (instance->obj.extensions.has_VK_EXT_debug_utils || instance->obj.extensions.has_VK_EXT_debug_report)
    {
        rb_init( &instance->objects, vulkan_object_compare );
        pthread_rwlock_init( &instance->objects_lock, NULL );
    }

    if (instance->obj.extensions.has_VK_KHR_win32_surface && vulkan_funcs.host_extensions.has_VK_EXT_surface_maintenance1)
        instance->obj.extensions.has_VK_EXT_surface_maintenance1 = 1;
    if (vulkan_funcs.host_extensions.has_VK_KHR_get_physical_device_properties2)
        instance->obj.extensions.has_VK_KHR_get_physical_device_properties2 = 1;
    if (use_external_memory())
        instance->obj.extensions.has_VK_KHR_external_memory_capabilities = 1;

    /* VK_KHR_win32_keyed_mutex only requires external memory extensions, but we will use
     * external semaphore fds to implement it, so we enable the instance extensions too */
    instance->obj.extensions.has_VK_KHR_external_semaphore_capabilities = 1;

    if (!(extensions = mem_alloc( pool, sizeof(instance->obj.extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (instance->obj.extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host instance extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static VkResult init_physical_device( struct vulkan_physical_device *physical_device, VkPhysicalDevice host_physical_device,
                                      VkPhysicalDevice client_physical_device, struct vulkan_instance *instance )
{
    struct vulkan_device_extensions extensions = {0};
    VkPhysicalDeviceProperties physdev_properties;
    VkExtensionProperties *properties;
    uint32_t count;
    VkResult res;

    vulkan_object_init_ptr( &physical_device->obj, (UINT_PTR)host_physical_device, &client_physical_device->obj );
    physical_device->instance = instance;

    instance->p_vkGetPhysicalDeviceProperties( host_physical_device, &physdev_properties );
    physical_device->api_version = physdev_properties.apiVersion;

    instance->p_vkGetPhysicalDeviceMemoryProperties( host_physical_device, &physical_device->memory_properties );

    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, NULL ))) return res;
    if (!(properties = calloc( count, sizeof(*properties) ))) return res;
    if ((res = instance->p_vkEnumerateDeviceExtensionProperties( host_physical_device, NULL, &count, properties ))) goto done;

    TRACE( "Host physical device extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    physical_device->extensions = extensions;

    if (zero_bits && physical_device->extensions.has_VK_EXT_map_memory_placed && physical_device->extensions.has_VK_KHR_map_memory2)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT map_placed_feature = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &map_placed_feature};

        instance->p_vkGetPhysicalDeviceFeatures2KHR( host_physical_device, &features );
        if (map_placed_feature.memoryMapPlaced && map_placed_feature.memoryUnmapReserve)
        {
            VkPhysicalDeviceMapMemoryPlacedPropertiesEXT map_placed_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext = &map_placed_props};

            instance->p_vkGetPhysicalDeviceProperties2( host_physical_device, &props );
            physical_device->map_placed_align = map_placed_props.minPlacedMemoryMapAlignment;
            TRACE( "Using placed map with alignment %u\n", physical_device->map_placed_align );
        }
    }

    if (zero_bits && physical_device->extensions.has_VK_EXT_external_memory_host && !physical_device->map_placed_align)
    {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_mem_props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host_mem_props};

        instance->p_vkGetPhysicalDeviceProperties2KHR( host_physical_device, &props );
        physical_device->external_memory_align = host_mem_props.minImportedHostPointerAlignment;
        if (physical_device->external_memory_align) WARN( "Not using VK_EXT_external_memory_host for memory mapping\n" );
        else TRACE( "Using VK_EXT_external_memory_host for memory mapping with alignment: %u\n", physical_device->external_memory_align );
    }

    driver_funcs->p_map_device_extensions( &extensions );
    if (extensions.has_VK_KHR_external_memory_win32 && zero_bits && !physical_device->map_placed_align)
    {
        WARN( "Cannot export WOW64 memory without VK_EXT_map_memory_placed\n" );
        extensions.has_VK_KHR_external_memory_win32 = 0;
    }
    extensions.has_VK_KHR_win32_keyed_mutex = extensions.has_VK_KHR_timeline_semaphore &&
                                              extensions.has_VK_KHR_external_semaphore_fd;

    /* filter out unsupported client device extensions */
#define USE_VK_EXT(x) client_physical_device->extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_DEVICE_EXTS
#undef USE_VK_EXT

done:
    free( properties );
    return res;
}

/* Helper function which stores wrapped physical devices in the instance object. */
static VkResult init_physical_devices( struct vulkan_instance *instance, struct vulkan_physical_device *physical_devices )
{
    VkInstance client_instance = instance->client.instance;
    VkPhysicalDevice *host_physical_devices;
    uint32_t physical_device_count;
    unsigned int i;
    VkResult res;

    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, NULL )))
    {
        ERR( "Failed to enumerate physical devices, res %d\n", res );
        return res;
    }
    if (!physical_device_count) return res;

    if (physical_device_count > client_instance->physical_device_count)
    {
        client_instance->physical_device_count = physical_device_count;
        return VK_ERROR_OUT_OF_POOL_MEMORY;
    }
    client_instance->physical_device_count = physical_device_count;

    if (!(host_physical_devices = calloc( physical_device_count, sizeof(*host_physical_devices) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = instance->p_vkEnumeratePhysicalDevices( instance->host.instance, &physical_device_count, host_physical_devices ))) goto failed;

    /* Wrap each host physical device handle into a dispatchable object for the ICD loader. */
    for (i = 0; i < physical_device_count; i++)
    {
        VkPhysicalDevice client_physical_device = &client_instance->physical_device[i];
        struct vulkan_physical_device *physical_device = physical_devices + i;
        if ((res = init_physical_device( physical_device, host_physical_devices[i], client_physical_device, instance ))) goto failed;
    }
    instance->physical_device_count = physical_device_count;
    instance->physical_devices = physical_devices;

failed:
    free( host_physical_devices );
    return res;
}

static BOOL add_instance_extension( const char *extension, size_t len, struct vulkan_instance_extensions *extensions )
{
#define USE_VK_EXT(x) \
    if (len == sizeof(#x) - 1 && !strncmp( #x, extension, len ))    \
    {                                                               \
        if (!extensions->has_ ## x) TRACE( "Adding %s\n", #x );     \
        return extensions->has_ ## x = 1;                           \
    }
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
    WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    return FALSE;
}

static void parse_instance_extensions( struct vulkan_instance_extensions *extensions, const char *str )
{
    const char *next;
    for (next = str; *next; next++)
    {
        if (*next != ' ') continue;
        add_instance_extension( str, next - str, extensions );
        str = next + 1;
    }
    if (next > str) add_instance_extension( str, next - str, extensions );
}

static VkResult win32u_vkCreateInstance( const VkInstanceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator,
                                         VkInstance *client_instance_ptr )
{
    VkInstanceCreateInfo *create_info = (VkInstanceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkInstance host_instance = VK_NULL_HANDLE, client_instance = *client_instance_ptr;
    const VkCreateInfoWineInstanceCallback *callback_info;
    struct vulkan_physical_device *physical_devices;
    struct mempool pool = {0};
    struct instance *instance;
    unsigned int i;
    VkResult res;

    if (!(instance = calloc( 1, sizeof(*instance) + sizeof(*physical_devices) * client_instance->physical_device_count) ))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (create_info->pApplicationInfo) instance->obj.api_version = create_info->pApplicationInfo->apiVersion;

    physical_devices = (struct vulkan_physical_device *)(instance + 1);
    instance->obj.extensions = client_instance->extensions;
    list_init( &instance->utils_messengers );
    list_init( &instance->report_callbacks );

    if (instance->obj.extensions.has_VK_WINE_openxr_instance_extensions)
    {
        parse_instance_extensions( &instance->obj.extensions, getenv( "__WINE_OPENXR_VK_INSTANCE_EXTENSIONS" ) );
        instance->obj.extensions.has_VK_WINE_openxr_instance_extensions = 0;
    }

    pthread_key_create(&instance->obj.transient_object_handle, free);

    if ((res = convert_instance_create_info( &pool, create_info, instance ))) goto failed;
    if ((callback_info = pop_next_struct( (VkBaseOutStructure **)&create_info->pNext, VK_STRUCTURE_TYPE_CREATE_INFO_WINE_INSTANCE_CALLBACK )))
    {
        PFN_vkCreateInstanceCallbackWINE callback = (void *)(UINT_PTR)callback_info->native_create_callback;
        if ((res = callback( create_info, allocator, &host_instance, p_vkGetInstanceProcAddr, (void *)(UINT_PTR)callback_info->context ))) goto failed;
    }
    else if ((res = p_vkCreateInstance( create_info, NULL /* allocator */, &host_instance ))) goto failed;

    vulkan_object_init_ptr( &instance->obj.obj, (UINT_PTR)host_instance, &client_instance->obj );
    instance->obj.p_insert_object = vulkan_instance_insert_object;
    instance->obj.p_remove_object = vulkan_instance_remove_object;
    instance->obj.p_client_handle_from_host = vulkan_instance_client_handle_from_host;

#define USE_VK_FUNC( name )                                                                          \
    instance->obj.p_##name = (void *)p_vkGetInstanceProcAddr( instance->obj.host.instance, #name );  \
    if (!instance->obj.p_##name) TRACE( "Instance proc %s not found.\n", #name );
    ALL_VK_INSTANCE_FUNCS
#undef USE_VK_FUNC

    /* Cache physical devices for vkEnumeratePhysicalDevices within the instance as each vkPhysicalDevice is a dispatchable
     * object, which means we need to wrap the host physical devices and present those to the application.
     */
    if ((res = init_physical_devices( &instance->obj, physical_devices ))) goto failed;

    TRACE( "Created instance %p, host_instance %p.\n", instance, instance->obj.host.instance );
    for (i = 0; i < instance->obj.physical_device_count; i++)
    {
        struct vulkan_physical_device *physical_device = &instance->obj.physical_devices[i];
        vulkan_instance_insert_object( &instance->obj, &physical_device->obj );
    }
    vulkan_instance_insert_object( &instance->obj, &instance->obj.obj );

failed:
    if (res)
    {
        WARN( "Failed to create vulkan instance, res %d\n", res );
        if (host_instance) instance->obj.p_vkDestroyInstance( host_instance, NULL /* allocator */ );
        free_debug_utils_messengers( &instance->utils_messengers );
        free_debug_report_callbacks( &instance->report_callbacks );
        free( instance );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyInstance( VkInstance client_instance, const VkAllocationCallbacks *allocator )
{
    struct instance *instance = instance_from_handle( client_instance );

    if (!instance) return;

    instance->obj.p_vkDestroyInstance( instance->obj.host.instance, NULL /* allocator */ );
    for (int i = 0; i < instance->obj.physical_device_count; i++)
        vulkan_instance_remove_object( &instance->obj, &instance->obj.physical_devices[i].obj );
    vulkan_instance_remove_object( &instance->obj, &instance->obj.obj );

    if (instance->objects.compare) pthread_rwlock_destroy( &instance->objects_lock );
    free_debug_utils_messengers( &instance->utils_messengers );
    free_debug_report_callbacks( &instance->report_callbacks );
    pthread_key_delete(instance->obj.transient_object_handle);
    free( instance );
}

static BOOL add_device_extension( const char *extension, size_t len, struct vulkan_device_extensions *extensions )
{
#define USE_VK_EXT(x) \
    if (len == sizeof(#x) - 1 && !strncmp( #x, extension, len ))    \
    {                                                               \
        if (!extensions->has_ ## x) TRACE( "Adding %s\n", #x );     \
        return extensions->has_ ## x = 1;                           \
    }
    ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT
    WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    return FALSE;
}

static void parse_device_extensions( struct vulkan_device_extensions *extensions, const char *str )
{
    const char *next;

    for (next = str; *next; next++)
    {
        if (*next != ' ') continue;
        add_device_extension( str, next - str, extensions );
        str = next + 1;
    }
    if (next > str) add_device_extension( str, next - str, extensions );
}

static VkResult convert_device_create_info( struct vulkan_physical_device *physical_device, VkDeviceCreateInfo *info,
                                            struct mempool *pool, struct vulkan_device *device )
{
    struct vulkan_instance *instance = physical_device->instance;
    const char **extensions;
    uint32_t count = 0;

    /* Should be filtered out by loader as ICDs don't support layers. */
    info->enabledLayerCount = 0;
    info->ppEnabledLayerNames = NULL;

    if (device->extensions.has_VK_KHR_win32_keyed_mutex)
    {
        device->extensions.has_VK_KHR_timeline_semaphore = 1;
        device->extensions.has_VK_KHR_external_semaphore_fd = 1;
        device->extensions.has_VK_KHR_external_semaphore = 1;
    }

    driver_funcs->p_map_device_extensions( &device->extensions );
    device->extensions.has_VK_KHR_win32_keyed_mutex = 0;
    device->extensions.has_VK_KHR_external_memory_win32 = 0;
    device->extensions.has_VK_KHR_external_fence_win32 = 0;
    device->extensions.has_VK_KHR_external_semaphore_win32 = 0;
    device->extensions.has_VK_WINE_openvr_device_extensions = 0;
    device->extensions.has_VK_WINE_openxr_device_extensions = 0;

    if (device->extensions.has_VK_EXT_external_memory_dma_buf)
        device->extensions.has_VK_KHR_external_memory_fd = 1;

    if (physical_device->map_placed_align)
    {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *map_placed_features;

        if (!(map_placed_features = mem_alloc( pool, sizeof(*map_placed_features) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        map_placed_features->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
        map_placed_features->pNext = (void *)info->pNext;
        map_placed_features->memoryMapPlaced = VK_TRUE;
        map_placed_features->memoryMapRangePlaced = VK_FALSE;
        map_placed_features->memoryUnmapReserve = VK_TRUE;
        info->pNext = map_placed_features;

        device->extensions.has_VK_EXT_map_memory_placed = 1;
        device->extensions.has_VK_KHR_map_memory2 = 1;
    }
    else if (physical_device->external_memory_align)
    {
        device->extensions.has_VK_KHR_external_memory = 1;
        device->extensions.has_VK_EXT_external_memory_host = 1;
    }

    /* win32u uses VkSwapchainPresentScalingCreateInfoEXT if available. */
    if (device->extensions.has_VK_KHR_swapchain && instance->extensions.has_VK_EXT_surface_maintenance1 &&
        physical_device->extensions.has_VK_EXT_swapchain_maintenance1)
        device->extensions.has_VK_EXT_swapchain_maintenance1 = 1;

    /* Force-enable the extensions the cross-process producer needs so we can
     * interpose a managed swapchain even when the app did not request them.
     * Only meaningful for swapchain devices. */
    if (device->extensions.has_VK_KHR_swapchain &&
        physical_device->extensions.has_VK_EXT_external_memory_dma_buf)
    {
        device->extensions.has_VK_EXT_external_memory_dma_buf = 1;
        device->extensions.has_VK_KHR_external_memory_fd = 1;
        device->extensions.has_VK_KHR_external_memory = 1;
        if (physical_device->extensions.has_VK_EXT_image_drm_format_modifier)
        {
            device->extensions.has_VK_EXT_image_drm_format_modifier = 1;
            /* VK_EXT_image_drm_format_modifier requires VK_KHR_image_format_list +
             * VK_KHR_bind_memory2 + VK_KHR_sampler_ycbcr_conversion (1.1 core).
             * Enable the KHR aliases defensively when the host advertises them. */
            if (physical_device->extensions.has_VK_KHR_image_format_list)
                device->extensions.has_VK_KHR_image_format_list = 1;
            if (physical_device->extensions.has_VK_KHR_bind_memory2)
                device->extensions.has_VK_KHR_bind_memory2 = 1;
            if (physical_device->extensions.has_VK_KHR_sampler_ycbcr_conversion)
                device->extensions.has_VK_KHR_sampler_ycbcr_conversion = 1;
        }
    }

    if (!(extensions = mem_alloc( pool, sizeof(device->extensions) * 8 * sizeof(*extensions) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
#define USE_VK_EXT(x) if (device->extensions.has_ ## x) extensions[count++] = #x;
    ALL_VK_DEVICE_EXTS
#undef USE_VK_EXT

    TRACE( "Enabling %u host device extensions\n", count );
    for (const char **extension = extensions, **end = extension + count; extension < end; extension++)
        TRACE( "  - %s\n", debugstr_a(*extension) );

    info->ppEnabledExtensionNames = extensions;
    info->enabledExtensionCount = count;
    return VK_SUCCESS;
}

static void init_device_queues( struct vulkan_device *device, const VkDeviceQueueCreateInfo *create_info, VkDevice client_device )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    VkQueue client_queues = client_device->queues + device->queue_count;
    struct vulkan_queue *queues = device->queues + device->queue_count;

    TRACE( "Queue family index %u, queue count %u.\n", create_info->queueFamilyIndex, create_info->queueCount );

    info.flags = create_info->flags;
    info.queueFamilyIndex = create_info->queueFamilyIndex;
    for (info.queueIndex = 0; info.queueIndex < create_info->queueCount; info.queueIndex++)
    {
        VkQueue host_queue, client_queue = client_queues + info.queueIndex;
        struct vulkan_queue *queue = queues + info.queueIndex;

        if (info.flags && device->p_vkGetDeviceQueue2) device->p_vkGetDeviceQueue2( device->host.device, &info, &host_queue );
        else device->p_vkGetDeviceQueue( device->host.device, info.queueFamilyIndex, info.queueIndex, &host_queue );
        vulkan_object_init_ptr( &queue->obj, (UINT_PTR)host_queue, &client_queue->obj );
        queue->device = device;
        queue->info = info;

        TRACE( "Got device %p queue %p, host_queue %p.\n", device, queue, queue->host.queue );
    }

    device->queue_count += create_info->queueCount;
}

static VkResult win32u_vkCreateDevice( VkPhysicalDevice client_physical_device, const VkDeviceCreateInfo *client_create_info,
                                       const VkAllocationCallbacks *allocator, VkDevice *client_device_ptr )
{
    VkDeviceCreateInfo *create_info = (VkDeviceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;
    VkDevice host_device, client_device = *client_device_ptr;
    const VkCreateInfoWineDeviceCallback *callback_info;
    unsigned int queue_count, props_count, i;
    struct vulkan_device *device;
    struct mempool pool = {0};
    VkPhysicalDeviceFeatures features = {0};
    VkResult res;

    if (TRACE_ON(vulkan))
    {
        VkPhysicalDeviceProperties properties = {0};
        instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        TRACE( "Device name: %s.\n", debugstr_a(properties.deviceName) );
        TRACE( "Vendor ID: %#x, Device ID: %#x.\n", properties.vendorID, properties.deviceID );
        TRACE( "Driver version: %#x.\n", properties.driverVersion );
    }

    /* We need to cache all queues within the device as each requires wrapping since queues are dispatchable objects. */
    for (queue_count = 0, i = 0; i < create_info->queueCreateInfoCount; i++) queue_count += create_info->pQueueCreateInfos[i].queueCount;
    instance->p_vkGetPhysicalDeviceQueueFamilyProperties(physical_device->host.physical_device, &props_count, NULL);

    if (!(device = calloc( 1, sizeof(*device) + queue_count * sizeof(*device->queues) + props_count * sizeof(*device->queue_props) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    device->extensions = client_device->extensions;
    device->queues = (void *)(device + 1);
    device->queue_props = (void *)(device->queues + queue_count);

{
        VkPhysicalDeviceFeatures2 *features2;

        /* Enable shaderStorageImageWriteWithoutFormat for fshack
         * This is available on all hardware and driver combinations we care about.
         */
        if (create_info->pEnabledFeatures)
        {
            features = *create_info->pEnabledFeatures;
            features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            create_info->pEnabledFeatures = &features;
        }
        if ((features2 = (void *)find_next_struct((const void *)create_info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)))
        {
            features2->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        }
        else if (!create_info->pEnabledFeatures)
        {
            features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
            create_info->pEnabledFeatures = &features;
        }
}

    if (device->extensions.has_VK_WINE_openvr_device_extensions)
    {
        VkPhysicalDeviceProperties properties = {0};
        const char *vr_exts;
        char name[64];
        instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, &properties );
        sprintf( name, "VK_WINE_OPENVR_DEVICE_EXTS_PCIID_%04x_%04x", properties.vendorID, (uint16_t)properties.deviceID );
        if (!(vr_exts = getenv( name ))) vr_exts = getenv( "VK_WINE_OPENVR_DEVICE_EXTS" );
        if (vr_exts) parse_device_extensions( &device->extensions, vr_exts );
        device->extensions.has_VK_WINE_openvr_device_extensions = 0;
    }
    if (device->extensions.has_VK_WINE_openxr_device_extensions)
    {
        parse_device_extensions( &device->extensions, getenv( "__WINE_OPENXR_VK_DEVICE_EXTENSIONS" ) );
        device->extensions.has_VK_WINE_openxr_device_extensions = 0;
    }

    if ((res = convert_device_create_info( physical_device, create_info, &pool, device ))) goto failed;
    if ((callback_info = pop_next_struct( (VkBaseOutStructure **)&create_info->pNext, VK_STRUCTURE_TYPE_CREATE_INFO_WINE_DEVICE_CALLBACK )))
    {
        PFN_vkCreateDeviceCallbackWINE callback = (void *)(UINT_PTR)callback_info->native_create_callback;
        if ((res = callback( physical_device->host.physical_device, create_info, allocator, &host_device, p_vkGetInstanceProcAddr, (void *)(UINT_PTR)callback_info->context ))) goto failed;
    }
    else if ((res = instance->p_vkCreateDevice( physical_device->host.physical_device, create_info, NULL /* allocator */, &host_device ))) goto failed;

    vulkan_object_init_ptr( &device->obj, (UINT_PTR)host_device, &client_device->obj );
    device->physical_device = physical_device;

    pthread_mutex_init( &device->signaller_mutex, NULL );
    list_init( &device->sem_poll_list );
    list_init( &device->free_fence_ops_list );

#define USE_VK_FUNC( name )                                                          \
    device->p_##name = (void *)p_vkGetDeviceProcAddr( device->host.device, #name );  \
    if (!device->p_##name) TRACE( "Device proc %s not found.\n", #name );
    ALL_VK_DEVICE_FUNCS
#undef USE_VK_FUNC

    for (i = 0; i < create_info->queueCreateInfoCount; i++) init_device_queues( device, create_info->pQueueCreateInfos + i, client_device );
    instance->p_vkGetPhysicalDeviceQueueFamilyProperties( physical_device->host.physical_device, &props_count, device->queue_props );

    TRACE( "Created device %p, host_device %p.\n", device, device->host.device );
    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        instance->p_insert_object( instance, &queue->obj );
    instance->p_insert_object( instance, &device->obj );

failed:
    if (res)
    {
        WARN( "Failed to create device, res %d\n", res );
        free( device );
    }
    mem_free( &pool );
    return res;
}

static void win32u_vkDestroyDevice( VkDevice client_device, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    struct pending_d3d12_fence_op *op, *next;
    unsigned int i;

    if (!device) return;

    if (device->signaller_thread)
    {
        TRACE( "Shutting down signaller thread.\n" );
        pthread_mutex_lock( &device->signaller_mutex );
        device->stop = 1;
        signal_timeline_sem( device, device->sem_poll_update.sem, &device->sem_poll_update.value );
        pthread_mutex_unlock( &device->signaller_mutex );
        pthread_join( device->signaller_thread, NULL );
        device->p_vkDestroySemaphore( device->host.device, device->sem_poll_update.sem, NULL );
        pthread_cond_destroy( &device->sem_poll_updated_cond );
        TRACE( "Signaller thread shut down.\n" );
    }
    pthread_mutex_destroy(&device->signaller_mutex);
    LIST_FOR_EACH_ENTRY_SAFE( op, next, &device->free_fence_ops_list, struct pending_d3d12_fence_op, entry )
    {
        device->p_vkDestroySemaphore( device->host.device, op->local_sem.sem, NULL );
        free( op );
    }

    device->p_vkDestroyDevice( device->host.device, NULL /* pAllocator */ );
    for (i = 0; i < device->queue_count; i++)
        instance->p_remove_object( instance, &device->queues[i].obj );
    instance->p_remove_object( instance, &device->obj );

    free( device );
}

static VkQueue device_find_queue( VkDevice client_device, const VkDeviceQueueInfo2 *info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    for (struct vulkan_queue *queue = device->queues; queue < device->queues + device->queue_count; queue++)
        if (!memcmp( &queue->info, info, sizeof(*info) )) return queue->client.queue;

    return VK_NULL_HANDLE;
}

static void win32u_vkGetDeviceQueue( VkDevice client_device, uint32_t family_index, uint32_t queue_index, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
    info.queueFamilyIndex = family_index;
    info.queueIndex = queue_index;

    *client_queue = device_find_queue( client_device, &info );
}

static void win32u_vkGetDeviceQueue2( VkDevice client_device, const VkDeviceQueueInfo2 *client_info, VkQueue *client_queue )
{
    VkDeviceQueueInfo2 info = *client_info;
    if (info.pNext) FIXME( "pNext not implemented\n" );
    info.pNext = NULL;

    *client_queue = device_find_queue( client_device, &info );
}

static void set_transient_client_handle(struct vulkan_instance *instance, uint64_t client_handle)
{
    uint64_t *handle = pthread_getspecific(instance->transient_object_handle);
    if (!handle)
    {
        handle = malloc(sizeof(uint64_t));
        pthread_setspecific(instance->transient_object_handle, handle);
    }
    *handle = client_handle;
}

static VkResult win32u_vkAllocateMemory( VkDevice client_device, const VkMemoryAllocateInfo *client_alloc_info,
                                         const VkAllocationCallbacks *allocator, VkDeviceMemory *ret )
{
    VkImportMemoryFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)client_alloc_info; /* cast away const, chain has been copied in the thunks */
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)alloc_info;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkImportMemoryHostPointerInfoEXT host_pointer_info, *pointer_info = NULL;
    VkExportMemoryWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkImportMemoryWin32HandleInfoKHR *import_win32 = NULL;
    VkDeviceMemory host_device_memory = VK_NULL_HANDLE;
    VkExportMemoryAllocateInfo *export_info = NULL;
    struct device_memory *memory;
    BOOL export_win32_handle = FALSE;
    BOOL nt_shared = FALSE;
    uint32_t mem_flags;
    void *mapping = NULL;
    VkResult res;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            export_info = (VkExportMemoryAllocateInfo *)*next;
            if (export_info->handleTypes & EXTERNAL_MEMORY_WIN32_BITS)
            {
                export_win32_handle = TRUE;
                nt_shared = !(export_info->handleTypes & (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT |
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT));
            }
            export_info->handleTypes = map_external_memory_handle_types( export_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT:
            pointer_info = (VkImportMemoryHostPointerInfoEXT *)*next;
            break;
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR:
            import_win32 = (VkImportMemoryWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM: break;
        case VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO: break;
        case VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    /* For host visible memory, we try to use VK_EXT_external_memory_host on wow64 to ensure that mapped pointer is 32-bit. */
    mem_flags = physical_device->memory_properties.memoryTypes[alloc_info->memoryTypeIndex].propertyFlags;
    if (physical_device->external_memory_align && (mem_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !pointer_info &&
        (res = allocate_external_host_memory( device, alloc_info, mem_flags, &host_pointer_info )))
        return res;

    if (!(memory = calloc( 1, sizeof(*memory) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (import_win32)
    {
        switch (import_win32->handleType)
        {
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
            memory->global = PtrToUlong( import_win32->handle );
            memory->local = d3dkmt_open_resource( memory->global, NULL, &memory->mutex, &memory->sync );
            break;
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
        case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        {
            HANDLE shared = import_win32->handle;
            if (import_win32->name && !(shared = open_shared_resource_from_name( import_win32->name ))) break;
            memory->local = d3dkmt_open_resource( 0, shared, &memory->mutex, &memory->sync );
            if (shared && shared != import_win32->handle) NtClose( shared );
            break;
        }
        default:
            FIXME( "Unsupported handle type %#x\n", import_win32->handleType );
            break;
        }

        if (device->client.device->extensions.has_VK_KHR_win32_keyed_mutex && memory->sync)
        {
            VkSemaphoreTypeCreateInfo semaphore_type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            VkSemaphoreCreateInfo semaphore_create = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &semaphore_type};
            VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};

            semaphore_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphore_create, NULL, &memory->semaphore ))) goto failed;

            fd_info.handleType = get_host_external_semaphore_type();
            fd_info.semaphore = memory->semaphore;
            if ((fd_info.fd = d3dkmt_object_get_fd( memory->sync )) < 0)
            {
                res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
                goto failed;
            }

            if ((res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info ))) goto failed;
        }

        if ((fd_info.fd = d3dkmt_object_get_fd( memory->local )) < 0)
        {
            res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            goto failed;
        }

        fd_info.handleType = get_host_external_memory_type();
        fd_info.pNext = alloc_info->pNext;
        alloc_info->pNext = &fd_info;
    }

    set_transient_client_handle(instance, (uintptr_t)&memory->obj.obj);
    if ((res = device->p_vkAllocateMemory( device->host.device, alloc_info, NULL, &host_device_memory ))) goto failed;

    if (export_info && export_win32_handle)
    {
        if (!memory->local)
        {
            VkMemoryGetFdInfoKHR get_fd_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = host_device_memory};
            int fd = -1;

            switch ((get_fd_info.handleType = get_host_external_memory_type()))
            {
            case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT:
            case VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT:
                if ((res = device->p_vkGetMemoryFdKHR( device->host.device, &get_fd_info, &fd ))) goto failed;
                break;
            default:
                FIXME( "Unsupported handle type %#x\n", get_fd_info.handleType );
                break;
            }

            memory->local = d3dkmt_create_resource( fd, nt_shared ? NULL : &memory->global );
            close( fd );

            if (!memory->local) goto failed;
        }
        if (nt_shared && !(memory->shared = create_shared_resource_handle( memory->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &memory->obj.obj, host_device_memory );
    memory->size = alloc_info->allocationSize;
    memory->vm_map = mapping;
    instance->p_insert_object( instance, &memory->obj.obj );

    *ret = memory->obj.client.device_memory;
    return VK_SUCCESS;

failed:
    WARN( "Failed to allocate memory, res %d\n", res );
    if (host_device_memory) device->p_vkFreeMemory( device->host.device, host_device_memory, NULL );
    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkFreeMemory( VkDevice client_device, VkDeviceMemory client_memory, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = device->physical_device->instance;
    struct device_memory *memory;

    if (!client_memory) return;
    memory = device_memory_from_handle( client_memory );

    if (memory->vm_map && !physical_device->external_memory_align)
    {
        const VkMemoryUnmapInfoKHR info =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
            .memory = memory->obj.host.device_memory,
            .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
        };
        device->p_vkUnmapMemory2KHR( device->host.device, &info );
    }

    device->p_vkFreeMemory( device->host.device, memory->obj.host.device_memory, NULL );
    instance->p_remove_object( instance, &memory->obj.obj );

    if (memory->vm_map)
    {
        SIZE_T alloc_size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &alloc_size, MEM_RELEASE );
    }

    if (memory->semaphore) device->p_vkDestroySemaphore( device->host.device, memory->semaphore, NULL );
    if (memory->shared) NtClose( memory->shared );
    d3dkmt_destroy_resource( memory->local );
    d3dkmt_destroy_mutex( memory->mutex );
    d3dkmt_destroy_sync( memory->sync );
    free( memory );
}

static VkResult win32u_vkGetMemoryWin32HandleKHR( VkDevice client_device, const VkMemoryGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct device_memory *memory = device_memory_from_handle( handle_info->memory );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", memory->global );
        *handle = UlongToPtr( memory->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT:
    case VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), memory->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", memory->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static BOOL is_d3dkmt_global( D3DKMT_HANDLE handle )
{
    return (handle & 0xc0000000) && (handle & 0x3f) == 2;
}

static VkResult win32u_vkGetMemoryWin32HandlePropertiesKHR( VkDevice client_device, VkExternalMemoryHandleTypeFlagBits handle_type, HANDLE handle,
                                                            VkMemoryWin32HandlePropertiesKHR *handle_properties )
{
    static const UINT d3dkmt_type_bits = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT | VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    TRACE( "device %p, handle_type %#x, handle %p, handle_properties %p\n", device, handle_type, handle, handle_properties );

    if (is_d3dkmt_global( HandleToULong( handle ) )) handle_properties->memoryTypeBits = d3dkmt_type_bits;
    else handle_properties->memoryTypeBits = EXTERNAL_MEMORY_WIN32_BITS & ~d3dkmt_type_bits;

    return VK_SUCCESS;
}

static VkResult win32u_vkMapMemory2KHR( VkDevice client_device, const VkMemoryMapInfoKHR *map_info, void **data )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( map_info->memory );
    VkMemoryMapInfoKHR info = *map_info;
    VkMemoryMapPlacedInfoEXT placed_info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,
    };
    VkResult res;

    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map)
    {
        *data = (char *)memory->vm_map + info.offset;
        TRACE( "returning %p\n", *data );
        return VK_SUCCESS;
    }

    if (physical_device->map_placed_align)
    {
        SIZE_T alloc_size = memory->size;

        placed_info.pNext = info.pNext;
        info.pNext = &placed_info;
        info.offset = 0;
        info.size = VK_WHOLE_SIZE;
        info.flags |= VK_MEMORY_MAP_PLACED_BIT_EXT;

        if (NtAllocateVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, zero_bits,
                                     &alloc_size, MEM_COMMIT, PAGE_READWRITE ))
        {
            ERR( "NtAllocateVirtualMemory failed\n" );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    if (device->p_vkMapMemory2KHR)
        res = device->p_vkMapMemory2KHR( device->host.device, &info, data );
    else
    {
        if (info.pNext) FIXME( "struct extension chain not implemented!\n" );
        res = device->p_vkMapMemory( device->host.device, info.memory, info.offset, info.size, info.flags, data );
    }

    if (placed_info.pPlacedAddress)
    {
        if (res != VK_SUCCESS)
        {
            SIZE_T alloc_size = 0;
            ERR( "vkMapMemory2EXT failed: %d\n", res );
            NtFreeVirtualMemory( GetCurrentProcess(), &placed_info.pPlacedAddress, &alloc_size, MEM_RELEASE );
            return res;
        }
        memory->vm_map = placed_info.pPlacedAddress;
        *data = (char *)memory->vm_map + map_info->offset;
        TRACE( "Using placed mapping %p\n", memory->vm_map );
    }

#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset && res == VK_SUCCESS && (UINT_PTR)*data >> 32)
    {
        FIXME( "returned mapping %p does not fit 32-bit pointer\n", *data );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        *data = NULL;
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif

    return res;
}

static VkResult win32u_vkMapMemory( VkDevice client_device, VkDeviceMemory client_memory, VkDeviceSize offset,
                                    VkDeviceSize size, VkMemoryMapFlags flags, void **data )
{
    const VkMemoryMapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR,
        .flags = flags,
        .memory = client_memory,
        .offset = offset,
        .size = size,
    };

    return win32u_vkMapMemory2KHR( client_device, &info, data );
}

static VkResult win32u_vkUnmapMemory2KHR( VkDevice client_device, const VkMemoryUnmapInfoKHR *unmap_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct device_memory *memory = device_memory_from_handle( unmap_info->memory );
    VkMemoryUnmapInfoKHR info;
    VkResult res;

    if (memory->vm_map && physical_device->external_memory_align) return VK_SUCCESS;

    if (!device->p_vkUnmapMemory2KHR)
    {
        if (unmap_info->pNext || memory->vm_map) FIXME( "Not implemented\n" );
        device->p_vkUnmapMemory( device->host.device, memory->obj.host.device_memory );
        return VK_SUCCESS;
    }

    info = *unmap_info;
    info.memory = memory->obj.host.device_memory;
    if (memory->vm_map) info.flags |= VK_MEMORY_UNMAP_RESERVE_BIT_EXT;

    res = device->p_vkUnmapMemory2KHR( device->host.device, &info );

    if (res == VK_SUCCESS && memory->vm_map)
    {
        SIZE_T size = 0;
        NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map, &size, MEM_RELEASE );
        memory->vm_map = NULL;
    }
    return res;
}

static void win32u_vkUnmapMemory( VkDevice client_device, VkDeviceMemory client_memory )
{
    const VkMemoryUnmapInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR,
        .memory = client_memory,
    };

    win32u_vkUnmapMemory2KHR( client_device, &info );
}

static VkResult win32u_vkCreateBuffer( VkDevice client_device, const VkBufferCreateInfo *create_info,
                                       const VkAllocationCallbacks *allocator, VkBuffer *buffer )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryBufferCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkBufferCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateBuffer( device->host.device, create_info, NULL, buffer );
}

static void win32u_vkGetDeviceBufferMemoryRequirements( VkDevice client_device, const VkDeviceBufferMemoryRequirements *buffer_requirements,
                                                        VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)buffer_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryBufferCreateInfo *external_info;

    TRACE( "device %p, buffer_requirements %p, memory_requirements %p\n", device, buffer_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            external_info = (VkExternalMemoryBufferCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceBufferMemoryRequirements( device->host.device, buffer_requirements, memory_requirements );
}

static void get_physical_device_external_buffer_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalBufferInfo *client_buffer_info,
                                                            VkExternalBufferProperties *buffer_properties, PFN_vkGetPhysicalDeviceExternalBufferProperties p_vkGetPhysicalDeviceExternalBufferProperties )
{
    VkPhysicalDeviceExternalBufferInfo *buffer_info = (VkPhysicalDeviceExternalBufferInfo *)client_buffer_info; /* cast away const, it has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;

    handle_type = buffer_info->handleType;
    buffer_info->handleType = map_external_memory_handle_types( handle_type );

    p_vkGetPhysicalDeviceExternalBufferProperties( physical_device->host.physical_device, buffer_info, buffer_properties );
    buffer_properties->externalMemoryProperties.compatibleHandleTypes = handle_type;
    buffer_properties->externalMemoryProperties.exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalBufferProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferProperties );
}

static void win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalBufferInfo *buffer_info,
                                                                   VkExternalBufferProperties *buffer_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, buffer_info %p, buffer_properties %p\n", physical_device, buffer_info, buffer_properties );

    get_physical_device_external_buffer_properties( physical_device, buffer_info, buffer_properties, instance->p_vkGetPhysicalDeviceExternalBufferPropertiesKHR );
}

static VkResult win32u_vkCreateImage( VkDevice client_device, const VkImageCreateInfo *create_info,
                                      const VkAllocationCallbacks *allocator, VkImage *image )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkExternalMemoryImageCreateInfo host_external_info, *external_info = NULL;

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (physical_device->external_memory_align && !external_info)
    {
        host_external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        host_external_info.pNext = create_info->pNext;
        host_external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        ((VkImageCreateInfo *)create_info)->pNext = &host_external_info; /* cast away const, it has been copied in the thunks */
    }

    return device->p_vkCreateImage( device->host.device, create_info, NULL, image );
}

static void win32u_vkGetDeviceImageMemoryRequirements( VkDevice client_device, const VkDeviceImageMemoryRequirements *image_requirements,
                                                       VkMemoryRequirements2 *memory_requirements )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)image_requirements->pCreateInfo; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExternalMemoryImageCreateInfo *external_info;

    TRACE( "device %p, image_requirements %p, memory_requirements %p\n", device, image_requirements, memory_requirements );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV: break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            external_info = (VkExternalMemoryImageCreateInfo *)*next;
            external_info->handleTypes = map_external_memory_handle_types( external_info->handleTypes );
            break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: break;
        case VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    device->p_vkGetDeviceImageMemoryRequirements( device->host.device, image_requirements, memory_requirements );
}

static VkResult get_physical_device_image_format_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                             VkImageFormatProperties2 *format_properties, PFN_vkGetPhysicalDeviceImageFormatProperties2 p_vkGetPhysicalDeviceImageFormatProperties2 )
{
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)format_info; /* cast away const, chain has been copied in the thunks */
    VkExternalMemoryHandleTypeFlagBits handle_type = 0;
    VkResult res;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT: break;
        case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO: break;
        case VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV: break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO:
        {
            VkPhysicalDeviceExternalImageFormatInfo *external_info = (VkPhysicalDeviceExternalImageFormatInfo *)*next;
            handle_type = external_info->handleType;
            external_info->handleType = map_external_memory_handle_types( handle_type );
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT: break;
        case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    res = p_vkGetPhysicalDeviceImageFormatProperties2( physical_device->host.physical_device, format_info, format_properties );
    for (prev = (VkBaseOutStructure *)format_properties, next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES:
        {
            VkExternalImageFormatProperties *props = (VkExternalImageFormatProperties *)*next;
            props->externalMemoryProperties.compatibleHandleTypes = handle_type;
            props->externalMemoryProperties.exportFromImportedHandleTypes = handle_type;
            break;
        }
        case VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY: break;
        case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT: break;
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES: break;
        case VK_STRUCTURE_TYPE_TEXTURE_LOD_GATHER_FORMAT_PROPERTIES_AMD: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    return res;
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                  VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2 );
}

static VkResult win32u_vkGetPhysicalDeviceImageFormatProperties2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceImageFormatInfo2 *format_info,
                                                                     VkImageFormatProperties2 *format_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, format_info %p, format_properties %p\n", physical_device, format_info, format_properties );

    return get_physical_device_image_format_properties( physical_device, format_info, format_properties, instance->p_vkGetPhysicalDeviceImageFormatProperties2KHR );
}

static VkResult win32u_vkCreateWin32SurfaceKHR( VkInstance client_instance, const VkWin32SurfaceCreateInfoKHR *create_info,
                                                const VkAllocationCallbacks *allocator, VkSurfaceKHR *ret )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    VkSurfaceKHR host_surface;
    struct surface *surface;
    HWND dummy = NULL;
    VkResult res;

    TRACE( "client_instance %p, create_info %p, allocator %p, ret %p\n", client_instance, create_info, allocator, ret );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    if (!(surface = calloc( 1, sizeof(*surface) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* Windows allows surfaces to be created with no HWND, they return VK_ERROR_SURFACE_LOST_KHR later */
    if (!(surface->hwnd = create_info->hwnd))
    {
        static const WCHAR staticW[] = {'s','t','a','t','i','c',0};
        UNICODE_STRING static_us = RTL_CONSTANT_STRING( staticW );
        dummy = NtUserCreateWindowEx( 0, &static_us, NULL, &static_us, WS_POPUP, 0, 0, 0, 0,
                                      NULL, NULL, NULL, NULL, 0, NULL, NULL, FALSE );
        WARN( "Created dummy window %p for null surface window\n", dummy );
        surface->hwnd = dummy;
    }

    if ((res = driver_funcs->p_vulkan_surface_create( surface->hwnd, fshack_enabled, instance,
                                                      &host_surface, &surface->client )))
    {
        if (dummy) NtUserDestroyWindow( dummy );
        free( surface );
        return res;
    }
    add_window_client_surface( surface->hwnd, surface->client );

    vulkan_object_init( &surface->obj.obj, host_surface );
    surface->obj.instance = instance;
    instance->p_insert_object( instance, &surface->obj.obj );

    if (dummy) NtUserDestroyWindow( dummy );

    *ret = surface->obj.client.surface;
    return VK_SUCCESS;
}

static void win32u_vkDestroySurfaceKHR( VkInstance client_instance, VkSurfaceKHR client_surface,
                                        const VkAllocationCallbacks *allocator )
{
    struct vulkan_instance *instance = vulkan_instance_from_handle( client_instance );
    struct surface *surface = surface_from_handle( client_surface );

    if (!surface) return;

    TRACE( "instance %p, handle 0x%s, allocator %p\n", instance, wine_dbgstr_longlong( client_surface ), allocator );
    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );

    instance->p_vkDestroySurfaceKHR( instance->host.instance, surface->obj.host.surface, NULL /* allocator */ );
    client_surface_release( surface->client );

    instance->p_remove_object( instance, &surface->obj.obj );

    free( surface );
}

static BOOL get_surface_rect( HWND hwnd, RECT *rect, UINT dpi )
{
    if (!NtUserGetPresentRect( hwnd, rect, dpi ) && !NtUserGetClientRect( hwnd, rect, dpi )) return FALSE;
    OffsetRect( rect, -rect->left, -rect->top );
    return TRUE;
}

static BOOL get_swapchain_surface_rect( HWND hwnd, RECT *rect, UINT dpi )
{
    if (!get_surface_rect( hwnd, rect, dpi )) return FALSE;
    return !IsRectEmpty( rect );
}

static void adjust_surface_capabilities( struct vulkan_instance *instance, struct surface *surface,
                                         VkSurfaceCapabilitiesKHR *capabilities )
{
    RECT client_rect;

    /* Many Windows games, for example Strange Brigade, No Man's Sky, Path of Exile
     * and World War Z, do not expect that maxImageCount can be set to 0.
     * A value of 0 means that there is no limit on the number of images.
     * Nvidia reports 8 on Windows, AMD 16.
     * https://vulkan.gpuinfo.org/displayreport.php?id=9122#surface
     * https://vulkan.gpuinfo.org/displayreport.php?id=9121#surface
     */
    if (!capabilities->maxImageCount) capabilities->maxImageCount = max( capabilities->minImageCount, 16 );

    /* Update the image extents to match what the Win32 WSI would provide. */
    /* FIXME: handle DPI scaling, somehow */
    get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) );
    capabilities->minImageExtent.width = client_rect.right - client_rect.left;
    capabilities->minImageExtent.height = client_rect.bottom - client_rect.top;
    capabilities->maxImageExtent.width = client_rect.right - client_rect.left;
    capabilities->maxImageExtent.height = client_rect.bottom - client_rect.top;
    capabilities->currentExtent.width = client_rect.right - client_rect.left;
    capabilities->currentExtent.height = client_rect.bottom - client_rect.top;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                  VkSurfaceCapabilitiesKHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device,
                                                       surface->obj.host.surface, capabilities );
    if (!res) adjust_surface_capabilities( instance, surface, capabilities );
    return res;
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                                   VkSurfaceCapabilities2KHR *capabilities )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
    {
        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (surface_info->pNext || capabilities->pNext) FIXME( "Emulating vkGetPhysicalDeviceSurfaceCapabilities2KHR, ignoring pNext.\n" );
        return win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( client_physical_device, surface_info->surface,
                                                                 &capabilities->surfaceCapabilities );
    }

    surface_info_host.surface = surface->obj.host.surface;

    if (!NtUserIsWindow( surface->hwnd )) return VK_ERROR_SURFACE_LOST_KHR;
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilities2KHR( physical_device->host.physical_device,
                                                                     &surface_info_host, capabilities );
    if (!res) adjust_surface_capabilities( instance, surface, &capabilities->surfaceCapabilities );
    return res;
}

static VkResult win32u_vkGetPhysicalDevicePresentRectanglesKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                                uint32_t *rect_count, VkRect2D *rects )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        if (rects && !*rect_count) return VK_INCOMPLETE;
        if (rects) memset( rects, 0, sizeof(VkRect2D) );
        *rect_count = 1;
        return VK_SUCCESS;
    }

    return instance->p_vkGetPhysicalDevicePresentRectanglesKHR( physical_device->host.physical_device,
                                                                   surface->obj.host.surface, rect_count, rects );
}

static void *find_vk_struct( void *s, VkStructureType t )
{
    VkBaseOutStructure *header;

    for (header = s; header; header = header->pNext)
    {
        if (header->sType == t) return header;
    }

    return NULL;
}

static void fixup_device_id_vulkan( UINT *vendor_id, UINT *device_id )
{
    struct pci_id id_real;
    const struct pci_id *id = &id_real;

    id_real.vendor = *vendor_id;
    id_real.device = *device_id;
    fixup_device_id( &id );
    *vendor_id = id->vendor;
    *device_id = id->device;
}

static void get_physical_device_properties2( struct vulkan_physical_device *physical_device, VkPhysicalDeviceProperties2 *properties2,
                                             PFN_vkGetPhysicalDeviceProperties2 p_vkGetPhysicalDeviceProperties2 )
{
    VkPhysicalDeviceIDProperties id_host = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties2_host;
    VkPhysicalDeviceVulkan11Properties *vk11;
    VkPhysicalDeviceIDProperties *id;
    VkBool32 device_luid_valid;
    UINT32 node_mask = 0;
    const GUID *uuid;
    LUID luid;

    vk11 = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES );
    id = find_vk_struct( properties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES );

    if (!vk11 && !id)
    {
        properties2_host = *properties2;
        id_host.pNext = properties2->pNext;
        properties2_host.pNext = &id_host;
        p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, &properties2_host );
        properties2->properties = properties2_host.properties;
    }
    else p_vkGetPhysicalDeviceProperties2( physical_device->host.physical_device, properties2 );

    if (id)        uuid = (const GUID *)id->deviceUUID;
    else if (vk11) uuid = (const GUID *)vk11->deviceUUID;
    else           uuid = (const GUID *)id_host.deviceUUID;

    device_luid_valid = get_gpu_info_from_uuid( uuid, &luid, &node_mask, properties2->properties.deviceName );
    if (!device_luid_valid) WARN( "luid for %s not found\n", debugstr_guid(uuid) );

    if (id)
    {
        if (device_luid_valid) memcpy( &id->deviceLUID, &luid, sizeof(id->deviceLUID) );
        id->deviceLUIDValid = device_luid_valid;
        id->deviceNodeMask = node_mask;
    }

    if (vk11)
    {
        if (device_luid_valid) memcpy( &vk11->deviceLUID, &luid, sizeof(vk11->deviceLUID) );
        vk11->deviceLUIDValid = device_luid_valid;
        vk11->deviceNodeMask = node_mask;
    }

    fixup_device_id_vulkan( &properties2->properties.vendorID, &properties2->properties.deviceID );

    TRACE( "deviceName:%s deviceLUIDValid:%d LUID:%08x:%08x.\n",
           properties2->properties.deviceName, device_luid_valid, luid.HighPart, luid.LowPart );
}

static void win32u_vkGetPhysicalDeviceProperties( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties *properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    VkPhysicalDeviceProperties2 properties2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };

    if (!physical_device->instance->extensions.has_VK_KHR_get_physical_device_properties2)
    {
        physical_device->instance->p_vkGetPhysicalDeviceProperties( physical_device->host.physical_device, properties );
        return;
    }
    get_physical_device_properties2( physical_device, &properties2,
                                     physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
    *properties = properties2.properties;
}

static void win32u_vkGetPhysicalDeviceProperties2( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2 );
}

static void win32u_vkGetPhysicalDeviceProperties2KHR( VkPhysicalDevice client_physical_device, VkPhysicalDeviceProperties2 *properties2 )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );

    get_physical_device_properties2( physical_device, properties2, physical_device->instance->p_vkGetPhysicalDeviceProperties2KHR );
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( VkPhysicalDevice client_physical_device, VkSurfaceKHR client_surface,
                                                             uint32_t *format_count, VkSurfaceFormatKHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( client_surface );
    struct vulkan_instance *instance = physical_device->instance;

    return instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device,
                                                             surface->obj.host.surface, format_count, formats );
}

static VkResult win32u_vkGetPhysicalDeviceSurfaceFormats2KHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceSurfaceInfo2KHR *surface_info,
                                                              uint32_t *format_count, VkSurfaceFormat2KHR *formats )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct surface *surface = surface_from_handle( surface_info->surface );
    VkPhysicalDeviceSurfaceInfo2KHR surface_info_host = *surface_info;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;

    if (!instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR)
    {
        VkSurfaceFormatKHR *surface_formats;
        UINT i;

        /* Until the loader version exporting this function is common, emulate it using the older non-2 version. */
        if (surface_info->pNext) FIXME( "Emulating vkGetPhysicalDeviceSurfaceFormats2KHR, ignoring pNext.\n" );
        if (!formats) return win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, NULL );

        surface_formats = calloc( *format_count, sizeof(*surface_formats) );
        if (!surface_formats) return VK_ERROR_OUT_OF_HOST_MEMORY;

        res = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR( client_physical_device, surface_info->surface, format_count, surface_formats );
        if (!res || res == VK_INCOMPLETE) for (i = 0; i < *format_count; i++) formats[i].surfaceFormat = surface_formats[i];

        free( surface_formats );
        return res;
    }

    surface_info_host.surface = surface->obj.host.surface;

    return instance->p_vkGetPhysicalDeviceSurfaceFormats2KHR( physical_device->host.physical_device,
                                                                 &surface_info_host, format_count, formats );
}

static VkBool32 win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR( VkPhysicalDevice client_physical_device, uint32_t queue )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static BOOL extents_equals( const VkExtent2D *extents, const RECT *rect )
{
    return extents->width == rect->right - rect->left && extents->height == rect->bottom - rect->top;
}

/*
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D texSampler;
layout(binding = 1) uniform writeonly image2D outImage;
layout(push_constant) uniform pushConstants {
    //both in real image coords
    vec2 offset;
    vec2 extents;
} constants;

void main()
{
    vec2 texcoord = (vec2(gl_GlobalInvocationID.xy) - constants.offset) / constants.extents;
    vec4 c = texture(texSampler, texcoord);

    // Convert linear -> srgb
    bvec3 isLo = lessThanEqual(c.rgb, vec3(0.0031308f));
    vec3 loPart = c.rgb * 12.92f;
    vec3 hiPart = pow(c.rgb, vec3(5.0f / 12.0f)) * 1.055f - 0.055f;
    c.rgb = mix(hiPart, loPart, isLo);

    imageStore(outImage, ivec2(gl_GlobalInvocationID.xy), c);
}

*/
const uint32_t blit_comp_spv[] =
{
    0x07230203, 0x00010000, 0x0008000a, 0x0000005e, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000038, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d,
    0x00060010, 0x00000004, 0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002,
    0x000001cc, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x63786574,
    0x64726f6f, 0x00000000, 0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f,
    0x496e6f69, 0x00000044, 0x00060005, 0x00000012, 0x68737570, 0x736e6f43, 0x746e6174, 0x00000073,
    0x00050006, 0x00000012, 0x00000000, 0x7366666f, 0x00007465, 0x00050006, 0x00000012, 0x00000001,
    0x65747865, 0x0073746e, 0x00050005, 0x00000014, 0x736e6f63, 0x746e6174, 0x00000073, 0x00030005,
    0x00000021, 0x00000063, 0x00050005, 0x00000025, 0x53786574, 0x6c706d61, 0x00007265, 0x00040005,
    0x0000002d, 0x6f4c7369, 0x00000000, 0x00040005, 0x00000035, 0x61506f6c, 0x00007472, 0x00040005,
    0x0000003a, 0x61506968, 0x00007472, 0x00050005, 0x00000055, 0x4974756f, 0x6567616d, 0x00000000,
    0x00040047, 0x0000000d, 0x0000000b, 0x0000001c, 0x00050048, 0x00000012, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x00000012, 0x00000001, 0x00000023, 0x00000008, 0x00030047, 0x00000012,
    0x00000002, 0x00040047, 0x00000025, 0x00000022, 0x00000000, 0x00040047, 0x00000025, 0x00000021,
    0x00000000, 0x00040047, 0x00000055, 0x00000022, 0x00000000, 0x00040047, 0x00000055, 0x00000021,
    0x00000001, 0x00030047, 0x00000055, 0x00000019, 0x00040047, 0x0000005d, 0x0000000b, 0x00000019,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007,
    0x00040015, 0x0000000a, 0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003,
    0x00040020, 0x0000000c, 0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001,
    0x00040017, 0x0000000e, 0x0000000a, 0x00000002, 0x0004001e, 0x00000012, 0x00000007, 0x00000007,
    0x00040020, 0x00000013, 0x00000009, 0x00000012, 0x0004003b, 0x00000013, 0x00000014, 0x00000009,
    0x00040015, 0x00000015, 0x00000020, 0x00000001, 0x0004002b, 0x00000015, 0x00000016, 0x00000000,
    0x00040020, 0x00000017, 0x00000009, 0x00000007, 0x0004002b, 0x00000015, 0x0000001b, 0x00000001,
    0x00040017, 0x0000001f, 0x00000006, 0x00000004, 0x00040020, 0x00000020, 0x00000007, 0x0000001f,
    0x00090019, 0x00000022, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x00000023, 0x00000022, 0x00040020, 0x00000024, 0x00000000, 0x00000023,
    0x0004003b, 0x00000024, 0x00000025, 0x00000000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000,
    0x00020014, 0x0000002a, 0x00040017, 0x0000002b, 0x0000002a, 0x00000003, 0x00040020, 0x0000002c,
    0x00000007, 0x0000002b, 0x00040017, 0x0000002e, 0x00000006, 0x00000003, 0x0004002b, 0x00000006,
    0x00000031, 0x3b4d2e1c, 0x0006002c, 0x0000002e, 0x00000032, 0x00000031, 0x00000031, 0x00000031,
    0x00040020, 0x00000034, 0x00000007, 0x0000002e, 0x0004002b, 0x00000006, 0x00000038, 0x414eb852,
    0x0004002b, 0x00000006, 0x0000003d, 0x3ed55555, 0x0006002c, 0x0000002e, 0x0000003e, 0x0000003d,
    0x0000003d, 0x0000003d, 0x0004002b, 0x00000006, 0x00000040, 0x3f870a3d, 0x0004002b, 0x00000006,
    0x00000042, 0x3d6147ae, 0x0004002b, 0x0000000a, 0x00000049, 0x00000000, 0x00040020, 0x0000004a,
    0x00000007, 0x00000006, 0x0004002b, 0x0000000a, 0x0000004d, 0x00000001, 0x0004002b, 0x0000000a,
    0x00000050, 0x00000002, 0x00090019, 0x00000053, 0x00000006, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000002, 0x00000000, 0x00040020, 0x00000054, 0x00000000, 0x00000053, 0x0004003b,
    0x00000054, 0x00000055, 0x00000000, 0x00040017, 0x00000059, 0x00000015, 0x00000002, 0x0004002b,
    0x0000000a, 0x0000005c, 0x00000008, 0x0006002c, 0x0000000b, 0x0000005d, 0x0000005c, 0x0000005c,
    0x0000004d, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000020, 0x00000021, 0x00000007,
    0x0004003b, 0x0000002c, 0x0000002d, 0x00000007, 0x0004003b, 0x00000034, 0x00000035, 0x00000007,
    0x0004003b, 0x00000034, 0x0000003a, 0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d,
    0x0007004f, 0x0000000e, 0x00000010, 0x0000000f, 0x0000000f, 0x00000000, 0x00000001, 0x00040070,
    0x00000007, 0x00000011, 0x00000010, 0x00050041, 0x00000017, 0x00000018, 0x00000014, 0x00000016,
    0x0004003d, 0x00000007, 0x00000019, 0x00000018, 0x00050083, 0x00000007, 0x0000001a, 0x00000011,
    0x00000019, 0x00050041, 0x00000017, 0x0000001c, 0x00000014, 0x0000001b, 0x0004003d, 0x00000007,
    0x0000001d, 0x0000001c, 0x00050088, 0x00000007, 0x0000001e, 0x0000001a, 0x0000001d, 0x0003003e,
    0x00000009, 0x0000001e, 0x0004003d, 0x00000023, 0x00000026, 0x00000025, 0x0004003d, 0x00000007,
    0x00000027, 0x00000009, 0x00070058, 0x0000001f, 0x00000029, 0x00000026, 0x00000027, 0x00000002,
    0x00000028, 0x0003003e, 0x00000021, 0x00000029, 0x0004003d, 0x0000001f, 0x0000002f, 0x00000021,
    0x0008004f, 0x0000002e, 0x00000030, 0x0000002f, 0x0000002f, 0x00000000, 0x00000001, 0x00000002,
    0x000500bc, 0x0000002b, 0x00000033, 0x00000030, 0x00000032, 0x0003003e, 0x0000002d, 0x00000033,
    0x0004003d, 0x0000001f, 0x00000036, 0x00000021, 0x0008004f, 0x0000002e, 0x00000037, 0x00000036,
    0x00000036, 0x00000000, 0x00000001, 0x00000002, 0x0005008e, 0x0000002e, 0x00000039, 0x00000037,
    0x00000038, 0x0003003e, 0x00000035, 0x00000039, 0x0004003d, 0x0000001f, 0x0000003b, 0x00000021,
    0x0008004f, 0x0000002e, 0x0000003c, 0x0000003b, 0x0000003b, 0x00000000, 0x00000001, 0x00000002,
    0x0007000c, 0x0000002e, 0x0000003f, 0x00000001, 0x0000001a, 0x0000003c, 0x0000003e, 0x0005008e,
    0x0000002e, 0x00000041, 0x0000003f, 0x00000040, 0x00060050, 0x0000002e, 0x00000043, 0x00000042,
    0x00000042, 0x00000042, 0x00050083, 0x0000002e, 0x00000044, 0x00000041, 0x00000043, 0x0003003e,
    0x0000003a, 0x00000044, 0x0004003d, 0x0000002e, 0x00000045, 0x0000003a, 0x0004003d, 0x0000002e,
    0x00000046, 0x00000035, 0x0004003d, 0x0000002b, 0x00000047, 0x0000002d, 0x000600a9, 0x0000002e,
    0x00000048, 0x00000047, 0x00000046, 0x00000045, 0x00050041, 0x0000004a, 0x0000004b, 0x00000021,
    0x00000049, 0x00050051, 0x00000006, 0x0000004c, 0x00000048, 0x00000000, 0x0003003e, 0x0000004b,
    0x0000004c, 0x00050041, 0x0000004a, 0x0000004e, 0x00000021, 0x0000004d, 0x00050051, 0x00000006,
    0x0000004f, 0x00000048, 0x00000001, 0x0003003e, 0x0000004e, 0x0000004f, 0x00050041, 0x0000004a,
    0x00000051, 0x00000021, 0x00000050, 0x00050051, 0x00000006, 0x00000052, 0x00000048, 0x00000002,
    0x0003003e, 0x00000051, 0x00000052, 0x0004003d, 0x00000053, 0x00000056, 0x00000055, 0x0004003d,
    0x0000000b, 0x00000057, 0x0000000d, 0x0007004f, 0x0000000e, 0x00000058, 0x00000057, 0x00000057,
    0x00000000, 0x00000001, 0x0004007c, 0x00000059, 0x0000005a, 0x00000058, 0x0004003d, 0x0000001f,
    0x0000005b, 0x00000021, 0x00040063, 0x00000056, 0x0000005a, 0x0000005b, 0x000100fd, 0x00010038,
};

static VkResult create_pipeline( struct vulkan_device *device, struct swapchain *swapchain, VkShaderModule shaderModule )
{
    VkComputePipelineCreateInfo pipelineInfo = {0};
    VkResult res;

    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = swapchain->pipeline_layout;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
    pipelineInfo.basePipelineIndex = -1;

    if ((res = device->p_vkCreateComputePipelines( device->host.device, VK_NULL_HANDLE, 1,
                                                   &pipelineInfo, NULL, &swapchain->pipeline )))
    {
        ERR( "vkCreateComputePipelines: %d\n", res );
        return res;
    }

    return VK_SUCCESS;
}

static VkResult create_descriptor_set( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    VkDescriptorImageInfo userDescriptorImageInfo = {0}, realDescriptorImageInfo = {0};
    VkDescriptorSetAllocateInfo descriptorAllocInfo = {0};
    VkWriteDescriptorSet descriptorWrites[2] = {{0}, {0}};
    VkResult res;

    descriptorAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAllocInfo.descriptorPool = swapchain->descriptor_pool;
    descriptorAllocInfo.descriptorSetCount = 1;
    descriptorAllocInfo.pSetLayouts = &swapchain->descriptor_set_layout;

    if ((res = device->p_vkAllocateDescriptorSets( device->host.device, &descriptorAllocInfo, &hack->descriptor_set )))
    {
        ERR( "vkAllocateDescriptorSets: %d\n", res );
        return res;
    }

    userDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    userDescriptorImageInfo.imageView = hack->user_view;
    userDescriptorImageInfo.sampler = swapchain->sampler;

    realDescriptorImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    realDescriptorImageInfo.imageView = hack->blit_view;

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = hack->descriptor_set;
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pImageInfo = &userDescriptorImageInfo;

    descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[1].dstSet = hack->descriptor_set;
    descriptorWrites[1].dstBinding = 1;
    descriptorWrites[1].dstArrayElement = 0;
    descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorWrites[1].descriptorCount = 1;
    descriptorWrites[1].pImageInfo = &realDescriptorImageInfo;

    device->p_vkUpdateDescriptorSets( device->host.device, 2, descriptorWrites, 0, NULL );
    return VK_SUCCESS;
}

static VkResult init_blit_images( struct vulkan_device *device, struct swapchain *swapchain )
{
    VkResult res;
    VkSamplerCreateInfo samplerInfo = {0};
    VkDescriptorPoolSize poolSizes[2] = {{0}, {0}};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    VkDescriptorSetLayoutBinding layoutBindings[2] = {{0}, {0}};
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo = {0};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
    VkPushConstantRange pushConstants;
    VkShaderModuleCreateInfo shaderInfo = {0};
    VkShaderModule shaderModule = 0;
    VkImageViewCreateInfo viewInfo = {0};
    uint32_t i;

    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = samplerInfo.minFilter = fs_hack_is_integer() ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if ((res = device->p_vkCreateSampler( device->host.device, &samplerInfo, NULL, &swapchain->sampler )))
    {
        WARN( "vkCreateSampler failed, res=%d\n", res );
        return res;
    }

    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = swapchain->n_images;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = swapchain->n_images;

    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = swapchain->n_images;

    if ((res = device->p_vkCreateDescriptorPool( device->host.device, &poolInfo, NULL, &swapchain->descriptor_pool )))
    {
        ERR( "vkCreateDescriptorPool: %d\n", res );
        goto fail;
    }

    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].pImmutableSamplers = NULL;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    layoutBindings[1].pImmutableSamplers = NULL;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 2;
    descriptorLayoutInfo.pBindings = layoutBindings;

    if ((res = device->p_vkCreateDescriptorSetLayout( device->host.device, &descriptorLayoutInfo,
                                                      NULL, &swapchain->descriptor_set_layout )))
    {
        ERR( "vkCreateDescriptorSetLayout: %d\n", res );
        goto fail;
    }

    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.offset = 0;
    pushConstants.size = 4 * sizeof(float); /* 2 * vec2 */

    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &swapchain->descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;

    if ((res = device->p_vkCreatePipelineLayout( device->host.device, &pipelineLayoutInfo, NULL,
                                                 &swapchain->pipeline_layout )))
    {
        ERR( "vkCreatePipelineLayout: %d\n", res );
        goto fail;
    }

    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(blit_comp_spv);
    shaderInfo.pCode = blit_comp_spv;

    if ((res = device->p_vkCreateShaderModule( device->host.device, &shaderInfo, NULL, &shaderModule )))
    {
        ERR( "vkCreateShaderModule: %d\n", res );
        goto fail;
    }

    if ((res = create_pipeline( device, swapchain, shaderModule ))) goto fail;

    device->p_vkDestroyShaderModule( device->host.device, shaderModule, NULL );

    for (i = 0; i < swapchain->n_images; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = hack->swapchain_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL, &hack->blit_view )))
        {
            ERR( "vkCreateImageView(blit): %d\n", res );
            goto fail;
        }

        if ((res = create_descriptor_set( device, swapchain, hack ))) goto fail;
    }

    return VK_SUCCESS;

fail:
    for (i = 0; i < swapchain->n_images; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        device->p_vkDestroyImageView( device->host.device, hack->blit_view, NULL );
        hack->blit_view = VK_NULL_HANDLE;
    }

    device->p_vkDestroyShaderModule( device->host.device, shaderModule, NULL );

    device->p_vkDestroyPipeline( device->host.device, swapchain->pipeline, NULL );
    swapchain->pipeline = VK_NULL_HANDLE;

    device->p_vkDestroyPipelineLayout( device->host.device, swapchain->pipeline_layout, NULL );
    swapchain->pipeline_layout = VK_NULL_HANDLE;

    device->p_vkDestroyDescriptorSetLayout( device->host.device, swapchain->descriptor_set_layout, NULL );
    swapchain->descriptor_set_layout = VK_NULL_HANDLE;

    device->p_vkDestroyDescriptorPool( device->host.device, swapchain->descriptor_pool, NULL );
    swapchain->descriptor_pool = VK_NULL_HANDLE;

    device->p_vkDestroySampler( device->host.device, swapchain->sampler, NULL );
    swapchain->sampler = VK_NULL_HANDLE;

    return res;
}

static void destroy_fs_hack_image( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    device->p_vkDestroyImageView( device->host.device, hack->user_view, NULL );
    device->p_vkDestroyImageView( device->host.device, hack->blit_view, NULL );
    device->p_vkDestroyImage( device->host.device, hack->user_image, NULL );
    if (hack->cmd) device->p_vkFreeCommandBuffers( device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
    device->p_vkDestroySemaphore( device->host.device, hack->blit_finished, NULL );
}

static VkResult init_fs_hack_images( struct vulkan_device *device, struct swapchain *swapchain,
                                     const VkSwapchainCreateInfoKHR *createinfo )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkResult res;
    VkImage *real_images = NULL;
    VkDeviceSize userMemTotal = 0, offs;
    VkImageCreateInfo imageInfo = {0};
    VkSemaphoreCreateInfo semaphoreInfo = {0};
    VkMemoryRequirements userMemReq;
    VkMemoryAllocateInfo allocInfo = {0};
    VkPhysicalDeviceMemoryProperties memProperties;
    VkImageViewCreateInfo viewInfo = {0};
    uint32_t count, i = 0, user_memory_type = -1;

    if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &count, NULL )))
    {
        WARN( "vkGetSwapchainImagesKHR failed, res=%d\n", res );
        return res;
    }

    real_images = malloc( count * sizeof(VkImage) );
    swapchain->cmd_pools = calloc( device->queue_count, sizeof(VkCommandPool) );
    swapchain->fs_hack_images = calloc( count, sizeof(struct fs_hack_image) );
    if (!real_images || !swapchain->cmd_pools || !swapchain->fs_hack_images) goto fail;

    if ((res = device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, &count, real_images )))
    {
        WARN( "vkGetSwapchainImagesKHR failed, res=%d\n", res );
        goto fail;
    }

    /* create user images */
    for (i = 0; i < count; ++i)
    {
        struct fs_hack_image *hack = &swapchain->fs_hack_images[i];

        hack->swapchain_image = real_images[i];

        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if ((res = device->p_vkCreateSemaphore( device->host.device, &semaphoreInfo, NULL, &hack->blit_finished )))
        {
            WARN( "vkCreateSemaphore failed, res=%d\n", res );
            goto fail;
        }

        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = swapchain->extents.width;
        imageInfo.extent.height = swapchain->extents.height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = createinfo->imageArrayLayers;
        imageInfo.format = createinfo->imageFormat;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = createinfo->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = createinfo->imageSharingMode;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.queueFamilyIndexCount = createinfo->queueFamilyIndexCount;
        imageInfo.pQueueFamilyIndices = createinfo->pQueueFamilyIndices;

        if (createinfo->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        else if (createinfo->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
            imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

        if ((res = device->p_vkCreateImage( device->host.device, &imageInfo, NULL, &hack->user_image )))
        {
            ERR( "vkCreateImage failed: %d\n", res );
            goto fail;
        }

        device->p_vkGetImageMemoryRequirements( device->host.device, hack->user_image, &userMemReq );

        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;

        userMemTotal += userMemReq.size;

        swapchain->n_images++;
    }

    /* allocate backing memory */
    instance->p_vkGetPhysicalDeviceMemoryProperties( physical_device->host.physical_device, &memProperties );

    for (i = 0; i < memProperties.memoryTypeCount; i++)
    {
        UINT flag = memProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (flag == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        {
            if (userMemReq.memoryTypeBits & (1 << i))
            {
                user_memory_type = i;
                break;
            }
        }
    }

    if (user_memory_type == -1)
    {
        ERR( "unable to find suitable memory type\n" );
        res = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto fail;
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = userMemTotal;
    allocInfo.memoryTypeIndex = user_memory_type;

    if ((res = device->p_vkAllocateMemory( device->host.device, &allocInfo, NULL, &swapchain->user_image_memory )))
    {
        ERR( "vkAllocateMemory: %d\n", res );
        goto fail;
    }

    /* bind backing memory and create imageviews */
    userMemTotal = 0;
    for (i = 0; i < count; ++i)
    {
        device->p_vkGetImageMemoryRequirements( device->host.device, swapchain->fs_hack_images[i].user_image, &userMemReq );

        offs = userMemTotal % userMemReq.alignment;
        if (offs) userMemTotal += userMemReq.alignment - offs;

        if ((res = device->p_vkBindImageMemory( device->host.device, swapchain->fs_hack_images[i].user_image,
                                                swapchain->user_image_memory, userMemTotal )))
        {
            ERR( "vkBindImageMemory: %d\n", res );
            goto fail;
        }

        userMemTotal += userMemReq.size;

        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain->fs_hack_images[i].user_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_SRGB;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ((res = device->p_vkCreateImageView( device->host.device, &viewInfo, NULL,
                                                &swapchain->fs_hack_images[i].user_view )))
        {
            ERR( "vkCreateImageView(user): %d\n", res );
            goto fail;
        }
    }

    free( real_images );

    return VK_SUCCESS;

fail:
    for (i = 0; i < swapchain->n_images; ++i) destroy_fs_hack_image( device, swapchain, &swapchain->fs_hack_images[i] );
    free( real_images );
    free( swapchain->cmd_pools );
    free( swapchain->fs_hack_images );
    return res;
}

static VkResult win32u_vkLatencySleepNV( VkDevice device, VkSwapchainKHR swapchain,
                                         const VkLatencySleepInfoNV *sleep_info )
{
    struct vulkan_device *vk_device = vulkan_device_from_handle( device );
    struct swapchain *vk_swapchain = swapchain_from_handle( swapchain );
    VkLatencySleepInfoNV sleep_info_host = *sleep_info;
    struct vulkan_semaphore *semaphore;

    if (!vk_swapchain) return VK_ERROR_OUT_OF_DATE_KHR;
    if (vk_swapchain->managed) return VK_SUCCESS;

    semaphore = sleep_info_host.signalSemaphore
            ? vulkan_semaphore_from_handle( sleep_info_host.signalSemaphore ) : NULL;
    sleep_info_host.signalSemaphore = semaphore ? semaphore->host.semaphore : 0;

    return vk_device->p_vkLatencySleepNV( vk_device->host.device,
                                          vk_swapchain->obj.host.swapchain, &sleep_info_host );
}

static void win32u_vkSetLatencyMarkerNV( VkDevice device, VkSwapchainKHR swapchain,
                                         const VkSetLatencyMarkerInfoNV *latency_marker_info )
{
    struct vulkan_device *vk_device = vulkan_device_from_handle( device );
    struct swapchain *vk_swapchain = swapchain_from_handle( swapchain );

    if (!vk_swapchain || vk_swapchain->managed) return;

    vk_device->p_vkSetLatencyMarkerNV( vk_device->host.device,
                                       vk_swapchain->obj.host.swapchain, latency_marker_info );
}

static void win32u_vkGetLatencyTimingsNV( VkDevice device, VkSwapchainKHR swapchain,
                                          VkGetLatencyMarkerInfoNV *latency_marker_info )
{
    struct vulkan_device *vk_device = vulkan_device_from_handle( device );
    struct swapchain *vk_swapchain = swapchain_from_handle( swapchain );

    if (!vk_swapchain || vk_swapchain->managed)
    {
        latency_marker_info->timingCount = 0;
        return;
    }

    vk_device->p_vkGetLatencyTimingsNV( vk_device->host.device,
                                        vk_swapchain->obj.host.swapchain, latency_marker_info );
}

static BOOL surface_get_fshack_dpi( struct surface *surface )
{
    UINT dpi = NtUserGetDpiForWindow( surface->hwnd ), raw = NtUserGetWinMonitorDpi( surface->hwnd, MDT_RAW_DPI );
    return fshack_enabled && dpi != raw ? raw : 0;
}

/* Cross-process producer helpers. */

/* Map a Vulkan swapchain VkFormat to a DRM fourcc. Vulkan B8G8R8A8 in memory
 * reads as a little-endian word ARGB and maps to the DRM *RGB* fourccs. Opaque
 * swapchains use the X-variant (ignore alpha). Returns 0 if unsupported. */
static unsigned int vk_format_to_drm_fourcc( VkFormat format, BOOL opaque )
{
    switch (format)
    {
    /* Vulkan BGRA in memory -> DRM *RGB* (little-endian word ARGB). */
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return opaque ? 0x34325258 /* XRGB8888 'XR24' */ : 0x34325241 /* ARGB8888 'AR24' */;
    /* Vulkan RGBA in memory -> DRM *BGR* (little-endian word ABGR). */
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return opaque ? 0x34324258 /* XBGR8888 'XB24' */ : 0x34324241 /* ABGR8888 'AB24' */;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return opaque ? 0x30334258 /* XB30 */ : 0x30334241 /* AB30 */;
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return opaque ? 0x30335258 /* XR30 */ : 0x30335241 /* AR30 */;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 0x48344241 /* ABGR16161616F 'AB4H' */;
    default:
        return 0;
    }
}


/* Memory plane count of (format, modifier) or 0 if unknown. Tiled modifiers
 * may carry an auxiliary plane (e.g. AMD DCC) that must also be published. */
static uint32_t vk_modifier_plane_count( struct vulkan_device *device, VkFormat format,
                                         uint64_t modifier )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    PFN_vkGetPhysicalDeviceFormatProperties2 p_get_format_props;
    VkDrmFormatModifierPropertiesListEXT mod_list =
    {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &mod_list };
    VkDrmFormatModifierPropertiesEXT *mods;
    uint32_t i, count = 0;

    p_get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties2)
        p_vkGetInstanceProcAddr( instance->host.instance, "vkGetPhysicalDeviceFormatProperties2" );
    if (!p_get_format_props)
        p_get_format_props = (PFN_vkGetPhysicalDeviceFormatProperties2)
            p_vkGetInstanceProcAddr( instance->host.instance, "vkGetPhysicalDeviceFormatProperties2KHR" );
    if (!p_get_format_props) return 0;

    p_get_format_props( physical_device->host.physical_device, format, &props );
    if (!mod_list.drmFormatModifierCount) return 0;
    if (!(mods = calloc( mod_list.drmFormatModifierCount, sizeof(*mods) ))) return 0;
    mod_list.pDrmFormatModifierProperties = mods;
    p_get_format_props( physical_device->host.physical_device, format, &props );

    for (i = 0; i < mod_list.drmFormatModifierCount; i++)
    {
        if (mods[i].drmFormatModifier != modifier) continue;
        count = mods[i].drmFormatModifierPlaneCount;
        break;
    }
    free( mods );
    return count;
}

static BOOL vk_host_image_exportable( struct vulkan_device *device, VkFormat format,
                                      VkImageUsageFlags usage, VkImageTiling tiling,
                                      const void *tiling_info,
                                      VkExternalMemoryHandleTypeFlagBits handle_type )
{
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkPhysicalDeviceExternalImageFormatInfo external_info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = tiling_info,
        .handleType = handle_type,
    };
    VkPhysicalDeviceImageFormatInfo2 format_info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = tiling,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_props =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 format_props =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props,
    };

    if (instance->p_vkGetPhysicalDeviceImageFormatProperties2( physical_device->host.physical_device,
                                                               &format_info, &format_props ))
        return FALSE;
    if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        return FALSE;
    return TRUE;
}

/* Query whether the host can export an image of (format, modifier) as a
 * DMA_BUF with the requested usage. Multi-plane modifiers are fine. */
static BOOL vk_host_modifier_exportable( struct vulkan_device *device, VkFormat format,
                                         VkImageUsageFlags usage, uint64_t modifier )
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info =
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    return vk_host_image_exportable( device, format, usage,
                                     VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, &mod_info,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT );
}

static BOOL vk_host_linear_exportable( struct vulkan_device *device, VkFormat format,
                                       VkImageUsageFlags usage )
{
    return vk_host_image_exportable( device, format, usage, VK_IMAGE_TILING_LINEAR, NULL,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT );
}

static uint32_t vk_collect_managed_modifiers( struct vulkan_device *device, VkFormat format,
                                              VkImageUsageFlags usage,
                                              const hwnd_dmabuf_format_modifier_t *caps_mods,
                                              UINT caps_count, unsigned int fourcc,
                                              enum wine_managed_image_layout layout,
                                              uint64_t *out_mods, uint64_t *out_wire_mods,
                                              uint32_t max_out )
{
    UINT best_tranche = ~0u, i;
    uint32_t out_count = 0;

    for (i = 0; i < caps_count; i++)
    {
        uint64_t wire_modifier = caps_mods[i].modifier;
        uint64_t modifier = wire_modifier;

        if (caps_mods[i].fourcc != fourcc) continue;
        if (layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR)
        {
            if (wire_modifier != 0 /* DRM_FORMAT_MOD_LINEAR */) continue;
        }
        else
        {
            if (modifier == WINE_VK_DRM_FORMAT_MOD_INVALID)
                modifier = 0 /* DRM_FORMAT_MOD_LINEAR */;
            if (!vk_host_modifier_exportable( device, format, usage, modifier )) continue;
        }
        if (caps_mods[i].tranche_index < best_tranche)
            best_tranche = caps_mods[i].tranche_index;
    }

    if (best_tranche == ~0u) return 0;

    for (i = 0; i < caps_count && out_count < max_out; i++)
    {
        uint64_t wire_modifier = caps_mods[i].modifier;
        uint64_t modifier = wire_modifier;

        if (caps_mods[i].fourcc != fourcc || caps_mods[i].tranche_index != best_tranche)
            continue;
        if (layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR)
        {
            if (wire_modifier != 0 /* DRM_FORMAT_MOD_LINEAR */) continue;
        }
        else
        {
            /* MOD_INVALID means "any/implicit". Let the host pick by offering LINEAR. */
            if (modifier == WINE_VK_DRM_FORMAT_MOD_INVALID)
                modifier = 0 /* DRM_FORMAT_MOD_LINEAR */;
            if (!vk_host_modifier_exportable( device, format, usage, modifier )) continue;
        }
        /* dedupe */
        {
            uint32_t j;
            BOOL dup = FALSE;
            for (j = 0; j < out_count; j++) if (out_mods[j] == modifier) { dup = TRUE; break; }
            if (dup) continue;
        }
        out_mods[out_count] = modifier;
        out_wire_mods[out_count] = wire_modifier;
        out_count++;
    }

    return out_count;
}

/* Build the candidate modifier list = caps modifiers (matching fourcc) that the
 * host can also export as a single-plane dmabuf. out_mods is what Vulkan gets,
 * out_wire_mods is what the consumer must see. Returns the count placed in
 * out_mods. 0 means no intersection -> caller falls back to the host swapchain. */
static uint32_t vk_select_managed_modifiers( struct vulkan_device *device, HWND hwnd, VkFormat format,
                                             VkImageUsageFlags usage, BOOL opaque, unsigned int *fourcc_out,
                                             enum wine_managed_image_layout layout,
                                             unsigned int *caps_flags_out,
                                             uint64_t *out_mods, uint64_t *out_wire_mods, uint32_t max_out )
{
    hwnd_dmabuf_format_modifier_t *caps_mods;
    hwnd_dmabuf_host_caps_t caps = {0};
    unsigned int fourcc_first, fourcc_second;
    UINT caps_count = 0;
    uint32_t out_count = 0;

    *caps_flags_out = 0;

    /* The fourcc must match compositeAlpha: opaque swapchains use the X-variant
     * (compositor ignores alpha), blended ones the A-variant. Try the matching
     * variant first, then the other. */
    if (layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR &&
        !vk_host_linear_exportable( device, format, usage ))
        return 0;

    fourcc_first = vk_format_to_drm_fourcc( format, opaque );
    fourcc_second = layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR ? 0 :
                    vk_format_to_drm_fourcc( format, !opaque );
    if (!fourcc_first && !fourcc_second) return 0;

    /* The compositor can advertise hundreds of (fourcc, modifier) pairs. Query
     * the real count and heap-allocate so a fixed cap cannot drop the fourcc or
     * LINEAR we need. The two-call probe also gates: an on-screen window returns
     * HWND_DMABUF_NOT_FOUND. */
    if (hwnd_dmabuf_get_caps( hwnd, &caps, NULL, 0, &caps_count ) != HWND_DMABUF_OK || !caps_count)
        return 0;
    if (!(caps_mods = calloc( caps_count, sizeof(*caps_mods) ))) return 0;
    memset( &caps, 0, sizeof(caps) );
    if (hwnd_dmabuf_get_caps( hwnd, &caps, caps_mods, caps_count, &caps_count ) != HWND_DMABUF_OK)
    {
        free( caps_mods );
        return 0;
    }
    if (!(caps_count = caps.format_modifier_count))
    {
        free( caps_mods );
        return 0;
    }
    *caps_flags_out = caps.flags;

    *fourcc_out = 0;
    if (fourcc_first)
    {
        out_count = vk_collect_managed_modifiers( device, format, usage, caps_mods, caps_count,
                                                  fourcc_first, layout, out_mods, out_wire_mods, max_out );
        if (out_count) *fourcc_out = fourcc_first;
    }
    if (!out_count && fourcc_second)
    {
        out_count = vk_collect_managed_modifiers( device, format, usage, caps_mods, caps_count,
                                                  fourcc_second, layout, out_mods, out_wire_mods, max_out );
        if (out_count) *fourcc_out = fourcc_second;
    }

    free( caps_mods );
    return out_count;
}

static struct wine_managed_consumer *managed_consumer_get( HWND hwnd )
{
    struct wine_managed_consumer *consumer;

    pthread_mutex_lock( &managed_consumers_lock );
    LIST_FOR_EACH_ENTRY( consumer, &managed_consumers, struct wine_managed_consumer, entry )
    {
        if (consumer->hwnd != hwnd) continue;
        consumer->refcount++;
        pthread_mutex_unlock( &managed_consumers_lock );
        return consumer;
    }

    if ((consumer = calloc( 1, sizeof(*consumer) )))
    {
        consumer->hwnd = hwnd;
        consumer->refcount = 1;
        list_add_tail( &managed_consumers, &consumer->entry );
    }
    pthread_mutex_unlock( &managed_consumers_lock );
    return consumer;
}

static void managed_consumer_put( struct wine_managed_consumer *consumer )
{
    if (!consumer) return;

    pthread_mutex_lock( &managed_consumers_lock );
    if (!--consumer->refcount)
    {
        list_remove( &consumer->entry );
        free( consumer );
    }
    pthread_mutex_unlock( &managed_consumers_lock );
}

static enum wine_managed_consumer_state managed_consumer_state( struct wine_managed_swapchain *managed )
{
    if (managed->discard) return WINE_MANAGED_CONSUMER_SUSPENDED;
    return ReadAcquire( &managed->consumer->state );
}

static void managed_consumer_set_state( struct wine_managed_swapchain *managed,
                                        enum wine_managed_consumer_state state )
{
    InterlockedExchange( &managed->consumer->state, state );
    InterlockedExchange( &managed->consumer->reannounce_pending, FALSE );
    InterlockedExchange( &managed->consumer->reannounce_delay_ms, 0 );
}

static void managed_consumer_request_state( struct wine_managed_swapchain *managed )
{
    struct wine_managed_consumer *consumer = managed->consumer;
    LONG delay, last, now = NtGetTickCount();

    if (managed_consumer_state( managed ) != WINE_MANAGED_CONSUMER_UNKNOWN) return;
    if (!InterlockedExchange( &consumer->reannounce_pending, TRUE ))
    {
        InterlockedExchange( &consumer->last_reannounce_ms, now );
        InterlockedExchange( &consumer->reannounce_delay_ms,
                             WINE_VK_MANAGED_REANNOUNCE_MIN_MS );
        hwnd_dmabuf_post_wake( managed->hwnd, HWND_DMABUF_WAKE_REANNOUNCE );
        return;
    }

    delay = ReadAcquire( &consumer->reannounce_delay_ms );
    if (delay < WINE_VK_MANAGED_REANNOUNCE_MIN_MS)
        delay = WINE_VK_MANAGED_REANNOUNCE_MIN_MS;
    last = ReadAcquire( &consumer->last_reannounce_ms );
    if ((DWORD)(now - last) < delay) return;
    if (InterlockedCompareExchange( &consumer->last_reannounce_ms, now, last ) == last)
    {
        delay = min( delay * 2, WINE_VK_MANAGED_REANNOUNCE_MAX_MS );
        InterlockedExchange( &consumer->reannounce_delay_ms, delay );
        hwnd_dmabuf_post_wake( managed->hwnd, HWND_DMABUF_WAKE_REANNOUNCE );
    }
}

static void managed_destroy_image( struct vulkan_device *device, struct wine_managed_image *image )
{
    if (image->dmabuf_fd >= 0) { close( image->dmabuf_fd ); image->dmabuf_fd = -1; }
    if (image->image) { device->p_vkDestroyImage( device->host.device, image->image, NULL ); image->image = VK_NULL_HANDLE; }
    if (image->memory) { device->p_vkFreeMemory( device->host.device, image->memory, NULL ); image->memory = VK_NULL_HANDLE; }
    image->valid = FALSE;
    image->acquired = image->busy = FALSE;
}

static void managed_free( struct vulkan_device *device, struct wine_managed_swapchain *managed )
{
    uint32_t i;

    if (!managed) return;
    /* No wait: the app idles the swapchain before destroy. The dmabuf stays
     * alive for the compositor (kernel-refcounted). */
    for (i = 0; i < managed->image_count; i++)
        managed_destroy_image( device, &managed->images[i] );
    if (managed->present_fence) device->p_vkDestroyFence( device->host.device, managed->present_fence, NULL );
    if (managed->channel_fd >= 0) close( managed->channel_fd );
    if (managed->channel_registered)
        hwnd_dmabuf_release_channel( managed->hwnd );
    if (managed->pending_registered)
        hwnd_dmabuf_set_pending( managed->hwnd, FALSE );
    managed_consumer_put( managed->consumer );
    pthread_mutex_destroy( &managed->lock );
    free( managed );
}

static uint32_t managed_find_memory_type( struct vulkan_device *device,
                                          uint32_t type_bits,
                                          VkMemoryPropertyFlags required )
{
    const VkPhysicalDeviceMemoryProperties *properties =
            &device->physical_device->memory_properties;
    uint32_t fallback = UINT32_MAX, i;

    for (i = 0; i < properties->memoryTypeCount; i++)
    {
        VkMemoryPropertyFlags flags = properties->memoryTypes[i].propertyFlags;

        if (!(type_bits & (1u << i)) || (flags & required) != required) continue;
        if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

/* Keep an inactive Windows presenter rendering without exposing another host
 * WSI stream for the same HWND. */
static VkResult managed_create_discard_image( struct vulkan_device *device,
                                              const VkSwapchainCreateInfoKHR *create_info,
                                              struct wine_managed_image *image )
{
    const VkImageFormatListCreateInfo *format_list;
    VkImageFormatListCreateInfo format_list_info;
    VkImageCreateInfo image_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = create_info->imageFormat,
        .extent = {create_info->imageExtent.width, create_info->imageExtent.height, 1},
        .mipLevels = 1,
        .arrayLayers = create_info->imageArrayLayers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = create_info->imageUsage,
        .sharingMode = create_info->imageSharingMode,
        .queueFamilyIndexCount = create_info->queueFamilyIndexCount,
        .pQueueFamilyIndices = create_info->pQueueFamilyIndices,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkMemoryRequirements requirements;
    VkMemoryPropertyFlags required = 0;
    uint32_t memory_type;
    VkResult res;

    image->dmabuf_fd = -1;
    if (create_info->flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (create_info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
        image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
                            VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (create_info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
    {
        image_info.flags |= VK_IMAGE_CREATE_PROTECTED_BIT;
        required |= VK_MEMORY_PROPERTY_PROTECTED_BIT;
    }
    if ((format_list = find_vk_struct( (void *)create_info->pNext,
                                       VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO )))
    {
        format_list_info = *format_list;
        format_list_info.pNext = NULL;
        image_info.pNext = &format_list_info;
    }

    if ((res = device->p_vkCreateImage( device->host.device, &image_info, NULL,
                                        &image->image )))
        return res;

    device->p_vkGetImageMemoryRequirements( device->host.device, image->image,
                                            &requirements );
    memory_type = managed_find_memory_type( device, requirements.memoryTypeBits,
                                            required );
    if (memory_type == UINT32_MAX)
    {
        res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        goto failed;
    }

    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = memory_type;
    if ((res = device->p_vkAllocateMemory( device->host.device, &alloc_info,
                                           NULL, &image->memory )))
        goto failed;
    if ((res = device->p_vkBindImageMemory( device->host.device, image->image,
                                            image->memory, 0 )))
        goto failed;

    image->valid = TRUE;
    return VK_SUCCESS;

failed:
    managed_destroy_image( device, image );
    return res;
}

static VkResult managed_discard_swapchain_create(
        struct vulkan_device *device, struct surface *surface,
        const VkSwapchainCreateInfoKHR *create_info,
        struct wine_managed_swapchain **out )
{
    struct wine_managed_swapchain *managed;
    uint32_t count, i;
    VkResult res;

    *out = NULL;
    count = max( create_info->minImageCount, 3u );
    if (count > WINE_VK_MANAGED_MAX_IMAGES)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!(managed = calloc( 1, sizeof(*managed) )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    pthread_mutex_init( &managed->lock, NULL );
    managed->channel_fd = -1;
    managed->hwnd = surface->hwnd;
    managed->format = create_info->imageFormat;
    managed->extents = create_info->imageExtent;
    managed->usage = create_info->imageUsage;
    managed->discard = TRUE;
    if (device->queue_count) managed->signal_queue = device->queues[0].host.queue;
    managed->p_vkWaitForFences = (void *)p_vkGetDeviceProcAddr(
            device->host.device, "vkWaitForFences" );
    managed->p_vkResetFences = (void *)p_vkGetDeviceProcAddr(
            device->host.device, "vkResetFences" );
    {
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (device->p_vkCreateFence( device->host.device, &fence_info, NULL,
                                     &managed->present_fence ))
            managed->present_fence = VK_NULL_HANDLE;
    }

    for (i = 0; i < count; i++)
    {
        if ((res = managed_create_discard_image( device, create_info,
                                                 &managed->images[i] )))
        {
            managed->image_count = i;
            managed_free( device, managed );
            return res;
        }
        managed->image_count = i + 1;
    }

    *out = managed;
    TRACE( "created locally retired swapchain %p for hwnd %p: %u images %ux%u\n",
           managed, surface->hwnd, managed->image_count,
           managed->extents.width, managed->extents.height );
    return VK_SUCCESS;
}

static BOOL managed_fd_is_dmabuf( int fd )
{
#if defined(__linux__) && defined(DMA_BUF_SET_NAME)
    const char name[] = "wine-managed-linear";

    return ioctl( fd, DMA_BUF_SET_NAME, name ) == 0;
#else
    return FALSE;
#endif
}

/* Create one exportable image + dedicated exportable memory, export its dmabuf
 * fd and cache the realized modifier + per-plane layouts. */
static VkResult managed_create_image( struct vulkan_device *device, struct wine_managed_swapchain *managed,
                                      const uint64_t *modifiers, const uint64_t *wire_modifiers,
                                      uint32_t modifier_count,
                                      struct wine_managed_image *image )
{
    VkImageDrmFormatModifierListCreateInfoEXT mod_list =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = modifier_count,
        .pDrmFormatModifiers = modifiers,
    };
    VkExternalMemoryImageCreateInfo external_image =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = managed->format,
        .extent = { managed->extents.width, managed->extents.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = managed->usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryDedicatedAllocateInfo dedicated =
    {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    };
    VkExportMemoryAllocateInfo export_mem =
    {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageMemoryRequirementsInfo2 req_info = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    VkMemoryRequirements2 req = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkMemoryAllocateInfo alloc_info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &export_mem };
    VkImageDrmFormatModifierPropertiesEXT mod_props = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT };
    VkMemoryGetFdInfoKHR get_fd = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    VkImageSubresource subresource = {0};
    struct vulkan_physical_device *physical_device = device->physical_device;
    VkSubresourceLayout layout = {0};
    uint32_t mem_type_index = ~0u, plane_count, i;
    uint64_t wire_modifier = 0;
    BOOL linear = managed->layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR;
    VkExternalMemoryHandleTypeFlagBits handle_type = linear ?
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    VkResult res;

    image->dmabuf_fd = -1;
    external_image.pNext = linear ? NULL : &mod_list;
    external_image.handleTypes = handle_type;
    image_info.tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    export_mem.handleTypes = handle_type;

    if ((res = device->p_vkCreateImage( device->host.device, &image_info, NULL, &image->image )))
    {
        WARN( "managed vkCreateImage failed, res %d\n", res );
        return res;
    }

    req_info.image = image->image;
    device->p_vkGetImageMemoryRequirements2( device->host.device, &req_info, &req );

    for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
    {
        if (!(req.memoryRequirements.memoryTypeBits & (1u << i))) continue;
        if (!(physical_device->memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        mem_type_index = i;
        break;
    }
    if (mem_type_index == ~0u)
    {
        /* fall back to any supported type */
        for (i = 0; i < physical_device->memory_properties.memoryTypeCount; i++)
            if (req.memoryRequirements.memoryTypeBits & (1u << i)) { mem_type_index = i; break; }
    }
    if (mem_type_index == ~0u) { res = VK_ERROR_OUT_OF_DEVICE_MEMORY; goto failed; }

    dedicated.image = image->image;
    alloc_info.allocationSize = req.memoryRequirements.size;
    alloc_info.memoryTypeIndex = mem_type_index;
    if ((res = device->p_vkAllocateMemory( device->host.device, &alloc_info, NULL, &image->memory )))
    {
        WARN( "managed vkAllocateMemory failed, res %d\n", res );
        goto failed;
    }

    if ((res = device->p_vkBindImageMemory( device->host.device, image->image, image->memory, 0 )))
    {
        WARN( "managed vkBindImageMemory failed, res %d\n", res );
        goto failed;
    }

    get_fd.memory = image->memory;
    get_fd.handleType = handle_type;
    if ((res = device->p_vkGetMemoryFdKHR( device->host.device, &get_fd, &fd )) || fd < 0)
    {
        WARN( "managed vkGetMemoryFdKHR failed, res %d\n", res );
        if (!res) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto failed;
    }
    image->dmabuf_fd = fd;
    if (linear && !managed_fd_is_dmabuf( fd ))
    {
        WARN( "managed linear opaque fd is not a dma-buf, error %d\n", errno );
        res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        goto failed;
    }

    if (linear)
    {
        managed->realized_modifier = 0 /* DRM_FORMAT_MOD_LINEAR */;
        wire_modifier = wire_modifiers[0];
        plane_count = 1;
    }
    else
    {
        if ((res = device->p_vkGetImageDrmFormatModifierPropertiesEXT( device->host.device,
                                                                       image->image, &mod_props )))
        {
            WARN( "managed vkGetImageDrmFormatModifierPropertiesEXT failed, res %d\n", res );
            goto failed;
        }
        /* Record the swapchain-level realized modifier from the first image. Each
         * image still publishes its own modifier/stride/offset below in case the
         * host picks differently per image. */
        managed->realized_modifier = mod_props.drmFormatModifier;
        wire_modifier = mod_props.drmFormatModifier;
        for (i = 0; i < modifier_count; i++)
        {
            if (modifiers[i] == mod_props.drmFormatModifier)
            {
                wire_modifier = wire_modifiers[i];
                break;
            }
        }

        /* Publish every plane of the realized modifier. A missing auxiliary plane
         * (e.g. AMD DCC) makes the consumer-side dmabuf import fail fatally. */
        plane_count = vk_modifier_plane_count( device, managed->format, mod_props.drmFormatModifier );
        if (!plane_count || plane_count > HWND_DMABUF_MAX_PLANES)
        {
            WARN( "managed image modifier 0x%s has unsupported plane count %u\n",
                  wine_dbgstr_longlong(mod_props.drmFormatModifier), plane_count );
            res = VK_ERROR_FORMAT_NOT_SUPPORTED;
            goto failed;
        }
    }

    /* Cache the per-image frame descriptor (filled with the per-frame fields at
     * publish time). */
    memset( &image->desc, 0, sizeof(image->desc) );
    for (i = 0; i < plane_count; i++)
    {
        static const VkImageAspectFlagBits plane_aspects[] =
        {
            VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
            VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT,
        };
        subresource.aspectMask = linear ? VK_IMAGE_ASPECT_COLOR_BIT : plane_aspects[i];
        memset( &layout, 0, sizeof(layout) );
        device->p_vkGetImageSubresourceLayout( device->host.device, image->image, &subresource, &layout );
        if (linear && (!layout.rowPitch || layout.offset > UINT_MAX || layout.rowPitch > UINT_MAX))
        {
            WARN( "managed linear image has unsupported offset/stride %s/%s\n",
                  wine_dbgstr_longlong(layout.offset), wine_dbgstr_longlong(layout.rowPitch) );
            res = VK_ERROR_FORMAT_NOT_SUPPORTED;
            goto failed;
        }
        image->desc.plane_offsets[i] = (unsigned int)layout.offset;
        image->desc.plane_strides[i] = (unsigned int)layout.rowPitch;
    }
    image->desc.plane_count = plane_count;
    image->desc.version = HWND_DMABUF_DESC_VERSION_V1;
    /* Each ring slot's dmabuf is exported once and dup'd per present. The busy
     * gate blocks re-render until the release token returns. So image_id names a
     * stable dmabuf the consumer may cache and reuse the wl_buffer for. */
    image->desc.flags = HWND_DMABUF_FLAG_STABLE_SLOT;
    image->desc.width = managed->extents.width;
    image->desc.height = managed->extents.height;
    image->desc.fourcc = managed->fourcc;
    image->desc.stride = image->desc.plane_strides[0];
    image->desc.offset = image->desc.plane_offsets[0];
    image->desc.modifier = wire_modifier;
    image->desc.alpha_mode = managed->alpha_mode;
    image->desc.sync_fd_kind = 0; /* HWND_DMABUF_SYNC_NONE: consumer ignores acquire today */
    image->desc.producer_unique_id = 0; /* set from the managed producer id at present */

    image->valid = TRUE;
    image->acquired = image->busy = FALSE;
    return VK_SUCCESS;

failed:
    managed_destroy_image( device, image );
    return res;
}

/* Try to build a wine-managed swapchain. Returns VK_SUCCESS with *out set on
 * success. On any failure returns the error and leaves *out NULL so the caller
 * can fall back to the host swapchain path (we never fail the create call). */
static VkResult managed_swapchain_create( struct vulkan_device *device, struct surface *surface,
                                          const VkSwapchainCreateInfoKHR *create_info,
                                          struct wine_managed_swapchain **out )
{
    uint64_t modifiers[WINE_VK_MANAGED_MAX_MODIFIERS];
    uint64_t wire_modifiers[WINE_VK_MANAGED_MAX_MODIFIERS];
    struct wine_managed_swapchain *managed;
    BOOL opaque_alpha;
    unsigned int caps_flags = 0, fourcc = 0;
    enum wine_managed_image_layout layout;
    uint32_t modifier_count, count, i;
    VkImageUsageFlags usage;
    VkResult res;
    unsigned int status;

    *out = NULL;

    opaque_alpha = create_info->compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    usage = create_info->imageUsage;
    layout = device->extensions.has_VK_EXT_image_drm_format_modifier ?
             WINE_MANAGED_IMAGE_LAYOUT_MODIFIER : WINE_MANAGED_IMAGE_LAYOUT_LINEAR;

    if (layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR &&
        (create_info->imageFormat != VK_FORMAT_R8G8B8A8_UNORM || !opaque_alpha))
    {
        TRACE( "linear managed fallback does not support format %u alpha %#x for hwnd %p\n",
               create_info->imageFormat, create_info->compositeAlpha, surface->hwnd );
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    /* Intersect the compositor's advertised (fourcc, modifier) caps with what the
     * host can export as a single-plane dmabuf, against the realized usage. */
    modifier_count = vk_select_managed_modifiers( device, surface->hwnd, create_info->imageFormat,
                                                  usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, opaque_alpha,
                                                  &fourcc, layout, &caps_flags, modifiers, wire_modifiers,
                                                  ARRAY_SIZE(modifiers) );
    if (!modifier_count)
    {
        TRACE( "no host-exportable %s layout for hwnd %p format %u usage %#x, falling back to host swapchain\n",
               layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR ? "linear" : "modifier",
               surface->hwnd, create_info->imageFormat, usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT );
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    if (!(managed = calloc( 1, sizeof(*managed) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init( &managed->lock, NULL );
    managed->channel_fd = -1;
    managed->hwnd = surface->hwnd;
    if (!(managed->consumer = managed_consumer_get( managed->hwnd )))
    {
        managed_free( device, managed );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (!(caps_flags & HWND_DMABUF_HOST_CAP_CONSUMER_STATE))
        managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_ACTIVE );
    managed->format = create_info->imageFormat;
    managed->fourcc = fourcc;
    managed->layout = layout;
    /* If the chosen fourcc is the opaque X-variant, tell the compositor to ignore
     * alpha. Otherwise leave alpha as straight. */
    managed->alpha_mode = (fourcc == vk_format_to_drm_fourcc( create_info->imageFormat, TRUE ))
                          ? HWND_DMABUF_ALPHA_MODE_IGNORE : HWND_DMABUF_ALPHA_MODE_UNSPECIFIED;
    managed->extents = create_info->imageExtent;
    managed->usage = usage;
    managed->next_release_token = 0;
    managed->producer_unique_id = InterlockedIncrement64( &managed_next_producer_id );
    if (!managed->producer_unique_id)
        managed->producer_unique_id = InterlockedIncrement64( &managed_next_producer_id );
    managed->ring_generation = 1;

    status = hwnd_dmabuf_set_pending( managed->hwnd, TRUE );
    if (status != HWND_DMABUF_OK)
    {
        WARN( "failed to mark hwnd %p as pending dmabuf producer, status %u\n",
              managed->hwnd, status );
        managed_free( device, managed );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    managed->pending_registered = TRUE;

    /* Keep enough images available for asynchronous consumer release. */
    count = max( create_info->minImageCount, 3u );
    if (count > WINE_VK_MANAGED_MAX_IMAGES) count = WINE_VK_MANAGED_MAX_IMAGES;

    /* Pick the signal queue (first host queue) for empty acquire-signal submits. */
    if (device->queue_count) managed->signal_queue = device->queues[0].host.queue;

    /* Per-frame fence to gate export on just this frame's render (see managed_present). */
    managed->p_vkWaitForFences = (void *)p_vkGetDeviceProcAddr( device->host.device, "vkWaitForFences" );
    managed->p_vkResetFences = (void *)p_vkGetDeviceProcAddr( device->host.device, "vkResetFences" );
    {
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (device->p_vkCreateFence( device->host.device, &fci, NULL, &managed->present_fence ))
            managed->present_fence = VK_NULL_HANDLE;
    }

    for (i = 0; i < count; i++)
    {
        if ((res = managed_create_image( device, managed, modifiers, wire_modifiers,
                                         modifier_count, &managed->images[i] )))
        {
            WARN( "failed to create managed image %u, res %d, falling back to host swapchain\n", i, res );
            managed->image_count = i;
            managed_free( device, managed );
            return res;
        }
        managed->images[i].desc.image_id = i;
        managed->image_count = i + 1;
    }

    managed->channel_fd = hwnd_dmabuf_open_channel( surface->hwnd );
    if (managed->channel_fd < 0)
    {
        WARN( "failed to open hwnd %p dmabuf producer channel, falling back to host swapchain\n",
              surface->hwnd );
        managed_free( device, managed );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    managed->channel_registered = TRUE;
    managed->pending_registered = FALSE;
    managed_consumer_request_state( managed );
    TRACE( "managed swapchain %p hwnd %p socket channel fd %d\n", managed, surface->hwnd, managed->channel_fd );

    *out = managed;
    TRACE( "created managed swapchain %p: %u images %ux%u fourcc %#x layout %s modifier 0x%s\n",
           managed, managed->image_count, managed->extents.width, managed->extents.height,
           managed->fourcc, managed->layout == WINE_MANAGED_IMAGE_LAYOUT_LINEAR ? "linear" : "modifier",
           wine_dbgstr_longlong( managed->realized_modifier ) );
    return VK_SUCCESS;
}

/* A managed swapchain has no host acquire operation to signal these objects. */
static VkResult managed_signal_acquire( struct vulkan_device *device, struct wine_managed_swapchain *managed,
                                        VkSemaphore host_semaphore, VkFence host_fence )
{
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };

    if (!host_semaphore && !host_fence) return VK_SUCCESS;
    if (!managed->signal_queue) return VK_SUCCESS;

    if (host_semaphore)
    {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &host_semaphore;
    }
    return device->p_vkQueueSubmit( managed->signal_queue, 1, &submit, host_fence );
}

static void managed_drain_releases( struct wine_managed_swapchain *managed );
static void managed_mark_lost( struct wine_managed_swapchain *managed );

/* Window changes may call into this thread; return VK_NOT_READY instead of waiting. */
static VkResult managed_acquire( struct vulkan_device *device, struct swapchain *swapchain,
                                 VkSemaphore host_semaphore, VkFence host_fence,
                                 uint32_t *image_index )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    struct surface *surface = swapchain->surface;
    RECT client_rect;
    VkResult res = VK_SUCCESS;
    BOOL busy_any = FALSE, valid_any = FALSE;
    uint32_t i, slot = ~0u;

    pthread_mutex_lock( &producer_device_lock );
    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (managed->lost)
    {
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    for (i = 0; i < managed->image_count; i++)
    {
        uint32_t idx = (managed->next_acquire + i) % managed->image_count;
        struct wine_managed_image *image = &managed->images[idx];

        if (!image->valid) continue;
        valid_any = TRUE;
        if (image->busy)
        {
            busy_any = TRUE;
            continue;
        }
        if (image->acquired) continue;
        slot = idx;
        break;
    }
    if (slot == ~0u)
    {
        if (!valid_any)
            res = VK_ERROR_OUT_OF_DATE_KHR;
        else
        {
            res = VK_NOT_READY;
            if (busy_any)
            {
                DWORD now = NtGetTickCount();

                if (!managed->ring_full_since_ms)
                    managed->ring_full_since_ms = now;
                else if ((DWORD)(now - managed->ring_full_since_ms) >=
                         WINE_VK_MANAGED_STALL_MS)
                {
                    WARN( "managed dmabuf consumer stopped releasing frames\n" );
                    managed_mark_lost( managed );
                    res = VK_ERROR_OUT_OF_DATE_KHR;
                }
            }
            else
                managed->ring_full_since_ms = 0;
        }
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );
        return res;
    }

    managed->ring_full_since_ms = 0;
    managed->images[slot].acquired = TRUE;
    managed->next_acquire = (slot + 1) % managed->image_count;
    *image_index = slot;
    res = managed_signal_acquire( device, managed, host_semaphore, host_fence );
    pthread_mutex_unlock( &managed->lock );
    pthread_mutex_unlock( &producer_device_lock );

    if (res) return res;

    if (get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &managed->extents, &client_rect ))
        return VK_SUBOPTIMAL_KHR;

    return VK_SUCCESS;
}

/* Release a busy slot by token. */
static void managed_release_token( struct wine_managed_swapchain *managed, UINT64 release_token, BOOL failed )
{
    uint32_t i;

    if (!release_token) return;
    for (i = 0; i < managed->image_count; i++)
    {
        struct wine_managed_image *image = &managed->images[i];
        if (image->release_token != release_token) continue;
        image->busy = FALSE;
        image->release_token = 0;
        if (failed) image->valid = FALSE;
        managed->ring_full_since_ms = 0;
        return;
    }
}

/* Caller holds managed->lock. */
static void managed_mark_lost( struct wine_managed_swapchain *managed )
{
    uint32_t i;

    managed->lost = TRUE;
    managed->ring_full_since_ms = 0;
    for (i = 0; i < managed->image_count; i++)
    {
        managed->images[i].busy = FALSE;
        managed->images[i].release_token = 0;
    }
}

static BOOL dmabuf_send_error_is_fatal( int err )
{
    return err == EPIPE || err == ECONNRESET || err == ENOTCONN ||
           err == ECONNABORTED || err == ESHUTDOWN || err == EBADF;
}

/* Drain exact release tokens from the consumer. Caller holds managed->lock. */
static void managed_drain_releases( struct wine_managed_swapchain *managed )
{
    hwnd_dmabuf_release_t rel;
    BOOL received = FALSE;
    ssize_t ret;

    if (managed->discard || managed->channel_fd < 0) return;
    for (;;)
    {
        ret = recv( managed->channel_fd, &rel, sizeof(rel), MSG_DONTWAIT );
        if (ret == (ssize_t)sizeof(rel))
        {
            struct wine_managed_image *image;

            received = TRUE;
            /* Zero-token records update the channel's consumer state. */
            if (!rel.release_token)
            {
                if (rel.flags & HWND_DMABUF_RELEASE_CONSUMER_SUSPENDED)
                {
                    TRACE( "hwnd %p consumer suspended\n", managed->hwnd );
                    managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_SUSPENDED );
                }
                else if (rel.flags & HWND_DMABUF_RELEASE_CONSUMER_ACTIVE)
                {
                    TRACE( "hwnd %p consumer active\n", managed->hwnd );
                    managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_ACTIVE );
                }
                continue;
            }

            if (rel.producer_unique_id != managed->producer_unique_id) continue;
            /* A release must not resume a suspended consumer. */
            if (managed_consumer_state( managed ) == WINE_MANAGED_CONSUMER_UNKNOWN)
                managed_consumer_set_state( managed, WINE_MANAGED_CONSUMER_ACTIVE );
            if (rel.ring_generation != managed->ring_generation) continue;
            if (rel.image_id >= managed->image_count) continue;

            image = &managed->images[rel.image_id];
            if (!image->release_token || image->release_token != rel.release_token) continue;

            image->busy = FALSE;
            image->release_token = 0;
            image->consumer_cached = !!(rel.flags & HWND_DMABUF_RELEASE_CACHED);
            managed->ring_full_since_ms = 0;
        }
        else if (ret < 0 && errno == EINTR) continue;
        else break;
    }

    if (received && managed_consumer_state( managed ) == WINE_MANAGED_CONSUMER_UNKNOWN)
    {
        InterlockedExchange( &managed->consumer->reannounce_pending, FALSE );
        InterlockedExchange( &managed->consumer->reannounce_delay_ms, 0 );
    }

    if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
    {
        managed_mark_lost( managed );
    }
}

/* Publish one managed image over the dmabuf channel. */
static VkResult managed_present( struct vulkan_device *device, struct vulkan_queue *queue,
                                 struct swapchain *swapchain, uint32_t image_index,
                                 BOOL present_waits_consumed )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    struct surface *surface = swapchain->surface;
    struct wine_managed_image *image;
    hwnd_dmabuf_frame_desc_t desc;
    UINT64 release_token = 0;
    int channel_fd_dup = -1;
    enum wine_managed_consumer_state consumer_state;
    BOOL send_frame = FALSE, send_fd = FALSE;
    RECT client_rect;
    VkResult res = VK_SUCCESS;

    if (swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;
    if (image_index >= managed->image_count) return VK_ERROR_OUT_OF_DATE_KHR;

    /* The window changed size, recreate is required. */
    if (!get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ))
        return VK_ERROR_OUT_OF_DATE_KHR;

    pthread_mutex_lock( &producer_device_lock );
    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (managed->lost)
    {
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    image = &managed->images[image_index];
    if (!image->valid)
    {
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    /* With an all-managed present, the present wait semaphores were consumed
     * before publishing. Mixed host/managed presents still rely on the old
     * conservative idle because the host present owns the wait semaphores. */
    if (!present_waits_consumed)
        res = device->p_vkDeviceWaitIdle( device->host.device );
    if (res < VK_SUCCESS)
    {
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );
        return res;
    }

    if (managed->discard)
    {
        managed->present_id++;
        image->acquired = FALSE;
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );

        if (!IsRectEmpty( &client_rect ) &&
            !extents_equals( &managed->extents, &client_rect ))
            return VK_SUBOPTIMAL_KHR;
        return VK_SUCCESS;
    }

    /* Unsent frames remain producer-owned and need no release token. */
    consumer_state = managed_consumer_state( managed );
    send_frame = consumer_state == WINE_MANAGED_CONSUMER_ACTIVE && managed->channel_fd >= 0;
    send_fd = send_frame && !image->consumer_cached;
    if (send_fd && (channel_fd_dup = dup( image->dmabuf_fd )) < 0)
    {
        image->acquired = FALSE;
        pthread_mutex_unlock( &managed->lock );
        pthread_mutex_unlock( &producer_device_lock );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    managed->present_id++;
    if (send_frame)
    {
        /* Assign a fresh release token (never 0: consumer rejects token 0). */
        release_token = ++managed->next_release_token;
        if (!release_token) release_token = ++managed->next_release_token;
        image->release_token = release_token;

        desc = image->desc;
        desc.producer_unique_id = managed->producer_unique_id;
        desc.image_id = image_index;
        desc.ring_generation = managed->ring_generation;
        desc.frame_seq = (unsigned int)managed->present_id;
        desc.release_token = release_token;
        image->busy = TRUE;
    }
    image->acquired = FALSE;
    pthread_mutex_unlock( &managed->lock );
    pthread_mutex_unlock( &producer_device_lock );

    /* Reclaim undelivered frames. Fatal channel errors force recreate. */
    if (send_frame)
    {
        int serr = hwnd_dmabuf_channel_send( managed->channel_fd, &desc, channel_fd_dup );
        if (serr)
        {
            BOOL fatal = dmabuf_send_error_is_fatal( serr );

            pthread_mutex_lock( &managed->lock );
            managed_release_token( managed, release_token, FALSE );
            if (fatal) managed_mark_lost( managed );
            pthread_mutex_unlock( &managed->lock );
            if (fatal) return VK_ERROR_OUT_OF_DATE_KHR;
        }
        else hwnd_dmabuf_post_wake( surface->hwnd, 0 );
    }
    else if (consumer_state == WINE_MANAGED_CONSUMER_UNKNOWN)
        managed_consumer_request_state( managed );

    if (res >= VK_SUCCESS && !IsRectEmpty( &client_rect ) && !extents_equals( &managed->extents, &client_rect ))
        res = VK_SUBOPTIMAL_KHR;

    return res;
}

static void release_pending_client_swapchain_ref( struct client_surface *client )
{
    InterlockedDecrement( &client->busy_ref );
}

static void begin_native_client_swapchain_create( struct client_surface *client )
{
    InterlockedIncrement( &client->native_pending_ref );
    client_surface_update( client );
}

static void end_native_client_swapchain_create( struct client_surface *client )
{
    InterlockedDecrement( &client->native_pending_ref );
    client_surface_update( client );
}

static void release_client_swapchain_ref( struct swapchain *swapchain )
{
    struct client_surface *client = swapchain->surface->client;

    if (swapchain->client_active)
        client_surface_deactivate( client, swapchain->native_presenter );
    release_pending_client_swapchain_ref( client );
}

void win32u_vkDestroySwapchainKHR( VkDevice client_device,
                                   VkSwapchainKHR client_swapchain,
                                   const VkAllocationCallbacks *allocator );

static BOOL commit_client_swapchain( struct swapchain *swapchain,
                                     BOOL expected_owner, BOOL native )
{
    struct client_surface *client = swapchain->surface->client;
    BOOL actual_owner;

    if (swapchain_is_out_of_date( swapchain )) return FALSE;

    actual_owner = client_surface_activate( client, native, NULL );
    swapchain->client_active = TRUE;
    swapchain->native_presenter = native;
    if (actual_owner != expected_owner)
    {
        WARN( "surface %p ownership changed while creating swapchain %p\n",
              swapchain->surface, swapchain );
        return FALSE;
    }

    /* Activation can invalidate the previous ownership generation. */
    swapchain->presentation_generation =
            ReadAcquire( &client->presentation_generation );
    return TRUE;
}

static VkResult finish_managed_swapchain_create(
        VkDevice client_device, struct vulkan_instance *instance,
        struct surface *surface, struct swapchain *swapchain,
        struct wine_managed_swapchain *managed, BOOL expected_owner,
        VkSwapchainKHR *ret )
{
    vulkan_object_init( &swapchain->obj.obj, (UINT_PTR)VK_NULL_HANDLE );
    swapchain->surface = surface;
    swapchain->extents = managed->extents;
    swapchain->managed = managed;
    swapchain->presentation_generation =
            ReadAcquire( &surface->client->presentation_generation );
    instance->p_insert_object( instance, &swapchain->obj.obj );
    set_window_pixel_format( surface->hwnd, -1, TRUE );

    if (!commit_client_swapchain( swapchain, expected_owner, FALSE ))
    {
        win32u_vkDestroySwapchainKHR( client_device,
                                      swapchain->obj.client.swapchain, NULL );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    *ret = swapchain->obj.client.swapchain;
    TRACE( "hwnd %p -> %s swapchain %p\n", surface->hwnd,
           managed->discard ? "locally retired background" :
                              "cross-process dmabuf producer", swapchain );
    return VK_SUCCESS;
}

static VkResult win32u_vkCreateSwapchainKHR( VkDevice client_device, const VkSwapchainCreateInfoKHR *create_info,
                                             const VkAllocationCallbacks *allocator, VkSwapchainKHR *ret )
{
    VkSwapchainPresentScalingCreateInfoEXT scaling = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT};
    struct swapchain *swapchain, *old_swapchain = swapchain_from_handle( create_info->oldSwapchain );
    struct surface *surface = surface_from_handle( create_info->surface );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_physical_device *physical_device = device->physical_device;
    struct vulkan_instance *instance = physical_device->instance;
    VkSwapchainCreateInfoKHR create_info_host = *create_info;
    VkSurfaceCapabilitiesKHR capabilities;
    VkSwapchainKHR host_swapchain;
    uint32_t format_count = 0;
    VkSurfaceFormatKHR *formats;
    BOOL presentation_owner;
    RECT client_rect;
    VkResult res;

    if (!NtUserIsWindow( surface->hwnd ))
    {
        ERR( "surface %p, hwnd %p is invalid!\n", surface, surface->hwnd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    presentation_owner = client_surface_is_presentation_candidate( surface->client );

    if (surface) create_info_host.surface = surface->obj.host.surface;
    if (old_swapchain) create_info_host.oldSwapchain = old_swapchain->obj.host.swapchain;

    /* Windows allows client rect to be empty, but host Vulkan often doesn't, adjust extents back to the host capabilities */
    res = instance->p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device->host.physical_device, surface->obj.host.surface, &capabilities );
    if (res) return res;

    create_info_host.imageColorSpace = driver_funcs->p_vulkan_map_colorspace( create_info_host.imageColorSpace, surface->client );
    driver_funcs->p_vulkan_surface_set_alpha( create_info_host.compositeAlpha, surface->client );
    create_info_host.imageExtent.width = max( create_info_host.imageExtent.width, capabilities.minImageExtent.width );
    create_info_host.imageExtent.height = max( create_info_host.imageExtent.height, capabilities.minImageExtent.height );

    /* DOOM Eternal and DOOM: The Dark Ages rely on an extra image to continue
     * acquiring while an earlier image is still pending presentation. Request
     * one when the application asks for exactly the surface minimum. */
    if (capabilities.minImageCount < UINT32_MAX &&
        create_info_host.minImageCount == capabilities.minImageCount &&
        (!capabilities.maxImageCount || capabilities.minImageCount < capabilities.maxImageCount))
    {
        create_info_host.minImageCount = capabilities.minImageCount + 1;
        TRACE( "Increasing host swapchain image count to %u for surface minimum %u\n",
               create_info_host.minImageCount, capabilities.minImageCount );
    }

    /* If the swapchain image size is not equal to the presentation size (e.g. because of DPI virtualization or
     * display mode change emulation), MoltenVK's vkQueuePresentKHR returns VK_SUBOPTIMAL_KHR.
     * Create the swapchain with VkSwapchainPresentScalingCreateInfoEXT to avoid this.
     */
    if (get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetWinMonitorDpi( surface->hwnd, MDT_WINE_RAW_DPI ) ) &&
        !extents_equals( &create_info_host.imageExtent, &client_rect ) &&
        instance->extensions.has_VK_EXT_surface_maintenance1 &&
        physical_device->extensions.has_VK_KHR_swapchain_maintenance1)
    {
        scaling.scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_EXT;
        create_info_host.pNext = &scaling;
    }

    if (!(swapchain = calloc( 1, sizeof(*swapchain) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if (presentation_owner &&
        (swapchain->fshack_dpi = surface_get_fshack_dpi( surface )))
    {
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT))
            FIXME( "Swapchain does not support required VK_IMAGE_USAGE_STORAGE_BIT\n" );

        swapchain->host_extents = capabilities.minImageExtent;
        create_info_host.imageExtent = capabilities.minImageExtent;
        create_info_host.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        create_info_host.imageUsage = VK_IMAGE_USAGE_STORAGE_BIT;

        if (create_info->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && create_info->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
            FIXME( "swapchain image format is not BGRA8 UNORM/SRGB. Things may go badly. %d\n",
                   create_info_host.imageFormat );
    }

    /* check if the new colorspace works with the provided format */
    if (create_info_host.imageColorSpace != create_info->imageColorSpace)
    {
        BOOL found = FALSE;

        res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device, surface->obj.host.surface, &format_count, NULL );

        if (!(formats = calloc( format_count, sizeof(*formats) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

        res = instance->p_vkGetPhysicalDeviceSurfaceFormatsKHR( physical_device->host.physical_device, surface->obj.host.surface, &format_count, formats );

        if (res)
        {
            free(formats);
            return res;
        }

    again:
        for (unsigned i = 0; i < format_count; i++)
        {
            if (formats[i].format == create_info_host.imageFormat &&
                formats[i].colorSpace == create_info_host.imageColorSpace)
                found = TRUE;
        }

        /* HACK: try again with VK_COLOR_SPACE_SRGB_NONLINEAR_KHR */
        if (!found && create_info_host.imageColorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT)
        {
            create_info_host.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            goto again;
        }

        if (!found)
        {
            ERR("Colorspace %u is not compatible with format %u\n",
                create_info_host.imageColorSpace, create_info_host.imageFormat);
            create_info_host.imageColorSpace = create_info->imageColorSpace;
        }
        else driver_funcs->p_vulkan_map_colorspace( create_info->imageColorSpace, surface->client );

        free(formats);
    }
    else driver_funcs->p_vulkan_map_colorspace( create_info->imageColorSpace, surface->client );

    InterlockedIncrement( &surface->client->busy_ref );
    client_surface_update( surface->client );

    /* Windows retires an older flip-model presenter behind the newest one.
     * Keep its Vulkan work and synchronization alive locally, without exposing
     * a competing host WSI surface for the same HWND. */
    if (!presentation_owner)
    {
        struct wine_managed_swapchain *managed = NULL;
        VkSwapchainCreateInfoKHR managed_info = *create_info;

        managed_info.minImageCount = create_info_host.minImageCount;
        res = managed_discard_swapchain_create( device, surface, &managed_info,
                                                &managed );
        if (res == VK_SUCCESS)
            return finish_managed_swapchain_create( client_device, instance,
                    surface, swapchain, managed, FALSE, ret );

        WARN( "failed to create locally retired swapchain for hwnd %p, res %d\n",
              surface->hwnd, res );
        release_pending_client_swapchain_ref( surface->client );
        free( swapchain );
        return res;
    }

    /* Interpose a managed cross-process producer only when the window advertises
     * HWND dmabuf caps (an off-screen child whose toplevel has no wl_surface) and
     * the host supports dmabuf external memory. On-screen windows return no caps
     * and use the host swapchain path below. */
    if (device->extensions.has_VK_EXT_external_memory_dma_buf)
    {
        struct wine_managed_swapchain *managed = NULL;
        hwnd_dmabuf_host_caps_t probe_caps = {0};
        UINT probe_count = 0;
        UINT probe_status;

        probe_status = hwnd_dmabuf_get_caps( surface->hwnd, &probe_caps, NULL, 0, &probe_count );
        if (probe_status == HWND_DMABUF_OK && probe_count)
        {
            /* Use the host-clamped extents so the dmabuf size matches the window. */
            VkSwapchainCreateInfoKHR managed_info = *create_info;
            managed_info.imageExtent = create_info_host.imageExtent;

            res = managed_swapchain_create( device, surface, &managed_info, &managed );
            if (res == VK_SUCCESS && managed)
                return finish_managed_swapchain_create( client_device, instance,
                        surface, swapchain, managed, TRUE, ret );
            /* Any failure -> fall through to the host swapchain path (never fail). */
            TRACE( "managed swapchain build failed (res %d) for hwnd %p, using host swapchain\n",
                   res, surface->hwnd );
        }
        else
        {
            TRACE( "managed dmabuf unavailable for hwnd %p (status %u, formats %u), using host swapchain\n",
                   surface->hwnd, probe_status, probe_count );
        }
    }
    else
    {
        TRACE( "managed dmabuf unavailable for hwnd %p (external memory %u), using host swapchain\n",
               surface->hwnd, device->extensions.has_VK_EXT_external_memory_dma_buf );
    }

    /* Host Wayland WSI needs the replacement wl_surface attached while it
     * creates the swapchain. It cannot be marked native until creation has
     * succeeded, so keep this narrow bootstrap state separate from busy_ref;
     * locally retired background swapchains must remain detached. */
    begin_native_client_swapchain_create( surface->client );
    if ((res = device->p_vkCreateSwapchainKHR( device->host.device, &create_info_host, NULL, &host_swapchain )))
    {
        InterlockedDecrement( &surface->client->busy_ref );
        end_native_client_swapchain_create( surface->client );
        free( swapchain );
        return res;
    }

    vulkan_object_init( &swapchain->obj.obj, host_swapchain );
    swapchain->surface = surface;
    swapchain->presentation_generation =
        ReadAcquire( &surface->client->presentation_generation );
    surface->swapchain = swapchain;
    swapchain->extents = create_info->imageExtent;
    instance->p_insert_object( instance, &swapchain->obj.obj );

    if (swapchain->fshack_dpi)
    {
        if ((res = init_fs_hack_images( device, swapchain, create_info )))
        {
            ERR( "creating fs hack images failed: %d\n", res );
            device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
            surface->swapchain = NULL;
            release_pending_client_swapchain_ref( surface->client );
            end_native_client_swapchain_create( surface->client );
            instance->p_remove_object( instance, &swapchain->obj.obj );
            free( swapchain );
            return res;
        }

        if ((res = init_blit_images( device, swapchain )))
        {
            ERR( "creating blit images failed: %d\n", res );
            device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
            surface->swapchain = NULL;
            release_pending_client_swapchain_ref( surface->client );
            end_native_client_swapchain_create( surface->client );
            instance->p_remove_object( instance, &swapchain->obj.obj );
            free( swapchain );
            return res;
        }

        WARN( "Enabled fullscreen hack on swapchain %p, scaling from %s -> %s\n", swapchain,
              debugstr_vkextent2d(&swapchain->extents), debugstr_vkextent2d(&swapchain->host_extents) );
    }

    set_window_pixel_format( surface->hwnd, -1, TRUE );

    if (!commit_client_swapchain( swapchain, TRUE, TRUE ))
    {
        end_native_client_swapchain_create( surface->client );
        win32u_vkDestroySwapchainKHR( client_device,
                                      swapchain->obj.client.swapchain, NULL );
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    end_native_client_swapchain_create( surface->client );

    *ret = swapchain->obj.client.swapchain;
    return VK_SUCCESS;
}

void win32u_vkDestroySwapchainKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                   const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct vulkan_instance *instance = device->physical_device->instance;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    struct surface *surface;

    if (allocator) FIXME( "Support for allocation callbacks not implemented yet\n" );
    if (!swapchain) return;

    if (swapchain->fshack_dpi)
    {
        for (uint32_t i = 0; i < swapchain->n_images; ++i)
        {
            destroy_fs_hack_image( device, swapchain, &swapchain->fs_hack_images[i] );
        }
        for (uint32_t i = 0; i < device->queue_count; ++i)
        {
            if (!swapchain->cmd_pools[i]) continue;
            device->p_vkDestroyCommandPool( device->host.device, swapchain->cmd_pools[i], NULL );
        }

        device->p_vkDestroyPipeline( device->host.device, swapchain->pipeline, NULL );
        device->p_vkDestroyPipelineLayout( device->host.device, swapchain->pipeline_layout, NULL );
        device->p_vkDestroyDescriptorSetLayout( device->host.device, swapchain->descriptor_set_layout, NULL );
        device->p_vkDestroyDescriptorPool( device->host.device, swapchain->descriptor_pool, NULL );
        device->p_vkDestroySampler( device->host.device, swapchain->sampler, NULL );
        device->p_vkFreeMemory( device->host.device, swapchain->user_image_memory, NULL );
        free( swapchain->cmd_pools );
        free( swapchain->fs_hack_images );
    }

    if (swapchain->managed)
    {
        /* managed: idle, close fds, destroy images+memory, free. No host swapchain. */
        managed_free( device, swapchain->managed );
        swapchain->managed = NULL;
    }
    else
    {
        device->p_vkDestroySwapchainKHR( device->host.device, swapchain->obj.host.swapchain, NULL );
    }
    if ((surface = swapchain->surface))
    {
        if (surface->swapchain == swapchain) surface->swapchain = NULL;
        release_client_swapchain_ref( swapchain );
    }
    instance->p_remove_object( instance, &swapchain->obj.obj );

    free( swapchain );
}

static VkResult acquire_host_image2( struct vulkan_device *device,
                                     struct swapchain *swapchain,
                                     VkAcquireNextImageInfoKHR *info,
                                     uint32_t *image_index )
{
    BOOL infinite = info->timeout == UINT64_MAX;
    uint64_t remaining = info->timeout;
    VkResult res;

    for (;;)
    {
        info->timeout = min( remaining, WINE_VK_ACQUIRE_WAIT_SLICE_NS );
        if (swapchain_is_out_of_date( swapchain ))
            return VK_ERROR_OUT_OF_DATE_KHR;
        res = device->p_vkAcquireNextImage2KHR( device->host.device, info,
                                                image_index );
        if (res != VK_TIMEOUT) return res;
        if (infinite) continue;
        if (remaining <= info->timeout) return VK_TIMEOUT;
        remaining -= info->timeout;
    }
}

static VkResult acquire_host_image( struct vulkan_device *device,
                                    struct swapchain *swapchain,
                                    uint64_t timeout, VkSemaphore semaphore,
                                    VkFence fence, uint32_t *image_index )
{
    BOOL infinite = timeout == UINT64_MAX;
    uint64_t remaining = timeout;
    VkResult res;

    for (;;)
    {
        uint64_t slice = min( remaining, WINE_VK_ACQUIRE_WAIT_SLICE_NS );

        if (swapchain_is_out_of_date( swapchain ))
            return VK_ERROR_OUT_OF_DATE_KHR;
        res = device->p_vkAcquireNextImageKHR( device->host.device,
                swapchain->obj.host.swapchain, slice, semaphore, fence,
                image_index );
        if (res != VK_TIMEOUT) return res;
        if (infinite) continue;
        if (remaining <= slice) return VK_TIMEOUT;
        remaining -= slice;
    }
}

static VkResult win32u_vkAcquireNextImage2KHR( VkDevice client_device, const VkAcquireNextImageInfoKHR *acquire_info,
                                               uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = acquire_info->semaphore ? vulkan_semaphore_from_handle( acquire_info->semaphore ) : NULL;
    struct vulkan_fence *fence = acquire_info->fence ? vulkan_fence_from_handle( acquire_info->fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( acquire_info->swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkAcquireNextImageInfoKHR acquire_info_host = *acquire_info;
    struct surface *surface;
    RECT client_rect;
    VkResult res;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    surface = swapchain->surface;

    if (swapchain->managed)
        return managed_acquire( device, swapchain, semaphore ? semaphore->host.semaphore : 0,
                                fence ? fence->host.fence : 0, image_index );

    acquire_info_host.swapchain = swapchain->obj.host.swapchain;
    acquire_info_host.semaphore = semaphore ? semaphore->host.semaphore : 0;
    acquire_info_host.fence = fence ? fence->host.fence : 0;
    res = acquire_host_image2( device, swapchain, &acquire_info_host, image_index );

    if (!res && swapchain->fshack_dpi != surface_get_fshack_dpi( surface ))
    {
        WARN( "window %p swapchain %p needs fullscreen hack VK_SUBOPTIMAL_KHR\n", surface->hwnd, swapchain );
        return VK_SUBOPTIMAL_KHR;
    }

    if (!res && get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }

    return res;
}

static VkResult win32u_vkAcquireNextImageKHR( VkDevice client_device, VkSwapchainKHR client_swapchain, uint64_t timeout,
                                              VkSemaphore client_semaphore, VkFence client_fence, uint32_t *image_index )
{
    struct vulkan_semaphore *semaphore = client_semaphore ? vulkan_semaphore_from_handle( client_semaphore ) : NULL;
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct surface *surface;
    RECT client_rect;
    VkResult res;

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    surface = swapchain->surface;

    if (swapchain->managed)
        return managed_acquire( device, swapchain, semaphore ? semaphore->host.semaphore : 0,
                                fence ? fence->host.fence : 0, image_index );

    res = acquire_host_image( device, swapchain, timeout,
                              semaphore ? semaphore->host.semaphore : 0,
                              fence ? fence->host.fence : 0, image_index );

    if (!res && swapchain->fshack_dpi != surface_get_fshack_dpi( surface ))
    {
        WARN( "window %p swapchain %p needs fullscreen hack VK_SUBOPTIMAL_KHR\n", surface->hwnd, swapchain );
        return VK_SUBOPTIMAL_KHR;
    }

    if (!res && get_swapchain_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ) &&
        !extents_equals( &swapchain->extents, &client_rect ))
    {
        WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
              swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
        return VK_SUBOPTIMAL_KHR;
    }

    return res;
}

static BOOL should_skip_wait( HWND hwnd )
{
    if (!NtUserIsWindowVisible( hwnd ))
    {
        WARN( "hwnd=%p not yet visible!\n", hwnd );
        return TRUE;
    }

    return FALSE;
}

/* Keep host waits interruptible across presentation-target changes and
 * recover infinite waits when the compositor stops reporting completion. */
#define WINE_VK_PRESENT_WAIT_SLICE_NS (100 * 1000000ull)
#define WINE_VK_PRESENT_WAIT_STALL_NS (3000 * 1000000ull)

static VkResult swapchain_wait_for_present( struct vulkan_device *device,
                                            struct swapchain *swapchain,
                                            uint64_t present_id, uint64_t timeout,
                                            const VkPresentWait2InfoKHR *info )
{
    BOOL infinite = timeout == UINT64_MAX;
    uint64_t stalled = 0;
    VkResult res;

    for (;;)
    {
        uint64_t slice = min( timeout, WINE_VK_PRESENT_WAIT_SLICE_NS );

        if (swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

        if (info)
        {
            VkPresentWait2InfoKHR slice_info = *info;

            slice_info.timeout = slice;
            res = device->p_vkWaitForPresent2KHR( device->host.device,
                                                  swapchain->obj.host.swapchain,
                                                  &slice_info );
        }
        else
        {
            res = device->p_vkWaitForPresentKHR( device->host.device,
                                                 swapchain->obj.host.swapchain,
                                                 present_id, slice );
        }

        if (res != VK_TIMEOUT) return res;
        if (should_skip_wait( swapchain->surface->hwnd )) return VK_SUCCESS;

        if (infinite && (stalled += slice) >= WINE_VK_PRESENT_WAIT_STALL_NS)
        {
            client_surface_invalidate_presentation( swapchain->surface->client );
            WARN( "hwnd %p swapchain %p present wait stalled, returning VK_ERROR_OUT_OF_DATE_KHR\n",
                  swapchain->surface->hwnd, swapchain );
            return VK_ERROR_OUT_OF_DATE_KHR;
        }

        if (infinite) continue;
        if (timeout <= slice) return VK_TIMEOUT;
        timeout -= slice;
    }
}

static VkResult win32u_vkWaitForPresentKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                            uint64_t presentId, uint64_t timeout )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    /* Managed swapchains have no host swapchain to wait on. Wine paces the present
     * itself. Report presentation as proceeding. */
    if (swapchain->managed) return VK_SUCCESS;

    if (swapchain->surface && should_skip_wait( swapchain->surface->hwnd ))
        return VK_SUCCESS;

    return swapchain_wait_for_present( device, swapchain, presentId, timeout, NULL );
}

static VkResult win32u_vkWaitForPresent2KHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                             const VkPresentWait2InfoKHR *info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (!swapchain || swapchain_is_out_of_date( swapchain )) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return VK_SUCCESS;

    if (swapchain->surface && should_skip_wait( swapchain->surface->hwnd ))
        return VK_SUCCESS;

    return swapchain_wait_for_present( device, swapchain, info->presentId,
                                       info->timeout, info );
}

static VkResult win32u_vkGetSwapchainImagesKHR( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                                uint32_t *count, VkImage *images )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    uint32_t i;

    if (!swapchain) return VK_ERROR_UNKNOWN;

    if (swapchain->managed)
    {
        struct wine_managed_swapchain *managed = swapchain->managed;
        uint32_t n;

        if (!images)
        {
            *count = managed->image_count;
            return VK_SUCCESS;
        }
        n = min( *count, managed->image_count );
        for (i = 0; i < n; i++) images[i] = managed->images[i].image; /* raw host VkImage */
        *count = n;
        return n < managed->image_count ? VK_INCOMPLETE : VK_SUCCESS;
    }

    if (images && swapchain->fshack_dpi)
    {
        if (*count > swapchain->n_images) *count = swapchain->n_images;
        for (i = 0; i < *count; ++i) images[i] = swapchain->fs_hack_images[i].user_image;
        return *count == swapchain->n_images ? VK_SUCCESS : VK_INCOMPLETE;
    }

    return device->p_vkGetSwapchainImagesKHR( device->host.device, swapchain->obj.host.swapchain, count, images );
}

static VkResult managed_swapchain_get_status( struct swapchain *swapchain, UINT64 *present_id )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    VkResult res = VK_SUCCESS;

    pthread_mutex_lock( &producer_device_lock );
    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (present_id) *present_id = managed->present_id;
    if (managed->lost) res = VK_ERROR_OUT_OF_DATE_KHR;
    pthread_mutex_unlock( &managed->lock );
    pthread_mutex_unlock( &producer_device_lock );
    return res;
}

static VkResult win32u_vkSetSwapchainPresentTimingQueueSizeEXT( VkDevice client_device,
                                                                VkSwapchainKHR client_swapchain, uint32_t size )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );

    if (!swapchain) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return managed_swapchain_get_status( swapchain, NULL );

    return device->p_vkSetSwapchainPresentTimingQueueSizeEXT( device->host.device,
                                                              swapchain->obj.host.swapchain, size );
}

static VkResult win32u_vkGetSwapchainTimingPropertiesEXT( VkDevice client_device, VkSwapchainKHR client_swapchain,
                                                          VkSwapchainTimingPropertiesEXT *properties,
                                                          uint64_t *counter )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( client_swapchain );
    VkResult res;

    if (!swapchain) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed)
    {
        res = managed_swapchain_get_status( swapchain, NULL );
        if (res < VK_SUCCESS) return res;
        properties->refreshDuration = 0;
        properties->refreshInterval = 0;
        if (counter) *counter = 0;
        return VK_SUCCESS;
    }

    return device->p_vkGetSwapchainTimingPropertiesEXT( device->host.device, swapchain->obj.host.swapchain,
                                                        properties, counter );
}

static VkResult win32u_vkGetPastPresentationTimingEXT( VkDevice client_device,
                                                       const VkPastPresentationTimingInfoEXT *info,
                                                       VkPastPresentationTimingPropertiesEXT *properties )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain;
    VkPastPresentationTimingInfoEXT info_host = *info;
    UINT64 present_id = 0;
    VkResult res;

    swapchain = swapchain_from_handle( info->swapchain );
    if (!swapchain) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed)
    {
        res = managed_swapchain_get_status( swapchain, &present_id );
        if (res < VK_SUCCESS) return res;
        properties->timingPropertiesCounter = present_id;
        properties->timeDomainsCounter = 0;
        properties->presentationTimingCount = 0;
        return VK_SUCCESS;
    }

    info_host.swapchain = swapchain->obj.host.swapchain;
    return device->p_vkGetPastPresentationTimingEXT( device->host.device, &info_host, properties );
}

static VkResult managed_release_swapchain_images( struct swapchain *swapchain,
                                                  const VkReleaseSwapchainImagesInfoKHR *info )
{
    struct wine_managed_swapchain *managed = swapchain->managed;
    VkResult res = VK_SUCCESS;

    pthread_mutex_lock( &producer_device_lock );
    pthread_mutex_lock( &managed->lock );
    managed_drain_releases( managed );
    if (managed->lost) res = VK_ERROR_OUT_OF_DATE_KHR;
    else
    {
        for (uint32_t i = 0; i < info->imageIndexCount; i++)
        {
            uint32_t image_index = info->pImageIndices[i];
            if (image_index >= managed->image_count)
            {
                res = VK_ERROR_OUT_OF_DATE_KHR;
                break;
            }
            managed->images[image_index].acquired = FALSE;
            managed->ring_full_since_ms = 0;
        }
    }
    pthread_mutex_unlock( &managed->lock );
    pthread_mutex_unlock( &producer_device_lock );
    return res;
}

static VkResult win32u_vkReleaseSwapchainImagesKHR( VkDevice client_device,
                                                    const VkReleaseSwapchainImagesInfoKHR *release_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( release_info->swapchain );
    VkReleaseSwapchainImagesInfoKHR release_info_host = *release_info;

    if (!swapchain) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return managed_release_swapchain_images( swapchain, release_info );

    release_info_host.swapchain = swapchain->obj.host.swapchain;
    return device->p_vkReleaseSwapchainImagesKHR( device->host.device, &release_info_host );
}

static VkResult win32u_vkReleaseSwapchainImagesEXT( VkDevice client_device,
                                                    const VkReleaseSwapchainImagesInfoKHR *release_info )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct swapchain *swapchain = swapchain_from_handle( release_info->swapchain );
    VkReleaseSwapchainImagesInfoKHR release_info_host = *release_info;

    if (!swapchain) return VK_ERROR_OUT_OF_DATE_KHR;

    if (swapchain->managed) return managed_release_swapchain_images( swapchain, release_info );

    release_info_host.swapchain = swapchain->obj.host.swapchain;
    return device->p_vkReleaseSwapchainImagesEXT( device->host.device, &release_info_host );
}

static void win32u_vkSetHdrMetadataEXT( VkDevice client_device, uint32_t swapchain_count,
                                        const VkSwapchainKHR *client_swapchains,
                                        const VkHdrMetadataEXT *metadata )
{
    VkSwapchainKHR stack_swapchains[16], *host_swapchains = stack_swapchains;
    VkHdrMetadataEXT stack_metadata[16], *host_metadata = stack_metadata;
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    uint32_t host_count = 0;

    if (swapchain_count > ARRAY_SIZE(stack_swapchains))
    {
        host_swapchains = malloc( swapchain_count * sizeof(*host_swapchains) );
        host_metadata = malloc( swapchain_count * sizeof(*host_metadata) );
        if (!host_swapchains || !host_metadata)
        {
            WARN( "failed to allocate HDR metadata arrays\n" );
            free( host_swapchains );
            free( host_metadata );
            return;
        }
    }

    for (uint32_t i = 0; i < swapchain_count; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        if (!swapchain) continue;
        if (swapchain->managed) continue;
        host_swapchains[host_count] = swapchain->obj.host.swapchain;
        host_metadata[host_count] = metadata[i];
        host_count++;
    }

    if (host_count)
        device->p_vkSetHdrMetadataEXT( device->host.device, host_count, host_swapchains, host_metadata );

    if (host_swapchains != stack_swapchains)
    {
        free( host_swapchains );
        free( host_metadata );
    }
}

static VkCommandBuffer create_hack_cmd( struct vulkan_queue *queue, struct swapchain *swapchain, uint32_t queue_idx )
{
    VkCommandBufferAllocateInfo allocInfo = {0};
    VkCommandBuffer cmd;
    VkResult res;

    if (!swapchain->cmd_pools[queue_idx])
    {
        VkCommandPoolCreateInfo poolInfo = {0};

        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = queue_idx;

        if ((res = queue->device->p_vkCreateCommandPool( queue->device->host.device, &poolInfo, NULL,
                                                         &swapchain->cmd_pools[queue_idx] )))
        {
            ERR( "vkCreateCommandPool failed, res=%d\n", res );
            return NULL;
        }
    }

    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = swapchain->cmd_pools[queue_idx];
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if ((res = queue->device->p_vkAllocateCommandBuffers( queue->device->host.device, &allocInfo, &cmd )))
    {
        ERR( "vkAllocateCommandBuffers failed, res=%d\n", res );
        return NULL;
    }

    return cmd;
}

static VkResult record_compute_cmd( struct vulkan_device *device, struct swapchain *swapchain, struct fs_hack_image *hack )
{
    VkResult res;
    VkImageMemoryBarrier barriers[3] = {{0}};
    VkCommandBufferBeginInfo beginInfo = {0};
    float constants[4];

    TRACE( "recording compute command\n" );

    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    device->p_vkBeginCommandBuffer( hack->cmd, &beginInfo );

    /* for the cs we run... */
    /* transition user image from PRESENT_SRC to SHADER_READ */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    /* storage image... */
    /* transition swapchain image from whatever to GENERAL */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 0, NULL, 0, NULL, 2, barriers );

    /* perform blit shader */
    device->p_vkCmdBindPipeline( hack->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swapchain->pipeline );

    device->p_vkCmdBindDescriptorSets( hack->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       swapchain->pipeline_layout, 0, 1, &hack->descriptor_set, 0, NULL );

    /* vec2: blit dst offset in real coords */
    constants[0] = 0;
    constants[1] = 0;

    /* offset by 0.5f because sampling is relative to pixel center */
    constants[0] -= 0.5f * swapchain->host_extents.width / swapchain->extents.width;
    constants[1] -= 0.5f * swapchain->host_extents.height / swapchain->extents.height;

    /* vec2: blit dst extents in real coords */
    constants[2] = swapchain->host_extents.width;
    constants[3] = swapchain->host_extents.height;
    device->p_vkCmdPushConstants( hack->cmd, swapchain->pipeline_layout,
                                  VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), constants );

    /* local sizes in shader are 8 */
    device->p_vkCmdDispatch( hack->cmd, ceil( swapchain->host_extents.width / 8. ),
                             ceil( swapchain->host_extents.height / 8. ), 1 );

    /* transition user image from SHADER_READ back to PRESENT_SRC */
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = hack->user_image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.baseMipLevel = 0;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = 0;

    /* transition swapchain image from GENERAL to PRESENT_SRC */
    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image = hack->swapchain_image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[1].subresourceRange.baseMipLevel = 0;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.baseArrayLayer = 0;
    barriers[1].subresourceRange.layerCount = 1;
    barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[1].dstAccessMask = 0;

    device->p_vkCmdPipelineBarrier( hack->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    0, 0, NULL, 0, NULL, 2, barriers );

    if ((res = device->p_vkEndCommandBuffer( hack->cmd )))
    {
        ERR( "vkEndCommandBuffer: %d\n", res );
        return res;
    }

    return VK_SUCCESS;
}

/* Consume present waits once before all managed presents. */
static VkResult managed_present_consume_waits( struct vulkan_device *device, struct vulkan_queue *queue,
                                               struct wine_managed_swapchain *managed,
                                               const VkSemaphore *semaphores, uint32_t count )
{
    VkPipelineStageFlags stack_stages[16], *stages = stack_stages;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VkResult res;
    uint32_t i;

    if (!count) return VK_SUCCESS;
    if (count > ARRAY_SIZE(stack_stages) && !(stages = malloc( count * sizeof(*stages) )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (i = 0; i < count; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    submit.waitSemaphoreCount = count;
    submit.pWaitSemaphores = semaphores;
    submit.pWaitDstStageMask = stages;
    if (managed && managed->present_fence && managed->p_vkWaitForFences && managed->p_vkResetFences)
        fence = managed->present_fence;

    pthread_mutex_lock( &producer_device_lock );
    res = device->p_vkQueueSubmit( queue->host.queue, 1, &submit, fence );
    if (res == VK_SUCCESS && !fence) res = device->p_vkDeviceWaitIdle( device->host.device );
    pthread_mutex_unlock( &producer_device_lock );
    if (res == VK_SUCCESS && fence)
    {
        res = managed->p_vkWaitForFences( device->host.device, 1, &fence, VK_TRUE, UINT64_MAX );
        if (res == VK_SUCCESS) res = managed->p_vkResetFences( device->host.device, 1, &fence );
    }
    if (stages != stack_stages) free( stages );
    return res;
}

static void append_present_pnext( const void ***tail, void *entry )
{
    VkBaseInStructure *header = entry;

    header->pNext = NULL;
    **tail = header;
    *tail = (const void **)&header->pNext;
}

static VkResult repack_present_pnext( struct mempool *pool, VkPresentInfoKHR *host_info,
                                      const VkPresentInfoKHR *present_info,
                                      const uint32_t *host_indices, uint32_t host_count )
{
    const VkBaseInStructure *header;
    const void **tail = &host_info->pNext;
    uint32_t i;

    host_info->pNext = NULL;
    for (header = present_info->pNext; header; header = header->pNext)
    {
        switch (header->sType)
        {
        case VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR:
        {
            const VkDeviceGroupPresentInfoKHR *src = (const VkDeviceGroupPresentInfoKHR *)header;
            VkDeviceGroupPresentInfoKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pDeviceMasks)
            {
                uint32_t *masks;

                if (!(masks = mem_alloc( pool, host_count * sizeof(*masks) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pDeviceMasks = masks;
                for (i = 0; i < host_count; i++) masks[i] = src->pDeviceMasks[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR:
        {
            const VkPresentIdKHR *src = (const VkPresentIdKHR *)header;
            VkPresentIdKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pPresentIds)
            {
                uint64_t *ids;

                if (!(ids = mem_alloc( pool, host_count * sizeof(*ids) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pPresentIds = ids;
                for (i = 0; i < host_count; i++) ids[i] = src->pPresentIds[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
        {
            const VkPresentRegionsKHR *src = (const VkPresentRegionsKHR *)header;
            VkPresentRegionsKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pRegions)
            {
                VkPresentRegionKHR *regions;

                if (!(regions = mem_alloc( pool, host_count * sizeof(*regions) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pRegions = regions;
                for (i = 0; i < host_count; i++) regions[i] = src->pRegions[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR:
        {
            const VkSwapchainPresentFenceInfoKHR *src = (const VkSwapchainPresentFenceInfoKHR *)header;
            VkSwapchainPresentFenceInfoKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pFences)
            {
                VkFence *fences;

                if (!(fences = mem_alloc( pool, host_count * sizeof(*fences) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pFences = fences;
                for (i = 0; i < host_count; i++) fences[i] = src->pFences[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR:
        {
            const VkSwapchainPresentModeInfoKHR *src = (const VkSwapchainPresentModeInfoKHR *)header;
            VkSwapchainPresentModeInfoKHR *dst;

            if (!(dst = mem_alloc( pool, sizeof(*dst) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            *dst = *src;
            dst->swapchainCount = host_count;
            if (src->pPresentModes)
            {
                VkPresentModeKHR *modes;

                if (!(modes = mem_alloc( pool, host_count * sizeof(*modes) )))
                    return VK_ERROR_OUT_OF_HOST_MEMORY;
                dst->pPresentModes = modes;
                for (i = 0; i < host_count; i++) modes[i] = src->pPresentModes[host_indices[i]];
            }
            append_present_pnext( &tail, dst );
            break;
        }
        default:
        {
            static int once;

            if (!once++)
                WARN( "dropping unhandled VkPresentInfoKHR pNext sType %u for host-only present\n",
                      header->sType );
            break;
        }
        }
    }

    return VK_SUCCESS;
}

static VkResult win32u_vkQueuePresentKHR( VkQueue client_queue, const VkPresentInfoKHR *client_present_info )
{
    VkPresentInfoKHR present_info_data = *client_present_info;
    VkPresentInfoKHR *present_info = &present_info_data;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkSwapchainKHR *client_swapchains;
    VkSwapchainKHR *swapchains;
    uint32_t host_indices_buffer[16], *host_indices = host_indices_buffer;
    VkSemaphore *host_wait_semaphores = NULL;
    const VkSwapchainPresentFenceInfoKHR *present_fence_info = NULL;
    struct wine_managed_swapchain *first_managed = NULL;
    uint32_t host_count = 0;
    BOOL skip_managed = FALSE;
    BOOL managed_present_waits_consumed = FALSE;
    VkCommandBuffer *blit_cmds;
    struct mempool pool = {0};
    uint32_t blit_count = 0;
    VkSemaphore blit_sema;

    TRACE( "queue %p, present_info %p\n", queue, present_info );

    if (!(swapchains = mem_alloc( &pool, present_info->swapchainCount * sizeof(*swapchains) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(blit_cmds = mem_alloc( &pool, present_info->swapchainCount * sizeof(blit_cmds) ))) goto failed;
    if (present_info->swapchainCount > ARRAY_SIZE(host_indices_buffer) &&
        !(host_indices = mem_alloc( &pool, present_info->swapchainCount * sizeof(*host_indices) ))) goto failed;

    for (uint32_t i = 0; i < present_info->swapchainCount; ++i)
    {
        struct swapchain *swapchain = swapchain_from_handle( present_info->pSwapchains[i] );
        struct fs_hack_image *hack = &swapchain->fs_hack_images[present_info->pImageIndices[i]];

        if (!swapchain->fshack_dpi) continue;
        blit_sema = hack->blit_finished;

        if (!hack->cmd || hack->cmd_queue_idx != queue->info.queueFamilyIndex)
        {
            if (hack->cmd) device->p_vkFreeCommandBuffers( queue->device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
            if (!(queue->device->queue_props[queue->info.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT)) goto failed; /* TODO */

            if (!(hack->cmd = create_hack_cmd( queue, swapchain, queue->info.queueFamilyIndex )) || record_compute_cmd( queue->device, swapchain, hack ))
            {
                device->p_vkFreeCommandBuffers( queue->device->host.device, swapchain->cmd_pools[hack->cmd_queue_idx], 1, &hack->cmd );
                hack->cmd = NULL;
                goto failed;
            }

            hack->cmd_queue_idx = queue->info.queueFamilyIndex;
        }

        blit_cmds[blit_count++] = hack->cmd;
    }

    if (present_info->waitSemaphoreCount &&
        !(host_wait_semaphores = mem_alloc( &pool, present_info->waitSemaphoreCount *
                                            sizeof(*host_wait_semaphores) )))
        goto failed;
    for (uint32_t i = 0; i < present_info->waitSemaphoreCount; i++)
    {
        struct vulkan_semaphore *semaphore =
            vulkan_semaphore_from_handle( present_info->pWaitSemaphores[i] );
        host_wait_semaphores[i] = semaphore->host.semaphore;
    }
    present_info->pWaitSemaphores = host_wait_semaphores;

    /* Host swapchains feed the real host present. Managed swapchains publish
     * cross-process dmabufs instead. */
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( present_info->pSwapchains[i] );
        if (swapchain->managed)
        {
            if (!first_managed) first_managed = swapchain->managed;
            continue;
        }
        swapchains[host_count] = swapchain->obj.host.swapchain;
        host_indices[host_count] = i;
        host_count++;
    }

    client_swapchains = present_info->pSwapchains;

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        struct surface *surface = swapchain->surface;
        client_surface_update( surface->client );
    }

    if (blit_count)
    {
        VkSubmitInfo submit_info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPipelineStageFlags *stages;

        if (!(stages = mem_alloc( &pool, sizeof(VkPipelineStageFlags) * present_info->waitSemaphoreCount ))) goto failed;
        for (uint32_t i = 0; i < present_info->waitSemaphoreCount; ++i) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        /* blit user image to real image */
        submit_info.waitSemaphoreCount = present_info->waitSemaphoreCount;
        submit_info.pWaitSemaphores = present_info->pWaitSemaphores;
        submit_info.pWaitDstStageMask = stages;
        submit_info.commandBufferCount = blit_count;
        submit_info.pCommandBuffers = blit_cmds;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &blit_sema;
        /* Serialize with every other host-queue op (external sync). */
        pthread_mutex_lock( &producer_device_lock );
        device->p_vkQueueSubmit( queue->host.queue, 1, &submit_info, VK_NULL_HANDLE );
        pthread_mutex_unlock( &producer_device_lock );

        present_info->waitSemaphoreCount = 1;
        present_info->pWaitSemaphores = &blit_sema;
    }

    res = VK_SUCCESS;
    /* Host present consumes waits for host swapchains. */
    if (host_count)
    {
        VkResult host_results_buffer[16], *host_results = NULL;
        uint32_t image_indices_buffer[16], *image_indices;
        VkPresentInfoKHR host_info = *present_info;

        image_indices = host_count <= ARRAY_SIZE(image_indices_buffer)
                        ? image_indices_buffer : malloc( host_count * sizeof(*image_indices) );
        if (!image_indices) res = VK_ERROR_OUT_OF_HOST_MEMORY;
        else
        {
            host_info.swapchainCount = host_count;
            host_info.pSwapchains = swapchains;
            if (host_count != present_info->swapchainCount &&
                (res = repack_present_pnext( &pool, &host_info, present_info, host_indices, host_count )))
                goto host_present_done;
            if (present_info->pResults)
            {
                host_results = host_count <= ARRAY_SIZE(host_results_buffer) ? host_results_buffer
                                                                             : malloc( host_count * sizeof(*host_results) );
                host_info.pResults = host_results;
            }
            /* Re-pack pImageIndices for the host-only subset (host_indices[k] is
             * the original position of the k-th host swapchain). */
            for (uint32_t i = 0; i < host_count; i++)
                image_indices[i] = present_info->pImageIndices[host_indices[i]];
            host_info.pImageIndices = image_indices;

            pthread_mutex_lock( &producer_device_lock );
            res = device->p_vkQueuePresentKHR( queue->host.queue, &host_info );
            pthread_mutex_unlock( &producer_device_lock );

host_present_done:
            if (image_indices != image_indices_buffer) free( image_indices );
            if (host_results)
            {
                for (uint32_t i = 0; i < host_count; i++)
                    if (present_info->pResults) present_info->pResults[host_indices[i]] = host_results[i];
                if (host_results != host_results_buffer) free( host_results );
            }
        }
    }

    /* Managed swapchains have no host present to signal these fences. */
    for (const VkBaseInStructure *header = present_info->pNext; header; header = header->pNext)
        if (header->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR)
        {
            present_fence_info = (const VkSwapchainPresentFenceInfoKHR *)header;
            break;
        }

    /* All-managed presents consume the live wait array once here. */
    if (!host_count && present_info->waitSemaphoreCount)
    {
        VkResult cres = managed_present_consume_waits( device, queue, first_managed, present_info->pWaitSemaphores,
                                                       present_info->waitSemaphoreCount );
        if (cres < VK_SUCCESS)
        {
            res = cres;
            skip_managed = TRUE;
        }
        else managed_present_waits_consumed = TRUE;
    }

    /* Present each managed swapchain after the present waits are consumed. */
    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        VkResult managed_res;

        if (!swapchain->managed) continue;

        if (skip_managed)
        {
            if (present_info->pResults) present_info->pResults[i] = res;
            continue;
        }

        managed_res = managed_present( device, queue, swapchain, present_info->pImageIndices[i],
                                       managed_present_waits_consumed );

        /* Managed swapchains have no host present to signal this fence. */
        if (present_fence_info && i < present_fence_info->swapchainCount && present_fence_info->pFences[i])
        {
            pthread_mutex_lock( &producer_device_lock );
            device->p_vkQueueSubmit( queue->host.queue, 0, NULL, present_fence_info->pFences[i] );
            pthread_mutex_unlock( &producer_device_lock );
        }

        if (present_info->pResults) present_info->pResults[i] = managed_res;
        if (managed_res < VK_SUCCESS && res >= VK_SUCCESS) res = managed_res;
        else if (managed_res == VK_SUBOPTIMAL_KHR && res == VK_SUCCESS) res = VK_SUBOPTIMAL_KHR;
    }

    for (uint32_t i = 0; i < present_info->swapchainCount; i++)
    {
        struct swapchain *swapchain = swapchain_from_handle( client_swapchains[i] );
        VkResult swapchain_res = present_info->pResults ? present_info->pResults[i] : res;
        struct surface *surface = swapchain->surface;
        RECT client_rect;

        if (!swapchain->managed || !swapchain->managed->discard)
            client_surface_present( surface->client );

        if (swapchain->managed) continue; /* managed already set its own result */
        if (swapchain_res < VK_SUCCESS) continue;
        if (!get_surface_rect( surface->hwnd, &client_rect, NtUserGetDpiForWindow( surface->hwnd ) ))
        {
            WARN( "Swapchain window %p is invalid, returning VK_ERROR_OUT_OF_DATE_KHR\n", surface->hwnd );
            if (present_info->pResults) present_info->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
            if (res >= VK_SUCCESS) res = VK_ERROR_OUT_OF_DATE_KHR;
        }
        else if (swapchain_res)
            WARN( "Present returned status %d for swapchain %p\n", swapchain_res, swapchain );
        else if (!IsRectEmpty( &client_rect ) &&
                 !extents_equals( &swapchain->extents, &client_rect ))
        {
            WARN( "Swapchain size %dx%d does not match client rect %s, returning VK_SUBOPTIMAL_KHR\n",
                  swapchain->extents.width, swapchain->extents.height, wine_dbgstr_rect( &client_rect ) );
            if (present_info->pResults) present_info->pResults[i] = VK_SUBOPTIMAL_KHR;
            if (!res) res = VK_SUBOPTIMAL_KHR;
        }
    }

    if (TRACE_ON( fps ))
    {
        static unsigned long frames, frames_total;
        static long prev_time, start_time;
        DWORD time;

        time = NtGetTickCount();
        frames++;
        frames_total++;

        if (time - prev_time > 1500)
        {
            TRACE_(fps)( "%p @ approx %.2ffps, total %.2ffps\n", queue, 1000.0 * frames / (time - prev_time),
                         1000.0 * frames_total / (time - start_time) );
            prev_time = time;
            frames = 0;

            if (!start_time) start_time = time;
        }
    }

failed:
    mem_free( &pool );
    return res;
}

static LARGE_INTEGER *get_nt_timeout( LARGE_INTEGER *time, DWORD timeout )
{
    if (timeout == INFINITE) return NULL;
    time->QuadPart = (ULONGLONG)timeout * -10000;
    return time;
}

static VkResult acquire_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->acquireCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->acquireCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->acquireCount; i++)
    {
        LARGE_INTEGER timeout;
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_ACQUIREKEYEDMUTEX acquire =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .pTimeout = get_nt_timeout( &timeout, mutex_info->pAcquireTimeouts[i] ),
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIAcquireKeyedMutex( &acquire ))) goto error;
        submit.value = memory->semaphore_value = acquire.FenceValue;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

error:
    WARN( "Failed to acquire keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pAcquireSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pAcquireKeys[i] ), status );

    while (i--)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pAcquireSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pAcquireKeys[i],
            .FenceValue = memory->semaphore_value,
        };
        NtGdiDdDDIReleaseKeyedMutex( &release );
    }
    return status == STATUS_TIMEOUT ? VK_TIMEOUT : VK_ERROR_UNKNOWN;
}

static VkResult release_keyed_mutexes( VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info, struct mempool *pool,
                                       const VkSemaphoreSubmitInfo **semaphores, UINT *semaphores_count )
{
    UINT i, count = *semaphores_count;
    VkSemaphoreSubmitInfo *submits;
    NTSTATUS status;

    if (!(submits = mem_alloc( pool, (count + mutex_info->releaseCount) * sizeof(*submits) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy( submits, *semaphores, count * sizeof(*submits) );
    memset( submits + count, 0, mutex_info->releaseCount * sizeof(*submits) );

    for (i = 0; i < mutex_info->releaseCount; i++)
    {
        struct device_memory *memory = device_memory_from_handle( mutex_info->pReleaseSyncs[i] );
        D3DKMT_RELEASEKEYEDMUTEX release =
        {
            .hKeyedMutex = memory->mutex,
            .Key = mutex_info->pReleaseKeys[i],
            .FenceValue = memory->semaphore_value + 1,
        };
        VkSemaphoreSubmitInfo submit =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = memory->semaphore,
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .deviceIndex = 0,
        };

        if ((status = NtGdiDdDDIReleaseKeyedMutex( &release ))) goto failed;
        submit.value = memory->semaphore_value + 1;
        submits[count++] = submit;
    }

    *semaphores = submits;
    *semaphores_count = count;
    return VK_SUCCESS;

failed:
    WARN( "Failed to release keyed mutex 0x%s key 0x%s, status %#x\n", wine_dbgstr_longlong( mutex_info->pReleaseSyncs[i] ),
          wine_dbgstr_longlong( mutex_info->pReleaseKeys[i] ), status );
    return VK_ERROR_UNKNOWN;
}

static VkResult win32u_vkQueueSubmit( VkQueue client_queue, uint32_t count, const VkSubmitInfo *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    VkTimelineSemaphoreSubmitInfo *timelines;
    struct mempool pool = {0};

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    if (!(timelines = mem_alloc( &pool, count * sizeof(*timelines) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset( timelines, 0, count * sizeof(*timelines) );

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo *submit = (VkSubmitInfo *)submits + i; /* cast away const, chain has been copied in the thunks */
        const VkSemaphoreSubmitInfo *wait_infos = NULL, *signal_infos = NULL;
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;
        VkTimelineSemaphoreSubmitInfo *timeline = timelines + i;
        BOOL convert_waits = FALSE, convert_signals = FALSE;
        VkSemaphore *wait_semaphores, *signal_semaphores;
        VkDeviceGroupSubmitInfo *device_group = NULL;
        UINT wait_count = 0, signal_count = 0;
        VkPipelineStageFlags *wait_stages;
        uint32_t *indexes;
        uint64_t *values;

        for (uint32_t j = 0; j < submit->commandBufferCount; j++)
        {
            VkCommandBuffer *command_buffers = (VkCommandBuffer *)submit->pCommandBuffers; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffers[j] );
            command_buffers[j] = command_buffer->host.command_buffer;
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pWaitSemaphores; /* cast away const, it has been copied in the thunks */
            struct semaphore *semaphore = semaphore_from_handle( semaphores[j] );
            if (semaphore->info.type) convert_waits = TRUE;
            else                      semaphores[j] = semaphore->obj.host.semaphore;
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreCount; j++)
        {
            VkSemaphore *semaphores = (VkSemaphore *)submit->pSignalSemaphores; /* cast away const, it has been copied in the thunks */
            struct semaphore *semaphore = semaphore_from_handle( semaphores[j] );
            if (semaphore->info.type) convert_signals = TRUE;
            else                      semaphores[j] = semaphore->obj.host.semaphore;
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR:
            {
                VkD3D12FenceSubmitInfoKHR *info = (VkD3D12FenceSubmitInfoKHR *)*next;

                if (timeline->sType == VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR)
                    ERR( "Duplicated d3d12 fence submit info.\n" );
                else if (timeline->sType)
                    FIXME( "Both d3d12 fence and timeline submit info.\n" );
                timeline->sType = info->sType;
                timeline->waitSemaphoreValueCount = info->waitSemaphoreValuesCount;
                timeline->pWaitSemaphoreValues = info->pWaitSemaphoreValues;
                timeline->signalSemaphoreValueCount = info->signalSemaphoreValuesCount;
                timeline->pSignalSemaphoreValues = info->pSignalSemaphoreValues;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            case VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO:
                device_group = (VkDeviceGroupSubmitInfo *)*next;
                break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO: break;
            case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
                if (timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                    ERR( "Duplicated timeline semaphore submit info.\n" );
                else if (timeline->sType)
                    FIXME( "Both d3d12 fence and timeline submit info.\n" );

                *timeline = *(VkTimelineSemaphoreSubmitInfo *)*next;
                *next = (*next)->pNext; next = &prev; /* remove it from the chain, we'll add it back below */
                break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &wait_infos, &wait_count ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &signal_infos, &signal_count ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }

        if (wait_count || convert_waits) /* extra wait semaphores, need to update arrays and counts */
        {
            if (!(wait_semaphores = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_semaphores) ))) goto failed;
            memcpy( wait_semaphores, submit->pWaitSemaphores, submit->waitSemaphoreCount * sizeof(*wait_semaphores) );
            if (!(values = mem_alloc( &pool, (timeline->waitSemaphoreValueCount + wait_count) * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pWaitSemaphoreValues, timeline->waitSemaphoreValueCount * sizeof(*values) );
            if (convert_waits)
            {
                for (uint32_t j = 0; j < submit->waitSemaphoreCount; j++)
                {
                    struct semaphore *semaphore = semaphore_from_handle( submit->pWaitSemaphores[j] );

                    if (!semaphore->info.type) continue;
                    add_sem_wait_op( device, semaphore, values[j], &wait_semaphores[j], &values[j] );
                }
            }
            submit->pWaitSemaphores = wait_semaphores;

            if (!(wait_stages = mem_alloc( &pool, (submit->waitSemaphoreCount + wait_count) * sizeof(*wait_stages) ))) goto failed;
            memcpy( wait_stages, submit->pWaitDstStageMask, submit->waitSemaphoreCount * sizeof(*wait_stages) );
            submit->pWaitDstStageMask = wait_stages;

            for (uint32_t j = 0; j < wait_count; j++)
            {
                wait_semaphores[submit->waitSemaphoreCount + j] = wait_infos[j].semaphore;
                wait_stages[submit->waitSemaphoreCount + j] = wait_infos[j].stageMask;
            }
            submit->waitSemaphoreCount += wait_count;

            for (uint32_t j = 0; j < wait_count; j++) values[submit->waitSemaphoreCount + j] = wait_infos[j].value;
            timeline->waitSemaphoreValueCount = submit->waitSemaphoreCount;
            timeline->pWaitSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->waitSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pWaitSemaphoreDeviceIndices, device_group->waitSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < wait_count; j++) indexes[device_group->waitSemaphoreCount + j] = wait_infos[j].deviceIndex;
                device_group->waitSemaphoreCount = submit->waitSemaphoreCount;
                device_group->pWaitSemaphoreDeviceIndices = indexes;
            }
        }

        if (signal_count || convert_signals) /* extra signal semaphores, need to update arrays and counts */
        {
            if (!(signal_semaphores = mem_alloc( &pool, (submit->signalSemaphoreCount + signal_count) * sizeof(*signal_semaphores) ))) goto failed;
            memcpy( signal_semaphores, submit->pSignalSemaphores, submit->signalSemaphoreCount * sizeof(*signal_semaphores) );
            if (!(values = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*values) ))) goto failed;
            memcpy( values, timeline->pSignalSemaphoreValues, timeline->signalSemaphoreValueCount * sizeof(*values) );
            if (convert_signals)
            {
                for (uint32_t j = 0; j < submit->signalSemaphoreCount; j++)
                {
                    struct semaphore *semaphore = semaphore_from_handle( submit->pSignalSemaphores[j] );

                    if (!semaphore->info.type) continue;
                    add_sem_signal_op( device, semaphore, values[j], &signal_semaphores[j], &values[j], FALSE );
                }
            }
            for (uint32_t j = 0; j < signal_count; j++) signal_semaphores[submit->signalSemaphoreCount + j] = signal_infos[j].semaphore;
            submit->signalSemaphoreCount += signal_count;
            submit->pSignalSemaphores = signal_semaphores;

            for (uint32_t j = 0; j < signal_count; j++) values[submit->signalSemaphoreCount + j] = signal_infos[j].value;
            timeline->signalSemaphoreValueCount = submit->signalSemaphoreCount;
            timeline->pSignalSemaphoreValues = values;

            if (device_group)
            {
                if (!(indexes = mem_alloc( &pool, submit->signalSemaphoreCount * sizeof(*indexes) ))) goto failed;
                memcpy( indexes, device_group->pSignalSemaphoreDeviceIndices, device_group->signalSemaphoreCount * sizeof(*indexes) );
                for (uint32_t j = 0; j < signal_count; j++) indexes[device_group->signalSemaphoreCount + j] = signal_infos[j].deviceIndex;
                device_group->signalSemaphoreCount = submit->signalSemaphoreCount;
                device_group->pSignalSemaphoreDeviceIndices = indexes;
            }
        }

        /* insert the timeline semaphore values in the chain if it was there or has been created */
        if (timeline->sType || wait_count || signal_count || convert_waits || convert_signals)
        {
            timeline->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timeline->pNext = submit->pNext;
            submit->pNext = timeline;
        }
    }

    res = device->p_vkQueueSubmit( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );

failed:
    mem_free( &pool );
    return res;
}

static VkResult queue_submit( struct vulkan_queue *queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence, PFN_vkQueueSubmit2 p_vkQueueSubmit2 )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_device *device = queue->device;
    struct mempool pool = {0};
    VkResult res;

    for (uint32_t i = 0; i < count; i++)
    {
        VkSubmitInfo2 *submit = (VkSubmitInfo2 *)submits + i; /* cast away const, chain has been copied in the thunks */
        VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)submit;

        for (uint32_t j = 0; j < submit->commandBufferInfoCount; j++)
        {
            VkCommandBufferSubmitInfoKHR *command_buffer_infos = (VkCommandBufferSubmitInfoKHR *)submit->pCommandBufferInfos; /* cast away const, chain has been copied in the thunks */
            struct vulkan_command_buffer *command_buffer = vulkan_command_buffer_from_handle( command_buffer_infos[j].commandBuffer );
            command_buffer_infos[j].commandBuffer = command_buffer->host.command_buffer;
            if (command_buffer_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->waitSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pWaitSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct semaphore *semaphore = semaphore_from_handle( semaphore_infos[j].semaphore );

            if (semaphore->info.type)
                add_sem_wait_op( device, semaphore, semaphore_infos[j].value, &semaphore_infos[j].semaphore, &semaphore_infos[j].value );
            else
                semaphore_infos[j].semaphore = semaphore->obj.host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (uint32_t j = 0; j < submit->signalSemaphoreInfoCount; j++)
        {
            VkSemaphoreSubmitInfo *semaphore_infos = (VkSemaphoreSubmitInfo *)submit->pSignalSemaphoreInfos; /* cast away const, it has been copied in the thunks */
            struct semaphore *semaphore = semaphore_from_handle( semaphore_infos[j].semaphore );

            if (semaphore->info.type)
                add_sem_signal_op( device, semaphore, semaphore_infos[j].value, &semaphore_infos[j].semaphore, &semaphore_infos[j].value, FALSE );
            else
                semaphore_infos[j].semaphore = semaphore->obj.host.semaphore;
            if (semaphore_infos->pNext) FIXME( "Unhandled struct chain\n" );
        }

        for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
        {
            switch ((*next)->sType)
            {
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT: break;
            case VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM: break;
            case VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV: break;
            case VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR: break;
            case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
            {
                VkWin32KeyedMutexAcquireReleaseInfoKHR *mutex_info = (VkWin32KeyedMutexAcquireReleaseInfoKHR *)*next;
                if ((res = acquire_keyed_mutexes( mutex_info, &pool, &submit->pWaitSemaphoreInfos, &submit->waitSemaphoreInfoCount ))) goto failed;
                if ((res = release_keyed_mutexes( mutex_info, &pool, &submit->pSignalSemaphoreInfos, &submit->signalSemaphoreInfoCount ))) goto failed;
                *next = (*next)->pNext; next = &prev;
                break;
            }
            default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
            }
        }
    }

    res = p_vkQueueSubmit2( queue->host.queue, count, submits, fence ? fence->host.fence : 0 );

failed:
    mem_free( &pool );
    return res;
}

static VkResult win32u_vkQueueSubmit2( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2 );
}

static VkResult win32u_vkQueueSubmit2KHR( VkQueue client_queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence client_fence )
{
    struct vulkan_fence *fence = client_fence ? vulkan_fence_from_handle( client_fence ) : NULL;
    struct vulkan_queue *queue = vulkan_queue_from_handle( client_queue );
    struct vulkan_device *device = queue->device;

    TRACE( "queue %p, count %u, submits %p, fence %p\n", queue, count, submits, fence );

    return queue_submit( queue, count, submits, client_fence, device->p_vkQueueSubmit2KHR );
}

static VkResult win32u_vkGetSemaphoreCounterValue( VkDevice client_device, VkSemaphore client_semaphore, uint64_t *value )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( client_semaphore );

    if (semaphore->info.type)
    {
        d3d12_semaphore_lock( semaphore );
        *value = semaphore->info.t->virtual_value;
        d3d12_semaphore_unlock( semaphore );
        return VK_SUCCESS;
    }
    return device->p_vkGetSemaphoreCounterValue( device->host.device, semaphore->obj.host.semaphore, value );
}

static VkResult win32u_vkGetSemaphoreCounterValueKHR( VkDevice client_device, VkSemaphore client_semaphore, uint64_t *value )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( client_semaphore );

    if (semaphore->info.type)
    {
        d3d12_semaphore_lock(semaphore);
        *value = semaphore->info.t->virtual_value;
        d3d12_semaphore_unlock(semaphore);
        return VK_SUCCESS;
    }
    return device->p_vkGetSemaphoreCounterValueKHR( device->host.device, semaphore->obj.host.semaphore, value );
}

static VkResult win32u_vkSignalSemaphore( VkDevice client_device, const VkSemaphoreSignalInfo *signal_info)
{
    struct semaphore *semaphore = semaphore_from_handle( signal_info->semaphore );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkSemaphoreSignalInfo host_info = *signal_info;

    if (semaphore->info.type)
    {
        add_sem_signal_op( device, semaphore, signal_info->value, &host_info.semaphore, &host_info.value, TRUE );
        return VK_SUCCESS;
    }
    host_info.semaphore = semaphore->obj.host.semaphore;
    return device->p_vkSignalSemaphore( device->host.device, &host_info );
}

static VkResult win32u_vkSignalSemaphoreKHR( VkDevice client_device, const VkSemaphoreSignalInfo *signal_info)
{
    struct semaphore *semaphore = semaphore_from_handle( signal_info->semaphore );
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkSemaphoreSignalInfo host_info = *signal_info;

    if (semaphore->info.type)
    {
        add_sem_signal_op( device, semaphore, signal_info->value, &host_info.semaphore, &host_info.value, TRUE );
        return VK_SUCCESS;
    }
    host_info.semaphore = semaphore->obj.host.semaphore;
    return device->p_vkSignalSemaphoreKHR( device->host.device, &host_info );
}

static VkResult wait_semaphores( struct vulkan_device *device, const VkSemaphoreWaitInfo *wait_info, uint64_t timeout,
                          PFN_vkWaitSemaphores p_vkWaitSemaphores )
{
    VkSemaphoreWaitInfo host_info;
    struct mempool pool = {0};
    uint64_t *values = NULL;
    VkResult ret;
    unsigned int i;

    for (i = 0; i < wait_info->semaphoreCount; ++i)
    {
        struct semaphore *semaphore = semaphore_from_handle( wait_info->pSemaphores[i] );
        /* cast away const, it has been copied in the thunks */
        if (semaphore->info.type)
        {
            if (!values)
            {
                host_info = *wait_info;
                wait_info = &host_info;
                values = mem_alloc( &pool, wait_info->semaphoreCount * sizeof(*values) );
                memcpy( values, wait_info->pValues, wait_info->semaphoreCount * sizeof(*values) );
                host_info.pValues = values;
            }
            add_sem_wait_op( device, semaphore, values[i], &((VkSemaphore *)wait_info->pSemaphores)[i], &values[i] );
        }
        else ((VkSemaphore *)wait_info->pSemaphores)[i] = semaphore->obj.host.semaphore;
    }
    ret = p_vkWaitSemaphores( device->host.device, wait_info, timeout );
    mem_free( &pool );
    if (ret && ret != VK_TIMEOUT)
        ERR( "vkWaitSemaphores failed, vr %d.\n", ret );
    return ret;
}

VkResult win32u_vkWaitSemaphores( VkDevice client_device, const VkSemaphoreWaitInfo *wait_info, uint64_t timeout )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    return wait_semaphores( device, wait_info, timeout, device->p_vkWaitSemaphores );
}

VkResult win32u_vkWaitSemaphoresKHR( VkDevice client_device, const VkSemaphoreWaitInfo *wait_info, uint64_t timeout )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );

    return wait_semaphores( device, wait_info, timeout, device->p_vkWaitSemaphoresKHR );
}

static HANDLE create_shared_semaphore_handle( D3DKMT_HANDLE local, const VkExportSemaphoreWin32HandleInfoKHR *info )
{
    SECURITY_DESCRIPTOR *security = info->pAttributes ? info->pAttributes->lpSecurityDescriptor : NULL;
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE shared;

    if (info->name) init_shared_resource_path( info->name, &name );
    InitializeObjectAttributes( &attr, info->name ? &name : NULL, OBJ_CASE_INSENSITIVE, NULL, security );

    if (!(status = NtGdiDdDDIShareObjects( 1, &local, &attr, info->dwAccess, &shared ))) return shared;
    WARN( "Failed to share resource %#x, status %#x\n", local, status );
    return NULL;
}

HANDLE open_shared_semaphore_from_name( const WCHAR *name )
{
    D3DKMT_OPENSYNCOBJECTNTHANDLEFROMNAME open_name = {0};
    WCHAR bufferW[MAX_PATH * 2];
    UNICODE_STRING name_str = {.Buffer = bufferW};
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;

    init_shared_resource_path( name, &name_str );
    InitializeObjectAttributes( &attr, &name_str, OBJ_OPENIF, NULL, NULL );

    open_name.dwDesiredAccess = GENERIC_ALL;
    open_name.pObjAttrib = &attr;
    status = NtGdiDdDDIOpenSyncObjectNtHandleFromName( &open_name );
    if (status) WARN( "Failed to open %s, status %#x\n", debugstr_w( name ), status );
    return open_name.hNtHandle;
}

static VkResult win32u_vkCreateSemaphore( VkDevice client_device, const VkSemaphoreCreateInfo *client_create_info,
                                          const VkAllocationCallbacks *allocator, VkSemaphore *ret )
{
    VkSemaphoreCreateInfo *create_info = (VkSemaphoreCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportSemaphoreCreateInfoKHR *export_info = NULL;
    struct semaphore *semaphore;
    VkSemaphore host_semaphore;
    BOOL nt_shared = FALSE;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    if (!(semaphore = calloc( 1, sizeof(*semaphore) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    list_init( &semaphore->pending_signals );
    list_init( &semaphore->pending_waits );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO:
            export_info = (VkExportSemaphoreCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_SEMAPHORE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                if (export_info->handleTypes & VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT)
                {
                    WARN( "Creating d3d12 shared fence.\n" );
                    semaphore->info.type = 1;
                    semaphore->info.pid = GetCurrentProcessId();
                    semaphore->info.t = calloc( 1, sizeof(*semaphore->info.t) );
                    semaphore->info.t->ref = 1;
                    pthread_mutex_init( &semaphore->info.t->mutex, NULL );
                }
                export_info->handleTypes = get_host_external_semaphore_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR:
            export_win32 = *(VkExportSemaphoreWin32HandleInfoKHR *)*next;
            *next = (*next)->pNext; next = &prev;
            break;
        case VK_STRUCTURE_TYPE_QUERY_LOW_LATENCY_SUPPORT_NV: break;
        case VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO: break;
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if ((res = device->p_vkCreateSemaphore( device->host.device, create_info, NULL /* allocator */, &host_semaphore )))
    {
        d3d12_fence_timeline_release( semaphore->info.t );
        free( semaphore );
        return res;
    }

    if (export_info)
    {
        VkSemaphoreGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, .semaphore = host_semaphore};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_semaphore_type()))
        {
        case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetSemaphoreFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        semaphore->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &semaphore->global, &semaphore->info, sizeof(semaphore->info) );
        close( fd );

        if (!semaphore->local) goto failed;
        if (nt_shared && !(semaphore->shared = create_shared_semaphore_handle( semaphore->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &semaphore->obj.obj, host_semaphore );
    instance->p_insert_object( instance, &semaphore->obj.obj );
    if (semaphore->info.type)
    {
        semaphore->shared_sem = host_semaphore;
        register_sem_poll( device, semaphore );
    }

    *ret = semaphore->obj.client.semaphore;
    TRACE( "semaphore %p.\n", semaphore );
    return res;

failed:
    WARN( "Failed to create semaphore, res %d\n", res );
    device->p_vkDestroySemaphore( device->host.device, host_semaphore, NULL );
    d3dkmt_destroy_sync( semaphore->local );
    d3d12_fence_timeline_release( semaphore->info.t );
    free( semaphore );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroySemaphore( VkDevice client_device, VkSemaphore client_semaphore, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( client_semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;
    VkSemaphore sem;

    TRACE( "device %p, semaphore %p, allocator %p\n", device, semaphore, allocator );

    if (!client_semaphore) return;

    if (semaphore->poll_entry.next) unregister_sem_poll( device, semaphore );
    sem = semaphore->info.type ? semaphore->shared_sem : semaphore->obj.host.semaphore;
    device->p_vkDestroySemaphore( device->host.device, sem, NULL /* allocator */ );
    instance->p_remove_object( instance, &semaphore->obj.obj );

    if (semaphore->shared) NtClose( semaphore->shared );
    d3dkmt_destroy_sync( semaphore->local );
    d3d12_fence_timeline_release( semaphore->info.t );
    free( semaphore );
}

static VkResult win32u_vkGetSemaphoreWin32HandleKHR( VkDevice client_device, const VkSemaphoreGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", semaphore->global );
        *handle = UlongToPtr( semaphore->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        NtDuplicateObject( NtCurrentProcess(), semaphore->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", semaphore->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportSemaphoreWin32HandleKHR( VkDevice client_device, const VkImportSemaphoreWin32HandleInfoKHR *handle_info )
{
    VkImportSemaphoreFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct semaphore *semaphore = semaphore_from_handle( handle_info->semaphore );
    struct vulkan_instance *instance = device->physical_device->instance;
    struct d3d12_fence_info fence_info = { 0 };
    D3DKMT_HANDLE local, global = 0;
    VkResult res = VK_SUCCESS;
    HANDLE shared = NULL;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL, &fence_info, sizeof(fence_info) ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
    case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared, &fence_info, sizeof(fence_info) )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    TRACE( "fence_info type %d, pid %04x.\n", fence_info.type, fence_info.pid );
    if (fence_info.type && fence_info.pid != GetCurrentProcessId())
    {
        FIXME( "d3d12 fence from other process.\n" );
        memset( &semaphore->info, 0, sizeof(semaphore->info) );
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    if (!res && handle_info->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT)
    {
        /* Recreate semaphore to make sure it has timeline type. */
        VkSemaphoreTypeCreateInfo type_info =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        };
        VkSemaphoreCreateInfo create_info =
        {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_info,
        };
        VkSemaphore new_semaphore;

        if ((res = device->p_vkCreateSemaphore( device->host.device, &create_info, NULL, &new_semaphore )))
        {
            ERR( "Failed to create timeline semaphore, vr %d.\n", res );
        }
        else
        {
            instance->p_remove_object( instance, &semaphore->obj.obj );
            device->p_vkDestroySemaphore( device->host.device, semaphore->obj.host.semaphore, NULL );
            semaphore->obj.host.semaphore = new_semaphore;
            instance->p_insert_object( instance, &semaphore->obj.obj );
        }
    }

    if (!res)
    {
        fd_info.handleType = get_host_external_semaphore_type();
        fd_info.semaphore = semaphore->obj.host.semaphore;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportSemaphoreFdKHR( device->host.device, &fd_info );
    }

    if (!res)
    {
        d3d12_fence_timeline_addref( fence_info.t );
        if (semaphore->info.type && semaphore->poll_entry.next) unregister_sem_poll( device, semaphore );
        d3d12_fence_timeline_release( semaphore->info.t );
        semaphore->info = fence_info;
        if (semaphore->info.type)
        {
            semaphore->shared_sem = semaphore->obj.host.semaphore;
            register_sem_poll( device, semaphore );
        }
    }

    if (res || handle_info->flags & VK_SEMAPHORE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetSemaphoreWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (semaphore->shared) NtClose( semaphore->shared );
        d3dkmt_destroy_sync( semaphore->local );
        semaphore->shared = shared;
        semaphore->global = global;
        semaphore->local = local;
    }
    return res;
}

static void get_physical_device_external_semaphore_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *client_semaphore_info,
                                                               VkExternalSemaphoreProperties *semaphore_properties, PFN_vkGetPhysicalDeviceExternalSemaphoreProperties p_vkGetPhysicalDeviceExternalSemaphoreProperties )
{
    VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info = (VkPhysicalDeviceExternalSemaphoreInfo *)client_semaphore_info; /* cast away const, it has been copied in the thunks */
    VkExternalSemaphoreHandleTypeFlagBits handle_type;

    handle_type = semaphore_info->handleType;
    if (semaphore_info->handleType & EXTERNAL_SEMAPHORE_WIN32_BITS) semaphore_info->handleType = get_host_external_semaphore_type();

    p_vkGetPhysicalDeviceExternalSemaphoreProperties( physical_device->host.physical_device, semaphore_info, semaphore_properties );
    semaphore_properties->compatibleHandleTypes = handle_type;
    semaphore_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalSemaphoreProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                   VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphoreProperties );
}

static void win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalSemaphoreInfo *semaphore_info,
                                                                      VkExternalSemaphoreProperties *semaphore_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, semaphore_info %p, semaphore_properties %p\n", physical_device, semaphore_info, semaphore_properties );

    get_physical_device_external_semaphore_properties( physical_device, semaphore_info, semaphore_properties, instance->p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR );
}

static VkResult win32u_vkCreateFence( VkDevice client_device, const VkFenceCreateInfo *client_create_info, const VkAllocationCallbacks *allocator, VkFence *ret )
{
    VkFenceCreateInfo *create_info = (VkFenceCreateInfo *)client_create_info; /* cast away const, chain has been copied in the thunks */
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    VkExportSemaphoreWin32HandleInfoKHR export_win32 = {.dwAccess = GENERIC_ALL};
    VkBaseOutStructure **next, *prev = (VkBaseOutStructure *)create_info;
    struct vulkan_instance *instance = device->physical_device->instance;
    VkExportFenceCreateInfoKHR *export_info = NULL;
    BOOL nt_shared = FALSE;
    struct fence *fence;
    VkFence host_fence;
    VkResult res;

    TRACE( "device %p, create_info %p, allocator %p, ret %p\n", device, create_info, allocator, ret );

    for (next = &prev->pNext; *next; prev = *next, next = &(*next)->pNext)
    {
        switch ((*next)->sType)
        {
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO:
            export_info = (VkExportFenceCreateInfoKHR *)*next;
            if (!(export_info->handleTypes & EXTERNAL_FENCE_WIN32_BITS))
                FIXME( "Unsupported handle types %#x\n", export_info->handleTypes );
            else
            {
                nt_shared = !(export_info->handleTypes & VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT);
                export_info->handleTypes = get_host_external_fence_type();
            }
            break;
        case VK_STRUCTURE_TYPE_EXPORT_FENCE_WIN32_HANDLE_INFO_KHR:
        {
            VkExportFenceWin32HandleInfoKHR *fence_win32 = (VkExportFenceWin32HandleInfoKHR *)*next;
            export_win32.pAttributes = fence_win32->pAttributes;
            export_win32.dwAccess = fence_win32->dwAccess;
            export_win32.name = fence_win32->name;
            *next = (*next)->pNext; next = &prev;
            break;
        }
        default: FIXME( "Unhandled sType %u.\n", (*next)->sType ); break;
        }
    }

    if (!(fence = calloc( 1, sizeof(*fence) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;

    if ((res = device->p_vkCreateFence( device->host.device, create_info, NULL /* allocator */, &host_fence )))
    {
        free( fence );
        return res;
    }

    if (export_info)
    {
        VkFenceGetFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = host_fence};
        int fd = -1;

        switch ((fd_info.handleType = get_host_external_fence_type()))
        {
        case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT:
            if ((res = device->p_vkGetFenceFdKHR( device->host.device, &fd_info, &fd ))) goto failed;
            break;
        default:
            FIXME( "Unsupported handle type %#x\n", fd_info.handleType );
            break;
        }

        fence->local = d3dkmt_create_sync( fd, nt_shared ? NULL : &fence->global, NULL, 0 );
        close( fd );

        if (!fence->local) goto failed;
        if (nt_shared && !(fence->shared = create_shared_semaphore_handle( fence->local, &export_win32 ))) goto failed;
    }

    vulkan_object_init( &fence->obj.obj, host_fence );
    instance->p_insert_object( instance, &fence->obj.obj );

    *ret = fence->obj.client.fence;
    return res;

failed:
    WARN( "Failed to create fence, res %d\n", res );
    device->p_vkDestroyFence( device->host.device, host_fence, NULL );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void win32u_vkDestroyFence( VkDevice client_device, VkFence client_fence, const VkAllocationCallbacks *allocator )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( client_fence );
    struct vulkan_instance *instance = device->physical_device->instance;

    TRACE( "device %p, fence %p, allocator %p\n", device, fence, allocator );

    if (!client_fence) return;

    device->p_vkDestroyFence( device->host.device, fence->obj.host.fence, NULL /* allocator */ );
    instance->p_remove_object( instance, &fence->obj.obj );

    if (fence->shared) NtClose( fence->shared );
    d3dkmt_destroy_sync( fence->local );
    free( fence );
}

static VkResult win32u_vkGetFenceWin32HandleKHR( VkDevice client_device, const VkFenceGetWin32HandleInfoKHR *handle_info, HANDLE *handle )
{
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );

    TRACE( "device %p, handle_info %p, handle %p\n", device, handle_info, handle );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        TRACE( "Returning global D3DKMT handle %#x\n", fence->global );
        *handle = UlongToPtr( fence->global );
        return VK_SUCCESS;

    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        NtDuplicateObject( NtCurrentProcess(), fence->shared, NtCurrentProcess(), handle, 0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS );
        TRACE( "Returning NT shared handle %p -> %p\n", fence->shared, *handle );
        return VK_SUCCESS;

    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }
}

static VkResult win32u_vkImportFenceWin32HandleKHR( VkDevice client_device, const VkImportFenceWin32HandleInfoKHR *handle_info )
{
    VkImportFenceFdInfoKHR fd_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR};
    struct vulkan_device *device = vulkan_device_from_handle( client_device );
    struct fence *fence = fence_from_handle( handle_info->fence );
    D3DKMT_HANDLE local, global = 0;
    HANDLE shared = NULL;
    VkResult res;

    TRACE( "device %p, handle_info %p\n", device, handle_info );

    switch (handle_info->handleType)
    {
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT:
        global = PtrToUlong( handle_info->handle );
        if (!(local = d3dkmt_open_sync( global, NULL, NULL, 0 ))) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        break;
    case VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT:
        if (handle_info->name && !(shared = open_shared_semaphore_from_name( handle_info->name )))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else if (!(shared = handle_info->handle) || NtDuplicateObject( NtCurrentProcess(), shared, NtCurrentProcess(), &shared,
                                                                       0, 0, DUPLICATE_SAME_ATTRIBUTES | DUPLICATE_SAME_ACCESS ))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;

        if (!(local = d3dkmt_open_sync( 0, shared, NULL, 0 )))
        {
            NtClose( shared );
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
        }
        break;
    default:
        FIXME( "Unsupported handle type %#x\n", handle_info->handleType );
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    if ((fd_info.fd = d3dkmt_object_get_fd( local )) < 0) res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    else
    {
        fd_info.handleType = get_host_external_fence_type();
        fd_info.fence = fence->obj.host.fence;
        fd_info.flags = handle_info->flags;
        res = device->p_vkImportFenceFdKHR( device->host.device, &fd_info );
    }

    if (res || handle_info->flags & VK_FENCE_IMPORT_TEMPORARY_BIT)
    {
        /* FIXME: Should we still keep the temporary handles for vkGetFenceWin32HandleKHR? */
        if (shared) NtClose( shared );
        d3dkmt_destroy_sync( local );
    }
    else
    {
        if (fence->shared) NtClose( fence->shared );
        if (fence->local) d3dkmt_destroy_sync( fence->local );
        fence->shared = shared;
        fence->global = global;
        fence->local = local;
    }
    return VK_SUCCESS;
}

static void get_physical_device_external_fence_properties( struct vulkan_physical_device *physical_device, const VkPhysicalDeviceExternalFenceInfo *client_fence_info,
                                                           VkExternalFenceProperties *fence_properties, PFN_vkGetPhysicalDeviceExternalFenceProperties p_vkGetPhysicalDeviceExternalFenceProperties )
{
    VkPhysicalDeviceExternalFenceInfo *fence_info = (VkPhysicalDeviceExternalFenceInfo *)client_fence_info; /* cast away const, it has been copied in the thunks */
    VkExternalFenceHandleTypeFlagBits handle_type;

    handle_type = fence_info->handleType;
    if (fence_info->handleType & EXTERNAL_FENCE_WIN32_BITS) fence_info->handleType = get_host_external_fence_type();

    p_vkGetPhysicalDeviceExternalFenceProperties( physical_device->host.physical_device, fence_info, fence_properties );
    fence_properties->compatibleHandleTypes = handle_type;
    fence_properties->exportFromImportedHandleTypes = handle_type;
}

static void win32u_vkGetPhysicalDeviceExternalFenceProperties( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                               VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFenceProperties );
}

static void win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR( VkPhysicalDevice client_physical_device, const VkPhysicalDeviceExternalFenceInfo *fence_info,
                                                                  VkExternalFenceProperties *fence_properties )
{
    struct vulkan_physical_device *physical_device = vulkan_physical_device_from_handle( client_physical_device );
    struct vulkan_instance *instance = physical_device->instance;

    TRACE( "physical_device %p, fence_info %p, fence_properties %p\n", physical_device, fence_info, fence_properties );

    get_physical_device_external_fence_properties( physical_device, fence_info, fence_properties, instance->p_vkGetPhysicalDeviceExternalFencePropertiesKHR );
}

static struct vulkan_funcs vulkan_funcs =
{
    .p_vkAcquireNextImage2KHR = win32u_vkAcquireNextImage2KHR,
    .p_vkAcquireNextImageKHR = win32u_vkAcquireNextImageKHR,
    .p_vkAllocateMemory = win32u_vkAllocateMemory,
    .p_vkCreateBuffer = win32u_vkCreateBuffer,
    .p_vkCreateDevice = win32u_vkCreateDevice,
    .p_vkCreateFence = win32u_vkCreateFence,
    .p_vkCreateImage = win32u_vkCreateImage,
    .p_vkCreateInstance = win32u_vkCreateInstance,
    .p_vkCreateSemaphore = win32u_vkCreateSemaphore,
    .p_vkCreateSwapchainKHR = win32u_vkCreateSwapchainKHR,
    .p_vkCreateWin32SurfaceKHR = win32u_vkCreateWin32SurfaceKHR,
    .p_vkDestroyDevice = win32u_vkDestroyDevice,
    .p_vkDestroyFence = win32u_vkDestroyFence,
    .p_vkDestroyInstance = win32u_vkDestroyInstance,
    .p_vkDestroySemaphore = win32u_vkDestroySemaphore,
    .p_vkDestroySurfaceKHR = win32u_vkDestroySurfaceKHR,
    .p_vkDestroySwapchainKHR = win32u_vkDestroySwapchainKHR,
    .p_vkFreeMemory = win32u_vkFreeMemory,
    .p_vkGetDeviceBufferMemoryRequirements = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceBufferMemoryRequirementsKHR = win32u_vkGetDeviceBufferMemoryRequirements,
    .p_vkGetDeviceImageMemoryRequirements = win32u_vkGetDeviceImageMemoryRequirements,
    .p_vkGetDeviceQueue = win32u_vkGetDeviceQueue,
    .p_vkGetDeviceQueue2 = win32u_vkGetDeviceQueue2,
    .p_vkGetFenceWin32HandleKHR = win32u_vkGetFenceWin32HandleKHR,
    .p_vkGetMemoryWin32HandleKHR = win32u_vkGetMemoryWin32HandleKHR,
    .p_vkGetMemoryWin32HandlePropertiesKHR = win32u_vkGetMemoryWin32HandlePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalBufferProperties = win32u_vkGetPhysicalDeviceExternalBufferProperties,
    .p_vkGetPhysicalDeviceExternalBufferPropertiesKHR = win32u_vkGetPhysicalDeviceExternalBufferPropertiesKHR,
    .p_vkGetPhysicalDeviceExternalFenceProperties = win32u_vkGetPhysicalDeviceExternalFenceProperties,
    .p_vkGetPhysicalDeviceExternalFencePropertiesKHR = win32u_vkGetPhysicalDeviceExternalFencePropertiesKHR,
    .p_vkGetPhysicalDeviceExternalSemaphoreProperties = win32u_vkGetPhysicalDeviceExternalSemaphoreProperties,
    .p_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR = win32u_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR,
    .p_vkGetPhysicalDeviceImageFormatProperties2 = win32u_vkGetPhysicalDeviceImageFormatProperties2,
    .p_vkGetPhysicalDeviceImageFormatProperties2KHR = win32u_vkGetPhysicalDeviceImageFormatProperties2KHR,
    .p_vkGetPhysicalDevicePresentRectanglesKHR = win32u_vkGetPhysicalDevicePresentRectanglesKHR,
    .p_vkGetPhysicalDeviceProperties = win32u_vkGetPhysicalDeviceProperties,
    .p_vkGetPhysicalDeviceProperties2 = win32u_vkGetPhysicalDeviceProperties2,
    .p_vkGetPhysicalDeviceProperties2KHR = win32u_vkGetPhysicalDeviceProperties2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilities2KHR = win32u_vkGetPhysicalDeviceSurfaceCapabilities2KHR,
    .p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = win32u_vkGetPhysicalDeviceSurfaceCapabilitiesKHR,
    .p_vkGetPhysicalDeviceSurfaceFormats2KHR = win32u_vkGetPhysicalDeviceSurfaceFormats2KHR,
    .p_vkGetPhysicalDeviceSurfaceFormatsKHR = win32u_vkGetPhysicalDeviceSurfaceFormatsKHR,
    .p_vkGetPhysicalDeviceWin32PresentationSupportKHR = win32u_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    .p_vkGetPastPresentationTimingEXT = win32u_vkGetPastPresentationTimingEXT,
    .p_vkGetLatencyTimingsNV = win32u_vkGetLatencyTimingsNV,
    .p_vkGetSemaphoreWin32HandleKHR = win32u_vkGetSemaphoreWin32HandleKHR,
    .p_vkGetSwapchainTimingPropertiesEXT = win32u_vkGetSwapchainTimingPropertiesEXT,
    .p_vkGetSwapchainImagesKHR = win32u_vkGetSwapchainImagesKHR,
    .p_vkImportFenceWin32HandleKHR = win32u_vkImportFenceWin32HandleKHR,
    .p_vkImportSemaphoreWin32HandleKHR = win32u_vkImportSemaphoreWin32HandleKHR,
    .p_vkLatencySleepNV = win32u_vkLatencySleepNV,
    .p_vkMapMemory = win32u_vkMapMemory,
    .p_vkMapMemory2KHR = win32u_vkMapMemory2KHR,
    .p_vkQueuePresentKHR = win32u_vkQueuePresentKHR,
    .p_vkReleaseSwapchainImagesEXT = win32u_vkReleaseSwapchainImagesEXT,
    .p_vkReleaseSwapchainImagesKHR = win32u_vkReleaseSwapchainImagesKHR,
    .p_vkSetHdrMetadataEXT = win32u_vkSetHdrMetadataEXT,
    .p_vkSetLatencyMarkerNV = win32u_vkSetLatencyMarkerNV,
    .p_vkSetSwapchainPresentTimingQueueSizeEXT = win32u_vkSetSwapchainPresentTimingQueueSizeEXT,
    .p_vkQueueSubmit = win32u_vkQueueSubmit,
    .p_vkQueueSubmit2 = win32u_vkQueueSubmit2,
    .p_vkQueueSubmit2KHR = win32u_vkQueueSubmit2KHR,
    .p_vkUnmapMemory = win32u_vkUnmapMemory,
    .p_vkUnmapMemory2KHR = win32u_vkUnmapMemory2KHR,
    .p_vkWaitForPresentKHR = win32u_vkWaitForPresentKHR,
    .p_vkWaitForPresent2KHR = win32u_vkWaitForPresent2KHR,
    .p_vkGetSemaphoreCounterValue = win32u_vkGetSemaphoreCounterValue,
    .p_vkGetSemaphoreCounterValueKHR = win32u_vkGetSemaphoreCounterValueKHR,
    .p_vkSignalSemaphore = win32u_vkSignalSemaphore,
    .p_vkSignalSemaphoreKHR = win32u_vkSignalSemaphoreKHR,
    .p_vkWaitSemaphores = win32u_vkWaitSemaphores,
    .p_vkWaitSemaphoresKHR = win32u_vkWaitSemaphoresKHR,
};

static VkResult nulldrv_vulkan_surface_create( HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *surface, struct client_surface **client )
{
    VkHeadlessSurfaceCreateInfoEXT create_info = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkResult res;

    if (!(*client = nulldrv_client_surface_create( hwnd ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((res = instance->p_vkCreateHeadlessSurfaceEXT( instance->host.instance, &create_info, NULL, surface )))
    {
        client_surface_release(*client);
        *client = NULL;
    }

    return res;
}

static VkColorSpaceKHR nulldrv_vulkan_map_colorspace( VkColorSpaceKHR colorspace, struct client_surface *client )
{
    return colorspace;
}

static void nulldrv_vulkan_surface_set_alpha( VkCompositeAlphaFlagBitsKHR alpha_bits,
                                              struct client_surface *client )
{
}

static VkBool32 nulldrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    return VK_TRUE;
}

static UINT nulldrv_vulkan_get_hwnd_dmabuf_caps( HWND hwnd, void *caps, void *format_modifiers,
                                                 UINT max_format_modifiers, UINT *format_modifier_count )
{
    if (format_modifier_count) *format_modifier_count = 0;
    return HWND_DMABUF_NOT_FOUND;
}

static void nulldrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_EXT_headless_surface = 1;
    if (extensions->has_VK_EXT_headless_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void nulldrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
    extensions->has_VK_WINE_openvr_device_extensions = 1;
    extensions->has_VK_WINE_openxr_device_extensions = 1;
}

static const struct vulkan_driver_funcs nulldrv_funcs =
{
    .p_vulkan_surface_create = nulldrv_vulkan_surface_create,
    .p_vulkan_map_colorspace = nulldrv_vulkan_map_colorspace,
    .p_vulkan_surface_set_alpha = nulldrv_vulkan_surface_set_alpha,
    .p_get_physical_device_presentation_support = nulldrv_get_physical_device_presentation_support,
    .p_vulkan_get_hwnd_dmabuf_caps = nulldrv_vulkan_get_hwnd_dmabuf_caps,
    .p_map_instance_extensions = nulldrv_map_instance_extensions,
    .p_map_device_extensions = nulldrv_map_device_extensions,
};

static void vulkan_driver_init(void)
{
    UINT status;

    if ((status = user_driver->pVulkanInit( WINE_VULKAN_DRIVER_VERSION, vulkan_handle, &driver_funcs )) &&
        status != STATUS_NOT_IMPLEMENTED)
    {
        ERR( "Failed to initialize the driver vulkan functions, status %#x\n", status );
        return;
    }

    if (status == STATUS_NOT_IMPLEMENTED) driver_funcs = &nulldrv_funcs;
}

static void vulkan_driver_load(void)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;
    pthread_once( &init_once, vulkan_driver_init );
}

static VkResult lazydrv_vulkan_surface_create( HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *surface, struct client_surface **client )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_surface_create( hwnd, raw, instance, surface, client );
}

static VkColorSpaceKHR lazydrv_vulkan_map_colorspace( VkColorSpaceKHR colorspace, struct client_surface *client )
{
    vulkan_driver_load();
    return driver_funcs->p_vulkan_map_colorspace( colorspace, client );
}

static void lazydrv_vulkan_surface_set_alpha( VkCompositeAlphaFlagBitsKHR alpha_bits,
                                              struct client_surface *client )
{
    vulkan_driver_load();
    driver_funcs->p_vulkan_surface_set_alpha( alpha_bits, client );
}

static VkBool32 lazydrv_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    vulkan_driver_load();
    return driver_funcs->p_get_physical_device_presentation_support( physical_device, queue );
}

static UINT lazydrv_vulkan_get_hwnd_dmabuf_caps( HWND hwnd, void *caps, void *format_modifiers,
                                                 UINT max_format_modifiers, UINT *format_modifier_count )
{
    vulkan_driver_load();
    if (!driver_funcs->p_vulkan_get_hwnd_dmabuf_caps)
    {
        if (format_modifier_count) *format_modifier_count = 0;
        return HWND_DMABUF_NOT_FOUND;
    }
    return driver_funcs->p_vulkan_get_hwnd_dmabuf_caps( hwnd, caps, format_modifiers,
                                                        max_format_modifiers, format_modifier_count );
}

static void lazydrv_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_instance_extensions( extensions );
}

static void lazydrv_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    vulkan_driver_load();
    return driver_funcs->p_map_device_extensions( extensions );
}

static const struct vulkan_driver_funcs lazydrv_funcs =
{
    .p_vulkan_surface_create = lazydrv_vulkan_surface_create,
    .p_vulkan_map_colorspace = lazydrv_vulkan_map_colorspace,
    .p_vulkan_surface_set_alpha = lazydrv_vulkan_surface_set_alpha,
    .p_get_physical_device_presentation_support = lazydrv_get_physical_device_presentation_support,
    .p_vulkan_get_hwnd_dmabuf_caps = lazydrv_vulkan_get_hwnd_dmabuf_caps,
    .p_map_instance_extensions = lazydrv_map_instance_extensions,
    .p_map_device_extensions = lazydrv_map_device_extensions,
};

static void vulkan_init_once(void)
{
    struct vulkan_instance_extensions extensions = {0};
    VkExtensionProperties *properties = NULL;
    uint32_t count = 0;
    VkResult res;

    const char *env = getenv( "WINE_DISABLE_FULLSCREEN_HACK" );
    fshack_enabled = !env || !atoi( env );

#ifdef SONAME_LIBVULKAN
    vulkan_handle = dlopen( SONAME_LIBVULKAN, RTLD_NOW );
    if (!vulkan_handle) ERR( "Failed to load %s\n", SONAME_LIBVULKAN );
#else
    ERR( "Wine was built without Vulkan support.\n" );
#endif
    if (!vulkan_handle) return;

#define LOAD_FUNCPTR( f )                                                                          \
    if (!(p_##f = dlsym( vulkan_handle, #f )))                                                     \
    {                                                                                              \
        ERR( "Failed to find " #f "\n" );                                                          \
        dlclose( vulkan_handle );                                                                  \
        vulkan_handle = NULL;                                                                      \
        return;                                                                                    \
    }

    LOAD_FUNCPTR( vkGetDeviceProcAddr );
    LOAD_FUNCPTR( vkGetInstanceProcAddr );
#undef LOAD_FUNCPTR

    driver_funcs = &lazydrv_funcs;
    vulkan_funcs.p_vkGetInstanceProcAddr = p_vkGetInstanceProcAddr;
    vulkan_funcs.p_vkGetDeviceProcAddr = p_vkGetDeviceProcAddr;

#define LOAD_FUNCPTR( f ) p_##f = (PFN_##f)p_vkGetInstanceProcAddr( NULL, #f );
    LOAD_FUNCPTR( vkCreateInstance );
    LOAD_FUNCPTR( vkEnumerateInstanceExtensionProperties );
#undef LOAD_FUNCPTR

    do
    {
        free( properties );
        properties = NULL;
        if ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, NULL ))) goto failed;
        if (!count || !(properties = malloc( count * sizeof(*properties) ))) goto failed;
    } while ((res = p_vkEnumerateInstanceExtensionProperties( NULL, &count, properties ) == VK_INCOMPLETE));
    if (res) goto failed;

    TRACE( "Host instance extensions:\n" );
    for (uint32_t i = 0; i < count; i++)
    {
        const char *extension = properties[i].extensionName;
#define USE_VK_EXT(x)                           \
        if (!strcmp( extension, #x ))           \
        {                                       \
            extensions.has_ ## x = 1;           \
            TRACE( "  - %s\n", extension );     \
        } else
        ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT
        WARN( "Extension %s is not supported.\n", debugstr_a(extension) );
    }
    vulkan_funcs.host_extensions = extensions;

    /* map host instance extensions for VK_KHR_win32_surface */
    driver_funcs->p_map_instance_extensions( &extensions );

    /* filter out unsupported client instance extensions */
#define USE_VK_EXT(x) vulkan_funcs.client_extensions.has_ ## x = extensions.has_ ## x;
    ALL_VK_CLIENT_INSTANCE_EXTS
#undef USE_VK_EXT

failed:
    if (res) ERR( "Failed to initialize instance extensions, res %d\n", res );
    free( properties );
}

static LONG vulkan_init_active;

BOOL vulkan_init_in_progress(void)
{
    return ReadAcquire( &vulkan_init_active ) != 0;
}

/***********************************************************************
 *      __wine_get_vulkan_driver  (win32u.so)
 */
const struct vulkan_funcs *__wine_get_vulkan_driver( UINT version )
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, vulkan wants %u but win32u has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return NULL;
    }

    InterlockedIncrement( &vulkan_init_active );
    pthread_once( &init_once, vulkan_init_once );
    InterlockedDecrement( &vulkan_init_active );
    if (!vulkan_handle) return NULL;
    return &vulkan_funcs;
}

/* unix side client-like instance wrapper to fit with the vulkan wrapping infrastructure */
struct instance_wrapper
{
    struct VkInstance_T client;
};

struct vulkan_instance *vulkan_instance_create( const struct vulkan_instance_extensions *extensions )
{
    const struct vulkan_funcs *funcs = __wine_get_vulkan_driver( WINE_VULKAN_DRIVER_VERSION );
    VkInstanceCreateInfo create_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char *extension_names[sizeof(*extensions) * 8];
    struct instance_wrapper *wrapper;
    UINT device_count = 8;
    VkResult res;

    if (!funcs) return NULL;

    create_info.ppEnabledExtensionNames = extension_names;
#define USE_VK_EXT(x) if (extensions->has_ ## x) extension_names[create_info.enabledExtensionCount++] = #x;
    ALL_VK_INSTANCE_EXTS
#undef USE_VK_EXT

    for (;;)
    {
        VkInstance instance;

        if (!(wrapper = calloc( 1, offsetof(struct instance_wrapper, client.physical_device[device_count]) ))) return NULL;
        wrapper->client.physical_device_count = device_count;
        wrapper->client.extensions = *extensions;
        instance = &wrapper->client;

        if ((res = funcs->p_vkCreateInstance( &create_info, NULL, &instance ))) break;
        if ((wrapper->client.physical_device_count <= device_count)) break;
        device_count = wrapper->client.physical_device_count;
        free( wrapper );
    }

    if (!res) return vulkan_instance_from_handle( &wrapper->client );
    WARN( "Failed to create instance, res %d\n", res );
    free( wrapper );
    return NULL;
}
