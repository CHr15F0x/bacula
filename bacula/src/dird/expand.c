/*
 *
 *   Bacula Director -- expand.c -- does variable expansion
 *    in particular for the LabelFormat specification.
 *
 *     Kern Sibbald, June MMIII
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



static int date_item(JCR *jcr, int code, 
	      const char **val_ptr, int *val_len, int *val_size)
{
   struct tm tm;
   time_t now = time(NULL);
   localtime_r(&now, &tm);
   int val = 0;
   char buf[10];

   switch (code) {
   case 1:			      /* year */
      val = tm.tm_year + 1900;
      break;
   case 2:			      /* month */
      val = tm.tm_mon + 1;
      break;
   case 3:			      /* day */
      val = tm.tm_mday;
      break;
   case 4:			      /* hour */
      val = tm.tm_hour;
      break;
   case 5:			      /* minute */
      val = tm.tm_min;
      break;
   case 6:			      /* second */
      val = tm.tm_sec;
      break;
   case 7:			      /* Week day */
      val = tm.tm_wday;
      break;
   }
   bsnprintf(buf, sizeof(buf), "%d", val);
   *val_ptr = bstrdup(buf);
   *val_len = strlen(buf);
   *val_size = *val_len;
   return 1;
}

static int job_item(JCR *jcr, int code, 
	      const char **val_ptr, int *val_len, int *val_size)
{
   char *str = " ";
   char buf[20];

   switch (code) {
   case 1:			      /* Job */
      str = jcr->job->hdr.name;
      break;
   case 2:                            /* Director's name */
      str = my_name;
      break;
   case 3:			      /* level */
      str = job_level_to_str(jcr->JobLevel);
      break;
   case 4:			      /* type */
      str = job_type_to_str(jcr->JobType);
      break;
   case 5:			      /* JobId */
      bsnprintf(buf, sizeof(buf), "%d", jcr->JobId);
      str = buf;
      break;
   case 6:			      /* Client */
      str = jcr->client->hdr.name;
      if (!str) {
         str = " ";
      }
      break;
   case 7:			      /* NumVols */
      bsnprintf(buf, sizeof(buf), "%d", jcr->NumVols);
      str = buf;
      break;
   case 8:			      /* Pool */
      str = jcr->pool->hdr.name;   
      break;
   case 9:			      /* Storage */
      str = jcr->store->hdr.name;
      break;
   case 10:			      /* Catalog */
      str = jcr->catalog->hdr.name;
      break;
   case 11:			      /* MediaType */
      str = jcr->store->media_type;
      break;
   case 12:			      /* JobName */
      str = jcr->Job;
      break;
   }
   *val_ptr = bstrdup(str);
   *val_len = strlen(str);
   *val_size = *val_len;
   return 1;
}


struct s_built_in_vars {char *var_name; int code; int (*func)(JCR *jcr, int code,
			 const char **val_ptr, int *val_len, int *val_size);};

static struct s_built_in_vars built_in_vars[] = {
   { N_("Year"),       1, date_item},
   { N_("Month"),      2, date_item},
   { N_("Day"),        3, date_item},
   { N_("Hour"),       4, date_item},
   { N_("Minute"),     5, date_item},
   { N_("Second"),     6, date_item},
   { N_("WeekDay"),    7, date_item},

   { N_("Job"),        1, job_item},
   { N_("Dir"),        2, job_item},
   { N_("Level"),      3, job_item},
   { N_("Type"),       4, job_item},
   { N_("JobId"),      5, job_item},
   { N_("Client"),     6, job_item},
   { N_("NumVols"),    7, job_item},
   { N_("Pool"),       8, job_item},
   { N_("Storage"),    9, job_item},
   { N_("Catalog"),   10, job_item},
   { N_("MediaType"), 11, job_item},
   { N_("JobName"),   12, job_item},

   { NULL, 0, NULL}
};


static var_rc_t lookup_built_in_var(var_t *ctx, void *my_ctx, 
	  const char *var_ptr, int var_len, int var_index, 
	  const char **val_ptr, int *val_len, int *val_size)
{
   JCR *jcr = (JCR *)my_ctx;
   int stat;

   for (int i=0; _(built_in_vars[i].var_name); i++) {
      if (strncmp(_(built_in_vars[i].var_name), var_ptr, var_len) == 0) {
	 stat = (*built_in_vars[i].func)(jcr, built_in_vars[i].code,
	    val_ptr, val_len, val_size);
	 if (stat) {
	    return VAR_OK;
	 }
	 break;
      }
   }
   return VAR_ERR_UNDEFINED_VARIABLE;
}


/*
 * Search counter variables 
 */
static var_rc_t lookup_counter_var(var_t *ctx, void *my_ctx, 
	  const char *var_ptr, int var_len, int var_inc, int var_index, 
	  const char **val_ptr, int *val_len, int *val_size)
{
   char buf[MAXSTRING];
   var_rc_t stat = VAR_ERR_UNDEFINED_VARIABLE;

   if (var_len > (int)sizeof(buf) - 1) {
       return VAR_ERR_OUT_OF_MEMORY;
   }
   memcpy(buf, var_ptr, var_len);
   buf[var_len] = 0;
   LockRes();
   for (COUNTER *counter=NULL; (counter = (COUNTER *)GetNextRes(R_COUNTER, (RES *)counter)); ) {
      if (strcmp(counter->hdr.name, buf) == 0) {
         Dmsg2(100, "Counter=%s val=%d\n", buf, counter->CurrentValue);
         bsnprintf(buf, sizeof(buf), "%d", counter->CurrentValue);
	 *val_ptr = bstrdup(buf);
	 *val_len = strlen(buf);
	 *val_size = *val_len;
	 if (var_inc && counter->Catalog) {
	    COUNTER_DBR cr;
	    JCR *jcr = (JCR *)my_ctx;
	    memset(&cr, 0, sizeof(cr));
	    bstrncpy(cr.Counter, counter->hdr.name, sizeof(cr.Counter));
	    cr.MinValue = counter->MinValue;
	    cr.MaxValue = counter->MaxValue;
	    if (counter->CurrentValue == counter->MaxValue) {
	       counter->CurrentValue = counter->MinValue;
	    } else {
	       counter->CurrentValue++;
	    }
	    cr.CurrentValue = counter->CurrentValue;
            Dmsg1(100, "New value=%d\n", cr.CurrentValue);
	    if (counter->WrapCounter) {
	       bstrncpy(cr.WrapCounter, counter->WrapCounter->hdr.name, sizeof(cr.WrapCounter));
	    } else {
	       cr.WrapCounter[0] = 0;
	    }
	    if (!db_update_counter_record(jcr, jcr->db, &cr)) {
               Jmsg(jcr, M_ERROR, 0, _("Count not update counter %s: ERR=%s\n"),
		  counter->hdr.name, db_strerror(jcr->db));
	    }
	 }  
	 stat = VAR_OK;
	 break;
      }
   }
   UnlockRes();
   return stat;
}


/*
 * Called here to look up a variable   
 */
static var_rc_t lookup_var(var_t *ctx, void *my_ctx, 
	  const char *var_ptr, int var_len, int var_inc, int var_index, 
	  const char **val_ptr, int *val_len, int *val_size)
{
   char buf[MAXSTRING], *val, *p, *v;
   var_rc_t stat;
   int count;

   if ((stat = lookup_built_in_var(ctx, my_ctx, var_ptr, var_len, var_index,
	val_ptr, val_len, val_size)) == VAR_OK) {
      return VAR_OK;
   }

   if ((stat = lookup_counter_var(ctx, my_ctx, var_ptr, var_len, var_inc, var_index,
	val_ptr, val_len, val_size)) == VAR_OK) {
      return VAR_OK;
   }

   /* Look in environment */
   if (var_len > (int)sizeof(buf) - 1) {
       return VAR_ERR_OUT_OF_MEMORY;
   }
   memcpy(buf, var_ptr, var_len + 1);
   buf[var_len] = 0;
   Dmsg1(100, "Var=%s\n", buf);

   if ((val = getenv(buf)) == NULL) {
       return VAR_ERR_UNDEFINED_VARIABLE;
   }
   if (var_index == 0) {
      *val_ptr = val;
      *val_len = strlen(val);
      *val_size = 0;
      return VAR_OK;
   }
   /* He wants to index the "array" */
   count = 0;
   /* Find the size of the "array"                           
    *	each element is separated by a |  
    */
   for (p = val; *p; p++) {
      if (*p == '|') {
	 count++;
      }
   }
   count++;
   Dmsg3(100, "For %s, reqest index=%d have=%d\n",
      buf, var_index, count);
   if (var_index < 0 || var_index > count) {
      return VAR_ERR_SUBMATCH_OUT_OF_RANGE;
   }
   /* Now find the particular item (var_index) he wants */
   count = 1;
   for (p=val; *p; ) {
      if (*p == '|') {
	 if (count < var_index) {
	    val = ++p;
	    count++;
	    continue;
	 }
	 break;
      }
      p++;
   }
   if (p-val > (int)sizeof(buf) - 1) {
       return VAR_ERR_OUT_OF_MEMORY;
   }
   Dmsg2(100, "val=%s len=%d\n", val, p-val);
   /* Make a copy of item, and pass it back */
   v = (char *)malloc(p-val+1);
   memcpy(v, val, p-val);
   v[p-val] = 0;
   *val_ptr = v;
   *val_len = p-val;
   *val_size = p-val;
   Dmsg1(100, "v=%s\n", v);
   return VAR_OK;
}

/*
 * Called here to do a special operation on a variable
 *   op_ptr  points to the special operation code (not EOS terminated)
 *   arg_ptr points to argument to special op code
 *   val_ptr points to the value string
 *   out_ptr points to string to be returned
 */
static var_rc_t operate_var(var_t *var, void *my_ctx, 
	  const char *op_ptr, int op_len, 
	  const char *arg_ptr, int arg_len,
	  const char *val_ptr, int val_len, 
	  char **out_ptr, int *out_len, int *out_size)
{
   var_rc_t stat = VAR_ERR_UNDEFINED_OPERATION;
   Dmsg0(100, "Enter operate_var\n");
   if (!val_ptr) {
      *out_size = 0;
      return stat;
   }
   if (op_len == 3 && strncmp(op_ptr, "inc", 3) == 0) {
      char buf[MAXSTRING];
      if (val_len > (int)sizeof(buf) - 1) {
	  return VAR_ERR_OUT_OF_MEMORY;
      }
      memcpy(buf, arg_ptr, arg_len);
      buf[arg_len] = 0;
      Dmsg1(100, "Arg=%s\n", buf);
      memcpy(buf, val_ptr, val_len);
      buf[val_len] = 0;   
      Dmsg1(100, "Val=%s\n", buf);
      LockRes();
      for (COUNTER *counter=NULL; (counter = (COUNTER *)GetNextRes(R_COUNTER, (RES *)counter)); ) {
	 if (strcmp(counter->hdr.name, buf) == 0) {
            Dmsg2(100, "counter=%s val=%s\n", counter->hdr.name, buf);
	    break;
	 }
      }
      UnlockRes();
      return stat;
   }
   *out_size = 0;
   return stat;
}


/* 
 * Expand an input line and return it.
 *
 *  Returns: 0 on failure
 *	     1 on success and exp has expanded input
 */
int variable_expansion(JCR *jcr, char *inp, POOLMEM **exp)
{
   var_t *var_ctx;
   var_rc_t stat;
   char *outp;
   int in_len, out_len;
   int rtn_stat = 0;

   in_len = strlen(inp);
   outp = NULL;
   out_len = 0;

   /* create context */
   if ((stat = var_create(&var_ctx)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot create var context: ERR=%s\n"), var_strerror(var_ctx, stat));
       goto bail_out;
   }
   /* define callback */
   if ((stat = var_config(var_ctx, VAR_CONFIG_CB_VALUE, lookup_var, (void *)jcr)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot set var callback: ERR=%s\n"), var_strerror(var_ctx, stat));
       goto bail_out;
   }

   /* define special operations */
   if ((stat = var_config(var_ctx, VAR_CONFIG_CB_OPERATION, operate_var, (void *)jcr)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot set var operate: ERR=%s\n"), var_strerror(var_ctx, stat));
       goto bail_out;
   }

   /* unescape in place */
   if ((stat = var_unescape(var_ctx, inp, in_len, inp, in_len+1, 0)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot unescape string: ERR=%s\n"), var_strerror(var_ctx, stat));
       goto bail_out;
   }

   in_len = strlen(inp);

   /* expand variables */
   if ((stat = var_expand(var_ctx, inp, in_len, &outp, &out_len, 1)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot expand LabelFormat \"%s\": ERR=%s\n"), 
	  inp, var_strerror(var_ctx, stat));
       goto bail_out;
   }

   /* unescape once more in place */
   if ((stat = var_unescape(var_ctx, outp, out_len, outp, out_len+1, 1)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot unescape string: ERR=%s\n"), var_strerror(var_ctx, stat));
       goto bail_out;
   }

   pm_strcpy(exp, outp);

   rtn_stat = 1;  

bail_out:
   /* destroy expansion context */
   if ((stat = var_destroy(var_ctx)) != VAR_OK) {
       Jmsg(jcr, M_ERROR, 0, _("Cannot destroy var context: ERR=%s\n"), var_strerror(var_ctx, stat));
   }
   if (outp) {
      free(outp);
   }
   return rtn_stat;
}
