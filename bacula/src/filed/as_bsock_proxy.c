#include "bacula.h"
#include "jcr.h"
#include "as_bsock_proxy.h"

AS_BSOCK_PROXY::AS_BSOCK_PROXY()
{
   Pmsg1(50, "\t\t>>>> %X AS_BSOCK_PROXY::AS_BSOCK_PROXY()\n", pthread_self());
}

void AS_BSOCK_PROXY::init()
{
   Pmsg1(50, "\t\t>>>> %X AS_BSOCK_PROXY::init()\n", pthread_self());

   memset(this, 0, sizeof(AS_BSOCK_PROXY));
   msg = (POOLMEM *)malloc(4096); // for fsend()
   allocated_size = 4096;
}

bool AS_BSOCK_PROXY::send()
{
   Pmsg2(50, "\t\t>>>> %X AS_BSOCK_PROXY::send() %d\n", pthread_self(), msglen);

   /* New file to be sent */
   if (as_buf == NULL)
   {
      as_buf = as_acquire_buffer(NULL);
   }
   /* Big file, header won't fit, need another buffer */
   else if (as_buf->size + sizeof(msglen) > AS_BUFFER_CAPACITY)
   {
      /* Make sure the current buffer is marked for big file */
      as_buf->parent = this;
      as_consumer_enqueue_buffer(as_buf, false);
      /* Get a new one which is already marked */
      as_buf = as_acquire_buffer(this);
   }

   /* Put the lenght of data first */
   memcpy(&as_buf->data[as_buf->size], &msglen, sizeof(msglen));
   as_buf->size += sizeof(msglen);

   /* This is a signal, there is no msg data */
   if (msglen < 0)
   {
      return true;
   }

   /* Put the real message next */
   char *pos = msg;
   int32_t to_send = msglen;

   while (to_send > AS_BUFFER_CAPACITY)
   {
      /* Fill the current buffer */
      int32_t send_now = AS_BUFFER_CAPACITY - as_buf->size;
      memcpy(&as_buf->data[as_buf->size], pos, send_now);
      as_buf->size += send_now;
      pos += send_now;
      to_send -= send_now;

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
   }

   return true;
}

/**
 * Based on BSOCK::fsend
 */
bool AS_BSOCK_PROXY::fsend(const char *fmt, ...)
{
   Pmsg2(50, "\t\t>>>> %X AS_BSOCK_PROXY::fsend() %s\n", pthread_self(), fmt);

   va_list arg_ptr;
   int maxlen;

   for (;;)
   {
      maxlen = allocated_size - 1;
      va_start(arg_ptr, fmt);
      msglen = bvsnprintf(msg, maxlen, fmt, arg_ptr);
      va_end(arg_ptr);
      if (msglen > 0 && msglen < (maxlen - 5))
      {
         break;
      }
      msg = (POOLMEM *)realloc(msg, maxlen + maxlen / 2);
      allocated_size = maxlen + maxlen / 2;
   }
   return send();
}

bool AS_BSOCK_PROXY::signal(int signal)
{
   Pmsg2(50, "\t\t>>>> %X AS_BSOCK_PROXY::signal() %d\n", pthread_self(), signal);

   msglen = signal;
   return send();
}

void AS_BSOCK_PROXY::finalize()
{
   Pmsg2(50, "\t\t>>>> %X AS_BSOCK_PROXY::finalize() %p\n", pthread_self(), as_buf);

   if (as_buf)
   {
      as_consumer_enqueue_buffer(as_buf, true);
      as_buf = NULL;
   }
}

void AS_BSOCK_PROXY::cleanup()
{
   Pmsg2(50, "\t\t>>>> %X AS_BSOCK_PROXY::cleanup() %p\n", pthread_self(), msg);

   if (msg)
   {
      free(msg);
      msg = NULL;
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
