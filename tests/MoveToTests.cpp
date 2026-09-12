//////////////////////////////////////////////////////////////////////
//
// MoveToTests.cpp : console harness for the parts of MoveTo that decide
//                   where the user's files end up.
//
// The preflight is the whole safety story for this extension -- there is
// no preview dialog standing between the user and the operation, by
// design.  So it is exercised against a real directory tree in %TEMP%
// rather than reasoned about.
//
//      bin\x64\Debug\MoveToTests.exe
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MenuLayout.h"
#include "MoveEngine.h"
#include "MoveDest.h"
#include <atlbase.h>
#include <shlobj.h>

// stdafx.h declares this for the DLL; the harness supplies a definition.
CComModule _Module;

static int g_nPass = 0;
static int g_nFail = 0;

static void Check ( bool bCond, LPCTSTR szWhat )
{
    if ( bCond )
        {
        g_nPass++;
        _tprintf ( _T("  ok    %s\n"), szWhat );
        }
    else
        {
        g_nFail++;
        _tprintf ( _T("  FAIL  %s\n"), szWhat );
        }
}

static void Section ( LPCTSTR szName )
{
    _tprintf ( _T("\n%s\n"), szName );
}


//////////////////////////////////////////////////////////////////////
// Scaffolding
//////////////////////////////////////////////////////////////////////

static CString g_sRoot;

static CString P ( LPCTSTR szRelative )
{
    return g_sRoot + szRelative;
}

static void MakeDir ( LPCTSTR szRelative )
{
CString s = P ( szRelative );

    SHCreateDirectoryEx ( NULL, s, NULL );
}

static void MakeFile ( LPCTSTR szRelative )
{
CString s = P ( szRelative );

    // Create the parent first: CreateFile does not, and a silently
    // missing test file turns into a failing assertion about the
    // product rather than about the harness.
    {
    int nSlash = s.ReverseFind ( _T('\\') );

    if ( -1 != nSlash )
        SHCreateDirectoryEx ( NULL, s.Left ( nSlash ), NULL );
    }

HANDLE  h = CreateFile ( s, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL );

    if ( INVALID_HANDLE_VALUE == h )
        {
        _tprintf ( _T("  !! could not create %s (%lu)\n"), (LPCTSTR) s, GetLastError() );
        return;
        }

DWORD dwWritten = 0;

    WriteFile ( h, "x", 1, &dwWritten, NULL );
    CloseHandle ( h );
}

static CStringList* MakeList ( LPCTSTR a, LPCTSTR b = NULL, LPCTSTR c = NULL )
{
static CStringList ls;

    ls.RemoveAll();
    ls.AddTail ( P ( a ) );

    if ( NULL != b ) ls.AddTail ( P ( b ) );
    if ( NULL != c ) ls.AddTail ( P ( c ) );

    return &ls;
}

// The reject reason recorded for one source path in a plan.
static MoveReject RejectFor ( const CMovePlan& plan, LPCTSTR szRelative )
{
CString sWant = P ( szRelative );

    for ( size_t i = 0; i < plan.m_aItems.size(); i++ )
        {
        if ( 0 == plan.m_aItems[i].m_sPath.CompareNoCase ( sWant ) )
            return plan.m_aItems[i].m_eReject;
        }

    return mrUnresolvable;
}


//////////////////////////////////////////////////////////////////////
// Tests
//////////////////////////////////////////////////////////////////////

static void TestPureLogic()
{
    Section ( _T("Pure logic self-tests (assert on failure)") );

#ifdef _DEBUG
    MenuLayoutSelfTest();
    Check ( true, _T("MenuLayoutSelfTest passed") );

    MoveEngineSelfTest();
    Check ( true, _T("MoveEngineSelfTest passed") );

    DestSelfTest();
    Check ( true, _T("DestSelfTest passed") );
#else
    _tprintf ( _T("  (skipped: Release build)\n") );
#endif
}


//////////////////////////////////////////////////////////////////////////
//
// The id arithmetic, stated as the property that actually matters:
// whatever QueryContextMenu claims, InvokeCommand must agree about --
// and nothing outside the claim may look like ours.
//
//////////////////////////////////////////////////////////////////////////

static void TestMenuLayout()
{
    Section ( _T("Menu id arithmetic") );

CMenuLayout L = MenuLayoutCompute ( 6, 100 );

    Check ( 9 == L.GetIdsUsed(),
            _T("6 destinations claim 9 id slots, not 1") );
    Check ( mkBrowse == L.Classify ( 7 ) && mkManage == L.Classify ( 8 ),
            _T("Browse and Manage sit after the destinations") );
    Check ( mkNone == L.Classify ( L.GetIdsUsed() ),
            _T("the first offset past the claim is not ours") );

CMenuLayout E = MenuLayoutCompute ( 0, 100 );

    Check ( mkBrowse == E.Classify ( 1 ) && mkManage == E.Classify ( 2 ),
            _T("with an empty history, Browse and Manage still work") );
}


static void TestDestinationValidity()
{
    Section ( _T("Destinations the move must refuse") );

    MakeDir  ( _T("\\dest") );
    MakeFile ( _T("\\notafolder.txt") );
    MakeFile ( _T("\\src\\a.txt") );

CMovePlan plan;

    MovePlanBuild ( *MakeList ( _T("\\src\\a.txt") ), P ( _T("\\dest") ), plan );
    Check ( dvOk == plan.m_eDestValidity && 1 == plan.m_nMovable,
            _T("an ordinary folder is a usable destination") );

    MovePlanBuild ( *MakeList ( _T("\\src\\a.txt") ), P ( _T("\\nope") ), plan );
    Check ( dvMissing == plan.m_eDestValidity,
            _T("a destination that is not there is dvMissing, not a rename") );

    MovePlanBuild ( *MakeList ( _T("\\src\\a.txt") ), P ( _T("\\notafolder.txt") ), plan );
    Check ( dvNotAFolder == plan.m_eDestValidity,
            _T("a FILE as the destination is refused") );

    // This is the SHFileOperation trap the engine exists to avoid: one
    // source plus a non-existent destination silently renames the source
    // and reports success.
    Check ( !plan.IsRunnable(),
            _T("a bad destination never produces a runnable plan") );
}


static void TestSourceRejections()
{
    Section ( _T("Sources the move must refuse") );

    MakeDir  ( _T("\\tree\\inner\\deep") );
    MakeFile ( _T("\\tree\\inner\\deep\\f.txt") );
    MakeDir  ( _T("\\dest2") );
    MakeFile ( _T("\\dest2\\already.txt") );

CMovePlan plan;

    // THE data-loss case: moving a folder into its own subtree.
    MovePlanBuild ( *MakeList ( _T("\\tree") ), P ( _T("\\tree\\inner\\deep") ), plan );
    Check ( mrDestInsideSource == RejectFor ( plan, _T("\\tree") ),
            _T("a folder cannot be moved into its own descendant") );
    Check ( 0 == plan.m_nMovable,
            _T("...and that leaves nothing to move") );

    // The item IS the destination.
    MovePlanBuild ( *MakeList ( _T("\\dest2") ), P ( _T("\\dest2") ), plan );
    Check ( mrSameItem == RejectFor ( plan, _T("\\dest2") ),
            _T("an item cannot be moved into itself") );

    // Already in the destination folder.
    MovePlanBuild ( *MakeList ( _T("\\dest2\\already.txt") ), P ( _T("\\dest2") ), plan );
    Check ( mrAlreadyThere == RejectFor ( plan, _T("\\dest2\\already.txt") ),
            _T("an item already in the destination is skipped, not overwritten") );

    // A drive root.
    {
    CStringList ls;

    ls.AddTail ( _T("C:\\") );

    MovePlanBuild ( ls, P ( _T("\\dest2") ), plan );
    Check ( mrIsRoot == plan.m_aItems[0].m_eReject,
            _T("a drive root cannot be moved") );
    }

    // Gone between the right-click and the click.
    MovePlanBuild ( *MakeList ( _T("\\vanished.txt") ), P ( _T("\\dest2") ), plan );
    Check ( mrMissing == RejectFor ( plan, _T("\\vanished.txt") ),
            _T("a source that no longer exists is reported, not guessed at") );
}


static void TestNestedSelection()
{
    Section ( _T("Nested selection") );

    MakeDir  ( _T("\\nest\\parent\\child") );
    MakeFile ( _T("\\nest\\parent\\child\\c.txt") );
    MakeDir  ( _T("\\nest\\other") );
    MakeDir  ( _T("\\nestdest") );

CMovePlan plan;
CStringList ls;

    ls.AddTail ( P ( _T("\\nest\\parent") ) );
    ls.AddTail ( P ( _T("\\nest\\parent\\child") ) );
    ls.AddTail ( P ( _T("\\nest\\other") ) );

    MovePlanBuild ( ls, P ( _T("\\nestdest") ), plan );

    Check ( mrOk == RejectFor ( plan, _T("\\nest\\parent") ),
            _T("the outermost selected folder moves") );
    Check ( mrNestedInSelection == RejectFor ( plan, _T("\\nest\\parent\\child") ),
            _T("a child of another selected folder is dropped") );
    Check ( mrOk == RejectFor ( plan, _T("\\nest\\other") ),
            _T("an unrelated sibling is unaffected") );
    Check ( 2 == plan.m_nMovable, _T("two items remain movable") );
}


//////////////////////////////////////////////////////////////////////////
//
// One real move, end to end, through IFileOperation.
//
//////////////////////////////////////////////////////////////////////////

static void TestRealMove()
{
    Section ( _T("A real move") );

    MakeDir  ( _T("\\real\\from") );
    MakeDir  ( _T("\\real\\to") );
    MakeFile ( _T("\\real\\from\\moved.txt") );

CMovePlan plan;

    MovePlanBuild ( *MakeList ( _T("\\real\\from\\moved.txt") ),
                    P ( _T("\\real\\to") ), plan );

    Check ( plan.IsRunnable(), _T("the plan is runnable") );

CMoveOutcome outcome;
HRESULT      hr = MovePlanExecute ( plan, NULL, outcome );

    Check ( SUCCEEDED ( hr ), _T("PerformOperations succeeded") );

    // The claim that matters is not the HRESULT: PerformOperations
    // returns S_OK even when the whole batch was refused. It is the
    // per-item sink count, which is also what gates promoting the
    // destination into the history.
    Check ( 1 == outcome.m_nMoved,
            _T("exactly one item was confirmed moved by the sink") );

    Check ( -1 != GetFileAttributes ( P ( _T("\\real\\to\\moved.txt") ) ),
            _T("the file is at the destination") );
    Check ( -1 == GetFileAttributes ( P ( _T("\\real\\from\\moved.txt") ) ),
            _T("...and no longer at the source") );
}


//////////////////////////////////////////////////////////////////////
// main
//////////////////////////////////////////////////////////////////////

int _tmain ( int argc, TCHAR* argv[], TCHAR* envp[] )
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);
    UNREFERENCED_PARAMETER(envp);

    if ( !AfxWinInit ( ::GetModuleHandle ( NULL ), NULL, ::GetCommandLine(), 0 ) )
        {
        _tprintf ( _T("AfxWinInit failed\n") );
        return 2;
        }

// IFileOperation is an apartment-threaded COM object, and the real
// handler runs in Explorer's STA (ThreadingModel=Apartment in the .rgs).
HRESULT hrCo = CoInitializeEx ( NULL, COINIT_APARTMENTTHREADED );

TCHAR szTemp [MAX_PATH];

    GetTempPath ( MAX_PATH, szTemp );
    g_sRoot.Format ( _T("%sMoveToTests_%lu"), szTemp, GetCurrentProcessId() );

    SHCreateDirectoryEx ( NULL, g_sRoot, NULL );
    _tprintf ( _T("Test tree: %s\n"), (LPCTSTR) g_sRoot );

    TestPureLogic();
    TestMenuLayout();
    TestDestinationValidity();
    TestSourceRejections();
    TestNestedSelection();
    TestRealMove();

    // Clean up.
    {
    CString sFrom = g_sRoot;

    sFrom += _T('\0');                  // SHFileOperation wants a double null

    SHFILEOPSTRUCT fos;

    ZeroMemory ( &fos, sizeof(fos) );
    fos.wFunc  = FO_DELETE;
    fos.pFrom  = (LPCTSTR) sFrom;
    fos.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    SHFileOperation ( &fos );
    }

    if ( SUCCEEDED ( hrCo ) )
        CoUninitialize();

    _tprintf ( _T("\n%d passed, %d failed\n"), g_nPass, g_nFail );

    return ( 0 == g_nFail ) ? 0 : 1;
}
