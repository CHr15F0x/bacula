/*
 * Bacula Catalog Database Update record interface routines
 * 
 *    Kern Sibbald, March 2000
 *
 *    Version $Id$
 */

/*
   Copyright (C) 2000-2003 Kern Sibbald and John Walker

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

/* The following is necessary so that we do not include
 * the dummy external definition of DB.
 */
#define __SQL_C 		      /* indicate that this is sql.c */

#include "bacula.h"
#include "cats.h"

#if    HAVE_MYSQL || HAVE_SQLITE || HAVE_POSTGRESQL

/* -----------------------------------------------------------------------
 *
 *   Generic Routines (or almost generic)
 *
 * -----------------------------------------------------------------------
 */

/* Imported subroutines */
extern void print_result(B_DB *mdb);
extern int UpdateDB(char *file, int line, JCR *jcr, B_DB *db, char *update_cmd);

/* -----------------------------------------------------------------------
 *
 *   Generic Routines (or almost generic)
 *
 * -----------------------------------------------------------------------
 */
/* Update the attributes record by adding the MD5 signature */
int
db_add_SIG_to_file_record(JCR *jcr, B_DB *mdb, FileId_t FileId, char *SIG,
			  int type)
{
   int stat;

   db_lock(mdb);
   Mmsg(&mdb->cmd, "UPDATE File SET MD5='%s' WHERE FileId=%u", SIG, FileId);
   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}

/* Mark the file record as being visited during database
 * verify compare. Stuff JobId into MarkedId field
 */
int db_mark_file_record(JCR *jcr, B_DB *mdb, FileId_t FileId, JobId_t JobId) 
{
   int stat;

   db_lock(mdb);
   Mmsg(&mdb->cmd, "UPDATE File SET MarkId=%u WHERE FileId=%u", JobId, FileId);
   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}

/*
 * Update the Job record at end of Job
 *
 *  Returns: 0 on failure
 *	     1 on success
 */
int
db_update_job_start_record(JCR *jcr, B_DB *mdb, JOB_DBR *jr)
{
   char dt[MAX_TIME_LENGTH];
   time_t stime;
   struct tm tm;
   btime_t JobTDate;
   int stat;
   char ed1[30];
       
   stime = jr->StartTime;
   localtime_r(&stime, &tm);
   strftime(dt, sizeof(dt), "%Y-%m-%d %T", &tm);
   JobTDate = (btime_t)stime;

   db_lock(mdb);
   Mmsg(&mdb->cmd, "UPDATE Job SET Level='%c', StartTime='%s',"
"ClientId=%u, JobTDate=%s WHERE JobId=%u",
      (char)(jr->Level), dt, jr->ClientId, edit_uint64(JobTDate, ed1), jr->JobId);

   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   mdb->changes = 0;
   return stat;
}



/*
 * Update the Job record at end of Job
 *
 *  Returns: 0 on failure
 *	     1 on success
 */
int
db_update_job_end_record(JCR *jcr, B_DB *mdb, JOB_DBR *jr)
{
   char dt[MAX_TIME_LENGTH];
   time_t ttime;
   struct tm tm;
   int stat;
   char ed1[30], ed2[30];
   btime_t JobTDate;
       
   ttime = jr->EndTime;
   localtime_r(&ttime, &tm);
   strftime(dt, sizeof(dt), "%Y-%m-%d %T", &tm);
   JobTDate = ttime;

   db_lock(mdb);
   Mmsg(&mdb->cmd,
      "UPDATE Job SET JobStatus='%c', EndTime='%s', \
ClientId=%u, JobBytes=%s, JobFiles=%u, JobErrors=%u, VolSessionId=%u, \
VolSessionTime=%u, PoolId=%u, FileSetId=%u, JobTDate=%s WHERE JobId=%u",
      (char)(jr->JobStatus), dt, jr->ClientId, edit_uint64(jr->JobBytes, ed1), 
      jr->JobFiles, jr->JobErrors, jr->VolSessionId, jr->VolSessionTime, 
      jr->PoolId, jr->FileSetId, edit_uint64(JobTDate, ed2), jr->JobId);

   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}


/*
 * Update Client record 
 *   Returns: 0 on failure
 *	      1 on success
 */
int
db_update_client_record(JCR *jcr, B_DB *mdb, CLIENT_DBR *cr)
{
   int stat;
   char ed1[50], ed2[50];
   CLIENT_DBR tcr;

   db_lock(mdb);
   memcpy(&tcr, cr, sizeof(tcr));
   if (!db_create_client_record(jcr, mdb, &tcr)) {
      db_unlock(mdb);
      return 0;
   }

   Mmsg(&mdb->cmd,
"UPDATE Client SET AutoPrune=%d,FileRetention=%s,JobRetention=%s," 
"Uname='%s' WHERE Name='%s'",
      cr->AutoPrune,
      edit_uint64(cr->FileRetention, ed1),
      edit_uint64(cr->JobRetention, ed2),
      cr->Uname, cr->Name);

   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}


/*
 * Update Counters record
 *   Returns: 0 on failure
 *	      1 on success
 */
int db_update_counter_record(JCR *jcr, B_DB *mdb, COUNTER_DBR *cr)
{
   db_lock(mdb);

   Mmsg(&mdb->cmd,
"UPDATE Counters SET MinValue=%d,MaxValue=%d,CurrentValue=%d," 
"WrapCounter='%s' WHERE Counter='%s'",
      cr->MinValue, cr->MaxValue, cr->CurrentValue,
      cr->WrapCounter, cr->Counter);

   int stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}


int
db_update_pool_record(JCR *jcr, B_DB *mdb, POOL_DBR *pr)
{
   int stat;
   char ed1[50], ed2[50], ed3[50];

   db_lock(mdb);
   Mmsg(&mdb->cmd,
"UPDATE Pool SET NumVols=%u,MaxVols=%u,UseOnce=%d,UseCatalog=%d," 
"AcceptAnyVolume=%d,VolRetention='%s',VolUseDuration='%s',"
"MaxVolJobs=%u,MaxVolFiles=%u,MaxVolBytes=%s,Recycle=%d,"
"AutoPrune=%d,LabelFormat='%s' WHERE PoolId=%u",
      pr->NumVols, pr->MaxVols, pr->UseOnce, pr->UseCatalog,
      pr->AcceptAnyVolume, edit_uint64(pr->VolRetention, ed1),
      edit_uint64(pr->VolUseDuration, ed2),
      pr->MaxVolJobs, pr->MaxVolFiles,
      edit_uint64(pr->MaxVolBytes, ed3),
      pr->Recycle, pr->AutoPrune,
      pr->LabelFormat, pr->PoolId);

   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}

/* 
 * Update the Media Record at end of Session
 *
 * Returns: 0 on failure
 *	    numrows on success
 */
int
db_update_media_record(JCR *jcr, B_DB *mdb, MEDIA_DBR *mr) 
{
   char dt[MAX_TIME_LENGTH];
   time_t ttime;
   struct tm tm;
   int stat;
   char ed1[30], ed2[30];
       

   Dmsg1(100, "update_media: FirstWritten=%d\n", mr->FirstWritten);
   db_lock(mdb);
   if (mr->VolJobs == 1) {
      Dmsg1(400, "Set FirstWritten Vol=%s\n", mr->VolumeName);
      ttime = mr->FirstWritten;
      localtime_r(&ttime, &tm);
      strftime(dt, sizeof(dt), "%Y-%m-%d %T", &tm);
      Mmsg(&mdb->cmd, "UPDATE Media SET FirstWritten='%s'\
 WHERE VolumeName='%s'", dt, mr->VolumeName);
      stat = UPDATE_DB(jcr, mdb, mdb->cmd);
      Dmsg1(400, "Firstwritten stat=%d\n", stat);
   }

   /* Label just done? */
   if (mr->VolBytes == 1) {
      ttime = mr->LabelDate;
      if (ttime == 0) {
	 ttime = time(NULL);
      }
      localtime_r(&ttime, &tm);
      strftime(dt, sizeof(dt), "%Y-%m-%d %T", &tm);
      Mmsg(&mdb->cmd, "UPDATE Media SET LabelDate='%s' "
           "WHERE VolumeName='%s'", dt, mr->VolumeName);
      stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   }
   
   /* Make sure InChanger is 0 for any record having the same Slot */
   db_make_inchanger_unique(jcr, mdb, mr);

   ttime = mr->LastWritten;
   localtime_r(&ttime, &tm);
   strftime(dt, sizeof(dt), "%Y-%m-%d %T", &tm);

   Mmsg(&mdb->cmd, "UPDATE Media SET VolJobs=%u,"
        "VolFiles=%u,VolBlocks=%u,VolBytes=%s,VolMounts=%u,VolErrors=%u,"
        "VolWrites=%u,MaxVolBytes=%s,LastWritten='%s',VolStatus='%s',"
        "Slot=%d,Drive=%d,InChanger=%d WHERE VolumeName='%s'",
	 mr->VolJobs, mr->VolFiles, mr->VolBlocks, edit_uint64(mr->VolBytes, ed1),
	 mr->VolMounts, mr->VolErrors, mr->VolWrites, 
	 edit_uint64(mr->MaxVolBytes, ed2), dt, 
	 mr->VolStatus, mr->Slot, mr->Drive, mr->InChanger, mr->VolumeName);

   Dmsg1(400, "%s\n", mdb->cmd);

   stat = UPDATE_DB(jcr, mdb, mdb->cmd);
   db_unlock(mdb);
   return stat;
}

/* 
 * If we have a non-zero InChanger, ensure that no other Media
 *  record in this Pool has InChanger set on the same Slot.
 *
 * This routine assumes the database is already locked.
 */
void
db_make_inchanger_unique(JCR *jcr, B_DB *mdb, MEDIA_DBR *mr) 
{
   if (mr->InChanger != 0 && mr->Slot != 0) {
      Mmsg(&mdb->cmd, "UPDATE Media SET InChanger=0 WHERE PoolId=%u "
           "AND Slot=%d\n", mr->PoolId, mr->Slot);
      Dmsg1(400, "%s\n", mdb->cmd);
      UPDATE_DB(jcr, mdb, mdb->cmd);
   }
}

#endif /* HAVE_MYSQL || HAVE_SQLITE || HAVE_POSTGRESQL*/
