/*
 * Includes specific to the Director User Agent Server
 *
 *     Kern Sibbald, August MMI
 *
 *     Version $Id$
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

#ifndef __UA_H_
#define __UA_H_ 1

struct UAContext {
   BSOCK *UA_sock;
   BSOCK *sd;
   JCR *jcr;
   B_DB *db;
   CAT *catalog;
   CONRES *cons;                      /* console resource */
   POOLMEM *cmd;                      /* return command/name buffer */
   POOLMEM *args;                     /* command line arguments */
   char *argk[MAX_CMD_ARGS];          /* argument keywords */
   char *argv[MAX_CMD_ARGS];          /* argument values */
   int argc;                          /* number of arguments */
   char **prompt;                     /* list of prompts */
   int max_prompts;                   /* max size of list */
   int num_prompts;                   /* current number in list */
   bool auto_display_messages;        /* if set, display messages */
   bool user_notified_msg_pending;    /* set when user notified */
   bool automount;                    /* if set, mount after label */
   bool quit;                         /* if set, quit */
   bool verbose;                      /* set for normal UA verbosity */
   bool batch;                        /* set for non-interactive mode */
   bool gui;                          /* set if talking to GUI program */
   uint32_t pint32_val;               /* positive integer */
   int32_t  int32_val;                /* positive/negative */
   int64_t  int64_val;                /* big int */
};

/* Context for insert_tree_handler() */
struct TREE_CTX {
   TREE_ROOT *root;                   /* root */
   TREE_NODE *node;                   /* current node */
   TREE_NODE *avail_node;             /* unused node last insert */
   int cnt;                           /* count for user feedback */
   bool all;                          /* if set mark all as default */
   UAContext *ua;
   uint32_t FileEstimate;             /* estimate of number of files */
   uint32_t FileCount;                /* current count of files */
   uint32_t LastCount;                /* last count of files */
   uint32_t DeltaCount;               /* trigger for printing */
};

struct NAME_LIST {
   char **name;                       /* list of names */
   int num_ids;                       /* ids stored */
   int max_ids;                       /* size of array */
   int num_del;                       /* number deleted */
   int tot_ids;                       /* total to process */
};


/* Main structure for obtaining JobIds or Files to be restored */
struct RESTORE_CTX {
   utime_t JobTDate;
   uint32_t TotalFiles;
   JobId_t JobId;
   char ClientName[MAX_NAME_LENGTH];
   char last_jobid[20];
   POOLMEM *JobIds;                   /* User entered string of JobIds */
   STORE  *store;
   JOB *restore_job;
   POOL *pool;
   int restore_jobs;
   uint32_t selected_files;
   char *where;
   RBSR *bsr;
   POOLMEM *fname;                    /* filename only */
   POOLMEM *path;                     /* path only */
   POOLMEM *query;
   int fnl;                           /* filename length */
   int pnl;                           /* path length */
   bool found;
   bool all;                          /* mark all as default */
   NAME_LIST name_list;
};

#define MAX_ID_LIST_LEN 2000000


#endif
