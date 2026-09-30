#pragma once
// Core Audio endpoint enumeration (IMMDeviceEnumerator). Read-only: never changes Windows'
// device configuration.
#include "common/ComUtil.h"
#include "devices/DeviceState.h"

#include <mmdeviceapi.h>
#include <vector>

namespace da {

class DeviceEnumerator {
public:
    // Caller's thread must have COM initialized.
    HRESULT Initialize();
    IMMDeviceEnumerator* Raw() const { return enumerator_.Get(); }

    std::vector<AudioDeviceInfo> ActiveRenderDevices(HRESULT* error = nullptr) const;
    std::vector<AudioDeviceInfo> ActiveCaptureDevices(HRESULT* error = nullptr) const;
    std::wstring DefaultRenderId() const;
    std::wstring DefaultCaptureId() const;
    ComPtr<IMMDevice> Open(const std::wstring& id) const;
    // DEVICE_STATE_* of an endpoint, or 0 if unknown to Windows.
    DWORD State(const std::wstring& id) const;

    static AudioDeviceInfo Describe(IMMDevice* device);

private:
    std::vector<AudioDeviceInfo> Enumerate(EDataFlow flow, HRESULT* error) const;
    ComPtr<IMMDeviceEnumerator> enumerator_;
};

} // namespace da
