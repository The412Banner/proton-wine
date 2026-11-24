/*
 * Built-in software MIDI synthesizer
 *
 * Windows always has a software synth for MIDI output, and games rely on it.
 * Wine only has an output if the user runs a synth of their own, so MIDI music
 * is silent otherwise. mmdevapi loads this module as device 0 in that case.
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

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winnls.h"
#include "mmsystem.h"
#include "mmddk.h"
#include "mmreg.h"
#include "wine/debug.h"

#include <fluidsynth.h>

WINE_DEFAULT_DEBUG_CHANNEL(midi);

#define SAMPLE_RATE 44100
/* MIDI events take effect at the start of a block, so keep blocks short enough
 * not to be heard as timing jitter. */
#define BLOCK_FRAMES 256
#define BLOCK_COUNT 8

struct client
{
    MIDIOPENDESC desc;
    WORD flags;
};

static CRITICAL_SECTION synth_cs;
static CRITICAL_SECTION_DEBUG synth_cs_debug =
{
    0, 0, &synth_cs,
    { &synth_cs_debug.ProcessLocksList, &synth_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": synth_cs") }
};
static CRITICAL_SECTION synth_cs = { &synth_cs_debug, -1, 0, 0, 0, 0 };

static fluid_synth_t *synth;
static HANDLE render_wake;
static unsigned int client_count;
static float volume_left = 1.0f, volume_right = 1.0f;
static DWORD volume = 0xffffffff;

extern int wine_fluid_sf2_mode;

static void log_fluid_message(int level, const char *message, void *data)
{
    if (level <= FLUID_ERR) ERR("%s\n", debugstr_a(message));
    else TRACE("%s\n", debugstr_a(message));
}

/* The sound font is a host path, and Z: may not exist or may be remapped. */
static void *font_open(const char *filename)
{
    WCHAR path[MAX_PATH + 16] = L"\\??\\unix";
    int prefix = wcslen(path);
    HANDLE file;

    if (!MultiByteToWideChar(CP_UTF8, 0, filename, -1, path + prefix, ARRAY_SIZE(path) - prefix))
        return NULL;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        ERR("Failed to open %s, error %lu.\n", debugstr_w(path), GetLastError());
        return NULL;
    }
    return file;
}

static int font_read(void *buffer, fluid_long_long_t count, void *handle)
{
    char *data = buffer;
    DWORD done;

    while (count > 0)
    {
        if (!ReadFile(handle, data, count > 0x1000000 ? 0x1000000 : count, &done, NULL) || !done)
            return FLUID_FAILED;
        data += done;
        count -= done;
    }
    return FLUID_OK;
}

static int font_seek(void *handle, fluid_long_long_t offset, int origin)
{
    LARGE_INTEGER distance;

    distance.QuadPart = offset;
    return SetFilePointerEx(handle, distance, NULL, origin) ? FLUID_OK : FLUID_FAILED;
}

static fluid_long_long_t font_tell(void *handle)
{
    LARGE_INTEGER distance = {{0}}, position;

    return SetFilePointerEx(handle, distance, &position, FILE_CURRENT) ? position.QuadPart : FLUID_FAILED;
}

static int font_close(void *handle)
{
    return CloseHandle(handle) ? FLUID_OK : FLUID_FAILED;
}

static BOOL create_synth(void)
{
    WCHAR pathW[MAX_PATH];
    char path[MAX_PATH * 3];
    fluid_settings_t *settings;
    fluid_sfloader_t *loader;
    fluid_synth_t *new_synth;

    if (!GetEnvironmentVariableW(L"WINE_MIDI_SOUNDFONT", pathW, ARRAY_SIZE(pathW))) return FALSE;
    if (!WideCharToMultiByte(CP_UTF8, 0, pathW, -1, path, sizeof(path), NULL, NULL)) return FALSE;

    fluid_set_log_function(FLUID_PANIC, log_fluid_message, NULL);
    fluid_set_log_function(FLUID_ERR, log_fluid_message, NULL);
    fluid_set_log_function(FLUID_WARN, log_fluid_message, NULL);
    fluid_set_log_function(FLUID_INFO, log_fluid_message, NULL);
    fluid_set_log_function(FLUID_DBG, log_fluid_message, NULL);

    /* Must be set before the first synth is created. */
    wine_fluid_sf2_mode = 1;

    if (!(settings = new_fluid_settings())) return FALSE;
    fluid_settings_setnum(settings, "synth.sample-rate", SAMPLE_RATE);
    fluid_settings_setnum(settings, "synth.gain", 0.5);
    /* synth_cs protects every call. */
    fluid_settings_setint(settings, "synth.threadsafe-api", 0);

    if (!(new_synth = new_fluid_synth(settings)))
    {
        delete_fluid_settings(settings);
        return FALSE;
    }

    if ((loader = new_fluid_defsfloader(settings)))
    {
        fluid_sfloader_set_callbacks(loader, font_open, font_read, font_seek, font_tell, font_close);
        fluid_synth_add_sfloader(new_synth, loader);
    }

    if (!loader || fluid_synth_sfload(new_synth, path, 1) == FLUID_FAILED)
    {
        ERR("Failed to load sound font %s.\n", debugstr_a(path));
        delete_fluid_synth(new_synth);
        delete_fluid_settings(settings);
        return FALSE;
    }

    TRACE("Loaded sound font %s.\n", debugstr_a(path));
    synth = new_synth;
    return TRUE;
}

static void close_wave(HWAVEOUT wave, WAVEHDR *headers)
{
    unsigned int i;

    waveOutReset(wave);
    for (i = 0; i < BLOCK_COUNT; ++i)
        waveOutUnprepareHeader(wave, &headers[i], sizeof(headers[i]));
    waveOutClose(wave);
}

static HWAVEOUT open_wave(WAVEHDR *headers, float *samples)
{
    WAVEFORMATEX format = {0};
    unsigned int i;
    HWAVEOUT wave;
    MMRESULT err;

    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if ((err = waveOutOpen(&wave, WAVE_MAPPER, &format, (DWORD_PTR)render_wake, 0, CALLBACK_EVENT)))
    {
        WARN("Failed to open wave output, error %u.\n", err);
        return NULL;
    }

    for (i = 0; i < BLOCK_COUNT; ++i)
    {
        memset(&headers[i], 0, sizeof(headers[i]));
        headers[i].lpData = (char *)(samples + i * BLOCK_FRAMES * 2);
        headers[i].dwBufferLength = BLOCK_FRAMES * format.nBlockAlign;
        waveOutPrepareHeader(wave, &headers[i], sizeof(headers[i]));
    }
    return wave;
}

static void render_block(HWAVEOUT wave, WAVEHDR *header)
{
    float *samples = (float *)header->lpData, left, right;
    unsigned int i;

    EnterCriticalSection(&synth_cs);
    fluid_synth_write_float(synth, BLOCK_FRAMES, samples, 0, 2, samples, 1, 2);
    left = volume_left;
    right = volume_right;
    LeaveCriticalSection(&synth_cs);

    if (left != 1.0f || right != 1.0f)
    {
        for (i = 0; i < BLOCK_FRAMES; ++i)
        {
            samples[i * 2] *= left;
            samples[i * 2 + 1] *= right;
        }
    }
    waveOutWrite(wave, header, sizeof(*header));
}

static DWORD WINAPI render_thread(void *arg)
{
    static float samples[BLOCK_COUNT * BLOCK_FRAMES * 2];
    WAVEHDR headers[BLOCK_COUNT];
    HWAVEOUT wave = NULL;
    unsigned int i;
    BOOL active;

    for (;;)
    {
        EnterCriticalSection(&synth_cs);
        active = client_count > 0;
        LeaveCriticalSection(&synth_cs);

        if (!active)
        {
            if (wave) close_wave(wave, headers);
            wave = NULL;
            WaitForSingleObject(render_wake, INFINITE);
            continue;
        }

        if (!wave && !(wave = open_wave(headers, samples)))
        {
            WaitForSingleObject(render_wake, 1000);
            continue;
        }

        for (i = 0; i < BLOCK_COUNT; ++i)
        {
            if (!(headers[i].dwFlags & WHDR_INQUEUE)) render_block(wave, &headers[i]);
        }
        WaitForSingleObject(render_wake, 100);
    }
    return 0;
}

static void notify(struct client *client, UINT msg, DWORD_PTR param)
{
    DriverCallback(client->desc.dwCallback, client->flags, (HDRVR)client->desc.hMidi, msg,
            client->desc.dwInstance, param, 0);
}

static void reset_notes(void)
{
    unsigned int channel;

    for (channel = 0; channel < 16; ++channel)
    {
        fluid_synth_cc(synth, channel, 123, 0); /* all notes off */
        fluid_synth_cc(synth, channel, 121, 0); /* reset controllers */
    }
}

static DWORD midi_open(MIDIOPENDESC *desc, DWORD flags, DWORD_PTR *user)
{
    static HANDLE thread;
    struct client *client;

    if (!desc) return MMSYSERR_INVALPARAM;
    if (!(client = malloc(sizeof(*client)))) return MMSYSERR_NOMEM;
    client->desc = *desc;
    client->flags = HIWORD(flags & CALLBACK_TYPEMASK);

    EnterCriticalSection(&synth_cs);
    if (!synth && !create_synth())
    {
        LeaveCriticalSection(&synth_cs);
        free(client);
        return MIDIERR_NODEVICE;
    }
    if (!render_wake) render_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!thread) thread = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
    ++client_count;
    LeaveCriticalSection(&synth_cs);
    SetEvent(render_wake);

    *user = (DWORD_PTR)client;
    notify(client, MOM_OPEN, 0);
    return MMSYSERR_NOERROR;
}

static DWORD midi_close(struct client *client)
{
    EnterCriticalSection(&synth_cs);
    if (!--client_count) fluid_synth_system_reset(synth);
    LeaveCriticalSection(&synth_cs);
    SetEvent(render_wake);

    notify(client, MOM_CLOSE, 0);
    free(client);
    return MMSYSERR_NOERROR;
}

static DWORD midi_data(DWORD message)
{
    BYTE status = message, data1 = message >> 8, data2 = message >> 16;
    int channel = status & 0x0f;

    EnterCriticalSection(&synth_cs);
    switch (status & 0xf0)
    {
    case 0x80: fluid_synth_noteoff(synth, channel, data1); break;
    case 0x90: fluid_synth_noteon(synth, channel, data1, data2); break;
    case 0xa0: fluid_synth_key_pressure(synth, channel, data1, data2); break;
    case 0xb0: fluid_synth_cc(synth, channel, data1, data2); break;
    case 0xc0: fluid_synth_program_change(synth, channel, data1); break;
    case 0xd0: fluid_synth_channel_pressure(synth, channel, data1); break;
    case 0xe0: fluid_synth_pitch_bend(synth, channel, (data2 << 7) | data1); break;
    case 0xf0:
        if (status == 0xff) fluid_synth_system_reset(synth);
        break;
    default:
        TRACE("Ignoring message %#lx.\n", message);
        break;
    }
    LeaveCriticalSection(&synth_cs);
    return MMSYSERR_NOERROR;
}

static DWORD midi_long_data(struct client *client, MIDIHDR *header, DWORD size)
{
    const BYTE *data, *end, *sysex_end;

    if (!header || size < offsetof(MIDIHDR, dwOffset)) return MMSYSERR_INVALPARAM;
    if (!header->lpData || !(header->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;
    if (header->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;

    data = (const BYTE *)header->lpData;
    end = data + header->dwBufferLength;

    EnterCriticalSection(&synth_cs);
    while (data < end)
    {
        if (*data != 0xf0)
        {
            ++data;
            continue;
        }
        for (sysex_end = data + 1; sysex_end < end && *sysex_end != 0xf7; ++sysex_end);
        if (sysex_end < end)
            fluid_synth_sysex(synth, (const char *)data + 1, sysex_end - data - 1, NULL, NULL, NULL, 0);
        data = sysex_end + 1;
    }
    LeaveCriticalSection(&synth_cs);

    header->dwFlags &= ~MHDR_INQUEUE;
    header->dwFlags |= MHDR_DONE;
    notify(client, MOM_DONE, (DWORD_PTR)header);
    return MMSYSERR_NOERROR;
}

static DWORD midi_prepare(MIDIHDR *header, DWORD size)
{
    if (!header || size < offsetof(MIDIHDR, dwOffset) || !header->lpData) return MMSYSERR_INVALPARAM;
    if (header->dwFlags & MHDR_PREPARED) return MMSYSERR_NOERROR;
    header->lpNext = NULL;
    header->dwFlags |= MHDR_PREPARED;
    header->dwFlags &= ~(MHDR_DONE | MHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

static DWORD midi_unprepare(MIDIHDR *header, DWORD size)
{
    if (!header || size < offsetof(MIDIHDR, dwOffset) || !header->lpData) return MMSYSERR_INVALPARAM;
    if (!(header->dwFlags & MHDR_PREPARED)) return MMSYSERR_NOERROR;
    if (header->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;
    header->dwFlags &= ~MHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

DWORD WINAPI modMessage(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR param1, DWORD_PTR param2)
{
    TRACE("%04x %04x %08Ix %08Ix %08Ix\n", id, msg, user, param1, param2);

    switch (msg)
    {
    case MODM_OPEN:
        return midi_open((MIDIOPENDESC *)param1, param2, (DWORD_PTR *)user);
    case MODM_CLOSE:
        return midi_close((struct client *)user);
    case MODM_DATA:
        return midi_data(param1);
    case MODM_LONGDATA:
        return midi_long_data((struct client *)user, (MIDIHDR *)param1, param2);
    case MODM_PREPARE:
        return midi_prepare((MIDIHDR *)param1, param2);
    case MODM_UNPREPARE:
        return midi_unprepare((MIDIHDR *)param1, param2);
    case MODM_RESET:
        EnterCriticalSection(&synth_cs);
        if (synth) reset_notes();
        LeaveCriticalSection(&synth_cs);
        return MMSYSERR_NOERROR;
    case MODM_GETVOLUME:
        if (!param1) return MMSYSERR_INVALPARAM;
        EnterCriticalSection(&synth_cs);
        *(DWORD *)param1 = volume;
        LeaveCriticalSection(&synth_cs);
        return MMSYSERR_NOERROR;
    case MODM_SETVOLUME:
        EnterCriticalSection(&synth_cs);
        volume = param1;
        volume_left = LOWORD(param1) / 65535.0f;
        volume_right = HIWORD(param1) / 65535.0f;
        LeaveCriticalSection(&synth_cs);
        return MMSYSERR_NOERROR;
    }

    FIXME("Unhandled message %#x.\n", msg);
    return MMSYSERR_NOTSUPPORTED;
}
