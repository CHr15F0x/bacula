#include "bacula.h"
#include "jcr.h"
#include "as_backup.h"




#include "../findlib/find.h"


#include "as_bsock_proxy.h"



#define KLDEBUG 0
#define KLDEBUG_LOOP 0
#define KLDEBUG_CONS_ENQUEUE 1



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
static as_buffer_t *as_bigfile_buffer_only = NULL;



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
#if KLDEBUG
   Pmsg2(50, "\t\t>>>> %4d as_acquire_buffer() BEGIN parent: %4X\n",
      my_thread_id(), HH(parent));
#endif

   as_buffer_t *buffer = NULL;

   /** Wait for a buffer to become available */
   P(as_buffer_lock);
   while (1) // TODO check for quit
   {
      P(as_consumer_queue_lock);
      if ((as_bigfile_bsock_proxy != NULL) && (as_bigfile_bsock_proxy == parent))
      {
         buffer = as_bigfile_buffer_only; // Can already be null
         as_bigfile_buffer_only = NULL;
         V(as_consumer_queue_lock);
      }
      else
      {
         V(as_consumer_queue_lock);
         buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);
      }

      if (buffer)
      {
         buffer->parent = parent;
         buffer->size = 0;

#if KLDEBUG
         Pmsg4(50, "\t\t>>>> %4d as_acquire_buffer() END parent: %4X, free size: %d, buf: %d\n",
            my_thread_id(), HH(parent), qsize(&as_free_buffer_queue), buffer->id);
#endif

         break;
      }
      else
      {

#if KLDEBUG
         P(as_consumer_queue_lock);
         Pmsg4(50, "\t\t>>>> %4d as_acquire_buffer() WAIT as_bigfile_bsock_proxy: %4X parent: %4X, free size: %d \n",
            my_thread_id(), HH(as_bigfile_bsock_proxy), HH(parent), qsize(&as_free_buffer_queue));
         V(as_consumer_queue_lock);
#endif

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

void dump_consumer_queue()
{
#if KLDEBUG_CONS_ENQUEUE

   as_buffer_t *buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, NULL);

   int cnt = 0;

   while (buffer)
   {
      Pmsg6(50, "\t\t>>>> %4d CONSUMER_Q[%d] %d (%p) parent: %4X, curr_bigfile: %4X\n",
         my_thread_id(), cnt, buffer->id, buffer, buffer ? HH(buffer->parent) : 0, HH(as_bigfile_bsock_proxy));

      buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, &buffer->bq);
      ++cnt;
   }
#endif
}

void as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize)
{
	P(as_consumer_queue_lock);

#if KLDEBUG_CONS_ENQUEUE
    Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), BEGIN bigfile: %4X, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, HH(as_bigfile_bsock_proxy),
       qsize(&as_consumer_buffer_queue));
    dump_consumer_queue();
#endif

	if (as_bigfile_bsock_proxy == NULL)
	{
	   as_bigfile_bsock_proxy = buffer->parent;

#if KLDEBUG_CONS_ENQUEUE
	   Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), NEW   bigfile: %4X, cons.q.size: %d\n",
         my_thread_id(), buffer->id, buffer, HH(as_bigfile_bsock_proxy),
         qsize(&as_consumer_buffer_queue));
#endif

	   if (as_bigfile_bsock_proxy != NULL)
	   {
	      // Push the first chunk of the first big file at the beginning
	      qinsert_after(&as_consumer_buffer_queue, NULL, &buffer->bq);
	   }
	   else
	   {
	      // Ordinary small file - at the end
	      QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	   }
	}
	else if (as_bigfile_bsock_proxy == buffer->parent)
	{
#if KLDEBUG_CONS_ENQUEUE
      Pmsg6(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), %s bigfile: %4X, cons.q.size: %d\n",
         my_thread_id(), buffer->id, buffer, finalize ? "LAST " : "CONT ",
         HH(as_bigfile_bsock_proxy),
         qsize(&as_consumer_buffer_queue));
#endif

      // TODO continue this big file
	   // Find the last buffer in the queue which refers to this file
      as_buffer_t *last = NULL;

      do
      {
         last = (as_buffer_t *)qnext(&as_consumer_buffer_queue, last ? &last->bq : NULL);

         if ((last != NULL) && (last->parent == as_bigfile_bsock_proxy))
         {
#if KLDEBUG_CONS_ENQUEUE
            Pmsg3(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() last: %d (%p)\n",
                my_thread_id(), last->id, last);
#endif
            break;
         }
      } while (last != NULL);

#if KLDEBUG_CONS_ENQUEUE
      Pmsg3(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() last: %d (%p)\n",
         my_thread_id(), last ? last->id : -1, last);
#endif

      // Buffers for this big file can have already been sent or this is the first one
      // ,then put this buffer at the beginning of the queue
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
#if KLDEBUG_CONS_ENQUEUE
	   Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), BLOCK bigfile: %4X, cons.q.size: %d\n",
         my_thread_id(), buffer->id, buffer, HH(as_bigfile_bsock_proxy),
         qsize(&as_consumer_buffer_queue));
#endif

	   QINSERT(&as_consumer_buffer_queue, &buffer->bq);
	}

#if KLDEBUG_CONS_ENQUEUE
    Pmsg5(50, "\t\t>>>> %4d as_consumer_enqueue_buffer() %d (%p), END   bigfile: %4X, cons.q.size: %d\n",
       my_thread_id(), buffer->id, buffer, HH(as_bigfile_bsock_proxy),
       qsize(&as_consumer_buffer_queue));
    dump_consumer_queue();
#endif

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
#if KLDEBUG
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

#if KLDEBUG
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

void as_release_buffer(as_buffer_t *buffer)
{
#if KLDEBUG
   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() BEGIN %d (%p), free size: %d \n",
	  my_thread_id(), buffer->id, buffer, qsize(&as_free_buffer_queue));
#endif

	P(as_buffer_lock);

   buffer->size = 0;
   buffer->parent = NULL;
#if 0
   BQUEUE bq;
   char data[AS_BUFFER_CAPACITY];
   int32_t size;
   int id; // For testing
   AS_BSOCK_PROXY *parent; /** Only set when a total file trasfer size is bigger than one buffer */
#endif

   if (as_bigfile_buffer_only == NULL)
   {
      as_bigfile_buffer_only = buffer;
   }
   else
   {
      QINSERT(&as_free_buffer_queue, &buffer->bq);
   }

   V(as_buffer_lock);

   pthread_cond_broadcast(&as_buffer_cond);

#if KLDEBUG
   Pmsg4(50, "\t\t>>>> %4d as_release_buffer() END %d (%p), free size: %d \n",
      my_thread_id(), buffer->id, buffer, qsize(&as_free_buffer_queue));
#endif

}

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

#if KLDEBUG_LOOP
   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() START, sock: %p\n", my_thread_id(), sd);
#endif

   as_buffer_t *buffer = NULL;

#define NEED_TO_INIT -9999

   int32_t to_send = NEED_TO_INIT;

   while (as_dont_quit_consumer_thread_loop())// && as_consumer_queue_not_empty())
   {
#if KLDEBUG_LOOP
	  Pmsg1(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION BEGIN\n", my_thread_id());
#endif

      P(as_consumer_queue_lock);

#if KLDEBUG_LOOP
      Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION BEGIN cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));
#endif

      while (as_dont_quit_consumer_thread_loop())
      {
         // Peek at the first element
         buffer = (as_buffer_t *)qnext(&as_consumer_buffer_queue, NULL);
         if (buffer)
         {
            // Check if we are in the middle of sending a big file
            if (as_bigfile_bsock_proxy != NULL)
            // in the middle of a Big file
            {
               // Only get a buffer if it is a continuation of a big file
               if (buffer->parent == as_bigfile_bsock_proxy)
               {
                  buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
                  if (buffer)
                  {
   #if KLDEBUG_LOOP
                     Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE BIGFILE buf: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
                     my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent , qsize(&as_consumer_buffer_queue));
   #endif

                     break;
                  }
               }
            }
            // No big file currently served
            else
            {
               // Get the buffer from the queue
               buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
               if (buffer)
               {
   #if KLDEBUG_LOOP
               Pmsg6(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE SMALL buf: %d bufsize: %d parent: %4X (%p), cons.q.size: %d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent , qsize(&as_consumer_buffer_queue));
   #endif
                  break;
               }
            }
         }

#if KLDEBUG_LOOP
         Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() DEQUEUE buf: WAIT, cons.q.size: %d\n", my_thread_id(), qsize(&as_consumer_buffer_queue));
#endif

#if 0
         struct timespec abs_time;
         clock_gettime(CLOCK_REALTIME, &abs_time);
         abs_time.tv_sec += 1;

         pthread_cond_timedwait(&as_consumer_queue_cond, &as_consumer_queue_lock, &abs_time);
#else
         pthread_cond_wait(&as_consumer_queue_cond, &as_consumer_queue_lock);
#endif
      }
      V(as_consumer_queue_lock);

      if (as_dont_quit_consumer_thread_loop() == false)
      {
         if (buffer)
         {
            as_release_buffer(buffer);
         }

         return NULL;
      }

      ASSERT(buffer);
      ASSERT(buffer->size > 0);
      ASSERT(buffer->size <= AS_BUFFER_CAPACITY);

      int pos_in_buffer = 0;

#if KLDEBUG_LOOP
      Pmsg8(50, "\t\t>>>> %4d as_consumer_thread_loop() START SENDING IN THIS ITER buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
         my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

      while (pos_in_buffer < buffer->size)
      {
         if (to_send == NEED_TO_INIT)
         {
            memcpy(&to_send, &buffer->data[pos_in_buffer], sizeof(to_send));
            pos_in_buffer += sizeof(to_send);

#if KLDEBUG_LOOP
            Pmsg8(50, "\t\t>>>> %4d GET_TO_SEND as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
               my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

//
// TODO tutaj sa errory !!!!
//
            ASSERT(to_send != 0);
         }

         // Shouldnt happen
         if (pos_in_buffer + to_send > buffer->size)
         {

#if KLDEBUG_LOOP
            Pmsg8(50, "\t\t>>>> %4d SEND_LESS_B as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            		my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            sd->msg = &buffer->data[pos_in_buffer];
            sd->msglen = (buffer->size - pos_in_buffer);
            sd->send();

            to_send -= (buffer->size - pos_in_buffer);

#if KLDEBUG_LOOP
            Pmsg8(50, "\t\t>>>> %4d SEND_LESS_E as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            // pos_in_buffer = buffer->size; niepotrzebne
            // End of buffer, need to pick up the next one
            break;
         }

         if ((to_send < 0) && (to_send != NEED_TO_INIT)) // signal
         {
#if KLDEBUG_LOOP
            Pmsg8(50, "\t\t>>>> %4d SEND_SIGNAL as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
                  my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

            sd->signal(to_send);
            to_send = NEED_TO_INIT;
         }
         else if (to_send > 0)
         {
#if KLDEBUG_LOOP
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

#if KLDEBUG_LOOP
      Pmsg8(50, "\t\t>>>> %4d END SENDING THIS ITER as_consumer_thread_loop() buf: %4d bufsize: %d parent: %4X (%p), tosend: %4d, pos: %4d, bufsize: %4d\n",
            my_thread_id(), buffer->id, buffer->size, HH(buffer->parent), buffer->parent, to_send, pos_in_buffer, buffer->size);
#endif

      ASSERT(buffer);
      as_release_buffer(buffer);

#if KLDEBUG_LOOP
      Pmsg1(50, "\t\t>>>> %4d as_consumer_thread_loop() ITERATION END\n", my_thread_id());
#endif

   }

#if KLDEBUG_LOOP
   Pmsg2(50, "\t\t>>>> %4d as_consumer_thread_loop() STOP, sock: %p\n", my_thread_id(), sd);
#endif

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

#if KLDEBUG
   Pmsg2(50, "\t\t>>>> %4d as_init_free_buffers_queue() size: %d\n",
      my_thread_id(), AS_BUFFERS);
#endif

   for (int i = 0; i < AS_BUFFERS; ++i)
   {
      buffer = (as_buffer_t *)malloc(sizeof(as_buffer_t));
      buffer->id = AS_BUFFER_BASE + i;
      QINSERT(&as_free_buffer_queue, &buffer->bq);
   }

   ASSERT(AS_BUFFERS == qsize(&as_free_buffer_queue));

   as_bigfile_buffer_only = (as_buffer_t *)malloc(sizeof(as_buffer_t));
   as_bigfile_buffer_only->id = AS_BUFFER_BASE + 999;

   ASSERT(as_bigfile_buffer_only != NULL);
}

void as_init_consumer_thread(BSOCK *sd)
{
	as_consumer_thread_quit = false;

	pthread_create(&as_consumer_thread, NULL, as_consumer_thread_loop, (void *)sd);

#if KLDEBUG
   Pmsg3(50, "\t\t>>>> %4d as_init_consumer_thread() id: %4d sock: %p\n",
      my_thread_id(), H(as_consumer_thread), sd);
#endif

}

void as_workqueue_init()
{
#if KLDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_init()\n", my_thread_id());
#endif

   workq_init(&as_work_queue, AS_PRODUCER_THREADS, as_workqueue_engine);
}


static POOLMEM *as_save_msg_pointer = NULL;
static uint32_t as_initial_buf_size = 0;

void as_init(BSOCK *sd, uint32_t buf_size)
{
#if KLDEBUG
   Pmsg2(50, "\t\t>>>> %4d as_init() sock: %p\n", my_thread_id(), sd);
#endif

   // Store the pointer to the poolmem
   as_save_msg_pointer = sd->msg;
   as_initial_buf_size = buf_size;

	as_init_free_buffers_queue();
	as_init_consumer_thread(sd);

	// wait for consumer thread started
	while (as_dont_quit_consumer_thread_loop() == false)
	{}

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
#if KLDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_request_consumer_thread_quit()\n", my_thread_id());
#endif


   P(as_consumer_thread_lock);
   as_consumer_thread_quit = true;
   V(as_consumer_thread_lock);
}

void as_join_consumer_thread()
{
   pthread_cond_signal(&as_consumer_queue_cond);


#if KLDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_join_consumer_thread()\n", my_thread_id());
#endif


	pthread_join(as_consumer_thread, NULL);
}

void as_dealloc_all_buffers()
{
#if KLDEBUG
   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() BEGIN, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue), qsize(&as_consumer_buffer_queue));
#endif


   as_buffer_t *buffer = NULL;

   do
   {
      buffer = (as_buffer_t *)QREMOVE(&as_free_buffer_queue);
      if (buffer)
      {
#if KLDEBUG
         Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() FREE buffer: %d (%p)\n",
    	      my_thread_id(), buffer->id, buffer);
#endif


    	  // Smart alloc does not allow free(NULL), which is inconsistent with how free works
         free(buffer);
      }
   } while (buffer != NULL);

   do
   {
      buffer = (as_buffer_t *)QREMOVE(&as_consumer_buffer_queue);
      if (buffer)
      {
#if KLDEBUG
         Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() CONS buffer: %d (%p)\n",
   	      my_thread_id(), buffer->id, buffer);
#endif


    	  // Smart alloc does not allow free(NULL), which is inconsistent with how free works
         free(buffer);
      }
   } while (buffer != NULL);

   if (as_bigfile_buffer_only)
   {
#if KLDEBUG
      Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() BIG buffer: %d (%p)\n",
         my_thread_id(), as_bigfile_buffer_only->id, as_bigfile_buffer_only);
#endif


      free(as_bigfile_buffer_only);
   }

#if KLDEBUG
   Pmsg3(50, "\t\t>>>> %4d as_dealloc_all_buffers() END, free size %4d consumer size %4d\n",
      my_thread_id(), qsize(&as_free_buffer_queue), qsize(&as_consumer_buffer_queue));
#endif

   ASSERT(0 == qsize(&as_free_buffer_queue));
   ASSERT(0 == qsize(&as_consumer_buffer_queue));
}

void as_workqueue_destroy()
{
#if KLDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_workqueue_destroy()\n", my_thread_id());
#endif

   workq_destroy(&as_work_queue);
}

void as_shutdown(BSOCK *sd)
{
#if KLDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_shutdown() BEGIN\n", my_thread_id());
#endif

   as_workqueue_destroy();

   as_request_consumer_thread_quit();
   as_join_consumer_thread();
   as_dealloc_all_buffers();

   // Restore the pointer to the poolmem
   sd->msg = as_save_msg_pointer;

#if KLDEBUG
   Pmsg1(50, "\t\t>>>> %4d as_shutdown() END\n", my_thread_id());
#endif
}
