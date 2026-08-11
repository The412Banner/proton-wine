/*
 * Unixlib for the DirectAudio driver (native Wine -> Android AAudio).
 *
 * mmdevapi owns IAudioClient/IAudioRenderClient; this backend only implements
 * the mmdevapi driver unixlib vtable (see ../mmdevapi/unixlib.h) against AAudio,
 * with no PulseAudio daemon and no ALSA aserver in the path.
 *
 * Structure and the mmdevapi ring-buffer bookkeeping are modelled on
 * winecoreaudio.drv (single native backend, render callback ~ AAudio callback);
 * the CoreAudio device layer is replaced with AAudio.
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
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <aaudio/AAudio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "mmdeviceapi.h"
#include "initguid.h"
#include "audioclient.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "../mmdevapi/unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(directaudio);

struct directaudio_stream
{
    pthread_mutex_t lock;
    AAudioStream *aq;             /* current AAudio output stream */

    EDataFlow flow;
    DWORD flags;
    AUDCLNT_SHAREMODE share;
    HANDLE event;

    BOOL playing, please_quit;
    REFERENCE_TIME period;
    UINT32 period_frames;
    UINT32 bufsize_frames;
    UINT32 lcl_offs_frames, held_frames, wri_offs_frames, tmp_buffer_frames;
    UINT64 written_frames;
    INT32 getbuf_last;
    WAVEFORMATEX *fmt;
    BYTE *local_buffer, *tmp_buffer;

    /* AAudio open parameters, kept so the stream can be re-opened on a route
     * change (headphone/BT/HDMI) without losing the guest's negotiated format. */
    aaudio_format_t aa_format;
    int32_t aa_channels, aa_rate;
    aaudio_performance_mode_t aa_perf;

    /* adaptive buffer control (ported from the ALSA/PA adaptive stacks) */
    BOOL adaptive;
    int32_t target_buf_frames;   /* initial buffer target, 0 = leave default */
    int32_t max_buf_frames;      /* cap for adaptive growth, 0 = capacity */
    int32_t last_xrun;

    /* software gain (AAudio NDK has no per-stream volume) */
    float vols[8];
    BOOL vols_active;

    /* route-follow reopen guard */
    int need_reopen;

    /* in-process mixer voice state */
    int in_mixer;        /* registered with the shared output mixer */
    double rs_pos;       /* fractional resample read position (voice rate != 48k) */
    BOOL is_float;       /* source samples are 32-bit float (else PCM by wBitsPerSample) */

    /* measurement */
    unsigned int cb_count;
};

static const REFERENCE_TIME def_period = 100000;
static const REFERENCE_TIME min_period = 50000;

static ULONG_PTR zero_bits = 0;

static NTSTATUS unix_not_implemented(void *args)
{
    return STATUS_SUCCESS;
}

/* We provide no MIDI backend, but mmdevapi's midMessage/modMessage leave the
 * notify_context on the stack UNINITIALISED and fire notify_client() whenever
 * notify->send_notify is non-zero. A plain not-implemented stub never clears it,
 * so we would deliver a bogus DriverCallback (garbage msg/params) into the guest
 * during its legacy-winmm probe - which winealsa/winepulse never do because
 * their handlers always set send_notify = FALSE. Mirror that here. */
static NTSTATUS unix_midi_out_message(void *args)
{
    struct midi_out_message_params *params = args;
    if (params->notify) params->notify->send_notify = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_midi_in_message(void *args)
{
    struct midi_in_message_params *params = args;
    if (params->notify) params->notify->send_notify = FALSE;
    return STATUS_SUCCESS;
}

static struct directaudio_stream *handle_get_stream(stream_handle h)
{
    return (struct directaudio_stream *)(UINT_PTR)h;
}

/* copied from kernelbase */
static int muldiv(int a, int b, int c)
{
    LONGLONG ret;

    if (!c) return -1;

    if (c < 0)
    {
        a = -a;
        c = -c;
    }

    if ((a < 0 && b < 0) || (a >= 0 && b >= 0))
        ret = (((LONGLONG)a * b) + (c / 2)) / c;
    else
        ret = (((LONGLONG)a * b) - (c / 2)) / c;

    if (ret > 2147483647 || ret < -2147483647) return -1;
    return ret;
}

static WAVEFORMATEX *clone_format(const WAVEFORMATEX *fmt)
{
    WAVEFORMATEX *ret;
    size_t size;

    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        size = sizeof(WAVEFORMATEXTENSIBLE);
    else
        size = sizeof(WAVEFORMATEX);

    ret = malloc(size);
    if (!ret) return NULL;

    memcpy(ret, fmt, size);
    ret->cbSize = size - sizeof(WAVEFORMATEX);
    return ret;
}

static void silence_buffer(struct directaudio_stream *stream, BYTE *buffer, UINT32 frames)
{
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE *)stream->fmt;
    if ((stream->fmt->wFormatTag == WAVE_FORMAT_PCM ||
         (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))) &&
        stream->fmt->wBitsPerSample == 8)
        memset(buffer, 128, frames * stream->fmt->nBlockAlign);
    else
        memset(buffer, 0, frames * stream->fmt->nBlockAlign);
}

static aaudio_format_t fmt_to_aaudio(const WAVEFORMATEX *fmt)
{
    const WAVEFORMATEXTENSIBLE *fex = (const WAVEFORMATEXTENSIBLE *)fmt;
    BOOL is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&fex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    BOOL is_pcm = fmt->wFormatTag == WAVE_FORMAT_PCM ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&fex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM));

    if (is_float && fmt->wBitsPerSample == 32) return AAUDIO_FORMAT_PCM_FLOAT;
    if (is_pcm && fmt->wBitsPerSample == 16) return AAUDIO_FORMAT_PCM_I16;
    if (is_pcm && fmt->wBitsPerSample == 32) return AAUDIO_FORMAT_PCM_I32;
    return AAUDIO_FORMAT_UNSPECIFIED;
}

/* ---- in-process software mixer -------------------------------------------
 * DirectAudio opens exactly ONE AAudio output stream for the whole process and
 * sums every guest render stream ("voice") into it, instead of one AAudio stream
 * per guest stream. Many concurrent AAudio streams, each fed by its own callback
 * under box64/FEX load, drift and underrun independently and phase at
 * AudioFlinger - heard as choppy/echo on multi-stream games (DiRT Showdown opens
 * 5). One mixed output = one callback and one buffer to keep full, with the final
 * hardware mix still done by AudioFlinger. This is what pulse/alsa get from their
 * daemon's mixer, but done in-process here with no daemon and no IPC.
 * Output is fixed 48 kHz / float / stereo (our advertised shared mix format);
 * each voice is format-converted, channel down/up-mixed to stereo, and linearly
 * resampled if its rate differs, as it is summed in. */

#define MIX_OUT_RATE     48000
#define MIX_OUT_CHANNELS 2
#define MIX_MAX_VOICES   64

struct directaudio_mixer
{
    pthread_mutex_t lock;
    AAudioStream *aq;
    struct directaudio_stream *voices[MIX_MAX_VOICES];
    int nvoices;
    aaudio_performance_mode_t perf;
    BOOL adaptive;
    int32_t max_buf_frames, target_buf_frames;
    int32_t last_xrun;
    int need_reopen;
    unsigned int cb_count;
};

static struct directaudio_mixer g_mixer = { PTHREAD_MUTEX_INITIALIZER };

/* one source sample of channel c -> float in [-1,1] */
static inline float samp_to_float(const struct directaudio_stream *v, const BYTE *frame, int c)
{
    if (v->is_float)
        return ((const float *)frame)[c];

    switch (v->fmt->wBitsPerSample)
    {
    case 16: return ((const INT16 *)frame)[c] * (1.0f / 32768.0f);
    case 32: return ((const INT32 *)frame)[c] * (1.0f / 2147483648.0f);
    case 8:  return (((const BYTE *)frame)[c] - 128) * (1.0f / 128.0f);
    case 24:
    {
        const BYTE *p = frame + c * 3;
        INT32 s = p[0] | (p[1] << 8) | (p[2] << 16);
        if (s & 0x800000) s |= ~0xffffff;
        return s * (1.0f / 8388608.0f);
    }
    default: return 0.0f;
    }
}

/* down/up-mix an N-channel source frame to stereo. WAVEFORMATEX channel order:
 * mono; L R; 5.1 = FL FR C LFE BL BR; 7.1 = FL FR C LFE BL BR SL SR. */
static inline void downmix_stereo(int ch, const float *s, float *L, float *R)
{
    const float c = 0.7071f;
    switch (ch)
    {
    case 1:  *L = *R = s[0]; return;
    case 2:  *L = s[0]; *R = s[1]; return;
    case 3:  *L = s[0]; *R = s[1]; return;                    /* 2.1: drop LFE */
    case 4:  *L = s[0] + c*s[2]; *R = s[1] + c*s[3]; return;  /* quad */
    case 6:  *L = s[0] + c*s[2] + c*s[4];                     /* 5.1 */
             *R = s[1] + c*s[2] + c*s[5]; return;
    case 8:  *L = s[0] + c*s[2] + c*s[4] + c*s[6];            /* 7.1 */
             *R = s[1] + c*s[2] + c*s[5] + c*s[7]; return;
    default: *L = s[0]; *R = (ch > 1) ? s[1] : s[0]; return;
    }
}

/* read the voice frame at (lcl_offs+idx), down-mixed to stereo float */
static inline void voice_frame_stereo(struct directaudio_stream *v, UINT32 idx,
                                      float *L, float *R)
{
    UINT32 pos = (v->lcl_offs_frames + idx) % v->bufsize_frames;
    const BYTE *frame = v->local_buffer + (size_t)pos * v->fmt->nBlockAlign;
    int ch = v->fmt->nChannels, c;
    float s[8];

    if (ch > 8) ch = 8;
    for (c = 0; c < ch; c++)
    {
        float val = samp_to_float(v, frame, c);
        if (v->vols_active) val *= v->vols[c < 8 ? c : 7];
        s[c] = val;
    }
    downmix_stereo(ch, s, L, R);
}

/* sum one voice into the stereo float mix buffer (out holds numFrames*2 floats) */
static void mix_voice(struct directaudio_stream *v, float *out, int32_t numFrames)
{
    pthread_mutex_lock(&v->lock);

    if (!v->playing || v->held_frames == 0)
    {
        pthread_mutex_unlock(&v->lock);
        return;
    }

    if (v->aa_rate == MIX_OUT_RATE)
    {
        UINT32 n = min((UINT32)numFrames, v->held_frames), f;
        for (f = 0; f < n; f++)
        {
            float L, R;
            voice_frame_stereo(v, f, &L, &R);
            out[2*f]     += L;
            out[2*f + 1] += R;
        }
        v->lcl_offs_frames = (v->lcl_offs_frames + n) % v->bufsize_frames;
        v->held_frames -= n;
    }
    else /* linear resample voice rate -> 48 kHz */
    {
        double ratio = (double)v->aa_rate / MIX_OUT_RATE;
        int32_t f, consumed;
        for (f = 0; f < numFrames; f++)
        {
            UINT32 i0 = (UINT32)v->rs_pos;
            float L0, R0, L1, R1;
            double frac;

            if (i0 + 1 >= v->held_frames) break;   /* ran dry - remaining stays silent */
            voice_frame_stereo(v, i0, &L0, &R0);
            voice_frame_stereo(v, i0 + 1, &L1, &R1);
            frac = v->rs_pos - i0;
            out[2*f]     += (float)(L0 + (L1 - L0) * frac);
            out[2*f + 1] += (float)(R0 + (R1 - R0) * frac);
            v->rs_pos += ratio;
        }
        consumed = (int32_t)v->rs_pos;
        if (consumed > (int32_t)v->held_frames) consumed = v->held_frames;
        if (consumed > 0)
        {
            v->lcl_offs_frames = (v->lcl_offs_frames + consumed) % v->bufsize_frames;
            v->held_frames -= consumed;
            v->rs_pos -= consumed;
        }
        if (v->held_frames == 0) v->rs_pos = 0.0;  /* resync fraction on underrun */
    }

    pthread_mutex_unlock(&v->lock);
}

/* AAudio pulls from the single mixed output on its high-priority audio thread. */
static aaudio_data_callback_result_t mixer_cb(AAudioStream *aq, void *user,
                                              void *audioData, int32_t numFrames)
{
    struct directaudio_mixer *mx = user;
    float *out = audioData;
    int32_t i, n2 = numFrames * MIX_OUT_CHANNELS;

    memset(out, 0, (size_t)n2 * sizeof(float));

    pthread_mutex_lock(&mx->lock);
    for (i = 0; i < mx->nvoices; i++)
        mix_voice(mx->voices[i], out, numFrames);
    pthread_mutex_unlock(&mx->lock);

    for (i = 0; i < n2; i++)
    {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }

    /* Adaptive: grow the single output's buffer by a burst on each xrun climb,
     * capped at max_buf_frames (or capacity). One output = one xrun source. */
    if (mx->adaptive)
    {
        int32_t xruns = AAudioStream_getXRunCount(aq);
        if (xruns > mx->last_xrun)
        {
            int32_t burst = AAudioStream_getFramesPerBurst(aq);
            int32_t cur = AAudioStream_getBufferSizeInFrames(aq);
            int32_t cap = AAudioStream_getBufferCapacityInFrames(aq);
            int32_t want = cur + (burst > 0 ? burst : 1);

            if (mx->max_buf_frames > 0 && want > mx->max_buf_frames) want = mx->max_buf_frames;
            if (want > cap) want = cap;
            if (want > cur) AAudioStream_setBufferSizeInFrames(aq, want);
            mx->last_xrun = xruns;
        }
    }

    if (TRACE_ON(directaudio) && !(++mx->cb_count % 1000))
        TRACE("mix hb: cb=%u voices=%d buf=%d cap=%d xruns=%d\n", mx->cb_count, mx->nvoices,
              AAudioStream_getBufferSizeInFrames(aq), AAudioStream_getBufferCapacityInFrames(aq),
              AAudioStream_getXRunCount(aq));

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void *mixer_reopen_thread(void *user);

static void mixer_error_cb(AAudioStream *aq, void *user, aaudio_result_t error)
{
    struct directaudio_mixer *mx = user;
    int expected = 0;

    if (error != AAUDIO_ERROR_DISCONNECTED)
        return;

    /* Route change (headphone/BT/HDMI). AAudio requires the reopen to happen on
     * another thread, never inside this callback. */
    if (__atomic_compare_exchange_n(&mx->need_reopen, &expected, 1, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
        pthread_t th;
        WARN("route change (err=%d) - reopening mixer output\n", error);
        if (pthread_create(&th, NULL, mixer_reopen_thread, mx))
            __atomic_store_n(&mx->need_reopen, 0, __ATOMIC_SEQ_CST);
        else
            pthread_detach(th);
    }
}

/* open the one shared AAudio output: 48 kHz / float / stereo */
static aaudio_result_t mixer_open_stream(struct directaudio_mixer *mx, AAudioStream **out)
{
    AAudioStreamBuilder *builder = NULL;
    AAudioStream *aq = NULL;
    aaudio_result_t r;

    r = AAudio_createStreamBuilder(&builder);
    if (r != AAUDIO_OK || !builder)
        return r != AAUDIO_OK ? r : AAUDIO_ERROR_NO_MEMORY;

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, mx->perf);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, MIX_OUT_CHANNELS);
    AAudioStreamBuilder_setSampleRate(builder, MIX_OUT_RATE);
    AAudioStreamBuilder_setDataCallback(builder, mixer_cb, mx);
    AAudioStreamBuilder_setErrorCallback(builder, mixer_error_cb, mx);

    /* Capacity headroom so adaptive has room to grow (builder-time only). One
     * output = far fewer xruns than N per-stream outputs, so a moderate initial
     * buffer stays low-latency. BANNER_AUDIO_DIRECT_MBF/_BF override. */
    {
        int32_t cap_req = mx->max_buf_frames > 0 ? mx->max_buf_frames : MIX_OUT_RATE / 4; /* ~250 ms */
        if (cap_req > 0)
            AAudioStreamBuilder_setBufferCapacityInFrames(builder, cap_req);
    }

    r = AAudioStreamBuilder_openStream(builder, &aq);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK || !aq)
        return r != AAUDIO_OK ? r : AAUDIO_ERROR_INTERNAL;

    {
        int32_t cap = AAudioStream_getBufferCapacityInFrames(aq);
        int32_t want = mx->target_buf_frames > 0 ? mx->target_buf_frames : MIX_OUT_RATE / 16; /* ~60 ms */
        if (want > cap) want = cap;
        if (want > 0)
            AAudioStream_setBufferSizeInFrames(aq, want);
    }
    mx->last_xrun = AAudioStream_getXRunCount(aq);

    r = AAudioStream_requestStart(aq);
    if (r != AAUDIO_OK)
    {
        AAudioStream_close(aq);
        return r;
    }

    TRACE("mixer open: 48000/float/2ch perf=%d burst=%d buf=%d cap=%d\n", mx->perf,
          AAudioStream_getFramesPerBurst(aq), AAudioStream_getBufferSizeInFrames(aq),
          AAudioStream_getBufferCapacityInFrames(aq));

    *out = aq;
    return AAUDIO_OK;
}

static void *mixer_reopen_thread(void *user)
{
    struct directaudio_mixer *mx = user;
    AAudioStream *old = NULL, *neu = NULL;

    if (mixer_open_stream(mx, &neu) == AAUDIO_OK)
    {
        pthread_mutex_lock(&mx->lock);
        old = mx->aq;
        mx->aq = neu;
        pthread_mutex_unlock(&mx->lock);
    }
    else
        WARN("mixer route-change reopen failed; keeping old stream\n");

    /* close() blocks until the old stream's in-flight callback returns; do it
     * outside the lock so the callback can drain. */
    if (old)
    {
        AAudioStream_requestStop(old);
        AAudioStream_close(old);
        TRACE("reopened mixer output on route change\n");
    }

    __atomic_store_n(&mx->need_reopen, 0, __ATOMIC_SEQ_CST);
    return NULL;
}

/* open the shared output on the first voice, using that voice's env-derived config */
static aaudio_result_t mixer_ensure_open(struct directaudio_mixer *mx,
                                         const struct directaudio_stream *cfg)
{
    if (mx->aq)
        return AAUDIO_OK;
    mx->perf = cfg->aa_perf;
    mx->adaptive = cfg->adaptive;
    mx->max_buf_frames = cfg->max_buf_frames;
    mx->target_buf_frames = cfg->target_buf_frames;
    return mixer_open_stream(mx, &mx->aq);
}

static aaudio_result_t mixer_add_voice(struct directaudio_mixer *mx, struct directaudio_stream *v)
{
    aaudio_result_t r;

    pthread_mutex_lock(&mx->lock);
    r = mixer_ensure_open(mx, v);
    if (r == AAUDIO_OK)
    {
        if (mx->nvoices < MIX_MAX_VOICES)
        {
            v->rs_pos = 0.0;
            v->in_mixer = 1;
            mx->voices[mx->nvoices++] = v;
        }
        else
            r = AAUDIO_ERROR_NO_MEMORY;
    }
    pthread_mutex_unlock(&mx->lock);
    return r;
}

static void mixer_remove_voice(struct directaudio_mixer *mx, struct directaudio_stream *v)
{
    int i;

    pthread_mutex_lock(&mx->lock);
    for (i = 0; i < mx->nvoices; i++)
    {
        if (mx->voices[i] == v)
        {
            mx->voices[i] = mx->voices[--mx->nvoices];
            break;
        }
    }
    v->in_mixer = 0;
    pthread_mutex_unlock(&mx->lock);
}

static void read_config_from_env(struct directaudio_stream *stream)
{
    const char *e;

    /* Engine-scoped keys per the app's NO-BLEED audio contract: the DirectAudio
     * engine tag is DIRECT, so config arrives as BANNER_AUDIO_DIRECT_* in the
     * container/shortcut env (perf is 0=NONE, 1=LOW_LATENCY, 2=POWER_SAVING).
     * DEFAULT = LOW_LATENCY ("auto"): device finding is that under box64/FEX load
     * the low-latency high-priority audio thread stays scheduled and crackle-free,
     * while NONE runs a normal-priority thread the guest preempts -> choppy. So a
     * game with no preset set gets the smooth path out of the box. (An explicit
     * BANNER_AUDIO_DIRECT_PERF still overrides below; buffers stay small - big
     * buffers are incompatible with the low-latency fast path.) */
    stream->aa_perf = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    stream->adaptive = TRUE;
    stream->target_buf_frames = 0;
    stream->max_buf_frames = 0;

    if ((e = getenv("BANNER_AUDIO_DIRECT_PERF")))
    {
        int v = atoi(e);
        if (v == 1) stream->aa_perf = AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
        else if (v == 2) stream->aa_perf = AAUDIO_PERFORMANCE_MODE_POWER_SAVING;
        else stream->aa_perf = AAUDIO_PERFORMANCE_MODE_NONE;
    }
    if ((e = getenv("BANNER_AUDIO_DIRECT_ADAPTIVE"))) stream->adaptive = atoi(e) != 0;
    if ((e = getenv("BANNER_AUDIO_DIRECT_BF"))) stream->target_buf_frames = atoi(e);
    if ((e = getenv("BANNER_AUDIO_DIRECT_MBF"))) stream->max_buf_frames = atoi(e);
}

static NTSTATUS unix_process_attach(void *args)
{
#ifdef _WIN64
    if (NtCurrentTeb()->WowTebOffset)
    {
        SYSTEM_BASIC_INFORMATION info;

        NtQuerySystemInformation(SystemEmulationBasicInformation, &info, sizeof(info), NULL);
        zero_bits = (ULONG_PTR)info.HighestUserAddress | 0x7fffffff;
    }
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS unix_main_loop(void *args)
{
    struct main_loop_params *params = args;
    NtSetEvent(params->event, NULL);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_test_connect(void *args)
{
    struct test_connect_params *params = args;
    AAudioStreamBuilder *builder = NULL;

    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK && builder)
    {
        AAudioStreamBuilder_delete(builder);
        params->priority = Priority_Preferred;
    }
    else
        params->priority = Priority_Unavailable;

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_endpoint_ids(void *args)
{
    static const WCHAR ep_name_out[] = {'D','i','r','e','c','t','A','u','d','i','o',0};
    static const WCHAR ep_name_in[] =
        {'D','i','r','e','c','t','A','u','d','i','o',' ','I','n','p','u','t',0};
    static const char ep_dev_out[] = "aaudio";
    static const char ep_dev_in[] = "aaudio_in";
    struct get_endpoint_ids_params *params = args;
    BOOL capture = params->flow == eCapture;
    const WCHAR *ep_name = capture ? ep_name_in : ep_name_out;
    const char *ep_dev = capture ? ep_dev_in : ep_dev_out;
    unsigned int name_bytes = capture ? sizeof(ep_name_in) : sizeof(ep_name_out);
    unsigned int dev_bytes = capture ? sizeof(ep_dev_in) : sizeof(ep_dev_out);
    unsigned int needed, offset;
    struct endpoint *endpoint = params->endpoints;

    params->default_idx = 0;
    /* Expose a render endpoint only. Capture (mic) is a later phase: create_stream
     * returns DEVICE_INVALIDATED for eCapture, so advertising a capture endpoint the
     * game can enumerate but never open makes it abandon audio init entirely and boot
     * to a black screen. A fresh same-setup winealsa capture that BOOTS exposes zero
     * capture endpoints and DiRT 3 initialises render fine, confirming no mic device is
     * needed. (The earlier assumption that a missing capture endpoint caused the hang
     * was wrong - that hang was the PhysicalSpeakers E_NOTIMPL gate, fixed separately.) */
    params->num = (params->flow == eRender) ? 1 : 0;

    TRACE("get_endpoint_ids: flow=%d -> num=%u\n", params->flow, params->num);

    if (params->num == 0)
    {
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    offset = needed = sizeof(*endpoint) * params->num;
    needed += name_bytes + ((dev_bytes + 1) & ~1);

    if (needed <= params->size)
    {
        endpoint->name = offset;
        memcpy((char *)params->endpoints + offset, ep_name, name_bytes);
        offset += name_bytes;

        endpoint->device = offset;
        memcpy((char *)params->endpoints + offset, ep_dev, dev_bytes);

        params->result = S_OK;
    }
    else
    {
        params->size = needed;
        params->result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_create_stream(void *args)
{
    struct create_stream_params *params = args;
    struct directaudio_stream *stream;
    aaudio_result_t r;
    SIZE_T size;
    int i;

    TRACE("create_stream: flow=%d share=%d flags=%#x tag=%#x ch=%u rate=%u bits=%u\n",
          params->flow, params->share, (unsigned)params->flags, params->fmt->wFormatTag,
          params->fmt->nChannels, (unsigned)params->fmt->nSamplesPerSec, params->fmt->wBitsPerSample);

    params->result = S_OK;

    if (!(stream = calloc(1, sizeof(*stream))))
    {
        params->result = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    pthread_mutex_init(&stream->lock, NULL);
    for (i = 0; i < 8; i++) stream->vols[i] = 1.0f;

    stream->fmt = clone_format(params->fmt);
    if (!stream->fmt)
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }

    stream->period = params->period;
    stream->period_frames = muldiv(params->period, stream->fmt->nSamplesPerSec, 10000000);
    stream->flow = params->flow;
    stream->flags = params->flags;
    stream->share = params->share;

    if (stream->flow != eRender)
    {
        /* capture is a later phase */
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }

    /* The mixer converts any PCM(8/16/24/32)/float source to the shared output
     * format as it sums it in, so accept the full set mmdevapi validated - not
     * only the AAudio-native ones. */
    {
        const WAVEFORMATEXTENSIBLE *fex = (const WAVEFORMATEXTENSIBLE *)stream->fmt;
        stream->is_float = stream->fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (stream->fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             IsEqualGUID(&fex->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
        if (stream->fmt->wBitsPerSample != 8 && stream->fmt->wBitsPerSample != 16 &&
            stream->fmt->wBitsPerSample != 24 && stream->fmt->wBitsPerSample != 32)
        {
            params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
            goto end;
        }
    }
    stream->aa_format = stream->is_float ? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_UNSPECIFIED;
    stream->aa_channels = stream->fmt->nChannels;
    stream->aa_rate = stream->fmt->nSamplesPerSec;

    read_config_from_env(stream);

    stream->bufsize_frames = muldiv(params->duration, stream->fmt->nSamplesPerSec, 10000000);
    if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
        stream->bufsize_frames -= stream->bufsize_frames % stream->period_frames;

    size = stream->bufsize_frames * stream->fmt->nBlockAlign;
    if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer, zero_bits,
                                &size, MEM_COMMIT, PAGE_READWRITE))
    {
        params->result = E_OUTOFMEMORY;
        goto end;
    }
    silence_buffer(stream, stream->local_buffer, stream->bufsize_frames);

    /* Register as a mixer voice; the shared output opens on the first voice.
     * stream->playing gates real audio vs. silence in the mix. */
    r = mixer_add_voice(&g_mixer, stream);
    if (r != AAUDIO_OK)
    {
        WARN("mixer add voice failed: %d\n", r);
        params->result = AUDCLNT_E_DEVICE_INVALIDATED;
        goto end;
    }
    params->result = S_OK;

end:
    if (FAILED(params->result))
    {
        if (stream->in_mixer)
            mixer_remove_voice(&g_mixer, stream);
        if (stream->local_buffer)
        {
            size = 0;
            NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                                &size, MEM_RELEASE);
        }
        free(stream->fmt);
        pthread_mutex_destroy(&stream->lock);
        free(stream);
    }
    else
    {
        *params->channel_count = params->fmt->nChannels;
        *params->stream = (stream_handle)(UINT_PTR)stream;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_stream(void *args)
{
    struct release_stream_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;

    if (params->timer_thread)
    {
        stream->please_quit = TRUE;
        NtWaitForSingleObject(params->timer_thread, FALSE, NULL);
        NtClose(params->timer_thread);
    }

    if (stream->in_mixer)
        mixer_remove_voice(&g_mixer, stream);

    if (stream->local_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->local_buffer,
                            &size, MEM_RELEASE);
    }
    if (stream->tmp_buffer)
    {
        size = 0;
        NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                            &size, MEM_RELEASE);
    }
    free(stream->fmt);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static DWORD get_channel_mask(unsigned int channels)
{
    switch (channels)
    {
    case 0:  return 0;
    case 1:  return KSAUDIO_SPEAKER_MONO;
    case 2:  return KSAUDIO_SPEAKER_STEREO;
    case 3:  return KSAUDIO_SPEAKER_STEREO | SPEAKER_LOW_FREQUENCY;
    case 4:  return KSAUDIO_SPEAKER_QUAD;
    case 5:  return KSAUDIO_SPEAKER_QUAD | SPEAKER_LOW_FREQUENCY;
    case 6:  return KSAUDIO_SPEAKER_5POINT1;
    case 7:  return KSAUDIO_SPEAKER_5POINT1 | SPEAKER_BACK_CENTER;
    case 8:  return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    }
    return 0;
}

static NTSTATUS unix_get_mix_format(void *args)
{
    struct get_mix_format_params *params = args;

    /* AAudio has no NDK device enumeration before API 34; advertise a standard
     * AAudio-friendly shared mix format. mmdevapi resamples the guest to this. */
    params->fmt->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    params->fmt->Format.nChannels = 2;
    params->fmt->Format.nSamplesPerSec = 48000;
    params->fmt->Format.wBitsPerSample = 32;
    params->fmt->dwChannelMask = get_channel_mask(2);
    params->fmt->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    params->fmt->Format.nBlockAlign = params->fmt->Format.wBitsPerSample *
        params->fmt->Format.nChannels / 8;
    params->fmt->Format.nAvgBytesPerSec = params->fmt->Format.nSamplesPerSec *
        params->fmt->Format.nBlockAlign;
    params->fmt->Samples.wValidBitsPerSample = params->fmt->Format.wBitsPerSample;
    params->fmt->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    TRACE("get_mix_format: flow=%d -> 48000/32f/2ch\n", params->flow);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_format_supported(void *args)
{
    struct is_format_supported_params *params = args;
    const WAVEFORMATEX *fmt = params->fmt_in;
    aaudio_format_t aafmt;

    if (!fmt || (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 fmt->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
    {
        params->result = E_INVALIDARG;
        return STATUS_SUCCESS;
    }

    if (params->flow != eRender && params->flow != eCapture)
    {
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
        return STATUS_SUCCESS;
    }

    aafmt = fmt_to_aaudio(fmt);

    if (fmt->nChannels < 1 || fmt->nChannels > 8 ||
        fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 192000)
    {
        /* No backend, exclusive or shared, can satisfy a nonsensical layout. */
        params->result = AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    else if (params->share == AUDCLNT_SHAREMODE_EXCLUSIVE)
    {
        /* Exclusive streams hand the guest format straight to AAudio with no
         * conversion, so we can only honour what AAudio opens natively. */
        params->result = (aafmt != AAUDIO_FORMAT_UNSPECIFIED)
                         ? S_OK : AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    else /* AUDCLNT_SHAREMODE_SHARED */
    {
        /* Shared mode goes through mmdevapi's mixer, which converts the guest
         * format to our AAudio mix format - exactly like the winealsa/winepulse
         * software-mixer backends. So report S_OK for any layout mmdevapi has
         * already validated, INCLUDING formats AAudio cannot open natively
         * (8-bit / 24-bit PCM): the guest only uses these to probe device
         * capability and always initialises the stream with the float mix
         * format (device-verified against DiRT 3's WASAPI negotiation). Both
         * known-good backends answer S_OK here; returning anything weaker
         * (S_FALSE, and certainly a hard AUDCLNT_E_UNSUPPORTED_FORMAT) makes the
         * game judge the endpoint incapable and fall back to legacy winmm, which
         * is what stalled DiRT 3's boot on the two prior builds.
         * TODO: if a title ever *initialises* a shared stream with a non-native
         * format, create_stream must convert it to the mix format rather than
         * fail; no observed title does this yet. */
        params->result = S_OK;
    }

    TRACE("is_format_supported: share=%d tag=%#x ch=%u rate=%u bits=%u aafmt=%d -> %#x\n",
          params->share, fmt->wFormatTag, fmt->nChannels, (unsigned)fmt->nSamplesPerSec,
          fmt->wBitsPerSample, aafmt, (unsigned)params->result);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_device_period(void *args)
{
    struct get_device_period_params *params = args;

    if (params->def_period) *params->def_period = def_period;
    if (params->min_period) *params->min_period = min_period;
    TRACE("get_device_period: flow=%d def=%d min=%d\n", params->flow, (int)def_period, (int)min_period);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_buffer_size(void *args)
{
    struct get_buffer_size_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->frames = stream->bufsize_frames;
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_latency(void *args)
{
    struct get_latency_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    int32_t buf_frames;

    pthread_mutex_lock(&stream->lock);
    buf_frames = g_mixer.aq ? AAudioStream_getBufferSizeInFrames(g_mixer.aq) : 0;
    if (buf_frames < 0) buf_frames = 0;
    /* pretend we process audio in Period chunks, so max latency includes it */
    *params->latency = muldiv(buf_frames, 10000000, stream->fmt->nSamplesPerSec) + stream->period;
    pthread_mutex_unlock(&stream->lock);

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static UINT32 get_current_padding_nolock(struct directaudio_stream *stream)
{
    return stream->held_frames;
}

static NTSTATUS unix_get_current_padding(void *args)
{
    struct get_current_padding_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);
    *params->padding = get_current_padding_nolock(stream);
    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_start(void *args)
{
    struct start_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if ((stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) && !stream->event)
        params->result = AUDCLNT_E_EVENTHANDLE_NOT_SET;
    else if (stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else
    {
        stream->playing = TRUE;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_stop(void *args)
{
    struct stop_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (!stream->playing)
        params->result = S_FALSE;
    else
    {
        stream->playing = FALSE;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_reset(void *args)
{
    struct reset_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    pthread_mutex_lock(&stream->lock);

    if (stream->playing)
        params->result = AUDCLNT_E_NOT_STOPPED;
    else if (stream->getbuf_last)
        params->result = AUDCLNT_E_BUFFER_OPERATION_PENDING;
    else
    {
        stream->written_frames = 0;
        stream->held_frames = 0;
        stream->lcl_offs_frames = 0;
        stream->wri_offs_frames = 0;
        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_timer_loop(void *args)
{
    struct timer_loop_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER delay, next, last;
    int adjust;

    delay.QuadPart = -stream->period;
    NtQueryPerformanceCounter(&last, NULL);
    next.QuadPart = last.QuadPart + stream->period;

    while (!stream->please_quit)
    {
        if (stream->event)
            NtSetEvent(stream->event, NULL);
        NtDelayExecution(FALSE, &delay);
        NtQueryPerformanceCounter(&last, NULL);

        adjust = next.QuadPart - last.QuadPart;
        if (adjust > stream->period / 2)
            adjust = stream->period / 2;
        else if (adjust < -stream->period / 2)
            adjust = -stream->period / 2;

        delay.QuadPart = -(stream->period + adjust);
        next.QuadPart += stream->period;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_render_buffer(void *args)
{
    struct get_render_buffer_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    SIZE_T size;
    UINT32 pad;

    pthread_mutex_lock(&stream->lock);

    pad = get_current_padding_nolock(stream);

    if (stream->getbuf_last)
    {
        params->result = AUDCLNT_E_OUT_OF_ORDER;
        goto end;
    }
    if (!params->frames)
    {
        params->result = S_OK;
        goto end;
    }
    if (pad + params->frames > stream->bufsize_frames)
    {
        params->result = AUDCLNT_E_BUFFER_TOO_LARGE;
        goto end;
    }

    if (stream->wri_offs_frames + params->frames > stream->bufsize_frames)
    {
        if (stream->tmp_buffer_frames < params->frames)
        {
            if (stream->tmp_buffer)
            {
                size = 0;
                NtFreeVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer,
                                    &size, MEM_RELEASE);
                stream->tmp_buffer = NULL;
            }
            size = params->frames * stream->fmt->nBlockAlign;
            if (NtAllocateVirtualMemory(GetCurrentProcess(), (void **)&stream->tmp_buffer, zero_bits,
                                        &size, MEM_COMMIT, PAGE_READWRITE))
            {
                stream->tmp_buffer_frames = 0;
                params->result = E_OUTOFMEMORY;
                goto end;
            }
            stream->tmp_buffer_frames = params->frames;
        }
        *params->data = stream->tmp_buffer;
        stream->getbuf_last = -params->frames;
    }
    else
    {
        *params->data = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        stream->getbuf_last = params->frames;
    }

    silence_buffer(stream, *params->data, params->frames);
    params->result = S_OK;

end:
    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static void wrap_buffer(BYTE *dst, UINT32 dst_offs, UINT32 dst_bytes, BYTE *src, UINT32 src_bytes)
{
    UINT32 chunk_bytes = dst_bytes - dst_offs;

    if (chunk_bytes < src_bytes)
    {
        memcpy(dst + dst_offs, src, chunk_bytes);
        memcpy(dst, src + chunk_bytes, src_bytes - chunk_bytes);
    }
    else
        memcpy(dst + dst_offs, src, src_bytes);
}

static NTSTATUS unix_release_render_buffer(void *args)
{
    struct release_render_buffer_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    BYTE *buffer;

    pthread_mutex_lock(&stream->lock);

    if (!params->written_frames)
    {
        stream->getbuf_last = 0;
        params->result = S_OK;
    }
    else if (!stream->getbuf_last)
        params->result = AUDCLNT_E_OUT_OF_ORDER;
    else if (params->written_frames > (stream->getbuf_last >= 0 ? stream->getbuf_last : -stream->getbuf_last))
        params->result = AUDCLNT_E_INVALID_SIZE;
    else
    {
        if (stream->getbuf_last >= 0)
            buffer = stream->local_buffer + stream->wri_offs_frames * stream->fmt->nBlockAlign;
        else
            buffer = stream->tmp_buffer;

        if (params->flags & AUDCLNT_BUFFERFLAGS_SILENT)
            silence_buffer(stream, buffer, params->written_frames);

        if (stream->getbuf_last < 0)
            wrap_buffer(stream->local_buffer,
                        stream->wri_offs_frames * stream->fmt->nBlockAlign,
                        stream->bufsize_frames * stream->fmt->nBlockAlign,
                        buffer, params->written_frames * stream->fmt->nBlockAlign);

        stream->wri_offs_frames += params->written_frames;
        stream->wri_offs_frames %= stream->bufsize_frames;
        stream->held_frames += params->written_frames;
        stream->written_frames += params->written_frames;
        stream->getbuf_last = 0;

        params->result = S_OK;
    }

    pthread_mutex_unlock(&stream->lock);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_capture_buffer(void *args)
{
    struct get_capture_buffer_params *params = args;
    *params->frames = 0;
    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_release_capture_buffer(void *args)
{
    struct release_capture_buffer_params *params = args;
    params->result = params->done ? AUDCLNT_E_OUT_OF_ORDER : S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_next_packet_size(void *args)
{
    struct get_next_packet_size_params *params = args;
    *params->frames = 0;
    params->result = AUDCLNT_E_DEVICE_INVALIDATED;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_position(void *args)
{
    struct get_position_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    LARGE_INTEGER stamp, freq;

    if (params->device)
    {
        params->result = E_NOTIMPL;
        return STATUS_SUCCESS;
    }

    pthread_mutex_lock(&stream->lock);

    *params->pos = stream->written_frames - stream->held_frames;
    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->pos *= stream->fmt->nBlockAlign;

    if (params->qpctime)
    {
        NtQueryPerformanceCounter(&stamp, &freq);
        *params->qpctime = (stamp.QuadPart * (INT64)10000000) / freq.QuadPart;
    }

    pthread_mutex_unlock(&stream->lock);
    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_frequency(void *args)
{
    struct get_frequency_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    if (stream->share == AUDCLNT_SHAREMODE_SHARED)
        *params->freq = (UINT64)stream->fmt->nSamplesPerSec * stream->fmt->nBlockAlign;
    else
        *params->freq = stream->fmt->nSamplesPerSec;

    params->result = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_is_started(void *args)
{
    struct is_started_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);

    params->result = stream->playing ? S_OK : S_FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_get_prop_value(void *args)
{
    /* PKEY_AudioEndpoint_* live under this fmtid; {...},3 = PhysicalSpeakers. */
    static const GUID PKEY_AudioEndpoint_GUID = {
        0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}
    };
    static const PROPERTYKEY devicepath_key = {
        {0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2
    };
    struct get_prop_value_params *params = args;

    TRACE("get_prop_value: flow=%d prop=%s,%u\n", params->flow,
          wine_dbgstr_guid(&params->prop->fmtid), params->prop->pid);

    /* Games (e.g. DiRT 3) refuse to call IAudioClient::Initialize until they can
     * read PhysicalSpeakers from the endpoint, so (like winealsa/winepulse) we
     * must supply it (winecoreaudio's E_NOTIMPL stub hangs those titles here). */
    if (params->flow == eRender &&
        IsEqualGUID(&params->prop->fmtid, &PKEY_AudioEndpoint_GUID) &&
        params->prop->pid == 3)
    {
        params->value->vt = VT_UI4;
        params->value->ulVal = KSAUDIO_SPEAKER_STEREO; /* our AAudio endpoint is stereo */
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    if (IsEqualPropertyKey(*params->prop, devicepath_key))
    {
        /* Each endpoint must present a UNIQUE device-instance path; a real system
         * never gives two endpoints the same one. Our render and capture devices
         * differ only by flow, so key the trailing instance id on that. */
        static const WCHAR path_out[] =
            {'{','1','}','.','R','O','O','T','\\','M','E','D','I','A','\\','0','0','0','0',0};
        static const WCHAR path_in[] =
            {'{','1','}','.','R','O','O','T','\\','M','E','D','I','A','\\','0','0','0','1',0};
        const WCHAR *path = (params->flow == eCapture) ? path_in : path_out;
        UINT len = ARRAYSIZE(path_out);

        if (*params->buffer_size < len * sizeof(WCHAR))
        {
            *params->buffer_size = len * sizeof(WCHAR);
            params->result = E_NOT_SUFFICIENT_BUFFER;
            return STATUS_SUCCESS;
        }
        params->value->vt = VT_LPWSTR;
        params->value->pwszVal = params->buffer;
        memcpy(params->buffer, path, len * sizeof(WCHAR));
        params->result = S_OK;
        return STATUS_SUCCESS;
    }

    params->result = E_NOTIMPL;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_volumes(void *args)
{
    struct set_volumes_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    BOOL active = FALSE;
    UINT32 i;

    pthread_mutex_lock(&stream->lock);
    for (i = 0; i < stream->fmt->nChannels && i < 8; i++)
    {
        stream->vols[i] = params->master_volume *
            params->session_volumes[i] * params->volumes[i];
        if (stream->vols[i] < 0.999f || stream->vols[i] > 1.001f) active = TRUE;
    }
    stream->vols_active = active;
    pthread_mutex_unlock(&stream->lock);

    return STATUS_SUCCESS;
}

static NTSTATUS unix_set_event_handle(void *args)
{
    struct set_event_handle_params *params = args;
    struct directaudio_stream *stream = handle_get_stream(params->stream);
    HRESULT hr = S_OK;

    pthread_mutex_lock(&stream->lock);
    if (!stream->in_mixer)
        hr = AUDCLNT_E_DEVICE_INVALIDATED;
    else if (!(stream->flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK))
        hr = AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED;
    else if (stream->event)
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    else
        stream->event = params->event;
    pthread_mutex_unlock(&stream->lock);

    params->result = hr;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    unix_process_attach,
    unix_not_implemented,        /* process_detach */
    unix_main_loop,
    unix_get_endpoint_ids,
    unix_create_stream,
    unix_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_timer_loop,
    unix_get_render_buffer,
    unix_release_render_buffer,
    unix_get_capture_buffer,
    unix_release_capture_buffer,
    unix_is_format_supported,
    unix_not_implemented,        /* get_loopback_capture_device */
    unix_get_mix_format,
    unix_get_device_period,
    unix_get_buffer_size,
    unix_get_latency,
    unix_get_current_padding,
    unix_get_next_packet_size,
    unix_get_frequency,
    unix_get_position,
    unix_set_volumes,
    unix_set_event_handle,
    unix_not_implemented,        /* set_sample_rate */
    unix_test_connect,
    unix_is_started,
    unix_get_prop_value,
    unix_not_implemented,        /* midi_get_driver */
    unix_not_implemented,        /* midi_init */
    unix_not_implemented,        /* midi_release */
    unix_midi_out_message,       /* midi_out_message */
    unix_midi_in_message,        /* midi_in_message */
    unix_not_implemented,        /* midi_notify_wait */
    unix_not_implemented,        /* aux_message */
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == funcs_count);

#ifdef _WIN64

typedef UINT PTR32;

static NTSTATUS unix_wow64_main_loop(void *args)
{
    struct
    {
        PTR32 event;
    } *params32 = args;
    struct main_loop_params params =
    {
        .event = ULongToHandle(params32->event)
    };
    return unix_main_loop(&params);
}

static NTSTATUS unix_wow64_test_connect(void *args)
{
    struct
    {
        PTR32 name;
        enum driver_priority priority;
    } *params32 = args;
    struct test_connect_params params =
    {
        .name = ULongToPtr(params32->name)
    };
    unix_test_connect(&params);
    params32->priority = params.priority;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_endpoint_ids(void *args)
{
    struct
    {
        EDataFlow flow;
        PTR32 endpoints;
        unsigned int size;
        HRESULT result;
        unsigned int num;
        unsigned int default_idx;
    } *params32 = args;
    struct get_endpoint_ids_params params =
    {
        .flow = params32->flow,
        .endpoints = ULongToPtr(params32->endpoints),
        .size = params32->size
    };
    unix_get_endpoint_ids(&params);
    params32->size = params.size;
    params32->result = params.result;
    params32->num = params.num;
    params32->default_idx = params.default_idx;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_create_stream(void *args)
{
    struct
    {
        PTR32 name;
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        DWORD flags;
        REFERENCE_TIME duration;
        REFERENCE_TIME period;
        PTR32 fmt;
        HRESULT result;
        PTR32 channel_count;
        PTR32 stream;
    } *params32 = args;
    struct create_stream_params params =
    {
        .name = ULongToPtr(params32->name),
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .flags = params32->flags,
        .duration = params32->duration,
        .period = params32->period,
        .fmt = ULongToPtr(params32->fmt),
        .channel_count = ULongToPtr(params32->channel_count),
        .stream = ULongToPtr(params32->stream)
    };
    unix_create_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_release_stream(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 timer_thread;
        HRESULT result;
    } *params32 = args;
    struct release_stream_params params =
    {
        .stream = params32->stream,
        .timer_thread = ULongToHandle(params32->timer_thread)
    };
    unix_release_stream(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_render_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        UINT32 frames;
        HRESULT result;
        PTR32 data;
    } *params32 = args;
    BYTE *data = NULL;
    struct get_render_buffer_params params =
    {
        .stream = params32->stream,
        .frames = params32->frames,
        .data = &data
    };
    unix_get_render_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->data) = PtrToUlong(data);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_capture_buffer(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 data;
        PTR32 frames;
        PTR32 flags;
        PTR32 devpos;
        PTR32 qpcpos;
    } *params32 = args;
    UINT32 frames = 0;
    struct get_capture_buffer_params params =
    {
        .stream = params32->stream,
        .frames = &frames
    };
    unix_get_capture_buffer(&params);
    params32->result = params.result;
    *(unsigned int *)ULongToPtr(params32->frames) = frames;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_is_format_supported(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        AUDCLNT_SHAREMODE share;
        PTR32 fmt_in;
        HRESULT result;
    } *params32 = args;
    struct is_format_supported_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .share = params32->share,
        .fmt_in = ULongToPtr(params32->fmt_in),
    };
    unix_is_format_supported(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_mix_format(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 fmt;
        HRESULT result;
    } *params32 = args;
    struct get_mix_format_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .fmt = ULongToPtr(params32->fmt)
    };
    unix_get_mix_format(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_device_period(void *args)
{
    struct
    {
        PTR32 device;
        EDataFlow flow;
        HRESULT result;
        PTR32 def_period;
        PTR32 min_period;
    } *params32 = args;
    struct get_device_period_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .def_period = ULongToPtr(params32->def_period),
        .min_period = ULongToPtr(params32->min_period),
    };
    unix_get_device_period(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_buffer_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_buffer_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_buffer_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_latency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 latency;
    } *params32 = args;
    struct get_latency_params params =
    {
        .stream = params32->stream,
        .latency = ULongToPtr(params32->latency)
    };
    unix_get_latency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_current_padding(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 padding;
    } *params32 = args;
    struct get_current_padding_params params =
    {
        .stream = params32->stream,
        .padding = ULongToPtr(params32->padding)
    };
    unix_get_current_padding(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_next_packet_size(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 frames;
    } *params32 = args;
    struct get_next_packet_size_params params =
    {
        .stream = params32->stream,
        .frames = ULongToPtr(params32->frames)
    };
    unix_get_next_packet_size(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_position(void *args)
{
    struct
    {
        stream_handle stream;
        BOOL device;
        HRESULT result;
        PTR32 pos;
        PTR32 qpctime;
    } *params32 = args;
    struct get_position_params params =
    {
        .stream = params32->stream,
        .device = params32->device,
        .pos = ULongToPtr(params32->pos),
        .qpctime = ULongToPtr(params32->qpctime)
    };
    unix_get_position(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_frequency(void *args)
{
    struct
    {
        stream_handle stream;
        HRESULT result;
        PTR32 freq;
    } *params32 = args;
    struct get_frequency_params params =
    {
        .stream = params32->stream,
        .freq = ULongToPtr(params32->freq)
    };
    unix_get_frequency(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_set_volumes(void *args)
{
    struct
    {
        stream_handle stream;
        float master_volume;
        PTR32 volumes;
        PTR32 session_volumes;
    } *params32 = args;
    struct set_volumes_params params =
    {
        .stream = params32->stream,
        .master_volume = params32->master_volume,
        .volumes = ULongToPtr(params32->volumes),
        .session_volumes = ULongToPtr(params32->session_volumes),
    };
    return unix_set_volumes(&params);
}

static NTSTATUS unix_wow64_set_event_handle(void *args)
{
    struct
    {
        stream_handle stream;
        PTR32 event;
        HRESULT result;
    } *params32 = args;
    struct set_event_handle_params params =
    {
        .stream = params32->stream,
        .event = ULongToHandle(params32->event)
    };
    unix_set_event_handle(&params);
    params32->result = params.result;
    return STATUS_SUCCESS;
}

static NTSTATUS unix_wow64_get_prop_value(void *args)
{
    struct propvariant32
    {
        WORD vt;
        WORD pad1, pad2, pad3;
        union
        {
            ULONG ulVal;
            PTR32 ptr;
            ULARGE_INTEGER uhVal;
        };
    } *value32;
    struct
    {
        PTR32 device;
        EDataFlow flow;
        PTR32 guid;
        PTR32 prop;
        HRESULT result;
        PTR32 value;
        PTR32 buffer;
        PTR32 buffer_size;
    } *params32 = args;
    PROPVARIANT value;
    struct get_prop_value_params params =
    {
        .device = ULongToPtr(params32->device),
        .flow = params32->flow,
        .guid = ULongToPtr(params32->guid),
        .prop = ULongToPtr(params32->prop),
        .value = &value,
        .buffer = ULongToPtr(params32->buffer),
        .buffer_size = ULongToPtr(params32->buffer_size)
    };
    unix_get_prop_value(&params);
    params32->result = params.result;
    if (SUCCEEDED(params.result))
    {
        value32 = UlongToPtr(params32->value);
        value32->vt = value.vt;
        switch (value.vt)
        {
        case VT_UI4:
            value32->ulVal = value.ulVal;
            break;
        case VT_LPWSTR:
            value32->ptr = params32->buffer;
            break;
        default:
            FIXME("Unhandled vt %04x\n", value.vt);
        }
    }
    return STATUS_SUCCESS;
}

/* wow64 counterpart of unix_midi_{out,in}_message: the 32-bit game (DiRT 3) hits
 * this path. Clear the 32-bit notify_context's send_notify (its first field) so
 * mmdevapi does not fire a garbage notify_client callback into the guest. Both
 * midi_out and midi_in share this param layout. */
static NTSTATUS unix_wow64_midi_message(void *args)
{
    struct
    {
        UINT dev_id;
        UINT msg;
        UINT user;
        UINT param_1;
        UINT param_2;
        PTR32 err;
        PTR32 notify;
    } *params32 = args;

    if (params32->notify)
        *(BOOL *)ULongToPtr(params32->notify) = FALSE; /* send_notify is field 0 */
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    unix_process_attach,
    unix_not_implemented,        /* process_detach */
    unix_wow64_main_loop,
    unix_wow64_get_endpoint_ids,
    unix_wow64_create_stream,
    unix_wow64_release_stream,
    unix_start,
    unix_stop,
    unix_reset,
    unix_timer_loop,
    unix_wow64_get_render_buffer,
    unix_release_render_buffer,
    unix_wow64_get_capture_buffer,
    unix_release_capture_buffer,
    unix_wow64_is_format_supported,
    unix_not_implemented,        /* get_loopback_capture_device */
    unix_wow64_get_mix_format,
    unix_wow64_get_device_period,
    unix_wow64_get_buffer_size,
    unix_wow64_get_latency,
    unix_wow64_get_current_padding,
    unix_wow64_get_next_packet_size,
    unix_wow64_get_frequency,
    unix_wow64_get_position,
    unix_wow64_set_volumes,
    unix_wow64_set_event_handle,
    unix_not_implemented,        /* set_sample_rate */
    unix_wow64_test_connect,
    unix_is_started,
    unix_wow64_get_prop_value,
    unix_not_implemented,        /* midi_get_driver */
    unix_not_implemented,        /* midi_init */
    unix_not_implemented,        /* midi_release */
    unix_midi_out_message,       /* midi_out_message */
    unix_midi_in_message,        /* midi_in_message */
    unix_not_implemented,        /* midi_notify_wait */
    unix_not_implemented,        /* aux_message */
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == funcs_count);

#endif /* _WIN64 */
