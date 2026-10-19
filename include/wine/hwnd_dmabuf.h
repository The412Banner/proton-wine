/* Wine-internal HWND buffer bridge helpers. The historical channel name also
 * carries CPU-backed SHM frames. */

#ifndef __WINE_HWND_DMABUF_H
#define __WINE_HWND_DMABUF_H

#include <wine/server.h>

#define HWND_DMABUF_MOD_LINEAR                 0
#define HWND_DMABUF_MOD_INVALID                0x00ffffffffffffffull

/* DXGI alpha-mode values carried over the hwnd-dmabuf wire protocol. */
#define HWND_DMABUF_ALPHA_MODE_UNSPECIFIED     0
#define HWND_DMABUF_ALPHA_MODE_IGNORE          3

#define HWND_DMABUF_HOST_CAP_CONSUMER_STATE    0x00000001

enum hwnd_dmabuf_wake_flags
{
    HWND_DMABUF_WAKE_REANNOUNCE = 0x00000001,
};

/* DRM fourcc values carried in hwnd_dmabuf_frame_desc_t.fourcc when
 * HWND_DMABUF_FLAG_SHM is set. Consumers translate them to native shm
 * protocol formats where needed. */
#define HWND_DMABUF_SHM_FORMAT_XRGB8888        0x34325258 /* 'XR24' */
#define HWND_DMABUF_SHM_FORMAT_ARGB8888        0x34325241 /* 'AR24' */

enum hwnd_dmabuf_channel_result
{
    HWND_DMABUF_CHANNEL_OK = 0,
    HWND_DMABUF_CHANNEL_EMPTY = 1,
    HWND_DMABUF_CHANNEL_ERROR = 2,
    HWND_DMABUF_CHANNEL_CLOSED = 3,
};

typedef struct
{
    unsigned int     fourcc;
    unsigned int     tranche_index;
    unsigned int     tranche_flags;
    unsigned __int64 modifier;
} hwnd_dmabuf_format_modifier_t;

typedef struct
{
    unsigned int     format_modifier_count;
    unsigned int     flags;
} hwnd_dmabuf_host_caps_t;

#define HWND_DMABUF_TRANCHE_FLAG_SCANOUT       0x1

/* Release token sent from the consumer back to the producer over the channel. */
typedef struct
{
    unsigned __int64 producer_unique_id;
    unsigned __int64 release_token;
    unsigned int     flags;
    unsigned int     image_id;
    unsigned int     ring_generation;
    unsigned int     reserved;
} hwnd_dmabuf_release_t;

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_list( HWND host_hwnd,
                                                             hwnd_dmabuf_frame_info_t *frames,
                                                             unsigned int max_frames,
                                                             unsigned int *total_count,
                                                             unsigned int *copied_count )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;
    data_size_t reply_size = 0;

    if (total_count) *total_count = 0;
    if (copied_count) *copied_count = 0;
    if (max_frames && !frames) return status;
    if (max_frames > (data_size_t)-1 / sizeof(*frames)) return status;

    SERVER_START_REQ( hwnd_list_dmabuf_frames )
    {
        req->host_hwnd = wine_server_user_handle( host_hwnd );
        reply_size = max_frames * sizeof(*frames);
        wine_server_set_reply( req, frames, reply_size );
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (total_count) *total_count = reply->count;
            if (copied_count) *copied_count = wine_server_reply_size( reply ) / sizeof(*frames);
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_set_pending( HWND hwnd, BOOL pending )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    SERVER_START_REQ( hwnd_dmabuf_set_pending )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->pending = pending;
        if (!wine_server_call_err( req ))
            status = reply->status;
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_get_channel( HWND hwnd, HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_get_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->flags = 0;
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_get_gdi_overlay_channel( HWND hwnd,
                                                                                HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_get_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->flags = HWND_DMABUF_CHANNEL_GDI_OVERLAY;
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_get_channel_exclusive( HWND hwnd,
                                                                              HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_get_channel_exclusive )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_claim_channel( HWND hwnd, HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_claim_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->flags = 0;
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_claim_gdi_overlay_channel( HWND hwnd,
                                                                                  HANDLE *channel_handle )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    if (!channel_handle) return status;
    *channel_handle = 0;

    SERVER_START_REQ( hwnd_dmabuf_claim_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->flags = HWND_DMABUF_CHANNEL_GDI_OVERLAY;
        if (!wine_server_call_err( req ))
        {
            status = reply->status;
            if (status == HWND_DMABUF_OK)
                *channel_handle = wine_server_ptr_handle( reply->channel_handle );
        }
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_release_channel( HWND hwnd )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    SERVER_START_REQ( hwnd_dmabuf_release_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->flags = 0;
        if (!wine_server_call_err( req ))
            status = reply->status;
    }
    SERVER_END_REQ;

    return status;
}

static inline enum hwnd_dmabuf_status wine_hwnd_dmabuf_release_gdi_overlay_channel( HWND hwnd )
{
    enum hwnd_dmabuf_status status = HWND_DMABUF_INVALID_ARGS;

    SERVER_START_REQ( hwnd_dmabuf_release_channel )
    {
        req->hwnd = wine_server_user_handle( hwnd );
        req->flags = HWND_DMABUF_CHANNEL_GDI_OVERLAY;
        if (!wine_server_call_err( req ))
            status = reply->status;
    }
    SERVER_END_REQ;

    return status;
}

#endif /* __WINE_HWND_DMABUF_H */
