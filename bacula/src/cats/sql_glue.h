/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2009-2014 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from many
   others, a complete list can be found in the file AUTHORS.

   You may use this file and others of this release according to the
   license defined in the LICENSE file, which includes the Affero General
   Public License, v3.0 ("AGPLv3") and some additional permissions and
   terms pursuant to its AGPLv3 Section 7.

   Bacula® is a registered trademark of Kern Sibbald.
*/
#ifndef __SQL_GLUE_H_
#define __SQL_GLUE_H_ 1

/*
 * Prototypes for entry points into the different backends.
 */
bool db_match_database(B_DB *mdb, const char *db_driver, const char *db_name,
                       const char *db_address, int db_port);
B_DB *db_clone_database_connection(B_DB *mdb, JCR *jcr, bool mult_db_connections);
int db_get_type_index(B_DB *mdb);
const char *db_get_type(B_DB *mdb);
B_DB *db_init_database(JCR *jcr, const char *db_driver, const char *db_name,
              const char *db_user, const char *db_password,
              const char *db_address, int db_port,
              const char *db_socket, bool mult_db_connections, bool disable_batch_insert);
bool db_open_database(JCR *jcr, B_DB *mdb);
void db_close_database(JCR *jcr, B_DB *mdb);
void db_thread_cleanup(B_DB *mdb);
void db_escape_string(JCR *jcr, B_DB *mdb, char *snew, char *old, int len);
char *db_escape_object(JCR *jcr, B_DB *mdb, char *old, int len);
void db_unescape_object(JCR *jcr, B_DB *mdb,
                        char *from, int32_t expected_len,
                        POOLMEM **dest, int32_t *len);
void db_start_transaction(JCR *jcr, B_DB *mdb);
void db_end_transaction(JCR *jcr, B_DB *mdb);
bool db_sql_query(B_DB *mdb, const char *query, int flags=0);
bool db_sql_query(B_DB *mdb, const char *query, DB_RESULT_HANDLER *result_handler, void *ctx);
bool db_big_sql_query(B_DB *mdb, const char *query, DB_RESULT_HANDLER *result_handler, void *ctx);

#ifdef _BDB_PRIV_INTERFACE_
void sql_free_result(B_DB *mdb);
SQL_ROW sql_fetch_row(B_DB *mdb);
bool sql_query(B_DB *mdb, const char *query, int flags=0);
const char *sql_strerror(B_DB *mdb);
int sql_num_rows(B_DB *mdb);
void sql_data_seek(B_DB *mdb, int row);
int sql_affected_rows(B_DB *mdb);
uint64_t sql_insert_autokey_record(B_DB *mdb, const char *query, const char *table_name);
void sql_field_seek(B_DB *mdb, int field);
SQL_FIELD *sql_fetch_field(B_DB *mdb);
int sql_num_fields(B_DB *mdb);
bool sql_field_is_not_null(B_DB *mdb, int field_type);
bool sql_field_is_numeric(B_DB *mdb, int field_type);
bool sql_batch_start(JCR *jcr, B_DB *mdb);
bool sql_batch_end(JCR *jcr, B_DB *mdb, const char *error);
bool sql_batch_insert(JCR *jcr, B_DB *mdb, ATTR_DBR *ar);
#endif /* _BDB_PRIV_INTERFACE_ */
#endif /* __SQL_GLUE_H_ */
