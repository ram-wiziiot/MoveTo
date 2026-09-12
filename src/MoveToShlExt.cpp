//////////////////////////////////////////////////////////////////////
//
// MoveToShlExt.cpp : the context menu handler.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MoveTo.h"
#include "MoveToShlExt.h"
#include "MoveEngine.h"
#include "ManageDestsDlg.h"
#include <afxole.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <strsafe.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

// Canonical verbs for the two FIXED commands only.
//
// Destinations deliberately get NO canonical verb.  "the third entry in
// my MRU today" is neither stable nor language independent, and
// publishing it would let any caller move files to whatever happens to
// be third this week.  GCS_VERB returns E_NOTIMPL for them; they are
// reachable by numeric offset alone, which is what the shell uses.
static LPCSTR const g_szVerbBrowse = "MoveToBrowse";
static LPCSTR const g_szVerbManage = "MoveToManage";


/////////////////////////////////////////////////////////////////////////////
// construction / destruction

CMoveToShlExt::CMoveToShlExt()
    : m_bTruncated(FALSE)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

#ifdef _DEBUG
    // The arithmetic that decides where the user's files go, asserted on
    // the first right-click rather than on someone's data.
    MenuLayoutSelfTest();
    MoveEngineSelfTest();
    DestSelfTest();
#endif

    // Read-only, no file system, no lock.  See MoveDest.h rule 1.
    DestVecLoad ( m_aDests );
}


CMoveToShlExt::~CMoveToShlExt()
{
    // Nothing is written here, deliberately.  Every right-click creates
    // an instance and two Explorer windows overlap constantly, so a
    // destructor that saved state would let one window's stale copy
    // clobber the other's edit.  DirClean shipped that bug for 25 years.
}


/////////////////////////////////////////////////////////////////////////////
// IShellExtInit

//////////////////////////////////////////////////////////////////////////
//
// Function:    Initialize()
//
// Description:
//  Reads the selection out of the data object.
//
//////////////////////////////////////////////////////////////////////////

HRESULT CMoveToShlExt::Initialize ( LPCITEMIDLIST pidlFolder,
                                    LPDATAOBJECT  pDO,
                                    HKEY          hProgID )
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    UNREFERENCED_PARAMETER(pidlFolder);
    UNREFERENCED_PARAMETER(hProgID);

    m_lsItems.RemoveAll();
    m_bTruncated = FALSE;

    if ( NULL == pDO )
        return E_INVALIDARG;

COleDataObject data;

    data.Attach ( pDO, FALSE );         // attach, do not auto-release

HGLOBAL hg = data.GetGlobalData ( CF_HDROP );

    if ( NULL == hg )
        return E_INVALIDARG;

HDROP hdrop = (HDROP) GlobalLock ( hg );

    if ( NULL == hdrop )
        {
        // GetGlobalData hands ownership of the HGLOBAL to us.  Failing
        // to free it leaks once per right-click for the life of the
        // Explorer session.
        GlobalFree ( hg );
        return E_INVALIDARG;
        }

UINT uNumFiles = DragQueryFile ( hdrop, 0xFFFFFFFF, NULL, 0 );
UINT uToRead   = uNumFiles;

    if ( uToRead > mxMaxSelection )
        {
        uToRead      = mxMaxSelection;
        m_bTruncated = TRUE;
        }

    // CString::GetBuffer throws on failure, and an MFC exception
    // escaping into the shell's call stack takes Explorer with it.
    try
        {
        for ( UINT uFile = 0; uFile < uToRead; uFile++ )
            {
            // Ask for the length first.  DragQueryFile truncates a long
            // path without reporting an error, and a truncated path is
            // very often a real existing ANCESTOR directory -- which is
            // then what gets moved.
            UINT uLen = DragQueryFile ( hdrop, uFile, NULL, 0 );

            if ( 0 == uLen )
                continue;

            CString sPath;
            LPTSTR  pszBuf = sPath.GetBuffer ( uLen + 1 );

            UINT uCopied = DragQueryFile ( hdrop, uFile, pszBuf, uLen + 1 );

            sPath.ReleaseBuffer();

            if ( 0 != uCopied && !sPath.IsEmpty() )
                m_lsItems.AddTail ( sPath );
            }
        }
    catch ( CException* pe )
        {
        pe->Delete();
        GlobalUnlock ( hg );
        GlobalFree ( hg );
        m_lsItems.RemoveAll();
        return E_OUTOFMEMORY;
        }

    GlobalUnlock ( hg );
    GlobalFree ( hg );

    return m_lsItems.IsEmpty() ? E_INVALIDARG : S_OK;
}


/////////////////////////////////////////////////////////////////////////////
// IContextMenu

//////////////////////////////////////////////////////////////////////////
//
// Function:    QueryContextMenu()
//
// Description:
//  Builds the "Move to" popup.
//
//  BUDGET: this runs on Explorer's UI thread for EVERY right-click on
//  EVERY file, alongside every other installed handler, and the shell
//  waits on all of them with no timeout.  Nothing here touches the disk,
//  the network or the registry -- the destination list was loaded in the
//  constructor and its labels were computed there.
//
//////////////////////////////////////////////////////////////////////////

HRESULT CMoveToShlExt::QueryContextMenu ( HMENU hmenu,       UINT uMenuIndex,
                                          UINT  uidFirstCmd, UINT uidLastCmd,
                                          UINT  uFlags )
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    // CMF_DEFAULTONLY: the shell only wants the default verb.
    // CMF_NOVERBS: it is probing, not drawing.  Either way, add nothing.
    if ( ( uFlags & CMF_DEFAULTONLY ) || ( uFlags & CMF_NOVERBS ) )
        return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, 0 );

    if ( m_lsItems.IsEmpty() )
        return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, 0 );

    // How many id slots the host granted, less one kept in reserve:
    // some hosts have historically passed idCmdLast one past the end,
    // and burning the last id there corrupts the next handler's items.
UINT nSlots = 0;

    if ( uidLastCmd >= uidFirstCmd )
        {
        nSlots = uidLastCmd - uidFirstCmd + 1;

        if ( nSlots > mlSpareIds )
            nSlots -= mlSpareIds;
        else
            nSlots = 0;
        }

    m_layout = MenuLayoutCompute ( (UINT) m_aDests.size(), nSlots );

    if ( m_layout.GetIdsUsed() > nSlots )
        {
        // Not enough room even for Browse and Manage. Add nothing at
        // all rather than a partial menu.
        return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, 0 );
        }

HMENU hSub = CreatePopupMenu();

    if ( NULL == hSub )
        return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, 0 );

MENUITEMINFO mii;
UINT         nPos = 0;

    for ( UINT i = 0; i < m_layout.GetDestCount(); i++ )
        {
        // GetDisplay() is already sanitised, elided and '&'-escaped by
        // the store.  The only thing added here is the digit mnemonic,
        // which depends on menu position and so cannot live there.
        // Escaping must happen exactly once or "R&D" renders "R&&D".
        CString sText;

        if ( i < 9 )
            sText.Format ( _T("&%u  %s"), i + 1, (LPCTSTR) m_aDests[i].GetDisplay() );
        else
            sText = m_aDests[i].GetDisplay();

        ZeroMemory ( &mii, sizeof(mii) );
        mii.cbSize     = sizeof(mii);
        mii.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE;
        mii.fState     = MFS_ENABLED;
        mii.wID        = uidFirstCmd + m_layout.OffsetOfDest ( i );
        mii.dwTypeData = (LPTSTR) (LPCTSTR) sText;

        InsertMenuItem ( hSub, nPos++, TRUE, &mii );
        }

    if ( m_layout.GetDestCount() > 0 )
        {
        // A separator consumes no command id.
        ZeroMemory ( &mii, sizeof(mii) );
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_FTYPE;
        mii.fType  = MFT_SEPARATOR;

        InsertMenuItem ( hSub, nPos++, TRUE, &mii );
        }

CString sBrowse ( _T("&Browse for folder...") );

    ZeroMemory ( &mii, sizeof(mii) );
    mii.cbSize     = sizeof(mii);
    mii.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE;
    mii.fState     = MFS_ENABLED;
    mii.wID        = uidFirstCmd + m_layout.OffsetOfBrowse();
    mii.dwTypeData = (LPTSTR) (LPCTSTR) sBrowse;

    InsertMenuItem ( hSub, nPos++, TRUE, &mii );

CString sManage ( _T("&Manage destinations...") );

    ZeroMemory ( &mii, sizeof(mii) );
    mii.cbSize     = sizeof(mii);
    mii.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE;
    mii.fState     = MFS_ENABLED;
    mii.wID        = uidFirstCmd + m_layout.OffsetOfManage();
    mii.dwTypeData = (LPTSTR) (LPCTSTR) sManage;

    InsertMenuItem ( hSub, nPos++, TRUE, &mii );

    //////////////////////////////////////////////////////////////////
    // The parent item.
    //////////////////////////////////////////////////////////////////

CString sParent;
INT_PTR nSel = m_lsItems.GetCount();

    if ( 1 == nSel )
        sParent = _T("Mo&ve to");
    else
        sParent.Format ( _T("Mo&ve %Id items to"), nSel );

    ZeroMemory ( &mii, sizeof(mii) );
    mii.cbSize     = sizeof(mii);
    mii.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_SUBMENU;
    mii.fState     = MFS_ENABLED;
    mii.wID        = uidFirstCmd + m_layout.OffsetOfParent();
    mii.hSubMenu   = hSub;
    mii.dwTypeData = (LPTSTR) (LPCTSTR) sParent;

    if ( !InsertMenuItem ( hmenu, uMenuIndex, TRUE, &mii ) )
        {
        // Only on THIS path do we own the popup.  Once InsertMenuItem
        // succeeds, the parent menu owns it and DestroyMenu(parent)
        // frees it recursively -- destroying it ourselves then would be
        // a double free inside Explorer.
        DestroyMenu ( hSub );
        return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, 0 );
        }

    // The return value is a CLAIM ON ID SLOTS, not a count of visible
    // items: the shell computes the next handler's base as
    // idCmdFirst + HRESULT_CODE(hr).  Returning 1 here ("I added one
    // top-level item") would hand our destination ids to the next
    // extension, and clicking "Move to > D:\Archive" would run whatever
    // it put there.
    return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, m_layout.GetIdsUsed() );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    GetCommandString()
//
// Description:
//  Flyby help, and canonical verbs for the two fixed commands.
//
//////////////////////////////////////////////////////////////////////////

HRESULT CMoveToShlExt::GetCommandString ( UINT_PTR uCmdID,     UINT  uFlags,
                                          UINT*    puReserved, LPSTR szName,
                                          UINT     cchMax )
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    UNREFERENCED_PARAMETER(puReserved);

MenuKind eKind = m_layout.Classify ( (UINT) uCmdID );

    if ( mkNone == eKind )
        return E_INVALIDARG;

CString sText;

    // GCS_HELPTEXTW / GCS_VERBW are the ANSI flags OR'd with
    // GCS_UNICODE, so mask that bit off before comparing.
    switch ( uFlags & ~GCS_UNICODE )
        {
        case GCS_HELPTEXTA:
            switch ( eKind )
                {
                case mkParent:
                    sText = _T("Move the selected items to a recent or pinned folder");
                break;

                case mkDest:
                    {
                    UINT i = m_layout.DestIndexOf ( (UINT) uCmdID );

                    if ( i < m_aDests.size() )
                        sText.Format ( _T("Move the selected items to %s"),
                                       (LPCTSTR) m_aDests[i].GetPath() );
                    else
                        return E_INVALIDARG;
                    }
                break;

                case mkBrowse:
                    sText = _T("Choose a folder to move the selected items to");
                break;

                case mkManage:
                    sText = _T("Pin, rename or forget folders on this menu");
                break;

                default:
                    return E_INVALIDARG;
                }
        break;

        case GCS_VERBA:
            // Fixed commands only -- see the note on g_szVerbBrowse.
            if ( mkBrowse == eKind )
                sText = CString ( g_szVerbBrowse );
            else if ( mkManage == eKind )
                sText = CString ( g_szVerbManage );
            else
                return E_NOTIMPL;
        break;

        default:
            return E_NOTIMPL;
        }

    if ( uFlags & GCS_UNICODE )
        return StringCchCopyW ( (LPWSTR) szName, cchMax, CT2CW ( (LPCTSTR) sText ) );
    else
        return StringCchCopyA ( szName, cchMax, CT2CA ( (LPCTSTR) sText ) );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    InvokeCommand()
//
// Description:
//  Carries out the chosen command.
//
//////////////////////////////////////////////////////////////////////////

HRESULT CMoveToShlExt::InvokeCommand ( LPCMINVOKECOMMANDINFO pInfo )
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if ( NULL == pInfo )
        return E_INVALIDARG;

// CMINVOKECOMMANDINFO::hwnd is legitimately NULL for a programmatic
// invocation.  Everything downstream tolerates NULL; what it must not do
// is dereference a CWnd built from it.
HWND hwndOwner = NULL;

    if ( NULL != pInfo->hwnd && ::IsWindow ( pInfo->hwnd ) )
        hwndOwner = pInfo->hwnd;

MenuKind eKind = mkNone;
UINT     uOffset = 0;

    if ( IS_INTRESOURCE ( pInfo->lpVerb ) )
        {
        uOffset = LOWORD ( pInfo->lpVerb );
        eKind   = m_layout.Classify ( uOffset );
        }
    else if ( 0 == lstrcmpA ( pInfo->lpVerb, g_szVerbBrowse ) )
        {
        eKind = mkBrowse;
        }
    else if ( 0 == lstrcmpA ( pInfo->lpVerb, g_szVerbManage ) )
        {
        eKind = mkManage;
        }
    else
        {
        return E_INVALIDARG;
        }

    try
        {
        switch ( eKind )
            {
            case mkDest:
                {
                UINT i = m_layout.DestIndexOf ( uOffset );

                if ( i >= m_aDests.size() )
                    return E_INVALIDARG;

                return DoMoveTo ( m_aDests[i].GetPath(), hwndOwner );
                }

            case mkBrowse:
                return DoBrowse ( hwndOwner );

            case mkManage:
                return DoManage ( hwndOwner );

            case mkParent:
                // The popup itself is not invocable.
                return E_INVALIDARG;

            default:
                return E_INVALIDARG;
            }
        }
    catch ( CException* pe )
        {
        pe->Delete();
        return E_FAIL;
        }
    catch ( ... )
        {
        return E_FAIL;
        }
}


/////////////////////////////////////////////////////////////////////////////
// the commands

//////////////////////////////////////////////////////////////////////////
//
// Function:    CheckNotTruncated()
//
// Description:
//  Refuses to act on a capped selection.
//
//  Moving the first 10 000 of a 200 000-item selection and reporting
//  nothing is the worst available outcome: it looks exactly like
//  success, and the user discovers the remainder days later.
//
//////////////////////////////////////////////////////////////////////////

BOOL CMoveToShlExt::CheckNotTruncated ( HWND hwndOwner )
{
    if ( !m_bTruncated )
        return TRUE;

CString sMsg;

    sMsg.Format ( _T("That selection is too large for MoveTo to handle safely ")
                  _T("(more than %u items).\n\nNothing has been moved. ")
                  _T("Move the containing folder instead, or select fewer items."),
                  (UINT) mxMaxSelection );

    MessageBox ( hwndOwner, sMsg, _T("MoveTo"), MB_ICONEXCLAMATION | MB_OK );

    return FALSE;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DoMoveTo()
//
// Description:
//  Validates, moves, reports, and only then updates the history.
//
//////////////////////////////////////////////////////////////////////////

HRESULT CMoveToShlExt::DoMoveTo ( LPCTSTR szDest, HWND hwndOwner )
{
    if ( !CheckNotTruncated ( hwndOwner ) )
        return S_OK;

CMovePlan plan;

    {
    // The preflight opens one handle per selected item. Bounded by the
    // selection cap, but still worth a cursor on a large selection.
    CWaitCursor wait;

    MovePlanBuild ( m_lsItems, szDest, plan );
    }

    //////////////////////////////////////////////////////////////////
    // A destination we cannot use.
    //////////////////////////////////////////////////////////////////

    if ( dvOk != plan.m_eDestValidity )
        {
        MessageBox ( hwndOwner, MoveDestValidityText ( plan.m_eDestValidity, szDest ),
                     _T("MoveTo"), MB_ICONEXCLAMATION | MB_OK );

        // A folder that is genuinely gone earns a strike; one that is
        // merely unreachable earns nothing.  Otherwise the first
        // morning off the VPN quietly deletes every network
        // destination the user pinned.
        if ( dvMissing == plan.m_eDestValidity )
            DestNoteMiss ( szDest );

        return S_OK;
        }

    //////////////////////////////////////////////////////////////////
    // Nothing movable: explain why rather than failing silently.
    //////////////////////////////////////////////////////////////////

    if ( 0 == plan.m_nMovable )
        {
        CString sMsg ( _T("Nothing was moved.\n") );
        int     nShown = 0;

        for ( size_t i = 0; i < plan.m_aItems.size() && nShown < 8; i++ )
            {
            if ( mrOk == plan.m_aItems[i].m_eReject )
                continue;

            CString sLine;

            sLine.Format ( _T("\n    %s\n        %s"),
                           (LPCTSTR) plan.m_aItems[i].m_sPath,
                           (LPCTSTR) MoveRejectText ( plan.m_aItems[i].m_eReject ) );
            sMsg += sLine;
            nShown++;
            }

        MessageBox ( hwndOwner, sMsg, _T("MoveTo"), MB_ICONINFORMATION | MB_OK );
        return S_OK;
        }

    //////////////////////////////////////////////////////////////////
    // Some items are being skipped: say so BEFORE moving the rest.
    //////////////////////////////////////////////////////////////////

UINT nSkipped = (UINT) plan.m_aItems.size() - plan.m_nMovable;

    if ( nSkipped > 0 )
        {
        CString sMsg;
        int     nShown = 0;

        sMsg.Format ( _T("%u of %Iu items cannot be moved:\n"),
                      nSkipped, plan.m_aItems.size() );

        for ( size_t i = 0; i < plan.m_aItems.size() && nShown < 8; i++ )
            {
            if ( mrOk == plan.m_aItems[i].m_eReject )
                continue;

            CString sLine;

            sLine.Format ( _T("\n    %s\n        %s"),
                           (LPCTSTR) plan.m_aItems[i].m_sPath,
                           (LPCTSTR) MoveRejectText ( plan.m_aItems[i].m_eReject ) );
            sMsg += sLine;
            nShown++;
            }

        if ( nSkipped > 8 )
            sMsg += _T("\n    ...");

        CString sTail;

        sTail.Format ( _T("\n\nMove the remaining %u?"), plan.m_nMovable );
        sMsg += sTail;

        if ( IDYES != MessageBox ( hwndOwner, sMsg, _T("MoveTo"),
                                   MB_ICONWARNING | MB_YESNO ) )
            {
            return S_OK;
            }
        }

    //////////////////////////////////////////////////////////////////
    // Move.
    //////////////////////////////////////////////////////////////////

CMoveOutcome outcome;

    MovePlanExecute ( plan, hwndOwner, outcome );

    //////////////////////////////////////////////////////////////////
    // Only a confirmed per-item success promotes the destination.
    //
    // PerformOperations returns S_OK when the whole batch was refused,
    // so promoting on its HRESULT would push a folder that never works
    // to the top of the list on every failure, permanently.
    //////////////////////////////////////////////////////////////////

    if ( outcome.m_nMoved > 0 )
        {
        DestPromote ( szDest );

        // Housekeeping, now that Explorer is no longer waiting on us to
        // draw a menu.  Never on the menu path.
        DestSweep ( 30, FALSE );
        }

    return S_OK;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DoBrowse()
//
// Description:
//  Picks a folder, moves there, and seeds the history through the same
//  promotion path as any other move.
//
//////////////////////////////////////////////////////////////////////////

HRESULT CMoveToShlExt::DoBrowse ( HWND hwndOwner )
{
    if ( !CheckNotTruncated ( hwndOwner ) )
        return S_OK;

CComPtr<IFileDialog> spDlg;

HRESULT hr = spDlg.CoCreateInstance ( CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER );

    if ( FAILED ( hr ) )
        return hr;

DWORD dwOptions = 0;

    spDlg->GetOptions ( &dwOptions );

    // FOS_FORCEFILESYSTEM keeps virtual folders (This PC, Libraries) out
    // of the result, so what comes back is always a real path we can
    // hand to the move engine and store.
    spDlg->SetOptions ( dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                        FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR );

    spDlg->SetTitle ( L"Move to folder" );
    spDlg->SetOkButtonLabel ( L"Move here" );

    hr = spDlg->Show ( hwndOwner );

    if ( FAILED ( hr ) )
        return S_OK;                    // cancelled; not an error

CComPtr<IShellItem> spItem;

    hr = spDlg->GetResult ( &spItem );

    if ( FAILED ( hr ) )
        return S_OK;

LPWSTR pszPath = NULL;

    hr = spItem->GetDisplayName ( SIGDN_FILESYSPATH, &pszPath );

    if ( FAILED ( hr ) || NULL == pszPath )
        return S_OK;

CString sDest ( pszPath );

    CoTaskMemFree ( pszPath );

    return DoMoveTo ( sDest, hwndOwner );
}


HRESULT CMoveToShlExt::DoManage ( HWND hwndOwner )
{
CWnd* pParent = ( NULL != hwndOwner ) ? CWnd::FromHandle ( hwndOwner ) : NULL;

CManageDestsDlg dlg ( pParent );

    dlg.DoModal();

    return S_OK;
}
