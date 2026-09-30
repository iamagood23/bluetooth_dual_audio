#include "devices/AudioSessions.h"

#include "common/ComUtil.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>

namespace da {

namespace {

std::wstring ExeNameForPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD len = static_cast<DWORD>(std::size(buf));
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        out.assign(buf, len);
        const size_t slash = out.find_last_of(L'\\');
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
    }
    CloseHandle(h);
    return out;
}

} // namespace

std::vector<AudioAppInfo> ListAudioApps() {
    std::vector<AudioAppInfo> apps;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return apps;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return apps;
    UINT count = 0;
    col->GetCount(&count);
    const DWORD self = GetCurrentProcessId();
    for (UINT d = 0; d < count; ++d) {
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioSessionManager2> mgr;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(col->Item(d, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &mgr)) ||
            FAILED(mgr->GetSessionEnumerator(&sessions)))
            continue;
        int n = 0;
        sessions->GetCount(&n);
        for (int i = 0; i < n; ++i) {
            ComPtr<IAudioSessionControl> ctl;
            ComPtr<IAudioSessionControl2> ctl2;
            if (FAILED(sessions->GetSession(i, &ctl)) || FAILED(ctl.As(&ctl2))) continue;
            if (ctl2->IsSystemSoundsSession() == S_OK) continue;
            DWORD pid = 0;
            if (FAILED(ctl2->GetProcessId(&pid)) || pid == 0 || pid == self) continue;
            AudioSessionState st = AudioSessionStateInactive;
            ctl->GetState(&st);
            std::wstring exe = ExeNameForPid(pid);
            if (exe.empty()) continue;
            auto it = std::find_if(apps.begin(), apps.end(),
                                   [&](const AudioAppInfo& a) { return _wcsicmp(a.exeName.c_str(), exe.c_str()) == 0; });
            if (it != apps.end()) {
                it->active = it->active || st == AudioSessionStateActive;
                continue;
            }
            AudioAppInfo info;
            info.pid = pid;
            info.exeName = exe;
            LPWSTR name = nullptr;
            if (SUCCEEDED(ctl->GetDisplayName(&name)) && name && *name && name[0] != L'@') info.displayName = name;
            if (name) CoTaskMemFree(name);
            if (info.displayName.empty()) {
                info.displayName = exe;
                const size_t dot = info.displayName.find_last_of(L'.');
                if (dot != std::wstring::npos) info.displayName.resize(dot);
                if (!info.displayName.empty()) info.displayName[0] = static_cast<wchar_t>(towupper(info.displayName[0]));
            }
            info.active = st == AudioSessionStateActive;
            apps.push_back(std::move(info));
        }
    }
    std::sort(apps.begin(), apps.end(), [](const AudioAppInfo& a, const AudioAppInfo& b) {
        if (a.active != b.active) return a.active;
        return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
    });
    return apps;
}

DWORD FindAudioAppPid(const std::wstring& exeName) {
    for (const auto& a : ListAudioApps())
        if (_wcsicmp(a.exeName.c_str(), exeName.c_str()) == 0) return a.pid;
    return 0;
}

} // namespace da
