//////////////////////////////////////////////////////////////////////
// 
// This utility written and copyright by Michael Dunn (mdunn at inreach
// dot com).  You may freely use and redistribute this source code and
// binary as long as this notice is retained.
//
// Contact me if you have any questions, comments, or bug reports. Get
// the latest updates at http://home.inreach.com/mdunn/code/
//
//////////////////////////////////////////////////////////////////////
// 
// Revision history:
//  Feb 28, 2000: Version 1.0: First release.
//
//  June 5, 2000: Version 1.1: Fixed (un)registration so the DLL works
//      on NT/2000.
//
//  Oct 28, 2001: Version 1.1.1: Added 4 default wildcards, *.ncb, *.aps,
//      *.bsc, *.sbr.
//
//////////////////////////////////////////////////////////////////////

// stdafx.h : include file for standard system include files,
//      or project specific include files that are used frequently,
//      but are changed infrequently

#if !defined(AFX_STDAFX_H__BA06A5A4_BDE3_11D3_BE82_0050DA63C294__INCLUDED_)
#define AFX_STDAFX_H__BA06A5A4_BDE3_11D3_BE82_0050DA63C294__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define STRICT

// Windows 11 / Windows 10 minimum target.
#ifndef WINVER
#define WINVER       0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <SDKDDKVer.h>

#define _ATL_APARTMENT_THREADED
#define _CRT_SECURE_NO_WARNINGS

#include <afxwin.h>
#include <afxdisp.h>
#include <afxcmn.h>

// PathMatchSpecEx / PathIsRoot / PathIsUNCServerShare live here.  Only
// shlwapi.lib was on the link line; the header was included nowhere.
#include <shlwapi.h>

#include <vector>
#include <algorithm>

#include <atlbase.h>
//You may derive a class from CComModule and use it if you want to override
//something, but do not change the name of _Module
extern CComModule _Module;
#include <atlcom.h>

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_STDAFX_H__BA06A5A4_BDE3_11D3_BE82_0050DA63C294__INCLUDED)
