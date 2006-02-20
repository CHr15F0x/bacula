/*
 * Second generation Storage daemon.
 *
 * It accepts a number of simple commands from the File daemon
 * and acts on them. When a request to append data is made,
 * it opens a data channel and accepts data from the
 * File daemon.
 *
 *   Version $Id$
 *
 */
/*
   Copyright (C) 2000-2006 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as amended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */

#include "bacula.h"
#include "stored.h"

/* Imported functions */


/* Forward referenced functions */
void terminate_stored(int sig);
static int check_resources();
static void cleanup_old_files();

extern "C" void *device_initialization(void *arg);

#define CONFIG_FILE "bacula-sd.conf"  /* Default config file */

/* Global variables exported */
char OK_msg[]   = "3000 OK\n";
char TERM_msg[] = "3999 Terminate\n";
STORES *me = NULL;                    /* our Global resource */
bool forge_on = false;                /* proceed inspite of I/O errors */
pthread_mutex_t device_release_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wait_device_release = PTHREAD_COND_INITIALIZER;


static uint32_t VolSessionId = 0;
uint32_t VolSessionTime;
char *configfile = NULL;
bool init_done = false;

/* Global static variables */
static int foreground = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static workq_t dird_workq;            /* queue for processing connections */


static void usage()
{
   fprintf(stderr, _(
"Copyright (C) 2000-2005 Kern Sibbald.\n"
"\nVersion: %s (%s)\n\n"
"Usage: stored [options] [-c config_file] [config_file]\n"
"        -c <file>   use <file> as configuration file\n"
"        -dnn        set debug level to nn\n"
"        -f          run in foreground (for debugging)\n"
"        -g <group>  set groupid to group\n"
"        -p          proceed despite I/O errors\n"
"        -s          no signals (for debugging)\n"
"        -t          test - read config and exit\n"
"        -u <user>   userid to <user>\n"
"        -v          verbose user messages\n"
"        -?          print this message.\n"
"\n"), VERSION, BDATE);
   exit(1);
}

/*********************************************************************
 *
 *  Main Bacula Unix Storage Daemon
 *
 */
int main (int argc, char *argv[])
{
   int ch;
   int no_signals = FALSE;
   int test_config = FALSE;
   pthread_t thid;
   char *uid = NULL;
   char *gid = NULL;

   setlocale(LC_ALL, "");
   bindtextdomain("bacula", LOCALEDIR);
   textdomain("bacula");

   init_stack_dump();
   my_name_is(argc, argv, "bacula-sd");
   init_msg(NULL, NULL);
   daemon_start_time = time(NULL);

   /* Sanity checks */
   if (TAPE_BSIZE % B_DEV_BSIZE != 0 || TAPE_BSIZE / B_DEV_BSIZE == 0) {
      Emsg2(M_ABORT, 0, _("Tape block size (%d) not multiple of system size (%d)\n"),
         TAPE_BSIZE, B_DEV_BSIZE);
   }
   if (TAPE_BSIZE != (1 << (ffs(TAPE_BSIZE)-1))) {
      Emsg1(M_ABORT, 0, _("Tape block size (%d) is not a power of 2\n"), TAPE_BSIZE);
   }

   while ((ch = getopt(argc, argv, "c:d:fg:pstu:v?")) != -1) {
      switch (ch) {
      case 'c':                    /* configuration file */
         if (configfile != NULL) {
            free(configfile);
         }
         configfile = bstrdup(optarg);
         break;

      case 'd':                    /* debug level */
         debug_level = atoi(optarg);
         if (debug_level <= 0) {
            debug_level = 1;
         }
         break;

      case 'f':                    /* run in foreground */
         foreground = TRUE;
         break;

      case 'g':                    /* set group id */
         gid = optarg;
         break;

      case 'p':                    /* proceed in spite of I/O errors */
         forge_on = true;
         break;

      case 's':                    /* no signals */
         no_signals = TRUE;
         break;

      case 't':
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
         break;
      }
   }
   argc -= optind;
   argv += optind;

   if (argc) {
      if (configfile != NULL) {
         free(configfile);
      }
      configfile = bstrdup(*argv);
      argc--;
      argv++;
   }
   if (argc)
      usage();

   if (!no_signals) {
      init_signals(terminate_stored);
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
      terminate_stored(0);
   }

   my_name_is(0, (char **)NULL, me->hdr.name);     /* Set our real name */

   if (!foreground) {
      daemon_start();                 /* become daemon */
      init_stack_dump();              /* pick up new pid */
   }

   create_pid_file(me->pid_directory, "bacula-sd", get_first_port_host_order(me->sdaddrs));
   read_state_file(me->working_directory, "bacula-sd", get_first_port_host_order(me->sdaddrs));

   drop(uid, gid);

   cleanup_old_files();


   /* Ensure that Volume Session Time and Id are both
    * set and are both non-zero.
    */
   VolSessionTime = (uint32_t)daemon_start_time;
   if (VolSessionTime == 0) { /* paranoid */
      Jmsg0(NULL, M_ABORT, 0, _("Volume Session Time is ZERO!\n"));
   }

   init_python_interpreter(me->hdr.name, me->scripts_directory, "SDStartUp");

   /* Make sure on Solaris we can run concurrent, watch dog + servers + misc */
   set_thread_concurrency(me->max_concurrent_jobs * 2 + 4);

    /*
     * Start the device allocation thread
     */
   init_volume_list();                /* do before device_init */
   if (pthread_create(&thid, NULL, device_initialization, NULL) != 0) {
      Emsg1(M_ABORT, 0, _("Unable to create thread. ERR=%s\n"), strerror(errno));
   }

   start_watchdog();                  /* start watchdog thread */
   init_jcr_subsystem();              /* start JCR watchdogs etc. */

   /* Single server used for Director and File daemon */
   bnet_thread_server(me->sdaddrs, me->max_concurrent_jobs * 2 + 1,
                      &dird_workq, handle_connection_request);
   exit(1);                           /* to keep compiler quiet */
}

/* Return a new Session Id */
uint32_t newVolSessionId()
{
   uint32_t Id;

   P(mutex);
   VolSessionId++;
   Id = VolSessionId;
   V(mutex);
   return Id;
}

/* Check Configuration file for necessary info */
static int check_resources()
{
   bool OK = true;


   me = (STORES *)GetNextRes(R_STORAGE, NULL);
   if (!me) {
      Jmsg1(NULL, M_ERROR, 0, _("No Storage resource defined in %s. Cannot continue.\n"),
         configfile);
      OK = false;
   }

   if (GetNextRes(R_STORAGE, (RES *)me) != NULL) {
      Jmsg1(NULL, M_ERROR, 0, _("Only one Storage resource permitted in %s\n"),
         configfile);
      OK = false;
   }
   if (GetNextRes(R_DIRECTOR, NULL) == NULL) {
      Jmsg1(NULL, M_ERROR, 0, _("No Director resource defined in %s. Cannot continue.\n"),
         configfile);
      OK = false;
   }
   if (GetNextRes(R_DEVICE, NULL) == NULL){
      Jmsg1(NULL, M_ERROR, 0, _("No Device resource defined in %s. Cannot continue.\n"),
           configfile);
      OK = false;
   }

   if (!me->messages) {
      me->messages = (MSGS *)GetNextRes(R_MSGS, NULL);
      if (!me->messages) {
         Jmsg1(NULL, M_ERROR, 0, _("No Messages resource defined in %s. Cannot continue.\n"),
            configfile);
         OK = false;
      }
   }

   if (!me->working_directory) {
      Jmsg1(NULL, M_ERROR, 0, _("No Working Directory defined in %s. Cannot continue.\n"),
         configfile);
      OK = false;
   }

   DIRRES *director;
   STORES *store;
   foreach_res(store, R_STORAGE) { 
      /* tls_require implies tls_enable */
      if (store->tls_require) {
         if (have_tls) {
            store->tls_enable = true;
         } else {
            Jmsg(NULL, M_FATAL, 0, _("TLS required but not configured in Bacula.\n"));
            OK = false;
            continue;
         }
      }

      if (!store->tls_certfile && store->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Certificate\" file not defined for Storage \"%s\" in %s.\n"),
              store->hdr.name, configfile);
         OK = false;
      }

      if (!store->tls_keyfile && store->tls_enable) {
         Jmsg(NULL, M_FATAL, 0, _("\"TLS Key\" file not defined for Storage \"%s\" in %s.\n"),
              store->hdr.name, configfile);
         OK = false;
      }

      if ((!store->tls_ca_certfile && !store->tls_ca_certdir) && store->tls_enable && store->tls_verify_peer) {
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
              " or \"TLS CA Certificate Dir\" are defined for Storage \"%s\" in %s."
              " At least one CA certificate store is required"
              " when using \"TLS Verify Peer\".\n"),
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
            store->tls_keyfile, NULL, NULL, store->tls_dhfile,
            store->tls_verify_peer);

         if (!store->tls_ctx) { 
            Jmsg(NULL, M_FATAL, 0, _("Failed to initialize TLS context for Storage \"%s\" in %s.\n"),
                 store->hdr.name, configfile);
            OK = false;
         }
      }
   }

   foreach_res(director, R_DIRECTOR) { 
      /* tls_require implies tls_enable */
      if (director->tls_require) {
         director->tls_enable = true;
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
         Jmsg(NULL, M_FATAL, 0, _("Neither \"TLS CA Certificate\""
              " or \"TLS CA Certificate Dir\" are defined for Director \"%s\" in %s."
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

   OK = init_autochangers();

   
   if (OK) {
      close_msg(NULL);                   /* close temp message handler */
      init_msg(NULL, me->messages);      /* open daemon message handler */
      set_working_directory(me->working_directory);
   }

   return OK;
}

static void cleanup_old_files()
{
   POOLMEM *cleanup = get_pool_memory(PM_MESSAGE);
   int len = strlen(me->working_directory);
   pm_strcpy(cleanup, "/bin/rm -f ");
   pm_strcat(cleanup, me->working_directory);
   if (len > 0 && me->working_directory[len-1] != '/') {
      pm_strcat(cleanup, "/");
   }
   pm_strcat(cleanup, my_name);
   pm_strcat(cleanup, "*.spool");
   run_program(cleanup, 0, NULL);
   free_pool_memory(cleanup);
}      


/*
 * Here we attempt to init and open each device. This is done
 *  once at startup in a separate thread.
 */
extern "C"
void *device_initialization(void *arg)
{
   DEVRES *device;
   DCR *dcr;
   JCR *jcr;
   DEVICE *dev;

   LockRes();

   pthread_detach(pthread_self());
   jcr = new_jcr(sizeof(JCR), stored_free_jcr);
   jcr->JobType = JT_SYSTEM;
   /* Initialize FD start condition variable */
   int errstat = pthread_cond_init(&jcr->job_start_wait, NULL);
   if (errstat != 0) {
      Jmsg1(jcr, M_ABORT, 0, _("Unable to init job cond variable: ERR=%s\n"), strerror(errstat));
   }

   foreach_res(device, R_DEVICE) {
      Dmsg1(90, "calling init_dev %s\n", device->device_name);
      dev = init_dev(NULL, device);
      Dmsg1(10, "SD init done %s\n", device->device_name);
      if (!dev) {
         Jmsg1(NULL, M_ERROR, 0, _("Could not initialize %s\n"), device->device_name);
         continue;
      }

      jcr->dcr = dcr = new_dcr(jcr, dev);
      if (dev->is_autochanger()) {
         /* If autochanger set slot in dev sturcture */
         get_autochanger_loaded_slot(dcr);
      }

      if (device->cap_bits & CAP_ALWAYSOPEN) {
         Dmsg1(20, "calling first_open_device %s\n", dev->print_name());
         if (!first_open_device(dcr)) {
            Jmsg1(NULL, M_ERROR, 0, _("Could not open device %s\n"), dev->print_name());
            Dmsg1(20, "Could not open device %s\n", dev->print_name());
            free_dcr(dcr);
            jcr->dcr = NULL;
            continue;
         }
      }
      if (device->cap_bits & CAP_AUTOMOUNT && dev->is_open()) {
         switch (read_dev_volume_label(dcr)) {
         case VOL_OK:
            memcpy(&dev->VolCatInfo, &dcr->VolCatInfo, sizeof(dev->VolCatInfo));
            break;
         default:
            Jmsg1(NULL, M_WARNING, 0, _("Could not mount device %s\n"), dev->print_name());
            break;
         }
      }
      free_dcr(dcr);
      jcr->dcr = NULL;
   }
   free_jcr(jcr); 
   init_done = true;
   UnlockRes();
   return NULL;
}


/* Clean up and then exit */
void terminate_stored(int sig)
{
   static bool in_here = false;
   DEVRES *device;
   JCR *jcr;

   if (in_here) {                     /* prevent loops */
      exit(1);
   }
   in_here = true;

   if (sig == SIGTERM) {              /* normal shutdown request? */
      /*
       * This is a normal shutdown request. We wiffle through
       *   all open jobs canceling them and trying to wake
       *   them up so that they will report back the correct
       *   volume status.
       */
      foreach_jcr(jcr) {
         BSOCK *fd;
         if (jcr->JobId == 0) {
            free_jcr(jcr);
            continue;                 /* ignore console */
         }
         set_jcr_job_status(jcr, JS_Canceled);
         fd = jcr->file_bsock;
         if (fd) {
            fd->timed_out = true;
            Dmsg1(100, "term_stored killing JobId=%d\n", jcr->JobId);
            pthread_kill(jcr->my_thread_id, TIMEOUT_SIGNAL);
            /* ***FIXME*** wiffle through all dcrs */
            if (jcr->dcr && jcr->dcr->dev && jcr->dcr->dev->dev_blocked) {
               pthread_cond_broadcast(&jcr->dcr->dev->wait_next_vol);
               pthread_cond_broadcast(&wait_device_release);
            }
            if (jcr->read_dcr && jcr->read_dcr->dev && jcr->read_dcr->dev->dev_blocked) {
               pthread_cond_broadcast(&jcr->read_dcr->dev->wait_next_vol);
               pthread_cond_broadcast(&wait_device_release);
            }
            bmicrosleep(0, 50000);
         }
         free_jcr(jcr);
      }
      bmicrosleep(0, 500000);         /* give them 1/2 sec to clean up */
   }

   write_state_file(me->working_directory, "bacula-sd", get_first_port_host_order(me->sdaddrs));
   delete_pid_file(me->pid_directory, "bacula-sd", get_first_port_host_order(me->sdaddrs));

   Dmsg1(200, "In terminate_stored() sig=%d\n", sig);

   foreach_res(device, R_DEVICE) {
      Dmsg1(10, "Term device %s\n", device->device_name);
      if (device->dev) {
         free_volume(device->dev);
         device->dev->term();
         device->dev = NULL;
      } else {
         Dmsg1(10, "No dev structure %s\n", device->device_name);
      }
   }

   if (configfile) {
      free(configfile);
      configfile = NULL;
   }
   free_config_resources();

   if (debug_level > 10) {
      print_memory_pool_stats();
   }
   term_msg();
   stop_watchdog();
   cleanup_crypto();
   free_volume_list();
   close_memory_pool();

   sm_dump(false);                    /* dump orphaned buffers */
   exit(sig);
}
