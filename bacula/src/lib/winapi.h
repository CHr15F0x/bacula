/*
 * Windows APIs that are different for each system.
 *   We use pointers to the entry points so that a
 *   single binary will run on all Windows systems.
 *
 *     Kern Sibbald MMIII
 */
/*
   Copyright (C) 2000-2005 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

 */

#ifndef __WINAPI_H
#define __WINAPI_H

#if defined(HAVE_WIN32)
/*
 * Commented out native.h include statement, which is not distributed with the
 * free version of VC++, and which is not used in bacula.
 * 
 * #if !defined(HAVE_MINGW) // native.h not present on mingw
 * #include <native.h>
 * #endif
 */
#include <windef.h>
#endif

#if defined(HAVE_CYGWIN) || defined(HAVE_WIN32)

// unicode enabling of win 32 needs some defines and functions
#define MAX_PATH_UNICODE 32767
#define MAX_PATH_UTF8    MAX_PATH*3

int wchar_2_UTF8(char *pszUTF, const WCHAR *pszUCS, int cchChar = MAX_PATH_UTF8);
int UTF8_2_wchar(WCHAR *pszUCS, const char *pszUTF, int cchWideChar = MAX_PATH);


/* In ADVAPI32.DLL */

typedef BOOL (WINAPI * t_OpenProcessToken)(HANDLE, DWORD, PHANDLE);
typedef BOOL (WINAPI * t_AdjustTokenPrivileges)(HANDLE, BOOL,
          PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);
typedef BOOL (WINAPI * t_LookupPrivilegeValue)(LPCTSTR, LPCTSTR, PLUID);

extern t_OpenProcessToken      p_OpenProcessToken;
extern t_AdjustTokenPrivileges p_AdjustTokenPrivileges;
extern t_LookupPrivilegeValue  p_LookupPrivilegeValue;

/* In MSVCRT.DLL */
typedef int (__cdecl * t_wunlink) (const wchar_t *);
typedef int (__cdecl * t_wmkdir) (const wchar_t *);
typedef int (__cdecl * t_wopen)  (const wchar_t *, int, ...);

typedef wchar_t* (__cdecl *t_cgetws) (wchar_t *);
typedef int      (__cdecl *t_cwprintf) (const wchar_t *, ...);

extern t_wunlink   p_wunlink;
extern t_wmkdir    p_wmkdir;
extern t_wopen     p_wopen;

extern t_cgetws    p_cgetws;
extern t_cwprintf  p_cwprintf;
/* In KERNEL32.DLL */
typedef BOOL (WINAPI * t_GetFileAttributesExA)(LPCSTR, GET_FILEEX_INFO_LEVELS,
       LPVOID);
typedef BOOL (WINAPI * t_GetFileAttributesExW)(LPCWSTR, GET_FILEEX_INFO_LEVELS,
       LPVOID);

typedef DWORD (WINAPI * t_GetFileAttributesA)(LPCSTR);
typedef DWORD (WINAPI * t_GetFileAttributesW)(LPCWSTR);
typedef BOOL (WINAPI * t_SetFileAttributesA)(LPCSTR, DWORD);
typedef BOOL (WINAPI * t_SetFileAttributesW)(LPCWSTR, DWORD);

typedef HANDLE (WINAPI * t_CreateFileA) (LPCSTR, DWORD ,DWORD, LPSECURITY_ATTRIBUTES,
        DWORD , DWORD, HANDLE);
typedef HANDLE (WINAPI * t_CreateFileW) (LPCWSTR, DWORD ,DWORD, LPSECURITY_ATTRIBUTES,
        DWORD , DWORD, HANDLE);

typedef BOOL (WINAPI * t_SetProcessShutdownParameters)(DWORD, DWORD);
typedef BOOL (WINAPI * t_BackupRead)(HANDLE,LPBYTE,DWORD,LPDWORD,BOOL,BOOL,LPVOID*);
typedef BOOL (WINAPI * t_BackupWrite)(HANDLE,LPBYTE,DWORD,LPDWORD,BOOL,BOOL,LPVOID*);

typedef int (WINAPI * t_WideCharToMultiByte) (UINT CodePage, DWORD , LPCWSTR, int,
                                              LPSTR, int, LPCSTR, LPBOOL);

typedef int (WINAPI * t_MultiByteToWideChar) (UINT, DWORD, LPCSTR, int, LPWSTR, int);
typedef HANDLE (WINAPI * t_FindFirstFileA) (LPCSTR, LPWIN32_FIND_DATAA);
typedef HANDLE (WINAPI * t_FindFirstFileW) (LPCWSTR, LPWIN32_FIND_DATAW);

typedef BOOL (WINAPI * t_FindNextFileA) (HANDLE, LPWIN32_FIND_DATAA);
typedef BOOL (WINAPI * t_FindNextFileW) (HANDLE, LPWIN32_FIND_DATAW);

typedef BOOL (WINAPI * t_SetCurrentDirectoryA) (LPCSTR);
typedef BOOL (WINAPI * t_SetCurrentDirectoryW) (LPCWSTR);

typedef DWORD (WINAPI * t_GetCurrentDirectoryA) (DWORD, LPSTR);
typedef DWORD (WINAPI * t_GetCurrentDirectoryW) (DWORD, LPWSTR);

extern t_GetFileAttributesA   p_GetFileAttributesA;
extern t_GetFileAttributesW   p_GetFileAttributesW;

extern t_GetFileAttributesExA   p_GetFileAttributesExA;
extern t_GetFileAttributesExW   p_GetFileAttributesExW;

extern t_SetFileAttributesA   p_SetFileAttributesA;
extern t_SetFileAttributesW   p_SetFileAttributesW;

extern t_CreateFileA   p_CreateFileA;
extern t_CreateFileW   p_CreateFileW;
extern t_SetProcessShutdownParameters p_SetProcessShutdownParameters;
extern t_BackupRead         p_BackupRead;
extern t_BackupWrite        p_BackupWrite;

extern t_WideCharToMultiByte p_WideCharToMultiByte;
extern t_MultiByteToWideChar p_MultiByteToWideChar;

extern t_FindFirstFileA p_FindFirstFileA;
extern t_FindFirstFileW p_FindFirstFileW;

extern t_FindNextFileA p_FindNextFileA;
extern t_FindNextFileW p_FindNextFileW;

extern t_SetCurrentDirectoryA p_SetCurrentDirectoryA;
extern t_SetCurrentDirectoryW p_SetCurrentDirectoryW;

extern t_GetCurrentDirectoryA p_GetCurrentDirectoryA;
extern t_GetCurrentDirectoryW p_GetCurrentDirectoryW;

#ifdef WIN32_VSS
class VSSClient;
extern VSSClient g_VSSClient;
#endif

void InitWinAPIWrapper();
#endif

#endif /* __WINAPI_H */
