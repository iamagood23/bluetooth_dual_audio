// Instantiate the PKEY_AudioEndpoint_* / PKEY_AudioEngine_* property keys in this unit.
#include <windows.h>
#include <initguid.h>

#include "devices/DeviceEnumerator.h"

#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <algorithm>
#include <cwctype>

namespace da {

namespace {

std::wstring ReadString(IPropertyStore* ps, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}

UINT ReadUInt(IPropertyStore* ps, const PROPERTYKEY& key, UINT fallback) {
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT out = fallback;
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_UI4) out = v.ulVal;
    PropVariantClear(&v);
    return out;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// The device ID of the adapter (KS filter) an endpoint is connected to, via the documented
// IDeviceTopology interface. Its bus prefix tells us how the endpoint is physically attached:
//   \\?\bthenum#...    Bluetooth Classic A2DP     \\?\bthhfenum#...  Bluetooth hands-free
//   \\?\bthleaudio..   Bluetooth LE Audio         \\?\usb#...        USB
//   \\?\hdaudio#...    HD Audio codec             \\?\root#...       software (virtual) device
std::wstring AdapterPath(IMMDevice* device) {
    ComPtr<IDeviceTopology> topo;
    if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, &topo))) return {};
    ComPtr<IConnector> connector;
    if (FAILED(topo->GetConnector(0, &connector))) return {};
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(connector->GetDeviceIdConnectedTo(&id)) && id) {
        out = Lower(id);
        CoTaskMemFree(id);
    }
    return out;
}

} // namespace

HRESULT DeviceEnumerator::Initialize() {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
}

AudioDeviceInfo DeviceEnumerator::Describe(IMMDevice* device) {
    AudioDeviceInfo info;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        info.id = id;
        CoTaskMemFree(id);
    }
    ComPtr<IPropertyStore> ps;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &ps))) {
        info.name = ReadString(ps.Get(), PKEY_Device_FriendlyName);
        info.deviceName = ReadString(ps.Get(), PKEY_DeviceInterface_FriendlyName);
        const UINT formFactor = ReadUInt(ps.Get(), PKEY_AudioEndpoint_FormFactor, UnknownFormFactor);

        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) && v.vt == VT_BLOB &&
            v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(v.blob.pBlobData);
            info.sampleRate = wf->nSamplesPerSec;
            info.channels = wf->nChannels;
        }
        PropVariantClear(&v);

        const std::wstring path = AdapterPath(device);
        if (path.find(L"bthhfenum") != std::wstring::npos) {
            info.kind = DeviceKind::Bluetooth;
            info.handsFree = true;
        } else if (path.find(L"bthenum") != std::wstring::npos || path.find(L"bthleaudio") != std::wstring::npos ||
                   path.find(L"bthledevice") != std::wstring::npos) {
            info.kind = DeviceKind::Bluetooth;
        } else if (formFactor == DigitalAudioDisplayDevice) {
            info.kind = DeviceKind::Hdmi;
        } else if (path.find(L"\\usb#") != std::wstring::npos || path.find(L"usbaudio") != std::wstring::npos) {
            info.kind = DeviceKind::Usb;
        } else if (path.find(L"\\root#") != std::wstring::npos || path.find(L"\\swd#") != std::wstring::npos) {
            info.kind = DeviceKind::Virtual;
        } else if (path.find(L"hdaudio") != std::wstring::npos || path.find(L"intelaudio") != std::wstring::npos ||
                   path.find(L"acpi") != std::wstring::npos || formFactor == Speakers ||
                   formFactor == Headphones || formFactor == LineLevel) {
            info.kind = DeviceKind::Wired;
        }
        // Name-based fallback for hands-free endpoints on stacks with unfamiliar adapter paths.
        if (info.kind == DeviceKind::Bluetooth && Lower(info.name).find(L"hands-free") != std::wstring::npos)
            info.handsFree = true;
    }
    if (info.deviceName.empty()) info.deviceName = info.name;
    return info;
}

std::vector<AudioDeviceInfo> DeviceEnumerator::Enumerate(EDataFlow flow, HRESULT* error) const {
    std::vector<AudioDeviceInfo> out;
    HRESULT hr = enumerator_ ? S_OK : E_POINTER;
    ComPtr<IMMDeviceCollection> col;
    if (SUCCEEDED(hr)) hr = enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col);
    UINT count = 0;
    if (SUCCEEDED(hr)) hr = col->GetCount(&count);
    const std::wstring def = flow == eRender ? DefaultRenderId() : DefaultCaptureId();
    for (UINT i = 0; SUCCEEDED(hr) && i < count; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        AudioDeviceInfo info = Describe(d.Get());
        info.isDefault = !def.empty() && info.id == def;
        out.push_back(std::move(info));
    }
    if (error) *error = hr;
    // Stable, friendly ordering: Bluetooth first, hands-free endpoints last, then by name.
    std::stable_sort(out.begin(), out.end(), [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
        auto rank = [](const AudioDeviceInfo& d) {
            if (d.kind == DeviceKind::Bluetooth && !d.handsFree) return 0;
            if (d.kind == DeviceKind::Virtual) return 3;
            if (d.handsFree) return 4;
            return 1;
        };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::vector<AudioDeviceInfo> DeviceEnumerator::ActiveRenderDevices(HRESULT* error) const {
    return Enumerate(eRender, error);
}

std::vector<AudioDeviceInfo> DeviceEnumerator::ActiveCaptureDevices(HRESULT* error) const {
    return Enumerate(eCapture, error);
}

namespace {
std::wstring DefaultId(IMMDeviceEnumerator* en, EDataFlow flow) {
    if (!en) return {};
    ComPtr<IMMDevice> d;
    if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, &d))) return {};
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(d->GetId(&id)) && id) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}
} // namespace

std::wstring DeviceEnumerator::DefaultRenderId() const { return DefaultId(enumerator_.Get(), eRender); }
std::wstring DeviceEnumerator::DefaultCaptureId() const { return DefaultId(enumerator_.Get(), eCapture); }

ComPtr<IMMDevice> DeviceEnumerator::Open(const std::wstring& id) const {
    ComPtr<IMMDevice> d;
    if (enumerator_ && !id.empty()) enumerator_->GetDevice(id.c_str(), &d);
    return d;
}

DWORD DeviceEnumerator::State(const std::wstring& id) const {
    ComPtr<IMMDevice> d = Open(id);
    DWORD state = 0;
    if (d) d->GetState(&state);
    return state;
}

} // namespace da
