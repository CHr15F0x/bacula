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
 * Main program to test loading and running Bacula plugins.
 *   Destined to become Bacula pluginloader, ...
 *
 * Kern Sibbald, October 2007
 */
#include "bacula.h"
#include "filed.h"

const int dbglvl = 150;
#ifdef HAVE_WIN32
const char *plugin_type = "-fd.dll";
#else
const char *plugin_type = "-fd.so";
#endif

extern int save_file(JCR *jcr, FF_PKT *ff_pkt, bool top_level);

/* Function pointers to be set here */
extern DLL_IMP_EXP int     (*plugin_bopen)(BFILE *bfd, const char *fname, int flags, mode_t mode);
extern DLL_IMP_EXP int     (*plugin_bclose)(BFILE *bfd);
extern DLL_IMP_EXP ssize_t (*plugin_bread)(BFILE *bfd, void *buf, size_t count);
extern DLL_IMP_EXP ssize_t (*plugin_bwrite)(BFILE *bfd, void *buf, size_t count);
extern DLL_IMP_EXP boffset_t (*plugin_blseek)(BFILE *bfd, boffset_t offset, int whence);


/* Forward referenced functions */
static bRC baculaGetValue(bpContext *ctx, bVariable var, void *value);
static bRC baculaSetValue(bpContext *ctx, bVariable var, void *value);
static bRC baculaRegisterEvents(bpContext *ctx, ...);
static bRC baculaJobMsg(bpContext *ctx, const char *file, int line,
  int type, utime_t mtime, const char *fmt, ...);
static bRC baculaDebugMsg(bpContext *ctx, const char *file, int line,
  int level, const char *fmt, ...);
static void *baculaMalloc(bpContext *ctx, const char *file, int line,
              size_t size);
static void baculaFree(bpContext *ctx, const char *file, int line, void *mem);
static bool is_plugin_compatible(Plugin *plugin);

/*
 * These will be plugged into the global pointer structure for
 *  the findlib.
 */
static int     my_plugin_bopen(BFILE *bfd, const char *fname, int flags, mode_t mode);
static int     my_plugin_bclose(BFILE *bfd);
static ssize_t my_plugin_bread(BFILE *bfd, void *buf, size_t count);
static ssize_t my_plugin_bwrite(BFILE *bfd, void *buf, size_t count);
static boffset_t my_plugin_blseek(BFILE *bfd, boffset_t offset, int whence);


/* Bacula info */
static bInfo binfo = {
   sizeof(bInfo),
   FD_PLUGIN_INTERFACE_VERSION 
};

/* Bacula entry points */
static bFuncs bfuncs = {
   sizeof(bFuncs),
   FD_PLUGIN_INTERFACE_VERSION,
   baculaRegisterEvents,
   baculaGetValue,
   baculaSetValue,
   baculaJobMsg,
   baculaDebugMsg,
   baculaMalloc,
   baculaFree
};

/* 
 * Bacula private context
 */
struct bacula_ctx {
   JCR *jcr;                             /* jcr for plugin */
   bRC  rc;                              /* last return code */
   bool disabled;                        /* set if plugin disabled */
};

static bool is_plugin_disabled(JCR *jcr)
{
   bacula_ctx *b_ctx;
   if (!jcr->plugin_ctx) {
      return true;
   }
   b_ctx = (bacula_ctx *)jcr->plugin_ctx->bContext;
   return b_ctx->disabled;
}


/*
 * Create a plugin event 
 */
void generate_plugin_event(JCR *jcr, bEventType eventType, void *value)     
{
   bEvent event;
   Plugin *plugin;
   int i = 0;

   if (!plugin_list || !jcr || !jcr->plugin_ctx_list || job_canceled(jcr)) {
      return;                         /* Return if no plugins loaded */
   }

   bpContext *plugin_ctx_list = (bpContext *)jcr->plugin_ctx_list;
   event.eventType = eventType;

   Dmsg2(dbglvl, "plugin_ctx=%p JobId=%d\n", jcr->plugin_ctx_list, jcr->JobId);

   /* Pass event to every plugin */
   foreach_alist(plugin, plugin_list) {
      bRC rc;
      jcr->plugin_ctx = &plugin_ctx_list[i++];
      jcr->plugin = plugin;
      if (is_plugin_disabled(jcr)) {
         continue;
      }
      rc = plug_func(plugin)->handlePluginEvent(jcr->plugin_ctx, &event, value);
      if (rc != bRC_OK) {
         break;
      }
   }

   jcr->plugin = NULL;
   jcr->plugin_ctx = NULL;
   return;
}

/*
 * Check if file was seen for accurate
 */
bool plugin_check_file(JCR *jcr, char *fname)
{
   Plugin *plugin;
   int rc = bRC_OK;
   int i = 0;

   if (!plugin_list || !jcr || !jcr->plugin_ctx_list || job_canceled(jcr)) {
      return false;                      /* Return if no plugins loaded */
   }

   bpContext *plugin_ctx_list = (bpContext *)jcr->plugin_ctx_list;

   Dmsg2(dbglvl, "plugin_ctx=%p JobId=%d\n", jcr->plugin_ctx_list, jcr->JobId);

   /* Pass event to every plugin */
   foreach_alist(plugin, plugin_list) {
      jcr->plugin_ctx = &plugin_ctx_list[i++];
      jcr->plugin = plugin;
      if (is_plugin_disabled(jcr)) {
         continue;
      }
      if (plug_func(plugin)->checkFile == NULL) {
         continue;
      }
      rc = plug_func(plugin)->checkFile(jcr->plugin_ctx, fname);
      if (rc == bRC_Seen) {
         break;
      }
   }

   jcr->plugin = NULL;
   jcr->plugin_ctx = NULL;
   return rc == bRC_Seen;
}


/*   
 * Sequence of calls for a backup:
 * 1. plugin_save() here is called with ff_pkt
 * 2. we find the plugin requested on the command string
 * 3. we generate a bEventBackupCommand event to the specified plugin
 *    and pass it the command string.
 * 4. we make a startPluginBackup call to the plugin, which gives
 *    us the data we need in save_pkt
 * 5. we call Bacula's save_file() subroutine to save the specified
 *    file.  The plugin will be called at pluginIO() to supply the
 *    file data.
 *
 * Sequence of calls for restore:
 *   See subroutine plugin_name_stream() below.
 */
int plugin_save(JCR *jcr, FF_PKT *ff_pkt, bool top_level)
{
   Plugin *plugin;
   int i = 0;
   int len;
   char *p;
   char *cmd = ff_pkt->top_fname;
   struct save_pkt sp;
   bEvent event;
   POOL_MEM fname(PM_FNAME);
   POOL_MEM link(PM_FNAME);

   if (!plugin_list || !jcr->plugin_ctx_list || job_canceled(jcr)) {
      Jmsg1(jcr, M_FATAL, 0, "Command plugin \"%s\" not loaded.\n", cmd);
      return 1;                            /* Return if no plugins loaded */
   }

   jcr->cmd_plugin = true;
   bpContext *plugin_ctx_list = (bpContext *)jcr->plugin_ctx_list;
   event.eventType = bEventBackupCommand;

   /* Handle plugin command here backup */
   Dmsg1(dbglvl, "plugin cmd=%s\n", cmd);
   if (!(p = strchr(cmd, ':'))) {
      Jmsg1(jcr, M_ERROR, 0, "Malformed plugin command: %s\n", cmd);
      goto bail_out;
   }
   len = p - cmd;
   if (len <= 0) {
      goto bail_out;
   }

   /* Note, we stop the loop on the first plugin that matches the name */
   foreach_alist(plugin, plugin_list) {
      Dmsg3(dbglvl, "plugin=%s cmd=%s len=%d\n", plugin->file, cmd, len);
      if (strncmp(plugin->file, cmd, len) != 0) {
         i++;
         continue;
      }
      /* 
       * We put the current plugin pointer, and the plugin context
       *  into the jcr, because during save_file(), the plugin
       *  will be called many times and these values are needed.
       */
      jcr->plugin_ctx = &plugin_ctx_list[i];
      jcr->plugin = plugin;
      if (is_plugin_disabled(jcr)) {
         goto bail_out;
      }

      Dmsg1(dbglvl, "Command plugin = %s\n", cmd);
      /* Send the backup command to the right plugin*/
      if (plug_func(plugin)->handlePluginEvent(jcr->plugin_ctx, &event, cmd) != bRC_OK) {
         goto bail_out;
      }
      /* Loop getting filenames to backup then saving them */
      while (!job_canceled(jcr)) { 
         memset(&sp, 0, sizeof(sp));
         sp.pkt_size = sizeof(sp);
         sp.pkt_end = sizeof(sp);
         sp.portable = true;
         sp.cmd = cmd;
         Dmsg3(dbglvl, "startBackup st_size=%p st_blocks=%p sp=%p\n", &sp.statp.st_size, &sp.statp.st_blocks,
                &sp);
         /* Get the file save parameters */
         if (plug_func(plugin)->startBackupFile(jcr->plugin_ctx, &sp) != bRC_OK) {
            goto bail_out;
         }
         if (sp.type == 0 || sp.fname == NULL) {
            Jmsg1(jcr, M_FATAL, 0, _("Command plugin \"%s\" returned bad startBackupFile packet.\n"),
               cmd);
            goto bail_out;
         }
         jcr->plugin_sp = &sp;
         ff_pkt = jcr->ff;
         /*
          * Copy fname and link because save_file() zaps them.  This 
          *  avoids zaping the plugin's strings.
          */
         pm_strcpy(fname, sp.fname);
         pm_strcpy(link, sp.link);
         ff_pkt->fname = fname.c_str();
         ff_pkt->link = link.c_str();
         ff_pkt->type = sp.type;
         memcpy(&ff_pkt->statp, &sp.statp, sizeof(ff_pkt->statp));
         Dmsg1(dbglvl, "Save_file: file=%s\n", fname.c_str());
         save_file(jcr, ff_pkt, true);
         bRC rc = plug_func(plugin)->endBackupFile(jcr->plugin_ctx);
         if (rc == bRC_More || rc == bRC_OK) {
            accurate_mark_file_as_seen(jcr, fname.c_str());
         }
         if (rc == bRC_More) {
            continue;
         }
         goto bail_out;
      }
      goto bail_out;
   }
   Jmsg1(jcr, M_FATAL, 0, "Command plugin \"%s\" not found.\n", cmd);

bail_out:
   jcr->cmd_plugin = false;
   jcr->plugin = NULL;
   jcr->plugin_ctx = NULL;
   return 1;
}

/* 
 * Send plugin name start/end record to SD
 */
bool send_plugin_name(JCR *jcr, BSOCK *sd, bool start)
{
   int stat;
   int index = jcr->JobFiles;
   struct save_pkt *sp = (struct save_pkt *)jcr->plugin_sp;

   if (!sp) {
      Jmsg0(jcr, M_FATAL, 0, _("Plugin save packet not found.\n"));
      return false;
   }
   if (job_canceled(jcr)) {
      return false;
   }
  
   if (start) {
      index++;                  /* JobFiles not incremented yet */
   }
   Dmsg1(dbglvl, "send_plugin_name=%s\n", sp->cmd);
   /* Send stream header */
   if (!sd->fsend("%ld %d 0", index, STREAM_PLUGIN_NAME)) {
     Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
           sd->bstrerror());
     return false;
   }
   Dmsg1(50, "send: %s\n", sd->msg);

   if (start) {
      /* Send data -- not much */
      stat = sd->fsend("%ld 1 %d %s%c", index, sp->portable, sp->cmd, 0);
   } else {
      /* Send end of data */
      stat = sd->fsend("0 0");
   }
   if (!stat) {
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to SD. ERR=%s\n"),
            sd->bstrerror());
         return false;
   }
   Dmsg1(dbglvl, "send: %s\n", sd->msg);
   sd->signal(BNET_EOD);            /* indicate end of plugin name data */
   return true;
}

/*
 * Plugin name stream found during restore.  The record passed in
 *  argument name was generated in send_plugin_name() above.
 *
 * Returns: true  if start of stream
 *          false if end of steam
 */
bool plugin_name_stream(JCR *jcr, char *name)    
{
   char *p = name;
   char *cmd;
   bool start, portable;
   Plugin *plugin;
   int len;
   int i = 0;
   bpContext *plugin_ctx_list = jcr->plugin_ctx_list;

   Dmsg1(dbglvl, "Read plugin stream string=%s\n", name);
   skip_nonspaces(&p);             /* skip over jcr->JobFiles */
   skip_spaces(&p);
   start = *p == '1';
   if (start) {
      /* Start of plugin data */
      skip_nonspaces(&p);          /* skip start/end flag */
      skip_spaces(&p);
      portable = *p == '1';
      skip_nonspaces(&p);          /* skip portable flag */
      skip_spaces(&p);
      cmd = p;
   } else {
      /*
       * End of plugin data, notify plugin, then clear flags   
       */
      Dmsg2(dbglvl, "End plugin data plugin=%p ctx=%p\n", jcr->plugin, jcr->plugin_ctx);
      if (jcr->plugin) {
         plug_func(jcr->plugin)->endRestoreFile(jcr->plugin_ctx);
      }
      jcr->plugin_ctx = NULL;
      jcr->plugin = NULL;
      goto bail_out;
   }
   if (!plugin_ctx_list) {
      goto bail_out;
   }
      
   /*
    * After this point, we are dealing with a restore start
    */

// Dmsg1(dbglvl, "plugin restore cmd=%s\n", cmd);
   if (!(p = strchr(cmd, ':'))) {
      Jmsg1(jcr, M_ERROR, 0,
           _("Malformed plugin command. Name not terminated by colon: %s\n"), cmd);
      goto bail_out;
   }
   len = p - cmd;
   if (len <= 0) {
      goto bail_out;
   }

   /*
    * Search for correct plugin as specified on the command 
    */
   foreach_alist(plugin, plugin_list) {
      bEvent event;
      Dmsg3(dbglvl, "plugin=%s cmd=%s len=%d\n", plugin->file, cmd, len);
      if (strncmp(plugin->file, cmd, len) != 0) {
         i++;
         continue;
      }
      jcr->plugin_ctx = &plugin_ctx_list[i];
      jcr->plugin = plugin;
      if (is_plugin_disabled(jcr)) {
         goto bail_out;
      }
      Dmsg1(dbglvl, "Restore Command plugin = %s\n", cmd);
      event.eventType = bEventRestoreCommand;     
      if (plug_func(plugin)->handlePluginEvent(jcr->plugin_ctx, 
            &event, cmd) != bRC_OK) {
         goto bail_out;
      }
      /* ***FIXME**** check error code */
      plug_func(plugin)->startRestoreFile((bpContext *)jcr->plugin_ctx, cmd);
      goto bail_out;
   }
   Jmsg1(jcr, M_WARNING, 0, _("Plugin=%s not found.\n"), cmd);

bail_out:
   return start;
}

/*
 * Tell the plugin to create the file.  Return values are
 *   This is called only during Restore
 *
 *  CF_ERROR    -- error
 *  CF_SKIP     -- skip processing this file
 *  CF_EXTRACT  -- extract the file (i.e.call i/o routines)
 *  CF_CREATED  -- created, but no content to extract (typically directories)
 *
 */
int plugin_create_file(JCR *jcr, ATTR *attr, BFILE *bfd, int replace)
{
   bpContext *plugin_ctx = jcr->plugin_ctx;
   Plugin *plugin = jcr->plugin;
   struct restore_pkt rp;
   int flags;
   int rc;

   if (!plugin || !plugin_ctx || !set_cmd_plugin(bfd, jcr) || job_canceled(jcr)) {
      return CF_ERROR;
   }
   rp.pkt_size = sizeof(rp);
   rp.pkt_end = sizeof(rp);
   rp.stream = attr->stream;
   rp.data_stream = attr->data_stream;
   rp.type = attr->type;
   rp.file_index = attr->file_index;
   rp.LinkFI = attr->LinkFI;
   rp.uid = attr->uid;
   rp.statp = attr->statp;                /* structure assignment */
   rp.attrEx = attr->attrEx;
   rp.ofname = attr->ofname;
   rp.olname = attr->olname;
   rp.where = jcr->where;
   rp.RegexWhere = jcr->RegexWhere;
   rp.replace = jcr->replace;
   rp.create_status = CF_ERROR;
   Dmsg1(dbglvl, "call plugin createFile=%s\n", rp.ofname);
   rc = plug_func(plugin)->createFile(plugin_ctx, &rp);
   if (rc != bRC_OK) {
      Qmsg2(jcr, M_ERROR, 0, _("Plugin createFile call failed. Stat=%d file=%s\n"),
            rc, attr->ofname);
      return CF_ERROR;
   }
   if (rp.create_status == CF_ERROR) {
      Qmsg1(jcr, M_ERROR, 0, _("Plugin createFile call failed. Returned CF_ERROR file=%s\n"),
            attr->ofname);
      return CF_ERROR;
   }
   /* Created link or directory? */
   if (rp.create_status == CF_CREATED) {
      return rp.create_status;        /* yes, no need to bopen */
   }

   flags =  O_WRONLY | O_CREAT | O_TRUNC | O_BINARY;
   Dmsg0(dbglvl, "call bopen\n");
   int stat = bopen(bfd, attr->ofname, flags, S_IRUSR | S_IWUSR);
   Dmsg1(50, "bopen status=%d\n", stat);
   if (stat < 0) {
      berrno be;
      be.set_errno(bfd->berrno);
      Qmsg2(jcr, M_ERROR, 0, _("Could not create %s: ERR=%s\n"),
            attr->ofname, be.bstrerror());
      Dmsg2(dbglvl,"Could not bopen file %s: ERR=%s\n", attr->ofname, be.bstrerror());
      return CF_ERROR;
   }

   if (!is_bopen(bfd)) {
      Dmsg0(000, "===== BFD is not open!!!!\n");
   }
   return CF_EXTRACT;
}

/*
 * Reset the file attributes after all file I/O is done -- this allows
 *  the previous access time/dates to be set properly, and it also allows
 *  us to properly set directory permissions.
 *  Not currently Implemented.
 */
bool plugin_set_attributes(JCR *jcr, ATTR *attr, BFILE *ofd)
{
   Dmsg0(dbglvl, "plugin_set_attributes\n");
   if (is_bopen(ofd)) {
      bclose(ofd);
   }
   pm_strcpy(attr->ofname, "*none*");
   return true;
}

/*
 * Print to file the plugin info.
 */
void dump_fd_plugin(Plugin *plugin, FILE *fp)
{
   if (!plugin) {
      return ;
   }
   pInfo *info = (pInfo *)plugin->pinfo;
   fprintf(fp, "\tversion=%d\n", info->version);
   fprintf(fp, "\tdate=%s\n", NPRTB(info->plugin_date));
   fprintf(fp, "\tmagic=%s\n", NPRTB(info->plugin_magic));
   fprintf(fp, "\tauthor=%s\n", NPRTB(info->plugin_author));
   fprintf(fp, "\tlicence=%s\n", NPRTB(info->plugin_license));
   fprintf(fp, "\tversion=%s\n", NPRTB(info->plugin_version));
   fprintf(fp, "\tdescription=%s\n", NPRTB(info->plugin_description));
}

/*
 * This entry point is called internally by Bacula to ensure
 *  that the plugin IO calls come into this code.
 */
void load_fd_plugins(const char *plugin_dir)
{
   Plugin *plugin;

   if (!plugin_dir) {
      Dmsg0(dbglvl, "plugin dir is NULL\n");
      return;
   }

   plugin_list = New(alist(10, not_owned_by_alist));
   if (!load_plugins((void *)&binfo, (void *)&bfuncs, plugin_dir, plugin_type,
                     is_plugin_compatible)) {
      /* Either none found, or some error */
      if (plugin_list->size() == 0) {
         delete plugin_list;
         plugin_list = NULL;
         Dmsg0(dbglvl, "No plugins loaded\n");
         return;
      }
   }

   /* Plug entry points called from findlib */
   plugin_bopen  = my_plugin_bopen;
   plugin_bclose = my_plugin_bclose;
   plugin_bread  = my_plugin_bread;
   plugin_bwrite = my_plugin_bwrite;
   plugin_blseek = my_plugin_blseek;

   /* 
    * Verify that the plugin is acceptable, and print information
    *  about it.
    */
   foreach_alist(plugin, plugin_list) {
      Jmsg(NULL, M_INFO, 0, _("Loaded plugin: %s\n"), plugin->file);
      Dmsg1(dbglvl, "Loaded plugin: %s\n", plugin->file);
   }

   dbg_plugin_add_hook(dump_fd_plugin);
}

/*
 * Check if a plugin is compatible.  Called by the load_plugin function
 *  to allow us to verify the plugin.
 */
static bool is_plugin_compatible(Plugin *plugin)
{
   pInfo *info = (pInfo *)plugin->pinfo;
   Dmsg0(50, "is_plugin_compatible called\n");
   if (debug_level >= 50) {
      dump_fd_plugin(plugin, stdin);
   }
   if (strcmp(info->plugin_magic, FD_PLUGIN_MAGIC) != 0) {
      Jmsg(NULL, M_ERROR, 0, _("Plugin magic wrong. Plugin=%s wanted=%s got=%s\n"),
           plugin->file, FD_PLUGIN_MAGIC, info->plugin_magic);
      Dmsg3(50, "Plugin magic wrong. Plugin=%s wanted=%s got=%s\n",
           plugin->file, FD_PLUGIN_MAGIC, info->plugin_magic);

      return false;
   }
   if (info->version != FD_PLUGIN_INTERFACE_VERSION) {
      Jmsg(NULL, M_ERROR, 0, _("Plugin version incorrect. Plugin=%s wanted=%d got=%d\n"),
           plugin->file, FD_PLUGIN_INTERFACE_VERSION, info->version);
      Dmsg3(50, "Plugin version incorrect. Plugin=%s wanted=%d got=%d\n",
           plugin->file, FD_PLUGIN_INTERFACE_VERSION, info->version);
      return false;
   }
   if (strcmp(info->plugin_license, "Bacula GPLv2") != 0 &&
       strcmp(info->plugin_license, "GPLv2") != 0) {
      Jmsg(NULL, M_ERROR, 0, _("Plugin license incompatible. Plugin=%s license=%s\n"),
           plugin->file, info->plugin_license);
      Dmsg2(50, "Plugin license incompatible. Plugin=%s license=%s\n",
           plugin->file, info->plugin_license);
      return false;
   }
      
   return true;
}


/*
 * Create a new instance of each plugin for this Job
 *   Note, plugin_list can exist but jcr->plugin_ctx_list can
 *   be NULL if no plugins were loaded.
 */
void new_plugins(JCR *jcr)
{
   Plugin *plugin;
   int i = 0;

   if (!plugin_list) {
      Dmsg0(dbglvl, "plugin list is NULL\n");
      return;
   }
   if (job_canceled(jcr)) {
      return;
   }

   int num = plugin_list->size();

   if (num == 0) {
      Dmsg0(dbglvl, "No plugins loaded\n");
      return;
   }

   jcr->plugin_ctx_list = (bpContext *)malloc(sizeof(bpContext) * num);

   bpContext *plugin_ctx_list = (bpContext *)jcr->plugin_ctx_list;
   Dmsg2(dbglvl, "Instantiate plugin_ctx=%p JobId=%d\n", plugin_ctx_list, jcr->JobId);
   foreach_alist(plugin, plugin_list) {
      /* Start a new instance of each plugin */
      bacula_ctx *b_ctx = (bacula_ctx *)malloc(sizeof(bacula_ctx));
      memset(b_ctx, 0, sizeof(bacula_ctx));
      b_ctx->jcr = jcr;
      plugin_ctx_list[i].bContext = (void *)b_ctx;   /* Bacula private context */
      plugin_ctx_list[i].pContext = NULL;
      if (plug_func(plugin)->newPlugin(&plugin_ctx_list[i++]) != bRC_OK) {
         b_ctx->disabled = true;
      }
   }
}

/*
 * Free the plugin instances for this Job
 */
void free_plugins(JCR *jcr)
{
   Plugin *plugin;
   int i = 0;

   if (!plugin_list || !jcr->plugin_ctx_list) {
      return;                         /* no plugins, nothing to do */
   }

   bpContext *plugin_ctx_list = (bpContext *)jcr->plugin_ctx_list;
   Dmsg2(dbglvl, "Free instance plugin_ctx=%p JobId=%d\n", plugin_ctx_list, jcr->JobId);
   foreach_alist(plugin, plugin_list) {   
      /* Free the plugin instance */
      plug_func(plugin)->freePlugin(&plugin_ctx_list[i]);
      free(plugin_ctx_list[i++].bContext);     /* free Bacula private context */
   }
   free(plugin_ctx_list);
   jcr->plugin_ctx_list = NULL;
}

static int my_plugin_bopen(BFILE *bfd, const char *fname, int flags, mode_t mode)
{
   JCR *jcr = bfd->jcr;
   Plugin *plugin = (Plugin *)jcr->plugin;
   struct io_pkt io;

   Dmsg1(dbglvl, "plugin_bopen flags=%x\n", flags);
   if (!plugin || !jcr->plugin_ctx) {
      return 0;
   }
   io.pkt_size = sizeof(io);
   io.pkt_end = sizeof(io);
   io.func = IO_OPEN;
   io.count = 0;
   io.buf = NULL;
   io.fname = fname;
   io.flags = flags;
   io.mode = mode;
   io.win32 = false;
   io.lerror = 0;
   plug_func(plugin)->pluginIO(jcr->plugin_ctx, &io);
   bfd->berrno = io.io_errno;
   if (io.win32) {
      errno = b_errno_win32;
   } else {
      errno = io.io_errno;
      bfd->lerror = io.lerror;
   }
   Dmsg1(50, "Return from plugin open status=%d\n", io.status);
   return io.status;
}

static int my_plugin_bclose(BFILE *bfd)
{
   JCR *jcr = bfd->jcr;
   Plugin *plugin = (Plugin *)jcr->plugin;
   struct io_pkt io;

   Dmsg0(dbglvl, "===== plugin_bclose\n");
   if (!plugin || !jcr->plugin_ctx) {
      return 0;
   }
   io.pkt_size = sizeof(io);
   io.pkt_end = sizeof(io);
   io.func = IO_CLOSE;
   io.count = 0;
   io.buf = NULL;
   io.win32 = false;
   io.lerror = 0;
   plug_func(plugin)->pluginIO(jcr->plugin_ctx, &io);
   bfd->berrno = io.io_errno;
   if (io.win32) {
      errno = b_errno_win32;
   } else {
      errno = io.io_errno;
      bfd->lerror = io.lerror;
   }
   Dmsg1(dbglvl, "plugin_bclose stat=%d\n", io.status);
   return io.status;
}

static ssize_t my_plugin_bread(BFILE *bfd, void *buf, size_t count)
{
   JCR *jcr = bfd->jcr;
   Plugin *plugin = (Plugin *)jcr->plugin;
   struct io_pkt io;

   Dmsg0(dbglvl, "plugin_bread\n");
   if (!plugin || !jcr->plugin_ctx) {
      return 0;
   }
   io.pkt_size = sizeof(io);
   io.pkt_end = sizeof(io);
   io.func = IO_READ;
   io.count = count;
   io.buf = (char *)buf;
   io.win32 = false;
   io.lerror = 0;
   plug_func(plugin)->pluginIO(jcr->plugin_ctx, &io);
   bfd->berrno = io.io_errno;
   if (io.win32) {
      errno = b_errno_win32;
   } else {
      errno = io.io_errno;
      bfd->lerror = io.lerror;
   }
   return (ssize_t)io.status;
}

static ssize_t my_plugin_bwrite(BFILE *bfd, void *buf, size_t count)
{
   JCR *jcr = bfd->jcr;
   Plugin *plugin = (Plugin *)jcr->plugin;
   struct io_pkt io;

   Dmsg0(dbglvl, "plugin_bwrite\n");
   if (!plugin || !jcr->plugin_ctx) {
      return 0;
   }
   io.pkt_size = sizeof(io);
   io.pkt_end = sizeof(io);
   io.func = IO_WRITE;
   io.count = count;
   io.buf = (char *)buf;
   io.win32 = false;
   io.lerror = 0;
   plug_func(plugin)->pluginIO(jcr->plugin_ctx, &io);
   bfd->berrno = io.io_errno;
   if (io.win32) {
      errno = b_errno_win32;
   } else {
      errno = io.io_errno;
      bfd->lerror = io.lerror;
   }
   return (ssize_t)io.status;
}

static boffset_t my_plugin_blseek(BFILE *bfd, boffset_t offset, int whence)
{
   JCR *jcr = bfd->jcr;
   Plugin *plugin = (Plugin *)jcr->plugin;
   struct io_pkt io;

   Dmsg0(dbglvl, "plugin_bseek\n");
   if (!plugin || !jcr->plugin_ctx) {
      return 0;
   }
   io.pkt_size = sizeof(io);
   io.pkt_end = sizeof(io);
   io.func = IO_SEEK;
   io.offset = offset;
   io.whence = whence;
   io.win32 = false;
   io.lerror = 0;
   plug_func(plugin)->pluginIO(jcr->plugin_ctx, &io);
   bfd->berrno = io.io_errno;
   if (io.win32) {
      errno = b_errno_win32;
   } else {
      errno = io.io_errno;
      bfd->lerror = io.lerror;
   }
   return (boffset_t)io.offset;
}

/* ==============================================================
 *
 * Callbacks from the plugin
 *
 * ==============================================================
 */
static bRC baculaGetValue(bpContext *ctx, bVariable var, void *value)
{
   JCR *jcr;
   if (!value || !ctx) {
      return bRC_Error;
   }
   jcr = ((bacula_ctx *)ctx->bContext)->jcr;
// Dmsg1(dbglvl, "bacula: baculaGetValue var=%d\n", var);
   jcr = ((bacula_ctx *)ctx->bContext)->jcr;
   if (!jcr) {
      return bRC_Error;
   }
// Dmsg1(dbglvl, "Bacula: jcr=%p\n", jcr); 
   switch (var) {
   case bVarJobId:
      *((int *)value) = jcr->JobId;
      Dmsg1(dbglvl, "Bacula: return bVarJobId=%d\n", jcr->JobId);
      break;
   case bVarFDName:
      *((char **)value) = my_name;
      Dmsg1(dbglvl, "Bacula: return my_name=%s\n", my_name);
      break;
   case bVarLevel:
      *((int *)value) = jcr->getJobLevel();
      Dmsg1(dbglvl, "Bacula: return bVarJobLevel=%d\n", jcr->getJobLevel());
      break;
   case bVarType:
      *((int *)value) = jcr->getJobType();
      Dmsg1(dbglvl, "Bacula: return bVarJobType=%d\n", jcr->getJobType());
      break;
   case bVarClient:
      *((char **)value) = jcr->client_name;
      Dmsg1(dbglvl, "Bacula: return Client_name=%s\n", jcr->client_name);
      break;
   case bVarJobName:
      *((char **)value) = jcr->Job;
      Dmsg1(dbglvl, "Bacula: return Job name=%s\n", jcr->Job);
      break;
   case bVarJobStatus:
      *((int *)value) = jcr->JobStatus;
      Dmsg1(dbglvl, "Bacula: return bVarJobStatus=%d\n", jcr->JobStatus);
      break;
   case bVarSinceTime:
      *((int *)value) = (int)jcr->mtime;
      Dmsg1(dbglvl, "Bacula: return since=%d\n", (int)jcr->mtime);
      break;
   case bVarAccurate:
      *((int *)value) = (int)jcr->accurate;
      Dmsg1(dbglvl, "Bacula: return accurate=%d\n", (int)jcr->accurate);
      break;
   case bVarFileSeen:
      break;                 /* a write only variable, ignore read request */
   }
   return bRC_OK;
}

static bRC baculaSetValue(bpContext *ctx, bVariable var, void *value)
{
   JCR *jcr;
   if (!value || !ctx) {
      return bRC_Error;
   }
   jcr = ((bacula_ctx *)ctx->bContext)->jcr;
// Dmsg1(dbglvl, "bacula: baculaGetValue var=%d\n", var);
   jcr = ((bacula_ctx *)ctx->bContext)->jcr;
   if (!jcr) {
      return bRC_Error;
   }
// Dmsg1(dbglvl, "Bacula: jcr=%p\n", jcr); 
   switch (var) {
   case bVarFileSeen:
      if (!accurate_mark_file_as_seen(jcr, (char *)value)) {
         return bRC_Error;
      } 
      break;
   default:
      break;
   }
   return bRC_OK;
}

static bRC baculaRegisterEvents(bpContext *ctx, ...)
{
   va_list args;
   uint32_t event;

   if (!ctx) {
      return bRC_Error;
   }

   va_start(args, ctx);
   while ((event = va_arg(args, uint32_t))) {
      Dmsg1(dbglvl, "Plugin wants event=%u\n", event);
   }
   va_end(args);
   return bRC_OK;
}

static bRC baculaJobMsg(bpContext *ctx, const char *file, int line,
  int type, utime_t mtime, const char *fmt, ...)
{
   va_list arg_ptr;
   char buf[2000];
   JCR *jcr;

   if (ctx) {
      jcr = ((bacula_ctx *)ctx->bContext)->jcr;
   } else {
      jcr = NULL;
   }

   va_start(arg_ptr, fmt);
   bvsnprintf(buf, sizeof(buf), fmt, arg_ptr);
   va_end(arg_ptr);
   Jmsg(jcr, type, mtime, "%s", buf);
   return bRC_OK;
}

static bRC baculaDebugMsg(bpContext *ctx, const char *file, int line,
  int level, const char *fmt, ...)
{
   va_list arg_ptr;
   char buf[2000];

   va_start(arg_ptr, fmt);
   bvsnprintf(buf, sizeof(buf), fmt, arg_ptr);
   va_end(arg_ptr);
   d_msg(file, line, level, "%s", buf);
   return bRC_OK;
}

static void *baculaMalloc(bpContext *ctx, const char *file, int line,
              size_t size)
{
#ifdef SMARTALLOC
   return sm_malloc(file, line, size);
#else
   return malloc(size);
#endif
}

static void baculaFree(bpContext *ctx, const char *file, int line, void *mem)
{
#ifdef SMARTALLOC
   sm_free(file, line, mem);
#else
   free(mem);
#endif
}



#ifdef TEST_PROGRAM

int     (*plugin_bopen)(JCR *jcr, const char *fname, int flags, mode_t mode) = NULL;
int     (*plugin_bclose)(JCR *jcr) = NULL;
ssize_t (*plugin_bread)(JCR *jcr, void *buf, size_t count) = NULL;
ssize_t (*plugin_bwrite)(JCR *jcr, void *buf, size_t count) = NULL;
boffset_t (*plugin_blseek)(JCR *jcr, boffset_t offset, int whence) = NULL;

int save_file(JCR *jcr, FF_PKT *ff_pkt, bool top_level)
{
   return 0;
}

bool set_cmd_plugin(BFILE *bfd, JCR *jcr)
{
   return true;
}

int main(int argc, char *argv[])
{
   char plugin_dir[1000];
   JCR mjcr1, mjcr2;
   JCR *jcr1 = &mjcr1;
   JCR *jcr2 = &mjcr2;

   strcpy(my_name, "test-fd");
    
   getcwd(plugin_dir, sizeof(plugin_dir)-1);
   load_fd_plugins(plugin_dir);

   jcr1->JobId = 111;
   new_plugins(jcr1);

   jcr2->JobId = 222;
   new_plugins(jcr2);

   generate_plugin_event(jcr1, bEventJobStart, (void *)"Start Job 1");
   generate_plugin_event(jcr1, bEventJobEnd);
   generate_plugin_event(jcr2, bEventJobStart, (void *)"Start Job 2");
   free_plugins(jcr1);
   generate_plugin_event(jcr2, bEventJobEnd);
   free_plugins(jcr2);

   unload_plugins();

   Dmsg0(dbglvl, "bacula: OK ...\n");
   close_memory_pool();
   sm_dump(false);
   return 0;
}

#endif /* TEST_PROGRAM */
