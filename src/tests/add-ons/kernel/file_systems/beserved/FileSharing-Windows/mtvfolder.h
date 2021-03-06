#if !defined(AFX_MTVFOLDER_H__60DF2AD0_F62B_11D5_90FA_00C04F0972A7__INCLUDED_)
#define AFX_MTVFOLDER_H__60DF2AD0_F62B_11D5_90FA_00C04F0972A7__INCLUDED_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000
// Machine generated IDispatch wrapper class(es) created by Microsoft Visual C++

// NOTE: Do not modify the contents of this file.  If this class is regenerated by
//  Microsoft Visual C++, your modifications will be overwritten.

/////////////////////////////////////////////////////////////////////////////
// CMTVFolder wrapper class

class CMTVFolder : public COleDispatchDriver
{
public:
	CMTVFolder() {}		// Calls COleDispatchDriver default constructor
	CMTVFolder(LPDISPATCH pDispatch) : COleDispatchDriver(pDispatch) {}
	CMTVFolder(const CMTVFolder& dispatchSrc) : COleDispatchDriver(dispatchSrc) {}

// Attributes
public:

// Operations
public:
	long GetHandle();
	CString GetDisplayName();
	CString GetPathName();
	VARIANT GetFolders();
	CString GetShareName();
	CString GetName();
	BOOL GetCheck();
	void SetCheck(BOOL bNewValue);
	LPUNKNOWN GetShellFolder();
	long GetAttributes(long dwMask);
	BOOL GetAttribute(long anAttribute);
};

//{{AFX_INSERT_LOCATION}}
// Microsoft Visual C++ will insert additional declarations immediately before the previous line.

#endif // !defined(AFX_MTVFOLDER_H__60DF2AD0_F62B_11D5_90FA_00C04F0972A7__INCLUDED_)
