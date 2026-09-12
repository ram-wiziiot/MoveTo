//////////////////////////////////////////////////////////////////////
//
// ManageDestsDlg.h : pin, rename and forget destinations.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////
//
// This dialog is not a convenience.  The destination list accumulates
// folder paths over weeks without ever being asked, and some of those
// paths are things the user would rather not have on a menu that pops up
// in front of whoever is looking at the screen.  Without a way to remove
// one, the feature is a privacy problem.  That is why "Manage
// destinations..." is always present rather than hidden behind
// CMF_EXTENDEDVERBS.
//
// Each button applies immediately -- every store mutation is a single
// registry value write, designed for exactly that (see MoveDest.h).
// There is therefore nothing to cancel, and the close button says
// "Close" rather than pretending otherwise.
//
//////////////////////////////////////////////////////////////////////

#ifndef __MANAGEDESTSDLG_H__
#define __MANAGEDESTSDLG_H__

#pragma once

#include "MoveDest.h"

class CManageDestsDlg : public CDialog
{
public:
    CManageDestsDlg ( CWnd* pParent );

    enum { IDD = IDD_MANAGE_DESTINATIONS };

protected:
    virtual void DoDataExchange ( CDataExchange* pDX );
    virtual BOOL OnInitDialog();
    virtual void OnOK();

    afx_msg void OnPin();
    afx_msg void OnUnpin();
    afx_msg void OnForget();
    afx_msg void OnMoveUp();
    afx_msg void OnMoveDown();
    afx_msg void OnRename();
    afx_msg void OnDblclkList ( NMHDR* pNMHDR, LRESULT* pResult );
    afx_msg void OnEndLabelEdit ( NMHDR* pNMHDR, LRESULT* pResult );
    afx_msg void OnKickIdle();
    afx_msg void OnUpdatePin ( CCmdUI* );
    afx_msg void OnUpdateUnpin ( CCmdUI* );
    afx_msg void OnUpdateSelected ( CCmdUI* );
    afx_msg void OnUpdateMoveUp ( CCmdUI* );
    afx_msg void OnUpdateMoveDown ( CCmdUI* );
    DECLARE_MESSAGE_MAP()

    void Reload ( int nSelectIndex );
    int  SelectedIndex() const;
    void ReorderPins ( int nIndex, int nDelta );

    CListCtrl m_list;
    CDestVec  m_aDests;
};

#endif // __MANAGEDESTSDLG_H__
