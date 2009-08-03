/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.

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
 *
 *   Bacula Director -- backup.c -- responsible for doing backup jobs
 *
 *     Kern Sibbald, March MM
 *
 *  Basic tasks done here:
 *     Open DB and create records for this job.
 *     Open Message Channel with Storage daemon to tell him a job will be starting.
 *     Open connection with File daemon and pass him commands
 *       to do the backup.
 *     When the File daemon finishes the job, update the DB.
 *
 *   Version $Id$
 */

#include "bacula.h"
#include "dird.h"
#include "ua.h"

/* Commands sent to File daemon */
static char backupcmd[] = "backup\n";
static char storaddr[]  = "storage address=%s port=%d ssl=%d\n";

/* Responses received from File daemon */
static char OKbackup[]   = "2000 OK backup\n";
static char OKstore[]    = "2000 OK storage\n";
static char EndJob[]     = "2800 End Job TermCode=%d JobFiles=%u "
                           "ReadBytes=%llu JobBytes=%llu Errors=%u "  
                           "VSS=%d Encrypt=%d\n";
/* Pre 1.39.29 (04Dec06) EndJob */
static char OldEndJob[]  = "2800 End Job TermCode=%d JobFiles=%u "
                           "ReadBytes=%llu JobBytes=%llu Errors=%u\n";
/* 
 * Called here before the job is run to do the job
 *   specific setup.
 */
bool do_backup_init(JCR *jcr)
{

   if (jcr->get_JobLevel() == L_VIRTUAL_FULL) {
      return do_vbackup_init(jcr);
   }
   free_rstorage(jcr);                   /* we don't read so release */

   if (!get_or_create_fileset_record(jcr)) {
      return false;
   }

   /* 
    * Get definitive Job level and since time
    */
   get_level_since_time(jcr, jcr->since, sizeof(jcr->since));

   apply_pool_overrides(jcr);

   if (!allow_duplicate_job(jcr)) {
      return false;
   }

   jcr->jr.PoolId = get_or_create_pool_record(jcr, jcr->pool->name());
   if (jcr->jr.PoolId == 0) {
      return false;
   }

   /* If pool storage specified, use it instead of job storage */
   copy_wstorage(jcr, jcr->pool->storage, _("Pool resource"));

   if (!jcr->wstorage) {
      Jmsg(jcr, M_FATAL, 0, _("No Storage specification found in Job or Pool.\n"));
      return false;
   }

   create_clones(jcr);                /* run any clone jobs */

   return true;
}

/* Take all base jobs from job resource and find the
 * last L_BASE jobid.
 */
static void get_base_jobids(JCR *jcr, POOLMEM *jobids)
{
   JOB_DBR jr;
   JOB *job;
   JobId_t id;
   char str_jobid[50];

   if (!jcr->job->base) {
      return;
   }

   memset(&jr, 0, sizeof(JOB_DBR));
   jr.StartTime = jcr->jr.StartTime;

   foreach_alist(job, jcr->job->base) {
      bstrncpy(jr.Name, job->name(), sizeof(jr.Name));
      db_get_base_jobid(jcr, jcr->db, &jr, &id);

      if (id) {
         if (jobids[0]) {
            pm_strcat(jobids, ",");
         }
         pm_strcat(jobids, edit_uint64(id, str_jobid));
      }
   }
}

/*
 * Foreach files in currrent list, send "/path/fname\0LStat" to FD
 */
static int accurate_list_handler(void *ctx, int num_fields, char **row)
{
   JCR *jcr = (JCR *)ctx;

   if (job_canceled(jcr)) {
      return 1;
   }
   
   if (row[2] > 0) {            /* discard when file_index == 0 */
      jcr->file_bsock->fsend("%s%s%c%s", row[0], row[1], 0, row[4]); 
   }
   return 0;
}

/*
 * Send current file list to FD
 *    DIR -> FD : accurate files=xxxx
 *    DIR -> FD : /path/to/file\0Lstat
 *    DIR -> FD : /path/to/dir/\0Lstat
 *    ...
 *    DIR -> FD : EOD
 */
bool send_accurate_current_files(JCR *jcr)
{
   POOL_MEM buf;
   bool ret=true;

   if (!jcr->accurate || job_canceled(jcr)) {
      return true;
   }
   /* In base level, no previous job is used */
   if (jcr->get_JobLevel() == L_BASE) {
      return true;
   }

   POOLMEM *nb = get_pool_memory(PM_FNAME);
   POOLMEM *jobids = get_pool_memory(PM_FNAME);
   nb[0] = jobids[0] = '\0';

   get_base_jobids(jcr, jobids);

   /* On Full mode, if no previous base job, no accurate things */
   if (jcr->get_JobLevel() == L_FULL && *jobids == 0) {
      goto bail_out;
   }

   if (jcr->get_JobLevel() == L_FULL && *jobids != 0) {
      db_init_base_file(jcr, jcr->db);
   }

   /* For Incr/Diff level, we search for older jobs */
   if (jcr->get_JobLevel() != L_FULL) {
      db_accurate_get_jobids(jcr, jcr->db, &jcr->jr, jobids);

      if (*jobids == 0) {
         ret=false;
         Jmsg(jcr, M_FATAL, 0, _("Cannot find previous jobids.\n"));
         goto bail_out;
      }
   }

   if (jcr->JobId) {            /* display the message only for real jobs */
      Jmsg(jcr, M_INFO, 0, _("Sending Accurate information.\n"));
   }

   /* to be able to allocate the right size for htable */
   Mmsg(buf, "SELECT sum(JobFiles) FROM Job WHERE JobId IN (%s)",jobids);
   db_sql_query(jcr->db, buf.c_str(), db_get_int_handler, nb);
   Dmsg2(200, "jobids=%s nb=%s\n", jobids, nb);
   jcr->file_bsock->fsend("accurate files=%s\n", nb); 

   if (!db_open_batch_connexion(jcr, jcr->db)) {
      ret = false;
      Jmsg0(jcr, M_FATAL, 0, "Can't get dedicate sql connexion");
      goto bail_out;
   }

   db_get_file_list(jcr, jcr->db_batch, jobids, accurate_list_handler, (void *)jcr);

   /* TODO: close the batch connexion ? (can be used very soon) */

   jcr->file_bsock->signal(BNET_EOD);

bail_out:
   free_pool_memory(jobids);
   free_pool_memory(nb);

   return ret;
}

/*
 * Do a backup of the specified FileSet
 *
 *  Returns:  false on failure
 *            true  on success
 */
bool do_backup(JCR *jcr)
{
   int stat;
   int tls_need = BNET_TLS_NONE;
   BSOCK   *fd;
   STORE *store;
   char ed1[100];

   if (jcr->get_JobLevel() == L_VIRTUAL_FULL) {
      return do_vbackup(jcr);
   }

   /* Print Job Start message */
   Jmsg(jcr, M_INFO, 0, _("Start Backup JobId %s, Job=%s\n"),
        edit_uint64(jcr->JobId, ed1), jcr->Job);

   set_jcr_job_status(jcr, JS_Running);
   Dmsg2(100, "JobId=%d JobLevel=%c\n", jcr->jr.JobId, jcr->jr.JobLevel);
   if (!db_update_job_start_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_FATAL, 0, "%s", db_strerror(jcr->db));
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
   /*
    * Start conversation with Storage daemon
    */
   if (!connect_to_storage_daemon(jcr, 10, SDConnectTimeout, 1)) {
      return false;
   }
   /*
    * Now start a job with the Storage daemon
    */
   if (!start_storage_daemon_job(jcr, NULL, jcr->wstorage)) {
      return false;
   }

   /*
    * Start the job prior to starting the message thread below
    * to avoid two threads from using the BSOCK structure at
    * the same time.
    */
   if (!bnet_fsend(jcr->store_bsock, "run")) {
      return false;
   }

   /*
    * Now start a Storage daemon message thread.  Note,
    *   this thread is used to provide the catalog services
    *   for the backup job, including inserting the attributes
    *   into the catalog.  See catalog_update() in catreq.c
    */
   if (!start_storage_daemon_message_thread(jcr)) {
      return false;
   }
   Dmsg0(150, "Storage daemon connection OK\n");

   set_jcr_job_status(jcr, JS_WaitFD);
   if (!connect_to_file_daemon(jcr, 10, FDConnectTimeout, 1)) {
      goto bail_out;
   }

   set_jcr_job_status(jcr, JS_Running);
   fd = jcr->file_bsock;

   if (!send_include_list(jcr)) {
      goto bail_out;
   }

   if (!send_exclude_list(jcr)) {
      goto bail_out;
   }

   if (!send_level_command(jcr)) {
      goto bail_out;
   }

   /*
    * send Storage daemon address to the File daemon
    */
   store = jcr->wstore;
   if (store->SDDport == 0) {
      store->SDDport = store->SDport;
   }

   /* TLS Requirement */
   if (store->tls_enable) {
      if (store->tls_require) {
         tls_need = BNET_TLS_REQUIRED;
      } else {
         tls_need = BNET_TLS_OK;
      }
   }

   fd->fsend(storaddr, store->address, store->SDDport, tls_need);
   if (!response(jcr, fd, OKstore, "Storage", DISPLAY_ERROR)) {
      goto bail_out;
   }

   if (!send_runscripts_commands(jcr)) {
      goto bail_out;
   }

   /*    
    * We re-update the job start record so that the start
    *  time is set after the run before job.  This avoids 
    *  that any files created by the run before job will
    *  be saved twice.  They will be backed up in the current
    *  job, but not in the next one unless they are changed.
    *  Without this, they will be backed up in this job and
    *  in the next job run because in that case, their date 
    *   is after the start of this run.
    */
   jcr->start_time = time(NULL);
   jcr->jr.StartTime = jcr->start_time;
   if (!db_update_job_start_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_FATAL, 0, "%s", db_strerror(jcr->db));
   }

   /*
    * If backup is in accurate mode, we send the list of
    * all files to FD.
    */
   if (!send_accurate_current_files(jcr)) {
      goto bail_out;
   }

   /* Send backup command */
   fd->fsend(backupcmd);
   if (!response(jcr, fd, OKbackup, "backup", DISPLAY_ERROR)) {
      goto bail_out;
   }

   /* Pickup Job termination data */
   stat = wait_for_job_termination(jcr);
   db_write_batch_file_records(jcr);    /* used by bulk batch file insert */
   if (stat == JS_Terminated) {
      backup_cleanup(jcr, stat);
      return true;
   }     
   return false;

/* Come here only after starting SD thread */
bail_out:
   set_jcr_job_status(jcr, JS_ErrorTerminated);
   Dmsg1(400, "wait for sd. use=%d\n", jcr->use_count());
   /* Cancel SD */
   wait_for_job_termination(jcr, FDConnectTimeout);
   Dmsg1(400, "after wait for sd. use=%d\n", jcr->use_count());
   return false;
}


/*
 * Here we wait for the File daemon to signal termination,
 *   then we wait for the Storage daemon.  When both
 *   are done, we return the job status.
 * Also used by restore.c
 */
int wait_for_job_termination(JCR *jcr, int timeout)
{
   int32_t n = 0;
   BSOCK *fd = jcr->file_bsock;
   bool fd_ok = false;
   uint32_t JobFiles, JobErrors;
   uint32_t JobWarnings = 0;
   uint64_t ReadBytes = 0;
   uint64_t JobBytes = 0;
   int VSS = 0;
   int Encrypt = 0;
   btimer_t *tid=NULL;

   set_jcr_job_status(jcr, JS_Running);

   if (fd) {
      if (timeout) {
         tid = start_bsock_timer(fd, timeout); /* TODO: New timeout directive??? */
      }
      /* Wait for Client to terminate */
      while ((n = bget_dirmsg(fd)) >= 0) {
         if (!fd_ok && 
             (sscanf(fd->msg, EndJob, &jcr->FDJobStatus, &JobFiles,
                     &ReadBytes, &JobBytes, &JobErrors, &VSS, &Encrypt) == 7 ||
              sscanf(fd->msg, OldEndJob, &jcr->FDJobStatus, &JobFiles,
                     &ReadBytes, &JobBytes, &JobErrors) == 5)) {
            fd_ok = true;
            set_jcr_job_status(jcr, jcr->FDJobStatus);
            Dmsg1(100, "FDStatus=%c\n", (char)jcr->JobStatus);
         } else {
            Jmsg(jcr, M_WARNING, 0, _("Unexpected Client Job message: %s\n"),
                 fd->msg);
         }
         if (job_canceled(jcr)) {
            break;
         }
      }
      if (tid) {
         stop_bsock_timer(tid);
      }

      if (is_bnet_error(fd)) {
         Jmsg(jcr, M_FATAL, 0, _("Network error with FD during %s: ERR=%s\n"),
              job_type_to_str(jcr->get_JobType()), fd->bstrerror());
      }
      fd->signal(BNET_TERMINATE);   /* tell Client we are terminating */
   }

   /* Force cancel in SD if failing */
   if (job_canceled(jcr) || !fd_ok) {
      cancel_storage_daemon_job(jcr);
   }

   /* Note, the SD stores in jcr->JobFiles/ReadBytes/JobBytes/JobErrors */
   wait_for_storage_daemon_termination(jcr);

   /* Return values from FD */
   if (fd_ok) {
      jcr->JobFiles = JobFiles;
      jcr->JobErrors += JobErrors;       /* Keep total errors */
      jcr->ReadBytes = ReadBytes;
      jcr->JobBytes = JobBytes;
      jcr->JobWarnings = JobWarnings;
      jcr->VSS = VSS;
      jcr->Encrypt = Encrypt;
   } else {
      Jmsg(jcr, M_FATAL, 0, _("No Job status returned from FD.\n"));
   }

// Dmsg4(100, "fd_ok=%d FDJS=%d JS=%d SDJS=%d\n", fd_ok, jcr->FDJobStatus,
//   jcr->JobStatus, jcr->SDJobStatus);

   /* Return the first error status we find Dir, FD, or SD */
   if (!fd_ok || is_bnet_error(fd)) { /* if fd not set, that use !fd_ok */
      jcr->FDJobStatus = JS_ErrorTerminated;
   }
   if (jcr->JobStatus != JS_Terminated) {
      return jcr->JobStatus;
   }
   if (jcr->FDJobStatus != JS_Terminated) {
      return jcr->FDJobStatus;
   }
   return jcr->SDJobStatus;
}

/*
 * Release resources allocated during backup.
 */
void backup_cleanup(JCR *jcr, int TermCode)
{
   char sdt[50], edt[50], schedt[50];
   char ec1[30], ec2[30], ec3[30], ec4[30], ec5[30], compress[50];
   char ec6[30], ec7[30], ec8[30], elapsed[50];
   char term_code[100], fd_term_msg[100], sd_term_msg[100];
   const char *term_msg;
   int msg_type = M_INFO;
   MEDIA_DBR mr;
   CLIENT_DBR cr;
   double kbps, compression;
   utime_t RunTime;

   if (jcr->get_JobLevel() == L_VIRTUAL_FULL) {
      vbackup_cleanup(jcr, TermCode);
      return;
   }

   Dmsg2(100, "Enter backup_cleanup %d %c\n", TermCode, TermCode);
   memset(&mr, 0, sizeof(mr));
   memset(&cr, 0, sizeof(cr));

   update_job_end(jcr, TermCode);

   if (!db_get_job_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_WARNING, 0, _("Error getting Job record for Job report: ERR=%s"),
         db_strerror(jcr->db));
      set_jcr_job_status(jcr, JS_ErrorTerminated);
   }

   bstrncpy(cr.Name, jcr->client->name(), sizeof(cr.Name));
   if (!db_get_client_record(jcr, jcr->db, &cr)) {
      Jmsg(jcr, M_WARNING, 0, _("Error getting Client record for Job report: ERR=%s"),
         db_strerror(jcr->db));
   }

   bstrncpy(mr.VolumeName, jcr->VolumeName, sizeof(mr.VolumeName));
   if (!db_get_media_record(jcr, jcr->db, &mr)) {
      Jmsg(jcr, M_WARNING, 0, _("Error getting Media record for Volume \"%s\": ERR=%s"),
         mr.VolumeName, db_strerror(jcr->db));
      set_jcr_job_status(jcr, JS_ErrorTerminated);
   }

   update_bootstrap_file(jcr);

   switch (jcr->JobStatus) {
      case JS_Terminated:
         if (jcr->JobErrors || jcr->SDErrors) {
            term_msg = _("Backup OK -- with warnings");
         } else {
            term_msg = _("Backup OK");
         }
         break;
      case JS_Warnings:
         term_msg = _("Backup OK -- with warnings");
         break;
      case JS_FatalError:
      case JS_ErrorTerminated:
         term_msg = _("*** Backup Error ***");
         msg_type = M_ERROR;          /* Generate error message */
         if (jcr->store_bsock) {
            jcr->store_bsock->signal(BNET_TERMINATE);
            if (jcr->SD_msg_chan) {
               pthread_cancel(jcr->SD_msg_chan);
            }
         }
         break;
      case JS_Canceled:
         term_msg = _("Backup Canceled");
         if (jcr->store_bsock) {
            jcr->store_bsock->signal(BNET_TERMINATE);
            if (jcr->SD_msg_chan) {
               pthread_cancel(jcr->SD_msg_chan);
            }
         }
         break;
      default:
         term_msg = term_code;
         sprintf(term_code, _("Inappropriate term code: %c\n"), jcr->JobStatus);
         break;
   }
   bstrftimes(schedt, sizeof(schedt), jcr->jr.SchedTime);
   bstrftimes(sdt, sizeof(sdt), jcr->jr.StartTime);
   bstrftimes(edt, sizeof(edt), jcr->jr.EndTime);
   RunTime = jcr->jr.EndTime - jcr->jr.StartTime;
   if (RunTime <= 0) {
      kbps = 0;
   } else {
      kbps = ((double)jcr->jr.JobBytes) / (1000.0 * (double)RunTime);
   }
   if (!db_get_job_volume_names(jcr, jcr->db, jcr->jr.JobId, &jcr->VolumeName)) {
      /*
       * Note, if the job has erred, most likely it did not write any
       *  tape, so suppress this "error" message since in that case
       *  it is normal.  Or look at it the other way, only for a
       *  normal exit should we complain about this error.
       */
      if (jcr->JobStatus == JS_Terminated && jcr->jr.JobBytes) {
         Jmsg(jcr, M_ERROR, 0, "%s", db_strerror(jcr->db));
      }
      jcr->VolumeName[0] = 0;         /* none */
   }

   if (jcr->ReadBytes == 0) {
      bstrncpy(compress, "None", sizeof(compress));
   } else {
      compression = (double)100 - 100.0 * ((double)jcr->JobBytes / (double)jcr->ReadBytes);
      if (compression < 0.5) {
         bstrncpy(compress, "None", sizeof(compress));
      } else {
         bsnprintf(compress, sizeof(compress), "%.1f %%", compression);
      }
   }
   jobstatus_to_ascii(jcr->FDJobStatus, fd_term_msg, sizeof(fd_term_msg));
   jobstatus_to_ascii(jcr->SDJobStatus, sd_term_msg, sizeof(sd_term_msg));

// bmicrosleep(15, 0);                /* for debugging SIGHUP */

   Jmsg(jcr, msg_type, 0, _("%s %s %s (%s): %s\n"
"  Build OS:               %s %s %s\n"
"  JobId:                  %d\n"
"  Job:                    %s\n"
"  Backup Level:           %s%s\n"
"  Client:                 \"%s\" %s\n"
"  FileSet:                \"%s\" %s\n"
"  Pool:                   \"%s\" (From %s)\n"
"  Catalog:                \"%s\" (From %s)\n"
"  Storage:                \"%s\" (From %s)\n"
"  Scheduled time:         %s\n"
"  Start time:             %s\n"
"  End time:               %s\n"
"  Elapsed time:           %s\n"
"  Priority:               %d\n"
"  FD Files Written:       %s\n"
"  SD Files Written:       %s\n"
"  FD Bytes Written:       %s (%sB)\n"
"  SD Bytes Written:       %s (%sB)\n"
"  Rate:                   %.1f KB/s\n"
"  Software Compression:   %s\n"
"  VSS:                    %s\n"
"  Encryption:             %s\n"
"  Accurate:               %s\n"
"  Volume name(s):         %s\n"
"  Volume Session Id:      %d\n"
"  Volume Session Time:    %d\n"
"  Last Volume Bytes:      %s (%sB)\n"
"  Non-fatal FD errors:    %d\n"
"  SD Errors:              %d\n"
"  FD termination status:  %s\n"
"  SD termination status:  %s\n"
"  Termination:            %s\n\n"),
        BACULA, my_name, VERSION, LSMDATE, edt,
        HOST_OS, DISTNAME, DISTVER,
        jcr->jr.JobId,
        jcr->jr.Job,
        level_to_str(jcr->get_JobLevel()), jcr->since,
        jcr->client->name(), cr.Uname,
        jcr->fileset->name(), jcr->FSCreateTime,
        jcr->pool->name(), jcr->pool_source,
        jcr->catalog->name(), jcr->catalog_source,
        jcr->wstore->name(), jcr->wstore_source,
        schedt,
        sdt,
        edt,
        edit_utime(RunTime, elapsed, sizeof(elapsed)),
        jcr->JobPriority,
        edit_uint64_with_commas(jcr->jr.JobFiles, ec1),
        edit_uint64_with_commas(jcr->SDJobFiles, ec2),
        edit_uint64_with_commas(jcr->jr.JobBytes, ec3),
        edit_uint64_with_suffix(jcr->jr.JobBytes, ec4),
        edit_uint64_with_commas(jcr->SDJobBytes, ec5),
        edit_uint64_with_suffix(jcr->SDJobBytes, ec6),
        kbps,
        compress,
        jcr->VSS?_("yes"):_("no"),
        jcr->Encrypt?_("yes"):_("no"),
        jcr->accurate?_("yes"):_("no"),
        jcr->VolumeName,
        jcr->VolSessionId,
        jcr->VolSessionTime,
        edit_uint64_with_commas(mr.VolBytes, ec7),
        edit_uint64_with_suffix(mr.VolBytes, ec8),
        jcr->JobErrors,
        jcr->SDErrors,
        fd_term_msg,
        sd_term_msg,
        term_msg);

   Dmsg0(100, "Leave backup_cleanup()\n");
}

void update_bootstrap_file(JCR *jcr)
{
   /* Now update the bootstrap file if any */
   if (jcr->JobStatus == JS_Terminated && jcr->jr.JobBytes &&
       jcr->job->WriteBootstrap) {
      FILE *fd;
      BPIPE *bpipe = NULL;
      int got_pipe = 0;
      POOLMEM *fname = get_pool_memory(PM_FNAME);
      fname = edit_job_codes(jcr, fname, jcr->job->WriteBootstrap, "");

      VOL_PARAMS *VolParams = NULL;
      int VolCount;
      char edt[50], ed1[50], ed2[50];

      if (*fname == '|') {
         got_pipe = 1;
         bpipe = open_bpipe(fname+1, 0, "w"); /* skip first char "|" */
         fd = bpipe ? bpipe->wfd : NULL;
      } else {
         /* ***FIXME*** handle BASE */
         fd = fopen(fname, jcr->get_JobLevel()==L_FULL?"w+b":"a+b");
      }
      if (fd) {
         VolCount = db_get_job_volume_parameters(jcr, jcr->db, jcr->JobId,
                    &VolParams);
         if (VolCount == 0) {
            Jmsg(jcr, M_ERROR, 0, _("Could not get Job Volume Parameters to "
                 "update Bootstrap file. ERR=%s\n"), db_strerror(jcr->db));
             if (jcr->SDJobFiles != 0) {
                set_jcr_job_status(jcr, JS_ErrorTerminated);
             }

         }
         /* Start output with when and who wrote it */
         bstrftimes(edt, sizeof(edt), time(NULL));
         fprintf(fd, "# %s - %s - %s%s\n", edt, jcr->jr.Job,
                 level_to_str(jcr->get_JobLevel()), jcr->since);
         for (int i=0; i < VolCount; i++) {
            /* Write the record */
            fprintf(fd, "Volume=\"%s\"\n", VolParams[i].VolumeName);
            fprintf(fd, "MediaType=\"%s\"\n", VolParams[i].MediaType);
            if (VolParams[i].Slot > 0) {
               fprintf(fd, "Slot=%d\n", VolParams[i].Slot);
            }
            fprintf(fd, "VolSessionId=%u\n", jcr->VolSessionId);
            fprintf(fd, "VolSessionTime=%u\n", jcr->VolSessionTime);
            fprintf(fd, "VolAddr=%s-%s\n", 
                    edit_uint64(VolParams[i].StartAddr, ed1),
                    edit_uint64(VolParams[i].EndAddr, ed2));
            fprintf(fd, "FileIndex=%d-%d\n", VolParams[i].FirstIndex,
                         VolParams[i].LastIndex);
         }
         if (VolParams) {
            free(VolParams);
         }
         if (got_pipe) {
            close_bpipe(bpipe);
         } else {
            fclose(fd);
         }
      } else {
         berrno be;
         Jmsg(jcr, M_ERROR, 0, _("Could not open WriteBootstrap file:\n"
              "%s: ERR=%s\n"), fname, be.bstrerror());
         set_jcr_job_status(jcr, JS_ErrorTerminated);
      }
      free_pool_memory(fname);
   }
}
