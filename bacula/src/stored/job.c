/*
 *   Job control and execution for Storage Daemon
 *
 *   Kern Sibbald, MM
 *
 *   Version $Id$
 *
 */
/*
   Copyright (C) 2000-2005 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as ammended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */

#include "bacula.h"
#include "stored.h"

/* Imported variables */
extern uint32_t VolSessionTime;

/* Imported functions */
extern uint32_t newVolSessionId();

/* Requests from the Director daemon */
static char jobcmd[] = "JobId=%d job=%127s job_name=%127s client_name=%127s "
      "type=%d level=%d FileSet=%127s NoAttr=%d SpoolAttr=%d FileSetMD5=%127s "
      "SpoolData=%d WritePartAfterJob=%d PreferMountedVols=%d\n";


/* Responses sent to Director daemon */
static char OKjob[]     = "3000 OK Job SDid=%u SDtime=%u Authorization=%s\n";
static char BAD_job[]   = "3915 Bad Job command: %s\n";
//static char OK_query[]  = "3001 OK query\n";
//static char NO_query[]  = "3918 Query failed\n";
//static char BAD_query[] = "3917 Bad query command: %s\n";

/*
 * Director requests us to start a job
 * Basic tasks done here:
 *  - We pickup the JobId to be run from the Director.
 *  - We pickup the device, media, and pool from the Director
 *  - Wait for a connection from the File Daemon (FD)
 *  - Accept commands from the FD (i.e. run the job)
 *  - Return when the connection is terminated or
 *    there is an error.
 */
bool job_cmd(JCR *jcr)
{
   int JobId;
   char auth_key[100];
   BSOCK *dir = jcr->dir_bsock;
   POOL_MEM job_name, client_name, job, fileset_name, fileset_md5;
   int JobType, level, spool_attributes, no_attributes, spool_data;
   int write_part_after_job, PreferMountedVols;

   JCR *ojcr;

   /*
    * Get JobId and permissions from Director
    */
   Dmsg1(100, "<dird: %s\n", dir->msg);
   if (sscanf(dir->msg, jobcmd, &JobId, job.c_str(), job_name.c_str(),
              client_name.c_str(),
              &JobType, &level, fileset_name.c_str(), &no_attributes,
              &spool_attributes, fileset_md5.c_str(), &spool_data, 
              &write_part_after_job, &PreferMountedVols) != 13) {
      pm_strcpy(jcr->errmsg, dir->msg);
      bnet_fsend(dir, BAD_job, jcr->errmsg);
      Dmsg1(100, ">dird: %s\n", dir->msg);
      Emsg1(M_FATAL, 0, _("Bad Job Command from Director: %s\n"), jcr->errmsg);
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return false;
   }
   /*
    * Since this job could be rescheduled, we
    *  check to see if we have it already. If so
    *  free the old jcr and use the new one.
    */
   ojcr = get_jcr_by_full_name(job.c_str());
   if (ojcr && !ojcr->authenticated) {
      Dmsg2(100, "Found ojcr=0x%x Job %s\n", (unsigned)(long)ojcr, job.c_str());
      free_jcr(ojcr);
   }
   jcr->JobId = JobId;
   jcr->VolSessionId = newVolSessionId();
   jcr->VolSessionTime = VolSessionTime;
   bstrncpy(jcr->Job, job, sizeof(jcr->Job));
   unbash_spaces(job_name);
   jcr->job_name = get_pool_memory(PM_NAME);
   pm_strcpy(jcr->job_name, job_name);
   unbash_spaces(client_name);
   jcr->client_name = get_pool_memory(PM_NAME);
   pm_strcpy(jcr->client_name, client_name);
   unbash_spaces(fileset_name);
   jcr->fileset_name = get_pool_memory(PM_NAME);
   pm_strcpy(jcr->fileset_name, fileset_name);
   jcr->JobType = JobType;
   jcr->JobLevel = level;
   jcr->no_attributes = no_attributes;
   jcr->spool_attributes = spool_attributes;
   jcr->spool_data = spool_data;
   jcr->write_part_after_job = write_part_after_job;
   jcr->fileset_md5 = get_pool_memory(PM_NAME);
   pm_strcpy(jcr->fileset_md5, fileset_md5);
   jcr->PreferMountedVols = PreferMountedVols;

   jcr->authenticated = false;

   /*
    * Pass back an authorization key for the File daemon
    */
   make_session_key(auth_key, NULL, 1);
   bnet_fsend(dir, OKjob, jcr->VolSessionId, jcr->VolSessionTime, auth_key);
   Dmsg1(100, ">dird: %s", dir->msg);
   jcr->sd_auth_key = bstrdup(auth_key);
   memset(auth_key, 0, sizeof(auth_key));
   generate_daemon_event(jcr, "JobStart");
   return true;
}

bool run_cmd(JCR *jcr)
{
   struct timeval tv;
   struct timezone tz;
   struct timespec timeout;
   int errstat;

   Dmsg1(100, "Run_cmd: %s\n", jcr->dir_bsock->msg);
   /* The following jobs don't need the FD */
   switch (jcr->JobType) {
   case JT_MIGRATION:
   case JT_COPY:
   case JT_ARCHIVE:
      jcr->authenticated = true;
      run_job(jcr);
      return false;
   }

   set_jcr_job_status(jcr, JS_WaitFD);          /* wait for FD to connect */
   dir_send_job_status(jcr);

   gettimeofday(&tv, &tz);
   timeout.tv_nsec = tv.tv_usec * 1000;
   timeout.tv_sec = tv.tv_sec + 30 * 60;        /* wait 30 minutes */

   Dmsg1(100, "%s waiting on FD to contact SD\n", jcr->Job);
   /*
    * Wait for the File daemon to contact us to start the Job,
    *  when he does, we will be released, unless the 30 minutes
    *  expires.
    */
   P(jcr->mutex);
   for ( ;!job_canceled(jcr); ) {
      errstat = pthread_cond_timedwait(&jcr->job_start_wait, &jcr->mutex, &timeout);
      if (errstat == 0 || errstat == ETIMEDOUT) {
         break;
      }
   }
   V(jcr->mutex);

   memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));

   if (jcr->authenticated && !job_canceled(jcr)) {
      Dmsg1(100, "Running job %s\n", jcr->Job);
      run_job(jcr);                   /* Run the job */
   }
   return false;
}

/*
 * After receiving a connection (in job.c) if it is
 *   from the File daemon, this routine is called.
 */
void handle_filed_connection(BSOCK *fd, char *job_name)
{
   JCR *jcr;

   bmicrosleep(0, 50000);             /* wait 50 millisecs */
   if (!(jcr=get_jcr_by_full_name(job_name))) {
      Jmsg1(NULL, M_FATAL, 0, _("Job name not found: %s\n"), job_name);
      Dmsg1(100, "Job name not found: %s\n", job_name);
      return;
   }

   jcr->file_bsock = fd;
   jcr->file_bsock->jcr = jcr;

   Dmsg1(110, "Found Job %s\n", job_name);

   if (jcr->authenticated) {
      Jmsg2(jcr, M_FATAL, 0, "Hey!!!! JobId %u Job %s already authenticated.\n",
         jcr->JobId, jcr->Job);
      free_jcr(jcr);
      return;
   }

   /*
    * Authenticate the File daemon
    */
   if (jcr->authenticated || !authenticate_filed(jcr)) {
      Dmsg1(100, "Authentication failed Job %s\n", jcr->Job);
      Jmsg(jcr, M_FATAL, 0, _("Unable to authenticate File daemon\n"));
   } else {
      jcr->authenticated = true;
      Dmsg1(110, "OK Authentication Job %s\n", jcr->Job);
   }

   P(jcr->mutex);
   if (!jcr->authenticated) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
   }
   pthread_cond_signal(&jcr->job_start_wait); /* wake waiting job */
   V(jcr->mutex);
   free_jcr(jcr);
   return;
}


#ifdef needed
/*
 *   Query Device command from Director
 *   Sends Storage Daemon's information on the device to the
 *    caller (presumably the Director).
 *   This command always returns "true" so that the line is
 *    not closed on an error.
 *
 */
bool query_cmd(JCR *jcr)
{
   POOL_MEM dev_name, VolumeName, MediaType, ChangerName;
   BSOCK *dir = jcr->dir_bsock;
   DEVRES *device;
   AUTOCHANGER *changer;
   bool ok;

   Dmsg1(100, "Query_cmd: %s", dir->msg);
   ok = sscanf(dir->msg, query_device, dev_name.c_str()) == 1;
   Dmsg1(100, "<dird: %s\n", dir->msg);
   if (ok) {
      unbash_spaces(dev_name);
//    LockRes();
      foreach_res(device, R_DEVICE) {
         /* Find resource, and make sure we were able to open it */
         if (fnmatch(dev_name.c_str(), device->hdr.name, 0) == 0) {
            if (!device->dev) {
               device->dev = init_dev(jcr, device);
            }
            if (!device->dev) {
               break;
            }  
//          UnlockRes();
            ok = dir_update_device(jcr, device->dev);
            if (ok) {
               ok = bnet_fsend(dir, OK_query);
            } else {
               bnet_fsend(dir, NO_query);
            }
            return ok;
         }
      }
      foreach_res(changer, R_AUTOCHANGER) {
         /* Find resource, and make sure we were able to open it */
         if (fnmatch(dev_name.c_str(), changer->hdr.name, 0) == 0) {
//          UnlockRes();
            if (!changer->device || changer->device->size() == 0) {
               continue;              /* no devices */
            }
            ok = dir_update_changer(jcr, changer);
            if (ok) {
               ok = bnet_fsend(dir, OK_query);
            } else {
               bnet_fsend(dir, NO_query);
            }
            return ok;
         }
      }
      /* If we get here, the device/autochanger was not found */
//    UnlockRes();
      unbash_spaces(dir->msg);
      pm_strcpy(jcr->errmsg, dir->msg);
      bnet_fsend(dir, NO_device, dev_name.c_str());
      Dmsg1(100, ">dird: %s\n", dir->msg);
   } else {
      unbash_spaces(dir->msg);
      pm_strcpy(jcr->errmsg, dir->msg);
      bnet_fsend(dir, BAD_query, jcr->errmsg);
      Dmsg1(100, ">dird: %s\n", dir->msg);
   }

   return true;
}

#endif


/*
 * Destroy the Job Control Record and associated
 * resources (sockets).
 */
void stored_free_jcr(JCR *jcr)
{
   if (jcr->file_bsock) {
      bnet_close(jcr->file_bsock);
      jcr->file_bsock = NULL;
   }
   if (jcr->job_name) {
      free_pool_memory(jcr->job_name);
   }
   if (jcr->client_name) {
      free_memory(jcr->client_name);
      jcr->client_name = NULL;
   }
   if (jcr->fileset_name) {
      free_memory(jcr->fileset_name);
   }
   if (jcr->fileset_md5) {
      free_memory(jcr->fileset_md5);
   }
   if (jcr->bsr) {
      free_bsr(jcr->bsr);
      jcr->bsr = NULL;
   }
   if (jcr->RestoreBootstrap) {
      unlink(jcr->RestoreBootstrap);
      free_pool_memory(jcr->RestoreBootstrap);
      jcr->RestoreBootstrap = NULL;
   }
   if (jcr->next_dev || jcr->prev_dev) {
      Emsg0(M_FATAL, 0, _("In free_jcr(), but still attached to device!!!!\n"));
   }
   pthread_cond_destroy(&jcr->job_start_wait);
   if (jcr->dcrs) {
      delete jcr->dcrs;
   }
   jcr->dcrs = NULL;
   if (jcr->dcr) {
      free_dcr(jcr->dcr);
      jcr->dcr = NULL;
   }
   return;
}
