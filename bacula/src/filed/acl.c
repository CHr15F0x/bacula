/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2004-2009 Free Software Foundation Europe e.V.

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
 * Functions to handle ACLs for bacula.
 *
 * We handle two different types of ACLs: access and default ACLS.
 * On most systems that support default ACLs they only apply to directories.
 *
 * On some systems (eg. linux and FreeBSD) we must obtain the two ACLs
 * independently, while others (eg. Solaris) provide both in one call.
 *
 * The Filed saves ACLs in their native format and uses different streams
 * for all different platforms. Currently we only allow ACLs to be restored
 * which were saved in the native format of the platform they are extracted
 * on. Later on we might add conversion functions for mapping from one
 * platform to an other or allow restores of systems that use the same
 * native format.
 *
 * Its also interesting to see what the exact format of acl text is on
 * certain platforms and if they use they same encoding we might allow
 * different platform streams to be decoded on an other similar platform.
 *
 *   Original written by Preben 'Peppe' Guldberg, December MMIV
 *   Major rewrite by Marco van Wieringen, November MMVIII
 */
  
#include "bacula.h"
#include "filed.h"
  
#if !defined(HAVE_ACL)
/*
 * Entry points when compiled without support for ACLs or on an unsupported platform.
 */
bacl_exit_code build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   return bacl_exit_fatal;
}

bacl_exit_code parse_acl_streams(JCR *jcr, int stream)
{
   return bacl_exit_fatal;
}
#else
/*
 * Send an ACL stream to the SD.
 */
static bacl_exit_code send_acl_stream(JCR *jcr, int stream)
{
   BSOCK *sd = jcr->store_bsock;
   POOLMEM *msgsave;
#ifdef FD_NO_SEND_TEST
   return bacl_exit_ok;
#endif

   /*
    * Sanity check
    */
   if (jcr->acl_data->content_length <= 0) {
      return bacl_exit_ok;
   }

   /*
    * Send header
    */
   if (!sd->fsend("%ld %d 0", jcr->JobFiles, stream)) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
      return bacl_exit_fatal;
   }

   /*
    * Send the buffer to the storage deamon
    */
   Dmsg1(400, "Backing up ACL <%s>\n", jcr->acl_data->content);
   msgsave = sd->msg;
   sd->msg = jcr->acl_data->content;
   sd->msglen = jcr->acl_data->content_length + 1;
   if (!sd->send()) {
      sd->msg = msgsave;
      sd->msglen = 0;
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
      return bacl_exit_fatal;
   }

   jcr->JobBytes += sd->msglen;
   sd->msg = msgsave;
   if (!sd->signal(BNET_EOD)) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
      return bacl_exit_fatal;
   }

   Dmsg1(200, "ACL of file: %s successfully backed up!\n", jcr->last_fname);
   return bacl_exit_ok;
}

#if defined(HAVE_AIX_OS)

#include <sys/access.h>

/*
 * Define the supported ACL streams for this OS
 */
static int os_access_acl_streams[1] = { STREAM_ACL_AIX_TEXT };
static int os_default_acl_streams[1] = { -1 };

static bacl_exit_code aix_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   char *acl_text;

   if ((acl_text = acl_get(jcr->last_fname)) != NULL) {
      jcr->acl_data->content_length = pm_strcpy(jcr->acl_data->content, acl_text);
      actuallyfree(acl_text);
      return send_acl_stream(jcr, STREAM_ACL_AIX_TEXT);
   }
   return bacl_exit_error;
}

static bacl_exit_code aix_parse_acl_streams(JCR *jcr, int stream)
{
   if (acl_put(jcr->last_fname, jcr->acl_data->content, 0) != 0) {
      return bacl_exit_error;
   }
   return bacl_exit_ok;
}

/*
 * For this OS setup the build and parse function pointer to the OS specific functions.
 */
static bacl_exit_code (*os_build_acl_streams)(JCR *jcr, FF_PKT *ff_pkt) = aix_build_acl_streams;
static bacl_exit_code (*os_parse_acl_streams)(JCR *jcr, int stream) = aix_parse_acl_streams;

#elif defined(HAVE_DARWIN_OS) || \
      defined(HAVE_FREEBSD_OS) || \
      defined(HAVE_IRIX_OS) || \
      defined(HAVE_OSF1_OS) || \
      defined(HAVE_LINUX_OS)

#include <sys/types.h>

#ifdef HAVE_SYS_ACL_H
#include <sys/acl.h>
#else
#error "configure failed to detect availability of sys/acl.h"
#endif

/* On IRIX we can get shortened ACLs */
#if defined(HAVE_IRIX_OS) && defined(BACL_WANT_SHORT_ACLS)
#define acl_to_text(acl,len)     acl_to_short_text((acl), (len))
#endif

/* In Linux we can get numeric and/or shorted ACLs */
#if defined(HAVE_LINUX_OS)
#if defined(BACL_WANT_SHORT_ACLS) && defined(BACL_WANT_NUMERIC_IDS)
#define BACL_ALTERNATE_TEXT            (TEXT_ABBREVIATE|TEXT_NUMERIC_IDS)
#elif defined(BACL_WANT_SHORT_ACLS)
#define BACL_ALTERNATE_TEXT            TEXT_ABBREVIATE
#elif defined(BACL_WANT_NUMERIC_IDS)
#define BACL_ALTERNATE_TEXT            TEXT_NUMERIC_IDS
#endif
#ifdef BACL_ALTERNATE_TEXT
#include <acl/libacl.h>
#define acl_to_text(acl,len)     (acl_to_any_text((acl), NULL, ',', BACL_ALTERNATE_TEXT))
#endif
#endif

/*
 * Some generic functions used by multiple OSes.
 */
static acl_type_t bac_to_os_acltype(bacl_type acltype)
{
   acl_type_t ostype;

   switch (acltype) {
   case BACL_TYPE_ACCESS:
      ostype = ACL_TYPE_ACCESS;
      break;
   case BACL_TYPE_DEFAULT:
      ostype = ACL_TYPE_DEFAULT;
      break;

#ifdef ACL_TYPE_DEFAULT_DIR
   case BACL_TYPE_DEFAULT_DIR:
      /*
       * OSF1 has an additional acl type named ACL_TYPE_DEFAULT_DIR.
       */
      ostype = ACL_TYPE_DEFAULT_DIR;
      break;
#endif
#ifdef ACL_TYPE_EXTENDED
   case BACL_TYPE_EXTENDED:
      /*
       * MacOSX has an additional acl type named ACL_TYPE_EXTENDED.
       */
      ostype = ACL_TYPE_EXTENDED;
      break;
#endif
   default:
      /*
       * This should never happen, as the per OS version function only tries acl
       * types supported on a certain platform.
       */
      ostype = (acl_type_t)ACL_TYPE_NONE;
      break;
   }
   return ostype;
}

#if !defined(HAVE_DARWIN_OS)
/*
 * See if an acl is a trivial one (e.g. just the stat bits encoded as acl.)
 * There is no need to store those acls as we already store the stat bits too.
 */
static bool acl_is_trivial(acl_t acl)
{
  /*
   * acl is trivial if it has only the following entries:
   * "user::",
   * "group::",
   * "other::"
   */
   acl_entry_t ace;
   acl_tag_t tag;
#if defined(HAVE_FREEBSD_OS) || \
    defined(HAVE_LINUX_OS)
   int entry_available;

   entry_available = acl_get_entry(acl, ACL_FIRST_ENTRY, &ace);
   while (entry_available == 1) {
      /*
       * Get the tag type of this acl entry.
       * If we fail to get the tagtype we call the acl non-trivial.
       */
      if (acl_get_tag_type(ace, &tag) < 0)
         return true;
      /*
       * Anything other the ACL_USER_OBJ, ACL_GROUP_OBJ or ACL_OTHER breaks the spell.
       */
      if (tag != ACL_USER_OBJ &&
          tag != ACL_GROUP_OBJ &&
          tag != ACL_OTHER)
         return false;
      entry_available = acl_get_entry(acl, ACL_NEXT_ENTRY, &ace);
   }
   return true;
#elif defined(HAVE_IRIX_OS)
   int n;

   for (n = 0; n < acl->acl_cnt; n++) {
      ace = &acl->acl_entry[n];
      tag = ace->ae_tag;

      /*
       * Anything other the ACL_USER_OBJ, ACL_GROUP_OBJ or ACL_OTHER breaks the spell.
       */
      if (tag != ACL_USER_OBJ &&
          tag != ACL_GROUP_OBJ &&
          tag != ACL_OTHER_OBJ)
         return false;
   }
   return true;
#elif defined(HAVE_OSF1_OS)
   int count;

   ace = acl->acl_first;
   count = acl->acl_num;

   while (count > 0) {
      tag = ace->entry->acl_type;
      /*
       * Anything other the ACL_USER_OBJ, ACL_GROUP_OBJ or ACL_OTHER breaks the spell.
       */
      if (tag != ACL_USER_OBJ &&
          tag != ACL_GROUP_OBJ &&
          tag != ACL_OTHER)
         return false;
      /*
       * On Tru64, perm can also contain non-standard bits such as
       * PERM_INSERT, PERM_DELETE, PERM_MODIFY, PERM_LOOKUP, ...
       */
      if ((ace->entry->acl_perm & ~(ACL_READ | ACL_WRITE | ACL_EXECUTE)))
         return false;
      ace = ace->next;
      count--;
   }
   return true;
#endif
}
#endif

/*
 * Generic wrapper around acl_get_file call.
 */
static bacl_exit_code generic_get_acl_from_os(JCR *jcr, bacl_type acltype)
{
   acl_t acl;
   acl_type_t ostype;
   char *acl_text;
   berrno be;

   ostype = bac_to_os_acltype(acltype);
   acl = acl_get_file(jcr->last_fname, ostype);
   if (acl) {
#if defined(HAVE_IRIX_OS)
      /* 
       * From observation, IRIX's acl_get_file() seems to return a
       * non-NULL acl with a count field of -1 when a file has no ACL
       * defined, while IRIX's acl_to_text() returns NULL when presented
       * with such an ACL. 
       *
       * Checking the count in the acl structure before calling
       * acl_to_text() lets us avoid error messages about files
       * with no ACLs, without modifying the flow of the code used for 
       * other operating systems, and it saves making some calls
       * to acl_to_text() besides.
       */
      if (acl->acl_cnt <= 0) {
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         acl_free(acl);
         return bacl_exit_ok;
      }
#endif

#if !defined(HAVE_DARWIN_OS)
      /*
       * Make sure this is not just a trivial ACL.
       */
      if (acltype == BACL_TYPE_ACCESS && acl_is_trivial(acl)) {
         /*
          * The ACLs simply reflect the (already known) standard permissions
          * So we don't send an ACL stream to the SD.
          */
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         acl_free(acl);
         return bacl_exit_ok;
      }
#endif

      if ((acl_text = acl_to_text(acl, NULL)) != NULL) {
         jcr->acl_data->content_length = pm_strcpy(jcr->acl_data->content, acl_text);
         acl_free(acl);
         acl_free(acl_text);
         return bacl_exit_ok;
      }

      Mmsg2(jcr->errmsg, _("acl_to_text error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg2(100, "acl_to_text error file=%s ERR=%s\n",  
            jcr->last_fname, be.bstrerror());

      pm_strcpy(jcr->acl_data->content, "");
      jcr->acl_data->content_length = 0;
      acl_free(acl);
      return bacl_exit_error;
   }

   /*
    * Handle errors gracefully.
    */
   if (acl == (acl_t)NULL) {
      switch (errno) {
#if defined(BACL_ENOTSUP)
      case BACL_ENOTSUP:
         break;                       /* not supported */
#endif
      case ENOENT:
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_ok;
      default:
         /* Some real error */
         Mmsg2(jcr->errmsg, _("acl_get_file error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         Dmsg2(100, "acl_get_file error file=%s ERR=%s\n",  
               jcr->last_fname, be.bstrerror());

         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_error;
      }
   }
   /*
    * Not supported, just pretend there is nothing to see
    */
   pm_strcpy(jcr->acl_data->content, "");
   jcr->acl_data->content_length = 0;
   return bacl_exit_ok;
}

/*
 * Generic wrapper around acl_set_file call.
 */
static bacl_exit_code generic_set_acl_on_os(JCR *jcr, bacl_type acltype)
{
   acl_t acl;
   acl_type_t ostype;
   berrno be;

   /*
    * If we get empty default ACLs, clear ACLs now
    */
   ostype = bac_to_os_acltype(acltype);
   if (ostype == ACL_TYPE_DEFAULT && strlen(jcr->acl_data->content) == 0) {
      if (acl_delete_def_file(jcr->last_fname) == 0) {
         return bacl_exit_ok;
      }
      switch (errno) {
      case ENOENT:
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("acl_delete_def_file error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         return bacl_exit_error;
      }
   }

   acl = acl_from_text(jcr->acl_data->content);
   if (acl == NULL) {
      Mmsg2(jcr->errmsg, _("acl_from_text error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "acl_from_text error acl=%s file=%s ERR=%s\n",  
         jcr->acl_data->content, jcr->last_fname, be.bstrerror());
      return bacl_exit_error;
   }

#ifndef HAVE_FREEBSD_OS
   /*
    * FreeBSD always fails acl_valid() - at least on valid input...
    * As it does the right thing, given valid input, just ignore acl_valid().
    */
   if (acl_valid(acl) != 0) {
      Mmsg2(jcr->errmsg, _("acl_valid error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "acl_valid error acl=%s file=%s ERR=%s\n",  
         jcr->acl_data->content, jcr->last_fname, be.bstrerror());
      acl_free(acl);
      return bacl_exit_error;
   }
#endif

   /*
    * Restore the ACLs, but don't complain about links which really should
    * not have attributes, and the file it is linked to may not yet be restored.
    * This is only true for the old acl streams as in the new implementation we
    * don't save acls of symlinks (which cannot have acls anyhow)
    */
   if (acl_set_file(jcr->last_fname, ostype, acl) != 0 && jcr->last_type != FT_LNK) {
      switch (errno) {
      case ENOENT:
         acl_free(acl);
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("acl_set_file error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         Dmsg3(100, "acl_set_file error acl=%s file=%s ERR=%s\n",
               jcr->acl_data->content, jcr->last_fname, be.bstrerror());
         acl_free(acl);
         return bacl_exit_error;
      }
   }
   acl_free(acl);
   return bacl_exit_ok;
}

/*
 * OS specific functions for handling different types of acl streams.
 */
#if defined(HAVE_DARWIN_OS)
/*
 * Define the supported ACL streams for this OS
 */
static int os_access_acl_streams[1] = { STREAM_ACL_DARWIN_ACCESS_ACL };
static int os_default_acl_streams[1] = { -1 };

static bacl_exit_code darwin_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
#if defined(ACL_TYPE_EXTENDED)
   /*
    * On MacOS X, acl_get_file (name, ACL_TYPE_ACCESS)
    * and acl_get_file (name, ACL_TYPE_DEFAULT)
    * always return NULL / EINVAL.  There is no point in making
    * these two useless calls.  The real ACL is retrieved through
    * acl_get_file (name, ACL_TYPE_EXTENDED).
    *
    * Read access ACLs for files, dirs and links
    */
   if (generic_get_acl_from_os(jcr, BACL_TYPE_EXTENDED) == bacl_exit_fatal)
      return bacl_exit_fatal;
#else
   /*
    * Read access ACLs for files, dirs and links
    */
   if (generic_get_acl_from_os(jcr, BACL_TYPE_ACCESS) == bacl_exit_fatal)
      return bacl_exit_fatal;
#endif

   if (jcr->acl_data->content_length > 0) {
      return send_acl_stream(jcr, STREAM_ACL_DARWIN_ACCESS_ACL);
   }
   return bacl_exit_ok;
}

static bacl_exit_code darwin_parse_acl_streams(JCR *jcr, int stream)
{
#if defined(ACL_TYPE_EXTENDED)
      return generic_set_acl_on_os(jcr, BACL_TYPE_EXTENDED);
#else
      return generic_set_acl_on_os(jcr, BACL_TYPE_ACCESS);
#endif
}

/*
 * For this OS setup the build and parse function pointer to the OS specific functions.
 */
static bacl_exit_code (*os_build_acl_streams)(JCR *jcr, FF_PKT *ff_pkt) = darwin_build_acl_streams;
static bacl_exit_code (*os_parse_acl_streams)(JCR *jcr, int stream) = darwin_parse_acl_streams;

#elif defined(HAVE_FREEBSD_OS) || \
      defined(HAVE_IRIX_OS) || \
      defined(HAVE_LINUX_OS)

/*
 * Define the supported ACL streams for these OSes
 */
#if defined(HAVE_FREEBSD_OS)
static int os_access_acl_streams[1] = { STREAM_ACL_FREEBSD_ACCESS_ACL };
static int os_default_acl_streams[1] = { STREAM_ACL_FREEBSD_DEFAULT_ACL };
#elif defined(HAVE_IRIX_OS)
static int os_access_acl_streams[1] = { STREAM_ACL_IRIX_ACCESS_ACL };
static int os_default_acl_streams[1] = { STREAM_ACL_IRIX_DEFAULT_ACL };
#elif defined(HAVE_LINUX_OS)
static int os_access_acl_streams[1] = { STREAM_ACL_LINUX_ACCESS_ACL };
static int os_default_acl_streams[1] = { STREAM_ACL_LINUX_DEFAULT_ACL };
#endif

static bacl_exit_code generic_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   /*
    * Read access ACLs for files, dirs and links
    */
   if (generic_get_acl_from_os(jcr, BACL_TYPE_ACCESS) == bacl_exit_fatal)
      return bacl_exit_fatal;

   if (jcr->acl_data->content_length > 0) {
      if (send_acl_stream(jcr, os_access_acl_streams[0]) == bacl_exit_fatal)
         return bacl_exit_fatal;
   }

   /*
    * Directories can have default ACLs too
    */
   if (ff_pkt->type == FT_DIREND) {
      if (generic_get_acl_from_os(jcr, BACL_TYPE_DEFAULT) == bacl_exit_fatal)
         return bacl_exit_fatal;
      if (jcr->acl_data->content_length > 0) {
         if (send_acl_stream(jcr, os_default_acl_streams[0]) == bacl_exit_fatal)
            return bacl_exit_fatal;
      }
   }
   return bacl_exit_ok;
}

static bacl_exit_code generic_parse_acl_streams(JCR *jcr, int stream)
{
   unsigned int cnt;

   switch (stream) {
   case STREAM_UNIX_ACCESS_ACL:
      return generic_set_acl_on_os(jcr, BACL_TYPE_ACCESS);
   case STREAM_UNIX_DEFAULT_ACL:
      return generic_set_acl_on_os(jcr, BACL_TYPE_DEFAULT);
   default:
      /*
       * See what type of acl it is.
       */
      for (cnt = 0; cnt < sizeof(os_access_acl_streams) / sizeof(int); cnt++) {
         if (os_access_acl_streams[cnt] == stream) {
            return generic_set_acl_on_os(jcr, BACL_TYPE_ACCESS);
         }
      }
      for (cnt = 0; cnt < sizeof(os_default_acl_streams) / sizeof(int); cnt++) {
         if (os_default_acl_streams[cnt] == stream) {
            return generic_set_acl_on_os(jcr, BACL_TYPE_DEFAULT);
         }
      }
      break;
   }
   return bacl_exit_error;
}

/*
 * For this OSes setup the build and parse function pointer to the OS specific functions.
 */
static bacl_exit_code (*os_build_acl_streams)(JCR *jcr, FF_PKT *ff_pkt) = generic_build_acl_streams;
static bacl_exit_code (*os_parse_acl_streams)(JCR *jcr, int stream) = generic_parse_acl_streams;

#elif defined(HAVE_OSF1_OS)

/*
 * Define the supported ACL streams for this OS
 */
static int os_access_acl_streams[1] = { STREAM_ACL_TRU64_ACCESS_ACL };
static int os_default_acl_streams[2] = { STREAM_ACL_TRU64_DEFAULT_ACL, STREAM_ACL_TRU64_DEFAULT_DIR_ACL };

static bacl_exit_code tru64_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   /*
    * Read access ACLs for files, dirs and links
    */
   if ((jcr->acl_data->content_length = generic_get_acl_from_os(jcr, BACL_TYPE_ACCESS)) < 0)
      return bacl_exit_error;
   if (jcr->acl_data->content_length > 0) {
      if (!send_acl_stream(jcr, STREAM_ACL_TRU64_ACCESS_ACL))
         return bacl_exit_error;
   }
   /*
    * Directories can have default ACLs too
    */
   if (ff_pkt->type == FT_DIREND) {
      if ((jcr->acl_data->content_length = generic_get_acl_from_os(jcr, BACL_TYPE_DEFAULT)) < 0)
         return bacl_exit_error;
      if (jcr->acl_data->content_length > 0) {
         if (!send_acl_stream(jcr, STREAM_ACL_TRU64_DEFAULT_ACL))
            return bacl_exit_error;
      }
      /*
       * Tru64 has next to BACL_TYPE_DEFAULT also BACL_TYPE_DEFAULT_DIR acls.
       * This is an inherited acl for all subdirs.
       * See http://www.helsinki.fi/atk/unix/dec_manuals/DOC_40D/AQ0R2DTE/DOCU_018.HTM
       * Section 21.5 Default ACLs 
       */
      if ((jcr->acl_data->content_length = generic_get_acl_from_os(jcr, BACL_TYPE_DEFAULT_DIR)) < 0)
         return bacl_exit_error;
      if (jcr->acl_data->content_length > 0) {
         if (!send_acl_stream(jcr, STREAM_ACL_TRU64_DEFAULT_DIR_ACL))
            return bacl_exit_error;
      }
   }
   return bacl_exit_ok;
}

static bacl_exit_code tru64_parse_acl_streams(JCR *jcr, int stream)
{
   switch (stream) {
   case STREAM_UNIX_ACCESS_ACL:
   case STREAM_ACL_TRU64_ACCESS_ACL:
      return generic_set_acl_on_os(jcr, BACL_TYPE_ACCESS);
   case STREAM_UNIX_DEFAULT_ACL:
   case STREAM_ACL_TRU64_DEFAULT_ACL:
      return generic_set_acl_on_os(jcr, BACL_TYPE_DEFAULT);
   case STREAM_ACL_TRU64_DEFAULT_DIR_ACL:
      return generic_set_acl_on_os(jcr, BACL_TYPE_DEFAULT_DIR);
}

/*
 * For this OS setup the build and parse function pointer to the OS specific functions.
 */
static bacl_exit_code (*os_build_acl_streams)(JCR *jcr, FF_PKT *ff_pkt) = tru64_build_acl_streams;
static bacl_exit_code (*os_parse_acl_streams)(JCR *jcr, int stream) = tru64_parse_acl_streams;

#endif

#elif defined(HAVE_HPUX_OS)
#ifdef HAVE_SYS_ACL_H
#include <sys/acl.h>
#else
#error "configure failed to detect availability of sys/acl.h"
#endif

#include <acllib.h>

/*
 * Define the supported ACL streams for this OS
 */
static int os_access_acl_streams[1] = { STREAM_ACL_HPUX_ACL_ENTRY };
static int os_default_acl_streams[1] = { -1 };

/*
 * See if an acl is a trivial one (e.g. just the stat bits encoded as acl.)
 * There is no need to store those acls as we already store the stat bits too.
 */
static bool acl_is_trivial(int count, struct acl_entry *entries, struct stat sb)
{
   int n;
   struct acl_entry ace

   for (n = 0; n < count; n++) {
      ace = entries[n];
      /*
       * See if this acl just is the stat mode in acl form.
       */
      if (!((ace.uid == sb.st_uid && ace.gid == ACL_NSGROUP) ||
            (ace.uid == ACL_NSUSER && ace.gid == sb.st_gid) ||
            (ace.uid == ACL_NSUSER && ace.gid == ACL_NSGROUP)))
         return false;
   }
   return true;
}

/*
 * OS specific functions for handling different types of acl streams.
 */
static bacl_exit_code hpux_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   int n;
   struct acl_entry acls[NACLENTRIES];
   char *acl_text;
   berrno be;

   if ((n = getacl(jcr->last_fname, 0, acls)) < 0) {
      switch (errno) {
#if defined(BACL_ENOTSUP)
      case BACL_ENOTSUP:
         /*
          * Not supported, just pretend there is nothing to see
          */
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_ok;
#endif
      case ENOENT:
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("getacl error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         Dmsg2(100, "getacl error file=%s ERR=%s\n",  
               jcr->last_fname, be.bstrerror());

         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_error;
      }
   }
   if (n == 0) {
      pm_strcpy(jcr->acl_data->content, "");
      jcr->acl_data->content_length = 0;
      return bacl_exit_ok;
   }
   if ((n = getacl(jcr->last_fname, n, acls)) > 0) {
      if (acl_is_trivial(n, acls, ff_pkt->statp)) {
         /*
          * The ACLs simply reflect the (already known) standard permissions
          * So we don't send an ACL stream to the SD.
          */
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_ok;
      }
      if ((acl_text = acltostr(n, acls, FORM_SHORT)) != NULL) {
         jcr->acl_data->content_length = pm_strcpy(jcr->acl_data->content, acl_text);
         actuallyfree(acl_text);

         return send_acl_stream(jcr, STREAM_ACL_HPUX_ACL_ENTRY);
      }
      Mmsg2(jcr->errmsg, _("acltostr error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "acltostr error acl=%s file=%s ERR=%s\n",  
            jcr->acl_data->content, jcr->last_fname, be.bstrerror());
      return bacl_exit_error;
   }
   return bacl_exit_error;
}

static bacl_exit_code hpux_parse_acl_streams(JCR *jcr, int stream)
{
   int n, stat;
   struct acl_entry acls[NACLENTRIES];
   berrno be;

   n = strtoacl(jcr->acl_data->content, 0, NACLENTRIES, acls, ACL_FILEOWNER, ACL_FILEGROUP);
   if (n <= 0) {
      Mmsg2(jcr->errmsg, _("strtoacl error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "strtoacl error acl=%s file=%s ERR=%s\n",  
            jcr->acl_data->content, jcr->last_fname, be.bstrerror());
      return bacl_exit_error;
   }
   if (strtoacl(jcr->acl_data->content, n, NACLENTRIES, acls, ACL_FILEOWNER, ACL_FILEGROUP) != n) {
      Mmsg2(jcr->errmsg, _("strtoacl error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "strtoacl error acl=%s file=%s ERR=%s\n",  
            jcr->acl_data->content, jcr->last_fname, be.bstrerror());

      return bacl_exit_error;
   }
   /*
    * Restore the ACLs, but don't complain about links which really should
    * not have attributes, and the file it is linked to may not yet be restored.
    * This is only true for the old acl streams as in the new implementation we
    * don't save acls of symlinks (which cannot have acls anyhow)
    */
   if (setacl(jcr->last_fname, n, acls) != 0 && jcr->last_type != FT_LNK) {
      switch (errno) {
      case ENOENT:
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("setacl error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         Dmsg3(100, "setacl error acl=%s file=%s ERR=%s\n",
               jcr->acl_data->content, jcr->last_fname, be.bstrerror());
         return bacl_exit_error;
      }
   }
   return bacl_exit_ok;
}

/*
 * For this OS setup the build and parse function pointer to the OS specific functions.
 */
static bacl_exit_code (*os_build_acl_streams)(JCR *jcr, FF_PKT *ff_pkt) = hpux_build_acl_streams;
static bacl_exit_code (*os_parse_acl_streams)(JCR *jcr, int stream) = hpux_parse_acl_streams;

#elif defined(HAVE_SUN_OS)
#ifdef HAVE_SYS_ACL_H
#include <sys/acl.h>
#else
#error "configure failed to detect availability of sys/acl.h"
#endif

#if defined(HAVE_EXTENDED_ACL)
/*
 * We define some internals of the Solaris acl libs here as those
 * are not exposed yet. Probably because they want us to see the
 * acls as opague data. But as we need to support different platforms
 * and versions of Solaris we need to expose some data to be able
 * to determine the type of acl used to stuff it into the correct
 * data stream. I know this is far from portable, but maybe the
 * proper interface is exposed later on and we can get ride of
 * this kludge. Newer versions of Solaris include sys/acl_impl.h
 * which has implementation details of acls, if thats included we
 * don't have to define it ourself.
 */
#if !defined(_SYS_ACL_IMPL_H)
typedef enum acl_type {
   ACLENT_T = 0,
   ACE_T = 1
} acl_type_t;
#endif

/*
 * Two external references to functions in the libsec library function not in current include files.
 */
extern "C" {
int acl_type(acl_t *);
char *acl_strerror(int);
}

/*
 * Define the supported ACL streams for this OS
 */
static int os_access_acl_streams[2] = { STREAM_ACL_SOLARIS_ACLENT, STREAM_ACL_SOLARIS_ACE };
static int os_default_acl_streams[1] = { -1 };

/*
 * As the new libsec interface with acl_totext and acl_fromtext also handles
 * the old format from acltotext we can use the new functions even
 * for acls retrieved and stored in the database with older fd versions. If the
 * new interface is not defined (Solaris 9 and older we fall back to the old code)
 */
static bacl_exit_code solaris_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   int acl_enabled, flags;
   acl_t *aclp;
   char *acl_text;
   bacl_exit_code stream_status = bacl_exit_error;
   berrno be;

   /*
    * See if filesystem supports acls.
    */
   acl_enabled = pathconf(jcr->last_fname, _PC_ACL_ENABLED);
   switch (acl_enabled) {
   case 0:
      pm_strcpy(jcr->acl_data->content, "");
      jcr->acl_data->content_length = 0;
      return bacl_exit_ok;
   case -1:
      switch (errno) {
      case ENOENT:
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("pathconf error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         Dmsg2(100, "pathconf error file=%s ERR=%s\n",  
               jcr->last_fname, be.bstrerror());
         return bacl_exit_error;
      }
   default:
      break;
   }

   /*
    * Get ACL info: don't bother allocating space if there is only a trivial ACL.
    */
   if (acl_get(jcr->last_fname, ACL_NO_TRIVIAL, &aclp) != 0) {
      switch (errno) {
      case ENOENT:
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("acl_get error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, acl_strerror(errno));
         Dmsg2(100, "acl_get error file=%s ERR=%s\n",  
               jcr->last_fname, acl_strerror(errno));
         return bacl_exit_error;
      }
   }

   if (!aclp) {
      /*
       * The ACLs simply reflect the (already known) standard permissions
       * So we don't send an ACL stream to the SD.
       */
      pm_strcpy(jcr->acl_data->content, "");
      jcr->acl_data->content_length = 0;
      return bacl_exit_ok;
   }

#if defined(ACL_SID_FMT)
   /*
    * New format flag added in newer Solaris versions.
    */
   flags = ACL_APPEND_ID | ACL_COMPACT_FMT | ACL_SID_FMT;
#else
   flags = ACL_APPEND_ID | ACL_COMPACT_FMT;
#endif /* ACL_SID_FMT */

   if ((acl_text = acl_totext(aclp, flags)) != NULL) {
      jcr->acl_data->content_length = pm_strcpy(jcr->acl_data->content, acl_text);
      actuallyfree(acl_text);

      switch (acl_type(aclp)) {
      case ACLENT_T:
         stream_status = send_acl_stream(jcr, STREAM_ACL_SOLARIS_ACLENT);
         break;
      case ACE_T:
         stream_status = send_acl_stream(jcr, STREAM_ACL_SOLARIS_ACE);
         break;
      default:
         break;
      }

      acl_free(aclp);
   }
   return stream_status;
}

static bacl_exit_code solaris_parse_acl_streams(JCR *jcr, int stream)
{
   acl_t *aclp;
   int acl_enabled, error;
   berrno be;

   switch (stream) {
   case STREAM_UNIX_ACCESS_ACL:
   case STREAM_ACL_SOLARIS_ACLENT:
   case STREAM_ACL_SOLARIS_ACE:
      /*
       * First make sure the filesystem supports acls.
       */
      acl_enabled = pathconf(jcr->last_fname, _PC_ACL_ENABLED);
      switch (acl_enabled) {
      case 0:
         Mmsg1(jcr->errmsg, _("Trying to restore acl on file \"%s\" on filesystem without acl support\n"),
               jcr->last_fname);
         return bacl_exit_error;
      case -1:
         switch (errno) {
         case ENOENT:
            return bacl_exit_ok;
         default:
            Mmsg2(jcr->errmsg, _("pathconf error on file \"%s\": ERR=%s\n"),
                  jcr->last_fname, be.bstrerror());
            Dmsg3(100, "pathconf error acl=%s file=%s ERR=%s\n",  
                  jcr->acl_data->content, jcr->last_fname, be.bstrerror());
            return bacl_exit_error;
         }
      default:
         /*
          * On a filesystem with ACL support make sure this particilar ACL type can be restored.
          */
         switch (stream) {
         case STREAM_ACL_SOLARIS_ACLENT:
            /*
             * An aclent can be restored on filesystems with _ACL_ACLENT_ENABLED or _ACL_ACE_ENABLED support.
             */
            if ((acl_enabled & (_ACL_ACLENT_ENABLED | _ACL_ACE_ENABLED)) == 0) {
               Mmsg1(jcr->errmsg, _("Trying to restore acl on file \"%s\" on filesystem without aclent acl support\n"),
                     jcr->last_fname);
               return bacl_exit_error;
            }
            break;
         case STREAM_ACL_SOLARIS_ACE:
            /*
             * An ace can only be restored on a filesystem with _ACL_ACE_ENABLED support.
             */
            if ((acl_enabled & _ACL_ACE_ENABLED) == 0) {
               Mmsg1(jcr->errmsg, _("Trying to restore acl on file \"%s\" on filesystem without ace acl support\n"),
                     jcr->last_fname);
               return bacl_exit_error;
            }
            break;
         default:
            /*
             * Stream id which doesn't describe the type of acl which is encoded.
             */
            break;
         }
         break;
      }

      if ((error = acl_fromtext(jcr->acl_data->content, &aclp)) != 0) {
         Mmsg2(jcr->errmsg, _("acl_fromtext error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, acl_strerror(error));
         Dmsg3(100, "acl_fromtext error acl=%s file=%s ERR=%s\n",  
               jcr->acl_data->content, jcr->last_fname, acl_strerror(error));
         return bacl_exit_error;
      }

      /*
       * Validate that the conversion gave us the correct acl type.
       */
      switch (stream) {
      case STREAM_ACL_SOLARIS_ACLENT:
         if (acl_type(aclp) != ACLENT_T) {
            Mmsg1(jcr->errmsg, _("wrong encoding of acl type in acl stream on file \"%s\"\n"),
                  jcr->last_fname);
            return bacl_exit_error;
         }
         break;
      case STREAM_ACL_SOLARIS_ACE:
         if (acl_type(aclp) != ACE_T) {
            Mmsg1(jcr->errmsg, _("wrong encoding of acl type in acl stream on file \"%s\"\n"),
                  jcr->last_fname);
            return bacl_exit_error;
         }
         break;
      default:
         /*
          * Stream id which doesn't describe the type of acl which is encoded.
          */
         break;
      }

      /*
       * Restore the ACLs, but don't complain about links which really should
       * not have attributes, and the file it is linked to may not yet be restored.
       * This is only true for the old acl streams as in the new implementation we
       * don't save acls of symlinks (which cannot have acls anyhow)
       */
      if ((error = acl_set(jcr->last_fname, aclp)) == -1 && jcr->last_type != FT_LNK) {
         switch (errno) {
         case ENOENT:
            acl_free(aclp);
            return bacl_exit_ok;
         default:
            Mmsg2(jcr->errmsg, _("acl_set error on file \"%s\": ERR=%s\n"),
                  jcr->last_fname, acl_strerror(error));
            Dmsg3(100, "acl_set error acl=%s file=%s ERR=%s\n",  
                  jcr->acl_data->content, jcr->last_fname, acl_strerror(error));
            acl_free(aclp);
            return bacl_exit_error;
         }
      }

      acl_free(aclp);
      return bacl_exit_ok;
   default:
      return bacl_exit_error;
   } /* end switch (stream) */
}

#else /* HAVE_EXTENDED_ACL */

/*
 * Define the supported ACL streams for this OS
 */
static int os_access_acl_streams[2] = { STREAM_ACL_SOLARIS_ACLENT };
static int os_default_acl_streams[1] = { -1 };

/*
 * See if an acl is a trivial one (e.g. just the stat bits encoded as acl.)
 * There is no need to store those acls as we already store the stat bits too.
 */
static bool acl_is_trivial(int count, aclent_t *entries)
{
   int n;
   aclent_t *ace;

   for (n = 0; n < count; n++) {
      ace = &entries[n];

      if (!(ace->a_type == USER_OBJ ||
            ace->a_type == GROUP_OBJ ||
            ace->a_type == OTHER_OBJ ||
            ace->a_type == CLASS_OBJ))
        return false;
   }
   return true;
}

/*
 * OS specific functions for handling different types of acl streams.
 */
static bacl_exit_code solaris_build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   int n;
   aclent_t *acls;
   char *acl_text;
   berrno be;

   n = acl(jcr->last_fname, GETACLCNT, 0, NULL);
   if (n < MIN_ACL_ENTRIES)
      return bacl_exit_error;

   acls = (aclent_t *)malloc(n * sizeof(aclent_t));
   if (acl(jcr->last_fname, GETACL, n, acls) == n) {
      if (acl_is_trivial(n, acls)) {
         /*
          * The ACLs simply reflect the (already known) standard permissions
          * So we don't send an ACL stream to the SD.
          */
         free(acls);
         pm_strcpy(jcr->acl_data->content, "");
         jcr->acl_data->content_length = 0;
         return bacl_exit_ok;
      }

      if ((acl_text = acltotext(acls, n)) != NULL) {
         jcr->acl_data->content_length = pm_strcpy(jcr->acl_data->content, acl_text);
         actuallyfree(acl_text);
         free(acls);
         return send_acl_stream(jcr, STREAM_ACL_SOLARIS_ACLENT);
      }

      Mmsg2(jcr->errmsg, _("acltotext error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "acltotext error acl=%s file=%s ERR=%s\n",  
            jcr->acl_data->content, jcr->last_fname, be.bstrerror());
   }

   free(acls);
   return bacl_exit_error;
}

static bacl_exit_code solaris_parse_acl_streams(JCR *jcr, int stream)
{
   int n;
   aclent_t *acls;
   berrno be;

   acls = aclfromtext(jcr->acl_data->content, &n);
   if (!acls) {
      Mmsg2(jcr->errmsg, _("aclfromtext error on file \"%s\": ERR=%s\n"),
            jcr->last_fname, be.bstrerror());
      Dmsg3(100, "aclfromtext error acl=%s file=%s ERR=%s\n",  
            jcr->acl_data->content, jcr->last_fname, be.bstrerror());
      return bacl_exit_error;
   }

   /*
    * Restore the ACLs, but don't complain about links which really should
    * not have attributes, and the file it is linked to may not yet be restored.
    */
   if (acl(jcr->last_fname, SETACL, n, acls) == -1 && jcr->last_type != FT_LNK) {
      switch (errno) {
      case ENOENT:
         actuallyfree(acls);
         return bacl_exit_ok;
      default:
         Mmsg2(jcr->errmsg, _("acl(SETACL) error on file \"%s\": ERR=%s\n"),
               jcr->last_fname, be.bstrerror());
         Dmsg3(100, "acl(SETACL) error acl=%s file=%s ERR=%s\n",
               jcr->acl_data->content, jcr->last_fname, be.bstrerror());
         actuallyfree(acls);
         return bacl_exit_error;
      }
   }
   actuallyfree(acls);
   return bacl_exit_ok;
}
#endif /* HAVE_EXTENDED_ACL */

/*
 * For this OS setup the build and parse function pointer to the OS specific functions.
 */
static bacl_exit_code (*os_build_acl_streams)(JCR *jcr, FF_PKT *ff_pkt) = solaris_build_acl_streams;
static bacl_exit_code (*os_parse_acl_streams)(JCR *jcr, int stream) = solaris_parse_acl_streams;

#endif /* HAVE_SUN_OS */

/*
 * Entry points when compiled with support for ACLs on a supported platform.
 */

/*
 * Read and send an ACL for the last encountered file.
 */
bacl_exit_code build_acl_streams(JCR *jcr, FF_PKT *ff_pkt)
{
   /*
    * Call the appropriate function.
    */
   if (os_build_acl_streams) {
      return (*os_build_acl_streams)(jcr, ff_pkt);
   }
   return bacl_exit_error;
}

bacl_exit_code parse_acl_streams(JCR *jcr, int stream)
{
   unsigned int cnt;

   switch (stream) {
   case STREAM_UNIX_ACCESS_ACL:
   case STREAM_UNIX_DEFAULT_ACL:
      /*
       * Handle legacy ACL streams.
       */
      if (os_parse_acl_streams) {
         return (*os_parse_acl_streams)(jcr, stream);
      }
      break;
   default:
      if (os_parse_acl_streams) {
         /*
          * Walk the os_access_acl_streams array with the supported Access ACL streams for this OS.
          */
         for (cnt = 0; cnt < sizeof(os_access_acl_streams) / sizeof(int); cnt++) {
            if (os_access_acl_streams[cnt] == stream) {
               return (*os_parse_acl_streams)(jcr, stream);
            }
         }
         /*
          * Walk the os_default_acl_streams array with the supported Default ACL streams for this OS.
          */
         for (cnt = 0; cnt < sizeof(os_default_acl_streams) / sizeof(int); cnt++) {
            if (os_default_acl_streams[cnt] == stream) {
               return (*os_parse_acl_streams)(jcr, stream);
            }
         }
      }
      break;
   }
   Qmsg2(jcr, M_WARNING, 0,
      _("Can't restore ACLs of %s - incompatible acl stream encountered - %d\n"),
      jcr->last_fname, stream);
   return bacl_exit_error;
}
#endif
