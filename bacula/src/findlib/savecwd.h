/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2007-2010 Free Software Foundation Europe e.V.

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
 *  Kern Sibbald, August MMVII
 *
 */

#ifndef _SAVECWD_H
#define _SAVECWD_H 1

class saveCWD {
   bool m_saved;                   /* set if we should do chdir i.e. save_cwd worked */
   int m_fd;                       /* fd of current dir before chdir */
   char *m_cwd;                    /* cwd before chdir if fd fchdir() works */

public:
   saveCWD() { m_saved=false; m_fd=-1; m_cwd=NULL; };
   ~saveCWD() { release(); };
   bool save(JCR *jcr);
   bool restore(JCR *jcr);
   void release();
   bool is_saved() { return m_saved; };
};

#endif /* _SAVECWD_H */
