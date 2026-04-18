#include "Mmcss.h"

#include <Avrt.h>

namespace asx {

MmcssScope::MmcssScope(const wchar_t* taskName)
{
    DWORD taskIndex = 0;
    handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
    if (handle_) {
        AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

MmcssScope::~MmcssScope()
{
    if (handle_) {
        AvRevertMmThreadCharacteristics(handle_);
    }
}

} // namespace asx

