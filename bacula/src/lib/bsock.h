/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2007 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation plus additions
   that are listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of John Walker.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
/*
 * Bacula Sock Class definition
 *   Note, the old non-class code is in bnet.c, and the
 *   new class code associated with this file is in bsock.c
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


class BSOCK {
private:
   BSOCK *m_next;                     /* next BSOCK if duped */
   JCR *m_jcr;                        /* jcr or NULL for error msgs */
   char *m_who;                       /* Name of daemon to which we are talking */
   char *m_host;                      /* Host name/IP */
   int m_port;                        /* desired port */

   void init(JCR * jcr, int sockfd, const char *who, const char *host, int port,
               struct sockaddr *lclient_addr);
   bool open(JCR *jcr, const char *name, char *host, char *service,
               int port, utime_t heart_beat, int *fatal);
   
public:
   uint64_t read_seqno;               /* read sequence number */
   uint32_t in_msg_no;                /* input message number */
   uint32_t out_msg_no;               /* output message number */
   int m_fd;                          /* socket file descriptor */
   TLS_CONNECTION *tls;               /* associated tls connection */
   int32_t msglen;                    /* message length */
   int b_errno;                       /* bsock errno */
   int m_blocking;                    /* blocking state (0 = nonblocking, 1 = blocking) */
   volatile int errors;               /* incremented for each error on socket */
   volatile bool m_suppress_error_msgs: 1; /* set to suppress error messages */
   volatile bool m_timed_out: 1;      /* timed out in read/write */
   volatile bool m_terminated: 1;     /* set when BNET_TERMINATE arrives */
   bool m_duped: 1;                   /* set if duped BSOCK */
   bool m_spool: 1;                   /* set for spooling */
   volatile time_t timer_start;       /* time started read/write */
   volatile time_t timeout;           /* timeout BSOCK after this interval */
   POOLMEM *msg;                      /* message pool buffer */
   POOLMEM *errmsg;                   /* edited error message */
   RES *res;                          /* Resource to which we are connected */
   FILE *m_spool_fd;                  /* spooling file */
   struct sockaddr client_addr;       /* client's IP address */
   struct sockaddr_in peer_addr;      /* peer's IP address */

   /* methods -- in bsock.c */
   BSOCK();
   ~BSOCK();
   bool connect(JCR * jcr, int retry_interval, utime_t max_retry_time,
                utime_t heart_beat, const char *name, char *host, 
                char *service, int port, int verbose);
   int32_t recv();
   bool send();
   bool fsend(const char*, ...);
   bool signal(int signal);
   void close();                      /* close connection and destroy packet */
   void destroy();                    /* destroy socket packet */
   const char *bstrerror();           /* last error on socket */
   int get_peer(char *buf, socklen_t buflen);
   bool despool(void update_attr_spool_size(ssize_t size), ssize_t tsize);
   bool set_buffer_size(uint32_t size, int rw);
   int set_nonblocking();
   int set_blocking();
   void restore_blocking(int flags);

   /* Inline functions */
   void set_jcr(JCR *jcr) { m_jcr = jcr; };
   void set_who(char *who) { m_who = who; };
   void set_host(char *host) { m_host = host; };
   void set_port(int port) { m_port = port; };
   char *who() { return m_who; };
   char *host() { return m_host; };
   int port() { return m_port; };
   JCR *jcr() { return m_jcr; };
   JCR *get_jcr() { return m_jcr; };
   bool is_terminated() { return m_terminated; };
   bool is_timed_out() { return m_timed_out; };
   void set_terminated() { m_terminated = true; };
};

/* 
 *  Signal definitions for use in bnet_sig()   
 *  Note! These must be negative.  There are signals that are generated
 *   by the bsock software not by the OS ...
 */
enum {
   BNET_EOD            = -1,          /* End of data stream, new data may follow */
   BNET_EOD_POLL       = -2,          /* End of data and poll all in one */
   BNET_STATUS         = -3,          /* Send full status */
   BNET_TERMINATE      = -4,          /* Conversation terminated, doing close() */
   BNET_POLL           = -5,          /* Poll request, I'm hanging on a read */
   BNET_HEARTBEAT      = -6,          /* Heartbeat Response requested */
   BNET_HB_RESPONSE    = -7,          /* Only response permited to HB */
   BNET_PROMPT         = -8,          /* Prompt for subcommand */
   BNET_BTIME          = -9,          /* Send UTC btime */
   BNET_BREAK          = -10,         /* Stop current command -- ctl-c */
   BNET_START_SELECT   = -11,         /* Start of a selection list */
   BNET_END_SELECT     = -12,         /* End of a select list */
   BNET_INVALID_CMD    = -13,         /* Invalid command sent */
   BNET_CMD_FAILED     = -14,         /* Command failed */
   BNET_CMD_OK         = -15,         /* Command succeeded */
   BNET_CMD_BEGIN      = -16,         /* Start command execution */
   BNET_MSGS_PENDING   = -17,         /* Messages pending */
   BNET_MAIN_PROMPT    = -18,         /* Server ready and waiting */
   BNET_SELECT_INPUT   = -19,         /* Return selection input */
   BNET_WARNING_MSG    = -20,         /* Warning message */
   BNET_ERROR_MSG      = -21,         /* Error message -- command failed */
   BNET_INFO_MSG       = -22,         /* Info message -- status line */
   BNET_RUN_CMD        = -23          /* Run command follows */
};

#define BNET_SETBUF_READ  1           /* Arg for bnet_set_buffer_size */
#define BNET_SETBUF_WRITE 2           /* Arg for bnet_set_buffer_size */

/* 
 * Return status from bnet_recv()
 * Note, the HARDEOF and ERROR refer to comm status/problems 
 *  rather than the BNET_xxx above, which are software signals.
 */
enum {
   BNET_SIGNAL         = -1,
   BNET_HARDEOF        = -2,
   BNET_ERROR          = -3
};

/*
 * TLS enabling values. Value is important for comparison, ie:
 * if (tls_remote_need < BNET_TLS_REQUIRED) { ... }
 */
enum {
   BNET_TLS_NONE        = 0,          /* cannot do TLS */
   BNET_TLS_OK          = 1,          /* can do, but not required on my end */
   BNET_TLS_REQUIRED    = 2           /* TLS is required */
};

int32_t read_nbytes(BSOCK * bsock, char *ptr, int32_t nbytes);
int32_t write_nbytes(BSOCK * bsock, char *ptr, int32_t nbytes);
