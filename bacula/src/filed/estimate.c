/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2001-2008 Free Software Foundation Europe e.V.

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
 *  Bacula File Daemon estimate.c
 *   Make and estimate of the number of files and size to be saved.
 *
 *    Kern Sibbald, September MMI
 *
 *   Version $Id$
 *
 */

#include "bacula.h"
#include "filed.h"

static int tally_file(JCR *jcr, FF_PKT *ff_pkt, bool);

/*
 * Find all the requested files and count them.
 */
int make_estimate(JCR *jcr)
{
   int stat;

   set_jcr_job_status(jcr, JS_Running);

   set_find_options((FF_PKT *)jcr->ff, jcr->incremental, jcr->mtime);
   /* in accurate mode, we overwrite the find_one check function */
   if (jcr->accurate) {
      set_find_changed_function((FF_PKT *)jcr->ff, accurate_check_file);
   } 

   stat = find_files(jcr, (FF_PKT *)jcr->ff, tally_file, NULL);

   accurate_free(jcr);
   return stat;
}

/*
 * Called here by find() for each file included.
 *
 */
static int tally_file(JCR *jcr, FF_PKT *ff_pkt, bool top_level) 
{
   ATTR attr;

   if (job_canceled(jcr)) {
      return 0;
   }
   switch (ff_pkt->type) {
   case FT_LNKSAVED:                  /* Hard linked, file already saved */
   case FT_REGE:
   case FT_REG:
   case FT_LNK:
   case FT_NORECURSE:
   case FT_NOFSCHG:
   case FT_INVALIDFS:
   case FT_INVALIDDT:
   case FT_REPARSE:
   case FT_DIREND:
   case FT_SPEC:
   case FT_RAW:
   case FT_FIFO:
      break;
   case FT_DIRBEGIN:
   case FT_NOACCESS:
   case FT_NOFOLLOW:
   case FT_NOSTAT:
   case FT_DIRNOCHG:
   case FT_NOCHG:
   case FT_ISARCH:
   case FT_NOOPEN:
   default:
      return 1;
   }

   if (ff_pkt->type != FT_LNKSAVED && S_ISREG(ff_pkt->statp.st_mode)) {
      if (ff_pkt->statp.st_size > 0) {
         jcr->JobBytes += ff_pkt->statp.st_size;
      }
#ifdef HAVE_DARWIN_OS
      if (ff_pkt->flags & FO_HFSPLUS) {
         if (ff_pkt->hfsinfo.rsrclength > 0) {
            jcr->JobBytes += ff_pkt->hfsinfo.rsrclength;
         }
         jcr->JobBytes += 32;    /* Finder info */
      }
#endif
   }
   jcr->num_files_examined++;
   jcr->JobFiles++;                  /* increment number of files seen */
   if (jcr->listing) {
      memcpy(&attr.statp, &ff_pkt->statp, sizeof(struct stat));
      attr.type = ff_pkt->type;
      attr.ofname = (POOLMEM *)ff_pkt->fname;
      attr.olname = (POOLMEM *)ff_pkt->link;
      print_ls_output(jcr, &attr);
   }
   return 1;
}
