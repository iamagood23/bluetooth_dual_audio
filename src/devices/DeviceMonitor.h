#pragma once
// Windows endpoint change notifications (IMMNotificationClient).
//
// Callbacks arrive on a Windows-owned thread. We never do work there: every notification is
// forwarded to a caller-supplied sink that just enqueues it for the controller thread.
#include "common/ComUtil.h"

#include <mmdeviceapi.h>
#include <atomic>
#include <functional>
#include <string>

namespace da {

enum class DeviceEventType { Added, Removed, StateChanged, DefaultChanged };

struct DeviceEvent {
    DeviceEventType type;
    std::wstring id;
    DWORD newState = 0;       // StateChanged
    EDataFlow flow = eRender; // DefaultChanged
};

class DeviceMonitor final : public IMMNotificationClient {
public:
    using Sink = std::function<void(DeviceEvent)>;

    HRESULT Start(IMMDeviceEnumerator* enumerator, Sink sink);
    void Stop();

    // IUnknown (the object is owned by its creator; COM refcount only guards callbacks).
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void Emit(DeviceEvent e);
    std::atomic<ULONG> refs_{1};
    ComPtr<IMMDeviceEnumerator> enumerator_;
    Sink sink_;
    std::atomic<bool> active_{false};
};

} // namespace da
