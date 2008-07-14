/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2008 Free Software Foundation Europe e.V.

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
 *  daemon.c by Kern Sibbald 2000
 *
 *   Version $Id$
 *
 *   This code is inspired by the Prentice Hall book
 *   "Unix Network Programming" by W. Richard Stevens
 *   and later updated from his book "Advanced Programming
 *   in the UNIX Environment"
 *
 * Initialize a daemon process completely detaching us from
 * any terminal processes.
 *
 */


#include "bacula.h"
extern int debug_level;

void
daemon_start()
{
#if !defined(HAVE_WIN32)
   int i;
   pid_t cpid;
   mode_t oldmask;
#ifdef DEVELOPER
   int low_fd = 2;
#else
   int low_fd = -1;
#endif
   /*
    *  Become a daemon.
    */

   Dmsg0(900, "Enter daemon_start\n");
   if ( (cpid = fork() ) < 0) {
      berrno be;
      Emsg1(M_ABORT, 0, _("Cannot fork to become daemon: ERR=%s\n"), be.bstrerror());
   } else if (cpid > 0) {
      exit(0);              /* parent exits */
   }
   /* Child continues */

   setsid();

   /* In the PRODUCTION system, we close ALL
    * file descriptors except stdin, stdout, and stderr.
    */
   if (debug_level > 0) {
      low_fd = 2;                     /* don't close debug output */
   }
   for (i=sysconf(_SC_OPEN_MAX)-1; i > low_fd; i--) {
      close(i);
   }

   /* Move to root directory. For debug we stay
    * in current directory so dumps go there.
    */
#ifndef DEBUG
   chdir("/");
#endif

   /*
    * Avoid creating files 666 but don't override any
    * more restrictive mask set by the user.
    */
   oldmask = umask(026);
   oldmask |= 026;
   umask(oldmask);


   /*
    * Make sure we have fd's 0, 1, 2 open
    *  If we don't do this one of our sockets may open
    *  there and if we then use stdout, it could
    *  send total garbage to our socket.
    *
    */
   int fd;
   fd = open("/dev/null", O_RDONLY, 0644);
   if (fd > 2) {
      close(fd);
   } else {
      for(i=1; fd + i <= 2; i++) {
         dup2(fd, fd+i);
      }
   }

#endif /* HAVE_WIN32 */
   Dmsg0(900, "Exit daemon_start\n");
}
