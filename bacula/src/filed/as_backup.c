#include "bacula.h"
#include "jcr.h"
#include "as_backup.h"





//
// AS TODO
// condition variables for buffers which are ready to be used
// or buffers which can be consumed by the consumer thread
//

//
// Producer related data structures
//

// TODO is one instance enough? Can bacula jobs be scheduled concurently?
static workq_t as_work_queue = { 0 };

// AS TODO tutaj dodać muteksy do danych które mogą być dzielone między workerami
// 1. dla JCR
// ... ?


//
// Consumer related data structures
//
// AS TODO ten muteks jest tylko do wychodzenia z pętli wątku
static pthread_mutex_t as_consumer_thread_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t as_consumer_thread = 0;

static bool as_consumer_thread_quit = false;

static BQUEUE as_consumer_buffer_queue =
{
   &as_consumer_buffer_queue,
   &as_consumer_buffer_queue
};

static AS_BSOCK_PROXY *as_bigfile_bsock_proxy = NULL;



//
// Data structures shared between producer threads and consumer thread
//

// AS TODO use condition variable
// AS TODO dwa muteksy - jeden dla buforów wolnych, drugi dla kolejki dla konsumenta
static pthread_mutex_t as_buffer_lock = PTHREAD_MUTEX_INITIALIZER;

static BQUEUE as_free_buffer_queue =
{
   &as_free_buffer_queue,
   &as_free_buffer_queue
};

//
// Producer loop
//

as_buffer_t *as_acquire_buffer(AS_BSOCK_PROXY *parent)
{
   Pmsg3(50, "\t\t>>>> %X as_acquire_buffer() parent: %p, free buffers: %d\n",
      pthread_self(), parent, qsize(&as_free_buffer_queue));

   as_buffer_t *buffer = NULL;

   /** Wait for a buffer to become available */
   while (as_workqueue_engine_quit() == false)
   {
      pthread_mutex_lock(&as_buffer_lock);
      buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);
      pthread_mutex_unlock(&as_buffer_lock);

      if (buffer)
      {
         buffer->parent = parent;
         buffer->size = 0;

         Pmsg4(50, "\t\t>>>> %X as_acquire_buffer() parent: %p, buffer: %d, free buffers: %d\n",
            pthread_self(), parent, buffer->id, qsize(&as_free_buffer_queue));
         break;
      }
   }

   return buffer;
}

void as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize)
{
	pthread_mutex_lock(&as_buffer_lock);

	if (as_bigfile_bsock_proxy == NULL)
	{
	   as_bigfile_bsock_proxy = buffer->parent;

	   Pmsg3(50, "\t\t>>>> %X as_consumer_enqueue_buffer() %d, NEW bigfile: %X\n",
         pthread_self(), buffer->id, as_bigfile_bsock_proxy);
	   QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}
	else if (as_bigfile_bsock_proxy == buffer->parent)
	{
      if (finalize)
      {
         Pmsg3(50, "\t\t>>>> %X as_consumer_enqueue_buffer() %d, LAST bigfile: %X\n",
            pthread_self(), buffer->id, as_bigfile_bsock_proxy);
      }
      else
      {
         Pmsg3(50, "\t\t>>>> %X as_consumer_enqueue_buffer() %d, CONT bigfile: %X\n",
            pthread_self(), buffer->id, as_bigfile_bsock_proxy);
      }
      // TODO continue this big file
	   // Find the last buffer in the queue which refers to this file
      as_buffer_t *last = NULL;

      do
      {
         last = (as_buffer_t *)qnext(&as_consumer_buffer_queue, &last->bq);

         if (last->parent == as_bigfile_bsock_proxy)
         {
            break;
         }

      } while (last != NULL);

	   // Buffers for this big file can have already been sent, then put this buffer at the beginning of the queue
      // Or are still in the queue, then put the buffer after the last buffer for this file
      qinsert_after(&as_consumer_buffer_queue, &last->bq, &buffer->bq);

      if (finalize)
      {
         // This was the last buffer for this big file
         as_bigfile_bsock_proxy = NULL;
      }
	}
	else // as_bigfile_bsock_proxy != buffer->parent
	{
      Pmsg3(50, "\t\t>>>> %X as_consumer_enqueue_buffer() %d, BLOCKED bigfile: %X\n",
         pthread_self(), buffer->id, as_bigfile_bsock_proxy);
      QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}

	pthread_mutex_unlock(&as_buffer_lock);
}

bool as_workqueue_engine_quit()
{
   bool quit = false;

   P(as_work_queue.mutex);
   quit = as_work_queue.quit;
   V(as_work_queue.mutex);

   return quit;
}

int as_save_file_schedule(
   JCR *jcr,
   FF_PKT *ff_pkt,
   bool do_plugin_set,
   DIGEST *digest,
   DIGEST *signing_digest,
   int digest_stream,
   bool has_file_data)
{
   //// Pmsg1(50, "\t\t>>>> %X as_save_file_schedule()\n", pthread_self());

   as_save_file_context_t *context = (as_save_file_context_t *)malloc(sizeof(as_save_file_context_t));
   context->jcr = jcr;
   context->ff_pkt = ff_pkt;
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

   Pmsg1(50, "\t\t>>>> %X as_workqueue_engine()\n", pthread_self());

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

//
// Consumer loop
//

bool as_quit_consumer_thread_loop()
{
   bool quit = false;
   pthread_mutex_lock(&as_consumer_thread_lock);
   quit = as_consumer_thread_quit;
   pthread_mutex_unlock(&as_consumer_thread_lock);
   return quit;
}

as_buffer_t *as_consumer_dequeue_buffer()
{
   as_buffer_t *buffer = NULL;

   pthread_mutex_lock(&as_buffer_lock);
   buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
   pthread_mutex_unlock(&as_buffer_lock);

   if (buffer)
   {
      Pmsg3(50, "\t\t>>>> %X as_consumer_dequeue_buffer() %d, consumer queue size: %d\n",
         pthread_self(), buffer->id, qsize(&as_consumer_buffer_queue));
   }

   return buffer;
}

void as_release_buffer(as_buffer_t *buffer)
{
   pthread_mutex_lock(&as_buffer_lock);

   buffer->size = 0; // Not really needed
   buffer->parent = NULL; // Not really needed

   QINSERT(&as_free_buffer_queue, &buffer->bq);

   Pmsg3(50, "\t\t>>>> %X as_release_buffer() %d, consumer queue size: %d\n",
      pthread_self(), buffer->id, qsize(&as_free_buffer_queue));

   pthread_mutex_unlock(&as_buffer_lock);
}

void *as_consumer_thread_loop(void *arg)
{
   BSOCK *sd = (BSOCK *)arg;

   Pmsg2(50, "\t\t>>>> %X as_consumer_thread_loop() START, sock: %p\n",
      pthread_self(), sd);

   as_buffer_t *buffer = NULL;

   int32_t to_send = 0;

   while (as_quit_consumer_thread_loop() == false)
   {
      buffer = as_consumer_dequeue_buffer();

      if (buffer == NULL)
      {
    	  // Send queue empty
    	  continue;
      }

      int pos_in_buffer = 0;

      Pmsg6(50, "\t\t>>>> %X as_consumer_thread_loop() buf: %d, tosend: %d, pos: %d, bufsize: %d, qsize: %d\n",
         pthread_self(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

      while (pos_in_buffer < buffer->size)
      {
         if (to_send == 0)
         {
            memcpy(&to_send, &buffer->data[pos_in_buffer], sizeof(to_send));
            pos_in_buffer += sizeof(to_send);

            Pmsg6(50, "\t\t>>>> %X as_consumer_thread_loop() buf: %d, tosend: %d, pos: %d, bufsize: %d, qsize: %d\n",
               pthread_self(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));
         }

         if (pos_in_buffer + to_send > buffer->size)
         {
            Pmsg6(50, "\t\t>>>> %X as_consumer_thread_loop() buf: %d, tosend: %d, pos: %d, bufsize: %d, qsize: %d\n",
               pthread_self(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = (buffer->size - pos_in_buffer);
            sd->send();

            to_send -= (buffer->size - pos_in_buffer);
            // End of buffer, need to pick up the next one
            break;
         }

         if (to_send < 0) // signal
         {
            Pmsg6(50, "\t\t>>>> %X as_consumer_thread_loop() buf: %d, tosend: %d, pos: %d, bufsize: %d, qsize: %d\n",
               pthread_self(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

            sd->signal(to_send);
            to_send = 0;
         }
         else
         {
            Pmsg6(50, "\t\t>>>> %X as_consumer_thread_loop() buf: %d, tosend: %d, pos: %d, bufsize: %d, qsize: %d\n",
               pthread_self(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = to_send;
            sd->send();

            pos_in_buffer += to_send;
            to_send = 0;
         }
      }

      ASSERT(buffer);
      as_release_buffer(buffer);
   }

   Pmsg2(50, "\t\t>>>> %X as_consumer_thread_loop() STOP, sock: %p\n",
      pthread_self(), sd);

   return NULL;
}

//
// Initialization
//
void as_init_free_buffers_queue()
{
   as_buffer_t *buffer = NULL;

   Pmsg2(50, "\t\t>>>> %X as_init_free_buffers_queue() size: %d\n",
      pthread_self(), AS_BUFFERS);

   for (int i = 0; i < AS_BUFFERS; ++i)
   {
      buffer = (as_buffer_t *)malloc(AS_BUFFER_CAPACITY);
      buffer->id = AS_BUFFER_BASE + i;
      QINSERT(&as_free_buffer_queue, &buffer->bq);
   }

   ASSERT(AS_BUFFERS == qsize(&as_free_buffer_queue));
}

void as_init_consumer_thread(BSOCK *sd)
{
	as_consumer_thread_quit = false;

	pthread_create(&as_consumer_thread, NULL, as_consumer_thread_loop, (void *)sd);

   Pmsg3(50, "\t\t>>>> %X as_init_consumer_thread() id: %X sock: %p\n",
      pthread_self(), as_consumer_thread, sd);
}

void as_workqueue_init()
{
   Pmsg1(50, "\t\t>>>> %X as_workqueue_init()\n", pthread_self());

   workq_init(&as_work_queue, AS_PRODUCER_THREADS, as_workqueue_engine);
}

void as_init(BSOCK *sd)
{
   Pmsg2(50, "\t\t>>>> %X as_init() sock: %p\n", pthread_self(), sd);

	as_init_free_buffers_queue();
	as_init_consumer_thread(sd);
	as_workqueue_init();
}

//
// Shutdown
//

void as_request_consumer_thread_quit()
{
   Pmsg1(50, "\t\t>>>> %X as_request_consumer_thread_quit()\n", pthread_self());

   pthread_mutex_lock(&as_consumer_thread_lock);
   as_consumer_thread_quit = true;
   pthread_mutex_unlock(&as_consumer_thread_lock);
}

void as_join_consumer_thread()
{
   Pmsg1(50, "\t\t>>>> %X as_join_consumer_thread()\n", pthread_self());
	pthread_join(as_consumer_thread, NULL);
}

void as_release_remaining_consumer_buffers()
{
	as_buffer_t *buffer = NULL;

	pthread_mutex_lock(&as_buffer_lock);

   Pmsg1(50, "\t\t>>>> %X as_release_remaining_consumer_buffers() START\n", pthread_self());

	do
	{
		buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
		if (buffer != NULL)
		{
		   Pmsg2(50, "\t\t>>>> %X as_release_remaining_consumer_buffers() buffer: %d\n", pthread_self(), buffer->id);
			QINSERT(&as_free_buffer_queue, &buffer->bq);
		}
	} while (buffer != NULL);

	Pmsg3(50, "\t\t>>>> %X as_release_remaining_consumer_buffers() END, consumer queue size %d, free queue size %d\n",
      pthread_self(), qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));

	pthread_mutex_unlock(&as_buffer_lock);

	ASSERT(0 == qsize(&as_consumer_buffer_queue));
}

void as_clear_free_buffers_queue()
{
   Pmsg2(50, "\t\t>>>> %X as_clear_free_buffers_queue() BEGIN, size %d\n",
      pthread_self(), qsize(&as_free_buffer_queue));
   ASSERT(AS_BUFFERS == qsize(&as_free_buffer_queue));

   as_buffer_t *buffer = NULL;
   int buffer_counter = 0;

   do
   {
      buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);
      if (buffer)
      {
         // Smart alloc does not allow free(NULL), which is inconsistent with how free works
         free(buffer);
         ++buffer_counter;
      }
   } while (buffer != NULL);

   Pmsg2(50, "\t\t>>>> %X as_clear_free_buffers_queue() END, size %d\n",
      pthread_self(), qsize(&as_free_buffer_queue));

   ASSERT(0 == qsize(&as_free_buffer_queue));
   ASSERT(buffer_counter == AS_BUFFERS);
}

void as_workqueue_destroy()
{
   Pmsg1(50, "\t\t>>>> %X as_workqueue_destroy()\n", pthread_self());

   workq_destroy(&as_work_queue);
}

void as_shutdown()
{
   Pmsg1(50, "\t\t>>>> %X as_shutdown() BEGIN\n", pthread_self());

   as_workqueue_destroy();

   as_request_consumer_thread_quit();
   as_join_consumer_thread();
   as_release_remaining_consumer_buffers();

   as_clear_free_buffers_queue();

   Pmsg1(50, "\t\t>>>> %X as_shutdown() END\n", pthread_self());
}
