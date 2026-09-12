//////////////////////////////////////////////////////////////////////
//
// MoveToShlExt.h : the context menu handler.
//
// MoveTo v1.0.  Plumbing patterns copied from DirClean 2.0 (originally
// by Michael Dunn, CodeProject 2000-2002).  You may freely use and
// redistribute this source code and binary as long as this notice is
// retained.
//
//////////////////////////////////////////////////////////////////////

#ifndef __MOVETOSHLEXT_H_
#define __MOVETOSHLEXT_H_

#include "resource.h"
#include "MoveDest.h"
#include "MenuLayout.h"
#include <comdef.h>

/////////////////////////////////////////////////////////////////////////////
// CMoveToShlExt

class CMoveToShlExt :
    public CComObjectRootEx<CComSingleThreadModel>,
    public CComCoClass<CMoveToShlExt, &CLSID_MoveToShlExt>,
    public IDispatchImpl<IMoveToShlExt, &IID_IMoveToShlExt, &LIBID_MOVETOLib>,
    public IShellExtInit,
    public IContextMenu
{
public:
    CMoveToShlExt();
    ~CMoveToShlExt();

protected:
    // The selection the shell handed us.
    CStringList m_lsItems;

    // TRUE when the selection exceeded the cap and m_lsItems holds only
    // a prefix.  InvokeCommand REFUSES rather than moving part of it --
    // silently moving the first N of a large selection and leaving the
    // rest is the worst possible outcome, because it looks like success.
    BOOL m_bTruncated;

    // Loaded once in the constructor, read-only thereafter.  The menu
    // path never writes; see MoveDest.h.
    CDestVec m_aDests;

    // Computed in QueryContextMenu, read by InvokeCommand and
    // GetCommandString.  All three go through this one object.
    CMenuLayout m_layout;

    HRESULT DoMoveTo ( LPCTSTR szDest, HWND hwndOwner );
    HRESULT DoBrowse ( HWND hwndOwner );
    HRESULT DoManage ( HWND hwndOwner );

    // TRUE if we may proceed; shows the explanation and returns FALSE if
    // the selection was capped.
    BOOL CheckNotTruncated ( HWND hwndOwner );

public:
    // IShellExtInit
    STDMETHOD(Initialize)(LPCITEMIDLIST, LPDATAOBJECT, HKEY);

    // IContextMenu
    STDMETHOD(GetCommandString)(UINT_PTR, UINT, UINT*, LPSTR, UINT);
    STDMETHOD(InvokeCommand)(LPCMINVOKECOMMANDINFO);
    STDMETHOD(QueryContextMenu)(HMENU, UINT, UINT, UINT, UINT);

DECLARE_REGISTRY_RESOURCEID(IDR_MOVETOSHLEXT)
DECLARE_NOT_AGGREGATABLE(CMoveToShlExt)

DECLARE_PROTECT_FINAL_CONSTRUCT()

BEGIN_COM_MAP(CMoveToShlExt)
    COM_INTERFACE_ENTRY(IMoveToShlExt)
    COM_INTERFACE_ENTRY(IDispatch)
    COM_INTERFACE_ENTRY(IShellExtInit)
    COM_INTERFACE_ENTRY(IContextMenu)
END_COM_MAP()

public:
};

// The most items we will accept in one selection.  Above this the
// preflight's per-item handle opens would freeze Explorer's UI thread,
// so we refuse the whole operation with a clear message instead.
enum { mxMaxSelection = 10000 };

#endif //__MOVETOSHLEXT_H_
