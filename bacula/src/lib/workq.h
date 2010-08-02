/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2001-2009 Free Software Foundation Europe e.V.

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
 * Bacula work queue routines. Permits passing work to
 *  multiple threads.
 *
 *  Kern Sibbald, January MMI
 *
 *  This code adapted from "Programming with POSIX Threads", by
 *    David R. Butenhof
 *
 *   Version $Id$
 */

#ifndef __WORKQ_H
#define __WORKQ_H 1

/*
 * Structure to keep track of work queue request
 */
typedef struct workq_ele_tag {
   struct workq_ele_tag *next;
   void                 *data;
} workq_ele_t;

/*
 * Structure describing a work queue
 */
typedef struct workq_tag {
   pthread_mutex_t   mutex;           /* queue access control */
   pthread_cond_t    work;            /* wait for work */
   pthread_attr_t    attr;            /* create detached threads */
   workq_ele_t       *first, *last;   /* work queue */
   int               valid;           /* queue initialized */
   int               quit;            /* workq should quit */
   int               max_workers;     /* max threads */
   int               num_workers;     /* current threads */
   int               idle_workers;    /* idle threads */
   void             *(*engine)(void *arg); /* user engine */
} workq_t;

#define WORKQ_VALID  0xdec1992

extern int workq_init(
              workq_t *wq,
              int     threads,        /* maximum threads */
              void   *(*engine)(void *)   /* engine routine */
                    );
extern int workq_destroy(workq_t *wq);
extern int workq_add(workq_t *wq, void *element, workq_ele_t **work_item, int priority);
extern int workq_remove(workq_t *wq, workq_ele_t *work_item);

#endif /* __WORKQ_H */
