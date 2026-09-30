#include "common/ComUtil.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstdio>

namespace da {

namespace {
struct NamedHr {
    HRESULT hr;
    const wchar_t* name;
};

constexpr NamedHr kAudioErrors[] = {
    {AUDCLNT_E_NOT_INITIALIZED, L"AUDCLNT_E_NOT_INITIALIZED"},
    {AUDCLNT_E_ALREADY_INITIALIZED, L"AUDCLNT_E_ALREADY_INITIALIZED"},
    {AUDCLNT_E_WRONG_ENDPOINT_TYPE, L"AUDCLNT_E_WRONG_ENDPOINT_TYPE"},
    {AUDCLNT_E_DEVICE_INVALIDATED, L"AUDCLNT_E_DEVICE_INVALIDATED"},
    {AUDCLNT_E_NOT_STOPPED, L"AUDCLNT_E_NOT_STOPPED"},
    {AUDCLNT_E_BUFFER_TOO_LARGE, L"AUDCLNT_E_BUFFER_TOO_LARGE"},
    {AUDCLNT_E_OUT_OF_ORDER, L"AUDCLNT_E_OUT_OF_ORDER"},
    {AUDCLNT_E_UNSUPPORTED_FORMAT, L"AUDCLNT_E_UNSUPPORTED_FORMAT"},
    {AUDCLNT_E_INVALID_SIZE, L"AUDCLNT_E_INVALID_SIZE"},
    {AUDCLNT_E_DEVICE_IN_USE, L"AUDCLNT_E_DEVICE_IN_USE"},
    {AUDCLNT_E_BUFFER_OPERATION_PENDING, L"AUDCLNT_E_BUFFER_OPERATION_PENDING"},
    {AUDCLNT_E_THREAD_NOT_REGISTERED, L"AUDCLNT_E_THREAD_NOT_REGISTERED"},
    {AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, L"AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED"},
    {AUDCLNT_E_ENDPOINT_CREATE_FAILED, L"AUDCLNT_E_ENDPOINT_CREATE_FAILED"},
    {AUDCLNT_E_SERVICE_NOT_RUNNING, L"AUDCLNT_E_SERVICE_NOT_RUNNING"},
    {AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED, L"AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED"},
    {AUDCLNT_E_EXCLUSIVE_MODE_ONLY, L"AUDCLNT_E_EXCLUSIVE_MODE_ONLY"},
    {AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL, L"AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL"},
    {AUDCLNT_E_EVENTHANDLE_NOT_SET, L"AUDCLNT_E_EVENTHANDLE_NOT_SET"},
    {AUDCLNT_E_INCORRECT_BUFFER_SIZE, L"AUDCLNT_E_INCORRECT_BUFFER_SIZE"},
    {AUDCLNT_E_BUFFER_SIZE_ERROR, L"AUDCLNT_E_BUFFER_SIZE_ERROR"},
    {AUDCLNT_E_CPUUSAGE_EXCEEDED, L"AUDCLNT_E_CPUUSAGE_EXCEEDED"},
    {AUDCLNT_E_BUFFER_ERROR, L"AUDCLNT_E_BUFFER_ERROR"},
    {AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, L"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"},
    {AUDCLNT_E_INVALID_DEVICE_PERIOD, L"AUDCLNT_E_INVALID_DEVICE_PERIOD"},
    {AUDCLNT_E_RESOURCES_INVALIDATED, L"AUDCLNT_E_RESOURCES_INVALIDATED"},
    {E_NOTFOUND, L"E_NOTFOUND (element not found)"},
};
} // namespace

std::wstring HResultToString(HRESULT hr) {
    wchar_t code[16];
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    for (const auto& e : kAudioErrors) {
        if (e.hr == hr) return std::wstring(e.name) + L" (" + code + L")";
    }
    wchar_t* msg = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(hr), 0,
                                     reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring out;
    if (len && msg) {
        out.assign(msg, len);
        while (!out.empty() && (out.back() == L'\n' || out.back() == L'\r' || out.back() == L' ' ||
                                out.back() == L'.'))
            out.pop_back();
        out += L" (";
        out += code;
        out += L")";
    } else {
        out = code;
    }
    if (msg) LocalFree(msg);
    return out;
}

} // namespace da
