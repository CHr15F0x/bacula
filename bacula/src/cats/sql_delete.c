/*
 * Bacula Catalog Database Delete record interface routines
 * 
 *    Kern Sibbald, December 2000
 *
 *    Version $Id$
 */

/*
   Copyright (C) 2000, 2001, 2002 Kern Sibbald and John Walker

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

/* *****FIXME**** fix fixed length of select_cmd[] and insert_cmd[] */

/* The following is necessary so that we do not include
 * the dummy external definition of DB.
 */
#define __SQL_C 		      /* indicate that this is sql.c */

#include "bacula.h"
#include "cats.h"


#if    HAVE_MYSQL || HAVE_SQLITE || HAVE_POSTGRESQL
/* -----------------------------------------------------------------------
 *
 *   Generic Routines (or almost generic)
 *
 * -----------------------------------------------------------------------
 */

/* Imported subroutines */
extern void print_dashes(B_DB *mdb);
extern void print_result(B_DB *mdb);
extern int QueryDB(char *file, int line, JCR *jcr, B_DB *db, char *select_cmd);
extern int DeleteDB(char *file, int line, JCR *jcr, B_DB *db, char *delete_cmd);
       
/*
 * Delete Pool record, must also delete all associated
 *  Media records.
 *
 *  Returns: 0 on error
 *	     1 on success
 *	     PoolId = number of Pools deleted (should be 1)
 *	     NumVols = number of Media records deleted
 */
int
db_delete_pool_record(JCR *jcr, B_DB *mdb, POOL_DBR *pr)
{
   SQL_ROW row;

   db_lock(mdb);
   Mmsg(&mdb->cmd, "SELECT PoolId FROM Pool WHERE Name='%s'", pr->Name);
   Dmsg1(10, "selectpool: %s\n", mdb->cmd);

   pr->PoolId = pr->NumVols = 0;

   if (QUERY_DB(jcr, mdb, mdb->cmd)) {

      mdb->num_rows = sql_num_rows(mdb);
   
      if (mdb->num_rows == 0) {
         Mmsg(&mdb->errmsg, _("No pool record %s exists\n"), pr->Name);
	 sql_free_result(mdb);
	 db_unlock(mdb);
	 return 0;
      } else if (mdb->num_rows != 1) {
         Mmsg(&mdb->errmsg, _("Expecting one pool record, got %d\n"), mdb->num_rows);
	 sql_free_result(mdb);
	 db_unlock(mdb);
	 return 0;
      }
      if ((row = sql_fetch_row(mdb)) == NULL) {
         Mmsg1(&mdb->errmsg, _("Error fetching row %s\n"), sql_strerror(mdb));
	 db_unlock(mdb);
	 return 0;
      }
      pr->PoolId = atoi(row[0]);
      sql_free_result(mdb);
   }

   /* Delete Media owned by this pool */
   Mmsg(&mdb->cmd,
"DELETE FROM Media WHERE Media.PoolId = %d", pr->PoolId);

   pr->NumVols = DELETE_DB(jcr, mdb, mdb->cmd);
   Dmsg1(200, "Deleted %d Media records\n", pr->NumVols);

   /* Delete Pool */
   Mmsg(&mdb->cmd,
"DELETE FROM Pool WHERE Pool.PoolId = %d", pr->PoolId);
   pr->PoolId = DELETE_DB(jcr, mdb, mdb->cmd);
   Dmsg1(200, "Deleted %d Pool records\n", pr->PoolId);

   db_unlock(mdb);
   return 1;
}

#define MAX_DEL_LIST_LEN 1000000

struct s_del_ctx {
   JobId_t *JobId; 
   int num_ids; 		      /* ids stored */
   int max_ids; 		      /* size of array */
   int num_del; 		      /* number deleted */
   int tot_ids; 		      /* total to process */
};

/*
 * Called here to make in memory list of JobIds to be
 *  deleted. The in memory list will then be transversed
 *  to issue the SQL DELETE commands.  Note, the list
 *  is allowed to get to MAX_DEL_LIST_LEN to limit the
 *  maximum malloc'ed memory.
 */
static int delete_handler(void *ctx, int num_fields, char **row)
{
   struct s_del_ctx *del = (struct s_del_ctx *)ctx;

   if (del->num_ids == MAX_DEL_LIST_LEN) {  
      return 1;
   }
   if (del->num_ids == del->max_ids) {
      del->max_ids = (del->max_ids * 3) / 2;
      del->JobId = (JobId_t *)brealloc(del->JobId, sizeof(JobId_t) *
	 del->max_ids);
   }
   del->JobId[del->num_ids++] = (JobId_t)str_to_int64(row[0]);
   return 0;
}


/* 
 * This routine will purge (delete) all records 
 * associated with a particular Volume. It will
 * not delete the media record itself.
 */
static int do_media_purge(B_DB *mdb, MEDIA_DBR *mr)
{
   POOLMEM *query = get_pool_memory(PM_MESSAGE);
   struct s_del_ctx del;
   int i;

   del.num_ids = 0;
   del.tot_ids = 0;
   del.num_del = 0;
   del.max_ids = 0;
   Mmsg(&mdb->cmd, "SELECT JobId from JobMedia WHERE MediaId=%d", mr->MediaId);
   del.max_ids = mr->VolJobs;
   if (del.max_ids < 100) {
      del.max_ids = 100;
   } else if (del.max_ids > MAX_DEL_LIST_LEN) {
      del.max_ids = MAX_DEL_LIST_LEN;
   }
   del.JobId = (JobId_t *)malloc(sizeof(JobId_t) * del.max_ids);
   db_sql_query(mdb, mdb->cmd, delete_handler, (void *)&del);

   for (i=0; i < del.num_ids; i++) {
      Dmsg1(400, "Delete JobId=%d\n", del.JobId[i]);
      Mmsg(&query, "DELETE FROM Job WHERE JobId=%u", del.JobId[i]);
      db_sql_query(mdb, query, NULL, (void *)NULL);
      Mmsg(&query, "DELETE FROM File WHERE JobId=%u", del.JobId[i]);
      db_sql_query(mdb, query, NULL, (void *)NULL);
      Mmsg(&query, "DELETE FROM JobMedia WHERE JobId=%u", del.JobId[i]);
      db_sql_query(mdb, query, NULL, (void *)NULL);
   }
   free(del.JobId);
   free_pool_memory(query);
   return 1;
}

/* Delete Media record and all records that
 * are associated with it.
 */
int db_delete_media_record(JCR *jcr, B_DB *mdb, MEDIA_DBR *mr)
{
   db_lock(mdb);
   if (mr->MediaId == 0 && !db_get_media_record(jcr, mdb, mr)) {
      db_unlock(mdb);
      return 0;
   } 
   /* Do purge if not already purged */
   if (strcmp(mr->VolStatus, "Purged") != 0) {
      /* Delete associated records */
      do_media_purge(mdb, mr);
   }

   Mmsg(&mdb->cmd, "DELETE FROM Media WHERE MediaId=%d", mr->MediaId);
   db_sql_query(mdb, mdb->cmd, NULL, (void *)NULL);
   db_unlock(mdb);
   return 1;
}

/*
 * Purge all records associated with a 
 * media record. This does not delete the
 * media record itself. But the media status
 * is changed to "Purged".
 */
int db_purge_media_record(JCR *jcr, B_DB *mdb, MEDIA_DBR *mr)
{
   db_lock(mdb);
   if (mr->MediaId == 0 && !db_get_media_record(jcr, mdb, mr)) {
      db_unlock(mdb);
      return 0;
   } 
   /* Delete associated records */
   do_media_purge(mdb, mr);	      /* Note, always purge */

   /* Mark Volume as purged */
   strcpy(mr->VolStatus, "Purged");
   if (!db_update_media_record(jcr, mdb, mr)) {
      db_unlock(mdb);
      return 0;
   }

   db_unlock(mdb);
   return 1;
}


#endif /* HAVE_MYSQL || HAVE_SQLITE || HAVE_POSTGRESQL */
