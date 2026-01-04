//
// AecApoDll.cpp -- Copyright (c) Microsoft Corporation. All rights reserved.
//
// Author:
//
// Description:
//
// AecApoDll.cpp : Implementation of DLL Exports.

#include <atlbase.h>
#include <atlcom.h>
#include <atlcoll.h>
#include <atlsync.h>
#include <mmreg.h>
#include <windows.h>
#include <strsafe.h>
#include "resource.h"
#include "AecApoDll_h.h"
#include <AecApo.h>
#include <AecApoHsa.h>

#include <AecApoDll_i.c>


//-------------------------------------------------------------------------
// Array of APO_REG_PROPERTIES structures implemented in this module.
// Each new APO implementation will be added to this array.
//
APO_REG_PROPERTIES const *gCoreAPOs[] =
{
    &CAecApoMFX::sm_RegProperties.m_Properties
};

// {secret}
class CAecApoDllModule : public CAtlDllModuleT< CAecApoDllModule >
{
public :
    DECLARE_LIBID(LIBID_AecApoDlllib)
    DECLARE_REGISTRY_APPID_RESOURCEID(IDR_AECAPODLL, "{9270B9A3-1F7B-441F-B533-21E8BF83D4A3}")

};

// {secret}
CAecApoDllModule _AtlModule;


// {secret}
extern "C" BOOL WINAPI DllMain(HINSTANCE /* hInstance */, DWORD dwReason, LPVOID lpReserved)
{
    if (DLL_PROCESS_ATTACH == dwReason)
    {
    }
    // do necessary cleanup only if the DLL is being unloaded dynamically
    else if ((DLL_PROCESS_DETACH == dwReason) && (NULL == lpReserved))
    {
    }

    return _AtlModule.DllMain(dwReason, lpReserved);
}


// {secret}
STDAPI DllCanUnloadNow(void)
{
    return _AtlModule.DllCanUnloadNow();
}


// {secret}
STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}
