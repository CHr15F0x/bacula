/*
 * Bacula Sock Structure definition
 *
 * Kern Sibbald, May MM
 *
 * Zero msglen from other end indicates soft eof (usually
 *   end of some binary data stream, but not end of conversation).
 *
 * Negative msglen, is special "signal" (no data follows).
 *   See below for SIGNAL codes.
 *
 *   Version $Id$
 */
/*
   Copyright (C) 2000-2005 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as ammended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */

struct BSOCK {
   uint64_t read_seqno;               /* read sequence number */
   uint32_t in_msg_no;                /* input message number */
   uint32_t out_msg_no;               /* output message number */
   int fd;                            /* socket file descriptor */
   TLS_CONNECTION *tls;               /* associated tls connection */
   int32_t msglen;                    /* message length */
   int b_errno;                       /* bsock errno */
   int port;                          /* desired port */
   int blocking;                      /* blocking state (0 = nonblocking, 1 = blocking) */
   volatile int errors;               /* incremented for each error on socket */
   volatile bool suppress_error_msgs: 1; /* set to suppress error messages */
   volatile bool timed_out: 1;        /* timed out in read/write */
   volatile bool terminated: 1;       /* set when BNET_TERMINATE arrives */
   bool duped: 1;                     /* set if duped BSOCK */
   bool spool: 1;                     /* set for spooling */
   volatile time_t timer_start;       /* time started read/write */
   volatile time_t timeout;           /* timeout BSOCK after this interval */
   POOLMEM *msg;                      /* message pool buffer */
   char *who;                         /* Name of daemon to which we are talking */
   char *host;                        /* Host name/IP */
   POOLMEM *errmsg;                   /* edited error message */
   RES *res;                          /* Resource to which we are connected */
   BSOCK *next;                       /* next BSOCK if duped */
   FILE *spool_fd;                    /* spooling file */
   JCR *jcr;                          /* jcr or NULL for error msgs */
   struct sockaddr client_addr;    /* client's IP address */
};

/* Signal definitions for use in bnet_sig() */
enum {
   BNET_EOD            = -1,          /* End of data stream, new data may follow */
   BNET_EOD_POLL       = -2,          /* End of data and poll all in one */
   BNET_STATUS         = -3,          /* Send full status */
   BNET_TERMINATE      = -4,          /* Conversation terminated, doing close() */
   BNET_POLL           = -5,          /* Poll request, I'm hanging on a read */
   BNET_HEARTBEAT      = -6,          /* Heartbeat Response requested */
   BNET_HB_RESPONSE    = -7,          /* Only response permited to HB */
   BNET_PROMPT         = -8,          /* Prompt for UA */
   BNET_BTIME          = -9,          /* Send UTC btime */
   BNET_BREAK          = -10          /* Stop current command -- ctl-c */
};

#define BNET_SETBUF_READ  1           /* Arg for bnet_set_buffer_size */
#define BNET_SETBUF_WRITE 2           /* Arg for bnet_set_buffer_size */

/* Return status from bnet_recv() */
#define BNET_SIGNAL  -1
#define BNET_HARDEOF -2
#define BNET_ERROR   -3

/*
 * TLS enabling values. Value is important for comparison, ie:
 * if (tls_remote_need < BNET_TLS_REQUIRED) { ... }
 */
#define BNET_TLS_NONE     0           /* cannot do TLS */
#define BNET_TLS_OK       1           /* can do, but not required on my end */
#define BNET_TLS_REQUIRED 2           /* TLS is required */

/*
 * This is the structure of the in memory BPKT
 */
typedef struct s_bpkt {
   char *id;                          /* String identifier or name of field */
   uint8_t type;                      /* field type */
   uint32_t len;                      /* field length for string, name, bytes */
   void *value;                       /* pointer to value */
} BPKT;

/*
 * These are the data types that can be sent.
 * For all values other than string, the storage space
 *  is assumed to be allocated in the receiving packet.
 *  For BP_STRING if the *value is non-zero, it is a
 *  pointer to a POOLMEM buffer, and the Memory Pool
 *  routines will be used to assure that the length is
 *  adequate. NOTE!!! This pointer will be changed
 *  if the memory is reallocated (sort of like Mmsg(&pool)
 *  does). If the pointer is NULL, a POOLMEM
 *  buffer will be allocated.
 */
#define BP_EOF       0                /* end of file */
#define BP_CHAR      1                /* Character */
#define BP_INT32     1                /* 32 bit integer */
#define BP_UINT32    3                /* Unsigned 32 bit integer */
#define BP_INT64     4                /* 64 bit integer */
#define BP_STRING    5                /* string */
#define BP_NAME      6                /* Name string -- limited length */
#define BP_BYTES     7                /* Binary bytes */
#define BP_FLOAT32   8                /* 32 bit floating point */
#define BP_FLOAT64   9                /* 64 bit floating point */
