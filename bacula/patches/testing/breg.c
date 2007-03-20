/*
 * Manipulation routines for BREGEXP list
 *
 *  Eric Bollengier, March 2007
 *
 *  Version $Id$
 *
 */
/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2006-2006 Free Software Foundation Europe e.V.

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


#include "bacula.h"

#include "breg.h"
#include "mem_pool.h"

BREGEXP *new_bregexp(const char *motif)
{
   Dmsg0(500, "bregexp: creating new bregexp object\n");
   BREGEXP *self = (BREGEXP *)malloc(sizeof(BREGEXP));
   memset(self, 0, sizeof(BREGEXP));
   
   if (!self->extract_regexp(motif)) {
//      Dmsg0(100, "bregexp: extract_regexp error\n");
      printf("bregexp: extract_regexp error\n");
      free_bregexp(self);
      return NULL;
   }

   static int _start[RE_NREGS];
   static int _end[RE_NREGS];

   self->result = get_pool_memory(PM_FNAME);
   self->result[0] = '\0';

#ifdef HAVE_REGEX_H
   /* TODO: que devient cette memoire... */
   self->_regs_match = (int *) malloc (2*RE_NREGS * sizeof(int));

   self->regs.num_regs = RE_NREGS;
   self->regs.start = self->_regs_match;
   self->regs.end   = self->_regs_match+(RE_NREGS * sizeof(int));
#endif 

   return self;
}

void free_bregexp(BREGEXP *self)
{
   Dmsg0(500, "bregexp: freeing BREGEXP object\n");

   if (self->expr) {
      free(self->expr);
   }
   if (self->result) {
      free_pool_memory(self->result);
   }
   if (self->_regs_match) {
      free(self->_regs_match);
   }

   regfree(&self->preg);
   free(self);
}

void free_bregexps(alist *bregexps)
{
   Dmsg0(500, "bregexp: freeing all BREGEXP object\n");

   BREGEXP *elt;
   foreach_alist(elt, bregexps) {
      free_bregexp(elt);
   }
}

bool BREGEXP::extract_regexp(const char *motif)
{
   
   if (!motif) {
      return false;
   }
   char sep = motif[0];
   char *search = (char *) motif + 1;
   char *replace;
   int options = REG_EXTENDED | REG_NEWLINE;
   bool ok = false;
   bool found_motif = false;

   /* extract 1st part */
   char *dest = expr = bstrdup(motif);

   while (*search && !ok) {
      if (*search == '\\' && *dest == sep) {
	 *dest++ = *++search;       /* we skip separator */ 
      } else if (*search == sep) {
	 *dest++ = '\0';

	 if (subst) {	/* already have found motif */
	    ok = true;
	 } else {
	    *dest++ = *++search; /* we skip separator */ 
	    subst = dest;	 /* get replaced string */
	 }
      } else {
	 *dest++ = *search++;
      }
   }
   *dest = '\0';		/* in case of */
   
   if (!ok || !subst) {
      /* bad regexp */
      return false;
   }

   /* find options */
   while (*search) {
      if (*search == 'i') {
	 options |= REG_ICASE;
      }
      if (*search == 'g') {
	      /* recherche multiple*/
      }
      search++;
   }

   int rc = regcomp(&preg, expr, options);
   if (rc != 0) {
      char prbuf[500];
      regerror(rc, &preg, prbuf, sizeof(prbuf));
      printf("bregexp: compile error: %s\n", prbuf);
//      Dmsg1(100, "bregexp: compile error: %s\n", prbuf);
      return false;
   }

   return true;
}

#ifndef HAVE_REGEX_H
 #define BREGEX_CAST unsigned
#else
 #define BREGEX_CAST const
#endif

/* return regexp->result */
char *BREGEXP::replace(const char *fname)
{
   int flen = strlen(fname);
   int rc = re_search(&preg, (BREGEX_CAST char*) fname, flen, 0, flen, &regs);

   if (rc < 0) {
      printf("E: regex mismatch\n");
      return return_fname(fname, flen);
   }

   int len = compute_dest_len(fname, &regs);

   if (len) {
      result = check_pool_memory_size(result, len);
      edit_subst(fname, &regs);

   } else {			/* error in substitution */
      printf("E: error in substitution\n");
      return return_fname(fname, flen);
   }

   return result;
} 

char *BREGEXP::return_fname(const char *fname, int len)
{
   result = check_pool_memory_size(result, len+1);
   strcpy(result,fname);
   return result;
}

int BREGEXP::compute_dest_len(const char *fname, struct re_registers *regs)
{
   int len=0;
   char *p;
   char *psubst = subst;
   int no;

   if (!fname || !regs) {
      return 0;
   }

   /* match failed ? */
   if (regs->start[0] < 0) {
      return 0;
   }

   for (p = psubst++; *p ; p = psubst++) {
      /* match $1 \1 back references */
      if ((*p == '$' || *p == '\\') && ('0' <= *psubst && *psubst <= '9')) {
	 no = *psubst++ - '0';

	 /* we check if the back reference exists */
	 if (regs->start[no] >= 0 && regs->end[no] >= 0) { 
	    len += regs->end[no] - regs->start[no];
	 } else {
	    return 0; /* back reference missing */
	 }
      } else {
	 len++;
      }
   }

   /* $0 is replaced by subst */
   len -= regs->end[0] - regs->start[0];
   len += strlen(fname) + 1;

   return len;
}

char *BREGEXP::edit_subst(const char *fname, struct re_registers *regs)
{
   int i;
   char *p;
   char *psubst = subst;
   int no;
   int len;

   /* il faut recopier fname dans dest
    *  on recopie le debut fname -> regs->start[0]
    */
   
   for (i = 0; i < regs->start[0] ; i++) {
      result[i] = fname[i];
   }

   /* on recopie le motif de remplacement (avec tous les $x) */

   for (p = psubst++; *p ; p = psubst++) {
      /* match $1 \1 back references */
      if ((*p == '$' || *p == '\\') && ('0' <= *psubst && *psubst <= '9')) {
	 no = *psubst++ - '0';

	 len = regs->end[no] - regs->start[no];
	 bstrncpy(result + i, fname + regs->start[no], len + 1);
	 i += len ;

      } else {
	 result[i++] = *p;
      }
   }

   /* we copy what is out of the match */
   strcpy(result + i, fname + regs->end[0]);

   return result;
}

void BREGEXP::debug()
{
   printf("expr=[%s]\n", expr);
   printf("subst=[%s]\n", subst);
   printf("result=%s\n", result?result:"(null)");
}

#ifdef TEST

int main(int argc, char **argv)
{
   BREGEXP *reg;
   
   reg = new_bregexp(argv[1]);

   if (reg) {
      reg->replace(argv[2]);
      reg->debug();
      printf("%s => %s\n", argv[1], reg->result);
   }
   exit(0);
}

#endif
