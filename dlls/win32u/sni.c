/*
 * StatusNotifierItem (SNI) system-tray backend, over D-Bus.
 *
 * Display-server agnostic: tried from system_tray_call() before the graphics
 * driver's pNotifyIcon. Serves winewayland.drv and winex11.drv when a
 * StatusNotifierWatcher (KDE/GNOME panel) owns the name on the session bus.
 * When absent, sni_notify_icon() returns -1 and the caller falls back to the
 * driver (XEmbed on X11) or explorer's standalone tray window.
 *
 * Copyright 2026 for the Wine project.
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "shellapi.h"
#include "ntuser.h"
#include "ntgdi.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

#ifdef SONAME_LIBDBUS_1

#include <dlfcn.h>
#include <dbus/dbus.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#define SNI_WATCHER_SERVICE   "org.kde.StatusNotifierWatcher"
#define SNI_ITEM_OBJECT       "/StatusNotifierItem"
#define SNI_WATCHER_PATH      "/StatusNotifierWatcher"
#define SNI_WATCHER_IFACE     "org.kde.StatusNotifierWatcher"
#define SNI_ITEM_IFACE        "org.kde.StatusNotifierItem"
#define NOTIFY_SERVICE        "org.freedesktop.Notifications"
#define NOTIFY_PATH           "/org/freedesktop/Notifications"
#define NOTIFY_IFACE          "org.freedesktop.Notifications"
#define DBUS_PROPS_IFACE      "org.freedesktop.DBus.Properties"
#define DBUS_INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"

/* libdbus entry points we use (loaded by name, like mountmgr/winebth). */
#define DBUS_FUNCS \
    DO_FUNC(dbus_bus_name_has_owner); \
    DO_FUNC(dbus_bus_add_match); \
    DO_FUNC(dbus_connection_add_filter); \
    DO_FUNC(dbus_message_is_signal); \
    DO_FUNC(dbus_message_iter_append_fixed_array); \
    DO_FUNC(dbus_connection_close); \
    DO_FUNC(dbus_connection_read_write_dispatch); \
    DO_FUNC(dbus_connection_send); \
    DO_FUNC(dbus_connection_try_register_object_path); \
    DO_FUNC(dbus_connection_unref); \
    DO_FUNC(dbus_connection_get_unix_fd); \
    DO_FUNC(dbus_connection_read_write); \
    DO_FUNC(dbus_connection_dispatch); \
    DO_FUNC(dbus_connection_get_dispatch_status); \
    DO_FUNC(dbus_connection_flush); \
    DO_FUNC(dbus_connection_get_is_connected); \
    DO_FUNC(dbus_connection_set_exit_on_disconnect); \
    DO_FUNC(dbus_connection_ref); \
    DO_FUNC(dbus_bus_get_unique_name); \
    DO_FUNC(dbus_bus_get_private); \
    DO_FUNC(dbus_error_free); \
    DO_FUNC(dbus_error_init); \
    DO_FUNC(dbus_error_is_set); \
    DO_FUNC(dbus_message_append_args); \
    DO_FUNC(dbus_message_get_args); \
    DO_FUNC(dbus_message_get_interface); \
    DO_FUNC(dbus_message_get_member); \
    DO_FUNC(dbus_message_get_path); \
    DO_FUNC(dbus_message_iter_append_basic); \
    DO_FUNC(dbus_message_iter_close_container); \
    DO_FUNC(dbus_message_iter_get_arg_type); \
    DO_FUNC(dbus_message_iter_get_basic); \
    DO_FUNC(dbus_message_iter_init); \
    DO_FUNC(dbus_message_iter_init_append); \
    DO_FUNC(dbus_message_iter_next); \
    DO_FUNC(dbus_message_iter_open_container); \
    DO_FUNC(dbus_message_new_method_call); \
    DO_FUNC(dbus_message_new_error); \
    DO_FUNC(dbus_message_new_method_return); \
    DO_FUNC(dbus_message_new_signal); \
    DO_FUNC(dbus_message_set_no_reply); \
    DO_FUNC(dbus_message_unref); \
    DO_FUNC(dbus_threads_init_default)

#define DO_FUNC(f) static typeof(f) * p_##f
DBUS_FUNCS;
#undef DO_FUNC

static void *dbus_handle;

static BOOL load_dbus_functions(void)
{
    if (!(dbus_handle = dlopen( SONAME_LIBDBUS_1, RTLD_NOW ))) goto failed;

#define DO_FUNC(f) if (!(p_##f = dlsym( dbus_handle, #f ))) goto failed
    DBUS_FUNCS;
#undef DO_FUNC
    return TRUE;

failed:
    WARN( "failed to load %s: %s\n", SONAME_LIBDBUS_1, dlerror() );
    if (dbus_handle) { dlclose( dbus_handle ); dbus_handle = NULL; }
    return FALSE;
}

struct sni_icon
{
    struct list   entry;
    HWND          owner;
    UINT          id;
    UINT          version;
    UINT          callback_message;
    /* reactive left-click double-click detector (per icon, mirrors win32u message.c) */
    UINT64        click_tick;        /* CLOCK_MONOTONIC ms of the armed 1st click. 0 = idle. */
    int           click_x, click_y;
    BOOL          click_coords_valid;
    unsigned int  ref;               /* 1 for the icon_list entry + 1 per in-flight user */
    DBusConnection *conn;            /* this item's OWN private connection (= its own bus name) */
    BOOL          dead;              /* enqueued on dead_list for the pump to reap */
    char          path[64];          /* object path, the constant SNI_ITEM_OBJECT */
    char          id_str[80];        /* stable SNI Id (one per icon), e.g. wine-<owner>-<uID> */
    char          tip[256];          /* tooltip, utf-8 */
    BOOL          registered;        /* announced to the watcher */
    BOOL          hidden;
    /* ARGB32 (network byte order) icon for the IconPixmap property */
    int           icon_w, icon_h;
    unsigned char *icon_bits;
};

static struct list icon_list = LIST_INIT( icon_list );
static struct list dead_list = LIST_INIT( dead_list );   /* icons handed to the pump to close+free */
static pthread_mutex_t sni_mutex = PTHREAD_MUTEX_INITIALIZER;

static DBusConnection *sni_connection; /* CONTROL connection: watcher tracking + balloons, no items */
static BOOL sni_available;           /* watcher present and connection up */
static BOOL sni_init_done;
static int wake_pipe[2] = { -1, -1 }; /* self-pipe: wakes the pump when the connection set changes */

/* Wake the pump thread out of poll() so it rebuilds its connection set (a new
 * item connection appeared or an icon was queued for reaping on dead_list). */
static void sni_wake( void )
{
    char b = 1;
    if (wake_pipe[1] >= 0) { ssize_t r = write( wake_pipe[1], &b, 1 ); (void)r; }
}

static void icon_grab( struct sni_icon *icon );    /* refcount, defined below */
static void icon_release( struct sni_icon *icon );

static struct sni_icon *find_icon( HWND owner, UINT id )
{
    struct sni_icon *icon;
    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct sni_icon, entry )
        if (icon->owner == owner && icon->id == id) return icon;
    return NULL;
}

/* Native context-menu placement. ContextMenu and Activate publish the tray
 * click position here. NtUserTrackPopupMenuEx reads it back so the app's own
 * menu opens at the tray instead of the stale cursor. */
/* The context stays valid this long after a click so a slow menu open still
 * lands at the tray. Keep it just above the worst realistic menu-open latency.
 * Longer only widens the no-menu cursor-override window. */
#define SNI_MENU_CONTEXT_MS 1500
static HWND native_menu_owner;
static HWND native_menu_hwnd;
static POINT native_menu_pos;
static UINT64 native_menu_time;
static BOOL native_menu_match_owner;
static const WCHAR sni_context_owner_prop[] =
    {'_','_','w','i','n','e','_','s','n','i','_','c','o','n','t','e','x','t','_','o','w','n','e','r',0};
static const WCHAR sni_context_x_prop[] =
    {'_','_','w','i','n','e','_','s','n','i','_','c','o','n','t','e','x','t','_','x',0};
static const WCHAR sni_context_y_prop[] =
    {'_','_','w','i','n','e','_','s','n','i','_','c','o','n','t','e','x','t','_','y',0};
static const WCHAR sni_context_time_prop[] =
    {'_','_','w','i','n','e','_','s','n','i','_','c','o','n','t','e','x','t','_','t','i','m','e',0};
static const WCHAR sni_context_match_owner_prop[] =
    {'_','_','w','i','n','e','_','s','n','i','_','c','o','n','t','e','x','t','_','m','a','t','c','h','_','o','w','n','e','r',0};

static UINT64 sni_now_ms( void );

static BOOL native_menu_context_is_current(void)
{
    UINT64 elapsed;

    if (!native_menu_owner) return FALSE;
    elapsed = sni_now_ms() - native_menu_time;
    if (elapsed <= SNI_MENU_CONTEXT_MS) return TRUE;
    native_menu_owner = 0;
    native_menu_hwnd = 0;
    native_menu_time = 0;
    native_menu_match_owner = FALSE;
    return FALSE;
}

static void clear_native_menu_context(void)
{
    native_menu_owner = 0;
    native_menu_hwnd = 0;
    native_menu_time = 0;
    native_menu_match_owner = FALSE;
}

static void remove_published_native_menu_context(void)
{
    HWND desktop = get_desktop_window();

    NtUserRemoveProp( desktop, sni_context_owner_prop );
    NtUserRemoveProp( desktop, sni_context_x_prop );
    NtUserRemoveProp( desktop, sni_context_y_prop );
    NtUserRemoveProp( desktop, sni_context_time_prop );
    NtUserRemoveProp( desktop, sni_context_match_owner_prop );
}

static void publish_native_menu_context( HWND owner, int x, int y, BOOL match_owner )
{
    HWND desktop = get_desktop_window();
    DWORD tick = NtGetTickCount();

    if (!tick) tick = 1;
    NtUserSetProp( desktop, sni_context_owner_prop, owner );
    NtUserSetProp( desktop, sni_context_x_prop, ULongToHandle( (UINT)x ) );
    NtUserSetProp( desktop, sni_context_y_prop, ULongToHandle( (UINT)y ) );
    NtUserSetProp( desktop, sni_context_time_prop, ULongToHandle( tick ) );
    NtUserSetProp( desktop, sni_context_match_owner_prop, ULongToHandle( match_owner ) );
}

static BOOL read_published_native_menu_context( HWND *owner, POINT *pos, BOOL *match_owner )
{
    HWND desktop = get_desktop_window();
    DWORD tick = HandleToULong( NtUserGetProp( desktop, sni_context_time_prop ) );

    if (!tick) return FALSE;
    if (NtGetTickCount() - tick > SNI_MENU_CONTEXT_MS)
    {
        remove_published_native_menu_context();
        return FALSE;
    }
    if (owner) *owner = NtUserGetProp( desktop, sni_context_owner_prop );
    if (pos)
    {
        pos->x = (INT)HandleToULong( NtUserGetProp( desktop, sni_context_x_prop ) );
        pos->y = (INT)HandleToULong( NtUserGetProp( desktop, sni_context_y_prop ) );
    }
    if (match_owner)
        *match_owner = !!HandleToULong( NtUserGetProp( desktop, sni_context_match_owner_prop ) );
    return TRUE;
}

static BOOL window_belongs_to_context_owner( HWND hwnd, HWND owner )
{
    return owner && (hwnd == owner || NtUserGetAncestor( hwnd, GA_ROOTOWNER ) == owner);
}

static BOOL rect_contains_context_pos( const RECT *rect, POINT pos )
{
    return rect->left <= pos.x && pos.x <= rect->right &&
           rect->top <= pos.y && pos.y <= rect->bottom;
}

static BOOL rect_matches_context_anchor( const RECT *rect, POINT pos )
{
    RECT anchor_rect = *rect;

    InflateRect( &anchor_rect, get_system_metrics( SM_CXDRAG ),
                 get_system_metrics( SM_CYDRAG ) );
    return rect_contains_context_pos( &anchor_rect, pos );
}

static POINT map_context_pos_raw_to_thread_dpi( POINT pos )
{
    RECT rect = {pos.x, pos.y, pos.x, pos.y};

    rect = map_rect_raw_to_virt( rect, get_thread_dpi() );
    pos.x = rect.left;
    pos.y = rect.top;
    return pos;
}

static BOOL rect_matches_context_pos( const RECT *rect, POINT pos )
{
    POINT mapped = map_context_pos_raw_to_thread_dpi( pos );

    return rect_matches_context_anchor( rect, pos ) ||
           ((mapped.x != pos.x || mapped.y != pos.y) && rect_matches_context_anchor( rect, mapped ));
}

/* Convert an HICON to a 32bpp ARGB buffer in network byte order, as the
 * StatusNotifierItem IconPixmap (a(iiay)) wants it. Returns malloc'd bits. */
static unsigned char *icon_to_argb( HICON hicon, int *width, int *height )
{
    char info_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)info_buf;
    unsigned char *src = NULL, *dst = NULL;
    ICONINFO ii = { 0 };
    HDC hdc = NULL;
    LONG w = 0, h = 0;
    int i, total;

    if (!NtUserGetIconInfo( hicon, &ii, NULL, NULL, NULL, 0 )) return NULL;
    if (!ii.hbmColor) goto done;  /* monochrome icons not handled in phase 1 */
    {
        BITMAP bm;
        if (!NtGdiExtGetObjectW( ii.hbmColor, sizeof(bm), &bm )) goto done;
        w = bm.bmWidth;
        h = bm.bmHeight;
    }
    if (w <= 0 || h <= 0 || w > 256 || h > 256) goto done;

    if (!(hdc = NtGdiCreateCompatibleDC( 0 ))) goto done;

    total = w * h;
    if (!(src = malloc( total * 4 ))) goto done;

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = w;
    info->bmiHeader.biHeight = -h;        /* top-down */
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;

    if (!NtGdiGetDIBitsInternal( hdc, ii.hbmColor, 0, h, src, info, DIB_RGB_COLORS,
                                 total * 4, total * 4 ))
        goto done;

    if (!(dst = malloc( total * 4 ))) goto done;

    /* src is BGRA little-endian (B,G,R,A bytes). SNI wants ARGB big-endian
     * (A,R,G,B bytes). */
    for (i = 0; i < total; i++)
    {
        unsigned char b = src[i*4+0], g = src[i*4+1], r = src[i*4+2], a = src[i*4+3];
        dst[i*4+0] = a;
        dst[i*4+1] = r;
        dst[i*4+2] = g;
        dst[i*4+3] = b;
    }

    /* Classic icons carry transparency in the 1-bpp AND mask and leave the colour
     * plane's alpha all-zero. That would make the SNI pixmap fully transparent (an
     * invisible icon). Rebuild alpha from the mask (a 0 bit means opaque). With no
     * usable mask, fall back to fully opaque. */
    for (i = 0; i < total; i++) if (dst[i*4]) break;
    if (i == total)
    {
        int stride = ((w + 31) / 32) * 4;
        unsigned char *mask = malloc( stride * h );

        memset( info, 0, sizeof(*info) );
        info->bmiHeader.biSize = sizeof(info->bmiHeader);
        info->bmiHeader.biWidth = w;
        info->bmiHeader.biHeight = -h;
        info->bmiHeader.biPlanes = 1;
        info->bmiHeader.biBitCount = 1;
        info->bmiHeader.biCompression = BI_RGB;

        if (mask && ii.hbmMask &&
            NtGdiGetDIBitsInternal( hdc, ii.hbmMask, 0, h, mask, info, DIB_RGB_COLORS,
                                    stride * h, stride * h ))
        {
            int x, y;
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++)
                {
                    int bit = (mask[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
                    dst[(y * w + x) * 4] = bit ? 0x00 : 0xff;
                }
        }
        else for (i = 0; i < total; i++) dst[i*4] = 0xff;
        free( mask );
    }

    *width = w;
    *height = h;

done:
    free( src );
    if (hdc) NtGdiDeleteObjectApp( hdc );
    if (ii.hbmColor) NtGdiDeleteObjectApp( ii.hbmColor );
    if (ii.hbmMask) NtGdiDeleteObjectApp( ii.hbmMask );
    return dst;
}

/* append a single (iiay) icon-pixmap struct into an already-open a(iiay) array */
static void append_pixmap( DBusMessageIter *array, const struct sni_icon *icon )
{
    DBusMessageIter st, bytes;
    int w = icon->icon_w, h = icon->icon_h, n = w * h * 4;

    if (!p_dbus_message_iter_open_container( array, DBUS_TYPE_STRUCT, NULL, &st )) return;
    p_dbus_message_iter_append_basic( &st, DBUS_TYPE_INT32, &w );
    p_dbus_message_iter_append_basic( &st, DBUS_TYPE_INT32, &h );
    if (p_dbus_message_iter_open_container( &st, DBUS_TYPE_ARRAY, "y", &bytes ))
    {
        const unsigned char *p = icon->icon_bits;
        p_dbus_message_iter_append_fixed_array( &bytes, DBUS_TYPE_BYTE, &p, n );
        p_dbus_message_iter_close_container( &st, &bytes );
    }
    p_dbus_message_iter_close_container( array, &st );
}

/* append the a(iiay) IconPixmap value (variant content) */
static void append_icon_pixmap( DBusMessageIter *iter, const struct sni_icon *icon )
{
    DBusMessageIter array;
    if (!p_dbus_message_iter_open_container( iter, DBUS_TYPE_ARRAY, "(iiay)", &array )) return;
    if (icon->icon_bits && icon->icon_w && icon->icon_h) append_pixmap( &array, icon );
    p_dbus_message_iter_close_container( iter, &array );
}

static void append_variant_string( DBusMessageIter *iter, const char *s )
{
    DBusMessageIter var;
    if (!p_dbus_message_iter_open_container( iter, DBUS_TYPE_VARIANT, "s", &var )) return;
    p_dbus_message_iter_append_basic( &var, DBUS_TYPE_STRING, &s );
    p_dbus_message_iter_close_container( iter, &var );
}

static void append_variant_bool( DBusMessageIter *iter, BOOL b )
{
    DBusMessageIter var;
    dbus_bool_t v = !!b;
    if (!p_dbus_message_iter_open_container( iter, DBUS_TYPE_VARIANT, "b", &var )) return;
    p_dbus_message_iter_append_basic( &var, DBUS_TYPE_BOOLEAN, &v );
    p_dbus_message_iter_close_container( iter, &var );
}

/* append the ToolTip property value: (s a(iiay) s s) = icon-name, icon, title, text */
static void append_variant_tooltip( DBusMessageIter *iter, const struct sni_icon *icon )
{
    DBusMessageIter var, st;
    const char *empty = "";
    const char *title = icon->tip;
    if (!p_dbus_message_iter_open_container( iter, DBUS_TYPE_VARIANT, "(sa(iiay)ss)", &var )) return;
    if (p_dbus_message_iter_open_container( &var, DBUS_TYPE_STRUCT, NULL, &st ))
    {
        DBusMessageIter array;
        p_dbus_message_iter_append_basic( &st, DBUS_TYPE_STRING, &empty );  /* icon name */
        if (p_dbus_message_iter_open_container( &st, DBUS_TYPE_ARRAY, "(iiay)", &array ))
            p_dbus_message_iter_close_container( &st, &array );             /* empty icon */
        p_dbus_message_iter_append_basic( &st, DBUS_TYPE_STRING, &title );  /* title */
        p_dbus_message_iter_append_basic( &st, DBUS_TYPE_STRING, &empty );  /* text */
        p_dbus_message_iter_close_container( &var, &st );
    }
    p_dbus_message_iter_close_container( iter, &var );
}

/* write the value of a single named property into iter (already a variant slot) */
static void append_property( DBusMessageIter *iter, const struct sni_icon *icon, const char *name )
{
    const char *category = "ApplicationStatus";
    const char *status = icon->hidden ? "Passive" : "Active";
    const char *empty = "";
    int window_id = 0;

    if (!strcmp( name, "Category" ))      append_variant_string( iter, category );
    else if (!strcmp( name, "Id" ))       append_variant_string( iter, icon->id_str );
    else if (!strcmp( name, "Title" ))    append_variant_string( iter, icon->tip[0] ? icon->tip : "Wine" );
    else if (!strcmp( name, "Status" ))   append_variant_string( iter, status );
    else if (!strcmp( name, "IconName" )) append_variant_string( iter, empty );
    else if (!strcmp( name, "ItemIsMenu" )) append_variant_bool( iter, FALSE );
    else if (!strcmp( name, "WindowId" ))
    {
        DBusMessageIter var;
        if (p_dbus_message_iter_open_container( iter, DBUS_TYPE_VARIANT, "i", &var ))
        {
            p_dbus_message_iter_append_basic( &var, DBUS_TYPE_INT32, &window_id );
            p_dbus_message_iter_close_container( iter, &var );
        }
    }
    else if (!strcmp( name, "IconPixmap" ))
    {
        DBusMessageIter var;
        if (p_dbus_message_iter_open_container( iter, DBUS_TYPE_VARIANT, "a(iiay)", &var ))
        {
            append_icon_pixmap( &var, icon );
            p_dbus_message_iter_close_container( iter, &var );
        }
    }
    else if (!strcmp( name, "ToolTip" )) append_variant_tooltip( iter, icon );
    else append_variant_string( iter, empty );
}

static const char *sni_introspect_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    " <interface name=\"org.kde.StatusNotifierItem\">\n"
    "  <property name=\"Category\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Id\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Title\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"IconName\" type=\"s\" access=\"read\"/>\n"
    "  <property name=\"IconPixmap\" type=\"a(iiay)\" access=\"read\"/>\n"
    "  <property name=\"ToolTip\" type=\"(sa(iiay)ss)\" access=\"read\"/>\n"
    "  <property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>\n"
    "  <method name=\"Activate\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "  <method name=\"SecondaryActivate\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "  <method name=\"ContextMenu\"><arg name=\"x\" type=\"i\" direction=\"in\"/><arg name=\"y\" type=\"i\" direction=\"in\"/></method>\n"
    "  <method name=\"Scroll\"><arg name=\"delta\" type=\"i\" direction=\"in\"/><arg name=\"orientation\" type=\"s\" direction=\"in\"/></method>\n"
    "  <signal name=\"NewIcon\"/>\n"
    "  <signal name=\"NewToolTip\"/>\n"
    "  <signal name=\"NewStatus\"><arg name=\"status\" type=\"s\"/></signal>\n"
    " </interface>\n"
    "</node>\n";

static void notify_owner( struct sni_icon *icon, UINT msg, int x, int y )
{
    WPARAM wp = icon->id;
    LPARAM lp = msg;

    if (icon->version >= NOTIFYICON_VERSION_4)
    {
        wp = MAKEWPARAM( x, y );
        lp = MAKELPARAM( msg, icon->id );
    }
    NtUserMessageCall( icon->owner, icon->callback_message, wp, lp, 0,
                       NtUserSendNotifyMessage, FALSE );
}

static UINT64 sni_now_ms( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (UINT64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

BOOL sni_get_context_menu_pos( POINT *pos )
{
    BOOL ret, latched;

    pthread_mutex_lock( &sni_mutex );
    /* Override the cursor only while the menu is being placed. A latched (on
     * screen) menu must see the real pointer. */
    latched = !!native_menu_hwnd;
    ret = !latched && native_menu_context_is_current();
    if (ret) *pos = native_menu_pos;
    pthread_mutex_unlock( &sni_mutex );
    if (!ret && !latched) ret = read_published_native_menu_context( NULL, pos, NULL );
    return ret;
}

BOOL sni_should_layer_context_menu( HWND hwnd, DWORD style, DWORD ex_style, const RECT *rect, BOOL fullscreen )
{
    BOOL candidate, context_current = FALSE, match_owner = FALSE;
    BOOL owner_match, position_match, remove_context = FALSE, ret = FALSE;
    DWORD owner_pid, window_pid;
    LONG width = rect->right - rect->left;
    LONG height = rect->bottom - rect->top;
    HWND owner = NULL;
    POINT pos;

    /* Borderless fullscreen games also use WS_POPUP and cover the tray click. */
    candidate = !fullscreen && (style & WS_POPUP) && !(style & WS_CHILD) && !(style & WS_THICKFRAME) &&
                !(ex_style & WS_EX_APPWINDOW) && width > 1 && height > 1 &&
                width <= get_system_metrics( SM_CXVIRTUALSCREEN ) &&
                height <= get_system_metrics( SM_CYVIRTUALSCREEN );

    pthread_mutex_lock( &sni_mutex );
    if (native_menu_hwnd == hwnd)
    {
        if (candidate && ((style & WS_VISIBLE) || native_menu_context_is_current())) ret = TRUE;
        else clear_native_menu_context();
    }
    else if (candidate && native_menu_context_is_current())
    {
        context_current = TRUE;
        owner = native_menu_owner;
        pos = native_menu_pos;
        match_owner = native_menu_match_owner;
    }
    pthread_mutex_unlock( &sni_mutex );
    if (ret || !candidate) return ret;

    if (!context_current && !read_published_native_menu_context( &owner, &pos, &match_owner )) return FALSE;
    if (owner && hwnd == owner) return FALSE;

    /* The published context is shared by every process on the Wine desktop.
     * Position alone must not let an unrelated game claim a launcher's click.
     * Retain explicitly owned menus, including cross-process owner chains. */
    owner_match = window_belongs_to_context_owner( hwnd, owner );
    if (!owner_match && (!get_window_thread( owner, &owner_pid ) ||
                        !get_window_thread( hwnd, &window_pid ) || owner_pid != window_pid))
        return FALSE;
    owner_match = match_owner && owner_match;
    position_match = (style & WS_VISIBLE) && rect_matches_context_pos( rect, pos );
    if (!owner_match && !position_match) return FALSE;

    pthread_mutex_lock( &sni_mutex );
    if (native_menu_hwnd && native_menu_hwnd != hwnd && !native_menu_context_is_current())
        clear_native_menu_context();
    if (!native_menu_hwnd)
    {
        native_menu_owner = owner;
        native_menu_hwnd = hwnd;
        native_menu_pos = pos;
        native_menu_time = sni_now_ms();
        native_menu_match_owner = match_owner;
        ret = TRUE;
        remove_context = TRUE;
    }
    else if (native_menu_hwnd == hwnd) ret = TRUE;
    if (ret)
        TRACE( "using SNI context menu window %p owner %p rect %s visible %u\n",
               hwnd, native_menu_owner, wine_dbgstr_rect( rect ), !!(style & WS_VISIBLE) );
    pthread_mutex_unlock( &sni_mutex );
    if (remove_context) remove_published_native_menu_context();

    return ret;
}

/* Win32 tray click streams. The notification area forwards the raw CS_DBLCLKS
 * stream to the owner: a single click is WM_LBUTTONDOWN then WM_LBUTTONUP (v4
 * adds NIN_SELECT). A double-click promotes the 2nd press to WM_LBUTTONDBLCLK.
 * Across both clicks the owner sees DOWN, UP, DBLCLK, UP. */
static void emit_tray_single( struct sni_icon *icon, int x, int y )
{
    notify_owner( icon, WM_LBUTTONDOWN, x, y );
    notify_owner( icon, WM_LBUTTONUP, x, y );
    if (icon->version >= NOTIFYICON_VERSION_4) notify_owner( icon, NIN_SELECT, x, y );
}

/* the 2nd click of a double: its down is promoted to DBLCLK, then its up */
static void emit_tray_double( struct sni_icon *icon, int x, int y )
{
    notify_owner( icon, WM_LBUTTONDBLCLK, x, y );
    notify_owner( icon, WM_LBUTTONUP, x, y );
}

/* incoming method-call dispatch for one of our /StatusNotifierItem objects */
static DBusHandlerResult sni_object_handler( DBusConnection *conn, DBusMessage *msg, void *data )
{
    const char *iface = p_dbus_message_get_interface( msg );
    const char *member = p_dbus_message_get_member( msg );
    const char *path = p_dbus_message_get_path( msg );
    DBusMessage *reply = NULL;
    struct sni_icon *icon = data;   /* one item per connection: the icon is the object user_data */

    if (!iface || !member || !path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    TRACE( "incoming %s.%s on %s\n", iface, member, path );

    pthread_mutex_lock( &sni_mutex );

    if (!strcmp( iface, DBUS_INTROSPECT_IFACE ) && !strcmp( member, "Introspect" ))
    {
        if ((reply = p_dbus_message_new_method_return( msg )))
            p_dbus_message_append_args( reply, DBUS_TYPE_STRING, &sni_introspect_xml, DBUS_TYPE_INVALID );
    }
    else if (!strcmp( iface, DBUS_PROPS_IFACE ) && !strcmp( member, "Get" ) && icon)
    {
        DBusMessageIter args, ret;
        const char *piface, *pname;

        if (p_dbus_message_iter_init( msg, &args ) &&
            p_dbus_message_iter_get_arg_type( &args ) == DBUS_TYPE_STRING)
        {
            p_dbus_message_iter_get_basic( &args, &piface );
            p_dbus_message_iter_next( &args );
            if (p_dbus_message_iter_get_arg_type( &args ) == DBUS_TYPE_STRING &&
                !strcmp( piface, SNI_ITEM_IFACE ))
            {
                p_dbus_message_iter_get_basic( &args, &pname );
                if (!strcmp( pname, "Menu" ))
                    reply = p_dbus_message_new_error( msg, DBUS_ERROR_UNKNOWN_PROPERTY,
                                                     "Tray menus are provided by ContextMenu" );
                else if ((reply = p_dbus_message_new_method_return( msg )))
                {
                    p_dbus_message_iter_init_append( reply, &ret );
                    append_property( &ret, icon, pname );
                }
            }
        }
    }
    else if (!strcmp( iface, DBUS_PROPS_IFACE ) && !strcmp( member, "GetAll" ) && icon)
    {
        /* No D-Bus menu is exported. Omit Menu so hosts call ContextMenu;
         * /NO_DBUSMENU is a KDE convention which other hosts may try to import. */
        static const char *props[] = { "Category", "Id", "Title", "Status", "IconName",
                                       "IconPixmap", "ToolTip", "ItemIsMenu", "WindowId" };
        DBusMessageIter ret, dict;
        unsigned int i;
        if ((reply = p_dbus_message_new_method_return( msg )))
        {
            p_dbus_message_iter_init_append( reply, &ret );
            if (p_dbus_message_iter_open_container( &ret, DBUS_TYPE_ARRAY, "{sv}", &dict ))
            {
                for (i = 0; i < ARRAY_SIZE(props); i++)
                {
                    DBusMessageIter e;
                    if (!p_dbus_message_iter_open_container( &dict, DBUS_TYPE_DICT_ENTRY, NULL, &e ))
                        continue;
                    p_dbus_message_iter_append_basic( &e, DBUS_TYPE_STRING, &props[i] );
                    append_property( &e, icon, props[i] );
                    p_dbus_message_iter_close_container( &dict, &e );
                }
                p_dbus_message_iter_close_container( &ret, &dict );
            }
        }
    }
    else if (!strcmp( iface, SNI_ITEM_IFACE ) && icon)
    {
        POINT point;
        int x = 0, y = 0;

        p_dbus_message_get_args( msg, NULL, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID );
        point.x = x;
        point.y = y;
        user_driver->pWindowMessage( 0, WM_WINE_MAP_NOTIFY_ICON_POINT, 0, (LPARAM)&point );
        x = point.x;
        y = point.y;
        if (!strcmp( member, "Activate" ))
        {
            /* SNI sends one Activate per left-click with no double-click notion.
             * We detect doubles reactively: emit the first click in full at once
             * (Windows never defers it either), then if a 2nd Activate lands within
             * the double-click time and rect, promote it to WM_LBUTTONDBLCLK (the
             * test win32u message.c uses on the 2nd button-down). No timer, no added
             * latency. (0,0) means the host omitted coords. Match on time alone. */
            UINT64 now = sni_now_ms();
            BOOL coords = (x || y);
            BOOL armed = (icon->click_tick != 0);
            BOOL in_time = armed && (now - icon->click_tick) < NtUserGetDoubleClickTime();
            BOOL in_rect = (!coords || !icon->click_coords_valid) ? TRUE
                         : (abs( x - icon->click_x ) <= get_system_metrics( SM_CXDOUBLECLK ) / 2 &&
                            abs( y - icon->click_y ) <= get_system_metrics( SM_CYDOUBLECLK ) / 2);
            BOOL dbl = in_time && in_rect;

            if (dbl)
                icon->click_tick = 0;   /* disarm. a 3rd click begins a fresh single. */
            else
            {
                icon->click_tick = now ? now : 1;   /* never store the idle sentinel. */
                icon->click_x = x;
                icon->click_y = y;
                icon->click_coords_valid = coords;
            }
            /* Some hosts open their tray menu on a left click and place it at
             * GetCursorPos. Publish the click position without owner matching. */
            if (x || y)
            {
                native_menu_owner = icon->owner;
                native_menu_hwnd = 0;
                native_menu_pos.x = x;
                native_menu_pos.y = y;
                native_menu_time = sni_now_ms();
                native_menu_match_owner = FALSE;
            }
            icon_grab( icon );
            pthread_mutex_unlock( &sni_mutex );
            if (x || y) publish_native_menu_context( icon->owner, x, y, FALSE );
            if (dbl) emit_tray_double( icon, x, y );
            else emit_tray_single( icon, x, y );
            icon_release( icon );
            pthread_mutex_lock( &sni_mutex );
        }
        else if (!strcmp( member, "ContextMenu" ))
        {
            TRACE( "context menu owner %p id %u version %u at %d,%d\n",
                   icon->owner, icon->id, icon->version, x, y );
            if (x || y)
            {
                native_menu_owner = icon->owner;
                native_menu_hwnd = 0;
                native_menu_pos.x = x;
                native_menu_pos.y = y;
                native_menu_time = sni_now_ms();
                native_menu_match_owner = TRUE;
            }
            icon_grab( icon );
            pthread_mutex_unlock( &sni_mutex );
            if (x || y) publish_native_menu_context( icon->owner, x, y, TRUE );
            notify_owner( icon, WM_RBUTTONDOWN, x, y );
            notify_owner( icon, WM_RBUTTONUP, x, y );
            if (icon->version) notify_owner( icon, WM_CONTEXTMENU, x, y );
            icon_release( icon );
            pthread_mutex_lock( &sni_mutex );
        }
        else if (!strcmp( member, "SecondaryActivate" ))
            notify_owner( icon, WM_MBUTTONUP, x, y );
        /* Scroll: no Win32 tray equivalent. Ignored. */
        reply = p_dbus_message_new_method_return( msg );
    }
    pthread_mutex_unlock( &sni_mutex );

    if (reply)
    {
        p_dbus_connection_send( conn, reply, NULL );
        p_dbus_message_unref( reply );
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable sni_vtable = { NULL, sni_object_handler, NULL, NULL, NULL, NULL };

static BOOL sni_watcher_present( void )
{
    DBusError error;
    BOOL has;
    if (!sni_connection) return FALSE;
    p_dbus_error_init( &error );
    has = p_dbus_bus_name_has_owner( sni_connection, SNI_WATCHER_SERVICE, &error );
    p_dbus_error_free( &error );
    return has;
}

/* (re-)announce one item to the watcher on ITS OWN connection, passing that
 * connection's unique bus name. Closing the connection on delete kills the name,
 * which is how the watcher learns the item is gone. Fire-and-forget. */
static void sni_announce_item( struct sni_icon *icon )
{
    DBusMessage *msg;
    const char *name;

    if (!icon->conn) return;
    name = p_dbus_bus_get_unique_name( icon->conn );
    if (name && (msg = p_dbus_message_new_method_call( SNI_WATCHER_SERVICE, SNI_WATCHER_PATH,
                                                       SNI_WATCHER_IFACE, "RegisterStatusNotifierItem" )))
    {
        p_dbus_message_append_args( msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID );
        p_dbus_connection_send( icon->conn, msg, NULL );
        p_dbus_message_unref( msg );
        p_dbus_connection_flush( icon->conn );
    }
}

/* Re-announce every item (each on its own connection/name), e.g. after the
 * watcher (re)appears. Each icon is snapshot under the mutex with a grabbed ref
 * so it stays alive across the off-mutex send. */
static void sni_reannounce_all( void )
{
    struct sni_icon *snap[64];
    unsigned int n = 0, i;
    struct sni_icon *icon;

    pthread_mutex_lock( &sni_mutex );
    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct sni_icon, entry )
    {
        if (!icon->registered || icon->dead || !icon->conn || n >= ARRAY_SIZE(snap)) continue;
        icon_grab( icon );
        snap[n++] = icon;
    }
    pthread_mutex_unlock( &sni_mutex );

    for (i = 0; i < n; i++) { sni_announce_item( snap[i] ); icon_release( snap[i] ); }
}

/* Bus signal filter (pump thread): track the StatusNotifierWatcher coming and
 * going so a panel that starts after us or restarts gets our items again. */
static DBusHandlerResult sni_filter( DBusConnection *conn, DBusMessage *msg, void *data )
{
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;

    if (!p_dbus_message_is_signal( msg, "org.freedesktop.DBus", "NameOwnerChanged" ))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (!p_dbus_message_get_args( msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID ))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (!name || strcmp( name, SNI_WATCHER_SERVICE )) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    (void)old_owner;

    if (new_owner && *new_owner)
    {
        /* watcher appeared or restarted: promote and (re)announce every item */
        TRACE( "StatusNotifierWatcher up (%s); re-announcing items\n", new_owner );
        pthread_mutex_lock( &sni_mutex );
        sni_available = TRUE;
        pthread_mutex_unlock( &sni_mutex );
        sni_reannounce_all();
    }
    else TRACE( "StatusNotifierWatcher gone; items stay exported for its return\n" );

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;  /* observers only */
}

/* Establish the session-bus connection and decide if SNI is usable. The watcher
 * may be absent now and appear later (autostart racing the panel, a restart).
 * Subscribe to NameOwnerChanged and never latch availability to one probe. */
static void sni_init( void )
{
    DBusError error;

    sni_init_done = TRUE;
    if (!load_dbus_functions()) return;
    p_dbus_threads_init_default();

    p_dbus_error_init( &error );
    if (!(sni_connection = p_dbus_bus_get_private( DBUS_BUS_SESSION, &error )))
    {
        WARN( "no session bus: %s\n", p_dbus_error_is_set( &error ) ? error.message : "?" );
        p_dbus_error_free( &error );
        return;
    }
    p_dbus_error_free( &error );

    /* a transient bus drop must never _exit() the app (we have N churning item
     * connections). The pump multiplexes via poll(), woken by wake_pipe. */
    p_dbus_connection_set_exit_on_disconnect( sni_connection, FALSE );
    if (wake_pipe[0] < 0)
    {
        if (pipe( wake_pipe ) < 0) wake_pipe[0] = wake_pipe[1] = -1;
        else
        {
            fcntl( wake_pipe[0], F_SETFL, O_NONBLOCK ); fcntl( wake_pipe[0], F_SETFD, FD_CLOEXEC );
            fcntl( wake_pipe[1], F_SETFL, O_NONBLOCK ); fcntl( wake_pipe[1], F_SETFD, FD_CLOEXEC );
        }
    }

    p_dbus_connection_add_filter( sni_connection, sni_filter, NULL, NULL );
    p_dbus_bus_add_match( sni_connection,
                          "type='signal',sender='org.freedesktop.DBus',"
                          "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                          "arg0='" SNI_WATCHER_SERVICE "'", NULL );

    sni_available = sni_watcher_present();
    TRACE( "SNI %s (StatusNotifierWatcher %spresent)\n",
           sni_available ? "available" : "pending", sni_available ? "" : "not " );
}

/* Grab the item's connection for an off-mutex send. Under the mutex, re-check it
 * is live and take a libdbus ref so a concurrent pump reap (which closes+unref's
 * it) cannot free the connection under the send. */
static DBusConnection *icon_conn_ref( struct sni_icon *icon )
{
    DBusConnection *conn;
    pthread_mutex_lock( &sni_mutex );
    conn = (icon->dead || !icon->conn) ? NULL : icon->conn;
    if (conn) p_dbus_connection_ref( conn );
    pthread_mutex_unlock( &sni_mutex );
    return conn;
}

static void sni_emit_signal( struct sni_icon *icon, const char *name )
{
    DBusConnection *conn = icon_conn_ref( icon );
    DBusMessage *sig;

    if (!conn) return;
    if ((sig = p_dbus_message_new_signal( icon->path, SNI_ITEM_IFACE, name )))
    {
        p_dbus_connection_send( conn, sig, NULL );
        p_dbus_message_unref( sig );
        p_dbus_connection_flush( conn );
    }
    p_dbus_connection_unref( conn );
}

/* NewStatus carries the new Active/Passive string. Hosts cache Status and only
 * re-read it when this fires. An app toggling NIS_HIDDEN needs the signal. */
static void sni_emit_status( struct sni_icon *icon )
{
    DBusConnection *conn = icon_conn_ref( icon );
    const char *status = icon->hidden ? "Passive" : "Active";
    DBusMessage *sig;

    if (!conn) return;
    if ((sig = p_dbus_message_new_signal( icon->path, SNI_ITEM_IFACE, "NewStatus" )))
    {
        p_dbus_message_append_args( sig, DBUS_TYPE_STRING, &status, DBUS_TYPE_INVALID );
        p_dbus_connection_send( conn, sig, NULL );
        p_dbus_message_unref( sig );
        p_dbus_connection_flush( conn );
    }
    p_dbus_connection_unref( conn );
}

/* A NIF_INFO balloon has no SNI item equivalent. Surface it as a desktop
 * notification (what an SNI host shows for tray toasts) instead of dropping it. */
static BOOL sni_show_balloon( DBusConnection *conn, const NOTIFYICONDATAW *nid )
{
    DBusMessage *msg;
    DBusMessageIter args, sub;
    DBusError error;
    char title[sizeof(nid->szInfoTitle) / sizeof(WCHAR) * 3 + 1];   /* worst-case UTF-8 size */
    char body[sizeof(nid->szInfo) / sizeof(WCHAR) * 3 + 1];
    const char *app = "Wine", *icon_name = "", *p_title = title, *p_body = body;
    dbus_uint32_t replaces = 0;
    dbus_int32_t timeout = -1;
    ULONG len = 0;
    BOOL ret = FALSE;
    int n;

    p_dbus_error_init( &error );
    if (!p_dbus_bus_name_has_owner( conn, NOTIFY_SERVICE, &error ))
    {
        if (p_dbus_error_is_set( &error ))
            WARN( "failed to query desktop notification service: %s\n", error.message );
        p_dbus_error_free( &error );
        return FALSE;
    }
    p_dbus_error_free( &error );

    for (n = 0; n < ARRAY_SIZE(nid->szInfoTitle) && nid->szInfoTitle[n]; n++) {}
    if (RtlUnicodeToUTF8N( title, sizeof(title) - 1, &len, nid->szInfoTitle, n * sizeof(WCHAR) )) len = 0;
    title[len] = 0;
    for (n = 0; n < ARRAY_SIZE(nid->szInfo) && nid->szInfo[n]; n++) {}
    if (RtlUnicodeToUTF8N( body, sizeof(body) - 1, &len, nid->szInfo, n * sizeof(WCHAR) )) len = 0;
    body[len] = 0;

    if (!(msg = p_dbus_message_new_method_call( NOTIFY_SERVICE, NOTIFY_PATH, NOTIFY_IFACE, "Notify" )))
        return FALSE;
    p_dbus_message_set_no_reply( msg, TRUE );
    p_dbus_message_iter_init_append( msg, &args );
    if (!p_dbus_message_iter_append_basic( &args, DBUS_TYPE_STRING, &app ) ||
        !p_dbus_message_iter_append_basic( &args, DBUS_TYPE_UINT32, &replaces ) ||
        !p_dbus_message_iter_append_basic( &args, DBUS_TYPE_STRING, &icon_name ) ||
        !p_dbus_message_iter_append_basic( &args, DBUS_TYPE_STRING, &p_title ) ||
        !p_dbus_message_iter_append_basic( &args, DBUS_TYPE_STRING, &p_body ) ||
        !p_dbus_message_iter_open_container( &args, DBUS_TYPE_ARRAY, "s", &sub ) ||
        !p_dbus_message_iter_close_container( &args, &sub ) ||
        !p_dbus_message_iter_open_container( &args, DBUS_TYPE_ARRAY, "{sv}", &sub ) ||
        !p_dbus_message_iter_close_container( &args, &sub ) ||
        !p_dbus_message_iter_append_basic( &args, DBUS_TYPE_INT32, &timeout ))
        goto done;

    if (p_dbus_connection_send( conn, msg, NULL ))
    {
        p_dbus_connection_flush( conn );
        ret = TRUE;
    }

done:
    p_dbus_message_unref( msg );
    return ret;
}

BOOL sni_notify_balloon( const NOTIFYICONDATAW *nid )
{
    DBusConnection *conn = NULL;
    BOOL ret;

    pthread_mutex_lock( &sni_mutex );
    if (!sni_init_done) sni_init();
    if (sni_connection && p_dbus_connection_get_is_connected( sni_connection ))
        conn = p_dbus_connection_ref( sni_connection );
    pthread_mutex_unlock( &sni_mutex );

    if (!conn) return FALSE;
    ret = sni_show_balloon( conn, nid );
    p_dbus_connection_unref( conn );
    return ret;
}

/* Give the icon its OWN private connection (= its own unique bus name), export
 * its two objects on it (icon* as user_data), publish it to the pump and
 * announce. Runs on the app thread, mutex released. dbus_bus_get_private blocks
 * on bus auth. It must never move under sni_mutex or onto the pump. */
static BOOL sni_register_item( struct sni_icon *icon )
{
    DBusConnection *conn;
    DBusError error;

    p_dbus_error_init( &error );
    if (!(conn = p_dbus_bus_get_private( DBUS_BUS_SESSION, &error )))
    {
        WARN( "no session bus for item %s: %s\n", icon->id_str,
              p_dbus_error_is_set( &error ) ? error.message : "?" );
        p_dbus_error_free( &error );
        goto fail;
    }
    p_dbus_error_free( &error );
    p_dbus_connection_set_exit_on_disconnect( conn, FALSE );

    if (!p_dbus_connection_try_register_object_path( conn, icon->path, &sni_vtable, icon, NULL ))
    {
        WARN( "failed to export item objects for %s\n", icon->id_str );
        p_dbus_connection_close( conn );   /* never published to the pump -> safe to close here */
        p_dbus_connection_unref( conn );
        goto fail;
    }

    /* publish under the mutex, then announce on the new connection and wake the
     * pump so it starts polling this connection's fd. */
    pthread_mutex_lock( &sni_mutex );
    icon->conn = conn;
    icon->registered = TRUE;
    pthread_mutex_unlock( &sni_mutex );

    sni_announce_item( icon );
    sni_wake();
    TRACE( "registered SNI item %s on %s (icon %dx%d, tip \"%s\")\n", icon->id_str,
           p_dbus_bus_get_unique_name( conn ), icon->icon_w, icon->icon_h, icon->tip );
    return TRUE;

fail:
    /* registration failed: hand the icon to the pump so it leaves icon_list (a
     * re-add of this (hWnd,uID) can then succeed) and is freed. conn is already
     * NULL/closed. The reap just frees the struct. */
    pthread_mutex_lock( &sni_mutex );
    if (!icon->dead)
    {
        icon->dead = TRUE;
        list_remove( &icon->entry );
        list_add_tail( &dead_list, &icon->entry );
    }
    pthread_mutex_unlock( &sni_mutex );
    sni_wake();
    return FALSE;
}

/* Icon refcount. The icon_list entry holds one ref. Code using an icon after
 * dropping sni_mutex grabs an extra ref under the mutex and drops it when done.
 * The last release frees the D-Bus objects. So a concurrent NIM_DELETE can
 * unlink the icon without freeing it under an in-flight send on another thread. */
static void icon_grab( struct sni_icon *icon )   /* call with sni_mutex held */
{
    icon->ref++;
}

/* call WITHOUT sni_mutex held. D-Bus teardown runs mutex-released */
static void icon_release( struct sni_icon *icon )
{
    BOOL last;

    pthread_mutex_lock( &sni_mutex );
    last = (--icon->ref == 0);
    pthread_mutex_unlock( &sni_mutex );
    if (!last) return;

    /* last ref. The pump reap already flushed+closed+unref'd the connection (the
     * close unregisters its object paths). No D-Bus teardown is left here. */
    free( icon->icon_bits );
    free( icon );
}

/* apply a NOTIFYICONDATA update onto an icon record. Flags out which signals to emit */
static void update_icon( struct sni_icon *icon, NOTIFYICONDATAW *nid, BOOL *image_changed,
                         BOOL *tip_changed, BOOL *status_changed )
{
    if (nid->uFlags & NIF_MESSAGE) icon->callback_message = nid->uCallbackMessage;

    if (nid->uFlags & NIF_ICON)
    {
        unsigned char *bits;
        int w = 0, h = 0;
        if ((bits = icon_to_argb( nid->hIcon, &w, &h )))
        {
            free( icon->icon_bits );
            icon->icon_bits = bits;
            icon->icon_w = w;
            icon->icon_h = h;
            *image_changed = TRUE;
        }
    }

    if (nid->uFlags & NIF_TIP)
    {
        char buf[256];
        ULONG reslen = 0;
        int len = 0;
        while (len < ARRAY_SIZE(nid->szTip) && nid->szTip[len]) len++;
        if (RtlUnicodeToUTF8N( buf, sizeof(buf) - 1, &reslen, nid->szTip, len * sizeof(WCHAR) ))
            reslen = 0;
        buf[reslen] = 0;
        if (strcmp( buf, icon->tip ))
        {
            memcpy( icon->tip, buf, reslen + 1 );
            *tip_changed = TRUE;
        }
    }

    /* Only touch hidden state when the caller masks NIS_HIDDEN. A NIF_STATE
     * update for other bits must not clobber a hidden icon back to visible. */
    if ((nid->uFlags & NIF_STATE) && (nid->dwStateMask & NIS_HIDDEN))
    {
        BOOL hidden = !!(nid->dwState & NIS_HIDDEN);
        if (hidden != icon->hidden) { icon->hidden = hidden; *status_changed = TRUE; }
    }
}

/***********************************************************************
 *   sni_notify_icon
 *
 * Handle a Shell_NotifyIcon request via SNI. Returns -1 if SNI is not
 * available (caller should fall back to the driver / explorer tray).
 */
LRESULT sni_notify_icon( HWND owner, UINT msg, NOTIFYICONDATAW *nid )
{
    enum { ACT_NONE, ACT_REGISTER } action = ACT_NONE;
    BOOL sig_icon = FALSE, sig_tip = FALSE, sig_status = FALSE, queued_dead = FALSE;
    struct sni_icon *icon, *target = NULL;
    LRESULT ret = -1;

    /* GNOME's AppIndicator host requires a D-Bus menu and already translates
     * double-clicks into Activate. Use explorer's XEmbed path on Mutter/X11;
     * our native-menu SNI backend cannot preserve those interactions there. */
    if (user_driver->pHasWindowManager( "Mutter" ))
    {
        if (msg == NIM_ADD) TRACE( "using XEmbed tray instead of SNI on Mutter\n" );
        return -1;
    }

    pthread_mutex_lock( &sni_mutex );
    if (!sni_init_done) sni_init();
    /* the watcher may have started after us. Re-probe on a fresh NIM_ADD rather
     * than staying on the fallback tray for the whole process lifetime. */
    if (!sni_available && sni_connection && msg == NIM_ADD)
        sni_available = sni_watcher_present();
    if (!sni_available) { pthread_mutex_unlock( &sni_mutex ); return -1; }

    /* Touch the icon list and icon data only under the mutex. Defer all D-Bus I/O
     * past the unlock: holding sni_mutex across a send would invert lock order
     * against the pump handler (connection lock then sni_mutex) and deadlock. */
    switch (msg)
    {
    case NIM_ADD:
        if (find_icon( nid->hWnd, nid->uID )) { ret = FALSE; break; }
        if (!(icon = calloc( 1, sizeof(*icon) ))) break;
        icon->owner = nid->hWnd;
        icon->id = nid->uID;
        icon->ref = 1;   /* the icon_list reference */
        snprintf( icon->path, sizeof(icon->path), "%s", SNI_ITEM_OBJECT );
        snprintf( icon->id_str, sizeof(icon->id_str), "wine-%p-%u", icon->owner, icon->id );
        list_add_tail( &icon_list, &icon->entry );
        update_icon( icon, nid, &sig_icon, &sig_tip, &sig_status );
        icon_grab( icon );   /* keep alive across the post-unlock register */
        target = icon;
        action = ACT_REGISTER;
        ret = TRUE;
        break;
    case NIM_MODIFY:
        if (!(icon = find_icon( nid->hWnd, nid->uID ))) { ret = -1; break; }  /* unknown here -> let the explorer tray try */
        update_icon( icon, nid, &sig_icon, &sig_tip, &sig_status );
        icon_grab( icon );   /* keep alive across the post-unlock signals */
        target = icon;
        ret = TRUE;
        break;
    case NIM_DELETE:
        if (!(icon = find_icon( nid->hWnd, nid->uID ))) { ret = -1; break; }  /* unknown here -> let the explorer tray try */
        /* hand the icon (and its list ref) to the pump. Only the pump closes the
         * item's connection. Its unique bus name dies and the watcher removes it. */
        icon->dead = TRUE;
        list_remove( &icon->entry );
        list_add_tail( &dead_list, &icon->entry );
        queued_dead = TRUE;
        ret = TRUE;
        break;
    case NIM_SETVERSION:
        if ((icon = find_icon( nid->hWnd, nid->uID ))) { icon->version = nid->uVersion; ret = TRUE; }
        else ret = -1;   /* unknown here -> let the explorer tray try */
        break;
    default:
        break;
    }
    pthread_mutex_unlock( &sni_mutex );

    /* D-Bus I/O, mutex released. target holds a ref taken under the mutex. It
     * stays alive here even if another thread deletes the icon concurrently. */
    /* a failed item export returns -1 so shell32 routes the icon to the explorer tray */
    if (action == ACT_REGISTER) { if (!sni_register_item( target )) ret = -1; icon_release( target ); }
    else if (target)   /* NIM_MODIFY: emit change signals on the item's own connection */
    {
        if (sig_icon) sni_emit_signal( target, "NewIcon" );
        if (sig_tip) sni_emit_signal( target, "NewToolTip" );
        if (sig_status) sni_emit_status( target );
        icon_release( target );
    }
    if (queued_dead) sni_wake();   /* let the pump reap (close+free) the deleted item */

    return ret;
}

/***********************************************************************
 *   sni_cleanup_icons
 *
 * Drop all icons owned by a window (e.g. its process exited).
 */
void sni_cleanup_icons( HWND owner )
{
    struct sni_icon *icon, *next;
    BOOL queued = FALSE;

    pthread_mutex_lock( &sni_mutex );
    if (sni_available)
        LIST_FOR_EACH_ENTRY_SAFE( icon, next, &icon_list, struct sni_icon, entry )
            if (icon->owner == owner && !icon->dead)
            {
                /* hand to the pump. It closes the connection (name death makes
                 * the watcher remove the item) and frees. */
                icon->dead = TRUE;
                list_remove( &icon->entry );
                list_add_tail( &dead_list, &icon->entry );
                queued = TRUE;
            }
    pthread_mutex_unlock( &sni_mutex );

    if (queued) sni_wake();
}

/***********************************************************************
 *   sni_adjust_menu_position
 *
 * Called from NtUserTrackPopupMenuEx on the app's thread. When the tray
 * published a context-menu position for this owner, move the menu there so
 * the app's own Win32 menu opens at the tray. The native menu then runs as
 * usual.
 */
void sni_adjust_menu_position( HWND hwnd, INT *x, INT *y )
{
    pthread_mutex_lock( &sni_mutex );
    if (native_menu_owner && native_menu_context_is_current() &&
        (hwnd == native_menu_owner || NtUserGetAncestor( hwnd, GA_ROOTOWNER ) == native_menu_owner))
    {
        *x = native_menu_pos.x;
        *y = native_menu_pos.y;
        TRACE( "using SNI context-menu position %d,%d for hwnd %p\n", *x, *y, hwnd );
    }
    pthread_mutex_unlock( &sni_mutex );
}

/***********************************************************************
 *   sni_run_loop
 *
 * Pump incoming D-Bus messages (Activate/ContextMenu/property reads).
 * Runs on a shell32-spawned win32 thread in the application's process so
 * NtUser posts are legal.
 */
/* Close and free every icon queued on dead_list. The SOLE place item connections
 * are closed, only on the pump thread strictly between dispatch passes. So a
 * close can never race a dispatch on the same connection (the use-after-free
 * safety argument). The close kills the unique bus name. The watcher learns the
 * item is gone. */
static void sni_reap_dead( void )
{
    struct list reap = LIST_INIT( reap );
    struct sni_icon *icon, *next;

    pthread_mutex_lock( &sni_mutex );
    LIST_FOR_EACH_ENTRY_SAFE( icon, next, &dead_list, struct sni_icon, entry )
    {
        list_remove( &icon->entry );
        list_add_tail( &reap, &icon->entry );
    }
    pthread_mutex_unlock( &sni_mutex );

    LIST_FOR_EACH_ENTRY_SAFE( icon, next, &reap, struct sni_icon, entry )
    {
        list_remove( &icon->entry );
        if (icon->conn)
        {
            p_dbus_connection_flush( icon->conn );   /* push any final method-return */
            p_dbus_connection_close( icon->conn );    /* name dies -> watcher unregisters item */
            p_dbus_connection_unref( icon->conn );
            icon->conn = NULL;
        }
        icon_release( icon );   /* drop the list ref the pump was handed */
    }
}

#define SNI_MAX_CONNS 64

void sni_run_loop( void )
{
    struct pollfd pfds[2 + SNI_MAX_CONNS];
    struct { DBusConnection *conn; struct sni_icon *icon; } svc[2 + SNI_MAX_CONNS];
    struct sni_icon *icon, *next;
    BOOL exit_loop = FALSE;

    pthread_mutex_lock( &sni_mutex );
    if (!sni_init_done) sni_init();
    pthread_mutex_unlock( &sni_mutex );

    if (!sni_available || !sni_connection || wake_pipe[0] < 0)
    {
        sni_reap_dead();   /* free any queued dead items so a respawn starts clean */
        return;
    }

    TRACE( "entering SNI dispatch loop\n" );
    while (!exit_loop)
    {
        int n = 0, k, fd, timeout, data_remains = 0;

        /* reap dead icons, between dispatch passes, sole closer */
        sni_reap_dead();

        /* leave when idle so the pump does not spin on an empty list.
         * shell32 re-arms the thread (and rechecks for a racing add). */
        pthread_mutex_lock( &sni_mutex );
        if (list_empty( &icon_list ) && list_empty( &dead_list )) exit_loop = TRUE;
        pthread_mutex_unlock( &sni_mutex );
        if (exit_loop) break;

        /* build the poll set: wake-pipe + control conn + each live item conn.
         * Grab a ref per icon so the pointer survives the unlocked poll. */
        pfds[n].fd = wake_pipe[0]; pfds[n].events = POLLIN; svc[n].conn = NULL; svc[n].icon = NULL; n++;

        /* always include the control conn. Even if its fd is momentarily
         * unavailable (poll ignores fd<0). */
        if (!p_dbus_connection_get_unix_fd( sni_connection, &fd )) fd = -1;

        pfds[n].fd = fd;
        pfds[n].events = POLLIN;
        svc[n].conn = sni_connection;
        svc[n].icon = NULL;
        n++;

        pthread_mutex_lock( &sni_mutex );
        LIST_FOR_EACH_ENTRY( icon, &icon_list, struct sni_icon, entry )
        {
            if (n >= ARRAY_SIZE(pfds)) { WARN( "more than %u tray icons; some unserviced\n", SNI_MAX_CONNS ); break; }
            if (!icon->conn || icon->dead) continue;
            if (!p_dbus_connection_get_unix_fd( icon->conn, &fd ) || fd < 0) continue;
            icon_grab( icon );
            pfds[n].fd = fd; pfds[n].events = POLLIN; svc[n].conn = icon->conn; svc[n].icon = icon; n++;
        }
        pthread_mutex_unlock( &sni_mutex );

        /* libdbus can hold whole buffered messages with no new fd activity */
        for (k = 1; k < n; k++)
            if (p_dbus_connection_get_dispatch_status( svc[k].conn ) == DBUS_DISPATCH_DATA_REMAINS)
                data_remains = 1;

        timeout = data_remains ? 0 : 100;
        if (poll( pfds, n, timeout ) < 0)
        {
            for (k = 1; k < n; k++) if (svc[k].icon) icon_release( svc[k].icon );
            if (errno != EINTR) usleep( 100000 );   /* back off a persistent poll error */
            continue;
        }

        /* wake-pipe readable -> connection set changed. drain and rebuild */
        if (pfds[0].revents & POLLIN)
        {
            char buf[64];
            while (read( wake_pipe[0], buf, sizeof(buf) ) > 0) {}
            for (k = 1; k < n; k++) if (svc[k].icon) icon_release( svc[k].icon );
            continue;
        }

        /* service control conn first (icon==NULL), then items */
        for (k = 1; k < n; k++)
        {
            DBusConnection *c = svc[k].conn;
            BOOL ready = (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)) ||
                         p_dbus_connection_get_dispatch_status( c ) == DBUS_DISPATCH_DATA_REMAINS;
            if (ready)
            {
                p_dbus_connection_read_write( c, 0 );
                while (p_dbus_connection_dispatch( c ) == DBUS_DISPATCH_DATA_REMAINS) {}
            }

            if (!p_dbus_connection_get_is_connected( c ))
            {
                if (svc[k].icon == NULL) /* control conn died: the session bus is gone */
                {
                    pthread_mutex_lock( &sni_mutex );
                    sni_available = FALSE;
                    pthread_mutex_unlock( &sni_mutex );
                    exit_loop = TRUE;
                }
                else /* item conn dropped: treat as an implicit delete */
                {
                    pthread_mutex_lock( &sni_mutex );
                    if (!svc[k].icon->dead)
                    {
                        svc[k].icon->dead = TRUE;
                        list_remove( &svc[k].icon->entry );
                        list_add_tail( &dead_list, &svc[k].icon->entry );
                    }
                    pthread_mutex_unlock( &sni_mutex );
                }
            }
        }

        /* release this iteration's snapshot refs */
        for (k = 1; k < n; k++) if (svc[k].icon) icon_release( svc[k].icon );
    }

    /* control conn lost: mark and reap every remaining icon so a shell32 respawn
     * of the pump rebuilds purely from a clean icon_list. */
    pthread_mutex_lock( &sni_mutex );
    LIST_FOR_EACH_ENTRY_SAFE( icon, next, &icon_list, struct sni_icon, entry )
    {
        icon->dead = TRUE;
        list_remove( &icon->entry );
        list_add_tail( &dead_list, &icon->entry );
    }
    pthread_mutex_unlock( &sni_mutex );
    sni_reap_dead();

    pthread_mutex_lock( &sni_mutex );
    if (!sni_available && sni_connection)
    {
        p_dbus_connection_close( sni_connection );
        p_dbus_connection_unref( sni_connection );
        sni_connection = NULL;
        sni_init_done = FALSE;
    }
    pthread_mutex_unlock( &sni_mutex );

    TRACE( "leaving SNI dispatch loop\n" );
}

/* TRUE while there is anything for the pump to service or reap. shell32 uses
 * this to spawn the pump and to re-arm without a stranded-icon race. */
BOOL sni_has_icons( void )
{
    BOOL ret;
    pthread_mutex_lock( &sni_mutex );
    ret = !list_empty( &icon_list ) || !list_empty( &dead_list );
    pthread_mutex_unlock( &sni_mutex );
    return ret;
}

BOOL sni_is_available( void )
{
    pthread_mutex_lock( &sni_mutex );
    if (!sni_init_done) sni_init();
    pthread_mutex_unlock( &sni_mutex );
    return sni_available;
}

#else  /* SONAME_LIBDBUS_1 */

LRESULT sni_notify_icon( HWND owner, UINT msg, NOTIFYICONDATAW *nid ) { return -1; }
BOOL sni_notify_balloon( const NOTIFYICONDATAW *nid ) { return FALSE; }
void sni_cleanup_icons( HWND owner ) { }
void sni_run_loop( void ) { }
BOOL sni_has_icons( void ) { return FALSE; }
BOOL sni_is_available( void ) { return FALSE; }
BOOL sni_get_context_menu_pos( POINT *pos ) { return FALSE; }
BOOL sni_should_layer_context_menu( HWND hwnd, DWORD style, DWORD ex_style, const RECT *rect, BOOL fullscreen ) { return FALSE; }
void sni_adjust_menu_position( HWND hwnd, INT *x, INT *y ) { }

#endif  /* SONAME_LIBDBUS_1 */
