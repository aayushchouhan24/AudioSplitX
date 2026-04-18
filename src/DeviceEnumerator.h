#pragma once

#include "AudioFormat.h"

#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace asx {

struct AudioEndpoint {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

class EndpointNotificationClient final : public IMMNotificationClient {
public:
    EndpointNotificationClient() = default;

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR defaultDeviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override;
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD newState) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override;

    uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

private:
    void bump() noexcept { generation_.fetch_add(1, std::memory_order_acq_rel); }

    std::atomic<ULONG> refCount_ { 1 };
    std::atomic<uint64_t> generation_ { 0 };
};

class NotificationRegistration {
public:
    NotificationRegistration();
    NotificationRegistration(const NotificationRegistration&) = delete;
    NotificationRegistration& operator=(const NotificationRegistration&) = delete;
    ~NotificationRegistration();

    EndpointNotificationClient& client() noexcept { return *client_; }

private:
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    EndpointNotificationClient* client_ = nullptr;
};

class DeviceEnumerator {
public:
    std::vector<AudioEndpoint> listRenderEndpoints() const;
    AudioEndpoint defaultRenderEndpoint() const;
    AudioEndpoint resolveRenderEndpoint(std::wstring_view idOrName) const;
    AudioFormat getMixFormat(std::wstring_view endpointId) const;
    static Microsoft::WRL::ComPtr<IMMDeviceEnumerator> createEnumerator();

private:
    static AudioEndpoint endpointFromDevice(IMMDevice& device, std::wstring_view defaultId);
};

std::wstring toLower(std::wstring_view text);

} // namespace asx
