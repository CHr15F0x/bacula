/*
 *
 *   Bacula Director daemon -- this is the main program
 *
 *     Kern Sibbald, March MM
 *
 *   Version $Id$
 */
/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2006 Free Software Foundation Europe e.V.

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
#include "dird.h"

/* Forward referenced subroutines */
void terminate_dird(int sig);
static int check_resources();
static void dir_sql_query(JCR *jcr, const char *cmd);
  
/* Exported subroutines */
extern "C" void reload_config(int sig);
extern void invalidate_schedules();


/* Imported subroutines */
JCR *wait_for_next_job(char *runjob);
void term_scheduler();
void term_ua_server();
void start_UA_server(dlist *addrs);
void init_job_server(int max_workers);
void term_job_server();
void store_jobtype(LEX *lc, RES_ITEM *item, int index, int pass);
void store_level(LEX *lc, RES_ITEM *item, int index, int pass);
void store_replace(LEX *lc, RES_ITEM *item, int index, int pass);
void init_device_resources();

static char *runjob = NULL;
static int background = 1;
static void init_reload(void);

/* Globals Exported */
DIRRES *director;                     /* Director resource */
int FDConnectTimeout;
int SDConnectTimeout;
char *configfile = NULL;

/* Globals Imported */
extern int r_first, r_last;           /* first and last resources */
extern RES_TABLE resources[];
extern RES **res_head;
extern RES_ITEM job_items[];

#if defined(_MSC_VER)
extern "C" { // work around visual compiler mangling variables
    extern URES res_all;
}
#else
extern URES res_all;
#endif

#define CONFIG_FILE "bacula-dir.conf" /* default configuration file */

static void usage()
{
   fprintf(stderr, _(
PROG_COPYRIGHT
"\nVersion: %s (%s)\n\n"
"Usage: dird [-f -s] [-c config_file] [-d debug_level] [config_file]\n"
"       -c <file>   set configuration file to file\n"
"       -dnn        set debug level to nn\n"
"       -f          run in foreground (for debugging)\n"
"       -g          groupid\n"
"       -r <job>    run <job> now\n"
"       -s          no signals\n"
"       -t          test - read configuration and exit\n"
"       -u          userid\n"
"       -v          verbose user messages\n"
"       -?          print this message.\n"
"\n"), BYEAR, VERSION, BDATE);

   exit(1);
}


/*********************************************************************
 *
 *         Main Bacula Server program
 *
 */
#if defined(HAVE_WIN32)
#define main BaculaMain
#endif

int main (int argc, char *argv[])
{
   int ch;
   JCR *jcr;
   int no_signals = FALSE;
   int test_config = FALSE;
   char *uid = NULL;
   char *gid = NULL;

   setlocale(LC_ALL, "");
   bindtextdomain("bacula", LOCALEDIR);
   textdomain("bacula");

   init_stack_dump();
   my_name_is(argc, argv, "bacula-dir");
   init_msg(NULL, NULL);              /* initialize message handler */
   init_reload();
   daemon_start_time = time(NULL);

   while ((ch = getopt(argc, argv, "c:d:fg:r:stu:v?")) != -1) {
      switch (ch) {
      case 'c':                    /* specify config file */
         if (configfile != NULL) {
            free(configfile);
         }
         configfile = bstrdup(optarg);
         break;

      case 'd':                    /* set debug level */
         debug_level = atoi(optarg);
         if (debug_level <= 0) {
            debug_level = 1;
         }
         Dmsg1(0, "Debug level = %d\n", debug_level);
         break;

      case 'f':                    /* run in foreground */
         background = FALSE;
         break;

      case 'g':                    /* set group id */
         gid = optarg;
         break;

      case 'r':                    /* run job */
         if (runjob != NULL) {
            free(runjob);
         }
         if (optarg) {
            runjob = bstrdup(optarg);
         }
         break;

      case 's':                    /* turn off signals */
         no_signals = TRUE;
         break;

      case 't':                    /* test config */
         test_config = TRUE;
         break;

      case 'u':                    /* set uid */
         uid = optarg;
         break;

      case 'v':                    /* verbose */
         verbose++;
         break;

      case '?':
      default:
         usage();

      }
   }
   argc -= optind;
   argv += optind;

   if (!no_signals) {
      init_signals(terminate_dird);
   }

   if (argc) {
      if (configfile != NULL) {
         free(configfile);
      }
      configfile = bstrdup(*argv);
      argc--;
      argv++;
   }
   if (argc) {
      usage();
   }

   if (configfile == NULL) {
      configfile = bstrdup(CONFIG_FILE);
   }

   parse_config(configfile);

   if (init_crypto() != 0) {
      Jmsg((JCR *)NULL, M_ERROR_TERM, 0, _("Cryptography library initialization failed.\n"));
   }

   if (!check_resources()) {
      Jmsg((JCR *)NULL, M_ERROR_TERM, 0, _("Please correct configuration file: %s\n"), configfile);
   }

   if (test_config) {
      terminate_dird(0);
   }

   my_name_is(0, NULL, director->hdr.name);    /* set user defined name */

   /* Plug database interface for library routines */
   p_sql_query = (sql_query)dir_sql_query;
   p_sql_escape = (sql_escape)db_escape_string;

   FDConnectTimeout = (int)director->FDConnectTimeout;
   SDConnectTimeout = (int)director->SDConnectTimeout;

   if (background) {
      daemon_start();
      init_stack_dump();              /* grab new pid */
   }

   /* Create pid must come after we are a daemon -- so we have our final pid */
   create_pid_file(director->pid_directory, "bacula-dir", get_first_port_host_order(director->DIRaddrs));
   read_state_file(director->working_directory, "bacula-dir", get_first_port_host_order(director->DIRaddrs));

   drop(uid, gid);                    /* reduce priveleges if requested */

#if !defined(HAVE_WIN32)
   signal(SIGHUP, reload_config);
#endif

   init_console_msg(working_directory);

   init_python_interpreter(director->hdr.name, director->scripts_directory, 
       "DirStartUp");

   set_thread_concurrency(director->MaxConcurrentJobs * 2 +
      4 /* UA */ + 4 /* sched+watchdog+jobsvr+misc */);

   Dmsg0(200, "Start UA server\n");
   start_UA_server(director->DIRaddrs);

   start_watchdog();                  /* start network watchdog thread */

   init_jcr_subsystem();              /* start JCR watchdogs etc. */

   init_job_server(director->MaxConcurrentJobs);

   Dmsg0(200, "wait for next job\n");
   /* Main loop -- call scheduler to get next job to run */
   while ( (jcr = wait_for_next_job(runjob)) ) {
      run_job(jcr);                   /* run job */
      free_jcr(jcr);                  /* release jcr */
      if (runjob) {                   /* command line, run a single job? */
         break;                       /* yes, terminate */
      }
   }

   terminate_dird(0);

   return 0;
}

static void dir_sql_query(JCR *jcr, const char *cmd)
{
   if (!jcr || !jcr->db) {
      return;
   }
   db_sql_query(jcr->db, cmd, NULL, NULL);
}

/* Cleanup and then exit */
void terminate_dird(int sig)
{
   static bool already_here = false;

   if (already_here) {                /* avoid recursive temination problems */
      exit(1);
   }
   already_here = true;
   generate_daemon_event(NULL, "Exit");
   write_state_file(director->working_directory, "bacula-dir", get_first_port_host_order(director->DIRaddrs));
   delete_pid_file(director->pid_directory, "bacula-dir", get_first_port_host_order(director->DIRaddrs));
// signal(SIGCHLD, SIG_IGN);          /* don't worry about children now */
   term_scheduler();
   term_job_server();
   if (runjob) {
      free(runjob);
   }
   if (configfile != NULL) {
      free(configfile);
   }
   if (debug_level > 5) {
      print_memory_pool_stats();
   }
   free_config_resources();
   term_ua_server();
   term_msg();                        /* terminate message handler */
   stop_watchdog();
   cleanup_crypto();
   close_memory_pool();               /* release free memory in pool */
   sm_dump(false);
   exit(sig);
}

struct RELOAD_TABLE {
   int job_count;
   RES **res_table;
};

static const int max_reloads = 32;
static RELOAD_TABLE reload_table[max_reloads];

static void init_reload(void)
{
   for (int i=0; i < max_reloads; i++) {
      reload_table[i].job_count = 0;
      reload_table[i].res_table = NULL;
   }
}

static void free_saved_resources(int table)
{
   int num = r_last - r_first + 1;
   RES **res_tab = reload_table[table].res_table;
   if (!res_tab) {
      Dmsg1(100, "res_tab for table %d already released.\n", table);
      return;
   }
   Dmsg1(100, "Freeing resources for table %d\n", table);
   for (int j=0; j<num; j++) {
      free_resource(res_tab[j], r_first + j);
   }
   free(res_tab);
   reload_table[table].job_count = 0;
   reload_table[table].res_table = NULL;
}

/*
 * Called here at the end of every job that was
 * hooked decrementing the active job_count. When
 * it goes to zero, no one is using the associated
 * resource table, so free it.
 */
static void reload_job_end_cb(JCR *jcr, void *ctx)
{
   int reload_id = (int)((long int)ctx);
   Dmsg3(100, "reload job_end JobId=%d table=%d cnt=%d\n", jcr->JobId,
      reload_id, reload_table[reload_id].job_count);
   lock_jobs();
   LockRes();
   if (--reload_table[reload_id].job_count <= 0) {
      free_saved_resources(reload_id);
   }
   UnlockRes();
   unlock_jobs();
}

static int find_free_reload_table_entry()
{
   int table = -1;
   for (int i=0; i < max_reloads; i++) {
      if (reload_table[i].res_table == NULL) {
         table = i;
         break;
      }
   }
   return table;
}

/*
 * If we get here, we have received a SIGHUP, which means to
 *    reread our configuration file.
 *
 * The algorithm used is as follows: we count how many jobs are
 *   running and mark the running jobs to make a callback on
 *   exiting. The old config is saved with the reload table
 *   id in a reload table. The new config file is read. Now, as
 *   each job exits, it calls back to the reload_job_end_cb(), which
 *   decrements the count of open jobs for the given reload table.
 *   When the count goes to zero, we release those resources.
 *   This allows us to have pointers into the resource table (from
 *   jobs), and once they exit and all the pointers are released, we
 *   release the old table. Note, if no new jobs are running since the
 *   last reload, then the old resources will be immediately release.
 *   A console is considered a job because it may have pointers to
 *   resources, but a SYSTEM job is not since it *should* not have any
 *   permanent pointers to jobs.
 */
extern "C"
void reload_config(int sig)
{
   static bool already_here = false;
#if !defined(HAVE_WIN32)
   sigset_t set;
#endif
   JCR *jcr;
   int njobs = 0;                     /* number of running jobs */
   int table, rtable;
   bool ok;       

   if (already_here) {
      abort();                        /* Oops, recursion -> die */
   }
   already_here = true;

#if !defined(HAVE_WIN32)
   sigemptyset(&set);
   sigaddset(&set, SIGHUP);
   sigprocmask(SIG_BLOCK, &set, NULL);
#endif

   lock_jobs();
   LockRes();

   table = find_free_reload_table_entry();
   if (table < 0) {
      Jmsg(NULL, M_ERROR, 0, _("Too many open reload requests. Request ignored.\n"));
      goto bail_out;
   }

   Dmsg1(100, "Reload_config njobs=%d\n", njobs);
   reload_table[table].res_table = save_config_resources();
   Dmsg1(100, "Saved old config in table %d\n", table);

   ok = parse_config(configfile, 0, M_ERROR);  /* no exit on error */

   Dmsg0(100, "Reloaded config file\n");
   if (!ok || !check_resources()) {
      rtable = find_free_reload_table_entry();    /* save new, bad table */
      if (rtable < 0) {
         Jmsg(NULL, M_ERROR, 0, _("Please correct configuration file: %s\n"), configfile);
         Jmsg(NULL, M_ERROR_TERM, 0, _("Out of reload table entries. Giving up.\n"));
      } else {
         Jmsg(NULL, M_ERROR, 0, _("Please correct configuration file: %s\n"), configfile);
         Jmsg(NULL, M_ERROR, 0, _("Resetting previous configuration.\n"));
      }
      reload_table[rtable].res_table = save_config_resources();
      /* Now restore old resoure values */
      int num = r_last - r_first + 1;
      RES **res_tab = reload_table[table].res_table;
      for (int i=0; i<num; i++) {
         res_head[i] = res_tab[i];
      }
      table = rtable;                 /* release new, bad, saved table below */
   } else {
      invalidate_schedules();
      /*
       * Hook all active jobs so that they release this table
       */
      foreach_jcr(jcr) {
         if (jcr->JobType != JT_SYSTEM) {
            reload_table[table].job_count++;
            job_end_push(jcr, reload_job_end_cb, (void *)((long int)table));
            njobs++;
         }
      }
      endeach_jcr(jcr);
   }

   /* Reset globals */
   set_working_directory(director->working_directory);
   FDConnectTimeout = director->FDConnectTimeout;
   SDConnectTimeout = director->SDConnectTimeout;
   Dmsg0(0, "Director's configuration file reread.\n");

   /* Now release saved resources, if no jobs using the resources */
   if (njobs == 0) {
      free_saved_resources(table);
   }

bail_out:
   UnlockRes();
   unlock_jobs();
#if !defined(HAVE_WIN32)
   sigprocmask(SIG_UNBLOCK, &set, NULL);
   signal(SIGHUP, reload_config);
#endif
   already_here = false;
}

/*
 * Make a quick check to see that we have all the
 * resources needed.
 *
 *  **** FIXME **** this routine could be a lot more
 *   intelligent and comprehensive.
 */
static int check_resources()
{
   bool OK = true;
   JOB *job;

   LockRes();

   job = (JOB *)GetNextRes(R_JOB, NULL);
   director = (DIRRES *)GetNextRes(R_DIRECTOR, NULL);
   if (!director) {
      Jmsg(NULL, M_FATAL, 0, _("No Director resource defined in %s\n"
"Without that I don't know who I am :-(\n"), configfile);
      OK = false;
   } else {
      set_working_directory(director->working_directory);
      if (!director->messages) {       /* If message resource not specified */
         director->messages = (MSGS *)GetNextRes(R_MSGS, NULL);
         if (!director->messages) {
            Jmsg(NULL, M_FATAL, 0, _("No Messages resource defined in %s\n"), configfile);
            OK = false;
         }
      }
      if (GetNextRes(R_DIRECTOR, (RES *)director) != NULL) {
         Jmsg(NULL, M_FATAL, 0, _("Only one Director resource permitted in %s\n"),
            configfile);
         OK = false;
      }
      /* tls_require implies tls_enable */
      if (director->tls_require) {
         if (have_tls) {
            director->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in Bacula.\n"));
            OK = false;
         }
      }

      if (!director->tls_certfile && director->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Director \"%s\" in %s.\n"),
            director->hdr.name, configfile);
         OK = false;
      }

      if (!director->tls_keyfile && director->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Key\" file not defined for Director \"%s\" in %s.\n"),
            director->hdr.name, configfile);
         OK = false;
      }

      if ((!director->tls_ca_certfile && !director->tls_ca_certdir) && director->tls_enable && director->tls_verify_peer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\" or \"TLS CA"
              " Certificate Dir\" are defined for Director \"%s\" in %s."
              " At least one CA certificate store is required"
              " when using \"TLS Verify Peer\".\n"),
              director->hdr.name, configfile);
         OK = false;
      }

      /* If everything is well, attempt to initialize our per-resource TLS context */
      if (OK && (director->tls_enable || director->tls_require)) {
         /* Initialize TLS context:
          * Args: CA certfile, CA certdir, Certfile, Keyfile,
          * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer */
         director->tls_ctx = new_tls_context(director->tls_ca_certfile,
            director->tls_ca_certdir, director->tls_certfile,
            director->tls_keyfile, NULL, NULL, director->tls_dhfile,
            director->tls_verify_peer);
         
         if (!director->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for Director \"%s\" in %s.\n"),
                 director->hdr.name, configfile);
            OK = false;
         }
      }
   }

   if (!job) {
      Jmsg(NULL, M_FATAL, 0, _("No Job records defined in %s\n"), configfile);
      OK = false;
   }
   foreach_res(job, R_JOB) {
      int i;

      if (job->jobdefs) {
         /* Handle Storage alists specifically */
         JOB *jobdefs = job->jobdefs;
         if (jobdefs->storage && !job->storage) {
            STORE *st;
            job->storage = New(alist(10, not_owned_by_alist));
            foreach_alist(st, jobdefs->storage) {
               job->storage->append(st);
            }
         }
         /* Handle RunScripts alists specifically */
         if (jobdefs->RunScripts) {
            RUNSCRIPT *rs, *elt;
            
            if (!job->RunScripts) {
               job->RunScripts = New(alist(10, not_owned_by_alist));
            }
           
            foreach_alist(rs, jobdefs->RunScripts) {
               elt = copy_runscript(rs);
               job->RunScripts->append(elt); /* we have to free it */
            }
         }

         /* Transfer default items from JobDefs Resource */
         for (i=0; job_items[i].name; i++) {
            char **def_svalue, **svalue;  /* string value */
            int *def_ivalue, *ivalue;     /* integer value */
            bool *def_bvalue, *bvalue;    /* bool value */
            int64_t *def_lvalue, *lvalue; /* 64 bit values */
            uint32_t offset;

            Dmsg4(1400, "Job \"%s\", field \"%s\" bit=%d def=%d\n",
                job->hdr.name, job_items[i].name,
                bit_is_set(i, job->hdr.item_present),
                bit_is_set(i, job->jobdefs->hdr.item_present));

            if (!bit_is_set(i, job->hdr.item_present) &&
                 bit_is_set(i, job->jobdefs->hdr.item_present)) {
               Dmsg2(400, "Job \"%s\", field \"%s\": getting default.\n",
                 job->hdr.name, job_items[i].name);
               offset = (char *)(job_items[i].value) - (char *)&res_all;
               /*
                * Handle strings and directory strings
                */
               if (job_items[i].handler == store_str ||
                   job_items[i].handler == store_dir) {
                  def_svalue = (char **)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_svalue=%s item %d offset=%u\n",
                       job->hdr.name, job_items[i].name, *def_svalue, i, offset);
                  svalue = (char **)((char *)job + offset);
                  if (*svalue) {
                     Pmsg1(000, _("Hey something is wrong. p=0x%lu\n"), *svalue);
                  }
                  *svalue = bstrdup(*def_svalue);
                  set_bit(i, job->hdr.item_present);
               /*
                * Handle resources
                */
               } else if (job_items[i].handler == store_res) {
                  def_svalue = (char **)((char *)(job->jobdefs) + offset);
                  Dmsg4(400, "Job \"%s\", field \"%s\" item %d offset=%u\n",
                       job->hdr.name, job_items[i].name, i, offset);
                  svalue = (char **)((char *)job + offset);
                  if (*svalue) {
                     Pmsg1(000, _("Hey something is wrong. p=0x%lu\n"), *svalue);
                  }
                  *svalue = *def_svalue;
                  set_bit(i, job->hdr.item_present);
               /*
                * Handle alist resources
                */
               } else if (job_items[i].handler == store_alist_res) {
                  if (bit_is_set(i, job->jobdefs->hdr.item_present)) {
                     set_bit(i, job->hdr.item_present);
                  }
               /*
                * Handle integer fields
                *    Note, our store_bit does not handle bitmaped fields
                */
               } else if (job_items[i].handler == store_bit     ||
                          job_items[i].handler == store_pint    ||
                          job_items[i].handler == store_jobtype ||
                          job_items[i].handler == store_level   ||
                          job_items[i].handler == store_pint    ||
                          job_items[i].handler == store_replace) {
                  def_ivalue = (int *)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_ivalue=%d item %d offset=%u\n",
                       job->hdr.name, job_items[i].name, *def_ivalue, i, offset);
                  ivalue = (int *)((char *)job + offset);
                  *ivalue = *def_ivalue;
                  set_bit(i, job->hdr.item_present);
               /*
                * Handle 64 bit integer fields
                */
               } else if (job_items[i].handler == store_time   ||
                          job_items[i].handler == store_size   ||
                          job_items[i].handler == store_int64) {
                  def_lvalue = (int64_t *)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_lvalue=%" lld " item %d offset=%u\n",
                       job->hdr.name, job_items[i].name, *def_lvalue, i, offset);
                  lvalue = (int64_t *)((char *)job + offset);
                  *lvalue = *def_lvalue;
                  set_bit(i, job->hdr.item_present);
               /*
                * Handle bool fields
                */
               } else if (job_items[i].handler == store_bool) {
                  def_bvalue = (bool *)((char *)(job->jobdefs) + offset);
                  Dmsg5(400, "Job \"%s\", field \"%s\" def_bvalue=%d item %d offset=%u\n",
                       job->hdr.name, job_items[i].name, *def_bvalue, i, offset);
                  bvalue = (bool *)((char *)job + offset);
                  *bvalue = *def_bvalue;
                  set_bit(i, job->hdr.item_present);
               }
            }
         }
      }
      /*
       * Ensure that all required items are present
       */
      for (i=0; job_items[i].name; i++) {
         if (job_items[i].flags & ITEM_REQUIRED) {
               if (!bit_is_set(i, job->hdr.item_present)) {
                  Jmsg(NULL, M_FATAL, 0, _("\"%s\" directive in Job \"%s\" resource is required, but not found.\n"),
                    job_items[i].name, job->hdr.name);
                  OK = false;
                }
         }
         /* If this triggers, take a look at lib/parse_conf.h */
         if (i >= MAX_RES_ITEMS) {
            Emsg0(M_ERROR_TERM, 0, _("Too many items in Job resource\n"));
         }
      }
      if (!job->storage && !job->pool->storage) {
         Jmsg(NULL, M_FATAL, 0, _("No storage specified in Job \"%s\" nor in Pool.\n"),
            job->hdr.name);
         OK = false;
      }
   } /* End loop over Job res */

   /* Loop over databases */
   CAT *catalog;
   foreach_res(catalog, R_CATALOG) {
      B_DB *db;
      /*
       * Make sure we can open catalog, otherwise print a warning
       * message because the server is probably not running.
       */
      db = db_init_database(NULL, catalog->db_name, catalog->db_user,
                         catalog->db_password, catalog->db_address,
                         catalog->db_port, catalog->db_socket,
                         catalog->mult_db_connections);
      if (!db || !db_open_database(NULL, db)) {
         Jmsg(NULL, M_FATAL, 0, _("Could not open Catalog \"%s\", database \"%s\".\n"),
              catalog->hdr.name, catalog->db_name);
         if (db) {
            Jmsg(NULL, M_FATAL, 0, _("%s"), db_strerror(db));
         }
         OK = false;
         continue;
      }

      /* Loop over all pools, defining/updating them in each database */
      POOL *pool;
      foreach_res(pool, R_POOL) {
         create_pool(NULL, db, pool, POOL_OP_UPDATE);  /* update request */
      }

      STORE *store;
      foreach_res(store, R_STORAGE) {
         STORAGE_DBR sr;
         MEDIATYPE_DBR mr;
         if (store->media_type) {
            bstrncpy(mr.MediaType, store->media_type, sizeof(mr.MediaType));
            mr.ReadOnly = 0;
            db_create_mediatype_record(NULL, db, &mr);
         } else {
            mr.MediaTypeId = 0;
         }
         bstrncpy(sr.Name, store->name(), sizeof(sr.Name));
         sr.AutoChanger = store->autochanger;
         db_create_storage_record(NULL, db, &sr);
         store->StorageId = sr.StorageId;   /* set storage Id */
         if (!sr.created) {                 /* if not created, update it */
            db_update_storage_record(NULL, db, &sr);
         }

         /* tls_require implies tls_enable */
         if (store->tls_require) {
            if (have_tls) {
               store->tls_enable = true;
            } else {
               Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in Bacula.\n"));
               OK = false;
            }
         } 

         if ((!store->tls_ca_certfile && !store->tls_ca_certdir) && store->tls_enable) {
            Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
                 " or \"TLS CA Certificate Dir\" are defined for Storage \"%s\" in %s.\n"),
                 store->hdr.name, configfile);
            OK = false;
         }

         /* If everything is well, attempt to initialize our per-resource TLS context */
         if (OK && (store->tls_enable || store->tls_require)) {
           /* Initialize TLS context:
            * Args: CA certfile, CA certdir, Certfile, Keyfile,
            * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer */
            store->tls_ctx = new_tls_context(store->tls_ca_certfile,
               store->tls_ca_certdir, store->tls_certfile,
               store->tls_keyfile, NULL, NULL, NULL, true);
         
            if (!store->tls_ctx) {
               Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for Storage \"%s\" in %s.\n"),
                    store->hdr.name, configfile);
               OK = false;
            }
         }
      }

      /* Loop over all counters, defining them in each database */
      /* Set default value in all counters */
      COUNTER *counter;
      foreach_res(counter, R_COUNTER) {
         /* Write to catalog? */
         if (!counter->created && counter->Catalog == catalog) {
            COUNTER_DBR cr;
            bstrncpy(cr.Counter, counter->hdr.name, sizeof(cr.Counter));
            cr.MinValue = counter->MinValue;
            cr.MaxValue = counter->MaxValue;
            cr.CurrentValue = counter->MinValue;
            if (counter->WrapCounter) {
               bstrncpy(cr.WrapCounter, counter->WrapCounter->hdr.name, sizeof(cr.WrapCounter));
            } else {
               cr.WrapCounter[0] = 0;  /* empty string */
            }
            if (db_create_counter_record(NULL, db, &cr)) {
               counter->CurrentValue = cr.CurrentValue;
               counter->created = true;
               Dmsg2(100, "Create counter %s val=%d\n", counter->hdr.name, counter->CurrentValue);
            }
         }
         if (!counter->created) {
            counter->CurrentValue = counter->MinValue;  /* default value */
         }
      }
      db_close_database(NULL, db);
   }

   /* Loop over Consoles */
   CONRES *cons;
   foreach_res(cons, R_CONSOLE) {
      /* tls_require implies tls_enable */
      if (cons->tls_require) {
         if (have_tls) {
            cons->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in Bacula.\n"));
            OK = false;
            continue;
         }
      }

      if (!cons->tls_certfile && cons->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Console \"%s\" in %s.\n"),
            cons->hdr.name, configfile);
         OK = false;
      }

      if (!cons->tls_keyfile && cons->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Key\" file not defined for Console \"%s\" in %s.\n"),
            cons->hdr.name, configfile);
         OK = false;
      }

      if ((!cons->tls_ca_certfile && !cons->tls_ca_certdir) && cons->tls_enable && cons->tls_verify_peer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\" or \"TLS CA"
            " Certificate Dir\" are defined for Console \"%s\" in %s."
            " At least one CA certificate store is required"
            " when using \"TLS Verify Peer\".\n"),
            cons->hdr.name, configfile);
         OK = false;
      }
      /* If everything is well, attempt to initialize our per-resource TLS context */
      if (OK && (cons->tls_enable || cons->tls_require)) {
         /* Initialize TLS context:
          * Args: CA certfile, CA certdir, Certfile, Keyfile,
          * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer */
         cons->tls_ctx = new_tls_context(cons->tls_ca_certfile,
            cons->tls_ca_certdir, cons->tls_certfile,
            cons->tls_keyfile, NULL, NULL, cons->tls_dhfile, cons->tls_verify_peer);
         
         if (!cons->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for File daemon \"%s\" in %s.\n"),
               cons->hdr.name, configfile);
            OK = false;
         }
      }

   }

   /* Loop over Clients */
   CLIENT *client;
   foreach_res(client, R_CLIENT) {
      /* tls_require implies tls_enable */
      if (client->tls_require) {
         if (have_tls) {
            client->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in Bacula.\n"));
            OK = false;
            continue;
         }
      }

      if ((!client->tls_ca_certfile && !client->tls_ca_certdir) && client->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
            " or \"TLS CA Certificate Dir\" are defined for File daemon \"%s\" in %s.\n"),
            client->hdr.name, configfile);
         OK = false;
      }

      /* If everything is well, attempt to initialize our per-resource TLS context */
      if (OK && (client->tls_enable || client->tls_require)) {
         /* Initialize TLS context:
          * Args: CA certfile, CA certdir, Certfile, Keyfile,
          * Keyfile PEM Callback, Keyfile CB Userdata, DHfile, Verify Peer */
         client->tls_ctx = new_tls_context(client->tls_ca_certfile,
            client->tls_ca_certdir, client->tls_certfile,
            client->tls_keyfile, NULL, NULL, NULL,
            true);
         
         if (!client->tls_ctx) {
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for File daemon \"%s\" in %s.\n"),
               client->hdr.name, configfile);
            OK = false;
         }
      }
   }

   UnlockRes();
   if (OK) {
      close_msg(NULL);                /* close temp message handler */
      init_msg(NULL, director->messages); /* open daemon message handler */
   }
   return OK;
}
