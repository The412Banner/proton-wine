/*
 * X11 graphics driver initialisation functions
 *
 * Copyright 1996 Alexandre Julliard
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

#include <stdarg.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "x11drv.h"
#include "xcomposite.h"
#include "wine/debug.h"
#ifdef HAVE_LIBXSHAPE
#include <X11/extensions/shape.h>
#endif

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

Display *gdi_display;  /* display to use for all GDI functions */

static int palette_size;

static Pixmap stock_bitmap_pixmap;  /* phys bitmap for the default stock bitmap */

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static const struct user_driver_funcs x11drv_funcs;
static const struct gdi_dc_funcs *xrender_funcs;


void init_recursive_mutex( pthread_mutex_t *mutex )
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( mutex, &attr );
    pthread_mutexattr_destroy( &attr );
}


/**********************************************************************
 *	     device_init
 *
 * Perform initializations needed upon creation of the first device.
 */
static void device_init(void)
{
    /* Initialize XRender */
    xrender_funcs = X11DRV_XRender_Init();

    /* Init Xcursor */
    X11DRV_Xcursor_Init();

    palette_size = X11DRV_PALETTE_Init();

    stock_bitmap_pixmap = XCreatePixmap( gdi_display, root_window, 1, 1, 1 );
}


static X11DRV_PDEVICE *create_x11_physdev( Drawable drawable )
{
    X11DRV_PDEVICE *physDev;

    pthread_once( &init_once, device_init );

    if (!(physDev = calloc( 1, sizeof(*physDev) ))) return NULL;

    physDev->drawable = drawable;
    physDev->gc = XCreateGC( gdi_display, drawable, 0, NULL );
    XSetGraphicsExposures( gdi_display, physDev->gc, False );
    XSetSubwindowMode( gdi_display, physDev->gc, IncludeInferiors );
    XFlush( gdi_display );
    return physDev;
}

/**********************************************************************
 *	     X11DRV_CreateDC
 */
static BOOL X11DRV_CreateDC( PHYSDEV *pdev, LPCWSTR device, LPCWSTR output, const DEVMODEW* initData )
{
    X11DRV_PDEVICE *physDev = create_x11_physdev( root_window );

    if (!physDev) return FALSE;

    physDev->depth         = default_visual.depth;
    physDev->color_shifts  = &X11DRV_PALETTE_default_shifts;
    physDev->dc_rect       = NtUserGetVirtualScreenRect( MDT_DEFAULT );
    OffsetRect( &physDev->dc_rect, -physDev->dc_rect.left, -physDev->dc_rect.top );
    push_dc_driver( pdev, &physDev->dev, &x11drv_funcs.dc_funcs );
    if (xrender_funcs && !xrender_funcs->pCreateDC( pdev, device, output, initData )) return FALSE;
    return TRUE;
}


/**********************************************************************
 *	     X11DRV_CreateCompatibleDC
 */
static BOOL X11DRV_CreateCompatibleDC( PHYSDEV orig, PHYSDEV *pdev )
{
    X11DRV_PDEVICE *physDev = create_x11_physdev( stock_bitmap_pixmap );

    if (!physDev) return FALSE;

    physDev->depth  = 1;
    SetRect( &physDev->dc_rect, 0, 0, 1, 1 );
    push_dc_driver( pdev, &physDev->dev, &x11drv_funcs.dc_funcs );
    if (orig) return TRUE;  /* we already went through Xrender if we have an orig device */
    if (xrender_funcs && !xrender_funcs->pCreateCompatibleDC( NULL, pdev )) return FALSE;
    return TRUE;
}


/**********************************************************************
 *	     X11DRV_DeleteDC
 */
static BOOL X11DRV_DeleteDC( PHYSDEV dev )
{
    X11DRV_PDEVICE *physDev = get_x11drv_dev( dev );

    XFreeGC( gdi_display, physDev->gc );
    if (physDev->region) NtGdiDeleteObjectApp( physDev->region );
    free( physDev );
    return TRUE;
}


void add_device_bounds( X11DRV_PDEVICE *dev, const RECT *rect )
{
    RECT rc;

    if (!dev->bounds) return;
    if (dev->region && NtGdiGetRgnBox( dev->region, &rc ))
    {
        if (intersect_rect( &rc, &rc, rect )) add_bounds_rect( dev->bounds, &rc );
    }
    else add_bounds_rect( dev->bounds, rect );
}

/***********************************************************************
 *           X11DRV_SetBoundsRect
 */
static UINT X11DRV_SetBoundsRect( PHYSDEV dev, RECT *rect, UINT flags )
{
    X11DRV_PDEVICE *pdev = get_x11drv_dev( dev );

    if (flags & DCB_DISABLE) pdev->bounds = NULL;
    else if (flags & DCB_ENABLE) pdev->bounds = rect;
    return DCB_RESET;  /* we don't have device-specific bounds */
}


/***********************************************************************
 *           GetDeviceCaps    (X11DRV.@)
 */
static INT X11DRV_GetDeviceCaps( PHYSDEV dev, INT cap )
{
    switch(cap)
    {
    case SIZEPALETTE:
        return palette_size;
    default:
        dev = GET_NEXT_PHYSDEV( dev, pGetDeviceCaps );
        return dev->funcs->pGetDeviceCaps( dev, cap );
    }
}


/***********************************************************************
 *           SelectFont
 */
static HFONT X11DRV_SelectFont( PHYSDEV dev, HFONT hfont, UINT *aa_flags )
{
    if (default_visual.depth <= 8) *aa_flags = GGO_BITMAP;  /* no anti-aliasing on <= 8bpp */
    dev = GET_NEXT_PHYSDEV( dev, pSelectFont );
    return dev->funcs->pSelectFont( dev, hfont, aa_flags );
}

static BOOL get_surface_rect( HWND hwnd, RECT *rect, UINT dpi )
{
    if (!NtUserGetPresentRect( hwnd, rect, dpi ) && !NtUserGetClientRect( hwnd, rect, dpi )) return FALSE;
    OffsetRect( rect, -rect->left, -rect->top );
    return TRUE;
}

static BOOL needs_client_window_clipping( HWND hwnd )
{
    RECT rect, client;
    UINT ret = 0;
    HRGN region;
    HDC hdc;

    if (NtUserGetPresentRect( hwnd, &client, 0 )) return FALSE;
    if (!NtUserGetClientRect( hwnd, &client, NtUserGetDpiForWindow( hwnd ) )) return FALSE;
    OffsetRect( &client, -client.left, -client.top );

    if (!(hdc = NtUserGetDCEx( hwnd, 0, DCX_CACHE | DCX_USESTYLE ))) return FALSE;
    if ((region = NtGdiCreateRectRgn( 0, 0, 0, 0 )))
    {
        ret = NtGdiGetRandomRgn( hdc, region, SYSRGN );
        if (ret > 0 && (ret = NtGdiGetRgnBox( region, &rect )) < NULLREGION) ret = 0;
        if (ret == SIMPLEREGION && EqualRect( &rect, &client )) ret = 0;
        NtGdiDeleteObjectApp( region );
    }
    NtUserReleaseDC( hwnd, hdc );

    return ret > 0;
}

static BOOL enable_fullscreen_hack( HWND hwnd )
{
    static int disable_fshack = -1;

    if (disable_fshack == -1)
    {
        const char *env = getenv( "WINE_DISABLE_FULLSCREEN_HACK" );
        disable_fshack = env && atoi( env );
    }
    if (disable_fshack) return FALSE;

    if (NtUserGetDpiForWindow( hwnd ) != NtUserGetWinMonitorDpi( hwnd, MDT_RAW_DPI )) return TRUE; /* needs DPI scaling */
    return FALSE;
}

/* Opt-in via WINE_LAYERED_OVERLAY_SHAPE=1. For borderless WS_EX_LAYERED windows that
 * paint per-pixel-alpha overlays through a 3D client surface (DWM-glass style, e.g.
 * desktop/taskbar overlay games), shape the X window to the rendered (non-black) pixels
 * so transparent areas show the desktop instead of an opaque black box, and let the
 * window receive mouse input. Disabled by default: no effect on any other game. */
BOOL layered_overlay_shape_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1)
    {
        const char *e = getenv( "WINE_LAYERED_OVERLAY_SHAPE" );
        enabled = e && atoi( e );
    }
    return enabled;
}

/* Opt-in via WINE_LAYERED_OVERLAY_ALPHA=1|2. Real per-pixel-alpha path: give the
 * Vulkan overlay window a 32-bit ARGB visual so the X compositor blends transparency
 * directly (no XShape). Pairs with vkd3d-proton requesting a non-opaque compositeAlpha. */
BOOL layered_overlay_alpha_enabled(void)
{
    static int enabled = -1;
    if (enabled == -1)
    {
        const char *e = getenv( "WINE_LAYERED_OVERLAY_ALPHA" );
        enabled = e && atoi( e );
    }
    return enabled;
}

BOOL needs_offscreen_rendering( HWND hwnd )
{
    UINT style = NtUserGetWindowLongW( hwnd, GWL_STYLE );
    UINT ex_style = NtUserGetWindowLongW( hwnd, GWL_EXSTYLE );
    struct window_surface *surface;
    struct x11drv_win_data *data;
    BOOL needs_offscreen;
    DWORD layered_flags;

    if (!(data = get_win_data( hwnd ))) needs_offscreen = TRUE; /* window is in a different process */
    else
    {
        needs_offscreen = (style & WS_VISIBLE) && !(style & WS_MINIMIZE) && !is_window_rect_mapped( &data->rects.visible );
        release_win_data( data );
    }

    if (!needs_offscreen && style & WS_EX_LAYERED && NtUserGetLayeredWindowAttributes( hwnd, NULL, NULL, &layered_flags )
        && layered_flags & LWA_COLORKEY)
        needs_offscreen = TRUE;

    /* Layered overlay (DWM-glass style) painting per-pixel alpha via a 3D client surface. */
    if (!needs_offscreen && (layered_overlay_shape_enabled() || layered_overlay_alpha_enabled()) && (ex_style & WS_EX_LAYERED))
        needs_offscreen = TRUE;

    if (!needs_offscreen && (surface = window_surface_get( hwnd )))
    {
        TRACE("hwnd %p, surface %p, surface->alpha_mask %#x.\n", hwnd, surface, surface->alpha_mask);
        /* 3d drawing to ULW window never gets onscreen directly, only though UpdateLayeredWindow(). */
        needs_offscreen = !!surface->alpha_mask;
        window_surface_release( surface );
    }
    if (needs_offscreen) return needs_offscreen;

    if (NtUserGetDpiForWindow( hwnd ) != NtUserGetWinMonitorDpi( hwnd, MDT_RAW_DPI ) && !enable_fullscreen_hack( hwnd )) return TRUE; /* needs DPI scaling */
    if (NtUserGetAncestor( hwnd, GA_PARENT ) != NtUserGetDesktopWindow()) return TRUE; /* child window, needs compositing */
    if (NtUserGetWindowRelative( hwnd, GW_CHILD )) return needs_client_window_clipping( hwnd ); /* window has children, needs compositing */
    return FALSE;
}

void set_dc_drawable( HDC hdc, Drawable drawable, const RECT *rect, int mode )
{
    struct x11drv_escape_set_drawable escape =
    {
        .code = X11DRV_SET_DRAWABLE,
        .drawable = drawable,
        .dc_rect = *rect,
        .mode = mode,
    };
    NtGdiExtEscape( hdc, NULL, 0, X11DRV_ESCAPE, sizeof(escape), (LPSTR)&escape, 0, NULL );
}

Drawable get_dc_drawable( HDC hdc, RECT *rect )
{
    struct x11drv_escape_get_drawable escape = {.code = X11DRV_GET_DRAWABLE};
    NtGdiExtEscape( hdc, NULL, 0, X11DRV_ESCAPE, sizeof(escape), (LPSTR)&escape, sizeof(escape), (LPSTR)&escape );
    *rect = escape.dc_rect;
    return escape.drawable;
}

HRGN get_dc_monitor_region( HWND hwnd, HDC hdc )
{
    HRGN region;

    if (!(region = NtGdiCreateRectRgn( 0, 0, 0, 0 ))) return 0;
    if (NtGdiGetRandomRgn( hdc, region, SYSRGN | NTGDI_RGN_MONITOR_DPI ) > 0) return region;
    NtGdiDeleteObjectApp( region );
    return 0;
}

static const struct client_surface_funcs x11drv_client_surface_funcs;

struct x11drv_client_surface
{
    struct client_surface client;
    XWindowChanges changes;
    Colormap colormap;
    Window window;
    RECT rect;
    BOOL raw;

    HDC hdc_src;
    HDC hdc_dst;
    BOOL other_process;

    BYTE *shape_age;
    unsigned int shape_cols;
    unsigned int shape_rows;

    XRectangle *shape_rects;
    unsigned int shape_rect_count;
};

static struct x11drv_client_surface *impl_from_client_surface( struct client_surface *client )
{
    return CONTAINING_RECORD( client, struct x11drv_client_surface, client );
}

static void x11drv_client_surface_destroy( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    if (surface->colormap != default_colormap) XFreeColormap( gdi_display, surface->colormap );
    if (surface->window) destroy_client_window( hwnd, surface->window );
    if (surface->hdc_dst) NtGdiDeleteObjectApp( surface->hdc_dst );
    if (surface->hdc_src) NtGdiDeleteObjectApp( surface->hdc_src );
    free( surface->shape_age );
    free( surface->shape_rects );
}

static void x11drv_client_surface_detach( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    Window client_window = surface->window;
    struct x11drv_win_data *data;
    HWND hwnd = client->hwnd;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    if ((data = get_win_data( hwnd )))
    {
        detach_client_window( data, client_window );
        release_win_data( data );
    }
}

static void client_surface_update_geometry( HWND hwnd, struct x11drv_client_surface *surface )
{
    UINT dpi = surface->raw ? NtUserGetWinMonitorDpi( hwnd, MDT_WINE_RAW_DPI ) : NtUserGetDpiForWindow( hwnd );
    HWND origin = hwnd, toplevel = NtUserGetAncestor( hwnd, GA_ROOT );
    XWindowChanges changes = surface->changes;
    struct x11drv_win_data *data;
    int mask = 0;
    RECT rect;

    if (NtUserGetPresentRect( hwnd, &rect, dpi )) OffsetRect( &rect, -rect.left, -rect.top );
    else if (!NtUserGetClientRect( hwnd, &rect, dpi )) return;
    else NtUserMapWindowPoints( origin, toplevel, (POINT *)&rect, 2, dpi );

    if ((data = get_win_data( toplevel )))
    {
        OffsetRect( &rect, data->rects.client.left - data->rects.visible.left,
                    data->rects.client.top - data->rects.visible.top );
        release_win_data( data );
    }

    changes.x = rect.left;
    changes.y = rect.top;
    changes.width  = min( max( 1, rect.right - rect.left ), 65535 );
    changes.height = min( max( 1, rect.bottom - rect.top ), 65535 );
    OffsetRect( &rect, -rect.left, -rect.top );
    surface->rect = rect;

    if (changes.x != surface->changes.x) mask |= CWX;
    if (changes.y != surface->changes.y) mask |= CWY;
    if (changes.width != surface->changes.width) mask |= CWWidth;
    if (changes.height != surface->changes.height) mask |= CWHeight;
    if (!mask) return;

    surface->changes = changes;
    TRACE( "client window %p/%lx, requesting position %d,%d size %d,%d mask %#x\n", hwnd,
           surface->window, changes.x, changes.y, changes.width, changes.height, mask );
    XConfigureWindow( gdi_display, surface->window, mask, &changes );
    XFlush( gdi_display );
}

static void client_surface_update_offscreen( HWND hwnd, struct x11drv_client_surface *surface )
{
    BOOL offscreen = needs_offscreen_rendering( hwnd );
    struct x11drv_win_data *data;

    if (surface->other_process) offscreen = TRUE;

    TRACE( "%s offscreen %u\n", debugstr_client_surface( &surface->client ), offscreen );

    if (InterlockedExchange( &surface->client.offscreen, offscreen ) == offscreen)
    {
        if (!offscreen && (data = get_win_data( hwnd )))
        {
            attach_client_window( data, surface->window );
            release_win_data( data );
        }
        return;
    }

    if (!offscreen)
    {
#ifdef SONAME_LIBXCOMPOSITE
        if (usexcomposite) pXCompositeUnredirectWindow( gdi_display, surface->window, CompositeRedirectManual );
#endif
        if (surface->hdc_dst)
        {
            NtGdiDeleteObjectApp( surface->hdc_dst );
            surface->hdc_dst = NULL;
        }
        if (surface->hdc_src)
        {
            NtGdiDeleteObjectApp( surface->hdc_src );
            surface->hdc_src = NULL;
        }
    }
    else
    {
        static const WCHAR displayW[] = {'D','I','S','P','L','A','Y', 0};
        UNICODE_STRING device_str = RTL_CONSTANT_STRING(displayW);
        surface->hdc_dst = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );
        surface->hdc_src = NtGdiOpenDCW( &device_str, NULL, NULL, 0, TRUE, NULL, NULL, NULL );
        set_dc_drawable( surface->hdc_src, surface->window, &surface->rect, IncludeInferiors );
#ifdef SONAME_LIBXCOMPOSITE
        if (usexcomposite) pXCompositeRedirectWindow( gdi_display, surface->window, CompositeRedirectManual );
#endif
    }

    if ((data = get_win_data( hwnd )))
    {
        if (offscreen) detach_client_window( data, surface->window );
        else attach_client_window( data, surface->window );
        release_win_data( data );
    }

    XFlush( gdi_display );
}

static void x11drv_client_surface_update( struct client_surface *client )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    client_surface_update_geometry( hwnd, surface );
    client_surface_update_offscreen( hwnd, surface );
}

/* Returns TRUE if the client window has actually-visible pixels this frame (ignoring age
   hysteresis). A FALSE return means the D3D pipeline delivered a cleared/black frame —
   the caller should skip blitting to avoid overwriting screen pixels with the clear colour. */
static BOOL update_layered_overlay_shape( struct x11drv_client_surface *surface, HWND toplevel )
{
#ifdef HAVE_LIBXSHAPE
    unsigned int width, height, cols, rows, x, y, count = 0, capacity;
    BOOL actually_visible = FALSE;
    XRectangle *rects;
    XImage *image;
    Window window;
    /* SHAPE mode clips the bounding region (visual); ALPHA mode sets the INPUT region
       (mouse click-through on transparent areas) while the ARGB visual handles the
       transparency. Input-shape changes don't repaint, so this adds click-through
       without bringing the reshape flicker back. */
    const int shape_kind = layered_overlay_alpha_enabled() ? ShapeInput : ShapeBounding;

    if (!layered_overlay_shape_enabled() && !layered_overlay_alpha_enabled()) return TRUE;

    width = surface->rect.right - surface->rect.left;
    height = surface->rect.bottom - surface->rect.top;
    if (!width || !height || !(window = X11DRV_get_whole_window( toplevel ))) return TRUE;

    cols = (width + 3) / 4;
    rows = (height + 3) / 4;
    if (cols != surface->shape_cols || rows != surface->shape_rows)
    {
        free( surface->shape_age );
        surface->shape_age = calloc( cols * rows, sizeof(*surface->shape_age) );
        surface->shape_cols = cols;
        surface->shape_rows = rows;
        free( surface->shape_rects );
        surface->shape_rects = NULL;
        surface->shape_rect_count = 0;
    }
    if (!surface->shape_age) return TRUE;

    if (!(image = XGetImage( gdi_display, surface->window, 0, 0, width, height, AllPlanes, ZPixmap ))) return TRUE;

    capacity = cols * rows;
    if (!(rects = malloc( capacity * sizeof(*rects) )))
    {
        XDestroyImage( image );
        return TRUE;
    }

    for (y = 0; y < height; y += 4)
    {
        int run_start = -1;

        for (x = 0; x < width; x += 4)
        {
            BYTE *age = &surface->shape_age[(y / 4) * cols + x / 4];
            unsigned int xx, yy;
            BOOL visible = FALSE;

            for (yy = y; yy < min( y + 4, height ) && !visible; yy++)
                for (xx = x; xx < min( x + 4, width ); xx++)
                    if (XGetPixel( image, xx, yy ) & 0x00f0f0f0)
                    {
                        visible = TRUE;
                        break;
                    }

            /* Hysteresis: a block stays in the shape for several frames after it was
               last seen lit. This absorbs the transient all/partial-black frames that
               XGetImage catches while the window is being moved or re-rendered on
               mouse hover (the live X window is read mid-update) -- which would
               otherwise shrink the shape every active frame and flicker. Growth is
               instant (content shows immediately); only shrink is delayed, a brief
               and barely-visible trail. */
            if (visible) { *age = 12; actually_visible = TRUE; }
            else if (*age) --*age;

            if (*age && run_start < 0) run_start = x;
            if ((!*age || x + 4 >= width) && run_start >= 0)
            {
                unsigned int end = *age ? width : x;

                rects[count].x = surface->changes.x + run_start;
                rects[count].y = surface->changes.y + y;
                rects[count].width = end - run_start;
                rects[count].height = min( 4, height - y );
                count++;
                run_start = -1;
            }
        }
    }

    XDestroyImage( image );

    /* Only call XShapeCombineRectangles when the shape actually changed. On the first call
       the window has the default full shape, so an all-black first frame (count==0) must be
       explicitly clipped to nothing. */
    if (!surface->shape_rects ||
        count != surface->shape_rect_count ||
        (count > 0 && memcmp( rects, surface->shape_rects, count * sizeof(*rects) )))
    {
        if (count)
            XShapeCombineRectangles( gdi_display, window, shape_kind, 0, 0, rects, count, ShapeSet, YXBanded );
        else
        {
            static XRectangle empty;
            XShapeCombineRectangles( gdi_display, window, shape_kind, 0, 0, &empty, 1, ShapeSet, YXBanded );
        }

        TRACE( "layered overlay window %p/%lx shape updated with %u rectangles\n", toplevel, window, count );
        XFlush( gdi_display );

        free( surface->shape_rects );
        surface->shape_rects = rects;
        surface->shape_rect_count = count;
    }
    else
    {
        free( rects );
    }

    /* ALPHA mode: always blit (the ARGB content carries its own alpha). SHAPE mode: skip
       the blit on an all-black frame so the previous correct pixels stay (flicker guard). */
    return layered_overlay_alpha_enabled() ? TRUE : actually_visible;
#else
    return TRUE;
#endif
}

static void X11DRV_client_surface_present( struct client_surface *client, HDC hdc )
{
    struct x11drv_client_surface *surface = impl_from_client_surface( client );
    HWND hwnd = client->hwnd, toplevel = NtUserGetAncestor( hwnd, GA_ROOT );
    struct window_surface *win_surface;
    struct x11drv_win_data *data;
    RECT rect_dst, rect;
    Drawable window;
    HRGN region;

    TRACE( "%s\n", debugstr_client_surface( client ) );

    client_surface_update_geometry( hwnd, surface );
    client_surface_update_offscreen( hwnd, surface );

    if (!hdc) return;

    if (hwnd && (win_surface = window_surface_get( hwnd )))
    {
        TRACE( "surface %p, alpha_mask %#x.\n", win_surface, win_surface->alpha_mask );
        if (win_surface->alpha_mask)
        {
            /* GL drawing to ULW window never gets onscreen directly, only though UpdateLayeredWindow(). */
            window_surface_release( win_surface );
            return;
        }

        TRACE( "Surface is present.\n" );
        region = get_dc_monitor_region( hwnd, hdc );
        if (region) NtGdiExtSelectClipRgn( hdc, region, RGN_COPY );
        /* Layered overlay: shape check runs first. If it returns FALSE the client
           window holds the D3D clear colour — skip the blit so the previous correct
           pixels stay in the whole window, avoiding flicker. */
        if (update_layered_overlay_shape( surface, toplevel ))
            NtGdiStretchBlt( hdc, 0, 0, surface->rect.right - surface->rect.left, surface->rect.bottom - surface->rect.top,
                             surface->hdc_src, 0, 0, surface->rect.right, surface->rect.bottom, SRCCOPY, 0 );
        if (region) NtGdiDeleteObjectApp( region );
        window_surface_release( win_surface );
        return;
    }

    window = X11DRV_get_whole_window( toplevel );

    if (NtUserGetPresentRect( toplevel, &rect_dst, -1 /* raw dpi */ ))
    {
        region = 0; /* window is exclusive fullscreen, ignore everything else */
        if (toplevel != hwnd) return; /* toplevel is exclusive fullscreen, don't present */
        OffsetRect( &rect_dst, -rect_dst.left, -rect_dst.top );
    }
    else
    {
        region = get_dc_monitor_region( hwnd, hdc ); /* otherwise use the window region for clipping rules */
        if (!NtUserGetClientRect( hwnd, &rect_dst, NtUserGetWinMonitorDpi( hwnd, MDT_WINE_RAW_DPI ) )) goto done;
        NtUserMapWindowPoints( hwnd, toplevel, (POINT *)&rect_dst, 2, NtUserGetWinMonitorDpi( hwnd, MDT_WINE_RAW_DPI ) );
    }
    if (IsRectEmpty( &rect_dst ) || IsRectEmpty( &surface->rect )) return;

    if ((data = get_win_data( toplevel )))
    {
        OffsetRect( &rect_dst, data->rects.client.left - data->rects.visible.left,
                    data->rects.client.top - data->rects.visible.top );
        release_win_data( data );
    }

    if (get_dc_drawable( surface->hdc_dst, &rect ) != window || !EqualRect( &rect, &rect_dst ))
        set_dc_drawable( surface->hdc_dst, window, &rect_dst, IncludeInferiors );
    if (region) NtGdiExtSelectClipRgn( surface->hdc_dst, region, RGN_COPY );

    if (update_layered_overlay_shape( surface, toplevel ))
        NtGdiStretchBlt( surface->hdc_dst, 0, 0, rect_dst.right - rect_dst.left, rect_dst.bottom - rect_dst.top,
                         surface->hdc_src, 0, 0, surface->rect.right, surface->rect.bottom, SRCCOPY, 0 );
    XFlush( gdi_display );

done:
    if (region) NtGdiDeleteObjectApp( region );
}

static const struct client_surface_funcs x11drv_client_surface_funcs =
{
    .destroy = x11drv_client_surface_destroy,
    .detach = x11drv_client_surface_detach,
    .update = x11drv_client_surface_update,
    .present = X11DRV_client_surface_present,
};

static int visual_class_alloc( int class )
{
    return class == PseudoColor || class == GrayScale || class == DirectColor ? AllocAll : AllocNone;
}

static BOOL disable_opwr(void)
{
    static int disable = -1;

    if (disable == -1)
    {
        const char *e = getenv( "WINE_DISABLE_VULKAN_OPWR" );
        disable = e && atoi( e );
    }
    return disable;
}

Window x11drv_client_surface_create( HWND hwnd, BOOL raw, int format, struct client_surface **client )
{
    UINT dpi = raw ? NtUserGetWinMonitorDpi( hwnd, MDT_WINE_RAW_DPI ) : NtUserGetDpiForWindow( hwnd );
    struct x11drv_client_surface *surface;
    XVisualInfo visual = default_visual;
    DWORD hwnd_pid, hwnd_thread_id;
    Colormap colormap;

    if (format && !visual_from_pixel_format( format, &visual )) return None;

    /* Layered overlay (WINE_LAYERED_OVERLAY_ALPHA): use a 32-bit ARGB visual for the
     * Vulkan surface window so the X compositor can blend its per-pixel alpha. The env
     * var already scopes this to the opted-in game, so apply unconditionally — the
     * window may not be WS_EX_LAYERED yet when the Vulkan surface is first created. */
    if (!format && layered_overlay_alpha_enabled())
    {
        if (argb_visual.visualid)
        {
            visual = argb_visual;
            TRACE( "layered overlay: using ARGB visual %#lx depth %d for hwnd %p\n",
                   visual.visualid, visual.depth, hwnd );
        }
        else
            WARN( "layered overlay: no 32-bit ARGB visual available, transparency unavailable\n" );
    }

    if (visual.visualid == default_visual.visualid) colormap = default_colormap;
    else colormap = XCreateColormap( gdi_display, get_dummy_parent(), visual.visual, visual_class_alloc( visual.class ) );
    if (!colormap) return None;

    if (!(surface = client_surface_create( sizeof(*surface), &x11drv_client_surface_funcs, hwnd ))) goto failed;
    surface->colormap = colormap;
    surface->raw = raw;

    if (!get_surface_rect( hwnd, &surface->rect, dpi )) goto failed;
    hwnd_thread_id = NtUserGetWindowThread(hwnd, &hwnd_pid);
    if (hwnd_thread_id && hwnd_pid != GetCurrentProcessId())
    {
        XSetWindowAttributes attr;
        RECT rect = surface->rect;
        unsigned int width, height;

        if (disable_opwr() && hwnd != NtUserGetDesktopWindow())
        {
            ERR( "HACK: Failing surface creation for other process window %p.\n", hwnd );
            goto failed;
        }

        width = max( rect.right - rect.left, 1 );
        height = max( rect.bottom - rect.top, 1 );
        attr.colormap = default_colormap;
        attr.bit_gravity = NorthWestGravity;
        attr.win_gravity = NorthWestGravity;
        attr.backing_store = NotUseful;
        attr.border_pixel = 0;
        surface->window = XCreateWindow( gdi_display, get_dummy_parent(), 0, 0, width, height, 0, default_visual.depth, InputOutput,
                                         default_visual.visual, CWBitGravity | CWWinGravity | CWBackingStore | CWColormap | CWBorderPixel, &attr );
        if (surface->window)
        {
            XMapWindow( gdi_display, surface->window );
            XSync( gdi_display, False );
            surface->other_process = TRUE;
        }
        WARN( "Other process window %p / %#lx.\n", hwnd, surface->window );
    }

    if (!surface->window && !(surface->window = create_client_window( hwnd, surface->rect, &visual, colormap ))) goto failed;

    TRACE( "Created %s for client window %lx\n", debugstr_client_surface( &surface->client ), surface->window );
    *client = &surface->client;
    return surface->window;

failed:
    if (surface) client_surface_release( &surface->client );
    else if (colormap != default_colormap) XFreeColormap( gdi_display, colormap );
    return None;
}

/**********************************************************************
 *           ExtEscape  (X11DRV.@)
 */
static INT X11DRV_ExtEscape( PHYSDEV dev, INT escape, INT in_count, LPCVOID in_data,
                             INT out_count, LPVOID out_data )
{
    X11DRV_PDEVICE *physDev = get_x11drv_dev( dev );

    switch(escape)
    {
    case QUERYESCSUPPORT:
        if (in_data && in_count >= sizeof(DWORD))
        {
            switch (*(const INT *)in_data)
            {
            case X11DRV_ESCAPE:
                return TRUE;
            }
        }
        break;

    case X11DRV_ESCAPE:
        if (in_data && in_count >= sizeof(enum x11drv_escape_codes))
        {
            switch(*(const enum x11drv_escape_codes *)in_data)
            {
            case X11DRV_SET_DRAWABLE:
                if (in_count >= sizeof(struct x11drv_escape_set_drawable))
                {
                    const struct x11drv_escape_set_drawable *data = in_data;
                    physDev->dc_rect = data->dc_rect;
                    physDev->drawable = data->drawable;
                    XFreeGC( gdi_display, physDev->gc );
                    physDev->gc = XCreateGC( gdi_display, physDev->drawable, 0, NULL );
                    XSetGraphicsExposures( gdi_display, physDev->gc, False );
                    XSetSubwindowMode( gdi_display, physDev->gc, data->mode );
                    TRACE( "SET_DRAWABLE hdc %p drawable %lx dc_rect %s\n",
                           dev->hdc, physDev->drawable, wine_dbgstr_rect(&physDev->dc_rect) );
                    return TRUE;
                }
                break;
            case X11DRV_GET_DRAWABLE:
                if (out_count >= sizeof(struct x11drv_escape_get_drawable))
                {
                    struct x11drv_escape_get_drawable *data = out_data;
                    data->drawable = physDev->drawable;
                    data->dc_rect = physDev->dc_rect;
                    return TRUE;
                }
                break;
            case X11DRV_START_EXPOSURES:
                XSetGraphicsExposures( gdi_display, physDev->gc, True );
                physDev->exposures = 0;
                return TRUE;
            case X11DRV_END_EXPOSURES:
                if (out_count >= sizeof(HRGN))
                {
                    HRGN hrgn = 0, tmp = 0;

                    XSetGraphicsExposures( gdi_display, physDev->gc, False );
                    if (physDev->exposures)
                    {
                        for (;;)
                        {
                            XEvent event;

                            XWindowEvent( gdi_display, physDev->drawable, ~0, &event );
                            if (event.type == NoExpose) break;
                            if (event.type == GraphicsExpose)
                            {
                                DWORD layout;
                                RECT rect;

                                rect.left   = event.xgraphicsexpose.x - physDev->dc_rect.left;
                                rect.top    = event.xgraphicsexpose.y - physDev->dc_rect.top;
                                rect.right  = rect.left + event.xgraphicsexpose.width;
                                rect.bottom = rect.top + event.xgraphicsexpose.height;
                                if (NtGdiGetDCDword( dev->hdc, NtGdiGetLayout, &layout ) &&
                                    (layout & LAYOUT_RTL))
                                    mirror_rect( &physDev->dc_rect, &rect );

                                TRACE( "got %s count %d\n", wine_dbgstr_rect(&rect),
                                       event.xgraphicsexpose.count );

                                if (!tmp) tmp = NtGdiCreateRectRgn( rect.left, rect.top,
                                                                    rect.right, rect.bottom );
                                else NtGdiSetRectRgn( tmp, rect.left, rect.top, rect.right, rect.bottom );
                                if (hrgn) NtGdiCombineRgn( hrgn, hrgn, tmp, RGN_OR );
                                else
                                {
                                    hrgn = tmp;
                                    tmp = 0;
                                }
                                if (!event.xgraphicsexpose.count) break;
                            }
                            else
                            {
                                ERR( "got unexpected event %d\n", event.type );
                                break;
                            }
                        }
                        if (tmp) NtGdiDeleteObjectApp( tmp );
                    }
                    *(HRGN *)out_data = hrgn;
                    return TRUE;
                }
                break;
            default:
                break;
            }
        }
        break;
    }
    return 0;
}


static const struct user_driver_funcs x11drv_funcs =
{
    .dc_funcs.pArc = X11DRV_Arc,
    .dc_funcs.pChord = X11DRV_Chord,
    .dc_funcs.pCreateCompatibleDC = X11DRV_CreateCompatibleDC,
    .dc_funcs.pCreateDC = X11DRV_CreateDC,
    .dc_funcs.pDeleteDC = X11DRV_DeleteDC,
    .dc_funcs.pEllipse = X11DRV_Ellipse,
    .dc_funcs.pExtEscape = X11DRV_ExtEscape,
    .dc_funcs.pExtFloodFill = X11DRV_ExtFloodFill,
    .dc_funcs.pFillPath = X11DRV_FillPath,
    .dc_funcs.pGetDeviceCaps = X11DRV_GetDeviceCaps,
    .dc_funcs.pGetImage = X11DRV_GetImage,
    .dc_funcs.pGetNearestColor = X11DRV_GetNearestColor,
    .dc_funcs.pGetSystemPaletteEntries = X11DRV_GetSystemPaletteEntries,
    .dc_funcs.pGradientFill = X11DRV_GradientFill,
    .dc_funcs.pLineTo = X11DRV_LineTo,
    .dc_funcs.pPaintRgn = X11DRV_PaintRgn,
    .dc_funcs.pPatBlt = X11DRV_PatBlt,
    .dc_funcs.pPie = X11DRV_Pie,
    .dc_funcs.pPolyPolygon = X11DRV_PolyPolygon,
    .dc_funcs.pPolyPolyline = X11DRV_PolyPolyline,
    .dc_funcs.pPutImage = X11DRV_PutImage,
    .dc_funcs.pRealizeDefaultPalette = X11DRV_RealizeDefaultPalette,
    .dc_funcs.pRealizePalette = X11DRV_RealizePalette,
    .dc_funcs.pRectangle = X11DRV_Rectangle,
    .dc_funcs.pRoundRect = X11DRV_RoundRect,
    .dc_funcs.pSelectBrush = X11DRV_SelectBrush,
    .dc_funcs.pSelectFont = X11DRV_SelectFont,
    .dc_funcs.pSelectPen = X11DRV_SelectPen,
    .dc_funcs.pSetBoundsRect = X11DRV_SetBoundsRect,
    .dc_funcs.pSetDCBrushColor = X11DRV_SetDCBrushColor,
    .dc_funcs.pSetDCPenColor = X11DRV_SetDCPenColor,
    .dc_funcs.pSetDeviceClipping = X11DRV_SetDeviceClipping,
    .dc_funcs.pSetPixel = X11DRV_SetPixel,
    .dc_funcs.pStretchBlt = X11DRV_StretchBlt,
    .dc_funcs.pStrokeAndFillPath = X11DRV_StrokeAndFillPath,
    .dc_funcs.pStrokePath = X11DRV_StrokePath,
    .dc_funcs.pUnrealizePalette = X11DRV_UnrealizePalette,
    .dc_funcs.priority = GDI_PRIORITY_GRAPHICS_DRV,

    .pActivateKeyboardLayout = X11DRV_ActivateKeyboardLayout,
    .pBeep = X11DRV_Beep,
    .pGetKeyNameText = X11DRV_GetKeyNameText,
    .pMapVirtualKeyEx = X11DRV_MapVirtualKeyEx,
    .pToUnicodeEx = X11DRV_ToUnicodeEx,
    .pVkKeyScanEx = X11DRV_VkKeyScanEx,
    .pNotifyIMEStatus = X11DRV_NotifyIMEStatus,
    .pSetIMECompositionRect = X11DRV_SetIMECompositionRect,
    .pDestroyCursorIcon = X11DRV_DestroyCursorIcon,
    .pSetCursor = X11DRV_SetCursor,
    .pSetCursorPos = X11DRV_SetCursorPos,
    .pClipCursor = X11DRV_ClipCursor,
    .pSystrayDockInit = X11DRV_SystrayDockInit,
    .pSystrayDockInsert = X11DRV_SystrayDockInsert,
    .pSystrayDockClear = X11DRV_SystrayDockClear,
    .pSystrayDockRemove = X11DRV_SystrayDockRemove,
    .pChangeDisplaySettings = X11DRV_ChangeDisplaySettings,
    .pUpdateDisplayDevices = X11DRV_UpdateDisplayDevices,
    .pCreateDesktop = X11DRV_CreateDesktop,
    .pCreateWindow = X11DRV_CreateWindow,
    .pDesktopWindowProc = X11DRV_DesktopWindowProc,
    .pDestroyWindow = X11DRV_DestroyWindow,
    .pFlashWindowEx = X11DRV_FlashWindowEx,
    .pHasWindowManager = X11DRV_HasWindowManager,
    .pGetDC = X11DRV_GetDC,
    .pProcessEvents = X11DRV_ProcessEvents,
    .pReleaseDC = X11DRV_ReleaseDC,
    .pScrollDC = X11DRV_ScrollDC,
    .pSetCapture = X11DRV_SetCapture,
    .pSetDesktopWindow = X11DRV_SetDesktopWindow,
    .pActivateWindow = X11DRV_ActivateWindow,
    .pSetLayeredWindowAttributes = X11DRV_SetLayeredWindowAttributes,
    .pSetParent = X11DRV_SetParent,
    .pSetWindowIcons = X11DRV_SetWindowIcons,
    .pSetWindowRgn = X11DRV_SetWindowRgn,
    .pSetWindowStyle = X11DRV_SetWindowStyle,
    .pSetWindowText = X11DRV_SetWindowText,
    .pShowWindow = X11DRV_ShowWindow,
    .pSysCommand = X11DRV_SysCommand,
    .pClipboardWindowProc = X11DRV_ClipboardWindowProc,
    .pUpdateClipboard = X11DRV_UpdateClipboard,
    .pUpdateLayeredWindow = X11DRV_UpdateLayeredWindow,
    .pWindowMessage = X11DRV_WindowMessage,
    .pWindowPosChanging = X11DRV_WindowPosChanging,
    .pGetWindowStyleMasks = X11DRV_GetWindowStyleMasks,
    .pGetWindowStateUpdates = X11DRV_GetWindowStateUpdates,
    .pCreateWindowSurface = X11DRV_CreateWindowSurface,
    .pMoveWindowBits = X11DRV_MoveWindowBits,
    .pWindowPosChanged = X11DRV_WindowPosChanged,
    .pSystemParametersInfo = X11DRV_SystemParametersInfo,
    .pWintabProc = X11DRV_WintabProc,
    .pVulkanInit = X11DRV_VulkanInit,
    .pOpenGLInit = X11DRV_OpenGLInit,
    .pThreadDetach = X11DRV_ThreadDetach,
};


void init_user_driver(void)
{
    __wine_set_user_driver( &x11drv_funcs, WINE_GDI_DRIVER_VERSION );
}
