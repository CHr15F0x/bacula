/*
 *
 *   Bacula Director -- newvol.c -- creates new Volumes in
 *    catalog Media table from the LabelFormat specification.
 *
 *     Kern Sibbald, May MMI
 *
 *    This routine runs as a thread and must be thread reentrant.
 *
 *  Basic tasks done here:
 *	If possible create a new Media entry
 *
 *   Version $Id$
 */
/*
   Copyright (C) 2001-2005 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

 */

#include "bacula.h"
#include "dird.h"

/* Forward referenced functions */
static int create_simple_name(JCR *jcr, MEDIA_DBR *mr, POOL_DBR *pr);
static int perform_full_name_substitution(JCR *jcr, MEDIA_DBR *mr, POOL_DBR *pr);


/*
 * Automatic Volume name creation using the LabelFormat
 */
bool newVolume(JCR *jcr, MEDIA_DBR *mr)
{
   POOL_DBR pr;

   memset(&pr, 0, sizeof(pr));

   /* See if we can create a new Volume */
   db_lock(jcr->db);
   pr.PoolId = jcr->PoolId;
   if (db_get_pool_record(jcr, jcr->db, &pr) && pr.LabelFormat[0] &&
       pr.LabelFormat[0] != '*') {
      if (pr.MaxVols == 0 || pr.NumVols < pr.MaxVols) {
	 memset(mr, 0, sizeof(MEDIA_DBR));
	 set_pool_dbr_defaults_in_media_dbr(mr, &pr);
	 jcr->VolumeName[0] = 0;
	 bstrncpy(mr->MediaType, jcr->store->media_type, sizeof(mr->MediaType));
         if (generate_event(jcr, "NewVolume") == 1 && jcr->VolumeName[0]) {
	    bstrncpy(mr->VolumeName, jcr->VolumeName, sizeof(mr->VolumeName));
	 /* Check for special characters */
	 } else if (is_volume_name_legal(NULL, pr.LabelFormat)) {
	    /* No special characters, so apply simple algorithm */
	    if (!create_simple_name(jcr, mr, &pr)) {
	       goto bail_out;
	    }
	 } else {  /* try full substitution */
	    /* Found special characters, so try substitution */
	    if (!perform_full_name_substitution(jcr, mr, &pr)) {
	       goto bail_out;
	    }
	    if (!is_volume_name_legal(NULL, mr->VolumeName)) {
               Jmsg(jcr, M_ERROR, 0, _("Illegal character in Volume name \"%s\"\n"),
		  mr->VolumeName);
	       goto bail_out;
	    }
	 }
	 pr.NumVols++;
	 if (db_create_media_record(jcr, jcr->db, mr) &&
	    db_update_pool_record(jcr, jcr->db, &pr)) {
	    db_unlock(jcr->db);
            Jmsg(jcr, M_INFO, 0, _("Created new Volume \"%s\" in catalog.\n"), mr->VolumeName);
            Dmsg1(90, "Created new Volume=%s\n", mr->VolumeName);
	    return true;
	 } else {
            Jmsg(jcr, M_ERROR, 0, "%s", db_strerror(jcr->db));
	 }
      }
   }
bail_out:
   db_unlock(jcr->db);
   return false;
}

static int create_simple_name(JCR *jcr, MEDIA_DBR *mr, POOL_DBR *pr)
{
   char name[MAXSTRING];
   char num[20];

   /* See if volume already exists */
   mr->VolumeName[0] = 0;
   bstrncpy(name, pr->LabelFormat, sizeof(name));
   for (int i=pr->NumVols+1; i<(int)pr->NumVols+11; i++) {
      MEDIA_DBR tmr;
      memset(&tmr, 0, sizeof(tmr));
      sprintf(num, "%04d", i);
      bstrncpy(tmr.VolumeName, name, sizeof(tmr.VolumeName));
      bstrncat(tmr.VolumeName, num, sizeof(tmr.VolumeName));
      if (db_get_media_record(jcr, jcr->db, &tmr)) {
	 Jmsg(jcr, M_WARNING, 0,
             _("Wanted to create Volume \"%s\", but it already exists. Trying again.\n"),
	     tmr.VolumeName);
	 continue;
      }
      bstrncpy(mr->VolumeName, name, sizeof(mr->VolumeName));
      bstrncat(mr->VolumeName, num, sizeof(mr->VolumeName));
      break;			/* Got good name */
   }
   if (mr->VolumeName[0] == 0) {
      Jmsg(jcr, M_ERROR, 0, _("Too many failures. Giving up creating Volume name.\n"));
      return 0;
   }
   return 1;
}

/*
 * Perform full substitution on Label
 */
static int perform_full_name_substitution(JCR *jcr, MEDIA_DBR *mr, POOL_DBR *pr)
{
   int stat = 0;
   POOLMEM *label = get_pool_memory(PM_FNAME);
   jcr->NumVols = pr->NumVols;
   if (variable_expansion(jcr, pr->LabelFormat, &label)) {
      bstrncpy(mr->VolumeName, label, sizeof(mr->VolumeName));
      stat = 1;
   }
   free_pool_memory(label);
   return stat;
}
