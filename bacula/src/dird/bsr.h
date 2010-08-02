/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2002-2008 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of Kern Sibbald.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
/*
 *
 *   Bootstrap Record header file
 *
 *      BSR (bootstrap record) handling routines split from
 *        ua_restore.c July MMIII
 *
 *     Kern Sibbald, July MMII
 *
 *   Version $Id$
 */



/* FileIndex entry in restore bootstrap record */
struct RBSR_FINDEX {
   RBSR_FINDEX *next;
   int32_t findex;
   int32_t findex2;
};

/*
 * Restore bootstrap record -- not the real one, but useful here
 *  The restore bsr is a chain of BSR records (linked by next).
 *  Each BSR represents a single JobId, and within it, it
 *    contains a linked list of file indexes for that JobId.
 *    The complete_bsr() routine, will then add all the volumes
 *    on which the Job is stored to the BSR.
 */
struct RBSR {
   RBSR *next;                        /* next JobId */
   JobId_t JobId;                     /* JobId this bsr */
   uint32_t VolSessionId;
   uint32_t VolSessionTime;
   int      VolCount;                 /* Volume parameter count */
   VOL_PARAMS *VolParams;             /* Volume, start/end file/blocks */
   RBSR_FINDEX *fi;                   /* File indexes this JobId */
   char *fileregex;                   /* Only restore files matching regex */
};

