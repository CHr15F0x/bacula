/*
 * Network Utility Routines
 *
 *  by Kern Sibbald
 *
 * Adapted and enhanced for Bacula, originally written 
 * for inclusion in the Apcupsd package
 *
 *   Version $Id$
 */
/*
   Copyright (C) 2000-2004 Kern Sibbald and John Walker

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
   MA 02111-1307, USA.

 */


#include "bacula.h"
#include "jcr.h"
#include <netdb.h>

extern time_t watchdog_time;

#ifndef   INADDR_NONE
#define   INADDR_NONE	 -1
#endif

#ifndef ENODATA 		   /* not defined on BSD systems */
#define ENODATA EPIPE
#endif

#ifdef HAVE_WIN32
#define socketRead(fd, buf, len)  recv(fd, buf, len, 0)
#define socketWrite(fd, buf, len) send(fd, buf, len, 0)
#define socketClose(fd) 	  closesocket(fd)
#else
#define socketRead(fd, buf, len)  read(fd, buf, len)
#define socketWrite(fd, buf, len) write(fd, buf, len)
#define socketClose(fd) 	  close(fd)
#endif


/*
 * Read a nbytes from the network.
 * It is possible that the total bytes require in several
 * read requests
 */

static int32_t read_nbytes(BSOCK * bsock, char *ptr, int32_t nbytes)
{
   int32_t nleft, nread;

   nleft = nbytes;
   while (nleft > 0) {
      do {
	 errno = 0;
	 nread = socketRead(bsock->fd, ptr, nleft);
	 if (bsock->timed_out || bsock->terminated) {
	    return nread;
	 }
      } while (nread == -1 && (errno == EINTR || errno == EAGAIN));
      if (nread <= 0) {
	 return nread;		   /* error, or EOF */
      }
      nleft -= nread;
      ptr += nread;
   }
   return nbytes - nleft;	   /* return >= 0 */
}

/*
 * Write nbytes to the network.
 * It may require several writes.
 */

static int32_t write_nbytes(BSOCK * bsock, char *ptr, int32_t nbytes)
{
   int32_t nleft, nwritten;

   if (bsock->spool) {
      nwritten = fwrite(ptr, 1, nbytes, bsock->spool_fd);
      if (nwritten != nbytes) {
	 berrno be;
         Qmsg1(bsock->jcr, M_FATAL, 0, _("Attr spool write error. ERR=%s\n"),
	       be.strerror());
         Dmsg2(400, "nwritten=%d nbytes=%d.\n", nwritten, nbytes);
	 return -1;
      }
      return nbytes;
   }
   nleft = nbytes;
   while (nleft > 0) {
      do {
	 errno = 0;
	 nwritten = socketWrite(bsock->fd, ptr, nleft);
	 if (bsock->timed_out || bsock->terminated) {
	    return nwritten;
	 }
      } while (nwritten == -1 && errno == EINTR);
      /*
       * If connection is non-blocking, we will get EAGAIN, so
       * use select() to keep from consuming all the CPU
       * and try again.
       */
      if (nwritten == -1 && errno == EAGAIN) {
	 fd_set fdset;
	 struct timeval tv;

	 FD_ZERO(&fdset);
	 FD_SET((unsigned)bsock->fd, &fdset);
	 tv.tv_sec = 10;
	 tv.tv_usec = 0;
	 select(bsock->fd + 1, NULL, &fdset, NULL, &tv);
	 continue;
      }
      if (nwritten <= 0) {
	 return nwritten;	   /* error */
      }
      nleft -= nwritten;
      ptr += nwritten;
   }
   return nbytes - nleft;
}

/* 
 * Receive a message from the other end. Each message consists of
 * two packets. The first is a header that contains the size
 * of the data that follows in the second packet.
 * Returns number of bytes read (may return zero)
 * Returns -1 on signal (BNET_SIGNAL) 
 * Returns -2 on hard end of file (BNET_HARDEOF)
 * Returns -3 on error	(BNET_ERROR)
 *
 *  Unfortunately, it is a bit complicated because we have these
 *    four return types:
 *    1. Normal data
 *    2. Signal including end of data stream
 *    3. Hard end of file		  
 *    4. Error
 *  Using is_bnet_stop() and is_bnet_error() you can figure this all out.
 */
int32_t bnet_recv(BSOCK * bsock)
{
   int32_t nbytes;
   int32_t pktsiz;

   ASSERT(bsock != NULL);
   mp_chr(bsock->msg)[0] = 0;
   if (bsock->errors || bsock->terminated) {
      return BNET_HARDEOF;
   }

   bsock->read_seqno++; 	   /* bump sequence number */
   bsock->timer_start = watchdog_time;	/* set start wait time */
   bsock->timed_out = 0;
   /* get data size -- in int32_t */
   if ((nbytes = read_nbytes(bsock, (char *)&pktsiz, sizeof(int32_t))) <= 0) {
      bsock->timer_start = 0;	   /* clear timer */
      /* probably pipe broken because client died */
      if (errno == 0) {
	 bsock->b_errno = ENODATA;
      } else {
	 bsock->b_errno = errno;
      }
      bsock->errors++;
      return BNET_HARDEOF;	   /* assume hard EOF received */
   }
   bsock->timer_start = 0;	   /* clear timer */
   if (nbytes != sizeof(int32_t)) {
      bsock->errors++;
      bsock->b_errno = EIO;
      Qmsg5(bsock->jcr, M_ERROR, 0, _("Read expected %d got %d from %s:%s:%d\n"),
	    sizeof(int32_t), nbytes, bsock->who, bsock->host, bsock->port);
      return BNET_ERROR;
   }

   pktsiz = ntohl(pktsiz);	   /* decode no. of bytes that follow */

   if (pktsiz == 0) {		   /* No data transferred */
      bsock->timer_start = 0;	   /* clear timer */
      bsock->in_msg_no++;
      bsock->msglen = 0;
      return 0; 		   /* zero bytes read */
   }

   /* If signal or packet size too big */
   if (pktsiz < 0 || pktsiz > 1000000) {
      if (pktsiz > 0) { 	   /* if packet too big */
	 Qmsg3(bsock->jcr, M_FATAL, 0,
               _("Packet size too big from \"%s:%s:%d. Terminating connection.\n"),
	       bsock->who, bsock->host, bsock->port);
	 pktsiz = BNET_TERMINATE;  /* hang up */
      }
      if (pktsiz == BNET_TERMINATE) {
	 bsock->terminated = 1;
      }
      bsock->timer_start = 0;	   /* clear timer */
      bsock->b_errno = ENODATA;
      bsock->msglen = pktsiz;	   /* signal code */
      return BNET_SIGNAL;	   /* signal */
   }

   /* Make sure the buffer is big enough + one byte for EOS */
   if (pktsiz >= (int32_t) sizeof_pool_memory(bsock->msg)) {
      bsock->msg = realloc_pool_memory(bsock->msg, pktsiz + 100);
   }

   bsock->timer_start = watchdog_time;	/* set start wait time */
   bsock->timed_out = 0;
   /* now read the actual data */
   if ((nbytes = read_nbytes(bsock, mp_chr(bsock->msg), pktsiz)) <= 0) {
      bsock->timer_start = 0;	   /* clear timer */
      if (errno == 0) {
	 bsock->b_errno = ENODATA;
      } else {
	 bsock->b_errno = errno;
      }
      bsock->errors++;
      Qmsg4(bsock->jcr, M_ERROR, 0, _("Read error from %s:%s:%d: ERR=%s\n"),
	    bsock->who, bsock->host, bsock->port, bnet_strerror(bsock));
      return BNET_ERROR;
   }
   bsock->timer_start = 0;	   /* clear timer */
   bsock->in_msg_no++;
   bsock->msglen = nbytes;
   if (nbytes != pktsiz) {
      bsock->b_errno = EIO;
      bsock->errors++;
      Qmsg5(bsock->jcr, M_ERROR, 0, _("Read expected %d got %d from %s:%s:%d\n"),
	    pktsiz, nbytes, bsock->who, bsock->host, bsock->port);
      return BNET_ERROR;
   }
   /* always add a zero by to properly terminate any
    * string that was send to us. Note, we ensured above that the
    * buffer is at least one byte longer than the message length.
    */
   mp_chr(bsock->msg)[nbytes] = 0; /* terminate in case it is a string */
   sm_check(__FILE__, __LINE__, false);
   return nbytes;		   /* return actual length of message */
}


/*
 * Return 1 if there are errors on this bsock or it is closed,	
 *   i.e. stop communicating on this line.
 */
bool is_bnet_stop(BSOCK * bsock)
{
   return bsock->errors || bsock->terminated;
}

/*
 * Return number of errors on socket 
 */
int is_bnet_error(BSOCK * bsock)
{
   return bsock->errors;
}

/*
 * Call here after error during closing to suppress error
 *  messages which are due to the other end shutting down too.
 */
void bnet_suppress_error_messages(BSOCK * bsock, bool flag)
{
   bsock->suppress_error_msgs = flag;
}


/*
 * Transmit spooled data now to a BSOCK
 */
int bnet_despool_to_bsock(BSOCK * bsock, void update_attr_spool_size(ssize_t size),
			  ssize_t tsize)
{
   int32_t pktsiz;
   size_t nbytes;
   ssize_t last = 0, size = 0;
   int count = 0;

   rewind(bsock->spool_fd);
   while (fread((char *)&pktsiz, 1, sizeof(int32_t), bsock->spool_fd) ==
	  sizeof(int32_t)) {
      size += sizeof(int32_t);
      bsock->msglen = ntohl(pktsiz);
      if (bsock->msglen > 0) {
	 if (bsock->msglen > (int32_t) sizeof_pool_memory(bsock->msg)) {
	    bsock->msg = realloc_pool_memory(bsock->msg, bsock->msglen + 1);
	 }
	 nbytes = fread(bsock->msg, 1, bsock->msglen, bsock->spool_fd);
	 if (nbytes != (size_t) bsock->msglen) {
	    berrno be;
            Dmsg2(400, "nbytes=%d msglen=%d\n", nbytes, bsock->msglen);
            Qmsg1(bsock->jcr, M_FATAL, 0, _("fread attr spool error. ERR=%s\n"),
		  be.strerror());
	    update_attr_spool_size(tsize - last);
	    return 0;
	 }
	 size += nbytes;
	 if ((++count & 0x3F) == 0) {
	    update_attr_spool_size(size - last);
	    last = size;
	 }
      }
      bnet_send(bsock);
   }
   update_attr_spool_size(tsize - last);
   if (ferror(bsock->spool_fd)) {
      berrno be;
      Qmsg1(bsock->jcr, M_FATAL, 0, _("fread attr spool error. ERR=%s\n"),
	    be.strerror());
      return 0;
   }
   return 1;
}


/*
 * Send a message over the network. The send consists of
 * two network packets. The first is sends a 32 bit integer containing
 * the length of the data packet which follows.
 *
 * Returns: 0 on failure
 *	    1 on success
 */
bool bnet_send(BSOCK * bsock)
{
   int32_t rc;
   int32_t pktsiz;

   if (bsock->errors || bsock->terminated || bsock->msglen > 1000000) {
      return false;
   }
   pktsiz = htonl((int32_t) bsock->msglen);
   /* send int32_t containing size of data packet */
   bsock->timer_start = watchdog_time;	/* start timer */
   bsock->timed_out = 0;
   rc = write_nbytes(bsock, (char *)&pktsiz, sizeof(int32_t));
   bsock->timer_start = 0;	   /* clear timer */
   if (rc != sizeof(int32_t)) {
      if (bsock->msglen == BNET_TERMINATE) {	/* if we were terminating */
	 bsock->terminated = 1;
	 return false;		   /* ignore any errors */
      }
      bsock->errors++;
      if (errno == 0) {
	 bsock->b_errno = EIO;
      } else {
	 bsock->b_errno = errno;
      }
      if (rc < 0) {
	 if (!bsock->suppress_error_msgs && !bsock->timed_out) {
	    Qmsg4(bsock->jcr, M_ERROR, 0,
                  _("Write error sending to %s:%s:%d: ERR=%s\n"), bsock->who,
		  bsock->host, bsock->port, bnet_strerror(bsock));
	 }
      } else {
	 Qmsg5(bsock->jcr, M_ERROR, 0,
               _("Wrote %d bytes to %s:%s:%d, but only %d accepted.\n"), bsock->who,
	       bsock->host, bsock->port, bsock->msglen, rc);
      }
      return false;
   }

   bsock->out_msg_no++; 	   /* increment message number */
   if (bsock->msglen <= 0) {	   /* length only? */
      return true;		   /* yes, no data */
   }

   /* send data packet */
   bsock->timer_start = watchdog_time;	/* start timer */
   bsock->timed_out = 0;
   rc = write_nbytes(bsock, mp_chr(bsock->msg), bsock->msglen);
   bsock->timer_start = 0;	   /* clear timer */
   if (rc != bsock->msglen) {
      bsock->errors++;
      if (errno == 0) {
	 bsock->b_errno = EIO;
      } else {
	 bsock->b_errno = errno;
      }
      if (rc < 0) {
	 if (!bsock->suppress_error_msgs) {
	    Qmsg4(bsock->jcr, M_ERROR, 0,
                  _("Write error sending to %s:%s:%d: ERR=%s\n"), bsock->who,
		  bsock->host, bsock->port, bnet_strerror(bsock));
	 }
      } else {
	 Qmsg5(bsock->jcr, M_ERROR, 0,
               _("Wrote %d bytes to %s:%s:%d, but only %d accepted.\n"),
	       bsock->msglen, bsock->who, bsock->host, bsock->port, rc);
      }
      return false;
   }
   return true;
}

/*
 * Establish an SSL connection -- server side	      
 *  Codes that ssl_need and ssl_has can take
 *    BNET_SSL_NONE	 I cannot do ssl
 *    BNET_SSL_OK	 I can do ssl, but it is not required on my end
 *    BNET_SSL_REQUIRED  ssl is required on my end
 */
int bnet_ssl_server(BSOCK * bsock, char *password, int ssl_need, int ssl_has)
{
   /* Check to see if what we need (ssl_need) corresponds to what he has (ssl_has) */
   /* The other side expects a response from us */
   return 1;
}

/*
 * Establish an SSL connection -- client side	
 */
int bnet_ssl_client(BSOCK * bsock, char *password, int ssl_need)
{
   /* We are the client so we must wait for the server to notify us */
   return 1;
}


/*
 * Wait for a specified time for data to appear on
 * the BSOCK connection.
 *
 *   Returns: 1 if data available
 *	      0 if timeout
 *	     -1 if error
 */
int bnet_wait_data(BSOCK * bsock, int sec)
{
   fd_set fdset;
   struct timeval tv;

   FD_ZERO(&fdset);
   FD_SET((unsigned)bsock->fd, &fdset);
   tv.tv_sec = sec;
   tv.tv_usec = 0;
   for (;;) {
      switch (select(bsock->fd + 1, &fdset, NULL, NULL, &tv)) {
      case 0:			   /* timeout */
	 bsock->b_errno = 0;
	 return 0;
      case -1:
	 bsock->b_errno = errno;
	 if (errno == EINTR || errno == EAGAIN) {
	    continue;
	 }
	 return -1;		   /* error return */
      default:
	 bsock->b_errno = 0;
	 return 1;
      }
   }
}

/*
 * As above, but returns on interrupt
 */
int bnet_wait_data_intr(BSOCK * bsock, int sec)
{
   fd_set fdset;
   struct timeval tv;

   FD_ZERO(&fdset);
   FD_SET((unsigned)bsock->fd, &fdset);
   tv.tv_sec = sec;
   tv.tv_usec = 0;
   for (;;) {
      switch (select(bsock->fd + 1, &fdset, NULL, NULL, &tv)) {
      case 0:			   /* timeout */
	 bsock->b_errno = 0;
	 return 0;
      case -1:
	 bsock->b_errno = errno;
	 return -1;		   /* error return */
      default:
	 bsock->b_errno = 0;
	 return 1;
      }
   }
}

#ifndef NETDB_INTERNAL
#define NETDB_INTERNAL	-1	   /* See errno. */
#endif
#ifndef NETDB_SUCCESS
#define NETDB_SUCCESS	0	   /* No problem. */
#endif
#ifndef HOST_NOT_FOUND
#define HOST_NOT_FOUND	1	   /* Authoritative Answer Host not found. */
#endif
#ifndef TRY_AGAIN
#define TRY_AGAIN	2	   /* Non-Authoritative Host not found, or SERVERFAIL. */
#endif
#ifndef NO_RECOVERY
#define NO_RECOVERY	3	   /* Non recoverable errors, FORMERR, REFUSED, NOTIMP. */
#endif
#ifndef NO_DATA
#define NO_DATA 	4	   /* Valid name, no data record of requested type. */
#endif

/*
 * Get human readable error for gethostbyname()
 */
static const char *gethost_strerror()
{
   switch (h_errno) {
   case NETDB_INTERNAL:
      return strerror(errno);
   case NETDB_SUCCESS:
      return "No problem.";
   case HOST_NOT_FOUND:
      return "Authoritative answer Host not found.";
   case TRY_AGAIN:
      return "Non-authoritative Host not found, or ServerFail.";
   case NO_RECOVERY:
      return "Non-recoverable errors, FORMERR, REFUSED, or NOTIMP.";
   case NO_DATA:
      return "Valid name, no data record of resquested type.";
   default:
      return "Unknown error.";
   }
}


static pthread_mutex_t ip_mutex = PTHREAD_MUTEX_INITIALIZER;


static IPADDR *add_any(int family)
{
   IPADDR *addr = new IPADDR(family);
   addr->set_type(IPADDR::R_MULTIPLE);
   addr->set_addr_any();
   return addr;
}

static int resolv_host(int family, const char *host, dlist * addr_list,
		       const char **errstr)
{
   struct hostent *hp;

   P(ip_mutex);
#ifdef HAVE_GETHOSTBYNAME2
   if ((hp = gethostbyname2(host, family)) == NULL) {
#else
   if ((hp = gethostbyname(host)) == NULL) {
#endif
      /* may be the strerror give not the right result -:( */
      *errstr = gethost_strerror();
      V(ip_mutex);
      return 0;
   } else {
      char **p;
      for (p = hp->h_addr_list; *p != 0; p++) {
	 IPADDR *addr =  new IPADDR(hp->h_addrtype);
	 addr->set_type(IPADDR::R_MULTIPLE);
	 if (addr->get_family() == AF_INET) {
	     addr->set_addr4((struct in_addr*)*p);
	 }
#ifdef HAVE_IPV6
	 else {
	     addr->set_addr6((struct in6_addr*)*p);
	 }
#endif
	 addr_list->append(addr);
      }
      V(ip_mutex);
   }
   return 1;
}

/*
 * i host = 0 mean INADDR_ANY only ipv4
 */
dlist *bnet_host2ipaddrs(const char *host, int family, const char **errstr)
{
   struct in_addr inaddr;
   IPADDR *addr = 0;
#ifdef HAVE_IPV6
   struct in6_addr inaddr6;
#endif

   dlist *addr_list = new dlist(addr, &addr->link);
   if (!host || host[0] == '\0') {
      if (family != 0) {
	 addr_list->append(add_any(family));
      } else {
	 addr_list->append(add_any(AF_INET));
#ifdef HAVE_IPV6
	 addr_list->append(add_any(AF_INET6));
#endif
      }
   } else if ((inaddr.s_addr = inet_addr(host)) != INADDR_NONE) {
      addr = new IPADDR(AF_INET);
      addr->set_type(IPADDR::R_MULTIPLE);
      addr->set_addr4(&inaddr);
      addr_list->append(addr);
   } else
#ifdef HAVE_IPV6
   if (inet_pton(AF_INET6, host, &inaddr6) > 1) {
      addr = new IPADDR(AF_INET6);
      addr->set_type(IPADDR::R_MULTIPLE);
      addr->set_addr6(&inaddr6);
      addr_list->append(addr);
   } else
#endif
   {
      if (family != 0) {
	 if (!resolv_host(family, host, addr_list, errstr)) {
	    free_addresses(addr_list);
	    return 0;
	 }
      } else {
	 int done = 0;
	 done |= resolv_host(AF_INET, host, addr_list, errstr);
#ifdef HAVE_IPV6
	 done |= resolv_host(AF_INET6, host, addr_list, errstr);
#endif
	 if (!done) {
	    free_addresses(addr_list);
	    return 0;
	 }
      }
   }
   return addr_list;
}

/*     
 * Open a TCP connection to the UPS network server
 * Returns NULL
 * Returns BSOCK * pointer on success
 *
 */
static BSOCK *bnet_open(JCR * jcr, const char *name, char *host, char *service,
			int port, int *fatal)
{
   int sockfd = -1;
   dlist *addr_list;
   IPADDR *ipaddr;
   bool connected = false;
   int turnon = 1;
   const char *errstr;
   int save_errno = 0;

   /* 
    * Fill in the structure serv_addr with the address of
    * the server that we want to connect with.
    */
   if ((addr_list = bnet_host2ipaddrs(host, 0, &errstr)) == NULL) {
      Qmsg2(jcr, M_ERROR, 0, "gethostbyname() for host \"%s\" failed: ERR=%s\n",
	    host, errstr);
      free((void *)errstr);
      *fatal = 1;
      return NULL;
   }

   foreach_dlist(ipaddr, addr_list) {
      ipaddr->set_port(htons(port));
      char allbuf[256 * addr_list->size()];
      char curbuf[256];
      Dmsg2(100, "Current %sAll %s\n", 
		   ipaddr->build_address_str(curbuf, sizeof(curbuf)), 
		   build_addresses_str(addr_list, allbuf, sizeof(allbuf)));
      /* Open a TCP socket */
      if ((sockfd = socket(ipaddr->get_family(), SOCK_STREAM, 0)) < 0) {
	 berrno be;
	 save_errno = errno;
	 *fatal = 1;
         Pmsg3(000, "Socket open error. proto=%d port=%d. ERR=%s\n", 
	    ipaddr->get_family(), ipaddr->get_port(), be.strerror());
	 continue;
      }
      /*
       * Receive notification when connection dies.
       */
      if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, (sockopt_val_t) & turnon,
	   sizeof(turnon)) < 0) {
	 berrno be;
         Qmsg1(jcr, M_WARNING, 0, _("Cannot set SO_KEEPALIVE on socket: %s\n"),
	       be.strerror());
      }
      /* connect to server */
      if (connect(sockfd, ipaddr->get_sockaddr(), ipaddr->get_sockaddr_len()) < 0) {
	 save_errno = errno;
	 close(sockfd);
	 continue;
      }
      *fatal = 0;
      connected = true;
      break;
   }

   free_addresses(addr_list);
   if (!connected) {
      errno = save_errno;
      return NULL;
   }
   return init_bsock(jcr, sockfd, name, host, port, ipaddr->get_sockaddr());
}

/*
 * Try to connect to host for max_retry_time at retry_time intervals.
 */
BSOCK *bnet_connect(JCR * jcr, int retry_interval, int max_retry_time,
		    const char *name, char *host, char *service, int port,
		    int verbose)
{
   int i;
   BSOCK *bsock;
   int fatal = 0;

   for (i = 0; (bsock = bnet_open(jcr, name, host, service, port, &fatal)) == NULL;
	i -= retry_interval) {
      berrno be;
      if (fatal || (jcr && job_canceled(jcr))) {
	 return NULL;
      }
      Dmsg4(100, "Unable to connect to %s on %s:%d. ERR=%s\n",
	    name, host, port, be.strerror());
      if (i < 0) {
	 i = 60 * 5;		   /* complain again in 5 minutes */
	 if (verbose)
            Qmsg4(jcr, M_WARNING, 0, "Could not connect to %s on %s:%d. ERR=%s\n\
Retrying ...\n", name, host, port, be.strerror());
      }
      bmicrosleep(retry_interval, 0);
      max_retry_time -= retry_interval;
      if (max_retry_time <= 0) {
         Qmsg4(jcr, M_FATAL, 0, _("Unable to connect to %s on %s:%d. ERR=%s\n"),
	       name, host, port, be.strerror());
	 return NULL;
      }
   }
   return bsock;
}


/*
 * Return the string for the error that occurred
 * on the socket. Only the first error is retained.
 */
char *bnet_strerror(BSOCK * bsock)
{
   return strerror(bsock->b_errno);
}

/*
 * Format and send a message
 *  Returns: false on error
 *	     true  on success
 */
bool bnet_fsend(BSOCK * bs, const char *fmt, ...)
{
   va_list arg_ptr;
   int maxlen;

   if (bs->errors || bs->terminated) {
      return false;
   }
   /* This probably won't work, but we vsnprintf, then if we
    * get a negative length or a length greater than our buffer
    * (depending on which library is used), the printf was truncated, so
    * get a bigger buffer and try again.
    */
   for (;;) {
      maxlen = sizeof_pool_memory(bs->msg) - 1;
      va_start(arg_ptr, fmt);
      bs->msglen = bvsnprintf(mp_chr(bs->msg), maxlen, fmt, arg_ptr);
      va_end(arg_ptr);
      if (bs->msglen > 0 && bs->msglen < (maxlen - 5)) {
	 break;
      }
      bs->msg = realloc_pool_memory(bs->msg, maxlen + maxlen / 2);
   }
   return bnet_send(bs);
}

/* 
 * Set the network buffer size, suggested size is in size.
 *  Actual size obtained is returned in bs->msglen
 *
 *  Returns: 0 on failure
 *	     1 on success
 */
bool bnet_set_buffer_size(BSOCK * bs, uint32_t size, int rw)
{
   uint32_t dbuf_size, start_size;
#if defined(IP_TOS) && defined(IPTOS_THROUGHPUT)
   int opt;

   opt = IPTOS_THROUGHPUT;
   setsockopt(bs->fd, IPPROTO_IP, IP_TOS, (sockopt_val_t) & opt, sizeof(opt));
#endif

   if (size != 0) {
      dbuf_size = size;
   } else {
      dbuf_size = DEFAULT_NETWORK_BUFFER_SIZE;
   }
   start_size = dbuf_size;
   if ((bs->msg = realloc_pool_memory(bs->msg, dbuf_size + 100)) == NULL) {
      Qmsg0(bs->jcr, M_FATAL, 0, _("Could not malloc BSOCK data buffer\n"));
      return false;
   }
   if (rw & BNET_SETBUF_READ) {
      while ((dbuf_size > TAPE_BSIZE) && (setsockopt(bs->fd, SOL_SOCKET, 
	      SO_RCVBUF, (sockopt_val_t) & dbuf_size, sizeof(dbuf_size)) < 0)) {
	 berrno be;
         Qmsg1(bs->jcr, M_ERROR, 0, _("sockopt error: %s\n"), be.strerror());
	 dbuf_size -= TAPE_BSIZE;
      }
      Dmsg1(200, "set network buffer size=%d\n", dbuf_size);
      if (dbuf_size != start_size) {
	 Qmsg1(bs->jcr, M_WARNING, 0,
               _("Warning network buffer = %d bytes not max size.\n"), dbuf_size);
      }
      if (dbuf_size % TAPE_BSIZE != 0) {
	 Qmsg1(bs->jcr, M_ABORT, 0,
               _("Network buffer size %d not multiple of tape block size.\n"),
	       dbuf_size);
      }
   }
   if (size != 0) {
      dbuf_size = size;
   } else {
      dbuf_size = DEFAULT_NETWORK_BUFFER_SIZE;
   }
   start_size = dbuf_size;
   if (rw & BNET_SETBUF_WRITE) {
      while ((dbuf_size > TAPE_BSIZE) && (setsockopt(bs->fd, SOL_SOCKET, 
	      SO_SNDBUF, (sockopt_val_t) & dbuf_size, sizeof(dbuf_size)) < 0)) {
	 berrno be;
         Qmsg1(bs->jcr, M_ERROR, 0, _("sockopt error: %s\n"), be.strerror());
	 dbuf_size -= TAPE_BSIZE;
      }
      Dmsg1(200, "set network buffer size=%d\n", dbuf_size);
      if (dbuf_size != start_size) {
	 Qmsg1(bs->jcr, M_WARNING, 0,
               _("Warning network buffer = %d bytes not max size.\n"), dbuf_size);
      }
      if (dbuf_size % TAPE_BSIZE != 0) {
	 Qmsg1(bs->jcr, M_ABORT, 0,
               _("Network buffer size %d not multiple of tape block size.\n"),
	       dbuf_size);
      }
   }

   bs->msglen = dbuf_size;
   return true;
}

/*
 * Send a network "signal" to the other end 
 *  This consists of sending a negative packet length
 *
 *  Returns: false on failure
 *	     true  on success
 */
bool bnet_sig(BSOCK * bs, int sig)
{
   bs->msglen = sig;
   return bnet_send(bs);
}

/*
 * Convert a network "signal" code into
 * human readable ASCII.
 */
const char *bnet_sig_to_ascii(BSOCK * bs)
{
   static char buf[30];
   switch (bs->msglen) {
   case BNET_EOD:
      return "BNET_EOD";           /* end of data stream */
   case BNET_EOD_POLL:
      return "BNET_EOD_POLL";
   case BNET_STATUS:
      return "BNET_STATUS";
   case BNET_TERMINATE:
      return "BNET_TERMINATE";     /* terminate connection */
   case BNET_POLL:
      return "BNET_POLL";
   case BNET_HEARTBEAT:
      return "BNET_HEARTBEAT";
   case BNET_HB_RESPONSE:
      return "BNET_HB_RESPONSE";
   case BNET_PROMPT:
      return "BNET_PROMPT";
   default:
      sprintf(buf, "Unknown sig %d", bs->msglen);
      return buf;
   }
}


/* Initialize internal socket structure.  
 *  This probably should be done in net_open
 */
BSOCK *init_bsock(JCR * jcr, int sockfd, const char *who, const char *host, int port,
		  struct sockaddr * client_addr)
{
   BSOCK *bsock = (BSOCK *)malloc(sizeof(BSOCK));
   memset(bsock, 0, sizeof(BSOCK));
   bsock->fd = sockfd;
   bsock->errors = 0;
   bsock->msg = get_pool_memory(PM_MESSAGE);
   bsock->errmsg = get_pool_memory(PM_MESSAGE);
   bsock->who = bstrdup(who);
   bsock->host = bstrdup(host);
   bsock->port = port;
   memcpy(&bsock->client_addr, client_addr, sizeof(bsock->client_addr));
   /*
    * ****FIXME**** reduce this to a few hours once   
    *	heartbeats are implemented
    */
   bsock->timeout = 60 * 60 * 6 * 24;	/* 6 days timeout */
   bsock->jcr = jcr;
   return bsock;
}

BSOCK *dup_bsock(BSOCK * osock)
{
   BSOCK *bsock = (BSOCK *) malloc(sizeof(BSOCK));
   memcpy(bsock, osock, sizeof(BSOCK));
   bsock->msg = get_pool_memory(PM_MESSAGE);
   bsock->errmsg = get_pool_memory(PM_MESSAGE);
   if (osock->who) {
      bsock->who = bstrdup(osock->who);
   }
   if (osock->host) {
      bsock->host = bstrdup(osock->host);
   }
   bsock->duped = true;
   return bsock;
}

/* Close the network connection */
void bnet_close(BSOCK * bsock)
{
   BSOCK *next;

   for (; bsock != NULL; bsock = next) {
      next = bsock->next;
      if (!bsock->duped) {
	 if (bsock->timed_out) {
	    shutdown(bsock->fd, 2);	/* discard any pending I/O */
	 }
	 socketClose(bsock->fd);   /* normal close */
      }
      term_bsock(bsock);
   }
   return;
}

void term_bsock(BSOCK * bsock)
{
   if (bsock->msg) {
      free_pool_memory(bsock->msg);
      bsock->msg = NULL;
   } else {
      ASSERT(1 == 0);		   /* double close */
   }
   if (bsock->errmsg) {
      free_pool_memory(bsock->errmsg);
      bsock->errmsg = NULL;
   }
   if (bsock->who) {
      free(bsock->who);
      bsock->who = NULL;
   }
   if (bsock->host) {
      free(bsock->host);
      bsock->host = NULL;
   }
   free(bsock);
}
