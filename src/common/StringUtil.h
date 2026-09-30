#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace da {

inline std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

inline std::wstring FromUtf8(std::string_view s) {
    if (s.empty()) return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::wstring FormatW(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(buf, n < 0 ? wcslen(buf) : static_cast<size_t>(n));
}

// Short, non-reversible tag for a device ID so logs can correlate devices
// without recording the full endpoint identifier.
inline std::wstring ShortIdTag(std::wstring_view id) {
    uint32_t h = 2166136261u; // FNV-1a
    for (wchar_t c : id) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    return FormatW(L"#%04X", h & 0xFFFF);
}

} // namespace da
