/*
 * Dwmapi
 *
 * Copyright 2007 Andras Kovacs
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
 *
 */

#include <stdarg.h>

#include "winternl.h"
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "ntuser.h"
#include "dwmapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwmapi);


/**********************************************************************
 *           DwmIsCompositionEnabled         (DWMAPI.@)
 */
HRESULT WINAPI DwmIsCompositionEnabled(BOOL *enabled)
{
    RTL_OSVERSIONINFOEXW version;

    TRACE("%p\n", enabled);

    if (!enabled)
        return E_INVALIDARG;

    *enabled = FALSE;
    version.dwOSVersionInfoSize = sizeof(version);
    if (!RtlGetVersion(&version))
        *enabled = (version.dwMajorVersion > 6 || (version.dwMajorVersion == 6 && version.dwMinorVersion >= 3));

    return S_OK;
}

/**********************************************************************
 *           DwmEnableComposition         (DWMAPI.102)
 */
HRESULT WINAPI DwmEnableComposition(UINT uCompositionAction)
{
    FIXME("(%d) stub\n", uCompositionAction);

    return S_OK;
}

/**********************************************************************
 *           DwmExtendFrameIntoClientArea    (DWMAPI.@)
 */
HRESULT WINAPI DwmExtendFrameIntoClientArea(HWND hwnd, const MARGINS* margins)
{
    static const WCHAR custom_frame_prop[] =
        {'_','_','w','i','n','e','_','d','w','m','_','c','u','s','t','o','m','_','f','r','a','m','e',0};
    static const WCHAR frameless_window_prop[] =
        {'_','_','w','i','n','e','_','w','i','n','3','2','u','_','f','r','a','m','e','l','e','s','s',0};
    BOOL owns, old;

    TRACE("(%p, %p)\n", hwnd, margins);

    owns = margins && margins->cyTopHeight != 0;
    old = NtUserGetProp(hwnd, custom_frame_prop) != NULL;
    if (owns != old)
    {
        if (owns) NtUserSetProp(hwnd, custom_frame_prop, (HANDLE)1);
        else
        {
            NtUserRemoveProp(hwnd, custom_frame_prop);
            NtUserRemoveProp(hwnd, frameless_window_prop);
        }
        SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    return S_OK;
}

/**********************************************************************
 *           DwmGetColorizationColor      (DWMAPI.@)
 */
HRESULT WINAPI DwmGetColorizationColor(DWORD *colorization, BOOL *opaque_blend)
{
    FIXME("(%p, %p) stub\n", colorization, opaque_blend);

    return E_NOTIMPL;
}

/**********************************************************************
 *        DwmInvalidateIconicBitmaps      (DWMAPI.@)
 */
HRESULT WINAPI DwmInvalidateIconicBitmaps(HWND hwnd)
{
    static BOOL once;

    if (!once++) FIXME("(%p) stub\n", hwnd);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmSetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetWindowAttribute(HWND hwnd, DWORD attributenum, LPCVOID attribute, DWORD size)
{
    static BOOL once;

    if (!once++) FIXME("(%p, %lx, %p, %lx) stub\n", hwnd, attributenum, attribute, size);

    return S_OK;
}

/**********************************************************************
 *           DwmGetGraphicsStreamClient         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamClient(UINT uIndex, UUID *pClientUuid)
{
    FIXME("(%d, %p) stub\n", uIndex, pClientUuid);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmGetTransportAttributes         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetTransportAttributes(BOOL *pfIsRemoting, BOOL *pfIsConnected, DWORD *pDwGeneration)
{
    FIXME("(%p, %p, %p) stub\n", pfIsRemoting, pfIsConnected, pDwGeneration);

    return DWM_E_COMPOSITIONDISABLED;
}

/**********************************************************************
 *           DwmUnregisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmUnregisterThumbnail(HTHUMBNAIL thumbnail)
{
    FIXME("(%p) stub\n", thumbnail);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmEnableMMCSS         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableMMCSS(BOOL enableMMCSS)
{
    FIXME("(%d) stub\n", enableMMCSS);

    return S_OK;
}

/**********************************************************************
 *           DwmGetGraphicsStreamTransformHint         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetGraphicsStreamTransformHint(UINT uIndex, MilMatrix3x2D *pTransform)
{
    FIXME("(%d, %p) stub\n", uIndex, pTransform);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmEnableBlurBehindWindow         (DWMAPI.@)
 */
HRESULT WINAPI DwmEnableBlurBehindWindow(HWND hWnd, const DWM_BLURBEHIND *pBlurBuf)
{
    FIXME("%p %p\n", hWnd, pBlurBuf);

    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmDefWindowProc         (DWMAPI.@)
 */
BOOL WINAPI DwmDefWindowProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam, LRESULT *plResult)
{
    static int i;

    if (!i++) FIXME("stub\n");

    return FALSE;
}

/* Wine has no real DWM cloaking. On Wayland the host composites another process's window.
 * It does not occlude ours even when Wine's flat cross-process list shows it on top.
 * Reporting it cloaked stops an occlusion tracker (such as CEF's) from counting it as an
 * opaque occluder and suspending our rendering. PROTON_CLOAK_OCCLUDERS picks the policy:
 *   off     never cloak (default). stock Windows occlusion.
 *   cover   cloak a foreign window that overlaps one of our own windows. recommended.
 *   fs      cloak only a foreign window covering a whole monitor. legacy heuristic.
 *   always  cloak any foreign visible top-level. blunt fallback. */
enum cloak_mode { CLOAK_OFF, CLOAK_COVER, CLOAK_FS, CLOAK_ALWAYS };

static BOOL token_is( const char *lowered, const char *lit )
{
    while (*lowered && *lit) { if (*lowered != *lit) return FALSE; lowered++; lit++; }
    return !*lowered && !*lit;
}

static enum cloak_mode read_cloak_mode( void )
{
    char buf[16];
    DWORD n, i;

    n = GetEnvironmentVariableA( "PROTON_CLOAK_OCCLUDERS", buf, sizeof(buf) );
    if (!n || n >= sizeof(buf)) return CLOAK_OFF;
    for (i = 0; buf[i]; i++) if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] += 32;

    if (token_is(buf,"off") || token_is(buf,"never") || token_is(buf,"none") ||
        token_is(buf,"stock") || token_is(buf,"disable") || token_is(buf,"0")) return CLOAK_OFF;
    if (token_is(buf,"cover") || token_is(buf,"auto") || token_is(buf,"on") ||
        token_is(buf,"1") || token_is(buf,"true")) return CLOAK_COVER;
    if (token_is(buf,"fs") || token_is(buf,"fullscreen")) return CLOAK_FS;
    if (token_is(buf,"always") || token_is(buf,"all") || token_is(buf,"force")) return CLOAK_ALWAYS;

    FIXME( "unknown PROTON_CLOAK_OCCLUDERS value, using off. valid: off cover fs always.\n" );
    return CLOAK_OFF;
}

static enum cloak_mode cloak_mode( void )
{
    static LONG mode = -1;
    if (mode == -1) InterlockedCompareExchange( &mode, read_cloak_mode(), -1 );
    return mode;
}

/* Foreign opaque top-level gate, shared by every cloak mode. Never our own process: a
 * tracker queries its OWN windows through this path and a self-cloak would hide it from
 * itself. Never the shared desktop. Match the visible-and-opaque test CEF uses so we
 * never cloak a window it skips. */
static BOOL is_foreign_opaque_toplevel( HWND hwnd, DWORD *style_out )
{
    DWORD pid = 0, style, exstyle;

    GetWindowThreadProcessId( hwnd, &pid );
    if (!pid || pid == GetCurrentProcessId()) return FALSE;
    if (hwnd == GetDesktopWindow()) return FALSE;

    style = GetWindowLongW( hwnd, GWL_STYLE );
    if (!(style & WS_VISIBLE) || (style & (WS_MINIMIZE | WS_CHILD))) return FALSE;

    exstyle = GetWindowLongW( hwnd, GWL_EXSTYLE );
    if (exstyle & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) return FALSE;

    if (style_out) *style_out = style;
    return TRUE;
}

struct own_toplevels { RECT rc[64]; UINT count; };

static BOOL CALLBACK collect_own_toplevel( HWND hwnd, LPARAM lparam )
{
    struct own_toplevels *own = (struct own_toplevels *)lparam;
    DWORD pid = 0, style;
    RECT rc;

    GetWindowThreadProcessId( hwnd, &pid );
    if (pid != GetCurrentProcessId()) return TRUE;
    style = GetWindowLongW( hwnd, GWL_STYLE );
    if (!(style & WS_VISIBLE) || (style & WS_MINIMIZE)) return TRUE;
    if (own->count >= sizeof(own->rc) / sizeof(own->rc[0])) return FALSE;
    if (GetWindowRect( hwnd, &rc ) && !IsRectEmpty( &rc )) own->rc[own->count++] = rc;
    return TRUE;
}

/* cover: a foreign opaque top-level overlapping one of our own visible top-levels.
 * Geometric overlap is the real occlusion condition. It catches fullscreen, windowed,
 * borderless and monitor-straddling occluders, never a window on another monitor or
 * beside us. */
static BOOL window_is_covering_occluder( HWND hwnd )
{
    struct own_toplevels own = { 0 };
    RECT rc, isect;
    UINT i;

    if (!is_foreign_opaque_toplevel( hwnd, NULL )) return FALSE;
    if (!GetWindowRect( hwnd, &rc ) || IsRectEmpty( &rc )) return FALSE;

    EnumWindows( collect_own_toplevel, (LPARAM)&own );
    for (i = 0; i < own.count; i++)
        if (IntersectRect( &isect, &rc, &own.rc[i] )) return TRUE;
    return FALSE;
}

/* fs: legacy heuristic, a foreign borderless top-level covering a whole monitor. */
static BOOL window_is_fullscreen_occluder( HWND hwnd )
{
    DWORD style;
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon;
    RECT rc;

    if (!is_foreign_opaque_toplevel( hwnd, &style )) return FALSE;
    if (style & (WS_CAPTION | WS_THICKFRAME)) return FALSE;
    if (!GetWindowRect( hwnd, &rc )) return FALSE;
    if (!(mon = MonitorFromWindow( hwnd, MONITOR_DEFAULTTONULL ))) return FALSE;
    if (!GetMonitorInfoW( mon, &mi )) return FALSE;

    return rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
           rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom;
}

static BOOL window_reports_cloaked( HWND hwnd )
{
    switch (cloak_mode())
    {
    case CLOAK_COVER:  return window_is_covering_occluder( hwnd );
    case CLOAK_FS:     return window_is_fullscreen_occluder( hwnd );
    case CLOAK_ALWAYS: return is_foreign_opaque_toplevel( hwnd, NULL );
    case CLOAK_OFF:
    default:           return FALSE;
    }
}

/**********************************************************************
 *           DwmGetWindowAttribute         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD attribute, PVOID pv_attribute, DWORD size)
{
    BOOL enabled = FALSE;
    HRESULT hr;

    TRACE("(%p %ld %p %ld)\n", hwnd, attribute, pv_attribute, size);

    if (DwmIsCompositionEnabled(&enabled) == S_OK && !enabled)
        return E_HANDLE;
    if (!IsWindow(hwnd))
        return E_HANDLE;
    if (!pv_attribute)
        return E_INVALIDARG;

    switch (attribute) {
    case DWMWA_NCRENDERING_ENABLED:
        if (size < sizeof(BOOL))
            return E_INVALIDARG;

        WARN("DWMWA_NCRENDERING_ENABLED: always returning FALSE.\n");
        *(BOOL*)(pv_attribute) = FALSE;
        hr = S_OK;
        break;

    case DWMWA_EXTENDED_FRAME_BOUNDS:
    {
        RECT *rect = (RECT *)pv_attribute;
        DPI_AWARENESS_CONTEXT context;

        if (size < sizeof(*rect))
            return E_NOT_SUFFICIENT_BUFFER;
        if (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)
            return E_HANDLE;

        /* DWM frame bounds are always in physical coords */
        context = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
        if (GetWindowRect(hwnd, rect))
            hr = S_OK;
        else
            hr = HRESULT_FROM_WIN32(GetLastError());

        SetThreadDpiAwarenessContext(context);
        break;
    }
    case DWMWA_CLOAKED:
        if (size < sizeof(DWORD))
            return E_INVALIDARG;

        if (window_reports_cloaked( hwnd ))
        {
            TRACE("DWMWA_CLOAKED: hwnd %p reported cloaked (mode %d).\n", hwnd, cloak_mode());
            *(DWORD*)(pv_attribute) = DWM_CLOAKED_SHELL;
        }
        else
            *(DWORD*)(pv_attribute) = 0;
        hr = S_OK;
        break;

    default:
        FIXME("attribute %ld not implemented.\n", attribute);
        hr = E_NOTIMPL;
        break;
    }

    return hr;
}

/**********************************************************************
 *           DwmRegisterThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmRegisterThumbnail(HWND dest, HWND src, PHTHUMBNAIL thumbnail_id)
{
    FIXME("(%p %p %p) stub\n", dest, src, thumbnail_id);

    return E_NOTIMPL;
}

static int get_display_frequency(void)
{
    DEVMODEW mode;
    BOOL ret;

    memset(&mode, 0, sizeof(mode));
    mode.dmSize = sizeof(mode);
    ret = EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &mode, 0);
    if (ret && mode.dmFields & DM_DISPLAYFREQUENCY && mode.dmDisplayFrequency)
    {
        return mode.dmDisplayFrequency;
    }
    else
    {
        WARN("Failed to query display frequency, returning a fallback value.\n");
        return 60;
    }
}

/**********************************************************************
 *           DwmGetCompositionTimingInfo         (DWMAPI.@)
 */
HRESULT WINAPI DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO *info)
{
    LARGE_INTEGER performance_frequency, qpc;
    static int i, display_frequency;

    if (!info)
        return E_INVALIDARG;

    if (info->cbSize != sizeof(DWM_TIMING_INFO))
        return MILERR_MISMATCHED_SIZE;

    if(!i++) FIXME("(%p %p)\n", hwnd, info);

    memset(info, 0, info->cbSize);
    info->cbSize = sizeof(DWM_TIMING_INFO);

    display_frequency = get_display_frequency();
    info->rateRefresh.uiNumerator = display_frequency;
    info->rateRefresh.uiDenominator = 1;
    info->rateCompose.uiNumerator = display_frequency;
    info->rateCompose.uiDenominator = 1;

    QueryPerformanceFrequency(&performance_frequency);
    info->qpcRefreshPeriod = performance_frequency.QuadPart / display_frequency;

    QueryPerformanceCounter(&qpc);
    info->qpcVBlank = (qpc.QuadPart / info->qpcRefreshPeriod) * info->qpcRefreshPeriod;

    return S_OK;
}

/**********************************************************************
 *                  DwmFlush              (DWMAPI.@)
 */
HRESULT WINAPI DwmFlush(void)
{
    LARGE_INTEGER qpf, qpc, delay;
    LONG64 qpc_refresh_period;
    int display_frequency;
    static BOOL once;

    if (!once++)
        FIXME("stub.\n");
    else
        TRACE("stub.\n");

    display_frequency = get_display_frequency();
    NtQueryPerformanceCounter(&qpc, &qpf);
    qpc_refresh_period = qpf.QuadPart / display_frequency;
    delay.QuadPart = (qpc.QuadPart - ((qpc.QuadPart + qpc_refresh_period - 1) / qpc_refresh_period) * qpc_refresh_period)
            * 10000000 / qpf.QuadPart;
    NtDelayExecution(FALSE, &delay);

    return S_OK;
}

/**********************************************************************
 *           DwmAttachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmAttachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmDetachMilContent         (DWMAPI.@)
 */
HRESULT WINAPI DwmDetachMilContent(HWND hwnd)
{
    FIXME("(%p) stub\n", hwnd);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmUpdateThumbnailProperties         (DWMAPI.@)
 */
HRESULT WINAPI DwmUpdateThumbnailProperties(HTHUMBNAIL thumbnail, const DWM_THUMBNAIL_PROPERTIES *props)
{
    FIXME("(%p, %p) stub\n", thumbnail, props);
    return E_NOTIMPL;
}

/**********************************************************************
 *           DwmSetPresentParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetPresentParameters(HWND hwnd, DWM_PRESENT_PARAMETERS *params)
{
    FIXME("(%p %p) stub\n", hwnd, params);
    return S_OK;
};

/**********************************************************************
 *           DwmSetIconicLivePreviewBitmap         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicLivePreviewBitmap(HWND hwnd, HBITMAP hbmp, POINT *pos, DWORD flags)
{
    FIXME("(%p %p %p %lx) stub\n", hwnd, hbmp, pos, flags);
    return S_OK;
};

/**********************************************************************
 *           DwmSetIconicThumbnail         (DWMAPI.@)
 */
HRESULT WINAPI DwmSetIconicThumbnail(HWND hwnd, HBITMAP hbmp, DWORD flags)
{
    FIXME("(%p %p %lx) stub\n", hwnd, hbmp, flags);
    return S_OK;
};

/**********************************************************************
 *           DwmpGetColorizationParameters         (DWMAPI.@)
 */
HRESULT WINAPI DwmpGetColorizationParameters(void *params)
{
    FIXME("(%p) stub\n", params);
    return E_NOTIMPL;
}
