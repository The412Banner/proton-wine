/*
 * Cursor and icon support
 *
 * Copyright 1995 Alexandre Julliard
 * Copyright 1996 Martin Von Loewis
 * Copyright 1997 Alex Korobka
 * Copyright 1998 Turchanov Sergey
 * Copyright 2007 Henri Verbeet
 * Copyright 2009 Vincent Povirk for CodeWeavers
 * Copyright 2016 Dmitry Timoshkov
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

#include <assert.h>
#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);
WINE_DECLARE_DEBUG_CHANNEL(icon);

struct cursoricon_object
{
    HICON                   handle;     /* cursor full handle */
    struct list             entry;      /* entry in shared icons list */
    struct free_icon_params params;     /* opaque params used by 16-bit code */
    UNICODE_STRING          module;     /* module for icons loaded from resources */
    WCHAR                  *resname;    /* resource name for icons loaded from resources */
    HRSRC                   rsrc;       /* resource for shared icons */
    BOOL                    is_shared;  /* whether this object is shared */
    BOOL                    is_icon;    /* whether icon or cursor */
    BOOL                    is_ani;     /* whether this object is a static cursor or an animated cursor */
    HANDLE                  shared;     /* image data for a cursor selected by another process */
    UINT                    delay;      /* delay between this frame and the next (in jiffies) */
    union
    {
        struct cursoricon_frame  frame; /* frame-specific icon data */
        struct
        {
            UINT  num_frames;           /* number of frames in the icon/cursor */
            UINT  num_steps;            /* number of sequence steps in the icon/cursor */
            HICON *frames;              /* list of animated cursor frames */
        } ani;
    };
};

static struct list icon_cache = LIST_INIT( icon_cache );

/* No pointers or GDI handles: the reader may have a different bitness. */
struct shared_cursor_bitmap
{
    int width, height;
    unsigned int bpp, size;
};

struct shared_cursor_data
{
    LONG ready;
    unsigned int size, xhotspot, yhotspot;
    unsigned int module_size, resname_size, resid;
    struct shared_cursor_bitmap mask, color;
    /* module, resource name, mask bits, then color bits */
};

#define MAX_SHARED_CURSOR_SIZE (64 * 1024 * 1024)

static void share_cursor( HCURSOR cursor );

static void get_cursor_section_name( HCURSOR cursor, UNICODE_STRING *name, WCHAR *buffer )
{
    char str[128];

    snprintf( str, sizeof(str), "\\Sessions\\%u\\BaseNamedObjects\\__wine_cursor_%08x",
              NtCurrentTeb()->Peb->SessionId, HandleToUlong( cursor ));
    name->Buffer = buffer;
    name->MaximumLength = asciiz_to_unicode( buffer, str );
    name->Length = name->MaximumLength - sizeof(WCHAR);
}

static unsigned int shared_cursor_bitmap_size( const struct shared_cursor_bitmap *bitmap )
{
    UINT64 size;

    if (bitmap->width <= 0 || bitmap->width > 0x7ffffff || bitmap->height <= 0) return 0;
    switch (bitmap->bpp)
    {
    case 1: case 4: case 8: case 16: case 24: case 32: break;
    default: return 0;
    }
    size = (((UINT64)bitmap->width * bitmap->bpp + 15) / 16) * 2 * bitmap->height;
    return size <= MAX_SHARED_CURSOR_SIZE ? size : 0;
}

static BOOL describe_shared_cursor_bitmap( HBITMAP handle, struct shared_cursor_bitmap *bitmap )
{
    BITMAP bmp;

    if (!handle) return TRUE;
    if (!NtGdiExtGetObjectW( handle, sizeof(bmp), &bmp )) return FALSE;
    bitmap->width = bmp.bmWidth;
    bitmap->height = bmp.bmHeight;
    bitmap->bpp = bmp.bmBitsPixel;
    return (bitmap->size = shared_cursor_bitmap_size( bitmap )) != 0;
}

static struct cursoricon_object *get_icon_ptr( HICON handle )
{
    struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );
    if (obj == OBJ_OTHER_PROCESS)
    {
        WARN( "icon handle %p from other process\n", handle );
        obj = NULL;
    }
    return obj;
}

BOOL process_wine_setcursor( HWND hwnd, HWND window, HCURSOR handle )
{
    TRACE( "hwnd %p, window %p, hcursor %p\n", hwnd, window, handle );
    user_driver->pSetCursor( window, handle );
    return TRUE;
}

/***********************************************************************
 *	     NtUserShowCursor    (win32u.@)
 */
INT WINAPI NtUserShowCursor( BOOL show )
{
    int increment = show ? 1 : -1;
    int count;

    SERVER_START_REQ( set_cursor )
    {
        req->flags = SET_CURSOR_COUNT;
        req->show_count = increment;
        wine_server_call( req );
        count = reply->prev_count + increment;
    }
    SERVER_END_REQ;

    TRACE("%d, count=%d\n", show, count );
    return count;
}

/***********************************************************************
 *	     NtUserSetCursor (win32u.@)
 */
HCURSOR WINAPI NtUserSetCursor( HCURSOR cursor )
{
    struct cursoricon_object *obj;
    HCURSOR old_cursor;
    BOOL ret;

    TRACE( "%p\n", cursor );

    /* Publish before the server forwards this handle to the window owner. */
    if (cursor) share_cursor( cursor );

    SERVER_START_REQ( set_cursor )
    {
        req->flags = SET_CURSOR_HANDLE;
        req->handle = wine_server_user_handle( cursor );
        if ((ret = !wine_server_call_err( req )))
            old_cursor = wine_server_ptr_handle( reply->prev_handle );
    }
    SERVER_END_REQ;
    if (!ret) return 0;

    check_for_events( QS_INPUT );

    if (!(obj = get_icon_ptr( old_cursor ))) return 0;
    release_user_handle_ptr( obj );
    return old_cursor;
}

/***********************************************************************
 *	     NtUserGetCursor (win32u.@)
 */
HCURSOR WINAPI NtUserGetCursor(void)
{
    HCURSOR ret;

    SERVER_START_REQ( set_cursor )
    {
        req->flags = 0;
        wine_server_call( req );
        ret = wine_server_ptr_handle( reply->prev_handle );
    }
    SERVER_END_REQ;
    return ret;
}

HICON alloc_cursoricon_handle( BOOL is_icon )
{
    struct cursoricon_object *obj;
    HICON handle;

    if (!(obj = calloc( 1, sizeof(*obj) ))) return NULL;
    obj->is_icon = is_icon;
    if (!(handle = alloc_user_handle( obj, NTUSER_OBJ_ICON ))) free( obj );
    else obj->handle = handle;
    return handle;
}

static struct cursoricon_object *get_icon_frame_ptr( HICON handle, UINT step )
{
    struct cursoricon_object *obj, *ret;

    if (!(obj = get_icon_ptr( handle ))) return NULL;
    if (!obj->is_ani) return obj;
    if (step >= obj->ani.num_steps)
    {
        release_user_handle_ptr( obj );
        return NULL;
    }
    ret = get_icon_ptr( obj->ani.frames[step] );
    release_user_handle_ptr( obj );
    return ret;
}

static void share_cursor( HCURSOR cursor )
{
    struct cursoricon_object *obj, *frame;
    struct shared_cursor_data header = {0}, *data = NULL;
    WCHAR buffer[128];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    LARGE_INTEGER size;
    SIZE_T view_size = 0;
    HANDLE section;
    char *bits;

    obj = get_user_handle_ptr( cursor, NTUSER_OBJ_ICON );
    if (!obj || obj == OBJ_OTHER_PROCESS) return;
    if (obj->is_icon || obj->shared) goto done;
    if (!(frame = get_icon_frame_ptr( cursor, 0 ))) goto done;
    if (!frame->frame.mask || !describe_shared_cursor_bitmap( frame->frame.mask, &header.mask ) ||
        !describe_shared_cursor_bitmap( frame->frame.color, &header.color )) goto done_frame;

    header.xhotspot = frame->frame.hotspot.x;
    header.yhotspot = frame->frame.hotspot.y;
    header.module_size = obj->module.Length;
    if (IS_INTRESOURCE( obj->resname )) header.resid = LOWORD(obj->resname);
    else header.resname_size = lstrlenW( obj->resname ) * sizeof(WCHAR);
    size.QuadPart = sizeof(header) + (UINT64)header.module_size + header.resname_size +
                    header.mask.size + header.color.size;
    if (size.QuadPart > MAX_SHARED_CURSOR_SIZE) goto done_frame;
    header.size = size.QuadPart;

    get_cursor_section_name( obj->handle, &name, buffer );
    InitializeObjectAttributes( &attr, &name, 0, NULL, NULL );
    if (NtCreateSection( &section, SECTION_MAP_READ | SECTION_MAP_WRITE, &attr, &size,
                         PAGE_READWRITE, SEC_COMMIT, NULL )) goto done_frame;
    if (NtMapViewOfSection( section, GetCurrentProcess(), (void **)&data, 0, 0, NULL,
                           &view_size, ViewUnmap, 0, PAGE_READWRITE )) goto close;

    *data = header;
    bits = (char *)(data + 1);
    if (header.module_size) memcpy( bits, obj->module.Buffer, header.module_size );
    bits += header.module_size;
    if (header.resname_size) memcpy( bits, obj->resname, header.resname_size );
    bits += header.resname_size;
    if (NtGdiGetBitmapBits( frame->frame.mask, header.mask.size, bits ) != header.mask.size) goto unmap;
    bits += header.mask.size;
    if (header.color.size && NtGdiGetBitmapBits( frame->frame.color, header.color.size, bits ) != header.color.size)
        goto unmap;

    InterlockedExchange( &data->ready, 1 );
    obj->shared = section;
    TRACE( "shared cursor %p, %u bytes\n", obj->handle, header.size );
unmap:
    NtUnmapViewOfSection( GetCurrentProcess(), data );
close:
    if (!obj->shared) NtClose( section );
done_frame:
    release_user_handle_ptr( frame );
done:
    release_user_handle_ptr( obj );
}

static BOOL free_icon_handle( HICON handle )
{
    struct cursoricon_object *obj = free_user_handle( handle, NTUSER_OBJ_ICON );

    if (obj == OBJ_OTHER_PROCESS) WARN( "icon handle %p from other process\n", handle );
    else if (obj)
    {
        struct free_icon_params params = obj->params;
        void *ret_ptr;
        ULONG ret_len;
        UINT i;

        assert( !obj->rsrc );  /* shared icons can't be freed */

        if (!obj->is_ani)
        {
            if (obj->frame.alpha) NtGdiDeleteObjectApp( obj->frame.alpha );
            if (obj->frame.color) NtGdiDeleteObjectApp( obj->frame.color );
            if (obj->frame.mask)  NtGdiDeleteObjectApp( obj->frame.mask );
        }
        else
        {
            for (i = 0; i < obj->ani.num_steps; i++)
            {
                HICON hFrame = obj->ani.frames[i];

                if (hFrame)
                {
                    UINT j;

                    free_icon_handle( obj->ani.frames[i] );
                    for (j = 0; j < obj->ani.num_steps; j++)
                    {
                        if (obj->ani.frames[j] == hFrame) obj->ani.frames[j] = 0;
                    }
                }
            }
            free( obj->ani.frames );
        }
        if (!IS_INTRESOURCE( obj->resname )) free( obj->resname );
        if (obj->module.Length) free(obj->module.Buffer);
        if (obj->shared) NtClose( obj->shared );
        free( obj );
        KeUserDispatchCallback( &params.dispatch, sizeof(params), &ret_ptr, &ret_len );
        user_driver->pDestroyCursorIcon( handle );
        return TRUE;
    }
    return FALSE;
}

/***********************************************************************
 *	     NtUserDestroyCursor (win32u.@)
 */
BOOL WINAPI NtUserDestroyCursor( HCURSOR cursor, ULONG arg )
{
    struct cursoricon_object *obj;
    BOOL shared, ret;

    TRACE( "%p\n", cursor );

    if (!(obj = get_icon_ptr( cursor ))) return FALSE;
    shared = obj->is_shared;
    release_user_handle_ptr( obj );
    ret = NtUserGetCursor() != cursor;
    if (!shared) free_icon_handle( cursor );
    return ret;
}

/***********************************************************************
 *	     NtUserSetCursorIconData (win32u.@)
 */
BOOL WINAPI NtUserSetCursorIconData( HCURSOR cursor, UNICODE_STRING *module, UNICODE_STRING *res_name,
                                     struct cursoricon_desc *desc )
{
    struct cursoricon_object *obj;
    UINT i, j;

    if (!(obj = get_icon_ptr( cursor ))) return FALSE;

    if (obj->is_ani || obj->frame.width)
    {
        /* already initialized */
        release_user_handle_ptr( obj );
        RtlSetLastWin32Error( ERROR_INVALID_CURSOR_HANDLE );
        return FALSE;
    }

    obj->delay = desc->delay;

    if (desc->num_steps)
    {
        if (!(obj->ani.frames = calloc( desc->num_steps, sizeof(*obj->ani.frames) )))
        {
            release_user_handle_ptr( obj );
            return FALSE;
        }
        obj->is_ani = TRUE;
        obj->ani.num_steps  = desc->num_steps;
        obj->ani.num_frames = desc->num_frames;
    }
    else obj->frame = desc->frames[0];

    if (!res_name)
        obj->resname = NULL;
    else if (res_name->Length)
    {
        obj->resname = malloc( res_name->Length + sizeof(WCHAR) );
        if (obj->resname)
        {
            memcpy( obj->resname, res_name->Buffer, res_name->Length );
            obj->resname[res_name->Length / sizeof(WCHAR)] = 0;
        }
    }
    else
        obj->resname = MAKEINTRESOURCEW( LOWORD(res_name->Buffer) );

    if (module && module->Length && (obj->module.Buffer = malloc( module->Length )))
    {
        memcpy( obj->module.Buffer, module->Buffer, module->Length );
        obj->module.Length = module->Length;
    }

    if (obj->is_ani)
    {
        /* Setup the animated frames in the correct sequence */
        for (i = 0; i < desc->num_steps; i++)
        {
            struct cursoricon_desc frame_desc;
            DWORD frame_id;

            if (obj->ani.frames[i]) continue; /* already set */

            frame_id = desc->frame_seq ? desc->frame_seq[i] : i;
            if (frame_id >= obj->ani.num_frames)
            {
                frame_id = obj->ani.num_frames - 1;
                ERR_(cursor)( "Sequence indicates frame past end of list, corrupt?\n" );
            }
            memset( &frame_desc, 0, sizeof(frame_desc) );
            frame_desc.delay  = desc->frame_rates ? desc->frame_rates[i] : desc->delay;
            frame_desc.frames = &desc->frames[frame_id];
            if (!(obj->ani.frames[i] = alloc_cursoricon_handle( obj->is_icon )) ||
                !NtUserSetCursorIconData( obj->ani.frames[i], NULL, NULL, &frame_desc ))
            {
                release_user_handle_ptr( obj );
                return 0;
            }

            if (desc->frame_seq)
            {
                for (j = i + 1; j < obj->ani.num_steps; j++)
                {
                    if (desc->frame_seq[j] == frame_id) obj->ani.frames[j] = obj->ani.frames[i];
                }
            }
        }
    }

    if (desc->flags & LR_SHARED)
    {
        obj->is_shared = TRUE;
        if (obj->module.Length)
        {
            obj->rsrc = desc->rsrc;
            list_add_head( &icon_cache, &obj->entry );
        }
    }

    release_user_handle_ptr( obj );
    return TRUE;
}

/***********************************************************************
 *	     NtUserFindExistingCursorIcon (win32u.@)
 */
HICON WINAPI NtUserFindExistingCursorIcon( UNICODE_STRING *module, UNICODE_STRING *res_name, void *desc )
{
    struct cursoricon_object *ptr;
    HICON ret = 0;

    user_lock();
    LIST_FOR_EACH_ENTRY( ptr, &icon_cache, struct cursoricon_object, entry )
    {
        if (ptr->module.Length != module->Length) continue;
        if (memcmp( ptr->module.Buffer, module->Buffer, module->Length )) continue;
        /* We pass rsrc as desc argument, this is not compatible with Windows */
        if (ptr->rsrc != desc) continue;
        ret = ptr->handle;
        break;
    }
    user_unlock();
    return ret;
}

/***********************************************************************
 *	     NtUserGetIconSize (win32u.@)
 */
BOOL WINAPI NtUserGetIconSize( HICON handle, UINT step, LONG *width, LONG *height )
{
    struct cursoricon_object *obj;

    if (!(obj = get_icon_frame_ptr( handle, step )))
    {
        RtlSetLastWin32Error( ERROR_INVALID_CURSOR_HANDLE );
        return FALSE;
    }

    *width  = obj->frame.width;
    *height = obj->frame.height * 2;
    release_user_handle_ptr( obj );
    return TRUE;
}

/**********************************************************************
 *           NtUserGetCursorFrameInfo (win32u.@)
 */
HCURSOR WINAPI NtUserGetCursorFrameInfo( HCURSOR cursor, DWORD istep, DWORD *rate_jiffies,
                                         DWORD *num_steps )
{
    struct cursoricon_object *obj;
    HCURSOR ret = 0;
    UINT icon_steps;

    if (!rate_jiffies || !num_steps) return 0;

    if (!(obj = get_icon_ptr( cursor ))) return 0;

    TRACE( "%p => %d %p %p\n", cursor, istep, rate_jiffies, num_steps );

    icon_steps = obj->is_ani ? obj->ani.num_steps : 1;
    if (istep < icon_steps || !obj->is_ani)
    {
        UINT icon_frames = 1;

        if (obj->is_ani)
            icon_frames = obj->ani.num_frames;
        if (obj->is_ani && icon_frames > 1)
            ret = obj->ani.frames[istep];
        else
            ret = cursor;
        if (icon_frames == 1)
        {
            *rate_jiffies = 0;
            *num_steps = 1;
        }
        else if (icon_steps == 1)
        {
            *num_steps = ~0;
            *rate_jiffies = obj->delay;
        }
        else if (istep < icon_steps)
        {
            struct cursoricon_object *frame;

            *num_steps = icon_steps;
            frame = get_icon_ptr( obj->ani.frames[istep] );
            if (obj->ani.num_steps == 1)
                *num_steps = ~0;
            else
                *num_steps = obj->ani.num_steps;
            *rate_jiffies = frame->delay;
            release_user_handle_ptr( frame );
        }
    }

    release_user_handle_ptr( obj );
    return ret;
}

/***********************************************************************
 *             copy_bitmap
 *
 * Helper function to duplicate a bitmap.
 */
static HBITMAP copy_bitmap( HBITMAP bitmap )
{
    HDC src, dst = 0;
    HBITMAP new_bitmap = 0;
    BITMAP bmp;

    if (!bitmap) return 0;
    if (!NtGdiExtGetObjectW( bitmap, sizeof(bmp), &bmp )) return 0;

    if ((src = NtGdiCreateCompatibleDC( 0 )) && (dst = NtGdiCreateCompatibleDC( 0 )))
    {
        NtGdiSelectBitmap( src, bitmap );
        if ((new_bitmap = NtGdiCreateCompatibleBitmap( src, bmp.bmWidth, bmp.bmHeight )))
        {
            NtGdiSelectBitmap( dst, new_bitmap );
            NtGdiBitBlt( dst, 0, 0, bmp.bmWidth, bmp.bmHeight, src, 0, 0, SRCCOPY, 0, 0 );
        }
    }
    NtGdiDeleteObjectApp( dst );
    NtGdiDeleteObjectApp( src );
    return new_bitmap;
}

static BOOL get_shared_cursor_info( HCURSOR cursor, ICONINFO *info, UNICODE_STRING *module,
                                    UNICODE_STRING *res_name )
{
    struct shared_cursor_data *view = NULL, *data = NULL;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    WCHAR buffer[128];
    SIZE_T view_size = 0;
    HANDLE section;
    const char *bits;
    unsigned int size;
    ICONINFO result = {0};
    BOOL ret = FALSE;

    get_cursor_section_name( cursor, &name, buffer );
    InitializeObjectAttributes( &attr, &name, 0, NULL, NULL );
    if (NtOpenSection( &section, SECTION_MAP_READ, &attr )) return FALSE;
    if (NtMapViewOfSection( section, GetCurrentProcess(), (void **)&view, 0, 0, NULL,
                           &view_size, ViewUnmap, 0, PAGE_READONLY )) goto done;
    if (view_size < sizeof(*view) || !ReadAcquire( &view->ready )) goto done;
    size = view->size;
    if (size < sizeof(*view) || size > view_size || size > MAX_SHARED_CURSOR_SIZE) goto done;
    if (!(data = malloc( size ))) goto done;
    memcpy( data, view, size );
    if (sizeof(*data) + (UINT64)data->module_size + data->resname_size + data->mask.size +
        data->color.size != size || (data->module_size | data->resname_size) % sizeof(WCHAR) ||
        !data->mask.size || data->mask.bpp != 1 ||
        data->mask.size != shared_cursor_bitmap_size( &data->mask ) ||
        (data->color.size && data->color.size != shared_cursor_bitmap_size( &data->color ))) goto done;

    bits = (char *)(data + 1) + data->module_size + data->resname_size;
    result.xHotspot = data->xhotspot;
    result.yHotspot = data->yhotspot;
    result.hbmMask = NtGdiCreateBitmap( data->mask.width, data->mask.height, 1, 1, bits );
    if (!result.hbmMask) goto done;
    if (data->color.size)
    {
        result.hbmColor = NtGdiCreateBitmap( data->color.width, data->color.height, 1,
                                           data->color.bpp, bits + data->mask.size );
        if (!result.hbmColor)
        {
            NtGdiDeleteObjectApp( result.hbmMask );
            goto done;
        }
    }
    *info = result;
    if (module)
    {
        size = min( module->MaximumLength, data->module_size );
        if (size) memcpy( module->Buffer, data + 1, size );
        module->Length = size / sizeof(WCHAR);
    }
    if (res_name)
    {
        if (data->module_size && data->resname_size)
        {
            size = min( res_name->MaximumLength, data->resname_size );
            if (size) memcpy( res_name->Buffer, (char *)(data + 1) + data->module_size, size );
            res_name->Length = size / sizeof(WCHAR);
        }
        else
        {
            res_name->Buffer = data->module_size ? MAKEINTRESOURCEW( data->resid ) : NULL;
            res_name->Length = 0;
        }
    }
    TRACE( "read shared cursor %p, %dx%d\n", cursor, data->mask.width, data->mask.height );
    ret = TRUE;
done:
    free( data );
    if (view) NtUnmapViewOfSection( GetCurrentProcess(), view );
    NtClose( section );
    return ret;
}

/**********************************************************************
 *           NtUserGetIconInfo (win32u.@)
 */
BOOL WINAPI NtUserGetIconInfo( HICON icon, ICONINFO *info, UNICODE_STRING *module,
                               UNICODE_STRING *res_name, DWORD *bpp, LONG unk )
{
    struct cursoricon_object *obj, *frame_obj;
    BOOL ret = TRUE;

    obj = get_user_handle_ptr( icon, NTUSER_OBJ_ICON );
    if (obj == OBJ_OTHER_PROCESS)
    {
        if (get_shared_cursor_info( icon, info, module, res_name )) return TRUE;
        obj = NULL;
    }
    if (!obj)
    {
        RtlSetLastWin32Error( ERROR_INVALID_CURSOR_HANDLE );
        return FALSE;
    }
    if (!(frame_obj = get_icon_frame_ptr( icon, 0 )))
    {
        release_user_handle_ptr( obj );
        return FALSE;
    }

    TRACE( "%p => %dx%d\n", icon, frame_obj->frame.width, frame_obj->frame.height );

    info->fIcon        = obj->is_icon;
    info->xHotspot     = frame_obj->frame.hotspot.x;
    info->yHotspot     = frame_obj->frame.hotspot.y;
    info->hbmColor     = copy_bitmap( frame_obj->frame.color );
    info->hbmMask      = copy_bitmap( frame_obj->frame.mask );
    if (!info->hbmMask || (!info->hbmColor && frame_obj->frame.color))
    {
        NtGdiDeleteObjectApp( info->hbmMask );
        NtGdiDeleteObjectApp( info->hbmColor );
        ret = FALSE;
    }
    else if (obj->module.Length)
    {
        if (module)
        {
            size_t size = min( module->MaximumLength, obj->module.Length );
            if (size) memcpy( module->Buffer, obj->module.Buffer, size );
            module->Length = size / sizeof(WCHAR); /* length in chars, not bytes */
        }
        if (res_name)
        {
            if (IS_INTRESOURCE( obj->resname ))
            {
                res_name->Buffer = obj->resname;
                res_name->Length = 0;
            }
            else
            {
                size_t size = min( res_name->MaximumLength, lstrlenW( obj->resname) * sizeof(WCHAR) );
                if (size) memcpy( res_name->Buffer, obj->resname, size );
                res_name->Length = size / sizeof(WCHAR); /* length in chars, not bytes */
            }
        }
    }
    else
    {
        if (module) module->Length = 0;
        if (res_name)
        {
            res_name->Length = 0;
            res_name->Buffer = NULL;
        }
    }
    release_user_handle_ptr( frame_obj );
    release_user_handle_ptr( obj );
    return ret;
}

/******************************************************************************
 *	     NtUserDrawIconEx (win32u.@)
 */
BOOL WINAPI NtUserDrawIconEx( HDC hdc, INT x0, INT y0, HICON icon, INT width,
                              INT height, UINT step, HBRUSH brush, UINT flags )
{
    struct cursoricon_object *obj;
    HBITMAP offscreen_bitmap = 0;
    HDC hdc_dest, mem_dc;
    COLORREF old_fg, old_bg;
    INT x, y, nStretchMode;
    BOOL result = FALSE;

    TRACE_(icon)( "(hdc=%p,pos=%d.%d,hicon=%p,extend=%d.%d,step=%d,br=%p,flags=0x%08x)\n",
                  hdc, x0, y0, icon, width, height, step, brush, flags );

    if (!(obj = get_icon_frame_ptr( icon, step )))
    {
        FIXME_(icon)("Error retrieving icon frame %d\n", step);
        return FALSE;
    }
    if (!(mem_dc = NtGdiCreateCompatibleDC( hdc )))
    {
        release_user_handle_ptr( obj );
        return FALSE;
    }

    if (flags & DI_NOMIRROR)
        FIXME_(icon)("Ignoring flag DI_NOMIRROR\n");

    /* Calculate the size of the destination image.  */
    if (width == 0)
    {
        if (flags & DI_DEFAULTSIZE)
            width = get_system_metrics( SM_CXICON );
        else
            width = obj->frame.width;
    }
    if (height == 0)
    {
        if (flags & DI_DEFAULTSIZE)
            height = get_system_metrics( SM_CYICON );
        else
            height = obj->frame.height;
    }

    if (get_gdi_object_type( brush ) == NTGDI_OBJ_BRUSH)
    {
        HBRUSH prev_brush;
        RECT r;

        SetRect(&r, 0, 0, width, width);

        if (!(hdc_dest = NtGdiCreateCompatibleDC(hdc))) goto failed;
        if (!(offscreen_bitmap = NtGdiCreateCompatibleBitmap(hdc, width, height)))
        {
            NtGdiDeleteObjectApp( hdc_dest );
            goto failed;
        }
        NtGdiSelectBitmap( hdc_dest, offscreen_bitmap );

        prev_brush = NtGdiSelectBrush( hdc_dest, brush );
        NtGdiPatBlt( hdc_dest, r.left, r.top, r.right - r.left, r.bottom - r.top, PATCOPY );
        if (prev_brush) NtGdiSelectBrush( hdc_dest, prev_brush );
        x = y = 0;
    }
    else
    {
        hdc_dest = hdc;
        x = x0;
        y = y0;
    }

    nStretchMode = set_stretch_blt_mode( hdc, STRETCH_DELETESCANS );
    NtGdiGetAndSetDCDword( hdc, NtGdiSetTextColor, RGB(0,0,0), &old_fg );
    NtGdiGetAndSetDCDword( hdc, NtGdiSetBkColor, RGB(255,255,255), &old_bg );

    if (obj->frame.alpha && (flags & DI_IMAGE))
    {
        BOOL alpha_blend = TRUE;

        if (get_gdi_object_type( hdc_dest ) == NTGDI_OBJ_MEMDC)
        {
            BITMAP bm;
            HBITMAP bmp = NtGdiGetDCObject( hdc_dest, NTGDI_OBJ_SURF );
            alpha_blend = NtGdiExtGetObjectW( bmp, sizeof(bm), &bm ) && bm.bmBitsPixel > 8;
        }
        if (alpha_blend)
        {
            NtGdiSelectBitmap( mem_dc, obj->frame.alpha );
            if (NtGdiAlphaBlend( hdc_dest, x, y, width, height, mem_dc,
                                 0, 0, obj->frame.width, obj->frame.height,
                                 MAKEFOURCC( AC_SRC_OVER, 0, 255, AC_SRC_ALPHA ), 0 ))
                goto done;
        }
    }

    if (flags & DI_MASK)
    {
        DWORD rop = (flags & DI_IMAGE) ? SRCAND : SRCCOPY;
        NtGdiSelectBitmap( mem_dc, obj->frame.mask );
        NtGdiStretchBlt( hdc_dest, x, y, width, height,
                         mem_dc, 0, 0, obj->frame.width, obj->frame.height, rop, 0 );
    }

    if (flags & DI_IMAGE)
    {
        if (obj->frame.color)
        {
            DWORD rop = (flags & DI_MASK) ? SRCINVERT : SRCCOPY;
            NtGdiSelectBitmap( mem_dc, obj->frame.color );
            NtGdiStretchBlt( hdc_dest, x, y, width, height,
                             mem_dc, 0, 0, obj->frame.width, obj->frame.height, rop, 0 );
        }
        else
        {
            DWORD rop = (flags & DI_MASK) ? SRCINVERT : SRCCOPY;
            NtGdiSelectBitmap( mem_dc, obj->frame.mask );
            NtGdiStretchBlt( hdc_dest, x, y, width, height,
                             mem_dc, 0, obj->frame.height, obj->frame.width,
                             obj->frame.height, rop, 0 );
        }
    }

done:
    if (offscreen_bitmap) NtGdiBitBlt( hdc, x0, y0, width, height, hdc_dest, 0, 0, SRCCOPY, 0, 0 );

    NtGdiGetAndSetDCDword( hdc, NtGdiSetTextColor, old_fg, NULL );
    NtGdiGetAndSetDCDword( hdc, NtGdiSetBkColor, old_bg, NULL );
    nStretchMode = set_stretch_blt_mode( hdc, nStretchMode );

    result = TRUE;
    if (hdc_dest != hdc) NtGdiDeleteObjectApp( hdc_dest );
    if (offscreen_bitmap) NtGdiDeleteObjectApp( offscreen_bitmap );
failed:
    NtGdiDeleteObjectApp( mem_dc );
    release_user_handle_ptr( obj );
    return result;
}

ULONG_PTR get_icon_param( HICON handle )
{
    ULONG_PTR ret = 0;
    struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );

    if (obj == OBJ_OTHER_PROCESS) WARN( "icon handle %p from other process\n", handle );
    else if (obj)
    {
        ret = obj->params.param;
        release_user_handle_ptr( obj );
    }
    return ret;
}

ULONG_PTR set_icon_param( HICON handle, const struct free_icon_params *params )
{
    ULONG_PTR ret = 0;
    struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );

    if (obj == OBJ_OTHER_PROCESS) WARN( "icon handle %p from other process\n", handle );
    else if (obj)
    {
        ret = obj->params.param;
        obj->params = *params;
        release_user_handle_ptr( obj );
    }
    return ret;
}

/******************************************************************************
 *	     CopyImage (win32u.so)
 */
HANDLE WINAPI CopyImage( HANDLE hwnd, UINT type, INT dx, INT dy, UINT flags )
{
    void *ret_ptr;
    ULONG ret_len;
    NTSTATUS ret;
    struct copy_image_params params =
        { .hwnd = hwnd, .type = type, .dx = dx, .dy = dy, .flags = flags };

    ret = KeUserModeCallback( NtUserCopyImage, &params, sizeof(params), &ret_ptr, &ret_len );
    if (!ret && ret_len == sizeof(HANDLE)) return *(HANDLE *)ret_ptr;
    return 0;
}

/******************************************************************************
 *           LoadImage (win32u.so)
 */
HANDLE WINAPI LoadImageW( HINSTANCE hinst, const WCHAR *name, UINT type,
                          INT dx, INT dy, UINT flags )
{
    void *ret_ptr;
    ULONG ret_len;
    NTSTATUS ret;
    struct load_image_params params =
        { .hinst = hinst, .name = name, .type = type, .dx = dx, .dy = dy, .flags = flags };

    if (HIWORD(name))
    {
        ERR( "name %s not supported in Unix modules\n", debugstr_w( name ));
        return 0;
    }
    ret = KeUserModeCallback( NtUserLoadImage, &params, sizeof(params), &ret_ptr, &ret_len );
    if (!ret && ret_len == sizeof(HANDLE)) return *(HANDLE *)ret_ptr;
    return 0;
}
