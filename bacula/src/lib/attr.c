/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2003-2007 Free Software Foundation Europe e.V.

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
 *   attr.c  Unpack an Attribute record returned from the tape
 *
 *    Kern Sibbald, June MMIII  (code pulled from filed/restore.c and updated)
 *
 *   Version $Id$
 */


#include "bacula.h"
#include "jcr.h"
#include "lib/breg.h"

ATTR *new_attr(JCR *jcr)
{
   ATTR *attr = (ATTR *)malloc(sizeof(ATTR));
   memset(attr, 0, sizeof(ATTR));
   attr->ofname = get_pool_memory(PM_FNAME);
   attr->olname = get_pool_memory(PM_FNAME);
   attr->attrEx = get_pool_memory(PM_FNAME);
   attr->jcr = jcr;
   attr->uid = getuid();
   return attr;
}

void free_attr(ATTR *attr)
{
   free_pool_memory(attr->olname);
   free_pool_memory(attr->ofname);
   free_pool_memory(attr->attrEx);
   free(attr);
}

int unpack_attributes_record(JCR *jcr, int32_t stream, char *rec, ATTR *attr)
{
   char *p;
   /*
    * An Attributes record consists of:
    *    File_index
    *    Type   (FT_types)
    *    Filename
    *    Attributes
    *    Link name (if file linked i.e. FT_LNK)
    *    Extended attributes (Win32)
    *  plus optional values determined by AR_ flags in upper bits of Type
    *    Data_stream
    *
    */
   attr->stream = stream;
   Dmsg1(400, "Attr: %s\n", rec);
   if (sscanf(rec, "%d %d", &attr->file_index, &attr->type) != 2) {
      Jmsg(jcr, M_FATAL, 0, _("Error scanning attributes: %s\n"), rec);
      Dmsg1(100, "\nError scanning attributes. %s\n", rec);
      return 0;
   }
   Dmsg2(400, "Got Attr: FilInx=%d type=%d\n", attr->file_index, attr->type);
   if (attr->type & AR_DATA_STREAM) {
      attr->data_stream = 1;
   } else {
      attr->data_stream = 0;
   }
   attr->type &= FT_MASK;             /* keep only type bits */
   p = rec;
   while (*p++ != ' ')               /* skip record file index */
      { }
   while (*p++ != ' ')               /* skip type */
      { }

   attr->fname = p;                   /* set filname position */
   while (*p++ != 0)                  /* skip filename */
      { }
   attr->attr = p;                    /* set attributes position */
   while (*p++ != 0)                  /* skip attributes */
      { }
   attr->lname = p;                   /* set link position */
   while (*p++ != 0)                  /* skip link */
      { }
   pm_strcpy(attr->attrEx, p);        /* copy extended attributes, if any */

   if (attr->data_stream) {
      int64_t val;
      while (*p++ != 0)               /* skip extended attributes */
         { }
      from_base64(&val, p);
      attr->data_stream = (int32_t)val;
   }
   Dmsg7(400, "unpack_attr FI=%d Type=%d fname=%s attr=%s lname=%s attrEx=%s ds=%d\n",
      attr->file_index, attr->type, attr->fname, attr->attr, attr->lname,
      attr->attrEx, attr->data_stream);
   *attr->ofname = 0;
   *attr->olname = 0;
   return 1;
}

#if defined(HAVE_WIN32)
static void strip_double_slashes(char *fname)
{
   char *p = fname;
   while (p && *p) {
      p = strpbrk(p, "/\\");
      if (p != NULL) {
         if (IsPathSeparator(p[1])) {
            strcpy(p, p+1);
         }
         p++;
      }
   }
}
#endif

/*
 * Build attr->ofname from attr->fname and
 *       attr->olname from attr->olname
 */
void build_attr_output_fnames(JCR *jcr, ATTR *attr)
{
   /*
    * Prepend the where directory so that the
    * files are put where the user wants.
    *
    * We do a little jig here to handle Win32 files with
    *   a drive letter -- we simply change the drive
    *   from, for example, c: to c/ for
    *   every filename if a prefix is supplied.
    *
    */

   if (jcr->where_bregexp) { 
      char *ret;
      apply_bregexps(attr->fname, jcr->where_bregexp, &ret);
      pm_strcpy(attr->ofname, ret);

      if (attr->type == FT_LNKSAVED || attr->type == FT_LNK) {
         /* Always add prefix to hard links (FT_LNKSAVED) and
          *  on user request to soft links
          */

         if ((attr->type == FT_LNKSAVED || jcr->prefix_links)) {
            apply_bregexps(attr->lname, jcr->where_bregexp, &ret);
            pm_strcpy(attr->olname, ret);

         } else {
            pm_strcpy(attr->olname, attr->lname);
         }
      }
      
   } else if (jcr->where[0] == 0) {
      pm_strcpy(attr->ofname, attr->fname);
      pm_strcpy(attr->olname, attr->lname);

   } else {
      const char *fn;
      int wherelen = strlen(jcr->where);
      pm_strcpy(attr->ofname, jcr->where);  /* copy prefix */
#if defined(HAVE_WIN32)
      if (attr->fname[1] == ':') {
         attr->fname[1] = '/';     /* convert : to / */
      }
#endif
      fn = attr->fname;            /* take whole name */
      /* Ensure where is terminated with a slash */
      if (!IsPathSeparator(jcr->where[wherelen-1]) && !IsPathSeparator(fn[0])) {
         pm_strcat(attr->ofname, "/");
      }
      pm_strcat(attr->ofname, fn); /* copy rest of name */
      /*
       * Fixup link name -- if it is an absolute path
       */
      if (attr->type == FT_LNKSAVED || attr->type == FT_LNK) {
         bool add_link;
         /* Always add prefix to hard links (FT_LNKSAVED) and
          *  on user request to soft links
          */
         if (IsPathSeparator(attr->lname[0]) &&
             (attr->type == FT_LNKSAVED || jcr->prefix_links)) {
            pm_strcpy(attr->olname, jcr->where);
            add_link = true;
         } else {
            attr->olname[0] = 0;
            add_link = false;
         }

#if defined(HAVE_WIN32)
         if (attr->lname[1] == ':') {
            attr->lname[1] = '/';    /* turn : into / */
         }
#endif
         fn = attr->lname;       /* take whole name */
         /* Ensure where is terminated with a slash */
         if (add_link && 
            !IsPathSeparator(jcr->where[wherelen-1]) && 
            !IsPathSeparator(fn[0])) {
            pm_strcat(attr->olname, "/");
         }
         pm_strcat(attr->olname, fn);     /* copy rest of link */
      }
   }
#if defined(HAVE_WIN32)
   strip_double_slashes(attr->ofname);
   strip_double_slashes(attr->olname);
#endif
}

extern char *getuser(uid_t uid, char *name, int len);
extern char *getgroup(gid_t gid, char *name, int len);

/*
 * Print an ls style message, also send M_RESTORED
 */
void print_ls_output(JCR *jcr, ATTR *attr)
{
   char buf[5000];
   char ec1[30];
   char en1[30], en2[30];
   char *p, *f;
   guid_list *guid;

   if (attr->type == FT_DELETED) { /* TODO: change this to get last seen values */
      bsnprintf(buf, sizeof(buf),
                "----------   - -        -                - ---------- --------  %s\n", attr->ofname);
      Dmsg1(20, "%s", buf);
      Jmsg(jcr, M_RESTORED, 1, "%s", buf);
      return;
   }

   if (!jcr->id_list) {
      jcr->id_list = new_guid_list();
   }
   guid = jcr->id_list;
   p = encode_mode(attr->statp.st_mode, buf);
   p += sprintf(p, "  %2d ", (uint32_t)attr->statp.st_nlink);
   p += sprintf(p, "%-8.8s %-8.8s", 
                guid->uid_to_name(attr->statp.st_uid, en1, sizeof(en1)),
                guid->gid_to_name(attr->statp.st_gid, en2, sizeof(en2)));
   p += sprintf(p, "%12.12s ", edit_int64(attr->statp.st_size, ec1));
   p = encode_time(attr->statp.st_ctime, p);
   *p++ = ' ';
   *p++ = ' ';
   for (f=attr->ofname; *f && (p-buf) < (int)sizeof(buf)-10; ) {
      *p++ = *f++;
   }
   if (attr->type == FT_LNK) {
      *p++ = ' ';
      *p++ = '-';
      *p++ = '>';
      *p++ = ' ';
      /* Copy link name */
      for (f=attr->olname; *f && (p-buf) < (int)sizeof(buf)-10; ) {
         *p++ = *f++;
      }
   }
   *p++ = '\n';
   *p = 0;
   Dmsg1(20, "%s", buf);
   Jmsg(jcr, M_RESTORED, 1, "%s", buf);
}
