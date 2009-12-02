/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2002-2009 Free Software Foundation Europe e.V.

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
 *   Bacula Director -- User Agent Commands
 *     These are "dot" commands, i.e. commands preceded
 *        by a period. These commands are meant to be used
 *        by a program, so there is no prompting, and the
 *        returned results are (supposed to be) predictable.
 *
 *     Kern Sibbald, April MMII
 *
 */

#include "bacula.h"
#include "dird.h"
#include "cats/bvfs.h"
#include "findlib/find.h"

/* Imported variables */

/* Imported functions */
extern void do_messages(UAContext *ua, const char *cmd);
extern int quit_cmd(UAContext *ua, const char *cmd);
extern int qhelp_cmd(UAContext *ua, const char *cmd);
extern bool dot_status_cmd(UAContext *ua, const char *cmd);


/* Forward referenced functions */
static bool diecmd(UAContext *ua, const char *cmd);
static bool jobscmd(UAContext *ua, const char *cmd);
static bool filesetscmd(UAContext *ua, const char *cmd);
static bool clientscmd(UAContext *ua, const char *cmd);
static bool msgscmd(UAContext *ua, const char *cmd);
static bool poolscmd(UAContext *ua, const char *cmd);
static bool storagecmd(UAContext *ua, const char *cmd);
static bool defaultscmd(UAContext *ua, const char *cmd);
static bool typescmd(UAContext *ua, const char *cmd);
static bool backupscmd(UAContext *ua, const char *cmd);
static bool levelscmd(UAContext *ua, const char *cmd);
static bool getmsgscmd(UAContext *ua, const char *cmd);
static bool volstatuscmd(UAContext *ua, const char *cmd);
static bool mediatypescmd(UAContext *ua, const char *cmd);
static bool locationscmd(UAContext *ua, const char *cmd);
static bool mediacmd(UAContext *ua, const char *cmd);
static bool aopcmd(UAContext *ua, const char *cmd);

static bool dot_bvfs_lsdirs(UAContext *ua, const char *cmd);
static bool dot_bvfs_lsfiles(UAContext *ua, const char *cmd);
static bool dot_bvfs_update(UAContext *ua, const char *cmd);

static bool api_cmd(UAContext *ua, const char *cmd);
static bool sql_cmd(UAContext *ua, const char *cmd);
static bool dot_quit_cmd(UAContext *ua, const char *cmd);
static bool dot_help_cmd(UAContext *ua, const char *cmd);

struct cmdstruct { const char *key; bool (*func)(UAContext *ua, const char *cmd); const char *help;const bool use_in_rs;};
static struct cmdstruct commands[] = { /* help */  /* can be used in runscript */
 { NT_(".api"),        api_cmd,          NULL,       false},
 { NT_(".backups"),    backupscmd,       NULL,       false},
 { NT_(".clients"),    clientscmd,       NULL,       true},
 { NT_(".defaults"),   defaultscmd,      NULL,       false},
 { NT_(".die"),        diecmd,           NULL,       false},
 { NT_(".exit"),       dot_quit_cmd,     NULL,       false},
 { NT_(".filesets"),   filesetscmd,      NULL,       false},
 { NT_(".help"),       dot_help_cmd,     NULL,       false},
 { NT_(".jobs"),       jobscmd,          NULL,       true},
 { NT_(".levels"),     levelscmd,        NULL,       false},
 { NT_(".messages"),   getmsgscmd,       NULL,       false},
 { NT_(".msgs"),       msgscmd,          NULL,       false},
 { NT_(".pools"),      poolscmd,         NULL,       true},
 { NT_(".quit"),       dot_quit_cmd,     NULL,       false},
 { NT_(".sql"),        sql_cmd,          NULL,       false},
 { NT_(".status"),     dot_status_cmd,   NULL,       false},
 { NT_(".storage"),    storagecmd,       NULL,       true},
 { NT_(".volstatus"),  volstatuscmd,     NULL,       true},
 { NT_(".media"),      mediacmd,         NULL,       true},
 { NT_(".mediatypes"), mediatypescmd,    NULL,       true},
 { NT_(".locations"),  locationscmd,     NULL,       true},
 { NT_(".actiononpurge"),aopcmd,         NULL,       true},
 { NT_(".bvfs_lsdirs"), dot_bvfs_lsdirs, NULL,       true},
 { NT_(".bvfs_lsfiles"),dot_bvfs_lsfiles,NULL,       true},
 { NT_(".bvfs_update"), dot_bvfs_update, NULL,       true},
 { NT_(".types"),      typescmd,         NULL,       false} 
             };
#define comsize ((int)(sizeof(commands)/sizeof(struct cmdstruct)))

/*
 * Execute a command from the UA
 */
bool do_a_dot_command(UAContext *ua) 
{
   int i;
   int len;
   bool ok = false;
   bool found = false;
   BSOCK *user = ua->UA_sock;

   Dmsg1(1400, "Dot command: %s\n", user->msg);
   if (ua->argc == 0) {
      return false;
   }

   len = strlen(ua->argk[0]);
   if (len == 1) {
      if (ua->api) user->signal(BNET_CMD_BEGIN);
      if (ua->api) user->signal(BNET_CMD_OK);
      return true;                    /* no op */
   }
   for (i=0; i<comsize; i++) {     /* search for command */
      if (strncasecmp(ua->argk[0],  _(commands[i].key), len) == 0) {
         /* Check if this command is authorized in RunScript */
         if (ua->runscript && !commands[i].use_in_rs) {
            ua->error_msg(_("Can't use %s command in a runscript"), ua->argk[0]);
            break;
         }
         bool gui = ua->gui;
         /* Check if command permitted, but "quit" is always OK */
         if (strcmp(ua->argk[0], NT_(".quit")) != 0 &&
             !acl_access_ok(ua, Command_ACL, ua->argk[0], len)) {
            break;
         }
         Dmsg1(100, "Cmd: %s\n", ua->cmd);
         ua->gui = true;
         if (ua->api) user->signal(BNET_CMD_BEGIN);
         ok = (*commands[i].func)(ua, ua->cmd);   /* go execute command */
         if (ua->api) user->signal(ok?BNET_CMD_OK:BNET_CMD_FAILED);
         ua->gui = gui;
         found = true;
         break;
      }
   }
   if (!found) {
      pm_strcat(user->msg, _(": is an invalid command.\n"));
      ua->error_msg("%s", user->msg);
      ok = false;
   }
   return ok;
}

static bool dot_bvfs_update(UAContext *ua, const char *cmd)
{

   if (!open_client_db(ua)) {
      return 1;
   }

   int pos = find_arg_with_value(ua, "jobid");
   if (pos != -1 && is_a_number_list(ua->argv[pos])) {
      POOL_MEM jobids;
      pm_strcpy(jobids, ua->argv[pos]);
      bvfs_update_path_hierarchy_cache(ua->jcr, ua->db, jobids.c_str());
   } else {
      /* update cache for all jobids */
      bvfs_update_cache(ua->jcr, ua->db);
   }
   return true;
}

static int bvfs_result_handler(void *ctx, int fields, char **row)
{
   UAContext *ua = (UAContext *)ctx;
   struct stat statp;
   int32_t LinkFI;
   const char *fileid;
   char *lstat;
   char empty[] = "A A A A A A A A A A A A A A";

   lstat = (row[BVFS_LStat] && row[BVFS_LStat][0])?row[BVFS_LStat]:empty;
   fileid = (row[BVFS_FileId] && row[BVFS_FileId][0])?row[BVFS_FileId]:"0";

   memset(&statp, 0, sizeof(struct stat));
   decode_stat(lstat, &statp, &LinkFI);

   Dmsg1(100, "type=%s\n", row[0]);
   if (bvfs_is_dir(row)) {
      char *path = bvfs_basename_dir(row[BVFS_Name]);
      ua->send_msg("%s\t0\t%s\t%s\t%s\t%s\n", row[BVFS_PathId], fileid,
                   row[BVFS_JobId], row[BVFS_LStat], path);

   } else if (bvfs_is_file(row)) {
      ua->send_msg("%s\t%s\t%s\t%s\t%s\t%s\n", row[BVFS_PathId],
                   row[BVFS_FilenameId], fileid, row[BVFS_JobId],
                   row[BVFS_LStat], row[BVFS_Name]);
   }

   return 0;
}

static bool bvfs_parse_arg(UAContext *ua, 
                           DBId_t *pathid, char **path, char **jobid,
                           int *limit, int *offset)
{
   *pathid=0;
   *limit=2000;
   *offset=0;
   *path=NULL;
   *jobid=NULL;

   for (int i=1; i<ua->argc; i++) {
      if (strcasecmp(ua->argk[i], NT_("pathid")) == 0) {
         if (is_a_number(ua->argv[i])) {
            *pathid = str_to_int64(ua->argv[i]);
         }
      }
      if (strcasecmp(ua->argk[i], NT_("path")) == 0) {
         *path = ua->argv[i];
      }
      
      if (strcasecmp(ua->argk[i], NT_("jobid")) == 0) {
         if (is_a_number_list(ua->argv[i])) {
            *jobid = ua->argv[i];
         }
      }

      if (strcasecmp(ua->argk[i], NT_("limit")) == 0) {
         if (is_a_number(ua->argv[i])) {
            *limit = str_to_int64(ua->argv[i]);
         }
      }

      if (strcasecmp(ua->argk[i], NT_("offset")) == 0) {
         if (is_a_number(ua->argv[i])) {
            *offset = str_to_int64(ua->argv[i]);
         }
      }
   }

   if (!((*pathid || *path) && *jobid)) {
      return false;
   }

   if (!open_client_db(ua)) {
      return false;
   }

   return true;
}

/* 
 * .bvfs_lsfiles jobid=1,2,3,4 pathid=10
 * .bvfs_lsfiles jobid=1,2,3,4 path=/
 */
static bool dot_bvfs_lsfiles(UAContext *ua, const char *cmd)
{
   DBId_t pathid=0;
   int limit=2000, offset=0;
   char *path=NULL, *jobid=NULL;

   if (!bvfs_parse_arg(ua, &pathid, &path, &jobid,
                       &limit, &offset))
   {
      ua->error_msg("Can't find jobid, pathid or path argument\n");
      return true;              /* not enough param */
   }

   Bvfs fs(ua->jcr, ua->db);
   fs.set_jobids(jobid);   
   fs.set_handler(bvfs_result_handler, ua);
   fs.set_limit(limit);

   if (pathid) {
      fs.ch_dir(pathid);
   } else {
      fs.ch_dir(path);
   }

   fs.set_offset(offset);

   fs.ls_files();

   return true;
}

/* 
 * .bvfs_lsdirs jobid=1,2,3,4 pathid=10
 * .bvfs_lsdirs jobid=1,2,3,4 path=/
 * .bvfs_lsdirs jobid=1,2,3,4 path=
 */
static bool dot_bvfs_lsdirs(UAContext *ua, const char *cmd)
{
   DBId_t pathid=0;
   int limit=2000, offset=0;
   char *path=NULL, *jobid=NULL;

   if (!bvfs_parse_arg(ua, &pathid, &path, &jobid,
                       &limit, &offset))
   {
      ua->error_msg("Can't find jobid, pathid or path argument\n");
      return true;              /* not enough param */
   }

   Bvfs fs(ua->jcr, ua->db);
   fs.set_jobids(jobid);   
   fs.set_limit(limit);
   fs.set_handler(bvfs_result_handler, ua);

   if (pathid) {
      fs.ch_dir(pathid);
   } else {
      fs.ch_dir(path);
   }

   fs.set_offset(offset);

   fs.ls_special_dirs();
   fs.ls_dirs();

   return true;
}

static bool dot_quit_cmd(UAContext *ua, const char *cmd)
{
   quit_cmd(ua, cmd);
   return true;
}

static bool dot_help_cmd(UAContext *ua, const char *cmd)
{
   qhelp_cmd(ua, cmd);
   return true;
}

static bool getmsgscmd(UAContext *ua, const char *cmd)
{
   if (console_msg_pending) {
      do_messages(ua, cmd);
   }
   return 1;
}

#ifdef DEVELOPER
static void do_storage_die(UAContext *ua, STORE *store)
{
   BSOCK *sd;
   JCR *jcr = ua->jcr;
   USTORE lstore;
   
   lstore.store = store;
   pm_strcpy(lstore.store_source, _("unknown source"));
   set_wstorage(jcr, &lstore);
   /* Try connecting for up to 15 seconds */
   ua->send_msg(_("Connecting to Storage daemon %s at %s:%d\n"),
      store->name(), store->address, store->SDport);
   if (!connect_to_storage_daemon(jcr, 1, 15, 0)) {
      ua->error_msg(_("Failed to connect to Storage daemon.\n"));
      return;
   }
   Dmsg0(120, _("Connected to storage daemon\n"));
   sd = jcr->store_bsock;
   sd->fsend(".die");
   if (sd->recv() >= 0) {
      ua->send_msg("%s", sd->msg);
   }
   sd->signal(BNET_TERMINATE);
   sd->close();
   jcr->store_bsock = NULL;
   return;
}

static void do_client_die(UAContext *ua, CLIENT *client)
{
   BSOCK *fd;

   /* Connect to File daemon */

   ua->jcr->client = client;
   /* Try to connect for 15 seconds */
   ua->send_msg(_("Connecting to Client %s at %s:%d\n"),
      client->name(), client->address, client->FDport);
   if (!connect_to_file_daemon(ua->jcr, 1, 15, 0)) {
      ua->error_msg(_("Failed to connect to Client.\n"));
      return;
   }
   Dmsg0(120, "Connected to file daemon\n");
   fd = ua->jcr->file_bsock;
   fd->fsend(".die");
   if (fd->recv() >= 0) {
      ua->send_msg("%s", fd->msg);
   }
   fd->signal(BNET_TERMINATE);
   fd->close();
   ua->jcr->file_bsock = NULL;
   return;
}

/*
 * Create segmentation fault
 */
static bool diecmd(UAContext *ua, const char *cmd)
{
   STORE *store;
   CLIENT *client;
   int i;
   JCR *jcr = NULL;
   int a;

   Dmsg1(120, "diecmd:%s:\n", cmd);

   /* General debug? */
   for (i=1; i<ua->argc; i++) {
      if (strcasecmp(ua->argk[i], "dir") == 0 ||
          strcasecmp(ua->argk[i], "director") == 0) {
         ua->send_msg(_("The Director will segment fault.\n"));
         a = jcr->JobId; /* ref NULL pointer */
         jcr->JobId = 1000; /* another ref NULL pointer */
         return 1;
      }
      if (strcasecmp(ua->argk[i], "client") == 0 ||
          strcasecmp(ua->argk[i], "fd") == 0) {
         client = NULL;
         if (ua->argv[i]) {
            client = (CLIENT *)GetResWithName(R_CLIENT, ua->argv[i]);
            if (client) {
               do_client_die(ua, client);
               return 1;
            }
         }
         client = select_client_resource(ua);
         if (client) {
            do_client_die(ua, client);
            return 1;
         }
      }

      if (strcasecmp(ua->argk[i], NT_("store")) == 0 ||
          strcasecmp(ua->argk[i], NT_("storage")) == 0 ||
          strcasecmp(ua->argk[i], NT_("sd")) == 0) {
         store = NULL;
         if (ua->argv[i]) {
            store = (STORE *)GetResWithName(R_STORAGE, ua->argv[i]);
            if (store) {
               do_storage_die(ua, store);
               return 1;
            }
         }
         store = get_storage_resource(ua, false/*no default*/);
         if (store) {
            do_storage_die(ua, store);
            return 1;
         }
      }
   }
   /*
    * We didn't find an appropriate keyword above, so
    * prompt the user.
    */
   start_prompt(ua, _("Available daemons are: \n"));
   add_prompt(ua, _("Director"));
   add_prompt(ua, _("Storage"));
   add_prompt(ua, _("Client"));
   switch(do_prompt(ua, "", _("Select daemon type to make die"), NULL, 0)) {
   case 0:                         /* Director */
      ua->send_msg(_("The Director will segment fault.\n"));
      a = jcr->JobId; /* ref NULL pointer */
      jcr->JobId = 1000; /* another ref NULL pointer */
      break;
   case 1:
      store = get_storage_resource(ua, false/*no default*/);
      if (store) {
         do_storage_die(ua, store);
      }
      break;
   case 2:
      client = select_client_resource(ua);
      if (client) {
         do_client_die(ua, client);
      }
      break;
   default:
      break;
   }
   return true;
}

#else

/*
 * Dummy routine for non-development version
 */
static bool diecmd(UAContext *ua, const char *cmd)
{
   return true;
}

#endif

/* 
 * Can use an argument to filter on JobType
 * .jobs [type=B]
 */
static bool jobscmd(UAContext *ua, const char *cmd)
{
   JOB *job;
   uint32_t type = 0;
   int pos;
   if ((pos = find_arg_with_value(ua, "type")) >= 0) {
      type = ua->argv[pos][0];
   }
   LockRes();
   foreach_res(job, R_JOB) {
      if (!type || type == job->JobType) {
         if (acl_access_ok(ua, Job_ACL, job->name())) {
            ua->send_msg("%s\n", job->name());
         }
      }
   }
   UnlockRes();
   return true;
}

static bool filesetscmd(UAContext *ua, const char *cmd)
{
   FILESET *fs;
   LockRes();
   foreach_res(fs, R_FILESET) {
      if (acl_access_ok(ua, FileSet_ACL, fs->name())) {
         ua->send_msg("%s\n", fs->name());
      }
   }
   UnlockRes();
   return true;
}

static bool clientscmd(UAContext *ua, const char *cmd)
{
   CLIENT *client;       
   LockRes();
   foreach_res(client, R_CLIENT) {
      if (acl_access_ok(ua, Client_ACL, client->name())) {
         ua->send_msg("%s\n", client->name());
      }
   }
   UnlockRes();
   return true;
}

static bool msgscmd(UAContext *ua, const char *cmd)
{
   MSGS *msgs = NULL;
   LockRes();
   foreach_res(msgs, R_MSGS) {
      ua->send_msg("%s\n", msgs->name());
   }
   UnlockRes();
   return true;
}

static bool poolscmd(UAContext *ua, const char *cmd)
{
   POOL *pool;       
   LockRes();
   foreach_res(pool, R_POOL) {
      if (acl_access_ok(ua, Pool_ACL, pool->name())) {
         ua->send_msg("%s\n", pool->name());
      }
   }
   UnlockRes();
   return true;
}

static bool storagecmd(UAContext *ua, const char *cmd)
{
   STORE *store;
   LockRes();
   foreach_res(store, R_STORAGE) {
      if (acl_access_ok(ua, Storage_ACL, store->name())) {
         ua->send_msg("%s\n", store->name());
      }
   }
   UnlockRes();
   return true;
}

static bool aopcmd(UAContext *ua, const char *cmd)
{
   ua->send_msg("None\n");
   ua->send_msg("Truncate\n");
   return true;
}

static bool typescmd(UAContext *ua, const char *cmd)
{
   ua->send_msg("Backup\n");
   ua->send_msg("Restore\n");
   ua->send_msg("Admin\n");
   ua->send_msg("Verify\n");
   ua->send_msg("Migrate\n");
   return true;
}

/*
 * If this command is called, it tells the director that we
 *  are a program that wants a sort of API, and hence,
 *  we will probably suppress certain output, include more
 *  error codes, and most of all send back a good number
 *  of new signals that indicate whether or not the command
 *  succeeded.
 */
static bool api_cmd(UAContext *ua, const char *cmd)
{
   if (ua->argc == 2) {
      ua->api = atoi(ua->argk[1]);
   } else {
      ua->api = 1;
   }
   return true;
}

static int client_backups_handler(void *ctx, int num_field, char **row)
{
   UAContext *ua = (UAContext *)ctx;
   ua->send_msg("| %s | %s | %s | %s | %s | %s | %s | %s |\n",
      row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[8]);
   return 0;
}

/*
 * Return the backups for this client 
 *
 *  .backups client=xxx fileset=yyy
 *
 */
static bool backupscmd(UAContext *ua, const char *cmd)
{
   if (!open_client_db(ua)) {
      return true;
   }
   if (ua->argc != 3 || strcmp(ua->argk[1], "client") != 0 || 
       strcmp(ua->argk[2], "fileset") != 0) {
      return true;
   }
   if (!acl_access_ok(ua, Client_ACL, ua->argv[1]) ||
       !acl_access_ok(ua, FileSet_ACL, ua->argv[2])) {
      ua->error_msg(_("Access to specified Client or FileSet not allowed.\n"));
      return true;
   }
   Mmsg(ua->cmd, client_backups, ua->argv[1], ua->argv[2]);
   if (!db_sql_query(ua->db, ua->cmd, client_backups_handler, (void *)ua)) {
      ua->error_msg(_("Query failed: %s. ERR=%s\n"), ua->cmd, db_strerror(ua->db));
      return true;
   }
   return true;
}

static int sql_handler(void *ctx, int num_field, char **row)
{
   UAContext *ua = (UAContext *)ctx;
   POOL_MEM rows(PM_MESSAGE);

   /* Check for nonsense */
   if (num_field == 0 || row == NULL || row[0] == NULL) {
      return 0;                       /* nothing returned */
   }
   for (int i=0; num_field--; i++) {
      if (i == 0) {
         pm_strcpy(rows, NPRT(row[0]));
      } else {
         pm_strcat(rows, NPRT(row[i]));
      }
      pm_strcat(rows, "\t");
   }
   if (!rows.c_str() || !*rows.c_str()) {
      ua->send_msg("\t");
   } else {
      ua->send_msg("%s", rows.c_str());
   }
   return 0;
}

static bool sql_cmd(UAContext *ua, const char *cmd)
{
   int index;
   if (!open_client_db(ua)) {
      return true;
   }
   index = find_arg_with_value(ua, "query");
   if (index < 0) {
      ua->error_msg(_("query keyword not found.\n"));
      return true;
   }
   if (!db_sql_query(ua->db, ua->argv[index], sql_handler, (void *)ua)) {
      Dmsg1(100, "Query failed: ERR=%s\n", db_strerror(ua->db));
      ua->error_msg(_("Query failed: %s. ERR=%s\n"), ua->cmd, db_strerror(ua->db));
      return true;
   }
   return true;
}
      
static int one_handler(void *ctx, int num_field, char **row)
{
   UAContext *ua = (UAContext *)ctx;
   ua->send_msg("%s\n", row[0]);
   return 0;
}

static bool mediatypescmd(UAContext *ua, const char *cmd)
{
   if (!open_client_db(ua)) {
      return true;
   }
   if (!db_sql_query(ua->db, 
                  "SELECT DISTINCT MediaType FROM MediaType ORDER BY MediaType",
                  one_handler, (void *)ua)) 
   {
      ua->error_msg(_("List MediaType failed: ERR=%s\n"), db_strerror(ua->db));
   }
   return true;
}

static bool mediacmd(UAContext *ua, const char *cmd)
{
   if (!open_client_db(ua)) {
      return true;
   }
   if (!db_sql_query(ua->db, 
                  "SELECT DISTINCT Media.VolumeName FROM Media ORDER BY VolumeName",
                  one_handler, (void *)ua)) 
   {
      ua->error_msg(_("List Media failed: ERR=%s\n"), db_strerror(ua->db));
   }
   return true;
}

static bool locationscmd(UAContext *ua, const char *cmd)
{
   if (!open_client_db(ua)) {
      return true;
   }
   if (!db_sql_query(ua->db, 
                  "SELECT DISTINCT Location FROM Location ORDER BY Location",
                  one_handler, (void *)ua)) 
   {
      ua->error_msg(_("List Location failed: ERR=%s\n"), db_strerror(ua->db));
   }
   return true;
}

static bool levelscmd(UAContext *ua, const char *cmd)
{
   ua->send_msg("Incremental\n");
   ua->send_msg("Full\n");
   ua->send_msg("Differential\n");
   ua->send_msg("VirtualFull\n");
   ua->send_msg("Catalog\n");
   ua->send_msg("InitCatalog\n");
   ua->send_msg("VolumeToCatalog\n");
   return true;
}

static bool volstatuscmd(UAContext *ua, const char *cmd)
{
   ua->send_msg("Append\n");
   ua->send_msg("Full\n");
   ua->send_msg("Used\n");
   ua->send_msg("Recycle\n");
   ua->send_msg("Purged\n");
   ua->send_msg("Cleaning\n");
   ua->send_msg("Error\n");
   return true;
}

/*
 * Return default values for a job
 */
static bool defaultscmd(UAContext *ua, const char *cmd)
{
   JOB *job;
   CLIENT *client;
   STORE *storage;
   POOL *pool;
   char ed1[50];

   if (ua->argc != 2 || !ua->argv[1]) {
      return true;
   }

   /* Job defaults */   
   if (strcmp(ua->argk[1], "job") == 0) {
      if (!acl_access_ok(ua, Job_ACL, ua->argv[1])) {
         return true;
      }
      job = (JOB *)GetResWithName(R_JOB, ua->argv[1]);
      if (job) {
         USTORE store;
         ua->send_msg("job=%s", job->name());
         ua->send_msg("pool=%s", job->pool->name());
         ua->send_msg("messages=%s", job->messages->name());
         ua->send_msg("client=%s", job->client->name());
         get_job_storage(&store, job, NULL);
         ua->send_msg("storage=%s", store.store->name());
         ua->send_msg("where=%s", job->RestoreWhere?job->RestoreWhere:"");
         ua->send_msg("level=%s", level_to_str(job->JobLevel));
         ua->send_msg("type=%s", job_type_to_str(job->JobType));
         ua->send_msg("fileset=%s", job->fileset->name());
         ua->send_msg("enabled=%d", job->enabled);
         ua->send_msg("catalog=%s", job->client->catalog->name());
      }
   } 
   /* Client defaults */
   else if (strcmp(ua->argk[1], "client") == 0) {
      if (!acl_access_ok(ua, Client_ACL, ua->argv[1])) {
         return true;   
      }
      client = (CLIENT *)GetResWithName(R_CLIENT, ua->argv[1]);
      if (client) {
         ua->send_msg("client=%s", client->name());
         ua->send_msg("address=%s", client->address);
         ua->send_msg("fdport=%d", client->FDport);
         ua->send_msg("file_retention=%s", edit_uint64(client->FileRetention, ed1));
         ua->send_msg("job_retention=%s", edit_uint64(client->JobRetention, ed1));
         ua->send_msg("autoprune=%d", client->AutoPrune);
         ua->send_msg("catalog=%s", client->catalog->name());
      }
   }
   /* Storage defaults */
   else if (strcmp(ua->argk[1], "storage") == 0) {
      if (!acl_access_ok(ua, Storage_ACL, ua->argv[1])) {
         return true;
      }
      storage = (STORE *)GetResWithName(R_STORAGE, ua->argv[1]);
      DEVICE *device;
      if (storage) {
         ua->send_msg("storage=%s", storage->name());
         ua->send_msg("address=%s", storage->address);
         ua->send_msg("enabled=%d", storage->enabled);
         ua->send_msg("media_type=%s", storage->media_type);
         ua->send_msg("sdport=%d", storage->SDport);
         device = (DEVICE *)storage->device->first();
         ua->send_msg("device=%s", device->name());
         if (storage->device->size() > 1) {
            while ((device = (DEVICE *)storage->device->next())) {
               ua->send_msg(",%s", device->name());
            }
         }
      }
   }
   /* Pool defaults */
   else if (strcmp(ua->argk[1], "pool") == 0) {
      if (!acl_access_ok(ua, Pool_ACL, ua->argv[1])) {
         return true;
      }
      pool = (POOL *)GetResWithName(R_POOL, ua->argv[1]);
      if (pool) {
         ua->send_msg("pool=%s", pool->name());
         ua->send_msg("pool_type=%s", pool->pool_type);
         ua->send_msg("label_format=%s", pool->label_format?pool->label_format:"");
         ua->send_msg("use_volume_once=%d", pool->use_volume_once);
         ua->send_msg("purge_oldest_volume=%d", pool->purge_oldest_volume);
         ua->send_msg("recycle_oldest_volume=%d", pool->recycle_oldest_volume);
         ua->send_msg("recycle_current_volume=%d", pool->recycle_current_volume);
         ua->send_msg("max_volumes=%d", pool->max_volumes);
         ua->send_msg("vol_retention=%s", edit_uint64(pool->VolRetention, ed1));
         ua->send_msg("vol_use_duration=%s", edit_uint64(pool->VolUseDuration, ed1));
         ua->send_msg("max_vol_jobs=%d", pool->MaxVolJobs);
         ua->send_msg("max_vol_files=%d", pool->MaxVolFiles);
         ua->send_msg("max_vol_bytes=%s", edit_uint64(pool->MaxVolBytes, ed1));
         ua->send_msg("auto_prune=%d", pool->AutoPrune);
         ua->send_msg("recycle=%d", pool->Recycle);
         ua->send_msg("file_retention=%s", edit_uint64(pool->FileRetention, ed1));
         ua->send_msg("job_retention=%s", edit_uint64(pool->JobRetention, ed1));
      }
   }
   return true;
}
