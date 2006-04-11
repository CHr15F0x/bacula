/*
 *  Bacula File Daemon Job processing
 *
 *    Kern Sibbald, October MM
 *
 *   Version $Id$
 *
 */
/*
   Copyright (C) 2000-2006 Kern Sibbald

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
#include "filed.h"
#ifdef WIN32_VSS
#include "vss.h"   
static pthread_mutex_t vss_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

extern char my_name[];
extern CLIENT *me;                    /* our client resource */

int enable_vss = 0;                   /* set to use vss */

/* Imported functions */
extern int status_cmd(JCR *jcr);
extern int qstatus_cmd(JCR *jcr);

/* Forward referenced functions */
static int backup_cmd(JCR *jcr);
static int bootstrap_cmd(JCR *jcr);
static int cancel_cmd(JCR *jcr);
static int setdebug_cmd(JCR *jcr);
static int estimate_cmd(JCR *jcr);
static int hello_cmd(JCR *jcr);
static int job_cmd(JCR *jcr);
static int fileset_cmd(JCR *jcr);
static int level_cmd(JCR *jcr);
static int verify_cmd(JCR *jcr);
static int restore_cmd(JCR *jcr);
static int storage_cmd(JCR *jcr);
static int session_cmd(JCR *jcr);
static int response(JCR *jcr, BSOCK *sd, char *resp, const char *cmd);
static void filed_free_jcr(JCR *jcr);
static int open_sd_read_session(JCR *jcr);
static int send_bootstrap_file(JCR *jcr);
static int runbefore_cmd(JCR *jcr);
static int runafter_cmd(JCR *jcr);
static bool run_cmd(JCR *jcr, char *cmd, const char *name);
static void set_options(findFOPTS *fo, const char *opts);


/* Exported functions */

struct s_cmds {
   const char *cmd;
   int (*func)(JCR *);
   int monitoraccess; /* specify if monitors have access to this function */
};

/*
 * The following are the recognized commands from the Director.
 */
static struct s_cmds cmds[] = {
   {"backup",       backup_cmd,    0},
   {"cancel",       cancel_cmd,    0},
   {"setdebug=",    setdebug_cmd,  0},
   {"estimate",     estimate_cmd,  0},
   {"Hello",        hello_cmd,     1},
   {"fileset",      fileset_cmd,   0},
   {"JobId=",       job_cmd,       0},
   {"level = ",     level_cmd,     0},
   {"restore",      restore_cmd,   0},
   {"session",      session_cmd,   0},
   {"status",       status_cmd,    1},
   {".status",      qstatus_cmd,   1},
   {"storage ",     storage_cmd,   0},
   {"verify",       verify_cmd,    0},
   {"bootstrap",    bootstrap_cmd, 0},
   {"RunBeforeJob", runbefore_cmd, 0},
   {"RunAfterJob",  runafter_cmd,  0},
   {NULL,       NULL}                  /* list terminator */
};

/* Commands received from director that need scanning */
static char jobcmd[]      = "JobId=%d Job=%127s SDid=%d SDtime=%d Authorization=%100s";
static char storaddr[]    = "storage address=%s port=%d ssl=%d\n";
static char sessioncmd[]  = "session %127s %ld %ld %ld %ld %ld %ld\n";
static char restorecmd[]  = "restore replace=%c prelinks=%d where=%s\n";
static char restorecmd1[] = "restore replace=%c prelinks=%d where=\n";
static char verifycmd[]   = "verify level=%30s\n";
static char estimatecmd[] = "estimate listing=%d\n";
static char runbefore[]   = "RunBeforeJob %s\n";
static char runafter[]    = "RunAfterJob %s\n";

/* Responses sent to Director */
static char errmsg[]      = "2999 Invalid command\n";
static char no_auth[]     = "2998 No Authorization\n";
static char illegal_cmd[] = "2997 Illegal command for a Director with Monitor directive enabled\n";
static char OKinc[]       = "2000 OK include\n";
static char OKest[]       = "2000 OK estimate files=%u bytes=%s\n";
static char OKlevel[]     = "2000 OK level\n";
static char OKbackup[]    = "2000 OK backup\n";
static char OKbootstrap[] = "2000 OK bootstrap\n";
static char OKverify[]    = "2000 OK verify\n";
static char OKrestore[]   = "2000 OK restore\n";
static char OKsession[]   = "2000 OK session\n";
static char OKstore[]     = "2000 OK storage\n";
static char OKjob[]       = "2000 OK Job %s,%s,%s";
static char OKsetdebug[]  = "2000 OK setdebug=%d\n";
static char BADjob[]      = "2901 Bad Job\n";
static char EndJob[]      = "2800 End Job TermCode=%d JobFiles=%u ReadBytes=%s JobBytes=%s Errors=%u\n";
static char OKRunBefore[] = "2000 OK RunBefore\n";
static char OKRunAfter[]  = "2000 OK RunAfter\n";

/* Responses received from Storage Daemon */
static char OK_end[]       = "3000 OK end\n";
static char OK_close[]     = "3000 OK close Status = %d\n";
static char OK_open[]      = "3000 OK open ticket = %d\n";
static char OK_data[]      = "3000 OK data\n";
static char OK_append[]    = "3000 OK append data\n";
static char OKSDbootstrap[] = "3000 OK bootstrap\n";


/* Commands sent to Storage Daemon */
static char append_open[]  = "append open session\n";
static char append_data[]  = "append data %d\n";
static char append_end[]   = "append end session %d\n";
static char append_close[] = "append close session %d\n";
static char read_open[]    = "read open session = %s %ld %ld %ld %ld %ld %ld\n";
static char read_data[]    = "read data %d\n";
static char read_close[]   = "read close session %d\n";

/*
 * Accept requests from a Director
 *
 * NOTE! We are running as a separate thread
 *
 * Send output one line
 * at a time followed by a zero length transmission.
 *
 * Return when the connection is terminated or there
 * is an error.
 *
 * Basic task here is:
 *   Authenticate Director (during Hello command).
 *   Accept commands one at a time from the Director
 *     and execute them.
 *
 */
void *handle_client_request(void *dirp)
{
   int i;
   bool found, quit;
   JCR *jcr;
   BSOCK *dir = (BSOCK *)dirp;

   jcr = new_jcr(sizeof(JCR), filed_free_jcr); /* create JCR */
   jcr->dir_bsock = dir;
   jcr->ff = init_find_files();
   jcr->start_time = time(NULL);
   jcr->last_fname = get_pool_memory(PM_FNAME);
   jcr->last_fname[0] = 0;
   jcr->client_name = get_memory(strlen(my_name) + 1);
   pm_strcpy(jcr->client_name, my_name);
   jcr->pki_sign = me->pki_sign;
   jcr->pki_encrypt = me->pki_encrypt;
   jcr->pki_keypair = me->pki_keypair;
   jcr->pki_signers = me->pki_signers;
   jcr->pki_recipients = me->pki_recipients;
   dir->jcr = jcr;
   enable_backup_privileges(NULL, 1 /* ignore_errors */);

   /**********FIXME******* add command handler error code */

   for (quit=false; !quit;) {

      /* Read command */
      if (bnet_recv(dir) < 0) {
         break;               /* connection terminated */
      }
      dir->msg[dir->msglen] = 0;
      Dmsg1(100, "<dird: %s", dir->msg);
      found = false;
      for (i=0; cmds[i].cmd; i++) {
         if (strncmp(cmds[i].cmd, dir->msg, strlen(cmds[i].cmd)) == 0) {
            found = true;         /* indicate command found */
            if (!jcr->authenticated && cmds[i].func != hello_cmd) {
               bnet_fsend(dir, no_auth);
               bnet_sig(dir, BNET_EOD);
               break;
            }
            if ((jcr->authenticated) && (!cmds[i].monitoraccess) && (jcr->director->monitor)) {
               Dmsg1(100, "Command %s illegal.\n", cmds[i].cmd);
               bnet_fsend(dir, illegal_cmd);
               bnet_sig(dir, BNET_EOD);
               break;
            }
            Dmsg1(100, "Executing %s command.\n", cmds[i].cmd);
            if (!cmds[i].func(jcr)) {         /* do command */
               quit = true;         /* error or fully terminated, get out */
               Dmsg1(20, "Quit command loop. Canceled=%d\n", job_canceled(jcr));
            }
            break;
         }
      }
      if (!found) {              /* command not found */
         bnet_fsend(dir, errmsg);
         quit = true;
         break;
      }
   }

   /* Inform Storage daemon that we are done */
   if (jcr->store_bsock) {
      bnet_sig(jcr->store_bsock, BNET_TERMINATE);
   }

   if (jcr->RunAfterJob && !job_canceled(jcr)) {
      run_cmd(jcr, jcr->RunAfterJob, "ClientRunAfterJob");
   }
   generate_daemon_event(jcr, "JobEnd");

   dequeue_messages(jcr);             /* send any queued messages */

   /* Inform Director that we are done */
   bnet_sig(dir, BNET_TERMINATE);

   /* Clean up fileset */
   FF_PKT *ff = jcr->ff;
   findFILESET *fileset = ff->fileset;
   if (fileset) {
      int i, j, k;
      /* Delete FileSet Include lists */
      for (i=0; i<fileset->include_list.size(); i++) {
         findINCEXE *incexe = (findINCEXE *)fileset->include_list.get(i);
         for (j=0; j<incexe->opts_list.size(); j++) {
            findFOPTS *fo = (findFOPTS *)incexe->opts_list.get(j);
            for (k=0; k<fo->regex.size(); k++) {
               regfree((regex_t *)fo->regex.get(k));
            }
            fo->regex.destroy();
            fo->regexdir.destroy();
            fo->regexfile.destroy();
            fo->wild.destroy();
            fo->wilddir.destroy();
            fo->wildfile.destroy();
            fo->base.destroy();
            fo->fstype.destroy();
            if (fo->reader) {
               free(fo->reader);
            }
            if (fo->writer) {
               free(fo->writer);
            }
         }
         incexe->opts_list.destroy();
         incexe->name_list.destroy();
      }
      fileset->include_list.destroy();

      /* Delete FileSet Exclude lists */
      for (i=0; i<fileset->exclude_list.size(); i++) {
         findINCEXE *incexe = (findINCEXE *)fileset->exclude_list.get(i);
         for (j=0; j<incexe->opts_list.size(); j++) {
            findFOPTS *fo = (findFOPTS *)incexe->opts_list.get(j);
            fo->regex.destroy();
            fo->regexdir.destroy();
            fo->regexfile.destroy();
            fo->wild.destroy();
            fo->wilddir.destroy();
            fo->wildfile.destroy();
            fo->base.destroy();
            fo->fstype.destroy();
         }
         incexe->opts_list.destroy();
         incexe->name_list.destroy();
      }
      fileset->exclude_list.destroy();
      free(fileset);
   }
   ff->fileset = NULL;
   Dmsg0(100, "Calling term_find_files\n");
   term_find_files(jcr->ff);
   jcr->ff = NULL;
   Dmsg0(100, "Done with term_find_files\n");
   free_jcr(jcr);                     /* destroy JCR record */
   Dmsg0(100, "Done with free_jcr\n");
   return NULL;
}

/*
 * Hello from Director he must identify himself and provide his
 *  password.
 */
static int hello_cmd(JCR *jcr)
{
   Dmsg0(120, "Calling Authenticate\n");
   if (!authenticate_director(jcr)) {
      return 0;
   }
   Dmsg0(120, "OK Authenticate\n");
   jcr->authenticated = true;
   return 1;
}

/*
 * Cancel a Job
 */
static int cancel_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   char Job[MAX_NAME_LENGTH];
   JCR *cjcr;

   if (sscanf(dir->msg, "cancel Job=%127s", Job) == 1) {
      if (!(cjcr=get_jcr_by_full_name(Job))) {
         bnet_fsend(dir, _("2901 Job %s not found.\n"), Job);
      } else {
         if (cjcr->store_bsock) {
            cjcr->store_bsock->timed_out = 1;
            cjcr->store_bsock->terminated = 1;
#if !defined(HAVE_CYGWIN)
            pthread_kill(cjcr->my_thread_id, TIMEOUT_SIGNAL);
#endif
         }
         set_jcr_job_status(cjcr, JS_Canceled);
         free_jcr(cjcr);
         bnet_fsend(dir, _("2001 Job %s marked to be canceled.\n"), Job);
      }
   } else {
      bnet_fsend(dir, _("2902 Error scanning cancel command.\n"));
   }
   bnet_sig(dir, BNET_EOD);
   return 1;
}


/*
 * Set debug level as requested by the Director
 *
 */
static int setdebug_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   int level, trace_flag;

   Dmsg1(110, "setdebug_cmd: %s", dir->msg);
   if (sscanf(dir->msg, "setdebug=%d trace=%d", &level, &trace_flag) != 2 || level < 0) {
      pm_strcpy(jcr->errmsg, dir->msg);
      bnet_fsend(dir, _("2991 Bad setdebug command: %s\n"), jcr->errmsg);
      return 0;
   }
   debug_level = level;
   set_trace(trace_flag);
   return bnet_fsend(dir, OKsetdebug, level);
}


static int estimate_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   char ed2[50];

   if (sscanf(dir->msg, estimatecmd, &jcr->listing) != 1) {
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg(jcr, M_FATAL, 0, _("Bad estimate command: %s"), jcr->errmsg);
      bnet_fsend(dir, _("2992 Bad estimate command.\n"));
      return 0;
   }
   make_estimate(jcr);
   bnet_fsend(dir, OKest, jcr->num_files_examined,
      edit_uint64_with_commas(jcr->JobBytes, ed2));
   bnet_sig(dir, BNET_EOD);
   return 1;
}

/*
 * Get JobId and Storage Daemon Authorization key from Director
 */
static int job_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   POOLMEM *sd_auth_key;
   
   sd_auth_key = get_memory(dir->msglen);
   if (sscanf(dir->msg, jobcmd,  &jcr->JobId, jcr->Job,
              &jcr->VolSessionId, &jcr->VolSessionTime,
              sd_auth_key) != 5) {
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg(jcr, M_FATAL, 0, _("Bad Job Command: %s"), jcr->errmsg);
      bnet_fsend(dir, BADjob);
      free_pool_memory(sd_auth_key);
      return 0;
   }
   jcr->sd_auth_key = bstrdup(sd_auth_key);
   free_pool_memory(sd_auth_key);
   Dmsg2(120, "JobId=%d Auth=%s\n", jcr->JobId, jcr->sd_auth_key);
   return bnet_fsend(dir, OKjob, HOST_OS, DISTNAME, DISTVER);
}

static int runbefore_cmd(JCR *jcr)
{
   bool ok;
   BSOCK *dir = jcr->dir_bsock;
   POOLMEM *cmd = get_memory(dir->msglen+1);

   Dmsg1(100, "runbefore_cmd: %s", dir->msg);
   if (sscanf(dir->msg, runbefore, cmd) != 1) {
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg1(jcr, M_FATAL, 0, _("Bad RunBeforeJob command: %s\n"), jcr->errmsg);
      bnet_fsend(dir, _("2905 Bad RunBeforeJob command.\n"));
      free_memory(cmd);
      return 0;
   }
   unbash_spaces(cmd);

   /* Run the command now */
   ok = run_cmd(jcr, cmd, "ClientRunBeforeJob");
   free_memory(cmd);
   if (ok) {
      bnet_fsend(dir, OKRunBefore);
      return 1;
   } else {
      bnet_fsend(dir, _("2905 Bad RunBeforeJob command.\n"));
      return 0;
   }
}

static int runafter_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   POOLMEM *msg = get_memory(dir->msglen+1);

   Dmsg1(100, "runafter_cmd: %s", dir->msg);
   if (sscanf(dir->msg, runafter, msg) != 1) {
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg1(jcr, M_FATAL, 0, _("Bad RunAfter command: %s\n"), jcr->errmsg);
      bnet_fsend(dir, _("2905 Bad RunAfterJob command.\n"));
      free_memory(msg);
      return 0;
   }
   unbash_spaces(msg);
   if (jcr->RunAfterJob) {
      free_pool_memory(jcr->RunAfterJob);
   }
   jcr->RunAfterJob = get_pool_memory(PM_FNAME);
   pm_strcpy(jcr->RunAfterJob, msg);
   free_pool_memory(msg);
   return bnet_fsend(dir, OKRunAfter);
}

static bool run_cmd(JCR *jcr, char *cmd, const char *name)
{
   POOLMEM *ecmd = get_pool_memory(PM_FNAME);
   int status;
   BPIPE *bpipe;
   char line[MAXSTRING];

   ecmd = edit_job_codes(jcr, ecmd, cmd, "");
   bpipe = open_bpipe(ecmd, 0, "r");
   free_pool_memory(ecmd);
   if (bpipe == NULL) {
      berrno be;
      Jmsg(jcr, M_FATAL, 0, _("%s could not execute. ERR=%s\n"), name,
         be.strerror());
      return false;
   }
   while (fgets(line, sizeof(line), bpipe->rfd)) {
      int len = strlen(line);
      if (len > 0 && line[len-1] == '\n') {
         line[len-1] = 0;
      }
      Jmsg(jcr, M_INFO, 0, _("%s: %s\n"), name, line);
   }
   status = close_bpipe(bpipe);
   if (status != 0) {
      berrno be;
      Jmsg(jcr, M_FATAL, 0, _("%s returned non-zero status=%d. ERR=%s\n"), name,
         status, be.strerror(status));
      return false;
   }
   return true;
}

static bool init_fileset(JCR *jcr)
{
   FF_PKT *ff;
   findFILESET *fileset;

   if (!jcr->ff) {
      return false;
   }
   ff = jcr->ff;
   if (ff->fileset) {
      return false;
   }
   fileset = (findFILESET *)malloc(sizeof(findFILESET));
   memset(fileset, 0, sizeof(findFILESET));
   ff->fileset = fileset;
   fileset->state = state_none;
   fileset->include_list.init(1, true);
   fileset->exclude_list.init(1, true);
   return true;
}

static findFOPTS *start_options(FF_PKT *ff)
{
   int state = ff->fileset->state;
   findINCEXE *incexe = ff->fileset->incexe;

   if (state != state_options) {
      ff->fileset->state = state_options;
      findFOPTS *fo = (findFOPTS *)malloc(sizeof(findFOPTS));
      memset(fo, 0, sizeof(findFOPTS));
      fo->regex.init(1, true);
      fo->regexdir.init(1, true);
      fo->regexfile.init(1, true);
      fo->wild.init(1, true);
      fo->wilddir.init(1, true);
      fo->wildfile.init(1, true);
      fo->base.init(1, true);
      fo->fstype.init(1, true);
      incexe->current_opts = fo;
      incexe->opts_list.append(fo);
   }
   return incexe->current_opts;

}

/*
 * Add fname to include/exclude fileset list. First check for
 * | and < and if necessary perform command.
 */
static void add_file_to_fileset(JCR *jcr, const char *fname, findFILESET *fileset)
{
   char *p;
   BPIPE *bpipe;
   POOLMEM *fn;
   FILE *ffd;
   char buf[1000];
   int ch;
   int stat;

   p = (char *)fname;
   ch = (uint8_t)*p;
   switch (ch) {
   case '|':
      p++;                            /* skip over | */
      fn = get_pool_memory(PM_FNAME);
      fn = edit_job_codes(jcr, fn, p, "");
      bpipe = open_bpipe(fn, 0, "r");
      free_pool_memory(fn);
      if (!bpipe) {
         Jmsg(jcr, M_FATAL, 0, _("Cannot run program: %s. ERR=%s\n"),
            p, strerror(errno));
         return;
      }
      while (fgets(buf, sizeof(buf), bpipe->rfd)) {
         strip_trailing_junk(buf);
         fileset->incexe->name_list.append(bstrdup(buf));
      }
      if ((stat=close_bpipe(bpipe)) != 0) {
         Jmsg(jcr, M_FATAL, 0, _("Error running program: %s. RtnStat=%d ERR=%s\n"),
            p, stat, strerror(errno));
         return;
      }
      break;
   case '<':
      Dmsg0(100, "Doing < include on client.\n");
      p++;                      /* skip over < */
      if ((ffd = fopen(p, "r")) == NULL) {
         berrno be;
         Jmsg(jcr, M_FATAL, 0, _("Cannot open FileSet input file: %s. ERR=%s\n"),
            p, be.strerror());
         return;
      }
      while (fgets(buf, sizeof(buf), ffd)) {
         strip_trailing_junk(buf);
         Dmsg1(100, "%s\n", buf);
         fileset->incexe->name_list.append(bstrdup(buf));
      }
      fclose(ffd);
      break;
   default:
      fileset->incexe->name_list.append(bstrdup(fname));
      break;
   }
}


static void add_fileset(JCR *jcr, const char *item)
{
   FF_PKT *ff = jcr->ff;
   findFILESET *fileset = ff->fileset;
   int state = fileset->state;
   findFOPTS *current_opts;

   /* Get code, optional subcode, and position item past the dividing space */
   Dmsg1(100, "%s\n", item);
   int code = item[0];
   if (code != '\0') {
      ++item;
   }
   int subcode = ' ';               /* A space is always a valid subcode */
   if (item[0] != '\0' && item[0] != ' ') {
      subcode = item[0];
      ++item;
   }
   if (*item == ' ') {
      ++item;
   }

   /* Skip all lines we receive after an error */
   if (state == state_error) {
      return;
   }

   /*
    * The switch tests the code for validity.
    * The subcode is always good if it is a space, otherwise we must confirm.
    * We set state to state_error first assuming the subcode is invalid,
    * requiring state to be set in cases below that handle subcodes.
    */
   if (subcode != ' ') {
      state = state_error;
   }
   switch (code) {
   case 'I':
      /* New include */
      fileset->incexe = (findINCEXE *)malloc(sizeof(findINCEXE));
      memset(fileset->incexe, 0, sizeof(findINCEXE));
      fileset->incexe->opts_list.init(1, true);
      fileset->incexe->name_list.init(1, true);
      fileset->include_list.append(fileset->incexe);
      break;
   case 'E':
      /* New exclude */
      fileset->incexe = (findINCEXE *)malloc(sizeof(findINCEXE));
      memset(fileset->incexe, 0, sizeof(findINCEXE));
      fileset->incexe->opts_list.init(1, true);
      fileset->incexe->name_list.init(1, true);
      fileset->exclude_list.append(fileset->incexe);
      break;
   case 'N':
      state = state_none;
      break;
   case 'F':
      /* File item to either include/include list */
      state = state_include;
      add_file_to_fileset(jcr, item, fileset);
      break;
   case 'R':
      current_opts = start_options(ff);
      regex_t *preg;
      int rc;
      char prbuf[500];
      preg = (regex_t *)malloc(sizeof(regex_t));
      if (current_opts->flags & FO_IGNORECASE) {
         rc = regcomp(preg, item, REG_EXTENDED|REG_ICASE);
      } else {
         rc = regcomp(preg, item, REG_EXTENDED);
      }
      if (rc != 0) {
         regerror(rc, preg, prbuf, sizeof(prbuf));
         regfree(preg);
         free(preg);
         Jmsg(jcr, M_FATAL, 0, _("REGEX %s compile error. ERR=%s\n"), item, prbuf);
         state = state_error;
         break;
      }
      state = state_options;
      if (subcode == ' ') {
         current_opts->regex.append(preg);
      } else if (subcode == 'D') {
         current_opts->regexdir.append(preg);
      } else if (subcode == 'F') {
         current_opts->regexfile.append(preg);
      } else {
         state = state_error;
      }
      break;
   case 'B':
      current_opts = start_options(ff);
      current_opts->base.append(bstrdup(item));
      state = state_options;
      break;
   case 'X':
      current_opts = start_options(ff);
      current_opts->fstype.append(bstrdup(item));
      state = state_options;
      break;
   case 'W':
      current_opts = start_options(ff);
      state = state_options;
      if (subcode == ' ') {
         current_opts->wild.append(bstrdup(item));
      } else if (subcode == 'D') {
         current_opts->wilddir.append(bstrdup(item));
      } else if (subcode == 'F') {
         current_opts->wildfile.append(bstrdup(item));
      } else {
         state = state_error;
      }
      break;
   case 'O':
      current_opts = start_options(ff);
      set_options(current_opts, item);
      state = state_options;
      break;
   case 'D':
      current_opts = start_options(ff);
      current_opts->reader = bstrdup(item);
      state = state_options;
      break;
   case 'T':
      current_opts = start_options(ff);
      current_opts->writer = bstrdup(item);
      state = state_options;
      break;
   default:
      Jmsg(jcr, M_FATAL, 0, _("Invalid FileSet command: %s\n"), item);
      state = state_error;
      break;
   }
   ff->fileset->state = state;
}

static bool term_fileset(JCR *jcr)
{
   FF_PKT *ff = jcr->ff;

#ifdef xxx
   findFILESET *fileset = ff->fileset;
   int i, j, k;

   for (i=0; i<fileset->include_list.size(); i++) {
      findINCEXE *incexe = (findINCEXE *)fileset->include_list.get(i);
      Dmsg0(400, "I\n");
      for (j=0; j<incexe->opts_list.size(); j++) {
         findFOPTS *fo = (findFOPTS *)incexe->opts_list.get(j);
         for (k=0; k<fo->regex.size(); k++) {
            Dmsg1(400, "R %s\n", (char *)fo->regex.get(k));
         }
         for (k=0; k<fo->regexdir.size(); k++) {
            Dmsg1(400, "RD %s\n", (char *)fo->regexdir.get(k));
         }
         for (k=0; k<fo->regexfile.size(); k++) {
            Dmsg1(400, "RF %s\n", (char *)fo->regexfile.get(k));
         }
         for (k=0; k<fo->wild.size(); k++) {
            Dmsg1(400, "W %s\n", (char *)fo->wild.get(k));
         }
         for (k=0; k<fo->wilddir.size(); k++) {
            Dmsg1(400, "WD %s\n", (char *)fo->wilddir.get(k));
         }
         for (k=0; k<fo->wildfile.size(); k++) {
            Dmsg1(400, "WF %s\n", (char *)fo->wildfile.get(k));
         }
         for (k=0; k<fo->base.size(); k++) {
            Dmsg1(400, "B %s\n", (char *)fo->base.get(k));
         }
         for (k=0; k<fo->fstype.size(); k++) {
            Dmsg1(400, "X %s\n", (char *)fo->fstype.get(k));
         }
         if (fo->reader) {
            Dmsg1(400, "D %s\n", fo->reader);
         }
         if (fo->writer) {
            Dmsg1(400, "T %s\n", fo->writer);
         }
      }
      for (j=0; j<incexe->name_list.size(); j++) {
         Dmsg1(400, "F %s\n", (char *)incexe->name_list.get(j));
      }
   }
   for (i=0; i<fileset->exclude_list.size(); i++) {
      findINCEXE *incexe = (findINCEXE *)fileset->exclude_list.get(i);
      Dmsg0(400, "E\n");
      for (j=0; j<incexe->opts_list.size(); j++) {
         findFOPTS *fo = (findFOPTS *)incexe->opts_list.get(j);
         for (k=0; k<fo->regex.size(); k++) {
            Dmsg1(400, "R %s\n", (char *)fo->regex.get(k));
         }
         for (k=0; k<fo->regexdir.size(); k++) {
            Dmsg1(400, "RD %s\n", (char *)fo->regexdir.get(k));
         }
         for (k=0; k<fo->regexfile.size(); k++) {
            Dmsg1(400, "RF %s\n", (char *)fo->regexfile.get(k));
         }
         for (k=0; k<fo->wild.size(); k++) {
            Dmsg1(400, "W %s\n", (char *)fo->wild.get(k));
         }
         for (k=0; k<fo->wilddir.size(); k++) {
            Dmsg1(400, "WD %s\n", (char *)fo->wilddir.get(k));
         }
         for (k=0; k<fo->wildfile.size(); k++) {
            Dmsg1(400, "WF %s\n", (char *)fo->wildfile.get(k));
         }
         for (k=0; k<fo->base.size(); k++) {
            Dmsg1(400, "B %s\n", (char *)fo->base.get(k));
         }
         for (k=0; k<fo->fstype.size(); k++) {
            Dmsg1(400, "X %s\n", (char *)fo->fstype.get(k));
         }
      }
      for (j=0; j<incexe->name_list.size(); j++) {
         Dmsg1(400, "F %s\n", (char *)incexe->name_list.get(j));
      }
   }
#endif
   return ff->fileset->state != state_error;
}


/*
 * As an optimization, we should do this during
 *  "compile" time in filed/job.c, and keep only a bit mask
 *  and the Verify options.
 */
static void set_options(findFOPTS *fo, const char *opts)
{
   int j;
   const char *p;

   for (p=opts; *p; p++) {
      switch (*p) {
      case 'a':                 /* alway replace */
      case '0':                 /* no option */
         break;
      case 'e':
         fo->flags |= FO_EXCLUDE;
         break;
      case 'f':
         fo->flags |= FO_MULTIFS;
         break;
      case 'h':                 /* no recursion */
         fo->flags |= FO_NO_RECURSION;
         break;
      case 'H':                 /* no hard link handling */
         fo->flags |= FO_NO_HARDLINK;
         break;
      case 'i':
         fo->flags |= FO_IGNORECASE;
         break;
      case 'M':                 /* MD5 */
         fo->flags |= FO_MD5;
         break;
      case 'n':
         fo->flags |= FO_NOREPLACE;
         break;
      case 'p':                 /* use portable data format */
         fo->flags |= FO_PORTABLE;
         break;
      case 'R':                 /* Resource forks and Finder Info */
         fo->flags |= FO_HFSPLUS;
      case 'r':                 /* read fifo */
         fo->flags |= FO_READFIFO;
         break;
      case 'S':
         switch(*(p + 1)) {
         case ' ':
            /* Old director did not specify SHA variant */
            fo->flags |= FO_SHA1;
            break;
         case '1':
            fo->flags |= FO_SHA1;
            p++;
            break;
#ifdef HAVE_SHA2
         case '2':
            fo->flags |= FO_SHA256;
            p++;
            break;
         case '3':
            fo->flags |= FO_SHA512;
            p++;
            break;
#endif
         default:
            /* Automatically downgrade to SHA-1 if an unsupported
             * SHA variant is specified */
            fo->flags |= FO_SHA1;
            p++;
            break;
         }
         break;
      case 's':
         fo->flags |= FO_SPARSE;
         break;
      case 'm':
         fo->flags |= FO_MTIMEONLY;
         break;
      case 'k':
         fo->flags |= FO_KEEPATIME;
         break;
      case 'A':
         fo->flags |= FO_ACL;
         break;
      case 'V':                  /* verify options */
         /* Copy Verify Options */
         for (j=0; *p && *p != ':'; p++) {
            fo->VerifyOpts[j] = *p;
            if (j < (int)sizeof(fo->VerifyOpts) - 1) {
               j++;
            }
         }
         fo->VerifyOpts[j] = 0;
         break;
      case 'w':
         fo->flags |= FO_IF_NEWER;
         break;
      case 'Z':                 /* gzip compression */
         fo->flags |= FO_GZIP;
         fo->GZIP_level = *++p - '0';
         Dmsg1(200, "Compression level=%d\n", fo->GZIP_level);
         break;
      default:
         Emsg1(M_ERROR, 0, _("Unknown include/exclude option: %c\n"), *p);
         break;
      }
   }
}


/*
 * Director is passing his Fileset
 */
static int fileset_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   int vss = 0;

   sscanf(dir->msg, "fileset vss=%d", &vss);
   enable_vss = vss;

   if (!init_fileset(jcr)) {
      return 0;
   }
   while (bnet_recv(dir) >= 0) {
      strip_trailing_junk(dir->msg);
      Dmsg1(500, "Fileset: %s\n", dir->msg);
      add_fileset(jcr, dir->msg);
   }
   if (!term_fileset(jcr)) {
      return 0;
   }
   return bnet_fsend(dir, OKinc);
}

static void free_bootstrap(JCR *jcr)
{
   if (jcr->RestoreBootstrap) {
      unlink(jcr->RestoreBootstrap);
      free_pool_memory(jcr->RestoreBootstrap);
      jcr->RestoreBootstrap = NULL;
   }
}


/* 
 * The Director sends us the bootstrap file, which
 *   we will in turn pass to the SD.
 */
static int bootstrap_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   POOLMEM *fname = get_pool_memory(PM_FNAME);
   FILE *bs;

   free_bootstrap(jcr);
   Mmsg(fname, "%s/%s.%s.bootstrap", me->working_directory, me->hdr.name,
      jcr->Job);
   Dmsg1(400, "bootstrap=%s\n", fname);
   jcr->RestoreBootstrap = fname;
   bs = fopen(fname, "a+");           /* create file */
   if (!bs) {
      berrno be;
      Jmsg(jcr, M_FATAL, 0, _("Could not create bootstrap file %s: ERR=%s\n"),
         jcr->RestoreBootstrap, be.strerror());
      /*
       * Suck up what he is sending to us so that he will then
       *   read our error message.
       */
      while (bnet_recv(dir) >= 0)
        {  }
      free_bootstrap(jcr);
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return 0;
   }

   while (bnet_recv(dir) >= 0) {
       Dmsg1(200, "filed<dird: bootstrap file %s\n", dir->msg);
       fputs(dir->msg, bs);
   }
   fclose(bs);
   /*
    * Note, do not free the bootstrap yet -- it needs to be 
    *  sent to the SD 
    */
   return bnet_fsend(dir, OKbootstrap);
}


/*
 * Get backup level from Director
 *
 */
static int level_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   POOLMEM *level, *buf = NULL;
   int mtime_only;

   level = get_memory(dir->msglen+1);
   Dmsg1(110, "level_cmd: %s", dir->msg);
   if (sscanf(dir->msg, "level = %s ", level) != 1) {
      goto bail_out;
   }
   /* Base backup requested? */
   if (strcmp(level, "base") == 0) {
      jcr->JobLevel = L_BASE;
   /* Full backup requested? */
   } else if (strcmp(level, "full") == 0) {
      jcr->JobLevel = L_FULL;
   } else if (strcmp(level, "differential") == 0) {
      jcr->JobLevel = L_DIFFERENTIAL;
      free_memory(level);
      return 1;
   } else if (strcmp(level, "incremental") == 0) {
      jcr->JobLevel = L_INCREMENTAL;
      free_memory(level);
      return 1;   
   /*
    * We get his UTC since time, then sync the clocks and correct it
    *   to agree with our clock.
    */
   } else if (strcmp(level, "since_utime") == 0) {
      buf = get_memory(dir->msglen+1);
      utime_t since_time, adj;
      btime_t his_time, bt_start, rt=0, bt_adj=0;
      if (jcr->JobLevel == L_NONE) {
         jcr->JobLevel = L_SINCE;     /* if no other job level set, do it now */
      }
      if (sscanf(dir->msg, "level = since_utime %s mtime_only=%d",
                 buf, &mtime_only) != 2) {
         goto bail_out;
      }
      since_time = str_to_uint64(buf);  /* this is the since time */
      char ed1[50], ed2[50];
      /*
       * Sync clocks by polling him for the time. We take
       *   10 samples of his time throwing out the first two.
       */
      for (int i=0; i<10; i++) {
         bt_start = get_current_btime();
         bnet_sig(dir, BNET_BTIME);   /* poll for time */
         if (bnet_recv(dir) <= 0) {   /* get response */
            goto bail_out;
         }
         if (sscanf(dir->msg, "btime %s", buf) != 1) {
            goto bail_out;
         }
         if (i < 2) {                 /* toss first two results */
            continue;
         }
         his_time = str_to_uint64(buf);
         rt = get_current_btime() - bt_start; /* compute round trip time */
         bt_adj -= his_time - bt_start - rt/2;
         Dmsg2(200, "rt=%s adj=%s\n", edit_uint64(rt, ed1), edit_uint64(bt_adj, ed2));
      }

      bt_adj = bt_adj / 8;            /* compute average time */
      Dmsg2(100, "rt=%s adj=%s\n", edit_uint64(rt, ed1), edit_uint64(bt_adj, ed2));
      adj = btime_to_utime(bt_adj);
      since_time += adj;              /* adjust for clock difference */
      if (adj != 0) {
         Jmsg(jcr, M_INFO, 0, _("DIR and FD clocks differ by %d seconds, FD automatically adjusting.\n"), adj);
      }
      bnet_sig(dir, BNET_EOD);

      Dmsg2(100, "adj = %d since_time=%d\n", (int)adj, (int)since_time);
      jcr->incremental = 1;           /* set incremental or decremental backup */
      jcr->mtime = (time_t)since_time; /* set since time */
   } else {
      Jmsg1(jcr, M_FATAL, 0, _("Unknown backup level: %s\n"), level);
      free_memory(level);
      return 0;
   }
   free_memory(level);
   if (buf) {
      free_memory(buf);
   }
   return bnet_fsend(dir, OKlevel);

bail_out:
   pm_strcpy(jcr->errmsg, dir->msg);
   Jmsg1(jcr, M_FATAL, 0, _("Bad level command: %s\n"), jcr->errmsg);
   free_memory(level);
   if (buf) {
      free_memory(buf);
   }
   return 0;
}

/*
 * Get session parameters from Director -- this is for a Restore command
 */
static int session_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;

   Dmsg1(100, "SessionCmd: %s", dir->msg);
   if (sscanf(dir->msg, sessioncmd, jcr->VolumeName,
              &jcr->VolSessionId, &jcr->VolSessionTime,
              &jcr->StartFile, &jcr->EndFile,
              &jcr->StartBlock, &jcr->EndBlock) != 7) {
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg(jcr, M_FATAL, 0, _("Bad session command: %s"), jcr->errmsg);
      return 0;
   }

   return bnet_fsend(dir, OKsession);
}

/*
 * Get address of storage daemon from Director
 *
 */
static int storage_cmd(JCR *jcr)
{
   int stored_port;                /* storage daemon port */
   int enable_ssl;                 /* enable ssl to sd */
   BSOCK *dir = jcr->dir_bsock;
   BSOCK *sd;                         /* storage daemon bsock */

   Dmsg1(100, "StorageCmd: %s", dir->msg);
   if (sscanf(dir->msg, storaddr, &jcr->stored_addr, &stored_port, &enable_ssl) != 3) {
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg(jcr, M_FATAL, 0, _("Bad storage command: %s"), jcr->errmsg);
      return 0;
   }
   Dmsg3(110, "Open storage: %s:%d ssl=%d\n", jcr->stored_addr, stored_port, enable_ssl);
   /* Open command communications with Storage daemon */
   /* Try to connect for 1 hour at 10 second intervals */
   sd = bnet_connect(jcr, 10, (int)me->SDConnectTimeout, _("Storage daemon"),
                     jcr->stored_addr, NULL, stored_port, 1);
   if (sd == NULL) {
      Jmsg(jcr, M_FATAL, 0, _("Failed to connect to Storage daemon: %s:%d\n"),
          jcr->stored_addr, stored_port);
      Dmsg2(100, "Failed to connect to Storage daemon: %s:%d\n",
          jcr->stored_addr, stored_port);
      return 0;
   }
   Dmsg0(110, "Connection OK to SD.\n");

   jcr->store_bsock = sd;

   bnet_fsend(sd, "Hello Start Job %s\n", jcr->Job);
   if (!authenticate_storagedaemon(jcr)) {
      Jmsg(jcr, M_FATAL, 0, _("Failed to authenticate Storage daemon.\n"));
      return 0;
   }
   Dmsg0(110, "Authenticated with SD.\n");

   /* Send OK to Director */
   return bnet_fsend(dir, OKstore);
}


/*
 * Do a backup. For now, we handle only Full and Incremental.
 */
static int backup_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   BSOCK *sd = jcr->store_bsock;
   int ok = 0;
   int SDJobStatus;
   char ed1[50], ed2[50];

   set_jcr_job_status(jcr, JS_Blocked);
   jcr->JobType = JT_BACKUP;
   Dmsg1(100, "begin backup ff=%p\n", jcr->ff);

   if (sd == NULL) {
      Jmsg(jcr, M_FATAL, 0, _("Cannot contact Storage daemon\n"));
      goto cleanup;
   }

   bnet_fsend(dir, OKbackup);
   Dmsg1(110, "bfiled>dird: %s", dir->msg);

   /*
    * Send Append Open Session to Storage daemon
    */
   bnet_fsend(sd, append_open);
   Dmsg1(110, ">stored: %s", sd->msg);
   /*
    * Expect to receive back the Ticket number
    */
   if (bget_msg(sd) >= 0) {
      Dmsg1(110, "<stored: %s", sd->msg);
      if (sscanf(sd->msg, OK_open, &jcr->Ticket) != 1) {
         Jmsg(jcr, M_FATAL, 0, _("Bad response to append open: %s\n"), sd->msg);
         goto cleanup;
      }
      Dmsg1(110, "Got Ticket=%d\n", jcr->Ticket);
   } else {
      Jmsg(jcr, M_FATAL, 0, _("Bad response from stored to open command\n"));
      goto cleanup;
   }

   /*
    * Send Append data command to Storage daemon
    */
   bnet_fsend(sd, append_data, jcr->Ticket);
   Dmsg1(110, ">stored: %s", sd->msg);

   /*
    * Expect to get OK data
    */
   Dmsg1(110, "<stored: %s", sd->msg);
   if (!response(jcr, sd, OK_data, "Append Data")) {
      goto cleanup;
   }
   
   generate_daemon_event(jcr, "JobStart");

#ifdef WIN32_VSS
   /* START VSS ON WIN 32 */
   if (g_pVSSClient && enable_vss) {
      /* Run only one at a time */
      P(vss_mutex);
      if (g_pVSSClient->InitializeForBackup()) {
         /* tell vss which drives to snapshot */   
         char szWinDriveLetters[27];   
         if (get_win32_driveletters(jcr->ff, szWinDriveLetters)) {
            Jmsg(jcr, M_INFO, 0, _("Generate VSS snapshots. Driver=\"%s\", Drive(s)=\"%s\"\n"), g_pVSSClient->GetDriverName(), szWinDriveLetters);
            if (!g_pVSSClient->CreateSnapshots(szWinDriveLetters)) {
               berrno be;
               Jmsg(jcr, M_WARNING, 0, _("Generate VSS snapshots failed. ERR=%s\n"),
                  be.strerror());
            } else {
               /* tell user if snapshot creation of a specific drive failed */
               size_t i;
               for (i=0; i<strlen (szWinDriveLetters); i++) {
                  if (islower(szWinDriveLetters[i])) {
                     Jmsg(jcr, M_WARNING, 0, _("Generate VSS snapshot of drive \"%c:\\\" failed\n"), szWinDriveLetters[i]);
                  }
               }
               /* inform user about writer states */
               for (i=0; i<g_pVSSClient->GetWriterCount(); i++) {
                  int msg_type = M_INFO;
                  if (g_pVSSClient->GetWriterState(i) < 0) {
                     msg_type = M_WARNING;
                  }
                  Jmsg(jcr, msg_type, 0, _("VSS Writer: %s\n"), g_pVSSClient->GetWriterInfo(i));
               }
            }
         } else {
            Jmsg(jcr, M_INFO, 0, _("No drive letters found for generating VSS snapshots.\n"));
         }
      } else {
         Jmsg(jcr, M_WARNING, 0, _("VSS was not initialized properly. VSS support is disabled.\n"));
      }
   }
#endif

   /*
    * Send Files to Storage daemon
    */
   Dmsg1(110, "begin blast ff=%p\n", (FF_PKT *)jcr->ff);
   if (!blast_data_to_storage_daemon(jcr, NULL)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      bnet_suppress_error_messages(sd, 1);
      bget_msg(sd);                   /* Read final response from append_data */
      Dmsg0(110, "Error in blast_data.\n");
   } else {
      set_jcr_job_status(jcr, JS_Terminated);
      if (jcr->JobStatus != JS_Terminated) {
         bnet_suppress_error_messages(sd, 1);
         goto cleanup;                /* bail out now */
      }
      /*
       * Expect to get response to append_data from Storage daemon
       */
      if (!response(jcr, sd, OK_append, "Append Data")) {
         set_jcr_job_status(jcr, JS_ErrorTerminated);
         goto cleanup;
      }

      /*
       * Send Append End Data to Storage daemon
       */
      bnet_fsend(sd, append_end, jcr->Ticket);
      /* Get end OK */
      if (!response(jcr, sd, OK_end, "Append End")) {
         set_jcr_job_status(jcr, JS_ErrorTerminated);
         goto cleanup;
      }

      /*
       * Send Append Close to Storage daemon
       */
      bnet_fsend(sd, append_close, jcr->Ticket);
      while (bget_msg(sd) >= 0) {    /* stop on signal or error */
         if (sscanf(sd->msg, OK_close, &SDJobStatus) == 1) {
            ok = 1;
            Dmsg2(200, "SDJobStatus = %d %c\n", SDJobStatus, (char)SDJobStatus);
         }
      }
      if (!ok) {
         Jmsg(jcr, M_FATAL, 0, _("Append Close with SD failed.\n"));
         goto cleanup;
      }
      if (SDJobStatus != JS_Terminated) {
         Jmsg(jcr, M_FATAL, 0, _("Bad status %d returned from Storage Daemon.\n"),
            SDJobStatus);
      }
   }

cleanup:
#ifdef WIN32_VSS
   /* STOP VSS ON WIN 32 */
   /* tell vss to close the backup session */
   if (g_pVSSClient && enable_vss) {
      g_pVSSClient->CloseBackup();
      V(vss_mutex);
   }
#endif

   bnet_fsend(dir, EndJob, jcr->JobStatus, jcr->JobFiles,
      edit_uint64(jcr->ReadBytes, ed1),
      edit_uint64(jcr->JobBytes, ed2), jcr->Errors);
   Dmsg1(110, "End FD msg: %s\n", dir->msg);
   
   return 0;                          /* return and stop command loop */
}

/*
 * Do a Verify for Director
 *
 */
static int verify_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   BSOCK *sd  = jcr->store_bsock;
   char level[100], ed1[50], ed2[50];

   jcr->JobType = JT_VERIFY;
   if (sscanf(dir->msg, verifycmd, level) != 1) {
      bnet_fsend(dir, _("2994 Bad verify command: %s\n"), dir->msg);
      return 0;
   }

   if (strcasecmp(level, "init") == 0) {
      jcr->JobLevel = L_VERIFY_INIT;
   } else if (strcasecmp(level, "catalog") == 0){
      jcr->JobLevel = L_VERIFY_CATALOG;
   } else if (strcasecmp(level, "volume") == 0){
      jcr->JobLevel = L_VERIFY_VOLUME_TO_CATALOG;
   } else if (strcasecmp(level, "data") == 0){
      jcr->JobLevel = L_VERIFY_DATA;
   } else if (strcasecmp(level, "disk_to_catalog") == 0) {
      jcr->JobLevel = L_VERIFY_DISK_TO_CATALOG;
   } else {
      bnet_fsend(dir, _("2994 Bad verify level: %s\n"), dir->msg);
      return 0;
   }

   bnet_fsend(dir, OKverify);

   generate_daemon_event(jcr, "JobStart");

   Dmsg1(110, "bfiled>dird: %s", dir->msg);

   switch (jcr->JobLevel) {
   case L_VERIFY_INIT:
   case L_VERIFY_CATALOG:
      do_verify(jcr);
      break;
   case L_VERIFY_VOLUME_TO_CATALOG:
      if (!open_sd_read_session(jcr)) {
         return 0;
      }
      start_dir_heartbeat(jcr);
      do_verify_volume(jcr);
      stop_dir_heartbeat(jcr);
      /*
       * Send Close session command to Storage daemon
       */
      bnet_fsend(sd, read_close, jcr->Ticket);
      Dmsg1(130, "bfiled>stored: %s", sd->msg);

      /* ****FIXME**** check response */
      bget_msg(sd);                      /* get OK */

      /* Inform Storage daemon that we are done */
      bnet_sig(sd, BNET_TERMINATE);

      break;
   case L_VERIFY_DISK_TO_CATALOG:
      do_verify(jcr);
      break;
   default:
      bnet_fsend(dir, _("2994 Bad verify level: %s\n"), dir->msg);
      return 0;
   }

   bnet_sig(dir, BNET_EOD);

   /* Send termination status back to Dir */
   bnet_fsend(dir, EndJob, jcr->JobStatus, jcr->JobFiles,
      edit_uint64(jcr->ReadBytes, ed1),
      edit_uint64(jcr->JobBytes, ed2), jcr->Errors);

   /* Inform Director that we are done */
   bnet_sig(dir, BNET_TERMINATE);
   return 0;                          /* return and terminate command loop */
}

/*
 * Do a Restore for Director
 *
 */
static int restore_cmd(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;
   BSOCK *sd = jcr->store_bsock;
   POOLMEM *where;
   int prefix_links;
   char replace;
   char ed1[50], ed2[50];

   /*
    * Scan WHERE (base directory for restore) from command
    */
   Dmsg0(150, "restore command\n");
   /* Pickup where string */
   where = get_memory(dir->msglen+1);
   *where = 0;

   if (sscanf(dir->msg, restorecmd, &replace, &prefix_links, where) != 3) {
      if (sscanf(dir->msg, restorecmd1, &replace, &prefix_links) != 2) {
         pm_strcpy(jcr->errmsg, dir->msg);
         Jmsg(jcr, M_FATAL, 0, _("Bad replace command. CMD=%s\n"), jcr->errmsg);
         return 0;
      }
      *where = 0;
   }
   /* Turn / into nothing */
   if (where[0] == '/' && where[1] == 0) {
      where[0] = 0;
   }

   Dmsg2(150, "Got replace %c, where=%s\n", replace, where);
   unbash_spaces(where);
   jcr->where = bstrdup(where);
   free_pool_memory(where);
   jcr->replace = replace;
   jcr->prefix_links = prefix_links;

   bnet_fsend(dir, OKrestore);
   Dmsg1(110, "bfiled>dird: %s", dir->msg);

   jcr->JobType = JT_RESTORE;

   set_jcr_job_status(jcr, JS_Blocked);

   if (!open_sd_read_session(jcr)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      goto bail_out;
   }

   set_jcr_job_status(jcr, JS_Running);

   /*
    * Do restore of files and data
    */
   start_dir_heartbeat(jcr);
   generate_daemon_event(jcr, "JobStart");
   do_restore(jcr);
   stop_dir_heartbeat(jcr);

   set_jcr_job_status(jcr, JS_Terminated);
   if (jcr->JobStatus != JS_Terminated) {
      bnet_suppress_error_messages(sd, 1);
   }

   /*
    * Send Close session command to Storage daemon
    */
   bnet_fsend(sd, read_close, jcr->Ticket);
   Dmsg1(130, "bfiled>stored: %s", sd->msg);

   bget_msg(sd);                      /* get OK */

   /* Inform Storage daemon that we are done */
   bnet_sig(sd, BNET_TERMINATE);

bail_out:

   if (jcr->Errors) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
   }
   /* Send termination status back to Dir */
   bnet_fsend(dir, EndJob, jcr->JobStatus, jcr->JobFiles,
      edit_uint64(jcr->ReadBytes, ed1),
      edit_uint64(jcr->JobBytes, ed2), jcr->Errors);

   /* Inform Director that we are done */
   bnet_sig(dir, BNET_TERMINATE);

   Dmsg0(130, "Done in job.c\n");
   return 0;                          /* return and terminate command loop */
}

static int open_sd_read_session(JCR *jcr)
{
   BSOCK *sd = jcr->store_bsock;

   if (!sd) {
      Jmsg(jcr, M_FATAL, 0, _("Improper calling sequence.\n"));
      return 0;
   }
   Dmsg4(120, "VolSessId=%ld VolsessT=%ld SF=%ld EF=%ld\n",
      jcr->VolSessionId, jcr->VolSessionTime, jcr->StartFile, jcr->EndFile);
   Dmsg2(120, "JobId=%d vol=%s\n", jcr->JobId, "DummyVolume");
   /*
    * Open Read Session with Storage daemon
    */
   bnet_fsend(sd, read_open, "DummyVolume",
      jcr->VolSessionId, jcr->VolSessionTime, jcr->StartFile, jcr->EndFile,
      jcr->StartBlock, jcr->EndBlock);
   Dmsg1(110, ">stored: %s", sd->msg);

   /*
    * Get ticket number
    */
   if (bget_msg(sd) >= 0) {
      Dmsg1(110, "bfiled<stored: %s", sd->msg);
      if (sscanf(sd->msg, OK_open, &jcr->Ticket) != 1) {
         Jmsg(jcr, M_FATAL, 0, _("Bad response to SD read open: %s\n"), sd->msg);
         return 0;
      }
      Dmsg1(110, "bfiled: got Ticket=%d\n", jcr->Ticket);
   } else {
      Jmsg(jcr, M_FATAL, 0, _("Bad response from stored to read open command\n"));
      return 0;
   }

   if (!send_bootstrap_file(jcr)) {
      return 0;
   }

   /*
    * Start read of data with Storage daemon
    */
   bnet_fsend(sd, read_data, jcr->Ticket);
   Dmsg1(110, ">stored: %s", sd->msg);

   /*
    * Get OK data
    */
   if (!response(jcr, sd, OK_data, "Read Data")) {
      return 0;
   }
   return 1;
}

/*
 * Destroy the Job Control Record and associated
 * resources (sockets).
 */
static void filed_free_jcr(JCR *jcr)
{
   if (jcr->store_bsock) {
      bnet_close(jcr->store_bsock);
   }
   free_bootstrap(jcr);
   if (jcr->last_fname) {
      free_pool_memory(jcr->last_fname);
   }
   if (jcr->RunAfterJob) {
      free_pool_memory(jcr->RunAfterJob);
   }


   return;
}

/*
 * Get response from Storage daemon to a command we
 * sent. Check that the response is OK.
 *
 *  Returns: 0 on failure
 *           1 on success
 */
int response(JCR *jcr, BSOCK *sd, char *resp, const char *cmd)
{
   if (sd->errors) {
      return 0;
   }
   if (bget_msg(sd) > 0) {
      Dmsg0(110, sd->msg);
      if (strcmp(sd->msg, resp) == 0) {
         return 1;
      }
   }
   if (job_canceled(jcr)) {
      return 0;                       /* if canceled avoid useless error messages */
   }
   if (is_bnet_error(sd)) {
      Jmsg2(jcr, M_FATAL, 0, _("Comm error with SD. bad response to %s. ERR=%s\n"),
         cmd, bnet_strerror(sd));
   } else {
      Jmsg3(jcr, M_FATAL, 0, _("Bad response to %s command. Wanted %s, got %s\n"),
         cmd, resp, sd->msg);
   }
   return 0;
}

static int send_bootstrap_file(JCR *jcr)
{
   FILE *bs;
   char buf[2000];
   BSOCK *sd = jcr->store_bsock;
   const char *bootstrap = "bootstrap\n";
   int stat = 0;

   Dmsg1(400, "send_bootstrap_file: %s\n", jcr->RestoreBootstrap);
   if (!jcr->RestoreBootstrap) {
      return 1;
   }
   bs = fopen(jcr->RestoreBootstrap, "r");
   if (!bs) {
      berrno be;
      Jmsg(jcr, M_FATAL, 0, _("Could not open bootstrap file %s: ERR=%s\n"),
         jcr->RestoreBootstrap, be.strerror());
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      goto bail_out;
   }
   sd->msglen = pm_strcpy(sd->msg, bootstrap);
   bnet_send(sd);
   while (fgets(buf, sizeof(buf), bs)) {
      sd->msglen = Mmsg(sd->msg, "%s", buf);
      bnet_send(sd);
   }
   bnet_sig(sd, BNET_EOD);
   fclose(bs);
   if (!response(jcr, sd, OKSDbootstrap, "Bootstrap")) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      goto bail_out;
   }
   stat = 1;

bail_out:
   free_bootstrap(jcr);
   return stat;
}
