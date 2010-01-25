
#undef  VERSION
#define VERSION "5.0.0"
#define BDATE   "26 January 2010"
#define LSMDATE "26Jan10"

#define PROG_COPYRIGHT "Copyright (C) %d-2010 Free Software Foundation Europe e.V.\n"
#define BYEAR "2010"       /* year for copyright messages in progs */

/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2010 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of Kern Sibbald.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/


/* Debug flags */
#undef  DEBUG
#define DEBUG 1
#define TRACEBACK 1
#define TRACE_FILE 1

/* If this is set stdout will not be closed on startup */
/* #define DEVELOPER 1 */

/*
 * SMCHECK does orphaned buffer checking (memory leaks)
 *  it can always be turned on, but has some minor performance
 *  penalties.
 */
#ifdef DEVELOPER
# define SMCHECK
#endif

/*
 * _USE_LOCKMGR does lock/unlock mutex tracking (dead lock)
 *   it can always be turned on, but we advise to use it only
 *   for debug
 */
#if DEVELOPER
# ifndef _USE_LOCKMGR
#  define _USE_LOCKMGR
# endif /* _USE_LOCKMGR */
/*
 * Enable priority management with the lock manager
 *
 * Note, turning this on will cause the Bacula SD to abort if
 *  mutexes are executed out of order, which could lead to a
 *  deadlock.  However, note that this is not necessarily a
 *  deadlock, so turn this on only for debugging.
 */
#define USE_LOCKMGR_PRIORITY
#endif  /* DEVELOPER */

#if !HAVE_LINUX_OS && !HAVE_SUN_OS && !HAVE_DARWIN_OS && !HAVE_FREEBSD_OS
# undef _USE_LOCKMGR
#endif

/*
 * USE_VTAPE is a dummy tape driver. This is useful to
 *  run regress test.
 */
#ifdef HAVE_LINUX_OS
# define USE_VTAPE
#endif

/* 
 * for fastest speed but you must have a UPS to avoid unwanted shutdowns
 */
//#define SQLITE3_INIT_QUERY "PRAGMA synchronous = OFF"

/*
 * for more safety, but is 30 times slower than above
 */
#define SQLITE3_INIT_QUERY "PRAGMA synchronous = NORMAL"

/*
 * This should always be on. It enables data encryption code 
 *  providing it is configured.
 */
#define DATA_ENCRYPTION 1

/*
 * This uses a Bacula specific bsnprintf rather than the sys lib
 *  version because it is much more secure. It should always be 
 *  on.
 */
#define USE_BSNPRINTF 1

/* Debug flags not normally turned on */

/* #define TRACE_JCR_CHAIN 1 */
/* #define TRACE_RES 1 */
/* #define DEBUG_MEMSET 1 */
/* #define DEBUG_MUTEX 1 */

/*
 * Set SMALLOC_SANITY_CHECK to zero to turn off, otherwise
 *  it is the maximum memory malloced before Bacula will
 *  abort.  Except for debug situations, this should be zero
 */
#define SMALLOC_SANITY_CHECK 0  /* 500000000  0.5 GB max */


/* Check if header of tape block is zero before writing */
/* #define DEBUG_BLOCK_ZEROING 1 */

/* #define FULL_DEBUG 1 */   /* normally on for testing only */

/* Turn this on ONLY if you want all Dmsg() to append to the
 *   trace file. Implemented mainly for Win32 ...
 */
/*  #define SEND_DMSG_TO_FILE 1 */


/* The following are turned on for performance testing */
/*  
 * If you turn on the NO_ATTRIBUTES_TEST and rebuild, the SD
 *  will receive the attributes from the FD, will write them
 *  to disk, then when the data is written to tape, it will
 *  read back the attributes, but they will not be sent to
 *  the Director. So this will eliminate: 1. the comm time
 *  to send the attributes to the Director. 2. the time it
 *  takes the Director to put them in the catalog database.
 */
/* #define NO_ATTRIBUTES_TEST 1 */

/* 
* If you turn on NO_TAPE_WRITE_TEST and rebuild, the SD
*  will do all normal actions, but will not write to the
*  Volume.  Note, this means a lot of functions such as
*  labeling will not work, so you must use it only when 
*  Bacula is going to append to a Volume. This will eliminate
*  the time it takes to write to the Volume (not the time
*  it takes to do any positioning).
*/
/* #define NO_TAPE_WRITE_TEST 1 */

/*
 * If you turn on FD_NO_SEND_TEST and rebuild, the FD will
 *  not send any attributes or data to the SD. This will
 *  eliminate the comm time sending to the SD.
 */
/* #define FD_NO_SEND_TEST 1 */
