/*
 *
 *   Bacula Director -- User Agent Database Query Commands
 *
 *     Kern Sibbald, December MMI
 *
 *   Version $Id$
 */

/*
   Copyright (C) 2000-2003 Kern Sibbald and John Walker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

 */

#include "bacula.h"
#include "dird.h"

extern DIRRES *director;

static POOLMEM *substitute_prompts(UAContext *ua, 
		       POOLMEM *query, char **prompt, int nprompt);

/*
 * Read a file containing SQL queries and prompt
 *  the user to select which one.
 *
 *   File format:
 *   #	=> comment
 *   :prompt for query
 *   *prompt for subst %1
 *   *prompt for subst %2
 *   ...
 *   SQL statement possibly terminated by ;
 *   :next query prompt
 */
int querycmd(UAContext *ua, char *cmd)
{
   FILE *fd = NULL;
   POOLMEM *query = get_pool_memory(PM_MESSAGE);
   char line[1000];
   int i, item, len;
   char *prompt[9];
   int nprompt = 0;;
   char *query_file = director->query_file;
   
   if (!open_db(ua)) {
      goto bail_out;
   }
   if ((fd=fopen(query_file, "r")) == NULL) {
      bsendmsg(ua, "Could not open %s: ERR=%s\n", query_file,
	 strerror(errno));
      goto bail_out;
   }

   start_prompt(ua, _("Available queries:\n"));
   while (fgets(line, sizeof(line), fd) != NULL) {
      if (line[0] == ':') {
	 strip_trailing_junk(line);
	 add_prompt(ua, line+1);
      }
   }
   if ((item=do_prompt(ua, "", _("Choose a query"), NULL, 0)) < 0) {
      goto bail_out;
   }
   rewind(fd);
   i = -1;
   while (fgets(line, sizeof(line), fd) != NULL) {
      if (line[0] == ':') {
	 i++;
      }
      if (i == item) {
	 break;
      }
   }
   if (i != item) {
      bsendmsg(ua, _("Could not find query.\n"));
      goto bail_out;
   }
   query[0] = 0;
   for (i=0; i<9; i++) {
      prompt[i] = NULL;
   }
   while (fgets(line, sizeof(line), fd) != NULL) {
      if (line[0] == '#') {
	 continue;
      }
      if (line[0] == ':') {
	 break;
      }
      strip_trailing_junk(line);
      len = strlen(line);
      if (line[0] == '*') {            /* prompt */
	 if (nprompt >= 9) {
            bsendmsg(ua, _("Too many prompts in query, max is 9.\n"));
	 } else {
            line[len++] = ' ';
	    line[len] = 0;
	    prompt[nprompt++] = bstrdup(line+1);
	    continue;
	 }
      }  
      if (*query != 0) {
         pm_strcat(&query, " ");
      }
      pm_strcat(&query, line);
      if (line[len-1] != ';') {
	 continue;
      }
      line[len-1] = 0;		   /* zap ; */
      if (query[0] != 0) {
	 query = substitute_prompts(ua, query, prompt, nprompt);
         Dmsg1(100, "Query2=%s\n", query);
         if (query[0] == '!') {
	    db_list_sql_query(ua->jcr, ua->db, query+1, prtit, ua, 0, VERT_LIST);
	 } else if (!db_list_sql_query(ua->jcr, ua->db, query, prtit, ua, 1, HORZ_LIST)) {
            bsendmsg(ua, "%s\n", query);
	 }
	 query[0] = 0;
      }
   } /* end while */

   if (query[0] != 0) {
      query = substitute_prompts(ua, query, prompt, nprompt);
      Dmsg1(100, "Query2=%s\n", query);
         if (query[0] == '!') {
	    db_list_sql_query(ua->jcr, ua->db, query+1, prtit, ua, 0, VERT_LIST);
	 } else if (!db_list_sql_query(ua->jcr, ua->db, query, prtit, ua, 1, HORZ_LIST)) {
            bsendmsg(ua, "%s\n", query);
	 }
   }

bail_out:
   if (fd) {
      fclose(fd);
   }
   free_pool_memory(query);
   for (i=0; i<nprompt; i++) {
      free(prompt[i]);
   }
   return 1;
}

static POOLMEM *substitute_prompts(UAContext *ua, 
		       POOLMEM *query, char **prompt, int nprompt)
{
   char *p, *q, *o;
   POOLMEM *new_query;
   int i, n, len, olen;
   char *subst[9];

   if (nprompt == 0) {
      return query;
   }
   for (i=0; i<9; i++) {
      subst[i] = NULL;
   }
   new_query = get_pool_memory(PM_FNAME);
   o = new_query;
   for (q=query; (p=strchr(q, '%')); ) {
      if (p) {
	olen = o - new_query;
	new_query = check_pool_memory_size(new_query, olen + p - q + 10);
	o = new_query + olen;
	 while (q < p) {	      /* copy up to % */
	    *o++ = *q++;
	 }
	 p++;
	 switch (*p) {
         case '1':
         case '2':
         case '3':
         case '4':
         case '5':
         case '6':
         case '7':
         case '8':
         case '9':
            n = (int)(*p) - (int)'1';
	    if (prompt[n]) {
	       if (!subst[n]) {
		  if (!get_cmd(ua, prompt[n])) {
		     q += 2;
		     break;
		  }
	       }
	       len = strlen(ua->cmd);
	       p = (char *)malloc(len * 2 + 1);
	       db_escape_string(p, ua->cmd, len);
	       subst[n] = p;
	       olen = o - new_query;
	       new_query = check_pool_memory_size(new_query, olen + strlen(p) + 10);
	       o = new_query + olen;
	       while (*p) {
		  *o++ = *p++;
	       }
	    } else {
               bsendmsg(ua, _("Warning prompt %d missing.\n"), n+1);
	    }
	    q += 2;
	    break;
         case '%':
            *o++ = '%';
	    q += 2;
	    break;
	 default:
            *o++ = '%';
	    q++;
	    break;
	 }
      }
   }
   olen = o - new_query;
   new_query = check_pool_memory_size(new_query, olen + strlen(q) + 10);
   o = new_query + olen;
   while (*q) {
      *o++ = *q++;
   }
   *o = 0;
   for (i=0; i<9; i++) {
      if (subst[i]) {
	 free(subst[i]);
      }
   }
   free_pool_memory(query);
   return new_query;
}

/*
 * Get general SQL query for Catalog
 */
int sqlquerycmd(UAContext *ua, char *cmd)
{
   POOLMEM *query = get_pool_memory(PM_MESSAGE);
   int len;
   char *msg;

   if (!open_db(ua)) {
      free_pool_memory(query);
      return 1;
   }
   *query = 0;

   bsendmsg(ua, _("Entering SQL query mode.\n\
Terminate each query with a semicolon.\n\
Terminate query mode with a blank line.\n"));
   msg = "Enter SQL query: ";
   while (get_cmd(ua, msg)) {
      len = strlen(ua->cmd);
      Dmsg2(400, "len=%d cmd=%s:\n", len, ua->cmd);
      if (len == 0) {
	 break;
      }
      query = check_pool_memory_size(query, len + 1);
      if (*query != 0) {
         strcat(query, " ");
      }
      strcat(query, ua->cmd);
      if (ua->cmd[len-1] == ';') {
	 ua->cmd[len-1] = 0;	      /* zap ; */
	 /* Submit query */
	 db_list_sql_query(ua->jcr, ua->db, query, prtit, ua, 1, HORZ_LIST);
	 *query = 0;		      /* start new query */
         msg = _("Enter SQL query: ");
      } else {
         msg = _("Add to SQL query: ");
      }
   }
   free_pool_memory(query);
   bsendmsg(ua, _("End query mode.\n"));
   return 1; 
}
