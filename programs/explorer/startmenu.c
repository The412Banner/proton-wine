/*
 * Copyright (C) 2008 Vincent Povirk
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
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include "wine/debug.h"
#include "wine/list.h"
#include "explorer_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(explorer);

struct menu_item
{
    struct list entry;
    LPWSTR displayname;

    /* parent information */
    struct menu_item* parent;
    LPITEMIDLIST pidl; /* relative to parent; absolute if parent->pidl is NULL */

    /* folder information */
    IShellFolder* folder;
    struct menu_item* base;
    HMENU menuhandle;
    BOOL menu_filled;
};

static struct list items = LIST_INIT(items);

static struct menu_item root_menu;
static struct menu_item public_startmenu;
static struct menu_item user_startmenu;

#define MENU_ID_RUN 1
#define MENU_ID_EXIT 2

static ULONG copy_pidls(struct menu_item* item, LPITEMIDLIST dest)
{
    ULONG item_size;
    ULONG bytes_copied = 2;

    if (item->parent->pidl)
    {
        bytes_copied = copy_pidls(item->parent, dest);
    }

    item_size = ILGetSize(item->pidl);

    if (dest)
        memcpy(((char*)dest) + bytes_copied - 2, item->pidl, item_size);

    return bytes_copied + item_size - 2;
}

static LPITEMIDLIST build_pidl(struct menu_item* item)
{
    ULONG length;
    LPITEMIDLIST result;

    length = copy_pidls(item, NULL);

    result = CoTaskMemAlloc(length);

    copy_pidls(item, result);

    return result;
}

static void exec_item(struct menu_item* item)
{
    LPITEMIDLIST abs_pidl;
    SHELLEXECUTEINFOW sei;

    abs_pidl = build_pidl(item);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_IDLIST;
    sei.nShow = SW_SHOWNORMAL;
    sei.lpIDList = abs_pidl;

    ShellExecuteExW(&sei);

    CoTaskMemFree(abs_pidl);
}

static HRESULT pidl_to_shellfolder(LPITEMIDLIST pidl, LPWSTR *displayname, IShellFolder **out_folder)
{
    IShellFolder* parent_folder=NULL;
    LPCITEMIDLIST relative_pidl=NULL;
    STRRET strret;
    HRESULT hr;

    hr = SHBindToParent(pidl, &IID_IShellFolder, (void**)&parent_folder, &relative_pidl);

    if (displayname)
    {
        if (SUCCEEDED(hr))
            hr = IShellFolder_GetDisplayNameOf(parent_folder, relative_pidl, SHGDN_INFOLDER, &strret);

        if (SUCCEEDED(hr))
            hr = StrRetToStrW(&strret, NULL, displayname);
    }

    if (SUCCEEDED(hr))
        hr = IShellFolder_BindToObject(parent_folder, relative_pidl, NULL, &IID_IShellFolder, (void**)out_folder);

    if (parent_folder)
        IShellFolder_Release(parent_folder);

    return hr;
}

static BOOL shell_folder_is_empty(IShellFolder* folder)
{
    IEnumIDList* enumidl;
    LPITEMIDLIST pidl=NULL;

    if (IShellFolder_EnumObjects(folder, NULL, SHCONTF_NONFOLDERS, &enumidl) == S_OK)
    {
        if (IEnumIDList_Next(enumidl, 1, &pidl, NULL) == S_OK)
        {
            CoTaskMemFree(pidl);
            IEnumIDList_Release(enumidl);
            return FALSE;
        }

        IEnumIDList_Release(enumidl);
    }

    if (IShellFolder_EnumObjects(folder, NULL, SHCONTF_FOLDERS, &enumidl) == S_OK)
    {
        BOOL found = FALSE;
        IShellFolder *child_folder;

        while (!found && IEnumIDList_Next(enumidl, 1, &pidl, NULL) == S_OK)
        {
            if (IShellFolder_BindToObject(folder, pidl, NULL, &IID_IShellFolder, (void *)&child_folder) == S_OK)
            {
                if (!shell_folder_is_empty(child_folder))
                    found = TRUE;

                IShellFolder_Release(child_folder);
            }

            CoTaskMemFree(pidl);
        }

        IEnumIDList_Release(enumidl);

        if (found)
            return FALSE;
    }

    return TRUE;
}

/* add an individual file or folder to the menu, takes ownership of pidl */
static struct menu_item* add_shell_item(struct menu_item* parent, LPITEMIDLIST pidl)
{
    struct menu_item* item;
    MENUITEMINFOW mii;
    HMENU parent_menu;
    int existing_item_count, i;
    BOOL match = FALSE;
    SFGAOF flags;

    item = calloc( 1, sizeof(struct menu_item) );

    if (parent->pidl == NULL)
    {
        pidl_to_shellfolder(pidl, &item->displayname, &item->folder);
    }
    else
    {
        STRRET strret;

        if (SUCCEEDED(IShellFolder_GetDisplayNameOf(parent->folder, pidl, SHGDN_INFOLDER, &strret)))
            StrRetToStrW(&strret, NULL, &item->displayname);

        flags = SFGAO_FOLDER;
        IShellFolder_GetAttributesOf(parent->folder, 1, (LPCITEMIDLIST*)&pidl, &flags);

        if (flags & SFGAO_FOLDER)
            IShellFolder_BindToObject(parent->folder, pidl, NULL, &IID_IShellFolder, (void *)&item->folder);
    }

    if (item->folder && shell_folder_is_empty(item->folder))
    {
        IShellFolder_Release(item->folder);
        free( item->displayname );
        free( item );
        CoTaskMemFree(pidl);
        return NULL;
    }

    parent_menu = parent->menuhandle;

    item->parent = parent;
    item->pidl = pidl;

    existing_item_count = GetMenuItemCount(parent_menu);
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_SUBMENU|MIIM_DATA;

    /* search for an existing menu item with this name or the spot to insert this item */
    if (parent->pidl != NULL)
    {
        for (i=0; i<existing_item_count; i++)
        {
            struct menu_item* existing_item;
            int cmp;

            GetMenuItemInfoW(parent_menu, i, TRUE, &mii);
            existing_item = ((struct menu_item*)mii.dwItemData);

            if (!existing_item)
                continue;

            /* folders before files */
            if (existing_item->folder && !item->folder)
                continue;
            if (!existing_item->folder && item->folder)
                break;

            cmp = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, item->displayname, -1, existing_item->displayname, -1);

            if (cmp == CSTR_LESS_THAN)
                break;

            if (cmp == CSTR_EQUAL)
            {
                match = TRUE;
                break;
            }
        }
    }
    else
        /* This item manually added to the root menu, so put it at the end */
        i = existing_item_count;

    if (!match)
    {
        /* no existing item with the same name; just add it */
        mii.fMask = MIIM_STRING|MIIM_DATA;
        mii.dwTypeData = item->displayname;
        mii.dwItemData = (ULONG_PTR)item;

        if (item->folder)
        {
            MENUINFO mi;
            item->menuhandle = CreatePopupMenu();
            mii.fMask |= MIIM_SUBMENU;
            mii.hSubMenu = item->menuhandle;

            mi.cbSize = sizeof(mi);
            mi.fMask = MIM_MENUDATA;
            mi.dwMenuData = (ULONG_PTR)item;
            SetMenuInfo(item->menuhandle, &mi);
        }

        InsertMenuItemW(parent->menuhandle, i, TRUE, &mii);

        list_add_tail(&items, &item->entry);
    }
    else if (item->folder)
    {
        /* there is an existing folder with the same name, combine them */
        MENUINFO mi;

        item->base = (struct menu_item*)mii.dwItemData;
        item->menuhandle = item->base->menuhandle;

        mii.dwItemData = (ULONG_PTR)item;
        SetMenuItemInfoW(parent_menu, i, TRUE, &mii);

        mi.cbSize = sizeof(mi);
        mi.fMask = MIM_MENUDATA;
        mi.dwMenuData = (ULONG_PTR)item;
        SetMenuInfo(item->menuhandle, &mi);

        list_add_tail(&items, &item->entry);
    }
    else {
        /* duplicate shortcut, do nothing */
        free( item->displayname );
        free( item );
        CoTaskMemFree(pidl);
        item = NULL;
    }

    return item;
}

static void add_folder_contents(struct menu_item* parent)
{
    IEnumIDList* enumidl;

    if (IShellFolder_EnumObjects(parent->folder, NULL,
        SHCONTF_FOLDERS|SHCONTF_NONFOLDERS, &enumidl) == S_OK)
    {
        LPITEMIDLIST rel_pidl=NULL;
        while (S_OK == IEnumIDList_Next(enumidl, 1, &rel_pidl, NULL))
        {
            add_shell_item(parent, rel_pidl);
        }

        IEnumIDList_Release(enumidl);
    }
}

static void destroy_menus(void)
{
    if (!root_menu.menuhandle)
        return;

    DestroyMenu(root_menu.menuhandle);
    root_menu.menuhandle = NULL;

    while (!list_empty(&items))
    {
        struct menu_item* item;

        item = LIST_ENTRY(list_head(&items), struct menu_item, entry);

        if (item->folder)
            IShellFolder_Release(item->folder);

        CoTaskMemFree(item->pidl);
        CoTaskMemFree(item->displayname);

        list_remove(&item->entry);
        free( item );
    }
}

static void fill_menu(struct menu_item* item)
{
    if (!item->menu_filled)
    {
        add_folder_contents(item);

        if (item->base)
        {
            fill_menu(item->base);
        }

        item->menu_filled = TRUE;
    }
}

static void run_dialog(void)
{
    void (WINAPI *pRunFileDlg)(HWND owner, HICON icon, const char *dir,
                               const char *title, const char *desc, DWORD flags);
    HMODULE hShell32;

    hShell32 = LoadLibraryW(L"shell32");
    pRunFileDlg = (void*)GetProcAddress(hShell32, (LPCSTR)61);

    pRunFileDlg(NULL, NULL, NULL, NULL, NULL, 0);

    FreeLibrary(hShell32);
}

static void shut_down(HWND hwnd)
{
    WCHAR prompt[256];
    int ret;

    LoadStringW(NULL, IDS_EXIT_PROMPT, prompt, ARRAY_SIZE(prompt));
    ret = MessageBoxW(hwnd, prompt, L"Wine", MB_YESNO|MB_ICONQUESTION|MB_SYSTEMMODAL);
    if (ret == IDYES)
        ExitWindows(0, 0);
}

LRESULT menu_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_INITMENUPOPUP:
        {
            HMENU hmenu = (HMENU)wparam;
            struct menu_item* item;
            MENUINFO mi;

            mi.cbSize = sizeof(mi);
            mi.fMask = MIM_MENUDATA;
            GetMenuInfo(hmenu, &mi);
            item = (struct menu_item*)mi.dwMenuData;

            if (item)
                fill_menu(item);
            return 0;
        }
        break;

    case WM_MENUCOMMAND:
        {
            HMENU hmenu = (HMENU)lparam;
            struct menu_item* item;
            MENUITEMINFOW mii;

            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_DATA|MIIM_ID;
            GetMenuItemInfoW(hmenu, wparam, TRUE, &mii);
            item = (struct menu_item*)mii.dwItemData;

            if (item)
                exec_item(item);
            else if (mii.wID == MENU_ID_RUN)
                run_dialog();
            else if (mii.wID == MENU_ID_EXIT)
                shut_down(hwnd);

            destroy_menus();

            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* create the root menu, and fill it with the user and common start menus;
 * returns FALSE if the menu couldn't be created */
static BOOL create_root_menu(BOOL *has_items)
{
    destroy_menus();

    TRACE( "creating start menu\n" );

    root_menu.menuhandle = public_startmenu.menuhandle = user_startmenu.menuhandle = CreatePopupMenu();
    if (!root_menu.menuhandle)
    {
        return FALSE;
    }

    user_startmenu.parent = public_startmenu.parent = &root_menu;
    user_startmenu.base = &public_startmenu;
    user_startmenu.menu_filled = public_startmenu.menu_filled = FALSE;

    if (!user_startmenu.pidl)
        SHGetSpecialFolderLocation(NULL, CSIDL_STARTMENU, &user_startmenu.pidl);

    if (!user_startmenu.folder)
        pidl_to_shellfolder(user_startmenu.pidl, NULL, &user_startmenu.folder);

    if (!public_startmenu.pidl)
        SHGetSpecialFolderLocation(NULL, CSIDL_COMMON_STARTMENU, &public_startmenu.pidl);

    if (!public_startmenu.folder)
        pidl_to_shellfolder(public_startmenu.pidl, NULL, &public_startmenu.folder);

    *has_items = (user_startmenu.folder && !shell_folder_is_empty(user_startmenu.folder)) ||
                 (public_startmenu.folder && !shell_folder_is_empty(public_startmenu.folder));
    if (*has_items)
        fill_menu(&user_startmenu);

    return TRUE;
}

void do_startmenu(HWND hwnd)
{
    LPITEMIDLIST pidl;
    MENUINFO mi;
    MENUITEMINFOW mii;
    RECT rc={0,0,0,0};
    TPMPARAMS tpm;
    WCHAR label[64];
    BOOL has_items;

    if (!create_root_menu(&has_items))
        return;

    if (has_items)
        AppendMenuW(root_menu.menuhandle, MF_SEPARATOR, 0, NULL);

    if (SUCCEEDED(SHGetSpecialFolderLocation(NULL, CSIDL_CONTROLS, &pidl)))
        add_shell_item(&root_menu, pidl);

    LoadStringW(NULL, IDS_RUN, label, ARRAY_SIZE(label));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING|MIIM_ID;
    mii.dwTypeData = label;
    mii.wID = MENU_ID_RUN;
    InsertMenuItemW(root_menu.menuhandle, -1, TRUE, &mii);

    mii.fMask = MIIM_FTYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemW(root_menu.menuhandle, -1, TRUE, &mii);

    LoadStringW(NULL, IDS_EXIT_LABEL, label, ARRAY_SIZE(label));
    mii.fMask = MIIM_STRING|MIIM_ID;
    mii.dwTypeData = label;
    mii.wID = MENU_ID_EXIT;
    InsertMenuItemW(root_menu.menuhandle, -1, TRUE, &mii);

    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_NOTIFYBYPOS;
    SetMenuInfo(root_menu.menuhandle, &mi);

    GetWindowRect(hwnd, &rc);

    tpm.cbSize = sizeof(tpm);
    tpm.rcExclude = rc;

    if (!TrackPopupMenuEx(root_menu.menuhandle,
        TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_VERTICAL,
        rc.left, rc.top, hwnd, &tpm))
    {
        ERR( "couldn't display menu\n" );
    }
}

/*
 * Luna ("Windows XP") start menu, used with the Luna taskbar
 */

#define XP_MAX_ITEMS     32
#define XP_MAX_RECENT    6
#define XP_MAX_SHORTCUTS 64
#define IDI_SHELL_RUN_ID 25   /* "Run" icon in shell32 */

enum xp_action
{
    XP_ACTION_NONE,
    XP_ACTION_SEPARATOR,
    XP_ACTION_PROGRAM,       /* ShellExecute the target */
    XP_ACTION_FOLDER,        /* open the special folder csidl */
    XP_ACTION_ALL_PROGRAMS,
    XP_ACTION_RUN,
    XP_ACTION_TURN_OFF,
};

struct xp_item
{
    enum xp_action action;
    BOOL  right;             /* in the right column */
    BOOL  bold;
    int   csidl;
    RECT  rect;
    HICON icon;
    WCHAR text[MAX_PATH];
    WCHAR target[MAX_PATH];
};

struct xp_stop
{
    BYTE     pos;            /* in percent of the height */
    COLORREF color;
};

struct xp_menu_scheme
{
    const struct xp_stop *header;
    unsigned int          header_count;
    const struct xp_stop *footer;
    unsigned int          footer_count;
    COLORREF name, name_shadow, footer_text;
    COLORREF right_back, divider, right_text, right_separator, hot, frame;
};

static const struct xp_stop blue_header[] =
{
    {   0, RGB(0x18,0x68,0xce) }, {  12, RGB(0x0e,0x60,0xcb) }, {  20, RGB(0x0e,0x60,0xcb) },
    {  32, RGB(0x11,0x64,0xcf) }, {  33, RGB(0x16,0x67,0xcf) }, {  47, RGB(0x1b,0x6c,0xd3) },
    {  54, RGB(0x1e,0x70,0xd9) }, {  60, RGB(0x24,0x76,0xdc) }, {  65, RGB(0x29,0x7a,0xe0) },
    {  77, RGB(0x34,0x82,0xe3) }, {  79, RGB(0x37,0x86,0xe5) }, {  90, RGB(0x42,0x8e,0xe9) },
    { 100, RGB(0x47,0x91,0xeb) },
};

static const struct xp_stop blue_footer[] =
{
    {   0, RGB(0x42,0x82,0xd6) }, {   3, RGB(0x3b,0x85,0xe0) }, {   5, RGB(0x41,0x8a,0xe3) },
    {  17, RGB(0x41,0x8a,0xe3) }, {  21, RGB(0x3c,0x87,0xe2) }, {  26, RGB(0x37,0x86,0xe4) },
    {  29, RGB(0x34,0x82,0xe3) }, {  39, RGB(0x2e,0x7e,0xe1) }, {  49, RGB(0x23,0x74,0xdf) },
    {  57, RGB(0x20,0x72,0xdb) }, {  62, RGB(0x19,0x6e,0xdb) }, {  72, RGB(0x17,0x6b,0xd8) },
    {  75, RGB(0x14,0x68,0xd6) }, {  83, RGB(0x11,0x65,0xd4) }, {  88, RGB(0x0f,0x61,0xcb) },
    { 100, RGB(0x0f,0x61,0xcb) },
};

static const struct xp_stop olive_header[] =
{
    {   0, RGB(0x8e,0xa5,0x5f) }, {  12, RGB(0x7f,0x9a,0x4f) }, {  20, RGB(0x7f,0x9a,0x4f) },
    {  47, RGB(0x8c,0xa6,0x5c) }, {  65, RGB(0x96,0xaf,0x66) }, {  90, RGB(0xa5,0xbc,0x75) },
    { 100, RGB(0xa9,0xc0,0x79) },
};

static const struct xp_stop olive_footer[] =
{
    {   0, RGB(0xa4,0xba,0x76) }, {  17, RGB(0x9d,0xb4,0x70) }, {  49, RGB(0x8c,0xa4,0x5e) },
    {  88, RGB(0x7b,0x93,0x4e) }, { 100, RGB(0x7b,0x93,0x4e) },
};

static const struct xp_stop silver_header[] =
{
    {   0, RGB(0xe4,0xe4,0xee) }, {  12, RGB(0xd0,0xd0,0xde) }, {  20, RGB(0xd0,0xd0,0xde) },
    {  47, RGB(0xd6,0xd6,0xe2) }, {  65, RGB(0xdd,0xdd,0xe8) }, {  90, RGB(0xe8,0xe8,0xef) },
    { 100, RGB(0xec,0xec,0xf2) },
};

static const struct xp_stop silver_footer[] =
{
    {   0, RGB(0xe8,0xe8,0xef) }, {  17, RGB(0xde,0xde,0xe8) }, {  49, RGB(0xcf,0xcf,0xdc) },
    {  88, RGB(0xbc,0xbc,0xcd) }, { 100, RGB(0xbc,0xbc,0xcd) },
};

static const struct xp_menu_scheme xp_menu_schemes[] =
{
    {
        blue_header, ARRAY_SIZE(blue_header), blue_footer, ARRAY_SIZE(blue_footer),
        RGB(0xff,0xff,0xff), RGB(0x0d,0x2a,0x6a), RGB(0xff,0xff,0xff),
        RGB(0xd3,0xe5,0xfa), RGB(0x95,0xbd,0xee), RGB(0x00,0x13,0x6b), RGB(0x87,0xb3,0xe2),
        RGB(0x31,0x6a,0xc5), RGB(0x1c,0x4c,0xc0),
    },
    {
        olive_header, ARRAY_SIZE(olive_header), olive_footer, ARRAY_SIZE(olive_footer),
        RGB(0xff,0xff,0xff), RGB(0x3b,0x4a,0x1c), RGB(0xff,0xff,0xff),
        RGB(0xe8,0xec,0xd6), RGB(0xb5,0xc3,0x96), RGB(0x37,0x43,0x1c), RGB(0xae,0xbd,0x8a),
        RGB(0x93,0xa5,0x68), RGB(0x6f,0x84,0x46),
    },
    {
        silver_header, ARRAY_SIZE(silver_header), silver_footer, ARRAY_SIZE(silver_footer),
        RGB(0x1c,0x1c,0x3c), RGB(0xff,0xff,0xff), RGB(0x1c,0x1c,0x3c),
        RGB(0xec,0xec,0xf2), RGB(0xbc,0xbc,0xcc), RGB(0x1c,0x1c,0x3c), RGB(0xb4,0xb4,0xc8),
        RGB(0x9d,0x9d,0xbd), RGB(0x8a,0x8a,0xa4),
    },
};

static struct xp_item xp_items[XP_MAX_ITEMS];
static struct xp_item xp_shortcuts[XP_MAX_SHORTCUTS];
static unsigned int xp_count;
static int xp_hot = -1, xp_selected = -1;
static BOOL xp_done, xp_in_submenu;
static UINT xp_dpi = USER_DEFAULT_SCREEN_DPI;
static HWND xp_window, xp_tray;
static HFONT xp_font, xp_bold_font, xp_title_font;
static HICON xp_logo;
static WCHAR xp_user[64];
static int xp_header_height, xp_footer_top, xp_split;
static const struct xp_menu_scheme *xp_scheme = &xp_menu_schemes[0];

static int xp_px( int size )
{
    return MulDiv( size, xp_dpi, USER_DEFAULT_SCREEN_DPI );
}

static COLORREF xp_blend( COLORREF c1, COLORREF c2, int num, int den )
{
    if (den <= 0) return c1;
    num = max( 0, min( num, den ));
    return RGB( GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * num / den,
                GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * num / den,
                GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * num / den );
}

static void xp_fill( HDC hdc, int x, int y, int width, int height, COLORREF color )
{
    HGDIOBJ old_brush = SelectObject( hdc, GetStockObject( DC_BRUSH ));

    SetDCBrushColor( hdc, color );
    PatBlt( hdc, x, y, width, height, PATCOPY );
    SelectObject( hdc, old_brush );
}

static void xp_vgradient( HDC hdc, int x, int y, int width, int height,
                          const struct xp_stop *stops, unsigned int count )
{
    HGDIOBJ old_brush = SelectObject( hdc, GetStockObject( DC_BRUSH ));
    unsigned int j;
    int i;

    for (i = 0; i < height; i++)
    {
        int pos = i * 1000 / max( height - 1, 1 );
        COLORREF color = stops[count - 1].color;

        for (j = 1; j < count; j++)
        {
            if (pos > stops[j].pos * 10) continue;
            color = xp_blend( stops[j - 1].color, stops[j].color, pos - stops[j - 1].pos * 10,
                              (stops[j].pos - stops[j - 1].pos) * 10 );
            break;
        }
        SetDCBrushColor( hdc, color );
        PatBlt( hdc, x, y + i, width, 1, PATCOPY );
    }
    SelectObject( hdc, old_brush );
}

static void xp_vgradient2( HDC hdc, int x, int y, int width, int height, COLORREF top, COLORREF bottom )
{
    const struct xp_stop stops[] = { { 0, top }, { 100, bottom } };

    xp_vgradient( hdc, x, y, width, height, stops, ARRAY_SIZE(stops) );
}

/* a horizontal line fading from the edges into the middle color */
static void xp_fade_line( HDC hdc, int x, int y, int width, int height, COLORREF edge, COLORREF middle )
{
    HGDIOBJ old_brush = SelectObject( hdc, GetStockObject( DC_BRUSH ));
    int i, half = max( width / 2, 1 );

    for (i = 0; i < width; i++)
    {
        SetDCBrushColor( hdc, xp_blend( edge, middle, i < half ? i : width - 1 - i, half ));
        PatBlt( hdc, x + i, y, 1, height, PATCOPY );
    }
    SelectObject( hdc, old_brush );
}

static HICON xp_icon_from_path( const WCHAR *path )
{
    SHFILEINFOW info;

    if (!SHGetFileInfoW( path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON )) return NULL;
    return info.hIcon;
}

static HICON xp_icon_from_csidl( int csidl )
{
    SHFILEINFOW info;
    LPITEMIDLIST pidl;
    HICON icon = NULL;

    if (FAILED(SHGetSpecialFolderLocation( NULL, csidl, &pidl ))) return NULL;
    if (SHGetFileInfoW( (const WCHAR *)pidl, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON ))
        icon = info.hIcon;
    CoTaskMemFree( pidl );
    return icon;
}

static BOOL xp_find_program( const WCHAR *name, WCHAR *path )
{
    return SearchPathW( NULL, name, NULL, MAX_PATH, path, NULL ) != 0;
}

static struct xp_item *xp_add( enum xp_action action, BOOL right, BOOL bold, UINT string_id )
{
    struct xp_item *item;

    if (xp_count >= ARRAY_SIZE(xp_items)) return NULL;
    item = &xp_items[xp_count++];
    memset( item, 0, sizeof(*item) );
    item->action = action;
    item->right = right;
    item->bold = bold;
    if (string_id) LoadStringW( NULL, string_id, item->text, ARRAY_SIZE(item->text) );
    return item;
}

static void xp_add_program( BOOL right, BOOL bold, UINT string_id, const WCHAR *path, HICON icon )
{
    struct xp_item *item;

    if (!(item = xp_add( XP_ACTION_PROGRAM, right, bold, string_id )))
    {
        if (icon) DestroyIcon( icon );
        return;
    }
    lstrcpynW( item->target, path, ARRAY_SIZE(item->target) );
    item->icon = icon ? icon : xp_icon_from_path( path );
}

static void xp_add_system_program( BOOL right, BOOL bold, UINT string_id, const WCHAR *name )
{
    WCHAR path[MAX_PATH];

    if (xp_find_program( name, path )) xp_add_program( right, bold, string_id, path, NULL );
}

static void xp_add_folder( BOOL bold, UINT string_id, int csidl )
{
    struct xp_item *item;

    if (!(item = xp_add( XP_ACTION_FOLDER, TRUE, bold, string_id ))) return;
    item->csidl = csidl;
    item->icon = xp_icon_from_csidl( csidl );
}

static int __cdecl xp_compare_items( const void *a, const void *b )
{
    return lstrcmpiW( ((const struct xp_item *)a)->text, ((const struct xp_item *)b)->text );
}

/* gather the shortcuts at the top of a start menu folder */
static void xp_collect_shortcuts( unsigned int *count, int csidl )
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    unsigned int i;
    HANDLE handle;

    if (FAILED(SHGetFolderPathW( NULL, csidl, NULL, SHGFP_TYPE_CURRENT, dir ))) return;
    if (lstrlenW( dir ) + 7 >= MAX_PATH) return;
    lstrcpyW( pattern, dir );
    lstrcatW( pattern, L"\\*.lnk" );
    if ((handle = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE) return;
    do
    {
        struct xp_item *item;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (*count >= ARRAY_SIZE(xp_shortcuts)) break;
        if (lstrlenW( dir ) + 1 + lstrlenW( data.cFileName ) >= MAX_PATH) continue;
        item = &xp_shortcuts[*count];
        memset( item, 0, sizeof(*item) );
        lstrcpynW( item->text, data.cFileName, ARRAY_SIZE(item->text) );
        PathRemoveExtensionW( item->text );
        /* the same shortcut can be in the user and in the common start menu */
        for (i = 0; i < *count; i++) if (!lstrcmpiW( xp_shortcuts[i].text, item->text )) break;
        if (i < *count) continue;
        lstrcpyW( item->target, dir );
        lstrcatW( item->target, L"\\" );
        lstrcatW( item->target, data.cFileName );
        (*count)++;
    } while (FindNextFileW( handle, &data ));
    FindClose( handle );
}

static void xp_add_shortcuts(void)
{
    unsigned int i, count = 0;

    xp_collect_shortcuts( &count, CSIDL_STARTMENU );
    xp_collect_shortcuts( &count, CSIDL_COMMON_STARTMENU );
    xp_collect_shortcuts( &count, CSIDL_PROGRAMS );
    xp_collect_shortcuts( &count, CSIDL_COMMON_PROGRAMS );
    if (!count) return;
    qsort( xp_shortcuts, count, sizeof(xp_shortcuts[0]), xp_compare_items );

    xp_add( XP_ACTION_SEPARATOR, FALSE, FALSE, 0 );
    for (i = 0; i < count && i < XP_MAX_RECENT; i++)
    {
        struct xp_item *item = xp_add( XP_ACTION_PROGRAM, FALSE, FALSE, 0 );

        if (!item) break;
        lstrcpyW( item->text, xp_shortcuts[i].text );
        lstrcpyW( item->target, xp_shortcuts[i].target );
        item->icon = xp_icon_from_path( item->target );
    }
}

static void xp_free_items(void)
{
    unsigned int i;

    for (i = 0; i < xp_count; i++) if (xp_items[i].icon) DestroyIcon( xp_items[i].icon );
    xp_count = 0;
}

static void xp_build_items(void)
{
    WCHAR path[MAX_PATH];
    struct xp_item *item;
    HICON icon = NULL;

    xp_free_items();

    /* left column: pinned programs, the start menu shortcuts and All Programs */
    if (xp_find_program( L"wfm.exe", path ) || xp_find_program( L"winefile.exe", path ))
        xp_add_program( FALSE, TRUE, IDS_XP_FILE_MANAGER, path, NULL );
    xp_add_system_program( FALSE, TRUE, IDS_XP_COMMAND_PROMPT, L"cmd.exe" );
    xp_add_shortcuts();
    xp_add( XP_ACTION_ALL_PROGRAMS, FALSE, TRUE, IDS_XP_ALL_PROGRAMS );

    /* right column: places, tools and Run */
    xp_add_folder( TRUE, IDS_XP_MY_DOCUMENTS, CSIDL_PERSONAL );
    xp_add_folder( TRUE, IDS_XP_MY_PICTURES, CSIDL_MYPICTURES );
    xp_add_folder( TRUE, IDS_XP_MY_MUSIC, CSIDL_MYMUSIC );
    if (xp_find_program( L"wfm.exe", path ))
        xp_add_program( TRUE, TRUE, IDS_XP_MY_COMPUTER, path, xp_icon_from_csidl( CSIDL_DRIVES ));
    else
        xp_add_folder( TRUE, IDS_XP_MY_COMPUTER, CSIDL_DRIVES );
    xp_add( XP_ACTION_SEPARATOR, TRUE, FALSE, 0 );
    xp_add_folder( FALSE, IDS_XP_CONTROL_PANEL, CSIDL_CONTROLS );
    xp_add_system_program( TRUE, FALSE, IDS_TASK_MANAGER, L"taskmgr.exe" );
    xp_add_system_program( TRUE, FALSE, IDS_XP_WINECFG, L"winecfg.exe" );
    xp_add( XP_ACTION_SEPARATOR, TRUE, FALSE, 0 );
    if ((item = xp_add( XP_ACTION_RUN, TRUE, FALSE, IDS_RUN )))
    {
        ExtractIconExW( L"shell32.dll", -IDI_SHELL_RUN_ID, &icon, NULL, 1 );
        item->icon = icon;
    }

    /* footer */
    xp_add( XP_ACTION_TURN_OFF, FALSE, FALSE, IDS_XP_TURN_OFF );
}

static void xp_create_fonts(void)
{
    LOGFONTW lf;

    if (xp_font) DeleteObject( xp_font );
    if (xp_bold_font) DeleteObject( xp_bold_font );
    if (xp_title_font) DeleteObject( xp_title_font );
    memset( &lf, 0, sizeof(lf) );
    lf.lfHeight = -xp_px( 11 );
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyW( lf.lfFaceName, L"Tahoma" );
    xp_font = CreateFontIndirectW( &lf );
    lf.lfWeight = FW_BOLD;
    xp_bold_font = CreateFontIndirectW( &lf );
    lf.lfHeight = -xp_px( 15 );
    xp_title_font = CreateFontIndirectW( &lf );
}

static void xp_delete_fonts(void)
{
    if (xp_font) DeleteObject( xp_font );
    if (xp_bold_font) DeleteObject( xp_bold_font );
    if (xp_title_font) DeleteObject( xp_title_font );
    xp_font = xp_bold_font = xp_title_font = 0;
}

static void xp_layout( int *width, int *height )
{
    struct xp_item *all_programs = NULL, *turn_off = NULL;
    int left_y, right_y, body_bottom, item_height;
    unsigned int i;

    *width = xp_px( 380 );
    xp_split = xp_px( 190 );
    xp_header_height = xp_px( 58 );
    left_y = right_y = xp_header_height + xp_px( 2 ) + xp_px( 6 );

    for (i = 0; i < xp_count; i++)
    {
        struct xp_item *item = &xp_items[i];

        if (item->action == XP_ACTION_ALL_PROGRAMS)
        {
            all_programs = item;
            continue;
        }
        if (item->action == XP_ACTION_TURN_OFF)
        {
            turn_off = item;
            continue;
        }
        if (item->right)
        {
            item_height = item->action == XP_ACTION_SEPARATOR ? xp_px( 9 ) : xp_px( 30 );
            SetRect( &item->rect, xp_split + xp_px( 3 ), right_y, *width - xp_px( 3 ), right_y + item_height );
            right_y += item_height;
        }
        else
        {
            item_height = item->action == XP_ACTION_SEPARATOR ? xp_px( 9 ) : xp_px( 38 );
            SetRect( &item->rect, xp_px( 3 ), left_y, xp_split - xp_px( 2 ), left_y + item_height );
            left_y += item_height;
        }
    }
    left_y += xp_px( 9 ) + xp_px( 32 );  /* separator and All Programs */
    body_bottom = max( left_y, right_y ) + xp_px( 6 );
    if (all_programs)
        SetRect( &all_programs->rect, xp_px( 3 ), body_bottom - xp_px( 38 ), xp_split - xp_px( 2 ),
                 body_bottom - xp_px( 6 ));
    xp_footer_top = body_bottom;
    *height = body_bottom + xp_px( 42 );

    if (turn_off)
    {
        RECT text = { 0, 0, 0, 0 };
        HGDIOBJ old_font;
        HDC hdc;

        hdc = GetDC( 0 );
        old_font = SelectObject( hdc, xp_font );
        DrawTextW( hdc, turn_off->text, -1, &text, DT_SINGLELINE | DT_CALCRECT | DT_HIDEPREFIX );
        SelectObject( hdc, old_font );
        ReleaseDC( 0, hdc );
        SetRect( &turn_off->rect, *width - xp_px( 8 ) - xp_px( 4 + 22 + 6 + 6 ) - text.right,
                 body_bottom + xp_px( 6 ), *width - xp_px( 8 ), *height - xp_px( 6 ));
    }
}

static void xp_draw_arrow_box( HDC hdc, int x, int y, int size )
{
    HRGN rgn = CreateRoundRectRgn( x, y, x + size + 1, y + size + 1, xp_px( 5 ), xp_px( 5 ));
    HGDIOBJ old_pen, old_brush;
    HBRUSH brush;
    POINT pts[3];

    SelectClipRgn( hdc, rgn );
    xp_vgradient2( hdc, x, y, size, size, RGB(0x70,0xc8,0x67), RGB(0x2b,0x8a,0x2a) );
    SelectClipRgn( hdc, NULL );
    brush = CreateSolidBrush( RGB(0x24,0x6f,0x22) );
    FrameRgn( hdc, rgn, brush, 1, 1 );
    DeleteObject( brush );
    DeleteObject( rgn );

    pts[0].x = x + size * 3 / 8;
    pts[0].y = y + size / 4;
    pts[1].x = x + size * 3 / 8;
    pts[1].y = y + size * 3 / 4;
    pts[2].x = x + size * 3 / 4;
    pts[2].y = y + size / 2;
    old_pen = SelectObject( hdc, GetStockObject( WHITE_PEN ));
    old_brush = SelectObject( hdc, GetStockObject( WHITE_BRUSH ));
    Polygon( hdc, pts, 3 );
    SelectObject( hdc, old_brush );
    SelectObject( hdc, old_pen );
}

static void xp_draw_power_icon( HDC hdc, int x, int y, int size )
{
    HRGN rgn = CreateRoundRectRgn( x, y, x + size + 1, y + size + 1, xp_px( 6 ), xp_px( 6 ));
    int cx = x + size / 2, cy = y + size / 2 + xp_px( 1 ), radius = size * 3 / 10;
    HGDIOBJ old_pen, old_brush;
    HBRUSH brush;
    HPEN pen;

    SelectClipRgn( hdc, rgn );
    xp_vgradient2( hdc, x, y, size, size, RGB(0xf4,0x8c,0x50), RGB(0xc9,0x40,0x14) );
    SelectClipRgn( hdc, NULL );
    brush = CreateSolidBrush( RGB(0x93,0x2b,0x0b) );
    FrameRgn( hdc, rgn, brush, 1, 1 );
    DeleteObject( brush );
    DeleteObject( rgn );

    /* power symbol: a ring open at the top and a vertical bar */
    pen = CreatePen( PS_SOLID, max( 2, xp_px( 2 )), RGB(0xff,0xff,0xff) );
    old_pen = SelectObject( hdc, pen );
    old_brush = SelectObject( hdc, GetStockObject( NULL_BRUSH ));
    SetArcDirection( hdc, AD_COUNTERCLOCKWISE );
    Arc( hdc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1,
         cx - radius / 2, cy - radius, cx + radius / 2, cy - radius );
    MoveToEx( hdc, cx, cy - radius - xp_px( 2 ), NULL );
    LineTo( hdc, cx, cy - xp_px( 1 ));
    SelectObject( hdc, old_brush );
    SelectObject( hdc, old_pen );
    DeleteObject( pen );
}

static void xp_draw_item( HDC hdc, unsigned int index )
{
    const struct xp_item *item = &xp_items[index];
    BOOL hot = (int)index == xp_hot;
    RECT rect = item->rect;
    int icon_size;

    if (item->action == XP_ACTION_SEPARATOR)
    {
        xp_fade_line( hdc, rect.left + xp_px( 6 ), (rect.top + rect.bottom) / 2, rect.right - rect.left - xp_px( 12 ), 1,
                      item->right ? xp_scheme->right_back : RGB(0xff,0xff,0xff),
                      item->right ? xp_scheme->right_separator : RGB(0xc5,0xc2,0xb8) );
        return;
    }

    if (item->action == XP_ACTION_TURN_OFF)
    {
        int size = xp_px( 22 );

        if (hot)
        {
            HRGN rgn = CreateRoundRectRgn( rect.left, rect.top, rect.right + 1, rect.bottom + 1, xp_px( 6 ), xp_px( 6 ));
            HBRUSH brush = CreateSolidBrush( xp_blend( xp_scheme->footer[xp_scheme->footer_count - 1].color,
                                                       RGB(0xff,0xff,0xff), 1, 3 ));
            FillRgn( hdc, rgn, brush );
            DeleteObject( brush );
            DeleteObject( rgn );
        }
        xp_draw_power_icon( hdc, rect.left + xp_px( 4 ), (rect.top + rect.bottom - size) / 2, size );
        rect.left += xp_px( 4 ) + size + xp_px( 6 );
        SelectObject( hdc, xp_font );
        SetTextColor( hdc, xp_scheme->footer_text );
        DrawTextW( hdc, item->text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_HIDEPREFIX );
        return;
    }

    if (item->action == XP_ACTION_ALL_PROGRAMS)
    {
        RECT text = { 0, 0, 0, 0 };
        int box = xp_px( 18 ), gap = xp_px( 8 ), x;

        xp_fade_line( hdc, rect.left + xp_px( 6 ), rect.top - xp_px( 5 ), rect.right - rect.left - xp_px( 12 ), 1,
                      RGB(0xff,0xff,0xff), RGB(0xc5,0xc2,0xb8) );
        if (hot) xp_fill( hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, xp_scheme->hot );
        SelectObject( hdc, xp_bold_font );
        DrawTextW( hdc, item->text, -1, &text, DT_SINGLELINE | DT_CALCRECT | DT_HIDEPREFIX );
        x = rect.left + (rect.right - rect.left - text.right - gap - box) / 2;
        SetRect( &text, x, rect.top, x + text.right, rect.bottom );
        SetTextColor( hdc, hot ? RGB(0xff,0xff,0xff) : RGB(0x00,0x00,0x00) );
        DrawTextW( hdc, item->text, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_HIDEPREFIX | DT_NOCLIP );
        xp_draw_arrow_box( hdc, text.right + gap, (rect.top + rect.bottom - box) / 2, box );
        return;
    }

    if (hot) xp_fill( hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, xp_scheme->hot );
    icon_size = item->right ? xp_px( 24 ) : xp_px( 32 );
    if (item->icon)
        DrawIconEx( hdc, rect.left + xp_px( 4 ), (rect.top + rect.bottom - icon_size) / 2, item->icon,
                    icon_size, icon_size, 0, NULL, DI_NORMAL );
    rect.left += xp_px( 4 ) + icon_size + xp_px( 6 );
    rect.right -= xp_px( 4 );
    SelectObject( hdc, item->bold ? xp_bold_font : xp_font );
    if (hot) SetTextColor( hdc, RGB(0xff,0xff,0xff) );
    else SetTextColor( hdc, item->right ? xp_scheme->right_text : RGB(0x00,0x00,0x00) );
    DrawTextW( hdc, item->text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_HIDEPREFIX );
}

static void xp_paint( HWND hwnd )
{
    HGDIOBJ old_bitmap, old_font;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    RECT client, rect;
    int tile, logo, body_top;
    unsigned int i;
    HBRUSH brush;
    HDC hdc, mem;
    HRGN rgn;

    hdc = BeginPaint( hwnd, &ps );
    GetClientRect( hwnd, &client );
    if (!(mem = CreateCompatibleDC( hdc )))
    {
        EndPaint( hwnd, &ps );
        return;
    }
    bitmap = CreateCompatibleBitmap( hdc, client.right, client.bottom );
    old_bitmap = SelectObject( mem, bitmap );
    old_font = SelectObject( mem, xp_font );
    SetBkMode( mem, TRANSPARENT );

    /* header with the user picture and name */
    xp_vgradient( mem, 0, 0, client.right, xp_header_height, xp_scheme->header, xp_scheme->header_count );
    tile = xp_px( 44 );
    rect.left = xp_px( 8 );
    rect.top = (xp_header_height - tile) / 2;
    xp_fill( mem, rect.left, rect.top, tile, tile, RGB(0xd8,0xe4,0xf8) );
    xp_fill( mem, rect.left + xp_px( 2 ), rect.top + xp_px( 2 ), tile - xp_px( 4 ), tile - xp_px( 4 ),
             RGB(0xfa,0xfc,0xff) );
    logo = tile - xp_px( 8 );
    if (xp_logo)
        DrawIconEx( mem, rect.left + (tile - logo) / 2, rect.top + (tile - logo) / 2, xp_logo,
                    logo, logo, 0, NULL, DI_NORMAL );
    SelectObject( mem, xp_title_font );
    SetRect( &rect, xp_px( 8 ) + tile + xp_px( 8 ) + 1, 1, client.right - xp_px( 4 ) + 1, xp_header_height + 1 );
    SetTextColor( mem, xp_scheme->name_shadow );
    DrawTextW( mem, xp_user, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS );
    OffsetRect( &rect, -1, -1 );
    SetTextColor( mem, xp_scheme->name );
    DrawTextW( mem, xp_user, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS );

    /* orange line under the header */
    xp_fade_line( mem, 0, xp_header_height, client.right, xp_px( 2 ),
                  xp_scheme->header[xp_scheme->header_count - 1].color, RGB(0xf0,0x93,0x46) );

    /* the two columns and the footer */
    body_top = xp_header_height + xp_px( 2 );
    xp_fill( mem, 0, body_top, xp_split, xp_footer_top - body_top, RGB(0xff,0xff,0xff) );
    xp_fill( mem, xp_split, body_top, client.right - xp_split, xp_footer_top - body_top, xp_scheme->right_back );
    xp_fill( mem, xp_split, body_top, 1, xp_footer_top - body_top, xp_scheme->divider );
    xp_vgradient( mem, 0, xp_footer_top, client.right, client.bottom - xp_footer_top,
                  xp_scheme->footer, xp_scheme->footer_count );

    for (i = 0; i < xp_count; i++) xp_draw_item( mem, i );

    /* outer frame, following the rounded top corners */
    rgn = CreateRectRgn( 0, 0, 0, 0 );
    if (GetWindowRgn( hwnd, rgn ) != ERROR)
    {
        brush = CreateSolidBrush( xp_scheme->frame );
        FrameRgn( mem, rgn, brush, 1, 1 );
        DeleteObject( brush );
    }
    DeleteObject( rgn );

    BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
    SelectObject( mem, old_font );
    SelectObject( mem, old_bitmap );
    DeleteObject( bitmap );
    DeleteDC( mem );
    EndPaint( hwnd, &ps );
}

static int xp_hit_test( POINT pt )
{
    unsigned int i;

    for (i = 0; i < xp_count; i++)
    {
        if (xp_items[i].action == XP_ACTION_SEPARATOR) continue;
        if (PtInRect( &xp_items[i].rect, pt )) return i;
    }
    return -1;
}

static void xp_set_hot( int hot )
{
    if (hot == xp_hot) return;
    if (xp_hot >= 0) InvalidateRect( xp_window, &xp_items[xp_hot].rect, FALSE );
    xp_hot = hot;
    if (xp_hot >= 0) InvalidateRect( xp_window, &xp_items[xp_hot].rect, FALSE );
}

static void xp_move_hot( int dir )
{
    int i, index = xp_hot;

    for (i = 0; i < (int)xp_count; i++)
    {
        index = (index + dir + (int)xp_count) % (int)xp_count;
        if (xp_items[index].action == XP_ACTION_SEPARATOR) continue;
        xp_set_hot( index );
        return;
    }
}

/* the whole start menu tree, cascading from All Programs */
static void xp_show_all_programs( const RECT *rect )
{
    BOOL has_items;
    MENUINFO mi;
    POINT pt;

    if (!create_root_menu( &has_items )) return;
    if (!has_items)
    {
        destroy_menus();
        return;
    }
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_NOTIFYBYPOS;
    SetMenuInfo( root_menu.menuhandle, &mi );

    pt.x = rect->right;
    pt.y = rect->bottom;
    ClientToScreen( xp_window, &pt );
    xp_in_submenu = TRUE;
    ReleaseCapture();
    /* the tray window handles WM_MENUCOMMAND through menu_wndproc */
    TrackPopupMenuEx( root_menu.menuhandle, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, xp_tray, NULL );
    xp_in_submenu = FALSE;
}

static void xp_activate( int index )
{
    if (index < 0 || index >= (int)xp_count) return;
    if (xp_items[index].action == XP_ACTION_ALL_PROGRAMS) xp_show_all_programs( &xp_items[index].rect );
    else xp_selected = index;
    xp_done = TRUE;
}

static void xp_execute( const struct xp_item *item )
{
    SHELLEXECUTEINFOW sei;
    LPITEMIDLIST pidl;

    switch (item->action)
    {
    case XP_ACTION_PROGRAM:
        ShellExecuteW( NULL, NULL, item->target, NULL, NULL, SW_SHOWNORMAL );
        break;
    case XP_ACTION_FOLDER:
        if (FAILED(SHGetSpecialFolderLocation( NULL, item->csidl, &pidl ))) break;
        memset( &sei, 0, sizeof(sei) );
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_IDLIST;
        sei.nShow = SW_SHOWNORMAL;
        sei.lpIDList = pidl;
        ShellExecuteExW( &sei );
        CoTaskMemFree( pidl );
        break;
    case XP_ACTION_RUN:
        run_dialog();
        break;
    case XP_ACTION_TURN_OFF:
        shut_down( xp_tray );
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK xp_menu_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    RECT client;
    POINT pt;

    switch (msg)
    {
    case WM_PAINT:
        xp_paint( hwnd );
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
        pt.x = (short)LOWORD( lparam );
        pt.y = (short)HIWORD( lparam );
        xp_set_hot( xp_hit_test( pt ));
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        pt.x = (short)LOWORD( lparam );
        pt.y = (short)HIWORD( lparam );
        GetClientRect( hwnd, &client );
        if (!PtInRect( &client, pt )) xp_done = TRUE;  /* clicked outside of the menu */
        return 0;

    case WM_LBUTTONUP:
        pt.x = (short)LOWORD( lparam );
        pt.y = (short)HIWORD( lparam );
        xp_set_hot( xp_hit_test( pt ));
        xp_activate( xp_hot );
        return 0;

    case WM_KEYDOWN:
        switch (wparam)
        {
        case VK_ESCAPE:
            xp_done = TRUE;
            break;
        case VK_UP:
        case VK_DOWN:
            xp_move_hot( wparam == VK_DOWN ? 1 : -1 );
            break;
        case VK_RETURN:
            xp_activate( xp_hot );
            break;
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE && !xp_in_submenu) xp_done = TRUE;
        break;

    case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd && !xp_in_submenu) xp_done = TRUE;
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void do_xp_startmenu( HWND tray )
{
    static const WCHAR classW[] = L"__wine_xp_start_menu";
    static BOOL registered;
    int width, height, x, y;
    RECT tray_rect;
    HRGN rgn, top;
    DWORD size;
    MSG msg;

    if (xp_window) return;
    if (!registered)
    {
        WNDCLASSEXW cls;

        memset( &cls, 0, sizeof(cls) );
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = xp_menu_proc;
        cls.hCursor = LoadCursorW( 0, (const WCHAR *)IDC_ARROW );
        cls.lpszClassName = classW;
        registered = RegisterClassExW( &cls ) != 0;
    }

    xp_tray = tray;
    xp_dpi = GetDpiForWindow( tray );
    if (!xp_dpi) xp_dpi = USER_DEFAULT_SCREEN_DPI;
    xp_scheme = &xp_menu_schemes[min( get_taskbar_scheme(), ARRAY_SIZE(xp_menu_schemes) - 1 )];
    xp_create_fonts();
    size = ARRAY_SIZE(xp_user);
    if (!GetUserNameW( xp_user, &size )) lstrcpyW( xp_user, L"User" );
    xp_logo = LoadImageW( GetModuleHandleW( NULL ), MAKEINTRESOURCEW( IDI_WINE_LOGO ), IMAGE_ICON,
                          xp_px( 36 ), xp_px( 36 ), 0 );
    xp_build_items();
    xp_layout( &width, &height );

    GetWindowRect( tray, &tray_rect );
    x = tray_rect.left;
    y = max( 0, tray_rect.top - height );
    xp_window = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, classW, NULL, WS_POPUP,
                                 x, y, width, height, tray, 0, 0, 0 );
    if (xp_window)
    {
        /* rounded top corners */
        rgn = CreateRoundRectRgn( 0, 0, width + 1, height + 1, xp_px( 14 ), xp_px( 14 ));
        top = CreateRectRgn( 0, xp_px( 8 ), width, height );
        CombineRgn( rgn, rgn, top, RGN_OR );
        DeleteObject( top );
        SetWindowRgn( xp_window, rgn, FALSE );

        xp_hot = xp_selected = -1;
        xp_done = xp_in_submenu = FALSE;
        ShowWindow( xp_window, SW_SHOWNORMAL );
        SetForegroundWindow( xp_window );
        SetFocus( xp_window );
        SetCapture( xp_window );
        UpdateWindow( xp_window );

        while (!xp_done)
        {
            if (!GetMessageW( &msg, 0, 0, 0 ))
            {
                PostQuitMessage( msg.wParam );
                break;
            }
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }

        xp_in_submenu = TRUE;
        if (GetCapture() == xp_window) ReleaseCapture();
        DestroyWindow( xp_window );
        xp_window = 0;
        xp_in_submenu = FALSE;
        /* launch after the menu is gone so that the program can take the foreground */
        if (xp_selected >= 0) xp_execute( &xp_items[xp_selected] );
    }

    xp_free_items();
    xp_delete_fonts();
    if (xp_logo) DestroyIcon( xp_logo );
    xp_logo = 0;
}
