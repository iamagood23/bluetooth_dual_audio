#pragma once
#include <cstdint>
#include <string>

namespace da {

enum class DeviceKind { Bluetooth, Usb, Hdmi, Wired, Virtual, Other };

inline const wchar_t* DeviceKindLabel(DeviceKind k) {
    switch (k) {
    case DeviceKind::Bluetooth: return L"Bluetooth";
    case DeviceKind::Usb: return L"USB";
    case DeviceKind::Hdmi: return L"HDMI";
    case DeviceKind::Wired: return L"Wired";
    case DeviceKind::Virtual: return L"Virtual";
    default: return L"Other";
    }
}

// Snapshot of one active Windows render (or capture) endpoint, as reported by Core Audio.
struct AudioDeviceInfo {
    std::wstring id;            // IMMDevice endpoint ID
    std::wstring name;          // PKEY_Device_FriendlyName, e.g. "Headphones (OnePlus Buds 3)"
    std::wstring deviceName;    // PKEY_Device_DeviceDesc-ish short name, e.g. "OnePlus Buds 3"
    DeviceKind kind = DeviceKind::Other;
    bool handsFree = false;     // Bluetooth HFP endpoint (call quality, mono)
    bool isDefault = false;     // Windows default output (console role)
    uint32_t sampleRate = 0;    // shared-mode engine format, if known
    uint16_t channels = 0;
};

} // namespace da
