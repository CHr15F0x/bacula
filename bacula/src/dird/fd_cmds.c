/*
 *
 *   Bacula Director -- fd_cmds.c -- send commands to File daemon
 *
 *     Kern Sibbald, October MM
 *
 *    This routine is run as a separate thread.  There may be more
 *    work to be done to make it totally reentrant!!!!
 * 
 *  Utility functions for sending info to File Daemon.
 *   These functions are used by both backup and verify.
 *   
 *   Version $Id$
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

/* Commands sent to File daemon */
static char inc[]         = "include\n";
static char exc[]         = "exclude\n";
static char fileset[]     = "fileset\n"; /* set full fileset */
static char jobcmd[]      = "JobId=%d Job=%s SDid=%u SDtime=%u Authorization=%s\n";
static char levelcmd[]    = "level = %s%s mtime_only=%d\n";
static char runbefore[]   = "RunBeforeJob %s\n";
static char runafter[]    = "RunAfterJob %s\n";


/* Responses received from File daemon */
static char OKinc[]       = "2000 OK include\n";
static char OKexc[]       = "2000 OK exclude\n";
static char OKjob[]       = "2000 OK Job";
static char OKbootstrap[] = "2000 OK bootstrap\n";
static char OKlevel[]     = "2000 OK level\n";
static char OKRunBefore[] = "2000 OK RunBefore\n";
static char OKRunAfter[]  = "2000 OK RunAfter\n";

/* Forward referenced functions */

/* External functions */
extern int debug_level;
extern DIRRES *director; 
extern int FDConnectTimeout;

#define INC_LIST 0
#define EXC_LIST 1

/*
 * Open connection with File daemon. 
 * Try connecting every retry_interval (default 10 sec), and
 *   give up after max_retry_time (default 30 mins).
 */

int connect_to_file_daemon(JCR *jcr, int retry_interval, int max_retry_time,
			   int verbose)
{
   BSOCK   *fd;

   fd = bnet_connect(jcr, retry_interval, max_retry_time,
        _("File daemon"), jcr->client->address, 
	NULL, jcr->client->FDport, verbose);
   if (fd == NULL) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }
   Dmsg0(10, "Opened connection with File daemon\n");
   fd->res = (RES *)jcr->client;      /* save resource in BSOCK */
   jcr->file_bsock = fd;
   set_jcr_job_status(jcr, JS_Running);

   if (!authenticate_file_daemon(jcr)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }
	
   /*
    * Now send JobId and authorization key
    */
   bnet_fsend(fd, jobcmd, jcr->JobId, jcr->Job, jcr->VolSessionId, 
      jcr->VolSessionTime, jcr->sd_auth_key);
   if (strcmp(jcr->sd_auth_key, "dummy") != 0) {
      memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));
   }
   Dmsg1(100, ">filed: %s", fd->msg);
   if (bget_dirmsg(fd) > 0) {
       Dmsg1(110, "<filed: %s", fd->msg);
       if (strncmp(fd->msg, OKjob, strlen(OKjob)) != 0) {
          Jmsg(jcr, M_FATAL, 0, _("File daemon \"%s\" rejected Job command: %s\n"), 
	     jcr->client->hdr.name, fd->msg);
	  set_jcr_job_status(jcr, JS_ErrorTerminated);
	  return 0;
       } else if (jcr->db) {
	  CLIENT_DBR cr;
	  memset(&cr, 0, sizeof(cr));
	  bstrncpy(cr.Name, jcr->client->hdr.name, sizeof(cr.Name));
	  cr.AutoPrune = jcr->client->AutoPrune;
	  cr.FileRetention = jcr->client->FileRetention;
	  cr.JobRetention = jcr->client->JobRetention;
	  bstrncpy(cr.Uname, fd->msg+strlen(OKjob)+1, sizeof(cr.Uname));
	  if (!db_update_client_record(jcr, jcr->db, &cr)) {
             Jmsg(jcr, M_WARNING, 0, _("Error updating Client record. ERR=%s\n"),
		db_strerror(jcr->db));
	  }
       }
   } else {
      Jmsg(jcr, M_FATAL, 0, _("FD gave bad response to JobId command: %s\n"),
	 bnet_strerror(fd));
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }
   return 1;
}

/*
 * This subroutine edits the last job start time into a
 *   "since=date/time" buffer that is returned in the  
 *   variable since.  This is used for display purposes in
 *   the job report.  The time in jcr->stime is later 
 *   passed to tell the File daemon what to do.
 */
void get_level_since_time(JCR *jcr, char *since, int since_len)
{
   /* Lookup the last
    * FULL backup job to get the time/date for a 
    * differential or incremental save.
    */
   if (!jcr->stime) {
      jcr->stime = get_pool_memory(PM_MESSAGE);
   }
   jcr->stime[0] = 0;
   since[0] = 0;
   switch (jcr->JobLevel) {
   case L_DIFFERENTIAL:
   case L_INCREMENTAL:
      /* Look up start time of last job */
      jcr->jr.JobId = 0;
      if (!db_find_job_start_time(jcr, jcr->db, &jcr->jr, &jcr->stime)) {
         Jmsg(jcr, M_INFO, 0, "%s", db_strerror(jcr->db));
         Jmsg(jcr, M_INFO, 0, _("No prior or suitable Full backup found. Doing FULL backup.\n"));
         bsnprintf(since, since_len, " (upgraded from %s)", 
	    level_to_str(jcr->JobLevel));
	 jcr->JobLevel = jcr->jr.Level = L_FULL;
      } else {
         bstrncpy(since, ", since=", since_len);
	 bstrncat(since, jcr->stime, since_len);
      }
      Dmsg1(100, "Last start time = %s\n", jcr->stime);
      break;
   }
}


/*
 * Send level command to FD. 
 * Used for backup jobs and estimate command.
 */
int send_level_command(JCR *jcr) 
{
   BSOCK   *fd = jcr->file_bsock;
   utime_t stime;
   char ed1[50];
   /* 
    * Send Level command to File daemon
    */
   switch (jcr->JobLevel) {
   case L_BASE:
      bnet_fsend(fd, levelcmd, "base", " ", 0);
      break;
   /* L_NONE is the console, sending something off to the FD */
   case L_NONE:
   case L_FULL:
      bnet_fsend(fd, levelcmd, "full", " ", 0);
      break;
   case L_DIFFERENTIAL:
   case L_INCREMENTAL:
//    bnet_fsend(fd, levelcmd, "since ", jcr->stime, 0); /* old code, deprecated */
      stime = str_to_utime(jcr->stime);
      bnet_fsend(fd, levelcmd, "since_utime ", edit_uint64(stime, ed1), 0);
      while (bget_dirmsg(fd) >= 0) {  /* allow him to poll us to sync clocks */
         Jmsg(jcr, M_INFO, 0, "%s\n", fd->msg);
      }
      break;
   case L_SINCE:
   default:
      Jmsg2(jcr, M_FATAL, 0, _("Unimplemented backup level %d %c\n"), 
	 jcr->JobLevel, jcr->JobLevel);
      return 0;
   }
   Dmsg1(120, ">filed: %s", fd->msg);
   if (!response(jcr, fd, OKlevel, "Level", DISPLAY_ERROR)) {
      return 0;
   }
   return 1;
}


/*
 * Send either an Included or an Excluded list to FD
 */
static int send_list(JCR *jcr, int list)
{
   FILESET *fileset;
   BSOCK   *fd;
   int num;

   fd = jcr->file_bsock;
   fileset = jcr->fileset;

   if (list == INC_LIST) {
      num = fileset->num_includes;
   } else {
      num = fileset->num_excludes;
   }

   for (int i=0; i<num; i++) {
      BPIPE *bpipe;
      FILE *ffd;
      char buf[2000];
      char *p;
      int optlen, stat;
      INCEXE *ie;


      if (list == INC_LIST) {
	 ie = fileset->include_items[i];
      } else {
	 ie = fileset->exclude_items[i];
      }
      for (int j=0; j<ie->name_list.size(); j++) {
	 p = (char *)ie->name_list.get(j);
	 switch (*p) {
         case '|':
	    p++;		      /* skip over the | */
            fd->msg = edit_job_codes(jcr, fd->msg, p, "");
            bpipe = open_bpipe(fd->msg, 0, "r");
	    if (!bpipe) {
               Jmsg(jcr, M_FATAL, 0, _("Cannot run program: %s. ERR=%s\n"),
		  p, strerror(errno));
	       goto bail_out;
	    }
	    /* Copy File options */
	    if (ie->num_opts) {
	       bstrncpy(buf, ie->opts_list[0]->opts, sizeof(buf));
               bstrncat(buf, " ", sizeof(buf));
	    } else {
               bstrncpy(buf, "0 ", sizeof(buf));
	    }
            Dmsg1(100, "Opts=%s\n", buf);
	    optlen = strlen(buf);
	    while (fgets(buf+optlen, sizeof(buf)-optlen, bpipe->rfd)) {
               fd->msglen = Mmsg(&fd->msg, "%s", buf);
               Dmsg2(200, "Inc/exc len=%d: %s", fd->msglen, fd->msg);
	       if (!bnet_send(fd)) {
                  Jmsg(jcr, M_FATAL, 0, _(">filed: write error on socket\n"));
		  goto bail_out;
	       }
	    }
	    if ((stat=close_bpipe(bpipe)) != 0) {
               Jmsg(jcr, M_FATAL, 0, _("Error running program: %s. RtnStat=%d ERR=%s\n"),
		  p, stat, strerror(errno));
	       goto bail_out;
	    }
	    break;
         case '<':
	    p++;		      /* skip over < */
            if ((ffd = fopen(p, "r")) == NULL) {
               Jmsg(jcr, M_FATAL, 0, _("Cannot open %s file: %s. ERR=%s\n"),
                  list==INC_LIST?"included":"excluded", p, strerror(errno));
	       goto bail_out;
	    }
	    /* Copy File options */
	    if (ie->num_opts) {
	       bstrncpy(buf, ie->opts_list[0]->opts, sizeof(buf));
               bstrncat(buf, " ", sizeof(buf));
	    } else {
               bstrncpy(buf, "0 ", sizeof(buf));
	    }
            Dmsg1(100, "Opts=%s\n", buf);
	    optlen = strlen(buf);
	    while (fgets(buf+optlen, sizeof(buf)-optlen, ffd)) {
               fd->msglen = Mmsg(&fd->msg, "%s", buf);
	       if (!bnet_send(fd)) {
                  Jmsg(jcr, M_FATAL, 0, _(">filed: write error on socket\n"));
		  goto bail_out;
	       }
	    }
	    fclose(ffd);
	    break;
         case '\\':
            p++;                      /* skip over \ */
	    /* Note, fall through wanted */
	 default:
	    if (ie->num_opts) {
               Dmsg2(100, "numopts=%d opts=%s\n", ie->num_opts, NPRT(ie->opts_list[0]->opts));
	       pm_strcpy(&fd->msg, ie->opts_list[0]->opts);
               pm_strcat(&fd->msg, " ");
	    } else {
               pm_strcpy(&fd->msg, "0 ");
	    }
	    fd->msglen = pm_strcat(&fd->msg, p);
            Dmsg1(100, "Inc/Exc name=%s\n", fd->msg);
	    if (!bnet_send(fd)) {
               Jmsg(jcr, M_FATAL, 0, _(">filed: write error on socket\n"));
	       goto bail_out;
	    }
	    break;
	 }
      }
   }
   bnet_sig(fd, BNET_EOD);	      /* end of data */
   if (list == INC_LIST) {
      if (!response(jcr, fd, OKinc, "Include", DISPLAY_ERROR)) {
	 goto bail_out;
      }
   } else if (!response(jcr, fd, OKexc, "Exclude", DISPLAY_ERROR)) {
	goto bail_out;
   }
   return 1;

bail_out:
   set_jcr_job_status(jcr, JS_ErrorTerminated);
   return 0;

}


/*
 * Send either an Included or an Excluded list to FD
 */
static int send_fileset(JCR *jcr)
{
   FILESET *fileset = jcr->fileset;
   BSOCK   *fd = jcr->file_bsock;
   int num = fileset->num_includes;

   for (int i=0; i<num; i++) {
      BPIPE *bpipe;
      FILE *ffd;
      char buf[2000];
      char *p;
      int optlen, stat;
      INCEXE *ie;
      int j, k;

      ie = fileset->include_items[i];
      bnet_fsend(fd, "I\n");

      for (j=0; j<ie->num_opts; j++) {
	 FOPTS *fo = ie->opts_list[j];
         bnet_fsend(fd, "O %s\n", fo->opts);
	 for (k=0; k<fo->regex.size(); k++) {
            bnet_fsend(fd, "R %s\n", fo->regex.get(k));
	 }
	 for (k=0; k<fo->wild.size(); k++) {
            bnet_fsend(fd, "W %s\n", fo->wild.get(k));
	 }
	 for (k=0; k<fo->base.size(); k++) {
            bnet_fsend(fd, "B %s\n", fo->base.get(k));
	 }
         bnet_fsend(fd, "N\n");
      }

      for (j=0; j<ie->name_list.size(); j++) {
	 p = (char *)ie->name_list.get(j);
	 switch (*p) {
         case '|':
	    p++;		      /* skip over the | */
            fd->msg = edit_job_codes(jcr, fd->msg, p, "");
            bpipe = open_bpipe(fd->msg, 0, "r");
	    if (!bpipe) {
               Jmsg(jcr, M_FATAL, 0, _("Cannot run program: %s. ERR=%s\n"),
		  p, strerror(errno));
	       goto bail_out;
	    }
            bstrncpy(buf, "F ", sizeof(buf));
            Dmsg1(100, "Opts=%s\n", buf);
	    optlen = strlen(buf);
	    while (fgets(buf+optlen, sizeof(buf)-optlen, bpipe->rfd)) {
               fd->msglen = Mmsg(&fd->msg, "%s", buf);
               Dmsg2(200, "Inc/exc len=%d: %s", fd->msglen, fd->msg);
	       if (!bnet_send(fd)) {
                  Jmsg(jcr, M_FATAL, 0, _(">filed: write error on socket\n"));
		  goto bail_out;
	       }
	    }
	    if ((stat=close_bpipe(bpipe)) != 0) {
               Jmsg(jcr, M_FATAL, 0, _("Error running program: %s. RtnStat=%d ERR=%s\n"),
		  p, stat, strerror(errno));
	       goto bail_out;
	    }
	    break;
         case '<':
	    p++;		      /* skip over < */
            if ((ffd = fopen(p, "r")) == NULL) {
               Jmsg(jcr, M_FATAL, 0, _("Cannot open included file: %s. ERR=%s\n"),
		  p, strerror(errno));
	       goto bail_out;
	    }
            bstrncpy(buf, "F ", sizeof(buf));
            Dmsg1(100, "Opts=%s\n", buf);
	    optlen = strlen(buf);
	    while (fgets(buf+optlen, sizeof(buf)-optlen, ffd)) {
               fd->msglen = Mmsg(&fd->msg, "%s", buf);
	       if (!bnet_send(fd)) {
                  Jmsg(jcr, M_FATAL, 0, _(">filed: write error on socket\n"));
		  goto bail_out;
	       }
	    }
	    fclose(ffd);
	    break;
         case '\\':
            p++;                      /* skip over \ */
	    /* Note, fall through wanted */
	 default:
            pm_strcpy(&fd->msg, "F ");
	    fd->msglen = pm_strcat(&fd->msg, p);
            Dmsg1(100, "Inc/Exc name=%s\n", fd->msg);
	    if (!bnet_send(fd)) {
               Jmsg(jcr, M_FATAL, 0, _(">filed: write error on socket\n"));
	       goto bail_out;
	    }
	    break;
	 }
      }
      bnet_fsend(fd, "N\n");
   }

   bnet_sig(fd, BNET_EOD);	      /* end of data */
   if (!response(jcr, fd, OKinc, "Include", DISPLAY_ERROR)) {
      goto bail_out;
   }
   return 1;

bail_out:
   set_jcr_job_status(jcr, JS_ErrorTerminated);
   return 0;

}


/*
 * Send include list to File daemon
 */
int send_include_list(JCR *jcr)
{
   BSOCK *fd = jcr->file_bsock;
   if (jcr->fileset->new_include) {
      bnet_fsend(fd, fileset);
      return send_fileset(jcr);
   } else {
      bnet_fsend(fd, inc);
   }
   return send_list(jcr, INC_LIST);
}


/*
 * Send exclude list to File daemon 
 */
int send_exclude_list(JCR *jcr)
{
   BSOCK *fd = jcr->file_bsock;
   bnet_fsend(fd, exc);
   return send_list(jcr, EXC_LIST);
}


/*
 * Send bootstrap file if any to the File daemon.
 *  This is used for restore and verify VolumeToCatalog
 */
int send_bootstrap_file(JCR *jcr)
{
   FILE *bs;
   char buf[1000];
   BSOCK *fd = jcr->file_bsock;
   char *bootstrap = "bootstrap\n";

   Dmsg1(400, "send_bootstrap_file: %s\n", jcr->RestoreBootstrap);
   if (!jcr->RestoreBootstrap) {
      return 1;
   }
   bs = fopen(jcr->RestoreBootstrap, "r");
   if (!bs) {
      Jmsg(jcr, M_FATAL, 0, _("Could not open bootstrap file %s: ERR=%s\n"), 
	 jcr->RestoreBootstrap, strerror(errno));
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }
   bnet_fsend(fd, bootstrap);
   while (fgets(buf, sizeof(buf), bs)) {
      bnet_fsend(fd, "%s", buf);       
   }
   bnet_sig(fd, BNET_EOD);
   fclose(bs);
   if (!response(jcr, fd, OKbootstrap, "Bootstrap", DISPLAY_ERROR)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }
   return 1;
}

/*
 * Send ClientRunBeforeJob and ClientRunAfterJob to File daemon
 */
int send_run_before_and_after_commands(JCR *jcr)
{
   POOLMEM *msg = get_pool_memory(PM_FNAME);
   BSOCK *fd = jcr->file_bsock;
   if (jcr->job->ClientRunBeforeJob) {
      pm_strcpy(&msg, jcr->job->ClientRunBeforeJob);
      bash_spaces(msg);
      bnet_fsend(fd, runbefore, msg);
      if (!response(jcr, fd, OKRunBefore, "ClientRunBeforeJob", DISPLAY_ERROR)) {
	 set_jcr_job_status(jcr, JS_ErrorTerminated);
	 free_pool_memory(msg);
	 return 0;
      }
   }
   if (jcr->job->ClientRunAfterJob) {
      fd->msglen = pm_strcpy(&msg, jcr->job->ClientRunAfterJob);
      bash_spaces(msg);
      bnet_fsend(fd, runafter, msg);
      if (!response(jcr, fd, OKRunAfter, "ClientRunAfterJob", DISPLAY_ERROR)) {
	 set_jcr_job_status(jcr, JS_ErrorTerminated);
	 free_pool_memory(msg);
	 return 0;
      }
   }
   free_pool_memory(msg);
   return 1;
}


/* 
 * Read the attributes from the File daemon for
 * a Verify job and store them in the catalog.
 */
int get_attributes_and_put_in_catalog(JCR *jcr)
{
   BSOCK   *fd;
   int n = 0;
   ATTR_DBR ar;

   fd = jcr->file_bsock;
   jcr->jr.FirstIndex = 1;
   memset(&ar, 0, sizeof(ar));
   jcr->FileIndex = 0;

   Dmsg0(120, "bdird: waiting to receive file attributes\n");
   /* Pickup file attributes and signature */
   while (!fd->errors && (n = bget_dirmsg(fd)) > 0) {

   /*****FIXME****** improve error handling to stop only on 
    * really fatal problems, or the number of errors is too
    * large.
    */
      long file_index;
      int stream, len;
      char *attr, *p, *fn;
      char Opts_SIG[MAXSTRING];      /* either Verify opts or MD5/SHA1 signature */
      char SIG[MAXSTRING];

      jcr->fname = check_pool_memory_size(jcr->fname, fd->msglen);
      if ((len = sscanf(fd->msg, "%ld %d %s", &file_index, &stream, Opts_SIG)) != 3) {
         Jmsg(jcr, M_FATAL, 0, _("<filed: bad attributes, expected 3 fields got %d\n\
msglen=%d msg=%s\n"), len, fd->msglen, fd->msg);
	 set_jcr_job_status(jcr, JS_ErrorTerminated);
	 return 0;
      }
      p = fd->msg;
      skip_nonspaces(&p);	      /* skip FileIndex */
      skip_spaces(&p);
      skip_nonspaces(&p);	      /* skip Stream */
      skip_spaces(&p);
      skip_nonspaces(&p);	      /* skip Opts_SHA1 */   
      p++;			      /* skip space */
      fn = jcr->fname;
      while (*p != 0) {
	 *fn++ = *p++;		      /* copy filename */
      }
      *fn = *p++;		      /* term filename and point to attribs */
      attr = p;

      if (stream == STREAM_UNIX_ATTRIBUTES || stream == STREAM_UNIX_ATTRIBUTES_EX) {
	 jcr->JobFiles++;
	 jcr->FileIndex = file_index;
	 ar.attr = attr;
	 ar.fname = jcr->fname;
	 ar.FileIndex = file_index;
	 ar.Stream = stream;
	 ar.link = NULL;
	 ar.JobId = jcr->JobId;
	 ar.ClientId = jcr->ClientId;
	 ar.PathId = 0;
	 ar.FilenameId = 0;

         Dmsg2(111, "dird<filed: stream=%d %s\n", stream, jcr->fname);
         Dmsg1(120, "dird<filed: attr=%s\n", attr);

	 if (!db_create_file_attributes_record(jcr, jcr->db, &ar)) {
            Jmsg1(jcr, M_ERROR, 0, "%s", db_strerror(jcr->db));
	    set_jcr_job_status(jcr, JS_Error);
	    continue;
	 }
	 jcr->FileId = ar.FileId;
      } else if (stream == STREAM_MD5_SIGNATURE || stream == STREAM_SHA1_SIGNATURE) {
	 if (jcr->FileIndex != (uint32_t)file_index) {
            Jmsg2(jcr, M_ERROR, 0, _("MD5/SHA1 index %d not same as attributes %d\n"),
	       file_index, jcr->FileIndex);
	    set_jcr_job_status(jcr, JS_Error);
	    continue;
	 }
	 db_escape_string(SIG, Opts_SIG, strlen(Opts_SIG));
         Dmsg2(120, "SIGlen=%d SIG=%s\n", strlen(SIG), SIG);
	 if (!db_add_SIG_to_file_record(jcr, jcr->db, jcr->FileId, SIG, 
		   stream==STREAM_MD5_SIGNATURE?MD5_SIG:SHA1_SIG)) {
            Jmsg1(jcr, M_ERROR, 0, "%s", db_strerror(jcr->db));
	    set_jcr_job_status(jcr, JS_Error);
	 }
      }
      jcr->jr.JobFiles = jcr->JobFiles = file_index;
      jcr->jr.LastIndex = file_index;
   } 
   if (is_bnet_error(fd)) {
      Jmsg1(jcr, M_FATAL, 0, _("<filed: Network error getting attributes. ERR=%s\n"),
			bnet_strerror(fd));
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }

   set_jcr_job_status(jcr, JS_Terminated);
   return 1;
}
