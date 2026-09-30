// Throwaway feasibility probe (architecture assessment step). Not part of the product.
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <endpointvolume.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <cmath>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace Microsoft::WRL;

static void DumpProps(IMMDevice* dev) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return;
    DWORD n = 0;
    ps->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        PROPERTYKEY k;
        ps->GetAt(i, &k);
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(k, &v))) {
            wchar_t buf[512] = L"";
            if (v.vt == VT_LPWSTR || v.vt == VT_UI4 || v.vt == VT_BOOL || v.vt == VT_CLSID)
                PropVariantToString(v, buf, 512);
            else
                swprintf_s(buf, L"<vt=%u>", v.vt);
            wchar_t g[64];
            StringFromGUID2(k.fmtid, g, 64);
            wprintf(L"    %s,%lu = %s\n", g, k.pid, buf);
        }
        PropVariantClear(&v);
    }
}

static void DumpTopology(IMMDevice* dev) {
    ComPtr<IDeviceTopology> topo;
    if (FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topo))) return;
    ComPtr<IConnector> c;
    if (FAILED(topo->GetConnector(0, &c))) return;
    LPWSTR id = nullptr;
    if (SUCCEEDED(c->GetDeviceIdConnectedTo(&id))) {
        wprintf(L"    ConnectedTo: %s\n", id);
        CoTaskMemFree(id);
    }
}

class Handler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase,
                                    IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    ComPtr<IAudioClient> client;
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT act = E_FAIL;
        ComPtr<IUnknown> unk;
        hr = op->GetActivateResult(&act, &unk);
        if (SUCCEEDED(hr)) hr = act;
        if (SUCCEEDED(hr)) hr = unk.As(&client);
        SetEvent(done);
        return S_OK;
    }
};

static void CaptureTest(IAudioClient* client, bool processLoopback, int seconds) {
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 8;
    wf.nAvgBytesPerSec = 48000 * 8;
    WAVEFORMATEX* fmt = &wf;
    WAVEFORMATEX* mix = nullptr;
    if (!processLoopback) {
        client->GetMixFormat(&mix);
        fmt = mix;
    }
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (processLoopback) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, fmt, nullptr);
    wprintf(L"  Initialize: 0x%08X  (rate %lu ch %u bits %u)\n", hr, fmt->nSamplesPerSec, fmt->nChannels,
            fmt->wBitsPerSample);
    if (FAILED(hr)) return;
    UINT32 bufFrames = 0;
    client->GetBufferSize(&bufFrames);
    REFERENCE_TIME lat = 0;
    HRESULT hrl = client->GetStreamLatency(&lat);
    wprintf(L"  buffer=%u frames, GetStreamLatency hr=0x%08X lat=%lld hns\n", bufFrames, hrl, lat);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client->SetEventHandle(ev);
    ComPtr<IAudioCaptureClient> cap;
    client->GetService(IID_PPV_ARGS(&cap));
    client->Start();
    const bool isFloat = processLoopback || true;
    (void)isFloat;
    uint64_t frames = 0, packets = 0, silent = 0, disc = 0, tsErr = 0, timeouts = 0;
    float peak = 0;
    UINT64 firstQpc = 0, lastQpc = 0, firstPos = 0, lastPos = 0;
    UINT32 lastN = 0;
    uint64_t hist[6] = {};
    double maxDev = 0;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)seconds * 1000) {
        if (WaitForSingleObject(ev, 200) == WAIT_TIMEOUT) { ++timeouts; continue; }
        for (;;) {
            BYTE* data;
            UINT32 n;
            DWORD fl;
            UINT64 pos, qpc;
            hr = cap->GetBuffer(&data, &n, &fl, &pos, &qpc);
            if (hr == AUDCLNT_S_BUFFER_EMPTY || FAILED(hr)) break;
            if (fl & AUDCLNT_BUFFERFLAGS_SILENT) ++silent;
            if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++disc;
            if (fl & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++tsErr;
            if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT) && fmt->wBitsPerSample == 32) {
                const float* f = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < n * fmt->nChannels; ++i) peak = std::max(peak, std::fabs(f[i]));
            }
            if (!packets) { firstQpc = qpc; firstPos = pos; }
            if (packets) {
                // deviation of this packet's timestamp from the continuation of the previous one
                const double dev = (double)qpc - ((double)lastQpc + lastN * 1e7 / fmt->nSamplesPerSec);
                const double a = fabs(dev) / 1e4; // ms
                int b = a < 0.1 ? 0 : a < 0.5 ? 1 : a < 1 ? 2 : a < 3 ? 3 : a < 15 ? 4 : 5;
                hist[b]++;
                if (a > maxDev) maxDev = a;
            }
            lastN = n;
            lastQpc = qpc; lastPos = pos;
            frames += n; ++packets;
            cap->ReleaseBuffer(n);
        }
    }
    client->Stop();
    double spanS = (lastQpc - firstQpc) / 1e7;
    wprintf(L"  packets=%llu frames=%llu silentPkts=%llu disc=%llu tsErr=%llu waitTimeouts=%llu peak=%.4f\n",
            packets, frames, silent, disc, tsErr, timeouts, peak);
    wprintf(L"  qpc span=%.3fs devpos span=%llu -> rate by qpc=%.1f Hz\n", spanS, lastPos - firstPos,
            spanS > 0 ? (lastPos - firstPos) / spanS : 0.0);
    wprintf(L"  timestamp deviation |dev|: <0.1ms %llu, <0.5 %llu, <1 %llu, <3 %llu, <15 %llu, >=15 %llu, max %.2f ms\n",
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], maxDev);
    if (mix) CoTaskMemFree(mix);
    CloseHandle(ev);
}

// Quiet (-30 dBFS) 440 Hz tone into device #idx, shared mode, for `secs` seconds.
static void Tone(IMMDevice* d, int secs) {
    ComPtr<IAudioClient> c;
    d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c);
    WAVEFORMATEX* mix = nullptr;
    c->GetMixFormat(&mix);
    HRESULT hr = c->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr);
    wprintf(L"tone init 0x%08X rate %lu ch %u\n", hr, mix->nSamplesPerSec, mix->nChannels);
    ComPtr<IAudioRenderClient> r;
    c->GetService(IID_PPV_ARGS(&r));
    UINT32 buf;
    c->GetBufferSize(&buf);
    c->Start();
    double ph = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        UINT32 pad;
        c->GetCurrentPadding(&pad);
        UINT32 n = buf - pad;
        BYTE* p;
        if (n && SUCCEEDED(r->GetBuffer(n, &p))) {
            float* f = (float*)p;
            for (UINT32 i = 0; i < n; ++i) {
                float s = 0.0316f * (float)sin(ph);
                ph += 2 * 3.14159265358979 * 440 / mix->nSamplesPerSec;
                for (int ch = 0; ch < mix->nChannels; ++ch) *f++ = s;
            }
            r->ReleaseBuffer(n, 0);
        }
        Sleep(20);
    }
    c->Stop();
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (argc > 3 && wcscmp(argv[1], L"tone") == 0) {
        ComPtr<IMMDeviceCollection> col;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
        ComPtr<IMMDevice> d;
        col->Item(_wtoi(argv[2]), &d);
        Tone(d.Get(), _wtoi(argv[3]));
        return 0;
    }
    bool dump = argc > 1 && wcscmp(argv[1], L"dump") == 0;
    ComPtr<IMMDevice> def;
    en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def);
    LPWSTR defId = nullptr;
    if (def) def->GetId(&defId);
    ComPtr<IMMDeviceCollection> col;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT cnt = 0;
    col->GetCount(&cnt);
    for (UINT i = 0; i < cnt; ++i) {
        ComPtr<IMMDevice> d;
        col->Item(i, &d);
        LPWSTR id;
        d->GetId(&id);
        ComPtr<IPropertyStore> ps;
        d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v;
        PropVariantInit(&v);
        ps->GetValue(PKEY_Device_FriendlyName, &v);
        wprintf(L"[%u]%s %s\n", i, (defId && !wcscmp(id, defId)) ? L" (DEFAULT)" : L"", v.pwszVal);
        PropVariantClear(&v);
        DumpTopology(d.Get());
        if (dump) DumpProps(d.Get());
        CoTaskMemFree(id);
    }
    int secs = 4;
    if (argc > 3 && wcscmp(argv[1], L"mute") == 0) {
        ComPtr<IMMDevice> d;
        col->Item(_wtoi(argv[2]), &d);
        ComPtr<IAudioEndpointVolume> vol;
        d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &vol);
        BOOL was = FALSE;
        vol->GetMute(&was);
        vol->SetMute(_wtoi(argv[3]), nullptr);
        wprintf(L"mute was %d now %d\n", was, _wtoi(argv[3]));
        return 0;
    }
    const bool include = argc > 2 && wcscmp(argv[1], L"capinc") == 0;
    if (!include && argc > 2 && wcscmp(argv[1], L"cap") == 0) secs = _wtoi(argv[2]);
    if ((argc > 1 && wcscmp(argv[1], L"cap") == 0) || include) {
        wprintf(L"\n== Process loopback (%s) ==\n", include ? L"include pid" : L"exclude self");
        auto h = Make<Handler>();
        AUDIOCLIENT_ACTIVATION_PARAMS p{};
        p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        p.ProcessLoopbackParams.TargetProcessId = include ? (DWORD)_wtoi(argv[2]) : GetCurrentProcessId();
        p.ProcessLoopbackParams.ProcessLoopbackMode = include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                                                              : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(p);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&p);
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                                 &pv, h.Get(), &op);
        wprintf(L"  ActivateAudioInterfaceAsync: 0x%08X\n", hr);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(h->done, 5000);
            wprintf(L"  activation result: 0x%08X\n", h->hr);
            if (h->client) CaptureTest(h->client.Get(), true, secs);
        }
        if (include) return 0;
        wprintf(L"\n== Endpoint loopback (default device) ==\n");
        ComPtr<IAudioClient> ac;
        def->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac);
        CaptureTest(ac.Get(), false, secs);
    }
    if (defId) CoTaskMemFree(defId);
    return 0;
}
