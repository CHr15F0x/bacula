/*
 *   Main configuration file parser for Bacula User Agent
 *    some parts may be split into separate files such as
 *    the schedule configuration (sch_config.c).
 *
 *   Note, the configuration file parser consists of three parts
 *
 *   1. The generic lexical scanner in lib/lex.c and lib/lex.h
 *
 *   2. The generic config  scanner in lib/parse_config.c and 
 *	lib/parse_config.h.
 *	These files contain the parser code, some utility
 *	routines, and the common store routines (name, int,
 *	string).
 *
 *   3. The daemon specific file, which contains the Resource
 *	definitions as well as any specific store routines
 *	for the resource records.
 *
 *     Kern Sibbald, January MM, September MM
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

/* Define the first and last resource ID record
 * types. Note, these should be unique for each
 * daemon though not a requirement.
 */
int r_first = R_FIRST;
int r_last  = R_LAST;

/* Forward referenced subroutines */


/* We build the current resource here as we are
 * scanning the resource configuration definition,
 * then move it to allocated memory when the resource
 * scan is complete.
 */
URES res_all;
int  res_all_size = sizeof(res_all);

/* Definition of records permitted within each
 * resource with the routine to process the record 
 * information.
 */ 

/*  Console "globals" */
static struct res_items cons_items[] = {
   {"name",        store_name,     ITEM(res_cons.hdr.name), 0, ITEM_REQUIRED, 0},
   {"description", store_str,      ITEM(res_cons.hdr.desc), 0, 0, 0},
   {"rcfile",      store_dir,      ITEM(res_cons.rc_file), 0, 0, 0},
   {"historyfile", store_dir,      ITEM(res_cons.hist_file), 0, 0, 0},
   {"requiressl",  store_yesno,    ITEM(res_cons.require_ssl), 1, ITEM_DEFAULT, 0},
   {"password",    store_password, ITEM(res_cons.password), 0, ITEM_REQUIRED, 0},
   {NULL, NULL, NULL, 0, 0, 0} 
};


/*  Director's that we can contact */
static struct res_items dir_items[] = {
   {"name",        store_name,     ITEM(res_dir.hdr.name), 0, ITEM_REQUIRED, 0},
   {"description", store_str,      ITEM(res_dir.hdr.desc), 0, 0, 0},
   {"dirport",     store_int,      ITEM(res_dir.DIRport),  0, ITEM_DEFAULT, 9101},
   {"address",     store_str,      ITEM(res_dir.address),  0, 0, 0},
   {"password",    store_password, ITEM(res_dir.password), 0, ITEM_REQUIRED, 0},
   {"enablessl",   store_yesno,    ITEM(res_dir.enable_ssl), 1, ITEM_DEFAULT, 0},
   {NULL, NULL, NULL, 0, 0, 0} 
};

/* 
 * This is the master resource definition.  
 * It must have one item for each of the resources.
 */
struct s_res resources[] = {
   {"console",       cons_items,  R_CONSOLE,   NULL},
   {"director",      dir_items,   R_DIRECTOR,  NULL},
   {NULL,	     NULL,	  0,	       NULL}
};


/* Dump contents of resource */
void dump_resource(int type, RES *reshdr, void sendit(void *sock, char *fmt, ...), void *sock)
{
   URES *res = (URES *)reshdr;
   int recurse = 1;

   if (res == NULL) {
      printf("No record for %d %s\n", type, res_to_str(type));
      return;
   }
   if (type < 0) {		      /* no recursion */
      type = - type;
      recurse = 0;
   }
   switch (type) {
      case R_CONSOLE:
         printf("Console: name=%s rcfile=%s histfile=%s\n", reshdr->name,
		res->res_cons.rc_file, res->res_cons.hist_file);
	 break;
      case R_DIRECTOR:
         printf("Director: name=%s address=%s DIRport=%d\n", reshdr->name, 
		 res->res_dir.address, res->res_dir.DIRport);
	 break;
      default:
         printf("Unknown resource type %d\n", type);
   }
   if (recurse && res->res_dir.hdr.next) {
      dump_resource(type, res->res_dir.hdr.next, sendit, sock);
   }
}

/* 
 * Free memory of resource.  
 * NB, we don't need to worry about freeing any references
 * to other resources as they will be freed when that 
 * resource chain is traversed.  Mainly we worry about freeing
 * allocated strings (names).
 */
void free_resource(int type)
{
   URES *res;
   RES *nres;
   int rindex = type - r_first;

   res = (URES *)resources[rindex].res_head;

   if (res == NULL)
      return;

   /* common stuff -- free the resource name */
   nres = (RES *)res->res_dir.hdr.next;
   if (res->res_dir.hdr.name) {
      free(res->res_dir.hdr.name);
   }
   if (res->res_dir.hdr.desc) {
      free(res->res_dir.hdr.desc);
   }

   switch (type) {
      case R_CONSOLE:
	 if (res->res_cons.rc_file) {
	    free(res->res_cons.rc_file);
	 }
	 if (res->res_cons.hist_file) {
	    free(res->res_cons.hist_file);
	 }
      case R_DIRECTOR:
	 if (res->res_dir.address)
	    free(res->res_dir.address);
	 break;
      default:
         printf("Unknown resource type %d\n", type);
   }
   /* Common stuff again -- free the resource, recurse to next one */
   free(res);
   resources[rindex].res_head = nres;
   if (nres)
      free_resource(type);
}

/* Save the new resource by chaining it into the head list for
 * the resource. If this is pass 2, we update any resource
 * pointers (currently only in the Job resource).
 */
void save_resource(int type, struct res_items *items, int pass)
{
   URES *res;
   int rindex = type - r_first;
   int i, size;    
   int error = 0;

   /* 
    * Ensure that all required items are present
    */
   for (i=0; items[i].name; i++) {
      if (items[i].flags & ITEM_REQUIRED) {
	    if (!bit_is_set(i, res_all.res_dir.hdr.item_present)) {  
               Emsg2(M_ABORT, 0, "%s item is required in %s resource, but not found.\n",
		 items[i].name, resources[rindex]);
	     }
      }
   }

   /* During pass 2, we looked up pointers to all the resources
    * referrenced in the current resource, , now we
    * must copy their address from the static record to the allocated
    * record.
    */
   if (pass == 2) {
      switch (type) {
	 /* Resources not containing a resource */
	 case R_CONSOLE:
	 case R_DIRECTOR:
	    break;

	 default:
            Emsg1(M_ERROR, 0, "Unknown resource type %d\n", type);
	    error = 1;
	    break;
      }
      /* Note, the resoure name was already saved during pass 1,
       * so here, we can just release it.
       */
      if (res_all.res_dir.hdr.name) {
	 free(res_all.res_dir.hdr.name);
	 res_all.res_dir.hdr.name = NULL;
      }
      if (res_all.res_dir.hdr.desc) {
	 free(res_all.res_dir.hdr.desc);
	 res_all.res_dir.hdr.desc = NULL;
      }
      return;
   }

   /* The following code is only executed during pass 1 */
   switch (type) {
      case R_CONSOLE:
	 size = sizeof(CONRES);
	 break;
      case R_DIRECTOR:
	 size = sizeof(DIRRES);
	 break;
      default:
         printf("Unknown resource type %d\n", type);
	 error = 1;
	 size = 1;
	 break;
   }
   /* Common */
   if (!error) {
      res = (URES *)malloc(size);
      memcpy(res, &res_all, size);
      if (!resources[rindex].res_head) {
	 resources[rindex].res_head = (RES *)res; /* store first entry */
      } else {
	 RES *next;
	 for (next=resources[rindex].res_head; next->next; next=next->next) {
	    if (strcmp(next->name, res->res_dir.hdr.name) == 0) {
	       Emsg2(M_ERROR_TERM, 0,
                  _("Attempt to define second %s resource named \"%s\" is not permitted.\n"),
		  resources[rindex].name, res->res_dir.hdr.name);
	    }
	 }
	 next->next = (RES *)res;
         Dmsg2(90, "Inserting %s res: %s\n", res_to_str(type),
	       res->res_dir.hdr.name);
      }
   }
}
