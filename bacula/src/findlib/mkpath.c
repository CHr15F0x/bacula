/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2007-2008 Free Software Foundation Europe e.V.

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
 *  Kern Sibbald, September MMVII
 * 
 *  This is tricky code, especially when writing from scratch. Fortunately,
 *    a non-copyrighted version of mkdir was available to consult.
 *
 *  Version $Id$
 */
#include "bacula.h"
#include "jcr.h"

static bool makedir(JCR *jcr, char *path, mode_t mode, int *created)
{
   struct stat statp;

   if (mkdir(path, mode) != 0) {
      berrno be;
      *created = false;
      if (stat(path, &statp) != 0) {
         Jmsg2(jcr, M_ERROR, 0, _("Cannot create directory %s: ERR=%s\n"),
              path, be.bstrerror());
         return false;
      } else if (!S_ISDIR(statp.st_mode)) {
         Jmsg1(jcr, M_ERROR, 0, _("%s exists but is not a directory.\n"), path);
         return false;
      }
      return true;                 /* directory exists */
   }
   *created = true;
   return true;
}

static void set_own_mod(ATTR *attr, char *path, uid_t owner, gid_t group, mode_t mode)
{
   if (chown(path, owner, group) != 0 && attr->uid == 0
#ifdef AFS
        && errno != EPERM
#endif
   ) {
      berrno be;
      Jmsg2(attr->jcr, M_WARNING, 0, _("Cannot change owner and/or group of %s: ERR=%s\n"),
           path, be.bstrerror());
   }
   if (chmod(path, mode) != 0 && attr->uid == 0) {
      berrno be;
      Jmsg2(attr->jcr, M_WARNING, 0, _("Cannot change permissions of %s: ERR=%s\n"),
           path, be.bstrerror());
   }
}

/*
 * mode is the mode bits to use in creating a new directory
 *
 * parent_mode are the parent's modes if we need to create parent 
 *    directories.
 *
 * owner and group are to set on any created dirs
 *
 * keep_dir_modes if set means don't change mode bits if dir exists
 */
bool makepath(ATTR *attr, const char *apath, mode_t mode, mode_t parent_mode,
            uid_t owner, gid_t group, int keep_dir_modes)
{
   struct stat statp;
   mode_t omask, tmode;
   char *path = (char *)apath;
   char *p;
   int len;
   bool ok = false;
   int created;
   char new_dir[5000];
   int ndir = 0;
   int i = 0;
   int max_dirs = (int)sizeof(new_dir);
   JCR *jcr = attr->jcr;

   if (stat(path, &statp) == 0) {     /* Does dir exist? */
      if (!S_ISDIR(statp.st_mode)) {
         Jmsg1(jcr, M_ERROR, 0, _("%s exists but is not a directory.\n"), path);
         return false;
      }
      /* Full path exists */
      if (keep_dir_modes) {
         return true;
      }
      set_own_mod(attr, path, owner, group, mode);
      return true;
   }
   omask = umask(0);
   umask(omask);
   len = strlen(apath);
   path = (char *)alloca(len+1);
   bstrncpy(path, apath, len+1);
   strip_trailing_slashes(path);
   /*
    * Now for one of the complexities. If we are not running as root,
    *  then if the parent_mode does not have wx user perms, or we are
    *  setting the userid or group, and the parent_mode has setuid, setgid,
    *  or sticky bits, we must create the dir with open permissions, then
    *  go back and patch all the dirs up with the correct perms.
    * Solution, set everything to 0777, then go back and reset them at the
    *  end.
    */
   tmode = 0777;

#if defined(HAVE_WIN32)
   /* Validate drive letter */
   if (path[1] == ':') {
      char drive[4] = "X:\\";

      drive[0] = path[0];

      UINT drive_type = GetDriveType(drive);

      if (drive_type == DRIVE_UNKNOWN || drive_type == DRIVE_NO_ROOT_DIR) {
         Jmsg1(jcr, M_ERROR, 0, _("%c: is not a valid drive.\n"), path[0]);
         goto bail_out;
      }

      if (path[2] == '\0') {          /* attempt to create a drive */
         ok = true;
         goto bail_out;               /* OK, it is already there */
      }

      p = &path[3];
   } else {
      p = path;
   }
#else
   p = path;
#endif

   /* Skip leading slash(es) */
   while (IsPathSeparator(*p)) {
      p++;
   }
   while ((p = first_path_separator(p))) {
      char save_p;
      save_p = *p;
      *p = 0;
      if (!makedir(jcr, path, tmode, &created)) {
         goto bail_out;
      }
      if (ndir < max_dirs) {
         new_dir[ndir++] = created;
      }
      *p = save_p;
      while (IsPathSeparator(*p)) {
         p++;
      }
   }
   /* Create final component */
   if (!makedir(jcr, path, tmode, &created)) {
      goto bail_out;
   }
   if (ndir < max_dirs) {
      new_dir[ndir++] = created;
   }
   if (ndir >= max_dirs) {
      Jmsg0(jcr, M_WARNING, 0, _("Too many subdirectories. Some permissions not reset.\n"));
   }

   /* Now set the proper owner and modes */
#if defined(HAVE_WIN32)
   if (path[1] == ':') {
      p = &path[3];
   } else {
      p = path;
   }
#else
   p = path;
#endif
   /* Skip leading slash(es) */
   while (IsPathSeparator(*p)) {
      p++;
   }
   while ((p = first_path_separator(p))) {
      char save_p;
      save_p = *p;
      *p = 0;
      if (i < ndir && new_dir[i++] && !keep_dir_modes) {
         set_own_mod(attr, path, owner, group, parent_mode);
      }
      *p = save_p;
      while (IsPathSeparator(*p)) {
         p++;
      }
   }

   /* Set for final component */
   if (i < ndir && new_dir[i++]) {
      set_own_mod(attr, path, owner, group, mode);
   }

   ok = true;
bail_out:
   umask(omask);
   return ok;
}
