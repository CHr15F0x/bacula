/*
 *
 *   Bacula Director -- migrate.c -- responsible for doing
 *     migration jobs.
 *
 *     Kern Sibbald, September MMIV
 *
 *  Basic tasks done here:
 *     Open DB and create records for this job.
 *     Open Message Channel with Storage daemon to tell him a job will be starting.
 *     Open connection with Storage daemon and pass him commands
 *       to do the backup.
 *     When the Storage daemon finishes the job, update the DB.
 *
 *   Version $Id$
 */
/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2004-2006 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation plus additions
   that are listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of John Walker.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/

#include "bacula.h"
#include "dird.h"
#include "ua.h"
#ifndef HAVE_REGEX_H
#include "lib/bregex.h"
#else
#include <regex.h>
#endif

static const int dbglevel = 10;

static char OKbootstrap[] = "3000 OK bootstrap\n";
static bool get_job_to_migrate(JCR *jcr);
struct idpkt;
static bool regex_find_jobids(JCR *jcr, idpkt *ids, const char *query1,
                 const char *query2, const char *type);
static bool find_mediaid_then_jobids(JCR *jcr, idpkt *ids, const char *query1,
                 const char *type);
static bool find_jobids_from_mediaid_list(JCR *jcr, idpkt *ids, const char *type);
static void start_migration_job(JCR *jcr);
static int get_next_dbid_from_list(char **p, DBId_t *DBId);

/* 
 * Called here before the job is run to do the job
 *   specific setup.  Note, one of the important things to
 *   complete in this init code is to make the definitive
 *   choice of input and output storage devices.  This is
 *   because immediately after the init, the job is queued
 *   in the jobq.c code, and it checks that all the resources
 *   (storage resources in particular) are available, so these
 *   must all be properly defined.
 *
 *  previous_jr refers to the job DB record of the Job that is
 *    going to be migrated.
 *  prev_job refers to the job resource of the Job that is
 *    going to be migrated.
 *  jcr is the jcr for the current "migration" job.  It is a
 *    control job that is put in the DB as a migration job, which
 *    means that this job migrated a previous job to a new job.
 *    No Volume or File data is associated with this control
 *    job.
 *  mig_jcr refers to the newly migrated job that is run by
 *    the current jcr.  It is a backup job that moves (migrates) the
 *    data written for the previous_jr into the new pool.  This
 *    job (mig_jcr) becomes the new backup job that replaces
 *    the original backup job.
 */
bool do_migration_init(JCR *jcr)
{
   POOL_DBR pr;
   POOL *pool;
   char ed1[100];
   JOB *job, *prev_job;
   JCR *mig_jcr;                   /* newly migrated job */

   /* If we find a job or jobs to migrate it is previous_jr.JobId */
   if (!get_job_to_migrate(jcr)) {
      return false;
   }
   Dmsg1(dbglevel, "Back from get_job_to_migrate JobId=%d\n", (int)jcr->JobId);

   if (jcr->previous_jr.JobId == 0) {
      Dmsg1(dbglevel, "JobId=%d no previous JobId\n", (int)jcr->JobId);
      Jmsg(jcr, M_INFO, 0, _("No previous Job found to migrate.\n"));
      return true;                    /* no work */
   }

   if (!get_or_create_fileset_record(jcr)) {
      Dmsg1(dbglevel, "JobId=%d no FileSet\n", (int)jcr->JobId);
      Jmsg(jcr, M_FATAL, 0, _("Could not get or create the FileSet record.\n"));
      return false;
   }

   apply_pool_overrides(jcr);

   jcr->jr.PoolId = get_or_create_pool_record(jcr, jcr->pool->hdr.name);
   if (jcr->jr.PoolId == 0) {
      Dmsg1(dbglevel, "JobId=%d no PoolId\n", (int)jcr->JobId);
      Jmsg(jcr, M_FATAL, 0, _("Could not get or create a Pool record.\n"));
      return false;
   }

   create_restore_bootstrap_file(jcr);

   if (jcr->previous_jr.JobId == 0 || jcr->ExpectedFiles == 0) {
      set_jcr_job_status(jcr, JS_Terminated);
      Dmsg1(dbglevel, "JobId=%d expected files == 0\n", (int)jcr->JobId);
      if (jcr->previous_jr.JobId == 0) {
         Jmsg(jcr, M_INFO, 0, _("No previous Job found to migrate.\n"));
      } else {
         Jmsg(jcr, M_INFO, 0, _("Previous Job has no data to migrate.\n"));
      }
      return true;                    /* no work */
   }

   Dmsg5(dbglevel, "JobId=%d: Previous: Name=%s JobId=%d Type=%c Level=%c\n",
      (int)jcr->JobId,
      jcr->previous_jr.Name, (int)jcr->previous_jr.JobId, 
      jcr->previous_jr.JobType, jcr->previous_jr.JobLevel);

   Dmsg5(dbglevel, "JobId=%d: Current: Name=%s JobId=%d Type=%c Level=%c\n",
      (int)jcr->JobId,
      jcr->jr.Name, (int)jcr->jr.JobId, 
      jcr->jr.JobType, jcr->jr.JobLevel);

   LockRes();
   job = (JOB *)GetResWithName(R_JOB, jcr->jr.Name);
   prev_job = (JOB *)GetResWithName(R_JOB, jcr->previous_jr.Name);
   UnlockRes();
   if (!job) {
      Jmsg(jcr, M_FATAL, 0, _("Job resource not found for \"%s\".\n"), jcr->jr.Name);
      return false;
   }
   if (!prev_job) {
      Jmsg(jcr, M_FATAL, 0, _("Previous Job resource not found for \"%s\".\n"), 
           jcr->previous_jr.Name);
      return false;
   }

   /* Create a migation jcr */
   mig_jcr = jcr->mig_jcr = new_jcr(sizeof(JCR), dird_free_jcr);
   memcpy(&mig_jcr->previous_jr, &jcr->previous_jr, sizeof(mig_jcr->previous_jr));

   /*
    * Turn the mig_jcr into a "real" job that takes on the aspects of
    *   the previous backup job "prev_job".
    */
   set_jcr_defaults(mig_jcr, prev_job);
   if (!setup_job(mig_jcr)) {
      Jmsg(jcr, M_FATAL, 0, _("setup job failed.\n"));
      return false;
   }

   /* Now reset the job record from the previous job */
   memcpy(&mig_jcr->jr, &jcr->previous_jr, sizeof(mig_jcr->jr));
   /* Update the jr to reflect the new values of PoolId, FileSetId, and JobId. */
   mig_jcr->jr.PoolId = jcr->jr.PoolId;
   mig_jcr->jr.FileSetId = jcr->jr.FileSetId;
   mig_jcr->jr.JobId = mig_jcr->JobId;

   Dmsg4(dbglevel, "mig_jcr: Name=%s JobId=%d Type=%c Level=%c\n",
      mig_jcr->jr.Name, (int)mig_jcr->jr.JobId, 
      mig_jcr->jr.JobType, mig_jcr->jr.JobLevel);

   /*
    * Get the PoolId used with the original job. Then
    *  find the pool name from the database record.
    */
   memset(&pr, 0, sizeof(pr));
   pr.PoolId = mig_jcr->previous_jr.PoolId;
   if (!db_get_pool_record(jcr, jcr->db, &pr)) {
      Jmsg(jcr, M_FATAL, 0, _("Pool for JobId %s not in database. ERR=%s\n"),
            edit_int64(pr.PoolId, ed1), db_strerror(jcr->db));
         return false;
   }
   /* Get the pool resource corresponding to the original job */
   pool = (POOL *)GetResWithName(R_POOL, pr.Name);
   if (!pool) {
      Jmsg(jcr, M_FATAL, 0, _("Pool resource \"%s\" not found.\n"), pr.Name);
      return false;
   }

   /* If pool storage specified, use it for restore */
   copy_rstorage(mig_jcr, pool->storage, _("Pool resource"));
   copy_rstorage(jcr, pool->storage, _("Pool resource"));

   /*
    * If the original backup pool has a NextPool, make sure a 
    *  record exists in the database. Note, in this case, we
    *  will be migrating from pool to pool->NextPool.
    */
   if (pool->NextPool) {
      jcr->jr.PoolId = get_or_create_pool_record(jcr, pool->NextPool->hdr.name);
      if (jcr->jr.PoolId == 0) {
         return false;
      }
      /*
       * put the "NextPool" resource pointer in our jcr so that we
       * can pull the Storage reference from it.
       */
      mig_jcr->pool = jcr->pool = pool->NextPool;
      mig_jcr->jr.PoolId = jcr->jr.PoolId;
      pm_strcpy(jcr->pool_source, _("NextPool in Pool resource"));
   } else {
      Jmsg(jcr, M_FATAL, 0, _("No Next Pool specification found in Pool \"%s\".\n"),
         pool->hdr.name);
      return false;
   }

   if (!jcr->pool->storage || jcr->pool->storage->size() == 0) {
      Jmsg(jcr, M_FATAL, 0, _("No Storage specification found in Next Pool \"%s\".\n"),
         jcr->pool->hdr.name);
      return false;
   }

   /* If pool storage specified, use it instead of job storage for backup */
   copy_wstorage(jcr, jcr->pool->storage, _("NextPool in Pool resource"));

   return true;
}

/*
 * Do a Migration of a previous job
 *
 *  Returns:  false on failure
 *            true  on success
 */
bool do_migration(JCR *jcr)
{
   char ed1[100];
   BSOCK *sd;
   JCR *mig_jcr = jcr->mig_jcr;    /* newly migrated job */

   if (!mig_jcr) {
      Jmsg(jcr, M_INFO, 0, _("No files found to migrate.\n"));
      return false;
   }

   /* Print Job Start message */
   Jmsg(jcr, M_INFO, 0, _("Start Migration JobId %s, Job=%s\n"),
        edit_uint64(jcr->JobId, ed1), jcr->Job);

   set_jcr_job_status(jcr, JS_Running);
   set_jcr_job_status(mig_jcr, JS_Running);
   Dmsg2(dbglevel, "JobId=%d JobLevel=%c\n", (int)jcr->jr.JobId, jcr->jr.JobLevel);

   /* Update job start record for this migration control job */
   if (!db_update_job_start_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_FATAL, 0, "%s", db_strerror(jcr->db));
      return false;
   }

   Dmsg4(dbglevel, "mig_jcr: Name=%s JobId=%d Type=%c Level=%c\n",
      mig_jcr->jr.Name, (int)mig_jcr->jr.JobId, 
      mig_jcr->jr.JobType, mig_jcr->jr.JobLevel);

   /* Update job start record for the real migration backup job */
   if (!db_update_job_start_record(mig_jcr, mig_jcr->db, &mig_jcr->jr)) {
      Jmsg(jcr, M_FATAL, 0, "%s", db_strerror(mig_jcr->db));
      return false;
   }


   /*
    * Open a message channel connection with the Storage
    * daemon. This is to let him know that our client
    * will be contacting him for a backup  session.
    *
    */
   Dmsg0(110, "Open connection with storage daemon\n");
   set_jcr_job_status(jcr, JS_WaitSD);
   set_jcr_job_status(mig_jcr, JS_WaitSD);
   /*
    * Start conversation with Storage daemon
    */
   if (!connect_to_storage_daemon(jcr, 10, SDConnectTimeout, 1)) {
      return false;
   }
   sd = jcr->store_bsock;
   /*
    * Now start a job with the Storage daemon
    */
   Dmsg2(dbglevel, "Read store=%s, write store=%s\n", 
      ((STORE *)jcr->rstorage->first())->name(),
      ((STORE *)jcr->wstorage->first())->name());
   if (((STORE *)jcr->rstorage->first())->name() == ((STORE *)jcr->wstorage->first())->name()) {
      Jmsg(jcr, M_FATAL, 0, _("Read storage \"%s\" same as write storage.\n"),
           ((STORE *)jcr->rstorage->first())->name());
      return false;
   }
   if (!start_storage_daemon_job(jcr, jcr->rstorage, jcr->wstorage)) {
      return false;
   }
   Dmsg0(150, "Storage daemon connection OK\n");

   if (!send_bootstrap_file(jcr, sd) ||
       !response(jcr, sd, OKbootstrap, "Bootstrap", DISPLAY_ERROR)) {
      return false;
   }

   if (!bnet_fsend(sd, "run")) {
      return false;
   }

   /*
    * Now start a Storage daemon message thread
    */
   if (!start_storage_daemon_message_thread(jcr)) {
      return false;
   }


   set_jcr_job_status(jcr, JS_Running);
   set_jcr_job_status(mig_jcr, JS_Running);

   /* Pickup Job termination data */
   /* Note, the SD stores in jcr->JobFiles/ReadBytes/JobBytes/Errors */
   wait_for_storage_daemon_termination(jcr);

   set_jcr_job_status(jcr, jcr->SDJobStatus);
   if (jcr->JobStatus != JS_Terminated) {
      return false;
   }
   migration_cleanup(jcr, jcr->JobStatus);
   if (mig_jcr) {
      UAContext *ua = new_ua_context(jcr);
      purge_files_from_job(ua, jcr->previous_jr.JobId);
      free_ua_context(ua);
   }
   return true;
}

struct idpkt {
   POOLMEM *list;
   uint32_t count;
};

/* Add an item to the list if it is unique */
static void add_unique_id(idpkt *ids, char *item) 
{
   char id[30];
   char *q = ids->list;

   /* Walk through current list to see if each item is the same as item */
   for ( ; *q; ) {
       id[0] = 0;
       for (int i=0; i<(int)sizeof(id); i++) {
          if (*q == 0) {
             break;
          } else if (*q == ',') {
             q++;
             break;
          }
          id[i] = *q++;
          id[i+1] = 0;
       }
       if (strcmp(item, id) == 0) {
          return;
       }
   }
   /* Did not find item, so add it to list */
   if (ids->count == 0) {
      ids->list[0] = 0;
   } else {
      pm_strcat(ids->list, ",");
   }
   pm_strcat(ids->list, item);
   ids->count++;
// Dmsg3(0, "add_uniq count=%d Ids=%p %s\n", ids->count, ids->list, ids->list);
   return;
}

/*
 * Callback handler make list of DB Ids
 */
static int unique_dbid_handler(void *ctx, int num_fields, char **row)
{
   idpkt *ids = (idpkt *)ctx;

   add_unique_id(ids, row[0]);
   Dmsg3(dbglevel, "dbid_hdlr count=%d Ids=%p %s\n", ids->count, ids->list, ids->list);
   return 0;
}


struct uitem {
   dlink link;   
   char *item;
};

static int item_compare(void *item1, void *item2)
{
   uitem *i1 = (uitem *)item1;
   uitem *i2 = (uitem *)item2;
   return strcmp(i1->item, i2->item);
}

static int unique_name_handler(void *ctx, int num_fields, char **row)
{
   dlist *list = (dlist *)ctx;

   uitem *new_item = (uitem *)malloc(sizeof(uitem));
   uitem *item;
   
   memset(new_item, 0, sizeof(uitem));
   new_item->item = bstrdup(row[0]);
   Dmsg1(dbglevel, "Unique_name_hdlr Item=%s\n", row[0]);
   item = (uitem *)list->binary_insert((void *)new_item, item_compare);
   if (item != new_item) {            /* already in list */
      free(new_item->item);
      free((char *)new_item);
      return 0;
   }
   return 0;
}

/* Get Job names in Pool */
const char *sql_job =
   "SELECT DISTINCT Job.Name from Job,Pool"
   " WHERE Pool.Name='%s' AND Job.PoolId=Pool.PoolId";

/* Get JobIds from regex'ed Job names */
const char *sql_jobids_from_job =
   "SELECT DISTINCT Job.JobId,Job.StartTime FROM Job,Pool"
   " WHERE Job.Name='%s' AND Pool.Name='%s' AND Job.PoolId=Pool.PoolId"
   " ORDER by Job.StartTime";

/* Get Client names in Pool */
const char *sql_client =
   "SELECT DISTINCT Client.Name from Client,Pool,Job"
   " WHERE Pool.Name='%s' AND Job.ClientId=Client.ClientId AND"
   " Job.PoolId=Pool.PoolId";

/* Get JobIds from regex'ed Client names */
const char *sql_jobids_from_client =
   "SELECT DISTINCT Job.JobId,Job.StartTime FROM Job,Pool,Client"
   " WHERE Client.Name='%s' AND Pool.Name='%s' AND Job.PoolId=Pool.PoolId"
   " AND Job.ClientId=Client.ClientId "
   " ORDER by Job.StartTime";

/* Get Volume names in Pool */
const char *sql_vol = 
   "SELECT DISTINCT VolumeName FROM Media,Pool WHERE"
   " VolStatus in ('Full','Used','Error') AND Media.Enabled=1 AND"
   " Media.PoolId=Pool.PoolId AND Pool.Name='%s'";

/* Get JobIds from regex'ed Volume names */
const char *sql_jobids_from_vol =
   "SELECT DISTINCT Job.JobId,Job.StartTime FROM Media,JobMedia,Job"
   " WHERE Media.VolumeName='%s' AND Media.MediaId=JobMedia.MediaId"
   " AND JobMedia.JobId=Job.JobId" 
   " ORDER by Job.StartTime";


const char *sql_smallest_vol = 
   "SELECT MediaId FROM Media,Pool WHERE"
   " VolStatus in ('Full','Used','Error') AND Media.Enabled=1 AND"
   " Media.PoolId=Pool.PoolId AND Pool.Name='%s'"
   " ORDER BY VolBytes ASC LIMIT 1";

const char *sql_oldest_vol = 
   "SELECT MediaId FROM Media,Pool WHERE"
   " VolStatus in ('Full','Used','Error') AND Media.Enabled=1 AND"
   " Media.PoolId=Pool.PoolId AND Pool.Name='%s'"
   " ORDER BY LastWritten ASC LIMIT 1";

/* Get JobIds when we have selected MediaId */
const char *sql_jobids_from_mediaid =
   "SELECT DISTINCT Job.JobId,Job.StartTime FROM JobMedia,Job"
   " WHERE JobMedia.JobId=Job.JobId AND JobMedia.MediaId=%s"
   " ORDER by Job.StartTime";

/* Get tne number of bytes in the pool */
const char *sql_pool_bytes =
   "SELECT SUM(VolBytes) FROM Media,Pool WHERE"
   " VolStatus in ('Full','Used','Error','Append') AND Media.Enabled=1 AND"
   " Media.PoolId=Pool.PoolId AND Pool.Name='%s'";

/* Get tne number of bytes in the Jobs */
const char *sql_job_bytes =
   "SELECT SUM(JobBytes) FROM Job WHERE JobId IN (%s)";


/* Get Media Ids in Pool */
const char *sql_mediaids =
   "SELECT MediaId FROM Media,Pool WHERE"
   " VolStatus in ('Full','Used','Error') AND Media.Enabled=1 AND"
   " Media.PoolId=Pool.PoolId AND Pool.Name='%s' ORDER BY LastWritten ASC";

/* Get JobIds in Pool longer than specified time */
const char *sql_pool_time = 
   "SELECT DISTINCT Job.JobId from Pool,Job,Media,JobMedia WHERE"
   " Pool.Name='%s' AND Media.PoolId=Pool.PoolId AND"
   " VolStatus in ('Full','Used','Error') AND Media.Enabled=1 AND"
   " JobMedia.JobId=Job.JobId AND Job.PoolId=Media.PoolId"
   " AND Job.RealEndTime<='%s'";

/*
* const char *sql_ujobid =
*   "SELECT DISTINCT Job.Job from Client,Pool,Media,Job,JobMedia "
*   " WHERE Media.PoolId=Pool.PoolId AND Pool.Name='%s' AND"
*   " JobMedia.JobId=Job.JobId AND Job.PoolId=Media.PoolId";
*/



/*
 *
 * This is the central piece of code that finds a job or jobs 
 *   actually JobIds to migrate.  It first looks to see if one
 *   has been "manually" specified in jcr->MigrateJobId, and if
 *   so, it returns that JobId to be run.  Otherwise, it
 *   examines the Selection Type to see what kind of migration
 *   we are doing (Volume, Job, Client, ...) and applies any
 *   Selection Pattern if appropriate to obtain a list of JobIds.
 *   Finally, it will loop over all the JobIds found, except the last
 *   one starting a new job with MigrationJobId set to that JobId, and
 *   finally, it returns the last JobId to the caller.
 *
 * Returns: false on error
 *          true  if OK and jcr->previous_jr filled in
 */
static bool get_job_to_migrate(JCR *jcr)
{
   char ed1[30];
   POOL_MEM query(PM_MESSAGE);
   JobId_t JobId;
   DBId_t  MediaId = 0;
   int stat;
   char *p;
   idpkt ids, mid, jids;
   db_int64_ctx ctx;
   int64_t pool_bytes;
   bool ok;
   time_t ttime;
   struct tm tm;
   char dt[MAX_TIME_LENGTH];

   ids.list = get_pool_memory(PM_MESSAGE);
   ids.list[0] = 0;
   ids.count = 0;
   mid.list = get_pool_memory(PM_MESSAGE);
   mid.list[0] = 0;
   mid.count = 0;
   jids.list = get_pool_memory(PM_MESSAGE);
   jids.list[0] = 0;
   jids.count = 0;


   /*
    * If MigrateJobId is set, then we migrate only that Job,
    *  otherwise, we go through the full selection of jobs to
    *  migrate.
    */
   if (jcr->MigrateJobId != 0) {
      Dmsg1(dbglevel, "At Job start previous jobid=%u\n", jcr->MigrateJobId);
      edit_uint64(jcr->MigrateJobId, ids.list);
      ids.count = 1;
   } else {
      switch (jcr->job->selection_type) {
      case MT_JOB:
         if (!regex_find_jobids(jcr, &ids, sql_job, sql_jobids_from_job, "Job")) {
            goto bail_out;
         } 
         break;
      case MT_CLIENT:
         if (!regex_find_jobids(jcr, &ids, sql_client, sql_jobids_from_client, "Client")) {
            goto bail_out;
         } 
         break;
      case MT_VOLUME:
         if (!regex_find_jobids(jcr, &ids, sql_vol, sql_jobids_from_vol, "Volume")) {
            goto bail_out;
         } 
         break;
      case MT_SQLQUERY:
         if (!jcr->job->selection_pattern) {
            Jmsg(jcr, M_FATAL, 0, _("No Migration SQL selection pattern specified.\n"));
            goto bail_out;
         }
         Dmsg1(dbglevel, "SQL=%s\n", jcr->job->selection_pattern);
         if (!db_sql_query(jcr->db, jcr->job->selection_pattern,
              unique_dbid_handler, (void *)&ids)) {
            Jmsg(jcr, M_FATAL, 0,
                 _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
            goto bail_out;
         }
         break;
      case MT_SMALLEST_VOL:
         if (!find_mediaid_then_jobids(jcr, &ids, sql_smallest_vol, "Smallest Volume")) {
            goto bail_out;
         }
         break;
      case MT_OLDEST_VOL:
         if (!find_mediaid_then_jobids(jcr, &ids, sql_oldest_vol, "Oldest Volume")) {
            goto bail_out;
         }
         break;

      case MT_POOL_OCCUPANCY:
         ctx.count = 0;
         /* Find count of bytes in pool */
         Mmsg(query, sql_pool_bytes, jcr->pool->hdr.name);
         if (!db_sql_query(jcr->db, query.c_str(), db_int64_handler, (void *)&ctx)) {
            Jmsg(jcr, M_FATAL, 0, _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
            goto bail_out;
         }
         if (ctx.count == 0) {
            Jmsg(jcr, M_INFO, 0, _("No Volumes found to migrate.\n"));
            goto ok_out;
         }
         pool_bytes = ctx.value;
         Dmsg2(dbglevel, "highbytes=%d pool=%d\n", (int)jcr->pool->MigrationHighBytes,
               (int)pool_bytes);
         if (pool_bytes < (int64_t)jcr->pool->MigrationHighBytes) {
            Jmsg(jcr, M_INFO, 0, _("No Volumes found to migrate.\n"));
            goto ok_out;
         }
         Dmsg0(dbglevel, "We should do Occupation migration.\n");

         ids.count = 0;
         /* Find a list of MediaIds that could be migrated */
         Mmsg(query, sql_mediaids, jcr->pool->hdr.name);
         Dmsg1(dbglevel, "query=%s\n", query.c_str());
         if (!db_sql_query(jcr->db, query.c_str(), unique_dbid_handler, (void *)&ids)) {
            Jmsg(jcr, M_FATAL, 0, _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
            goto bail_out;
         }
         if (ids.count == 0) {
            Jmsg(jcr, M_INFO, 0, _("No Volumes found to migrate.\n"));
            goto ok_out;
         }
         Dmsg2(dbglevel, "Pool Occupancy ids=%d MediaIds=%s\n", ids.count, ids.list);

         /*
          * Now loop over MediaIds getting more JobIds to migrate until
          *  we reduce the pool occupancy below the low water mark.
          */
         p = ids.list;
         for (int i=0; i < (int)ids.count; i++) {
            stat = get_next_dbid_from_list(&p, &MediaId);
            Dmsg2(dbglevel, "get_next_dbid stat=%d MediaId=%u\n", stat, MediaId);
            if (stat < 0) {
               Jmsg(jcr, M_FATAL, 0, _("Invalid MediaId found.\n"));
               goto bail_out;
            } else if (stat == 0) {
               break;
            }
            mid.count = 1;
            Mmsg(mid.list, "%s", edit_int64(MediaId, ed1));
            ok = find_jobids_from_mediaid_list(jcr, &mid, "Volumes");
            if (!ok) {
               continue;
            }
            if (i != 0) {
               pm_strcat(jids.list, ",");
            }
            pm_strcat(jids.list, mid.list);
            jids.count += mid.count;

            /* Now get the count of bytes added */
            ctx.count = 0;
            /* Find count of bytes from Jobs */
            Mmsg(query, sql_job_bytes, mid.list);
            if (!db_sql_query(jcr->db, query.c_str(), db_int64_handler, (void *)&ctx)) {
               Jmsg(jcr, M_FATAL, 0, _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
               goto bail_out;
            }
            pool_bytes -= ctx.value;
            Dmsg1(dbglevel, "Job bytes=%d\n", (int)ctx.value);
            Dmsg2(dbglevel, "lowbytes=%d pool=%d\n", (int)jcr->pool->MigrationLowBytes,
                  (int)pool_bytes);
            if (pool_bytes <= (int64_t)jcr->pool->MigrationLowBytes) {
               Dmsg0(dbglevel, "We should be done.\n");
               break;
            }

         }
         Dmsg2(dbglevel, "Pool Occupancy ids=%d JobIds=%s\n", jids.count, jids.list);

         break;

      case MT_POOL_TIME:
         ttime = time(NULL) - (time_t)jcr->pool->MigrationTime;
         (void)localtime_r(&ttime, &tm);
         strftime(dt, sizeof(dt), "%Y-%m-%d %H:%M:%S", &tm);

         ids.count = 0;
         Mmsg(query, sql_pool_time, jcr->pool->hdr.name, dt);
         Dmsg1(dbglevel, "query=%s\n", query.c_str());
         if (!db_sql_query(jcr->db, query.c_str(), unique_dbid_handler, (void *)&ids)) {
            Jmsg(jcr, M_FATAL, 0, _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
            goto bail_out;
         }
         if (ids.count == 0) {
            Jmsg(jcr, M_INFO, 0, _("No Volumes found to migrate.\n"));
            goto ok_out;
         }
         Dmsg2(dbglevel, "PoolTime ids=%d JobIds=%s\n", ids.count, ids.list);
         break;

      default:
         Jmsg(jcr, M_FATAL, 0, _("Unknown Migration Selection Type.\n"));
         goto bail_out;
      }
   }

   /*
    * Loop over all jobids except the last one, sending
    *  them to start_migration_job(), which will start a job
    *  for each of them.  For the last JobId, we handle it below.
    */
   p = ids.list;
   if (ids.count == 0) {
      Jmsg(jcr, M_INFO, 0, _("No JobIds found to migrate.\n"));
      goto ok_out;
   }
   Jmsg(jcr, M_INFO, 0, _("The following %u JobId%s will be migrated: %s\n"),
      ids.count, ids.count==0?"":"s", ids.list);
   Dmsg2(dbglevel, "Before loop count=%d ids=%s\n", ids.count, ids.list);
   for (int i=1; i < (int)ids.count; i++) {
      JobId = 0;
      stat = get_next_jobid_from_list(&p, &JobId);
      Dmsg3(dbglevel, "get_jobid_no=%d stat=%d JobId=%u\n", i, stat, JobId);
      jcr->MigrateJobId = JobId;
      start_migration_job(jcr);
      Dmsg0(dbglevel, "Back from start_migration_job\n");
      if (stat < 0) {
         Jmsg(jcr, M_FATAL, 0, _("Invalid JobId found.\n"));
         goto bail_out;
      } else if (stat == 0) {
         Jmsg(jcr, M_INFO, 0, _("No JobIds found to migrate.\n"));
         goto bail_out;
      }
   }
   
   /* Now get the last JobId and handle it in the current job */
   JobId = 0;
   stat = get_next_jobid_from_list(&p, &JobId);
   Dmsg2(dbglevel, "Last get_next_jobid stat=%d JobId=%u\n", stat, (int)JobId);
   if (stat < 0) {
      Jmsg(jcr, M_FATAL, 0, _("Invalid JobId found.\n"));
      goto bail_out;
   } else if (stat == 0) {
      Jmsg(jcr, M_INFO, 0, _("No JobIds found to migrate.\n"));
      goto bail_out;
   }

   jcr->previous_jr.JobId = JobId;
   Dmsg1(dbglevel, "Previous jobid=%d\n", (int)jcr->previous_jr.JobId);

   if (!db_get_job_record(jcr, jcr->db, &jcr->previous_jr)) {
      Jmsg(jcr, M_FATAL, 0, _("Could not get job record for JobId %s to migrate. ERR=%s"),
           edit_int64(jcr->previous_jr.JobId, ed1),
           db_strerror(jcr->db));
      goto bail_out;
   }
   Jmsg(jcr, M_INFO, 0, _("Migration using JobId=%s Job=%s\n"),
      edit_int64(jcr->previous_jr.JobId, ed1), jcr->previous_jr.Job);
   Dmsg3(dbglevel, "Migration JobId=%d  using JobId=%s Job=%s\n",
      jcr->JobId,
      edit_int64(jcr->previous_jr.JobId, ed1), jcr->previous_jr.Job);

ok_out:
   ok = true;
   goto out;

bail_out:
   ok = false;
           
out:
   free_pool_memory(ids.list);
   free_pool_memory(mid.list);
   free_pool_memory(jids.list);
   return ok;
}

static void start_migration_job(JCR *jcr)
{
   UAContext *ua = new_ua_context(jcr);
   char ed1[50];
   ua->batch = true;
   Mmsg(ua->cmd, "run %s jobid=%s", jcr->job->hdr.name, 
        edit_uint64(jcr->MigrateJobId, ed1));
   Dmsg1(dbglevel, "=============== Migration cmd=%s\n", ua->cmd);
   parse_ua_args(ua);                 /* parse command */
   int stat = run_cmd(ua, ua->cmd);
   if (stat == 0) {
      Jmsg(jcr, M_ERROR, 0, _("Could not start migration job.\n"));
   } else {
      Jmsg(jcr, M_INFO, 0, _("Migration JobId %d started.\n"), stat);
   }
   free_ua_context(ua);
}

static bool find_mediaid_then_jobids(JCR *jcr, idpkt *ids, const char *query1,
                 const char *type) 
{
   bool ok = false;
   POOL_MEM query(PM_MESSAGE);

   ids->count = 0;
   /* Basic query for MediaId */
   Mmsg(query, query1, jcr->pool->hdr.name);
   if (!db_sql_query(jcr->db, query.c_str(), unique_dbid_handler, (void *)ids)) {
      Jmsg(jcr, M_FATAL, 0, _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
      goto bail_out;
   }
   if (ids->count == 0) {
      Jmsg(jcr, M_INFO, 0, _("No %ss found to migrate.\n"), type);
   }
   if (ids->count != 1) {
      Jmsg(jcr, M_FATAL, 0, _("SQL logic error. Count should be 1 but is %d\n"), 
         ids->count);
      goto bail_out;
   }
   Dmsg1(dbglevel, "Smallest Vol Jobids=%s\n", ids->list);

   ok = find_jobids_from_mediaid_list(jcr, ids, type);

bail_out:
   return ok;
}

static bool find_jobids_from_mediaid_list(JCR *jcr, idpkt *ids, const char *type) 
{
   bool ok = false;
   POOL_MEM query(PM_MESSAGE);

   Mmsg(query, sql_jobids_from_mediaid, ids->list);
   ids->count = 0;
   if (!db_sql_query(jcr->db, query.c_str(), unique_dbid_handler, (void *)ids)) {
      Jmsg(jcr, M_FATAL, 0, _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
      goto bail_out;
   }
   if (ids->count == 0) {
      Jmsg(jcr, M_INFO, 0, _("No %ss found to migrate.\n"), type);
   }
   ok = true;
bail_out:
   return ok;
}

static bool regex_find_jobids(JCR *jcr, idpkt *ids, const char *query1,
                 const char *query2, const char *type) 
{
   dlist *item_chain;
   uitem *item = NULL;
   uitem *last_item = NULL;
   regex_t preg;
   char prbuf[500];
   int rc;
   bool ok = false;
   POOL_MEM query(PM_MESSAGE);

   item_chain = New(dlist(item, &item->link));
   if (!jcr->job->selection_pattern) {
      Jmsg(jcr, M_FATAL, 0, _("No Migration %s selection pattern specified.\n"),
         type);
      goto bail_out;
   }
   Dmsg1(dbglevel, "regex=%s\n", jcr->job->selection_pattern);
   /* Compile regex expression */
   rc = regcomp(&preg, jcr->job->selection_pattern, REG_EXTENDED);
   if (rc != 0) {
      regerror(rc, &preg, prbuf, sizeof(prbuf));
      Jmsg(jcr, M_FATAL, 0, _("Could not compile regex pattern \"%s\" ERR=%s\n"),
           jcr->job->selection_pattern, prbuf);
      goto bail_out;
   }
   /* Basic query for names */
   Mmsg(query, query1, jcr->pool->hdr.name);
   Dmsg1(dbglevel, "get name query1=%s\n", query.c_str());
   if (!db_sql_query(jcr->db, query.c_str(), unique_name_handler, 
        (void *)item_chain)) {
      Jmsg(jcr, M_FATAL, 0,
           _("SQL to get %s failed. ERR=%s\n"), type, db_strerror(jcr->db));
      goto bail_out;
   }
   /* Now apply the regex to the names and remove any item not matched */
   foreach_dlist(item, item_chain) {
      const int nmatch = 30;
      regmatch_t pmatch[nmatch];
      if (last_item) {
         Dmsg1(dbglevel, "Remove item %s\n", last_item->item);
         free(last_item->item);
         item_chain->remove(last_item);
      }
      Dmsg1(dbglevel, "get name Item=%s\n", item->item);
      rc = regexec(&preg, item->item, nmatch, pmatch,  0);
      if (rc == 0) {
         last_item = NULL;   /* keep this one */
      } else {   
         last_item = item;
      }
   }
   if (last_item) {
      free(last_item->item);
      Dmsg1(dbglevel, "Remove item %s\n", last_item->item);
      item_chain->remove(last_item);
   }
   regfree(&preg);
   /* 
    * At this point, we have a list of items in item_chain
    *  that have been matched by the regex, so now we need
    *  to look up their jobids.
    */
   ids->count = 0;
   foreach_dlist(item, item_chain) {
      Dmsg2(dbglevel, "Got %s: %s\n", type, item->item);
      Mmsg(query, query2, item->item, jcr->pool->hdr.name);
      Dmsg1(dbglevel, "get id from name query2=%s\n", query.c_str());
      if (!db_sql_query(jcr->db, query.c_str(), unique_dbid_handler, (void *)ids)) {
         Jmsg(jcr, M_FATAL, 0,
              _("SQL failed. ERR=%s\n"), db_strerror(jcr->db));
         goto bail_out;
      }
   }
   if (ids->count == 0) {
      Jmsg(jcr, M_INFO, 0, _("No %ss found to migrate.\n"), type);
   }
   ok = true;
bail_out:
   Dmsg2(dbglevel, "Count=%d Jobids=%s\n", ids->count, ids->list);
   delete item_chain;
   Dmsg0(dbglevel, "After delete item_chain\n");
   return ok;
}


/*
 * Release resources allocated during backup.
 */
void migration_cleanup(JCR *jcr, int TermCode)
{
   char sdt[MAX_TIME_LENGTH], edt[MAX_TIME_LENGTH];
   char ec1[30], ec2[30], ec3[30], ec4[30], ec5[30], elapsed[50];
   char ec6[50], ec7[50], ec8[50];
   char term_code[100], sd_term_msg[100];
   const char *term_msg;
   int msg_type;
   MEDIA_DBR mr;
   double kbps;
   utime_t RunTime;
   JCR *mig_jcr = jcr->mig_jcr;
   POOL_MEM query(PM_MESSAGE);

   Dmsg2(100, "Enter migrate_cleanup %d %c\n", TermCode, TermCode);
   dequeue_messages(jcr);             /* display any queued messages */
   memset(&mr, 0, sizeof(mr));
   set_jcr_job_status(jcr, TermCode);
   update_job_end_record(jcr);        /* update database */

   /* 
    * Check if we actually did something.  
    *  mig_jcr is jcr of the newly migrated job.
    */
   if (mig_jcr) {
      mig_jcr->JobFiles = jcr->JobFiles = jcr->SDJobFiles;
      mig_jcr->JobBytes = jcr->JobBytes = jcr->SDJobBytes;
      mig_jcr->VolSessionId = jcr->VolSessionId;
      mig_jcr->VolSessionTime = jcr->VolSessionTime;
      mig_jcr->jr.RealEndTime = 0; 
      mig_jcr->jr.PriorJobId = jcr->previous_jr.JobId;

      set_jcr_job_status(mig_jcr, TermCode);

  
      update_job_end_record(mig_jcr);
     
      /* Update final items to set them to the previous job's values */
      Mmsg(query, "UPDATE Job SET StartTime='%s',EndTime='%s',"
                  "JobTDate=%s WHERE JobId=%s", 
         jcr->previous_jr.cStartTime, jcr->previous_jr.cEndTime, 
         edit_uint64(jcr->previous_jr.JobTDate, ec1),
         edit_uint64(mig_jcr->jr.JobId, ec2));
      db_sql_query(mig_jcr->db, query.c_str(), NULL, NULL);

      /* Now marke the previous job as migrated */
      Mmsg(query, "UPDATE Job SET Type='%c' WHERE JobId=%s",
           (char)JT_MIGRATED_JOB, edit_uint64(jcr->previous_jr.JobId, ec1));
      db_sql_query(mig_jcr->db, query.c_str(), NULL, NULL);

      if (!db_get_job_record(jcr, jcr->db, &jcr->jr)) {
         Jmsg(jcr, M_WARNING, 0, _("Error getting job record for stats: %s"),
            db_strerror(jcr->db));
         set_jcr_job_status(jcr, JS_ErrorTerminated);
      }

      bstrncpy(mr.VolumeName, jcr->VolumeName, sizeof(mr.VolumeName));
      if (!db_get_media_record(jcr, jcr->db, &mr)) {
         Jmsg(jcr, M_WARNING, 0, _("Error getting Media record for Volume \"%s\": ERR=%s"),
            mr.VolumeName, db_strerror(jcr->db));
         set_jcr_job_status(jcr, JS_ErrorTerminated);
      }

      update_bootstrap_file(mig_jcr);

      if (!db_get_job_volume_names(mig_jcr, mig_jcr->db, mig_jcr->jr.JobId, &mig_jcr->VolumeName)) {
         /*
          * Note, if the job has erred, most likely it did not write any
          *  tape, so suppress this "error" message since in that case
          *  it is normal.  Or look at it the other way, only for a
          *  normal exit should we complain about this error.
          */
         if (jcr->JobStatus == JS_Terminated && jcr->jr.JobBytes) {
            Jmsg(jcr, M_ERROR, 0, "%s", db_strerror(mig_jcr->db));
         }
         mig_jcr->VolumeName[0] = 0;         /* none */
      }
      switch (jcr->JobStatus) {
      case JS_Terminated:
         if (jcr->Errors || jcr->SDErrors) {
            term_msg = _("%s OK -- with warnings");
         } else {
            term_msg = _("%s OK");
         }
         break;
      case JS_FatalError:
      case JS_ErrorTerminated:
         term_msg = _("*** %s Error ***");
         msg_type = M_ERROR;          /* Generate error message */
         if (jcr->store_bsock) {
            bnet_sig(jcr->store_bsock, BNET_TERMINATE);
            if (jcr->SD_msg_chan) {
               pthread_cancel(jcr->SD_msg_chan);
            }
         }
         break;
      case JS_Canceled:
         term_msg = _("%s Canceled");
         if (jcr->store_bsock) {
            bnet_sig(jcr->store_bsock, BNET_TERMINATE);
            if (jcr->SD_msg_chan) {
               pthread_cancel(jcr->SD_msg_chan);
            }
         }
         break;
      default:
         term_msg = _("Inappropriate %s term code");
         break;
      }
  } else {
     term_msg = _("%s -- no files to migrate");
  }

   bsnprintf(term_code, sizeof(term_code), term_msg, "Migration");
   bstrftimes(sdt, sizeof(sdt), jcr->jr.StartTime);
   bstrftimes(edt, sizeof(edt), jcr->jr.EndTime);
   RunTime = jcr->jr.EndTime - jcr->jr.StartTime;
   if (RunTime <= 0) {
      kbps = 0;
   } else {
      kbps = (double)jcr->SDJobBytes / (1000 * RunTime);
   }


   jobstatus_to_ascii(jcr->SDJobStatus, sd_term_msg, sizeof(sd_term_msg));

   Jmsg(jcr, msg_type, 0, _("Bacula %s (%s): %s\n"
"  Prev Backup JobId:      %s\n"
"  New Backup JobId:       %s\n"
"  Migration JobId:        %s\n"
"  Migration Job:          %s\n"
"  Backup Level:           %s%s\n"
"  Client:                 %s\n"
"  FileSet:                \"%s\" %s\n"
"  Pool:                   \"%s\" (From %s)\n"
"  Read Storage:           \"%s\" (From %s)\n"
"  Write Storage:          \"%s\" (From %s)\n"
"  Start time:             %s\n"
"  End time:               %s\n"
"  Elapsed time:           %s\n"
"  Priority:               %d\n"
"  SD Files Written:       %s\n"
"  SD Bytes Written:       %s (%sB)\n"
"  Rate:                   %.1f KB/s\n"
"  Volume name(s):         %s\n"
"  Volume Session Id:      %d\n"
"  Volume Session Time:    %d\n"
"  Last Volume Bytes:      %s (%sB)\n"
"  SD Errors:              %d\n"
"  SD termination status:  %s\n"
"  Termination:            %s\n\n"),
   VERSION,
   LSMDATE,
        edt, 
        mig_jcr ? edit_uint64(jcr->previous_jr.JobId, ec6) : "0", 
        mig_jcr ? edit_uint64(mig_jcr->jr.JobId, ec7) : "0",
        edit_uint64(jcr->jr.JobId, ec8),
        jcr->jr.Job,
        level_to_str(jcr->JobLevel), jcr->since,
        jcr->client->name(),
        jcr->fileset->name(), jcr->FSCreateTime,
        jcr->pool->name(), jcr->pool_source,
        jcr->rstore?jcr->rstore->name():"*None*", 
        NPRT(jcr->rstore_source), 
        jcr->wstore?jcr->wstore->name():"*None*", 
        NPRT(jcr->wstore_source),
        sdt,
        edt,
        edit_utime(RunTime, elapsed, sizeof(elapsed)),
        jcr->JobPriority,
        edit_uint64_with_commas(jcr->SDJobFiles, ec1),
        edit_uint64_with_commas(jcr->SDJobBytes, ec2),
        edit_uint64_with_suffix(jcr->SDJobBytes, ec3),
        (float)kbps,
        mig_jcr ? mig_jcr->VolumeName : "",
        jcr->VolSessionId,
        jcr->VolSessionTime,
        edit_uint64_with_commas(mr.VolBytes, ec4),
        edit_uint64_with_suffix(mr.VolBytes, ec5),
        jcr->SDErrors,
        sd_term_msg,
        term_code);

   Dmsg1(100, "migrate_cleanup() mig_jcr=0x%x\n", jcr->mig_jcr);
   if (jcr->mig_jcr) {
      free_jcr(jcr->mig_jcr);
      jcr->mig_jcr = NULL;
   }
   Dmsg0(100, "Leave migrate_cleanup()\n");
}

/* 
 * Return next DBId from comma separated list   
 *
 * Returns:
 *   1 if next DBId returned
 *   0 if no more DBIds are in list
 *  -1 there is an error
 */
static int get_next_dbid_from_list(char **p, DBId_t *DBId)
{
   char id[30];
   char *q = *p;

   id[0] = 0;
   for (int i=0; i<(int)sizeof(id); i++) {
      if (*q == 0) {
         break;
      } else if (*q == ',') {
         q++;
         break;
      }
      id[i] = *q++;
      id[i+1] = 0;
   }
   if (id[0] == 0) {
      return 0;
   } else if (!is_a_number(id)) {
      return -1;                      /* error */
   }
   *p = q;
   *DBId = str_to_int64(id);
   return 1;
}
