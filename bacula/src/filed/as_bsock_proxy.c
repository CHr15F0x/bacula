#include "bacula.h"
#include "as_bsock_proxy.h"

#if AS_BACKUP
#define ASDEBUG 0
#define ASDEBUG_FI 0

void AS_BSOCK_PROXY::init(AS_ENGINE *as_engine)
{
   memset(this, 0, sizeof(AS_BSOCK_PROXY));

   ase = as_engine;

   msg = get_pool_memory(PM_AS_BSOCK_PROXY);
   msg = realloc_pool_memory(msg, (int32_t)ase->as_get_initial_bsock_proxy_buf_size());

#if ASDEBUG
   Pmsg2(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::init()\n", my_thread_id(), HH(this));
#endif
}

bool AS_BSOCK_PROXY::send()
{
#if ASDEBUG
   Pmsg7(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::send() BEGIN buf: %d bufsize: %4d parent: %4X msglen: %4d msg: %4d\n",
      my_thread_id(),
      HH(this),
      as_buf ? as_buf->id : -1,
      as_buf ? as_buf->size : -1,
      as_buf ? HH(as_buf->parent) : 0,
      msglen,
      H(msg));
#endif

   /* Nothing to send and this is not a signal */
   if (msglen == 0) {
      return true;
   }

   if (as_buf == NULL) {
      /* New file to be sent */
      as_buf = ase->as_acquire_buffer(NULL, file_idx);
      as_buf->file_idx = file_idx;

#if ASDEBUG
      Pmsg7(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::send() NULL BUF GET NEW buf: %d bufsize: %4d parent: %4X msglen: %4d msg: %4d\n",
         my_thread_id(),
         HH(this),
         as_buf ? as_buf->id : -1,
         as_buf ? as_buf->size : -1,
         as_buf ? (as_buf->parent ? HH(as_buf->parent) : 0) : -1,
         msglen,
         H(msg));
#endif

      ASSERT(as_buf != NULL);
      ASSERT(as_buf->size == 0);
   } else if (as_buf->size + sizeof(msglen) + 1 > AS_BUFFER_CAPACITY) {
      /*
       * Big file (ie. bigger than buffer size),
       * header plus at least one byte won't fit, need another buffer
       */

      /* Make sure the current buffer is marked for big file */
      as_buf->parent = this;
      ase->as_consumer_enqueue_buffer(as_buf, false);

#if ASDEBUG
      Pmsg7(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::send() WONT FIT GET NEW buf: %d bufsize: %4d parent: %4X msglen: %4d msg: %4d\n",
         my_thread_id(),
         HH(this),
         as_buf ? as_buf->id : -1,
         as_buf ? as_buf->size : -1,
         as_buf ? (as_buf->parent ? HH(as_buf->parent) : 0) : -1,
         msglen,
         H(msg));
#endif

      /* Get a new one which is already marked */
      as_buf = ase->as_acquire_buffer(this, file_idx);
      as_buf->file_idx = file_idx;

      ASSERT(as_buf != NULL);
      ASSERT(as_buf->size == 0);
   } else {
      /* Continue using a previously allocated buffer */
   }

   ASSERT(as_buf != NULL);
   ASSERT(as_buf->size <= AS_BUFFER_CAPACITY - sizeof(msglen));

#if ASDEBUG
   Pmsg7(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::send() PUT MSGLEN buf: %d bufsize: %4d parent: %4X msglen: %4d msg: %4d\n",
      my_thread_id(),
      HH(this),
      as_buf ? as_buf->id : -1,
      as_buf ? as_buf->size : -1,
      as_buf ? (as_buf->parent ? HH(as_buf->parent) : 0) : -1,
      msglen,
      H(msg));
#endif

   /* This is a signal, there is no msg data */
   if (msglen < 0) {
      /* Put the length of the data only */
      memcpy(&as_buf->data[as_buf->size], &msglen, sizeof(msglen));
      as_buf->size += sizeof(msglen);

      return true;
   }

   /* This is a real message, there is some payload */
   char *pos = msg;
   int32_t to_send = msglen;

   /* The entire message will not fit into the buffer */
   while (as_buf->size + sizeof(to_send) + to_send > AS_BUFFER_CAPACITY) {
      /* Check how much we can put into the current buffer */
      int32_t send_now = AS_BUFFER_CAPACITY - as_buf->size - sizeof(to_send);

      if (send_now > 0) {
         memcpy(&as_buf->data[as_buf->size], &send_now, sizeof(send_now));
         as_buf->size += sizeof(send_now);

         memcpy(&as_buf->data[as_buf->size], pos, send_now);
         as_buf->size += send_now;
         pos += send_now;
         to_send -= send_now;

         ASSERT(pos <= msg + msglen);

         /* Make sure the current buffer is marked for a big file */
         as_buf->parent = this;
      } else {
         /* Just send the buffer, we can't fit any more data */
      }

      ASSERT(as_buf->size <= AS_BUFFER_CAPACITY);

      ase->as_consumer_enqueue_buffer(as_buf, false);
      /* Get a new one which is already marked */
      as_buf = ase->as_acquire_buffer(this, file_idx);
      as_buf->file_idx = file_idx;

#if ASDEBUG
      Pmsg7(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::send() FULL GET NEW buf: %d bufsize: %4d parent: %4X msglen: %4d msg: %4d\n",
         my_thread_id(),
         HH(this),
         as_buf ? as_buf->id : -1,
         as_buf ? as_buf->size : -1,
         as_buf ? (as_buf->parent ? HH(as_buf->parent) : 0) : -1,
         msglen,
         H(msg));
#endif

      ASSERT(as_buf != NULL);
      ASSERT(as_buf->size == 0);
   }

   /* There is still some data to be sent, which fits into the current buffer */
   if (to_send > 0) {
      memcpy(&as_buf->data[as_buf->size], &to_send, sizeof(to_send));
      as_buf->size += sizeof(to_send);

      memcpy(&as_buf->data[as_buf->size], pos, to_send);
      as_buf->size += to_send;
      pos += to_send;

      ASSERT(pos <= msg + msglen);
      ASSERT(as_buf->size <= AS_BUFFER_CAPACITY);
   }

#if ASDEBUG
   Pmsg7(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::send() END   buf: %d bufsize: %4d parent: %4X msglen: %4d msg: %4d\n",
      my_thread_id(),
      HH(this),
      as_buf ? as_buf->id : -1,
      as_buf ? as_buf->size : -1,
      as_buf ? HH(as_buf->parent) : 0,
      msglen,
      H(msg));
#endif

   return true;
}

/*
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


#if ASDEBUG
   Pmsg3(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::fsend() \"%s\"\n", my_thread_id(), HH(this), msg);
#endif

   return send();
}

/*
 * Based on BSOCK::signal
 */
bool AS_BSOCK_PROXY::signal(int signal)
{
#if ASDEBUG
   Pmsg3(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::signal() %d\n", my_thread_id(), HH(this), signal);
#endif

   msglen = signal;
   return send();
}

/**
 * Marks an end to processing of the current file
 */
void AS_BSOCK_PROXY::finalize()
{
   if (as_buf) {
#if ASDEBUG
      Pmsg3(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::finalize() as_buf: %4d\n", my_thread_id(), HH(this), as_buf->id);
#endif
      ase->as_consumer_enqueue_buffer(as_buf, true);
      as_buf = NULL;
   } else {
#if ASDEBUG
      Pmsg2(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::finalize() null\n", my_thread_id(), HH(this));
#endif
   }
}

void AS_BSOCK_PROXY::update_fi(int fi)
{
   file_idx = fi;
}

void AS_BSOCK_PROXY::cleanup()
{
#if ASDEBUG
   Pmsg4(50, "\t\t>>>> %4d %4X AS_BSOCK_PROXY::cleanup() msglen: %4d, msg: %4d\n", my_thread_id(), HH(this), msglen, H(msg));
#endif

   finalize();

   if (msg != NULL) {
      free_pool_memory(msg);
   }

   memset(this, 0, sizeof(AS_BSOCK_PROXY));
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

#endif /* AS_BACKUP */
