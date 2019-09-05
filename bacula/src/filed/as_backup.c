#include "bacula.h"
#include "jcr.h"
#include "as_backup.h"
#include "../findlib/find.h"
#include "as_bsock_proxy.h"

#if AS_BACKUP

#define ASDEBUG 0
#define ASDEBUG_LOOP 0
#define ASDEBUG_CONS_ENQUEUE 0
#define ASDEBUG_DEALLOC_BUFFERS 0
#define ASDEBUG_CONS_QUEUE 0
#define ASDEBUG_FI 0
#define ASDEBUG_LOOP_DEQUEUE 0
#define ASDEBUG_INIT_SHUT 0

/* Used for debugging only */
#define AS_BUFFER_BASE 1000

/* Used for debugging only */
int my_thread_id()
{
   return H(pthread_self());
}

void AS_ENGINE::init()
{
   memset(this, 0, sizeof(AS_ENGINE));

   /* Producer stuff */
   pthread_mutex_init(&free_buf_q_lock, NULL);
   pthread_cond_init(&free_buf_q_cond, NULL);
   free_buf_q.qnext = &free_buf_q;
   free_buf_q.qprev = &free_buf_q;
   /* Consumer stuff */
   pthread_mutex_init(&cons_thr_lock, NULL);
   pthread_cond_init(&cons_thr_started_cond, NULL);
   pthread_mutex_init(&cons_q_lock, NULL);
   pthread_cond_init(&cons_q_cond, NULL);
   cons_q.qnext = &cons_q;
   cons_q.qprev = &cons_q;
}

void AS_ENGINE::cleanup()
{
   /* Producer stuff */
   pthread_mutex_destroy(&free_buf_q_lock);
   pthread_cond_destroy(&free_buf_q_cond);
   /* Consumer stuff */
   pthread_mutex_destroy(&cons_thr_lock);
   pthread_cond_destroy(&cons_thr_started_cond);
   pthread_mutex_destroy(&cons_q_lock);
   pthread_cond_destroy(&cons_q_cond);

   memset(this, 0, sizeof(AS_ENGINE));
}

void AS_ENGINE::destroy()
{
   cleanup();
   free(this);
}

int AS_ENGINE::smallest_fi_in_consumer_queue()
{
   int smallest_fi = INT_MAX;

   as_buffer_t *buffer = (as_buffer_t *)qnext(&cons_q, NULL);

   while (buffer) {
      if (buffer->file_idx < smallest_fi) {
         smallest_fi = buffer->file_idx;
      }

      buffer = (as_buffer_t *)qnext(&cons_q, &buffer->bq);
   }

   return smallest_fi;
}

as_buffer_t *AS_ENGINE::acquire_buf(AS_BSOCK_PROXY *parent, int file_idx)
{
#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d acquire_buf() BEGIN parent: %4X\n",
      my_thread_id(), HH(parent));
#endif

   as_buffer_t *buffer = NULL;

   /** Wait for a buffer to become available */
   P(free_buf_q_lock);
   /* TODO check for quit/job cancel */
   while (1) {
      P(cons_q_lock);

      /*
       * We may have used up all buffers for files whose file index is to
       * large to maintain continuous ascending file index order.
       * In such case we use a special extra buffer to fix that.
       */
      if ((qsize(&cons_q) == AS_BUFFERS) &&
          (smallest_fi_in_consumer_queue() > last_file_idx + 1) &&
          (file_idx == last_file_idx + 1)) {
         buffer = fix_fi_order_buf;
         fix_fi_order_buf = NULL;
         V(cons_q_lock);
      }
      /*
       * We may have used up all buffers for other files, while currently
       * we are in need of the next buffer to serve a file big enough
       * that it cannot be handled by a single buffer.
       * In such case we use a special extra buffer to fix that.
       */
      else if ((bigfile_bsock_proxy != NULL) &&
         (bigfile_bsock_proxy == parent)) {
         buffer = bigfile_buf; /* Can already be null */
         bigfile_buf = NULL;
         V(cons_q_lock);
      } else {
         V(cons_q_lock);
         buffer = (as_buffer_t *)qremove(&free_buf_q);
      }

      if (buffer != NULL) {
         /* Success, got a buffer, need to make sure it is clear */
         buffer->parent = parent;
         buffer->size = 0;
         buffer->file_idx = 0;
         buffer->final = 0;

#if ASDEBUG
         Pmsg4(50, "\t\t>>>> %4d acquire_buf() END parent: %4X, free size: %d, buf: %d\n",
            my_thread_id(), HH(parent), qsize(&free_buf_q), buffer->id);
#endif
         break;
      } else
      {
         /* No luck, let's wait until a free buffer shows up */
#if ASDEBUG
         P(cons_q_lock);
         Pmsg4(50, "\t\t>>>> %4d acquire_buf() WAIT bigfile_bsock_proxy: %4X parent: %4X, free size: %d \n",
            my_thread_id(), HH(bigfile_bsock_proxy), HH(parent), qsize(&free_buf_q));
         V(cons_q_lock);
#endif

         /* Wait for max 1 second */
         struct timespec abs_time;
         clock_gettime(CLOCK_REALTIME, &abs_time);
         abs_time.tv_sec += 1;

         pthread_cond_timedwait(&free_buf_q_cond, &free_buf_q_lock, &abs_time);
      }
   }
   V(free_buf_q_lock);

   ASSERT(buffer);
   ASSERT(buffer->parent == parent);
   ASSERT(buffer->size == 0);

   return buffer;
}

void AS_ENGINE::dump_consumer_queue()
{
#if ASDEBUG_CONS_QUEUE

   as_buffer_t *buffer = (as_buffer_t *)qnext(&cons_q, NULL);

   int cnt = 0;

   while (buffer)
   {
      Pmsg7(50, "\t\t>>>> %4d CONSUMER_Q[%d] %d (%p) FI: %4d parent: %4X, curr_bigfile: %4X\n",
         my_thread_id(), cnt, buffer->id, buffer, buffer->file_idx,
         buffer ? HH(buffer->parent) : 0, HH(bigfile_bsock_proxy));

      buffer = (as_buffer_t *)qnext(&cons_q, &buffer->bq);
      ++cnt;
   }
#endif
}

void AS_ENGINE::dump_consumer_queue_locked()
{
   P(cons_q_lock);
   dump_consumer_queue();
   V(cons_q_lock);
}

void AS_ENGINE::consumer_enqueue_buf(as_buffer_t *buffer, bool finalize)
{
   P(cons_q_lock);

#if ASDEBUG_CONS_ENQUEUE
    Pmsg5(50, "\t\t>>>> %4d consumer_enqueue_buf() %d (%p), BEGIN bigfile: %4X, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, HH(bigfile_bsock_proxy),
       qsize(&cons_q));
    dump_consumer_queue();
#endif

   if (finalize) {
      /* This was the last buffer for this big file */
      buffer->final = 1;
   } else {
      buffer->final = 0;
   }

#if ASDEBUG_FI
    Pmsg7(50, "\t\t>>>> %4d consumer_enqueue_buf() %d FI: %4d parent: %4X bigfile: %4X, final: %d cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer->file_idx, HH(buffer->parent), HH(bigfile_bsock_proxy),
	   buffer->final, qsize(&cons_q));
#endif

   qinsert(&cons_q, &buffer->bq);

#if ASDEBUG_CONS_ENQUEUE
    Pmsg5(50, "\t\t>>>> %4d consumer_enqueue_buf() %d (%p), END   bigfile: %4X, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, HH(bigfile_bsock_proxy),
       qsize(&cons_q));
    dump_consumer_queue();
#endif

   V(cons_q_lock);

   /* Let the consumer thread know */
   pthread_cond_signal(&cons_q_cond);
}

/**
 * Stuff used for cloning ff_pkt
 */
#define STRDUP(X) if (orig_ff_pkt->X) ff_pkt->X = bstrdup(orig_ff_pkt->X)
#define STRFREE(X) if (ff_pkt->X) free(ff_pkt->X)
#define POOLMEMDUP(X) if (orig_ff_pkt->X) { \
   ff_pkt->X = get_pool_memory(PM_FNAME); \
   int32_t orig_size = sizeof_pool_memory(orig_ff_pkt->X); \
   if (orig_size != sizeof_pool_memory(ff_pkt->X)) { \
      ff_pkt->X = realloc_pool_memory(ff_pkt->X, orig_size); \
   } \
}
#define POOLMEMFREE(X) if (ff_pkt->X) free_pool_memory(ff_pkt->X)

FF_PKT *as_new_ff_pkt_clone(FF_PKT *orig_ff_pkt)
{
   FF_PKT *ff_pkt = (FF_PKT *)bmalloc(sizeof(FF_PKT));
   memcpy(ff_pkt, orig_ff_pkt, sizeof(FF_PKT));

   STRDUP(top_fname);
   STRDUP(fname);
   STRDUP(link);
   STRDUP(object_name);
   STRDUP(object);
   STRDUP(plugin);
   POOLMEMDUP(fname_save);
   POOLMEMDUP(link_save);
   POOLMEMDUP(ignoredir_fname);
   STRDUP(digest);

   /* TODO check */
   ff_pkt->included_files_list = NULL;
   ff_pkt->excluded_files_list = NULL;
   ff_pkt->excluded_paths_list = NULL;
   ff_pkt->fileset = NULL;
   ff_pkt->linkhash = NULL;

   return ff_pkt;
}

void as_free_ff_pkt_clone(FF_PKT *ff_pkt)
{
   /* Smartalloc treats freeing NULL with warning */
   STRFREE(top_fname);
   STRFREE(fname);
   STRFREE(link);
   STRFREE(object_name);
   STRFREE(object);
   STRFREE(plugin);
   POOLMEMFREE(fname_save);
   POOLMEMFREE(link_save);
   POOLMEMFREE(ignoredir_fname);
   STRFREE(digest);

   free(ff_pkt);
}

#undef STRDUP
#undef STRFREE
#undef POOLMEMDUP
#undef POOLMEMFREE

int AS_ENGINE::save_file_schedule(
   JCR *jcr,
   FF_PKT *ff_pkt,
   bool do_plugin_set,
   DIGEST *digest,
   DIGEST *signing_digest,
   int digest_stream,
   bool has_file_data)
{
#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d save_file_schedule() file: %s\n", my_thread_id(), ff_pkt->fname);
#endif

   as_save_file_context_t *context = (as_save_file_context_t *)bmalloc(sizeof(as_save_file_context_t));
   context->jcr = jcr;
   context->ff_pkt = as_new_ff_pkt_clone(ff_pkt);
   context->do_plugin_set = do_plugin_set;
   context->digest = digest;
   context->signing_digest = signing_digest;
   context->digest_stream = digest_stream;
   context->has_file_data = has_file_data;

   workq_add(&work_q, context, NULL, 0);

   return 1;
}

void *as_workqueue_engine(void *arg)
{
   as_save_file_context_t *context = (as_save_file_context_t *)arg;

#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_engine()\n", my_thread_id());
#endif

   as_save_file(
      context->jcr,
      context->ff_pkt,
      context->do_plugin_set,
      context->digest,
      context->signing_digest,
      context->digest_stream,
      context->has_file_data);

   free(context);

   return NULL;
}

void AS_ENGINE::wait_consumer_thread_started()
{
   P(cons_thr_lock);
   while (cons_thr_started == false) {
      pthread_cond_wait(&cons_thr_started_cond, &cons_thr_lock);
   }
   V(cons_thr_lock);
}


bool AS_ENGINE::quit_consumer_thread_loop()
{
   bool quit = false;

   P(cons_thr_lock);
   quit = cons_thr_quit;
   V(cons_thr_lock);

   return quit;
}

bool AS_ENGINE::consumer_queue_empty(bool lockme)
{
   bool empty = false;

   if (lockme) {
      P(cons_q_lock);
   }

   empty = (qsize(&cons_q) == 0);

   if (lockme) {
      V(cons_q_lock);
   }

   return empty;
}

void AS_ENGINE::release_buffer(as_buffer_t *buffer)
{
#if ASDEBUG
   Pmsg4(50, "\t\t>>>> %4d release_buffer() BEGIN %d (%p), free size: %d \n",
	  my_thread_id(), buffer->id, buffer, qsize(&free_buf_q));
#endif

   P(free_buf_q_lock);

   buffer->size = 0;
   buffer->parent = NULL;
   buffer->final = 0;
   buffer->file_idx = 0;

   if (fix_fi_order_buf == NULL) {
      fix_fi_order_buf = buffer;
   } else if (bigfile_buf == NULL) {
      bigfile_buf = buffer;
   } else {
      qinsert(&free_buf_q, &buffer->bq);
   }

   V(free_buf_q_lock);

   pthread_cond_broadcast(&free_buf_q_cond);

#if ASDEBUG
   Pmsg4(50, "\t\t>>>> %4d release_buffer() END %d (%p), free size: %d \n",
      my_thread_id(), buffer->id, buffer, qsize(&free_buf_q));
#endif
}

static void *as_consumer_thread_loop_wrapper(void *arg)
{
   as_cons_thread_loop_context_t *ctxt = (as_cons_thread_loop_context_t *)arg;

   ctxt->as_engine->consumer_thread_loop(ctxt->sd);

   free(ctxt);

   return NULL;
}

void AS_ENGINE::consumer_thread_loop(BSOCK *sd)
{
   sd->clear_locking();

#if ASDEBUG_LOOP
   Pmsg3(50, "\t\t>>>> %4d consumer_thread_loop() START, sock: %p, quit_consumer_thread_loop(): %d\n",
         my_thread_id(), sd, quit_consumer_thread_loop());
#endif

   P(cons_thr_lock);
   cons_thr_started = true;
   pthread_cond_signal(&cons_thr_started_cond);
   V(cons_thr_lock);

   as_buffer_t *buffer = NULL;

   /* Somehow mark the variable as invalid, but don't use zero */
#define NEED_TO_INIT INT32_MIN

   int32_t to_send = NEED_TO_INIT;

   while ((quit_consumer_thread_loop() == false) ||
      (quit_consumer_thread_loop() &&
       (consumer_queue_empty(true) == false))) {
#if ASDEBUG_LOOP
	  Pmsg1(50, "\t\t>>>> %4d consumer_thread_loop() ITERATION BEGIN\n", my_thread_id());
#endif

      P(cons_q_lock);

#if ASDEBUG_LOOP
      Pmsg2(50, "\t\t>>>> %4d consumer_thread_loop() ITERATION BEGIN cons.q.size: %d\n", my_thread_id(), qsize(&cons_q));
#endif

      while ((quit_consumer_thread_loop() == false) ||
         (quit_consumer_thread_loop() &&
          (consumer_queue_empty(false) == false))) {
         /* Peek at the first elem */
         buffer = (as_buffer_t *)qnext(&cons_q, NULL);

         if (buffer) {
            /*
             * If this buffer refers to the next file index or the current one - ok
             * If not - this buffer has to wait until last file idx is large enough
             */
            if (buffer->file_idx == last_file_idx)
            {
               /* OK */
            }
            /* We may only increment if the current big file is done */
            else if ((buffer->file_idx == last_file_idx + 1) &&
               (bigfile_bsock_proxy == NULL)) {
               ++last_file_idx;
               /* OK */
            } else {
               /*
                * buffer->file_idx > last_file_idx + 1
                * We need to wait, find another one
                */
               while (buffer != NULL) {
                  buffer = (as_buffer_t *)qnext(&cons_q, &buffer->bq);

                  if (buffer != NULL) {
                     if (buffer->file_idx == last_file_idx) {
                        /* OK */
                        break;
                     } else if ((buffer->file_idx == last_file_idx + 1) &&
                        (bigfile_bsock_proxy == NULL)) {
                        /* We can only increment if the current big file is done */
                        ++last_file_idx;
                        /* OK */
                        break;
                     }
                  }
               }

               /* buffer can be NULL here */
            }

            if (buffer != NULL) {
               /* If this is the first chunk of a bigger file - mark it */
               if (bigfile_bsock_proxy == NULL) {
                  bigfile_bsock_proxy = buffer->parent;
                  /* Dequeue this buffer */
                  buffer = (as_buffer_t *)qdchain(&buffer->bq);

#if ASDEBUG_LOOP
         Pmsg7(50, "\t\t>>>> %4d consumer_thread_loop() DEQUEUE buf: %d fi: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
            my_thread_id(), buffer->id, buffer->file_idx, buffer->size, HH(buffer->parent), buffer->parent , qsize(&cons_q));
#endif
                  ASSERT(buffer);
                  break;
               } else {
                  /* If this is the next chunk of this bigger file */
                  if (bigfile_bsock_proxy == buffer->parent) {
                     /* If this is the last chunk of a bigger file - unmark it */
                     if (buffer->final) {
                        bigfile_bsock_proxy = NULL;
                     }
                     /* Good to go */
                     buffer = (as_buffer_t *)qdchain(&buffer->bq);

#if ASDEBUG_LOOP
            Pmsg7(50, "\t\t>>>> %4d consumer_thread_loop() DEQUEUE buf: %d fi: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
               my_thread_id(), buffer->id, buffer->file_idx, buffer->size, HH(buffer->parent), buffer->parent , qsize(&cons_q));
#endif

                     ASSERT(buffer);
                     break;
                  } else {
                     /*
                      * Cannot process this chunk now, try to find the next
                      * chunk from the current big file
                      */
                     while (buffer != NULL) {
                        buffer = (as_buffer_t *)qnext(&cons_q, &buffer->bq);

                        if ((buffer != NULL) &&
                           (buffer->parent == bigfile_bsock_proxy))
                        {
                           /* Found one */
                           break;
                        }
                     }

                     if ((buffer != NULL) &&
                        (buffer->parent == bigfile_bsock_proxy)) {
                        /* If this is the last chunk of a bigger file - unmark it */
                        if (buffer->final) {
                           bigfile_bsock_proxy = NULL;
                        }
                        /* Good to go */
                        buffer = (as_buffer_t *)qdchain(&buffer->bq);
   #if ASDEBUG_LOOP
            Pmsg7(50, "\t\t>>>> %4d consumer_thread_loop() DEQUEUE buf: %d fi: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
               my_thread_id(), buffer->id, buffer->file_idx, buffer->size, HH(buffer->parent), buffer->parent , qsize(&cons_q));
   #endif

                        ASSERT(buffer);
                        break;
                     }
                  }
               }
            } /* buffer was null need to try again */
         }

#if ASDEBUG_LOOP
         Pmsg2(50, "\t\t>>>> %4d consumer_thread_loop() DEQUEUE buf: WAIT, cons.q.size: %d\n", my_thread_id(), qsize(&cons_q));
#endif

         /* Let's wait for 1 second, maybe a free buffer will pop up */
         struct timespec abs_time;
         clock_gettime(CLOCK_REALTIME, &abs_time);
         abs_time.tv_sec += 1;

         pthread_cond_timedwait(&cons_q_cond, &cons_q_lock, &abs_time);
      }
      V(cons_q_lock);

      if (buffer == NULL)
      {
         /* This means we exit the thread loop */
         return;
      }
      else
      {
#if ASDEBUG_LOOP_DEQUEUE
         Pmsg8(50, "\t\t>>>> %4d consumer_thread_loop() DQ %d, FI: %d parent: %4X bigfile: %4X, final: %d cons.q.size: %d LAST_FI: %d\n",
            my_thread_id(), buffer->id, buffer->file_idx, HH(buffer->parent), HH(bigfile_bsock_proxy),
            buffer->final, qsize(&cons_q), last_file_idx);
#endif
      }

      ASSERT(buffer);
      ASSERT(buffer->size > 0);
      ASSERT(buffer->size <= AS_BUFFER_CAPACITY);

      /* We've got a buffer, so now we need to send it via sd socket */
      int pos_in_buffer = 0;

#if ASDEBUG_LOOP_SEND
      Pmsg8(50, "\t\t>>>> %4d consumer_thread_loop() START SENDING IN THIS ITER buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
         my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

      while (pos_in_buffer < buffer->size) {
         if (to_send == NEED_TO_INIT) {
            memcpy(&to_send, &buffer->data[pos_in_buffer], sizeof(to_send));
            pos_in_buffer += sizeof(to_send);

#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d GET_TO_SEND consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            ASSERT(to_send != 0);
         }

         /* Shouldn't happen */
         if (pos_in_buffer + to_send > buffer->size) {

#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_LESS_B consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            		my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif
            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = (buffer->size - pos_in_buffer);
            sd->send();

            to_send -= (buffer->size - pos_in_buffer);

#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_LESS_E consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            /* End of buffer, need to pick up the next one */
            break;
         }

         if ((to_send < 0) && (to_send != NEED_TO_INIT)) {
            /* A signal */
#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_SIGNAL consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            sd->signal(to_send);
            to_send = NEED_TO_INIT;
         } else if (to_send > 0) {
            /* A normal message */
#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_ENTIRE consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = to_send;
            sd->send();

            pos_in_buffer += to_send;
            to_send = NEED_TO_INIT;
         }

         ASSERT(to_send != 0);
      }

#if ASDEBUG_LOOP_SEND
      Pmsg8(50, "\t\t>>>> %4d END SENDING THIS ITER consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

      ASSERT(buffer);
      release_buffer(buffer);

#if ASDEBUG_LOOP
      Pmsg1(50, "\t\t>>>> %4d consumer_thread_loop() ITERATION END\n", my_thread_id());
#endif
   }

#if ASDEBUG_LOOP
   Pmsg2(50, "\t\t>>>> %4d consumer_thread_loop() STOP, sock: %p\n", my_thread_id(), sd);
#endif

   P(cons_thr_lock);
   cons_thr_started = false;
   V(cons_thr_lock);

   sd->set_locking();

   return;
}

#undef NEED_TO_INIT

void AS_ENGINE::init_free_buffers_queue()
{
   as_buffer_t *buffer = NULL;
   char *start = NULL;

#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d init_free_buffers_queue() size: %d\n",
      my_thread_id(), AS_BUFFERS);
#endif

   for (int i = 0; i < AS_BUFFERS; ++i) {
      buffer = (as_buffer_t *)bmalloc(sizeof(as_buffer_t));
      buffer->id = AS_BUFFER_BASE + i;
      buffer->size = 0;
      buffer->parent = NULL;
      buffer->final = 0;
      qinsert(&free_buf_q, &buffer->bq);
   }

   ASSERT(AS_BUFFERS == qsize(&free_buf_q));

   bigfile_buf = (as_buffer_t *)bmalloc(sizeof(as_buffer_t));
   bigfile_buf->id = AS_BUFFER_BASE + 998;

   ASSERT(bigfile_buf != NULL);

   fix_fi_order_buf = (as_buffer_t *)bmalloc(sizeof(as_buffer_t));
   fix_fi_order_buf->id = AS_BUFFER_BASE + 999;

   ASSERT(fix_fi_order_buf != NULL);
}

void AS_ENGINE::init_consumer_thread(BSOCK *sd)
{
   char buf[16] = "ba-CON-";
   static int len = strlen(buf);

   P(cons_thr_lock);
   cons_thr_quit = false;
   V(cons_thr_lock);

   as_cons_thread_loop_context_t *ctxt = (as_cons_thread_loop_context_t *)bmalloc(sizeof(as_cons_thread_loop_context_t));
   ctxt->as_engine = this;
   ctxt->sd = sd;

   pthread_create(&cons_thr, NULL, as_consumer_thread_loop_wrapper, (void *)ctxt);
   sprintf(buf + len, "%08X", pthread_self());
   pthread_setname_np(pthread_self(), buf);

#if ASDEBUG
   Pmsg3(50, "\t\t>>>> %4d init_consumer_thread() id: %4d sock: %p\n",
      my_thread_id(), H(cons_thr), sd);
#endif
}

void AS_ENGINE::work_queue_init()
{
#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d work_queue_init()\n", my_thread_id());
#endif

   workq_init(&work_q, AS_PRODUCER_THREADS, as_workqueue_engine);
}


void AS_ENGINE::start(BSOCK *sd, uint32_t buf_size)
{
#if ASDEBUG_INIT_SHUT
   Pmsg2(50, "\t\t>>>> %4d start() sock: %p\n", my_thread_id(), sd);
#endif

   /* Store the pointer to the poolmem */
   save_msg_pointer = sd->msg;
   initial_buf_size = buf_size;

   init_free_buffers_queue();
   init_consumer_thread(sd);
   wait_consumer_thread_started();

   work_queue_init();
}

uint32_t AS_ENGINE::get_initial_bsock_proxy_buf_size()
{
   return initial_buf_size;
}

void AS_ENGINE::request_consumer_thread_quit()
{
#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d request_consumer_thread_quit()\n", my_thread_id());
#endif

   P(cons_thr_lock);
   cons_thr_quit = true;
   V(cons_thr_lock);
}

void AS_ENGINE::join_consumer_thread()
{
   pthread_cond_signal(&cons_q_cond);

#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d join_consumer_thread()\n", my_thread_id());
#endif

   pthread_join(cons_thr, NULL);
}

void AS_ENGINE::dealloc_all_buffers()
{
#if ASDEBUG_DEALLOC_BUFFERS
   Pmsg3(50, "\t\t>>>> %4d dealloc_all_buffers() BEGIN, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&free_buf_q), qsize(&cons_q));
#endif

   as_buffer_t *buffer = NULL;

   do {
      buffer = (as_buffer_t *)qremove(&free_buf_q);

      if (buffer != NULL) {
#if ASDEBUG_DEALLOC_BUFFERS
         Pmsg3(50, "\t\t>>>> %4d dealloc_all_buffers() FREE buffer: %d (%p)\n",
    	      my_thread_id(), buffer->id, buffer);
#endif

         /*
          * Smart alloc does not allow free(NULL),
          * which is inconsistent with how free works
          */
         free(buffer);
      }
   } while (buffer != NULL);

   do {
      buffer = (as_buffer_t *)qremove(&cons_q);

      if (buffer != NULL) {

#if ASDEBUG_DEALLOC_BUFFERS
         Pmsg3(50, "\t\t>>>> %4d dealloc_all_buffers() CONS buffer: %d (%p)\n",
   	      my_thread_id(), buffer->id, buffer);
#endif

         /*
          * Smart alloc does not allow free(NULL),
          * which is inconsistent with how free works
          */
         free(buffer);
      }
   } while (buffer != NULL);

   if (bigfile_buf != NULL) {
#if ASDEBUG_DEALLOC_BUFFERS
      Pmsg3(50, "\t\t>>>> %4d dealloc_all_buffers() BIG buffer: %d (%p)\n",
         my_thread_id(), bigfile_buf->id, bigfile_buf);
#endif

      free(bigfile_buf);
      bigfile_buf = NULL;
   }

   if (fix_fi_order_buf != NULL) {
#if ASDEBUG_DEALLOC_BUFFERS
      Pmsg3(50, "\t\t>>>> %4d dealloc_all_buffers() FI FIX buffer: %d (%p)\n",
         my_thread_id(), fix_fi_order_buf->id, bigfile_buf);
#endif

      free(fix_fi_order_buf);
      fix_fi_order_buf = NULL;
   }

#if ASDEBUG_DEALLOC_BUFFERS
   Pmsg3(50, "\t\t>>>> %4d dealloc_all_buffers() END, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&free_buf_q), qsize(&cons_q));
#endif

   ASSERT(0 == qsize(&free_buf_q));
   ASSERT(0 == qsize(&cons_q));
}

void AS_ENGINE::work_queue_destroy()
{
#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d work_queue_destroy()\n", my_thread_id());
#endif

   workq_destroy(&work_q);
}

void AS_ENGINE::stop(BSOCK *sd)
{
#if ASDEBUG_INIT_SHUT
   Pmsg1(50, "\t\t>>>> %4d stop() BEGIN\n", my_thread_id());
#endif

   work_queue_destroy();
   request_consumer_thread_quit();
   join_consumer_thread();
   dealloc_all_buffers();

   /* Restore the pointer to the poolmem */
   sd->msg = save_msg_pointer;

   /* Clear remainig members */
   cons_thr_quit = false;
   bigfile_bsock_proxy = NULL;
   cons_thr_started = false;
   last_file_idx = 0;

#if ASDEBUG_INIT_SHUT
   Pmsg1(50, "\t\t>>>> %4d stop() END\n", my_thread_id());
#endif
}

#endif /* AS_BACKUP */
