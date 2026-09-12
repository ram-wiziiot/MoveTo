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

// stdafx.cpp : source file that includes just the standard includes
//  stdafx.pch will be the pre-compiled header
//  stdafx.obj will contain the pre-compiled type information

#include "stdafx.h"

// ATL 3's statreg.cpp / atlimpl.cpp were removed in ATL 7+ (VS.NET and later);
// their contents now live in the ATL headers themselves.
