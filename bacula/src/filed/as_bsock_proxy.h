#ifndef __AS_BSOCK_PROXY_H
#define __AS_BSOCK_PROXY_H

#include "as_backup.h"

class AS_BSOCK_PROXY
{
public:

   POOLMEM *msg;
   int32_t msglen;

private:

   AS_ENGINE *ase;
   as_buffer_t *as_buf;
   int file_idx;

public:

   void init(AS_ENGINE *as_engine);
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
