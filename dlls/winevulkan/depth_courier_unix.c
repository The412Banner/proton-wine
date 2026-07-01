/* SGSR2 depth-export moonshot — Gate 0 "courier probe" (UNIX side).
 *
 * Copyright 2026 The412Banner / Bannerlator
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS
 *
 * A logging-only capability probe that proves the guest->host *depth transport
 * primitive* end to end, before any DXVK / depth-math work (Gate 1). It:
 *
 *   1. Enumerates the HOST device extensions on the wrapper's physical device
 *      (reached via the raw host vkGetInstanceProcAddr that win32u dlsym'd out
 *      of libvulkan_wrapper.so) and LOGS whether
 *      VK_ANDROID_external_memory_android_hardware_buffer is advertised. This is
 *      the moonshot go/no-go: if the wrapper filters the AHB extension the whole
 *      export path is dead.
 *
 *   2. If present: allocates a small 16x16 AHB-EXPORT-backed image, calls
 *      vkGetMemoryAndroidHardwareBufferANDROID, confirms a non-null
 *      AHardwareBuffer*, and LOGS VkAndroidHardwareBufferFormatPropertiesANDROID
 *      (format + externalFormat) so Gate 1 can decide R32_SFLOAT vs RGBA8-pack.
 *      It first tries VK_FORMAT_R32_SFLOAT (the ideal linear-depth format) and,
 *      if that can't be created/exported, falls back to VK_FORMAT_R8G8B8A8_UNORM
 *      and logs which one worked.
 *
 *   3. Opens a SECOND, independent X (DRI3) connection to $DISPLAY (raw X11 wire
 *      protocol over the AF_UNIX socket -- this Wine build has no libxcb, and
 *      speaking the wire directly gives exact byte control + native SCM_RIGHTS
 *      FD passing) and issues a DRI3 PixmapFromBuffers carrying modifier
 *      ((frameId<<32)|1256) + one socketpair FD, targeting the current present
 *      window, so the app's Java X server receives it. The retained socketpair
 *      end then serves the AHB handle via AHardwareBuffer_sendHandleToUnixSocket
 *      (the exact protocol the app's GPUImage(fd) receiver expects).
 *
 * Everything here is UNIX-side (bionic NDK compiled -- see #pragma makedep unix)
 * so AHardwareBuffer_*, sockets and the X wire are all legal. The module is
 * self-contained: it creates its OWN throwaway host VkInstance/VkDevice and
 * never touches the guest's real device, so it cannot perturb normal rendering.
 * It only runs when WINE_DEPTH_COURIER is set (see depth_courier.h).
 * ---------------------------------------------------------------------------
 */

#if 0
#pragma makedep unix
#endif

/* Keep this translation unit in its own clean Vulkan universe (NDK headers),
 * decoupled from wine/vulkan.h. The only thing we borrow from Wine is the raw
 * host vkGetInstanceProcAddr, handed to us as a void* (see depth_courier.h). */
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <android/hardware_buffer.h>

#include <errno.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include "depth_courier.h"

/* ------------------------------------------------------------------ logging */

#define DC_TAG "SGSR2_COURIER"

/* liblog is dlopen'd so we don't add a link dependency; if it isn't there we
 * still log to stderr, which lands in the container's wine_debug.log. */
typedef int (*pfn_android_log_print)(int prio, const char *tag, const char *fmt, ...);
static pfn_android_log_print p_android_log_print;

static void dc_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s: %s\n", DC_TAG, buf);
    fflush(stderr);
    if (p_android_log_print)
        p_android_log_print(4 /* ANDROID_LOG_INFO */, DC_TAG, "%s", buf);
}

/* --------------------------------------------------- AHardwareBuffer via dlopen
 * We dlopen libandroid.so (fallback libnativewindow.so) rather than linking
 * -landroid so the shared Makefile.in stays portable to non-Android builds. */
typedef void (*pfn_AHardwareBuffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
typedef int  (*pfn_AHardwareBuffer_sendHandleToUnixSocket)(const AHardwareBuffer *, int);
typedef void (*pfn_AHardwareBuffer_release)(AHardwareBuffer *);

static pfn_AHardwareBuffer_describe               p_AHB_describe;
static pfn_AHardwareBuffer_sendHandleToUnixSocket p_AHB_send;
static pfn_AHardwareBuffer_release                p_AHB_release;

static int load_android_lib(void)
{
    void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { dc_log("dlopen(libandroid/libnativewindow) failed: %s", dlerror()); return 0; }

    p_AHB_describe = (pfn_AHardwareBuffer_describe)dlsym(lib, "AHardwareBuffer_describe");
    p_AHB_send     = (pfn_AHardwareBuffer_sendHandleToUnixSocket)dlsym(lib, "AHardwareBuffer_sendHandleToUnixSocket");
    p_AHB_release  = (pfn_AHardwareBuffer_release)dlsym(lib, "AHardwareBuffer_release");
    if (!p_AHB_send)
        dc_log("WARN: AHardwareBuffer_sendHandleToUnixSocket not found (AHB delivery will be skipped)");
    return 1;
}

/* ================================================================== X11 wire */

/* Robust "read exactly n bytes". Returns 1 on success, 0 on EOF/error. */
static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n)
    {
        ssize_t r = read(fd, p, n);
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

static int pad4(int n) { return (4 - (n & 3)) & 3; }

/* Best-effort MIT-MAGIC-COOKIE-1 lookup from $XAUTHORITY / ~/.Xauthority for the
 * given display number. Returns cookie length (bytes copied into out, up to
 * out_max) or 0 if none found. */
static int read_xauth_cookie(int display_num, uint8_t *out, int out_max)
{
    const char *path = getenv("XAUTHORITY");
    char home_path[512];
    if (!path || !*path)
    {
        const char *home = getenv("HOME");
        if (!home) return 0;
        snprintf(home_path, sizeof(home_path), "%s/.Xauthority", home);
        path = home_path;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int found = 0;
    while (!found)
    {
        uint16_t be;
        uint8_t addr[256], num[64], name[64], data[256];
        uint16_t addrlen, numlen, namelen, datalen;

        if (fread(&be, 2, 1, f) != 1) break;                        /* family */
        if (fread(&be, 2, 1, f) != 1) break; addrlen = (uint16_t)((be << 8) | (be >> 8));
        if (addrlen > sizeof(addr) || fread(addr, 1, addrlen, f) != addrlen) break;
        if (fread(&be, 2, 1, f) != 1) break; numlen  = (uint16_t)((be << 8) | (be >> 8));
        if (numlen  > sizeof(num)  || fread(num,  1, numlen,  f) != numlen)  break;
        if (fread(&be, 2, 1, f) != 1) break; namelen = (uint16_t)((be << 8) | (be >> 8));
        if (namelen > sizeof(name) || fread(name, 1, namelen, f) != namelen) break;
        if (fread(&be, 2, 1, f) != 1) break; datalen = (uint16_t)((be << 8) | (be >> 8));
        if (datalen > sizeof(data) || fread(data, 1, datalen, f) != datalen) break;

        int name_ok = (namelen == 18 && !memcmp(name, "MIT-MAGIC-COOKIE-1", 18));
        int num_ok  = 1;
        if (numlen > 0)
        {
            char ns[65]; int L = numlen < 64 ? numlen : 64;
            memcpy(ns, num, L); ns[L] = 0;
            num_ok = (atoi(ns) == display_num);
        }
        if (name_ok && num_ok && datalen <= out_max)
        {
            memcpy(out, data, datalen);
            found = datalen;
        }
    }
    fclose(f);
    return found;
}

/* Connect a filesystem AF_UNIX socket at `path`. Fresh fd per call (a failed
 * connect on a stream socket must not be reused). Returns fd or -1. */
static int x11_connect_fs(const char *path)
{
    if (!path || !*path) return -1;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) return fd;
    close(fd);
    return -1;
}

/* Connect an abstract-namespace AF_UNIX socket ("\0"+name). Returns fd or -1. */
static int x11_connect_abstract(const char *name)
{
    if (!name || !*name) return -1;
    size_t L = strlen(name);
    if (L + 1 > sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    sa.sun_path[0] = '\0';
    memcpy(sa.sun_path + 1, name, L);
    socklen_t sl = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + L);
    if (connect(fd, (struct sockaddr *)&sa, sl) == 0) return fd;
    close(fd);
    return -1;
}

/* Connect to the X server for display N.
 *
 * This build's Wine runs *natively* (no proot path translation): its env uses
 * absolute host paths, e.g. TMPDIR=/data/.../imagefs/usr/tmp, and its Xlib is
 * termux-patched to place the X socket under $TMPDIR/.X11-unix (NOT the standard
 * /tmp/.X11-unix). So we resolve the socket the SAME way the guest does --
 * $TMPDIR first -- then fall back through other plausible locations. Returns fd
 * or -1. */
static int x11_connect_socket(int display_num)
{
    char p[256];
    const char *tmpdir = getenv("TMPDIR");
    const char *xdg    = getenv("XDG_RUNTIME_DIR");
    const char *prefix = getenv("PREFIX");
    int fd;

    /* 1. $TMPDIR/.X11-unix/XN  -- exactly how the guest Xlib connects here. */
    if (tmpdir && *tmpdir)
    {
        snprintf(p, sizeof(p), "%s/.X11-unix/X%d", tmpdir, display_num);
        if ((fd = x11_connect_fs(p)) >= 0) { dc_log("X socket via TMPDIR: %s", p); return fd; }
    }
    /* 2. $XDG_RUNTIME_DIR/.X11-unix/XN */
    if (xdg && *xdg)
    {
        snprintf(p, sizeof(p), "%s/.X11-unix/X%d", xdg, display_num);
        if ((fd = x11_connect_fs(p)) >= 0) { dc_log("X socket via XDG_RUNTIME_DIR: %s", p); return fd; }
    }
    /* 3. $PREFIX/tmp/.X11-unix/XN */
    if (prefix && *prefix)
    {
        snprintf(p, sizeof(p), "%s/tmp/.X11-unix/X%d", prefix, display_num);
        if ((fd = x11_connect_fs(p)) >= 0) { dc_log("X socket via PREFIX: %s", p); return fd; }
    }
    /* 4/5. imagefs-relative and standard locations. */
    snprintf(p, sizeof(p), "/usr/tmp/.X11-unix/X%d", display_num);
    if ((fd = x11_connect_fs(p)) >= 0) { dc_log("X socket via %s", p); return fd; }
    snprintf(p, sizeof(p), "/tmp/.X11-unix/X%d", display_num);
    if ((fd = x11_connect_fs(p)) >= 0) { dc_log("X socket via %s", p); return fd; }

    /* 6. Abstract namespace (unused by this server, but cheap to try last). */
    snprintf(p, sizeof(p), "/tmp/.X11-unix/X%d", display_num);
    if ((fd = x11_connect_abstract(p)) >= 0) { dc_log("X socket via abstract @%s", p); return fd; }

    dc_log("could not connect X socket for display %d (tried TMPDIR/XDG/PREFIX/usr-tmp/tmp/abstract)", display_num);
    return -1;
}

/* Parsed pieces of the X11 setup reply we care about. */
struct x11_conn
{
    int      fd;
    uint32_t root;
    uint32_t rid_base;
    uint32_t rid_mask;
    uint32_t next_id;
};

/* Perform the X11 connection setup handshake (little-endian). auth_name/auth
 * may be empty. Returns 1 on Success and fills *root/rid_*; 0 otherwise. */
static int x11_do_setup(int fd, const char *auth_name, const uint8_t *auth, int authlen,
                        struct x11_conn *out)
{
    int name_len = auth_name ? (int)strlen(auth_name) : 0;

    uint8_t req[12];
    memset(req, 0, sizeof(req));
    req[0] = 0x6c;                 /* 'l' little-endian */
    req[2] = 11; req[3] = 0;       /* protocol-major 11 */
    req[4] = 0;  req[5] = 0;       /* protocol-minor 0  */
    req[6] = (uint8_t)(name_len & 0xff); req[7] = (uint8_t)(name_len >> 8);
    req[8] = (uint8_t)(authlen  & 0xff); req[9] = (uint8_t)(authlen  >> 8);

    uint8_t zpad[4] = {0,0,0,0};
    if (!write_full(fd, req, 12)) return 0;
    if (name_len && !write_full(fd, auth_name, name_len)) return 0;
    if (pad4(name_len) && !write_full(fd, zpad, pad4(name_len))) return 0;
    if (authlen && !write_full(fd, auth, authlen)) return 0;
    if (pad4(authlen) && !write_full(fd, zpad, pad4(authlen))) return 0;

    /* setup reply prefix: status(1), reason_len(1), major(2), minor(2), len(2) */
    uint8_t hdr[8];
    if (!read_full(fd, hdr, 8)) return 0;
    uint8_t status = hdr[0];
    uint16_t extra_units = (uint16_t)(hdr[6] | (hdr[7] << 8)); /* additional data in 4-byte units */

    if (status != 1)
    {
        /* Not Success: the caller closes this fd and retries on a fresh
         * connection, so there is no need to drain the reason string. */
        dc_log("X11 setup not Success (status=%u) auth_name='%s' authlen=%d",
               status, auth_name ? auth_name : "", authlen);
        return 0;
    }

    /* Read the remainder of the setup reply into a buffer and parse it. */
    size_t body = (size_t)extra_units * 4;
    uint8_t *b = malloc(body);
    if (!b) return 0;
    if (!read_full(fd, b, body)) { free(b); return 0; }

    /* Fixed part of the success body (Xproto SetupReply, after the 8-byte prefix):
     *   release(4), rid_base(4), rid_mask(4), motion_buf(4), vendor_len(2),
     *   max_req_len(2), num_screens(1), num_formats(1), img_byte_order(1),
     *   bitmap_bit_order(1), scanline_unit(1), scanline_pad(1),
     *   min_keycode(1), max_keycode(1), pad(4) = 32 bytes, then vendor (padded)
     *   + pixmap formats (8 bytes each) + SCREENs (each begins with root). */
    uint32_t rid_base = b[4]  | (b[5]<<8)  | (b[6]<<16)  | (b[7]<<24);
    uint32_t rid_mask = b[8]  | (b[9]<<8)  | (b[10]<<16) | (b[11]<<24);
    uint16_t vendor_len = (uint16_t)(b[16] | (b[17]<<8));
    uint8_t  num_screens = b[20];
    uint8_t  num_formats = b[21];

    size_t off = 32;
    off += vendor_len + pad4(vendor_len);   /* vendor string */
    off += (size_t)num_formats * 8;         /* pixmap formats (8 bytes each) */

    uint32_t root = 0;
    if (num_screens >= 1 && off + 4 <= body)
        root = b[off] | (b[off+1]<<8) | (b[off+2]<<16) | (b[off+3]<<24); /* SCREEN.root */

    free(b);

    out->fd = fd;
    out->root = root;
    out->rid_base = rid_base;
    out->rid_mask = rid_mask;
    out->next_id = 1;
    dc_log("X11 setup OK: root=0x%x rid_base=0x%x rid_mask=0x%x", root, rid_base, rid_mask);
    return (root != 0);
}

/* Open + authenticate an independent X connection. Tries cookie auth then
 * empty auth (reconnecting between). Returns 1 on success. */
static int x11_open(struct x11_conn *c)
{
    /* DISPLAY may be absent from some process envs even though the server is at
     * display 0 (observed on device), so default to 0 rather than bailing. */
    const char *disp = getenv("DISPLAY");
    const char *colon = disp ? strchr(disp, ':') : NULL;
    int display_num = colon ? atoi(colon + 1) : 0;
    dc_log("DISPLAY=%s -> display %d", (disp && *disp) ? disp : "(unset, assuming :0)", display_num);

    uint8_t cookie[256];
    int cookielen = read_xauth_cookie(display_num, cookie, sizeof(cookie));
    dc_log("xauth cookie: %s", cookielen ? "found" : "none");

    /* Attempt 1: empty auth. The app's Java X server ignores auth content
     * (it always marks the client authenticated), so this clean 12-byte setup
     * is the most robust path and avoids any Xauthority-parsing dependence. */
    {
        int fd = x11_connect_socket(display_num);
        if (fd >= 0 && x11_do_setup(fd, "", NULL, 0, c))
            return 1;
        if (fd >= 0) close(fd);
    }

    /* Attempt 2: MIT-MAGIC-COOKIE-1, in case we ever target a stricter server. */
    if (cookielen)
    {
        int fd = x11_connect_socket(display_num);
        if (fd >= 0 && x11_do_setup(fd, "MIT-MAGIC-COOKIE-1", cookie, cookielen, c))
            return 1;
        if (fd >= 0) close(fd);
    }

    dc_log("X11 open failed (both empty and cookie auth)");
    return 0;
}

/* Send a request and read back a 32-byte reply/error; returns first byte
 * (1=reply, 0=error, >=2=event) and copies the 32 bytes into rep. Handles the
 * variable-length reply tail by draining rep[4..7] extra 4-byte units. */
static int x11_request_reply(int fd, const void *req, size_t reqlen, uint8_t rep[32])
{
    if (!write_full(fd, req, reqlen)) return -1;
    for (;;)
    {
        if (!read_full(fd, rep, 32)) return -1;
        if (rep[0] == 1) /* reply -- drain any additional length */
        {
            uint32_t extra = rep[4] | (rep[5]<<8) | (rep[6]<<16) | (rep[7]<<24);
            while (extra--)
            {
                uint8_t junk[4];
                if (!read_full(fd, junk, 4)) return -1;
            }
            return 1;
        }
        if (rep[0] == 0) return 0;    /* error */
        /* else: an event -- ignore and keep reading for our reply */
    }
}

/* QueryExtension("DRI3") -> major opcode (0 if not present). */
static uint8_t x11_query_dri3_opcode(int fd)
{
    static const char name[] = "DRI3";
    int nl = (int)sizeof(name) - 1;
    uint8_t req[12]; /* 8-byte fixed header + 4 name bytes ("DRI3", pad 0) */
    int reqlen = 8 + nl + pad4(nl);                 /* = 12 (3 units) for "DRI3" */
    memset(req, 0, sizeof(req));
    req[0] = 98;                                    /* QueryExtension opcode */
    req[2] = (uint8_t)((reqlen/4) & 0xff);
    req[3] = (uint8_t)((reqlen/4) >> 8);
    req[4] = (uint8_t)(nl & 0xff); req[5] = (uint8_t)(nl >> 8);
    memcpy(req + 8, name, nl);

    uint8_t rep[32];
    if (x11_request_reply(fd, req, reqlen, rep) != 1) return 0;
    /* QueryExtension reply: present(1)@8, major_opcode(1)@9 */
    uint8_t present = rep[8];
    uint8_t major   = rep[9];
    dc_log("QueryExtension(DRI3): present=%u major_opcode=%u", present, major);
    return present ? major : 0;
}

/* QueryTree(root) -> topmost child (last in the returned list) or root itself
 * if there are no children. This is our "current present window" target. */
static uint32_t x11_present_window(int fd, uint32_t root)
{
    uint8_t req[8];
    memset(req, 0, sizeof(req));
    req[0] = 15;            /* QueryTree opcode */
    req[2] = 2; req[3] = 0; /* length = 2 units */
    req[4] = root & 0xff; req[5] = (root>>8)&0xff; req[6] = (root>>16)&0xff; req[7] = (root>>24)&0xff;

    if (!write_full(fd, req, 8)) return root;

    uint8_t rep[32];
    for (;;)
    {
        if (!read_full(fd, rep, 32)) return root;
        if (rep[0] == 0) return root;      /* error */
        if (rep[0] == 1) break;            /* reply */
        /* event: ignore */
    }
    uint32_t reply_units = rep[4] | (rep[5]<<8) | (rep[6]<<16) | (rep[7]<<24);
    uint16_t nchildren   = (uint16_t)(rep[16] | (rep[17]<<8));
    uint32_t chosen = root;
    for (uint32_t i = 0; i < reply_units; i++)
    {
        uint8_t w[4];
        if (!read_full(fd, w, 4)) return chosen;
        if (i < nchildren) /* children list is the reply tail */
            chosen = w[0] | (w[1]<<8) | (w[2]<<16) | (w[3]<<24); /* keep last => topmost */
    }
    dc_log("QueryTree(root=0x%x): %u children, present window=0x%x", root, nchildren, chosen);
    return chosen;
}

/* Send DRI3 PixmapFromBuffers (minor opcode 7) with exactly one buffer FD,
 * passing the FD as an SCM_RIGHTS ancillary on the X socket. The 60-byte body
 * layout matches the app's DRI3Extension.pixmapFromBuffers parser byte-for-byte
 * (interleaved stride0/offset0..stride3/offset3, then depth/bpp/pad, then the
 * 64-bit modifier). Returns 1 on send success. */
static int x11_send_pixmap_from_buffers(int fd, uint8_t dri3_major,
                                        uint32_t pixmap, uint32_t window,
                                        uint16_t w, uint16_t h, uint32_t stride,
                                        uint8_t depth, uint64_t modifier, int ahb_fd)
{
    uint8_t req[64];
    memset(req, 0, sizeof(req));
    /* request header */
    req[0] = dri3_major;   /* DRI3 major opcode */
    req[1] = 7;            /* PixmapFromBuffers minor opcode */
    req[2] = 16; req[3] = 0; /* length = 16 units (64 bytes) */
    /* pixmap, window */
    req[4]=pixmap&0xff; req[5]=(pixmap>>8)&0xff; req[6]=(pixmap>>16)&0xff; req[7]=(pixmap>>24)&0xff;
    req[8]=window&0xff; req[9]=(window>>8)&0xff; req[10]=(window>>16)&0xff; req[11]=(window>>24)&0xff;
    /* num_buffers(1) + 3 pad @12 */
    req[12] = 1;
    /* width(2) @16, height(2) @18 */
    req[16]=w&0xff; req[17]=(w>>8)&0xff;
    req[18]=h&0xff; req[19]=(h>>8)&0xff;
    /* stride0 @20, offset0 @24 (buffers 1..3 left zero) */
    req[20]=stride&0xff; req[21]=(stride>>8)&0xff; req[22]=(stride>>16)&0xff; req[23]=(stride>>24)&0xff;
    /* depth(1) @52, bpp(1) @53, pad(2) */
    req[52] = depth;
    req[53] = 32; /* bpp */
    /* modifier(8) @56 */
    for (int i = 0; i < 8; i++) req[56+i] = (uint8_t)((modifier >> (8*i)) & 0xff);

    struct iovec iov = { req, sizeof(req) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &ahb_fd, sizeof(int));

    ssize_t n;
    do { n = sendmsg(fd, &msg, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) { dc_log("sendmsg(PixmapFromBuffers) failed: %s", strerror(errno)); return 0; }
    dc_log("DRI3 PixmapFromBuffers sent (pixmap=0x%x window=0x%x %ux%u modifier=0x%llx fd=%d)",
           pixmap, window, w, h, (unsigned long long)modifier, ahb_fd);
    return 1;
}

/* Round-trip GetInputFocus to detect a protocol error from the previous
 * (reply-less) PixmapFromBuffers and to confirm the server is still alive. */
static void x11_check_accepted(int fd)
{
    uint8_t req[4] = { 43 /*GetInputFocus*/, 0, 1, 0 };
    uint8_t rep[32];
    int r = x11_request_reply(fd, req, sizeof(req), rep);
    if (r == 1)      dc_log("X server accepted the pixmap request (post round-trip OK)");
    else if (r == 0) dc_log("X server returned a protocol ERROR (code=%u, seq=%u) -- request rejected",
                            rep[1], (unsigned)(rep[2] | (rep[3]<<8)));
    else             dc_log("X connection lost after PixmapFromBuffers");
}

/* =============================================================== Vulkan probe */

/* Everything we resolve out of the throwaway host instance/device. */
struct vk_probe
{
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr   gdpa;
    VkInstance                instance;
    VkPhysicalDevice          phys;
    VkDevice                  device;

    PFN_vkGetPhysicalDeviceMemoryProperties         GetPhysDevMemProps;
    PFN_vkCreateImage                               CreateImage;
    PFN_vkGetImageMemoryRequirements                GetImageMemReq;
    PFN_vkAllocateMemory                            AllocMemory;
    PFN_vkBindImageMemory                           BindImageMemory;
    PFN_vkDestroyImage                              DestroyImage;
    PFN_vkFreeMemory                                FreeMemory;
    PFN_vkDestroyDevice                             DestroyDevice;
    PFN_vkGetMemoryAndroidHardwareBufferANDROID     GetMemAHB;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAHBProps;
};

static int pick_memory_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u<<i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (bits & (1u<<i)) return (int)i;
    return -1;
}

/* Try to create + AHB-export a 16x16 image of the given format. On success
 * fills *out_ahb and returns 1; the caller owns the AHB (release it). */
static int try_export_format(struct vk_probe *p, VkFormat format, const char *fmt_name,
                             AHardwareBuffer **out_ahb)
{
    VkExternalMemoryImageCreateInfo ext_img = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { 16, 16, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage image = VK_NULL_HANDLE;
    VkResult r = p->CreateImage(p->device, &ici, NULL, &image);
    if (r != VK_SUCCESS) { dc_log("  [%s] vkCreateImage failed (%d)", fmt_name, r); return 0; }

    VkMemoryRequirements mr;
    p->GetImageMemReq(p->device, image, &mr);

    VkPhysicalDeviceMemoryProperties mp;
    p->GetPhysDevMemProps(p->phys, &mp);
    int mt = pick_memory_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { dc_log("  [%s] no memory type", fmt_name); p->DestroyImage(p->device, image, NULL); return 0; }

    VkExportMemoryAllocateInfo exp = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkMemoryDedicatedAllocateInfo ded = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &exp,
        .image = image,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &ded,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = p->AllocMemory(p->device, &mai, NULL, &mem);
    if (r != VK_SUCCESS) { dc_log("  [%s] vkAllocateMemory(export AHB) failed (%d)", fmt_name, r);
                           p->DestroyImage(p->device, image, NULL); return 0; }

    r = p->BindImageMemory(p->device, image, mem, 0);
    if (r != VK_SUCCESS) { dc_log("  [%s] vkBindImageMemory failed (%d)", fmt_name, r);
                           p->FreeMemory(p->device, mem, NULL); p->DestroyImage(p->device, image, NULL); return 0; }

    VkMemoryGetAndroidHardwareBufferInfoANDROID gi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .memory = mem,
    };
    AHardwareBuffer *ahb = NULL;
    r = p->GetMemAHB(p->device, &gi, &ahb);
    if (r != VK_SUCCESS || !ahb)
    {
        dc_log("  [%s] vkGetMemoryAndroidHardwareBufferANDROID failed (res=%d ahb=%p)", fmt_name, r, (void*)ahb);
        p->FreeMemory(p->device, mem, NULL); p->DestroyImage(p->device, image, NULL);
        return 0;
    }

    dc_log("  [%s] EXPORT OK: got AHardwareBuffer=%p", fmt_name, (void*)ahb);
    if (p_AHB_describe)
    {
        AHardwareBuffer_Desc d; memset(&d, 0, sizeof(d));
        p_AHB_describe(ahb, &d);
        dc_log("  [%s] AHB desc: %ux%u layers=%u fmt=0x%x usage=0x%llx stride=%u",
               fmt_name, d.width, d.height, d.layers, d.format,
               (unsigned long long)d.usage, d.stride);
    }

    /* Read back the Vulkan-visible format properties -- this is the Gate 1
     * R32F-vs-RGBA8 decision. externalFormat==0 means it maps to a normal
     * VkFormat (round-trips); nonzero means it needs an external format. */
    if (p->GetAHBProps)
    {
        VkAndroidHardwareBufferFormatPropertiesANDROID fp = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID,
        };
        VkAndroidHardwareBufferPropertiesANDROID ap = {
            .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID,
            .pNext = &fp,
        };
        r = p->GetAHBProps(p->device, ahb, &ap);
        if (r == VK_SUCCESS)
            dc_log("  [%s] AHB format props: VkFormat=%d externalFormat=0x%llx allocSize=%llu -> %s",
                   fmt_name, fp.format, (unsigned long long)fp.externalFormat,
                   (unsigned long long)ap.allocationSize,
                   (fp.format != VK_FORMAT_UNDEFINED && fp.externalFormat == 0)
                       ? "round-trips as a native VkFormat" : "needs VkExternalFormatANDROID");
        else
            dc_log("  [%s] vkGetAndroidHardwareBufferPropertiesANDROID failed (%d)", fmt_name, r);
    }

    /* Free the Vulkan image/memory now; the AHB holds its own ref so it (and its
     * underlying dma-buf) survives for delivery. Caller releases the AHB. */
    p->FreeMemory(p->device, mem, NULL);
    p->DestroyImage(p->device, image, NULL);
    *out_ahb = ahb;
    return 1;
}

/* Create the throwaway host instance + device on the wrapper, enabling the AHB
 * extension (+ its deps). Returns 1 on success. */
static int vk_bring_up(struct vk_probe *p)
{
    /* --- instance (API 1.1 so the AHB deps are core) --- */
    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "sgsr2-depth-courier",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };

    PFN_vkCreateInstance CreateInstance =
        (PFN_vkCreateInstance)p->gipa(NULL, "vkCreateInstance");
    if (!CreateInstance) { dc_log("host vkCreateInstance not found"); return 0; }
    VkResult r = CreateInstance(&ici, NULL, &p->instance);
    if (r != VK_SUCCESS) { dc_log("probe vkCreateInstance failed (%d)", r); return 0; }

    p->gdpa = (PFN_vkGetDeviceProcAddr)p->gipa(p->instance, "vkGetDeviceProcAddr");
    PFN_vkEnumeratePhysicalDevices EnumPhys =
        (PFN_vkEnumeratePhysicalDevices)p->gipa(p->instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties GetProps =
        (PFN_vkGetPhysicalDeviceProperties)p->gipa(p->instance, "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateDeviceExtensionProperties EnumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties)p->gipa(p->instance, "vkEnumerateDeviceExtensionProperties");
    p->GetPhysDevMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)p->gipa(p->instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!EnumPhys || !GetProps || !EnumDevExt) { dc_log("missing instance procs"); return 0; }

    uint32_t n = 0;
    EnumPhys(p->instance, &n, NULL);
    if (!n) { dc_log("no physical devices"); return 0; }
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    EnumPhys(p->instance, &n, devs);

    /* Select the wrapper's device (name contains "Wrapper"/"Adreno"), logging all. */
    int sel = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        VkPhysicalDeviceProperties props;
        GetProps(devs[i], &props);
        dc_log("physical device[%u]: '%s' (type=%d)", i, props.deviceName, props.deviceType);
        if (strstr(props.deviceName, "Wrapper") || strstr(props.deviceName, "Adreno"))
            sel = (int)i;
    }
    p->phys = devs[sel];

    /* --- enumerate device extensions; report AHB presence (Gate 0 step a) --- */
    uint32_t ec = 0;
    EnumDevExt(p->phys, NULL, &ec, NULL);
    VkExtensionProperties *exts = calloc(ec ? ec : 1, sizeof(*exts));
    EnumDevExt(p->phys, NULL, &ec, exts);

    int have_ahb = 0, have_qff = 0, have_ext_mem = 0, have_ded = 0, have_ycbcr = 0;
    for (uint32_t i = 0; i < ec; i++)
    {
        const char *e = exts[i].extensionName;
        if (!strcmp(e, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) have_ahb = 1;
        else if (!strcmp(e, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))                   have_qff = 1;
        else if (!strcmp(e, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME))                        have_ext_mem = 1;
        else if (!strcmp(e, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME))                   have_ded = 1;
        else if (!strcmp(e, VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME))               have_ycbcr = 1;
    }

    dc_log("================ GATE 0 STEP (a) ================");
    dc_log("wrapper physdev advertises VK_ANDROID_external_memory_android_hardware_buffer: %s",
           have_ahb ? "YES (transport is possible)" : "NO -- MOONSHOT DEAD");
    if (!have_ahb) { free(exts); return 0; }

    /* --- device with AHB (+ present deps) enabled --- */
    const char *want[8]; uint32_t wc = 0;
    want[wc++] = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
    if (have_qff)     want[wc++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
    if (have_ext_mem) want[wc++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
    if (have_ded)     want[wc++] = VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME;
    if (have_ycbcr)   want[wc++] = VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME;
    free(exts);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = wc,
        .ppEnabledExtensionNames = want,
    };
    PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)p->gipa(p->instance, "vkCreateDevice");
    r = CreateDevice(p->phys, &dci, NULL, &p->device);
    if (r != VK_SUCCESS) { dc_log("probe vkCreateDevice failed (%d)", r); return 0; }

#define GD(field, name) p->field = (PFN_##name)p->gdpa(p->device, #name)
    GD(CreateImage,     vkCreateImage);
    GD(GetImageMemReq,  vkGetImageMemoryRequirements);
    GD(AllocMemory,     vkAllocateMemory);
    GD(BindImageMemory, vkBindImageMemory);
    GD(DestroyImage,    vkDestroyImage);
    GD(FreeMemory,      vkFreeMemory);
    GD(DestroyDevice,   vkDestroyDevice);
    GD(GetMemAHB,       vkGetMemoryAndroidHardwareBufferANDROID);
    GD(GetAHBProps,     vkGetAndroidHardwareBufferPropertiesANDROID);
#undef GD
    if (!p->CreateImage || !p->AllocMemory || !p->GetMemAHB)
    { dc_log("missing device procs (AHB export unavailable)"); return 0; }

    return 1;
}

static void vk_tear_down(struct vk_probe *p)
{
    if (p->device && p->DestroyDevice) p->DestroyDevice(p->device, NULL);
    if (p->instance)
    {
        PFN_vkDestroyInstance DestroyInstance =
            (PFN_vkDestroyInstance)p->gipa(p->instance, "vkDestroyInstance");
        if (DestroyInstance) DestroyInstance(p->instance, NULL);
    }
}

/* ============================================================= probe thread */

static void *courier_thread(void *arg)
{
    struct vk_probe p;
    memset(&p, 0, sizeof(p));
    p.gipa = (PFN_vkGetInstanceProcAddr)arg;

    dc_log("==================================================================");
    dc_log("SGSR2 Gate 0 depth-courier probe starting (WINE_DEPTH_COURIER set)");

    void *log = dlopen("liblog.so", RTLD_NOW);
    if (log) p_android_log_print = (pfn_android_log_print)dlsym(log, "__android_log_print");
    load_android_lib();

    if (!vk_bring_up(&p)) { dc_log("probe: bring-up/step-(a) did not pass; stopping"); vk_tear_down(&p); return NULL; }

    /* ---- Gate 0 step (b): AHB export + format decision ---- */
    dc_log("================ GATE 0 STEP (b) ================");
    AHardwareBuffer *ahb = NULL;
    const char *chosen = NULL;
    if (try_export_format(&p, VK_FORMAT_R32_SFLOAT, "R32_SFLOAT", &ahb))
        chosen = "R32_SFLOAT";
    else if (try_export_format(&p, VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", &ahb))
        chosen = "R8G8B8A8_UNORM (RGBA8-pack)";

    if (!ahb) { dc_log("step (b) FAIL: could not export any AHB-backed image"); vk_tear_down(&p); return NULL; }
    dc_log("step (b) PASS: exported an AHB (Gate 1 depth format = %s)", chosen);

    /* ---- Gate 0 step (c): DRI3 hand-off to the Java X server ---- */
    dc_log("================ GATE 0 STEP (c) ================");
    struct x11_conn c; memset(&c, 0, sizeof(c));
    if (x11_open(&c))
    {
        uint8_t dri3 = x11_query_dri3_opcode(c.fd);
        if (dri3)
        {
            uint32_t window = x11_present_window(c.fd, c.root);
            uint32_t pixmap = c.rid_base | (c.next_id++ & c.rid_mask);

            int sv[2] = { -1, -1 };
            if (p_AHB_send && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0)
            {
                uint32_t frame_id = 1; /* synthetic Gate 0 frame id */
                uint64_t modifier = ((uint64_t)frame_id << 32) | 1256ULL;

                if (x11_send_pixmap_from_buffers(c.fd, dri3, pixmap, window,
                                                 16, 16, 16 * 4 /*stride*/, 32 /*depth*/,
                                                 modifier, sv[1]))
                {
                    close(sv[1]); /* server owns its copy now */
                    x11_check_accepted(c.fd);

                    /* app writes 1 ack byte, then recvHandleFromUnixSocket */
                    struct pollfd pf = { sv[0], POLLIN, 0 };
                    if (poll(&pf, 1, 3000) > 0 && (pf.revents & POLLIN))
                    {
                        uint8_t ack = 0;
                        if (read(sv[0], &ack, 1) == 1)
                        {
                            int sr = p_AHB_send(ahb, sv[0]);
                            dc_log("received app ack; AHardwareBuffer_sendHandleToUnixSocket -> %d (%s)",
                                   sr, sr == 0 ? "AHB delivered" : "send failed");
                        }
                        else dc_log("app ack read failed");
                    }
                    else dc_log("timed out waiting for app ack on the AHB socket (app stub not wired?)");
                }
                close(sv[0]);
            }
            else
            {
                /* No AHB sender available: still exercise the DRI3 path with a
                 * dup of the AHB socket end so we prove the wire is accepted. */
                dc_log("AHB send unavailable/socketpair failed; sending DRI3 with a placeholder fd only");
                int placeholder = dup(c.fd);
                uint64_t modifier = ((uint64_t)1u << 32) | 1256ULL;
                if (placeholder >= 0 &&
                    x11_send_pixmap_from_buffers(c.fd, dri3, pixmap, window, 16, 16, 64, 32, modifier, placeholder))
                    x11_check_accepted(c.fd);
                if (placeholder >= 0) close(placeholder);
            }
        }
        else dc_log("DRI3 extension not present on this X server -- cannot hand off");
        close(c.fd);
    }

    if (p_AHB_release) p_AHB_release(ahb);
    vk_tear_down(&p);
    dc_log("SGSR2 Gate 0 depth-courier probe finished");
    dc_log("==================================================================");
    return NULL;
}

/* --------------------------------------------------------------- public API */

static void *g_saved_gipa;

static void dc_launch_once(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, courier_thread, g_saved_gipa) == 0)
        pthread_detach(t);
    else
        dc_log("failed to spawn courier thread");
}

void depth_courier_maybe_start(void *host_get_instance_proc_addr)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    const char *env = getenv("WINE_DEPTH_COURIER");
    if (!env || !*env || !atoi(env)) return;      /* INERT by default */
    if (!host_get_instance_proc_addr) return;

    g_saved_gipa = host_get_instance_proc_addr;

    /* Spawn the detached worker at most once per process; it never blocks the
     * guest's own Vulkan calls. */
    pthread_once(&once, dc_launch_once);
}
