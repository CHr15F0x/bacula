#include "bacula.h"
#include "jcr.h"
#include "as_backup.h"




#include "../findlib/find.h"


#include "as_bsock_proxy.h"




#define STAMP_SIZE 64 // Unused

int stamp_ok(as_buffer_t *buf) // Unused
{
	char *start = &buf->data[AS_BUFFER_CAPACITY];

	for (int i = 0; i < STAMP_SIZE; ++i)
	{
		if ((unsigned char)start[i] != (unsigned char)0xAA)
		{
			return 0;
		}
	}

	return 1;
}

#define CHECK_STAMP(BUF) ASSERT(stamp_ok(BUF));


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
static pthread_mutex_t as_consumer_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t as_consumer_queue_cond = PTHREAD_COND_INITIALIZER;

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
static pthread_cond_t as_buffer_cond = PTHREAD_COND_INITIALIZER;

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
   Pmsg2(50, "\t\t>>>> %4d as_acquire_buffer() BEGIN parent: %4d\n",
      my_thread_id(), parent ? parent->id : -1);

   as_buffer_t *buffer = NULL;

   /** Wait for a buffer to become available */
   // while (as_workqueue_engine_quit() == false) // cancel if job canceled
   P(as_buffer_lock);
   //while (as_workqueue_engine_quit() == false) // TODO check for quit << to nie dzia�a trzeba jako� inaczej sprawdzac
   while (1) // TODO check for quit
   {
      buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);

      if (buffer)
      {
         buffer->parent = parent;
         buffer->size = 0;

         Pmsg3(50, "\t\t>>>> %4d as_acquire_buffer() END parent: %4d, free size: %d \n",
            my_thread_id(), parent ? parent->id : -1, qsize(&as_free_buffer_queue));
         break;
      }
      else
      {
         Pmsg3(50, "\t\t>>>> %4d as_acquire_buffer() WAIT parent: %4d, free size: %d \n",
            my_thread_id(), parent ? parent->id : -1, qsize(&as_free_buffer_queue));

         pthread_cond_wait(&as_buffer_cond, &as_buffer_lock); // timed wait? because of while (1)
      }
   }
   V(as_buffer_lock);

   return buffer;
}

void as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize)
{
	P(as_consumer_queue_lock);

    Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), BEGIN bigfile: %4d, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, as_bigfile_bsock_proxy ? as_bigfile_bsock_proxy->id : -1,
       qsize(&as_consumer_buffer_queue));

	if (as_bigfile_bsock_proxy == NULL)
	{
	   as_bigfile_bsock_proxy = buffer->parent;

      Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), NEW bigfile: %4d, cons.q.size: %d\n",
         my_thread_id(), buffer->id, buffer, as_bigfile_bsock_proxy ? as_bigfile_bsock_proxy->id : -1,
         qsize(&as_consumer_buffer_queue));

	   QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}
	else if (as_bigfile_bsock_proxy == buffer->parent)
	{
      Pmsg6(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), %s bigfile: %4d, cons.q.size: %d\n",
         my_thread_id(), buffer->id, buffer, finalize ? "LAST" : "CONT",
         as_bigfile_bsock_proxy ? as_bigfile_bsock_proxy->id : -1,
         qsize(&as_consumer_buffer_queue));

      // TODO continue this big file
	   // Find the last buffer in the queue which refers to this file
      as_buffer_t *last = NULL;

      do
      {
         last = (as_buffer_t *)qnext(&as_consumer_buffer_queue, last ? &last->bq : NULL);

         if ((last != NULL) && (last->parent == as_bigfile_bsock_proxy))
         {
             Pmsg3(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() last: %d (%p)\n",
                my_thread_id(), last->id, last);

            break;
         }
      } while (last != NULL);

      Pmsg3(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() last: %d (%p)\n",
         my_thread_id(), last ? last->id : -1, last);

      // Buffers for this big file can have already been sent, then put this buffer at the beginning of the queue
      if (last == NULL)
      {
         qinsert_after(&as_consumer_buffer_queue, NULL, &buffer->bq);
      }
      // Or are still in the queue, then put the buffer after the last buffer for this file
      else
      {
         qinsert_after(&as_consumer_buffer_queue, &last->bq, &buffer->bq);
      }

      if (finalize)
      {
         // This was the last buffer for this big file
         as_bigfile_bsock_proxy = NULL;
      }
	}
	else // as_bigfile_bsock_proxy != buffer->parent
	{
      Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), BLOCKED bigfile: %4d, cons.q.size: %d\n",
         my_thread_id(), buffer->id, buffer, as_bigfile_bsock_proxy ? as_bigfile_bsock_proxy->id : -1,
         qsize(&as_consumer_buffer_queue));

	   QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}

    Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), END bigfile: %4d, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, as_bigfile_bsock_proxy ? as_bigfile_bsock_proxy->id : -1,
       qsize(&as_consumer_buffer_queue));

	V(as_consumer_queue_lock);

	pthread_cond_signal(&as_consumer_queue_cond);
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


#if 1
#define STRDUP(X)	if (orig_ff_pkt->X) ff_pkt->X = bstrdup(orig_ff_pkt->X)
#define STRFREE(X)	if (ff_pkt->X) free(ff_pkt->X)
#define POOLMEMDUP(X) if (orig_ff_pkt->X) \
{ \
	ff_pkt->X = get_pool_memory(PM_FNAME); \
	int32_t orig_size = sizeof_pool_memory(orig_ff_pkt->X); \
	if (orig_size != sizeof_pool_memory(ff_pkt->X)) \
	{ \
		ff_pkt->X = realloc_pool_memory(ff_pkt->X, orig_size); \
	} \
}
#define POOLMEMFREE(X) if (ff_pkt->X) free_pool_memory(ff_pkt->X)


// TODO skopiować tylko te atrybuty które są używane w as_save_file i wołanych w
// niej funkcjach
FF_PKT *as_new_ff_pkt_clone(FF_PKT *orig_ff_pkt)
{
   FF_PKT *ff_pkt = (FF_PKT *)bmalloc(sizeof(FF_PKT));
   memcpy(ff_pkt, orig_ff_pkt, sizeof(FF_PKT));

#if 0
   struct s_included_file *included_files_list; /* KLIS TODO check */
   struct s_excluded_file *excluded_files_list; /* KLIS TODO check */
   struct s_excluded_file *excluded_paths_list; /* KLIS TODO check */
   findFILESET *fileset; /* KLIS TODO check */
   /* List of all hard linked files found */
   /* KLIS TODO not that trivial */
   struct f_link **linkhash;          /* hard linked files */
#endif

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

   /* TODO wtf z tymi memberami */
   ff_pkt->included_files_list = NULL;
   ff_pkt->excluded_files_list = NULL;
   ff_pkt->excluded_paths_list = NULL;
   ff_pkt->fileset = NULL;
   ff_pkt->linkhash = NULL;

   //ff_pkt->fname_save = NULL;
   //ff_pkt->link_save = NULL;
   //ff_pkt->ignoredir_fname = NULL;


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
#else



// TODO skopiowa� tylko te atrybuty kt�re s� u�ywane w as_save_file i wo�anych w
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


#endif





















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

bool as_dont_quit_consumer_thread_loop()
{
   bool quit = false;

   P(as_consumer_thread_lock);
   quit = as_consumer_thread_quit;
   V(as_consumer_thread_lock);

   return (!quit);
}

#if 0
bool as_consumer_queue_not_empty()
{
   int size = 0;

   P(as_consumer_queue_lock);
   size = qsize(&as_consumer_buffer_queue);
   V(as_consumer_queue_lock);

   return (size > 0);
}
#endif

as_buffer_t *as_consumer_dequeue_buffer()
{
   as_buffer_t *buffer = NULL;

   P(as_consumer_queue_lock);
   buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);

   Pmsg4(50, "\t\t>>>> %4d as_consumer_dequeue_buffer() %d (%p), cons.q.size: %d\n",
      my_thread_id(), buffer ? buffer->id : -1, buffer, qsize(&as_consumer_buffer_queue));

   V(as_consumer_queue_lock);

   return buffer;
}

void as_release_buffer(as_buffer_t *buffer)
{
   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() BEGIN %d (%p), free size: %d \n",
	  my_thread_id(), buffer->id, buffer, qsize(&as_free_buffer_queue));

	P(as_buffer_lock);

   buffer->size = 0; // Not really needed
   buffer->parent = NULL; // Not really needed

   QINSERT(&as_free_buffer_queue, &buffer->bq);

   V(as_buffer_lock);


   pthread_cond_broadcast(&as_buffer_cond);

   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() END %d (%p), free size: %d \n",
      my_thread_id(), buffer->id, buffer, qsize(&as_free_buffer_queue));
}

pthread_cond_t as_consumer_thread_begin_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t as_consumer_thread_begin_mutex = PTHREAD_MUTEX_INITIALIZER;

//
//
// TODO AS_BSOCK_PROXY - use as_buffers directly! (da sie tak?)
//
//

void *as_consumer_thread_loop(void *arg)
{
   BSOCK *sd = (BSOCK *)arg;

   // Socket is ours now (todo check for sure)
   // sd->clear_locking(); // TODO potrzebne?

   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() START, sock: %p\n", my_thread_id(), sd);

   // signal thread started
   pthread_cond_signal(&as_consumer_thread_begin_cond);

   as_buffer_t *buffer = NULL;

#define NEED_TO_INIT -9999

   int32_t to_send = NEED_TO_INIT;

   while (as_dont_quit_consumer_thread_loop())// && as_consumer_queue_not_empty())
   {
	  Pmsg1(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION BEGIN\n", my_thread_id());

      P(as_consumer_queue_lock);

      Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION BEGIN cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));

      while (as_dont_quit_consumer_thread_loop())
      {
         buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
         if (buffer)
         {
        	Pmsg4(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buffer: %d (%p), cons.q.size: %d\n", my_thread_id(), buffer->id, buffer, qsize(&as_consumer_buffer_queue));
            break;
         }
         else
         {
         	Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buffer: WAIT, cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));
            pthread_cond_wait(&as_consumer_queue_cond, &as_consumer_queue_lock);
         }
      }
      V(as_consumer_queue_lock);

      int pos_in_buffer = 0;

      Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() buf: %4d (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
         my_thread_id(), buffer->id, buffer, to_send, pos_in_buffer, buffer->size);

      while (pos_in_buffer < buffer->size)
      {
         if (to_send == NEED_TO_INIT)
         {
            memcpy(&to_send, &buffer->data[pos_in_buffer], sizeof(to_send));
            pos_in_buffer += sizeof(to_send);

            Pmsg6(50, "\t\t>>>> %4d GET_TO_SEND as_consumer_thread_loop() buf: %4d (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer, to_send, pos_in_buffer, buffer->size);
         }

         if (pos_in_buffer + to_send > buffer->size)
         {
            Pmsg6(50, "\t\t>>>> %4d SEND_LESS_B as_consumer_thread_loop() buf: %4d (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            my_thread_id(), buffer->id, buffer, to_send, pos_in_buffer, buffer->size);

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = (buffer->size - pos_in_buffer);
            sd->send();

            to_send -= (buffer->size - pos_in_buffer);

            Pmsg6(50, "\t\t>>>> %4d SEND_LESS_E as_consumer_thread_loop() buf: %4d (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer, to_send, pos_in_buffer, buffer->size);

            // pos_in_buffer = buffer->size; niepotrzebne
            // End of buffer, need to pick up the next one
            break;
         }

         if (to_send < 0) // signal
         {
            Pmsg6(50, "\t\t>>>> %4d SEND_SIGNAL as_consumer_thread_loop() buf: %4d (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer, to_send, pos_in_buffer, buffer->size);

            sd->signal(to_send);
            to_send = NEED_TO_INIT;
         }
         else if (to_send > 0)
         {
            Pmsg6(50, "\t\t>>>> %4d SEND_ENTIRE as_consumer_thread_loop() buf: %4d (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer, to_send, pos_in_buffer, buffer->size);

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = to_send;
            sd->send();

            pos_in_buffer += to_send;
            to_send = NEED_TO_INIT;
         }

         ASSERT(to_send != 0);
      }

      ASSERT(buffer);
      as_release_buffer(buffer);

#if 0
      P(as_consumer_queue_lock);
      Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION END cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));
      V(as_consumer_queue_lock);
#endif

      Pmsg1(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION END\n", my_thread_id());
   }

   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() STOP, sock: %p\n", my_thread_id(), sd);

   return NULL;
}

#undef NEED_TO_INIT

//
// Initialization
//
void as_init_free_buffers_queue()
{
   as_buffer_t *buffer = NULL;
   char *start = NULL;

   Pmsg2(50, "\t\t>>>> %4d as_init_free_buffers_queue() size: %d\n",
      my_thread_id(), AS_BUFFERS);

   for (int i = 0; i < AS_BUFFERS; ++i)
   {
      buffer = (as_buffer_t *)malloc(sizeof(as_buffer_t) + STAMP_SIZE); // REMOVE STAMP
      start = &buffer->data[AS_BUFFER_CAPACITY]; // Unused
      memset(start, 0xAA, STAMP_SIZE); // Unused

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

void as_wait_consumer_thread_started()
{
   P(as_consumer_thread_begin_mutex);
   while (as_dont_quit_consumer_thread_loop())
   {
      Pmsg1(50, "\t\t>>>> %4d as_wait_consumer_thread_started()\n", my_thread_id());
      pthread_cond_wait(&as_consumer_thread_begin_cond, &as_consumer_thread_begin_mutex);
   }
   V(as_consumer_thread_begin_mutex);
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

	// wait for consumer thread started
	while (as_dont_quit_consumer_thread_loop() == false)
	{}

	// as_wait_consumer_thread_started();
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

   P(as_consumer_thread_lock);
   as_consumer_thread_quit = true;
   V(as_consumer_thread_lock);
}

void as_join_consumer_thread()
{
   Pmsg1(50, "\t\t>>>> %4d as_join_consumer_thread()\n", my_thread_id());
	pthread_join(as_consumer_thread, NULL);
}

void as_dealloc_all_buffers()
{
   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() BEGIN, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue), qsize(&as_consumer_buffer_queue));

   as_buffer_t *buffer = NULL;
   int buffer_counter = 0;

   do
   {
      buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);
      if (buffer)
      {
    	   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() FREE buffer: %d (%p)\n",
    	      my_thread_id(), buffer->id, buffer);

    	  // Smart alloc does not allow free(NULL), which is inconsistent with how free works
         free(buffer);
         ++buffer_counter;
      }
   } while (buffer != NULL);

   do
   {
      buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
      if (buffer)
      {
   	   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() CONS buffer: %d (%p)\n",
   	      my_thread_id(), buffer->id, buffer);

    	  // Smart alloc does not allow free(NULL), which is inconsistent with how free works
         free(buffer);
         ++buffer_counter;
      }
   } while (buffer != NULL);

   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() END, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue), qsize(&as_consumer_buffer_queue));

   ASSERT(0 == qsize(&as_free_buffer_queue));
   ASSERT(0 == qsize(&as_consumer_buffer_queue));
   ASSERT(buffer_counter == AS_BUFFERS);
}




#if 0
void as_release_remaining_consumer_buffers()
{
	as_buffer_t *buffer = NULL;


   Pmsg1(50, "\t\t>>>> %4d as_release_remaining_consumer_buffers() START\n", my_thread_id());

	do
	{
	   P(as_consumer_queue_lock);
	   buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
	   V(as_consumer_queue_lock);

	   if (buffer != NULL)
		{
		   Pmsg2(50, "\t\t>>>> %4d as_release_remaining_consumer_buffers() buffer: %4d (%p)\n", my_thread_id(), buffer->id);

		   P(as_buffer_lock);
		   QINSERT(&as_free_buffer_queue, &buffer->bq);
	      V(as_buffer_lock);
		}
	} while (buffer != NULL);

	Pmsg1(50, "\t\t>>>> %4d as_release_remaining_consumer_buffers() END\n",
      my_thread_id());


	ASSERT(0 == qsize(&as_consumer_buffer_queue));
}

void as_clear_free_buffers_queue()
{
   // No need to lock anymore

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
#endif

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


//   as_release_remaining_consumer_buffers();
//   as_clear_free_buffers_queue();

   as_dealloc_all_buffers();

   // Restore the pointer to the poolmem
   sd->msg = as_save_msg_pointer;

   Pmsg1(50, "\t\t>>>> %4d as_shutdown() END\n", my_thread_id());
}
