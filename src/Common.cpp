#include "Common.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace asx {

REFERENCE_TIME msToHns(double ms)
{
    return static_cast<REFERENCE_TIME>((ms / 1000.0) * static_cast<double>(kHnsPerSecond));
}

double hnsToMs(REFERENCE_TIME hns)
{
    return (static_cast<double>(hns) * 1000.0) / static_cast<double>(kHnsPerSecond);
}

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }

    std::wstring out(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), required);
    return out;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }

    std::string out(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), required, nullptr, nullptr);
    return out;
}

std::string hresultMessage(HRESULT hr)
{
    char* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageA(flags, nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&buffer), 0, nullptr);

    std::string message;
    if (len && buffer) {
        message.assign(buffer, len);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
    }

    if (buffer) {
        LocalFree(buffer);
    }

    std::ostringstream oss;
    oss << "0x" << std::hex << static_cast<unsigned long>(hr);
    if (!message.empty()) {
        oss << " (" << message << ")";
    }
    return oss.str();
}

HResultError::HResultError(HRESULT hr, std::string what)
    : std::runtime_error(std::move(what) + ": " + hresultMessage(hr))
    , hr_(hr)
{
}

void checkHr(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        throw HResultError(hr, what);
    }
}

ComApartment::ComApartment(DWORD model)
{
    const HRESULT hr = CoInitializeEx(nullptr, model);
    if (SUCCEEDED(hr)) {
        initialized_ = true;
        return;
    }

    if (hr == RPC_E_CHANGED_MODE) {
        initialized_ = false;
        return;
    }

    checkHr(hr, "CoInitializeEx");
}

ComApartment::~ComApartment()
{
    if (initialized_) {
        CoUninitialize();
    }
}

std::chrono::steady_clock::time_point steadyNow()
{
    return std::chrono::steady_clock::now();
}

} // namespace asx

