/*
 * Bacula Catalog Database Find record interface routines
 *
 *  Note, generally, these routines are more complicated
 *        that a simple search by name or id. Such simple
 *        request are in get.c
 *
 *    Kern Sibbald, December 2000
 *
 *    Version $Id$
 */
/*
   Copyright (C) 2000-2005 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as amended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */



/* The following is necessary so that we do not include
 * the dummy external definition of DB.
 */
#define __SQL_C                       /* indicate that this is sql.c */

#include "bacula.h"
#include "cats.h"

#if    HAVE_SQLITE3 || HAVE_MYSQL || HAVE_SQLITE || HAVE_POSTGRESQL

/* -----------------------------------------------------------------------
 *
 *   Generic Routines (or almost generic)
 *
 * -----------------------------------------------------------------------
 */

/* Imported subroutines */
extern void print_result(B_DB *mdb);
extern int QueryDB(const char *file, int line, JCR *jcr, B_DB *db, char *select_cmd);

/*
 * Find job start time if JobId specified, otherwise
 * find last full save for Incremental and Differential saves.
 *
 *  StartTime is returned in stime
 *
 * Returns: 0 on failure
 *          1 on success, jr is unchanged, but stime is set
 */
bool
db_find_job_start_time(JCR *jcr, B_DB *mdb, JOB_DBR *jr, POOLMEM **stime)
{
   SQL_ROW row;
   char ed1[50], ed2[50];

   db_lock(mdb);

   pm_strcpy(stime, "0000-00-00 00:00:00");   /* default */
   /* If no Id given, we must find corresponding job */
   if (jr->JobId == 0) {
      /* Differential is since last Full backup */
         Mmsg(mdb->cmd,
"SELECT StartTime FROM Job WHERE JobStatus='T' AND Type='%c' AND "
"Level='%c' AND Name='%s' AND ClientId=%s AND FileSetId=%s "
"ORDER BY StartTime DESC LIMIT 1",
           jr->JobType, L_FULL, jr->Name, 
           edit_int64(jr->ClientId, ed1), edit_int64(jr->FileSetId, ed2));

      if (jr->JobLevel == L_DIFFERENTIAL) {
         /* SQL cmd for Differential backup already edited above */

      /* Incremental is since last Full, Incremental, or Differential */
      } else if (jr->JobLevel == L_INCREMENTAL) {
         /*
          * For an Incremental job, we must first ensure
          *  that a Full backup was done (cmd edited above)
          *  then we do a second look to find the most recent
          *  backup
          */
         if (!QUERY_DB(jcr, mdb, mdb->cmd)) {
            Mmsg2(&mdb->errmsg, _("Query error for start time request: ERR=%s\nCMD=%s\n"),
               sql_strerror(mdb), mdb->cmd);
            goto bail_out;
         }
         if ((row = sql_fetch_row(mdb)) == NULL) {
            sql_free_result(mdb);
            Mmsg(mdb->errmsg, _("No prior Full backup Job record found.\n"));
            goto bail_out;
         }
         sql_free_result(mdb);
         /* Now edit SQL command for Incremental Job */
         Mmsg(mdb->cmd,
"SELECT StartTime FROM Job WHERE JobStatus='T' AND Type='%c' AND "
"Level IN ('%c','%c','%c') AND Name='%s' AND ClientId=%s "
"AND FileSetId=%s ORDER BY StartTime DESC LIMIT 1",
            jr->JobType, L_INCREMENTAL, L_DIFFERENTIAL, L_FULL, jr->Name,
            edit_int64(jr->ClientId, ed1), edit_int64(jr->FileSetId, ed2));
      } else {
         Mmsg1(mdb->errmsg, _("Unknown level=%d\n"), jr->JobLevel);
         goto bail_out;
      }
   } else {
      Dmsg1(100, "Submitting: %s\n", mdb->cmd);
      Mmsg(mdb->cmd, "SELECT StartTime FROM Job WHERE Job.JobId=%s", 
           edit_int64(jr->JobId, ed1));
   }

   if (!QUERY_DB(jcr, mdb, mdb->cmd)) {
      pm_strcpy(stime, "");                   /* set EOS */
      Mmsg2(&mdb->errmsg, _("Query error for start time request: ERR=%s\nCMD=%s\n"),
         sql_strerror(mdb),  mdb->cmd);
      goto bail_out;
   }

   if ((row = sql_fetch_row(mdb)) == NULL) {
      Mmsg2(&mdb->errmsg, _("No Job record found: ERR=%s\nCMD=%s\n"),
         sql_strerror(mdb),  mdb->cmd);
      sql_free_result(mdb);
      goto bail_out;
   }
   Dmsg1(100, "Got start time: %s\n", row[0]);
   pm_strcpy(stime, row[0]);

   sql_free_result(mdb);

   db_unlock(mdb);
   return true;

bail_out:
   db_unlock(mdb);
   return false;
}

/*
 * Find last failed job since given start-time
 *   it must be either Full or Diff.
 *
 * Returns: false on failure
 *          true  on success, jr is unchanged and stime unchanged
 *                level returned in JobLevel
 */
bool
db_find_failed_job_since(JCR *jcr, B_DB *mdb, JOB_DBR *jr, POOLMEM *stime, int &JobLevel)
{
   SQL_ROW row;
   char ed1[50], ed2[50];

   db_lock(mdb);
   /* Differential is since last Full backup */
   Mmsg(mdb->cmd,
"SELECT Level FROM Job WHERE JobStatus!='T' AND Type='%c' AND "
"Level IN ('%c','%c') AND Name='%s' AND ClientId=%s "
"AND FileSetId=%s AND StartTime>'%s' "
"ORDER BY StartTime DESC LIMIT 1",
         jr->JobType, L_FULL, L_DIFFERENTIAL, jr->Name,
         edit_int64(jr->ClientId, ed1), edit_int64(jr->FileSetId, ed2),
         stime);

   if (!QUERY_DB(jcr, mdb, mdb->cmd)) {
      db_unlock(mdb);
      return false;
   }

   if ((row = sql_fetch_row(mdb)) == NULL) {
      sql_free_result(mdb);
      db_unlock(mdb);
      return false;
   }
   JobLevel = (int)*row[0];
   sql_free_result(mdb);

   db_unlock(mdb);
   return true;
}


/*
 * Find JobId of last job that ran.  E.g. for
 *   VERIFY_CATALOG we want the JobId of the last INIT.
 *   For VERIFY_VOLUME_TO_CATALOG, we want the JobId of the last Job.
 *
 * Returns: true  on success
 *          false on failure
 */
bool
db_find_last_jobid(JCR *jcr, B_DB *mdb, const char *Name, JOB_DBR *jr)
{
   SQL_ROW row;
   char ed1[50];

   /* Find last full */
   db_lock(mdb);
   Dmsg2(100, "JobLevel=%d JobType=%d\n", jr->JobLevel, jr->JobType);
   if (jr->JobLevel == L_VERIFY_CATALOG) {
      Mmsg(mdb->cmd,
"SELECT JobId FROM Job WHERE Type='V' AND Level='%c' AND "
" JobStatus='T' AND Name='%s' AND "
"ClientId=%s ORDER BY StartTime DESC LIMIT 1",
           L_VERIFY_INIT, jr->Name, 
           edit_int64(jr->ClientId, ed1));
   } else if (jr->JobLevel == L_VERIFY_VOLUME_TO_CATALOG ||
              jr->JobLevel == L_VERIFY_DISK_TO_CATALOG ||
              jr->JobType == JT_BACKUP) {
      if (Name) {
         Mmsg(mdb->cmd,
"SELECT JobId FROM Job WHERE Type='B' AND JobStatus='T' AND "
"Name='%s' ORDER BY StartTime DESC LIMIT 1", Name);
      } else {
         Mmsg(mdb->cmd,
"SELECT JobId FROM Job WHERE Type='B' AND JobStatus='T' AND "
"ClientId=%s ORDER BY StartTime DESC LIMIT 1", 
           edit_int64(jr->ClientId, ed1));
      }
   } else {
      Mmsg1(&mdb->errmsg, _("Unknown Job level=%d\n"), jr->JobLevel);
      db_unlock(mdb);
      return false;
   }
   Dmsg1(100, "Query: %s\n", mdb->cmd);
   if (!QUERY_DB(jcr, mdb, mdb->cmd)) {
      db_unlock(mdb);
      return false;
   }
   if ((row = sql_fetch_row(mdb)) == NULL) {
      Mmsg1(&mdb->errmsg, _("No Job found for: %s.\n"), mdb->cmd);
      sql_free_result(mdb);
      db_unlock(mdb);
      return false;
   }

   jr->JobId = str_to_int64(row[0]);
   sql_free_result(mdb);

   Dmsg1(100, "db_get_last_jobid: got JobId=%d\n", jr->JobId);
   if (jr->JobId <= 0) {
      Mmsg1(&mdb->errmsg, _("No Job found for: %s\n"), mdb->cmd);
      db_unlock(mdb);
      return false;
   }

   db_unlock(mdb);
   return true;
}

/*
 * Find Available Media (Volume) for Pool
 *
 * Find a Volume for a given PoolId, MediaType, and Status.
 *
 * Returns: 0 on failure
 *          numrows on success
 */
int
db_find_next_volume(JCR *jcr, B_DB *mdb, int item, bool InChanger, MEDIA_DBR *mr)
{
   SQL_ROW row;
   int numrows;
   const char *order;

   char ed1[50];

   db_lock(mdb);
   if (item == -1) {       /* find oldest volume */
      /* Find oldest volume */
      Mmsg(mdb->cmd, "SELECT MediaId,VolumeName,VolJobs,VolFiles,VolBlocks,"
          "VolBytes,VolMounts,VolErrors,VolWrites,MaxVolBytes,VolCapacityBytes,"
          "VolRetention,VolUseDuration,MaxVolJobs,MaxVolFiles,Recycle,Slot,"
          "FirstWritten,LastWritten,VolStatus,InChanger,VolParts,"
          "LabelType "
          "FROM Media WHERE PoolId=%s AND MediaType='%s' AND VolStatus IN ('Full',"
          "'Recycle','Purged','Used','Append') "
          "ORDER BY LastWritten LIMIT 1", 
          edit_int64(mr->PoolId, ed1), mr->MediaType);
     item = 1;
   } else {
      char changer[100];
      /* Find next available volume */
      if (InChanger) {
         bsnprintf(changer, sizeof(changer), "AND InChanger=1 AND StorageId=%s",
            edit_int64(mr->StorageId, ed1));
      } else {
         changer[0] = 0;
      }
      if (strcmp(mr->VolStatus, "Recycled") == 0 ||
          strcmp(mr->VolStatus, "Purged") == 0) {
         order = "ORDER BY LastWritten ASC,MediaId";  /* take oldest */
      } else {
         order = "ORDER BY LastWritten IS NULL,LastWritten DESC,MediaId";   /* take most recently written */
      }
      Mmsg(mdb->cmd, "SELECT MediaId,VolumeName,VolJobs,VolFiles,VolBlocks,"
          "VolBytes,VolMounts,VolErrors,VolWrites,MaxVolBytes,VolCapacityBytes,"
          "VolRetention,VolUseDuration,MaxVolJobs,MaxVolFiles,Recycle,Slot,"
          "FirstWritten,LastWritten,VolStatus,InChanger,VolParts,"
          "LabelType "
          "FROM Media WHERE PoolId=%s AND MediaType='%s' AND VolStatus='%s' "
          "%s "
          "%s LIMIT %d",
          edit_int64(mr->PoolId, ed1), mr->MediaType,
          mr->VolStatus, changer, order, item);
   }
   if (!QUERY_DB(jcr, mdb, mdb->cmd)) {
      db_unlock(mdb);
      return 0;
   }

   numrows = sql_num_rows(mdb);
   if (item > numrows) {
      Mmsg2(&mdb->errmsg, _("Request for Volume item %d greater than max %d\n"),
         item, numrows);
      db_unlock(mdb);
      return 0;
   }

   /* Seek to desired item
    * Note, we use base 1; SQL uses base 0
    */
   sql_data_seek(mdb, item-1);

   if ((row = sql_fetch_row(mdb)) == NULL) {
      Mmsg1(&mdb->errmsg, _("No Volume record found for item %d.\n"), item);
      sql_free_result(mdb);
      db_unlock(mdb);
      return 0;
   }

   /* Return fields in Media Record */
   mr->MediaId = str_to_int64(row[0]);
   bstrncpy(mr->VolumeName, row[1], sizeof(mr->VolumeName));
   mr->VolJobs = str_to_int64(row[2]);
   mr->VolFiles = str_to_int64(row[3]);
   mr->VolBlocks = str_to_int64(row[4]);
   mr->VolBytes = str_to_uint64(row[5]);
   mr->VolMounts = str_to_int64(row[6]);
   mr->VolErrors = str_to_int64(row[7]);
   mr->VolWrites = str_to_int64(row[8]);
   mr->MaxVolBytes = str_to_uint64(row[9]);
   mr->VolCapacityBytes = str_to_uint64(row[10]);
   mr->VolRetention = str_to_uint64(row[11]);
   mr->VolUseDuration = str_to_uint64(row[12]);
   mr->MaxVolJobs = str_to_int64(row[13]);
   mr->MaxVolFiles = str_to_int64(row[14]);
   mr->Recycle = str_to_int64(row[15]);
   mr->Slot = str_to_int64(row[16]);
   bstrncpy(mr->cFirstWritten, row[17]!=NULL?row[17]:"", sizeof(mr->cFirstWritten));
   mr->FirstWritten = (time_t)str_to_utime(mr->cFirstWritten);
   bstrncpy(mr->cLastWritten, row[18]!=NULL?row[18]:"", sizeof(mr->cLastWritten));
   mr->LastWritten = (time_t)str_to_utime(mr->cLastWritten);
   bstrncpy(mr->VolStatus, row[19], sizeof(mr->VolStatus));
   mr->InChanger = str_to_int64(row[20]);
   mr->VolParts = str_to_int64(row[21]);
   mr->LabelType = str_to_int64(row[22]);
   sql_free_result(mdb);

   db_unlock(mdb);
   return numrows;
}


#endif /* HAVE_SQLITE3 || HAVE_MYSQL || HAVE_SQLITE || HAVE_POSTGRESQL*/
