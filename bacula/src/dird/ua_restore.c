/*
 *
 *   Bacula Director -- User Agent Database restore Command
 *	Creates a bootstrap file for restoring files and
 *	starts the restore job.
 *
 *	Tree handling routines split into ua_tree.c July MMIII.
 *	BSR (bootstrap record) handling routines split into
 *	  bsr.c July MMIII
 *
 *     Kern Sibbald, July MMII
 *
 *   Version $Id$
 */

/*
   Copyright (C) 2002-2003 Kern Sibbald and John Walker

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


/* Imported functions */
extern int runcmd(UAContext *ua, char *cmd);
extern void print_bsr(UAContext *ua, RBSR *bsr);

/* Imported variables */
extern char *uar_list_jobs,	*uar_file,	  *uar_sel_files;
extern char *uar_del_temp,	*uar_del_temp1,   *uar_create_temp;
extern char *uar_create_temp1,	*uar_last_full,   *uar_full;
extern char *uar_inc_dec,	*uar_list_temp,   *uar_sel_jobid_temp;
extern char *uar_sel_all_temp1, *uar_sel_fileset, *uar_mediatype;
extern char *uar_jobid_fileindex;


struct NAME_LIST {
   char **name; 		      /* list of names */
   int num_ids; 		      /* ids stored */
   int max_ids; 		      /* size of array */
   int num_del; 		      /* number deleted */
   int tot_ids; 		      /* total to process */
};


/* Main structure for obtaining JobIds or Files to be restored */
struct RESTORE_CTX {
   utime_t JobTDate;
   uint32_t TotalFiles;
   uint32_t JobId;
   char ClientName[MAX_NAME_LENGTH];
   char last_jobid[10];
   POOLMEM *JobIds;		      /* User entered string of JobIds */
   STORE  *store;
   JOB *restore_job;
   int restore_jobs;
   uint32_t selected_files;
   char *where;
   RBSR *bsr;
   POOLMEM *fname;
   POOLMEM *path;
   POOLMEM *query;
   int fnl;
   int pnl;
   bool found;
   NAME_LIST name_list;
};


#define MAX_ID_LIST_LEN 1000000


/* Forward referenced functions */
static int last_full_handler(void *ctx, int num_fields, char **row);
static int jobid_handler(void *ctx, int num_fields, char **row);
static int next_jobid_from_list(char **p, uint32_t *JobId);
static int user_select_jobids_or_files(UAContext *ua, RESTORE_CTX *rx);
static int fileset_handler(void *ctx, int num_fields, char **row);
static void print_name_list(UAContext *ua, NAME_LIST *name_list);
static int unique_name_list_handler(void *ctx, int num_fields, char **row);
static void free_name_list(NAME_LIST *name_list);
static void get_storage_from_mediatype(UAContext *ua, NAME_LIST *name_list, RESTORE_CTX *rx);
static int select_backups_before_date(UAContext *ua, RESTORE_CTX *rx, char *date);
static void build_directory_tree(UAContext *ua, RESTORE_CTX *rx);
static void free_rx(RESTORE_CTX *rx);
static void split_path_and_filename(RESTORE_CTX *rx, char *fname);
static int jobid_fileindex_handler(void *ctx, int num_fields, char **row);
static int insert_file_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *file);
static void insert_one_file(UAContext *ua, RESTORE_CTX *rx);
static int get_client_name(UAContext *ua, RESTORE_CTX *rx);

/*
 *   Restore files
 *
 */
int restorecmd(UAContext *ua, char *cmd)
{
   RESTORE_CTX rx;		      /* restore context */
   JOB *job = NULL;
   int i;

   memset(&rx, 0, sizeof(rx));

   rx.path = get_pool_memory(PM_FNAME);
   rx.fname = get_pool_memory(PM_FNAME);
   rx.JobIds = get_pool_memory(PM_FNAME);
   rx.query = get_pool_memory(PM_FNAME);
   rx.bsr = new_bsr();

   i = find_arg_with_value(ua, "where");
   if (i >= 0) {
      rx.where = ua->argv[i];
   }

   if (!open_db(ua)) {
      free_rx(&rx);
      return 0;
   }

   /* Ensure there is at least one Restore Job */
   LockRes();
   while ( (job = (JOB *)GetNextRes(R_JOB, (RES *)job)) ) {
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
         "No Restore Job Resource found. You must create at least\n"
         "one before running this command.\n"));
      free_rx(&rx);
      return 0;
   }

   /* 
    * Request user to select JobIds or files by various different methods
    *  last 20 jobs, where File saved, most recent backup, ...
    *  In the end, a list of files are pumped into
    *  add_findex()
    */
   switch (user_select_jobids_or_files(ua, &rx)) {
   case 0:
      free_rx(&rx);
      return 0; 		      /* error */
   case 1:			      /* select by jobid */
      build_directory_tree(ua, &rx);
      break;
   case 2:
      break;
   }

   if (rx.bsr->JobId) {
      if (!complete_bsr(ua, rx.bsr)) {	 /* find Vol, SessId, SessTime from JobIds */
         bsendmsg(ua, _("Unable to construct a valid BSR. Cannot continue.\n"));
	 free_rx(&rx);
	 return 0;
      }
      write_bsr_file(ua, rx.bsr);
      bsendmsg(ua, _("\n%u file%s selected to restore.\n\n"), rx.selected_files,
         rx.selected_files==1?"":"s");
   } else {
      bsendmsg(ua, _("No files selected to restore.\n"));
      free_rx(&rx);
      return 0;
   }

   if (rx.restore_jobs == 1) {
      job = rx.restore_job;
   } else {
      job = select_restore_job_resource(ua);
   }
   if (!job) {
      bsendmsg(ua, _("No Restore Job resource found!\n"));
      free_rx(&rx);
      return 0;
   }

   get_client_name(ua, &rx);

   /* Build run command */
   if (rx.where) {
      Mmsg(&ua->cmd, 
          "run job=\"%s\" client=\"%s\" storage=\"%s\" bootstrap=\"%s/restore.bsr\""
          " where=\"%s\"",
          job->hdr.name, rx.ClientName, rx.store?rx.store->hdr.name:"",
	  working_directory, rx.where);
   } else {
      Mmsg(&ua->cmd, 
          "run job=\"%s\" client=\"%s\" storage=\"%s\" bootstrap=\"%s/restore.bsr\"",
          job->hdr.name, rx.ClientName, rx.store?rx.store->hdr.name:"",
	  working_directory);
   }
   
   Dmsg1(400, "Submitting: %s\n", ua->cmd);
   parse_ua_args(ua);
   runcmd(ua, ua->cmd);

   bsendmsg(ua, _("Restore command done.\n"));
   free_rx(&rx);
   return 1;
}

static void free_rx(RESTORE_CTX *rx) 
{
   free_bsr(rx->bsr);
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

static int get_client_name(UAContext *ua, RESTORE_CTX *rx)
{
   /* If no client name specified yet, get it now */
   if (!rx->ClientName[0]) {
      CLIENT_DBR cr;
      /* try command line argument */
      int i = find_arg_with_value(ua, _("client"));
      if (i >= 0) {
	 bstrncpy(rx->ClientName, ua->argv[i], sizeof(rx->ClientName));
	 return 1;
      }
      memset(&cr, 0, sizeof(cr));
      if (!get_client_dbr(ua, &cr)) {
	 free_rx(rx);
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
 */
static int user_select_jobids_or_files(UAContext *ua, RESTORE_CTX *rx)
{
   char *p;
   char date[MAX_TIME_LENGTH];
   JobId_t JobId;
   JOB_DBR jr;
   bool done = false;
   int i;
   char *list[] = { 
      "List last 20 Jobs run",
      "List Jobs where a given File is saved",
      "Enter list of JobIds to select",
      "Enter SQL list command", 
      "Select the most recent backup for a client",
      "Select backup for a client before a specified time",
      "Enter a list of files to restore",
      "Cancel",
      NULL };

   char *kw[] = {
      "jobid",     /* 0 */
      "current",   /* 1 */
      "before",    /* 2 */
      "file",      /* 3 */
      NULL
   };

   switch (find_arg_keyword(ua, kw)) {
   case 0:			      /* jobid */
      i = find_arg_with_value(ua, _("jobid"));
      if (i < 0) {
	 return 0;
      }
      pm_strcpy(&rx->JobIds, ua->argv[i]);
      done = true;
      break;
   case 1:			      /* current */
      bstrutime(date, sizeof(date), time(NULL));
      if (!select_backups_before_date(ua, rx, date)) {
	 return 0;
      }
      done = true;
      break;
   case 2:			      /* before */
      i = find_arg_with_value(ua, _("before"));
      if (i < 0) {
	 return 0;
      }
      if (str_to_utime(ua->argv[i]) == 0) {
         bsendmsg(ua, _("Improper date format: %s\n"), ua->argv[i]);
	 return 0;
      }
      bstrncpy(date, ua->argv[i], sizeof(date));
      if (!select_backups_before_date(ua, rx, date)) {
	 return 0;
      }
      done = true;
      break;
   case 3:			      /* file */
      if (!get_client_name(ua, rx)) {
	 return 0;
      }
      for ( ;; ) {
         i = find_arg_with_value(ua, _("file"));
	 if (i < 0) {
	    break;
	 }
	 pm_strcpy(&ua->cmd, ua->argv[i]);
	 insert_one_file(ua, rx);
	 ua->argk[i][0] = 0;
      }
      /* Check MediaType and select storage that corresponds */
      get_storage_from_mediatype(ua, &rx->name_list, rx);
      return 2;
   default:
      break;
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

      start_prompt(ua, _("To select the JobIds, you have the following choices:\n"));
      for (int i=0; list[i]; i++) {
	 add_prompt(ua, list[i]);
      }
      done = true;
      switch (do_prompt(ua, "", _("Select item: "), NULL, 0)) {
      case -1:			      /* error */
	 return 0;
      case 0:			      /* list last 20 Jobs run */
	 db_list_sql_query(ua->jcr, ua->db, uar_list_jobs, prtit, ua, 1, HORZ_LIST);
	 done = false;
	 break;
      case 1:			      /* list where a file is saved */
         if (!get_cmd(ua, _("Enter Filename: "))) {
	    return 0;
	 }
	 len = strlen(ua->cmd);
	 fname = (char *)malloc(len * 2 + 1);
	 db_escape_string(fname, ua->cmd, len);
	 Mmsg(&rx->query, uar_file, fname);
	 free(fname);
	 db_list_sql_query(ua->jcr, ua->db, rx->query, prtit, ua, 1, HORZ_LIST);
	 done = false;
	 break;
      case 2:			      /* enter a list of JobIds */
         if (!get_cmd(ua, _("Enter JobId(s), comma separated, to restore: "))) {
	    return 0;
	 }
	 pm_strcpy(&rx->JobIds, ua->cmd);
	 break;
      case 3:			      /* Enter an SQL list command */
         if (!get_cmd(ua, _("Enter SQL list command: "))) {
	    return 0;
	 }
	 db_list_sql_query(ua->jcr, ua->db, ua->cmd, prtit, ua, 1, HORZ_LIST);
	 done = false;
	 break;
      case 4:			      /* Select the most recent backups */
	 bstrutime(date, sizeof(date), time(NULL));
	 if (!select_backups_before_date(ua, rx, date)) {
	    return 0;
	 }
	 break;
      case 5:			      /* select backup at specified time */
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
	 bstrncpy(date, ua->cmd, sizeof(date));
	 if (!select_backups_before_date(ua, rx, date)) {
	    return 0;
	 }
	 break;
      case 6:			      /* Enter files */
	 if (!get_client_name(ua, rx)) {
	    return 0;
	 }
         bsendmsg(ua, _("Enter file names, or < to enter a filename\n"      
                        "containg a list of file names, and terminate\n"
                        "them with a blank line.\n"));
	 for ( ;; ) {
            if (!get_cmd(ua, _("Enter filename: "))) {
	       return 0;
	    }
	    len = strlen(ua->cmd);
	    if (len == 0) {
	       break;
	    }
	    insert_one_file(ua, rx);
	 }
	 /* Check MediaType and select storage that corresponds */
	 get_storage_from_mediatype(ua, &rx->name_list, rx);
	 return 2;
      
      case 7:			      /* Cancel or quit */
	 return 0;
      }
   }

   if (*rx->JobIds == 0) {
      bsendmsg(ua, _("No Jobs selected.\n"));
      return 0;
   }
   bsendmsg(ua, _("You have selected the following JobId%s: %s\n"), 
      strchr(rx->JobIds,',')?"s":"",rx->JobIds);

   memset(&jr, 0, sizeof(JOB_DBR));

   rx->TotalFiles = 0;
   for (p=rx->JobIds; ; ) {
      int stat = next_jobid_from_list(&p, &JobId);
      if (stat < 0) {
         bsendmsg(ua, _("Invalid JobId in list.\n"));
	 return 0;
      }
      if (stat == 0) {
	 break;
      }
      if (jr.JobId == JobId) {
	 continue;		      /* duplicate of last JobId */
      }
      jr.JobId = JobId;
      if (!db_get_job_record(ua->jcr, ua->db, &jr)) {
         bsendmsg(ua, _("Unable to get Job record. ERR=%s\n"), db_strerror(ua->db));
	 return 0;
      }
      rx->TotalFiles += jr.JobFiles;
   }
   return 1;
}

static void insert_one_file(UAContext *ua, RESTORE_CTX *rx)
{
   FILE *ffd;
   char file[5000];
   char *p = ua->cmd;
   int line = 0;
  
   switch (*p) {
   case '<':
      p++;
      if ((ffd = fopen(p, "r")) == NULL) {
         bsendmsg(ua, _("Cannot open file %s: ERR=%s\n"),
	    p, strerror(errno));
	 break;
      }
      while (fgets(file, sizeof(file), ffd)) {
	 line++;
	 if (!insert_file_into_findex_list(ua, rx, file)) {
            bsendmsg(ua, _("Error occurred on line %d of %s\n"), line, p);
	 }
      }
      fclose(ffd);
      break;
   default:
      insert_file_into_findex_list(ua, rx, ua->cmd);
      break;
   }
}

/*
 * For a given file (path+filename), split into path and file, then
 *   lookup the most recent backup in the catalog to get the JobId
 *   and FileIndex, then insert them into the findex list.
 */
static int insert_file_into_findex_list(UAContext *ua, RESTORE_CTX *rx, char *file)
{
   strip_trailing_junk(file);
   split_path_and_filename(rx, file);
   Mmsg(&rx->query, uar_jobid_fileindex, rx->path, rx->fname, rx->ClientName);
   rx->found = false;
   /* Find and insert jobid and File Index */
   if (!db_sql_query(ua->db, rx->query, jobid_fileindex_handler, (void *)rx)) {
      bsendmsg(ua, _("Query failed: %s. ERR=%s\n"), 
	 rx->query, db_strerror(ua->db));
   }
   if (!rx->found) {
      bsendmsg(ua, _("No database record found for: %s\n"), file);
      return 0;
   }
   rx->selected_files++;
   /*
    * Find the FileSets for this JobId and add to the name_list
    */
   Mmsg(&rx->query, uar_mediatype, rx->JobId);
   if (!db_sql_query(ua->db, rx->query, unique_name_list_handler, (void *)&rx->name_list)) {
      bsendmsg(ua, "%s", db_strerror(ua->db));
      return 0;
   }
   return 1;
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
	 f = p; 		      /* set pos of last slash */
      }
   }
   if (*f == '/') {                   /* did we find a slash? */
      f++;			      /* yes, point to filename */
   } else {			      /* no, whole thing must be path name */
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
      memcpy(rx->fname, f, rx->fnl);	/* copy filename */
      rx->fname[rx->fnl] = 0;
   } else {
      rx->fname[0] = ' ';            /* blank filename */
      rx->fname[1] = 0;
      rx->fnl = 1;
   }

   rx->pnl = f - name;	  
   if (rx->pnl > 0) {
      rx->path = check_pool_memory_size(rx->path, rx->pnl+1);
      memcpy(rx->path, name, rx->pnl);
      rx->path[rx->pnl] = 0;
   } else {
      rx->path[0] = ' ';
      rx->path[1] = 0;
      rx->pnl = 1;
   }

   Dmsg2(100, "sllit path=%s file=%s\n", rx->path, rx->fname);
}

static void build_directory_tree(UAContext *ua, RESTORE_CTX *rx)
{
   TREE_CTX tree;
   JobId_t JobId, last_JobId;
   char *p;
   char *nofname = "";

   memset(&tree, 0, sizeof(TREE_CTX));
   /* 
    * Build the directory tree containing JobIds user selected
    */
   tree.root = new_tree(rx->TotalFiles);
   tree.root->fname = nofname;
   tree.ua = ua;
   last_JobId = 0;
   /*
    * For display purposes, the same JobId, with different volumes may
    * appear more than once, however, we only insert it once.
    */
   int items = 0;
   for (p=rx->JobIds; next_jobid_from_list(&p, &JobId) > 0; ) {

      if (JobId == last_JobId) {	     
	 continue;		      /* eliminate duplicate JobIds */
      }
      last_JobId = JobId;
      bsendmsg(ua, _("Building directory tree for JobId %u ...\n"), JobId);
      items++;
      /*
       * Find files for this JobId and insert them in the tree
       */
      Mmsg(&rx->query, uar_sel_files, JobId);
      if (!db_sql_query(ua->db, rx->query, insert_tree_handler, (void *)&tree)) {
         bsendmsg(ua, "%s", db_strerror(ua->db));
      }
      /*
       * Find the FileSets for this JobId and add to the name_list
       */
      Mmsg(&rx->query, uar_mediatype, JobId);
      if (!db_sql_query(ua->db, rx->query, unique_name_list_handler, (void *)&rx->name_list)) {
         bsendmsg(ua, "%s", db_strerror(ua->db));
      }
   }
   bsendmsg(ua, "%d Job%s inserted into the tree and marked for extraction.\n", 
      items, items==1?"":"s");

   /* Check MediaType and select storage that corresponds */
   get_storage_from_mediatype(ua, &rx->name_list, rx);

   if (find_arg(ua, _("all")) < 0) {
      /* Let the user select which files to restore */
      user_select_files_from_tree(&tree);
   }

   /*
    * Walk down through the tree finding all files marked to be 
    *  extracted making a bootstrap file.
    */
   for (TREE_NODE *node=first_tree_node(tree.root); node; node=next_tree_node(node)) {
      Dmsg2(400, "FI=%d node=0x%x\n", node->FileIndex, node);
      if (node->extract) {
         Dmsg2(400, "type=%d FI=%d\n", node->type, node->FileIndex);
	 add_findex(rx->bsr, node->JobId, node->FileIndex);
	 rx->selected_files++;
      }
   }

   free_tree(tree.root);	      /* free the directory tree */
}


/*
 * This routine is used to get the current backup or a backup
 *   before the specified date.
 */
static int select_backups_before_date(UAContext *ua, RESTORE_CTX *rx, char *date)
{
   int stat = 0;
   FILESET_DBR fsr;
   CLIENT_DBR cr;
   char fileset_name[MAX_NAME_LENGTH];
   char ed1[50];


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
    * Select FileSet 
    */
   Mmsg(&rx->query, uar_sel_fileset, cr.ClientId, cr.ClientId);
   start_prompt(ua, _("The defined FileSet resources are:\n"));
   if (!db_sql_query(ua->db, rx->query, fileset_handler, (void *)ua)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
   }
   if (do_prompt(ua, _("FileSet"), _("Select FileSet resource"), 
		 fileset_name, sizeof(fileset_name)) < 0) {
      goto bail_out;
   }
   fsr.FileSetId = atoi(fileset_name);	/* Id is first part of name */
   if (!db_get_fileset_record(ua->jcr, ua->db, &fsr)) {
      bsendmsg(ua, _("Error getting FileSet record: %s\n"), db_strerror(ua->db));
      bsendmsg(ua, _("This probably means you modified the FileSet.\n"
                     "Continuing anyway.\n"));
   }


   /* Find JobId of last Full backup for this client, fileset */
   Mmsg(&rx->query, uar_last_full, cr.ClientId, cr.ClientId, date, fsr.FileSetId);
   if (!db_sql_query(ua->db, rx->query, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
      goto bail_out;
   }

   /* Find all Volumes used by that JobId */
   if (!db_sql_query(ua->db, uar_full, NULL, NULL)) {
      bsendmsg(ua, "%s\n", db_strerror(ua->db));
      goto bail_out;
   }
   /* Note, this is needed as I don't seem to get the callback
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

   /* Now find all Incremental/Decremental Jobs after Full save */
   Mmsg(&rx->query, uar_inc_dec, edit_uint64(rx->JobTDate, ed1), date,
	cr.ClientId, fsr.FileSetId);
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
   } else {
      bsendmsg(ua, _("No jobs found.\n")); 
   }

   stat = 1;
 
bail_out:
   db_sql_query(ua->db, uar_del_temp, NULL, NULL);
   db_sql_query(ua->db, uar_del_temp1, NULL, NULL);
   return stat;
}

/* Return next JobId from comma separated list */
static int next_jobid_from_list(char **p, uint32_t *JobId)
{
   char jobid[30];
   char *q = *p;

   jobid[0] = 0;
   for (int i=0; i<(int)sizeof(jobid); i++) {
      if (*q == ',' || *q == 0) {
	 q++;
	 break;
      }
      jobid[i] = *q++;
      jobid[i+1] = 0;
   }
   if (jobid[0] == 0 || !is_a_number(jobid)) {
      return 0;
   }
   *p = q;
   *JobId = strtoul(jobid, NULL, 10);
   return 1;
}

/*
 * Callback handler to get JobId and FileIndex for files
 */
static int jobid_fileindex_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;
   rx->JobId = atoi(row[0]);
   add_findex(rx->bsr, rx->JobId, atoi(row[1]));
   rx->found = true;
   return 0;
}

/*
 * Callback handler make list of JobIds
 */
static int jobid_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;

   if (strcmp(rx->last_jobid, row[0]) == 0) {		
      return 0; 		      /* duplicate id */
   }
   bstrncpy(rx->last_jobid, row[0], sizeof(rx->last_jobid));
   if (rx->JobIds[0] != 0) {
      pm_strcat(&rx->JobIds, ",");
   }
   pm_strcat(&rx->JobIds, row[0]);
   return 0;
}


/*
 * Callback handler to pickup last Full backup JobTDate
 */
static int last_full_handler(void *ctx, int num_fields, char **row)
{
   RESTORE_CTX *rx = (RESTORE_CTX *)ctx;

   rx->JobTDate = strtoll(row[1], NULL, 10);

   return 0;
}

/*
 * Callback handler build fileset prompt list
 */
static int fileset_handler(void *ctx, int num_fields, char **row)
{
   char prompt[MAX_NAME_LENGTH+200];

   snprintf(prompt, sizeof(prompt), "%s  %s  %s", row[0], row[1], row[2]);
   add_prompt((UAContext *)ctx, prompt);
   return 0;
}

/*
 * Called here with each name to be added to the list. The name is
 *   added to the list if it is not already in the list.
 *
 * Used to make unique list of FileSets and MediaTypes
 */
static int unique_name_list_handler(void *ctx, int num_fields, char **row)
{
   NAME_LIST *name = (NAME_LIST *)ctx;

   if (name->num_ids == MAX_ID_LIST_LEN) {  
      return 1;
   }
   if (name->num_ids == name->max_ids) {
      if (name->max_ids == 0) {
	 name->max_ids = 1000;
	 name->name = (char **)bmalloc(sizeof(char *) * name->max_ids);
      } else {
	 name->max_ids = (name->max_ids * 3) / 2;
	 name->name = (char **)brealloc(name->name, sizeof(char *) * name->max_ids);
      }
   }
   for (int i=0; i<name->num_ids; i++) {
      if (strcmp(name->name[i], row[0]) == 0) {
	 return 0;		      /* already in list, return */
      }
   }
   /* Add new name to list */
   name->name[name->num_ids++] = bstrdup(row[0]);
   return 0;
}


/*
 * Print names in the list
 */
static void print_name_list(UAContext *ua, NAME_LIST *name_list)
{ 
   for (int i=0; i < name_list->num_ids; i++) {
      bsendmsg(ua, "%s\n", name_list->name[i]);
   }
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
   }
   name_list->max_ids = 0;
   name_list->num_ids = 0;
}

static void get_storage_from_mediatype(UAContext *ua, NAME_LIST *name_list, RESTORE_CTX *rx)
{
   char name[MAX_NAME_LENGTH];
   STORE *store = NULL;

   if (name_list->num_ids > 1) {
      bsendmsg(ua, _("Warning, the JobIds that you selected refer to more than one MediaType.\n"
         "Restore is not possible. The MediaTypes used are:\n"));
      print_name_list(ua, name_list);
      rx->store = select_storage_resource(ua);
      return;
   }

   if (name_list->num_ids == 0) {
      bsendmsg(ua, _("No MediaType found for your JobIds.\n"));
      rx->store = select_storage_resource(ua);
      return;
   }

   start_prompt(ua, _("The defined Storage resources are:\n"));
   LockRes();
   while ((store = (STORE *)GetNextRes(R_STORAGE, (RES *)store))) {
      if (strcmp(store->media_type, name_list->name[0]) == 0) {
	 add_prompt(ua, store->hdr.name);
      }
   }
   UnlockRes();
   do_prompt(ua, _("Storage"),  _("Select Storage resource"), name, sizeof(name));
   rx->store = (STORE *)GetResWithName(R_STORAGE, name);
   if (!rx->store) {
      bsendmsg(ua, _("\nWarning. Unable to find Storage resource for\n"
         "MediaType %s, needed by the Jobs you selected.\n"
         "You will be allowed to select a Storage device later.\n"),
	 name_list->name[0]); 
   }
}
