/*
 *   Bacula Director -- restore.c -- responsible for restoring files
 *
 *     Kern Sibbald, November MM
 *
 *    This routine is run as a separate thread.  
 * 
 * Current implementation is Catalog verification only (i.e. no
 *  verification versus tape).
 *
 *  Basic tasks done here:
 *     Open DB
 *     Open Message Channel with Storage daemon to tell him a job will be starting.
 *     Open connection with File daemon and pass him commands
 *	 to do the restore.
 *     Update the DB according to what files where restored????
 *
 *   Version $Id$
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

#include "bacula.h"
#include "dird.h"

/* Commands sent to File daemon */
static char restorecmd[]   = "restore replace=%c prelinks=%d where=%s\n";
static char storaddr[]     = "storage address=%s port=%d ssl=0\n";
static char sessioncmd[]   = "session %s %ld %ld %ld %ld %ld %ld\n";  

/* Responses received from File daemon */
static char OKrestore[]   = "2000 OK restore\n";
static char OKstore[]     = "2000 OK storage\n";
static char OKsession[]   = "2000 OK session\n";

/* Forward referenced functions */
static void restore_cleanup(JCR *jcr, int status);

/* External functions */

/* 
 * Do a restore of the specified files
 *    
 *  Returns:  0 on failure
 *	      1 on success
 */
int do_restore(JCR *jcr) 
{
   BSOCK   *fd;
   JOB_DBR rjr; 		      /* restore job record */

   if (!get_or_create_client_record(jcr)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }

   memset(&rjr, 0, sizeof(rjr));
   jcr->jr.Level = 'F';            /* Full restore */
   jcr->jr.StartTime = jcr->start_time;
   if (!db_update_job_start_record(jcr, jcr->db, &jcr->jr)) {
      Jmsg(jcr, M_ERROR, 0, "%s", db_strerror(jcr->db));
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }
   Dmsg0(20, "Updated job start record\n");
   jcr->fname = (char *) get_pool_memory(PM_FNAME);

   Dmsg1(20, "RestoreJobId=%d\n", jcr->job->RestoreJobId);

   /* 
    * The following code is kept temporarily for compatibility.
    * It is the predecessor to the Bootstrap file.
    */
   if (!jcr->RestoreBootstrap) {
      /*
       * Find Job Record for Files to be restored
       */
      if (jcr->RestoreJobId != 0) {
	 rjr.JobId = jcr->RestoreJobId;     /* specified by UA */
      } else {
	 rjr.JobId = jcr->job->RestoreJobId; /* specified by Job Resource */
      }
      if (!db_get_job_record(jcr, jcr->db, &rjr)) {
         Jmsg2(jcr, M_FATAL, 0, _("Cannot get job record id=%d %s"), rjr.JobId,
	    db_strerror(jcr->db));
	 restore_cleanup(jcr, JS_ErrorTerminated);
	 return 0;
      }

      /*
       * Now find the Volumes we will need for the Restore
       */
      jcr->VolumeName[0] = 0;
      if (!db_get_job_volume_names(jcr, jcr->db, rjr.JobId, &jcr->VolumeName) ||
	   jcr->VolumeName[0] == 0) {
         Jmsg(jcr, M_FATAL, 0, _("Cannot find Volume Name for restore Job %d. %s"), 
	    rjr.JobId, db_strerror(jcr->db));
	 restore_cleanup(jcr, JS_ErrorTerminated);
	 return 0;
      }
      Dmsg1(20, "Got job Volume Names: %s\n", jcr->VolumeName);
   }
      

   /* Print Job Start message */
   Jmsg(jcr, M_INFO, 0, _("Start Restore Job %s\n"), jcr->Job);

   /*
    * Open a message channel connection with the Storage
    * daemon. This is to let him know that our client
    * will be contacting him for a backup  session.
    *
    */
   Dmsg0(10, "Open connection with storage daemon\n");
   set_jcr_job_status(jcr, JS_Blocked);
   /*
    * Start conversation with Storage daemon  
    */
   if (!connect_to_storage_daemon(jcr, 10, SDConnectTimeout, 1)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }
   /*
    * Now start a job with the Storage daemon
    */
   if (!start_storage_daemon_job(jcr)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }
   /*
    * Now start a Storage daemon message thread
    */
   if (!start_storage_daemon_message_thread(jcr)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }
   Dmsg0(50, "Storage daemon connection OK\n");

   /* 
    * Start conversation with File daemon  
    */
   if (!connect_to_file_daemon(jcr, 10, FDConnectTimeout, 1)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }

   fd = jcr->file_bsock;
   set_jcr_job_status(jcr, JS_Running);

   if (!send_include_list(jcr)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }

   if (!send_exclude_list(jcr)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }

   /* 
    * send Storage daemon address to the File daemon,
    *	then wait for File daemon to make connection
    *	with Storage daemon.
    */
   set_jcr_job_status(jcr, JS_Blocked);
   if (jcr->store->SDDport == 0) {
      jcr->store->SDDport = jcr->store->SDport;
   }
   bnet_fsend(fd, storaddr, jcr->store->address, jcr->store->SDDport);
   Dmsg1(6, "dird>filed: %s\n", fd->msg);
   if (!response(jcr, fd, OKstore, "Storage", DISPLAY_ERROR)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }
   set_jcr_job_status(jcr, JS_Running);

   /* 
    * Send the bootstrap file -- what Volumes/files to restore
    */
   if (!send_bootstrap_file(jcr)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }

   /* 
    * The following code is deprecated	 
    */
   if (!jcr->RestoreBootstrap) {
      /*
       * Pass the VolSessionId, VolSessionTime, Start and
       * end File and Blocks on the session command.
       */
      bnet_fsend(fd, sessioncmd, 
		jcr->VolumeName,
		rjr.VolSessionId, rjr.VolSessionTime, 
		rjr.StartFile, rjr.EndFile, rjr.StartBlock, 
		rjr.EndBlock);
      if (!response(jcr, fd, OKsession, "Session", DISPLAY_ERROR)) {
	 restore_cleanup(jcr, JS_ErrorTerminated);
	 return 0;
      }
   }

   /* Send restore command */
   char replace, *where;

   if (jcr->replace != 0) {
      replace = jcr->replace;
   } else if (jcr->job->replace != 0) {
      replace = jcr->job->replace;
   } else {
      replace = REPLACE_ALWAYS;       /* always replace */
   }
   if (jcr->where) {
      where = jcr->where;	      /* override */
   } else if (jcr->job->RestoreWhere) {
      where = jcr->job->RestoreWhere; /* no override take from job */
   } else {
      where = "";                     /* None */
   }
   bash_spaces(where);
   bnet_fsend(fd, restorecmd, replace, jcr->prefix_links, where);
   unbash_spaces(where);

   if (!response(jcr, fd, OKrestore, "Restore", DISPLAY_ERROR)) {
      restore_cleanup(jcr, JS_ErrorTerminated);
      return 0;
   }

   /* Wait for Job Termination */
   int stat = wait_for_job_termination(jcr);
   restore_cleanup(jcr, stat);

   return 1;
}

/*
 * Release resources allocated during restore.
 *
 */
static void restore_cleanup(JCR *jcr, int TermCode)
{
   char sdt[MAX_TIME_LENGTH], edt[MAX_TIME_LENGTH];
   char ec1[30], ec2[30];
   char term_code[100], fd_term_msg[100], sd_term_msg[100];
   char *term_msg;
   int msg_type;
   double kbps;

   Dmsg0(20, "In restore_cleanup\n");
   set_jcr_job_status(jcr, TermCode);

   update_job_end_record(jcr);

   msg_type = M_INFO;		      /* by default INFO message */
   switch (TermCode) {
   case JS_Terminated:
      term_msg = _("Restore OK");
      break;
   case JS_FatalError:
   case JS_ErrorTerminated:
      term_msg = _("*** Restore Error ***"); 
      msg_type = M_ERROR;	   /* Generate error message */
      if (jcr->store_bsock) {
	 bnet_sig(jcr->store_bsock, BNET_TERMINATE);
	 pthread_cancel(jcr->SD_msg_chan);
      }
      break;
   case JS_Canceled:
      term_msg = _("Restore Canceled");
      if (jcr->store_bsock) {
	 bnet_sig(jcr->store_bsock, BNET_TERMINATE);
	 pthread_cancel(jcr->SD_msg_chan);
      }
      break;
   default:
      term_msg = term_code;
      sprintf(term_code, _("Inappropriate term code: %c\n"), TermCode);
      break;
   }
   bstrftime(sdt, sizeof(sdt), jcr->jr.StartTime);
   bstrftime(edt, sizeof(edt), jcr->jr.EndTime);
   if (jcr->jr.EndTime - jcr->jr.StartTime > 0) {
      kbps = (double)jcr->jr.JobBytes / (1000 * (jcr->jr.EndTime - jcr->jr.StartTime));
   } else {
      kbps = 0;
   }
   if (kbps < 0.05) {
      kbps = 0;
   }

   jobstatus_to_ascii(jcr->FDJobStatus, fd_term_msg, sizeof(fd_term_msg));
   jobstatus_to_ascii(jcr->SDJobStatus, sd_term_msg, sizeof(sd_term_msg));

   Jmsg(jcr, msg_type, 0, _("Bacula " VERSION " (" LSMDATE "): %s\n\
JobId:                  %d\n\
Job:                    %s\n\
Client:                 %s\n\
Start time:             %s\n\
End time:               %s\n\
Files Restored:         %s\n\
Bytes Restored:         %s\n\
Rate:                   %.1f KB/s\n\
Non-fatal FD Errors:    %d\n\
FD termination status:  %s\n\
SD termination status:  %s\n\
Termination:            %s\n\n"),
	edt,
	jcr->jr.JobId,
	jcr->jr.Job,
	jcr->client->hdr.name,
	sdt,
	edt,
	edit_uint64_with_commas((uint64_t)jcr->jr.JobFiles, ec1),
	edit_uint64_with_commas(jcr->jr.JobBytes, ec2),
	(float)kbps,
	jcr->Errors,
	fd_term_msg,
	sd_term_msg,
	term_msg);

   Dmsg0(20, "Leaving restore_cleanup\n");
}
