/*
   Bacula® - The Network Backup Solution
   Copyright (C) 2009-2010 Free Software Foundation Europe e.V.
   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.
   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.
   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
   Bacula® is a registered trademark of Kern Sibbald.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
#ifndef _MYINGRES_SH
#define _MYINGRES_SH
#include <eqpname.h>
#include <eqdefcc.h>
#include <eqsqlda.h>

/* ---typedefs--- */
typedef struct ing_timestamp {
   unsigned short year;
   unsigned char month;
   unsigned char day;
   unsigned int secs;
   unsigned int nsecs;
   unsigned char tzh;
   unsigned char tzm;
} ING_TIMESTAMP;
typedef struct ing_field {
   char *name;
   int max_length;
   unsigned int type;
   unsigned int flags;		/* 1 == not null */
} INGRES_FIELD;
typedef struct ing_row {
   IISQLVAR *sqlvar;		/* ptr to sqlvar[sqld] for one row */
   struct ing_row *next;
   int row_number;
} ING_ROW;
typedef enum ing_status {
   ING_COMMAND_OK,
   ING_TUPLES_OK,
   ING_NO_RESULT,
   ING_NO_ROWS_PROCESSED,
   ING_EMPTY_RESULT,
   ING_ERROR
} ING_STATUS;
typedef struct ing_varchar {
   short len;
   char* value;
} ING_VARCHAR;
/* It seems, Bacula needs the complete query result stored in one data structure */
typedef struct ing_result {
   IISQLDA *sqlda;		/* descriptor */
   INGRES_FIELD *fields;
   int num_rows;
   int num_fields;
   ING_STATUS status;
   ING_ROW *first_row;
   ING_ROW *act_row;		/* just for iterating */
} INGresult;
typedef struct ing_conn {
   char *dbname;
   char *user;
   char *password;
   int session_id;
   char *msg;
} INGconn;
/* ---Prototypes--- */
int INGgetCols(INGconn *dbconn, const char *query, bool transaction);
char *INGgetvalue(INGresult *ing_res, int row_number, int column_number);
bool INGgetisnull(INGresult *ing_res, int row_number, int column_number);
int INGntuples(const INGresult *ing_res);
int INGnfields(const INGresult *ing_res);
char *INGfname(const INGresult *ing_res, int column_number);
short INGftype(const INGresult *ing_res, int column_number);
int INGexec(INGconn *dbconn, const char *query, bool transaction);
INGresult *INGquery(INGconn *dbconn, const char *query, bool transaction);
void INGclear(INGresult *ing_res);
void INGcommit(const INGconn *dbconn);
INGconn *INGconnectDB(char *dbname, char *user, char *passwd, int session_id);
void INGsetDefaultLockingMode(INGconn *dbconn);
void INGdisconnectDB(INGconn *dbconn);
char *INGerrorMessage(const INGconn *dbconn);
char *INGcmdTuples(INGresult *ing_res);
/* # line 109 "myingres.sh" */	
#endif /* _MYINGRES_SH */
