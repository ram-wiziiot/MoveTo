//////////////////////////////////////////////////////////////////////
//
// MoveEngine.h : preflight validation and the move itself.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////
//
// WHY IFileOperation AND NOT SHFileOperation
//
// SHFileOperation with ONE source and a destination that is not an
// existing directory does not fail.  It RENAMES the source to the
// destination's name and returns 0.  Move "report.docx" to "D:\Archive"
// when Archive is not there and you get an extension-less file called
// "Archive" and a success code.
//
// MoveTo's destinations come from a history that accumulates over weeks
// and points at removable drives and network shares, so "the destination
// vanished between drawing the menu and clicking it" is the ordinary
// case here, not an edge case.  DirClean could live with SHFileOperation
// because its preview dialog stood between the user and the operation;
// MoveTo has no such dialog by design -- the whole point is one hover
// and one click.
//
// IFileOperation::MoveItem takes the destination as an IShellItem, which
// must bind before anything is touched.  A missing destination therefore
// fails structurally at SHCreateItemFromParsingName, with nothing
// modified.
//
//////////////////////////////////////////////////////////////////////
//
// TWO CANONICAL FORMS, DELIBERATELY NAMED APART
//
// m_sCanonical is a "\\?\Volume{GUID}\..." identity string used ONLY for
// comparing two paths.  It must never reach the shell or the store:
// SHFileOperation rejects a "\\?\" source outright, and the destination
// store strips the prefix and then fails its own drive/UNC test, which
// makes the entry silently disappear from the user's list.
//
// The store's form (MoveDest.h) is the stripped DOS path, and that is
// the only one that may be displayed, stored, or handed to an API.
//
//////////////////////////////////////////////////////////////////////

#ifndef __MOVEENGINE_H__
#define __MOVEENGINE_H__

#pragma once

#include <vector>

//////////////////////////////////////////////////////////////////////
// Why one source item cannot be moved.
//////////////////////////////////////////////////////////////////////

enum MoveReject
{
    mrOk = 0,
    mrMissing,              // gone between the right-click and the click
    mrIsRoot,               // a drive root or share root; not movable
    mrDestInsideSource,     // moving a folder into its own subtree
    mrAlreadyThere,         // its parent already IS the destination
    mrSameItem,             // the item IS the destination folder
    mrNestedInSelection,    // an ancestor of it is also selected
    mrUnresolvable          // could not obtain a canonical identity
};

//////////////////////////////////////////////////////////////////////
// Why the destination as a whole is unusable.
//
// dvMissing and dvUnreachable are deliberately distinct: a folder that
// is genuinely gone should eventually leave the history, but one that is
// merely unreachable (VPN down, NAS asleep) must not, or the first
// disconnected morning deletes the user's pinned network destinations.
//////////////////////////////////////////////////////////////////////

enum DestValidity
{
    dvOk = 0,
    dvMissing,              // resolved, and it is not there
    dvNotAFolder,           // it exists but it is a file
    dvUnreachable,          // network or device error; say nothing, drop nothing
    dvTooLong               // >= MAX_PATH and we cannot express it
};

//////////////////////////////////////////////////////////////////////
// One item in the plan.
//////////////////////////////////////////////////////////////////////

class CMoveItem
{
public:
    CMoveItem() : m_eReject(mrOk), m_bIsDirectory(FALSE) {}

    CString    m_sPath;         // the shell form; what actually gets moved
    CString    m_sCanonical;    // comparison only -- never leaves this file
    MoveReject m_eReject;
    BOOL       m_bIsDirectory;
};

typedef std::vector<CMoveItem> CMoveItemVec;

//////////////////////////////////////////////////////////////////////
// The validated plan.
//////////////////////////////////////////////////////////////////////

class CMovePlan
{
public:
    CMovePlan() : m_eDestValidity(dvOk), m_nMovable(0) {}

    CString      m_sDest;           // shell form, no trailing backslash
    CString      m_sDestCanonical;  // comparison only
    DestValidity m_eDestValidity;
    CMoveItemVec m_aItems;
    UINT         m_nMovable;        // how many have m_eReject == mrOk

    BOOL IsRunnable() const { return ( dvOk == m_eDestValidity ) && ( m_nMovable > 0 ); }
};

//////////////////////////////////////////////////////////////////////
// What actually happened.
//////////////////////////////////////////////////////////////////////

class CMoveOutcome
{
public:
    CMoveOutcome() : m_nMoved(0), m_nFailed(0), m_bAborted(FALSE), m_hr(S_OK) {}

    UINT    m_nMoved;       // items a sink callback confirmed moved
    UINT    m_nFailed;
    BOOL    m_bAborted;     // user cancelled, or the shell stopped early
    HRESULT m_hr;           // from PerformOperations
};

//////////////////////////////////////////////////////////////////////
// Build a plan.  Touches the disk (one handle per item) but changes
// nothing.  Safe to call and discard.
//
// COSTS ONE HANDLE OPEN PER SELECTED ITEM, so it is bounded by the
// caller's selection cap and wrapped in a wait cursor. Do not call it
// from QueryContextMenu.
//////////////////////////////////////////////////////////////////////

void MovePlanBuild ( const CStringList& lsSelection, LPCTSTR szDest,
                     CMovePlan& planOut );

//////////////////////////////////////////////////////////////////////
// Run a plan. Only items with m_eReject == mrOk are queued.
//
// hwndOwner may be NULL; IFileOperation accepts that and parents its
// progress UI itself.
//////////////////////////////////////////////////////////////////////

HRESULT MovePlanExecute ( const CMovePlan& plan, HWND hwndOwner,
                          CMoveOutcome& outcomeOut );

//////////////////////////////////////////////////////////////////////
// Human-readable explanations, for the one message box we may show.
//////////////////////////////////////////////////////////////////////

CString MoveRejectText ( MoveReject eReject );
CString MoveDestValidityText ( DestValidity eValidity, LPCTSTR szDest );

// TRUE if szChild is szParent or sits beneath it. Both must already be
// canonical. Ordinal and case-insensitive, with a component-boundary
// test so "C:\foo" does not appear to contain "C:\foobar".
BOOL MovePathIsUnderOrEqual ( LPCTSTR szChild, LPCTSTR szParent );

#ifdef _DEBUG
void MoveEngineSelfTest();
#endif

#endif // __MOVEENGINE_H__
