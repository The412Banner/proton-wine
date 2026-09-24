#include <stddef.h>
#include "steamclient_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(steamclient);

/* ── Bannerlator server browser: PE-side item pointer cache ─────────────────────────────────
 * With the Bannerlator browser (BL_SERVER_BROWSER=1) the unix side keeps every item at a stable
 * address for the life of the request (deque; refresh clears it, and RefreshQuery/ReleaseRequest
 * pass through here first). The game's list sort re-fetches details for every comparison — CS:S
 * makes ~800k GetServerDetails calls for a 5000-server list — so on 64-bit we hand back the unix
 * pointer directly after the first fetch instead of a unix call + copy each time. */
BOOL bl_browser_active(void)
{
    static int v = -1;
    if (v < 0)
    {
        char b[4] = {0};
        v = (GetEnvironmentVariableA( "BL_SERVER_BROWSER", b, sizeof(b) ) && b[0] && b[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

/* x86-64 front (blsteambrowser.dll, next to this DLL): the game's sort-driven
 * GetServerDetails/GetServerCount/IsRefreshing stay inside the x86 JIT. 64-bit only. */
#ifdef _WIN64
typedef void *(__cdecl *bl_shim_create_fn)( UINT32, void *, UINT32, UINT32, UINT32, UINT32 );
static bl_shim_create_fn bl_shim_create_ptr;
static int bl_shim_state; /* 0 untried, 1 ok, -1 unavailable */
static struct { struct w_iface *real; struct w_iface *shim; } bl_shims[8];
static int bl_shim_count;

static void bl_shim_load(void)
{
    WCHAR path[MAX_PATH];
    HMODULE self, dll;
    WCHAR *p;
    bl_shim_state = -1;
    /* By name first: the file ships in the layer's lib/wine/<arch>-windows next to this DLL, which
     * Wine's loader searches for a bare name (our own module path reports as system32). */
    if (!(dll = LoadLibraryW( L"blsteambrowser.dll" )))
    {
        DWORD err = GetLastError();
        if (GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (const WCHAR *)bl_shim_load, &self ) && GetModuleFileNameW( self, path, MAX_PATH ))
        {
            for (p = path + lstrlenW( path ); p > path && p[-1] != '\\'; p--) ;
            if (p != path) { lstrcpyW( p, L"blsteambrowser.dll" ); dll = LoadLibraryW( path ); }
        }
        if (!dll) { WARN( "no blsteambrowser.dll (%lu, by path %s %lu) — browser calls stay ARM64EC\n", err, debugstr_w( path ), GetLastError() ); return; }
    }
    lstrcpyW( path, L"blsteambrowser.dll" );
    bl_shim_create_ptr = (bl_shim_create_fn)GetProcAddress( dll, "bl_shim_create" );
    if (bl_shim_create_ptr) { bl_shim_state = 1; TRACE( "loaded %s\n", debugstr_w( path ) ); }
}
#endif

struct w_iface *bl_shim_wrap( const char *name, struct w_iface *real )
{
#ifdef _WIN64
    UINT32 version;
    void *shim;
    int i;
    if (!real || !bl_browser_active()) return real;
    if (!strcmp( name, "SteamMatchMakingServers002" )) version = 2;
    else if (!strcmp( name, "SteamMatchMakingServers003" )) version = 3;
    else return real;
    if (!bl_shim_state) bl_shim_load();
    if (bl_shim_state < 0) return real;
    for (i = 0; i < bl_shim_count; i++) if (bl_shims[i].real == real) return bl_shims[i].shim;
    shim = bl_shim_create_ptr( version, real, sizeof(gameserveritem_t_165),
                               offsetof( struct w_request, bl_items ), offsetof( struct w_request, bl_items_count ),
                               offsetof( struct w_request, bl_refreshing ) );
    if (!shim) return real;
    if (bl_shim_count < 8) { bl_shims[bl_shim_count].real = real; bl_shims[bl_shim_count].shim = shim; bl_shim_count++; }
    TRACE( "%s: x86 front %p over %p\n", name, shim, real );
    return shim;
#else
    return real;
#endif
}

void bl_cache_reset( struct w_request *request )
{
    if (!request) return;
    if (request->bl_ptrs) HeapFree( GetProcessHeap(), 0, (void *)(UINT_PTR)request->bl_ptrs );
    request->bl_ptrs = 0;
    request->bl_count = 0;
    if (request->details) HeapFree( GetProcessHeap(), 0, request->details );
    request->details = NULL;
    request->details_count = 0;
}


/* Some games (e.g. The Forest) call ReleaseRequest more than once on the same
 * server list request handle, sometimes from different threads. This is
 * harmless on Windows, but here the handle is our own w_request allocation, so
 * a second release would pass freed memory to the native client and free it
 * again. Keep track of live requests and ignore releases of unknown ones. */
struct live_request
{
    struct list entry;
    struct w_request *request;
};

static struct list live_requests = LIST_INIT( live_requests );
static CRITICAL_SECTION live_requests_cs = { NULL, -1, 0, 0, 0, 0 };

static void track_request( struct w_request *request )
{
    struct live_request *live;

    if (!(live = HeapAlloc( GetProcessHeap(), 0, sizeof(*live) ))) return;
    live->request = request;

    EnterCriticalSection( &live_requests_cs );
    list_add_tail( &live_requests, &live->entry );
    LeaveCriticalSection( &live_requests_cs );
}

static BOOL untrack_request( struct w_request *request )
{
    struct live_request *live;
    BOOL found = FALSE;

    EnterCriticalSection( &live_requests_cs );
    LIST_FOR_EACH_ENTRY( live, &live_requests, struct live_request, entry )
    {
        if (live->request != request) continue;
        list_remove( &live->entry );
        HeapFree( GetProcessHeap(), 0, live );
        found = TRUE;
        break;
    }
    LeaveCriticalSection( &live_requests_cs );

    return found;
}

void __thiscall winISteamMatchmakingServers_SteamMatchMakingServers001_CancelQuery(struct w_iface *_this, uint32_t eType)
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers001_CancelQuery_params params =
    {
        .u_iface = _this->u_iface,
        .eType = eType,
    };
    TRACE("%p\n", _this);
    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers001_CancelQuery, &params );
    execute_pending_callbacks();
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_RequestInternetServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_RequestInternetServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_RequestInternetServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_RequestLANServerList( struct w_iface *_this, uint32_t iApp, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_RequestLANServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_RequestLANServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_RequestFriendsServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_RequestFriendsServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_RequestFriendsServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_RequestFavoritesServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_RequestFavoritesServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_RequestFavoritesServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_RequestHistoryServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_RequestHistoryServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_RequestHistoryServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_RequestSpectatorServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_RequestSpectatorServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_RequestSpectatorServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_CancelQuery(struct w_iface *_this, void *hRequest)
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_CancelQuery_params params =
    {
        .u_iface = _this->u_iface,
        .hRequest = hRequest,
    };
    TRACE("%p\n", _this);
    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_CancelQuery, &params );
    execute_pending_callbacks();
}

void __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_ReleaseRequest( struct w_iface *_this, void *hServerListRequest )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_ReleaseRequest_params params =
    {
        .u_iface = _this->u_iface,
        .hServerListRequest = hServerListRequest,
    };
    struct w_request *request = hServerListRequest;

    TRACE( "%p %p\n", _this, hServerListRequest );

    if (request && !untrack_request( request ))
    {
        WARN( "ignoring release of unknown or already released request %p\n", request );
        return;
    }

    execute_pending_callbacks(); /* execute any pending callbacks that might still need to use the request */

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_ReleaseRequest, &params );

    bl_cache_reset( request );
    HeapFree( GetProcessHeap(), 0, request );
}

gameserveritem_t_105 * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers002_GetServerDetails( struct w_iface *_this, void *hRequest, int32_t iServer )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerDetails_params params =
    {
        .u_iface = _this->u_iface,
        .hRequest = hRequest,
        .iServer = iServer,
    };
    struct w_request *request = hRequest;

    TRACE( "%p\n", _this );

#ifdef _WIN64
    if (request && bl_browser_active())
    {
        void **ptrs = (void **)(UINT_PTR)request->bl_ptrs;
        if (!ptrs)
        {
            struct ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerCount_params count_params =
            {
                .u_iface = _this->u_iface,
                .hRequest = hRequest,
            };
            STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerCount, &count_params );
            if (count_params._ret > 0 && (ptrs = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)count_params._ret * sizeof(void *) )))
            {
                request->bl_ptrs = (UINT_PTR)ptrs;
                request->bl_count = count_params._ret;
            }
        }
        if (ptrs && iServer >= 0 && (UINT64)iServer < request->bl_count)
        {
            if (!ptrs[iServer])
            {
                STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerDetails, &params );
                ptrs[iServer] = get_unix_buffer( params._ret );
            }
            return (gameserveritem_t_105 *)ptrs[iServer];
        }
        /* unknown count yet or out of range: normal path below */
    }
#endif
    if (request && !request->details)
    {
        struct ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerCount_params count_params =
        {
            .u_iface = _this->u_iface,
            .hRequest = hRequest,
        };

        STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerCount, &count_params );
        request->details_count = count_params._ret;
        if (count_params._ret) request->details = HeapAlloc( GetProcessHeap(), 0,
                                                             (count_params._ret + 1) * sizeof(*request->details) );
    }
    if (request && request->details && (iServer < 0 || iServer >= request->details_count))
    {
        /* Linux Steamclient will return some pointer in such a case with the structure being all zero. */
        ERR( "Invalid iServer %d, request->details_count %I64u.\n", iServer, request->details_count );
        memset( request->details + request->details_count, 0, sizeof(*request->details) );
        /* request->details is array of the latest gameserveritem_t version but so far the newer structures only
         * have added fields in the end. */
        return (gameserveritem_t_105 *)(request->details + request->details_count);
    }

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers002_GetServerDetails, &params );
    /* request->details is array of the latest gameserveritem_t version but so far the newer structures only
     * have added fields in the end. */
    if (request && request->details && params._ret.ptr) return (gameserveritem_t_105 *)(request->details + iServer);
    return get_unix_buffer( params._ret );
}

/* SteamMatchMakingServers003 */
void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_RequestInternetServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_RequestInternetServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_RequestInternetServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_RequestLANServerList( struct w_iface *_this, uint32_t iApp, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_RequestLANServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_RequestLANServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_RequestFriendsServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_RequestFriendsServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_RequestFriendsServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_RequestFavoritesServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_RequestFavoritesServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_RequestFavoritesServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_RequestHistoryServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_RequestHistoryServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_RequestHistoryServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_RequestSpectatorServerList( struct w_iface *_this, uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, w_ISteamMatchmakingServerListResponse_106 *pRequestServersResponse )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_RequestSpectatorServerList_params params =
    {
        .u_iface = _this->u_iface,
        .iApp = iApp,
        .ppchFilters = ppchFilters,
        .nFilters = nFilters,
        .pRequestServersResponse = pRequestServersResponse,
    };
    struct w_request *request;

    TRACE( "%p\n", _this );

    if (!(request = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*request) ))) return NULL;
    params._ret = request;

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_RequestSpectatorServerList, &params );
    if (!request->u_request.handle)
    {
        HeapFree( GetProcessHeap(), 0, request );
        return NULL;
    }

    track_request( request );
    return request;
}

void __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_CancelQuery(struct w_iface *_this, void *hRequest)
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_CancelQuery_params params =
    {
        .u_iface = _this->u_iface,
        .hRequest = hRequest,
    };
    TRACE("%p\n", _this);
    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_CancelQuery, &params );
    execute_pending_callbacks();
}

void __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_ReleaseRequest( struct w_iface *_this, void *hServerListRequest )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_ReleaseRequest_params params =
    {
        .u_iface = _this->u_iface,
        .hServerListRequest = hServerListRequest,
    };
    struct w_request *request = hServerListRequest;

    TRACE( "%p %p\n", _this, hServerListRequest );

    if (request && !untrack_request( request ))
    {
        WARN( "ignoring release of unknown or already released request %p\n", request );
        return;
    }

    execute_pending_callbacks(); /* execute any pending callbacks that might still need to use the request */

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_ReleaseRequest, &params );

    bl_cache_reset( request );
    HeapFree( GetProcessHeap(), 0, request );
}

gameserveritem_t_105 * __thiscall winISteamMatchmakingServers_SteamMatchMakingServers003_GetServerDetails( struct w_iface *_this, void *hRequest, int32_t iServer )
{
    struct ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerDetails_params params =
    {
        .u_iface = _this->u_iface,
        .hRequest = hRequest,
        .iServer = iServer,
    };
    struct w_request *request = hRequest;

    TRACE( "%p\n", _this );

#ifdef _WIN64
    if (request && bl_browser_active())
    {
        void **ptrs = (void **)(UINT_PTR)request->bl_ptrs;
        if (!ptrs)
        {
            struct ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerCount_params count_params =
            {
                .u_iface = _this->u_iface,
                .hRequest = hRequest,
            };
            STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerCount, &count_params );
            if (count_params._ret > 0 && (ptrs = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)count_params._ret * sizeof(void *) )))
            {
                request->bl_ptrs = (UINT_PTR)ptrs;
                request->bl_count = count_params._ret;
            }
        }
        if (ptrs && iServer >= 0 && (UINT64)iServer < request->bl_count)
        {
            if (!ptrs[iServer])
            {
                STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerDetails, &params );
                ptrs[iServer] = get_unix_buffer( params._ret );
            }
            return (gameserveritem_t_105 *)ptrs[iServer];
        }
        /* unknown count yet or out of range: normal path below */
    }
#endif
    if (request && !request->details)
    {
        struct ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerCount_params count_params =
        {
            .u_iface = _this->u_iface,
            .hRequest = hRequest,
        };

        STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerCount, &count_params );
        request->details_count = count_params._ret;
        if (count_params._ret) request->details = HeapAlloc( GetProcessHeap(), 0,
                                                             (count_params._ret + 1) * sizeof(*request->details) );
    }
    if (request && request->details && (iServer < 0 || iServer >= request->details_count))
    {
        /* Linux Steamclient will return some pointer in such a case with the structure being all zero. */
        ERR( "Invalid iServer %d, request->details_count %I64u.\n", iServer, request->details_count );
        memset( request->details + request->details_count, 0, sizeof(*request->details) );
        /* request->details is array of the latest gameserveritem_t version but so far the newer structures only
         * have added fields in the end. */
        return (gameserveritem_t_105 *)(request->details + request->details_count);
    }

    STEAMCLIENT_CALL( ISteamMatchmakingServers_SteamMatchMakingServers003_GetServerDetails, &params );
    /* request->details is array of the latest gameserveritem_t version but so far the newer structures only
     * have added fields in the end. */
    if (request && request->details && params._ret.ptr) return (gameserveritem_t_105 *)(request->details + iServer);
    return get_unix_buffer( params._ret );
}
