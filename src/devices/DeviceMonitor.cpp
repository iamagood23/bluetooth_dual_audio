#include "devices/DeviceMonitor.h"

namespace da {

HRESULT DeviceMonitor::Start(IMMDeviceEnumerator* enumerator, Sink sink) {
    if (!enumerator) return E_POINTER;
    enumerator_ = enumerator;
    sink_ = std::move(sink);
    active_ = true;
    const HRESULT hr = enumerator_->RegisterEndpointNotificationCallback(this);
    if (FAILED(hr)) active_ = false;
    return hr;
}

void DeviceMonitor::Stop() {
    if (!enumerator_) return;
    active_ = false;
    // Blocks until in-flight callbacks have returned, so the sink is safe to destroy afterwards.
    enumerator_->UnregisterEndpointNotificationCallback(this);
    enumerator_.Reset();
}

HRESULT DeviceMonitor::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
        *ppv = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

void DeviceMonitor::Emit(DeviceEvent e) {
    if (active_ && sink_) sink_(std::move(e));
}

HRESULT DeviceMonitor::OnDeviceStateChanged(LPCWSTR id, DWORD state) {
    Emit({DeviceEventType::StateChanged, id ? id : L"", state});
    return S_OK;
}

HRESULT DeviceMonitor::OnDeviceAdded(LPCWSTR id) {
    Emit({DeviceEventType::Added, id ? id : L""});
    return S_OK;
}

HRESULT DeviceMonitor::OnDeviceRemoved(LPCWSTR id) {
    Emit({DeviceEventType::Removed, id ? id : L""});
    return S_OK;
}

HRESULT DeviceMonitor::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) {
    // Windows fires once per role; the console role is the one apps normally follow.
    if (role == eConsole) Emit({DeviceEventType::DefaultChanged, id ? id : L"", 0, flow});
    return S_OK;
}

} // namespace da
