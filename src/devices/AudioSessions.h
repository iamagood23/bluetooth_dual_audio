#pragma once
// Lists applications that currently have audio sessions (IAudioSessionManager2), for the
// "Single app" capture source. Read-only.
#include <windows.h>
#include <string>
#include <vector>

namespace da {

struct AudioAppInfo {
    DWORD pid = 0;
    std::wstring exeName;   // e.g. "spotify.exe"
    std::wstring displayName;
    bool active = false;    // session currently playing
};

std::vector<AudioAppInfo> ListAudioApps();
// First running process with an audio session whose executable matches (case-insensitive).
DWORD FindAudioAppPid(const std::wstring& exeName);

} // namespace da
