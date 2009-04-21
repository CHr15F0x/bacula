/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2002-2009 Free Software Foundation Europe e.V.

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
/*
 * Watchdog timer routines
 *
 *    Kern Sibbald, December MMII
 *
*/

enum {
   TYPE_CHILD = 1,
   TYPE_PTHREAD,
   TYPE_BSOCK
};

#define TIMEOUT_SIGNAL SIGUSR2

struct s_watchdog_t {
        bool one_shot;
        utime_t interval;
        void (*callback)(struct s_watchdog_t *wd);
        void (*destructor)(struct s_watchdog_t *wd);
        void *data;
        /* Private data below - don't touch outside of watchdog.c */
        dlink link;
        utime_t next_fire;
};
typedef struct s_watchdog_t watchdog_t;

/* Exported globals */
extern utime_t DLL_IMP_EXP watchdog_time;             /* this has granularity of SLEEP_TIME */
extern utime_t DLL_IMP_EXP watchdog_sleep_time;      /* examine things every 60 seconds */
