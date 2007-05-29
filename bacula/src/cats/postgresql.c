/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2003-2007 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation plus additions
   that are listed in the file LICENSE.

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
 * Bacula Catalog Database routines specific to PostgreSQL
 *   These are PostgreSQL specific routines
 *
 *    Dan Langille, December 2003
 *    based upon work done by Kern Sibbald, March 2000
 *
 *    Version $Id$
 */


/* The following is necessary so that we do not include
 * the dummy external definition of DB.
 */
#define __SQL_C                       /* indicate that this is sql.c */

#include "bacula.h"
#include "cats.h"

#ifdef HAVE_POSTGRESQL

#include "postgres_ext.h"       /* needed for NAMEDATALEN */

/* -----------------------------------------------------------------------
 *
 *   PostgreSQL dependent defines and subroutines
 *
 * -----------------------------------------------------------------------
 */

/* List of open databases */
static BQUEUE db_list = {&db_list, &db_list};

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Retrieve database type
 */
const char *
db_get_type(void)
{
   return "PostgreSQL";

}

/*
 * Initialize database data structure. In principal this should
 * never have errors, or it is really fatal.
 */
B_DB *
db_init_database(JCR *jcr, const char *db_name, const char *db_user, const char *db_password,
                 const char *db_address, int db_port, const char *db_socket,
                 int mult_db_connections)
{
   B_DB *mdb;

   if (!db_user) {
      Jmsg(jcr, M_FATAL, 0, _("A user name for PostgreSQL must be supplied.\n"));
      return NULL;
   }
   P(mutex);                          /* lock DB queue */
   if (!mult_db_connections) {
      /* Look to see if DB already open */
      for (mdb=NULL; (mdb=(B_DB *)qnext(&db_list, &mdb->bq)); ) {
         if (bstrcmp(mdb->db_name, db_name) &&
             bstrcmp(mdb->db_address, db_address) &&
             mdb->db_port == db_port) {
            Dmsg2(100, "DB REopen %d %s\n", mdb->ref_count, db_name);
            mdb->ref_count++;
            V(mutex);
            return mdb;                  /* already open */
         }
      }
   }
   Dmsg0(100, "db_open first time\n");
   mdb = (B_DB *)malloc(sizeof(B_DB));
   memset(mdb, 0, sizeof(B_DB));
   mdb->db_name = bstrdup(db_name);
   mdb->db_user = bstrdup(db_user);
   if (db_password) {
      mdb->db_password = bstrdup(db_password);
   }
   if (db_address) {
      mdb->db_address  = bstrdup(db_address);
   }
   if (db_socket) {
      mdb->db_socket   = bstrdup(db_socket);
   }
   mdb->db_port        = db_port;
   mdb->have_insert_id = TRUE;
   mdb->errmsg         = get_pool_memory(PM_EMSG); /* get error message buffer */
   *mdb->errmsg        = 0;
   mdb->cmd            = get_pool_memory(PM_EMSG); /* get command buffer */
   mdb->cached_path    = get_pool_memory(PM_FNAME);
   mdb->cached_path_id = 0;
   mdb->ref_count      = 1;
   mdb->fname          = get_pool_memory(PM_FNAME);
   mdb->path           = get_pool_memory(PM_FNAME);
   mdb->esc_name       = get_pool_memory(PM_FNAME);
   mdb->esc_path      = get_pool_memory(PM_FNAME);
   mdb->allow_transactions = mult_db_connections;
   qinsert(&db_list, &mdb->bq);            /* put db in list */
   V(mutex);
   return mdb;
}

/*
 * Now actually open the database.  This can generate errors,
 *   which are returned in the errmsg
 *
 * DO NOT close the database or free(mdb) here !!!!
 */
int
db_open_database(JCR *jcr, B_DB *mdb)
{
   int errstat;
   char buf[10], *port;

   P(mutex);
   if (mdb->connected) {
      V(mutex);
      return 1;
   }
   mdb->connected = false;

   if ((errstat=rwl_init(&mdb->lock)) != 0) {
      Mmsg1(&mdb->errmsg, _("Unable to initialize DB lock. ERR=%s\n"),
            strerror(errstat));
      V(mutex);
      return 0;
   }

   if (mdb->db_port) {
      bsnprintf(buf, sizeof(buf), "%d", mdb->db_port);
      port = buf;
   } else {
      port = NULL;
   }

   /* If connection fails, try at 5 sec intervals for 30 seconds. */
   for (int retry=0; retry < 6; retry++) {
      /* connect to the database */
      mdb->db = PQsetdbLogin(
           mdb->db_address,           /* default = localhost */
           port,                      /* default port */
           NULL,                      /* pg options */
           NULL,                      /* tty, ignored */
           mdb->db_name,              /* database name */
           mdb->db_user,              /* login name */
           mdb->db_password);         /* password */

      /* If no connect, try once more in case it is a timing problem */
      if (PQstatus(mdb->db) == CONNECTION_OK) {
         break;
      }
      bmicrosleep(5, 0);
   }

   Dmsg0(50, "pg_real_connect done\n");
   Dmsg3(50, "db_user=%s db_name=%s db_password=%s\n", mdb->db_user, mdb->db_name,
            mdb->db_password==NULL?"(NULL)":mdb->db_password);

   if (PQstatus(mdb->db) != CONNECTION_OK) {
      Mmsg2(&mdb->errmsg, _("Unable to connect to PostgreSQL server.\n"
            "Database=%s User=%s\n"
            "It is probably not running or your password is incorrect.\n"),
             mdb->db_name, mdb->db_user);
      V(mutex);
      return 0;
   }

   mdb->connected = true;

   if (!check_tables_version(jcr, mdb)) {
      V(mutex);
      return 0;
   }

   sql_query(mdb, "SET datestyle TO 'ISO, YMD'");

   V(mutex);
   return 1;
}

void
db_close_database(JCR *jcr, B_DB *mdb)
{
   if (!mdb) {
      return;
   }
   db_end_transaction(jcr, mdb);
   P(mutex);
   sql_free_result(mdb);
   mdb->ref_count--;
   if (mdb->ref_count == 0) {
      qdchain(&mdb->bq);
      if (mdb->connected && mdb->db) {
         sql_close(mdb);
      }
      rwl_destroy(&mdb->lock);
      free_pool_memory(mdb->errmsg);
      free_pool_memory(mdb->cmd);
      free_pool_memory(mdb->cached_path);
      free_pool_memory(mdb->fname);
      free_pool_memory(mdb->path);
      free_pool_memory(mdb->esc_name);
      free_pool_memory(mdb->esc_path);
      if (mdb->db_name) {
         free(mdb->db_name);
      }
      if (mdb->db_user) {
         free(mdb->db_user);
      }
      if (mdb->db_password) {
         free(mdb->db_password);
      }
      if (mdb->db_address) {
         free(mdb->db_address);
      }
      if (mdb->db_socket) {
         free(mdb->db_socket);
      }
      my_postgresql_free_result(mdb);
      free(mdb);
   }
   V(mutex);
}

void db_thread_cleanup()
{ }

/*
 * Return the next unique index (auto-increment) for
 * the given table.  Return NULL on error.
 *
 * For PostgreSQL, NULL causes the auto-increment value
 *  to be updated.
 */
int db_next_index(JCR *jcr, B_DB *mdb, char *table, char *index)
{
   strcpy(index, "NULL");
   return 1;
}


/*
 * Escape strings so that PostgreSQL is happy
 *
 *   NOTE! len is the length of the old string. Your new
 *         string must be long enough (max 2*old+1) to hold
 *         the escaped output.
 */
void
db_escape_string(char *snew, char *old, int len)
{
   PQescapeString(snew, old, len);
}

/*
 * Submit a general SQL command (cmd), and for each row returned,
 *  the sqlite_handler is called with the ctx.
 */
int db_sql_query(B_DB *mdb, const char *query, DB_RESULT_HANDLER *result_handler, void *ctx)
{
   SQL_ROW row;

   Dmsg0(500, "db_sql_query started\n");

   db_lock(mdb);
   if (sql_query(mdb, query) != 0) {
      Mmsg(mdb->errmsg, _("Query failed: %s: ERR=%s\n"), query, sql_strerror(mdb));
      db_unlock(mdb);
      Dmsg0(500, "db_sql_query failed\n");
      return 0;
   }
   Dmsg0(500, "db_sql_query succeeded. checking handler\n");

   if (result_handler != NULL) {
      Dmsg0(500, "db_sql_query invoking handler\n");
      if ((mdb->result = sql_store_result(mdb)) != NULL) {
         int num_fields = sql_num_fields(mdb);

         Dmsg0(500, "db_sql_query sql_store_result suceeded\n");
         while ((row = sql_fetch_row(mdb)) != NULL) {

            Dmsg0(500, "db_sql_query sql_fetch_row worked\n");
            if (result_handler(ctx, num_fields, row))
               break;
         }

        sql_free_result(mdb);
      }
   }
   db_unlock(mdb);

   Dmsg0(500, "db_sql_query finished\n");

   return 1;
}



POSTGRESQL_ROW my_postgresql_fetch_row(B_DB *mdb)
{
   int j;
   POSTGRESQL_ROW row = NULL; // by default, return NULL

   Dmsg0(500, "my_postgresql_fetch_row start\n");

   if (mdb->row_number == -1 || mdb->row == NULL) {
      Dmsg1(500, "we have need space of %d bytes\n", sizeof(char *) * mdb->num_fields);

      if (mdb->row != NULL) {
         Dmsg0(500, "my_postgresql_fetch_row freeing space\n");
         free(mdb->row);
         mdb->row = NULL;
      }

      mdb->row = (POSTGRESQL_ROW) malloc(sizeof(char *) * mdb->num_fields);

      // now reset the row_number now that we have the space allocated
      mdb->row_number = 0;
   }

   // if still within the result set
   if (mdb->row_number < mdb->num_rows) {
      Dmsg2(500, "my_postgresql_fetch_row row number '%d' is acceptable (0..%d)\n", mdb->row_number, mdb->num_rows);
      // get each value from this row
      for (j = 0; j < mdb->num_fields; j++) {
         mdb->row[j] = PQgetvalue(mdb->result, mdb->row_number, j);
         Dmsg2(500, "my_postgresql_fetch_row field '%d' has value '%s'\n", j, mdb->row[j]);
      }
      // increment the row number for the next call
      mdb->row_number++;

      row = mdb->row;
   } else {
      Dmsg2(500, "my_postgresql_fetch_row row number '%d' is NOT acceptable (0..%d)\n", mdb->row_number, mdb->num_rows);
   }

   Dmsg1(500, "my_postgresql_fetch_row finishes returning %x\n", row);

   return row;
}

int my_postgresql_max_length(B_DB *mdb, int field_num) {
   //
   // for a given column, find the max length
   //
   int max_length;
   int i;
   int this_length;

   max_length = 0;
   for (i = 0; i < mdb->num_rows; i++) {
      if (PQgetisnull(mdb->result, i, field_num)) {
          this_length = 4;        // "NULL"
      } else {
          this_length = cstrlen(PQgetvalue(mdb->result, i, field_num));
      }

      if (max_length < this_length) {
          max_length = this_length;
      }
   }

   return max_length;
}

POSTGRESQL_FIELD * my_postgresql_fetch_field(B_DB *mdb)
{
   int     i;

   Dmsg0(500, "my_postgresql_fetch_field starts\n");
   if (mdb->fields == NULL) {
      Dmsg1(500, "allocating space for %d fields\n", mdb->num_fields);
      mdb->fields = (POSTGRESQL_FIELD *)malloc(sizeof(POSTGRESQL_FIELD) * mdb->num_fields);

      for (i = 0; i < mdb->num_fields; i++) {
         Dmsg1(500, "filling field %d\n", i);
         mdb->fields[i].name           = PQfname(mdb->result, i);
         mdb->fields[i].max_length = my_postgresql_max_length(mdb, i);
         mdb->fields[i].type       = PQftype(mdb->result, i);
         mdb->fields[i].flags      = 0;

         Dmsg4(500, "my_postgresql_fetch_field finds field '%s' has length='%d' type='%d' and IsNull=%d\n",
            mdb->fields[i].name, mdb->fields[i].max_length, mdb->fields[i].type,
            mdb->fields[i].flags);
      } // end for
   } // end if

   // increment field number for the next time around

   Dmsg0(500, "my_postgresql_fetch_field finishes\n");
   return &mdb->fields[mdb->field_number++];
}

void my_postgresql_data_seek(B_DB *mdb, int row)
{
   // set the row number to be returned on the next call
   // to my_postgresql_fetch_row
   mdb->row_number = row;
}

void my_postgresql_field_seek(B_DB *mdb, int field)
{
   mdb->field_number = field;
}

/*
 * Note, if this routine returns 1 (failure), Bacula expects
 *  that no result has been stored.
 */
int my_postgresql_query(B_DB *mdb, const char *query) {
   Dmsg0(500, "my_postgresql_query started\n");
   // We are starting a new query.  reset everything.
   mdb->num_rows     = -1;
   mdb->row_number   = -1;
   mdb->field_number = -1;

   if (mdb->result) {
      PQclear(mdb->result);  /* hmm, someone forgot to free?? */
      mdb->result = NULL;
   }

   Dmsg1(500, "my_postgresql_query starts with '%s'\n", query);
   mdb->result = PQexec(mdb->db, query);
   mdb->status = PQresultStatus(mdb->result);
   if (mdb->status == PGRES_TUPLES_OK || mdb->status == PGRES_COMMAND_OK) {
      Dmsg1(500, "we have a result\n", query);

      // how many fields in the set?
      mdb->num_fields = (int) PQnfields(mdb->result);
      Dmsg1(500, "we have %d fields\n", mdb->num_fields);

      mdb->num_rows = PQntuples(mdb->result);
      Dmsg1(500, "we have %d rows\n", mdb->num_rows);

      mdb->status = 0;
   } else {
      Dmsg1(500, "we failed\n", query);
      mdb->status = 1;
   }

   Dmsg0(500, "my_postgresql_query finishing\n");

   return mdb->status;
}

void my_postgresql_free_result(B_DB *mdb)
{
   if (mdb->result) {
      PQclear(mdb->result);
      mdb->result = NULL;
   }

   if (mdb->row) {
      free(mdb->row);
      mdb->row = NULL;
   }

   if (mdb->fields) {
      free(mdb->fields);
      mdb->fields = NULL;
   }
}

int my_postgresql_currval(B_DB *mdb, char *table_name)
{
   // Obtain the current value of the sequence that
   // provides the serial value for primary key of the table.

   // currval is local to our session.  It is not affected by
   // other transactions.

   // Determine the name of the sequence.
   // PostgreSQL automatically creates a sequence using
   // <table>_<column>_seq.
   // At the time of writing, all tables used this format for
   // for their primary key: <table>id
   // Except for basefiles which has a primary key on baseid.
   // Therefore, we need to special case that one table.

   // everything else can use the PostgreSQL formula.

   char      sequence[NAMEDATALEN-1];
   char      query   [NAMEDATALEN+50];
   PGresult *result;
   int       id = 0;

   if (strcasecmp(table_name, "basefiles") == 0) {
      bstrncpy(sequence, "basefiles_baseid", sizeof(sequence));
   } else {
      bstrncpy(sequence, table_name, sizeof(sequence));
      bstrncat(sequence, "_",        sizeof(sequence));
      bstrncat(sequence, table_name, sizeof(sequence));
      bstrncat(sequence, "id",       sizeof(sequence));
   }

   bstrncat(sequence, "_seq", sizeof(sequence));
   bsnprintf(query, sizeof(query), "SELECT currval('%s')", sequence);

// Mmsg(query, "SELECT currval('%s')", sequence);
   Dmsg1(500, "my_postgresql_currval invoked with '%s'\n", query);
   result = PQexec(mdb->db, query);

   Dmsg0(500, "exec done");

   if (PQresultStatus(result) == PGRES_TUPLES_OK) {
      Dmsg0(500, "getting value");
      id = atoi(PQgetvalue(result, 0, 0));
      Dmsg2(500, "got value '%s' which became %d\n", PQgetvalue(result, 0, 0), id);
   } else {
      Mmsg1(&mdb->errmsg, _("error fetching currval: %s\n"), PQerrorMessage(mdb->db));
   }

   PQclear(result);

   return id;
}

int my_postgresql_batch_start(JCR *jcr, B_DB *mdb)
{
   Dmsg0(500, "my_postgresql_batch_start started\n");

   if (my_postgresql_query(mdb,
                           " CREATE TEMPORARY TABLE batch "
                           "        (fileindex int,       "
                           "        jobid int,            "
                           "        path varchar,         "
                           "        name varchar,         "
                           "        lstat varchar,        "
                           "        md5 varchar)") == 1)
   {
      Dmsg0(500, "my_postgresql_batch_start failed\n");
      return 1;
   }
   
   // We are starting a new query.  reset everything.
   mdb->num_rows     = -1;
   mdb->row_number   = -1;
   mdb->field_number = -1;

   if (mdb->result != NULL) {
      my_postgresql_free_result(mdb);
   }

   mdb->result = PQexec(mdb->db, "COPY batch FROM STDIN");
   mdb->status = PQresultStatus(mdb->result);
   if (mdb->status == PGRES_COPY_IN) {
      // how many fields in the set?
      mdb->num_fields = (int) PQnfields(mdb->result);
      mdb->num_rows   = 0;
      mdb->status = 1;
   } else {
      Dmsg0(500, "we failed\n");
      mdb->status = 0;
   }

   Dmsg0(500, "my_postgresql_batch_start finishing\n");

   return mdb->status;
}

/* set error to something to abort operation */
int my_postgresql_batch_end(JCR *jcr, B_DB *mdb, const char *error)
{
   int res;
   int count=30;
   Dmsg0(500, "my_postgresql_batch_end started\n");

   if (!mdb) {                  /* no files ? */
      return 0;
   }

   do { 
      res = PQputCopyEnd(mdb->db, error);
   } while (res == 0 && --count > 0);

   if (res == 1) {
      Dmsg0(500, "ok\n");
      mdb->status = 1;
   }
   
   if (res <= 0) {
      Dmsg0(500, "we failed\n");
      mdb->status = 0;
      Mmsg1(&mdb->errmsg, _("error ending batch mode: %s\n"), PQerrorMessage(mdb->db));
   }
   
   Dmsg0(500, "my_postgresql_batch_end finishing\n");

   return mdb->status;
}

int my_postgresql_batch_insert(JCR *jcr, B_DB *mdb, ATTR_DBR *ar)
{
   int res;
   int count=30;
   size_t len;
   char *digest;
   char ed1[50];

   mdb->esc_name = check_pool_memory_size(mdb->esc_name, mdb->fnl*2+1);
   my_postgresql_copy_escape(mdb->esc_name, mdb->fname, mdb->fnl);

   mdb->esc_path = check_pool_memory_size(mdb->esc_path, mdb->pnl*2+1);
   my_postgresql_copy_escape(mdb->esc_path, mdb->path, mdb->pnl);

   if (ar->Digest == NULL || ar->Digest[0] == 0) {
      digest = "0";
   } else {
      digest = ar->Digest;
   }

   len = Mmsg(mdb->cmd, "%u\t%s\t%s\t%s\t%s\t%s\n", 
              ar->FileIndex, edit_int64(ar->JobId, ed1), mdb->esc_path, 
              mdb->esc_name, ar->attr, digest);

   do { 
      res = PQputCopyData(mdb->db,
                          mdb->cmd,
                          len);
   } while (res == 0 && --count > 0);

   if (res == 1) {
      Dmsg0(500, "ok\n");
      mdb->changes++;
      mdb->status = 1;
   }

   if (res <= 0) {
      Dmsg0(500, "we failed\n");
      mdb->status = 0;
      Mmsg1(&mdb->errmsg, _("error ending batch mode: %s\n"), PQerrorMessage(mdb->db));
   }

   Dmsg0(500, "my_postgresql_batch_insert finishing\n");

   return mdb->status;
}

/*
 * Escape strings so that PostgreSQL is happy on COPY
 *
 *   NOTE! len is the length of the old string. Your new
 *         string must be long enough (max 2*old+1) to hold
 *         the escaped output.
 */
char *my_postgresql_copy_escape(char *dest, char *src, size_t len)
{
   /* we have to escape \t, \n, \r, \ */
   char c = '\0' ;

   while (len > 0 && *src) {
      switch (*src) {
      case '\n':
         c = 'n';
         break;
      case '\\':
         c = '\\';
         break;
      case '\t':
         c = 't';
         break;
      case '\r':
         c = 'r';
         break;
      default:
         c = '\0' ;
      }

      if (c) {
         *dest = '\\';
         dest++;
         *dest = c;
      } else {
         *dest = *src;
      }

      len--;
      src++;
      dest++;
   }

   *dest = '\0';
   return dest;
}

char *my_pg_batch_lock_path_query = "BEGIN; LOCK TABLE Path IN SHARE ROW EXCLUSIVE MODE";


char *my_pg_batch_lock_filename_query = "BEGIN; LOCK TABLE Filename IN SHARE ROW EXCLUSIVE MODE";

char *my_pg_batch_unlock_tables_query = "COMMIT";

char *my_pg_batch_fill_path_query = "INSERT INTO Path (Path)                                    "
                                    "  SELECT a.Path FROM                                       "
                                    "      (SELECT DISTINCT Path FROM batch) AS a               "
                                    "  WHERE NOT EXISTS (SELECT Path FROM Path WHERE Path = a.Path) ";


char *my_pg_batch_fill_filename_query = "INSERT INTO Filename (Name)        "
                                        "  SELECT a.Name FROM               "
                                        "    (SELECT DISTINCT Name FROM batch) as a "
                                        "    WHERE NOT EXISTS               "
                                        "      (SELECT Name FROM Filename WHERE Name = a.Name)";
#endif /* HAVE_POSTGRESQL */
