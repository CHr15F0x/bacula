//  Copyright (C) 1999 AT&T Laboratories Cambridge. All Rights Reserved.
//
//  This file was part of the VNC system.
//
//  The VNC system is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
//  USA.
//
// If the source code for the VNC system is not available from the place
// whence you received this file, check http://www.uk.research.att.com/vnc or contact
// the authors on vnc@uk.research.att.com for information on obtaining it.
//
// This file has been adapted to the Win32 version of Bacula
// by Kern E. Sibbald.  Many thanks to ATT and James Weatherall,
// the original author, for providing an excellent template.
//
// Copyright (2000) Kern E. Sibbald
//


// WinUPS header file

#define STRICT 1
#include <windows.h>
#include <stdio.h>
#include <process.h>
#include "winres.h"

#ifdef HAVE_MINGW
#include "compat.h"
#endif

// Application specific messages

// Message used for system tray notifications
#define WM_TRAYNOTIFY                           WM_USER+1

// Messages used for the server object to notify windows of things
#define WM_SRV_CLIENT_CONNECT           WM_USER+2
#define WM_SRV_CLIENT_AUTHENTICATED     WM_USER+3
#define WM_SRV_CLIENT_DISCONNECT        WM_USER+4

// Export the application details
extern HINSTANCE        hAppInstance;
extern const char       *szAppName;
extern DWORD            mainthreadId;

// Main UPS server routine
extern int BaculaAppMain();

extern void LogErrorMsg(char *msg);

// Standard command-line flag definitions
const char BaculaRunService[]            = "/service";
const char BaculaRunServiceHelper[]      = "/servicehelper";
const char BaculaRunAsUserApp[]          = "/run";

const char BaculaInstallService[]        = "/install";
const char BaculaRemoveService[]         = "/remove";

const char BaculaShowAbout[]             = "/about";
const char BaculaShowStatus[]            = "/status";
const char BaculaShowEvents[]            = "/events";
const char BaculaKillRunningCopy[]       = "/kill";

const char BaculaShowHelp[]              = "/help";

// Usage string
const char BaculaUsageText[] = "Bacula [/run] [/kill] [/install] [/remove] [/about] [/status] [/events]\n";

void LogErrorMsg(char *msg, char *fname, int lineno);
#define log_error_message(msg) LogErrorMsg((msg), __FILE__, __LINE__)
