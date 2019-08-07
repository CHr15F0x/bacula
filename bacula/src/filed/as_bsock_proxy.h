#ifndef __AS_BSOCK_PROXY_H
#define __AS_BSOCK_PROXY_H

#define AS_BACKUP 1

#include "as_backup.h"

class AS_BSOCK_PROXY
{
public:

   POOLMEM *msg;
   int32_t msglen;

private:

   as_buffer_t *as_buf;
   int file_idx;

public:

   AS_BSOCK_PROXY();

   void init();
   bool send();
   bool fsend(const char *fmt, ...);
   bool signal(int signal);
   void finalize();
   void update_fi(int file_idx);
   void cleanup();
   void destroy();
   const char *bstrerror();
};

#endif /* __AS_BSOCK_PROXY_H */
