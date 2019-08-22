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
   POOLMEM *save_msg_pointer;
   uint32_t initial_buf_size;
   workq_t work_q;
   pthread_mutex_t free_buf_q_lock;
   pthread_cond_t free_buf_q_cond;
   BQUEUE free_buf_q;
   /* Consumer stuff - related to the consumer (storage socket) thread */
   bool cons_thr_started;
   pthread_mutex_t cons_thr_lock;
   pthread_cond_t cons_thr_started_cond;
   pthread_t cons_thr;
   bool cons_thr_quit;
   pthread_mutex_t cons_q_lock;
   pthread_cond_t cons_q_cond;
   /* Consumer stuff used by producer threads too */
   BQUEUE cons_q;
   as_buffer_t *bigfile_buf;
   AS_BSOCK_PROXY *bigfile_bsock_proxy;
   as_buffer_t *fix_fi_order_buf;
   int last_file_idx;

public:

   void init();
   void cleanup();
   void destroy();

   int smallest_fi_in_consumer_queue();

   as_buffer_t *acquire_buf(AS_BSOCK_PROXY *parent, int file_idx);
   void dump_consumer_queue(); // TODO REMOVE
   void dump_consumer_queue_locked(); // TODO REMOVE

   void consumer_enqueue_buf(as_buffer_t *buffer, bool finalize);

   int save_file_schedule(
      JCR *jcr,
      FF_PKT *ff_pkt,
      bool do_plugin_set,
      DIGEST *digest,
      DIGEST *signing_digest,
      int digest_stream,
      bool has_file_data);

   void wait_consumer_thread_started();

   bool quit_consumer_thread_loop();

   bool consumer_queue_empty(bool lockme);

   void release_buffer(as_buffer_t *buffer);

   void consumer_thread_loop(BSOCK *sd);

   void init_free_buffers_queue();

   void init_consumer_thread(BSOCK *sd);

   void work_queue_init();

   void start(BSOCK *sd, uint32_t buf_size);

   uint32_t get_initial_bsock_proxy_buf_size();

   void request_consumer_thread_quit();

   void join_consumer_thread();

   void dealloc_all_buffers();

   void work_queue_destroy();

   void stop(BSOCK *sd);
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
