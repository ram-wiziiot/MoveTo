//////////////////////////////////////////////////////////////////////
//
// MenuLayout.cpp : the command-id arithmetic for the "Move to" submenu.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "MenuLayout.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif


//////////////////////////////////////////////////////////////////////////
//
// Function:    MenuLayoutCompute()
//
// Description:
//  Decides how many destinations fit, and therefore how many id slots
//  this handler claims.
//
//  Browse and Manage are ALWAYS present, even with no destinations and
//  even in a pathologically small id range.  Browse is the only way to
//  seed an empty history, and Manage is the only way to remove a stored
//  path the user regrets -- an accumulating list of folder paths with no
//  escape hatch is a privacy problem, not just an inconvenience.
//
//  That costs 3 slots minimum (parent + Browse + Manage).  A host that
//  cannot spare 3 gets a layout with zero destinations, which is still
//  coherent; the caller checks GetIdsUsed() against what it was granted
//  before inserting anything.
//
//////////////////////////////////////////////////////////////////////////

CMenuLayout MenuLayoutCompute ( UINT nDestsAvailable, UINT nIdSlotsAvailable )
{
CMenuLayout layout;

    // parent + Browse + Manage, before any destination.
const UINT nFixed = 3;

    layout.m_nDests     = 0;
    layout.m_bTruncated = FALSE;

    if ( nIdSlotsAvailable < nFixed )
        {
        // Not enough room for even the fixed commands.  Report what we
        // would need; the caller refuses to insert anything.
        layout.m_nIdsUsed   = nFixed;
        layout.m_bTruncated = ( nDestsAvailable > 0 );
        return layout;
        }

UINT nRoomForDests = nIdSlotsAvailable - nFixed;
UINT nDests        = nDestsAvailable;

    if ( nDests > mlMaxDests )
        nDests = mlMaxDests;

    if ( nDests > nRoomForDests )
        nDests = nRoomForDests;

    layout.m_nDests     = nDests;
    layout.m_nIdsUsed   = nDests + nFixed;
    layout.m_bTruncated = ( nDests < nDestsAvailable );

    return layout;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    CMenuLayout::Classify()
//
// Description:
//  Turns a command-id offset back into a meaning.  Every offset the
//  shell can hand us passes through here, including ones we never
//  assigned -- another handler's id can reach us if some host gets its
//  own bookkeeping wrong, and the answer for those must be mkNone, not
//  a plausible-looking destination index.
//
//////////////////////////////////////////////////////////////////////////

MenuKind CMenuLayout::Classify ( UINT uOffset ) const
{
    if ( uOffset >= m_nIdsUsed )
        return mkNone;

    if ( 0 == uOffset )
        return mkParent;

    if ( uOffset <= m_nDests )
        return mkDest;

    if ( uOffset == m_nDests + 1 )
        return mkBrowse;

    if ( uOffset == m_nDests + 2 )
        return mkManage;

    return mkNone;
}


#ifdef _DEBUG

//////////////////////////////////////////////////////////////////////////
//
// Function:    MenuLayoutSelfTest()
//
// Description:
//  Asserts the arithmetic that decides where the user's files go.
//
//  The case that matters is the last one: every offset this handler
//  claims must classify to exactly one thing, and the count it reports
//  must cover every offset it assigned.  If those two ever disagree, the
//  shell hands our ids to another extension or vice versa.
//
//////////////////////////////////////////////////////////////////////////

void MenuLayoutSelfTest()
{
    // Ordinary case: 6 destinations, plenty of room.
    {
    CMenuLayout L = MenuLayoutCompute ( 6, 100 );

    ASSERT ( 6 == L.GetDestCount() );
    ASSERT ( 9 == L.GetIdsUsed() );             // 6 + parent + Browse + Manage
    ASSERT ( !L.WasTruncated() );

    ASSERT ( mkParent == L.Classify ( 0 ) );
    ASSERT ( mkDest   == L.Classify ( 1 ) );
    ASSERT ( 0        == L.DestIndexOf ( 1 ) );
    ASSERT ( mkDest   == L.Classify ( 6 ) );
    ASSERT ( 5        == L.DestIndexOf ( 6 ) );
    ASSERT ( mkBrowse == L.Classify ( 7 ) );
    ASSERT ( mkManage == L.Classify ( 8 ) );
    ASSERT ( mkNone   == L.Classify ( 9 ) );    // one past the end
    ASSERT ( mkNone   == L.Classify ( 999 ) );

    ASSERT ( 7 == L.OffsetOfBrowse() );
    ASSERT ( 8 == L.OffsetOfManage() );
    }

    // Empty history: Browse and Manage must still be reachable.
    {
    CMenuLayout L = MenuLayoutCompute ( 0, 100 );

    ASSERT ( 0 == L.GetDestCount() );
    ASSERT ( 3 == L.GetIdsUsed() );
    ASSERT ( mkParent == L.Classify ( 0 ) );
    ASSERT ( mkBrowse == L.Classify ( 1 ) );
    ASSERT ( mkManage == L.Classify ( 2 ) );
    ASSERT ( mkNone   == L.Classify ( 3 ) );
    }

    // Hard cap on destinations.
    {
    CMenuLayout L = MenuLayoutCompute ( 500, 1000 );

    ASSERT ( mlMaxDests == L.GetDestCount() );
    ASSERT ( L.WasTruncated() );
    }

    // Cramped id range: destinations give way, fixed commands do not.
    {
    CMenuLayout L = MenuLayoutCompute ( 10, 5 );

    ASSERT ( 2 == L.GetDestCount() );           // 5 - 3 fixed
    ASSERT ( 5 == L.GetIdsUsed() );
    ASSERT ( L.WasTruncated() );
    ASSERT ( mkDest   == L.Classify ( 2 ) );
    ASSERT ( mkBrowse == L.Classify ( 3 ) );
    ASSERT ( mkManage == L.Classify ( 4 ) );
    ASSERT ( mkNone   == L.Classify ( 5 ) );
    }

    // Range too small for even the fixed commands.
    {
    CMenuLayout L = MenuLayoutCompute ( 10, 2 );

    ASSERT ( 0 == L.GetDestCount() );
    ASSERT ( L.GetIdsUsed() > 2 );              // caller must refuse
    }

    // THE INVARIANT.  Across a wide range of shapes: every offset below
    // GetIdsUsed() classifies to exactly one non-None kind, destination
    // indices are contiguous from zero, and nothing at or past
    // GetIdsUsed() is ours.
    for ( UINT nAvail = 0; nAvail <= 30; nAvail++ )
        {
        for ( UINT nSlots = 0; nSlots <= 40; nSlots++ )
            {
            CMenuLayout L = MenuLayoutCompute ( nAvail, nSlots );

            if ( L.GetIdsUsed() > nSlots )
                continue;                       // caller refuses this one

            ASSERT ( L.GetIdsUsed() == L.GetDestCount() + 3 );

            UINT nSeenDests = 0;

            for ( UINT u = 0; u < L.GetIdsUsed(); u++ )
                {
                MenuKind eKind = L.Classify ( u );

                ASSERT ( mkNone != eKind );

                if ( mkDest == eKind )
                    {
                    ASSERT ( L.DestIndexOf ( u ) == nSeenDests );
                    nSeenDests++;
                    }
                }

            ASSERT ( nSeenDests == L.GetDestCount() );
            ASSERT ( mkNone == L.Classify ( L.GetIdsUsed() ) );
            }
        }
}

#endif // _DEBUG
