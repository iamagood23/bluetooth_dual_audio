#include "diagnostics/Logger.h"

#include <cstdio>

namespace da {

namespace {
constexpr const wchar_t* kFileNames[] = {L"app.log", L"audio.log", L"device.log", L"synchronization.log"};
constexpr const wchar_t* kChannelTags[] = {L"app", L"audio", L"device", L"sync"};
constexpr const wchar_t* kLevelTags[] = {L"DEBUG", L"INFO", L"WARN", L"ERROR"};
constexpr LONGLONG kMaxLogBytes = 4 * 1024 * 1024;
constexpr size_t kRecentLines = 400;

HANDLE OpenLog(const std::wstring& path) {
    // Roll over large logs once at startup instead of growing forever.
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        const LONGLONG size = (static_cast<LONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (size > kMaxLogBytes) MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
} // namespace

Logger& Logger::Get() {
    static Logger instance;
    return instance;
}

void Logger::Start(const std::wstring& directory, bool verbose) {
    std::lock_guard lock(mu_);
    if (running_) return;
    dir_ = directory;
    verbose_ = verbose;
    for (int i = 0; i < static_cast<int>(LogChannel::Count); ++i) {
        files_[i] = OpenLog(directory + L"\\" + kFileNames[i]);
    }
    stop_ = false;
    running_ = true;
    worker_ = std::thread([this] { Run(); });
}

void Logger::Stop() {
    {
        std::lock_guard lock(mu_);
        if (!running_) return;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mu_);
    for (HANDLE& h : files_) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = nullptr;
    }
    running_ = false;
}

void Logger::Write(LogChannel ch, LogLevel lvl, std::wstring message) {
    if (lvl == LogLevel::Debug && !verbose_) return;
    Entry e{ch, lvl, {}, std::move(message)};
    GetLocalTime(&e.time);
    if (echo_) {
        wprintf(L"[%02u:%02u:%02u.%03u] %-5s %s: %s\n", e.time.wHour, e.time.wMinute, e.time.wSecond,
                e.time.wMilliseconds, kLevelTags[static_cast<int>(lvl)], kChannelTags[static_cast<int>(ch)],
                e.text.c_str());
    }
    {
        std::lock_guard lock(mu_);
        wchar_t prefix[64];
        swprintf_s(prefix, L"[%02u:%02u:%02u] %s ", e.time.wHour, e.time.wMinute, e.time.wSecond,
                   kChannelTags[static_cast<int>(ch)]);
        recent_.push_back(prefix + e.text);
        if (recent_.size() > kRecentLines) recent_.pop_front();
        if (!running_) return;
        queue_.push_back(std::move(e));
    }
    cv_.notify_one();
}

std::vector<std::wstring> Logger::RecentLines() const {
    std::lock_guard lock(mu_);
    return {recent_.begin(), recent_.end()};
}

void Logger::Run() {
    std::deque<Entry> batch;
    for (;;) {
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            batch.swap(queue_);
            if (batch.empty() && stop_) return;
        }
        Flush(batch);
        batch.clear();
    }
}

void Logger::Flush(std::deque<Entry>& batch) {
    for (const Entry& e : batch) {
        wchar_t prefix[64];
        swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %-5s ", e.time.wYear, e.time.wMonth,
                   e.time.wDay, e.time.wHour, e.time.wMinute, e.time.wSecond, e.time.wMilliseconds,
                   kLevelTags[static_cast<int>(e.lvl)]);
        const std::string line = ToUtf8(std::wstring(prefix) + e.text) + "\r\n";
        HANDLE h = files_[static_cast<int>(e.ch)];
        DWORD written = 0;
        if (h && h != INVALID_HANDLE_VALUE)
            WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        // Warnings and errors from every channel are mirrored into app.log for a single timeline.
        if (e.ch != LogChannel::App && e.lvl >= LogLevel::Warn) {
            HANDLE a = files_[static_cast<int>(LogChannel::App)];
            if (a && a != INVALID_HANDLE_VALUE)
                WriteFile(a, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
    }
}

} // namespace da
