#include "common/Paths.h"

#include <windows.h>
#include <shlobj.h>

namespace da {

std::wstring ExeDirectory() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool EnsureDirectory(const std::wstring& path) {
    const int r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

namespace {
bool IsWritable(const std::wstring& dir) {
    if (!EnsureDirectory(dir)) return false;
    const std::wstring probe = dir + L"\\.write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}
} // namespace

std::wstring DataDirectory() {
    static const std::wstring dir = [] {
        const std::wstring portable = ExeDirectory();
        if (IsWritable(portable + L"\\config")) return portable;
        PWSTR local = nullptr;
        std::wstring base = portable;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            base = std::wstring(local) + L"\\DualAudio";
            CoTaskMemFree(local);
        }
        EnsureDirectory(base);
        return base;
    }();
    return dir;
}

std::wstring ConfigDirectory() {
    const std::wstring d = DataDirectory() + L"\\config";
    EnsureDirectory(d);
    return d;
}

std::wstring LogsDirectory() {
    const std::wstring d = DataDirectory() + L"\\logs";
    EnsureDirectory(d);
    return d;
}

} // namespace da
