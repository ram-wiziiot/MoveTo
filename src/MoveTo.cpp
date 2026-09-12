//////////////////////////////////////////////////////////////////////
//
// MoveTo.cpp : Implementation of DLL Exports.
//
// The registration plumbing is DirClean's, with four changes, each of
// which is explained where it happens:
//
//   1. The handler registers under AllFilesystemObjects, not Folder.
//   2. The Approved-shell-extensions entry is written *after* a
//      successful RegisterServer, and a failure to write it rolls the
//      registration back, so regsvr32 never leaves half a handler.
//   3. DllInstall is exported, so the DLL can be registered per-user
//      (HKCU\Software\Classes) with no elevation.
//   4. The Approved entry is skipped for a per-user registration: that
//      list lives in HKLM and a standard user cannot write it.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "resource.h"
#include <initguid.h>
#include "MoveTo.h"

#include "MoveTo_i.c"
#include "MoveToShlExt.h"
#include <shlobj.h>

CComModule _Module;

// The *coclass* CLSID, in the string form the Approved list expects.
// DirClean 1.1.1 wrote the type library GUID here and swapped the value
// name with the value data, so its Approved entry never took effect and
// DllUnregisterServer deleted a name that had never been written.
// Value NAME = CLSID, value DATA = description.
static LPCTSTR const szCLSID =
    _T("{DE113EFB-7938-4BCA-919F-787F0729C2A3}");

static LPCTSTR const szApprovedKey =
    _T("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved");

static LPCTSTR const szApprovedDesc = _T("MoveTo Shell Extension");

BEGIN_OBJECT_MAP(ObjectMap)
OBJECT_ENTRY(CLSID_MoveToShlExt, CMoveToShlExt)
END_OBJECT_MAP()

class CMoveToApp : public CWinApp
{
public:
    virtual BOOL InitInstance();
    virtual int  ExitInstance();

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CMoveToApp, CWinApp)
END_MESSAGE_MAP()

CMoveToApp theApp;

BOOL CMoveToApp::InitInstance()
{
    _Module.Init ( ObjectMap, m_hInstance, &LIBID_MOVETOLib );

    // Fixes HKCU\Software\<this>\MoveTo as the root for GetProfile* /
    // WriteProfile*, which is where the destination MRU and the pinned
    // list live.  Must be set before anything reads them.
    SetRegistryKey ( _T("Wiziiot") );

    return CWinApp::InitInstance();
}

int CMoveToApp::ExitInstance()
{
    _Module.Term();
    return CWinApp::ExitInstance();
}


/////////////////////////////////////////////////////////////////////////////
// Used to determine whether the DLL can be unloaded by OLE

STDAPI DllCanUnloadNow(void)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    return ( AfxDllCanUnloadNow() == S_OK && _Module.GetLockCount() == 0 )
               ? S_OK : S_FALSE;
}

/////////////////////////////////////////////////////////////////////////////
// Returns a class factory to create an object of the requested type

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    return _Module.GetClassObject ( rclsid, riid, ppv );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    IsPerUserRegistration()
//
// Description:
//  True when DllInstall was called with "user", i.e. when ATL's
//  registrar is rewriting the leading HKCR of MoveToShlExt.rgs into
//  HKCU\Software\Classes and registering the type library with
//  RegisterTypeLibForUser.
//
//  Verified: atlbase.h:7092 AtlSetPerUserRegistration sets the global
//  _AtlRegisterPerUser (atlbase.h:302); statreg.h:1192-1235
//  CRegParser::PreProcessBuffer does the HKCR -> HKCU\Software\Classes
//  textual rewrite; atlbase.h:7426-7440 AtlRegisterTypeLib switches to
//  OLEAUT32!RegisterTypeLibForUser.
//
//////////////////////////////////////////////////////////////////////////

static bool IsPerUserRegistration()
{
bool bEnabled = false;

    return SUCCEEDED ( AtlGetPerUserRegistration ( &bEnabled ) ) && bEnabled;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    SetApprovedEntry()
//
// Description:
//  Adds or removes our CLSID in the machine-wide Approved list.
//
//  That list is HKLM-only -- there is no per-user equivalent -- so a
//  per-user registration cannot participate in it.  That matters only
//  where the EnforceShellExtensionSecurity policy is set; where it is
//  not set (the default, including this development machine) the shell
//  loads unapproved handlers regardless.
//
//////////////////////////////////////////////////////////////////////////

static HRESULT SetApprovedEntry ( BOOL bRegister )
{
CRegKey reg;
LONG    lRet;

    if ( IsPerUserRegistration() )
        return S_FALSE;             // nothing to do, and not an error

    lRet = reg.Open ( HKEY_LOCAL_MACHINE, szApprovedKey, KEY_SET_VALUE );

    if ( ERROR_SUCCESS != lRet )
        return HRESULT_FROM_WIN32 ( lRet );

    if ( bRegister )
        {
        lRet = reg.SetStringValue ( szCLSID, szApprovedDesc );
        }
    else
        {
        lRet = reg.DeleteValue ( szCLSID );

        // Already gone is success, not failure.  DirClean's unregister
        // path ignored the return value entirely; being explicit is
        // better than being silent.
        if ( ERROR_FILE_NOT_FOUND == lRet )
            lRet = ERROR_SUCCESS;
        }

    return ( ERROR_SUCCESS == lRet ) ? S_OK : HRESULT_FROM_WIN32 ( lRet );
}


/////////////////////////////////////////////////////////////////////////////
// DllRegisterServer - Adds entries to the system registry

STDAPI DllRegisterServer(void)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    // Order matters, and it is the reverse of DirClean's.  ATL reads
    // the .rgs out of *this* module's resources and expands %MODULE% to
    // GetModuleFileName of this DLL, so the CLSID keys are the thing
    // that can genuinely fail (no rights, path >= MAX_PATH:
    // atlbase.h:6582-6587 returns ERROR_INSUFFICIENT_BUFFER for that).
    // Doing it first means a failure leaves nothing behind.
HRESULT hr = _Module.RegisterServer ( TRUE );

    if ( FAILED(hr) )
        return hr;

HRESULT hrApproved = SetApprovedEntry ( TRUE );

    if ( FAILED(hrApproved) )
        {
        // Roll back rather than leave a registered-but-unapproved
        // handler that would silently not load under the
        // EnforceShellExtensionSecurity policy.
        _Module.UnregisterServer ( TRUE );
        return hrApproved;
        }

    // Tells the shell its association data is stale.  It does NOT make
    // a running explorer.exe release this DLL -- Explorer must be
    // restarted before the file can be rebuilt or deleted.
    SHChangeNotify ( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );

    return S_OK;
}


/////////////////////////////////////////////////////////////////////////////
// DllUnregisterServer - Removes entries from the system registry

STDAPI DllUnregisterServer(void)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    // Unregistration is best-effort in the opposite order: drop the
    // Approved entry first so that, if the CLSID removal fails, what is
    // left behind is inert rather than approved.
    SetApprovedEntry ( FALSE );

HRESULT hr = _Module.UnregisterServer ( TRUE );

    SHChangeNotify ( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );

    return hr;
}


/////////////////////////////////////////////////////////////////////////////
// DllInstall - per-user (un)registration.
//
//   regsvr32 /n /i:user  MoveTo.dll     register   under HKCU
//   regsvr32 /u /n /i:user MoveTo.dll   unregister from HKCU
//
// /n suppresses DllRegisterServer so ONLY the per-user path runs; /i
// passes the command line through to here.  Without /n, regsvr32 calls
// DllRegisterServer as well and you get a machine registration attempt
// that fails (or worse, half-succeeds) alongside the per-user one.
//
// The signature is fixed by regsvr32: pszCmdLine is always WCHAR, in
// both ANSI and Unicode builds.
/////////////////////////////////////////////////////////////////////////////

STDAPI DllInstall ( BOOL bInstall, LPCWSTR pszCmdLine )
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if ( NULL != pszCmdLine )
        {
        if ( 0 == _wcsicmp ( pszCmdLine, L"user" ) )
            {
            HRESULT hr = AtlSetPerUserRegistration ( true );

            if ( FAILED(hr) )
                return hr;
            }
        else
            {
            // Refuse anything we do not understand rather than silently
            // performing a machine registration the caller did not ask
            // for.
            return E_INVALIDARG;
            }
        }

    return bInstall ? DllRegisterServer() : DllUnregisterServer();
}
