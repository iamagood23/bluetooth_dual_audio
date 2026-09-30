#pragma once
// Asynchronous, application-local logging.
//
// Callers format a line and enqueue it; a background thread timestamps-to-disk. Real-time
// audio threads must NOT call the logger: they bump counters that the sync/controller threads
// turn into log lines.
#include "common/StringUtil.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace da {

enum class LogChannel { App = 0, Audio, Device, Sync, Count };
enum class LogLevel { Debug = 0, Info, Warn, Error };

class Logger {
public:
    static Logger& Get();

    // Opens logs\app.log, audio.log, device.log, synchronization.log in `directory`.
    void Start(const std::wstring& directory, bool verbose);
    void Stop();
    void SetVerbose(bool verbose) { verbose_ = verbose; }
    bool Verbose() const { return verbose_; }
    // Also echo to stdout (CLI tools / tests).
    void SetConsoleEcho(bool echo) { echo_ = echo; }

    void Write(LogChannel ch, LogLevel lvl, std::wstring message);
    std::vector<std::wstring> RecentLines() const; // last ~400 lines, all channels
    std::wstring Directory() const { return dir_; }

private:
    struct Entry {
        LogChannel ch;
        LogLevel lvl;
        SYSTEMTIME time;
        std::wstring text;
    };
    void Run();
    void Flush(std::deque<Entry>& batch);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Entry> queue_;
    std::deque<std::wstring> recent_;
    std::thread worker_;
    bool running_ = false;
    bool stop_ = false;
    std::atomic<bool> verbose_{false};
    std::atomic<bool> echo_{false};
    std::wstring dir_;
    HANDLE files_[static_cast<int>(LogChannel::Count)] = {};
};

} // namespace da

#define DA_LOG(ch, lvl, ...) ::da::Logger::Get().Write(ch, lvl, ::da::FormatW(__VA_ARGS__))
#define LOG_APP(...) DA_LOG(::da::LogChannel::App, ::da::LogLevel::Info, __VA_ARGS__)
#define LOG_AUDIO(...) DA_LOG(::da::LogChannel::Audio, ::da::LogLevel::Info, __VA_ARGS__)
#define LOG_DEVICE(...) DA_LOG(::da::LogChannel::Device, ::da::LogLevel::Info, __VA_ARGS__)
#define LOG_SYNC(...) DA_LOG(::da::LogChannel::Sync, ::da::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(ch, ...) DA_LOG(ch, ::da::LogLevel::Warn, __VA_ARGS__)
#define LOG_ERROR(ch, ...) DA_LOG(ch, ::da::LogLevel::Error, __VA_ARGS__)
#define LOG_DEBUG(ch, ...) \
    do { if (::da::Logger::Get().Verbose()) DA_LOG(ch, ::da::LogLevel::Debug, __VA_ARGS__); } while (0)
