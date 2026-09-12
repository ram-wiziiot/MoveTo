//////////////////////////////////////////////////////////////////////
//
// MoveDest.h : the destination store -- recent and pinned folders.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////
//
// THIS HEADER IS THE SINGLE OWNER of DestKind and CMoveDest.  No other
// header may declare either -- see Pattern.h for why DirClean insists
// on that (four numberings of one enum, and file wildcards silently
// became folder wildcards).
//
//////////////////////////////////////////////////////////////////////
//
// WHY THE STORE LOOKS LIKE THIS
//
// Every right-click creates a handler instance, and two Explorer
// windows overlap constantly.  DirClean was burned by exactly this:
// v1.1.1 rewrote its whole settings list from the COM object's
// destructor, so one window's stale in-memory copy clobbered an edit
// just made in the other (docs/porting-notes.md, "Settings written from
// the destructor").  A move-to MRU is worse, because the write is not a
// rare Options-dialog event -- it happens on every move.
//
// Three rules follow, and they are the whole design:
//
//  1. THE MENU PATH NEVER WRITES.  DestVecLoad() opens one key with
//     KEY_READ, enumerates, sorts in memory and returns.  No registry
//     write, no file system call, no mutex.  Over-capacity lists are
//     trimmed in the returned vector, never in the registry.
//
//  2. ONE FOLDER, ONE REGISTRY VALUE, AND THE VALUE NAME IS DERIVED
//     FROM THE FOLDER.  There is no Recent0/Recent1/... index, so there
//     is no index to alias.  Two windows moving to two different
//     folders touch two different values and cannot interact at all.
//     Two windows moving to the SAME folder compute the same value name
//     and write near-identical content, so last-writer-wins is correct
//     rather than merely survivable.  This is the same trick as
//     Pattern.cpp's PatternVecSaveToRegistry: weld the discriminator
//     into the value instead of spreading state across parallel
//     indexed values.
//
//  3. ORDER LIVES INSIDE THE VALUE, NOT IN ITS NAME.  Each entry
//     carries a 64-bit FILETIME stamp; "most recent first" is a sort at
//     load time.  Promoting an entry rewrites ONE value.  Nothing has
//     to be renumbered, so there is no window in which a crash or a
//     concurrent writer leaves two entries claiming position 0.
//
// The stored value string is:
//
//      1|R|0123456789ABCDEF|0000|00|My label|C:\Users\me\Archive
//      ^ ^ ^                ^    ^  ^        ^
//      | | |                |    |  |        +-- path (never holds '|')
//      | | |                |    |  +----------- label (may be empty)
//      | | |                |    +-------------- consecutive misses, 2 hex
//      | | |                +------------------- pin order, 4 hex
//      | | +------------------------------------ stamp, 16 hex FILETIME
//      | +-------------------------------------- R = recent, P = pinned
//      +---------------------------------------- schema version
//
// The first 29 characters are fixed-width, so parsing is by index and a
// stray separator cannot shift a field.  '|' is illegal in a Windows
// path, so the path is recovered with ReverseFind('|') -- which means
// even a hand-edited label containing '|' cannot corrupt the path.  A
// string with no "1|" prefix is read as a bare path (recent, stamp 0):
// that is the free forever-migration, exactly as Pattern.cpp treats an
// unprefixed value as ptFiles.
//
//////////////////////////////////////////////////////////////////////

#ifndef __MOVEDEST_H__
#define __MOVEDEST_H__

#pragma once

#include <vector>

//////////////////////////////////////////////////////////////////////
// Which pool an entry is in.
//
// Persisted as a letter, not a digit, so a corrupt value cannot be read
// as a plausible-looking neighbouring state.  One entry is in exactly
// one pool: pinning MOVES an entry out of recents rather than copying
// it, so a pin can never be evicted by recency and unpinning cannot
// resurrect a stale duplicate.
//////////////////////////////////////////////////////////////////////

enum DestKind
{
    dkRecent = 0,
    dkPinned = 1
};

//////////////////////////////////////////////////////////////////////
// Tunables.  All of these are clamped on read: a corrupt or hostile
// registry must not be able to make QueryContextMenu slow.
//////////////////////////////////////////////////////////////////////

enum
{
    dcMinRecent      = 4,       // capacity floor
    dcMaxRecent      = 40,      // capacity ceiling
    dcDefaultRecent  = 12,      // default MRU capacity

    dcMaxPins        = 24,      // refuse the 25th pin; never auto-drop one
    dcMaxValues      = 256,     // hard stop when enumerating the key
    dcMaxMisses      = 2,       // strikes before a RECENT is dropped

    dcMaxLabelChars  = 48,      // menu text budget, before '&' escaping
    dcMaxDisambig    = 3        // path components used to disambiguate
};

//////////////////////////////////////////////////////////////////////
// One destination folder.
//
// Held in std::vector, so it needs correct copy semantics -- it has
// them, via the compiler-generated copy ctor and operator= over
// CStrings and PODs.
//////////////////////////////////////////////////////////////////////

class CMoveDest
{
public:
    CMoveDest()
        : m_eKind(dkRecent), m_uStamp(0), m_uPinOrder(0), m_uMisses(0) {}

    // The canonical path: absolute, no trailing backslash except on a
    // drive root ("C:\"), in the casing the file system reported.
    const CString& GetPath() const      { return m_sPath; }
    void SetPath ( LPCTSTR sz )         { m_sPath = sz; }

    // User-supplied override.  Empty means "derive from the path".
    const CString& GetLabel() const     { return m_sLabel; }
    void SetLabel ( LPCTSTR sz )        { m_sLabel = sz; }

    DestKind  GetKind() const           { return m_eKind; }
    void      SetKind ( DestKind e )    { m_eKind = e; }

    ULONGLONG GetStamp() const          { return m_uStamp; }
    void      SetStamp ( ULONGLONG u )  { m_uStamp = u; }

    UINT      GetPinOrder() const       { return m_uPinOrder; }
    void      SetPinOrder ( UINT u )    { m_uPinOrder = u; }

    UINT      GetMisses() const         { return m_uMisses; }
    void      SetMisses ( UINT u )      { m_uMisses = u; }

    // What the menu actually shows.  NOT persisted: it depends on the
    // other entries in the list, so it is recomputed by
    // DestVecComputeLabels() every time the list is loaded.
    const CString& GetDisplay() const   { return m_sDisplay; }
    void SetDisplay ( LPCTSTR sz )      { m_sDisplay = sz; }

    // Round-trips through ONE registry value.  See the header comment.
    CString ToStorageString() const;
    static BOOL FromStorageString ( LPCTSTR szStored, CMoveDest& dstOut );

protected:
    CString   m_sPath;
    CString   m_sLabel;
    CString   m_sDisplay;
    DestKind  m_eKind;
    ULONGLONG m_uStamp;
    UINT      m_uPinOrder;
    UINT      m_uMisses;
};

typedef std::vector<CMoveDest> CDestVec;

//////////////////////////////////////////////////////////////////////
// Canonicalisation and identity
//
// TWO functions, because they have wildly different costs and the
// expensive one must never run while a context menu is being built.
//////////////////////////////////////////////////////////////////////

// Syntax only: rejects relative, device and wildcard paths, collapses
// "." / ".." / duplicate separators, normalises the trailing backslash.
// Touches no disk and cannot block.  Safe anywhere.
BOOL DestCanonicalizeLexical ( LPCTSTR szIn, CString& sOut );

// Syntax + one directory handle.  GetFinalPathNameByHandleW collapses
// 8.3 short names, SUBST drives, mapped network drives, junctions,
// symlinks and volume mount points onto the real path, and reports the
// on-disk casing.  That one call is the entire dedup story beyond
// syntax -- see the .cpp for where the line is drawn and why.
//
// COSTS A ROUND TRIP ON A NETWORK PATH.  Call it at move time only.
BOOL DestCanonicalizeOnDisk ( LPCTSTR szIn, CString& sOut );

// Are these two canonical paths the same folder?  Ordinal, case
// insensitive -- deliberately NOT lstrcmpi, which is locale sensitive
// (see the .cpp).
BOOL DestSameFolder ( LPCTSTR szA, LPCTSTR szB );

// The invariant-uppercase dedup key for a canonical path.
CString DestMakeKey ( LPCTSTR szCanonicalPath );

// The registry value name for a canonical path: "D" + 16 hex digits of
// a 64-bit hash of the key.  Stable, bounded, and identical in every
// process, which is what makes concurrent promotion of the same folder
// a no-op instead of a duplicate.
CString DestValueName ( LPCTSTR szCanonicalPath );

//////////////////////////////////////////////////////////////////////
// Load  (the context-menu path -- read only, no file system, no lock)
//////////////////////////////////////////////////////////////////////

// Fills aOut with pins first (by pin order), then recents (newest
// first), trims recents to capacity IN MEMORY, and computes display
// labels.  Never writes.  Never touches the disk.
void DestVecLoad ( CDestVec& aOut );

// Recomputes GetDisplay() for every entry so that no two entries in the
// list show the same text.  Pure function of the vector; no I/O.
void DestVecComputeLabels ( CDestVec& aVec );

//////////////////////////////////////////////////////////////////////
// Mutate  (the invoke path -- read-modify-write, serialised)
//////////////////////////////////////////////////////////////////////

// Record a successful move to szFolder.  Resolves the folder on disk,
// creates or refreshes its single value, and evicts over-capacity
// recents.  Returns FALSE if the folder could not be resolved -- in
// which case NOTHING is stored, because a destination we cannot name is
// a destination we must not offer again.
BOOL DestPromote ( LPCTSTR szFolder );

// Pin or unpin.  Pinning takes the entry out of the recents pool;
// unpinning puts it back with a fresh stamp so it is not instantly
// evicted.  Refuses to exceed dcMaxPins.
BOOL DestSetPinned ( LPCTSTR szFolder, BOOL bPin, UINT uPinOrder );

// Set or clear the custom label.  '|' and control characters are
// refused, mirroring Pattern.cpp's refusal of ';'.
BOOL DestSetLabel ( LPCTSTR szFolder, LPCTSTR szLabel, CString& sError );

// Remove one entry outright (the "Forget this folder" command).
BOOL DestForget ( LPCTSTR szFolder );

// Record that a move to this folder failed because it was not there.
// dcMaxMisses consecutive misses drop a RECENT.  A PIN is never
// dropped automatically -- it is user intent, and a disconnected VPN
// must not delete it.
void DestNoteMiss ( LPCTSTR szFolder );

//////////////////////////////////////////////////////////////////////
// Housekeeping  (never on the menu path)
//////////////////////////////////////////////////////////////////////

// Round-robin existence check over a few entries, bounded by a wall
// clock budget.  Call it AFTER the file operation has returned, when
// Explorer is no longer waiting on us to draw a menu.  Remote paths are
// skipped unless bIncludeRemote.
void DestSweep ( DWORD dwBudgetMs, BOOL bIncludeRemote );

//////////////////////////////////////////////////////////////////////
// Capacity
//////////////////////////////////////////////////////////////////////

UINT DestGetCapacity();                 // clamped to [dcMinRecent, dcMaxRecent]
void DestSetCapacity ( UINT uCap );

#ifdef _DEBUG
// Asserts the round trip, the parser's field offsets and the label
// disambiguator.  Called once from the handler's ctor so a regression
// fires on the first right-click, not on a user's history.
void DestSelfTest();
#endif

#endif // __MOVEDEST_H__
