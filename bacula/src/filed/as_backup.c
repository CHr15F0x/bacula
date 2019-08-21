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

void AS_ENGINE::init(int32_t compress_buf_size)
{
   memset(this, 0, sizeof(AS_ENGINE));

   /* Producer stuff */
   pthread_mutex_init(&as_buffer_lock, NULL);
   pthread_cond_init(&as_buffer_cond, NULL);
   as_free_buffer_queue.qnext = &as_free_buffer_queue;
   as_free_buffer_queue.qprev = &as_free_buffer_queue;
   /* Consumer stuff */
   pthread_mutex_init(&as_consumer_thread_lock, NULL);
   pthread_cond_init(&as_consumer_thread_started_cond, NULL);
   pthread_mutex_init(&as_consumer_queue_lock, NULL);
   pthread_cond_init(&as_consumer_queue_cond, NULL);
   as_consumer_buffer_queue.qnext = &as_consumer_buffer_queue;
   as_consumer_buffer_queue.qprev = &as_consumer_buffer_queue;

#if defined(HAVE_LZO) || defined(HAVE_LIBZ)
   for (int i = 0; i < AS_PRODUCER_THREADS; ++i) {
      compress_buf[i] = get_memory(compress_buf_size);
   }
#endif /* defined(HAVE_LZO) || defined(HAVE_LIBZ) */

#ifdef HAVE_LZO
   if (lzo_init() == LZO_E_OK) {
      for (int i = 0; i < AS_PRODUCER_THREADS; ++i) {
         lzo_voidp pLzoMem = (lzo_voidp) malloc(LZO1X_1_MEM_COMPRESS);
         if (pLzoMem) {
            LZO_compress_workset[i] = pLzoMem;
         }
      }
   }
#endif /* HAVE_LZO */

#ifdef HAVE_LIBZ
   for (int i = 0; i < AS_PRODUCER_THREADS; ++i) {
      z_stream *pZlibStream = (z_stream*)malloc(sizeof(z_stream));
      if (pZlibStream) {
         pZlibStream->zalloc = Z_NULL;
         pZlibStream->zfree = Z_NULL;
         pZlibStream->opaque = Z_NULL;
         pZlibStream->state = Z_NULL;

         if (deflateInit(pZlibStream, Z_DEFAULT_COMPRESSION) == Z_OK) {
            pZLIB_compress_workset[i] = pZlibStream;
         } else {
            free (pZlibStream);
         }
      }
   }
#endif /* HAVE_LIBZ */
}

void AS_ENGINE::cleanup()
{
   /* Producer stuff */
   pthread_mutex_destroy(&as_buffer_lock);
   pthread_cond_destroy(&as_buffer_cond);
   /* Consumer stuff */
   pthread_mutex_destroy(&as_consumer_thread_lock);
   pthread_cond_destroy(&as_consumer_thread_started_cond);
   pthread_mutex_destroy(&as_consumer_queue_lock);
   pthread_cond_destroy(&as_consumer_queue_cond);

#if defined(HAVE_LZO) || defined(HAVE_LIBZ)
   for (int i = 0; i < AS_PRODUCER_THREADS; ++i) {
      if (compress_buf) {
         free_pool_memory(compress_buf);
      }
   }
#endif /* defined(HAVE_LZO) || defined(HAVE_LIBZ) */

#ifdef HAVE_LZO
   if (LZO_compress_workset) {
      free (LZO_compress_workset);
   }
#endif /* HAVE_LZO */

#ifdef HAVE_LIBZ
   if (pZLIB_compress_workset) {
      deflateEnd((z_stream *)pZLIB_compress_workset);
      free (pZLIB_compress_workset);
   }
#endif /* HAVE_LIBZ */

   memset(this, 0, sizeof(AS_ENGINE));
}

void AS_ENGINE::destroy()
{
   cleanup();
   free(this);
}

int AS_ENGINE::as_smallest_fi_in_consumer_queue()
{
   int smallest_fi = INT_MAX;

   as_buffer_t *buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, NULL);

   while (buffer) {
      if (buffer->file_idx < smallest_fi) {
         smallest_fi = buffer->file_idx;
      }

      buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, &buffer->bq);
   }

   return smallest_fi;
}

as_buffer_t *AS_ENGINE::as_acquire_buffer(AS_BSOCK_PROXY *parent, int file_idx)
{
#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d as_acquire_buffer() BEGIN parent: %4X\n",
      my_thread_id(), HH(parent));
#endif

   as_buffer_t *buffer = NULL;

   /** Wait for a buffer to become available */
   P(as_buffer_lock);
   /* TODO check for quit/job cancel */
   while (1) {
      P(as_consumer_queue_lock);

      /*
       * We may have used up all buffers for files whose file index is to
       * large to maintain continuous ascending file index order.
       * In such case we use a special extra buffer to fix that.
       */
      if ((qsize(&as_consumer_buffer_queue) == AS_BUFFERS) &&
          (as_smallest_fi_in_consumer_queue() > as_last_file_idx + 1) &&
          (file_idx == as_last_file_idx + 1)) {
         buffer = as_fix_fi_order_buffer;
         as_fix_fi_order_buffer = NULL;
         V(as_consumer_queue_lock);
      }
      /*
       * We may have used up all buffers for other files, while currently
       * we are in need of the next buffer to serve a file big enough
       * that it cannot be handled by a single buffer.
       * In such case we use a special extra buffer to fix that.
       */
      else if ((as_bigfile_bsock_proxy != NULL) &&
         (as_bigfile_bsock_proxy == parent)) {
         buffer = as_bigfile_buffer_only; /* Can already be null */
         as_bigfile_buffer_only = NULL;
         V(as_consumer_queue_lock);
      } else {
         V(as_consumer_queue_lock);
         buffer = (as_buffer_t *)qremove(&as_free_buffer_queue);
      }

      if (buffer != NULL) {
         /* Success, got a buffer, need to make sure it is clear */
         buffer->parent = parent;
         buffer->size = 0;
         buffer->file_idx = 0;
         buffer->final = 0;

#if ASDEBUG
         Pmsg4(50, "\t\t>>>> %4d as_acquire_buffer() END parent: %4X, free size: %d, buf: %d\n",
            my_thread_id(), HH(parent), qsize(&as_free_buffer_queue), buffer->id);
#endif
         break;
      } else
      {
         /* No luck, let's wait until a free buffer shows up */
#if ASDEBUG
         P(as_consumer_queue_lock);
         Pmsg4(50, "\t\t>>>> %4d as_acquire_buffer() WAIT as_bigfile_bsock_proxy: %4X parent: %4X, free size: %d \n",
            my_thread_id(), HH(as_bigfile_bsock_proxy), HH(parent), qsize(&as_free_buffer_queue));
         V(as_consumer_queue_lock);
#endif

         /* Wait for max 1 second */
         struct timespec abs_time;
         clock_gettime(CLOCK_REALTIME, &abs_time);
         abs_time.tv_sec += 1;

         pthread_cond_timedwait(&as_buffer_cond, &as_buffer_lock, &abs_time);
      }
   }
   V(as_buffer_lock);

   ASSERT(buffer);
   ASSERT(buffer->parent == parent);
   ASSERT(buffer->size == 0);

   return buffer;
}

void AS_ENGINE::dump_consumer_queue()
{
#if ASDEBUG_CONS_QUEUE

   as_buffer_t *buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, NULL);

   int cnt = 0;

   while (buffer)
   {
      Pmsg7(50, "\t\t>>>> %4d CONSUMER_Q[%d] %d (%p) FI: %4d parent: %4X, curr_bigfile: %4X\n",
         my_thread_id(), cnt, buffer->id, buffer, buffer->file_idx,
         buffer ? HH(buffer->parent) : 0, HH(as_bigfile_bsock_proxy));

      buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, &buffer->bq);
      ++cnt;
   }
#endif
}

void AS_ENGINE::dump_consumer_queue_locked()
{
   P(as_consumer_queue_lock);
   dump_consumer_queue();
   V(as_consumer_queue_lock);
}

void AS_ENGINE::as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize)
{
   P(as_consumer_queue_lock);

#if ASDEBUG_CONS_ENQUEUE
    Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), BEGIN bigfile: %4X, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, HH(as_bigfile_bsock_proxy),
       qsize(&as_consumer_buffer_queue));
    dump_consumer_queue();
#endif

   if (finalize) {
      /* This was the last buffer for this big file */
      buffer->final = 1;
   } else {
      buffer->final = 0;
   }

#if ASDEBUG_FI
    Pmsg7(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d FI: %4d parent: %4X bigfile: %4X, final: %d cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer->file_idx, HH(buffer->parent), HH(as_bigfile_bsock_proxy),
	   buffer->final, qsize(&as_consumer_buffer_queue));
#endif

   qinsert(&as_consumer_buffer_queue, &buffer->bq);

#if ASDEBUG_CONS_ENQUEUE
    Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), END   bigfile: %4X, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, HH(as_bigfile_bsock_proxy),
       qsize(&as_consumer_buffer_queue));
    dump_consumer_queue();
#endif

   V(as_consumer_queue_lock);

   /* Let the consumer thread know */
   pthread_cond_signal(&as_consumer_queue_cond);
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
   POOLMEMDUP(sys_fname);
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
   POOLMEMFREE(sys_fname);
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

int AS_ENGINE::as_save_file_schedule(
   JCR *jcr,
   FF_PKT *ff_pkt,
   bool do_plugin_set,
   DIGEST *digest,
   DIGEST *signing_digest,
   int digest_stream,
   bool has_file_data)
{
#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d as_save_file_schedule() file: %s\n", my_thread_id(), ff_pkt->fname);
#endif

   as_save_file_context_t *context = (as_save_file_context_t *)malloc(sizeof(as_save_file_context_t));
   context->jcr = jcr;
   context->ff_pkt = as_new_ff_pkt_clone(ff_pkt);
   context->do_plugin_set = do_plugin_set;
   context->digest = digest;
   context->signing_digest = signing_digest;
   context->digest_stream = digest_stream;
   context->has_file_data = has_file_data;

   workq_add(&as_work_queue, context, NULL, 0);

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

void AS_ENGINE::as_wait_for_consumer_thread_started()
{
   P(as_consumer_thread_lock);
   while (as_consumer_thread_started == false) {
      pthread_cond_wait(&as_consumer_thread_started_cond, &as_consumer_thread_lock);
   }
   V(as_consumer_thread_lock);
}


bool AS_ENGINE::as_quit_consumer_thread_loop()
{
   bool quit = false;

   P(as_consumer_thread_lock);
   quit = as_consumer_thread_quit;
   V(as_consumer_thread_lock);

   return quit;
}

bool AS_ENGINE::as_is_consumer_queue_empty(bool lockme)
{
   bool empty = false;

   if (lockme) {
      P(as_consumer_queue_lock);
   }

   empty = (qsize(&as_consumer_buffer_queue) == 0);

   if (lockme) {
      V(as_consumer_queue_lock);
   }

   return empty;
}

void AS_ENGINE::as_release_buffer(as_buffer_t *buffer)
{
#if ASDEBUG
   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() BEGIN %d (%p), free size: %d \n",
	  my_thread_id(), buffer->id, buffer, qsize(&as_free_buffer_queue));
#endif

   P(as_buffer_lock);

   buffer->size = 0;
   buffer->parent = NULL;
   buffer->final = 0;
   buffer->file_idx = 0;

   if (as_fix_fi_order_buffer == NULL) {
      as_fix_fi_order_buffer = buffer;
   } else if (as_bigfile_buffer_only == NULL) {
      as_bigfile_buffer_only = buffer;
   } else {
      qinsert(&as_free_buffer_queue, &buffer->bq);
   }

   V(as_buffer_lock);

   pthread_cond_broadcast(&as_buffer_cond);

#if ASDEBUG
   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() END %d (%p), free size: %d \n",
      my_thread_id(), buffer->id, buffer, qsize(&as_free_buffer_queue));
#endif
}

static void *as_consumer_thread_loop_wrapper(void *arg)
{
   as_cons_thread_loop_context_t *ctxt = (as_cons_thread_loop_context_t *)arg;

   ctxt->as_engine->as_consumer_thread_loop(ctxt->sd);

   free(ctxt);

   return NULL;
}

void AS_ENGINE::as_consumer_thread_loop(BSOCK *sd)
{
   sd->clear_locking();

#if ASDEBUG_LOOP
   Pmsg3(50, "\t\t>>>> %4d as_consumer_thread_loop() START, sock: %p, as_quit_consumer_thread_loop(): %d\n",
         my_thread_id(), sd, as_quit_consumer_thread_loop());
#endif

   P(as_consumer_thread_lock);
   as_consumer_thread_started = true;
   pthread_cond_signal(&as_consumer_thread_started_cond);
   V(as_consumer_thread_lock);

   as_buffer_t *buffer = NULL;

   /* Somehow mark the variable as invalid, but don't use zero */
#define NEED_TO_INIT INT32_MIN

   int32_t to_send = NEED_TO_INIT;

   while ((as_quit_consumer_thread_loop() == false) ||
      (as_quit_consumer_thread_loop() &&
       (as_is_consumer_queue_empty(true) == false))) {
#if ASDEBUG_LOOP
	  Pmsg1(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION BEGIN\n", my_thread_id());
#endif

      P(as_consumer_queue_lock);

#if ASDEBUG_LOOP
      Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION BEGIN cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));
#endif

      while ((as_quit_consumer_thread_loop() == false) ||
         (as_quit_consumer_thread_loop() &&
          (as_is_consumer_queue_empty(false) == false))) {
         /* Peek at the first elem */
         buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, NULL);

         if (buffer) {
            /*
             * If this buffer refers to the next file index or the current one - ok
             * If not - this buffer has to wait until last file idx is large enough
             */
            if (buffer->file_idx == as_last_file_idx)
            {
               /* OK */
            }
            /* We may only increment if the current big file is done */
            else if ((buffer->file_idx == as_last_file_idx + 1) &&
               (as_bigfile_bsock_proxy == NULL)) {
               ++as_last_file_idx;
               /* OK */
            } else {
               /*
                * buffer->file_idx > last_file_idx + 1
                * We need to wait, find another one
                */
               while (buffer != NULL) {
                  buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, &buffer->bq);

                  if (buffer != NULL) {
                     if (buffer->file_idx == as_last_file_idx) {
                        /* OK */
                        break;
                     } else if ((buffer->file_idx == as_last_file_idx + 1) &&
                        (as_bigfile_bsock_proxy == NULL)) {
                        /* We can only increment if the current big file is done */
                        ++as_last_file_idx;
                        /* OK */
                        break;
                     }
                  }
               }

               /* buffer can be NULL here */
            }

            if (buffer != NULL) {
               /* If this is the first chunk of a bigger file - mark it */
               if (as_bigfile_bsock_proxy == NULL) {
                  as_bigfile_bsock_proxy = buffer->parent;
                  /* Dequeue this buffer */
                  buffer = (as_buffer_t *)qdchain(&buffer->bq);

#if ASDEBUG_LOOP
         Pmsg7(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buf: %d fi: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
            my_thread_id(), buffer->id, buffer->file_idx, buffer->size, HH(buffer->parent), buffer->parent , qsize(&as_consumer_buffer_queue));
#endif
                  ASSERT(buffer);
                  break;
               } else {
                  /* If this is the next chunk of this bigger file */
                  if (as_bigfile_bsock_proxy == buffer->parent) {
                     /* If this is the last chunk of a bigger file - unmark it */
                     if (buffer->final) {
                        as_bigfile_bsock_proxy = NULL;
                     }
                     /* Good to go */
                     buffer = (as_buffer_t *)qdchain(&buffer->bq);

#if ASDEBUG_LOOP
            Pmsg7(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buf: %d fi: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
               my_thread_id(), buffer->id, buffer->file_idx, buffer->size, HH(buffer->parent), buffer->parent , qsize(&as_consumer_buffer_queue));
#endif

                     ASSERT(buffer);
                     break;
                  } else {
                     /*
                      * Cannot process this chunk now, try to find the next
                      * chunk from the current big file
                      */
                     while (buffer != NULL) {
                        buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, &buffer->bq);

                        if ((buffer != NULL) &&
                           (buffer->parent == as_bigfile_bsock_proxy))
                        {
                           /* Found one */
                           break;
                        }
                     }

                     if ((buffer != NULL) &&
                        (buffer->parent == as_bigfile_bsock_proxy)) {
                        /* If this is the last chunk of a bigger file - unmark it */
                        if (buffer->final) {
                           as_bigfile_bsock_proxy = NULL;
                        }
                        /* Good to go */
                        buffer = (as_buffer_t *)qdchain(&buffer->bq);
   #if ASDEBUG_LOOP
            Pmsg7(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buf: %d fi: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
               my_thread_id(), buffer->id, buffer->file_idx, buffer->size, HH(buffer->parent), buffer->parent , qsize(&as_consumer_buffer_queue));
   #endif

                        ASSERT(buffer);
                        break;
                     }
                  }
               }
            } /* buffer was null need to try again */
         }

#if ASDEBUG_LOOP
         Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buf: WAIT, cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));
#endif

         /* Let's wait for 1 second, maybe a free buffer will pop up */
         struct timespec abs_time;
         clock_gettime(CLOCK_REALTIME, &abs_time);
         abs_time.tv_sec += 1;

         pthread_cond_timedwait(&as_consumer_queue_cond, &as_consumer_queue_lock, &abs_time);
      }
      V(as_consumer_queue_lock);

      if (buffer == NULL)
      {
         /* This means we exit the thread loop */
         return;
      }
      else
      {
#if ASDEBUG_LOOP_DEQUEUE
         Pmsg8(50, "\t\t>>>> %4d as_consumer_thread_loop() DQ %d, FI: %d parent: %4X bigfile: %4X, final: %d cons.q.size: %d LAST_FI: %d\n",
            my_thread_id(), buffer->id, buffer->file_idx, HH(buffer->parent), HH(as_bigfile_bsock_proxy),
            buffer->final, qsize(&as_consumer_buffer_queue), as_last_file_idx);
#endif
      }

      ASSERT(buffer);
      ASSERT(buffer->size > 0);
      ASSERT(buffer->size <= AS_BUFFER_CAPACITY);

      /* We've got a buffer, so now we need to send it via sd socket */
      int pos_in_buffer = 0;

#if ASDEBUG_LOOP_SEND
      Pmsg8(50, "\t\t>>>> %4d as_consumer_thread_loop() START SENDING IN THIS ITER buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
         my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

      while (pos_in_buffer < buffer->size) {
         if (to_send == NEED_TO_INIT) {
            memcpy(&to_send, &buffer->data[pos_in_buffer], sizeof(to_send));
            pos_in_buffer += sizeof(to_send);

#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d GET_TO_SEND as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            ASSERT(to_send != 0);
         }

         /* Shouldn't happen */
         if (pos_in_buffer + to_send > buffer->size) {

#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_LESS_B as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            		my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif
            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = (buffer->size - pos_in_buffer);
            sd->send();

            to_send -= (buffer->size - pos_in_buffer);

#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_LESS_E as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            /* End of buffer, need to pick up the next one */
            break;
         }

         if ((to_send < 0) && (to_send != NEED_TO_INIT)) {
            /* A signal */
#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_SIGNAL as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            sd->signal(to_send);
            to_send = NEED_TO_INIT;
         } else if (to_send > 0) {
            /* A normal message */
#if ASDEBUG_LOOP_SEND
            Pmsg8(50, "\t\t>>>> %4d SEND_ENTIRE as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
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
      Pmsg8(50, "\t\t>>>> %4d END SENDING THIS ITER as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

      ASSERT(buffer);
      as_release_buffer(buffer);

#if ASDEBUG_LOOP
      Pmsg1(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION END\n", my_thread_id());
#endif
   }

#if ASDEBUG_LOOP
   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() STOP, sock: %p\n", my_thread_id(), sd);
#endif

   P(as_consumer_thread_lock);
   as_consumer_thread_started = false;
   V(as_consumer_thread_lock);

   sd->set_locking();

   return;
}

#undef NEED_TO_INIT

void AS_ENGINE::as_init_free_buffers_queue()
{
   as_buffer_t *buffer = NULL;
   char *start = NULL;

#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d as_init_free_buffers_queue() size: %d\n",
      my_thread_id(), AS_BUFFERS);
#endif

   for (int i = 0; i < AS_BUFFERS; ++i) {
      buffer = (as_buffer_t *)malloc(sizeof(as_buffer_t));
      buffer->id = AS_BUFFER_BASE + i;
      buffer->size = 0;
      buffer->parent = NULL;
      buffer->final = 0;
      qinsert(&as_free_buffer_queue, &buffer->bq);
   }

   ASSERT(AS_BUFFERS == qsize(&as_free_buffer_queue));

   as_bigfile_buffer_only = (as_buffer_t *)malloc(sizeof(as_buffer_t));
   as_bigfile_buffer_only->id = AS_BUFFER_BASE + 998;

   ASSERT(as_bigfile_buffer_only != NULL);

   as_fix_fi_order_buffer = (as_buffer_t *)malloc(sizeof(as_buffer_t));
   as_fix_fi_order_buffer->id = AS_BUFFER_BASE + 999;

   ASSERT(as_fix_fi_order_buffer != NULL);
}

void AS_ENGINE::as_init_consumer_thread(BSOCK *sd)
{
   P(as_consumer_thread_lock);
   as_consumer_thread_quit = false;
   V(as_consumer_thread_lock);

   as_cons_thread_loop_context_t *ctxt = (as_cons_thread_loop_context_t *)malloc(sizeof(as_cons_thread_loop_context_t));
   ctxt->as_engine = this;
   ctxt->sd = sd;

   pthread_create(&as_consumer_thread, NULL, as_consumer_thread_loop_wrapper, (void *)ctxt);

#if ASDEBUG
   Pmsg3(50, "\t\t>>>> %4d as_init_consumer_thread() id: %4d sock: %p\n",
      my_thread_id(), H(as_consumer_thread), sd);
#endif
}

void AS_ENGINE::as_workqueue_init()
{
#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_init()\n", my_thread_id());
#endif

   workq_init(&as_work_queue, AS_PRODUCER_THREADS, as_workqueue_engine);
}


void AS_ENGINE::as_init(BSOCK *sd, uint32_t buf_size)
{
#if ASDEBUG_INIT_SHUT
   Pmsg2(50, "\t\t>>>> %4d as_init() sock: %p\n", my_thread_id(), sd);
#endif

   /* Store the pointer to the poolmem */
   as_save_msg_pointer = sd->msg;
   as_initial_buf_size = buf_size;

   as_init_free_buffers_queue();
   as_init_consumer_thread(sd);
   as_wait_for_consumer_thread_started();

   as_workqueue_init();
}

uint32_t AS_ENGINE::as_get_initial_bsock_proxy_buf_size()
{
   return as_initial_buf_size;
}

void AS_ENGINE::as_request_consumer_thread_quit()
{
#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_request_consumer_thread_quit()\n", my_thread_id());
#endif

   P(as_consumer_thread_lock);
   as_consumer_thread_quit = true;
   V(as_consumer_thread_lock);
}

void AS_ENGINE::as_join_consumer_thread()
{
   pthread_cond_signal(&as_consumer_queue_cond);

#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_join_consumer_thread()\n", my_thread_id());
#endif

   pthread_join(as_consumer_thread, NULL);
}

void AS_ENGINE::as_dealloc_all_buffers()
{
#if ASDEBUG_DEALLOC_BUFFERS
   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() BEGIN, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue), qsize(&as_consumer_buffer_queue));
#endif

   as_buffer_t *buffer = NULL;

   do {
      buffer = (as_buffer_t *)qremove(&as_free_buffer_queue);

      if (buffer != NULL) {
#if ASDEBUG_DEALLOC_BUFFERS
         Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() FREE buffer: %d (%p)\n",
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
      buffer = (as_buffer_t *)qremove(&as_consumer_buffer_queue);

      if (buffer != NULL) {

#if ASDEBUG_DEALLOC_BUFFERS
         Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() CONS buffer: %d (%p)\n",
   	      my_thread_id(), buffer->id, buffer);
#endif

         /*
          * Smart alloc does not allow free(NULL),
          * which is inconsistent with how free works
          */
         free(buffer);
      }
   } while (buffer != NULL);

   if (as_bigfile_buffer_only != NULL) {
#if ASDEBUG_DEALLOC_BUFFERS
      Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() BIG buffer: %d (%p)\n",
         my_thread_id(), as_bigfile_buffer_only->id, as_bigfile_buffer_only);
#endif

      free(as_bigfile_buffer_only);
      as_bigfile_buffer_only = NULL;
   }

   if (as_fix_fi_order_buffer != NULL) {
#if ASDEBUG_DEALLOC_BUFFERS
      Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() FI FIX buffer: %d (%p)\n",
         my_thread_id(), as_fix_fi_order_buffer->id, as_bigfile_buffer_only);
#endif

      free(as_fix_fi_order_buffer);
      as_fix_fi_order_buffer = NULL;
   }

#if ASDEBUG_DEALLOC_BUFFERS
   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() END, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue), qsize(&as_consumer_buffer_queue));
#endif

   ASSERT(0 == qsize(&as_free_buffer_queue));
   ASSERT(0 == qsize(&as_consumer_buffer_queue));
}

void AS_ENGINE::as_workqueue_destroy()
{
#if ASDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_destroy()\n", my_thread_id());
#endif

   workq_destroy(&as_work_queue);
}

void AS_ENGINE::as_shutdown(BSOCK *sd)
{
#if ASDEBUG_INIT_SHUT
   Pmsg1(50, "\t\t>>>> %4d as_shutdown() BEGIN\n", my_thread_id());
#endif

   as_workqueue_destroy();
   as_request_consumer_thread_quit();
   as_join_consumer_thread();
   as_dealloc_all_buffers();

   /* Restore the pointer to the poolmem */
   sd->msg = as_save_msg_pointer;

   /* Clear remainig members */
   as_consumer_thread_quit = false;
   as_bigfile_bsock_proxy = NULL;
   as_consumer_thread_started = false;
   as_last_file_idx = 0;

#if ASDEBUG_INIT_SHUT
   Pmsg1(50, "\t\t>>>> %4d as_shutdown() END\n", my_thread_id());
#endif
}

int AS_ENGINE::get_comp_idx()
{
   for (int i = 0; i < AS_PRODUCER_THREADS; ++i) {
      if (comp_usage & 1u) {
         comp_usage |= (1u << i);
         return i;
      }
   }

   return -1;
}

void AS_ENGINE::free_comp_idx(int idx)
{
   if ((idx >= 0) && (idx < AS_PRODUCER_THREADS)) {
      comp_usage &= ~(1u << idx);
   }
}

#endif /* AS_BACKUP */
