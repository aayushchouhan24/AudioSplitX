#include "DeviceEnumerator.h"

#include "Common.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <Propvarutil.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace asx {

std::wstring toLower(std::wstring_view text)
{
    std::wstring out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return out;
}

ULONG EndpointNotificationClient::AddRef()
{
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG EndpointNotificationClient::Release()
{
    const ULONG value = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (value == 0) {
        delete this;
    }
    return value;
}

HRESULT EndpointNotificationClient::QueryInterface(REFIID iid, void** object)
{
    if (!object) {
        return E_POINTER;
    }
    *object = nullptr;

    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

HRESULT EndpointNotificationClient::OnDefaultDeviceChanged(EDataFlow flow, ERole, LPCWSTR)
{
    if (flow == eRender) {
        bump();
    }
    return S_OK;
}

HRESULT EndpointNotificationClient::OnDeviceAdded(LPCWSTR)
{
    bump();
    return S_OK;
}

HRESULT EndpointNotificationClient::OnDeviceRemoved(LPCWSTR)
{
    bump();
    return S_OK;
}

HRESULT EndpointNotificationClient::OnDeviceStateChanged(LPCWSTR, DWORD)
{
    return S_OK;
}

HRESULT EndpointNotificationClient::OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY)
{
    return S_OK;
}

NotificationRegistration::NotificationRegistration()
{
    enumerator_ = DeviceEnumerator::createEnumerator();
    client_ = new EndpointNotificationClient();
    checkHr(enumerator_->RegisterEndpointNotificationCallback(client_), "RegisterEndpointNotificationCallback");
}

NotificationRegistration::~NotificationRegistration()
{
    if (enumerator_ && client_) {
        enumerator_->UnregisterEndpointNotificationCallback(client_);
    }
    if (client_) {
        client_->Release();
        client_ = nullptr;
    }
}

ComPtr<IMMDeviceEnumerator> DeviceEnumerator::createEnumerator()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    checkHr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
        "CoCreateInstance(MMDeviceEnumerator)");
    return enumerator;
}

AudioEndpoint DeviceEnumerator::endpointFromDevice(IMMDevice& device, std::wstring_view defaultId)
{
    AudioEndpoint endpoint;

    LPWSTR id = nullptr;
    checkHr(device.GetId(&id), "IMMDevice::GetId");
    endpoint.id = id ? id : L"";
    CoTaskMemFree(id);
    endpoint.isDefault = !defaultId.empty() && endpoint.id == defaultId;

    ComPtr<IPropertyStore> props;
    checkHr(device.OpenPropertyStore(STGM_READ, &props), "IMMDevice::OpenPropertyStore");

    PROPVARIANT value;
    PropVariantInit(&value);
    checkHr(props->GetValue(PKEY_Device_FriendlyName, &value), "IPropertyStore::GetValue(PKEY_Device_FriendlyName)");
    if (value.vt == VT_LPWSTR && value.pwszVal) {
        endpoint.name = value.pwszVal;
    }
    PropVariantClear(&value);

    return endpoint;
}

std::vector<AudioEndpoint> DeviceEnumerator::listRenderEndpoints() const
{
    auto enumerator = createEnumerator();

    std::wstring defaultId;
    ComPtr<IMMDevice> defaultDevice;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice)) && defaultDevice) {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&raw)) && raw) {
            defaultId = raw;
            CoTaskMemFree(raw);
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    checkHr(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "EnumAudioEndpoints(eRender)");

    UINT count = 0;
    checkHr(collection->GetCount(&count), "IMMDeviceCollection::GetCount");

    std::vector<AudioEndpoint> endpoints;
    endpoints.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        checkHr(collection->Item(i, &device), "IMMDeviceCollection::Item");
        endpoints.push_back(endpointFromDevice(*device.Get(), defaultId));
    }

    return endpoints;
}

AudioEndpoint DeviceEnumerator::defaultRenderEndpoint() const
{
    auto enumerator = createEnumerator();
    ComPtr<IMMDevice> device;
    checkHr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "GetDefaultAudioEndpoint(eRender)");

    LPWSTR raw = nullptr;
    checkHr(device->GetId(&raw), "IMMDevice::GetId(default)");
    std::wstring defaultId = raw ? raw : L"";
    CoTaskMemFree(raw);
    return endpointFromDevice(*device.Get(), defaultId);
}

AudioEndpoint DeviceEnumerator::resolveRenderEndpoint(std::wstring_view idOrName) const
{
    const auto endpoints = listRenderEndpoints();
    const std::wstring query(idOrName);
    const std::wstring lowered = toLower(query);

    for (const auto& endpoint : endpoints) {
        if (endpoint.id == query) {
            return endpoint;
        }
    }

    std::vector<AudioEndpoint> matches;
    for (const auto& endpoint : endpoints) {
        if (toLower(endpoint.name).find(lowered) != std::wstring::npos) {
            matches.push_back(endpoint);
        }
    }

    if (matches.size() == 1) {
        return matches.front();
    }

    if (matches.empty()) {
        throw std::runtime_error("No active render endpoint matches '" + narrow(query) + "'");
    }

    throw std::runtime_error("Multiple active render endpoints match '" + narrow(query) + "'. Use --list and pass --out-id/--source-id.");
}

AudioFormat DeviceEnumerator::getMixFormat(std::wstring_view endpointId) const
{
    auto enumerator = createEnumerator();

    ComPtr<IMMDevice> device;
    checkHr(enumerator->GetDevice(std::wstring(endpointId).c_str(), &device), "IMMDeviceEnumerator::GetDevice");

    ComPtr<IAudioClient> client;
    checkHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "IMMDevice::Activate(IAudioClient)");

    WAVEFORMATEX* raw = nullptr;
    checkHr(client->GetMixFormat(&raw), "IAudioClient::GetMixFormat");
    CoMemPtr<WAVEFORMATEX> format(raw);
    return parseWaveFormat(*format);
}

} // namespace asx
