#ifndef __AS_BACKUP_H
#define __AS_BACKUP_H

class AS_BSOCK_PROXY;
typedef struct Digest DIGEST;
struct FF_PKT;
struct JCR;

// TODO cześć to lokalne funkcje i powinny wylecieć z headera

//
// AS TODO
// condition variables for buffers which are ready to be used
// or buffers which can be consumed by the consumer thread
//


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

#if AS_DEBUG
#define QINSERT(head, object)    { Pmsg4(50, ">>>> QINSERT %s %d %s %s\n", __FILE__, __LINE__, #head, #object); qinsert(head, object); }
BQUEUE *qremove_wrapper(char *file, int line, char* headstr, BQUEUE *qhead)
{
   Pmsg3(50, ">>>> QREMOVE %s %d %s\n", file, line, headstr);
   return qremove(qhead);
}
#define QREMOVE(head) qremove_wrapper(__FILE__, __LINE__, #head, head)
#else
#define QINSERT qinsert
#define QREMOVE qremove
#endif // !AS_DEBUG

//
// Producer related data structures
//
#define AS_PRODUCER_THREADS 4

//
// Data structures shared between producer threads and consumer thread
//

#define AS_BUFFERS (AS_PRODUCER_THREADS + 1)
#define AS_BUFFER_CAPACITY 1024*1024*5

struct as_buffer_t
{
   BQUEUE bq;
   char data[AS_BUFFER_CAPACITY];
   int32_t size;
   int id; // For testing
   AS_BSOCK_PROXY *parent; /** Only set when a total file trasfer size is bigger than one buffer */
   int final;
};

//
// Producer loop
//

as_buffer_t *as_acquire_buffer(AS_BSOCK_PROXY *parent);
void as_consumer_enqueue_buffer(as_buffer_t *buffer, bool finalize);
int as_workqueue_engine_quit();

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

int as_save_file_schedule(
   JCR *jcr,
   FF_PKT *ff_pkt,
   bool do_plugin_set,
   DIGEST *digest,
   DIGEST *signing_digest,
   int digest_stream,
   bool has_file_data);

void *as_workqueue_engine(void *arg);

//
// Consumer loop
//

bool as_dont_quit_consumer_thread_loop();

as_buffer_t *as_consumer_dequeue_buffer();
void as_release_buffer(as_buffer_t *buffer);
void *as_consumer_thread_loop(void *arg);
//
// Initialization
//
void as_init_free_buffers_queue();
void as_init_consumer_thread(BSOCK *sd);
void as_workqueue_init();

void as_init(BSOCK *sd, uint32_t buf_size);
uint32_t as_get_initial_bsock_proxy_buf_size();
//
// Shutdown
//

void as_request_consumer_thread_quit();
void as_join_consumer_thread();

void as_release_remaining_consumer_buffers();
void as_clear_free_buffers_queue();
void as_workqueue_destroy();
void as_shutdown(BSOCK *sd);


#endif /* __AS_BACKUP_H */
