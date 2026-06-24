/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#ifndef __WINE_WINEDMO_H
#define __WINE_WINEDMO_H

#include <stddef.h>
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "mfapi.h"

union winedmo_format
{
    WAVEFORMATEX audio;
    MFVIDEOFORMAT video;
};

struct winedmo_stream
{
    NTSTATUS (CDECL *p_seek)( struct winedmo_stream *stream, UINT64 *pos );
    NTSTATUS (CDECL *p_read)( struct winedmo_stream *stream, BYTE *buffer, ULONG *size );
};

struct winedmo_demuxer   { UINT64 handle; };
struct winedmo_muxer     { UINT64 handle; };
struct winedmo_transform { UINT64 handle; };

/* Flags for winedmo_transform_push_input */
#define WINEDMO_SAMPLE_FLAG_SYNC_POINT      0x01
#define WINEDMO_SAMPLE_FLAG_DISCONTINUITY   0x02
/* Flags returned by winedmo_transform_get_output */
#define WINEDMO_SAMPLE_FLAG_INCOMPLETE      0x04  /* more output pending without new input */
#define WINEDMO_SAMPLE_FLAG_FORMAT_CHANGED  0x08  /* output format changed; call get_output_format */

NTSTATUS CDECL winedmo_muxer_create( const char *format, struct winedmo_muxer *muxer );
NTSTATUS CDECL winedmo_muxer_destroy( struct winedmo_muxer muxer );
NTSTATUS CDECL winedmo_muxer_add_stream( struct winedmo_muxer muxer, UINT32 stream_id, const GUID *major_type,
                                          const union winedmo_format *format, UINT32 format_size );
NTSTATUS CDECL winedmo_muxer_start( struct winedmo_muxer muxer );
NTSTATUS CDECL winedmo_muxer_push_sample( struct winedmo_muxer muxer, UINT32 stream_id,
                                           const BYTE *data, UINT32 size, INT64 pts, INT64 duration, DWORD flags );
NTSTATUS CDECL winedmo_muxer_read_data( struct winedmo_muxer muxer, BYTE *buffer, UINT32 *size, UINT64 *offset );
NTSTATUS CDECL winedmo_muxer_finalize( struct winedmo_muxer muxer );

NTSTATUS CDECL winedmo_demuxer_check( const char *mime_type );
NTSTATUS CDECL winedmo_demuxer_create( const WCHAR *url, struct winedmo_stream *stream, UINT64 stream_size, INT64 *duration,
                                       UINT *stream_count, WCHAR *mime_type, struct winedmo_demuxer *demuxer );
NTSTATUS CDECL winedmo_demuxer_destroy( struct winedmo_demuxer *demuxer );
NTSTATUS CDECL winedmo_demuxer_read( struct winedmo_demuxer demuxer, UINT *stream, DMO_OUTPUT_DATA_BUFFER *buffer, UINT *buffer_size );
NTSTATUS CDECL winedmo_demuxer_seek( struct winedmo_demuxer demuxer, INT64 timestamp );
NTSTATUS CDECL winedmo_demuxer_stream_lang( struct winedmo_demuxer demuxer, UINT stream, WCHAR *buffer, UINT len );
NTSTATUS CDECL winedmo_demuxer_stream_name( struct winedmo_demuxer demuxer, UINT stream, WCHAR *buffer, UINT len );
NTSTATUS CDECL winedmo_demuxer_stream_type( struct winedmo_demuxer demuxer, UINT stream,
                                            GUID *major, union winedmo_format **format );

NTSTATUS CDECL winedmo_transform_create( GUID major_type,
                                         const union winedmo_format *input_format,  UINT32 input_format_size,
                                         const union winedmo_format *output_format, UINT32 output_format_size,
                                         struct winedmo_transform *transform );
NTSTATUS CDECL winedmo_transform_destroy( struct winedmo_transform transform );
NTSTATUS CDECL winedmo_transform_push_input( struct winedmo_transform transform,
                                              const BYTE *data, UINT32 size,
                                              INT64 pts, INT64 dts, INT64 duration, DWORD flags );
NTSTATUS CDECL winedmo_transform_get_output( struct winedmo_transform transform,
                                              BYTE *data, UINT32 *size,
                                              INT64 *pts, INT64 *duration, DWORD *flags );
NTSTATUS CDECL winedmo_transform_drain( struct winedmo_transform transform );
NTSTATUS CDECL winedmo_transform_flush( struct winedmo_transform transform );
NTSTATUS CDECL winedmo_transform_get_output_format( struct winedmo_transform transform,
                                                     GUID *major, union winedmo_format **format );
NTSTATUS CDECL winedmo_transform_set_output_format( struct winedmo_transform transform,
                                                     const union winedmo_format *format, UINT32 format_size );

#endif /* __WINE_WINEDMO_H */
