/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2007-2010 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation, which is 
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of Kern Sibbald.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
/*
 * Application Programming Interface (API) definition for Bacula Plugins
 *
 * Kern Sibbald, October 2007
 *
 */
 
#ifndef __FD_PLUGINS_H 
#define __FD_PLUGINS_H

#ifndef _BACULA_H
#ifdef __cplusplus
/* Workaround for SGI IRIX 6.5 */
#define _LANGUAGE_C_PLUS_PLUS 1
#endif
#define _REENTRANT    1
#define _THREAD_SAFE  1
#define _POSIX_PTHREAD_SEMANTICS 1
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE_SOURCE 1
#define _LARGE_FILES 1
#endif  /* ! _BACULA_H */

#include <sys/types.h>

#if defined(HAVE_WIN32)
#if defined(HAVE_MINGW)
#include "mingwconfig.h"
#else
#include "winconfig.h"
#endif
#else  /* !HAVE_WIN32 */
#ifndef __CONFIG_H
#include "config.h"
#define __CONFIG_H
#endif
#endif

#include "bc_types.h"
#include "lib/plugins.h"
#include <sys/stat.h>
#ifdef HAVE_WIN32
#include "../win32/filed/vss.h"
#endif

/*
 * This packet is used for the restore objects
 *  It is passed to the plugin when restoring
 *  the object.
 */
struct restore_object_pkt {
   int32_t pkt_size;                  /* size of this packet */
   char *object_name;                 /* Object name */
   char *object;                      /* restore object data to save */
   int32_t object_type;               /* FT_xx for this file */             
   int32_t object_len;                /* restore object length */
   int32_t object_index;              /* restore object index */
   int32_t stream;                    /* attribute stream id */
   uint32_t JobId;                    /* JobId object came from */
   int32_t pkt_end;                   /* end packet sentinel */
};

/*
 * This packet is used for file save info transfer.
*/
struct save_pkt {
   int32_t pkt_size;                  /* size of this packet */
   char *fname;                       /* Full path and filename */
   char *link;                        /* Link name if any */
   struct stat statp;                 /* System stat() packet for file */
   int32_t type;                      /* FT_xx for this file */             
   uint32_t flags;                    /* Bacula internal flags */
   bool portable;                     /* set if data format is portable */
   char *cmd;                         /* command */
   char *object_name;                 /* Object name to create */
   char *object;                      /* restore object data to save */
   int32_t object_len;                /* restore object length */
   int32_t index;                     /* restore object index */
   int32_t pkt_end;                   /* end packet sentinel */
};

/*
 * This packet is used for file restore info transfer.
*/
struct restore_pkt {
   int32_t pkt_size;                  /* size of this packet */
   int32_t stream;                    /* attribute stream id */
   int32_t data_stream;               /* id of data stream to follow */
   int32_t type;                      /* file type FT */
   int32_t file_index;                /* file index */
   int32_t LinkFI;                    /* file index to data if hard link */
   uid_t uid;                         /* userid */
   struct stat statp;                 /* decoded stat packet */
   const char *attrEx;                /* extended attributes if any */
   const char *ofname;                /* output filename */
   const char *olname;                /* output link name */
   const char *where;                 /* where */
   const char *RegexWhere;            /* regex where */
   int replace;                       /* replace flag */
   int create_status;                 /* status from createFile() */
   int32_t pkt_end;                   /* end packet sentinel */
};

enum {
   IO_OPEN = 1,
   IO_READ = 2,
   IO_WRITE = 3,
   IO_CLOSE = 4,
   IO_SEEK = 5
};

struct io_pkt {
   int32_t pkt_size;                  /* Size of this packet */
   int32_t func;                      /* Function code */
   int32_t count;                     /* read/write count */
   int32_t flags;                     /* Open flags */
   mode_t mode;                       /* permissions for created files */
   char *buf;                         /* read/write buffer */
   const char *fname;                 /* open filename */
   int32_t status;                    /* return status */
   int32_t io_errno;                  /* errno code */  
   int32_t lerror;                    /* Win32 error code */
   int32_t whence;                    /* lseek argument */
   boffset_t offset;                  /* lseek argument */
   bool win32;                        /* Win32 GetLastError returned */
   int32_t pkt_end;                   /* end packet sentinel */
};

/****************************************************************************
 *                                                                          *
 *                Bacula definitions                                        *
 *                                                                          *
 ****************************************************************************/

/* Bacula Variable Ids */
typedef enum {
  bVarJobId     = 1,
  bVarFDName    = 2,
  bVarLevel     = 3,
  bVarType      = 4,
  bVarClient    = 5,
  bVarJobName   = 6,
  bVarJobStatus = 7,
  bVarSinceTime = 8,
  bVarAccurate  = 9,
  bVarFileSeen  = 10,
  bVarVssObject = 11,
  bVarVssDllHandle = 12
} bVariable;

/* Events that are passed to plugin */
typedef enum {
  bEventJobStart                        = 1,
  bEventJobEnd                          = 2,
  bEventStartBackupJob                  = 3,
  bEventEndBackupJob                    = 4,
  bEventStartRestoreJob                 = 5,
  bEventEndRestoreJob                   = 6,
  bEventStartVerifyJob                  = 7,
  bEventEndVerifyJob                    = 8,
  bEventBackupCommand                   = 9,
  bEventRestoreCommand                  = 10,
  bEventLevel                           = 11,
  bEventSince                           = 12,
  bEventCancelCommand                   = 13,
  bEventVssBackupAddComponents          = 14,
  bEventVssRestoreLoadComponentMetadata = 15,
  bEventVssRestoreSetComponentsSelected = 16,
  bEventRestoreObject                   = 17,
} bEventType;

typedef struct s_bEvent {
   uint32_t eventType;
} bEvent;

typedef struct s_baculaInfo {
   uint32_t size;
   uint32_t version;
} bInfo;

/* Bacula Core Routines -- not used within a plugin */
#ifdef FILE_DAEMON
struct BFILE;                   /* forward referenced */
struct FF_PKT;
void load_fd_plugins(const char *plugin_dir);
void new_plugins(JCR *jcr);
void free_plugins(JCR *jcr);
void generate_plugin_event(JCR *jcr, bEventType event, void *value=NULL);
bool send_plugin_name(JCR *jcr, BSOCK *sd, bool start);
bool plugin_name_stream(JCR *jcr, char *name);    
int plugin_create_file(JCR *jcr, ATTR *attr, BFILE *bfd, int replace);
bool plugin_set_attributes(JCR *jcr, ATTR *attr, BFILE *ofd);
int plugin_save(JCR *jcr, FF_PKT *ff_pkt, bool top_level);
bool plugin_check_file(JCR *jcr, char *fname);
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * Bacula interface version and function pointers -- 
 *  i.e. callbacks from the plugin to Bacula
 */
typedef struct s_baculaFuncs {  
   uint32_t size;
   uint32_t version;
   bRC (*registerBaculaEvents)(bpContext *ctx, ...);
   bRC (*getBaculaValue)(bpContext *ctx, bVariable var, void *value);
   bRC (*setBaculaValue)(bpContext *ctx, bVariable var, void *value);
   bRC (*JobMessage)(bpContext *ctx, const char *file, int line, 
       int type, utime_t mtime, const char *fmt, ...);     
   bRC (*DebugMessage)(bpContext *ctx, const char *file, int line,
       int level, const char *fmt, ...);
   void *(*baculaMalloc)(bpContext *ctx, const char *file, int line, 
       size_t size);
   void (*baculaFree)(bpContext *ctx, const char *file, int line, void *mem);
   bRC (*AddExclude)(bpContext *ctx, const char *file);
} bFuncs;




/****************************************************************************
 *                                                                          *
 *                Plugin definitions                                        *
 *                                                                          *
 ****************************************************************************/

typedef enum {
  pVarName = 1,
  pVarDescription = 2
} pVariable;


#define FD_PLUGIN_MAGIC     "*FDPluginData*" 
#define FD_PLUGIN_INTERFACE_VERSION  4

typedef struct s_pluginInfo {
   uint32_t size;
   uint32_t version;
   const char *plugin_magic;
   const char *plugin_license;
   const char *plugin_author;
   const char *plugin_date;
   const char *plugin_version;
   const char *plugin_description;
} pInfo;

/*
 * This is a set of function pointers that Bacula can call
 *  within the plugin.
 */
typedef struct s_pluginFuncs {  
   uint32_t size;
   uint32_t version;
   bRC (*newPlugin)(bpContext *ctx);
   bRC (*freePlugin)(bpContext *ctx);
   bRC (*getPluginValue)(bpContext *ctx, pVariable var, void *value);
   bRC (*setPluginValue)(bpContext *ctx, pVariable var, void *value);
   bRC (*handlePluginEvent)(bpContext *ctx, bEvent *event, void *value);
   bRC (*startBackupFile)(bpContext *ctx, struct save_pkt *sp);
   bRC (*endBackupFile)(bpContext *ctx);
   bRC (*startRestoreFile)(bpContext *ctx, const char *cmd);
   bRC (*endRestoreFile)(bpContext *ctx);
   bRC (*pluginIO)(bpContext *ctx, struct io_pkt *io);
   bRC (*createFile)(bpContext *ctx, struct restore_pkt *rp);
   bRC (*setFileAttributes)(bpContext *ctx, struct restore_pkt *rp);
   bRC (*checkFile)(bpContext *ctx, char *fname);
} pFuncs;

#define plug_func(plugin) ((pFuncs *)(plugin->pfuncs))
#define plug_info(plugin) ((pInfo *)(plugin->pinfo))

#ifdef __cplusplus
}
#endif

#endif /* __FD_PLUGINS_H */
