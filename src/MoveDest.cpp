//////////////////////////////////////////////////////////////////////
//
// MoveDest.cpp : the destination store -- recent and pinned folders.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MoveDest.h"
#include <pathcch.h>            // link pathcch.lib
#include <strsafe.h>


#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

//////////////////////////////////////////////////////////////////////
// Where everything lives.
//
// The app key is the one MFC's own profile functions use --
// HKCU\Software\<SetRegistryKey arg>\<AFX_IDS_APP_TITLE> -- so the
// scalar options can keep using GetProfileInt/WriteProfileInt and the
// whole program has ONE settings root.
//
// The destination list does NOT go through GetProfileString, for two
// verified reasons:
//
//  * CWinApp::GetSectionKey re-derives the key on every single call
//    (appui3.cpp:112), and CWinApp::GetAppRegistryKey underneath it
//    opens HKCU\software with KEY_WRITE|KEY_READ (appui3.cpp:82-83) and
//    RegCreateKeyEx's the company and app keys.  Reading 40 values that
//    way is ~240 registry calls and takes a WRITE handle on the menu
//    path.  We open one key, KEY_READ, once.
//
//  * CWinApp::GetProfileString only ASSERTs that the value is REG_SZ
//    (appui3.cpp:181,188) and hands RegQueryValueEx's byte count
//    straight to CString::GetBuffer.  The registry does not guarantee a
//    terminating NUL.  ATL's CRegKey::QueryStringValue checks both --
//    it returns ERROR_INVALID_DATA for a wrong type, an odd byte count
//    or a missing terminator (atlbase.h, QueryStringValue) -- and
//    ReadRegString below does the same thing for the enumeration case,
//    which CRegKey does not cover.
//////////////////////////////////////////////////////////////////////

static LPCTSTR const g_szDestSubkey  = _T("Destinations");
static LPCTSTR const g_szOptSection  = _T("Options");
static LPCTSTR const g_szValMaxRecent= _T("uMaxRecent");
static LPCTSTR const g_szValSweepPos = _T("uSweepCursor");

// Session scoped on purpose.  See CStoreLock.
static LPCTSTR const g_szStoreMutex  = _T("Local\\MoveTo.Destinations.v1");

// Fixed part of a stored value: "1|R|<16 hex>|<4 hex>|<2 hex>|"
static const int g_nFixedLen = 29;

// Longest value we will even look at.  A path is capped at 32767 WCHARs
// (0x8000 == PATHCCH_MAX_CCH), and label + framing cannot double it.
static const DWORD g_cbMaxValue = 160 * 1024;


//////////////////////////////////////////////////////////////////////
// Small helpers
//////////////////////////////////////////////////////////////////////

static BOOL IsHexDigit ( TCHAR ch )
{
    return ( ch >= _T('0') && ch <= _T('9') ) ||
           ( ch >= _T('A') && ch <= _T('F') ) ||
           ( ch >= _T('a') && ch <= _T('f') );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    ParseHex()
//
// Description:
//  Reads exactly nDigits hex characters from sIn at nAt.  Deliberately
//  NOT _tcstoui64: that skips leading whitespace, accepts a sign, and
//  stops silently at the first bad character -- three ways to read a
//  corrupt value as a plausible number.  DirClean's rule applies:
//  corrupt means drop it, not guess it.
//
//////////////////////////////////////////////////////////////////////////

static BOOL ParseHex ( const CString& sIn, int nAt, int nDigits,
                       ULONGLONG& uOut )
{
    if ( nAt < 0  ||  nDigits <= 0  ||  nDigits > 16 )
        return FALSE;

    if ( sIn.GetLength() < nAt + nDigits )
        return FALSE;

ULONGLONG u = 0;

    for ( int i = 0; i < nDigits; i++ )
        {
        TCHAR ch = sIn[nAt + i];

        if ( !IsHexDigit ( ch ) )
            return FALSE;

        u <<= 4;

        if ( ch >= _T('0') && ch <= _T('9') )
            u |= (ULONGLONG)( ch - _T('0') );
        else if ( ch >= _T('A') && ch <= _T('F') )
            u |= (ULONGLONG)( ch - _T('A') + 10 );
        else
            u |= (ULONGLONG)( ch - _T('a') + 10 );
        }

    uOut = u;
    return TRUE;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    NowStamp()
//
// Description:
//  UTC FILETIME as a 64-bit integer.  GetSystemTimePreciseAsFileTime
//  rather than GetSystemTimeAsFileTime: the latter ticks at the system
//  timer interval (nominally 15.6 ms), so two moves inside one Explorer
//  session can easily share a stamp and then sort arbitrarily.  The
//  precise variant is sub-microsecond, which makes a tie effectively
//  impossible -- and DestNextStamp forces monotonicity anyway.
//
//////////////////////////////////////////////////////////////////////////

static ULONGLONG NowStamp()
{
FILETIME      ft;
ULARGE_INTEGER u;

    GetSystemTimePreciseAsFileTime ( &ft );

    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;

    return u.QuadPart;
}


//////////////////////////////////////////////////////////////////////
// CStoreLock
//
// Serialises read-modify-write of the whole key.  It is a CONVENIENCE,
// not the correctness mechanism: one-folder-one-value already makes
// concurrent promotion of different folders non-interacting, and
// RegSetValueEx replaces a value atomically.  The lock exists so that
// the enumerate-then-evict step sees a stable list.
//
// "Local\" is session scoped.  "Global\" would also cover the same user
// signed in twice (console + RDP), but creating a Global\ object needs
// SeCreateGlobalPrivilege, which an ordinary interactive token does not
// have -- so it would simply fail for most users.  A missed lock across
// two sessions costs at most one skipped eviction, so the trade is
// obvious.
//
// NEVER construct one of these on the QueryContextMenu path.
//////////////////////////////////////////////////////////////////////

class CStoreLock
{
public:
    CStoreLock() : m_hMutex(NULL), m_bHeld(FALSE)
    {
        m_hMutex = ::CreateMutex ( NULL, FALSE, g_szStoreMutex );

        if ( NULL == m_hMutex )
            return;

        DWORD dwWait = ::WaitForSingleObject ( m_hMutex, 2000 );

        // WAIT_ABANDONED means a process died holding it.  We DO own the
        // mutex in that case and must release it.  The store may be
        // mid-update, but every individual value is either the old one
        // or the new one, so there is nothing to repair.
        m_bHeld = ( WAIT_OBJECT_0 == dwWait  ||  WAIT_ABANDONED == dwWait );
    }

    ~CStoreLock()
    {
        if ( m_bHeld )
            ::ReleaseMutex ( m_hMutex );

        if ( NULL != m_hMutex )
            ::CloseHandle ( m_hMutex );
    }

    BOOL IsHeld() const     { return m_bHeld; }

private:
    HANDLE m_hMutex;
    BOOL   m_bHeld;

    CStoreLock ( const CStoreLock& );
    CStoreLock& operator= ( const CStoreLock& );
};


//////////////////////////////////////////////////////////////////////
// Key access
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//
// Function:    BuildDestKeyPath()
//
// Description:
//  "Software\<company>\<app>\Destinations", derived from the very same
//  CWinApp members MFC uses, so we cannot drift away from wherever
//  GetProfileInt is writing.
//
//  m_pszProfileName is set by SetRegistryKey from m_pszAppName
//  (appui3.cpp:27-28), and m_pszAppName comes from AFX_IDS_APP_TITLE
//  (appinit.cpp:112-118).  That string resource is therefore load
//  bearing: without it the app name falls back to the module base name,
//  and renaming the DLL would relocate every setting.  MoveTo.rc must
//  carry AFX_IDS_APP_TITLE "MoveTo", with the same do-not-touch comment
//  DirClean.rc carries at line 232.
//
//////////////////////////////////////////////////////////////////////////

static BOOL BuildDestKeyPath ( CString& sOut )
{
CWinApp* pApp = AfxGetApp();

    if ( NULL == pApp  ||  NULL == pApp->m_pszRegistryKey  ||
         NULL == pApp->m_pszProfileName )
        {
        return FALSE;
        }

    sOut.Format ( _T("Software\\%s\\%s\\%s"),
                  pApp->m_pszRegistryKey,
                  pApp->m_pszProfileName,
                  g_szDestSubkey );
    return TRUE;
}


// bForWrite == FALSE: open read only and DO NOT create.  A fresh
// install has no key, and the menu path must not create one.
static LONG OpenDestKey ( CRegKey& key, BOOL bForWrite )
{
CString sPath;

    if ( !BuildDestKeyPath ( sPath ) )
        return ERROR_INVALID_DATA;

    if ( !bForWrite )
        return key.Open ( HKEY_CURRENT_USER, sPath, KEY_READ );

    return key.Create ( HKEY_CURRENT_USER, sPath, REG_NONE,
                        REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    ReadRegString()
//
// Description:
//  RegQueryValueEx with the three checks the registry does not make for
//  you: the type is REG_SZ, the byte count is a whole number of TCHARs,
//  and the data is NUL terminated.  ATL's CRegKey::QueryStringValue
//  makes exactly these checks; CWinApp::GetProfileString makes none of
//  them outside a debug ASSERT.
//
//////////////////////////////////////////////////////////////////////////

static BOOL ReadRegString ( HKEY hKey, LPCTSTR szName, CString& sOut )
{
    sOut.Empty();

DWORD dwType = REG_NONE;
DWORD cb     = 0;

    if ( ERROR_SUCCESS != ::RegQueryValueEx ( hKey, szName, NULL, &dwType,
                                              NULL, &cb ) )
        return FALSE;

    if ( REG_SZ != dwType )
        return FALSE;

    if ( cb < sizeof(TCHAR)  ||  cb > g_cbMaxValue )
        return FALSE;

    if ( 0 != ( cb % sizeof(TCHAR) ) )
        return FALSE;

int    nChars = (int)( cb / sizeof(TCHAR) );
LPTSTR pszBuf = sOut.GetBuffer ( nChars );      // nChars + 1 writable

    if ( NULL == pszBuf )
        return FALSE;

DWORD cbRead = cb;
LONG  lRet   = ::RegQueryValueEx ( hKey, szName, NULL, &dwType,
                                   (LPBYTE) pszBuf, &cbRead );

    // Force a terminator at the one slot GetBuffer guarantees past the
    // data.  Without it a value written with no NUL (perfectly legal,
    // and what RegEdit's binary editor produces) leaves ReleaseBuffer to
    // scan uninitialised memory to the end of the allocation.
    pszBuf[nChars] = _T('\0');
    sOut.ReleaseBuffer();

    if ( ERROR_SUCCESS != lRet  ||  REG_SZ != dwType )
        {
        sOut.Empty();
        return FALSE;
        }

    return !sOut.IsEmpty();
}


//////////////////////////////////////////////////////////////////////
// CMoveDest serialisation
//////////////////////////////////////////////////////////////////////

CString CMoveDest::ToStorageString() const
{
CString sLabel ( m_sLabel );

    // Defence in depth: DestSetLabel refuses these at entry, but a
    // hand-edited registry can still get one in and we must not write
    // it back out.
    sLabel.Remove ( _T('|') );

CString s;

    s.Format ( _T("1|%c|%016I64X|%04X|%02X|%s|%s"),
               ( dkPinned == m_eKind ) ? _T('P') : _T('R'),
               m_uStamp,
               (UINT)( m_uPinOrder & 0xFFFF ),
               (UINT)( m_uMisses   & 0xFF ),
               (LPCTSTR) sLabel,
               (LPCTSTR) m_sPath );
    return s;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    CMoveDest::FromStorageString()
//
// Description:
//  Parses the fixed 29-character prefix by index, then splits the tail
//  at its LAST '|'.  '|' is one of the characters Windows forbids in a
//  path, so the last separator is always the label/path boundary --
//  even if a label somehow contains one.  Splitting from the left would
//  put half a label into the path field, which is exactly the class of
//  bug that made DirClean weld the type into the pattern string.
//
//  A string with no "1|" prefix is a bare path: recent, never seen,
//  stamp 0.  That is both the v0 migration and what a user gets if they
//  type a path straight into RegEdit, and it needs no version check
//  anywhere else in the program.
//
//////////////////////////////////////////////////////////////////////////

BOOL CMoveDest::FromStorageString ( LPCTSTR szStored, CMoveDest& dstOut )
{
    if ( NULL == szStored  ||  _T('\0') == *szStored )
        return FALSE;

CString sIn ( szStored );

    dstOut = CMoveDest();

    // ---- unversioned: the whole string is a path -------------------
    if ( sIn.GetLength() < 2  ||  _T('1') != sIn[0]  ||  _T('|') != sIn[1] )
        {
        CString sCanon;

        if ( !DestCanonicalizeLexical ( sIn, sCanon ) )
            return FALSE;

        dstOut.SetPath ( sCanon );
        return TRUE;
        }

    if ( sIn.GetLength() <= g_nFixedLen )
        return FALSE;

    // ---- fixed prefix, by index ------------------------------------
    if ( _T('|') != sIn[3]   ||  _T('|') != sIn[20]  ||
         _T('|') != sIn[25]  ||  _T('|') != sIn[28] )
        {
        return FALSE;
        }

TCHAR chKind = sIn[2];

    if ( _T('P') == chKind )
        dstOut.SetKind ( dkPinned );
    else if ( _T('R') == chKind )
        dstOut.SetKind ( dkRecent );
    else
        return FALSE;               // corrupt -- drop it, do not guess

ULONGLONG uStamp = 0, uOrder = 0, uMiss = 0;

    if ( !ParseHex ( sIn,  4, 16, uStamp ) )  return FALSE;
    if ( !ParseHex ( sIn, 21,  4, uOrder ) )  return FALSE;
    if ( !ParseHex ( sIn, 26,  2, uMiss  ) )  return FALSE;

    dstOut.SetStamp ( uStamp );
    dstOut.SetPinOrder ( (UINT) uOrder );
    dstOut.SetMisses ( (UINT) uMiss );

    // ---- tail: label | path ----------------------------------------
CString sRest = sIn.Mid ( g_nFixedLen );
int     nBar  = sRest.ReverseFind ( _T('|') );

    if ( -1 == nBar )
        return FALSE;

CString sLabel = sRest.Left ( nBar );
CString sPath  = sRest.Mid ( nBar + 1 );
CString sCanon;

    // Re-canonicalise on the way in.  The registry is a file anyone can
    // edit; a stored path is no more trusted than a .dirclean line.
    if ( !DestCanonicalizeLexical ( sPath, sCanon ) )
        return FALSE;

    sLabel.Trim();
    sLabel.Remove ( _T('|') );

    dstOut.SetPath ( sCanon );
    dstOut.SetLabel ( sLabel );
    return TRUE;
}


//////////////////////////////////////////////////////////////////////
// Canonicalisation
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//
// Function:    DestCanonicalizeLexical()
//
// Description:
//  Pure syntax, no disk.  Handles the cases that cost nothing:
//  forward slashes, doubled separators, "." and ".." segments, a
//  trailing backslash, and the \\?\ prefix.
//
//  PathCchCanonicalizeEx with PATHCCH_ALLOW_LONG_PATHS is used rather
//  than the shlwapi PathCanonicalize, which has a fixed MAX_PATH
//  contract and will happily overrun a longer input.  Note what the SDK
//  header says the flag does (pathcch.h, PATHCCH_OPTIONS): for a
//  process that is not long-path enabled it CONVERTS a long result into
//  the \\?\ device form.  We strip that prefix again before storing,
//  because the stored path is also the display path and the thing we
//  hand to the shell -- SHFileOperation does not accept \\?\ at all.
//
//  Trailing backslash rule: strip it, then put it back only for a bare
//  drive root.  "C:" without the backslash is not the root of C: -- it
//  means "whatever directory the process currently has on C:", which is
//  a different folder on every process in the machine.
//
//////////////////////////////////////////////////////////////////////////

BOOL DestCanonicalizeLexical ( LPCTSTR szIn, CString& sOut )
{
    sOut.Empty();

    if ( NULL == szIn  ||  _T('\0') == *szIn )
        return FALSE;

CString sIn ( szIn );

    sIn.Trim();

    if ( sIn.IsEmpty()  ||  sIn.GetLength() >= PATHCCH_MAX_CCH )
        return FALSE;

    for ( int i = 0; i < sIn.GetLength(); i++ )
        {
        if ( sIn[i] < _T(' ') )
            return FALSE;           // control character
        }

    sIn.Replace ( _T('/'), _T('\\') );

    // THE PREFIX COMES OFF FIRST.
    //
    // Order matters here and the self-test is what proved it: the
    // wildcard check below rejects '?', and "\\?\" contains one.  With
    // the checks the other way round, every extended-length path was
    // refused -- including the one GetFinalPathNameByHandle hands back,
    // which silently turned DestCanonicalizeOnDisk into a no-op that
    // never expanded a short name or fixed a path's casing.
    if ( 0 == sIn.Left(8).CompareNoCase ( _T("\\\\?\\UNC\\") ) )
        sIn = _T("\\\\") + sIn.Mid ( 8 );
    else if ( 0 == sIn.Left(4).Compare ( _T("\\\\?\\") ) )
        sIn = sIn.Mid ( 4 );

    // Device paths (\\.\PhysicalDrive0) and volume GUID paths
    // (\\?\Volume{...}, now "Volume{...}" after the strip above) are not
    // folders a user moves files into.
    if ( 0 == sIn.Left(4).Compare ( _T("\\\\.\\") ) )
        return FALSE;

    // A wildcard in a stored destination is either corruption or an
    // attempt to make the move land somewhere unintended.  DirClean's
    // CShellFileOp::AddSourceFile refuses '*' and '?' for the same
    // reason; this is the destination-side twin of that check.
    if ( -1 != sIn.FindOneOf ( _T("*?\"<>|") ) )
        return FALSE;

    // Collapse duplicate separators.  PathCchCanonicalizeEx folds "."
    // and ".." but leaves "C:\a\\b" alone -- verified by the self-test,
    // which failed on exactly that input.  The leading "\\" of a UNC
    // name is preserved.
    {
    int nStart = ( 0 == sIn.Left(2).Compare ( _T("\\\\") ) ) ? 2 : 0;
    CString sFix = sIn.Left ( nStart );

    for ( int i = nStart; i < sIn.GetLength(); i++ )
        {
        if ( _T('\\') == sIn[i]  &&  i + 1 < sIn.GetLength()  &&
             _T('\\') == sIn[i + 1] )
            {
            continue;
            }

        sFix += sIn[i];
        }

    sIn = sFix;
    }

std::vector<TCHAR> aBuf ( PATHCCH_MAX_CCH, _T('\0') );

    if ( FAILED ( ::PathCchCanonicalizeEx ( &aBuf[0], aBuf.size(), sIn,
                                            PATHCCH_ALLOW_LONG_PATHS ) ) )
        {
        return FALSE;
        }

CString sPath ( &aBuf[0] );

    // The canonicaliser may have ADDED the prefix back (that is what
    // PATHCCH_ALLOW_LONG_PATHS does for a long result in a process that
    // is not long-path enabled -- pathcch.h, PATHCCH_OPTIONS).
    if ( 0 == sPath.Left(8).CompareNoCase ( _T("\\\\?\\UNC\\") ) )
        sPath = _T("\\\\") + sPath.Mid ( 8 );
    else if ( 0 == sPath.Left(4).Compare ( _T("\\\\?\\") ) )
        sPath = sPath.Mid ( 4 );

    // Must be absolute: a drive-qualified path or a UNC share.
    if ( sPath.GetLength() < 3 )
        return FALSE;

BOOL bDrive = ( _T(':') == sPath[1] ) &&
              ( ( sPath[0] >= _T('A') && sPath[0] <= _T('Z') ) ||
                ( sPath[0] >= _T('a') && sPath[0] <= _T('z') ) ) &&
              ( _T('\\') == sPath[2] );

BOOL bUNC   = ( 0 == sPath.Left(2).Compare ( _T("\\\\") ) );

    if ( !bDrive  &&  !bUNC )
        return FALSE;

    if ( bUNC  &&  ::PathIsUNCServerShare ( sPath ) == FALSE  &&
         ::PathIsUNC ( sPath ) == FALSE )
        {
        return FALSE;
        }

    // Normalise the trailing backslash.
    while ( sPath.GetLength() > 0 &&
            _T('\\') == sPath[sPath.GetLength() - 1] )
        {
        sPath = sPath.Left ( sPath.GetLength() - 1 );
        }

    if ( 2 == sPath.GetLength()  &&  _T(':') == sPath[1] )
        sPath += _T('\\');          // "C:" -> "C:\"  -- see the comment

    if ( sPath.GetLength() < 3 )
        return FALSE;

    sOut = sPath;
    return TRUE;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestCanonicalizeOnDisk()
//
// Description:
//  Resolves a folder to the name the file system itself uses.
//
//  HOW FAR DEDUP GOES, AND WHY IT STOPS THERE
//
//  One handle plus GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED |
//  VOLUME_NAME_DOS) collapses, in a single documented call:
//
//      PROGRA~1        8.3 short name    -> long name
//      S:\proj         SUBST drive       -> C:\work\proj
//      Z:\team         mapped drive      -> \\srv\share\team
//      C:\link         junction/symlink  -> the target
//      C:\mnt\data     volume mount pt   -> the mounted volume's path
//      c:\USERS\Me     wrong casing      -> C:\Users\me
//
//  We stop there.  Not attempted:
//
//   * Server aliases, IP vs name vs FQDN, and DFS namespace links.
//     \\SRV\pub and \\srv.corp.local\pub may be one folder, but proving
//     it needs name resolution and an extra round trip that can stall
//     for seconds against a dead server.  The cost of being wrong is a
//     second menu entry, which is cosmetic.
//
//   * Volume serial + file id identity (FILE_ID_INFO via
//     GetFileInformationByHandleEx).  It adds nothing over the final
//     path -- a directory has one id and the final path already names
//     it -- and an id stored in the registry goes stale the moment a
//     folder is deleted and recreated, which is exactly what a build
//     directory does.
//
//  The asymmetry is what justifies the line: failing to merge two
//  spellings shows a duplicate menu item, while wrongly merging two
//  folders sends the user's files somewhere they did not choose.  We
//  merge only what a documented local call proves identical.
//
//  Note: NOT on the menu path.  CreateFile on a mapped drive whose
//  server is asleep blocks, and blocking inside QueryContextMenu hangs
//  every Explorer window in the process.
//
//////////////////////////////////////////////////////////////////////////

BOOL DestCanonicalizeOnDisk ( LPCTSTR szIn, CString& sOut )
{
CString sLex;

    if ( !DestCanonicalizeLexical ( szIn, sLex ) )
        return FALSE;

    sOut = sLex;                    // sensible fallback

    // dwDesiredAccess 0 asks for name/attribute access only -- enough
    // for GetFinalPathNameByHandle, and it succeeds on folders whose
    // contents we may not read.  FILE_FLAG_BACKUP_SEMANTICS is
    // mandatory to open a directory at all.
HANDLE hDir = ::CreateFile ( sLex, 0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, NULL );

    if ( INVALID_HANDLE_VALUE == hDir )
        return FALSE;               // gone, or not ours to open

BOOL bOK = FALSE;

    {
    DWORD cch = ::GetFinalPathNameByHandle ( hDir, NULL, 0,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS );

    // On failure it returns 0; when the buffer is too small it returns
    // the required size INCLUDING the terminator.  Asking with a NULL
    // buffer is the documented way to size it.
    if ( cch > 0  &&  cch < PATHCCH_MAX_CCH )
        {
        std::vector<TCHAR> aBuf ( cch + 1, _T('\0') );

        DWORD cchGot = ::GetFinalPathNameByHandle ( hDir, &aBuf[0], cch,
                                                    FILE_NAME_NORMALIZED | VOLUME_NAME_DOS );

        if ( cchGot > 0  &&  cchGot < cch + 1 )
            {
            CString sFinal;

            // The result carries \\?\ or \\?\UNC\; the lexical pass
            // strips both and re-validates.
            if ( DestCanonicalizeLexical ( &aBuf[0], sFinal ) )
                {
                sOut = sFinal;
                bOK  = TRUE;
                }
            }
        }
    }

    ::CloseHandle ( hDir );

    // TRUE either way: the handle opened, so the folder exists, and the
    // lexical form is a usable fallback if the final-path step failed.
    // The caller gets no signal that resolution was partial -- by
    // design, because "exists" is the decision it actually makes.
    //
    // Do NOT write this as "return bOK ? TRUE : TRUE;".  That is what
    // the first draft said, and it hid a real failure for an entire
    // test round: the lexical pass was rejecting the "\\?\" prefix this
    // API returns, so bOK was always FALSE, every short name stayed
    // short and no path ever got its on-disk casing.  A ternary whose
    // arms are equal is a comment that lies.
    UNREFERENCED_PARAMETER(bOK);

    return TRUE;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestMakeKey()
//
// Description:
//  Invariant uppercase, for hashing and for nothing else.
//
//  LCMapStringEx with LOCALE_NAME_INVARIANT and plain LCMAP_UPPERCASE.
//  The SDK spells out why that is the right flag set: the header notes
//  that LCMAP_LINGUISTIC_CASING means "use linguistic rules for casing"
//  (winnls.h:274) -- so WITHOUT it, casing follows FILE SYSTEM rules,
//  which is precisely the mapping NTFS uses.  CharUpperBuff and
//  CString::MakeUpper are locale sensitive: under tr-TR, 'i' does not
//  uppercase to 'I', so the same folder would hash two different ways
//  on two machines.
//
//////////////////////////////////////////////////////////////////////////

CString DestMakeKey ( LPCTSTR szCanonicalPath )
{
CString sIn ( szCanonicalPath );

    if ( sIn.IsEmpty() )
        return sIn;

int nNeed = ::LCMapStringEx ( LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                              sIn, sIn.GetLength(), NULL, 0,
                              NULL, NULL, 0 );

    if ( nNeed <= 0 )
        {
        CString sFallback ( sIn );      // should not happen; stay usable

        sFallback.MakeUpper();
        return sFallback;
        }

CString sOut;
LPTSTR  pszBuf = sOut.GetBuffer ( nNeed );

    if ( NULL == pszBuf )
        return sIn;

int nGot = ::LCMapStringEx ( LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                             sIn, sIn.GetLength(), pszBuf, nNeed,
                             NULL, NULL, 0 );

    sOut.ReleaseBuffer ( nGot > 0 ? nGot : 0 );

    return sOut.IsEmpty() ? sIn : sOut;
}


BOOL DestSameFolder ( LPCTSTR szA, LPCTSTR szB )
{
    if ( NULL == szA  ||  NULL == szB )
        return FALSE;

    // Ordinal, case insensitive.  NOT lstrcmpi / CompareString, which
    // sort by the current locale's word rules -- the Turkish dotless-i
    // makes those disagree with the file system about whether two paths
    // are the same folder.
    return ( CSTR_EQUAL == ::CompareStringOrdinal ( szA, -1, szB, -1, TRUE ) );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestValueName()
//
// Description:
//  "D" + 16 hex digits of FNV-1a over the uppercased key.
//
//  The name must be DERIVED, not allocated, or two Explorer windows
//  moving to the same folder at the same moment would each invent a
//  name and the folder would appear twice.  It must be BOUNDED,
//  because a registry value name is capped at 16383 characters while a
//  path can reach 32767.  A hash gives both.
//
//  Collisions are handled rather than prayed about: the full path is
//  stored inside the value, every reader compares it, and the writer
//  falls back to "D<hash>.1", ".2"... when the name is taken by a
//  different folder.  That costs ten lines and removes the failure
//  mode where two unrelated folders silently share one entry.
//
//////////////////////////////////////////////////////////////////////////

static ULONGLONG Fnv1a64 ( const void* pv, size_t cb )
{
const BYTE* p = (const BYTE*) pv;
ULONGLONG   h = 14695981039346656037ULL;

    for ( size_t i = 0; i < cb; i++ )
        {
        h ^= (ULONGLONG) p[i];
        h *= 1099511628211ULL;
        }

    return h;
}


CString DestValueName ( LPCTSTR szCanonicalPath )
{
CString sKey = DestMakeKey ( szCanonicalPath );
ULONGLONG uHash = Fnv1a64 ( (LPCTSTR) sKey,
                            (size_t) sKey.GetLength() * sizeof(TCHAR) );
CString s;

    s.Format ( _T("D%016I64X"), uHash );
    return s;
}


//////////////////////////////////////////////////////////////////////
// Enumeration
//////////////////////////////////////////////////////////////////////

// One row as it exists in the registry: the entry plus the value name
// it came from, so the mutating paths can delete or rewrite it without
// recomputing anything.
struct DestRow
{
    CString   sValueName;
    CMoveDest dst;
};

typedef std::vector<DestRow> CDestRowVec;


//////////////////////////////////////////////////////////////////////////
//
// Function:    EnumDestRows()
//
// Description:
//  Reads every value under the key.  Bounded by dcMaxValues so a
//  corrupt or hostile key cannot make the menu path loop.  Unparsable
//  values are skipped silently -- a bad row must not take the whole
//  menu with it.
//
//  RegEnumValue's name buffer is sized in CHARACTERS and, on return,
//  holds the length WITHOUT the terminator; it must be reset on every
//  iteration or the second call fails with ERROR_MORE_DATA.  That reset
//  is the classic bug in this loop.
//
//////////////////////////////////////////////////////////////////////////

static void EnumDestRows ( HKEY hKey, CDestRowVec& aOut )
{
    aOut.clear();

    // 16383 is the documented ceiling on a registry value name.
    const DWORD cchNameMax = 16384;

std::vector<TCHAR> aName ( cchNameMax, _T('\0') );

    for ( DWORD dwIndex = 0; dwIndex < (DWORD) dcMaxValues; dwIndex++ )
        {
        DWORD cchName = cchNameMax;         // RESET every iteration
        DWORD dwType  = REG_NONE;

        LONG lRet = ::RegEnumValue ( hKey, dwIndex, &aName[0], &cchName,
                                     NULL, &dwType, NULL, NULL );

        if ( ERROR_NO_MORE_ITEMS == lRet )
            break;

        if ( ERROR_SUCCESS != lRet )
            break;

        if ( REG_SZ != dwType  ||  0 == cchName )
            continue;

        aName[cchName] = _T('\0');          // RegEnumValue does terminate,
                                            // but be explicit about it

        CString sStored;

        if ( !ReadRegString ( hKey, &aName[0], sStored ) )
            continue;

        DestRow row;

        if ( !CMoveDest::FromStorageString ( sStored, row.dst ) )
            continue;

        row.sValueName = &aName[0];
        aOut.push_back ( row );
        }
}


//////////////////////////////////////////////////////////////////////
// Sorting
//////////////////////////////////////////////////////////////////////

static bool PinLess ( const DestRow& a, const DestRow& b )
{
    if ( a.dst.GetPinOrder() != b.dst.GetPinOrder() )
        return a.dst.GetPinOrder() < b.dst.GetPinOrder();

    // Ties are broken by value name so the order is identical in every
    // process, rather than being whatever RegEnumValue happened to say.
    return ( ::CompareStringOrdinal ( a.sValueName, -1,
                                      b.sValueName, -1, FALSE ) == CSTR_LESS_THAN );
}


static bool RecentNewerFirst ( const DestRow& a, const DestRow& b )
{
    if ( a.dst.GetStamp() != b.dst.GetStamp() )
        return a.dst.GetStamp() > b.dst.GetStamp();

    return ( ::CompareStringOrdinal ( a.sValueName, -1,
                                      b.sValueName, -1, FALSE ) == CSTR_LESS_THAN );
}


//////////////////////////////////////////////////////////////////////
// Capacity
//////////////////////////////////////////////////////////////////////

UINT DestGetCapacity()
{
CWinApp* pApp = AfxGetApp();

    if ( NULL == pApp )
        return dcDefaultRecent;

UINT u = pApp->GetProfileInt ( g_szOptSection, g_szValMaxRecent,
                               dcDefaultRecent );

    if ( u < (UINT) dcMinRecent )  u = dcMinRecent;
    if ( u > (UINT) dcMaxRecent )  u = dcMaxRecent;

    return u;
}


void DestSetCapacity ( UINT uCap )
{
CWinApp* pApp = AfxGetApp();

    if ( NULL == pApp )
        return;

    if ( uCap < (UINT) dcMinRecent )  uCap = dcMinRecent;
    if ( uCap > (UINT) dcMaxRecent )  uCap = dcMaxRecent;

    pApp->WriteProfileInt ( g_szOptSection, g_szValMaxRecent, (int) uCap );
}


//////////////////////////////////////////////////////////////////////
// Load -- the context menu path
//////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//
// Function:    DestVecLoad()
//
// Description:
//  Everything QueryContextMenu needs, and nothing it does not.
//
//  Budget: one RegOpenKeyEx, one RegEnumValue loop, two sorts, and a
//  label pass over at most 64 strings.  No file system call of any kind
//  -- not GetFileAttributes, not SHGetFileInfo, not GetDriveType.  Any
//  one of those against a disconnected share costs seconds, once per
//  entry, on the thread Explorer draws menus with.
//
//  Over-capacity recents are dropped from the RETURNED VECTOR only.
//  Trimming the registry here would make the menu path a writer, and
//  two windows right-clicking at once would then race over eviction --
//  the DirClean destructor bug, rebuilt.
//
//////////////////////////////////////////////////////////////////////////

void DestVecLoad ( CDestVec& aOut )
{
    aOut.clear();

CRegKey key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, FALSE ) )
        return;                     // fresh install: Browse... only

CDestRowVec aRows;

    EnumDestRows ( key, aRows );
    key.Close();

CDestRowVec aPins, aRecent;

    for ( size_t i = 0; i < aRows.size(); i++ )
        {
        if ( dkPinned == aRows[i].dst.GetKind() )
            aPins.push_back ( aRows[i] );
        else
            aRecent.push_back ( aRows[i] );
        }

    std::sort ( aPins.begin(),   aPins.end(),   PinLess );
    std::sort ( aRecent.begin(), aRecent.end(), RecentNewerFirst );

    if ( aPins.size() > (size_t) dcMaxPins )
        aPins.resize ( dcMaxPins );

size_t nCap = (size_t) DestGetCapacity();

    if ( aRecent.size() > nCap )
        aRecent.resize ( nCap );

    aOut.reserve ( aPins.size() + aRecent.size() );

    for ( size_t i = 0; i < aPins.size(); i++ )
        aOut.push_back ( aPins[i].dst );

    for ( size_t i = 0; i < aRecent.size(); i++ )
        aOut.push_back ( aRecent[i].dst );

    DestVecComputeLabels ( aOut );
}


//////////////////////////////////////////////////////////////////////
// Display labels
//////////////////////////////////////////////////////////////////////

// Last n backslash-separated components of a path, e.g.
// TailComponents("C:\a\b\c", 2) == "b\c".  A drive or share root has
// no components and returns itself.
static CString TailComponents ( const CString& sPath, int nWanted )
{
    if ( nWanted <= 0 )
        return CString();

    // A drive root has no leaf.  Return it whole, backslash included:
    // the self-test caught "D:\" being displayed as "D:", which is the
    // spelling that means "the current directory on D:".
    if ( 3 == sPath.GetLength()  &&  _T(':') == sPath[1] )
        return sPath;

int nEnd = sPath.GetLength();

    // Ignore a root's trailing backslash ("C:\").
    if ( nEnd > 0  &&  _T('\\') == sPath[nEnd - 1] )
        nEnd--;

int nStart = nEnd;
int nFound = 0;

    while ( nStart > 0  &&  nFound < nWanted )
        {
        int nPrev = nStart - 1;

        while ( nPrev > 0  &&  _T('\\') != sPath[nPrev] )
            nPrev--;

        if ( _T('\\') != sPath[nPrev] )
            {
            nStart = 0;
            nFound++;
            break;
            }

        nStart = nPrev;
        nFound++;
        }

    if ( nStart <= 0 )
        return sPath.Left ( nEnd );

    return sPath.Mid ( nStart + 1, nEnd - nStart - 1 );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    SanitizeMenuText()
//
// Description:
//  Makes one string safe to hand to InsertMenu.
//
//   * '&' becomes '&&', or the menu swallows it as an accelerator and
//     "R&D archive" shows as "RD archive" with a mnemonic on D.
//   * '\t' and '\b' are column and right-align separators in a menu
//     string; a folder name containing either would split the item.
//   * U+202A..U+202E and U+2066..U+2069 are the bidi overrides.  They
//     are legal in a folder name and they visually reverse the text
//     that FOLLOWS them -- including the rest of the menu item.
//
//  Escaping is done LAST, after truncation, because "&&" costs two
//  characters of budget but draws as one.
//
//////////////////////////////////////////////////////////////////////////

static CString SanitizeMenuText ( const CString& sIn )
{
CString sOut;

    sOut.Preallocate ( sIn.GetLength() * 2 + 2 );

    for ( int i = 0; i < sIn.GetLength(); i++ )
        {
        TCHAR ch = sIn[i];

        if ( ch < _T(' ') )
            continue;                       // includes \t and \b

        if ( ( ch >= 0x202A && ch <= 0x202E ) ||
             ( ch >= 0x2066 && ch <= 0x2069 ) )
            {
            continue;                       // bidi override
            }

        if ( _T('&') == ch )
            sOut += _T("&&");
        else
            sOut += ch;
        }

    return sOut;
}


// Middle-ellipsis for something that is still too long.
static CString Shorten ( const CString& sIn, int nMax )
{
    if ( sIn.GetLength() <= nMax  ||  nMax < 8 )
        return sIn;

int nHead = ( nMax - 1 ) / 2;
int nTail = nMax - 1 - nHead;

    return sIn.Left ( nHead ) + _T("\x2026") + sIn.Right ( nTail );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestVecComputeLabels()
//
// Description:
//  Gives every entry a distinct menu string.
//
//  Two pinned folders called "build" is the case that matters, and it
//  is not rare -- it is the normal state of a machine with two
//  checkouts.  Showing "build" twice makes the menu actively dangerous,
//  and showing two 200-character paths makes it unusable.
//
//  The rule:
//
//    1. A user-supplied label wins outright and is never disambiguated.
//       If the user named two things the same, that is their answer.
//    2. Otherwise start with the leaf folder name.
//    3. While two entries collide, widen the colliding ones by one more
//       parent component, up to dcMaxDisambig.  Shown as
//       "build  (proj-a)", then "build  (src\proj-a)".
//    4. Still colliding after that: fall back to the whole path,
//       middle-ellipsised.
//    5. Still colliding (two paths that ellipsise alike): append
//       " (2)", " (3)".  Termination is guaranteed, and the numbering
//       is deterministic because the vector order is.
//
//  Pure function of the vector -- no SHGetFileInfo, no disk.  The full
//  path is not lost: it goes in the status bar text from
//  GetCommandString(GCS_HELPTEXT), where there is room for it.
//
//////////////////////////////////////////////////////////////////////////

void DestVecComputeLabels ( CDestVec& aVec )
{
size_t n = aVec.size();

    if ( 0 == n )
        return;

std::vector<CString> asBase ( n );
std::vector<int>     anWidth ( n, 1 );
std::vector<bool>    abCustom ( n, false );

    for ( size_t i = 0; i < n; i++ )
        {
        if ( !aVec[i].GetLabel().IsEmpty() )
            {
            asBase[i]   = aVec[i].GetLabel();
            abCustom[i] = true;
            }
        else
            {
            asBase[i] = TailComponents ( aVec[i].GetPath(), 1 );

            if ( asBase[i].IsEmpty() )
                asBase[i] = aVec[i].GetPath();
            }
        }

    // Widen colliding entries, one component at a time.
    for ( int nPass = 1; nPass < dcMaxDisambig; nPass++ )
        {
        std::vector<bool> abClash ( n, false );
        bool bAny = false;

        for ( size_t i = 0; i < n; i++ )
            {
            for ( size_t j = i + 1; j < n; j++ )
                {
                if ( abCustom[i] || abCustom[j] )
                    continue;

                if ( DestSameFolder ( asBase[i], asBase[j] ) )
                    {
                    abClash[i] = abClash[j] = true;
                    bAny = true;
                    }
                }
            }

        if ( !bAny )
            break;

        for ( size_t i = 0; i < n; i++ )
            {
            if ( !abClash[i] )
                continue;

            anWidth[i]++;

            CString sLeaf = TailComponents ( aVec[i].GetPath(), 1 );
            CString sCtx  = TailComponents ( aVec[i].GetPath(), anWidth[i] );

            // Drop the leaf off the front of the context chain.
            if ( sCtx.GetLength() > sLeaf.GetLength() + 1 )
                sCtx = sCtx.Left ( sCtx.GetLength() - sLeaf.GetLength() - 1 );

            if ( sCtx.IsEmpty()  ||  DestSameFolder ( sCtx, sLeaf ) )
                asBase[i] = aVec[i].GetPath();
            else
                asBase[i] = sLeaf + _T("  (") + sCtx + _T(")");
            }
        }

    // Anything still colliding falls back to the full path, then to a
    // numeric suffix.  This loop is what makes uniqueness a guarantee
    // rather than a hope.
    for ( size_t i = 0; i < n; i++ )
        {
        for ( size_t j = 0; j < i; j++ )
            {
            if ( !DestSameFolder ( asBase[i], asBase[j] ) )
                continue;

            if ( !DestSameFolder ( asBase[i], aVec[i].GetPath() ) )
                {
                asBase[i] = aVec[i].GetPath();
                j = (size_t) -1;            // restart the inner scan
                continue;
                }

            for ( int nTry = 2; nTry < 100; nTry++ )
                {
                CString sTry;

                sTry.Format ( _T("%s (%d)"), (LPCTSTR) aVec[i].GetPath(), nTry );

                bool bTaken = false;

                for ( size_t k = 0; k < i; k++ )
                    {
                    if ( DestSameFolder ( sTry, asBase[k] ) )
                        { bTaken = true; break; }
                    }

                if ( !bTaken )
                    { asBase[i] = sTry; break; }
                }

            break;
            }
        }

    for ( size_t i = 0; i < n; i++ )
        aVec[i].SetDisplay ( SanitizeMenuText ( Shorten ( asBase[i], dcMaxLabelChars ) ) );
}


//////////////////////////////////////////////////////////////////////
// Mutation -- the invoke path
//////////////////////////////////////////////////////////////////////

// Find the row holding this folder, resolving a hash collision by
// comparing the stored path.  Returns the index or -1, and always sets
// sNameOut to the value name this folder should be written to.
static int FindRow ( const CDestRowVec& aRows, HKEY hKey,
                     const CString& sCanon, CString& sNameOut )
{
CString sWanted = DestValueName ( sCanon );

    UNREFERENCED_PARAMETER(hKey);

    for ( size_t i = 0; i < aRows.size(); i++ )
        {
        if ( DestSameFolder ( aRows[i].dst.GetPath(), sCanon ) )
            {
            sNameOut = aRows[i].sValueName;
            return (int) i;
            }
        }

    // Not present.  Make sure the name we are about to claim is not
    // already occupied by a DIFFERENT folder that hashed the same way.
int nSuffix = 0;

    for ( ;; )
        {
        CString sTry ( sWanted );

        if ( nSuffix > 0 )
            sTry.AppendFormat ( _T(".%d"), nSuffix );

        bool bTaken = false;

        for ( size_t i = 0; i < aRows.size(); i++ )
            {
            if ( 0 == aRows[i].sValueName.CompareNoCase ( sTry ) )
                { bTaken = true; break; }
            }

        if ( !bTaken )
            {
            sNameOut = sTry;
            return -1;
            }

        if ( ++nSuffix > 16 )
            {
            sNameOut = sTry;        // absurd; overwrite rather than spin
            return -1;
            }
        }
}


// Highest stamp in the store, so a clock that has gone backwards (NTP
// correction, a user fixing the time zone, a restored VM snapshot)
// cannot push a just-used folder to the bottom of the menu.
static ULONGLONG DestNextStamp ( const CDestRowVec& aRows )
{
ULONGLONG uMax = 0;

    for ( size_t i = 0; i < aRows.size(); i++ )
        {
        if ( aRows[i].dst.GetStamp() > uMax )
            uMax = aRows[i].dst.GetStamp();
        }

ULONGLONG uNow = NowStamp();

    return ( uNow > uMax ) ? uNow : ( uMax + 1 );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    EvictRecents()
//
// Description:
//  Deletes the oldest recents beyond capacity.
//
//  Three rules, all of them about not destroying something another
//  window just created:
//
//   * Only recents are ever evicted.  A pin is user intent and is
//     immune to recency entirely.  That is also what makes pinning
//     useful: pin a folder and it stops competing for MRU slots.
//   * The entry just written is excluded by name, never by position, so
//     it cannot evict itself if the clock is odd.
//   * Nothing with a stamp at or above the one we just wrote is
//     touched.  If another Explorer window promoted a folder while we
//     were working, its entry is newer than ours and is left alone.
//
//////////////////////////////////////////////////////////////////////////

static void EvictRecents ( CRegKey& key, CDestRowVec& aRows,
                           const CString& sJustWritten,
                           ULONGLONG uJustWrittenStamp,
                           BOOL bJustWrittenIsRecent )
{
CDestRowVec aRecent;

    for ( size_t i = 0; i < aRows.size(); i++ )
        {
        if ( dkPinned == aRows[i].dst.GetKind() )
            continue;

        if ( 0 == aRows[i].sValueName.CompareNoCase ( sJustWritten ) )
            continue;

        aRecent.push_back ( aRows[i] );
        }

    std::sort ( aRecent.begin(), aRecent.end(), RecentNewerFirst );

size_t nCap = (size_t) DestGetCapacity();

    if ( bJustWrittenIsRecent )
        nCap = ( nCap > 0 ) ? nCap - 1 : 0;

    for ( size_t i = nCap; i < aRecent.size(); i++ )
        {
        if ( aRecent[i].dst.GetStamp() >= uJustWrittenStamp )
            continue;               // someone else's newer write

        key.DeleteValue ( aRecent[i].sValueName );
        }
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestPromote()
//
// Description:
//  Records a successful move.  THE ONLY PLACE the recency order
//  changes, and it runs after the file operation has finished -- never
//  from a constructor, never from a destructor, never while a menu is
//  being built.  DirClean's v1.1.1 wrote its whole list from the COM
//  object's destructor, i.e. on every right-click, and two Explorer
//  windows then overwrote each other constantly
//  (docs/porting-notes.md).  An MRU would hit that every single day.
//
//  The read-modify-write reads the CURRENT registry, not a copy cached
//  when the handler was constructed.  That is the actual fix: a
//  right-click that happened ten minutes ago must not be able to
//  resurrect the list as it was ten minutes ago.
//
//  Exactly one value is written.  Everything else in the key is either
//  untouched or deleted by the eviction rules above.
//
//////////////////////////////////////////////////////////////////////////

BOOL DestPromote ( LPCTSTR szFolder )
{
CString sCanon;

    // Resolve against the disk.  If we cannot, we store nothing: an
    // entry we cannot name is one we would offer again and fail again.
    if ( !DestCanonicalizeOnDisk ( szFolder, sCanon ) )
        return FALSE;

CStoreLock lock;                    // best effort; see the class comment
CRegKey    key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, TRUE ) )
        return FALSE;

CDestRowVec aRows;

    EnumDestRows ( key, aRows );

CString sName;
int     nFound = FindRow ( aRows, key, sCanon, sName );
CMoveDest dst;

    if ( nFound >= 0 )
        {
        // Keep kind, pin order and the custom label.  Promoting a
        // PINNED folder refreshes its stamp but does NOT demote it to
        // the recents pool -- otherwise using a pin would unpin it.
        dst = aRows[nFound].dst;
        }

    dst.SetPath ( sCanon );         // adopt the freshly resolved casing
    dst.SetMisses ( 0 );

ULONGLONG uStamp = DestNextStamp ( aRows );

    dst.SetStamp ( uStamp );

    if ( ERROR_SUCCESS != key.SetStringValue ( sName, dst.ToStorageString() ) )
        return FALSE;

    // Skip eviction if we never got the lock: a stale extra entry is
    // harmless and self-corrects on the next move, whereas deleting
    // from a list we might be reading half of is not.
    if ( lock.IsHeld() )
        {
        EvictRecents ( key, aRows, sName, uStamp,
                       ( dkRecent == dst.GetKind() ) );
        }

    return TRUE;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestSetPinned()
//
// Description:
//  Moves an entry between the two pools.  One value is rewritten; no
//  entry ever exists in both pools or in neither, because "which pool"
//  is a field inside the value rather than which subkey the value lives
//  under.  Splitting pins and recents into two keys would make this a
//  delete-plus-create, and a crash or a concurrent writer between the
//  two halves would lose the entry or duplicate it.
//
//  uPinOrder is sparse (the caller spaces pins 16 apart) so inserting a
//  pin between two others rewrites ONE value instead of renumbering the
//  whole list -- the same reason the recency order is a stamp and not
//  an index.
//
//////////////////////////////////////////////////////////////////////////

BOOL DestSetPinned ( LPCTSTR szFolder, BOOL bPin, UINT uPinOrder )
{
CString sCanon;

    if ( !DestCanonicalizeLexical ( szFolder, sCanon ) )
        return FALSE;

CStoreLock lock;
CRegKey    key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, TRUE ) )
        return FALSE;

CDestRowVec aRows;

    EnumDestRows ( key, aRows );

CString sName;
int     nFound = FindRow ( aRows, key, sCanon, sName );
CMoveDest dst;

    if ( nFound >= 0 )
        dst = aRows[nFound].dst;
    else
        dst.SetPath ( sCanon );

    if ( bPin )
        {
        size_t nPins = 0;

        for ( size_t i = 0; i < aRows.size(); i++ )
            {
            if ( dkPinned == aRows[i].dst.GetKind()  &&
                 (int) i != nFound )
                {
                nPins++;
                }
            }

        if ( nPins >= (size_t) dcMaxPins )
            return FALSE;           // caller reports "too many pins"

        dst.SetKind ( dkPinned );
        dst.SetPinOrder ( uPinOrder & 0xFFFF );
        }
    else
        {
        dst.SetKind ( dkRecent );
        dst.SetPinOrder ( 0 );

        // A freshly unpinned folder gets a current stamp, so it lands
        // at the top of the recents rather than being evicted before
        // the user can blink.
        dst.SetStamp ( DestNextStamp ( aRows ) );
        }

    dst.SetPath ( sCanon );

    return ( ERROR_SUCCESS == key.SetStringValue ( sName,
                                                   dst.ToStorageString() ) );
}


BOOL DestSetLabel ( LPCTSTR szFolder, LPCTSTR szLabel, CString& sError )
{
CString sLabel ( szLabel );

    sLabel.Trim();

    if ( sLabel.GetLength() > 64 )
        {
        sError = _T("That name is too long.");
        return FALSE;
        }

    // '|' is the field separator in the stored value.  We parse the
    // path from the right so a label containing one could not actually
    // corrupt anything -- but shipping two plausible readings of one
    // string is how DirClean's ';' problem started, so refuse it.
    if ( -1 != sLabel.Find ( _T('|') ) )
        {
        sError = _T("A name cannot contain '|'.");
        return FALSE;
        }

    for ( int i = 0; i < sLabel.GetLength(); i++ )
        {
        if ( sLabel[i] < _T(' ') )
            {
            sError = _T("That name contains a control character.");
            return FALSE;
            }
        }

CString sCanon;

    if ( !DestCanonicalizeLexical ( szFolder, sCanon ) )
        {
        sError = _T("That is not a usable folder path.");
        return FALSE;
        }

CStoreLock lock;
CRegKey    key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, TRUE ) )
        {
        sError = _T("The MoveTo settings could not be opened.");
        return FALSE;
        }

CDestRowVec aRows;

    EnumDestRows ( key, aRows );

CString sName;
int     nFound = FindRow ( aRows, key, sCanon, sName );
CMoveDest dst;

    if ( nFound >= 0 )
        dst = aRows[nFound].dst;
    else
        dst.SetStamp ( DestNextStamp ( aRows ) );

    dst.SetPath ( sCanon );
    dst.SetLabel ( sLabel );

    return ( ERROR_SUCCESS == key.SetStringValue ( sName,
                                                   dst.ToStorageString() ) );
}


BOOL DestForget ( LPCTSTR szFolder )
{
CString sCanon;

    if ( !DestCanonicalizeLexical ( szFolder, sCanon ) )
        return FALSE;

CStoreLock lock;
CRegKey    key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, TRUE ) )
        return FALSE;

CDestRowVec aRows;

    EnumDestRows ( key, aRows );

CString sName;

    if ( FindRow ( aRows, key, sCanon, sName ) < 0 )
        return FALSE;

    return ( ERROR_SUCCESS == key.DeleteValue ( sName ) );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestNoteMiss()
//
// Description:
//  Called when a move failed because the destination was not there.
//
//  Two strikes, not one, and pins are never dropped.  A USB stick
//  unplugged for an afternoon, a VPN that dropped, a OneDrive folder
//  mid-resync: every one of those looks exactly like a deleted folder
//  for a while.  Deleting on the first miss means an unplugged drive
//  silently empties the user's history.
//
//////////////////////////////////////////////////////////////////////////

void DestNoteMiss ( LPCTSTR szFolder )
{
CString sCanon;

    if ( !DestCanonicalizeLexical ( szFolder, sCanon ) )
        return;

CStoreLock lock;
CRegKey    key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, TRUE ) )
        return;

CDestRowVec aRows;

    EnumDestRows ( key, aRows );

CString sName;
int     nFound = FindRow ( aRows, key, sCanon, sName );

    if ( nFound < 0 )
        return;

CMoveDest dst = aRows[nFound].dst;

    dst.SetMisses ( dst.GetMisses() + 1 );

    if ( dkRecent == dst.GetKind()  &&
         dst.GetMisses() >= (UINT) dcMaxMisses )
        {
        key.DeleteValue ( sName );
        return;
        }

    key.SetStringValue ( sName, dst.ToStorageString() );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    DestSweep()
//
// Description:
//  Prunes entries whose folder is gone.
//
//  WHEN, and why it is not anywhere else:
//
//   * NOT in QueryContextMenu.  That is the whole point.  A menu build
//     is a few milliseconds and must stay that way; GetFileAttributes
//     against a mapped drive whose server is off can block for the full
//     SMB timeout, and with twelve entries the user gets a frozen
//     Explorer instead of a menu.
//
//   * NOT in the handler's constructor or destructor either.  Those run
//     on every right-click too -- they are the same budget wearing a
//     different hat, and the destructor is exactly where DirClean's
//     concurrency bug lived.
//
//   * HERE: on the invoke path, after SHFileOperation has returned.
//     The user has already waited for a file operation, Explorer is not
//     waiting to draw anything, and we are already doing disk I/O.
//
//   * And in the manage-destinations dialog, which is modal and
//     user-initiated, where a full sweep with a wait cursor is fine.
//
//  Even here the work is bounded twice: a round-robin cursor so no one
//  move checks everything, and a wall-clock budget so a slow volume
//  ends the sweep rather than extending it.  Remote paths are skipped
//  unless the caller asks for them.
//
//////////////////////////////////////////////////////////////////////////

static BOOL LooksRemote ( const CString& sPath )
{
    if ( ::PathIsUNC ( sPath ) )
        return TRUE;

    if ( sPath.GetLength() >= 3  &&  _T(':') == sPath[1] )
        {
        CString sRoot = sPath.Left ( 3 );

        // GetDriveType consults the local mount table; it does not talk
        // to the server.  Safe here, and we are off the menu path
        // anyway.
        UINT uType = ::GetDriveType ( sRoot );

        if ( DRIVE_REMOTE == uType  ||  DRIVE_REMOVABLE == uType  ||
             DRIVE_CDROM  == uType  ||  DRIVE_NO_ROOT_DIR == uType )
            {
            return TRUE;
            }
        }

    return FALSE;
}


void DestSweep ( DWORD dwBudgetMs, BOOL bIncludeRemote )
{
CWinApp* pApp = AfxGetApp();

    if ( NULL == pApp )
        return;

CStoreLock lock;
CRegKey    key;

    if ( ERROR_SUCCESS != OpenDestKey ( key, TRUE ) )
        return;

CDestRowVec aRows;

    EnumDestRows ( key, aRows );

    if ( aRows.empty() )
        return;

    std::sort ( aRows.begin(), aRows.end(), RecentNewerFirst );

UINT uCursor = pApp->GetProfileInt ( g_szOptSection, g_szValSweepPos, 0 );

    if ( uCursor >= (UINT) aRows.size() )
        uCursor = 0;

ULONGLONG uStart   = ::GetTickCount64();
size_t    nChecked = 0;

    for ( size_t nStep = 0; nStep < aRows.size(); nStep++ )
        {
        if ( ::GetTickCount64() - uStart >= (ULONGLONG) dwBudgetMs )
            break;

        if ( nChecked >= 8 )
            break;

        size_t i = ( uCursor + nStep ) % aRows.size();

        if ( !bIncludeRemote  &&  LooksRemote ( aRows[i].dst.GetPath() ) )
            continue;

        nChecked++;

        DWORD dwAttrs = ::GetFileAttributes ( aRows[i].dst.GetPath() );
        BOOL  bMiss   = FALSE;

        if ( INVALID_FILE_ATTRIBUTES == dwAttrs )
            {
            DWORD dwErr = ::GetLastError();

            // Fail CLOSED, exactly as FolderSettings.cpp's DcProbe does:
            // "there but we cannot see it" is not "not there".  Access
            // denied, a locked volume or a stalled network leaves the
            // entry alone.
            bMiss = ( ERROR_FILE_NOT_FOUND == dwErr  ||
                      ERROR_PATH_NOT_FOUND == dwErr  ||
                      ERROR_INVALID_NAME   == dwErr  ||
                      ERROR_BAD_NETPATH    == dwErr  ||
                      ERROR_BAD_PATHNAME   == dwErr );
            }
        else if ( 0 == ( dwAttrs & FILE_ATTRIBUTE_DIRECTORY ) )
            {
            bMiss = TRUE;           // a file now stands where a folder was
            }

        CMoveDest dst = aRows[i].dst;

        if ( !bMiss )
            {
            if ( 0 != dst.GetMisses() )
                {
                dst.SetMisses ( 0 );
                key.SetStringValue ( aRows[i].sValueName,
                                     dst.ToStorageString() );
                }
            continue;
            }

        dst.SetMisses ( dst.GetMisses() + 1 );

        if ( dkRecent == dst.GetKind()  &&
             dst.GetMisses() >= (UINT) dcMaxMisses )
            {
            key.DeleteValue ( aRows[i].sValueName );
            }
        else
            {
            key.SetStringValue ( aRows[i].sValueName,
                                 dst.ToStorageString() );
            }
        }

    pApp->WriteProfileInt ( g_szOptSection, g_szValSweepPos,
                            (int) ( ( uCursor + nChecked ) % aRows.size() ) );
}


#ifdef _DEBUG

//////////////////////////////////////////////////////////////////////////
//
// Function:    DestSelfTest()
//
// Description:
//  Asserts the contracts that, if they regress, corrupt the user's
//  history silently.  Called once from the handler's ctor, like
//  PatternSelfTest.
//
//////////////////////////////////////////////////////////////////////////

void DestSelfTest()
{
CString s;

    // ---- lexical canonicalisation ---------------------------------
    ASSERT (  DestCanonicalizeLexical ( _T("C:\\Users\\me\\Archive\\"), s ) );
    ASSERT ( _T("C:\\Users\\me\\Archive") == s );

    ASSERT (  DestCanonicalizeLexical ( _T("C:/Users//me/./Archive"), s ) );
    ASSERT ( _T("C:\\Users\\me\\Archive") == s );

    ASSERT (  DestCanonicalizeLexical ( _T("C:\\Users\\x\\..\\me"), s ) );
    ASSERT ( _T("C:\\Users\\me") == s );

    // A bare drive root keeps its backslash: "C:" alone is the process's
    // current directory on C:, not the root.
    ASSERT (  DestCanonicalizeLexical ( _T("C:\\"), s ) );
    ASSERT ( _T("C:\\") == s );

    ASSERT (  DestCanonicalizeLexical ( _T("\\\\?\\C:\\tmp"), s ) );
    ASSERT ( _T("C:\\tmp") == s );

    ASSERT (  DestCanonicalizeLexical ( _T("\\\\?\\UNC\\srv\\share\\x"), s ) );
    ASSERT ( _T("\\\\srv\\share\\x") == s );

    ASSERT ( !DestCanonicalizeLexical ( _T("relative\\path"), s ) );
    ASSERT ( !DestCanonicalizeLexical ( _T("C:\\tmp\\*"),     s ) );
    ASSERT ( !DestCanonicalizeLexical ( _T("\\\\.\\PhysicalDrive0"), s ) );
    ASSERT ( !DestCanonicalizeLexical ( _T(""), s ) );

    // ---- identity --------------------------------------------------
    ASSERT (  DestSameFolder ( _T("C:\\Temp"), _T("c:\\tEmP") ) );
    ASSERT ( !DestSameFolder ( _T("C:\\Temp"), _T("C:\\Temp2") ) );

    // The value name must not depend on casing, or one folder gets two
    // entries the moment Explorer hands us a different spelling.
    ASSERT ( DestValueName ( _T("C:\\Temp") ) ==
             DestValueName ( _T("c:\\TEMP") ) );
    ASSERT ( DestValueName ( _T("C:\\Temp") ) !=
             DestValueName ( _T("C:\\Temp2") ) );

    // ---- storage round trip ---------------------------------------
    {
    CMoveDest a;

    a.SetPath ( _T("C:\\Users\\me\\Archive") );
    a.SetLabel ( _T("Archive") );
    a.SetKind ( dkPinned );
    a.SetStamp ( 0x01DAB0C0FFEE1234ULL );
    a.SetPinOrder ( 0x20 );
    a.SetMisses ( 1 );

    CString sStored = a.ToStorageString();

    ASSERT ( g_nFixedLen < sStored.GetLength() );
    ASSERT ( _T('1') == sStored[0] && _T('|') == sStored[1] );
    ASSERT ( _T('P') == sStored[2] );

    CMoveDest b;

    ASSERT ( CMoveDest::FromStorageString ( sStored, b ) );
    ASSERT ( b.GetPath()     == a.GetPath() );
    ASSERT ( b.GetLabel()    == a.GetLabel() );
    ASSERT ( b.GetKind()     == a.GetKind() );
    ASSERT ( b.GetStamp()    == a.GetStamp() );
    ASSERT ( b.GetPinOrder() == a.GetPinOrder() );
    ASSERT ( b.GetMisses()   == a.GetMisses() );
    }

    // An unversioned value is a bare path: the forever-migration.
    {
    CMoveDest b;

    ASSERT ( CMoveDest::FromStorageString ( _T("C:\\Temp"), b ) );
    ASSERT ( _T("C:\\Temp") == b.GetPath() );
    ASSERT ( dkRecent == b.GetKind() );
    ASSERT ( 0 == b.GetStamp() );
    }

    // Corrupt values are DROPPED, not guessed at.
    {
    CMoveDest b;

    ASSERT ( !CMoveDest::FromStorageString ( _T("1|Z|0000000000000000|0000|00||C:\\x"), b ) );
    ASSERT ( !CMoveDest::FromStorageString ( _T("1|R|zzzzzzzzzzzzzzzz|0000|00||C:\\x"), b ) );
    ASSERT ( !CMoveDest::FromStorageString ( _T("1|R|0000000000000000|0000|00|"), b ) );
    }

    // ---- label disambiguation --------------------------------------
    {
    CDestVec a;
    CMoveDest d;

    d.SetPath ( _T("C:\\work\\proj-a\\build") );  a.push_back ( d );
    d.SetPath ( _T("C:\\work\\proj-b\\build") );  a.push_back ( d );
    d.SetPath ( _T("D:\\Downloads") );            a.push_back ( d );

    DestVecComputeLabels ( a );

    ASSERT ( a[0].GetDisplay() != a[1].GetDisplay() );
    ASSERT ( -1 != a[0].GetDisplay().Find ( _T("proj-a") ) );
    ASSERT ( -1 != a[1].GetDisplay().Find ( _T("proj-b") ) );
    ASSERT ( _T("Downloads") == a[2].GetDisplay() );
    }

    // '&' must survive into the menu as a literal.
    {
    CDestVec a;
    CMoveDest d;

    d.SetPath ( _T("C:\\R&D") );
    a.push_back ( d );
    DestVecComputeLabels ( a );

    ASSERT ( _T("R&&D") == a[0].GetDisplay() );
    }
}

#endif // _DEBUG
