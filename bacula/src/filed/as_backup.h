#ifndef __AS_BACKUP_H
#define __AS_BACKUP_H

class AS_BSOCK_PROXY;
typedef struct Digest DIGEST;
struct FF_PKT;
struct JCR;

#define H(x) (int)((unsigned char)( \
      ((unsigned long)x >> 0) ^ ((unsigned long)x >> 1) ^ \
      ((unsigned long)x >> 2) ^ ((unsigned long)x >> 3) ^ \
      ((unsigned long)x >> 4) ^ ((unsigned long)x >> 5) ^ \
      ((unsigned long)x >> 6) ^ ((unsigned long)x >> 7)))

#define HH(x) (int)((unsigned short)( \
      ((unsigned long)x >> 0) ^ ((unsigned long)x >> 2) ^ \
      ((unsigned long)x >> 4) ^ ((unsigned long)x >> 6)))


int my_thread_id();
int thread_id(pthread_t pth_id);

#define AS_DEBUG 0
#define AS_BUFFER_BASE 1000

//
// Producer related data structures
//
#define AS_PRODUCER_THREADS 4

//
// Data structures shared between producer threads and consumer thread
//

#define AS_BUFFERS (AS_PRODUCER_THREADS * 2)
#define AS_BUFFER_CAPACITY 1024*1024*5

struct as_buffer_t
{
   BQUEUE bq;
   char data[AS_BUFFER_CAPACITY];
   int32_t size;
   int id; // For testing
   AS_BSOCK_PROXY *parent; /** Only set when a total file trasfer size is bigger than one buffer */
   int final;
   int file_idx;
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

   /* Producer stuff */
   POOLMEM *as_save_msg_pointer;
   uint32_t as_initial_buf_size;
   workq_t as_work_queue;
   pthread_mutex_t as_buffer_lock;
   pthread_cond_t as_buffer_cond;
   BQUEUE as_free_buffer_queue;
   /* Consumer stuff */
   bool as_consumer_thread_started;
   pthread_mutex_t as_consumer_thread_lock;
   pthread_cond_t as_consumer_thread_started_cond;
   pthread_t as_consumer_thread;
   bool as_consumer_thread_quit;
   pthread_mutex_t as_consumer_queue_lock;
   pthread_cond_t as_consumer_queue_cond;
   BQUEUE as_consumer_buffer_queue;
   as_buffer_t *as_bigfile_buffer_only;
   AS_BSOCK_PROXY *as_bigfile_bsock_proxy;
   as_buffer_t *as_fix_fi_order_buffer;
   int as_last_file_idx;

public:

   void init();
   void cleanup();
   void destroy();

   int as_smallest_fi_in_consumer_queue();

   as_buffer_t *as_acquire_buffer(AS_BSOCK_PROXY *parent, int file_idx);
   void dump_consumer_queue(); // TODO REMOVE
   void dump_consumer_queue_locked(); // TODO REMOVE

   void as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize);

   int as_workqueue_engine_quit();

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
};

typedef struct
{
   JCR *jcr;
   FF_PKT *ff_pkt; // TODO will need a copy?
   bool do_plugin_set;
   DIGEST *digest; // TODO will beed a copy per thread
   DIGEST *signing_digest; // TODO will beed a copy per thread
   int digest_stream;
   bool has_file_data;
} as_save_file_context_t;

FF_PKT *as_new_ff_pkt_clone(FF_PKT *ff_pkt);
void as_free_ff_pkt_clone(FF_PKT *ff_pkt);

int as_save_file(
   JCR *jcr,
   FF_PKT *ff_pkt,
   bool do_plugin_set,
   DIGEST *digest,
   DIGEST *signing_digest,
   int digest_stream,
   bool has_file_data);

#endif /* __AS_BACKUP_H */
