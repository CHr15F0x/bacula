#ifndef __AS_BACKUP_H
#define __AS_BACKUP_H

/**
 * Async backup configuration: number of worker threads
 *
 * Number of threads used to read files concurrently in
 * backup.c/blast_data_to_storage_daemon()
 */
#define AS_PRODUCER_THREADS 4
/**
 * Async backup configuration: number of buffers used by worker threads
 *
 * Number of buffers used by worker threads while reading files in
 * backup.c/blast_data_to_storage_daemon(); these buffers, are then
 * queued in the consumer (sd socket) thread, which sends the data to the
 * storage daemon
 */
#define AS_BUFFERS (AS_PRODUCER_THREADS * 2)
/**
 * Async backup configuration: buffer capacity
 */
#define AS_BUFFER_CAPACITY 1024*1024*5

class AS_BSOCK_PROXY;
typedef struct Digest DIGEST;
struct FF_PKT;
struct JCR;

/* Used for debugging only */
#define H(x) (int)((unsigned char)( \
      ((unsigned long)x >> 0) ^ ((unsigned long)x >> 1) ^ \
      ((unsigned long)x >> 2) ^ ((unsigned long)x >> 3) ^ \
      ((unsigned long)x >> 4) ^ ((unsigned long)x >> 5) ^ \
      ((unsigned long)x >> 6) ^ ((unsigned long)x >> 7)))

/* Used for debugging only */
#define HH(x) (int)((unsigned short)( \
      ((unsigned long)x >> 0) ^ ((unsigned long)x >> 2) ^ \
      ((unsigned long)x >> 4) ^ ((unsigned long)x >> 6)))

/* Used for debugging only */
int my_thread_id();

struct as_buffer_t
{
   BQUEUE bq; /* Buffers are queued in the free buffers queue or in the consumer queue */
   char data[AS_BUFFER_CAPACITY]; /* Buffer for the data */
   int32_t size; /* Actual size of the data stored in the buffer */
   int id; /* Used for debugging only */
   AS_BSOCK_PROXY *parent; /* Set when file does not fit in a single buffer */
   int final; /* If 1 marks the last buffer of a given file */
   int file_idx; /* Helps keeping files in ascending order as required by storage daemon */
};

class AS_ENGINE;

struct as_cons_thread_loop_context_t
{
   AS_ENGINE *as_engine;
   BSOCK *sd;
};

class AS_ENGINE
{
private:

   /* Producer stuff - used by worker threads that read files to be backed up */
   POOLMEM *as_save_msg_pointer;
   uint32_t as_initial_buf_size;
   workq_t as_work_queue;
   pthread_mutex_t as_buffer_lock;
   pthread_cond_t as_buffer_cond;
   BQUEUE as_free_buffer_queue;
   /* Consumer stuff - related to the consumer (storage socket) thread */
   bool as_consumer_thread_started;
   pthread_mutex_t as_consumer_thread_lock;
   pthread_cond_t as_consumer_thread_started_cond;
   pthread_t as_consumer_thread;
   bool as_consumer_thread_quit;
   pthread_mutex_t as_consumer_queue_lock;
   pthread_cond_t as_consumer_queue_cond;
   /* Consumer stuff used by producer threads too */
   BQUEUE as_consumer_buffer_queue;
   as_buffer_t *as_bigfile_buffer_only;
   AS_BSOCK_PROXY *as_bigfile_bsock_proxy;
   as_buffer_t *as_fix_fi_order_buffer;
   int as_last_file_idx;

   uint32_t comp_usage;

public:

   void init(int32_t compress_buf_size);
   void cleanup();
   void destroy();

   int as_smallest_fi_in_consumer_queue();

   as_buffer_t *as_acquire_buffer(AS_BSOCK_PROXY *parent, int file_idx);
   void dump_consumer_queue(); // TODO REMOVE
   void dump_consumer_queue_locked(); // TODO REMOVE

   void as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize);

   int as_save_file_schedule(
      JCR *jcr,
      FF_PKT *ff_pkt,
      bool do_plugin_set,
      DIGEST *digest,
      DIGEST *signing_digest,
      int digest_stream,
      bool has_file_data);

   void as_wait_for_consumer_thread_started();

   bool as_quit_consumer_thread_loop();

   bool as_is_consumer_queue_empty(bool lockme);

   void as_release_buffer(as_buffer_t *buffer);

   void as_consumer_thread_loop(BSOCK *sd);

   void as_init_free_buffers_queue();

   void as_init_consumer_thread(BSOCK *sd);

   void as_workqueue_init();

   void as_init(BSOCK *sd, uint32_t buf_size);

   uint32_t as_get_initial_bsock_proxy_buf_size();

   void as_request_consumer_thread_quit();

   void as_join_consumer_thread();

   void as_dealloc_all_buffers();

   void as_workqueue_destroy();

   void as_shutdown(BSOCK *sd);

   POOLMEM *compress_buf       [AS_PRODUCER_THREADS];
   void *pZLIB_compress_workset[AS_PRODUCER_THREADS];
   void *LZO_compress_workset  [AS_PRODUCER_THREADS];

   int get_comp_idx();
   void free_comp_idx(int idx);
};

/**
 * Context passed to a single worker thread
 */
typedef struct
{
   JCR *jcr;
   FF_PKT *ff_pkt; /* Points to a cloned COPY ff_pkt */
   bool do_plugin_set;
   DIGEST *digest;
   DIGEST *signing_digest;
   int digest_stream;
   bool has_file_data;
} as_save_file_context_t;

/* Clones ff_pkt */
FF_PKT *as_new_ff_pkt_clone(FF_PKT *ff_pkt);
/* Frees cloned ff_pkt and associated resources */
void as_free_ff_pkt_clone(FF_PKT *ff_pkt);

/**
 * Part of the save_file() function that is called asynchronously in a
 * worker thread
 */
int as_save_file(
   JCR *jcr,
   FF_PKT *ff_pkt,
   bool do_plugin_set,
   DIGEST *digest,
   DIGEST *signing_digest,
   int digest_stream,
   bool has_file_data);

#endif /* __AS_BACKUP_H */
