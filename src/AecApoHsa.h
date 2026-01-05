//
// AecApoHsa.h -- AEC3APO settings manager (stub).
//

#pragma once

#include <atlbase.h>
#include <atlcom.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <AecApoDll_h.h>

_Analysis_mode_(_Analysis_code_type_user_driver_)

#pragma AVRT_VTABLES_BEGIN
    class CAecApoHsa : public CComObjectRootEx<CComMultiThreadModel>,
                       public CComCoClass<CAecApoHsa, &CLSID_Aec3ApoHsa>,
                       public IAudioSystemEffectsPropertyStore
{
public:
    CAecApoHsa() = default;

    DECLARE_NO_REGISTRY()

    BEGIN_COM_MAP(CAecApoHsa)
    COM_INTERFACE_ENTRY(IAudioSystemEffectsPropertyStore)
    END_COM_MAP()

    // IAudioSystemEffectsPropertyStore
    STDMETHOD(OpenDefaultPropertyStore)(DWORD /* stgmAccess */, IPropertyStore **propStore) override
    {
        if (propStore == nullptr)
        {
            return E_POINTER;
        }
        *propStore = nullptr;
        return PSCreateMemoryPropertyStore(IID_PPV_ARGS(propStore));
    }

    STDMETHOD(OpenUserPropertyStore)(DWORD /* stgmAccess */, IPropertyStore **propStore) override
    {
        return OpenDefaultPropertyStore(STGM_READ, propStore);
    }

    STDMETHOD(OpenVolatilePropertyStore)(DWORD /* stgmAccess */, IPropertyStore **propStore) override
    {
        return OpenDefaultPropertyStore(STGM_READWRITE, propStore);
    }

    STDMETHOD(ResetUserPropertyStore)() override
    {
        return S_OK;
    }

    STDMETHOD(ResetVolatilePropertyStore)() override
    {
        return S_OK;
    }

    STDMETHOD(RegisterPropertyChangeNotification)(
        IAudioSystemEffectsPropertyChangeNotificationClient * /* callback */) override
    {
        return S_OK;
    }

    STDMETHOD(UnregisterPropertyChangeNotification)(
        IAudioSystemEffectsPropertyChangeNotificationClient * /* callback */) override
    {
        return S_OK;
    }
};
#pragma AVRT_VTABLES_END

OBJECT_ENTRY_AUTO(__uuidof(Aec3ApoHsa), CAecApoHsa)
