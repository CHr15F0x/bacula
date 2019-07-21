#include "bacula.h"
#include "jcr.h"
#include "as_backup.h"




#include "../findlib/find.h"


#include "as_bsock_proxy.h"


int my_thread_id()
{
   return H(pthread_self());
}



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
   if (parent)
   {
      Pmsg4(50, "\t\t>>>> %4d as_acquire_buffer() BEGIN parent: %4d, cons.q.size: %d, free size: %d \n",
         my_thread_id(), parent->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
   }
   else
   {
      Pmsg3(50, "\t\t>>>> %4d as_acquire_buffer() BEGIN parent: null, cons.q.size: %d, free size: %d \n",
         my_thread_id(), qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
   }


   as_buffer_t *buffer = NULL;

   /** Wait for a buffer to become available */
   // while (as_workqueue_engine_quit() == false) // cancel if job canceled
   while (1)
   {
      pthread_mutex_lock(&as_buffer_lock);
      buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);
      pthread_mutex_unlock(&as_buffer_lock);

      if (buffer)
      {
         buffer->parent = parent;
         buffer->size = 0;

         if (parent)
         {
            Pmsg4(50, "\t\t>>>> %4d as_acquire_buffer() END parent: %4d, cons.q.size: %d, free size: %d \n",
               my_thread_id(), parent->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
         }
         else
         {
            Pmsg3(50, "\t\t>>>> %4d as_acquire_buffer() END parent: null, cons.q.size: %d, free size: %d \n",
               my_thread_id(), qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
         }
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

	   if (as_bigfile_bsock_proxy)
	   {
	      Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, NEW bigfile: %4d, cons.q.size: %d, free size: %d\n",
	         my_thread_id(), buffer->id, as_bigfile_bsock_proxy->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
	   }
	   else
	   {
         Pmsg4(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, NEW bigfile: null, cons.q.size: %d, free size: %d\n",
            my_thread_id(), buffer->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
	   }

	   QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}
	else if (as_bigfile_bsock_proxy == buffer->parent)
	{
      if (finalize)
      {
         if (as_bigfile_bsock_proxy)
         {
            Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, LAST bigfile: %4d, cons.q.size: %d, free size: %d\n",
               my_thread_id(), buffer->id, as_bigfile_bsock_proxy->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
         }
         else
         {
            Pmsg4(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, LAST bigfile: null, cons.q.size: %d, free size: %d\n",
               my_thread_id(), buffer->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
         }
      }
      else
      {
         if (as_bigfile_bsock_proxy)
         {
            Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, CONT bigfile: %4d, cons.q.size: %d, free size: %d\n",
               my_thread_id(), buffer->id, as_bigfile_bsock_proxy->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
         }
         else
         {
            Pmsg4(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, CONT bigfile: null, cons.q.size: %d, free size: %d\n",
               my_thread_id(), buffer->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
         }
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
	   if (as_bigfile_bsock_proxy)
	   {
	      Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, BLOCKED bigfile: %4d, cons.q.size: %d, free size: %d\n",
	         my_thread_id(), buffer->id, as_bigfile_bsock_proxy->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
	   }
	   else
	   {
	      Pmsg4(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, BLOCKED bigfile: null, cons.q.size: %d, free size: %d\n",
	         my_thread_id(), buffer->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
	   }

	   QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}

	pthread_mutex_unlock(&as_buffer_lock);
}

int as_workqueue_engine_quit()
{
   int quit = 0;

   P(as_work_queue.mutex);
   quit = as_work_queue.quit;
   V(as_work_queue.mutex);

   return quit;
}

#if 0
struct FF_PKT {
   char *top_fname;                   /* full filename before descending */
   char *fname;                       /* full filename */
   char *link;                        /* link if file linked */
   char *object_name;                 /* Object name */
   char *object;                      /* restore object */
   char *plugin;                      /* Current Options{Plugin=} name */
   POOLMEM *sys_fname;                /* system filename */
   POOLMEM *fname_save;               /* save when stripping path */
   POOLMEM *link_save;                /* save when stripping path */
   POOLMEM *ignoredir_fname;          /* used to ignore directories */
   char *digest;                      /* set to file digest when the file is a hardlink */
   struct stat statp;                 /* stat packet */
   uint32_t digest_len;               /* set to the digest len when the file is a hardlink*/
   int32_t digest_stream;             /* set to digest type when the file is hardlink */
   int32_t FileIndex;                 /* FileIndex of this file */
   int32_t LinkFI;                    /* FileIndex of main hard linked file */
   int32_t delta_seq;                 /* Delta Sequence number */
   int32_t object_index;              /* Object index */
   int32_t object_len;                /* Object length */
   int32_t object_compression;        /* Type of compression for object */
   struct f_link *linked;             /* Set if this file is hard linked */
   int type;                          /* FT_ type from above */
   int ff_errno;                      /* errno */
   BFILE bfd;                         /* Bacula file descriptor */
   time_t save_time;                  /* start of incremental time */
   bool accurate_found;               /* Found in the accurate hash (valid after check_changes()) */
   bool dereference;                  /* follow links (not implemented) */
   bool null_output_device;           /* using null output device */
   bool incremental;                  /* incremental save */
   bool no_read;                      /* Do not read this file when using Plugin */
   char VerifyOpts[20];
   char AccurateOpts[20];
   char BaseJobOpts[20];
   struct s_included_file *included_files_list;
   struct s_excluded_file *excluded_files_list;
   struct s_excluded_file *excluded_paths_list;
   findFILESET *fileset;
   int (*file_save)(JCR *, FF_PKT *, bool); /* User's callback */
   int (*plugin_save)(JCR *, FF_PKT *, bool); /* User's callback */
   bool (*check_fct)(JCR *, FF_PKT *); /* optionnal user fct to check file changes */

   /* Values set by accept_file while processing Options */
   uint32_t flags;                    /* backup options */
   uint32_t Compress_algo;            /* compression algorithm. 4 letters stored as an interger */
   int Compress_level;                /* compression level */
   int strip_path;                    /* strip path count */
   bool cmd_plugin;                   /* set if we have a command plugin */
   bool opt_plugin;                   /* set if we have an option plugin */
   alist fstypes;                     /* allowed file system types */
   alist drivetypes;                  /* allowed drive types */

   /* List of all hard linked files found */
   struct f_link **linkhash;          /* hard linked files */

   /* Darwin specific things.
    * To avoid clutter, we always include rsrc_bfd and volhas_attrlist */
   BFILE rsrc_bfd;                    /* fd for resource forks */
   bool volhas_attrlist;              /* Volume supports getattrlist() */
   struct HFSPLUS_INFO hfsinfo;       /* Finder Info and resource fork size */
};
#endif

// TODO skopiować tylko te atrybuty które są używane w as_save_file i wołanych w
// niej funkcjach
FF_PKT *as_new_ff_pkt_clone(FF_PKT *orig_ff_pkt)
{
   FF_PKT *ff_pkt = (FF_PKT *)bmalloc(sizeof(FF_PKT));
   memcpy(ff_pkt, orig_ff_pkt, sizeof(FF_PKT));
   ff_pkt->fname = bstrdup(orig_ff_pkt->fname);
   ff_pkt->link = bstrdup(orig_ff_pkt->link);
   ff_pkt->sys_fname = get_pool_memory(PM_FNAME);
   ff_pkt->included_files_list = NULL;
   ff_pkt->excluded_files_list = NULL;
   ff_pkt->excluded_paths_list = NULL;
   ff_pkt->linkhash = NULL;
   ff_pkt->fname_save = NULL;
   ff_pkt->link_save = NULL;
   ff_pkt->ignoredir_fname = NULL;
   return ff_pkt;
}

void as_free_ff_pkt_clone(FF_PKT *ff_pkt)
{
   free(ff_pkt->fname);
   free(ff_pkt->link);
   free_pool_memory(ff_pkt->sys_fname);
   if (ff_pkt->fname_save) {
      free_pool_memory(ff_pkt->fname_save);
   }
   if (ff_pkt->link_save) {
      free_pool_memory(ff_pkt->link_save);
   }
   if (ff_pkt->ignoredir_fname) {
      free_pool_memory(ff_pkt->ignoredir_fname);
   }
   free(ff_pkt);
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
   Pmsg2(50, "\t\t>>>> %4d as_save_file_schedule() file: %s\n", my_thread_id(), ff_pkt->fname);

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

   Pmsg1(50, "\t\t>>>> %4d as_workqueue_engine()\n", my_thread_id());

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

   /* Only quit loop if quit requested and all data sent to socket */
   return quit && (qsize(&as_consumer_buffer_queue) == 0);
}

as_buffer_t *as_consumer_dequeue_buffer()
{
   as_buffer_t *buffer = NULL;

   pthread_mutex_lock(&as_buffer_lock);
   buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
   pthread_mutex_unlock(&as_buffer_lock);

   if (buffer)
   {
      Pmsg4(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d, cons.q.size: %d, free size: %d\n",
         my_thread_id(), buffer->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));
   }

   return buffer;
}

void as_release_buffer(as_buffer_t *buffer)
{
   pthread_mutex_lock(&as_buffer_lock);

   buffer->size = 0; // Not really needed
   buffer->parent = NULL; // Not really needed

   QINSERT(&as_free_buffer_queue, &buffer->bq);

   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() %d, cons.q.size: %d, free size: %d \n",
      my_thread_id(), buffer->id, qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));

   pthread_mutex_unlock(&as_buffer_lock);
}

void *as_consumer_thread_loop(void *arg)
{
   BSOCK *sd = (BSOCK *)arg;

   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() START, sock: %p\n",
      my_thread_id(), sd);

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

      Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() buf: %4d, tosend: %4d, pos: %4d, bufsize: %4d, cons.q.size: %4d\n",
         my_thread_id(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

      while (pos_in_buffer < buffer->size)
      {
         if (to_send == 0)
         {
            memcpy(&to_send, &buffer->data[pos_in_buffer], sizeof(to_send));
            pos_in_buffer += sizeof(to_send);

            Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() buf: %4d, tosend: %4d, pos: %4d, bufsize: %4d, cons.q.size: %4d\n",
               my_thread_id(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));
         }

         if (pos_in_buffer + to_send > buffer->size)
         {
            Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() buf: %4d, tosend: %4d, pos: %4d, bufsize: %4d, cons.q.size: %4d\n",
               my_thread_id(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = (buffer->size - pos_in_buffer);
            sd->send();

            to_send -= (buffer->size - pos_in_buffer);
            // End of buffer, need to pick up the next one
            break;
         }

         if (to_send < 0) // signal
         {
            Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() buf: %4d, tosend: %4d, pos: %4d, bufsize: %4d, cons.q.size: %4d\n",
               my_thread_id(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

            sd->signal(to_send);
            to_send = 0;
         }
         else
         {
            Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() buf: %4d, tosend: %4d, pos: %4d, bufsize: %4d, cons.q.size: %4d\n",
               my_thread_id(), buffer->id, to_send, pos_in_buffer, buffer->size, qsize(&as_consumer_buffer_queue));

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

   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() STOP, sock: %p\n",
      my_thread_id(), sd);

   return NULL;
}

//
// Initialization
//
void as_init_free_buffers_queue()
{
   as_buffer_t *buffer = NULL;

   Pmsg2(50, "\t\t>>>> %4d as_init_free_buffers_queue() size: %d\n",
      my_thread_id(), AS_BUFFERS);

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

   Pmsg3(50, "\t\t>>>> %4d as_init_consumer_thread() id: %4d sock: %p\n",
      my_thread_id(), H(as_consumer_thread), sd);
}

void as_workqueue_init()
{
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_init()\n", my_thread_id());

   workq_init(&as_work_queue, AS_PRODUCER_THREADS, as_workqueue_engine);
}


static POOLMEM *as_save_msg_pointer = NULL;
static uint32_t as_initial_buf_size = 0;

void as_init(BSOCK *sd, uint32_t buf_size)
{
   Pmsg2(50, "\t\t>>>> %4d as_init() sock: %p\n", my_thread_id(), sd);

   // Store the pointer to the poolmem
   as_save_msg_pointer = sd->msg;
   as_initial_buf_size = buf_size;

	as_init_free_buffers_queue();
	as_init_consumer_thread(sd);
	as_workqueue_init();
}

uint32_t as_get_initial_bsock_proxy_buf_size()
{
   return as_initial_buf_size;
}

//
// Shutdown
//

void as_request_consumer_thread_quit()
{
   Pmsg1(50, "\t\t>>>> %4d as_request_consumer_thread_quit()\n", my_thread_id());

   pthread_mutex_lock(&as_consumer_thread_lock);
   as_consumer_thread_quit = true;
   pthread_mutex_unlock(&as_consumer_thread_lock);
}

void as_join_consumer_thread()
{
   Pmsg1(50, "\t\t>>>> %4d as_join_consumer_thread()\n", my_thread_id());
	pthread_join(as_consumer_thread, NULL);
}

void as_release_remaining_consumer_buffers()
{
	as_buffer_t *buffer = NULL;

	pthread_mutex_lock(&as_buffer_lock);

   Pmsg1(50, "\t\t>>>> %4d as_release_remaining_consumer_buffers() START\n", my_thread_id());

	do
	{
		buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
		if (buffer != NULL)
		{
		   Pmsg2(50, "\t\t>>>> %4d as_release_remaining_consumer_buffers() buffer: %4d\n", my_thread_id(), buffer->id);
			QINSERT(&as_free_buffer_queue, &buffer->bq);
		}
	} while (buffer != NULL);

	Pmsg3(50, "\t\t>>>> %4d as_release_remaining_consumer_buffers() END, cons.q.size %4d, free size %4d\n",
      my_thread_id(), qsize(&as_consumer_buffer_queue), qsize(&as_free_buffer_queue));

	pthread_mutex_unlock(&as_buffer_lock);

	ASSERT(0 == qsize(&as_consumer_buffer_queue));
}

void as_clear_free_buffers_queue()
{
   Pmsg2(50, "\t\t>>>> %4d as_clear_free_buffers_queue() BEGIN, size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue));
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

   Pmsg2(50, "\t\t>>>> %4d as_clear_free_buffers_queue() END, size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue));

   ASSERT(0 == qsize(&as_free_buffer_queue));
   ASSERT(buffer_counter == AS_BUFFERS);
}

void as_workqueue_destroy()
{
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_destroy()\n", my_thread_id());

   workq_destroy(&as_work_queue);
}

void as_shutdown(BSOCK *sd)
{
   Pmsg1(50, "\t\t>>>> %4d as_shutdown() BEGIN\n", my_thread_id());

   as_workqueue_destroy();

   as_request_consumer_thread_quit();
   as_join_consumer_thread();
   as_release_remaining_consumer_buffers();

   as_clear_free_buffers_queue();

   // Restore the pointer to the poolmem
   sd->msg = as_save_msg_pointer;

   Pmsg1(50, "\t\t>>>> %4d as_shutdown() END\n", my_thread_id());
}
