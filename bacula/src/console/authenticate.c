/*
 *
 *   Bacula UA authentication. Provides authentication with
 *     the Director.
 *
 *     Kern Sibbald, June MMI
 *
 *    This routine runs as a thread and must be thread reentrant.
 *
 *  Basic tasks done here:
 *
 */
/*
   Copyright (C) 2000, 2001 Kern Sibbald and John Walker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "bacula.h"
#include "console_conf.h"
#include "jcr.h"


void senditf(char *fmt, ...);
void sendit(char *buf); 

/* Commands sent to Director */
static char hello[]    = "Hello %s calling\n";

/* Response from Director */
static char OKhello[]   = "1000 OK:";

/* Forward referenced functions */

/*
 * Authenticate Director
 */
int authenticate_director(JCR *jcr, DIRRES *director, CONRES *cons)
{
   BSOCK *dir = jcr->dir_bsock;
   int ssl_need = BNET_SSL_NONE;
   char bashed_name[MAX_NAME_LENGTH];
   char *password;

   /* 
    * Send my name to the Director then do authentication
    */
   if (cons) {
      bstrncpy(bashed_name, cons->hdr.name, sizeof(bashed_name));
      bash_spaces(bashed_name);
      password = cons->password;
   } else {
      bstrncpy(bashed_name, "*UserAgent*", sizeof(bashed_name));
      password = director->password;
   }
   bnet_fsend(dir, hello, bashed_name);

   if (!cram_md5_get_auth(dir, password, ssl_need) || 
       !cram_md5_auth(dir, password, ssl_need)) {
      sendit( _("Director authorization problem.\n"
            "Most likely the passwords do not agree.\n"));  
      return 0;
   }

   Dmsg1(6, ">dird: %s", dir->msg);
   if (bnet_recv(dir) <= 0) {
      senditf(_("Bad response to Hello command: ERR=%s\n"),
	 bnet_strerror(dir));
      return 0;
   }
   Dmsg1(10, "<dird: %s", dir->msg);
   if (strncmp(dir->msg, OKhello, sizeof(OKhello)-1) != 0) {
      sendit(_("Director rejected Hello command\n"));
      return 0;
   } else {
      sendit(dir->msg);
   }
   return 1;
}
