/*
 * Copyright (C) 2004 Mike Hearn, for CodeWeavers
 * Copyright (C) 2005 Robert Shearman
 * Copyright (C) 2008 Alexandre Julliard
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

#include <assert.h>

#include <windows.h>
#include <ntuser.h>
#include <commctrl.h>
#include <shellapi.h>

#include <wine/debug.h>
#include <wine/list.h>

#include "explorer_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(systray);

#define TRAY_MINIMIZE_ALL 419
#define TRAY_MINIMIZE_ALL_UNDO 416

struct notify_data_icon
{
    /* data for the icon bitmap */
    UINT width;
    UINT height;
    UINT planes;
    UINT bpp;
};

struct notify_data  /* platform-independent format for NOTIFYICONDATA */
{
    LONG  hWnd;
    UINT  uID;
    UINT  uFlags;
    UINT  uCallbackMessage;
    struct notify_data_icon icon_info; /* systray icon bitmap info */
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    } u;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID  guidItem;
    struct notify_data_icon balloon_icon_info; /* balloon icon bitmap info */
    BYTE icon_data[];
};

#define ICON_DISPLAY_HIDDEN -1
#define ICON_DISPLAY_DOCKED -2

/* an individual systray icon, unpacked from the NOTIFYICONDATA and always in unicode */
struct icon
{
    struct list    entry;
    HICON          image;    /* the image to render */
    HWND           owner;    /* the HWND passed in to the Shell_NotifyIcon call */
    HWND           window;   /* the adaptor window */
    BOOL           layered;  /* whether we are using a layered window */
    HWND           tooltip;  /* Icon tooltip */
    UINT           state;    /* state flags */
    UINT           id;       /* the unique id given by the app */
    UINT           callback_message;
    int            display;  /* index in display list, or -1 if hidden */
    WCHAR          tiptext[128]; /* Tooltip text. If empty => tooltip disabled */
    WCHAR          info_text[256];  /* info balloon text */
    WCHAR          info_title[64];  /* info balloon title */
    UINT           info_flags;      /* flags for info balloon */
    UINT           info_timeout;    /* timeout for info balloon */
    HICON          info_icon;       /* info balloon icon */
    UINT           version;         /* notify icon api version */
};

static struct list icon_list = LIST_INIT( icon_list );

struct taskbar_button
{
    struct list entry;
    HWND        hwnd;
    HWND        button;
    BOOL        active;
    BOOL        visible;
    HICON       icon;         /* small icon of the owning executable */
    BOOL        icon_loaded;  /* whether we tried to load the icon */
};

static struct list taskbar_buttons = LIST_INIT( taskbar_buttons );

static HWND tray_window;

static unsigned int nb_displayed;

static BOOL enable_taskbar; /* show full taskbar, with dedicated systray area */
static BOOL show_systray; /* show a standalone systray window */
static BOOL enable_dock; /* allow systray icons to be docked in the host systray */
static BOOL no_tray_items; /* hide the systray and all systray icons */

static int icon_cx, icon_cy, tray_width, tray_height;
static int start_button_width, taskbar_button_width;
static WCHAR start_label[50];

/* Luna ("Windows XP") taskbar style */

#define TASKBAR_MAX_ROWS  3
#define CLOCK_TIMER       0x100
#define SYNC_TIMER        0x101
#define SIZE_TIMER        0x102
#define SYNC_DELAY        40
#define SIZE_POLL_DELAY   15

#define IDM_TASKBAR_LOCK  0x7001
#define IDM_TASK_MANAGER  0x7002
#define IDM_TASKBAR_ROWS  0x7010  /* + number of rows */

static BOOL  xp_style;           /* paint the taskbar in the Luna style */
static BOOL  xp_initialized;     /* clock timer, tooltip and event hooks are set up */
static UINT  taskbar_scheme;     /* Luna color scheme */
static BOOL  show_clock = TRUE;  /* show the clock in the notification area */
static BOOL  taskbar_locked;     /* taskbar height can't be changed */
static int   taskbar_rows = 1;   /* number of rows of task buttons */
static UINT  taskbar_dpi = USER_DEFAULT_SCREEN_DPI;
static int   row_height = 30;    /* height of a single taskbar row */
static int   clock_width;        /* width of the clock text area */
static int   notify_left;        /* left edge of the notification area */
static HFONT xp_font, xp_start_font;
static HICON start_icon, default_task_icon;
static HWND  clock_tooltip;
static BOOL  start_pressed;      /* the start menu is open */
static BOOL  sizing_taskbar;     /* the taskbar height is being dragged */
static int   sizing_start_y, sizing_start_rows;
static WCHAR start_text[50];
static WCHAR clock_time[32], clock_day[32], clock_date[32], clock_tip[128];

static POINT xp_get_icon_pos( int display );
static void xp_draw_background( HDC hdc, int dst_x, int dst_y, int x, int y, int width, int height );
static void xp_sync_taskbar_buttons(void);

static struct icon *balloon_icon;
static HWND balloon_window;
static POINT balloon_pos;

#define MIN_DISPLAYED 8
#define ICON_BORDER  2

#define BALLOON_CREATE_TIMER 1
#define BALLOON_SHOW_TIMER   2

#define BALLOON_CREATE_TIMEOUT   2000
#define BALLOON_SHOW_MIN_TIMEOUT 10000
#define BALLOON_SHOW_MAX_TIMEOUT 30000

#define WM_POPUPSYSTEMMENU  0x0313

static LRESULT WINAPI shell_traywnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );
static LRESULT WINAPI tray_icon_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam );

static void show_icon( struct icon *icon );
static void hide_icon( struct icon *icon );
static BOOL delete_icon( struct icon *icon );

static WNDCLASSEXW shell_traywnd_class =
{
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_DBLCLKS | CS_HREDRAW,
    .lpfnWndProc = shell_traywnd_proc,
    .hbrBackground = (HBRUSH)COLOR_WINDOW,
    .lpszClassName = L"Shell_TrayWnd",
};
static WNDCLASSEXW tray_icon_class =
{
    .cbSize = sizeof(WNDCLASSEXW),
    .style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
    .lpfnWndProc = tray_icon_wndproc,
    .lpszClassName = L"__wine_tray_icon",
};

static void do_hide_systray(void);
static void do_show_systray(void);

/* Retrieves icon record by owner window and ID */
static struct icon *get_icon(HWND owner, UINT id)
{
    struct icon *this;

    /* search for the icon */
    LIST_FOR_EACH_ENTRY( this, &icon_list, struct icon, entry )
        if ((this->id == id) && (this->owner == owner)) return this;

    return NULL;
}

static void init_common_controls(void)
{
    static BOOL initialized = FALSE;

    if (!initialized)
    {
        INITCOMMONCONTROLSEX init_tooltip;

        init_tooltip.dwSize = sizeof(INITCOMMONCONTROLSEX);
        init_tooltip.dwICC = ICC_TAB_CLASSES|ICC_STANDARD_CLASSES;

        InitCommonControlsEx(&init_tooltip);
        initialized = TRUE;
    }
}

/* Creates tooltip window for icon. */
static void create_tooltip(struct icon *icon)
{
    TTTOOLINFOW ti;

    init_common_controls();
    icon->tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     icon->window, NULL, NULL, NULL );

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;
    SendMessageW(icon->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void set_balloon_position( struct icon *icon )
{
    RECT rect;
    POINT pos;

    GetWindowRect( icon->window, &rect );
    pos.x = (rect.left + rect.right) / 2;
    pos.y = (rect.top + rect.bottom) / 2;
    SendMessageW( balloon_window, TTM_TRACKPOSITION, 0, MAKELONG( pos.x, pos.y ));
}

static void update_systray_balloon_position(void)
{
    RECT rect;
    POINT pos;

    if (!balloon_icon) return;
    GetWindowRect( balloon_icon->window, &rect );
    pos.x = (rect.left + rect.right) / 2;
    pos.y = (rect.top + rect.bottom) / 2;
    if (pos.x == balloon_pos.x && pos.y == balloon_pos.y) return; /* nothing changed */
    balloon_pos = pos;
    SendMessageW( balloon_window, TTM_TRACKPOSITION, 0, MAKELONG( pos.x, pos.y ));
}

static void balloon_create_timer( struct icon *icon )
{
    TTTOOLINFOW ti;

    init_common_controls();
    balloon_window = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_BALLOON | TTS_CLOSE,
                                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                      icon->window, NULL, NULL, NULL );

    memset( &ti, 0, sizeof(ti) );
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.uFlags = TTF_TRACK | TTF_IDISHWND;
    ti.lpszText = icon->info_text;
    SendMessageW( balloon_window, TTM_ADDTOOLW, 0, (LPARAM)&ti );
    if ((icon->info_flags & NIIF_ICONMASK) == NIIF_USER)
    {
        SendMessageW( balloon_window, TTM_SETTITLEW, (WPARAM)icon->info_icon, (LPARAM)icon->info_title );
    }
    else
    {
        UINT info_flags_wparam = icon->info_flags & NIIF_ERROR;

        if (icon->info_flags & NIIF_LARGEICON)
            info_flags_wparam += TTI_ERROR;
        SendMessageW( balloon_window, TTM_SETTITLEW, info_flags_wparam, (LPARAM)icon->info_title );
    }
    balloon_icon = icon;
    balloon_pos.x = balloon_pos.y = MAXLONG;
    update_systray_balloon_position();
    SendMessageW( balloon_window, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti );
    KillTimer( icon->window, BALLOON_CREATE_TIMER );
    SetTimer( icon->window, BALLOON_SHOW_TIMER, icon->info_timeout, NULL );
}

static BOOL show_balloon( struct icon *icon )
{
    if (!show_systray) return FALSE;  /* systray has been hidden */
    if (icon->display == ICON_DISPLAY_HIDDEN) return FALSE;  /* not displayed */
    if (!icon->info_text[0]) return FALSE;  /* no balloon */
    balloon_icon = icon;
    SetTimer( icon->window, BALLOON_CREATE_TIMER, BALLOON_CREATE_TIMEOUT, NULL );
    return TRUE;
}

static void hide_balloon( struct icon *icon )
{
    if (!balloon_icon) return;
    if (balloon_window)
    {
        KillTimer( balloon_icon->window, BALLOON_SHOW_TIMER );
        DestroyWindow( balloon_window );
        balloon_window = 0;
    }
    else KillTimer( balloon_icon->window, BALLOON_CREATE_TIMER );
    balloon_icon = NULL;
}

static void show_next_balloon(void)
{
    struct icon *icon;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        if (show_balloon( icon )) break;
}

static void update_balloon( struct icon *icon )
{
    if (balloon_icon == icon)
    {
        hide_balloon( icon );
        show_balloon( icon );
    }
    else if (!balloon_icon)
    {
        show_balloon( icon );
    }
}

static void balloon_timer( struct icon *icon )
{
    icon->info_text[0] = 0;  /* clear text now that balloon has been shown */
    hide_balloon( icon );
    show_next_balloon();
}

/* Synchronize tooltip text with tooltip window */
static void update_tooltip_text(struct icon *icon)
{
    TTTOOLINFOW ti;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;

    SendMessageW(icon->tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

/* get the position of an icon in the stand-alone tray */
static POINT get_icon_pos( struct icon *icon )
{
    POINT pos;

    if (enable_taskbar && xp_style) return xp_get_icon_pos( icon->display );

    if (enable_taskbar)
    {
        pos.x = tray_width - icon_cx * (icon->display + 1);
        pos.y = (tray_height - icon_cy) / 2;
    }
    else
    {
        pos.x = icon_cx * icon->display;
        pos.y = 0;
    }

    return pos;
}

/* get the size of the stand-alone tray window */
static SIZE get_window_size(void)
{
    SIZE size;
    RECT rect;

    rect.left = 0;
    rect.top = 0;
    rect.right = icon_cx * max( nb_displayed, MIN_DISPLAYED );
    rect.bottom = icon_cy;
    AdjustWindowRect( &rect, WS_CAPTION, FALSE );
    size.cx = rect.right - rect.left;
    size.cy = rect.bottom - rect.top;
    return size;
}

/* synchronize tooltip position with tooltip window */
static void update_tooltip_position( struct icon *icon )
{
    TTTOOLINFOW ti;

    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = icon->window;
    ti.uId = (UINT_PTR)icon->window;
    ti.lpszText = icon->tiptext;
    SendMessageW( icon->tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti );
    if (balloon_icon == icon) set_balloon_position( icon );
}

static void paint_layered_icon( struct icon *icon )
{
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    int width = GetSystemMetrics( SM_CXSMICON );
    int height = GetSystemMetrics( SM_CYSMICON );
    BITMAPINFO *info;
    HBITMAP dib, mask;
    HDC hdc;
    RECT rc;
    SIZE size;
    POINT pos;
    int i, x, y;
    void *color_bits, *mask_bits;
    DWORD *ptr;
    BOOL has_alpha = FALSE;

    GetWindowRect( icon->window, &rc );
    size.cx = rc.right - rc.left;
    size.cy = rc.bottom - rc.top;
    pos.x = (size.cx - width) / 2;
    pos.y = (size.cy - height) / 2;

    if (!(info = calloc( 1, FIELD_OFFSET( BITMAPINFO, bmiColors[2] ) ))) return;
    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth = size.cx;
    info->bmiHeader.biHeight = size.cy;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biCompression = BI_RGB;

    hdc = CreateCompatibleDC( 0 );
    if (!(dib = CreateDIBSection( 0, info, DIB_RGB_COLORS, &color_bits, NULL, 0 ))) goto done;
    SelectObject( hdc, dib );
    DrawIconEx( hdc, pos.x, pos.y, icon->image, width, height, 0, 0, DI_DEFAULTSIZE | DI_NORMAL );

    /* check if the icon was drawn with an alpha channel */
    for (i = 0, ptr = color_bits; i < size.cx * size.cy; i++)
        if ((has_alpha = (ptr[i] & 0xff000000) != 0)) break;

    if (!has_alpha)
    {
        unsigned int width_bytes = (size.cx + 31) / 32 * 4;

        info->bmiHeader.biBitCount = 1;
        info->bmiColors[0].rgbRed = 0;
        info->bmiColors[0].rgbGreen = 0;
        info->bmiColors[0].rgbBlue = 0;
        info->bmiColors[0].rgbReserved = 0;
        info->bmiColors[1].rgbRed = 0xff;
        info->bmiColors[1].rgbGreen = 0xff;
        info->bmiColors[1].rgbBlue = 0xff;
        info->bmiColors[1].rgbReserved = 0;

        if (!(mask = CreateDIBSection( 0, info, DIB_RGB_COLORS, &mask_bits, NULL, 0 ))) goto done;
        memset( mask_bits, 0xff, width_bytes * size.cy );
        SelectObject( hdc, mask );
        DrawIconEx( hdc, pos.x, pos.y, icon->image, width, height, 0, 0, DI_DEFAULTSIZE | DI_MASK );

        for (y = 0, ptr = color_bits; y < size.cy; y++)
            for (x = 0; x < size.cx; x++, ptr++)
                if (!((((BYTE *)mask_bits)[y * width_bytes + x / 8] << (x % 8)) & 0x80))
                    *ptr |= 0xff000000;

        SelectObject( hdc, dib );
        DeleteObject( mask );
    }

    UpdateLayeredWindow( icon->window, 0, NULL, NULL, hdc, NULL, 0, &blend, ULW_ALPHA );
done:
    free( info );
    if (hdc) DeleteDC( hdc );
    if (dib) DeleteObject( dib );
}

static BOOL notify_owner( struct icon *icon, UINT msg, LPARAM lparam )
{
    WPARAM wp = icon->id;
    LPARAM lp = msg;

    if (icon->version >= NOTIFYICON_VERSION_4)
    {
        POINT pt = {.x = (short)LOWORD(lparam), .y = (short)HIWORD(lparam)};
        ClientToScreen( icon->window, &pt );
        wp = MAKEWPARAM( pt.x, pt.y );
        lp = MAKELPARAM( msg, icon->id );
    }

    TRACE( "relaying 0x%x\n", msg );
    if (!SendNotifyMessageW( icon->owner, icon->callback_message, wp, lp ) &&
        (GetLastError() == ERROR_INVALID_WINDOW_HANDLE))
    {
        WARN( "application window was destroyed, removing icon %u\n", icon->id );
        delete_icon( icon );
        return FALSE;
    }
    return TRUE;
}

/* window procedure for the individual tray icon window */
static LRESULT WINAPI tray_icon_wndproc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct icon *icon = (struct icon *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    TRACE( "hwnd %p, msg %#x, wparam %#Ix, lparam %#Ix\n", hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_NCCREATE:
    {
        /* set the icon data for the window from the data passed into CreateWindow */
        const CREATESTRUCTW *info = (const CREATESTRUCTW *)lparam;
        icon = info->lpCreateParams;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)icon );
        break;
    }

    case WM_CLOSE:
        if (icon->display == ICON_DISPLAY_DOCKED)
        {
            TRACE( "icon %u no longer embedded\n", icon->id );
            hide_icon( icon );
            show_icon( icon );
        }
        return 0;

    case WM_CREATE:
        icon->window = hwnd;
        create_tooltip( icon );
        break;

    case WM_SIZE:
    case WM_MOVE:
        if (icon->layered) paint_layered_icon( icon );
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc;
        int cx, cy;

        if (icon->layered) break;

        cx = GetSystemMetrics( SM_CXSMICON );
        cy = GetSystemMetrics( SM_CYSMICON );
        hdc = BeginPaint( hwnd, &ps );
        GetClientRect( hwnd, &rc );
        TRACE( "painting rect %s\n", wine_dbgstr_rect( &rc ) );
        if (xp_style && icon->display >= 0)
        {
            /* the tray window clips its children, paint the taskbar under the icon */
            POINT pt = { 0, 0 };

            MapWindowPoints( hwnd, tray_window, &pt, 1 );
            xp_draw_background( hdc, 0, 0, pt.x, pt.y, rc.right, rc.bottom );
        }
        DrawIconEx( hdc, (rc.left + rc.right - cx) / 2, (rc.top + rc.bottom - cy) / 2,
                    icon->image, cx, cy, 0, 0, DI_DEFAULTSIZE | DI_NORMAL );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    {
        MSG message = {.hwnd = hwnd, .message = msg, .wParam = wparam, .lParam = lparam};
        SendMessageW( icon->tooltip, TTM_RELAYEVENT, 0, (LPARAM)&message );
        if (!notify_owner( icon, msg, lparam )) break;
        if (icon->version > 0)
        {
            if (msg == WM_LBUTTONUP) notify_owner( icon, NIN_SELECT, lparam );
            if (msg == WM_RBUTTONUP) notify_owner( icon, WM_CONTEXTMENU, lparam );
        }
        break;
    }

    case WM_WINDOWPOSCHANGING:
        if (icon->display == ICON_DISPLAY_HIDDEN)
        {
            /* Changing the icon's parent via SetParent would activate it, stealing the focus. */
            WINDOWPOS *wp = (WINDOWPOS*)lparam;
            wp->flags |= SWP_NOACTIVATE;
        }
        break;

    case WM_WINDOWPOSCHANGED:
        update_systray_balloon_position();
        break;

    case WM_TIMER:
        switch (wparam)
        {
        case BALLOON_CREATE_TIMER: balloon_create_timer( icon ); break;
        case BALLOON_SHOW_TIMER: balloon_timer( icon ); break;
        }
        return 0;
    }

    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* add an icon to the system tray window */
static void systray_add_icon( struct icon *icon )
{
    POINT pos;

    if (icon->display != ICON_DISPLAY_HIDDEN) return;  /* already added */

    SetWindowLongW( icon->window, GWL_STYLE, GetWindowLongW( icon->window, GWL_STYLE ) | WS_CHILD );
    SetParent( icon->window, tray_window );
    icon->display = nb_displayed++;
    pos = get_icon_pos( icon );
    SetWindowPos( icon->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW );

    if (nb_displayed == 1 && show_systray) do_show_systray();
    TRACE( "added %u now %d icons\n", icon->id, nb_displayed );
}

/* remove an icon from the stand-alone tray */
static void systray_remove_icon( struct icon *icon )
{
    struct icon *ptr;
    POINT pos;

    if (icon->display == ICON_DISPLAY_HIDDEN) return;  /* already removed */

    assert( nb_displayed );
    LIST_FOR_EACH_ENTRY( ptr, &icon_list, struct icon, entry )
    {
        if (ptr == icon) continue;
        if (ptr->display < icon->display) continue;
        ptr->display--;
        update_tooltip_position( ptr );
        pos = get_icon_pos( ptr );
        SetWindowPos( ptr->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
    }

    if (!--nb_displayed && !enable_taskbar) do_hide_systray();
    TRACE( "removed %u now %d icons\n", icon->id, nb_displayed );

    icon->display = ICON_DISPLAY_HIDDEN;
    SetParent( icon->window, GetDesktopWindow() );
    SetWindowLongW( icon->window, GWL_STYLE, GetWindowLongW( icon->window, GWL_STYLE ) & ~WS_CHILD );
}

/* make an icon visible */
static void show_icon(struct icon *icon)
{
    TRACE( "id=0x%x, hwnd=%p\n", icon->id, icon->owner );

    if (no_tray_items) return;

    if (icon->display != ICON_DISPLAY_HIDDEN) return;  /* already displayed */

    if (enable_dock)
    {
        DWORD old_exstyle = GetWindowLongW( icon->window, GWL_EXSTYLE );

        /* make sure it is layered before calling into the driver */
        SetWindowLongW( icon->window, GWL_EXSTYLE, old_exstyle | WS_EX_LAYERED );
        paint_layered_icon( icon );

        if (!NtUserMessageCall( icon->window, WINE_SYSTRAY_DOCK_INSERT, icon_cx, icon_cy,
                                icon, NtUserSystemTrayCall, FALSE ))
            SetWindowLongW( icon->window, GWL_EXSTYLE, old_exstyle );
        else
        {
            icon->display = ICON_DISPLAY_DOCKED;
            icon->layered = TRUE;
            SendMessageW( icon->window, WM_SIZE, SIZE_RESTORED, MAKELONG( icon_cx, icon_cy ) );
        }
    }
    systray_add_icon( icon );

    update_tooltip_position( icon );
    update_balloon( icon );
}

/* make an icon invisible */
static void hide_icon(struct icon *icon)
{
    TRACE( "id=0x%x, hwnd=%p\n", icon->id, icon->owner );

    if (icon->display == ICON_DISPLAY_HIDDEN) return;  /* already hidden */

    if (enable_dock && NtUserMessageCall( icon->window, WINE_SYSTRAY_DOCK_REMOVE, 0, 0,
                                          NULL, NtUserSystemTrayCall, FALSE ))
    {
        icon->display = ICON_DISPLAY_HIDDEN;
        icon->layered = FALSE;
        SetWindowLongW( icon->window, GWL_EXSTYLE, GetWindowLongW( icon->window, GWL_EXSTYLE ) & ~WS_EX_LAYERED );
    }
    ShowWindow( icon->window, SW_HIDE );
    systray_remove_icon( icon );

    update_balloon( icon );
    update_tooltip_position( icon );
}

/* Modifies an existing icon record */
static BOOL modify_icon( struct icon *icon, NOTIFYICONDATAW *nid )
{
    TRACE( "id=0x%x, hwnd=%p\n", nid->uID, nid->hWnd );

    /* demarshal the request from the NID */
    if (!icon)
    {
        WARN( "Invalid icon ID (0x%x) for HWND %p\n", nid->uID, nid->hWnd );
        return FALSE;
    }

    if (nid->uFlags & NIF_STATE)
    {
        icon->state = (icon->state & ~nid->dwStateMask) | (nid->dwState & nid->dwStateMask);
    }

    if (nid->uFlags & NIF_ICON)
    {
        if (icon->image) DestroyIcon(icon->image);
        icon->image = CopyIcon(nid->hIcon);

        if (icon->display >= 0)
            InvalidateRect( icon->window, NULL, TRUE );
        else if (icon->layered)
            paint_layered_icon( icon );
        else if (enable_dock)
            NtUserMessageCall( icon->window, WINE_SYSTRAY_DOCK_CLEAR, 0, 0,
                               NULL, NtUserSystemTrayCall, FALSE );
    }

    if (nid->uFlags & NIF_MESSAGE)
    {
        icon->callback_message = nid->uCallbackMessage;
    }
    if (nid->uFlags & NIF_TIP)
    {
        lstrcpynW( icon->tiptext, nid->szTip, ARRAY_SIZE( icon->tiptext ));
        update_tooltip_text( icon );
    }
    if (nid->uFlags & NIF_INFO && nid->cbSize >= NOTIFYICONDATAA_V2_SIZE)
    {
        lstrcpynW( icon->info_text, nid->szInfo, ARRAY_SIZE( icon->info_text ));
        lstrcpynW( icon->info_title, nid->szInfoTitle, ARRAY_SIZE( icon->info_title ));
        icon->info_flags = nid->dwInfoFlags;
        icon->info_timeout = max(min(nid->uTimeout, BALLOON_SHOW_MAX_TIMEOUT), BALLOON_SHOW_MIN_TIMEOUT);

        if (icon->info_icon) DestroyIcon( icon->info_icon );
        icon->info_icon = CopyIcon( nid->hBalloonIcon );

        update_balloon( icon );
    }
    if (icon->state & NIS_HIDDEN) hide_icon( icon );
    else show_icon( icon );
    return TRUE;
}

/* Adds a new icon record to the list */
static BOOL add_icon(NOTIFYICONDATAW *nid)
{
    struct icon  *icon;

    TRACE( "id=0x%x, hwnd=%p\n", nid->uID, nid->hWnd );

    if ((icon = get_icon(nid->hWnd, nid->uID)))
    {
        WARN( "duplicate tray icon add, buggy app?\n" );
        return FALSE;
    }

    if (!(icon = calloc( 1, sizeof(*icon) )))
    {
        ERR( "out of memory\n" );
        return FALSE;
    }

    ZeroMemory(icon, sizeof(struct icon));
    icon->id     = nid->uID;
    icon->owner  = nid->hWnd;
    icon->display = ICON_DISPLAY_HIDDEN;

    CreateWindowExW( 0, tray_icon_class.lpszClassName, NULL, WS_CLIPSIBLINGS | WS_POPUP,
                     0, 0, icon_cx, icon_cy, 0, NULL, NULL, icon );
    if (!icon->window) ERR( "Failed to create systray icon window\n" );

    list_add_tail(&icon_list, &icon->entry);

    return modify_icon( icon, nid );
}

/* Deletes tray icon window and icon record */
static BOOL delete_icon( struct icon *icon )
{
    hide_icon( icon );
    list_remove( &icon->entry );
    DestroyWindow( icon->tooltip );
    DestroyWindow( icon->window );
    DestroyIcon( icon->image );
    free( icon );
    return TRUE;
}

/* cleanup icons belonging to a window that has been destroyed */
static void cleanup_systray_window( HWND hwnd )
{
    NOTIFYICONDATAW nid = {.cbSize = sizeof(nid), .hWnd = hwnd};
    struct icon *icon, *next;

    LIST_FOR_EACH_ENTRY_SAFE( icon, next, &icon_list, struct icon, entry )
        if (icon->owner == hwnd) delete_icon( icon );

    NtUserMessageCall( hwnd, WINE_SYSTRAY_CLEANUP_ICONS, 0, 0, NULL, NtUserSystemTrayCall, FALSE );
}

/* update the taskbar buttons when something changed */
static void sync_taskbar_buttons(void)
{
    struct taskbar_button *win;
    int pos = 0, count = 0;
    int width = taskbar_button_width;
    int right = tray_width - nb_displayed * icon_cx;
    HWND foreground = GetAncestor( GetForegroundWindow(), GA_ROOTOWNER );

    if (!enable_taskbar) return;
    if (!IsWindowVisible( tray_window )) return;
    if (xp_style)
    {
        xp_sync_taskbar_buttons();
        return;
    }

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!win->hwnd)  /* start button */
        {
            SetWindowPos( win->button, 0, pos, 0, start_button_width, tray_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            pos += start_button_width;
            continue;
        }
        win->active = (win->hwnd == foreground);
        win->visible = IsWindowVisible( win->hwnd ) && !GetWindow( win->hwnd, GW_OWNER );
        if (win->visible) count++;
    }

    /* shrink buttons if space is tight */
    if (count && (count * width > right - pos))
        width = max( taskbar_button_width / 4, (right - pos) / count );

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!win->hwnd) continue;  /* start button */
        if (win->visible && right - pos >= width)
        {
            SetWindowPos( win->button, 0, pos, 0, width, tray_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            InvalidateRect( win->button, NULL, TRUE );
            pos += width;
        }
        else SetWindowPos( win->button, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW );
    }
}

static BOOL handle_incoming(HWND hwndSource, COPYDATASTRUCT *cds)
{
    struct icon *icon = NULL;
    const struct notify_data *data;
    const BYTE *icon_data;
    NOTIFYICONDATAW nid;
    int ret = FALSE;

    if (cds->cbData < sizeof(*data)) return FALSE;
    data = cds->lpData;
    icon_data = data->icon_data;

    nid.cbSize           = sizeof(nid);
    nid.hWnd             = LongToHandle( data->hWnd );
    nid.uID              = data->uID;
    nid.uFlags           = data->uFlags;
    nid.uCallbackMessage = data->uCallbackMessage;
    nid.hIcon            = 0;
    nid.dwState          = data->dwState;
    nid.dwStateMask      = data->dwStateMask;
    nid.uTimeout         = data->u.uTimeout;
    nid.dwInfoFlags      = data->dwInfoFlags;
    nid.guidItem         = data->guidItem;
    lstrcpyW( nid.szTip, data->szTip );
    lstrcpyW( nid.szInfo, data->szInfo );
    lstrcpyW( nid.szInfoTitle, data->szInfoTitle );
    nid.hBalloonIcon     = 0;

    /* FIXME: if statement only needed because we don't support interprocess
     * icon handles */
    if (nid.uFlags & NIF_ICON)
    {
        LONG cbMaskBits;
        LONG cbColourBits;

        cbMaskBits = (data->icon_info.width * data->icon_info.height + 15) / 16 * 2;
        cbColourBits = (data->icon_info.planes * data->icon_info.width * data->icon_info.height * data->icon_info.bpp + 15) / 16 * 2;

        if (cds->cbData < sizeof(*data) + cbMaskBits + cbColourBits)
        {
            ERR( "buffer underflow\n" );
            return FALSE;
        }
        nid.hIcon = CreateIcon(NULL, data->icon_info.width, data->icon_info.height, data->icon_info.planes, data->icon_info.bpp,
                               icon_data, icon_data + cbMaskBits);
        icon_data += cbMaskBits + cbColourBits;
    }

    if ((nid.uFlags & NIF_INFO) && (nid.dwInfoFlags & NIIF_ICONMASK) == NIIF_USER)
    {
        /* Balloon icon */
        LONG cbMaskBits;
        LONG cbColourBits;

        cbMaskBits = (data->balloon_icon_info.width * data->balloon_icon_info.height + 15) / 16 * 2;
        cbColourBits = (data->balloon_icon_info.planes * data->balloon_icon_info.width * data->balloon_icon_info.height * data->balloon_icon_info.bpp + 15) / 16 * 2;

        if (cds->cbData < ((char*)icon_data - (char*)data) + cbMaskBits + cbColourBits)
        {
            ERR( "buffer underflow\n" );
            return FALSE;
        }
        nid.hBalloonIcon = CreateIcon(NULL, data->balloon_icon_info.width, data->balloon_icon_info.height, data->balloon_icon_info.planes, data->balloon_icon_info.bpp,
                                      icon_data, icon_data + cbMaskBits);
    }
    /* try forwarding to the display driver first */
    if (cds->dwData == NIM_ADD || !(icon = get_icon( nid.hWnd, nid.uID )))
    {
        if ((ret = NtUserMessageCall( hwndSource, WINE_SYSTRAY_NOTIFY_ICON, cds->dwData, 0,
                                      &nid, NtUserSystemTrayCall, FALSE )) != -1)
            goto done;
        ret = FALSE;
    }

    switch (cds->dwData)
    {
    case NIM_ADD:
        ret = add_icon(&nid);
        break;
    case NIM_DELETE:
        if (icon) ret = delete_icon( icon );
        break;
    case NIM_MODIFY:
        if (icon) ret = modify_icon( icon, &nid );
        break;
    case NIM_SETVERSION:
        if (icon)
        {
            icon->version = nid.uVersion;
            ret = TRUE;
        }
        break;
    default:
        FIXME( "unhandled tray message: %Id\n", cds->dwData );
        break;
    }

done:
    if (nid.hIcon) DestroyIcon( nid.hIcon );
    if (nid.hBalloonIcon) DestroyIcon( nid.hBalloonIcon );
    sync_taskbar_buttons();
    return ret;
}

static void add_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win;

    if (!enable_taskbar) return;

    /* ignore our own windows */
    if (hwnd)
    {
        DWORD process;
        if (!GetWindowThreadProcessId( hwnd, &process ) || process == GetCurrentProcessId()) return;
    }

    if (!(win = calloc( 1, sizeof(*win) ))) return;
    win->hwnd = hwnd;
    win->button = CreateWindowW( WC_BUTTONW, NULL, WS_CHILD | BS_OWNERDRAW,
                                 0, 0, 0, 0, tray_window, (HMENU)hwnd, 0, 0 );
    list_add_tail( &taskbar_buttons, &win->entry );
}

static struct taskbar_button *find_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win;

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
        if (win->hwnd == hwnd) return win;

    return NULL;
}

static void remove_taskbar_button( HWND hwnd )
{
    struct taskbar_button *win = find_taskbar_button( hwnd );

    if (!win) return;
    list_remove( &win->entry );
    DestroyWindow( win->button );
    if (win->icon) DestroyIcon( win->icon );
    free( win );
}

static void xp_paint_taskbar_button( const DRAWITEMSTRUCT *dis, struct taskbar_button *win );

static void paint_taskbar_button( const DRAWITEMSTRUCT *dis )
{
    RECT rect;
    UINT flags = DC_TEXT;
    struct taskbar_button *win = find_taskbar_button( LongToHandle( dis->CtlID ));

    if (!win) return;
    if (xp_style)
    {
        xp_paint_taskbar_button( dis, win );
        return;
    }
    GetClientRect( dis->hwndItem, &rect );
    DrawFrameControl( dis->hDC, &rect, DFC_BUTTON, DFCS_BUTTONPUSH | DFCS_ADJUSTRECT |
                      ((dis->itemState & ODS_SELECTED) ? DFCS_PUSHED : 0 ));
    if (win->hwnd)
    {
        flags |= win->active ? DC_ACTIVE : DC_INBUTTON;
        DrawCaptionTempW( win->hwnd, dis->hDC, &rect, 0, 0, NULL, flags );
    }
    else  /* start button */
        DrawCaptionTempW( 0, dis->hDC, &rect, 0, 0, start_label, flags | DC_INBUTTON | DC_ICON );
}

static void click_taskbar_button( HWND button )
{
    LONG_PTR id = GetWindowLongPtrW( button, GWLP_ID );
    HWND hwnd = (HWND)id;

    if (!hwnd)  /* start button */
    {
        if (xp_style)
        {
            /* keep the button pushed while the menu is open */
            start_pressed = TRUE;
            InvalidateRect( button, NULL, FALSE );
            UpdateWindow( button );
            do_xp_startmenu( tray_window );
        }
        else do_startmenu( tray_window );
        if (xp_style)
        {
            start_pressed = FALSE;
            InvalidateRect( button, NULL, FALSE );
        }
        return;
    }

    if (IsIconic( hwnd ))
    {
        SendMessageW( hwnd, WM_SYSCOMMAND, SC_RESTORE, 0 );
        return;
    }

    if (IsWindowEnabled( hwnd ))
    {
        if (hwnd == GetForegroundWindow())
        {
            SendMessageW( hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0 );
            return;
        }
    }
    else  /* look for an enabled window owned by this one */
    {
        HWND owned = GetWindow( GetDesktopWindow(), GW_CHILD );
        while (owned && owned != hwnd)
        {
            if (IsWindowVisible( owned ) &&
                IsWindowEnabled( owned ) &&
                (GetWindow( owned, GW_OWNER ) == hwnd))
                break;
            owned = GetWindow( owned, GW_HWNDNEXT );
        }
        hwnd = owned;
    }
    SetForegroundWindow( hwnd );
}

static void show_taskbar_contextmenu( HWND button, LPARAM lparam )
{
    ULONG_PTR id = GetWindowLongPtrW( button, GWLP_ID );

    if (id) SendNotifyMessageW( (HWND)id, WM_POPUPSYSTEMMENU, 0, lparam );
}

/*
 * Luna ("Windows XP") taskbar
 *
 * The taskbar, the start button, the task buttons and the notification area
 * are painted by hand so that the look doesn't depend on the current theme.
 * HKCU\Software\Wine\Explorer\Taskbar: Style ("xp" or "classic"), Rows, Locked.
 */

struct gradient_stop
{
    BYTE     pos;    /* in percent of the height of one taskbar row */
    COLORREF color;
};

static const struct gradient_stop blue_bar_gradient[] =
{
    {   0, RGB(0x1f,0x2f,0x86) }, {   3, RGB(0x31,0x65,0xc4) }, {   6, RGB(0x36,0x82,0xe5) },
    {  10, RGB(0x44,0x90,0xe6) }, {  12, RGB(0x38,0x83,0xe5) }, {  15, RGB(0x2b,0x71,0xe0) },
    {  18, RGB(0x26,0x63,0xda) }, {  20, RGB(0x23,0x5b,0xd6) }, {  23, RGB(0x22,0x58,0xd5) },
    {  38, RGB(0x21,0x57,0xd6) }, {  54, RGB(0x24,0x5d,0xdb) }, {  86, RGB(0x25,0x62,0xdf) },
    {  89, RGB(0x24,0x5f,0xdc) }, {  92, RGB(0x21,0x58,0xd4) }, {  95, RGB(0x1d,0x4e,0xc0) },
    {  98, RGB(0x19,0x41,0xa5) }, { 100, RGB(0x19,0x41,0xa5) },
};

static const struct gradient_stop blue_notify_gradient[] =
{
    {   0, RGB(0x0c,0x59,0xb9) }, {   1, RGB(0x0c,0x59,0xb9) }, {   6, RGB(0x13,0x9e,0xe9) },
    {  10, RGB(0x18,0xb5,0xf2) }, {  14, RGB(0x13,0x9b,0xeb) }, {  19, RGB(0x12,0x90,0xe8) },
    {  63, RGB(0x0d,0x8d,0xea) }, {  81, RGB(0x0d,0x9f,0xf1) }, {  88, RGB(0x0f,0x9e,0xed) },
    {  91, RGB(0x11,0x9b,0xe9) }, {  94, RGB(0x13,0x92,0xe2) }, {  97, RGB(0x13,0x7e,0xd7) },
    { 100, RGB(0x09,0x5b,0xc9) },
};

static const struct gradient_stop olive_bar_gradient[] =
{
    {   0, RGB(0x6b,0x7a,0x45) }, {   3, RGB(0xa3,0xb8,0x7a) }, {   6, RGB(0xc3,0xd4,0x9c) },
    {  10, RGB(0xd0,0xde,0xa8) }, {  12, RGB(0xbc,0xcd,0x93) }, {  15, RGB(0xa9,0xbb,0x7f) },
    {  18, RGB(0x9a,0xad,0x72) }, {  20, RGB(0x93,0xa6,0x6c) }, {  23, RGB(0x91,0xa4,0x6a) },
    {  38, RGB(0x8f,0xa2,0x68) }, {  54, RGB(0x93,0xa6,0x6c) }, {  86, RGB(0x97,0xaa,0x70) },
    {  89, RGB(0x94,0xa7,0x6d) }, {  92, RGB(0x8c,0x9f,0x65) }, {  95, RGB(0x7f,0x92,0x5a) },
    {  98, RGB(0x6e,0x80,0x49) }, { 100, RGB(0x6e,0x80,0x49) },
};

static const struct gradient_stop olive_notify_gradient[] =
{
    {   0, RGB(0x7d,0x8c,0x58) }, {   1, RGB(0x7d,0x8c,0x58) }, {   6, RGB(0xc9,0xd6,0xa6) },
    {  10, RGB(0xd6,0xe2,0xb5) }, {  14, RGB(0xc7,0xd4,0xa2) }, {  19, RGB(0xbc,0xcb,0x96) },
    {  63, RGB(0xb6,0xc5,0x8f) }, {  81, RGB(0xbf,0xcd,0x99) }, {  88, RGB(0xbc,0xcb,0x96) },
    {  91, RGB(0xb7,0xc6,0x90) }, {  94, RGB(0xae,0xbd,0x87) }, {  97, RGB(0x9f,0xae,0x78) },
    { 100, RGB(0x86,0x95,0x5f) },
};

static const struct gradient_stop silver_bar_gradient[] =
{
    {   0, RGB(0x8e,0x8e,0xa8) }, {   3, RGB(0xf2,0xf2,0xf7) }, {   6, RGB(0xff,0xff,0xff) },
    {  10, RGB(0xf7,0xf7,0xfb) }, {  12, RGB(0xec,0xec,0xf3) }, {  15, RGB(0xe3,0xe3,0xec) },
    {  18, RGB(0xdc,0xdc,0xe6) }, {  20, RGB(0xd8,0xd8,0xe3) }, {  23, RGB(0xd6,0xd6,0xe1) },
    {  38, RGB(0xd4,0xd4,0xdf) }, {  54, RGB(0xd8,0xd8,0xe3) }, {  86, RGB(0xdc,0xdc,0xe7) },
    {  89, RGB(0xd9,0xd9,0xe4) }, {  92, RGB(0xd2,0xd2,0xde) }, {  95, RGB(0xc5,0xc5,0xd3) },
    {  98, RGB(0xa9,0xa9,0xbd) }, { 100, RGB(0xa9,0xa9,0xbd) },
};

static const struct gradient_stop silver_notify_gradient[] =
{
    {   0, RGB(0x9a,0x9a,0xb0) }, {   1, RGB(0x9a,0x9a,0xb0) }, {   6, RGB(0xfb,0xfb,0xfd) },
    {  10, RGB(0xff,0xff,0xff) }, {  14, RGB(0xf4,0xf4,0xf8) }, {  19, RGB(0xec,0xec,0xf2) },
    {  63, RGB(0xe6,0xe6,0xee) }, {  81, RGB(0xee,0xee,0xf4) }, {  88, RGB(0xec,0xec,0xf2) },
    {  91, RGB(0xe6,0xe6,0xee) }, {  94, RGB(0xdc,0xdc,0xe6) }, {  97, RGB(0xca,0xca,0xd8) },
    { 100, RGB(0xa8,0xa8,0xbc) },
};

/* colors of the Luna color schemes: Default (blue), Olive Green and Silver */
struct xp_scheme
{
    const struct gradient_stop *bar;
    unsigned int                bar_count;
    const struct gradient_stop *notify;
    unsigned int                notify_count;
    COLORREF notify_edge, notify_light;
    COLORREF button_top, button_bottom, button_light_top, button_light_left, button_dark_right, button_dark_bottom;
    COLORREF down_top, down_bottom, down_dark, down_dark2;
    COLORREF text;
    COLORREF grip_light, grip_dark;
};

static const struct xp_scheme xp_schemes[] =
{
    {
        blue_bar_gradient, ARRAY_SIZE(blue_bar_gradient), blue_notify_gradient, ARRAY_SIZE(blue_notify_gradient),
        RGB(0x10,0x42,0xaf), RGB(0x18,0xbb,0xff),
        RGB(0x4f,0x92,0xf7), RGB(0x35,0x78,0xee), RGB(0x78,0xad,0xfa), RGB(0x63,0x9e,0xf8),
        RGB(0x25,0x57,0xbf), RGB(0x2a,0x62,0xcc),
        RGB(0x1a,0x4a,0xab), RGB(0x1e,0x55,0xbd), RGB(0x10,0x32,0x7a), RGB(0x16,0x41,0x98),
        RGB(0xff,0xff,0xff), RGB(0x86,0xb4,0xf8), RGB(0x12,0x3a,0x9c),
    },
    {
        olive_bar_gradient, ARRAY_SIZE(olive_bar_gradient), olive_notify_gradient, ARRAY_SIZE(olive_notify_gradient),
        RGB(0x6d,0x7c,0x47), RGB(0xdf,0xe9,0xc2),
        RGB(0xb3,0xc4,0x8c), RGB(0x9a,0xac,0x72), RGB(0xcf,0xdc,0xaa), RGB(0xc2,0xd1,0x9c),
        RGB(0x75,0x86,0x4f), RGB(0x82,0x94,0x5b),
        RGB(0x7d,0x8f,0x53), RGB(0x89,0x9c,0x5e), RGB(0x5c,0x6b,0x3a), RGB(0x6c,0x7c,0x45),
        RGB(0xff,0xff,0xff), RGB(0xdc,0xe7,0xbd), RGB(0x5f,0x6e,0x3c),
    },
    {
        silver_bar_gradient, ARRAY_SIZE(silver_bar_gradient), silver_notify_gradient, ARRAY_SIZE(silver_notify_gradient),
        RGB(0x9d,0x9d,0xb3), RGB(0xff,0xff,0xff),
        RGB(0xfc,0xfc,0xfe), RGB(0xe2,0xe2,0xea), RGB(0xff,0xff,0xff), RGB(0xff,0xff,0xff),
        RGB(0xa4,0xa4,0xb8), RGB(0xb8,0xb8,0xc8),
        RGB(0xc6,0xc6,0xd4), RGB(0xd6,0xd6,0xe0), RGB(0x8a,0x8a,0xa0), RGB(0xa3,0xa3,0xb6),
        RGB(0x00,0x00,0x00), RGB(0xff,0xff,0xff), RGB(0x9a,0x9a,0xae),
    },
};

static const struct xp_scheme *xp_current_scheme(void)
{
    return &xp_schemes[taskbar_scheme < ARRAY_SIZE(xp_schemes) ? taskbar_scheme : 0];
}

/* the start menu follows the color scheme of the taskbar */
UINT get_taskbar_scheme(void)
{
    return taskbar_scheme < ARRAY_SIZE(xp_schemes) ? taskbar_scheme : 0;
}

static const struct gradient_stop start_gradient[] =
{
    {   0, RGB(0x2f,0x7a,0x2c) }, {   5, RGB(0x78,0xc6,0x6f) }, {  12, RGB(0x60,0xb7,0x58) },
    {  25, RGB(0x4b,0xa6,0x44) }, {  60, RGB(0x3d,0x9a,0x39) }, {  85, RGB(0x37,0x90,0x33) },
    {  95, RGB(0x2d,0x7c,0x2a) }, { 100, RGB(0x24,0x62,0x21) },
};

static const struct gradient_stop start_pushed_gradient[] =
{
    {   0, RGB(0x1f,0x55,0x1d) }, {   5, RGB(0x3f,0x86,0x39) }, {  12, RGB(0x39,0x80,0x34) },
    {  25, RGB(0x34,0x7a,0x30) }, {  60, RGB(0x2e,0x72,0x2b) }, {  85, RGB(0x2a,0x6b,0x27) },
    {  95, RGB(0x23,0x5e,0x21) }, { 100, RGB(0x1c,0x4c,0x1a) },
};

static int xp_scale( int size )
{
    return MulDiv( size, taskbar_dpi, USER_DEFAULT_SCREEN_DPI );
}

static int xp_band_left(void)
{
    return start_button_width + xp_scale( taskbar_locked ? 6 : 14 );
}

static COLORREF blend_color( COLORREF c1, COLORREF c2, int num, int den )
{
    if (den <= 0) return c1;
    num = max( 0, min( num, den ));
    return RGB( GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * num / den,
                GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * num / den,
                GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * num / den );
}

/* map a pixel row of a bar onto the gradient of a single row (in 1/1000), stretching
 * the middle part so that the top highlight and the bottom shade keep their size */
static int gradient_pos( int y, int height )
{
    int ref = max( row_height, 1 ), top = ref / 4, bottom = ref / 7;

    if (height <= ref) return y * 1000 / max( height, 1 );
    if (y < top) return y * 1000 / ref;
    if (y >= height - bottom) return (ref - (height - y)) * 1000 / ref;
    return (top + (y - top) * (ref - bottom - top) / (height - bottom - top)) * 1000 / ref;
}

static COLORREF gradient_color( const struct gradient_stop *stops, unsigned int count, int y, int height )
{
    int pos = gradient_pos( y, height );
    unsigned int i;

    for (i = 1; i < count; i++)
        if (pos <= stops[i].pos * 10)
            return blend_color( stops[i - 1].color, stops[i].color, pos - stops[i - 1].pos * 10,
                                (stops[i].pos - stops[i - 1].pos) * 10 );
    return stops[count - 1].color;
}

static void fill_solid( HDC hdc, int x, int y, int width, int height, COLORREF color )
{
    HGDIOBJ old_brush = SelectObject( hdc, GetStockObject( DC_BRUSH ));

    SetDCBrushColor( hdc, color );
    PatBlt( hdc, x, y, width, height, PATCOPY );
    SelectObject( hdc, old_brush );
}

/* fill a rectangle with the rows [offset, offset + height) of a gradient spanning 'total' rows */
static void fill_gradient( HDC hdc, int x, int y, int width, int height, int offset, int total,
                           const struct gradient_stop *stops, unsigned int count )
{
    HGDIOBJ old_brush = SelectObject( hdc, GetStockObject( DC_BRUSH ));
    int i;

    for (i = 0; i < height; i++)
    {
        SetDCBrushColor( hdc, gradient_color( stops, count, offset + i, total ));
        PatBlt( hdc, x, y + i, width, 1, PATCOPY );
    }
    SelectObject( hdc, old_brush );
}

static void fill_gradient2( HDC hdc, int x, int y, int width, int height, COLORREF top, COLORREF bottom )
{
    HGDIOBJ old_brush = SelectObject( hdc, GetStockObject( DC_BRUSH ));
    int i;

    for (i = 0; i < height; i++)
    {
        SetDCBrushColor( hdc, blend_color( top, bottom, i, max( height - 1, 1 )));
        PatBlt( hdc, x, y + i, width, 1, PATCOPY );
    }
    SelectObject( hdc, old_brush );
}

/* paint the taskbar background of the tray window area (x, y, width, height) at (dst_x, dst_y) */
static void xp_draw_background( HDC hdc, int dst_x, int dst_y, int x, int y, int width, int height )
{
    const struct xp_scheme *scheme = xp_current_scheme();
    int split = notify_left - x;  /* start of the notification area */

    if (width <= 0 || height <= 0) return;
    if (split > 0)
        fill_gradient( hdc, dst_x, dst_y, min( split, width ), height, y, tray_height,
                       scheme->bar, scheme->bar_count );
    if (split < width)
    {
        int start = max( split, 0 );

        fill_gradient( hdc, dst_x + start, dst_y, width - start, height, y, tray_height,
                       scheme->notify, scheme->notify_count );
        /* dark edge and highlight on the left of the notification area */
        if (split >= 0) fill_solid( hdc, dst_x + split, dst_y, 1, height, scheme->notify_edge );
        if (split + 1 >= 0 && split + 1 < width)
            fill_solid( hdc, dst_x + split + 1, dst_y, 1, height, scheme->notify_light );
    }
}

static void xp_draw_gripper( HDC hdc, int x )
{
    const struct xp_scheme *scheme = xp_current_scheme();
    int y, dot = max( xp_scale( 1 ), 1 ), step = max( xp_scale( 4 ), 3 );

    for (y = xp_scale( 5 ); y + 2 * dot <= tray_height - xp_scale( 4 ); y += step)
    {
        fill_solid( hdc, x + dot, y + dot, dot, dot, scheme->grip_dark );
        fill_solid( hdc, x, y, dot, dot, scheme->grip_light );
    }
}

static unsigned int xp_clock_lines( const WCHAR **lines )
{
    lines[0] = clock_time;
    if (taskbar_rows < 2) return 1;
    lines[1] = clock_day;
    lines[2] = clock_date;
    return 3;
}

static void get_clock_rect( RECT *rect )
{
    rect->right = tray_width - xp_scale( 6 );
    rect->left = rect->right - clock_width;
    rect->top = 0;
    rect->bottom = tray_height;
}

static void xp_draw_clock( HDC hdc )
{
    const WCHAR *lines[3];
    unsigned int i, count = xp_clock_lines( lines );
    HGDIOBJ old_font = SelectObject( hdc, xp_font );
    TEXTMETRICW tm;
    RECT rect, line;
    int top;

    if (!show_clock)
    {
        SelectObject( hdc, old_font );
        return;
    }
    GetTextMetricsW( hdc, &tm );
    get_clock_rect( &rect );
    top = (tray_height - (int)count * tm.tmHeight) / 2;
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, xp_current_scheme()->text );
    for (i = 0; i < count; i++)
    {
        SetRect( &line, rect.left, top + i * tm.tmHeight, rect.right, top + (i + 1) * tm.tmHeight );
        DrawTextW( hdc, lines[i], -1, &line, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX );
    }
    SelectObject( hdc, old_font );
}

/* refresh the clock strings, returns TRUE if the width of the clock changed */
static BOOL xp_update_clock(void)
{
    const WCHAR *lines[3];
    unsigned int i, count;
    int width = 0;
    SYSTEMTIME st;
    HGDIOBJ old_font;
    SIZE size;
    HDC hdc;

    GetLocalTime( &st );
    if (!GetTimeFormatW( LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, clock_time, ARRAY_SIZE(clock_time) ))
        clock_time[0] = 0;
    if (!GetDateFormatW( LOCALE_USER_DEFAULT, 0, &st, L"dddd", clock_day, ARRAY_SIZE(clock_day) ))
        clock_day[0] = 0;
    if (!GetDateFormatW( LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, clock_date, ARRAY_SIZE(clock_date) ))
        clock_date[0] = 0;
    if (!GetDateFormatW( LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, NULL, clock_tip, ARRAY_SIZE(clock_tip) ))
        clock_tip[0] = 0;

    hdc = GetDC( 0 );
    old_font = SelectObject( hdc, xp_font );
    count = xp_clock_lines( lines );
    for (i = 0; i < count; i++)
        if (GetTextExtentPoint32W( hdc, lines[i], lstrlenW( lines[i] ), &size )) width = max( width, size.cx );
    SelectObject( hdc, old_font );
    ReleaseDC( 0, hdc );

    width = show_clock ? width + xp_scale( 8 ) : 0;
    if (width == clock_width) return FALSE;
    clock_width = width;
    return TRUE;
}

static void xp_set_clock_timer(void)
{
    SYSTEMTIME st;

    GetLocalTime( &st );
    /* fire right after the next minute starts */
    SetTimer( tray_window, CLOCK_TIMER, (60 - st.wSecond) * 1000 - st.wMilliseconds + 50, NULL );
}

static void xp_update_clock_tooltip(void)
{
    TTTOOLINFOW ti;

    if (!clock_tooltip) return;
    memset( &ti, 0, sizeof(ti) );
    ti.cbSize = sizeof(ti);
    ti.hwnd = tray_window;
    ti.uId = 1;
    ti.lpszText = clock_tip;
    get_clock_rect( &ti.rect );
    SendMessageW( clock_tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti );
    SendMessageW( clock_tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti );
}

static void xp_create_clock_tooltip(void)
{
    TTTOOLINFOW ti;

    init_common_controls();
    clock_tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                     tray_window, NULL, NULL, NULL );
    if (!clock_tooltip) return;
    memset( &ti, 0, sizeof(ti) );
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = tray_window;
    ti.uId = 1;
    ti.lpszText = clock_tip;
    get_clock_rect( &ti.rect );
    SendMessageW( clock_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti );
}

static POINT xp_get_icon_pos( int display )
{
    int col = display / taskbar_rows, row = display % taskbar_rows;
    POINT pos;

    pos.x = tray_width - xp_scale( 6 ) - clock_width - xp_scale( 2 ) - (col + 1) * icon_cx;
    pos.y = row * row_height + (row_height - icon_cy) / 2;
    return pos;
}

/* width of the notification area, with one row of icons per taskbar row */
static int xp_notify_width(void)
{
    int cols = (nb_displayed + taskbar_rows - 1) / taskbar_rows;

    return xp_scale( 8 ) + cols * icon_cx + xp_scale( 2 ) + clock_width + xp_scale( 6 );
}

static void xp_reposition_icons(void)
{
    struct icon *icon;
    POINT pos;

    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
    {
        if (icon->display < 0) continue;
        pos = xp_get_icon_pos( icon->display );
        SetWindowPos( icon->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
        InvalidateRect( icon->window, NULL, FALSE );
        update_tooltip_position( icon );
    }
}

/* same rules as Windows: visible, and either unowned or with WS_EX_APPWINDOW */
static BOOL xp_is_task_window( HWND hwnd )
{
    DWORD ex_style;

    if (!IsWindowVisible( hwnd )) return FALSE;
    ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    if (ex_style & WS_EX_TOOLWINDOW) return FALSE;
    return !GetWindow( hwnd, GW_OWNER );
}

static void xp_sync_taskbar_buttons(void)
{
    struct taskbar_button *win;
    HWND foreground = GetAncestor( GetForegroundWindow(), GA_ROOTOWNER );
    int spacing = xp_scale( 3 ), margin = xp_scale( 3 ), max_width = xp_scale( 160 ), min_width = xp_scale( 36 );
    int band_left, band_width, per_row, width, count = 0, index = 0, left;

    left = tray_width - xp_notify_width();
    if (left != notify_left)
    {
        notify_left = left;
        InvalidateRect( tray_window, NULL, TRUE );
    }

    band_left = xp_band_left();
    band_width = max( 0, notify_left - xp_scale( 4 ) - band_left );

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!win->hwnd)  /* start button */
        {
            SetWindowPos( win->button, 0, 0, 0, start_button_width, row_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            continue;
        }
        win->active = (win->hwnd == foreground);
        win->visible = xp_is_task_window( win->hwnd );
        if (win->visible) count++;
    }

    /* fill the first row before wrapping, shrink the buttons once all the rows are full */
    per_row = max( 1, (band_width + spacing) / (max_width + spacing) );
    if (count > per_row * taskbar_rows)
    {
        per_row = (count + taskbar_rows - 1) / taskbar_rows;
        if ((band_width + spacing) / per_row - spacing < min_width)
            per_row = max( 1, (band_width + spacing) / (min_width + spacing) );
    }
    width = min( max_width, (band_width + spacing) / per_row - spacing );

    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
    {
        if (!win->hwnd) continue;  /* start button */
        if (win->visible && width > 0 && index < per_row * taskbar_rows)
        {
            SetWindowPos( win->button, 0, band_left + (index % per_row) * (width + spacing),
                          (index / per_row) * row_height + margin, width, row_height - 2 * margin,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
            InvalidateRect( win->button, NULL, FALSE );
            index++;
        }
        else SetWindowPos( win->button, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW );
    }
    xp_update_clock_tooltip();
}

/* Wine can't draw icon handles of other processes, so use the icon of the executable */
static HICON xp_get_task_icon( struct taskbar_button *win )
{
    if (!win->icon_loaded)
    {
        WCHAR path[MAX_PATH];
        DWORD pid = 0, len = ARRAY_SIZE(path);
        HANDLE process;

        win->icon_loaded = TRUE;
        GetWindowThreadProcessId( win->hwnd, &pid );
        if ((process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid )))
        {
            if (QueryFullProcessImageNameW( process, 0, path, &len ))
                ExtractIconExW( path, 0, NULL, &win->icon, 1 );
            CloseHandle( process );
        }
    }
    if (win->icon) return win->icon;
    if (!default_task_icon)
        default_task_icon = LoadImageW( 0, (const WCHAR *)IDI_APPLICATION, IMAGE_ICON,
                                        GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), LR_SHARED );
    return default_task_icon;
}

static void xp_draw_task_button( HDC hdc, struct taskbar_button *win, int width, int height, BOOL down )
{
    const struct xp_scheme *scheme = xp_current_scheme();
    int icon_size = GetSystemMetrics( SM_CXSMICON ), pad = xp_scale( 6 ), shift = down ? 1 : 0;
    int corner = xp_scale( 6 );
    HRGN rgn = CreateRoundRectRgn( 0, 0, width + 1, height + 1, corner, corner );
    WCHAR title[256];
    HGDIOBJ old_font;
    RECT rect;

    SelectClipRgn( hdc, rgn );
    if (down)
    {
        fill_gradient2( hdc, 0, 0, width, height, scheme->down_top, scheme->down_bottom );
        fill_solid( hdc, 0, 0, width, 1, scheme->down_dark );
        fill_solid( hdc, 0, 0, 1, height, scheme->down_dark );
        fill_solid( hdc, 1, 1, width - 1, 1, scheme->down_dark2 );
        fill_solid( hdc, 1, 1, 1, height - 1, scheme->down_dark2 );
    }
    else
    {
        fill_gradient2( hdc, 0, 0, width, height, scheme->button_top, scheme->button_bottom );
        fill_solid( hdc, 0, 0, width, 1, scheme->button_light_top );
        fill_solid( hdc, 0, 0, 1, height, scheme->button_light_left );
        fill_solid( hdc, width - 1, 0, 1, height, scheme->button_dark_right );
        fill_solid( hdc, 0, height - 1, width, 1, scheme->button_dark_bottom );
    }
    SelectClipRgn( hdc, NULL );
    DeleteObject( rgn );

    DrawIconEx( hdc, pad + shift, (height - icon_size) / 2 + shift, xp_get_task_icon( win ),
                icon_size, icon_size, 0, NULL, DI_NORMAL );

    title[0] = 0;
    InternalGetWindowText( win->hwnd, title, ARRAY_SIZE(title) );
    SetRect( &rect, pad + icon_size + xp_scale( 5 ) + shift, shift, width - xp_scale( 5 ) + shift, height + shift );
    old_font = SelectObject( hdc, xp_font );
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, scheme->text );
    DrawTextW( hdc, title, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX );
    SelectObject( hdc, old_font );
}

static void xp_draw_start_button( HDC hdc, int width, int height, BOOL down )
{
    int icon_size = xp_scale( 20 ), x = xp_scale( 8 ), shift = down ? 1 : 0;
    /* square on the left, rounded on the right */
    HRGN rgn = CreateRoundRectRgn( -height, 0, width + 1, height + 1, xp_scale( 24 ), height );
    HBRUSH edge = CreateSolidBrush( RGB(0x1d,0x5a,0x1a) );
    HGDIOBJ old_font;
    RECT rect;

    SelectClipRgn( hdc, rgn );
    if (down)
        fill_gradient( hdc, 0, 0, width, height, 0, height,
                       start_pushed_gradient, ARRAY_SIZE(start_pushed_gradient) );
    else
        fill_gradient( hdc, 0, 0, width, height, 0, height, start_gradient, ARRAY_SIZE(start_gradient) );
    SelectClipRgn( hdc, NULL );
    FrameRgn( hdc, rgn, edge, 1, 1 );
    DeleteObject( edge );
    DeleteObject( rgn );

    if (start_icon)
        DrawIconEx( hdc, x + shift, (height - icon_size) / 2 + shift, start_icon,
                    icon_size, icon_size, 0, NULL, DI_NORMAL );

    old_font = SelectObject( hdc, xp_start_font );
    SetBkMode( hdc, TRANSPARENT );
    SetRect( &rect, x + icon_size + xp_scale( 5 ) + shift + 1, shift + 1, width + 1, height + shift + 1 );
    SetTextColor( hdc, RGB(0x1b,0x4b,0x18) );
    DrawTextW( hdc, start_text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX );
    OffsetRect( &rect, -1, -1 );
    SetTextColor( hdc, RGB(0xff,0xff,0xff) );
    DrawTextW( hdc, start_text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX );
    SelectObject( hdc, old_font );
}

static void xp_paint_taskbar_button( const DRAWITEMSTRUCT *dis, struct taskbar_button *win )
{
    BOOL down = (dis->itemState & ODS_SELECTED) != 0;
    POINT origin = { 0, 0 };
    HGDIOBJ old_bitmap;
    HBITMAP bitmap;
    RECT rect;
    HDC hdc;

    GetClientRect( dis->hwndItem, &rect );
    if (rect.right <= 0 || rect.bottom <= 0) return;
    if (!(hdc = CreateCompatibleDC( dis->hDC ))) return;
    bitmap = CreateCompatibleBitmap( dis->hDC, rect.right, rect.bottom );
    old_bitmap = SelectObject( hdc, bitmap );

    /* rounded corners show the taskbar through */
    MapWindowPoints( dis->hwndItem, tray_window, &origin, 1 );
    xp_draw_background( hdc, 0, 0, origin.x, origin.y, rect.right, rect.bottom );
    if (!win->hwnd) xp_draw_start_button( hdc, rect.right, rect.bottom, down || start_pressed );
    else xp_draw_task_button( hdc, win, rect.right, rect.bottom, down || win->active );

    BitBlt( dis->hDC, 0, 0, rect.right, rect.bottom, hdc, 0, 0, SRCCOPY );
    SelectObject( hdc, old_bitmap );
    DeleteObject( bitmap );
    DeleteDC( hdc );
}

static void xp_paint_tray( HWND hwnd )
{
    PAINTSTRUCT ps;
    HGDIOBJ old_bitmap;
    HBITMAP bitmap;
    int width, height;
    HDC hdc, mem;

    hdc = BeginPaint( hwnd, &ps );
    width = ps.rcPaint.right - ps.rcPaint.left;
    height = ps.rcPaint.bottom - ps.rcPaint.top;
    if (width > 0 && height > 0 && (mem = CreateCompatibleDC( hdc )))
    {
        bitmap = CreateCompatibleBitmap( hdc, width, height );
        old_bitmap = SelectObject( mem, bitmap );
        SetWindowOrgEx( mem, ps.rcPaint.left, ps.rcPaint.top, NULL );
        xp_draw_background( mem, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.left, ps.rcPaint.top, width, height );
        if (!taskbar_locked) xp_draw_gripper( mem, xp_band_left() - xp_scale( 7 ));
        xp_draw_clock( mem );
        BitBlt( hdc, ps.rcPaint.left, ps.rcPaint.top, width, height, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY );
        SelectObject( mem, old_bitmap );
        DeleteObject( bitmap );
        DeleteDC( mem );
    }
    EndPaint( hwnd, &ps );
}

static void xp_update_metrics(void)
{
    UINT dpi = GetDpiForWindow( tray_window );
    TEXTMETRICW tm;
    HGDIOBJ old_font;
    LOGFONTW lf;
    SIZE size;
    HDC hdc;

    taskbar_dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;

    if (xp_font) DeleteObject( xp_font );
    if (xp_start_font) DeleteObject( xp_start_font );
    memset( &lf, 0, sizeof(lf) );
    lf.lfHeight = -xp_scale( 11 );
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyW( lf.lfFaceName, L"Tahoma" );
    xp_font = CreateFontIndirectW( &lf );
    lf.lfHeight = -xp_scale( 15 );
    lf.lfWeight = FW_BOLD;
    lf.lfItalic = TRUE;
    xp_start_font = CreateFontIndirectW( &lf );

    lstrcpynW( start_text, start_label, ARRAY_SIZE(start_text) );
    CharLowerW( start_text );

    hdc = GetDC( 0 );
    old_font = SelectObject( hdc, xp_font );
    GetTextMetricsW( hdc, &tm );
    row_height = max( xp_scale( 30 ), max( icon_cy + xp_scale( 8 ), tm.tmHeight + xp_scale( 12 )));
    SelectObject( hdc, xp_start_font );
    GetTextExtentPoint32W( hdc, start_text, lstrlenW( start_text ), &size );
    start_button_width = xp_scale( 8 + 20 + 5 ) + size.cx + xp_scale( 22 );
    SelectObject( hdc, old_font );
    ReleaseDC( 0, hdc );

    if (start_icon) DestroyIcon( start_icon );
    start_icon = LoadImageW( GetModuleHandleW( NULL ), MAKEINTRESOURCEW( IDI_WINE_LOGO ), IMAGE_ICON,
                             xp_scale( 20 ), xp_scale( 20 ), 0 );
    if (!start_icon)
        start_icon = LoadImageW( 0, (const WCHAR *)IDI_WINLOGO, IMAGE_ICON, xp_scale( 20 ), xp_scale( 20 ), 0 );

    xp_update_clock();
}

static void xp_set_taskbar_rows( int rows )
{
    rows = max( 1, min( rows, TASKBAR_MAX_ROWS ));
    if (rows == taskbar_rows) return;
    taskbar_rows = rows;
    do_show_systray();
}

static void xp_save_setting( const WCHAR *name, DWORD value )
{
    HKEY hkey;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\Explorer\\Taskbar", 0, NULL, 0,
                         KEY_SET_VALUE, NULL, &hkey, NULL )) return;
    RegSetValueExW( hkey, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( hkey );
}

static void xp_load_settings(void)
{
    WCHAR style[16];
    DWORD value, size, len;
    HKEY hkey;

    xp_style = TRUE;
    /* @@ Wine registry key: HKCU\Software\Wine\Explorer\Taskbar */
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\Explorer\\Taskbar", 0, KEY_QUERY_VALUE, &hkey ))
    {
        size = sizeof(style);
        if (!RegGetValueW( hkey, NULL, L"Style", RRF_RT_REG_SZ, NULL, style, &size ) &&
            !lstrcmpiW( style, L"classic" ))
            xp_style = FALSE;
        size = sizeof(value);
        if (!RegGetValueW( hkey, NULL, L"Rows", RRF_RT_REG_DWORD, NULL, &value, &size ))
            taskbar_rows = max( 1, min( (int)value, TASKBAR_MAX_ROWS ));
        size = sizeof(value);
        if (!RegGetValueW( hkey, NULL, L"Locked", RRF_RT_REG_DWORD, NULL, &value, &size ))
            taskbar_locked = value != 0;
        size = sizeof(value);
        if (!RegGetValueW( hkey, NULL, L"Scheme", RRF_RT_REG_DWORD, NULL, &value, &size ))
            taskbar_scheme = value < ARRAY_SIZE(xp_schemes) ? value : 0;
        size = sizeof(value);
        if (!RegGetValueW( hkey, NULL, L"ShowClock", RRF_RT_REG_DWORD, NULL, &value, &size ))
            show_clock = value != 0;
        RegCloseKey( hkey );
    }
    /* WINE_TASKBAR_STYLE=classic brings back the plain Wine taskbar */
    len = GetEnvironmentVariableW( L"WINE_TASKBAR_STYLE", style, ARRAY_SIZE(style) );
    if (len && len < ARRAY_SIZE(style)) xp_style = lstrcmpiW( style, L"classic" ) != 0;
    TRACE( "style %s rows %d locked %d\n", xp_style ? "xp" : "classic", taskbar_rows, taskbar_locked );
}

static void xp_launch_task_manager(void)
{
    WCHAR cmdline[] = L"taskmgr.exe";
    PROCESS_INFORMATION pi;
    STARTUPINFOW si;

    memset( &si, 0, sizeof(si) );
    si.cb = sizeof(si);
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi )) return;
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
}

static void xp_show_taskbar_menu( HWND hwnd, LPARAM lparam )
{
    HMENU menu, size_menu;
    WCHAR str[64];
    POINT pt;
    int i, cmd;

    if (lparam == -1) GetCursorPos( &pt );
    else
    {
        pt.x = (short)LOWORD( lparam );
        pt.y = (short)HIWORD( lparam );
    }
    if (!(menu = CreatePopupMenu())) return;
    if ((size_menu = CreatePopupMenu()))
    {
        for (i = 1; i <= TASKBAR_MAX_ROWS; i++)
        {
            LoadStringW( NULL, IDS_TASKBAR_ROWS1 + i - 1, str, ARRAY_SIZE(str) );
            AppendMenuW( size_menu, MF_STRING | (i == taskbar_rows ? MF_CHECKED : 0) |
                         (taskbar_locked ? MF_GRAYED : 0), IDM_TASKBAR_ROWS + i, str );
        }
        LoadStringW( NULL, IDS_TASKBAR_SIZE, str, ARRAY_SIZE(str) );
        AppendMenuW( menu, MF_POPUP, (UINT_PTR)size_menu, str );
    }
    LoadStringW( NULL, IDS_TASK_MANAGER, str, ARRAY_SIZE(str) );
    AppendMenuW( menu, MF_STRING, IDM_TASK_MANAGER, str );
    AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
    LoadStringW( NULL, IDS_TASKBAR_LOCK, str, ARRAY_SIZE(str) );
    AppendMenuW( menu, MF_STRING | (taskbar_locked ? MF_CHECKED : 0), IDM_TASKBAR_LOCK, str );

    cmd = TrackPopupMenuEx( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                            pt.x, pt.y, hwnd, NULL );
    DestroyMenu( menu );

    if (cmd == IDM_TASK_MANAGER) xp_launch_task_manager();
    else if (cmd == IDM_TASKBAR_LOCK)
    {
        taskbar_locked = !taskbar_locked;
        xp_save_setting( L"Locked", taskbar_locked );
        sync_taskbar_buttons();
        InvalidateRect( tray_window, NULL, TRUE );
    }
    else if (cmd > IDM_TASKBAR_ROWS && cmd <= IDM_TASKBAR_ROWS + TASKBAR_MAX_ROWS)
    {
        xp_set_taskbar_rows( cmd - IDM_TASKBAR_ROWS );
        xp_save_setting( L"Rows", taskbar_rows );
    }
}

/* dragging the taskbar up or down snaps it to a whole number of rows */
static void xp_update_sizing(void)
{
    POINT pt;
    int delta;

    GetCursorPos( &pt );
    delta = sizing_start_y - pt.y;
    if (delta >= 0) xp_set_taskbar_rows( sizing_start_rows + (delta + row_height / 2) / row_height );
    else xp_set_taskbar_rows( sizing_start_rows - (row_height / 2 - delta) / row_height );
}

static void xp_end_sizing(void)
{
    if (!sizing_taskbar) return;
    sizing_taskbar = FALSE;
    KillTimer( tray_window, SIZE_TIMER );
    xp_save_setting( L"Rows", taskbar_rows );
}

/* keep the task buttons in sync with window state and title changes */
static void CALLBACK xp_winevent_proc( HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object_id,
                                       LONG child_id, DWORD thread, DWORD time )
{
    if (!hwnd || object_id != OBJID_WINDOW || child_id != CHILDID_SELF) return;
    if (event != EVENT_SYSTEM_FOREGROUND && !find_taskbar_button( hwnd )) return;
    SetTimer( tray_window, SYNC_TIMER, SYNC_DELAY, NULL );
}

static void xp_init_taskbar(void)
{
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;

    if (xp_initialized)
    {
        /* switching back from the classic style */
        if (clock_tooltip) SendMessageW( clock_tooltip, TTM_ACTIVATE, TRUE, 0 );
        xp_update_clock_tooltip();
        xp_set_clock_timer();
        return;
    }
    xp_initialized = TRUE;
    xp_create_clock_tooltip();
    xp_update_clock_tooltip();
    xp_set_clock_timer();
    SetWinEventHook( EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, xp_winevent_proc,
                     0, 0, WINEVENT_OUTOFCONTEXT );
    SetWinEventHook( EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL, xp_winevent_proc, 0, 0, flags );
    SetWinEventHook( EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, NULL, xp_winevent_proc, 0, 0, flags );
    SetWinEventHook( EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL, xp_winevent_proc, 0, 0, flags );
}

/*
 * Display Properties, opened from the desktop context menu
 */

#define IDM_DESKTOP_REFRESH     0x7101
#define IDM_DESKTOP_PROPERTIES  0x7102

struct display_settings
{
    BOOL xp;
    UINT scheme;
    int  rows;
    BOOL locked;
    BOOL clock;
};

static HWND display_dialog;

static void invalidate_taskbar(void)
{
    struct taskbar_button *win;
    struct icon *icon;

    InvalidateRect( tray_window, NULL, TRUE );
    LIST_FOR_EACH_ENTRY( win, &taskbar_buttons, struct taskbar_button, entry )
        InvalidateRect( win->button, NULL, TRUE );
    LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
    {
        POINT pos;

        if (icon->display < 0) continue;
        pos = get_icon_pos( icon );
        SetWindowPos( icon->window, 0, pos.x, pos.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER );
        InvalidateRect( icon->window, NULL, TRUE );
        update_tooltip_position( icon );
    }
}

/* switch between the Luna and the classic Wine taskbar while running */
static void set_taskbar_style( BOOL xp )
{
    DWORD style = GetWindowLongW( tray_window, GWL_STYLE );

    if (xp == xp_style) return;
    xp_style = xp;
    SetWindowLongW( tray_window, GWL_STYLE, xp ? style | WS_CLIPCHILDREN : style & ~WS_CLIPCHILDREN );
    if (xp) xp_init_taskbar();
    else
    {
        KillTimer( tray_window, CLOCK_TIMER );
        if (clock_tooltip) SendMessageW( clock_tooltip, TTM_ACTIVATE, FALSE, 0 );
    }
    do_show_systray();
}

static void apply_display_settings( const struct display_settings *settings )
{
    HKEY hkey;

    taskbar_scheme = settings->scheme < ARRAY_SIZE(xp_schemes) ? settings->scheme : 0;
    taskbar_rows = max( 1, min( settings->rows, TASKBAR_MAX_ROWS ));
    taskbar_locked = settings->locked;
    show_clock = settings->clock;

    if (!RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\Explorer\\Taskbar", 0, NULL, 0,
                          KEY_SET_VALUE, NULL, &hkey, NULL ))
    {
        const WCHAR *style = settings->xp ? L"xp" : L"classic";

        RegSetValueExW( hkey, L"Style", 0, REG_SZ, (const BYTE *)style, (lstrlenW( style ) + 1) * sizeof(WCHAR) );
        RegCloseKey( hkey );
    }
    xp_save_setting( L"Scheme", taskbar_scheme );
    xp_save_setting( L"Rows", taskbar_rows );
    xp_save_setting( L"Locked", taskbar_locked );
    xp_save_setting( L"ShowClock", show_clock );

    if (settings->xp != xp_style) set_taskbar_style( settings->xp );
    else do_show_systray();
    invalidate_taskbar();
}

static void load_combo_strings( HWND combo, const UINT *ids, unsigned int count, unsigned int selection )
{
    WCHAR str[64], *src, *dst;
    unsigned int i;

    for (i = 0; i < count; i++)
    {
        LoadStringW( NULL, ids[i], str, ARRAY_SIZE(str) );
        /* the menu strings carry accelerator prefixes */
        for (src = dst = str; *src; src++) if (*src != '&') *dst++ = *src;
        *dst = 0;
        SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)str );
    }
    SendMessageW( combo, CB_SETCURSEL, selection, 0 );
}

static void get_dialog_settings( HWND dlg, struct display_settings *settings )
{
    LRESULT sel;

    settings->xp = SendDlgItemMessageW( dlg, IDC_DP_STYLE, CB_GETCURSEL, 0, 0 ) != 1;
    sel = SendDlgItemMessageW( dlg, IDC_DP_SCHEME, CB_GETCURSEL, 0, 0 );
    settings->scheme = sel > 0 ? sel : 0;
    sel = SendDlgItemMessageW( dlg, IDC_DP_ROWS, CB_GETCURSEL, 0, 0 );
    settings->rows = sel >= 0 ? sel + 1 : 1;
    settings->locked = IsDlgButtonChecked( dlg, IDC_DP_LOCK ) == BST_CHECKED;
    settings->clock = IsDlgButtonChecked( dlg, IDC_DP_CLOCK ) == BST_CHECKED;
}

static void update_dialog_state( HWND dlg )
{
    struct display_settings settings;
    UINT ids[] = { IDC_DP_SCHEME, IDC_DP_ROWS, IDC_DP_LOCK, IDC_DP_CLOCK };
    unsigned int i;

    get_dialog_settings( dlg, &settings );
    for (i = 0; i < ARRAY_SIZE(ids); i++) EnableWindow( GetDlgItem( dlg, ids[i] ), settings.xp );
    InvalidateRect( GetDlgItem( dlg, IDC_DP_PREVIEW ), NULL, FALSE );
}

/* a small desktop with the taskbar as it would look with the selected settings */
static void draw_display_preview( const DRAWITEMSTRUCT *dis, const struct display_settings *settings )
{
    const struct xp_scheme *scheme = &xp_schemes[settings->scheme < ARRAY_SIZE(xp_schemes) ? settings->scheme : 0];
    int width = dis->rcItem.right - dis->rcItem.left, height = dis->rcItem.bottom - dis->rcItem.top;
    int bar = max( height / 4, 12 ), top = height - bar, start_width = bar * 3;
    HGDIOBJ old_bitmap, old_font;
    HBITMAP bitmap;
    RECT rect;
    HRGN rgn;
    HDC hdc;

    if (width <= 0 || height <= 0) return;
    hdc = CreateCompatibleDC( dis->hDC );
    bitmap = CreateCompatibleBitmap( dis->hDC, width, height );
    old_bitmap = SelectObject( hdc, bitmap );
    old_font = SelectObject( hdc, xp_font ? xp_font : GetStockObject( DEFAULT_GUI_FONT ));
    SetBkMode( hdc, TRANSPARENT );

    fill_gradient2( hdc, 0, 0, width, top, RGB(0x3a,0x6e,0xd6), RGB(0x6a,0xa2,0xf0) );
    if (settings->xp)
    {
        fill_gradient( hdc, 0, top, width, bar, 0, bar, scheme->bar, scheme->bar_count );
        fill_gradient( hdc, width - bar * 3, top, bar * 3, bar, 0, bar, scheme->notify, scheme->notify_count );
        fill_solid( hdc, width - bar * 3, top, 1, bar, scheme->notify_edge );

        rgn = CreateRoundRectRgn( -bar, top, start_width + 1, height + 1, bar, bar );
        SelectClipRgn( hdc, rgn );
        fill_gradient( hdc, 0, top, start_width, bar, 0, bar, start_gradient, ARRAY_SIZE(start_gradient) );
        SelectClipRgn( hdc, NULL );
        DeleteObject( rgn );

        rgn = CreateRoundRectRgn( start_width + bar / 2, top + 2, start_width + bar * 5, height - 1, 4, 4 );
        SelectClipRgn( hdc, rgn );
        fill_gradient2( hdc, start_width + bar / 2, top + 2, bar * 5, bar - 3, scheme->down_top, scheme->down_bottom );
        SelectClipRgn( hdc, NULL );
        DeleteObject( rgn );

        SetTextColor( hdc, RGB(0xff,0xff,0xff) );
        SetRect( &rect, 0, top, start_width, height );
        DrawTextW( hdc, start_text[0] ? start_text : L"start", -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX );
        if (settings->clock)
        {
            SetTextColor( hdc, scheme->text );
            SetRect( &rect, width - bar * 3, top, width, height );
            DrawTextW( hdc, clock_time, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX );
        }
    }
    else
    {
        fill_solid( hdc, 0, top, width, bar, GetSysColor( COLOR_BTNFACE ));
        SetRect( &rect, 2, top + 2, start_width, height - 2 );
        DrawFrameControl( hdc, &rect, DFC_BUTTON, DFCS_BUTTONPUSH );
        SetTextColor( hdc, GetSysColor( COLOR_BTNTEXT ));
        DrawTextW( hdc, start_label, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX );
    }

    BitBlt( dis->hDC, dis->rcItem.left, dis->rcItem.top, width, height, hdc, 0, 0, SRCCOPY );
    SelectObject( hdc, old_font );
    SelectObject( hdc, old_bitmap );
    DeleteObject( bitmap );
    DeleteDC( hdc );
}

static INT_PTR CALLBACK display_properties_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    static const UINT style_ids[] = { IDS_DP_STYLE_XP, IDS_DP_STYLE_CLASSIC };
    static const UINT scheme_ids[] = { IDS_DP_SCHEME_BLUE, IDS_DP_SCHEME_OLIVE, IDS_DP_SCHEME_SILVER };
    static const UINT rows_ids[] = { IDS_TASKBAR_ROWS1, IDS_TASKBAR_ROWS2, IDS_TASKBAR_ROWS3 };
    struct display_settings settings;

    switch (msg)
    {
    case WM_INITDIALOG:
        display_dialog = dlg;
        load_combo_strings( GetDlgItem( dlg, IDC_DP_STYLE ), style_ids, ARRAY_SIZE(style_ids), xp_style ? 0 : 1 );
        load_combo_strings( GetDlgItem( dlg, IDC_DP_SCHEME ), scheme_ids, ARRAY_SIZE(scheme_ids), get_taskbar_scheme() );
        load_combo_strings( GetDlgItem( dlg, IDC_DP_ROWS ), rows_ids, ARRAY_SIZE(rows_ids), taskbar_rows - 1 );
        CheckDlgButton( dlg, IDC_DP_LOCK, taskbar_locked ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( dlg, IDC_DP_CLOCK, show_clock ? BST_CHECKED : BST_UNCHECKED );
        update_dialog_state( dlg );
        return TRUE;

    case WM_DRAWITEM:
        if (wparam != IDC_DP_PREVIEW) break;
        get_dialog_settings( dlg, &settings );
        draw_display_preview( (const DRAWITEMSTRUCT *)lparam, &settings );
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD( wparam ))
        {
        case IDC_DP_STYLE:
        case IDC_DP_SCHEME:
        case IDC_DP_ROWS:
            if (HIWORD( wparam ) == CBN_SELCHANGE) update_dialog_state( dlg );
            break;
        case IDC_DP_LOCK:
        case IDC_DP_CLOCK:
            update_dialog_state( dlg );
            break;
        case IDC_DP_APPLY:
        case IDOK:
            get_dialog_settings( dlg, &settings );
            apply_display_settings( &settings );
            if (LOWORD( wparam ) == IDOK) EndDialog( dlg, IDOK );
            break;
        case IDCANCEL:
            EndDialog( dlg, IDCANCEL );
            break;
        }
        return TRUE;
    }
    return FALSE;
}

static void show_display_properties( HWND owner )
{
    if (display_dialog)  /* already open */
    {
        SetForegroundWindow( display_dialog );
        return;
    }
    init_common_controls();
    /* no owner: a dialog owned by the desktop window would disable it, and since a window whose
     * parent is the desktop doesn't get an owner, EndDialog would never enable it again */
    DialogBoxParamW( GetModuleHandleW( NULL ), MAKEINTRESOURCEW( IDD_DISPLAY_PROPERTIES ), NULL,
                     display_properties_proc, 0 );
    display_dialog = 0;
    if (owner && !IsWindowEnabled( owner )) EnableWindow( owner, TRUE );
}

/* right click on the desktop */
void show_desktop_menu( HWND hwnd, LPARAM lparam )
{
    WCHAR str[64];
    HMENU menu;
    POINT pt;
    int cmd;

    if (!enable_taskbar || !tray_window) return;
    pt.x = (short)LOWORD( lparam );
    pt.y = (short)HIWORD( lparam );
    ClientToScreen( hwnd, &pt );
    if (!(menu = CreatePopupMenu())) return;
    LoadStringW( NULL, IDS_DESKTOP_REFRESH, str, ARRAY_SIZE(str) );
    AppendMenuW( menu, MF_STRING, IDM_DESKTOP_REFRESH, str );
    AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
    LoadStringW( NULL, IDS_DESKTOP_PROPERTIES, str, ARRAY_SIZE(str) );
    AppendMenuW( menu, MF_STRING, IDM_DESKTOP_PROPERTIES, str );
    cmd = TrackPopupMenuEx( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN, pt.x, pt.y, hwnd, NULL );
    DestroyMenu( menu );

    if (cmd == IDM_DESKTOP_REFRESH) InvalidateRect( hwnd, NULL, TRUE );
    else if (cmd == IDM_DESKTOP_PROPERTIES) show_display_properties( hwnd );
}

static void do_hide_systray(void)
{
    ShowWindow( tray_window, SW_HIDE );
}

static void do_show_systray(void)
{
    SIZE size;
    NONCLIENTMETRICSW ncm;
    HFONT font;
    HDC hdc;

    if (!enable_taskbar)
    {
        size = get_window_size();
        SetWindowPos( tray_window, 0, 0, 0, size.cx, size.cy, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW );
        return;
    }

    if (xp_style)
    {
        xp_update_metrics();
        tray_width = GetSystemMetrics( SM_CXSCREEN );
        tray_height = taskbar_rows * row_height;
        notify_left = -1;
        SetWindowPos( tray_window, 0, 0, GetSystemMetrics( SM_CYSCREEN ) - tray_height,
                      tray_width, tray_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
        xp_reposition_icons();
        sync_taskbar_buttons();
        InvalidateRect( tray_window, NULL, TRUE );
        return;
    }

    hdc = GetDC( 0 );

    ncm.cbSize = sizeof(NONCLIENTMETRICSW);
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &ncm, 0 );
    font = CreateFontIndirectW( &ncm.lfCaptionFont );
    /* FIXME: Implement BCM_GETIDEALSIZE and use that instead. */
    SelectObject( hdc, font );
    GetTextExtentPointA( hdc, "abcdefghijklmnopqrstuvwxyz", 26, &size );
    taskbar_button_width = size.cx;
    GetTextExtentPointW( hdc, start_label, lstrlenW(start_label), &size );
    /* add some margins (FIXME) */
    size.cx += 12 + GetSystemMetrics( SM_CXSMICON );
    size.cy += 4;
    ReleaseDC( 0, hdc );
    DeleteObject( font );

    tray_width = GetSystemMetrics( SM_CXSCREEN );
    tray_height = max( icon_cy, size.cy );
    start_button_width = size.cx;
    SetWindowPos( tray_window, 0, 0, GetSystemMetrics( SM_CYSCREEN ) - tray_height,
                  tray_width, tray_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );
    sync_taskbar_buttons();
}

static LRESULT WINAPI shell_traywnd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_COPYDATA:
        return handle_incoming((HWND)wparam, (COPYDATASTRUCT *)lparam);

    case WM_DISPLAYCHANGE:
        if (!show_systray) do_hide_systray();
        else if (!nb_displayed && !enable_taskbar) do_hide_systray();
        else do_show_systray();
        break;

    case WM_WINDOWPOSCHANGING:
    {
        WINDOWPOS *p = (WINDOWPOS *)lparam;

        if (p->flags & SWP_SHOWWINDOW && (!show_systray || !nb_displayed) && !enable_taskbar)
        {
            TRACE( "WM_WINDOWPOSCHANGING clearing SWP_SHOWWINDOW.\n" );
            p->flags &= ~SWP_SHOWWINDOW;
        }
        break;
    }

    case WM_MOVE:
        update_systray_balloon_position();
        break;

    case WM_CLOSE:
        /* don't destroy the tray window, just hide it */
        ShowWindow( hwnd, SW_HIDE );
        hide_balloon( balloon_icon );
        show_systray = FALSE;
        return 0;

    case WM_DRAWITEM:
        paint_taskbar_button( (const DRAWITEMSTRUCT *)lparam );
        break;

    case WM_PAINT:
        if (!xp_style) return DefWindowProcW( hwnd, msg, wparam, lparam );
        xp_paint_tray( hwnd );
        break;

    case WM_ERASEBKGND:
        if (!xp_style) return DefWindowProcW( hwnd, msg, wparam, lparam );
        return 1;

    case WM_CTLCOLORBTN:
        /* the owner drawn buttons paint their whole area */
        if (!xp_style) return DefWindowProcW( hwnd, msg, wparam, lparam );
        return (LRESULT)GetStockObject( NULL_BRUSH );

    case WM_TIMER:
        switch (wparam)
        {
        case CLOCK_TIMER:
            if (xp_update_clock())
            {
                xp_reposition_icons();
                sync_taskbar_buttons();
                InvalidateRect( hwnd, NULL, TRUE );
            }
            else
            {
                RECT rect;

                get_clock_rect( &rect );
                InvalidateRect( hwnd, &rect, TRUE );
                xp_update_clock_tooltip();
            }
            xp_set_clock_timer();
            break;
        case SYNC_TIMER:
            KillTimer( hwnd, SYNC_TIMER );
            sync_taskbar_buttons();
            break;
        case SIZE_TIMER:
            /* poll, the taskbar never takes the foreground so it can't rely on the capture */
            xp_update_sizing();
            if (!(GetAsyncKeyState( VK_LBUTTON ) & 0x8000)) xp_end_sizing();
            break;
        default:
            return DefWindowProcW( hwnd, msg, wparam, lparam );
        }
        break;

    case WM_LBUTTONDOWN:
        if (xp_style && !taskbar_locked)
        {
            POINT pt;

            GetCursorPos( &pt );
            sizing_taskbar = TRUE;
            sizing_start_y = pt.y;
            sizing_start_rows = taskbar_rows;
            SetTimer( hwnd, SIZE_TIMER, SIZE_POLL_DELAY, NULL );
            break;
        }
        return DefWindowProcW( hwnd, msg, wparam, lparam );

    case WM_MOUSEMOVE:
        if (sizing_taskbar) xp_update_sizing();
        return DefWindowProcW( hwnd, msg, wparam, lparam );

    case WM_LBUTTONUP:
        if (sizing_taskbar)
        {
            xp_update_sizing();
            xp_end_sizing();
            break;
        }
        return DefWindowProcW( hwnd, msg, wparam, lparam );

    case WM_SETCURSOR:
        if (xp_style && !taskbar_locked && (HWND)wparam == hwnd && LOWORD(lparam) == HTCLIENT)
        {
            POINT pt;

            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            if (sizing_taskbar || pt.y < xp_scale( 4 ))
            {
                SetCursor( LoadCursorW( 0, (const WCHAR *)IDC_SIZENS ));
                return TRUE;
            }
        }
        return DefWindowProcW( hwnd, msg, wparam, lparam );

    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED)
        {
            if (LOWORD(wparam) == TRAY_MINIMIZE_ALL || LOWORD(wparam) == TRAY_MINIMIZE_ALL_UNDO)
            {
                FIXME( "Shell command %u is not supported.\n", LOWORD(wparam) );
                break;
            }
            click_taskbar_button( (HWND)lparam );
        }
        break;

    case WM_CONTEXTMENU:
        if (xp_style && (HWND)wparam == hwnd) xp_show_taskbar_menu( hwnd, lparam );
        else show_taskbar_contextmenu( (HWND)wparam, lparam );
        break;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_INITMENUPOPUP:
    case WM_MENUCOMMAND:
        return menu_wndproc(hwnd, msg, wparam, lparam);

    case WM_USER + 0:
        update_systray_balloon_position();
        return 0;

    case WM_USER + 1:
    {
        struct icon *icon;

        LIST_FOR_EACH_ENTRY( icon, &icon_list, struct icon, entry )
        {
            if (!icon->window) continue;
            hide_icon( icon );
            show_icon( icon );
        }

        return 0;
    }

    default:
        return DefWindowProcW( hwnd, msg, wparam, lparam );
    }
    return 0;
}

/* notification posted to the desktop window */
void handle_parent_notify( HWND hwnd, WPARAM wp )
{
    switch (LOWORD(wp))
    {
    case WM_CREATE:
        add_taskbar_button( hwnd );
        break;
    case WM_DESTROY:
        remove_taskbar_button( hwnd );
        cleanup_systray_window( hwnd );
        break;
    }
    sync_taskbar_buttons();
}

/* this function creates the listener window */
void initialize_systray( BOOL arg_using_root, BOOL arg_enable_shell, BOOL arg_show_systray, BOOL arg_no_tray_items )
{
    RECT work_rect, primary_rect, taskbar_rect;

    shell_traywnd_class.hIcon = LoadIconW( 0, (const WCHAR *)IDI_WINLOGO );
    shell_traywnd_class.hCursor = LoadCursorW( 0, (const WCHAR *)IDC_ARROW );
    tray_icon_class.hIcon = shell_traywnd_class.hIcon;
    tray_icon_class.hCursor = shell_traywnd_class.hCursor;

    icon_cx = GetSystemMetrics( SM_CXSMICON ) + 2*ICON_BORDER;
    icon_cy = GetSystemMetrics( SM_CYSMICON ) + 2*ICON_BORDER;

    if (arg_using_root)
    {
        show_systray = arg_show_systray;
        enable_taskbar = FALSE;
        enable_dock = TRUE;
    }
    else
    {
        show_systray = arg_show_systray && !arg_enable_shell;
        enable_taskbar = arg_enable_shell;
        enable_dock = FALSE;
    }

    no_tray_items = arg_no_tray_items;

    /* register the systray listener window class */
    if (!RegisterClassExW( &shell_traywnd_class ))
    {
        ERR( "Could not register SysTray window class\n" );
        return;
    }
    if (!RegisterClassExW( &tray_icon_class ))
    {
        ERR( "Could not register Wine SysTray window classes\n" );
        return;
    }

    if (enable_taskbar)
    {
        xp_load_settings();

        SystemParametersInfoW( SPI_GETWORKAREA, 0, &work_rect, 0 );
        SetRect( &primary_rect, 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
        SubtractRect( &taskbar_rect, &primary_rect, &work_rect );

        tray_window = CreateWindowExW( WS_EX_NOACTIVATE, shell_traywnd_class.lpszClassName, NULL,
                                       WS_POPUP | (xp_style ? WS_CLIPCHILDREN : 0),
                                       taskbar_rect.left, taskbar_rect.top, taskbar_rect.right - taskbar_rect.left,
                                       taskbar_rect.bottom - taskbar_rect.top, 0, 0, 0, 0 );
    }
    else
    {
        SIZE size = get_window_size();
        tray_window = CreateWindowExW( WS_EX_NOACTIVATE, shell_traywnd_class.lpszClassName, L"", WS_CAPTION | WS_SYSMENU,
                                       CW_USEDEFAULT, CW_USEDEFAULT, size.cx, size.cy, 0, 0, 0, 0 );
        NtUserMessageCall( tray_window, WINE_SYSTRAY_DOCK_INIT, 0, 0, NULL, NtUserSystemTrayCall, FALSE );
    }

    if (!tray_window)
    {
        ERR( "Could not create tray window\n" );
        return;
    }

    LoadStringW( NULL, IDS_START_LABEL, start_label, ARRAY_SIZE( start_label ));

    add_taskbar_button( 0 );

    if (enable_taskbar)
    {
        do_show_systray();
        if (xp_style) xp_init_taskbar();
    }
    else do_hide_systray();
}
