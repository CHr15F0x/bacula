/*
 * Bacula job queue routines.
 *
 *  This code consists of three queues, the waiting_jobs
 *  queue, where jobs are initially queued, the ready_jobs
 *  queue, where jobs are placed when all the resources are
 *  allocated and they can immediately be run, and the
 *  running queue where jobs are placed when they are
 *  running.
 *
 *  Kern Sibbald, July MMIII
 *
 *   Version $Id$
 *
 *  This code was adapted from the Bacula workq, which was
 *    adapted from "Programming with POSIX Threads", by
 *    David R. Butenhof
 *
 */
/*
   Copyright (C) 2003-2006 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as amended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */

#include "bacula.h"
#include "dird.h"

extern JCR *jobs;

/* Forward referenced functions */
extern "C" void *jobq_server(void *arg);
extern "C" void *sched_wait(void *arg);

static int  start_server(jobq_t *jq);
static bool acquire_resources(JCR *jcr);



/*
 * Initialize a job queue
 *
 *  Returns: 0 on success
 *           errno on failure
 */
int jobq_init(jobq_t *jq, int threads, void *(*engine)(void *arg))
{
   int stat;
   jobq_item_t *item = NULL;

   if ((stat = pthread_attr_init(&jq->attr)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_attr_init: ERR=%s\n"), be.strerror(stat));
      return stat;
   }
   if ((stat = pthread_attr_setdetachstate(&jq->attr, PTHREAD_CREATE_DETACHED)) != 0) {
      pthread_attr_destroy(&jq->attr);
      return stat;
   }
   if ((stat = pthread_mutex_init(&jq->mutex, NULL)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_mutex_init: ERR=%s\n"), be.strerror(stat));
      pthread_attr_destroy(&jq->attr);
      return stat;
   }
   if ((stat = pthread_cond_init(&jq->work, NULL)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_cond_init: ERR=%s\n"), be.strerror(stat));
      pthread_mutex_destroy(&jq->mutex);
      pthread_attr_destroy(&jq->attr);
      return stat;
   }
   jq->quit = false;
   jq->max_workers = threads;         /* max threads to create */
   jq->num_workers = 0;               /* no threads yet */
   jq->idle_workers = 0;              /* no idle threads */
   jq->engine = engine;               /* routine to run */
   jq->valid = JOBQ_VALID;
   /* Initialize the job queues */
   jq->waiting_jobs = New(dlist(item, &item->link));
   jq->running_jobs = New(dlist(item, &item->link));
   jq->ready_jobs = New(dlist(item, &item->link));
   return 0;
}

/*
 * Destroy the job queue
 *
 * Returns: 0 on success
 *          errno on failure
 */
int jobq_destroy(jobq_t *jq)
{
   int stat, stat1, stat2;

   if (jq->valid != JOBQ_VALID) {
      return EINVAL;
   }
   if ((stat = pthread_mutex_lock(&jq->mutex)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_mutex_lock: ERR=%s\n"), be.strerror(stat));
      return stat;
   }
   jq->valid = 0;                      /* prevent any more operations */

   /* 
    * If any threads are active, wake them 
    */
   if (jq->num_workers > 0) {
      jq->quit = true;
      if (jq->idle_workers) {
         if ((stat = pthread_cond_broadcast(&jq->work)) != 0) {
            berrno be;
            Jmsg1(NULL, M_ERROR, 0, _("pthread_cond_broadcast: ERR=%s\n"), be.strerror(stat));
            pthread_mutex_unlock(&jq->mutex);
            return stat;
         }
      }
      while (jq->num_workers > 0) {
         if ((stat = pthread_cond_wait(&jq->work, &jq->mutex)) != 0) {
            berrno be;
            Jmsg1(NULL, M_ERROR, 0, _("pthread_cond_wait: ERR=%s\n"), be.strerror(stat));
            pthread_mutex_unlock(&jq->mutex);
            return stat;
         }
      }
   }
   if ((stat = pthread_mutex_unlock(&jq->mutex)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_mutex_unlock: ERR=%s\n"), be.strerror(stat));
      return stat;
   }
   stat  = pthread_mutex_destroy(&jq->mutex);
   stat1 = pthread_cond_destroy(&jq->work);
   stat2 = pthread_attr_destroy(&jq->attr);
   delete jq->waiting_jobs;
   delete jq->running_jobs;
   delete jq->ready_jobs;
   return (stat != 0 ? stat : (stat1 != 0 ? stat1 : stat2));
}

struct wait_pkt {
   JCR *jcr;
   jobq_t *jq;
};

/*
 * Wait until schedule time arrives before starting. Normally
 *  this routine is only used for jobs started from the console
 *  for which the user explicitly specified a start time. Otherwise
 *  most jobs are put into the job queue only when their
 *  scheduled time arives.
 */
extern "C"
void *sched_wait(void *arg)
{
   JCR *jcr = ((wait_pkt *)arg)->jcr;
   jobq_t *jq = ((wait_pkt *)arg)->jq;

   Dmsg0(2300, "Enter sched_wait.\n");
   free(arg);
   time_t wtime = jcr->sched_time - time(NULL);
   set_jcr_job_status(jcr, JS_WaitStartTime);
   /* Wait until scheduled time arrives */
   if (wtime > 0) {
      Jmsg(jcr, M_INFO, 0, _("Job %s waiting %d seconds for scheduled start time.\n"),
         jcr->Job, wtime);
   }
   /* Check every 30 seconds if canceled */
   while (wtime > 0) {
      Dmsg3(2300, "Waiting on sched time, jobid=%d secs=%d use=%d\n", 
         jcr->JobId, wtime, jcr->use_count());
      if (wtime > 30) {
         wtime = 30;
      }
      bmicrosleep(wtime, 0);
      if (job_canceled(jcr)) {
         break;
      }
      wtime = jcr->sched_time - time(NULL);
   }
   Dmsg1(200, "resched use=%d\n", jcr->use_count());
   jobq_add(jq, jcr);
   free_jcr(jcr);                     /* we are done with jcr */
   Dmsg0(2300, "Exit sched_wait\n");
   return NULL;
}

/*
 *  Add a job to the queue
 *    jq is a queue that was created with jobq_init
 */
int jobq_add(jobq_t *jq, JCR *jcr)
{
   int stat;
   jobq_item_t *item, *li;
   bool inserted = false;
   time_t wtime = jcr->sched_time - time(NULL);
   pthread_t id;
   wait_pkt *sched_pkt;

   if (!jcr->term_wait_inited) { 
      /* Initialize termination condition variable */
      if ((stat = pthread_cond_init(&jcr->term_wait, NULL)) != 0) {
         berrno be;
         Jmsg1(jcr, M_FATAL, 0, _("Unable to init job cond variable: ERR=%s\n"), be.strerror(stat));
         return stat;
      }
      jcr->term_wait_inited = true;
   }                           
                             
   Dmsg3(2300, "jobq_add jobid=%d jcr=0x%x use_count=%d\n", jcr->JobId, jcr, jcr->use_count());
   if (jq->valid != JOBQ_VALID) {
      Jmsg0(jcr, M_ERROR, 0, "Jobq_add queue not initialized.\n");
      return EINVAL;
   }

   jcr->inc_use_count();                 /* mark jcr in use by us */
   Dmsg3(2300, "jobq_add jobid=%d jcr=0x%x use_count=%d\n", jcr->JobId, jcr, jcr->use_count());
   if (!job_canceled(jcr) && wtime > 0) {
      set_thread_concurrency(jq->max_workers + 2);
      sched_pkt = (wait_pkt *)malloc(sizeof(wait_pkt));
      sched_pkt->jcr = jcr;
      sched_pkt->jq = jq;
      stat = pthread_create(&id, &jq->attr, sched_wait, (void *)sched_pkt);        
      if (stat != 0) {                /* thread not created */
         berrno be;
         Jmsg1(jcr, M_ERROR, 0, _("pthread_thread_create: ERR=%s\n"), be.strerror(stat));
      }
      return stat;
   }

   if ((stat = pthread_mutex_lock(&jq->mutex)) != 0) {
      berrno be;
      Jmsg1(jcr, M_ERROR, 0, _("pthread_mutex_lock: ERR=%s\n"), be.strerror(stat));
      free_jcr(jcr);                    /* release jcr */
      return stat;
   }

   if ((item = (jobq_item_t *)malloc(sizeof(jobq_item_t))) == NULL) {
      free_jcr(jcr);                    /* release jcr */
      return ENOMEM;
   }
   item->jcr = jcr;

   if (job_canceled(jcr)) {
      /* Add job to ready queue so that it is canceled quickly */
      jq->ready_jobs->prepend(item);
      Dmsg1(2300, "Prepended job=%d to ready queue\n", jcr->JobId);
   } else {
      /* Add this job to the wait queue in priority sorted order */
      foreach_dlist(li, jq->waiting_jobs) {
         Dmsg2(2300, "waiting item jobid=%d priority=%d\n",
            li->jcr->JobId, li->jcr->JobPriority);
         if (li->jcr->JobPriority > jcr->JobPriority) {
            jq->waiting_jobs->insert_before(item, li);
            Dmsg2(2300, "insert_before jobid=%d before waiting job=%d\n",
               li->jcr->JobId, jcr->JobId);
            inserted = true;
            break;
         }
      }
      /* If not jobs in wait queue, append it */
      if (!inserted) {
         jq->waiting_jobs->append(item);
         Dmsg1(2300, "Appended item jobid=%d to waiting queue\n", jcr->JobId);
      }
   }

   /* Ensure that at least one server looks at the queue. */
   stat = start_server(jq);

   pthread_mutex_unlock(&jq->mutex);
   Dmsg0(2300, "Return jobq_add\n");
   return stat;
}

/*
 *  Remove a job from the job queue. Used only by cancel_job().
 *    jq is a queue that was created with jobq_init
 *    work_item is an element of work
 *
 *   Note, it is "removed" from the job queue.
 *    If you want to cancel it, you need to provide some external means
 *    of doing so (e.g. pthread_kill()).
 */
int jobq_remove(jobq_t *jq, JCR *jcr)
{
   int stat;
   bool found = false;
   jobq_item_t *item;

   Dmsg2(2300, "jobq_remove jobid=%d jcr=0x%x\n", jcr->JobId, jcr);
   if (jq->valid != JOBQ_VALID) {
      return EINVAL;
   }

   if ((stat = pthread_mutex_lock(&jq->mutex)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_mutex_lock: ERR=%s\n"), be.strerror(stat));
      return stat;
   }

   foreach_dlist(item, jq->waiting_jobs) {
      if (jcr == item->jcr) {
         found = true;
         break;
      }
   }
   if (!found) {
      pthread_mutex_unlock(&jq->mutex);
      Dmsg2(2300, "jobq_remove jobid=%d jcr=0x%x not in wait queue\n", jcr->JobId, jcr);
      return EINVAL;
   }

   /* Move item to be the first on the list */
   jq->waiting_jobs->remove(item);
   jq->ready_jobs->prepend(item);
   Dmsg2(2300, "jobq_remove jobid=%d jcr=0x%x moved to ready queue\n", jcr->JobId, jcr);

   stat = start_server(jq);

   pthread_mutex_unlock(&jq->mutex);
   Dmsg0(2300, "Return jobq_remove\n");
   return stat;
}


/*
 * Start the server thread if it isn't already running
 */
static int start_server(jobq_t *jq)
{
   int stat = 0;
   pthread_t id;

   /*
    * if any threads are idle, wake one --                
    *   actually we do a broadcast because on /lib/tls 
    *   these signals seem to get lost from time to time.
    */
   if (jq->idle_workers > 0) {
      Dmsg0(2300, "Signal worker to wake up\n");
      if ((stat = pthread_cond_broadcast(&jq->work)) != 0) {
         berrno be;
         Jmsg1(NULL, M_ERROR, 0, _("pthread_cond_signal: ERR=%s\n"), be.strerror(stat));
         return stat;
      }
   } else if (jq->num_workers < jq->max_workers) {
      Dmsg0(2300, "Create worker thread\n");
      /* No idle threads so create a new one */
      set_thread_concurrency(jq->max_workers + 1);
      if ((stat = pthread_create(&id, &jq->attr, jobq_server, (void *)jq)) != 0) {
         berrno be;
         Jmsg1(NULL, M_ERROR, 0, _("pthread_create: ERR=%s\n"), be.strerror(stat));
         return stat;
      }
   }
   return stat;
}


/*
 * This is the worker thread that serves the job queue.
 * When all the resources are acquired for the job,
 *  it will call the user's engine.
 */
extern "C"
void *jobq_server(void *arg)
{
   struct timespec timeout;
   jobq_t *jq = (jobq_t *)arg;
   jobq_item_t *je;                   /* job entry in queue */
   int stat;
   bool timedout = false;
   bool work = true;

   Dmsg0(2300, "Start jobq_server\n");
   if ((stat = pthread_mutex_lock(&jq->mutex)) != 0) {
      berrno be;
      Jmsg1(NULL, M_ERROR, 0, _("pthread_mutex_lock: ERR=%s\n"), be.strerror(stat));
      return NULL;
   }
   jq->num_workers++;

   for (;;) {
      struct timeval tv;
      struct timezone tz;

      Dmsg0(2300, "Top of for loop\n");
      if (!work && !jq->quit) {
         gettimeofday(&tv, &tz);
         timeout.tv_nsec = 0;
         timeout.tv_sec = tv.tv_sec + 4;

         while (!jq->quit) {
            /*
             * Wait 4 seconds, then if no more work, exit
             */
            Dmsg0(2300, "pthread_cond_timedwait()\n");
            stat = pthread_cond_timedwait(&jq->work, &jq->mutex, &timeout);
            if (stat == ETIMEDOUT) {
               Dmsg0(2300, "timedwait timedout.\n");
               timedout = true;
               break;
            } else if (stat != 0) {
               /* This shouldn't happen */
               Dmsg0(2300, "This shouldn't happen\n");
               jq->num_workers--;
               pthread_mutex_unlock(&jq->mutex);
               return NULL;
            }
            break;
         }
      }
      /*
       * If anything is in the ready queue, run it
       */
      Dmsg0(2300, "Checking ready queue.\n");
      while (!jq->ready_jobs->empty() && !jq->quit) {
         JCR *jcr;
         je = (jobq_item_t *)jq->ready_jobs->first();
         jcr = je->jcr;
         jq->ready_jobs->remove(je);
         if (!jq->ready_jobs->empty()) {
            Dmsg0(2300, "ready queue not empty start server\n");
            if (start_server(jq) != 0) {
               jq->num_workers--;
               pthread_mutex_unlock(&jq->mutex);
               return NULL;
            }
         }
         jq->running_jobs->append(je);
         Dmsg1(2300, "Took jobid=%d from ready and appended to run\n", jcr->JobId);

         /* Release job queue lock */
         V(jq->mutex);

         /* Call user's routine here */
         Dmsg2(2300, "Calling user engine for jobid=%d use=%d\n", jcr->JobId,
            jcr->use_count());
         jq->engine(je->jcr);

         Dmsg2(2300, "Back from user engine jobid=%d use=%d.\n", jcr->JobId,
            jcr->use_count());

         /* Reacquire job queue lock */
         P(jq->mutex);
         Dmsg0(200, "Done lock mutex after running job. Release locks.\n");
         jq->running_jobs->remove(je);
         /*
          * Release locks if acquired. Note, they will not have
          *  been acquired for jobs canceled before they were
          *  put into the ready queue.
          */
         if (jcr->acquired_resource_locks) {
            if (jcr->rstore) {
               jcr->rstore->NumConcurrentJobs = 0;
               Dmsg1(200, "Dec rncj=%d\n", jcr->rstore->NumConcurrentJobs);
            }
            if (jcr->wstore) {
               jcr->wstore->NumConcurrentJobs--;
               Dmsg1(200, "Dec wncj=%d\n", jcr->wstore->NumConcurrentJobs);
            }
            jcr->client->NumConcurrentJobs--;
            jcr->job->NumConcurrentJobs--;
            jcr->acquired_resource_locks = false;
         }

         /*
          * Reschedule the job if necessary and requested
          */
         if (jcr->job->RescheduleOnError &&
             jcr->JobStatus != JS_Terminated &&
             jcr->JobStatus != JS_Canceled &&
             jcr->job->RescheduleTimes > 0 &&
             jcr->JobType == JT_BACKUP &&
             (jcr->job->RescheduleTimes == 0 ||
              jcr->reschedule_count < jcr->job->RescheduleTimes)) {
             char dt[50], dt2[50];

             /*
              * Reschedule this job by cleaning it up, but
              *  reuse the same JobId if possible.
              */
            time_t now = time(NULL);
            jcr->reschedule_count++;
            jcr->sched_time = now + jcr->job->RescheduleInterval;
            bstrftime(dt, sizeof(dt), now);
            bstrftime(dt2, sizeof(dt2), jcr->sched_time);
            Dmsg4(2300, "Rescheduled Job %s to re-run in %d seconds.(now=%u,then=%u)\n", jcr->Job,
                  (int)jcr->job->RescheduleInterval, now, jcr->sched_time);
            Jmsg(jcr, M_INFO, 0, _("Rescheduled Job %s at %s to re-run in %d seconds (%s).\n"),
                 jcr->Job, dt, (int)jcr->job->RescheduleInterval, dt2);
            dird_free_jcr_pointers(jcr);     /* partial cleanup old stuff */
            jcr->JobStatus = -1;
            set_jcr_job_status(jcr, JS_WaitStartTime);
            jcr->SDJobStatus = 0;
            if (jcr->JobBytes == 0) {
               Dmsg2(2300, "Requeue job=%d use=%d\n", jcr->JobId, jcr->use_count());
               V(jq->mutex);
               jobq_add(jq, jcr);     /* queue the job to run again */
               P(jq->mutex);
               free_jcr(jcr);         /* release jcr */
               free(je);              /* free the job entry */
               continue;              /* look for another job to run */
            }
            /*
             * Something was actually backed up, so we cannot reuse
             *   the old JobId or there will be database record
             *   conflicts.  We now create a new job, copying the
             *   appropriate fields.
             */           
            JCR *njcr = new_jcr(sizeof(JCR), dird_free_jcr);
            set_jcr_defaults(njcr, jcr->job);
            njcr->reschedule_count = jcr->reschedule_count;
            njcr->sched_time = jcr->sched_time;
            njcr->JobLevel = jcr->JobLevel;
            njcr->JobStatus = -1;
            set_jcr_job_status(njcr, jcr->JobStatus);
            if (jcr->rstore) {
               copy_rstorage(njcr, jcr->rstorage, _("previous Job"));
            } else {
               free_rstorage(njcr);
            }
            if (jcr->wstore) {
               copy_wstorage(njcr, jcr->wstorage, _("previous Job"));
            } else {
               free_wstorage(njcr);
            }
            njcr->messages = jcr->messages;
            Dmsg0(2300, "Call to run new job\n");
            V(jq->mutex);
            run_job(njcr);            /* This creates a "new" job */
            free_jcr(njcr);           /* release "new" jcr */
            P(jq->mutex);
            Dmsg0(2300, "Back from running new job.\n");
         }
         /* Clean up and release old jcr */
         if (jcr->db) {
            db_close_database(jcr, jcr->db);
            jcr->db = NULL;
         }
         Dmsg2(2300, "====== Termination job=%d use_cnt=%d\n", jcr->JobId, jcr->use_count());
         jcr->SDJobStatus = 0;
         V(jq->mutex);                /* release internal lock */
         free_jcr(jcr);
         free(je);                    /* release job entry */
         P(jq->mutex);                /* reacquire job queue lock */
      }
      /*
       * If any job in the wait queue can be run,
       *  move it to the ready queue
       */
      Dmsg0(2300, "Done check ready, now check wait queue.\n");
      if (!jq->waiting_jobs->empty() && !jq->quit) {
         int Priority;
         je = (jobq_item_t *)jq->waiting_jobs->first();
         jobq_item_t *re = (jobq_item_t *)jq->running_jobs->first();
         if (re) {
            Priority = re->jcr->JobPriority;
            Dmsg2(2300, "JobId %d is running. Look for pri=%d\n", re->jcr->JobId, Priority);
         } else {
            Priority = je->jcr->JobPriority;
            Dmsg1(2300, "No job running. Look for Job pri=%d\n", Priority);
         }
         /*
          * Walk down the list of waiting jobs and attempt
          *   to acquire the resources it needs.
          */
         for ( ; je;  ) {
            /* je is current job item on the queue, jn is the next one */
            JCR *jcr = je->jcr;
            jobq_item_t *jn = (jobq_item_t *)jq->waiting_jobs->next(je);

            Dmsg3(2300, "Examining Job=%d JobPri=%d want Pri=%d\n",
               jcr->JobId, jcr->JobPriority, Priority);

            /* Take only jobs of correct Priority */
            if (jcr->JobPriority != Priority) {
               set_jcr_job_status(jcr, JS_WaitPriority);
               break;
            }

            if (!acquire_resources(jcr)) {
               /* If resource conflict, job is canceled */
               if (!job_canceled(jcr)) {
                  je = jn;            /* point to next waiting job */
                  continue;
               }
            }

            /*
             * Got all locks, now remove it from wait queue and append it
             *   to the ready queue.  Note, we may also get here if the
             *    job was canceled.  Once it is "run", it will quickly
             *    terminate.
             */
            jq->waiting_jobs->remove(je);
            jq->ready_jobs->append(je);
            Dmsg1(2300, "moved JobId=%d from wait to ready queue\n", je->jcr->JobId);
            je = jn;                  /* Point to next waiting job */
         } /* end for loop */

      } /* end if */

      Dmsg0(2300, "Done checking wait queue.\n");
      /*
       * If no more ready work and we are asked to quit, then do it
       */
      if (jq->ready_jobs->empty() && jq->quit) {
         jq->num_workers--;
         if (jq->num_workers == 0) {
            Dmsg0(2300, "Wake up destroy routine\n");
            /* Wake up destroy routine if he is waiting */
            pthread_cond_broadcast(&jq->work);
         }
         break;
      }
      Dmsg0(2300, "Check for work request\n");
      /*
       * If no more work requests, and we waited long enough, quit
       */
      Dmsg2(2300, "timedout=%d read empty=%d\n", timedout,
         jq->ready_jobs->empty());
      if (jq->ready_jobs->empty() && timedout) {
         Dmsg0(2300, "break big loop\n");
         jq->num_workers--;
         break;
      }

      work = !jq->ready_jobs->empty() || !jq->waiting_jobs->empty();
      if (work) {
         /*
          * If a job is waiting on a Resource, don't consume all
          *   the CPU time looping looking for work, and even more
          *   important, release the lock so that a job that has
          *   terminated can give us the resource.
          */
         V(jq->mutex);
         bmicrosleep(2, 0);              /* pause for 2 seconds */
         P(jq->mutex);
         /* Recompute work as something may have changed in last 2 secs */
         work = !jq->ready_jobs->empty() || !jq->waiting_jobs->empty();
      }
      Dmsg1(2300, "Loop again. work=%d\n", work);
   } /* end of big for loop */

   Dmsg0(200, "unlock mutex\n");
   V(jq->mutex);
   Dmsg0(2300, "End jobq_server\n");
   return NULL;
}

/*
 * See if we can acquire all the necessary resources for the job (JCR)
 *
 *  Returns: true  if successful
 *           false if resource failure
 */
static bool acquire_resources(JCR *jcr)
{
   bool skip_this_jcr = false;

   jcr->acquired_resource_locks = false;
   if (jcr->rstore) {
      Dmsg1(200, "Rstore=%s\n", jcr->rstore->name());
      /* 
       * Let only one Restore/verify job run at a time regardless
       *   of MaxConcurrentJobs.
       */ 
      if (jcr->rstore->NumConcurrentJobs == 0) {
         jcr->rstore->NumConcurrentJobs = 1;
         Dmsg0(200, "Set rncj=1\n");
      } else {
         Dmsg1(200, "Fail rncj=%d\n", jcr->rstore->NumConcurrentJobs);
         set_jcr_job_status(jcr, JS_WaitStoreRes);
         return false;
      }
   }
   
   if (jcr->wstore) {
      if (jcr->rstore == jcr->wstore) {           /* deadlock */
         jcr->rstore->NumConcurrentJobs = 0;      /* back out rstore */
         Jmsg(jcr, M_FATAL, 0, _("Job canceled. Attempt to read and write same device.\n"));
         set_jcr_job_status(jcr, JS_Canceled);
         return false;
      }
      if (jcr->wstore->NumConcurrentJobs == 0 &&
          jcr->wstore->NumConcurrentJobs < jcr->wstore->MaxConcurrentJobs) {
         /* Simple case, first job */
         jcr->wstore->NumConcurrentJobs = 1;
         Dmsg0(200, "Set wncj=1\n");
      } else if (jcr->wstore->NumConcurrentJobs < jcr->wstore->MaxConcurrentJobs) {
         jcr->wstore->NumConcurrentJobs++;
         Dmsg1(200, "Inc wncj=%d\n", jcr->wstore->NumConcurrentJobs);
      } else if (jcr->rstore) {
         jcr->rstore->NumConcurrentJobs = 0;      /* back out rstore */
         Dmsg1(200, "Fail wncj=%d\n", jcr->wstore->NumConcurrentJobs);
         skip_this_jcr = true;
      } else {
         Dmsg1(200, "Fail wncj=%d\n", jcr->wstore->NumConcurrentJobs);
         skip_this_jcr = true;
      }
   }
   if (skip_this_jcr) {
      set_jcr_job_status(jcr, JS_WaitStoreRes);
      return false;
   }

   if (jcr->client->NumConcurrentJobs < jcr->client->MaxConcurrentJobs) {
      jcr->client->NumConcurrentJobs++;
   } else {
      /* Back out previous locks */
      if (jcr->wstore) {
         jcr->wstore->NumConcurrentJobs--;
         Dmsg1(200, "Dec wncj=%d\n", jcr->wstore->NumConcurrentJobs);
      }
      if (jcr->rstore) {
         jcr->rstore->NumConcurrentJobs = 0;
         Dmsg1(200, "Dec rncj=%d\n", jcr->rstore->NumConcurrentJobs);
      }
      set_jcr_job_status(jcr, JS_WaitClientRes);
      return false;
   }
   if (jcr->job->NumConcurrentJobs < jcr->job->MaxConcurrentJobs) {
      jcr->job->NumConcurrentJobs++;
   } else {
      /* Back out previous locks */
      if (jcr->wstore) {
         jcr->wstore->NumConcurrentJobs--;
         Dmsg1(200, "Dec wncj=%d\n", jcr->wstore->NumConcurrentJobs);
      }
      if (jcr->rstore) {
         jcr->rstore->NumConcurrentJobs = 0;
         Dmsg1(200, "Dec rncj=%d\n", jcr->rstore->NumConcurrentJobs);
      }
      jcr->client->NumConcurrentJobs--;
      set_jcr_job_status(jcr, JS_WaitJobRes);
      return false;
   }

   jcr->acquired_resource_locks = true;
   return true;
}
