/*
 *
 *   Bacula Director Job processing routines
 *
 *     Kern Sibbald, October MM
 *
 *    Version $Id$
 */
/*
   Copyright (C) 2000-2004 Kern Sibbald and John Walker

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

/* Forward referenced subroutines */
static void *job_thread(void *arg);
static void job_monitor_watchdog(watchdog_t *self);
static void job_monitor_destructor(watchdog_t *self);
static bool job_check_maxwaittime(JCR *control_jcr, JCR *jcr);
static bool job_check_maxruntime(JCR *control_jcr, JCR *jcr);

/* Exported subroutines */

/* Imported subroutines */
extern void term_scheduler();
extern void term_ua_server();
extern int do_backup(JCR *jcr);
extern int do_admin(JCR *jcr);
extern int do_restore(JCR *jcr);
extern int do_verify(JCR *jcr);

/* Imported variables */
extern time_t watchdog_time;

jobq_t	job_queue;

void init_job_server(int max_workers)
{
   int stat;
   watchdog_t *wd;
   
   if ((stat = jobq_init(&job_queue, max_workers, job_thread)) != 0) {
      berrno be;
      be.set_errno(stat);
      Emsg1(M_ABORT, 0, _("Could not init job queue: ERR=%s\n"), be.strerror());
   }
   if ((wd = new_watchdog()) == NULL) {
      Emsg0(M_ABORT, 0, _("Could not init job monitor watchdogs\n"));
   }
   wd->callback = job_monitor_watchdog;
   wd->destructor = job_monitor_destructor;
   wd->one_shot = false;
   wd->interval = 60;
   wd->data = new_control_jcr("*JobMonitor*", JT_SYSTEM);
   register_watchdog(wd);
}

void term_job_server()
{
   int stat;
   if ((stat=jobq_destroy(&job_queue)) != 0) {
      berrno be;
      be.set_errno(stat);
      Emsg1(M_INFO, 0, _("Could not term job queue: ERR=%s\n"), be.strerror());
   }
}

/*
 * Run a job -- typically called by the scheduler, but may also
 *		be called by the UA (Console program).
 *
 *  Returns: 0 on failure
 *	     JobId on success
 *
 */
JobId_t run_job(JCR *jcr)
{
   int stat, errstat;
   JobId_t JobId = 0;

   P(jcr->mutex);
   sm_check(__FILE__, __LINE__, true);
   init_msg(jcr, jcr->messages);

   /* Initialize termination condition variable */
   if ((errstat = pthread_cond_init(&jcr->term_wait, NULL)) != 0) {
      berrno be;
      be.set_errno(errstat);
      Jmsg1(jcr, M_FATAL, 0, _("Unable to init job cond variable: ERR=%s\n"), be.strerror());
      goto bail_out;
   }
   jcr->term_wait_inited = true;

   /*
    * Open database
    */
   Dmsg0(50, "Open database\n");
   jcr->db=db_init_database(jcr, jcr->catalog->db_name, jcr->catalog->db_user,
			    jcr->catalog->db_password, jcr->catalog->db_address,
			    jcr->catalog->db_port, jcr->catalog->db_socket);
   if (!jcr->db || !db_open_database(jcr, jcr->db)) {
      Jmsg(jcr, M_FATAL, 0, _("Could not open database \"%s\".\n"),
		 jcr->catalog->db_name);
      if (jcr->db) {
         Jmsg(jcr, M_FATAL, 0, "%s", db_strerror(jcr->db));
      }
      goto bail_out;
   }
   Dmsg0(50, "DB opened\n");

   /*
    * Create Job record  
    */
   create_unique_job_name(jcr, jcr->job->hdr.name);
   set_jcr_job_status(jcr, JS_Created);
   init_jcr_job_record(jcr);
   if (!db_create_job_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_FATAL, 0, "%s", db_strerror(jcr->db));
      goto bail_out;
   }
   JobId = jcr->JobId = jcr->jr.JobId;

   Dmsg4(100, "Created job record JobId=%d Name=%s Type=%c Level=%c\n", 
       jcr->JobId, jcr->Job, jcr->jr.JobType, jcr->jr.JobLevel);
   Dmsg0(200, "Add jrc to work queue\n");

   /* Queue the job to be run */
   if ((stat = jobq_add(&job_queue, jcr)) != 0) {
      berrno be;
      be.set_errno(stat);
      Jmsg(jcr, M_FATAL, 0, _("Could not add job queue: ERR=%s\n"), be.strerror());
      JobId = 0;
      goto bail_out;
   }
   Dmsg0(100, "Done run_job()\n");

   V(jcr->mutex);
   return JobId;

bail_out:
   set_jcr_job_status(jcr, JS_ErrorTerminated);
   V(jcr->mutex);
   return JobId;

}


/* 
 * This is the engine called by jobq.c:jobq_add() when we were pulled		     
 *  from the work queue.
 *  At this point, we are running in our own thread and all
 *    necessary resources are allocated -- see jobq.c
 */
static void *job_thread(void *arg)
{
   JCR *jcr = (JCR *)arg;

   jcr->my_thread_id = pthread_self();
   pthread_detach(jcr->my_thread_id);
   sm_check(__FILE__, __LINE__, true);

   for ( ;; ) {
      Dmsg0(200, "=====Start Job=========\n");
      jcr->start_time = time(NULL);	 /* set the real start time */
      set_jcr_job_status(jcr, JS_Running);

      if (job_canceled(jcr)) {
	 update_job_end_record(jcr);
      } else if (jcr->job->MaxStartDelay != 0 && jcr->job->MaxStartDelay <
	  (utime_t)(jcr->start_time - jcr->sched_time)) {
         Jmsg(jcr, M_FATAL, 0, _("Job canceled because max start delay time exceeded.\n"));
	 set_jcr_job_status(jcr, JS_Canceled);
	 update_job_end_record(jcr);
      } else {

	 /* Run Job */
	 if (jcr->job->RunBeforeJob) {
	    POOLMEM *before = get_pool_memory(PM_FNAME);
	    int status;
	    BPIPE *bpipe;
	    char line[MAXSTRING];
	    
            before = edit_job_codes(jcr, before, jcr->job->RunBeforeJob, "");
            bpipe = open_bpipe(before, 0, "r");
	    free_pool_memory(before);
	    while (fgets(line, sizeof(line), bpipe->rfd)) {
               Jmsg(jcr, M_INFO, 0, _("RunBefore: %s"), line);
	    }
	    status = close_bpipe(bpipe);
	    if (status != 0) {
	       berrno be;
	       be.set_errno(status);
               Jmsg(jcr, M_FATAL, 0, _("RunBeforeJob error: ERR=%s\n"), be.strerror());
	       set_jcr_job_status(jcr, JS_FatalError);
	       update_job_end_record(jcr);
	       goto bail_out;
	    }
	 }
	 switch (jcr->JobType) {
	 case JT_BACKUP:
	    do_backup(jcr);
	    if (jcr->JobStatus == JS_Terminated) {
	       do_autoprune(jcr);
	    }
	    break;
	 case JT_VERIFY:
	    do_verify(jcr);
	    if (jcr->JobStatus == JS_Terminated) {
	       do_autoprune(jcr);
	    }
	    break;
	 case JT_RESTORE:
	    do_restore(jcr);
	    if (jcr->JobStatus == JS_Terminated) {
	       do_autoprune(jcr);
	    }
	    break;
	 case JT_ADMIN:
	    do_admin(jcr);
	    if (jcr->JobStatus == JS_Terminated) {
	       do_autoprune(jcr);
	    }
	    break;
	 default:
            Pmsg1(0, "Unimplemented job type: %d\n", jcr->JobType);
	    break;
	 }
	 if ((jcr->job->RunAfterJob && jcr->JobStatus == JS_Terminated) ||
	     (jcr->job->RunAfterFailedJob && jcr->JobStatus != JS_Terminated)) {
	    POOLMEM *after = get_pool_memory(PM_FNAME);
	    int status;
	    BPIPE *bpipe;
	    char line[MAXSTRING];
	    
	    if (jcr->JobStatus == JS_Terminated) {
               after = edit_job_codes(jcr, after, jcr->job->RunAfterJob, "");
	    } else {
               after = edit_job_codes(jcr, after, jcr->job->RunAfterFailedJob, "");
	    }
            bpipe = open_bpipe(after, 0, "r");
	    free_pool_memory(after);
	    while (fgets(line, sizeof(line), bpipe->rfd)) {
               Jmsg(jcr, M_INFO, 0, _("RunAfter: %s"), line);
	    }
	    status = close_bpipe(bpipe);
	    /*
	     * Note, if we get an error here, do not mark the
	     *	job in error, simply report the error condition.   
	     */
	    if (status != 0) {
	       berrno be;
	       be.set_errno(status);
	       if (jcr->JobStatus == JS_Terminated) {
                  Jmsg(jcr, M_WARNING, 0, _("RunAfterJob error: ERR=%s\n"), be.strerror());
	       } else {
                  Jmsg(jcr, M_FATAL, 0, _("RunAfterFailedJob error: ERR=%s\n"), be.strerror());
	       }
	    }
	 }
	 /* Send off any queued messages */
	 if (jcr->msg_queue->size() > 0) {
	    dequeue_messages(jcr);
	 }
      }
bail_out:
      break;
   }

   Dmsg0(50, "======== End Job ==========\n");
   sm_check(__FILE__, __LINE__, true);
   return NULL;
}


/*
 * Cancel a job -- typically called by the UA (Console program), but may also
 *		be called by the job watchdog.
 * 
 *  Returns: 1 if cancel appears to be successful
 *	     0 on failure. Message sent to ua->jcr.
 */
int cancel_job(UAContext *ua, JCR *jcr)
{
   BSOCK *sd, *fd;

   switch (jcr->JobStatus) {
   case JS_Created:
   case JS_WaitJobRes:
   case JS_WaitClientRes:
   case JS_WaitStoreRes:
   case JS_WaitPriority:
   case JS_WaitMaxJobs:
   case JS_WaitStartTime:
      set_jcr_job_status(jcr, JS_Canceled);
      bsendmsg(ua, _("JobId %d, Job %s marked to be canceled.\n"),
	      jcr->JobId, jcr->Job);
      jobq_remove(&job_queue, jcr); /* attempt to remove it from queue */
      return 1;
	 
   default:
      set_jcr_job_status(jcr, JS_Canceled);

      /* Cancel File daemon */
      if (jcr->file_bsock) {
	 ua->jcr->client = jcr->client;
	 if (!connect_to_file_daemon(ua->jcr, 10, FDConnectTimeout, 1)) {
            bsendmsg(ua, _("Failed to connect to File daemon.\n"));
	    return 0;
	 }
         Dmsg0(200, "Connected to file daemon\n");
	 fd = ua->jcr->file_bsock;
         bnet_fsend(fd, "cancel Job=%s\n", jcr->Job);
	 while (bnet_recv(fd) >= 0) {
            bsendmsg(ua, "%s", fd->msg);
	 }
	 bnet_sig(fd, BNET_TERMINATE);
	 bnet_close(fd);
	 ua->jcr->file_bsock = NULL;
      }

      /* Cancel Storage daemon */
      if (jcr->store_bsock) {
	 ua->jcr->store = jcr->store;
	 if (!connect_to_storage_daemon(ua->jcr, 10, SDConnectTimeout, 1)) {
            bsendmsg(ua, _("Failed to connect to Storage daemon.\n"));
	    return 0;
	 }
         Dmsg0(200, "Connected to storage daemon\n");
	 sd = ua->jcr->store_bsock;
         bnet_fsend(sd, "cancel Job=%s\n", jcr->Job);
	 while (bnet_recv(sd) >= 0) {
            bsendmsg(ua, "%s", sd->msg);
	 }
	 bnet_sig(sd, BNET_TERMINATE);
	 bnet_close(sd);
	 ua->jcr->store_bsock = NULL;
      }
   }

   return 1;
}


static void job_monitor_destructor(watchdog_t *self)
{
   JCR *control_jcr = (JCR *) self->data;

   free_jcr(control_jcr);
}

static void job_monitor_watchdog(watchdog_t *self)
{
   JCR *control_jcr, *jcr;

   control_jcr = (JCR *)self->data;

   Dmsg1(400, "job_monitor_watchdog %p called\n", self);

   lock_jcr_chain();

   foreach_jcr(jcr) {
      bool cancel;

      if (jcr->JobId == 0) {
         Dmsg2(400, "Skipping JCR %p (%s) with JobId 0\n",
	       jcr, jcr->Job);
	 /* Keep reference counts correct */
	 free_locked_jcr(jcr);
	 continue;
      }

      /* check MaxWaitTime */
      cancel = job_check_maxwaittime(control_jcr, jcr);

      /* check MaxRunTime */
      cancel |= job_check_maxruntime(control_jcr, jcr);

      if (cancel) {
         Dmsg3(200, "Cancelling JCR %p jobid %d (%s)\n",
	       jcr, jcr->JobId, jcr->Job);

	 UAContext *ua = new_ua_context(jcr);
	 ua->jcr = control_jcr;
	 cancel_job(ua, jcr);
	 free_ua_context(ua);

         Dmsg1(200, "Have cancelled JCR %p\n", jcr);
      }

      /* Keep reference counts correct */
      free_locked_jcr(jcr);
   }
   unlock_jcr_chain();
}

/*
 * Check if the maxwaittime has expired and it is possible
 *  to cancel the job.
 */
static bool job_check_maxwaittime(JCR *control_jcr, JCR *jcr)
{
   bool cancel = false;

   if (jcr->job->MaxWaitTime == 0) {
      return false;
   }
   if ((watchdog_time - jcr->start_time) < jcr->job->MaxWaitTime) {
      Dmsg3(200, "Job %p (%s) with MaxWaitTime %d not expired\n",
	    jcr, jcr->Job, jcr->job->MaxWaitTime);
      return false;
   }
   Dmsg3(200, "Job %d (%s): MaxWaitTime of %d seconds exceeded, "
         "checking status\n",
	 jcr->JobId, jcr->Job, jcr->job->MaxWaitTime);
   switch (jcr->JobStatus) {
   case JS_Created:
   case JS_Blocked:
   case JS_WaitFD:
   case JS_WaitSD:
   case JS_WaitStoreRes:
   case JS_WaitClientRes:
   case JS_WaitJobRes:
   case JS_WaitPriority:
   case JS_WaitMaxJobs:
   case JS_WaitStartTime:
      cancel = true;
      Dmsg0(200, "JCR blocked in #1\n");
      break;
   case JS_Running:
      Dmsg0(200, "JCR running, checking SD status\n");
      switch (jcr->SDJobStatus) {
      case JS_WaitMount:
      case JS_WaitMedia:
      case JS_WaitFD:
	 cancel = true;
         Dmsg0(200, "JCR blocked in #2\n");
	 break;
      default:
         Dmsg0(200, "JCR not blocked in #2\n");
	 break;
      }
      break;
   case JS_Terminated:
   case JS_ErrorTerminated:
   case JS_Canceled:
   case JS_FatalError:
      Dmsg0(200, "JCR already dead in #3\n");
      break;
   default:
      Jmsg1(jcr, M_ERROR, 0, _("Unhandled job status code %d\n"),
	    jcr->JobStatus);
   }
   Dmsg3(200, "MaxWaitTime result: %scancel JCR %p (%s)\n",
         cancel ? "" : "do not ", jcr, jcr->job);

   return cancel;
}

/*
 * Check if maxruntime has expired and if the job can be
 *   canceled.
 */
static bool job_check_maxruntime(JCR *control_jcr, JCR *jcr)
{
   bool cancel = false;

   if (jcr->job->MaxRunTime == 0) {
      return false;
   }
   if ((watchdog_time - jcr->start_time) < jcr->job->MaxRunTime) {
      Dmsg3(200, "Job %p (%s) with MaxRunTime %d not expired\n",
	    jcr, jcr->Job, jcr->job->MaxRunTime);
      return false;
   }

   switch (jcr->JobStatus) {
   case JS_Created:
   case JS_Running:
   case JS_Blocked:
   case JS_WaitFD:
   case JS_WaitSD:
   case JS_WaitStoreRes:
   case JS_WaitClientRes:
   case JS_WaitJobRes:
   case JS_WaitPriority:
   case JS_WaitMaxJobs:
   case JS_WaitStartTime:
   case JS_Differences:
      cancel = true;
      break;
   case JS_Terminated:
   case JS_ErrorTerminated:
   case JS_Canceled:
   case JS_FatalError:
      cancel = false;
      break;
   default:
      Jmsg1(jcr, M_ERROR, 0, _("Unhandled job status code %d\n"),
	    jcr->JobStatus);
   }

   Dmsg3(200, "MaxRunTime result: %scancel JCR %p (%s)\n",
         cancel ? "" : "do not ", jcr, jcr->job);

   return cancel;
}


/*
 * Get or create a Client record for this Job
 */
bool get_or_create_client_record(JCR *jcr)
{
   CLIENT_DBR cr;

   memset(&cr, 0, sizeof(cr));
   bstrncpy(cr.Name, jcr->client->hdr.name, sizeof(cr.Name));
   cr.AutoPrune = jcr->client->AutoPrune;
   cr.FileRetention = jcr->client->FileRetention;
   cr.JobRetention = jcr->client->JobRetention;
   if (!jcr->client_name) {
      jcr->client_name = get_pool_memory(PM_NAME);
   }
   pm_strcpy(jcr->client_name, jcr->client->hdr.name);
   if (!db_create_client_record(jcr, jcr->db, &cr)) {
      Jmsg(jcr, M_FATAL, 0, _("Could not create Client record. ERR=%s\n"), 
	 db_strerror(jcr->db));
      return false;
   }
   jcr->jr.ClientId = cr.ClientId;
   if (cr.Uname[0]) {
      if (!jcr->client_uname) {
	 jcr->client_uname = get_pool_memory(PM_NAME);
      }
      pm_strcpy(jcr->client_uname, cr.Uname);
   }
   Dmsg2(100, "Created Client %s record %d\n", jcr->client->hdr.name, 
      jcr->jr.ClientId);
   return true;
}

bool get_or_create_fileset_record(JCR *jcr, FILESET_DBR *fsr)
{
   /*
    * Get or Create FileSet record
    */
   memset(fsr, 0, sizeof(FILESET_DBR));
   bstrncpy(fsr->FileSet, jcr->fileset->hdr.name, sizeof(fsr->FileSet));
   if (jcr->fileset->have_MD5) {
      struct MD5Context md5c;
      unsigned char signature[16];
      memcpy(&md5c, &jcr->fileset->md5c, sizeof(md5c));
      MD5Final(signature, &md5c);
      bin_to_base64(fsr->MD5, (char *)signature, 16); /* encode 16 bytes */
      bstrncpy(jcr->fileset->MD5, fsr->MD5, sizeof(jcr->fileset->MD5));
   } else {
      Jmsg(jcr, M_WARNING, 0, _("FileSet MD5 signature not found.\n"));
   }
   if (!db_create_fileset_record(jcr, jcr->db, fsr)) {
      Jmsg(jcr, M_ERROR, 0, _("Could not create FileSet \"%s\" record. ERR=%s\n"), 
	 fsr->FileSet, db_strerror(jcr->db));
      return false;
   }   
   jcr->jr.FileSetId = fsr->FileSetId;
   if (fsr->created) {
      Jmsg(jcr, M_INFO, 0, _("Created new FileSet record \"%s\" %s\n"), 
	 fsr->FileSet, fsr->cCreateTime);
   }
   Dmsg2(119, "Created FileSet %s record %u\n", jcr->fileset->hdr.name, 
      jcr->jr.FileSetId);
   return true;
}

void init_jcr_job_record(JCR *jcr)
{
   jcr->jr.SchedTime = jcr->sched_time;
   jcr->jr.StartTime = jcr->start_time;
   jcr->jr.EndTime = 0; 	      /* perhaps rescheduled, clear it */
   jcr->jr.JobType = jcr->JobType;
   jcr->jr.JobLevel = jcr->JobLevel;
   jcr->jr.JobStatus = jcr->JobStatus;
   jcr->jr.JobId = jcr->JobId;
   bstrncpy(jcr->jr.Name, jcr->job->hdr.name, sizeof(jcr->jr.Name));
   bstrncpy(jcr->jr.Job, jcr->Job, sizeof(jcr->jr.Job));
}

/*
 * Write status and such in DB
 */
void update_job_end_record(JCR *jcr)
{
   jcr->jr.EndTime = time(NULL);
   jcr->end_time = jcr->jr.EndTime;
   jcr->jr.JobId = jcr->JobId;
   jcr->jr.JobStatus = jcr->JobStatus;
   jcr->jr.JobFiles = jcr->JobFiles;
   jcr->jr.JobBytes = jcr->JobBytes;
   jcr->jr.VolSessionId = jcr->VolSessionId;
   jcr->jr.VolSessionTime = jcr->VolSessionTime;
   if (!db_update_job_end_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_WARNING, 0, _("Error updating job record. %s"), 
	 db_strerror(jcr->db));
   }
}

/*
 * Takes base_name and appends (unique) current
 *   date and time to form unique job name.
 *
 *  Returns: unique job name in jcr->Job
 *    date/time in jcr->start_time
 */
void create_unique_job_name(JCR *jcr, const char *base_name)
{
   /* Job start mutex */
   static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
   static time_t last_start_time = 0;
   time_t now;
   struct tm tm;
   char dt[MAX_TIME_LENGTH];
   char name[MAX_NAME_LENGTH];
   char *p;

   /* Guarantee unique start time -- maximum one per second, and
    * thus unique Job Name 
    */
   P(mutex);			      /* lock creation of jobs */
   now = time(NULL);
   while (now == last_start_time) {
      bmicrosleep(0, 500000);
      now = time(NULL);
   }
   last_start_time = now;
   V(mutex);			      /* allow creation of jobs */
   jcr->start_time = now;
   /* Form Unique JobName */
   localtime_r(&now, &tm);
   /* Use only characters that are permitted in Windows filenames */
   strftime(dt, sizeof(dt), "%Y-%m-%d_%H.%M.%S", &tm); 
   bstrncpy(name, base_name, sizeof(name));
   name[sizeof(name)-22] = 0;	       /* truncate if too long */
   bsnprintf(jcr->Job, sizeof(jcr->Job), "%s.%s", name, dt); /* add date & time */
   /* Convert spaces into underscores */
   for (p=jcr->Job; *p; p++) {
      if (*p == ' ') {
         *p = '_';
      }
   }
}

/*
 * Free the Job Control Record if no one is still using it.
 *  Called from main free_jcr() routine in src/lib/jcr.c so
 *  that we can do our Director specific cleanup of the jcr.
 */
void dird_free_jcr(JCR *jcr)
{
   Dmsg0(200, "Start dird free_jcr\n");

   if (jcr->sd_auth_key) {
      free(jcr->sd_auth_key);
      jcr->sd_auth_key = NULL;
   }
   if (jcr->where) {
      free(jcr->where);
      jcr->where = NULL;
   }
   if (jcr->file_bsock) {
      Dmsg0(200, "Close File bsock\n");
      bnet_close(jcr->file_bsock);
      jcr->file_bsock = NULL;
   }
   if (jcr->store_bsock) {
      Dmsg0(200, "Close Store bsock\n");
      bnet_close(jcr->store_bsock);
      jcr->store_bsock = NULL;
   }
   if (jcr->fname) {  
      Dmsg0(200, "Free JCR fname\n");
      free_pool_memory(jcr->fname);
      jcr->fname = NULL;
   }
   if (jcr->stime) {
      Dmsg0(200, "Free JCR stime\n");
      free_pool_memory(jcr->stime);
      jcr->stime = NULL;
   }
   if (jcr->RestoreBootstrap) {
      free(jcr->RestoreBootstrap);
      jcr->RestoreBootstrap = NULL;
   }
   if (jcr->client_uname) {
      free_pool_memory(jcr->client_uname);
      jcr->client_uname = NULL;
   }
   if (jcr->term_wait_inited) {
      pthread_cond_destroy(&jcr->term_wait);
   }
   jcr->job_end_push.destroy();
   Dmsg0(200, "End dird free_jcr\n");
}

/*
 * Set some defaults in the JCR necessary to
 * run. These items are pulled from the job
 * definition as defaults, but can be overridden
 * later either by the Run record in the Schedule resource,
 * or by the Console program.
 */
void set_jcr_defaults(JCR *jcr, JOB *job)
{
   jcr->job = job;
   jcr->JobType = job->JobType;
   switch (jcr->JobType) {
   case JT_ADMIN:
   case JT_RESTORE:
      jcr->JobLevel = L_NONE;
      break;
   default:
      jcr->JobLevel = job->level;
      break;
   }
   jcr->JobPriority = job->Priority;
   jcr->store = job->storage;
   jcr->client = job->client;
   if (!jcr->client_name) {
      jcr->client_name = get_pool_memory(PM_NAME);
   }
   pm_strcpy(jcr->client_name, jcr->client->hdr.name);
   jcr->pool = job->pool;
   jcr->full_pool = job->full_pool;
   jcr->inc_pool = job->inc_pool;
   jcr->dif_pool = job->dif_pool;
   jcr->catalog = job->client->catalog;
   jcr->fileset = job->fileset;
   jcr->messages = job->messages; 
   jcr->spool_data = job->spool_data;
   if (jcr->RestoreBootstrap) {
      free(jcr->RestoreBootstrap);
      jcr->RestoreBootstrap = NULL;
   }
   /* This can be overridden by Console program */
   if (job->RestoreBootstrap) {
      jcr->RestoreBootstrap = bstrdup(job->RestoreBootstrap);
   }
   /* If no default level given, set one */
   if (jcr->JobLevel == 0) {
      switch (jcr->JobType) {
      case JT_VERIFY:
	 jcr->JobLevel = L_VERIFY_CATALOG;
	 break;
      case JT_BACKUP:
	 jcr->JobLevel = L_INCREMENTAL;
	 break;
      case JT_RESTORE:
      case JT_ADMIN:
	 jcr->JobLevel = L_NONE;
	 break;
      default:
	 break;
      }
   }
}
