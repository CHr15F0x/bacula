/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2002-2007 Free Software Foundation Europe e.V.

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

   Bacula® is a registered trademark of John Walker.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
/*
 *
 *   Bacula Director -- User Agent Database prune Command
 *      Applies retention periods
 *
 *     Kern Sibbald, February MMII
 *
 *   Version $Id$
 */

#include "bacula.h"
#include "dird.h"

/* Imported functions */

/* Forward referenced functions */

/*
 * Called here to count entries to be deleted
 */
int del_count_handler(void *ctx, int num_fields, char **row)
{
   struct s_count_ctx *cnt = (struct s_count_ctx *)ctx;

   if (row[0]) {
      cnt->count = str_to_int64(row[0]);
   } else {
      cnt->count = 0;
   }
   return 0;
}


/*
 * Called here to make in memory list of JobIds to be
 *  deleted and the associated PurgedFiles flag.
 *  The in memory list will then be transversed
 *  to issue the SQL DELETE commands.  Note, the list
 *  is allowed to get to MAX_DEL_LIST_LEN to limit the
 *  maximum malloc'ed memory.
 */
int job_delete_handler(void *ctx, int num_fields, char **row)
{
   struct del_ctx *del = (struct del_ctx *)ctx;

   if (del->num_ids == MAX_DEL_LIST_LEN) {
      return 1;
   }
   if (del->num_ids == del->max_ids) {
      del->max_ids = (del->max_ids * 3) / 2;
      del->JobId = (JobId_t *)brealloc(del->JobId, sizeof(JobId_t) * del->max_ids);
      del->PurgedFiles = (char *)brealloc(del->PurgedFiles, del->max_ids);
   }
   del->JobId[del->num_ids] = (JobId_t)str_to_int64(row[0]);
// Dmsg2(60, "row=%d val=%d\n", del->num_ids, del->JobId[del->num_ids]);
   del->PurgedFiles[del->num_ids++] = (char)str_to_int64(row[1]);
   return 0;
}

int file_delete_handler(void *ctx, int num_fields, char **row)
{
   struct del_ctx *del = (struct del_ctx *)ctx;

   if (del->num_ids == MAX_DEL_LIST_LEN) {
      return 1;
   }
   if (del->num_ids == del->max_ids) {
      del->max_ids = (del->max_ids * 3) / 2;
      del->JobId = (JobId_t *)brealloc(del->JobId, sizeof(JobId_t) *
         del->max_ids);
   }
   del->JobId[del->num_ids++] = (JobId_t)str_to_int64(row[0]);
// Dmsg2(150, "row=%d val=%d\n", del->num_ids-1, del->JobId[del->num_ids-1]);
   return 0;
}

/*
 *   Prune records from database
 *
 *    prune files (from) client=xxx
 *    prune jobs (from) client=xxx
 *    prune volume=xxx
 */
int prunecmd(UAContext *ua, const char *cmd)
{
   CLIENT *client;
   POOL_DBR pr;
   MEDIA_DBR mr;
   int kw;

   static const char *keywords[] = {
      NT_("Files"),
      NT_("Jobs"),
      NT_("Volume"),
      NULL};

   if (!open_client_db(ua)) {
      return false;
   }

   /* First search args */
   kw = find_arg_keyword(ua, keywords);
   if (kw < 0 || kw > 2) {
      /* no args, so ask user */
      kw = do_keyword_prompt(ua, _("Choose item to prune"), keywords);
   }

   switch (kw) {
   case 0:  /* prune files */
      client = get_client_resource(ua);
      if (!client || !confirm_retention(ua, &client->FileRetention, "File")) {
         return false;
      }
      prune_files(ua, client);
      return true;
   case 1:  /* prune jobs */
      client = get_client_resource(ua);
      if (!client || !confirm_retention(ua, &client->JobRetention, "Job")) {
         return false;
      }
      /* ****FIXME**** allow user to select JobType */
      prune_jobs(ua, client, JT_BACKUP);
      return 1;
   case 2:  /* prune volume */
      if (!select_pool_and_media_dbr(ua, &pr, &mr)) {
         return false;
      }
      if (mr.Enabled == 2) {
         ua->error_msg(_("Cannot prune Volume \"%s\" because it is archived.\n"),
            mr.VolumeName);
         return false;
      }
      if (!confirm_retention(ua, &mr.VolRetention, "Volume")) {
         return false;
      }
      prune_volume(ua, &mr);
      return true;
   default:
      break;
   }

   return true;
}

/*
 * Prune File records from the database. For any Job which
 * is older than the retention period, we unconditionally delete
 * all File records for that Job.  This is simple enough that no
 * temporary tables are needed. We simply make an in memory list of
 * the JobIds meeting the prune conditions, then delete all File records
 * pointing to each of those JobIds.
 *
 * This routine assumes you want the pruning to be done. All checking
 *  must be done before calling this routine.
 */
int prune_files(UAContext *ua, CLIENT *client)
{
   struct del_ctx del;
   struct s_count_ctx cnt;
   POOL_MEM query(PM_MESSAGE);
   utime_t now, period;
   CLIENT_DBR cr;
   char ed1[50], ed2[50];

   db_lock(ua->db);
   memset(&cr, 0, sizeof(cr));
   memset(&del, 0, sizeof(del));
   bstrncpy(cr.Name, client->hdr.name, sizeof(cr.Name));
   if (!db_create_client_record(ua->jcr, ua->db, &cr)) {
      db_unlock(ua->db);
      return 0;
   }

   period = client->FileRetention;
   now = (utime_t)time(NULL);

   /* Select Jobs -- for counting */
   Mmsg(query, count_select_job, edit_uint64(now - period, ed1), 
        edit_int64(cr.ClientId, ed2));
   Dmsg3(050, "select now=%u period=%u sql=%s\n", (uint32_t)now, 
               (uint32_t)period, query.c_str());
   cnt.count = 0;
   if (!db_sql_query(ua->db, query.c_str(), del_count_handler, (void *)&cnt)) {
      ua->error_msg("%s", db_strerror(ua->db));
      Dmsg0(050, "Count failed\n");
      goto bail_out;
   }

   if (cnt.count == 0) {
      if (ua->verbose) {
         ua->warning_msg(_("No Files found to prune.\n"));
      }
      goto bail_out;
   }

   if (cnt.count < MAX_DEL_LIST_LEN) {
      del.max_ids = cnt.count + 1;
   } else {
      del.max_ids = MAX_DEL_LIST_LEN;
   }
   del.tot_ids = 0;

   del.JobId = (JobId_t *)malloc(sizeof(JobId_t) * del.max_ids);

   /* Now process same set but making a delete list */
   Mmsg(query, select_job, edit_uint64(now - period, ed1), 
        edit_int64(cr.ClientId, ed2));
   db_sql_query(ua->db, query.c_str(), file_delete_handler, (void *)&del);

   purge_files_from_job_list(ua, del);

   edit_uint64_with_commas(del.num_del, ed1);
   ua->info_msg(_("Pruned Files from %s Jobs for client %s from catalog.\n"),
      ed1, client->name());

bail_out:
   db_unlock(ua->db);
   if (del.JobId) {
      free(del.JobId);
   }
   return 1;
}


static void drop_temp_tables(UAContext *ua)
{
   int i;
   for (i=0; drop_deltabs[i]; i++) {
      db_sql_query(ua->db, drop_deltabs[i], NULL, (void *)NULL);
   }
}

static bool create_temp_tables(UAContext *ua)
{
   /* Create temp tables and indicies */
   if (!db_sql_query(ua->db, create_deltabs[db_type], NULL, (void *)NULL)) {
      ua->error_msg("%s", db_strerror(ua->db));
      Dmsg0(050, "create DelTables table failed\n");
      return false;
   }
   if (!db_sql_query(ua->db, create_delindex, NULL, (void *)NULL)) {
       ua->error_msg("%s", db_strerror(ua->db));
       Dmsg0(050, "create DelInx1 index failed\n");
       return false;
   }
   return true;
}



/*
 * Pruning Jobs is a bit more complicated than purging Files
 * because we delete Job records only if there is a more current
 * backup of the FileSet. Otherwise, we keep the Job record.
 * In other words, we never delete the only Job record that
 * contains a current backup of a FileSet. This prevents the
 * Volume from being recycled and destroying a current backup.
 *
 * For Verify Jobs, we do not delete the last InitCatalog.
 *
 * For Restore Jobs there are no restrictions.
 */
int prune_jobs(UAContext *ua, CLIENT *client, int JobType)
{
   struct del_ctx del;
   POOL_MEM query(PM_MESSAGE);
   utime_t now, period;
   CLIENT_DBR cr;
   char ed1[50], ed2[50];

   db_lock(ua->db);
   memset(&cr, 0, sizeof(cr));
   memset(&del, 0, sizeof(del));

   bstrncpy(cr.Name, client->name(), sizeof(cr.Name));
   if (!db_create_client_record(ua->jcr, ua->db, &cr)) {
      db_unlock(ua->db);
      return 0;
   }

   period = client->JobRetention;
   now = (utime_t)time(NULL);

   /* Drop any previous temporary tables still there */
   drop_temp_tables(ua);

   /* Create temp tables and indicies */
   if (!create_temp_tables(ua)) {
      goto bail_out;
   }

   /*
    * Select all files that are older than the JobRetention period
    *  and stuff them into the "DeletionCandidates" table.
    */
   edit_uint64(now - period, ed1);
   Mmsg(query, insert_delcand, (char)JobType, ed1, 
        edit_int64(cr.ClientId, ed2));
   if (!db_sql_query(ua->db, query.c_str(), NULL, (void *)NULL)) {
      if (ua->verbose) {
         ua->error_msg("%s", db_strerror(ua->db));
      }
      Dmsg0(050, "insert delcand failed\n");
      goto bail_out;
   }

   del.max_ids = 100;
   del.JobId = (JobId_t *)malloc(sizeof(JobId_t) * del.max_ids);
   del.PurgedFiles = (char *)malloc(del.max_ids);

   /* ed1 = JobTDate */
   edit_int64(cr.ClientId, ed2);
   switch (JobType) {
   case JT_BACKUP:
      Mmsg(query, select_backup_del, ed1, ed2);
      break;
   case JT_RESTORE:
      Mmsg(query, select_restore_del, ed1, ed2);
      break;
   case JT_VERIFY:
      Mmsg(query, select_verify_del, ed1, ed2);
      break;
   case JT_ADMIN:
      Mmsg(query, select_admin_del, ed1, ed2);
      break;
   case JT_MIGRATE:
      Mmsg(query, select_migrate_del, ed1, ed2);
      break;
   }

   Dmsg1(150, "Query=%s\n", query.c_str());
   if (!db_sql_query(ua->db, query.c_str(), job_delete_handler, (void *)&del)) {
      ua->error_msg("%s", db_strerror(ua->db));
   }

   purge_job_list_from_catalog(ua, del);

   if (del.num_del > 0) {
      ua->info_msg(_("Pruned %d %s for client %s from catalog.\n"), del.num_del,
         del.num_del==1?_("Job"):_("Jobs"), client->name());
    } else if (ua->verbose) {
       ua->info_msg(_("No Jobs found to prune.\n"));
    }

bail_out:
   drop_temp_tables(ua);
   db_unlock(ua->db);
   if (del.JobId) {
      free(del.JobId);
   }
   if (del.PurgedFiles) {
      free(del.PurgedFiles);
   }
   return 1;
}

/*
 * Prune a given Volume
 */
bool prune_volume(UAContext *ua, MEDIA_DBR *mr)
{
   POOL_MEM query(PM_MESSAGE);
   struct del_ctx del;
   bool ok = false;
   int count;

   if (mr->Enabled == 2) {
      return false;                   /* Cannot prune archived volumes */
   }

   memset(&del, 0, sizeof(del));
   del.max_ids = 10000;
   del.JobId = (JobId_t *)malloc(sizeof(JobId_t) * del.max_ids);

   db_lock(ua->db);

   /* Prune only Volumes with status "Full", or "Used" */
   if (strcmp(mr->VolStatus, "Full")   == 0 ||
       strcmp(mr->VolStatus, "Used")   == 0) {
      Dmsg2(050, "get prune list MediaId=%d Volume %s\n", (int)mr->MediaId, mr->VolumeName);
      count = get_prune_list_for_volume(ua, mr, &del);
      Dmsg1(050, "Num pruned = %d\n", count);
      if (count != 0) {
         purge_job_list_from_catalog(ua, del);
      }
      ok = is_volume_purged(ua, mr);
   }

   db_unlock(ua->db);
   if (del.JobId) {
      free(del.JobId);
   }
   return ok;
}

/*
 * Get prune list for a volume
 */
int get_prune_list_for_volume(UAContext *ua, MEDIA_DBR *mr, del_ctx *del)
{
   POOL_MEM query(PM_MESSAGE);
   int count = 0;
   int i;          
   utime_t now, period;
   char ed1[50], ed2[50];

   if (mr->Enabled == 2) {
      return 0;                    /* cannot prune Archived volumes */
   }

   db_lock(ua->db);

   /*
    * Now add to the  list of JobIds for Jobs written to this Volume
    */
   edit_int64(mr->MediaId, ed1); 
   period = mr->VolRetention;
   now = (utime_t)time(NULL);
   edit_uint64(now-period, ed2);
   Mmsg(query, sel_JobMedia, ed1, ed2);
   Dmsg3(250, "Now=%d period=%d now-period=%d\n", (int)now, (int)period,
      (int)(now-period));

   Dmsg1(050, "Query=%s\n", query.c_str());
   if (!db_sql_query(ua->db, query.c_str(), file_delete_handler, (void *)del)) {
      if (ua->verbose) {
         ua->error_msg("%s", db_strerror(ua->db));
      }
      Dmsg0(050, "Count failed\n");
      goto bail_out;
   }

   for (i=0; i < del->num_ids; i++) {
      if (ua->jcr->JobId == del->JobId[i]) {
         Dmsg2(150, "skip same job JobId[%d]=%d\n", i, (int)del->JobId[i]);
         del->JobId[i] = 0;
         continue;
      }
      Dmsg2(150, "accept JobId[%d]=%d\n", i, (int)del->JobId[i]);
      count++;
   }

bail_out:
   db_unlock(ua->db);
   return count;
}
