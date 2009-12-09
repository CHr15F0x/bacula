/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2009-2009 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation, which is 
   listed in the file LICENSE.

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

#ifndef MUTEX_LIST_H
#define MUTEX_LIST_H 1

/* 
 * Use this list to manage lock order and protect the Bacula from
 * race conditions and dead locks
 */

#define PRIO_SD_DEV_ACQUIRE   4            /* dev.acquire_mutex */
#define PRIO_SD_DEV_ACCESS    5            /* dev.m_mutex */
#define PRIO_SD_VOL_LIST      10           /* vol_list_lock */
#define PRIO_SD_DEV_SPOOL     14           /* dev.spool_mutex */

#endif
