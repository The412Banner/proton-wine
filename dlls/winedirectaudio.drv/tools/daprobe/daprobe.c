/*
 * daprobe - what does this audio driver actually offer?
 *
 * Answers three DIFFERENT questions that are easy to confuse, and prints them
 * separately, because a tool that prints only the first one is misleading:
 *
 *   1. ACCEPTED  - what IsFormatSupported() says in SHARED mode. On a driver
 *                  with a software mixer (DirectAudio, winealsa, winepulse)
 *                  this is close to "everything", because mmdevapi converts the
 *                  guest format before the driver ever sees it.
 *   2. NATIVE    - the same query in EXCLUSIVE mode, where there is no
 *                  conversion. This is the honest one.
 *   3. RENDERED  - GetMixFormat(): the format everything is actually converted
 *                  TO. A game told "yes, 7.1" still hears whatever this says.
 *
 * Run it inside the container with the driver under test selected. Writes
 * daprobe.txt NEXT TO THE EXE (not the working directory, which depends on how
 * it was launched) as well as printing, because a Wine console is not always
 * where you can read it. The resolved path is printed, so there is no hunting.
 *
 * Build: x86_64 PE (runs under FEX/box64 in any container).
 */

#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <string.h>

/* Defined here rather than pulled from ksmedia.h: the mingw headers declare these
 * subtype GUIDs but do not emit them, so linking against the header names fails.
 * They are fixed, well-known values - the PCM/float subtypes of
 * WAVEFORMATEXTENSIBLE - so carrying them locally costs nothing and removes a
 * header dependency from a tool that must build anywhere. */
DEFINE_GUID(DA_SUBTYPE_PCM,   0x00000001, 0x0000, 0x0010, 0x80, 0x00,
            0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(DA_SUBTYPE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00,
            0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

static FILE *g_out;
static char g_out_path[MAX_PATH * 2];

/* Beside the EXE, deliberately: the working directory depends on how the program
 * was launched (a shortcut sets it to the game folder, a bare run may leave it at
 * C:\\windows\\system32), and a report you have to go looking for is a report
 * that gets lost. Falls back to the working directory if the module path cannot
 * be resolved. */
static void open_report(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash;

    if (n > 0 && n < MAX_PATH)
    {
        slash = strrchr(path, '\\');
        if (slash)
        {
            *(slash + 1) = 0;
            snprintf(g_out_path, sizeof(g_out_path), "%sdaprobe.txt", path);
            g_out = fopen(g_out_path, "w");
        }
    }
    if (!g_out)
    {
        snprintf(g_out_path, sizeof(g_out_path), "daprobe.txt (working directory)");
        g_out = fopen("daprobe.txt", "w");
    }
}

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_out)
    {
        va_start(ap, fmt);
        vfprintf(g_out, fmt, ap);
        va_end(ap);
        fflush(g_out);
    }
}

struct layout { int channels; DWORD mask; const char *name; };

/* WAVEFORMATEXTENSIBLE channel masks for the layouts a game might ask for. */
static const struct layout LAYOUTS[] = {
    { 1, SPEAKER_FRONT_CENTER, "mono" },
    { 2, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, "stereo" },
    { 3, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY, "2.1" },
    { 4, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT, "quad" },
    { 6, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
         SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT, "5.1" },
    { 8, SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
         SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT, "7.1" },
};

static const int RATES[] = { 22050, 44100, 48000, 96000, 192000 };

/* 0 = 16-bit PCM, 1 = 32-bit float — the two a game realistically asks for. */
static void fill_format(WAVEFORMATEXTENSIBLE *w, const struct layout *l, int rate, int is_float)
{
    const int bits = is_float ? 32 : 16;

    ZeroMemory(w, sizeof(*w));
    w->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    w->Format.nChannels       = (WORD)l->channels;
    w->Format.nSamplesPerSec  = rate;
    w->Format.wBitsPerSample  = (WORD)bits;
    w->Format.nBlockAlign     = (WORD)(l->channels * bits / 8);
    w->Format.nAvgBytesPerSec = rate * w->Format.nBlockAlign;
    w->Format.cbSize          = sizeof(*w) - sizeof(WAVEFORMATEX);
    w->Samples.wValidBitsPerSample = (WORD)bits;
    w->dwChannelMask          = l->mask;
    w->SubFormat = is_float ? DA_SUBTYPE_FLOAT : DA_SUBTYPE_PCM;
}

static const char *verdict(HRESULT hr)
{
    if (hr == S_OK)                            return "yes";
    if (hr == S_FALSE)                         return "closest";   /* accepted, but it would substitute */
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)    return "no";
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED)    return "gone";
    if (hr == E_INVALIDARG)                    return "bad-arg";
    return "err";
}

static void describe_mix_format(const WAVEFORMATEX *f)
{
    const char *kind = "PCM";

    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) kind = "float";
    else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)f;
        if (IsEqualGUID(&e->SubFormat, &DA_SUBTYPE_FLOAT)) kind = "float";
    }
    emit("  %lu Hz  %u-bit %s  %u ch", f->nSamplesPerSec, f->wBitsPerSample, kind, f->nChannels);
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        emit("  (channel mask 0x%lx)", ((const WAVEFORMATEXTENSIBLE *)f)->dwChannelMask);
    emit("\n");
}

static void sweep(IAudioClient *client, AUDCLNT_SHAREMODE mode, const char *mode_name)
{
    size_t li, ri, fi;

    emit("\n%s MODE\n", mode_name);
    emit("  %-8s %-7s", "layout", "format");
    for (ri = 0; ri < sizeof(RATES) / sizeof(RATES[0]); ri++)
        emit(" %8d", RATES[ri]);
    emit("\n");

    for (li = 0; li < sizeof(LAYOUTS) / sizeof(LAYOUTS[0]); li++)
    {
        for (fi = 0; fi < 2; fi++)
        {
            emit("  %-8s %-7s", LAYOUTS[li].name, fi ? "float32" : "pcm16");
            for (ri = 0; ri < sizeof(RATES) / sizeof(RATES[0]); ri++)
            {
                WAVEFORMATEXTENSIBLE w;
                WAVEFORMATEX *closest = NULL;
                HRESULT hr;

                fill_format(&w, &LAYOUTS[li], RATES[ri], (int)fi);
                hr = IAudioClient_IsFormatSupported(client, mode, &w.Format,
                                                    mode == AUDCLNT_SHAREMODE_SHARED ? &closest : NULL);
                emit(" %8s", verdict(hr));
                if (closest) CoTaskMemFree(closest);
            }
            emit("\n");
        }
    }
}

static void probe_endpoint(IMMDevice *dev, const char *flow_name)
{
    IAudioClient *client = NULL;
    IPropertyStore *props = NULL;
    WAVEFORMATEX *mix = NULL;
    REFERENCE_TIME def_period = 0, min_period = 0;
    HRESULT hr;

    emit("\n================ %s ENDPOINT ================\n", flow_name);

    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &props)))
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &v)) && v.pwszVal)
            emit("device: %ls\n", v.pwszVal);
        PropVariantClear(&v);
        IPropertyStore_Release(props);
    }

    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr)) { emit("Activate failed: 0x%lx\n", hr); return; }

    emit("\nRENDERED  (GetMixFormat - what everything is converted TO)\n");
    if (SUCCEEDED(IAudioClient_GetMixFormat(client, &mix)) && mix)
    {
        describe_mix_format(mix);
        CoTaskMemFree(mix);
    }
    else emit("  unavailable\n");

    if (SUCCEEDED(IAudioClient_GetDevicePeriod(client, &def_period, &min_period)))
        emit("\nperiod: default %.2f ms, minimum %.2f ms\n",
             def_period / 10000.0, min_period / 10000.0);

    sweep(client, AUDCLNT_SHAREMODE_SHARED, "ACCEPTED - SHARED");
    sweep(client, AUDCLNT_SHAREMODE_EXCLUSIVE, "NATIVE - EXCLUSIVE");

    IAudioClient_Release(client);
}

static void enumerate(IMMDeviceEnumerator *en, EDataFlow flow, const char *name)
{
    IMMDeviceCollection *coll = NULL;
    IMMDevice *dev = NULL;
    UINT count = 0;

    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, flow, DEVICE_STATE_ACTIVE, &coll)))
    {
        IMMDeviceCollection_GetCount(coll, &count);
        emit("\n%s endpoints reported: %u\n", name, count);
        IMMDeviceCollection_Release(coll);
    }

    if (!count) return;

    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, flow, eMultimedia, &dev)))
    {
        probe_endpoint(dev, name);
        IMMDevice_Release(dev);
    }
}

int main(void)
{
    IMMDeviceEnumerator *en = NULL;
    HRESULT hr;

    open_report();

    emit("daprobe - audio capability report\n");
    emit("=================================\n");
    emit("ACCEPTED is what the driver ALLOWS (a software mixer converts, so it says yes a lot).\n");
    emit("NATIVE is what it opens WITHOUT conversion. RENDERED is what you actually hear.\n");

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) { emit("CoInitializeEx failed: 0x%lx\n", hr); return 1; }

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { emit("no device enumerator: 0x%lx\n", hr); CoUninitialize(); return 1; }

    enumerate(en, eRender, "RENDER");
    enumerate(en, eCapture, "CAPTURE");

    IMMDeviceEnumerator_Release(en);
    CoUninitialize();

    emit("\nwritten to %s\n", g_out_path);
    if (g_out) fclose(g_out);
    return 0;
}
