#include "application/Settings.h"

#include "common/ComUtil.h"
#include "common/Paths.h"
#include "common/StringUtil.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <map>

namespace da {

std::wstring Settings::DefaultPath() { return ConfigDirectory() + L"\\settings.ini"; }

namespace {

bool ParseBool(const std::wstring& v) { return v == L"1" || v == L"true" || v == L"yes"; }

template <class E>
E ParseEnum(const std::wstring& v, int count, E fallback) {
    const int i = _wtoi(v.c_str());
    return (i >= 0 && i < count && !v.empty()) ? static_cast<E>(i) : fallback;
}

} // namespace

bool Settings::Load(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string bytes(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 1 << 20)), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    bytes.resize(read);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF) bytes.erase(0, 3);

    std::map<std::wstring, std::wstring> kv;
    const std::wstring text = FromUtf8(bytes);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find_first_of(L"\r\n", pos);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.empty() || line[0] == L'#' || line[0] == L';' || line[0] == L'[') continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    auto get = [&](const wchar_t* k) -> const std::wstring* {
        auto it = kv.find(k);
        return it == kv.end() ? nullptr : &it->second;
    };
    if (auto v = get(L"theme")) theme = ParseEnum(*v, 3, ThemeMode::System);
    if (auto v = get(L"start_minimized")) startMinimized = ParseBool(*v);
    if (auto v = get(L"auto_start")) autoStart = ParseBool(*v);
    if (auto v = get(L"buffer")) buffer = ParseEnum(*v, 3, BufferMode::Balanced);
    if (auto v = get(L"sample_rate")) sampleRate = (_wtoi(v->c_str()) == 44100) ? 44100 : 48000;
    if (auto v = get(L"sync_strength")) strength = ParseEnum(*v, 3, SyncStrength::Normal);
    if (auto v = get(L"mic_calibration")) micCalibration = ParseBool(*v);
    if (auto v = get(L"clip_protection")) clipProtection = ParseBool(*v);
    if (auto v = get(L"sync")) syncEnabled = ParseBool(*v);
    if (auto v = get(L"source")) source = ParseEnum(*v, 2, SourceMode::AllApps);
    if (auto v = get(L"source_app")) sourceApp = *v;
    if (auto v = get(L"show_diagnostics")) showDiagnostics = ParseBool(*v);
    if (auto v = get(L"verbose_logging")) verboseLogging = ParseBool(*v);
    for (int i = 0; i < 2; ++i) {
        const std::wstring p = FormatW(L"output%d_", i + 1);
        if (auto v = get((p + L"id").c_str())) outputId[i] = *v;
        if (auto v = get((p + L"name").c_str())) outputName[i] = *v;
        if (auto v = get((p + L"volume").c_str())) volume[i] = std::clamp(static_cast<float>(_wtof(v->c_str())), 0.0f, 1.0f);
        if (auto v = get((p + L"muted").c_str())) muted[i] = ParseBool(*v);
        if (auto v = get((p + L"offset_ms").c_str())) offsetMs[i] = std::clamp(_wtof(v->c_str()), -500.0, 500.0);
    }
    return true;
}

bool Settings::Save(const std::wstring& path) const {
    std::wstring s = L"# DualAudio settings (UTF-8). Safe to delete; defaults are restored.\r\n";
    auto line = [&](const std::wstring& k, const std::wstring& v) { s += k + L"=" + v + L"\r\n"; };
    line(L"theme", std::to_wstring(static_cast<int>(theme)));
    line(L"start_minimized", startMinimized ? L"1" : L"0");
    line(L"auto_start", autoStart ? L"1" : L"0");
    line(L"buffer", std::to_wstring(static_cast<int>(buffer)));
    line(L"sample_rate", std::to_wstring(sampleRate));
    line(L"sync_strength", std::to_wstring(static_cast<int>(strength)));
    line(L"mic_calibration", micCalibration ? L"1" : L"0");
    line(L"clip_protection", clipProtection ? L"1" : L"0");
    line(L"sync", syncEnabled ? L"1" : L"0");
    line(L"source", std::to_wstring(static_cast<int>(source)));
    line(L"source_app", sourceApp);
    line(L"show_diagnostics", showDiagnostics ? L"1" : L"0");
    line(L"verbose_logging", verboseLogging ? L"1" : L"0");
    for (int i = 0; i < 2; ++i) {
        const std::wstring p = FormatW(L"output%d_", i + 1);
        line(p + L"id", outputId[i]);
        line(p + L"name", outputName[i]);
        line(p + L"volume", FormatW(L"%.3f", volume[i]));
        line(p + L"muted", muted[i] ? L"1" : L"0");
        line(p + L"offset_ms", FormatW(L"%.0f", offsetMs[i]));
    }
    const std::string bytes = ToUtf8(s);
    // Write to a temp file then replace, so a crash never leaves a half-written file.
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

namespace StartupShortcut {

namespace {
std::wstring ShortcutPath() {
    PWSTR dir = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &dir))) {
        out = std::wstring(dir) + L"\\DualAudio.lnk";
        CoTaskMemFree(dir);
    }
    return out;
}
} // namespace

bool IsEnabled() {
    const std::wstring p = ShortcutPath();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SetEnabled(bool enabled) {
    const std::wstring p = ShortcutPath();
    if (p.empty()) return false;
    if (!enabled) return DeleteFileW(p.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(exe);
    link->SetArguments(L"--startup");
    link->SetWorkingDirectory(ExeDirectory().c_str());
    link->SetDescription(L"DualAudio");
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    return SUCCEEDED(file->Save(p.c_str(), TRUE));
}

} // namespace StartupShortcut

} // namespace da
