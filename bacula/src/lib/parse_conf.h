/*
 *   Version $Id$
 */
/*
   Copyright (C) 2000, 2001, 2002 Kern Sibbald and John Walker

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

struct res_items;                   /* Declare forward referenced structure */ 
typedef void (MSG_RES_HANDLER)(LEX *lc, struct res_items *item, int index, int pass);

/* This is the structure that defines
 * the record types (items) permitted within each
 * resource. It is used to define the configuration
 * tables.
 */
struct res_items {
   char *name;                        /* Resource name i.e. Director, ... */
   MSG_RES_HANDLER *handler;          /* Routine storing the resource item */
   void **value;                      /* Where to store the item */
   int  code;                         /* item code/additional info */
   int  flags;                        /* flags: default, required, ... */
   int  default_value;                /* default value */
};

/* For storing name_addr items in res_items table */
#define ITEM(x) ((void **)&res_all.x)

#define MAX_RES_ITEMS 50              /* maximum resource items per RES */

/* This is the universal header that is
 * at the beginning of every resource
 * record.
 */
struct RES {
   RES *next;                         /* pointer to next resource of this type */
   char *name;                        /* resource name */
   char *desc;                        /* resource description */
   int   rcode;                       /* resource id or type */
   int   refcnt;                      /* reference count for releasing */
   char  item_present[MAX_RES_ITEMS]; /* set if item is present in conf file */
};


/* 
 * Master Resource configuration structure definition
 * This is the structure that defines the
 * resources that are available to this daemon.
 */
struct s_res {       
   char *name;                        /* resource name */
   struct res_items *items;           /* list of resource keywords */
   int rcode;                         /* code if needed */
   RES *res_head;                     /* where to store it */
};

/* Common Resource definitions */

#define MAX_RES_NAME_LENGTH MAX_NAME_LENGTH-1       /* maximum resource name length */

#define ITEM_REQUIRED    0x1          /* item required */
#define ITEM_DEFAULT     0x2          /* default supplied */
#define ITEM_NO_EQUALS   0x4          /* Don't scan = after name */

/* Message Resource */
struct MSGS {
   RES   hdr;
   char *mail_cmd;                    /* mail command */
   char *operator_cmd;                /* Operator command */
   DEST *dest_chain;                  /* chain of destinations */
   char send_msg[nbytes_for_bits(M_MAX+1)];  /* bit array of types */
};


/* Define the Union of all the above common
 * resource structure definitions.
 */
union CURES {
   MSGS  res_msgs;
   RES hdr;
};


/* Configuration routines */
void  parse_config(char *cf);
void  free_config_resources(void);

/* Resource routines */
RES *GetResWithName(int rcode, char *name);
RES *GetNextRes(int rcode, RES *res);
void LockRes(void);
void UnlockRes(void);
void dump_resource(int type, RES *res, void sendmsg(void *sock, char *fmt, ...), void *sock);
void free_resource(int type);
void init_resource(int type, struct res_items *item);
void save_resource(int type, struct res_items *item, int pass);
char *res_to_str(int rcode);
/* Loop through each resource of type, returning in var */
#ifdef HAVE_WIN32
#define foreach_res(var, type) \
    for(var=NULL; *((void **)&(var))=(void *)GetNextRes((type), (RES *)var);) 
#else
#define foreach_res(var, type) \
        for((var)=NULL; (((void *)(var))=GetNextRes((type), (RES *)var));) 
#endif


void store_str(LEX *lc, struct res_items *item, int index, int pass);
void store_dir(LEX *lc, struct res_items *item, int index, int pass);
void store_password(LEX *lc, struct res_items *item, int index, int pass);
void store_name(LEX *lc, struct res_items *item, int index, int pass);
void store_strname(LEX *lc, struct res_items *item, int index, int pass);
void store_res(LEX *lc, struct res_items *item, int index, int pass);
void store_int(LEX *lc, struct res_items *item, int index, int pass);
void store_pint(LEX *lc, struct res_items *item, int index, int pass);
void store_msgs(LEX *lc, struct res_items *item, int index, int pass);
void store_int64(LEX *lc, struct res_items *item, int index, int pass);
void store_yesno(LEX *lc, struct res_items *item, int index, int pass);
void store_time(LEX *lc, struct res_items *item, int index, int pass);
void store_size(LEX *lc, struct res_items *item, int index, int pass);
void store_defs(LEX *lc, struct res_items *item, int index, int pass);
