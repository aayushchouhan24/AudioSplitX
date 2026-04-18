#pragma once

#include <Windows.h>
#include <Audioclient.h>
#include <Objbase.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace asx {

constexpr REFERENCE_TIME kHnsPerSecond = 10'000'000;

REFERENCE_TIME msToHns(double ms);
double hnsToMs(REFERENCE_TIME hns);

std::wstring widen(std::string_view text);
std::string narrow(std::wstring_view text);

std::string hresultMessage(HRESULT hr);

class HResultError final : public std::runtime_error {
public:
    HResultError(HRESULT hr, std::string what);
    HRESULT code() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

void checkHr(HRESULT hr, const char* what);

template <typename T>
struct CoTaskMemDeleter {
    void operator()(T* value) const noexcept
    {
        if (value) {
            CoTaskMemFree(value);
        }
    }
};

template <typename T>
using CoMemPtr = std::unique_ptr<T, CoTaskMemDeleter<T>>;

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    ~UniqueHandle() { reset(); }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE release() noexcept
    {
        HANDLE out = handle_;
        handle_ = nullptr;
        return out;
    }

    void reset(HANDLE handle = nullptr) noexcept
    {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class ComApartment {
public:
    explicit ComApartment(DWORD model = COINIT_MULTITHREADED);
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ~ComApartment();

private:
    bool initialized_ = false;
};

std::chrono::steady_clock::time_point steadyNow();

} // namespace asx
