#pragma once
// Small COM / Win32 RAII helpers shared by every module.
#include <windows.h>
#include <wrl/client.h>
#include <memory>
#include <string>

namespace da {

using Microsoft::WRL::ComPtr;

// Human-readable HRESULT, including the WASAPI AUDCLNT_E_* names.
std::wstring HResultToString(HRESULT hr);

// Per-thread COM apartment guard. Only uninitializes if this instance initialized.
class ComInit {
public:
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) noexcept
        : hr_(CoInitializeEx(nullptr, model)) {}
    ~ComInit() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    // RPC_E_CHANGED_MODE means COM is usable on this thread, just in another apartment.
    bool usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
    HRESULT hr() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { CoTaskMemFree(p); }
};
template <class T>
using CoTaskMemPtr = std::unique_ptr<T, CoTaskMemDeleter>;

// Owns a kernel HANDLE (events, threads). Null and INVALID_HANDLE_VALUE are both "empty".
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : h_(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle&& o) noexcept : h_(o.release()) {}
    UniqueHandle& operator=(UniqueHandle&& o) noexcept {
        if (this != &o) reset(o.release());
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void reset(HANDLE h = nullptr) noexcept {
        if (*this) CloseHandle(h_);
        h_ = h;
    }

private:
    HANDLE h_ = nullptr;
};

} // namespace da
