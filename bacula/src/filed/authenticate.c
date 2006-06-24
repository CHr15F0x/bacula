/*
 * Authenticate Director who is attempting to connect.
 *
 *   Kern Sibbald, October 2000
 *
 *   Version $Id$
 *
 */
/*
   Copyright (C) 2000-2005 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as amended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */

#include "bacula.h"
#include "filed.h"

static char OK_hello[]  = "2000 OK Hello\n";
static char Dir_sorry[] = "2999 No go\n";
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*********************************************************************
 *
 */
static int authenticate(int rcode, BSOCK *bs, JCR* jcr)
{
   POOLMEM *dirname = get_pool_memory(PM_MESSAGE);
   DIRRES *director = NULL;
   int tls_local_need = BNET_TLS_NONE;
   int tls_remote_need = BNET_TLS_NONE;
   bool auth_success = false;
   alist *verify_list = NULL;
   btimer_t *tid = NULL;

   if (rcode != R_DIRECTOR) {
      Dmsg1(50, "I only authenticate directors, not %d\n", rcode);
      Emsg1(M_FATAL, 0, _("I only authenticate directors, not %d\n"), rcode);
      goto auth_fatal;
   }
   if (bs->msglen < 25 || bs->msglen > 200) {
      Dmsg2(50, "Bad Hello command from Director at %s. Len=%d.\n",
            bs->who, bs->msglen);
      char addr[64];
      char *who = bnet_get_peer(bs, addr, sizeof(addr)) ? bs->who : addr;
      Emsg2(M_FATAL, 0, _("Bad Hello command from Director at %s. Len=%d.\n"),
             who, bs->msglen);
      goto auth_fatal;
   }
   dirname = check_pool_memory_size(dirname, bs->msglen);

   if (sscanf(bs->msg, "Hello Director %s calling\n", dirname) != 1) {
       char addr[64];
       char *who = bnet_get_peer(bs, addr, sizeof(addr)) ? bs->who : addr;
      bs->msg[100] = 0;
      Dmsg2(50, "Bad Hello command from Director at %s: %s\n",
            bs->who, bs->msg);
      Emsg2(M_FATAL, 0, _("Bad Hello command from Director at %s: %s\n"),
            who, bs->msg);
      goto auth_fatal;
   }
   unbash_spaces(dirname);
   LockRes();
   foreach_res(director, R_DIRECTOR) {
      if (strcmp(director->hdr.name, dirname) == 0)
         break;
   }
   UnlockRes();
   if (!director) {
       char addr[64];
       char *who = bnet_get_peer(bs, addr, sizeof(addr)) ? bs->who : addr;
      Emsg2(M_FATAL, 0, _("Connection from unknown Director %s at %s rejected.\n"), 
            dirname, who);
      goto auth_fatal;
   }

   if (have_tls) {
      /* TLS Requirement */
      if (director->tls_enable) {
         if (director->tls_require) {
            tls_local_need = BNET_TLS_REQUIRED;
         } else {
            tls_local_need = BNET_TLS_OK;
         }
      }

      if (director->tls_verify_peer) {
         verify_list = director->tls_allowed_cns;
      }
   }

   tid = start_bsock_timer(bs, AUTH_TIMEOUT);
   auth_success = cram_md5_auth(bs, director->password, tls_local_need);  
   if (auth_success) {
      auth_success = cram_md5_get_auth(bs, director->password, &tls_remote_need);
      if (!auth_success) {
          char addr[64];
          char *who = bnet_get_peer(bs, addr, sizeof(addr)) ? bs->who : addr;
          Dmsg1(50, "cram_get_auth failed for %s\n", who);
      }
   } else {
       char addr[64];
       char *who = bnet_get_peer(bs, addr, sizeof(addr)) ? bs->who : addr;
       Dmsg1(50, "cram_auth failed for %s\n", who);
   }
   if (!auth_success) {
       Emsg1(M_FATAL, 0, _("Incorrect password given by Director at %s.\n"),
             bs->who);
       goto auth_fatal;
   }

   /* Verify that the remote host is willing to meet our TLS requirements */
   if (tls_remote_need < tls_local_need && tls_local_need != BNET_TLS_OK && tls_remote_need != BNET_TLS_OK) {
      Emsg0(M_FATAL, 0, _("Authorization problem: Remote server did not"
           " advertize required TLS support.\n"));
      auth_success = false;
      goto auth_fatal;
   }

   /* Verify that we are willing to meet the remote host's requirements */
   if (tls_remote_need > tls_local_need && tls_local_need != BNET_TLS_OK && tls_remote_need != BNET_TLS_OK) {
      Emsg0(M_FATAL, 0, _("Authorization problem: Remote server requires TLS.\n"));
      auth_success = false;
      goto auth_fatal;
   }

   if (have_tls) {
      if (tls_local_need >= BNET_TLS_OK && tls_remote_need >= BNET_TLS_OK) {
         /* Engage TLS! Full Speed Ahead! */
         if (!bnet_tls_server(director->tls_ctx, bs, verify_list)) {
            Emsg0(M_FATAL, 0, _("TLS negotiation failed.\n"));
            auth_success = false;
            goto auth_fatal;
         }
      }
   }

auth_fatal:
   if (tid) {
      stop_bsock_timer(tid);
      tid = NULL;
   }
   free_pool_memory(dirname);
   jcr->director = director;
   /* Single thread all failures to avoid DOS */
   if (auth_success) {
      P(mutex);
      bmicrosleep(6, 0);
      V(mutex);
   }
   return (director != NULL);
}

/*
 * Inititiate the communications with the Director.
 * He has made a connection to our server.
 *
 * Basic tasks done here:
 *   We read Director's initial message and authorize him.
 *
 */
int authenticate_director(JCR *jcr)
{
   BSOCK *dir = jcr->dir_bsock;

   if (!authenticate(R_DIRECTOR, dir, jcr)) {
      /* Single thread all failures to avoid DOS */
      P(mutex);
      bmicrosleep(6, 0);
      V(mutex);
      bnet_fsend(dir, "%s", Dir_sorry);
      Emsg0(M_FATAL, 0, _("Unable to authenticate Director\n"));
      return 0;
   }
   return bnet_fsend(dir, "%s", OK_hello);
}

/*
 * First prove our identity to the Storage daemon, then
 * make him prove his identity.
 */
int authenticate_storagedaemon(JCR *jcr)
{
   BSOCK *sd = jcr->store_bsock;
   int tls_local_need = BNET_TLS_NONE;
   int tls_remote_need = BNET_TLS_NONE;
   bool auth_success = false;

   btimer_t *tid = start_bsock_timer(sd, AUTH_TIMEOUT);

   /* TLS Requirement */
   if (have_tls && me->tls_enable) {
      if (me->tls_require) {
         tls_local_need = BNET_TLS_REQUIRED;
      } else {
         tls_local_need = BNET_TLS_OK;
      }
   }

   auth_success = cram_md5_get_auth(sd, jcr->sd_auth_key, &tls_remote_need);
   if (!auth_success) {
      Dmsg1(50, "cram_get_auth failed for %s\n", sd->who);
   } else {
      auth_success = cram_md5_auth(sd, jcr->sd_auth_key, tls_local_need);
      if (!auth_success) {
         Dmsg1(50, "cram_auth failed for %s\n", sd->who);
      }
   }

   /* Destroy session key */
   memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));

   if (!auth_success) {
      Jmsg(jcr, M_FATAL, 0, _("Authorization key rejected by Storage daemon.\n"
       "Please see http://www.bacula.org/rel-manual/faq.html#AuthorizationErrors for help.\n"));
      goto auth_fatal;
   }

   /* Verify that the remote host is willing to meet our TLS requirements */
   if (tls_remote_need < tls_local_need && tls_local_need != BNET_TLS_OK && tls_remote_need != BNET_TLS_OK) {
      Jmsg(jcr, M_FATAL, 0, _("Authorization problem: Remote server did not" 
           " advertise required TLS support.\n"));
      auth_success = false;
      goto auth_fatal;
   }

   /* Verify that we are willing to meet the remote host's requirements */
   if (tls_remote_need > tls_local_need && tls_local_need != BNET_TLS_OK && tls_remote_need != BNET_TLS_OK) {
      Jmsg(jcr, M_FATAL, 0, _("Authorization problem: Remote server requires TLS.\n"));
      auth_success = false;
      goto auth_fatal;
   }

   if (have_tls && tls_local_need >= BNET_TLS_OK && tls_remote_need >= BNET_TLS_OK) {
      /* Engage TLS! Full Speed Ahead! */
      if (!bnet_tls_client(me->tls_ctx, sd)) {
         Jmsg(jcr, M_FATAL, 0, _("TLS negotiation failed.\n"));
         auth_success = false;
         goto auth_fatal;
      }
   }

auth_fatal:
   stop_bsock_timer(tid);
   /* Single thread all failures to avoid DOS */
   if (!auth_success) {
      P(mutex);
      bmicrosleep(6, 0);
      V(mutex);
   }
   return auth_success;
}
