#pragma once

#include <Windows.h>

namespace asx {

class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* taskName = L"Pro Audio");
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;
    ~MmcssScope();

private:
    HANDLE handle_ = nullptr;
};

} // namespace asx

