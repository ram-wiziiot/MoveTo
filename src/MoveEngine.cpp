//////////////////////////////////////////////////////////////////////
//
// MoveEngine.cpp : preflight validation and the move itself.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MoveEngine.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <algorithm>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif


//////////////////////////////////////////////////////////////////////
// The flag word.
//
// ONE flag word, ONE call site, and it is a DWORD.  IFileOperation's
// SetOperationFlags takes DWORD; the older SHFILEOPSTRUCT field is
// FILEOP_FLAGS, which is a WORD -- so routing these through the old API
// would silently truncate FOFX_ADDUNDORECORD (0x20000000) to nothing,
// with at most a conversion warning.
//
// Deliberately NOT set, each for a reason:
//
//  FOF_NOCONFIRMATION      would answer "yes" to every prompt including
//                          the replace-existing-file question.  Moving
//                          is not deleting, but silently overwriting a
//                          file at the destination loses data just the
//                          same.  Let the shell ask.
//  FOF_RENAMEONCOLLISION   would silently produce "report (2).docx".
//                          The user should decide.
//  FOF_MULTIDESTFILES      means pFrom[i] -> pTo[i], i.e. a parallel
//                          rename.  We have one destination folder and
//                          many sources, which is the default behaviour
//                          without it.
//  FOF_SILENT / NOERRORUI  the progress and error UI is the only
//                          feedback this feature has.
//  FOFX_MOVEACLSACROSSVOLUMES  a cross-volume move is a copy+delete, and
//                          carrying the source ACLs into a folder with
//                          different inheritance is usually not what the
//                          user wants and is hard to undo.
//////////////////////////////////////////////////////////////////////

static const DWORD g_dwMoveFlags = FOF_NOCONFIRMMKDIR
                                 | FOF_ALLOWUNDO
                                 | FOFX_ADDUNDORECORD;


//////////////////////////////////////////////////////////////////////
// Canonical identity
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//
// Function:    PathIdGet()
//
// Description:
//  Produces a comparison-only identity for a path, by opening a handle
//  with no access rights and asking the file system what the path really
//  is.
//
//  String comparison alone is defeated by "..", 8.3 short names,
//  junctions, SUBST drives and mapped network drives -- and the shell
//  resolves every one of those before it acts.  So a purely lexical
//  "is the destination inside the source" test fails OPEN on exactly the
//  inputs that matter, which is the wrong direction for a check whose
//  job is preventing data loss.
//
//  VOLUME_NAME_GUID is used rather than VOLUME_NAME_DOS so that a mapped
//  drive and its UNC target, or a SUBST drive and its target, compare
//  equal.  The result starts "\\?\Volume{...}" and MUST NOT be handed to
//  the shell or stored -- see the header.
//
//////////////////////////////////////////////////////////////////////////

static BOOL PathIdGet ( LPCTSTR szPath, CString& sOut )
{
    sOut.Empty();

    if ( NULL == szPath || _T('\0') == *szPath )
        return FALSE;

HANDLE hFile = CreateFile ( szPath,
                            0,                      // no access: works on
                                                    // in-use and read-only items
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL,
                            OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS,   // required for directories
                            NULL );

    if ( INVALID_HANDLE_VALUE == hFile )
        return FALSE;

DWORD dwLen = GetFinalPathNameByHandle ( hFile, NULL, 0,
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_GUID );

    if ( 0 == dwLen )
        {
        CloseHandle ( hFile );
        return FALSE;
        }

LPTSTR pszBuf = sOut.GetBuffer ( dwLen + 1 );

    if ( NULL == pszBuf )
        {
        CloseHandle ( hFile );
        return FALSE;
        }

DWORD dwGot = GetFinalPathNameByHandle ( hFile, pszBuf, dwLen + 1,
                                         FILE_NAME_NORMALIZED | VOLUME_NAME_GUID );

    sOut.ReleaseBuffer();
    CloseHandle ( hFile );

    if ( 0 == dwGot || dwGot > dwLen + 1 )
        {
        sOut.Empty();
        return FALSE;
        }

    sOut.TrimRight ( _T('\\') );

    return !sOut.IsEmpty();
}


BOOL MovePathIsUnderOrEqual ( LPCTSTR szChild, LPCTSTR szParent )
{
    if ( NULL == szChild || NULL == szParent || _T('\0') == *szParent )
        return FALSE;

int nParent = lstrlen ( szParent );
int nChild  = lstrlen ( szChild );

    // CompareStringOrdinal honours an explicit length literally, so a
    // child shorter than the parent would read past the end of its
    // buffer.  Guard first.
    if ( nChild < nParent )
        return FALSE;

    if ( CSTR_EQUAL != CompareStringOrdinal ( szChild, nParent,
                                              szParent, nParent, TRUE ) )
        {
        return FALSE;
        }

    // Equal, or the next character starts a fresh component.  Without
    // this, "C:\foobar" would look like it sits under "C:\foo".
    return ( nChild == nParent || _T('\\') == szChild[nParent] );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    PathIsRootish()
//
// Description:
//  A drive root or a UNC share root cannot be moved, and asking the
//  shell to try produces a confusing error rather than a clear refusal.
//
//////////////////////////////////////////////////////////////////////////

static BOOL PathIsRootish ( LPCTSTR szPath )
{
CString s ( szPath );

    if ( PathIsRoot ( s ) )
        return TRUE;

    s.TrimRight ( _T('\\') );

    if ( 2 == s.GetLength() && _T(':') == s[1] )
        return TRUE;

    if ( PathIsUNCServerShare ( s ) || PathIsUNCServer ( s ) )
        return TRUE;

    return FALSE;
}


//////////////////////////////////////////////////////////////////////
// Building the plan
//////////////////////////////////////////////////////////////////////

// Above this many items, the pairwise nested-selection scan is replaced
// by a sort plus one linear pass.  The pairwise version is O(n^2)
// CompareStringOrdinal calls, which at ten thousand items is minutes of
// unresponsive Explorer.
static const size_t g_nPairwiseLimit = 500;


//////////////////////////////////////////////////////////////////////////
//
// Function:    PruneNestedSelection()
//
// Description:
//  Drops any item whose ancestor is also selected.
//
//  Selecting a folder and something inside it and moving both is a
//  partial-failure generator: the shell moves the parent first, and the
//  child's recorded path is then stale, so its own move fails after the
//  batch has already half-run.  Moving the parent moves the child
//  anyway, so dropping the child changes nothing about the result.
//
//////////////////////////////////////////////////////////////////////////

static void PruneNestedSelection ( CMoveItemVec& aItems )
{
    if ( aItems.size() < 2 )
        return;

    if ( aItems.size() <= g_nPairwiseLimit )
        {
        for ( size_t i = 0; i < aItems.size(); i++ )
            {
            if ( mrOk != aItems[i].m_eReject || !aItems[i].m_bIsDirectory )
                continue;

            for ( size_t j = 0; j < aItems.size(); j++ )
                {
                if ( i == j || mrOk != aItems[j].m_eReject )
                    continue;

                if ( aItems[j].m_sCanonical == aItems[i].m_sCanonical )
                    continue;

                if ( MovePathIsUnderOrEqual ( aItems[j].m_sCanonical,
                                              aItems[i].m_sCanonical ) )
                    {
                    aItems[j].m_eReject = mrNestedInSelection;
                    }
                }
            }

        return;
        }

    // Large selection: sort canonical paths, then one linear pass. An
    // ancestor always sorts immediately before its descendants, so a
    // single running "current ancestor" is enough.
std::vector<size_t> aIdx;

    aIdx.reserve ( aItems.size() );

    for ( size_t i = 0; i < aItems.size(); i++ )
        {
        if ( mrOk == aItems[i].m_eReject )
            aIdx.push_back ( i );
        }

const CMoveItemVec* pItems = &aItems;

    std::sort ( aIdx.begin(), aIdx.end(),
        [pItems] ( size_t a, size_t b ) -> bool
        {
        return ( CSTR_LESS_THAN ==
                 CompareStringOrdinal ( (*pItems)[a].m_sCanonical, -1,
                                        (*pItems)[b].m_sCanonical, -1, TRUE ) );
        } );

CString sAncestor;

    for ( size_t k = 0; k < aIdx.size(); k++ )
        {
        CMoveItem& item = aItems[ aIdx[k] ];

        if ( !sAncestor.IsEmpty() &&
             MovePathIsUnderOrEqual ( item.m_sCanonical, sAncestor ) )
            {
            item.m_eReject = mrNestedInSelection;
            continue;
            }

        if ( item.m_bIsDirectory )
            sAncestor = item.m_sCanonical;
        }
}


void MovePlanBuild ( const CStringList& lsSelection, LPCTSTR szDest,
                     CMovePlan& planOut )
{
    planOut = CMovePlan();

CString sDest ( szDest );

    sDest.TrimRight ( _T('\\') );

    // A drive root keeps its separator, or the shell reads "C:" as the
    // process's current directory on C:.
    if ( 2 == sDest.GetLength() && _T(':') == sDest[1] )
        sDest += _T('\\');

    planOut.m_sDest = sDest;

    if ( sDest.IsEmpty() )
        {
        planOut.m_eDestValidity = dvMissing;
        return;
        }

    if ( sDest.GetLength() >= MAX_PATH )
        {
        planOut.m_eDestValidity = dvTooLong;
        return;
        }

    //////////////////////////////////////////////////////////////////
    // The destination, before anything else.
    //////////////////////////////////////////////////////////////////

WIN32_FILE_ATTRIBUTE_DATA fad;

    if ( !GetFileAttributesEx ( sDest, GetFileExInfoStandard, &fad ) )
        {
        DWORD dwErr = GetLastError();

        // "Not there" and "cannot get there" are different answers, and
        // the caller treats them differently: one may eventually retire
        // the entry, the other must never touch it.
        if ( ERROR_FILE_NOT_FOUND == dwErr || ERROR_PATH_NOT_FOUND == dwErr ||
             ERROR_INVALID_NAME == dwErr || ERROR_BAD_NETPATH == dwErr )
            {
            planOut.m_eDestValidity = dvMissing;
            }
        else
            {
            planOut.m_eDestValidity = dvUnreachable;
            }

        return;
        }

    if ( 0 == ( fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) )
        {
        planOut.m_eDestValidity = dvNotAFolder;
        return;
        }

    if ( !PathIdGet ( sDest, planOut.m_sDestCanonical ) )
        {
        planOut.m_eDestValidity = dvUnreachable;
        return;
        }

    planOut.m_eDestValidity = dvOk;

    //////////////////////////////////////////////////////////////////
    // Each source.
    //////////////////////////////////////////////////////////////////

POSITION pos = lsSelection.GetHeadPosition();

    while ( NULL != pos )
        {
        CMoveItem item;

        item.m_sPath = lsSelection.GetNext ( pos );
        item.m_sPath.TrimRight ( _T('\\') );

        if ( item.m_sPath.IsEmpty() )
            continue;

        WIN32_FILE_ATTRIBUTE_DATA fadSrc;

        if ( !GetFileAttributesEx ( item.m_sPath, GetFileExInfoStandard, &fadSrc ) )
            {
            item.m_eReject = mrMissing;
            planOut.m_aItems.push_back ( item );
            continue;
            }

        item.m_bIsDirectory =
            ( 0 != ( fadSrc.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) );

        if ( PathIsRootish ( item.m_sPath ) )
            {
            item.m_eReject = mrIsRoot;
            planOut.m_aItems.push_back ( item );
            continue;
            }

        if ( !PathIdGet ( item.m_sPath, item.m_sCanonical ) )
            {
            item.m_eReject = mrUnresolvable;
            planOut.m_aItems.push_back ( item );
            continue;
            }

        // The item IS the destination.
        if ( item.m_sCanonical == planOut.m_sDestCanonical )
            {
            item.m_eReject = mrSameItem;
            planOut.m_aItems.push_back ( item );
            continue;
            }

        // Moving a folder into itself or into its own subtree. This is
        // the data-loss case, and it is why identity is resolved through
        // a handle rather than compared as strings.
        if ( item.m_bIsDirectory &&
             MovePathIsUnderOrEqual ( planOut.m_sDestCanonical, item.m_sCanonical ) )
            {
            item.m_eReject = mrDestInsideSource;
            planOut.m_aItems.push_back ( item );
            continue;
            }

        // Already in the destination folder: a no-op the user did not
        // intend, and the shell would offer to overwrite the file with
        // itself.
        CString sParent = item.m_sCanonical;
        int     nSlash  = sParent.ReverseFind ( _T('\\') );

        if ( -1 != nSlash )
            {
            sParent = sParent.Left ( nSlash );

            if ( sParent == planOut.m_sDestCanonical )
                {
                item.m_eReject = mrAlreadyThere;
                planOut.m_aItems.push_back ( item );
                continue;
                }
            }

        planOut.m_aItems.push_back ( item );
        }

    PruneNestedSelection ( planOut.m_aItems );

    for ( size_t i = 0; i < planOut.m_aItems.size(); i++ )
        {
        if ( mrOk == planOut.m_aItems[i].m_eReject )
            planOut.m_nMovable++;
        }
}


//////////////////////////////////////////////////////////////////////
// The progress sink
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//
// Class:       CMoveSink
//
// Description:
//  Counts what actually moved.
//
//  PerformOperations returns S_OK when one item failed, and S_OK when
//  the whole operation was structurally refused and nothing moved at
//  all.  GetAnyOperationsAborted is TRUE for a plain user cancel as well
//  as for a failure, so it distinguishes neither.  The only trustworthy
//  signal is per-item: a PostMoveItem callback reporting success AND a
//  non-NULL psiNewlyCreated.
//
//  This matters beyond reporting.  It is what decides whether the
//  destination is promoted in the history -- without it, a folder that
//  refuses every move climbs to the top of the list on each failure and
//  stays there.
//
//////////////////////////////////////////////////////////////////////////

class CMoveSink : public IFileOperationProgressSink
{
public:
    CMoveSink() : m_cRef(1), m_nMoved(0), m_nFailed(0) {}
    virtual ~CMoveSink() {}

    UINT GetMoved() const  { return m_nMoved; }
    UINT GetFailed() const { return m_nFailed; }

    // IUnknown
    IFACEMETHODIMP QueryInterface ( REFIID riid, void** ppv )
    {
        if ( NULL == ppv )
            return E_POINTER;

        if ( IsEqualIID ( riid, IID_IUnknown ) ||
             IsEqualIID ( riid, __uuidof(IFileOperationProgressSink) ) )
            {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
            }

        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return (ULONG) InterlockedIncrement ( &m_cRef );
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        LONG c = InterlockedDecrement ( &m_cRef );

        if ( 0 == c )
            delete this;

        return (ULONG) c;
    }

    // The one callback we care about.
    IFACEMETHODIMP PostMoveItem ( DWORD, IShellItem*, IShellItem*, LPCWSTR,
                                  HRESULT hrMove, IShellItem* psiNewlyCreated )
    {
        // COPYENGINE_S_* codes are SUCCESS codes in which nothing was
        // moved -- the user chose to skip, or the item was already
        // there.  psiNewlyCreated is NULL in those cases, which is what
        // separates them from a real move.
        if ( SUCCEEDED ( hrMove ) && NULL != psiNewlyCreated )
            m_nMoved++;
        else if ( FAILED ( hrMove ) )
            m_nFailed++;

        return S_OK;
    }

    // Everything else is a no-op. Returning S_OK from PreMoveItem is
    // what lets the operation proceed; returning E_ABORT there would
    // stop the whole batch.
    IFACEMETHODIMP StartOperations()                               { return S_OK; }
    IFACEMETHODIMP FinishOperations ( HRESULT )                    { return S_OK; }
    IFACEMETHODIMP PreRenameItem ( DWORD, IShellItem*, LPCWSTR )   { return S_OK; }
    IFACEMETHODIMP PostRenameItem ( DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem* )
                                                                   { return S_OK; }
    IFACEMETHODIMP PreMoveItem ( DWORD, IShellItem*, IShellItem*, LPCWSTR )
                                                                   { return S_OK; }
    IFACEMETHODIMP PreCopyItem ( DWORD, IShellItem*, IShellItem*, LPCWSTR )
                                                                   { return S_OK; }
    IFACEMETHODIMP PostCopyItem ( DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem* )
                                                                   { return S_OK; }
    IFACEMETHODIMP PreDeleteItem ( DWORD, IShellItem* )            { return S_OK; }
    IFACEMETHODIMP PostDeleteItem ( DWORD, IShellItem*, HRESULT, IShellItem* )
                                                                   { return S_OK; }
    IFACEMETHODIMP PreNewItem ( DWORD, IShellItem*, LPCWSTR )      { return S_OK; }
    IFACEMETHODIMP PostNewItem ( DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem* )
                                                                   { return S_OK; }
    IFACEMETHODIMP UpdateProgress ( UINT, UINT )                   { return S_OK; }
    IFACEMETHODIMP ResetTimer()                                    { return S_OK; }
    IFACEMETHODIMP PauseTimer()                                    { return S_OK; }
    IFACEMETHODIMP ResumeTimer()                                   { return S_OK; }

protected:
    LONG m_cRef;
    UINT m_nMoved;
    UINT m_nFailed;
};


HRESULT MovePlanExecute ( const CMovePlan& plan, HWND hwndOwner,
                          CMoveOutcome& outcomeOut )
{
    outcomeOut = CMoveOutcome();

    if ( !plan.IsRunnable() )
        return E_INVALIDARG;

CComPtr<IFileOperation> spOp;

HRESULT hr = spOp.CoCreateInstance ( CLSID_FileOperation, NULL, CLSCTX_ALL );

    if ( FAILED ( hr ) )
        {
        outcomeOut.m_hr = hr;
        return hr;
        }

    hr = spOp->SetOperationFlags ( g_dwMoveFlags );

    if ( FAILED ( hr ) )
        {
        outcomeOut.m_hr = hr;
        return hr;
        }

    if ( NULL != hwndOwner && ::IsWindow ( hwndOwner ) )
        spOp->SetOwnerWindow ( hwndOwner );

CComPtr<IShellItem> spDest;

    // The destination binds BEFORE anything is touched. This is the
    // structural difference from SHFileOperation, which would have
    // renamed a lone source onto a missing destination.
    hr = SHCreateItemFromParsingName ( CT2CW ( (LPCTSTR) plan.m_sDest ), NULL,
                                       IID_PPV_ARGS ( &spDest ) );

    if ( FAILED ( hr ) )
        {
        outcomeOut.m_hr = hr;
        return hr;
        }

CMoveSink* pSink = new CMoveSink();
DWORD      dwCookie = 0;

    hr = spOp->Advise ( pSink, &dwCookie );

    if ( FAILED ( hr ) )
        {
        pSink->Release();
        outcomeOut.m_hr = hr;
        return hr;
        }

UINT nQueued = 0;

    for ( size_t i = 0; i < plan.m_aItems.size(); i++ )
        {
        if ( mrOk != plan.m_aItems[i].m_eReject )
            continue;

        CComPtr<IShellItem> spItem;

        if ( FAILED ( SHCreateItemFromParsingName (
                          CT2CW ( (LPCTSTR) plan.m_aItems[i].m_sPath ), NULL,
                          IID_PPV_ARGS ( &spItem ) ) ) )
            {
            outcomeOut.m_nFailed++;
            continue;
            }

        if ( SUCCEEDED ( spOp->MoveItem ( spItem, spDest, NULL, NULL ) ) )
            nQueued++;
        else
            outcomeOut.m_nFailed++;
        }

    if ( 0 == nQueued )
        {
        spOp->Unadvise ( dwCookie );
        pSink->Release();
        outcomeOut.m_hr = E_FAIL;
        return E_FAIL;
        }

    hr = spOp->PerformOperations();

BOOL bAborted = FALSE;

    spOp->GetAnyOperationsAborted ( &bAborted );

    outcomeOut.m_hr       = hr;
    outcomeOut.m_bAborted = bAborted;
    outcomeOut.m_nMoved   = pSink->GetMoved();
    outcomeOut.m_nFailed += pSink->GetFailed();

    spOp->Unadvise ( dwCookie );
    pSink->Release();

    return hr;
}


//////////////////////////////////////////////////////////////////////
// Explanations
//////////////////////////////////////////////////////////////////////

CString MoveRejectText ( MoveReject eReject )
{
    switch ( eReject )
        {
        case mrMissing:           return _T("no longer exists");
        case mrIsRoot:            return _T("is a drive or share root and cannot be moved");
        case mrDestInsideSource:  return _T("cannot be moved into a folder inside itself");
        case mrAlreadyThere:      return _T("is already in that folder");
        case mrSameItem:          return _T("is the destination folder");
        case mrNestedInSelection: return _T("is inside another selected folder, so it moves with it");
        case mrUnresolvable:      return _T("could not be opened");
        default:                  return _T("");
        }
}


CString MoveDestValidityText ( DestValidity eValidity, LPCTSTR szDest )
{
CString s;

    switch ( eValidity )
        {
        case dvMissing:
            s.Format ( _T("The folder\n\n    %s\n\nis no longer there. Nothing was moved."),
                       szDest );
        break;

        case dvNotAFolder:
            s.Format ( _T("\n\n    %s\n\nis a file, not a folder. Nothing was moved."),
                       szDest );
        break;

        case dvUnreachable:
            s.Format ( _T("The folder\n\n    %s\n\ncould not be reached. It may be on a drive or network share that is not available right now.\n\nNothing was moved, and the folder has been left in your list."),
                       szDest );
        break;

        case dvTooLong:
            s.Format ( _T("That destination path is too long to use.\n\n    %s"), szDest );
        break;

        default:
        break;
        }

    return s;
}


#ifdef _DEBUG

void MoveEngineSelfTest()
{
    // The component-boundary rule. Without it a sibling folder whose
    // name merely starts with the same characters reads as a descendant,
    // and mrDestInsideSource would fire on a perfectly legal move.
    ASSERT (  MovePathIsUnderOrEqual ( _T("C:\\foo"),       _T("C:\\foo") ) );
    ASSERT (  MovePathIsUnderOrEqual ( _T("C:\\foo\\bar"),  _T("C:\\foo") ) );
    ASSERT ( !MovePathIsUnderOrEqual ( _T("C:\\foobar"),    _T("C:\\foo") ) );
    ASSERT ( !MovePathIsUnderOrEqual ( _T("C:\\fo"),        _T("C:\\foo") ) );
    ASSERT ( !MovePathIsUnderOrEqual ( _T("C:\\bar"),       _T("C:\\foo") ) );

    // Case-insensitive, ordinal.
    ASSERT (  MovePathIsUnderOrEqual ( _T("C:\\FOO\\bar"),  _T("c:\\foo") ) );

    ASSERT ( !MovePathIsUnderOrEqual ( NULL,                _T("C:\\foo") ) );
    ASSERT ( !MovePathIsUnderOrEqual ( _T("C:\\foo"),       _T("") ) );

    ASSERT (  PathIsRootish ( _T("C:\\") ) );
    ASSERT (  PathIsRootish ( _T("C:") ) );
    ASSERT ( !PathIsRootish ( _T("C:\\Windows") ) );

    // Nested-selection pruning, both code paths, same answer.
    for ( int nPass = 0; nPass < 2; nPass++ )
        {
        CMoveItemVec a;

        CMoveItem parent;
        parent.m_sPath      = _T("C:\\proj");
        parent.m_sCanonical = _T("C:\\proj");
        parent.m_bIsDirectory = TRUE;
        a.push_back ( parent );

        CMoveItem child;
        child.m_sPath      = _T("C:\\proj\\src");
        child.m_sCanonical = _T("C:\\proj\\src");
        child.m_bIsDirectory = TRUE;
        a.push_back ( child );

        CMoveItem other;
        other.m_sPath      = _T("C:\\elsewhere");
        other.m_sCanonical = _T("C:\\elsewhere");
        other.m_bIsDirectory = TRUE;
        a.push_back ( other );

        if ( 1 == nPass )
            {
            // Push past the pairwise limit so the sort-based path runs,
            // with filler that cannot be anyone's ancestor.
            for ( size_t i = 0; i < g_nPairwiseLimit + 10; i++ )
                {
                CMoveItem filler;
                CString   s;

                s.Format ( _T("C:\\zfill\\f%Iu"), i );
                filler.m_sPath        = s;
                filler.m_sCanonical   = s;
                filler.m_bIsDirectory = FALSE;
                a.push_back ( filler );
                }
            }

        PruneNestedSelection ( a );

        ASSERT ( mrOk                == a[0].m_eReject );   // the parent stays
        ASSERT ( mrNestedInSelection == a[1].m_eReject );   // the child goes
        ASSERT ( mrOk                == a[2].m_eReject );   // the sibling stays
        }
}

#endif // _DEBUG
