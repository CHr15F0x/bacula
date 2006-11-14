/*
 *
 *   Bacula Director -- User Agent Database restore Command
 *      Creates a bootstrap file for restoring files and
 *      starts the restore job.
 *
 *      Tree handling routines split into ua_tree.c July MMIII.
 *      BSR (bootstrap record) handling routines split into
 *        bsr.c July MMIII
 *
 *     Kern Sibbald, July MMII
 *
 *   Version $Id$
 */
/*
   Copyright (C) 2002-2006 Kern Sibbald

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


/* Imported functions */
extern void print_bsr(UAContext *ua, RBSR *bsr);



/* Forward referenced functions */
static int last_full_handler(void *ctx, int num_fields, char **row);
static int jobid_handler(void *ctx, int num_fields, char **row);
static int user_select_jobids_or_files(UAContext *ua, RESTORE_CTX *rx);
static int fileset_handler(void *ctx, int num_fields, char **row);
static void free_name_list(NAME_LIST *name_list);
static bool select_backups_before_date(UAContext *ua, RESTORE_CTX *rx, char *date);
static bool build_directory_tree(UAContext *ua, RESTORE_CTX *rx);
static void free_rx(RESTORE_CTX *rx);
static void split_path_and_filename(RESTORE_CTX *rx, char *fname);
static int jobid_fileindex_handler(void *ctx, int num_fields, char **row);
static bool insert_file_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *file,
                                         char *date);
static bool insert_dir_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *dir,
                                        char *date);
static void insert_one_file_or_dir(UAContext *ua, RESTORE_CTX *rx, char *date, bool dir);
static int get_client_name(UAContext *ua, RESTORE_CTX *rx);
static int get_date(UAContext *ua, char *date, int date_len);
static int count_handler(void *ctx, int num_fields, char **row);
static bool insert_table_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *table);

/*
 *   Restore files
 *
 */
int restore_cmd(UAContext *ua, const char *cmd)
{
   RESTORE_CTX rx;                    /* restore context */
   JOB *job;
   int i;
   JCR *jcr = ua->jcr;
   char *escaped_bsr_name = NULL;
   char *escaped_where_name = NULL;

   memset(&rx, 0, sizeof(rx));
   rx.path = get_pool_memory(PM_FNAME);
   rx.fname = get_pool_memory(PM_FNAME);
   rx.JobIds = get_pool_memory(PM_FNAME);
   rx.query = get_pool_memory(PM_FNAME);
   rx.bsr = new_bsr();

   i = find_arg_with_value(ua, "where");
   if (i >= 0) {
      rx.where = ua->argv[i];
      if (!acl_access_ok(ua, Where_ACL, rx.where)) {
         bsendmsg(ua, _("Forbidden \"where\" specified.\n"));
         goto bail_out;
      }
   }

   if (!open_db(ua)) {
      goto bail_out;
   }

   /* Ensure there is at least one Restore Job */
   LockRes();
   foreach_res(job, R_JOB) {
      if (job->JobType == JT_RESTORE) {
         if (!rx.restore_job) {
            rx.restore_job = job;
         }
         rx.restore_jobs++;
      }
   }
   UnlockRes();
   if (!rx.restore_jobs) {
      bsendmsg(ua, _(
         "No Restore Job Resource found in bacula-dir.conf.\n"
         "You must create at least one before running this command.\n"));
      goto bail_out;
   }

   /*
    * Request user to select JobIds or files by various different methods
    *  last 20 jobs, where File saved, most recent backup, ...
    *  In the end, a list of files are pumped into
    *  add_findex()
    */
   switch (user_select_jobids_or_files(ua, &rx)) {
   case 0:                            /* error */
      goto bail_out;
   case 1:                            /* selected by jobid */
      if (!build_directory_tree(ua, &rx)) {
         bsendmsg(ua, _("Restore not done.\n"));
         goto bail_out;
      }
      break;
   case 2:                            /* selected by filename, no tree needed */
      break;
   }

   if (rx.bsr->JobId) {
      uint32_t selected_files;
      char ed1[50];
      if (!complete_bsr(ua, rx.bsr)) {   /* find Vol, SessId, SessTime from JobIds */
         bsendmsg(ua, _("Unable to construct a valid BSR. Cannot continue.\n"));
         goto bail_out;
      }
      if (!(selected_files = write_bsr_file(ua, rx))) {
         bsendmsg(ua, _("No files selected to be restored.\n"));
         goto bail_out;
      }
      /* If no count of files, use bsr generated value (often wrong) */
      if (rx.selected_files == 0) {
         rx.selected_files = selected_files;
      }
      if (rx.selected_files==1) {
         bsendmsg(ua, _("\n1 file selected to be restored.\n\n"));
      }
      else {
         bsendmsg(ua, _("\n%s files selected to be restored.\n\n"), 
            edit_uint64_with_commas(rx.selected_files, ed1));
      }
   } else {
      bsendmsg(ua, _("No files selected to be restored.\n"));
      goto bail_out;
   }

   if (rx.restore_jobs == 1) {
      job = rx.restore_job;
   } else {
      job = select_restore_job_resource(ua);
   }
   if (!job) {
      goto bail_out;
   }

   get_client_name(ua, &rx);
   if (!rx.ClientName) {
      bsendmsg(ua, _("No Restore Job resource found!\n"));
      goto bail_out;
   }

   escaped_bsr_name = escape_filename(jcr->RestoreBootstrap);
   escaped_where_name = escape_filename(rx.where);

   /* Build run command */
   if (rx.where) {
      if (!acl_access_ok(ua, Where_ACL, rx.where)) {
         bsendmsg(ua, _("Forbidden \"where\" specified.\n"));
         goto bail_out;
      }

      Mmsg(ua->cmd,
          "run job=\"%s\" client=\"%s\" storage=\"%s\" bootstrap=\"%s\""
          " where=\"%s\" files=%d catalog=\"%s\"",
          job->name(), rx.ClientName, rx.store?rx.store->name():"",
          escaped_bsr_name ? escaped_bsr_name : jcr->RestoreBootstrap,
          escaped_where_name ? escaped_where_name : rx.where,
          rx.selected_files, ua->catalog->name());
   } else {
      Mmsg(ua->cmd,
          "run job=\"%s\" client=\"%s\" storage=\"%s\" bootstrap=\"%s\""
          " files=%d catalog=\"%s\"",
          job->name(), rx.ClientName, rx.store?rx.store->name():"",
          escaped_bsr_name ? escaped_bsr_name : jcr->RestoreBootstrap,
          rx.selected_files, ua->catalog->name());
   }

   if (escaped_bsr_name != NULL) {
      bfree(escaped_bsr_name);
   }

   if (escaped_where_name != NULL) {
      bfree(escaped_where_name);
   }

   if (find_arg(ua, NT_("yes")) > 0) {
      pm_strcat(ua->cmd, " yes");    /* pass it on to the run command */
   }
   Dmsg1(200, "Submitting: %s\n", ua->cmd);
   parse_ua_args(ua);
   run_cmd(ua, ua->cmd);
   free_rx(&rx);
   return 1;

bail_out:
   if (escaped_bsr_name != NULL) {
      bfree(escaped_bsr_name);
   }

   if (escaped_where_name != NULL) {
      bfree(escaped_where_name);
   }

   free_rx(&rx);
   return 0;

}

static void free_rx(RESTORE_CTX *rx)
{
   free_bsr(rx->bsr);
   rx->bsr = NULL;
   if (rx->JobIds) {
      free_pool_memory(rx->JobIds);
      rx->JobIds = NULL;
   }
   if (rx->fname) {
      free_pool_memory(rx->fname);
      rx->fname = NULL;
   }
   if (rx->path) {
      free_pool_memory(rx->path);
      rx->path = NULL;
   }
   if (rx->query) {
      free_pool_memory(rx->query);
      rx->query = NULL;
   }
   free_name_list(&rx->name_list);
}

static bool has_value(UAContext *ua, int i)
{
   if (!ua->argv[i]) {
      bsendmsg(ua, _("Missing value for keyword: %s\n"), ua->argk[i]);
      return false;
   }
   return true;
}

static int get_client_name(UAContext *ua, RESTORE_CTX *rx)
{
   /* If no client name specified yet, get it now */
   if (!rx->ClientName[0]) {
      CLIENT_DBR cr;
      /* try command line argument */
      int i = find_arg_with_value(ua, NT_("client"));
      if (i >= 0) {
         if (!has_value(ua, i)) {
            return 0;
         }
         bstrncpy(rx->ClientName, ua->argv[i], sizeof(rx->ClientName));
         return 1;
      }
      memset(&cr, 0, sizeof(cr));
      if (!get_client_dbr(ua, &cr)) {
         return 0;
      }
      bstrncpy(rx->ClientName, cr.Name, sizeof(rx->ClientName));
   }
   return 1;
}


/*
 * The first step in the restore process is for the user to
 *  select a list of JobIds from which he will subsequently
 *  select which files are to be restored.
 *
 *  Returns:  2  if filename list made
 *            1  if jobid list made
 *            0  on error
 */
static int user_select_jobids_or_files(UAContext *ua, RESTORE_CTX *rx)
{
   char *p;
   char date[MAX_TIME_LENGTH];
   bool have_date = false;
   JobId_t JobId;
   JOB_DBR jr = { (JobId_t)-1 };
   bool done = false;
   int i, j;
   const char *list[] = {
      _("List last 20 Jobs run"),
      _("List Jobs where a given File is saved"),
      _("Enter list of comma separated JobIds to select"),
      _("Enter SQL list command"),
      _("Select the most recent backup for a client"),
      _("Select backup for a client before a specified time"),
      _("Enter a list of files to restore"),
      _("Enter a list of files to restore before a specified time"),
      _("Find the JobIds of the most recent backup for a client"),
      _("Find the JobIds for a backup for a client before a specified time"),
      _("Enter a list of directories to restore for found JobIds"),
      _("Cancel"),
      NULL };

   const char *kw[] = {
       /* These keywords are handled in a for loop */
      "jobid",     /* 0 */
      "current",   /* 1 */
      "before",    /* 2 */
      "file",      /* 3 */
      "directory", /* 4 */
      "select",    /* 5 */
      "pool",      /* 6 */
      "all",       /* 7 */

      /* The keyword below are handled by individual arg lookups */
      "client",    /* 8 */
      "storage",   /* 9 */
      "fileset",   /* 10 */
      "where",     /* 11 */
      "yes",       /* 12 */
      "bootstrap", /* 13 */
      "done",      /* 14 */
      NULL
   };

   *rx->JobIds = 0;

   for (i=1; i<ua->argc; i++) {       /* loop through arguments */
      bool found_kw = false;
      for (j=0; kw[j]; j++) {         /* loop through keywords */
         if (strcasecmp(kw[j], ua->argk[i]) == 0) {
            found_kw = true;
            break;
         }
      }
      if (!found_kw) {
         bsendmsg(ua, _("Unknown keyword: %s\n"), ua->argk[i]);
         return 0;
      }
      /* Found keyword in kw[] list, process it */
      switch (j) {
      case 0:                            /* jobid */
         if (!has_value(ua, i)) {
            return 0;
         }
         if (*rx->JobIds != 0) {
            pm_strcat(rx->JobIds, ",");
         }
         pm_strcat(rx->JobIds, ua->argv[i]);
         done = true;
         break;
      case 1:                            /* current */
         bstrutime(date, sizeof(date), time(NULL));
         have_date = true;
         break;
      case 2:                            /* before */
         if (!has_value(ua, i)) {
            return 0;
         }
         if (str_to_utime(ua->argv[i]) == 0) {
            bsendmsg(ua, _("Improper date format: %s\n"), ua->argv[i]);
            return 0;
         }
         bstrncpy(date, ua->argv[i], sizeof(date));
         have_date = true;
         break;
      case 3:                            /* file */
      case 4:                            /* dir */
         if (!has_value(ua, i)) {
            return 0;
         }
         if (!have_date) {
            bstrutime(date, sizeof(date), time(NULL));
         }
         if (!get_client_name(ua, rx)) {
            return 0;
         }
         pm_strcpy(ua->cmd, ua->argv[i]);
         insert_one_file_or_dir(ua, rx, date, j==4);
         return 2;
      case 5:                            /* select */
         if (!have_date) {
            bstrutime(date, sizeof(date), time(NULL));
         }
         if (!select_backups_before_date(ua, rx, date)) {
            return 0;
         }
         done = true;
         break;
      case 6:                            /* pool specified */
         if (!has_value(ua, i)) {
            return 0;
         }
         rx->pool = (POOL *)GetResWithName(R_POOL, ua->argv[i]);
         if (!rx->pool) {
            bsendmsg(ua, _("Error: Pool resource \"%s\" does not exist.\n"), ua->argv[i]);
            return 0;
         }
         if (!acl_access_ok(ua, Pool_ACL, ua->argv[i])) {
            rx->pool = NULL;
            bsendmsg(ua, _("Error: Pool resource \"%s\" access not allowed.\n"), ua->argv[i]);
            return 0;
         }
         break;
      case 7:                         /* all specified */
         rx->all = true;
         break;
      /*
       * All keywords 7 or greater are ignored or handled by a select prompt
       */
      default:
         break;
      }
   }

   if (!done) {
      bsendmsg(ua, _("\nFirst you select one or more JobIds that contain files\n"
                  "to be restored. You will be presented several methods\n"
                  "of specifying the JobIds. Then you will be allowed to\n"
                  "select which files from those JobIds are to be restored.\n\n"));
   }

   /* If choice not already made above, prompt */
   for ( ; !done; ) {
      char *fname;
      int len;
      bool gui_save;

      start_prompt(ua, _("To select the JobIds, you have the following choices:\n"));
      for (int i=0; list[i]; i++) {
         add_prompt(ua, list[i]);
      }
      done = true;
      switch (do_prompt(ua, "", _("Select item: "), NULL, 0)) {
      case -1:                        /* error or cancel */
         return 0;
      case 0:                         /* list last 20 Jobs run */
         if (!acl_access_ok(ua, Command_ACL, NT_("sqlquery"), 8)) {
            bsendmsg(ua, _("SQL query not authorized.\n"));
            return 0;
         }
         gui_save = ua->jcr->gui;
         ua->jcr->gui = true;
         db_list_sql_query(ua->jcr, ua->db, uar_list_jobs, prtit, ua, 1, HORZ_LIST);
         ua->jcr->gui = gui_save;
         done = false;
         break;
      case 1:                         /* list where a file is saved */
         if (!get_client_name(ua, rx)) {
            return 0;
         }
         if (!get_cmd(ua, _("Enter Filename (no path):"))) {
            return 0;
         }
         len = strlen(ua->cmd);
         fname = (char *)malloc(len * 2 + 1);
         db_escape_string(fname, ua->cmd, len);
         Mmsg(rx->query, uar_file, rx->ClientName, fname);
         free(fname);
         gui_save = ua->jcr->gui;
         ua->jcr->gui = true;
         db_list_sql_query(ua->jcr, ua->db, rx->query, prtit, ua, 1, HORZ_LIST);
         ua->jcr->gui = gui_save;
         done = false;
         break;
      case 2:                         /* enter a list of JobIds */
         if (!get_cmd(ua, _("Enter JobId(s), comma separated, to restore: "))) {
            return 0;
         }
         pm_strcpy(rx->JobIds, ua->cmd);
         break;
      case 3:                         /* Enter an SQL list command */
         if (!acl_access_ok(ua, Command_ACL, NT_("sqlquery"), 8)) {
            bsendmsg(ua, _("SQL query not authorized.\n"));
            return 0;
         }
         if (!get_cmd(ua, _("Enter SQL list command: "))) {
            return 0;
         }
         gui_save = ua->jcr->gui;
         ua->jcr->gui = true;
         db_list_sql_query(ua->jcr, ua->db, ua->cmd, prtit, ua, 1, HORZ_LIST);
         ua->jcr->gui = gui_save;
         done = false;
         break;
      case 4:                         /* Select the most recent backups */
         bstrutime(date, sizeof(date), time(NULL));
         if (!select_backups_before_date(ua, rx, date)) {
            return 0;
         }
         break;
      case 5:                         /* select backup at specified time */
         if (!get_date(ua, date, sizeof(date))) {
            return 0;
         }
         if (!select_backups_before_date(ua, rx, date)) {
            return 0;
         }
         break;
      case 6:                         /* Enter files */
         bstrutime(date, sizeof(date), time(NULL));
         if (!get_client_name(ua, rx)) {
            return 0;
         }
         bsendmsg(ua, _("Enter file names with paths, or < to enter a filename\n"
                        "containing a list of file names with paths, and terminate\n"
                        "them with a blank line.\n"));
         for ( ;; ) {
            if (!get_cmd(ua, _("Enter full filename: "))) {
               return 0;
            }
            len = strlen(ua->cmd);
            if (len == 0) {
               break;
            }
            insert_one_file_or_dir(ua, rx, date, false);
         }
         return 2;
       case 7:                        /* enter files backed up before specified time */
         if (!get_date(ua, date, sizeof(date))) {
            return 0;
         }
         if (!get_client_name(ua, rx)) {
            return 0;
         }
         bsendmsg(ua, _("Enter file names with paths, or < to enter a filename\n"
                        "containing a list of file names with paths, and terminate\n"
                        "them with a blank line.\n"));
         for ( ;; ) {
            if (!get_cmd(ua, _("Enter full filename: "))) {
               return 0;
            }
            len = strlen(ua->cmd);
            if (len == 0) {
               break;
            }
            insert_one_file_or_dir(ua, rx, date, false);
         }
         return 2;

      case 8:                         /* Find JobIds for current backup */
         bstrutime(date, sizeof(date), time(NULL));
         if (!select_backups_before_date(ua, rx, date)) {
            return 0;
         }
         done = false;
         break;

      case 9:                         /* Find JobIds for give date */
         if (!get_date(ua, date, sizeof(date))) {
            return 0;
         }
         if (!select_backups_before_date(ua, rx, date)) {
            return 0;
         }
         done = false;
         break;

      case 10:                        /* Enter directories */
         if (*rx->JobIds != 0) {
            bsendmsg(ua, _("You have already seleted the following JobIds: %s\n"),
               rx->JobIds);
         } else if (get_cmd(ua, _("Enter JobId(s), comma separated, to restore: "))) {
            if (*rx->JobIds != 0 && *ua->cmd) {
               pm_strcat(rx->JobIds, ",");
            }
            pm_strcat(rx->JobIds, ua->cmd);
         }
         if (*rx->JobIds == 0 || *rx->JobIds == '.') {
            return 0;                 /* nothing entered, return */
         }
         bstrutime(date, sizeof(date), time(NULL));
         if (!get_client_name(ua, rx)) {
            return 0;
         }
         bsendmsg(ua, _("Enter full directory names or start the name\n"
                        "with a < to indicate it is a filename containing a list\n"
                        "of directories and terminate them with a blank line.\n"));
         for ( ;; ) {
            if (!get_cmd(ua, _("Enter directory name: "))) {
               return 0;
            }
            len = strlen(ua->cmd);
            if (len == 0) {
               break;
            }
            /* Add trailing slash to end of directory names */
            if (ua->cmd[0] != '<' && ua->cmd[len-1] != '/') {
               strcat(ua->cmd, "/");
            }
            insert_one_file_or_dir(ua, rx, date, true);
         }
         return 2;

      case 11:                        /* Cancel or quit */
         return 0;
      }
   }

   if (*rx->JobIds == 0) {
      bsendmsg(ua, _("No Jobs selected.\n"));
      return 0;
   }
   if (strchr(rx->JobIds,',')) {
      bsendmsg(ua, _("You have selected the following JobIds: %s\n"), rx->JobIds);
   }
   else {
      bsendmsg(ua, _("You have selected the following JobId: %s\n"), rx->JobIds);
   }


   rx->TotalFiles = 0;
   for (p=rx->JobIds; ; ) {
      int stat = get_next_jobid_from_list(&p, &JobId);
      if (stat < 0) {
         bsendmsg(ua, _("Invalid JobId in list.\n"));
         return 0;
      }
      if (stat == 0) {
         break;
      }
      if (jr.JobId == JobId) {
         continue;                    /* duplicate of last JobId */
      }
      memset(&jr, 0, sizeof(JOB_DBR));
      jr.JobId = JobId;
      if (!db_get_job_record(ua->jcr, ua->db, &jr)) {
         char ed1[50];
         bsendmsg(ua, _("Unable to get Job record for JobId=%s: ERR=%s\n"),
            edit_int64(JobId, ed1), db_strerror(ua->db));
         return 0;
      }
      if (!acl_access_ok(ua, Job_ACL, jr.Name)) {
         bsendmsg(ua, _("No authorization. Job \"%s\" not selected.\n"),
            jr.Name);
         continue;
      }
      rx->TotalFiles += jr.JobFiles;
   }
   return 1;
}

/*
 * Get date from user
 */
static int get_date(UAContext *ua, char *date, int date_len)
{
   bsendmsg(ua, _("The restored files will the most current backup\n"
                  "BEFORE the date you specify below.\n\n"));
   for ( ;; ) {
      if (!get_cmd(ua, _("Enter date as YYYY-MM-DD HH:MM:SS :"))) {
         return 0;
      }
      if (str_to_utime(ua->cmd) != 0) {
         break;
      }
      bsendmsg(ua, _("Improper date format.\n"));
   }
   bstrncpy(date, ua->cmd, date_len);
   return 1;
}

/*
 * Insert a single file, or read a list of files from a file
 */
static void insert_one_file_or_dir(UAContext *ua, RESTORE_CTX *rx, char *date, bool dir)
{
   FILE *ffd;
   char file[5000];
   char *p = ua->cmd;
   int line = 0;

   switch (*p) {
   case '<':
      p++;
      if ((ffd = fopen(p, "rb")) == NULL) {
         berrno be;
         bsendmsg(ua, _("Cannot open file %s: ERR=%s\n"),
            p, be.strerror());
         break;
      }
      while (fgets(file, sizeof(file), ffd)) {
         line++;
         if (dir) {
            if (!insert_dir_into_findex_list(ua, rx, file, date)) {
               bsendmsg(ua, _("Error occurred on line %d of %s\n"), line, p);
            }
         } else {
            if (!insert_file_into_findex_list(ua, rx, file, date)) {
               bsendmsg(ua, _("Error occurred on line %d of %s\n"), line, p);
            }
         }
      }
      fclose(ffd);
      break;
   case '?':
      p++;
      insert_table_into_findex_list(ua, rx, p);
      break;
   default:
      if (dir) {
         insert_dir_into_findex_list(ua, rx, ua->cmd, date);
      } else {
         insert_file_into_findex_list(ua, rx, ua->cmd, date);
      }
      break;
   }
}

/*
 * For a given file (path+filename), split into path and file, then
 *   lookup the most recent backup in the catalog to get the JobId
 *   and FileIndex, then insert them into the findex list.
 */
static bool insert_file_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *file,
                                        char *date)
{
   strip_trailing_newline(file);
   split_path_and_filename(rx, file);
   if (*rx->JobIds == 0) {
      Mmsg(rx->query, uar_jobid_fileindex, date, rx->path, rx->fname, 
           rx->ClientName);
   } else {
      Mmsg(rx->query, uar_jobids_fileindex, rx->JobIds, date,
           rx->path, rx->fname, rx->ClientName);
   }
   rx->found = false;
   /* Find and insert jobid and File Index */
   if (!db_sql_query(ua->db, rx->query, jobid_fileindex_handler, (void *)rx)) {
      bsendmsg(ua, _("Query failed: %s. ERR=%s\n"),
         rx->query, db_strerror(ua->db));
   }
   if (!rx->found) {
      bsendmsg(ua, _("No database record found for: %s\n"), file);
      return true;
   }
   return true;
}

/*
 * For a given path lookup the most recent backup in the catalog
 * to get the JobId and FileIndexes of all files in that directory.
 */
static bool insert_dir_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *dir,
                                        char *date)
{
   strip_trailing_junk(dir);
   if (*rx->JobIds == 0) {
      bsendmsg(ua, _("No JobId specified cannot continue.\n"));
      return false;
   } else {
      Mmsg(rx->query, uar_jobid_fileindex_from_dir, rx->JobIds, 
           dir, rx->ClientName);
   }
   rx->found = false;
   /* Find and insert jobid and File Index */
   if (!db_sql_query(ua->db, rx->query, jobid_fileindex_handler, (void *)rx)) {
      bsendmsg(ua, _("Query failed: %s. ERR=%s\n"),
         rx->query, db_strerror(ua->db));
   }
   if (!rx->found) {
      bsendmsg(ua, _("No database record found for: %s\n"), dir);
      return true;
   }
   return true;
}

/*
 * Get the JobId and FileIndexes of all files in the specified table
 */
static bool insert_table_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *table)
{
   strip_trailing_junk(table);
   Mmsg(rx->query, uar_jobid_fileindex_from_table, table);

   rx->found = false;
   /* Find and insert jobid and File Index */
   if (!db_sql_query(ua->db, rx->query, jobid_fileindex_handler, (void *)rx)) {
      bsendmsg(ua, _("Query failed: %s. ERR=%s\n"),
         rx->query, db_strerror(ua->db));
   }
   if (!rx->found) {
      bsendmsg(ua, _("No table found: %s\n"), table);
      return true;
   }
   return true;
}

static void split_path_and_filename(RESTORE_CTX *rx, char *name)
{
   char *p, *f;

   /* Find path without the filename.
    * I.e. everything after the last / is a "filename".
    * OK, maybe it is a directory name, but we treat it like
    * a filename. If we don't find a / then the whole name
    * must be a path name (e.g. c:).
    */
   for (p=f=name; *p; p++) {
      if (*p == '/') {
         f = p;                       /* set pos of last slash */
      }
   }
   if (*f == '/') {                   /* did we find a slash? */
      f++;                            /* yes, point to filename */
   } else {                           /* no, whole thing must be path name */
      f = p;
   }

   /* If filename doesn't exist (i.e. root directory), we
    * simply create a blank name consisting of a single
    * space. This makes handling zero length filenames
    * easier.
    */
   rx->fnl = p - f;
   if (rx->fnl > 0) {
      rx->fname = check_pool_memory_size(rx->fname, rx->fnl+1);
      memcpy(rx->fname, f, rx->fnl);    /* copy filename */
      rx->fname[rx->fnl] = 0;
   } else {
      rx->fname[0] = 0;
      rx->fnl = 0;
   }

   rx->pnl = f - name;
   if (rx->pnl > 0) {
      rx->path = check_pool_memory_size(rx->path, rx->pnl+1);
      memcpy(rx->path, name, rx->pnl);
      rx->path[rx->pnl] = 0;
   } else {
      rx->path[0] = 0;
      rx->pnl = 0;
   }

   Dmsg2(100, "split path=%s file=%s\n", rx->path, rx->fname);
}

static bool build_directory_tree(UAContext *ua, RESTORE_CTX *rx)
{
   TREE_CTX tree;
   JobId_t JobId, last_JobId;
   char *p;
   bool OK = true;
   char ed1[50];

   memset(&tree, 0, sizeof(TREE_CTX));
   /*
    * Build the directory tree containing JobIds user selected
    */
   tree.root = new_tree(rx->TotalFiles);
   tree.ua = ua;
   tree.all = rx->all;
   last_JobId = 0;
   /*
    * For display purposes, the same JobId, with different volumes may
    * appear more than once, however, we only insert it once.
    */
   int items = 0;
   p = rx->JobIds;
   tree.FileEstimate = 0;
   if (get_next_jobid_from_list(&p, &JobId) > 0) {
      /* Use first JobId as estimate of the number of files to restore */
      Mmsg(rx->query, uar_count_files, edit_int64(JobId, ed1));
      if (!db_sql_query(ua->db, rx->query, count_handler, (void *)rx)) {
         bsendmsg(ua, "%s\n", db_strerror(ua->db));
      }
      if (rx->found) {
         /* Add about 25% more than this job for over estimate */
         tree.FileEstimate = rx->JobId + (rx->JobId >> 2);
         tree.DeltaCount = rx->JobId/50; /* print 50 ticks */
      }
   }
   for (p=rx->JobIds; get_next_jobid_from_list(&p, &JobId) > 0; ) {
      char ed1[50];

      if (JobId == last_JobId) {
         continue;                    /* eliminate duplicate JobIds */
      }
      last_JobId = JobId;
      bsendmsg(ua, _("\nBuilding directory tree for JobId %s ...  "), 
         edit_int64(JobId, ed1));
      items++;
      /*
       * Find files for this JobId and insert them in the tree
       */
      Mmsg(rx->query, uar_sel_files, edit_int64(JobId, ed1));
      if (!db_sql_query(ua->db, rx->query, insert_tree_handler, (void *)&tree)) {
         bsendmsg(ua, "%s", db_strerror(ua->db));
      }
   }
   if (tree.FileCount == 0) {
      bsendmsg(ua, _("\nThere were no files inserted into the tree, so file selection\n"
         "is not possible.Most likely your retention policy pruned the files\n"));
      if (!get_yesno(ua, _("\nDo you want to restore all the files? (yes|no): "))) {
         OK = false;
      } else {
         last_JobId = 0;
         for (p=rx->JobIds; get_next_jobid_from_list(&p, &JobId) > 0; ) {
             if (JobId == last_JobId) {
                continue;                    /* eliminate duplicate JobIds */
             }
             add_findex_all(rx->bsr, JobId);
          }
          OK = true;
      }
   } else {
      char ec1[50];
      if (items==1) {
         if (tree.all) {
            bsendmsg(ua, _("\n1 Job, %s files inserted into the tree and marked for extraction.\n"),
              edit_uint64_with_commas(tree.FileCount, ec1));
         }
         else {
            bsendmsg(ua, _("\n1 Job, %s files inserted into the tree.\n"),
              edit_uint64_with_commas(tree.FileCount, ec1));
         }
      }
      else {
         if (tree.all) {
            bsendmsg(ua, _("\n%d Jobs, %s files inserted into the tree and marked for extraction.\n"),
              items, edit_uint64_with_commas(tree.FileCount, ec1));
         }
         else {
            bsendmsg(ua, _("\n%d Jobs, %s files inserted into the tree.\n"),
              items, edit_uint64_with_commas(tree.FileCount, ec1));
         }
      }

      if (find_arg(ua, NT_("done")) < 0) {
         /* Let the user interact in selecting which files to restore */
         OK = user_select_files_from_tree(&tree);
      }

      /*
       * Walk down through the tree finding all files marked to be
       *  extracted making a bootstrap file.
       */
      if (OK) {
         for (TREE_NODE *node=first_tree_node(tree.root); node; node=next_tree_node(node)) {
            Dmsg2(400, "FI=%d node=0x%x\n", node->FileIndex, node);
            if (node->extract || node->extract_dir) {
               Dmsg2(400, "type=%d FI=%d\n", node->type, node->FileIndex);
               add_findex(rx->bsr, node->JobId, node->FileIndex);
               if (node->extract && node->type != TN_NEWDIR) {
                  rx->selected_files++;  /* count only saved files */
               }
            }
         }
      }
   }

   free_tree(tree.root);              /* free the directory tree */
   return OK;
}


/*
 * This routine is used to get the current backup or a backup
 *   before the specified date.
 */
static bool select_backups_before_date(UAContext *ua, RESTORE_CTX *rx, char *date)
{
   bool ok = false;
   FILESET_DBR fsr;
   CLIENT_DBR cr;
   char fileset_name[MAX_NAME_LENGTH];
   char ed1[50], ed2[50];
   char pool_select[MAX_NAME_LENGTH];
   int i;


   /* Create temp tables */
   db_sql_query(ua->db, uar_del_temp, NULL, NULL);
   db_sql_query(ua->db, uar_del_temp1, NULL, NULL);
   if (!db_sql_query(ua->db, uar_create_temp, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }
   if (!db_sql_query(ua->db, uar_create_temp1, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }
   /*
    * Select Client from the Catalog
    */
   memset(&cr, 0, sizeof(cr));
   if (!get_client_dbr(ua, &cr)) {
      goto bail_out;
   }
   bstrncpy(rx->ClientName, cr.Name, sizeof(rx->ClientName));

   /*
    * Get FileSet
    */
   memset(&fsr, 0, sizeof(fsr));
   i = find_arg_with_value(ua, "FileSet");
   if (i >= 0) {
      bstrncpy(fsr.FileSet, ua->argv[i], sizeof(fsr.FileSet));
      if (!db_get_fileset_record(ua->jcr, ua->db, &fsr)) {
         bsendmsg(ua, _("Error getting FileSet \"%s\": ERR=%s\n"), fsr.FileSet,
            db_strerror(ua->db));
         i = -1;
      }
   }
   if (i < 0) {                       /* fileset not found */
      edit_int64(cr.ClientId, ed1);
      Mmsg(rx->query, uar_sel_fileset, ed1, ed1);
      start_prompt(ua, _("The defined FileSet resources are:\n"));
      if (!db_sql_query(ua->db, rx->query, fileset_handler, (void *)ua)) {
         bsendmsg(ua, "%s\n", db_strerror(ua->db));
      }
      if (do_prompt(ua, _("FileSet"), _("Select FileSet resource"),
                 fileset_name, sizeof(fileset_name)) < 0) {
         goto bail_out;
      }

      bstrncpy(fsr.FileSet, fileset_name, sizeof(fsr.FileSet));
      if (!db_get_fileset_record(ua->jcr, ua->db, &fsr)) {
         bsendmsg(ua, _("Error getting FileSet record: %s\n"), db_strerror(ua->db));
         bsendmsg(ua, _("This probably means you modified the FileSet.\n"
                     "Continuing anyway.\n"));
      }
   }

   /* If Pool specified, add PoolId specification */
   pool_select[0] = 0;
   if (rx->pool) {
      POOL_DBR pr;
      memset(&pr, 0, sizeof(pr));
      bstrncpy(pr.Name, rx->pool->name(), sizeof(pr.Name));
      if (db_get_pool_record(ua->jcr, ua->db, &pr)) {
         bsnprintf(pool_select, sizeof(pool_select), "AND Media.PoolId=%s ", 
            edit_int64(pr.PoolId, ed1));
      } else {
         bsendmsg(ua, _("Pool \"%s\" not found, using any pool.\n"), pr.Name);
      }
   }

   /* Find JobId of last Full backup for this client, fileset */
   edit_int64(cr.ClientId, ed1);
   Mmsg(rx->query, uar_last_full, ed1, ed1, date, fsr.FileSet,
         pool_select);
   if (!db_sql_query(ua->db, rx->query, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
      goto bail_out;
   }

   /* Find all Volumes used by that JobId */
   if (!db_sql_query(ua->db, uar_full, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
      goto bail_out;
   }
   /* Note, this is needed because I don't seem to get the callback
    * from the call just above.
    */
   rx->JobTDate = 0;
   if (!db_sql_query(ua->db, uar_sel_all_temp1, last_full_handler, (void *)rx)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }
   if (rx->JobTDate == 0) {
      bsendmsg(ua, _("No Full backup before %s found.\n"), date);
      goto bail_out;
   }

   /* Now find most recent Differental Job after Full save, if any */
   Mmsg(rx->query, uar_dif, edit_uint64(rx->JobTDate, ed1), date,
        edit_int64(cr.ClientId, ed2), fsr.FileSet, pool_select);
   if (!db_sql_query(ua->db, rx->query, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }
   /* Now update JobTDate to lock onto Differental, if any */
   rx->JobTDate = 0;
   if (!db_sql_query(ua->db, uar_sel_all_temp, last_full_handler, (void *)rx)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }
   if (rx->JobTDate == 0) {
      bsendmsg(ua, _("No Full backup before %s found.\n"), date);
      goto bail_out;
   }

   /* Now find all Incremental Jobs after Full/dif save */
   Mmsg(rx->query, uar_inc, edit_uint64(rx->JobTDate, ed1), date,
        edit_int64(cr.ClientId, ed2), fsr.FileSet, pool_select);
   if (!db_sql_query(ua->db, rx->query, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }

   /* Get the JobIds from that list */
   rx->JobIds[0] = 0;
   rx->last_jobid[0] = 0;
   if (!db_sql_query(ua->db, uar_sel_jobid_temp, jobid_handler, (void *)rx)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }

   if (rx->JobIds[0] != 0) {
      /* Display a list of Jobs selected for this restore */
      db_list_sql_query(ua->jcr, ua->db, uar_list_temp, prtit, ua, 1, HORZ_LIST);
      ok = true;
   } else {
      bsendmsg(ua, _("No jobs found.\n"));
   }

bail_out:
   db_sql_query(ua->db, uar_del_temp, NULL, NULL);
   db_sql_query(ua->db, uar_del_temp1, NULL, NULL);
   return ok;
}


/* 
 * Return next JobId from comma separated list   
 *
 * Returns:
 *   1 if next JobId returned
 *   0 if no more JobIds are in list
 *  -1 there is an error
 */
int get_next_jobid_from_list(char **p, JobId_t *JobId)
{
   char jobid[30];
   char *q = *p;

   jobid[0] = 0;
   for (int i=0; i<(int)sizeof(jobid); i++) {
      if (*q == 0) {
         break;
      } else if (*q == ',') {
         q++;
         break;
      }
      jobid[i] = *q++;
      jobid[i+1] = 0;
   }
   if (jobid[0] == 0) {
      return 0;
   } else if (!is_a_number(jobid)) {
      return -1;                      /* error */
   }
   *p = q;
   *JobId = str_to_int64(jobid);
   return 1;
}

static int count_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;
   rx->JobId = str_to_int64(row[0]);
   rx->found = true;
   return 0;
}

/*
 * Callback handler to get JobId and FileIndex for files
 *   can insert more than one depending on the caller.
 */
static int jobid_fileindex_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;
   rx->JobId = str_to_int64(row[0]);
   add_findex(rx->bsr, rx->JobId, str_to_int64(row[1]));
   rx->found = true;
   rx->selected_files++;
   return 0;
}

/*
 * Callback handler make list of JobIds
 */
static int jobid_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;

   if (strcmp(rx->last_jobid, row[0]) == 0) {
      return 0;                       /* duplicate id */
   }
   bstrncpy(rx->last_jobid, row[0], sizeof(rx->last_jobid));
   if (rx->JobIds[0] != 0) {
      pm_strcat(rx->JobIds, ",");
   }
   pm_strcat(rx->JobIds, row[0]);
   return 0;
}


/*
 * Callback handler to pickup last Full backup JobTDate
 */
static int last_full_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;

   rx->JobTDate = str_to_int64(row[1]);
   return 0;
}

/*
 * Callback handler build FileSet name prompt list
 */
static int fileset_handler(void *ctx, int num_fields, char **row)
{
   /* row[0] = FileSet (name) */
   if (row[0]) {
      add_prompt((UAContext *)ctx, row[0]);
   }
   return 0;
}

/*
 * Free names in the list
 */
static void free_name_list(NAME_LIST *name_list)
{
   for (int i=0; i < name_list->num_ids; i++) {
      free(name_list->name[i]);
   }
   if (name_list->name) {
      free(name_list->name);
      name_list->name = NULL;
   }
   name_list->max_ids = 0;
   name_list->num_ids = 0;
}

void find_storage_resource(UAContext *ua, RESTORE_CTX &rx, char *Storage, char *MediaType) 
{
   STORE *store;

   if (rx.store) {
      Dmsg1(200, "Already have store=%s\n", rx.store->name());
      return;
   }
   /*
    * Try looking up Storage by name
    */
   LockRes();
   foreach_res(store, R_STORAGE) {
      if (strcmp(Storage, store->name()) == 0) {
         if (acl_access_ok(ua, Storage_ACL, store->name())) {
            rx.store = store;
         }
         break;
      }
   }
   UnlockRes();

   if (rx.store) {
      /* Check if an explicit storage resource is given */
      store = NULL;
      int i = find_arg_with_value(ua, "storage");
      if (i > 0) {
         store = (STORE *)GetResWithName(R_STORAGE, ua->argv[i]);
         if (store && !acl_access_ok(ua, Storage_ACL, store->name())) {
            store = NULL;
         }
      }
      if (store && (store != rx.store)) {
         bsendmsg(ua, _("Warning default storage overridden by \"%s\" on command line.\n"),
            store->name());
         rx.store = store;
         Dmsg1(200, "Set store=%s\n", rx.store->name());
      }
      return;
   }

   /* If no storage resource, try to find one from MediaType */
   if (!rx.store) {
      LockRes();
      foreach_res(store, R_STORAGE) {
         if (strcmp(MediaType, store->media_type) == 0) {
            if (acl_access_ok(ua, Storage_ACL, store->name())) {
               rx.store = store;
               Dmsg1(200, "Set store=%s\n", rx.store->name());
               bsendmsg(ua, _("Storage \"%s\" not found, using Storage \"%s\" from MediaType \"%s\".\n"),
                  Storage, store->name(), MediaType);
            }
            UnlockRes();
            return;
         }
      }
      UnlockRes();
      bsendmsg(ua, _("\nUnable to find Storage resource for\n"
         "MediaType \"%s\", needed by the Jobs you selected.\n"), MediaType);
   }

   /* Take command line arg, or ask user if none */
   rx.store = get_storage_resource(ua, false /* don't use default */);
   Dmsg1(200, "Set store=%s\n", rx.store->name());

}
