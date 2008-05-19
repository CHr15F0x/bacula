/*************************************************************************************************
 * The abstract database API of Tokyo Cabinet
 *                                                      Copyright (C) 2006-2008 Mikio Hirabayashi
 * This file is part of Tokyo Cabinet.
 * Tokyo Cabinet is free software; you can redistribute it and/or modify it under the terms of
 * the GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License or any later version.  Tokyo Cabinet is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 * You should have received a copy of the GNU Lesser General Public License along with Tokyo
 * Cabinet; if not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA.
 *************************************************************************************************/


#ifndef _TCADB_H                         /* duplication check */
#define _TCADB_H

#if defined(__cplusplus)
#define __TCADB_CLINKAGEBEGIN extern "C" {
#define __TCADB_CLINKAGEEND }
#else
#define __TCADB_CLINKAGEBEGIN
#define __TCADB_CLINKAGEEND
#endif
__TCADB_CLINKAGEBEGIN


#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <tcutil.h>
#include <tchdb.h>
#include <tcbdb.h>



/*************************************************************************************************
 * API
 *************************************************************************************************/


typedef struct {                         /* type of structure for an abstract database */
  char *name;                            /* name of the database */
  TCMDB *mdb;                            /* on-memory database object */
  TCHDB *hdb;                            /* hash database object */
  TCBDB *bdb;                            /* B+ tree database object */
  int64_t capnum;                        /* capacity number of records */
  int64_t capsiz;                        /* capacity size of using memory */
  uint32_t capcnt;                       /* count for capacity check */
  BDBCUR *cur;                           /* cursor of B+ tree */
} TCADB;


/* Create an abstract database object.
   The return value is the new abstract database object. */
TCADB *tcadbnew(void);


/* Delete an abstract database object.
   `adb' specifies the abstract database object. */
void tcadbdel(TCADB *adb);


/* Open an abstract database.
   `adb' specifies the abstract database object.
   `name' specifies the name of the database.  If it is "*", the database will be an on-memory
   database.  If its suffix is ".tch", the database will be a hash database.  If its suffix is
   ".tcb", the database will be a B+ tree database.  Otherwise, this function fails.  Tuning
   parameters can trail the name, separated by "#".  Each parameter is composed of the name and
   the number, separated by "=".  On-memory database supports "bnum", "capnum", and "capsiz".
   Hash database supports "mode", "bnum", "apow", "fpow", "opts", and "rcnum".  B+ tree database
   supports "mode", "lmemb", "nmemb", "bnum", "apow", "fpow", "opts", "lcnum", and "ncnum".
   "capnum" specifies the capacity number of records.  "capsiz" specifies the capacity size of
   using memory.  Records spilled the capacity are removed by the storing order.  "mode" can
   contain "w" of writer, "r" of reader, "c" of creating, "t" of truncating, "e" of no locking,
   and "f" of non-blocking lock.  The default mode is relevant to "wc".  "opts" can contains "l"
   of large option, "d" of Deflate option, and "b" of TCBS option.  For example,
   "casket.tch#bnum=1000000#opts=ld" means that the name of the database file is "casket.tch",
   and the bucket number is 1000000, and the options are large and Deflate.
   If successful, the return value is true, else, it is false. */
bool tcadbopen(TCADB *adb, const char *name);


/* Close an abstract database object.
   `adb' specifies the abstract database object.
   If successful, the return value is true, else, it is false.
   Update of a database is assured to be written when the database is closed.  If a writer opens
   a database but does not close it appropriately, the database will be broken. */
bool tcadbclose(TCADB *adb);


/* Store a record into an abstract database object.
   `adb' specifies the abstract database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   If successful, the return value is true, else, it is false.
   If a record with the same key exists in the database, it is overwritten. */
bool tcadbput(TCADB *adb, const void *kbuf, int ksiz, const void *vbuf, int vsiz);


/* Store a string record into an abstract object.
   `adb' specifies the abstract database object.
   `kstr' specifies the string of the key.
   `vstr' specifies the string of the value.
   If successful, the return value is true, else, it is false.
   If a record with the same key exists in the database, it is overwritten. */
bool tcadbput2(TCADB *adb, const char *kstr, const char *vstr);


/* Store a new record into an abstract database object.
   `adb' specifies the abstract database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   If successful, the return value is true, else, it is false.
   If a record with the same key exists in the database, this function has no effect. */
bool tcadbputkeep(TCADB *adb, const void *kbuf, int ksiz, const void *vbuf, int vsiz);


/* Store a new string record into an abstract database object.
   `adb' specifies the abstract database object.
   `kstr' specifies the string of the key.
   `vstr' specifies the string of the value.
   If successful, the return value is true, else, it is false.
   If a record with the same key exists in the database, this function has no effect. */
bool tcadbputkeep2(TCADB *adb, const char *kstr, const char *vstr);


/* Concatenate a value at the end of the existing record in an abstract database object.
   `adb' specifies the abstract database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   If successful, the return value is true, else, it is false.
   If there is no corresponding record, a new record is created. */
bool tcadbputcat(TCADB *adb, const void *kbuf, int ksiz, const void *vbuf, int vsiz);


/* Concatenate a string value at the end of the existing record in an abstract database object.
   `adb' specifies the abstract database object.
   `kstr' specifies the string of the key.
   `vstr' specifies the string of the value.
   If successful, the return value is true, else, it is false.
   If there is no corresponding record, a new record is created. */
bool tcadbputcat2(TCADB *adb, const char *kstr, const char *vstr);


/* Remove a record of an abstract database object.
   `adb' specifies the abstract database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is true, else, it is false. */
bool tcadbout(TCADB *adb, const void *kbuf, int ksiz);


/* Remove a string record of an abstract database object.
   `adb' specifies the abstract database object.
   `kstr' specifies the string of the key.
   If successful, the return value is true, else, it is false. */
bool tcadbout2(TCADB *adb, const char *kstr);


/* Retrieve a record in an abstract database object.
   `adb' specifies the abstract database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `sp' specifies the pointer to the variable into which the size of the region of the return
   value is assigned.
   If successful, the return value is the pointer to the region of the value of the corresponding
   record.  `NULL' is returned if no record corresponds.
   Because an additional zero code is appended at the end of the region of the return value,
   the return value can be treated as a character string.  Because the region of the return
   value is allocated with the `malloc' call, it should be released with the `free' call when
   it is no longer in use. */
void *tcadbget(TCADB *adb, const void *kbuf, int ksiz, int *sp);


/* Retrieve a string record in an abstract database object.
   `adb' specifies the abstract database object.
   `kstr' specifies the string of the key.
   If successful, the return value is the string of the value of the corresponding record.
   `NULL' is returned if no record corresponds.
   Because the region of the return value is allocated with the `malloc' call, it should be
   released with the `free' call when it is no longer in use. */
char *tcadbget2(TCADB *adb, const char *kstr);


/* Get the size of the value of a record in an abstract database object.
   `adb' specifies the abstract database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is the size of the value of the corresponding record, else,
   it is -1. */
int tcadbvsiz(TCADB *adb, const void *kbuf, int ksiz);


/* Get the size of the value of a string record in an abstract database object.
   `adb' specifies the abstract database object.
   `kstr' specifies the string of the key.
   If successful, the return value is the size of the value of the corresponding record, else,
   it is -1. */
int tcadbvsiz2(TCADB *adb, const char *kstr);


/* Initialize the iterator of an abstract database object.
   `adb' specifies the abstract database object.
   If successful, the return value is true, else, it is false.
   The iterator is used in order to access the key of every record stored in a database. */
bool tcadbiterinit(TCADB *adb);


/* Get the next key of the iterator of an abstract database object.
   `adb' specifies the abstract database object.
   `sp' specifies the pointer to the variable into which the size of the region of the return
   value is assigned.
   If successful, the return value is the pointer to the region of the next key, else, it is
   `NULL'.  `NULL' is returned when no record is to be get out of the iterator.
   Because an additional zero code is appended at the end of the region of the return value, the
   return value can be treated as a character string.  Because the region of the return value is
   allocated with the `malloc' call, it should be released with the `free' call when it is no
   longer in use.  It is possible to access every record by iteration of calling this function.
   It is allowed to update or remove records whose keys are fetched while the iteration.
   However, it is not assured if updating the database is occurred while the iteration.  Besides,
   the order of this traversal access method is arbitrary, so it is not assured that the order of
   storing matches the one of the traversal access. */
void *tcadbiternext(TCADB *adb, int *sp);


/* Get the next key string of the iterator of an abstract database object.
   `adb' specifies the abstract database object.
   If successful, the return value is the string of the next key, else, it is `NULL'.  `NULL' is
   returned when no record is to be get out of the iterator.
   Because the region of the return value is allocated with the `malloc' call, it should be
   released with the `free' call when it is no longer in use.  It is possible to access every
   record by iteration of calling this function.  However, it is not assured if updating the
   database is occurred while the iteration.  Besides, the order of this traversal access method
   is arbitrary, so it is not assured that the order of storing matches the one of the traversal
   access. */
char *tcadbiternext2(TCADB *adb);


/* Get forward matching keys in an abstract database object.
   `adb' specifies the abstract database object.
   `pbuf' specifies the pointer to the region of the prefix.
   `psiz' specifies the size of the region of the prefix.
   `max' specifies the maximum number of keys to be fetched.  If it is negative, no limit is
   specified.
   The return value is a list object of the corresponding keys.  This function does never fail
   and return an empty list even if no key corresponds.
   Because the object of the return value is created with the function `tclistnew', it should be
   deleted with the function `tclistdel' when it is no longer in use.  Note that this function
   may be very slow because every key in the database is scanned. */
TCLIST *tcadbfwmkeys(TCADB *adb, const void *pbuf, int psiz, int max);


/* Get forward matching string keys in an abstract database object.
   `adb' specifies the abstract database object.
   `pstr' specifies the string of the prefix.
   `max' specifies the maximum number of keys to be fetched.  If it is negative, no limit is
   specified.
   The return value is a list object of the corresponding keys.  This function does never fail
   and return an empty list even if no key corresponds.
   Because the object of the return value is created with the function `tclistnew', it should be
   deleted with the function `tclistdel' when it is no longer in use.  Note that this function
   may be very slow because every key in the database is scanned. */
TCLIST *tcadbfwmkeys2(TCADB *adb, const char *pstr, int max);


/* Synchronize updated contents of an abstract database object with the file and the device.
   `adb' specifies the abstract database object.
   If successful, the return value is true, else, it is false.
   This function fails and has no effect for on-memory database. */
bool tcadbsync(TCADB *adb);


/* Remove all records of an abstract database object.
   `adb' specifies the abstract database object.
   If successful, the return value is true, else, it is false. */
bool tcadbvanish(TCADB *adb);


/* Copy the database file of an abstract database object.
   `adb' specifies the abstract database object.
   `path' specifies the path of the destination file.  If it begins with `@', the trailing
   substring is executed as a command line.
   If successful, the return value is true, else, it is false.  False is returned if the executed
   command returns non-zero code.
   The database file is assured to be kept synchronized and not modified while the copying or
   executing operation is in progress.  So, this function is useful to create a backup file of
   the database file.  This function fails and has no effect for on-memory database. */
bool tcadbcopy(TCADB *adb, const char *path);


/* Get the number of records of an abstract database object.
   `adb' specifies the abstract database object.
   The return value is the number of records or 0 if the object does not connect to any database
   instance. */
uint64_t tcadbrnum(TCADB *adb);


/* Get the size of the database of an abstract database object.
   `adb' specifies the abstract database object.
   The return value is the size of the database or 0 if the object does not connect to any
   database instance. */
uint64_t tcadbsize(TCADB *adb);



__TCADB_CLINKAGEEND
#endif                                   /* duplication check */


/* END OF FILE */
