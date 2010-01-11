/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2009-2009 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation, which is 
   listed in the file LICENSE.

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

#define __SQL_C                       /* indicate that this is sql.c */

#include "bacula.h"
#include "cats/cats.h"
#include "lib/htable.h"
#include "bvfs.h"

#define dbglevel 10
#define dbglevel_sql 15

static int result_handler(void *ctx, int fields, char **row)
{
   if (fields == 4) {
      Pmsg4(0, "%s\t%s\t%s\t%s\n", 
            row[0], row[1], row[2], row[3]);
   } else if (fields == 5) {
      Pmsg5(0, "%s\t%s\t%s\t%s\t%s\n", 
            row[0], row[1], row[2], row[3], row[4]);
   } else if (fields == 6) {
      Pmsg6(0, "%s\t%s\t%s\t%s\t%s\t%s\n", 
            row[0], row[1], row[2], row[3], row[4], row[5]);
   } else if (fields == 7) {
      Pmsg7(0, "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", 
            row[0], row[1], row[2], row[3], row[4], row[5], row[6]);
   }
   return 0;
}

Bvfs::Bvfs(JCR *j, B_DB *mdb) {
   jcr = j;
   jcr->inc_use_count();
   db = mdb;                 /* need to inc ref count */
   jobids = get_pool_memory(PM_NAME);
   prev_dir = get_pool_memory(PM_NAME);
   pattern = get_pool_memory(PM_NAME);
   *jobids = *prev_dir = *pattern = 0;
   dir_filenameid = pwd_id = offset = 0;
   see_copies = see_all_version = false;
   limit = 1000;
   attr = new_attr(jcr);
   list_entries = result_handler;
   user_data = this;
}

Bvfs::~Bvfs() {
   free_pool_memory(jobids);
   free_pool_memory(pattern);
   free_pool_memory(prev_dir);
   free_attr(attr);
   jcr->dec_use_count();
}

/* 
 * TODO: Find a way to let the user choose how he wants to display
 * files and directories
 */


/* 
 * Working Object to store PathId already seen (avoid
 * database queries), equivalent to %cache_ppathid in perl
 */

#define NITEMS 50000
class pathid_cache {
private:
   hlink *nodes;
   int nb_node;
   int max_node;
   htable *cache_ppathid;

public:
   pathid_cache() {
      hlink link;
      cache_ppathid = (htable *)malloc(sizeof(htable));
      cache_ppathid->init(&link, &link, NITEMS);
      max_node = NITEMS;
      nodes = (hlink *) malloc(max_node * sizeof (hlink));
      nb_node = 0;
   }

   hlink *get_hlink() {
      if (nb_node >= max_node) {
         max_node *= 2;
         nodes = (hlink *)brealloc(nodes, sizeof(hlink) * max_node);
      }
      return nodes + nb_node++;
   }

   bool lookup(char *pathid) {
      bool ret = cache_ppathid->lookup(pathid) != NULL;
      return ret;
   }
   
   void insert(char *pathid) {
      hlink *h = get_hlink();
      cache_ppathid->insert(pathid, h);
   }

   ~pathid_cache() {
      cache_ppathid->destroy();
      free(cache_ppathid);
      free(nodes);
   }
private:
   pathid_cache(const pathid_cache &); /* prohibit pass by value */
   pathid_cache &operator= (const pathid_cache &);/* prohibit class assignment*/
} ;

/* Return the parent_dir with the trailing /  (update the given string)
 * TODO: see in the rest of bacula if we don't have already this function
 * dir=/tmp/toto/
 * dir=/tmp/
 * dir=/
 * dir=
 */
char *bvfs_parent_dir(char *path)
{
   char *p = path;
   int len = strlen(path) - 1;

   if (len >= 0 && path[len] == '/') {      /* if directory, skip last / */
      path[len] = '\0';
   }

   if (len > 0) {
      p += len;
      while (p > path && !IsPathSeparator(*p)) {
         p--;
      }
      p[1] = '\0';
   }
   return path;
}

/* Return the basename of the with the trailing /
 * TODO: see in the rest of bacula if we don't have
 * this function already
 */
char *bvfs_basename_dir(char *path)
{
   char *p = path;
   int len = strlen(path) - 1;

   if (path[len] == '/') {      /* if directory, skip last / */
      len -= 1;
   }

   if (len > 0) {
      p += len;
      while (p > path && !IsPathSeparator(*p)) {
         p--;
      }
      if (*p == '/') {
         p++;                  /* skip first / */
      }
   } 
   return p;
}

static void build_path_hierarchy(JCR *jcr, B_DB *mdb, 
                                 pathid_cache &ppathid_cache, 
                                 char *org_pathid, char *path)
{
   Dmsg1(dbglevel, "build_path_hierarchy(%s)\n", path);
   char pathid[50];
   ATTR_DBR parent;
   char *bkp = mdb->path;
   strncpy(pathid, org_pathid, sizeof(pathid));

   /* Does the ppathid exist for this ? we use a memory cache...  In order to
    * avoid the full loop, we consider that if a dir is allready in the
    * PathHierarchy table, then there is no need to calculate all the
    * hierarchy
    */
   while (path && *path)
   {
      if (!ppathid_cache.lookup(pathid))
      {
         Mmsg(mdb->cmd, 
              "SELECT PPathId FROM PathHierarchy WHERE PathId = %s",
              pathid);

         QUERY_DB(jcr, mdb, mdb->cmd);
         /* Do we have a result ? */
         if (sql_num_rows(mdb) > 0) {
            ppathid_cache.insert(pathid);
            /* This dir was in the db ...
             * It means we can leave, the tree has allready been built for
             * this dir
             */
            goto bail_out;
         } else {
            /* search or create parent PathId in Path table */
            mdb->path = bvfs_parent_dir(path);
            mdb->pnl = strlen(mdb->path);
            if (!db_create_path_record(jcr, mdb, &parent)) {
               goto bail_out;
            }
            ppathid_cache.insert(pathid);
            
            Mmsg(mdb->cmd,
                 "INSERT INTO PathHierarchy (PathId, PPathId) "
                 "VALUES (%s,%lld)",
                 pathid, (uint64_t) parent.PathId);
            
            INSERT_DB(jcr, mdb, mdb->cmd);

            edit_uint64(parent.PathId, pathid);
            path = mdb->path;   /* already done */
         }
      } else {
         /* It's already in the cache.  We can leave, no time to waste here,
          * all the parent dirs have allready been done
          */
         goto bail_out;
      }
   }   

bail_out:
   mdb->path = bkp;
   mdb->fnl = 0;
}

/* 
 * Internal function to update path_hierarchy cache with a shared pathid cache
 */
static void update_path_hierarchy_cache(JCR *jcr,
                                        B_DB *mdb,
                                        pathid_cache &ppathid_cache,
                                        JobId_t JobId)
{
   Dmsg0(dbglevel, "update_path_hierarchy_cache()\n");

   uint32_t num;
   char jobid[50];
   edit_uint64(JobId, jobid);
 
   db_lock(mdb);
   db_start_transaction(jcr, mdb);

   Mmsg(mdb->cmd, "SELECT 1 FROM Job WHERE JobId = %s AND HasCache=1", jobid);
   
   if (!QUERY_DB(jcr, mdb, mdb->cmd) || sql_num_rows(mdb) > 0) {
      Dmsg1(dbglevel, "already computed %d\n", (uint32_t)JobId );
      goto bail_out;
   }

   /* Inserting path records for JobId */
   Mmsg(mdb->cmd, "INSERT INTO PathVisibility (PathId, JobId) "
                  "SELECT DISTINCT PathId, JobId FROM File WHERE JobId = %s",
        jobid);
   QUERY_DB(jcr, mdb, mdb->cmd);


   /* Now we have to do the directory recursion stuff to determine missing
    * visibility We try to avoid recursion, to be as fast as possible We also
    * only work on not allready hierarchised directories...
    */
   Mmsg(mdb->cmd, 
     "SELECT PathVisibility.PathId, Path "
       "FROM PathVisibility "
            "JOIN Path ON( PathVisibility.PathId = Path.PathId) "
            "LEFT JOIN PathHierarchy "
         "ON (PathVisibility.PathId = PathHierarchy.PathId) "
      "WHERE PathVisibility.JobId = %s "
        "AND PathHierarchy.PathId IS NULL "
      "ORDER BY Path", jobid);
   Dmsg1(dbglevel_sql, "q=%s\n", mdb->cmd);
   QUERY_DB(jcr, mdb, mdb->cmd);

   /* TODO: I need to reuse the DB connection without emptying the result 
    * So, now i'm copying the result in memory to be able to query the
    * catalog descriptor again.
    */
   num = sql_num_rows(mdb);
   if (num > 0) {
      char **result = (char **)malloc (num * 2 * sizeof(char *));
      
      SQL_ROW row;
      int i=0;
      while((row = sql_fetch_row(mdb))) {
         result[i++] = bstrdup(row[0]);
         result[i++] = bstrdup(row[1]);
      }
      
      i=0;
      while (num > 0) {
         build_path_hierarchy(jcr, mdb, ppathid_cache, result[i], result[i+1]);
         free(result[i++]);
         free(result[i++]);
         num--;
      }
      free(result);
   }
   
   Mmsg(mdb->cmd, 
  "INSERT INTO PathVisibility (PathId, JobId)  "
   "SELECT a.PathId,%s "
   "FROM ( "
     "SELECT DISTINCT h.PPathId AS PathId "
       "FROM PathHierarchy AS h "
       "JOIN  PathVisibility AS p ON (h.PathId=p.PathId) "
      "WHERE p.JobId=%s) AS a LEFT JOIN "
       "(SELECT PathId "
          "FROM PathVisibility "
         "WHERE JobId=%s) AS b ON (a.PathId = b.PathId) "
   "WHERE b.PathId IS NULL",  jobid, jobid, jobid);

   do {
      QUERY_DB(jcr, mdb, mdb->cmd);
   } while (sql_affected_rows(mdb) > 0);
   
   Mmsg(mdb->cmd, "UPDATE Job SET HasCache=1 WHERE JobId=%s", jobid);
   UPDATE_DB(jcr, mdb, mdb->cmd);

bail_out:
   db_end_transaction(jcr, mdb);
   db_unlock(mdb);
}

/* 
 * Find an store the filename descriptor for empty directories Filename.Name=''
 */
DBId_t Bvfs::get_dir_filenameid()
{
   uint32_t id;
   if (dir_filenameid) {
      return dir_filenameid;
   }
   POOL_MEM q;
   Mmsg(q, "SELECT FilenameId FROM Filename WHERE Name = ''");
   db_sql_query(db, q.c_str(), db_int_handler, &id);
   dir_filenameid = id;
   return dir_filenameid;
}

void bvfs_update_cache(JCR *jcr, B_DB *mdb)
{
   uint32_t nb=0;
   db_list_ctx jobids_list;

   db_lock(mdb);
   db_start_transaction(jcr, mdb);

#ifdef xxx
   /* TODO: Remove this code when updating make_bacula_table script */
   Mmsg(mdb->cmd, "SELECT 1 FROM Job WHERE HasCache<>2 LIMIT 1");
   if (!QUERY_DB(jcr, mdb, mdb->cmd)) {
      Dmsg0(dbglevel, "Creating cache table\n");
      Mmsg(mdb->cmd, "ALTER TABLE Job ADD HasCache int DEFAULT 0");
      QUERY_DB(jcr, mdb, mdb->cmd);

      Mmsg(mdb->cmd,
           "CREATE TABLE PathHierarchy ( "
           "PathId integer NOT NULL, "
           "PPathId integer NOT NULL, "
           "CONSTRAINT pathhierarchy_pkey "
           "PRIMARY KEY (PathId))");
      QUERY_DB(jcr, mdb, mdb->cmd); 

      Mmsg(mdb->cmd,
           "CREATE INDEX pathhierarchy_ppathid "
           "ON PathHierarchy (PPathId)");
      QUERY_DB(jcr, mdb, mdb->cmd);

      Mmsg(mdb->cmd, 
           "CREATE TABLE PathVisibility ("
           "PathId integer NOT NULL, "
           "JobId integer NOT NULL, "
           "Size int8 DEFAULT 0, "
           "Files int4 DEFAULT 0, "
           "CONSTRAINT pathvisibility_pkey "
           "PRIMARY KEY (JobId, PathId))");
      QUERY_DB(jcr, mdb, mdb->cmd);

      Mmsg(mdb->cmd, 
           "CREATE INDEX pathvisibility_jobid "
           "ON PathVisibility (JobId)");
      QUERY_DB(jcr, mdb, mdb->cmd);

   }
#endif

   Mmsg(mdb->cmd, 
 "SELECT JobId from Job "
  "WHERE HashCache = 0 "
    "AND Type IN ('B') AND JobStatus IN ('T', 'f', 'A') "
  "ORDER BY JobId");

   db_sql_query(mdb, mdb->cmd, db_list_handler, &jobids_list);

   bvfs_update_path_hierarchy_cache(jcr, mdb, jobids_list.list);

   db_end_transaction(jcr, mdb);
   db_start_transaction(jcr, mdb);
   Dmsg0(dbglevel, "Cleaning pathvisibility\n");
   Mmsg(mdb->cmd, 
        "DELETE FROM PathVisibility "
         "WHERE NOT EXISTS "
        "(SELECT 1 FROM Job WHERE JobId=PathVisibility.JobId)");
   nb = DELETE_DB(jcr, mdb, mdb->cmd);
   Dmsg1(dbglevel, "Affected row(s) = %d\n", nb);

   db_end_transaction(jcr, mdb);
}

/*
 * Update the bvfs cache for given jobids (1,2,3,4)
 */
void
bvfs_update_path_hierarchy_cache(JCR *jcr, B_DB *mdb, char *jobids)
{
   pathid_cache ppathid_cache;
   JobId_t JobId;
   char *p;

   for (p=jobids; ; ) {
      int stat = get_next_jobid_from_list(&p, &JobId);
      if (stat < 0) {
         return;
      }
      if (stat == 0) {
         break;
      }
      Dmsg1(dbglevel, "Updating cache for %lld\n", (uint64_t)JobId);
      update_path_hierarchy_cache(jcr, mdb, ppathid_cache, JobId);
   }
}

/* 
 * Update the bvfs cache for current jobids
 */
void Bvfs::update_cache()
{
   bvfs_update_path_hierarchy_cache(jcr, db, jobids);
}

/* Change the current directory, returns true if the path exists */
bool Bvfs::ch_dir(const char *path)
{
   pm_strcpy(db->path, path);
   db->pnl = strlen(db->path);
   ch_dir(db_get_path_record(jcr, db)); 
   return pwd_id != 0;
}

/* 
 * Get all file versions for a specified client
 */
void Bvfs::get_all_file_versions(DBId_t pathid, DBId_t fnid, const char *client)
{
   Dmsg3(dbglevel, "get_all_file_versions(%lld, %lld, %s)\n", (uint64_t)pathid,
         (uint64_t)fnid, client);
   char ed1[50], ed2[50];
   POOL_MEM q;
   if (see_copies) {
      Mmsg(q, " AND Job.Type IN ('C', 'B') ");
   } else {
      Mmsg(q, " AND Job.Type = 'B' ");
   }

   POOL_MEM query;

   Mmsg(query,//    1           2          3       4
"SELECT 'V', File.FileId, File.Md5, File.JobId, File.LStat, "
//         5                6
       "Media.VolumeName, Media.InChanger "
"FROM File, Job, Client, JobMedia, Media "
"WHERE File.FilenameId = %s "
  "AND File.PathId=%s "
  "AND File.JobId = Job.JobId "
  "AND Job.ClientId = Client.ClientId "
  "AND Job.JobId = JobMedia.JobId "
  "AND File.FileIndex >= JobMedia.FirstIndex "
  "AND File.FileIndex <= JobMedia.LastIndex "
  "AND JobMedia.MediaId = Media.MediaId "
  "AND Client.Name = '%s' "
  "%s ORDER BY FileId LIMIT %d OFFSET %d"
        ,edit_uint64(fnid, ed1), edit_uint64(pathid, ed2), client, q.c_str(),
        limit, offset);
   Dmsg1(dbglevel_sql, "q=%s\n", query.c_str());
   db_sql_query(db, query.c_str(), list_entries, user_data);
}

DBId_t Bvfs::get_root()
{
   *db->path = 0;
   return db_get_path_record(jcr, db);
}

static int path_handler(void *ctx, int fields, char **row)
{
   Bvfs *fs = (Bvfs *) ctx;
   return fs->_handle_path(ctx, fields, row);
}

int Bvfs::_handle_path(void *ctx, int fields, char **row)
{
   if (bvfs_is_dir(row)) {
      /* can have the same path 2 times */
      if (strcmp(row[BVFS_Name], prev_dir)) {
         pm_strcpy(prev_dir, row[BVFS_Name]);
         return list_entries(user_data, fields, row);
      }
   }
   return 0;
}

/* 
 * Retrieve . and .. information
 */
void Bvfs::ls_special_dirs()
{
   Dmsg1(dbglevel, "ls_special_dirs(%lld)\n", (uint64_t)pwd_id);
   char ed1[50], ed2[50];
   if (*jobids == 0) {
      return;
   }
   if (!dir_filenameid) {
      get_dir_filenameid();
   }

   /* Will fetch directories  */
   *prev_dir = 0;

   POOL_MEM query;
   Mmsg(query, 
"((SELECT PPathId AS PathId, '..' AS Path "
    "FROM  PathHierarchy "
   "WHERE  PathId = %s) "
"UNION "
 "(SELECT %s AS PathId, '.' AS Path))",
        edit_uint64(pwd_id, ed1), ed1);

   POOL_MEM query2;
   Mmsg(query2,// 1      2     3        4     5       6
"SELECT 'D', tmp.PathId, 0, tmp.Path, JobId, LStat, FileId "
  "FROM %s AS tmp  LEFT JOIN ( " // get attributes if any
       "SELECT File1.PathId AS PathId, File1.JobId AS JobId, "
              "File1.LStat AS LStat, File1.FileId AS FileId FROM File AS File1 "
       "WHERE File1.FilenameId = %s "
       "AND File1.JobId IN (%s)) AS listfile1 "
  "ON (tmp.PathId = listfile1.PathId) "
  "ORDER BY tmp.Path, JobId DESC ",
        query.c_str(), edit_uint64(dir_filenameid, ed2), jobids);

   Dmsg1(dbglevel_sql, "q=%s\n", query2.c_str());
   db_sql_query(db, query2.c_str(), path_handler, this);
}

/* Returns true if we have dirs to read */
bool Bvfs::ls_dirs()
{
   Dmsg1(dbglevel, "ls_dirs(%lld)\n", (uint64_t)pwd_id);
   char ed1[50], ed2[50];
   if (*jobids == 0) {
      return false;
   }

   POOL_MEM filter;
   if (*pattern) {
      Mmsg(filter, " AND Path2.Path %s '%s' ", SQL_MATCH, pattern);
   }

   if (!dir_filenameid) {
      get_dir_filenameid();
   }

   /* the sql query displays same directory multiple time, take the first one */
   *prev_dir = 0;

   /* Let's retrieve the list of the visible dirs in this dir ...
    * First, I need the empty filenameid to locate efficiently
    * the dirs in the file table
    * my $dir_filenameid = $self->get_dir_filenameid();
    */
   /* Then we get all the dir entries from File ... */
   POOL_MEM query;
   Mmsg(query,
//       0     1     2   3      4     5       6
"SELECT 'D', PathId, 0, Path, JobId, LStat, FileId FROM ( "
    "SELECT Path1.PathId AS PathId, Path1.Path AS Path, "
           "lower(Path1.Path) AS lpath, "
           "listfile1.JobId AS JobId, listfile1.LStat AS LStat, "
           "listfile1.FileId AS FileId "
    "FROM ( "
      "SELECT DISTINCT PathHierarchy1.PathId AS PathId "
      "FROM PathHierarchy AS PathHierarchy1 "
      "JOIN Path AS Path2 "
        "ON (PathHierarchy1.PathId = Path2.PathId) "
      "JOIN PathVisibility AS PathVisibility1 "
        "ON (PathHierarchy1.PathId = PathVisibility1.PathId) "
      "WHERE PathHierarchy1.PPathId = %s "
      "AND PathVisibility1.jobid IN (%s) "
           "%s "
     ") AS listpath1 "
   "JOIN Path AS Path1 ON (listpath1.PathId = Path1.PathId) "

   "LEFT JOIN ( " /* get attributes if any */
       "SELECT File1.PathId AS PathId, File1.JobId AS JobId, "
              "File1.LStat AS LStat, File1.FileId AS FileId FROM File AS File1 "
       "WHERE File1.FilenameId = %s "
       "AND File1.JobId IN (%s)) AS listfile1 "
       "ON (listpath1.PathId = listfile1.PathId) "
    ") AS A ORDER BY 2,3 DESC LIMIT %d OFFSET %d",
        edit_uint64(pwd_id, ed1),
        jobids,
        filter.c_str(),
        edit_uint64(dir_filenameid, ed2),
        jobids,
        limit, offset);

   Dmsg1(dbglevel_sql, "q=%s\n", query.c_str());

   db_lock(db);
   db_sql_query(db, query.c_str(), path_handler, this);
   nb_record = db->num_rows;
   db_unlock(db);

   return nb_record == limit;
}

/* Returns true if we have files to read */
bool Bvfs::ls_files()
{
   Dmsg1(dbglevel, "ls_files(%lld)\n", (uint64_t)pwd_id);
   char ed1[50];
   if (*jobids == 0) {
      return false;
   }

   if (!pwd_id) {
      ch_dir(get_root());
   }

   POOL_MEM filter;
   if (*pattern) {
      Mmsg(filter, " AND Filename.Name %s '%s' ", SQL_MATCH, pattern);
   }

   POOL_MEM query;
   Mmsg(query, //    1              2             3          4
"SELECT 'F', File.PathId, File.FilenameId, listfiles.Name, File.JobId, "
        "File.LStat, listfiles.id "
"FROM File, ( "
       "SELECT Filename.Name as Name, max(File.FileId) as id "
         "FROM File, Filename "
        "WHERE File.FilenameId = Filename.FilenameId "
          "AND Filename.Name != '' "
          "AND File.PathId = %s "
          "AND File.JobId IN (%s) "
          "%s "
        "GROUP BY Filename.Name "
        "ORDER BY Filename.Name LIMIT %d OFFSET %d "
     ") AS listfiles "
"WHERE File.FileId = listfiles.id",
        edit_uint64(pwd_id, ed1),
        jobids,
        filter.c_str(),
        limit,
        offset);
   Dmsg1(dbglevel_sql, "q=%s\n", query.c_str());

   db_lock(db);
   db_sql_query(db, query.c_str(), list_entries, user_data);
   nb_record = db->num_rows;
   db_unlock(db);

   return nb_record == limit;
}
