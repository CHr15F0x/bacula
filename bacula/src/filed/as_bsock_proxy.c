#include "bacula.h"
#include "as_bsock_proxy.h"


#define SOCK_BASE 1000
pthread_mutex_t proxy_cnt_lock = PTHREAD_MUTEX_INITIALIZER;
int proxy_cnt = 0;




AS_BSOCK_PROXY::AS_BSOCK_PROXY()
{
   Pmsg1(50, "\t\t>>>> %4d AS_BSOCK_PROXY::AS_BSOCK_PROXY()\n", my_thread_id());
}

void AS_BSOCK_PROXY::init()
{
   memset(this, 0, sizeof(AS_BSOCK_PROXY));
   msg = get_pool_memory(PM_AS_BSOCK_PROXY);

   msg = realloc_pool_memory(msg, (int32_t)as_get_initial_bsock_proxy_buf_size());

   P(proxy_cnt_lock);
   id = proxy_cnt + SOCK_BASE;
   ++proxy_cnt;
   V(proxy_cnt_lock);

   Pmsg2(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::init()\n", my_thread_id(), id);
}

bool AS_BSOCK_PROXY::send()
{
   Pmsg4(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::send() msglen: %4d msg: %4d\n", my_thread_id(), id, msglen, H(msg));

   /* New file to be sent */
   if (as_buf == NULL)
   {
      as_buf = as_acquire_buffer(NULL);

//      Pmsg2(50, "\t\t>>>> %4d 1 as_buf: %p\n", my_thread_id(), as_buf);
      ASSERT(as_buf != NULL);
      ASSERT(as_buf->size == 0);
   }
   /* Big file, header won't fit, need another buffer */
   else if (as_buf->size + sizeof(msglen) > AS_BUFFER_CAPACITY)
   {
      /* Make sure the current buffer is marked for big file */
      as_buf->parent = this;
      as_consumer_enqueue_buffer(as_buf, false);
      /* Get a new one which is already marked */ // TODO <<< na pewno?
      as_buf = as_acquire_buffer(this);

//      Pmsg2(50, "\t\t>>>> %4d 1 as_buf: %p\n", my_thread_id(), as_buf);

      ASSERT(as_buf != NULL);
      ASSERT(as_buf->size == 0);
   }

   ASSERT(as_buf != NULL);
   ASSERT(as_buf->size <= AS_BUFFER_CAPACITY);

   /* Put the lenght of data first */
   memcpy(&as_buf->data[as_buf->size], &msglen, sizeof(msglen));
   as_buf->size += sizeof(msglen);

   ASSERT(as_buf->size <= AS_BUFFER_CAPACITY);

   /* This is a signal, there is no msg data */
   if (msglen < 0)
   {
      return true;
   }

   /* Put the real message next */
   char *pos = msg;
   int32_t to_send = msglen;

   while (as_buf->size + to_send > AS_BUFFER_CAPACITY)
   {
      /* Fill the current buffer */
      int32_t send_now = AS_BUFFER_CAPACITY - as_buf->size;
      memcpy(&as_buf->data[as_buf->size], pos, send_now);
      as_buf->size += send_now;
      pos += send_now;
      to_send -= send_now;

      ASSERT(as_buf->size <= AS_BUFFER_CAPACITY);

      /* Make sure the current buffer is marked for big file */
      as_buf->parent = this;
      as_consumer_enqueue_buffer(as_buf, false);
      /* Get a new one which is already marked */
      as_buf = as_acquire_buffer(this);
   }

   if (to_send > 0)
   {
      memcpy(&as_buf->data[as_buf->size], pos, to_send);
      as_buf->size += to_send;

      ASSERT(as_buf->size <= AS_BUFFER_CAPACITY);
   }

   return true;
}

/**
 * Based on BSOCK::fsend
 */
bool AS_BSOCK_PROXY::fsend(const char *fmt, ...)
{
   va_list arg_ptr;
   int maxlen;

   for (;;) {
      maxlen = sizeof_pool_memory(msg) - 1;
      va_start(arg_ptr, fmt);
      msglen = bvsnprintf(msg, maxlen, fmt, arg_ptr);
      va_end(arg_ptr);
      if (msglen > 0 && msglen < (maxlen - 5)) {
         break;
      }
      msg = realloc_pool_memory(msg, maxlen + maxlen / 2);
   }

   Pmsg3(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::fsend() \"%s\"\n", my_thread_id(), id, msg);

   return send();
}

bool AS_BSOCK_PROXY::signal(int signal)
{
   Pmsg3(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::signal() %d\n", my_thread_id(), id, signal);

   msglen = signal;
   return send();
}

void AS_BSOCK_PROXY::finalize()
{
   if (as_buf)
   {
      Pmsg3(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::finalize() as_buf: %4d\n", my_thread_id(), id, as_buf->id);
      as_consumer_enqueue_buffer(as_buf, true);
      as_buf = NULL;
   }
   else
   {
      Pmsg2(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::finalize() null\n", my_thread_id(), id);
   }
}

void AS_BSOCK_PROXY::cleanup()
{
   Pmsg4(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::cleanup() msglen: %4d, msg: %4d\n", my_thread_id(), id, msglen, H(msg));

   // TODO VERY IMPORTANT
   finalize();

   if (msg)
   {
      free_pool_memory(msg);
      msg = NULL;
   }
   else
   {
      Pmsg2(50, "\t\t>>>> %4d %4d AS_BSOCK_PROXY::cleanup() AGAIN!!!\n", my_thread_id(), id);
   }
}

void AS_BSOCK_PROXY::destroy()
{
   cleanup();

   free(this);
}

const char *AS_BSOCK_PROXY::bstrerror()
{
   return "";
}
