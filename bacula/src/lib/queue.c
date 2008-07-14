/*

                         Q U E U E
                     Queue Handling Routines

        Taken from smartall written by John Walker.

                  http://www.fourmilab.ch/smartall/


        Version $Id$

*/
/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2006 Free Software Foundation Europe e.V.

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


#include "bacula.h"

/*  General purpose queue  */

#ifdef REALLY_NEEDED
struct b_queue {
        struct b_queue *qnext,       /* Next item in queue */
                     *qprev;       /* Previous item in queue */
};
#endif

/*
 * To define a queue, use the following
 *
 *  static BQUEUE xyz = { &xyz, &xyz };
 *
 *   Also, note, that the only real requirement is that
 *   the object that is passed to these routines contain
 *   a BQUEUE object as the very first member. The
 *   rest of the structure may be anything.
 *
 *   NOTE!!!! The casting here is REALLY painful, but this avoids
 *            doing ugly casting every where else in the code.
 */


/*  Queue manipulation functions.  */


/*  QINSERT  --  Insert object at end of queue  */

void qinsert(BQUEUE *qhead, BQUEUE *object)
{
#define qh ((BQUEUE *)qhead)
#define obj ((BQUEUE *)object)

        ASSERT(qh->qprev->qnext == qh);
        ASSERT(qh->qnext->qprev == qh);

        obj->qnext = qh;
        obj->qprev = qh->qprev;
        qh->qprev = obj;
        obj->qprev->qnext = obj;
#undef qh
#undef obj
}


/*  QREMOVE  --  Remove next object from the queue given
                 the queue head (or any item).
     Returns NULL if queue is empty  */

BQUEUE *qremove(BQUEUE *qhead)
{
#define qh ((BQUEUE *)qhead)
        BQUEUE *object;

        ASSERT(qh->qprev->qnext == qh);
        ASSERT(qh->qnext->qprev == qh);

        if ((object = qh->qnext) == qh)
           return NULL;
        qh->qnext = object->qnext;
        object->qnext->qprev = qh;
        return object;
#undef qh
}

/*  QNEXT   --   Return next item from the queue
 *               returns NULL at the end of the queue.
 *               If qitem is NULL, the first item from
 *               the queue is returned.
 */

BQUEUE *qnext(BQUEUE *qhead, BQUEUE *qitem)
{
#define qh ((BQUEUE *)qhead)
#define qi ((BQUEUE *)qitem)

        BQUEUE *object;

        if (qi == NULL)
           qitem = qhead;
        ASSERT(qi->qprev->qnext == qi);
        ASSERT(qi->qnext->qprev == qi);

        if ((object = qi->qnext) == qh)
           return NULL;
        return object;
#undef qh
#undef qi
}


/*  QDCHAIN  --  Dequeue an item from the middle of a queue.  Passed
                 the queue item, returns the (now dechained) queue item. */

BQUEUE *qdchain(BQUEUE *qitem)
{
#define qi ((BQUEUE *)qitem)

        ASSERT(qi->qprev->qnext == qi);
        ASSERT(qi->qnext->qprev == qi);

        return qremove(qi->qprev);
#undef qi
}
