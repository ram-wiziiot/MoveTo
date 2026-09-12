//////////////////////////////////////////////////////////////////////
//
// MenuLayout.h : the command-id arithmetic for the "Move to" submenu.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////
//
// WHY THIS IS ITS OWN FILE
//
// Three methods -- QueryContextMenu, InvokeCommand and GetCommandString
// -- must agree exactly on what each command-id offset means.  The
// mapping varies with the number of destinations and with the id range
// the host granted, so duplicating the arithmetic in three places is how
// they drift apart.
//
// Drift here is not a cosmetic bug.  If QueryContextMenu lays out
// [parent, dest0..destN, Browse, Manage] and InvokeCommand reads the
// same offsets as [Browse, Manage, dest0..destN], then clicking
// "Browse for folder..." silently moves the entire selection to the last
// folder in the history, with no dialog and no error.  Both halves
// return S_OK.  Nothing in the shell contract catches it.
//
// So: one pure function computes the layout, one pure function
// interprets an offset, and a _DEBUG self-test asserts both on the first
// right-click.  Nothing else may compute an offset.
//
//////////////////////////////////////////////////////////////////////
//
// THE SHELL'S CONTRACT, WHICH IS EASY TO GET WRONG
//
// The shell merges context-menu handlers serially, computing the next
// handler's base id as:
//
//      idCmdFirst(k+1) = idCmdFirst(k) + HRESULT_CODE(hr(k))
//
// So QueryContextMenu's return value is a CLAIM ON ID SLOTS, not a count
// of visible menu items.  It must be (largest offset used + 1), counting
// every id assigned at any depth -- submenu children included.
//
// Return "1" because you added one top-level popup, and the next handler
// is told it may start at your second id.  It inserts its items over the
// top of your destinations.  USER32 reports the clicked id; the shell
// resolves the owner by searching its (handler, first, count) table and
// finds the OTHER handler's range first, because you only claimed one
// slot.  Clicking "Move to > D:\Archive" then runs whatever that
// extension put there.  This passes on a clean VM and fails on a real
// machine, non-deterministically, by install order.
//
// Separators consume no id.  The popup parent consumes one only because
// we give it MIIM_ID, which we do so that GetCommandString can answer
// for it.
//
//////////////////////////////////////////////////////////////////////

#ifndef __MENULAYOUT_H__
#define __MENULAYOUT_H__

#pragma once

//////////////////////////////////////////////////////////////////////
// What a given command-id offset means.
//////////////////////////////////////////////////////////////////////

enum MenuKind
{
    mkNone = 0,     // not ours, or out of range
    mkParent,       // the "Move to" popup itself; never invocable
    mkDest,         // a destination; see CMenuLayout::DestIndexOf()
    mkBrowse,       // "Browse for folder..."
    mkManage        // "Manage destinations..."
};

//////////////////////////////////////////////////////////////////////
// The computed layout for one QueryContextMenu call.
//
// Offsets, in order:
//
//      0                   the popup parent
//      1 .. nDests         destinations 0 .. nDests-1
//                          (separator here consumes no id)
//      nDests + 1          Browse
//      nDests + 2          Manage
//
//      nIdsUsed = nDests + 3   <-- QueryContextMenu returns this
//
//////////////////////////////////////////////////////////////////////

class CMenuLayout
{
public:
    CMenuLayout() : m_nDests(0), m_nIdsUsed(0), m_bTruncated(FALSE) {}

    UINT GetDestCount() const   { return m_nDests; }
    UINT GetIdsUsed() const     { return m_nIdsUsed; }

    // TRUE when the id range forced us to show fewer destinations than
    // the store offered. The caller says so in the menu rather than
    // silently dropping entries.
    BOOL WasTruncated() const   { return m_bTruncated; }

    UINT OffsetOfParent() const { return 0; }
    UINT OffsetOfDest ( UINT i ) const  { return 1 + i; }
    UINT OffsetOfBrowse() const { return m_nDests + 1; }
    UINT OffsetOfManage() const { return m_nDests + 2; }

    // The only interpreter of an offset in this program.
    MenuKind Classify ( UINT uOffset ) const;

    // Valid only when Classify() returned mkDest.
    UINT DestIndexOf ( UINT uOffset ) const { return uOffset - 1; }

    friend CMenuLayout MenuLayoutCompute ( UINT nDestsAvailable,
                                           UINT nIdSlotsAvailable );

protected:
    UINT m_nDests;
    UINT m_nIdsUsed;
    BOOL m_bTruncated;
};

//////////////////////////////////////////////////////////////////////
// Compute a layout.
//
//  nDestsAvailable  : how many destinations the store offered
//  nIdSlotsAvailable: idCmdLast - idCmdFirst + 1, already reduced by any
//                     spare the caller wants to keep in hand
//
// Pure. No globals, no I/O. Always produces a usable layout: with zero
// destinations you still get Browse and Manage.
//////////////////////////////////////////////////////////////////////

CMenuLayout MenuLayoutCompute ( UINT nDestsAvailable, UINT nIdSlotsAvailable );

// The most destinations we will ever draw, regardless of the id range.
// A menu taller than the screen scrolls, which is miserable to use, and
// this bounds the per-right-click work.
enum { mlMaxDests = 24 };

// Ids we never assign, kept in reserve at the top of the granted range.
// Some hosts have historically passed idCmdLast one past the end, and
// burning the last id there corrupts the next handler's items.
enum { mlSpareIds = 1 };

#ifdef _DEBUG
// Asserts the layout arithmetic and the round trip through Classify().
// Called from the handler's constructor.
void MenuLayoutSelfTest();
#endif

#endif // __MENULAYOUT_H__
