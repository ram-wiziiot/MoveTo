//////////////////////////////////////////////////////////////////////
//
// ManageDestsDlg.cpp : pin, rename and forget destinations.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "resource.h"
#include "ManageDestsDlg.h"
#include <afxpriv.h>        // WM_KICKIDLE

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

enum { colFolder = 0, colKind, colLabel };


CManageDestsDlg::CManageDestsDlg ( CWnd* pParent )
    : CDialog ( CManageDestsDlg::IDD, pParent )
{
}


void CManageDestsDlg::DoDataExchange ( CDataExchange* pDX )
{
    CDialog::DoDataExchange ( pDX );
    DDX_Control ( pDX, IDC_DEST_LIST, m_list );
}


BEGIN_MESSAGE_MAP(CManageDestsDlg, CDialog)
    ON_BN_CLICKED(IDC_DEST_PIN, OnPin)
    ON_BN_CLICKED(IDC_DEST_UNPIN, OnUnpin)
    ON_BN_CLICKED(IDC_FORGET, OnForget)
    ON_BN_CLICKED(IDC_MOVE_UP, OnMoveUp)
    ON_BN_CLICKED(IDC_MOVE_DOWN, OnMoveDown)
    ON_BN_CLICKED(IDC_RENAME, OnRename)
    ON_NOTIFY(NM_DBLCLK, IDC_DEST_LIST, OnDblclkList)
    ON_NOTIFY(LVN_ENDLABELEDIT, IDC_DEST_LIST, OnEndLabelEdit)
    ON_MESSAGE_VOID(WM_KICKIDLE, OnKickIdle)
    ON_UPDATE_COMMAND_UI(IDC_DEST_PIN, OnUpdatePin)
    ON_UPDATE_COMMAND_UI(IDC_DEST_UNPIN, OnUpdateUnpin)
    ON_UPDATE_COMMAND_UI(IDC_FORGET, OnUpdateSelected)
    ON_UPDATE_COMMAND_UI(IDC_RENAME, OnUpdateSelected)
    ON_UPDATE_COMMAND_UI(IDC_MOVE_UP, OnUpdateMoveUp)
    ON_UPDATE_COMMAND_UI(IDC_MOVE_DOWN, OnUpdateMoveDown)
END_MESSAGE_MAP()


BOOL CManageDestsDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_list.SetExtendedStyle ( m_list.GetExtendedStyle() |
                              LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP |
                              LVS_EX_DOUBLEBUFFER );

    // In-place rename. LVS_EDITLABELS is a window style, not an
    // extended one, so it cannot go in the SetExtendedStyle call above.
    m_list.ModifyStyle ( 0, LVS_EDITLABELS );

    m_list.InsertColumn ( colFolder, _T("Folder"), LVCFMT_LEFT, 330 );
    m_list.InsertColumn ( colKind,   _T("Kind"),   LVCFMT_LEFT,  60 );
    m_list.InsertColumn ( colLabel,  _T("Shown as"), LVCFMT_LEFT, 140 );

    SetDlgItemInt ( IDC_CAPACITY, DestGetCapacity(), FALSE );

    Reload ( 0 );

    return TRUE;
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    CManageDestsDlg::Reload()
//
// Description:
//  Rebuilds the list from the store.  The vector is the model; the
//  control holds only an index into it, never a path.  Reconstructing a
//  path from display text is how a management dialog ends up acting on
//  the wrong row after a sort or an elision.
//
//////////////////////////////////////////////////////////////////////////

void CManageDestsDlg::Reload ( int nSelectIndex )
{
    m_aDests.clear();
    DestVecLoad ( m_aDests );

    m_list.SetRedraw ( FALSE );
    m_list.DeleteAllItems();

    for ( size_t i = 0; i < m_aDests.size(); i++ )
        {
        int nRow = m_list.InsertItem ( (int) i, m_aDests[i].GetPath() );

        if ( -1 == nRow )
            continue;

        m_list.SetItemText ( nRow, colKind,
                             ( dkPinned == m_aDests[i].GetKind() )
                                 ? _T("Pinned") : _T("Recent") );
        m_list.SetItemText ( nRow, colLabel, m_aDests[i].GetLabel() );
        m_list.SetItemData ( nRow, (DWORD_PTR) i );
        }

    m_list.SetRedraw ( TRUE );
    m_list.Invalidate();

    if ( nSelectIndex >= 0 && nSelectIndex < m_list.GetItemCount() )
        {
        m_list.SetItemState ( nSelectIndex, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED );
        m_list.EnsureVisible ( nSelectIndex, FALSE );
        }
}


int CManageDestsDlg::SelectedIndex() const
{
int nRow = m_list.GetNextItem ( -1, LVNI_SELECTED );

    if ( -1 == nRow )
        return -1;

DWORD_PTR dw = m_list.GetItemData ( nRow );

    return ( dw < m_aDests.size() ) ? (int) dw : -1;
}


void CManageDestsDlg::OnPin()
{
int n = SelectedIndex();

    if ( -1 == n || dkPinned == m_aDests[n].GetKind() )
        return;

    // New pins go to the end of the pinned block.
UINT uOrder = 0;

    for ( size_t i = 0; i < m_aDests.size(); i++ )
        {
        if ( dkPinned == m_aDests[i].GetKind() && m_aDests[i].GetPinOrder() >= uOrder )
            uOrder = m_aDests[i].GetPinOrder() + 1;
        }

    if ( !DestSetPinned ( m_aDests[n].GetPath(), TRUE, uOrder ) )
        {
        CString sMsg;

        sMsg.Format ( _T("You can pin up to %u folders. Unpin one first."),
                      (UINT) dcMaxPins );
        AfxMessageBox ( sMsg, MB_ICONINFORMATION );
        return;
        }

    Reload ( 0 );
}


void CManageDestsDlg::OnUnpin()
{
int n = SelectedIndex();

    if ( -1 == n || dkPinned != m_aDests[n].GetKind() )
        return;

    DestSetPinned ( m_aDests[n].GetPath(), FALSE, 0 );
    Reload ( 0 );
}


void CManageDestsDlg::OnForget()
{
int n = SelectedIndex();

    if ( -1 == n )
        return;

CString sMsg;

    sMsg.Format ( _T("Remove this folder from the Move to menu?\n\n    %s\n\n")
                  _T("The folder itself is not touched."),
                  (LPCTSTR) m_aDests[n].GetPath() );

    if ( IDYES != AfxMessageBox ( sMsg, MB_YESNO | MB_ICONQUESTION ) )
        return;

    DestForget ( m_aDests[n].GetPath() );
    Reload ( min ( n, (int) m_aDests.size() - 2 ) );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    CManageDestsDlg::ReorderPins()
//
// Description:
//  Swaps a pinned entry with its pinned neighbour.  Only pins have an
//  order the user controls; recents are ordered by when they were last
//  used, and letting the user drag those around would be a lie the next
//  move would undo.
//
//////////////////////////////////////////////////////////////////////////

void CManageDestsDlg::ReorderPins ( int nIndex, int nDelta )
{
    if ( nIndex < 0 || dkPinned != m_aDests[nIndex].GetKind() )
        return;

int nOther = nIndex + nDelta;

    if ( nOther < 0 || nOther >= (int) m_aDests.size() )
        return;

    if ( dkPinned != m_aDests[nOther].GetKind() )
        return;

UINT uA = m_aDests[nIndex].GetPinOrder();
UINT uB = m_aDests[nOther].GetPinOrder();

    DestSetPinned ( m_aDests[nIndex].GetPath(), TRUE, uB );
    DestSetPinned ( m_aDests[nOther].GetPath(), TRUE, uA );

    Reload ( nOther );
}


void CManageDestsDlg::OnMoveUp()   { ReorderPins ( SelectedIndex(), -1 ); }
void CManageDestsDlg::OnMoveDown() { ReorderPins ( SelectedIndex(),  1 ); }


//////////////////////////////////////////////////////////////////////////
//
// Function:    CManageDestsDlg::OnRename()
//
// Description:
//  Sets the text shown on the menu for this folder.  Uses the list's own
//  in-place label edit rather than a second dialog template.
//
//////////////////////////////////////////////////////////////////////////

void CManageDestsDlg::OnRename()
{
int nRow = m_list.GetNextItem ( -1, LVNI_SELECTED );

    if ( -1 == nRow )
        return;

    // MFC has no input-box primitive, and a second dialog template for
    // one string is not worth it.  The list control's own in-place
    // editor is the least surprising thing available, and it is what
    // Explorer itself uses for renaming.
    m_list.SetFocus();
    m_list.EditLabel ( nRow );
}


//////////////////////////////////////////////////////////////////////////
//
// Function:    CManageDestsDlg::OnEndLabelEdit()
//
// Description:
//  Commits an in-place rename.
//
//  pszText is NULL when the user pressed Escape; committing that would
//  clear the label on a cancel.  Returning FALSE in *pResult tells the
//  control not to write the text into the row itself -- Reload() does
//  that from the store, so the row can never show a label the store
//  rejected.
//
//////////////////////////////////////////////////////////////////////////

void CManageDestsDlg::OnEndLabelEdit ( NMHDR* pNMHDR, LRESULT* pResult )
{
NMLVDISPINFO* pDI = (NMLVDISPINFO*) pNMHDR;

    *pResult = FALSE;

    if ( NULL == pDI->item.pszText )
        return;                         // cancelled

DWORD_PTR dw = m_list.GetItemData ( pDI->item.iItem );

    if ( dw >= m_aDests.size() )
        return;

CString sError;

    if ( !DestSetLabel ( m_aDests[dw].GetPath(), pDI->item.pszText, sError ) )
        {
        AfxMessageBox ( sError.IsEmpty()
                            ? CString ( _T("That name cannot be used.") )
                            : sError,
                        MB_ICONEXCLAMATION );
        return;
        }

    Reload ( pDI->item.iItem );
}


void CManageDestsDlg::OnDblclkList ( NMHDR* pNMHDR, LRESULT* pResult )
{
    UNREFERENCED_PARAMETER(pNMHDR);
    *pResult = 0;

    OnRename();
}


void CManageDestsDlg::OnOK()
{
    // Capacity is the one setting not applied by a button press.
UINT uCap = GetDlgItemInt ( IDC_CAPACITY, NULL, FALSE );

    if ( uCap > 0 )
        DestSetCapacity ( uCap );

    CDialog::OnOK();
}


void CManageDestsDlg::OnKickIdle()
{
    UpdateDialogControls ( this, FALSE );
}

void CManageDestsDlg::OnUpdateSelected ( CCmdUI* pCmdUI )
{
    pCmdUI->Enable ( -1 != SelectedIndex() );
}

void CManageDestsDlg::OnUpdatePin ( CCmdUI* pCmdUI )
{
int n = SelectedIndex();

    pCmdUI->Enable ( -1 != n && dkPinned != m_aDests[n].GetKind() );
}

void CManageDestsDlg::OnUpdateUnpin ( CCmdUI* pCmdUI )
{
int n = SelectedIndex();

    pCmdUI->Enable ( -1 != n && dkPinned == m_aDests[n].GetKind() );
}

void CManageDestsDlg::OnUpdateMoveUp ( CCmdUI* pCmdUI )
{
int n = SelectedIndex();

    pCmdUI->Enable ( n > 0 && dkPinned == m_aDests[n].GetKind() &&
                     dkPinned == m_aDests[n - 1].GetKind() );
}

void CManageDestsDlg::OnUpdateMoveDown ( CCmdUI* pCmdUI )
{
int n = SelectedIndex();

    pCmdUI->Enable ( n >= 0 && n + 1 < (int) m_aDests.size() &&
                     dkPinned == m_aDests[n].GetKind() &&
                     dkPinned == m_aDests[n + 1].GetKind() );
}
