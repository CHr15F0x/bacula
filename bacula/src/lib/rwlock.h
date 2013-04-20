/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2001-2010 Free Software Foundation Europe e.V.

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
/*
 * Bacula Thread Read/Write locking code. It permits
 *  multiple readers but only one writer.
 *
 *  Kern Sibbald, January MMI
 *
 *  This code adapted from "Programming with POSIX Threads", by
 *    David R. Butenhof
 *
 *   Version $Id$
 *
 */

#ifndef __RWLOCK_H
#define __RWLOCK_H 1

typedef struct s_rwlock_tag {
   pthread_mutex_t   mutex;
   pthread_cond_t    read;            /* wait for read */
   pthread_cond_t    write;           /* wait for write */
   pthread_t         writer_id;       /* writer's thread id */
   int               priority;        /* used in deadlock detection */
   int               valid;           /* set when valid */
   int               r_active;        /* readers active */
   int               w_active;        /* writers active */
   int               r_wait;          /* readers waiting */
   int               w_wait;          /* writers waiting */
} brwlock_t;

typedef struct s_rwsteal_tag {
   pthread_t         writer_id;       /* writer's thread id */
   int               state;
} brwsteal_t;


#define RWLOCK_VALID  0xfacade

#define RWL_INIIALIZER \
   {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, \
    PTHREAD_COND_INITIALIZER, RWLOCK_VALID, 0, 0, 0, 0}

#define rwl_writelock(x)     rwl_writelock_p((x), __FILE__, __LINE__)

/*
 * read/write lock prototypes
 */
extern int rwl_init(brwlock_t *wrlock, int priority=0);
extern int rwl_destroy(brwlock_t *rwlock);
extern bool rwl_is_init(brwlock_t *rwl);
extern int rwl_readlock(brwlock_t *rwlock);
extern int rwl_readtrylock(brwlock_t *rwlock);
extern int rwl_readunlock(brwlock_t *rwlock);
extern int rwl_writelock_p(brwlock_t *rwlock,
                           const char *file="*unknown*", int line=0);
extern int rwl_writetrylock(brwlock_t *rwlock);
extern int rwl_writeunlock(brwlock_t *rwlock);

#endif /* __RWLOCK_H */
