/*
 *
 *   Bacula Tape manipulation program
 *
 *    Has various tape manipulation commands -- mostly for
 *    use in determining how tapes really work.
 *
 *     Kern Sibbald, April MM
 *
 *   Note, this program reads stored.conf, and will only
 *     talk to devices that are configured.
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

/* Dummy functions */
int generate_daemon_event(JCR *jcr, const char *event) { return 1; }

/* External subroutines */
extern void free_config_resources();

/* Exported variables */
int quit = 0;
char buf[100000];
int bsize = TAPE_BSIZE;
char VolName[MAX_NAME_LENGTH];
STORES *me = NULL;                    /* our Global resource */
bool forge_on = false;                /* proceed inspite of I/O errors */
pthread_mutex_t device_release_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wait_device_release = PTHREAD_COND_INITIALIZER;

/*
 * If you change the format of the state file,
 *  increment this value
 */
static uint32_t btape_state_level = 2;

DEVICE *dev = NULL;
DCR *dcr;
DEVRES *device = NULL;


/* Forward referenced subroutines */
static void do_tape_cmds();
static void helpcmd();
static void scancmd();
static void rewindcmd();
static void clearcmd();
static void wrcmd();
static void rrcmd();
static void eodcmd();
static void fillcmd();
static void qfillcmd();
static void statcmd();
static void unfillcmd();
static int flush_block(DEV_BLOCK *block, int dump);
static bool quickie_cb(DCR *dcr, DEV_RECORD *rec);
static bool compare_blocks(DEV_BLOCK *last_block, DEV_BLOCK *block);
static bool my_mount_next_read_volume(DCR *dcr);
static void scan_blocks();
static void set_volume_name(const char *VolName, int volnum);
static void rawfill_cmd();
static void bfill_cmd();
static bool open_the_device();
static void autochangercmd();
static void do_unfill();


/* Static variables */
#define CONFIG_FILE "bacula-sd.conf"
char *configfile = NULL;

#define MAX_CMD_ARGS 30
static POOLMEM *cmd;
static POOLMEM *args;
static char *argk[MAX_CMD_ARGS];
static char *argv[MAX_CMD_ARGS];
static int argc;

static int quickie_count = 0;
static uint64_t write_count = 0;
static BSR *bsr = NULL;
static int signals = TRUE;
static bool ok;
static int stop = 0;
static uint64_t vol_size;
static uint64_t VolBytes;
static time_t now;
static double kbs;
static int32_t file_index;
static int end_of_tape = 0;
static uint32_t LastBlock = 0;
static uint32_t eot_block;
static uint32_t eot_block_len;
static uint32_t eot_FileIndex;
static int dumped = 0;
static DEV_BLOCK *last_block1 = NULL;
static DEV_BLOCK *last_block2 = NULL;
static DEV_BLOCK *last_block = NULL;
static DEV_BLOCK *this_block = NULL;
static DEV_BLOCK *first_block = NULL;
static uint32_t last_file1 = 0;
static uint32_t last_file2 = 0;
static uint32_t last_file = 0;
static uint32_t last_block_num1 = 0;
static uint32_t last_block_num2 = 0;
static uint32_t last_block_num = 0;
static uint32_t BlockNumber = 0;
static bool simple = true;

static const char *VolumeName = NULL;
static int vol_num = 0;

static JCR *jcr = NULL;


static void usage();
static void terminate_btape(int sig);
int get_cmd(const char *prompt);


/*********************************************************************
 *
 *     Bacula tape testing program
 *
 */
int main(int margc, char *margv[])
{
   int ch, i;
   uint32_t x32, y32;
   uint64_t x64, y64;
   char buf[1000];
   
   setlocale(LC_ALL, "");
   bindtextdomain("bacula", LOCALEDIR);
   textdomain("bacula");

   /* Sanity checks */
   if (TAPE_BSIZE % B_DEV_BSIZE != 0 || TAPE_BSIZE / B_DEV_BSIZE == 0) {
      Emsg2(M_ABORT, 0, _("Tape block size (%d) not multiple of system size (%d)\n"),
         TAPE_BSIZE, B_DEV_BSIZE);
   }
   if (TAPE_BSIZE != (1 << (ffs(TAPE_BSIZE)-1))) {
      Emsg1(M_ABORT, 0, _("Tape block size (%d) is not a power of 2\n"), TAPE_BSIZE);
   }
   if (sizeof(off_t) < 8) {
      Pmsg1(-1, _("\n\n!!!! Warning large disk addressing disabled. off_t=%d should be 8 or more !!!!!\n\n\n"),
         sizeof(off_t));
   }
   x32 = 123456789;
   bsnprintf(buf, sizeof(buf), "%u", x32);
   i = bsscanf(buf, "%u", &y32);
   if (i != 1 || x32 != y32) {
      Pmsg3(-1, _("32 bit printf/scanf problem. i=%d x32=%u y32=%u\n"), i, x32, y32);
      exit(1);
   }
   x64 = 123456789;
   x64 = x64 << 32;
   x64 += 123456789;
   bsnprintf(buf, sizeof(buf), "%" llu, x64);
   i = bsscanf(buf, "%llu", &y64);
   if (i != 1 || x64 != y64) {
      Pmsg3(-1, _("64 bit printf/scanf problem. i=%d x64=%" llu " y64=%" llu "\n"), 
            i, x64, y64);
      exit(1);
   }

   printf(_("Tape block granularity is %d bytes.\n"), TAPE_BSIZE);

   working_directory = "/tmp";
   my_name_is(margc, margv, "btape");
   init_msg(NULL, NULL);

   OSDependentInit();

   while ((ch = getopt(margc, margv, "b:c:d:psv?")) != -1) {
      switch (ch) {
      case 'b':                    /* bootstrap file */
         bsr = parse_bsr(NULL, optarg);
//       dump_bsr(bsr, true);
         break;

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
         break;

      case 'p':
         forge_on = true;
         break;

      case 's':
         signals = false;
         break;

      case 'v':
         verbose++;
         break;

      case '?':
      default:
         helpcmd();
         exit(0);

      }
   }
   margc -= optind;
   margv += optind;

   cmd = get_pool_memory(PM_FNAME);
   args = get_pool_memory(PM_FNAME);

   if (signals) {
      init_signals(terminate_btape);
   }

   if (configfile == NULL) {
      configfile = bstrdup(CONFIG_FILE);
   }

   daemon_start_time = time(NULL);

   parse_config(configfile);


   /* See if we can open a device */
   if (margc == 0) {
      Pmsg0(000, _("No archive name specified.\n"));
      usage();
      exit(1);
   } else if (margc != 1) {
      Pmsg0(000, _("Improper number of arguments specified.\n"));
      usage();
      exit(1);
   }

   jcr = setup_jcr("btape", margv[0], bsr, NULL, 0); /* write */
   if (!jcr) {
      exit(1);
   }
   dev = jcr->dcr->dev;
   if (!dev) {
      exit(1);
   }
   dcr = jcr->dcr;
   if (!open_the_device()) {
      goto terminate;
   }

   Dmsg0(200, "Do tape commands\n");
   do_tape_cmds();

terminate:
   terminate_btape(0);
   return 0;
}

static void terminate_btape(int stat)
{

   sm_check(__FILE__, __LINE__, false);
   if (configfile) {
      free(configfile);
   }
   free_config_resources();
   if (args) {
      free_pool_memory(args);
      args = NULL;
   }
   if (cmd) {
      free_pool_memory(cmd);
      cmd = NULL;
   }

   if (bsr) {
      free_bsr(bsr);
   }

   free_jcr(jcr);
   jcr = NULL;

   if (dev) {
      dev->term();
   }

   free_volume_list();

   if (debug_level > 10)
      print_memory_pool_stats();

   if (this_block) {
      free_block(this_block);
   }

   stop_watchdog();
   term_msg();
   close_memory_pool();               /* free memory in pool */
   term_last_jobs_list();

   sm_dump(false);
   exit(stat);
}

static bool open_the_device()
{
   DEV_BLOCK *block;

   block = new_block(dev);
   lock_device(dev);
   Dmsg1(200, "Opening device %s\n", dcr->VolumeName);
   if (dev->open(dcr, OPEN_READ_WRITE) < 0) {
      Emsg1(M_FATAL, 0, _("dev open failed: %s\n"), dev->errmsg);
      unlock_device(dev);
      free_block(block);
      return false;
   }
   Pmsg1(000, _("open device %s: OK\n"), dev->print_name());
   dev->set_append();                 /* put volume in append mode */
   unlock_device(dev);
   free_block(block);
   return true;
}


void quitcmd()
{
   quit = 1;
}

/*
 * Write a label to the tape
 */
static void labelcmd()
{
   if (VolumeName) {
      pm_strcpy(cmd, VolumeName);
   } else {
      if (!get_cmd(_("Enter Volume Name: "))) {
         return;
      }
   }

   if (!dev->is_open()) {
      if (!first_open_device(dcr)) {
         Pmsg1(0, _("Device open failed. ERR=%s\n"), dev->bstrerror());
      }
   }
   dev->rewind(dcr);
   write_new_volume_label_to_dev(dcr, cmd, "Default");
   Pmsg1(-1, _("Wrote Volume label for volume \"%s\".\n"), cmd);
}

/*
 * Read the tape label
 */
static void readlabelcmd()
{
   int save_debug_level = debug_level;
   int stat;

   stat = read_dev_volume_label(dcr);
   switch (stat) {
   case VOL_NO_LABEL:
      Pmsg0(0, _("Volume has no label.\n"));
      break;
   case VOL_OK:
      Pmsg0(0, _("Volume label read correctly.\n"));
      break;
   case VOL_IO_ERROR:
      Pmsg1(0, _("I/O error on device: ERR=%s"), dev->bstrerror());
      break;
   case VOL_NAME_ERROR:
      Pmsg0(0, _("Volume name error\n"));
      break;
   case VOL_CREATE_ERROR:
      Pmsg1(0, _("Error creating label. ERR=%s"), dev->bstrerror());
      break;
   case VOL_VERSION_ERROR:
      Pmsg0(0, _("Volume version error.\n"));
      break;
   case VOL_LABEL_ERROR:
      Pmsg0(0, _("Bad Volume label type.\n"));
      break;
   default:
      Pmsg0(0, _("Unknown error.\n"));
      break;
   }

   debug_level = 20;
   dump_volume_label(dev);
   debug_level = save_debug_level;
}


/*
 * Load the tape should have prevously been taken
 * off line, otherwise this command is not necessary.
 */
static void loadcmd()
{

   if (!load_dev(dev)) {
      Pmsg1(0, _("Bad status from load. ERR=%s\n"), dev->bstrerror());
   } else
      Pmsg1(0, _("Loaded %s\n"), dev->print_name());
}

/*
 * Rewind the tape.
 */
static void rewindcmd()
{
   if (!dev->rewind(dcr)) {
      Pmsg1(0, _("Bad status from rewind. ERR=%s\n"), dev->bstrerror());
      dev->clrerror(-1);
   } else {
      Pmsg1(0, _("Rewound %s\n"), dev->print_name());
   }
}

/*
 * Clear any tape error
 */
static void clearcmd()
{
   dev->clrerror(-1);
}

/*
 * Write and end of file on the tape
 */
static void weofcmd()
{
   int num = 1;
   if (argc > 1) {
      num = atoi(argk[1]);
   }
   if (num <= 0) {
      num = 1;
   }

   if (!dev->weof(num)) {
      Pmsg1(0, _("Bad status from weof. ERR=%s\n"), dev->bstrerror());
      return;
   } else {
      if (num==1) {
         Pmsg1(0, _("Wrote 1 EOF to %s\n"), dev->print_name());
      }
      else {
         Pmsg2(0, _("Wrote %d EOFs to %s\n"), num, dev->print_name());
      }
   }
}


/* Go to the end of the medium -- raw command
 * The idea was orginally that the end of the Bacula
 * medium would be flagged differently. This is not
 * currently the case. So, this is identical to the
 * eodcmd().
 */
static void eomcmd()
{
   if (!dev->eod()) {
      Pmsg1(0, "%s", dev->bstrerror());
      return;
   } else {
      Pmsg0(0, _("Moved to end of medium.\n"));
   }
}

/*
 * Go to the end of the medium (either hardware determined
 *  or defined by two eofs.
 */
static void eodcmd()
{
   eomcmd();
}

/*
 * Backspace file
 */
static void bsfcmd()
{
   int num = 1;
   if (argc > 1) {
      num = atoi(argk[1]);
   }
   if (num <= 0) {
      num = 1;
   }

   if (!dev->bsf(num)) {
      Pmsg1(0, _("Bad status from bsf. ERR=%s\n"), dev->bstrerror());
   } else {
      Pmsg2(0, _("Backspaced %d file%s.\n"), num, num==1?"":"s");
   }
}

/*
 * Backspace record
 */
static void bsrcmd()
{
   int num = 1;
   if (argc > 1) {
      num = atoi(argk[1]);
   }
   if (num <= 0) {
      num = 1;
   }
   if (!dev->bsr(num)) {
      Pmsg1(0, _("Bad status from bsr. ERR=%s\n"), dev->bstrerror());
   } else {
      Pmsg2(0, _("Backspaced %d record%s.\n"), num, num==1?"":"s");
   }
}

/*
 * List device capabilities as defined in the
 *  stored.conf file.
 */
static void capcmd()
{
   printf(_("Configured device capabilities:\n"));
   printf("%sEOF ", dev->capabilities & CAP_EOF ? "" : "!");
   printf("%sBSR ", dev->capabilities & CAP_BSR ? "" : "!");
   printf("%sBSF ", dev->capabilities & CAP_BSF ? "" : "!");
   printf("%sFSR ", dev->capabilities & CAP_FSR ? "" : "!");
   printf("%sFSF ", dev->capabilities & CAP_FSF ? "" : "!");
   printf("%sFASTFSF ", dev->capabilities & CAP_FASTFSF ? "" : "!");
   printf("%sBSFATEOM ", dev->capabilities & CAP_BSFATEOM ? "" : "!");
   printf("%sEOM ", dev->capabilities & CAP_EOM ? "" : "!");
   printf("%sREM ", dev->capabilities & CAP_REM ? "" : "!");
   printf("%sRACCESS ", dev->capabilities & CAP_RACCESS ? "" : "!");
   printf("%sAUTOMOUNT ", dev->capabilities & CAP_AUTOMOUNT ? "" : "!");
   printf("%sLABEL ", dev->capabilities & CAP_LABEL ? "" : "!");
   printf("%sANONVOLS ", dev->capabilities & CAP_ANONVOLS ? "" : "!");
   printf("%sALWAYSOPEN ", dev->capabilities & CAP_ALWAYSOPEN ? "" : "!");
   printf("%sMTIOCGET ", dev->capabilities & CAP_MTIOCGET ? "" : "!");
   printf("\n");

   printf(_("Device status:\n"));
   printf("%sOPENED ", dev->is_open() ? "" : "!");
   printf("%sTAPE ", dev->is_tape() ? "" : "!");
   printf("%sLABEL ", dev->is_labeled() ? "" : "!");
   printf("%sMALLOC ", dev->state & ST_MALLOC ? "" : "!");
   printf("%sAPPEND ", dev->can_append() ? "" : "!");
   printf("%sREAD ", dev->can_read() ? "" : "!");
   printf("%sEOT ", dev->at_eot() ? "" : "!");
   printf("%sWEOT ", dev->state & ST_WEOT ? "" : "!");
   printf("%sEOF ", dev->at_eof() ? "" : "!");
   printf("%sNEXTVOL ", dev->state & ST_NEXTVOL ? "" : "!");
   printf("%sSHORT ", dev->state & ST_SHORT ? "" : "!");
   printf("\n");

   printf(_("Device parameters:\n"));
   printf("Device name: %s\n", dev->dev_name);
   printf("File=%u block=%u\n", dev->file, dev->block_num);
   printf("Min block=%u Max block=%u\n", dev->min_block_size, dev->max_block_size);

   printf(_("Status:\n"));
   statcmd();

}

/*
 * Test writting larger and larger records.
 * This is a torture test for records.
 */
static void rectestcmd()
{
   DEV_BLOCK *block;
   DEV_RECORD *rec;
   int i, blkno = 0;

   Pmsg0(0, _("Test writting larger and larger records.\n"
"This is a torture test for records.\nI am going to write\n"
"larger and larger records. It will stop when the record size\n"
"plus the header exceeds the block size (by default about 64K)\n"));


   get_cmd(_("Do you want to continue? (y/n): "));
   if (cmd[0] != 'y') {
      Pmsg0(000, _("Command aborted.\n"));
      return;
   }

   sm_check(__FILE__, __LINE__, false);
   block = new_block(dev);
   rec = new_record();

   for (i=1; i<500000; i++) {
      rec->data = check_pool_memory_size(rec->data, i);
      memset(rec->data, i & 0xFF, i);
      rec->data_len = i;
      sm_check(__FILE__, __LINE__, false);
      if (write_record_to_block(block, rec)) {
         empty_block(block);
         blkno++;
         Pmsg2(0, _("Block %d i=%d\n"), blkno, i);
      } else {
         break;
      }
      sm_check(__FILE__, __LINE__, false);
   }
   free_record(rec);
   free_block(block);
   sm_check(__FILE__, __LINE__, false);
}

/*
 * This test attempts to re-read a block written by Bacula
 *   normally at the end of the tape. Bacula will then back up
 *   over the two eof marks, backup over the record and reread
 *   it to make sure it is valid.  Bacula can skip this validation
 *   if you set "Backward space record = no"
 */
static int re_read_block_test()
{
   DEV_BLOCK *block = dcr->block;
   DEV_RECORD *rec;
   int stat = 0;
   int len;

   if (!(dev->capabilities & CAP_BSR)) {
      Pmsg0(-1, _("Skipping read backwards test because BSR turned off.\n"));
      return 0;
   }

   Pmsg0(-1, _("\n=== Write, backup, and re-read test ===\n\n"
      "I'm going to write three records and an EOF\n"
      "then backup over the EOF and re-read the last record.\n"
      "Bacula does this after writing the last block on the\n"
      "tape to verify that the block was written correctly.\n\n"
      "This is not an *essential* feature ...\n\n"));
   rewindcmd();
   empty_block(block);
   rec = new_record();
   rec->data = check_pool_memory_size(rec->data, block->buf_len);
   len = rec->data_len = block->buf_len-100;
   memset(rec->data, 1, rec->data_len);
   if (!write_record_to_block(block, rec)) {
      Pmsg0(0, _("Error writing record to block.\n"));
      goto bail_out;
   }
   if (!write_block_to_dev(dcr)) {
      Pmsg0(0, _("Error writing block to device.\n"));
      goto bail_out;
   } else {
      Pmsg1(0, _("Wrote first record of %d bytes.\n"), rec->data_len);
   }
   memset(rec->data, 2, rec->data_len);
   if (!write_record_to_block(block, rec)) {
      Pmsg0(0, _("Error writing record to block.\n"));
      goto bail_out;
   }
   if (!write_block_to_dev(dcr)) {
      Pmsg0(0, _("Error writing block to device.\n"));
      goto bail_out;
   } else {
      Pmsg1(0, _("Wrote second record of %d bytes.\n"), rec->data_len);
   }
   memset(rec->data, 3, rec->data_len);
   if (!write_record_to_block(block, rec)) {
      Pmsg0(0, _("Error writing record to block.\n"));
      goto bail_out;
   }
   if (!write_block_to_dev(dcr)) {
      Pmsg0(0, _("Error writing block to device.\n"));
      goto bail_out;
   } else {
      Pmsg1(0, _("Wrote third record of %d bytes.\n"), rec->data_len);
   }
   weofcmd();
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }
   if (!dev->bsf(1)) {
      Pmsg1(0, _("Backspace file failed! ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   if (dev_cap(dev, CAP_TWOEOF)) {
      if (!dev->bsf(1)) {
         Pmsg1(0, _("Backspace file failed! ERR=%s\n"), dev->bstrerror());
         goto bail_out;
      }
   }
   Pmsg0(0, _("Backspaced over EOF OK.\n"));
   if (!dev->bsr(1)) {
      Pmsg1(0, _("Backspace record failed! ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg0(0, _("Backspace record OK.\n"));
   if (!read_block_from_dev(dcr, NO_BLOCK_NUMBER_CHECK)) {
      berrno be;
      Pmsg1(0, _("Read block failed! ERR=%s\n"), be.strerror(dev->dev_errno));
      goto bail_out;
   }
   memset(rec->data, 0, rec->data_len);
   if (!read_record_from_block(block, rec)) {
      berrno be;
      Pmsg1(0, _("Read block failed! ERR=%s\n"), be.strerror(dev->dev_errno));
      goto bail_out;
   }
   for (int i=0; i<len; i++) {
      if (rec->data[i] != 3) {
         Pmsg0(0, _("Bad data in record. Test failed!\n"));
         goto bail_out;
      }
   }
   Pmsg0(0, _("\nBlock re-read correct. Test succeeded!\n"));
   Pmsg0(-1, _("=== End Write, backup, and re-read test ===\n\n"));

   stat = 1;

bail_out:
   free_record(rec);
   if (stat == 0) {
      Pmsg0(0, _("This is not terribly serious since Bacula only uses\n"
                 "this function to verify the last block written to the\n"
                 "tape. Bacula will skip the last block verification\n"
                 "if you add:\n\n"
                  "Backward Space Record = No\n\n"
                  "to your Storage daemon's Device resource definition.\n"));
   }
   return stat;
}


/*
 * This test writes Bacula blocks to the tape in
 *   several files. It then rewinds the tape and attepts
 *   to read these blocks back checking the data.
 */
static int write_read_test()
{
   DEV_BLOCK *block;
   DEV_RECORD *rec;
   int stat = 0;
   int len, i, j;
   int *p;

   Pmsg0(-1, _("\n=== Write, rewind, and re-read test ===\n\n"
      "I'm going to write 1000 records and an EOF\n"
      "then write 1000 records and an EOF, then rewind,\n"
      "and re-read the data to verify that it is correct.\n\n"
      "This is an *essential* feature ...\n\n"));
   block = dcr->block;
   rec = new_record();
   if (!dev->rewind(dcr)) {
      Pmsg1(0, _("Bad status from rewind. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   rec->data = check_pool_memory_size(rec->data, block->buf_len);
   rec->data_len = block->buf_len-100;
   len = rec->data_len/sizeof(i);
   for (i=1; i<=1000; i++) {
      p = (int *)rec->data;
      for (j=0; j<len; j++) {
         *p++ = i;
      }
      if (!write_record_to_block(block, rec)) {
         Pmsg0(0, _("Error writing record to block.\n"));
         goto bail_out;
      }
      if (!write_block_to_dev(dcr)) {
         Pmsg0(0, _("Error writing block to device.\n"));
         goto bail_out;
      }
   }
   Pmsg1(0, _("Wrote 1000 blocks of %d bytes.\n"), rec->data_len);
   weofcmd();
   for (i=1001; i<=2000; i++) {
      p = (int *)rec->data;
      for (j=0; j<len; j++) {
         *p++ = i;
      }
      if (!write_record_to_block(block, rec)) {
         Pmsg0(0, _("Error writing record to block.\n"));
         goto bail_out;
      }
      if (!write_block_to_dev(dcr)) {
         Pmsg0(0, _("Error writing block to device.\n"));
         goto bail_out;
      }
   }
   Pmsg1(0, _("Wrote 1000 blocks of %d bytes.\n"), rec->data_len);
   weofcmd();
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }
   if (!dev->rewind(dcr)) {
      Pmsg1(0, _("Bad status from rewind. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   } else {
      Pmsg0(0, _("Rewind OK.\n"));
   }
   for (i=1; i<=2000; i++) {
read_again:
      if (!read_block_from_dev(dcr, NO_BLOCK_NUMBER_CHECK)) {
         berrno be;
         if (dev_state(dev, ST_EOF)) {
            Pmsg0(-1, _("Got EOF on tape.\n"));
            if (i == 1001) {
               goto read_again;
            }
         }
         Pmsg2(0, _("Read block %d failed! ERR=%s\n"), i, be.strerror(dev->dev_errno));
         goto bail_out;
      }
      memset(rec->data, 0, rec->data_len);
      if (!read_record_from_block(block, rec)) {
         berrno be;
         Pmsg2(0, _("Read record failed. Block %d! ERR=%s\n"), i, be.strerror(dev->dev_errno));
         goto bail_out;
      }
      p = (int *)rec->data;
      for (j=0; j<len; j++) {
         if (*p != i) {
            Pmsg3(0, _("Bad data in record. Expected %d, got %d at byte %d. Test failed!\n"),
               i, *p, j);
            goto bail_out;
         }
         p++;
      }
      if (i == 1000 || i == 2000) {
         Pmsg0(-1, _("1000 blocks re-read correctly.\n"));
      }
   }
   Pmsg0(-1, _("=== Test Succeeded. End Write, rewind, and re-read test ===\n\n"));
   stat = 1;

bail_out:
   free_record(rec);
   return stat;
}

/*
 * This test writes Bacula blocks to the tape in
 *   several files. It then rewinds the tape and attepts
 *   to read these blocks back checking the data.
 */
static int position_test()
{
   DEV_BLOCK *block = dcr->block;
   DEV_RECORD *rec;
   int stat = 0;
   int len, i, j;
   bool ok = true;
   int recno = 0;
   int file = 0, blk = 0;
   int *p;
   bool got_eof = false;

   Pmsg0(-1, _("\n=== Write, rewind, and position test ===\n\n"
      "I'm going to write 1000 records and an EOF\n"
      "then write 1000 records and an EOF, then rewind,\n"
      "and position to a few blocks and verify that it is correct.\n\n"
      "This is an *essential* feature ...\n\n"));
   empty_block(block);
   rec = new_record();
   if (!dev->rewind(dcr)) {
      Pmsg1(0, _("Bad status from rewind. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   rec->data = check_pool_memory_size(rec->data, block->buf_len);
   rec->data_len = block->buf_len-100;
   len = rec->data_len/sizeof(i);
   for (i=1; i<=1000; i++) {
      p = (int *)rec->data;
      for (j=0; j<len; j++) {
         *p++ = i;
      }
      if (!write_record_to_block(block, rec)) {
         Pmsg0(0, _("Error writing record to block.\n"));
         goto bail_out;
      }
      if (!write_block_to_dev(dcr)) {
         Pmsg0(0, _("Error writing block to device.\n"));
         goto bail_out;
      }
   }
   Pmsg1(0, _("Wrote 1000 blocks of %d bytes.\n"), rec->data_len);
   weofcmd();
   for (i=1001; i<=2000; i++) {
      p = (int *)rec->data;
      for (j=0; j<len; j++) {
         *p++ = i;
      }
      if (!write_record_to_block(block, rec)) {
         Pmsg0(0, _("Error writing record to block.\n"));
         goto bail_out;
      }
      if (!write_block_to_dev(dcr)) {
         Pmsg0(0, _("Error writing block to device.\n"));
         goto bail_out;
      }
   }
   Pmsg1(0, _("Wrote 1000 blocks of %d bytes.\n"), rec->data_len);
   weofcmd();
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }
   if (!dev->rewind(dcr)) {
      Pmsg1(0, _("Bad status from rewind. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   } else {
      Pmsg0(0, _("Rewind OK.\n"));
   }

   while(ok) {
      /* Set up next item to read based on where we are */
      switch (recno) {
      case 0:
         recno = 5;
         file = 0;
         blk = 4;
         break;
      case 5:
         recno = 201;
         file = 0;
         blk = 200;
         break;
      case 201:
         recno = 1000;
         file = 0;
         blk = 999;
         break;
      case 1000:
         recno = 1001;
         file = 1;
         blk = 0;
         break;
      case 1001:
         recno = 1601;
         file = 1;
         blk = 600;
         break;
      case 1601:
         recno = 2000;
         file = 1;
         blk = 999;
         break;
      case 2000:
         ok = false;
         continue;
      }
      Pmsg2(-1, _("Reposition to file:block %d:%d\n"), file, blk);
      if (!dev->reposition(file, blk)) {
         Pmsg0(0, _("Reposition error.\n"));
         goto bail_out;
      }
read_again:
      if (!read_block_from_dev(dcr, NO_BLOCK_NUMBER_CHECK)) {
         berrno be;
         if (dev_state(dev, ST_EOF)) {
            Pmsg0(-1, _("Got EOF on tape.\n"));
            if (!got_eof) {
               got_eof = true;
               goto read_again;
            }
         }
         Pmsg4(0, _("Read block %d failed! file=%d blk=%d. ERR=%s\n\n"),
            recno, file, blk, be.strerror(dev->dev_errno));
         Pmsg0(0, _("This may be because the tape drive block size is not\n"
                    " set to variable blocking as normally used by Bacula.\n"
                    " Please see the Tape Testing chapter in the manual and \n"
                    " look for using mt with defblksize and setoptions\n"
                    "If your tape drive block size is correct, then perhaps\n"
                    " your SCSI driver is *really* stupid and does not\n"
                    " correctly report the file:block after a FSF. In this\n"
                    " case try setting:\n"
                    "    Fast Forward Space File = no\n"
                    " in your Device resource.\n"));

         goto bail_out;
      }
      memset(rec->data, 0, rec->data_len);
      if (!read_record_from_block(block, rec)) {
         berrno be;
         Pmsg1(0, _("Read record failed! ERR=%s\n"), be.strerror(dev->dev_errno));
         goto bail_out;
      }
      p = (int *)rec->data;
      for (j=0; j<len; j++) {
         if (p[j] != recno) {
            Pmsg3(0, _("Bad data in record. Expected %d, got %d at byte %d. Test failed!\n"),
               recno, p[j], j);
            goto bail_out;
         }
      }
      Pmsg1(-1, _("Block %d re-read correctly.\n"), recno);
   }
   Pmsg0(-1, _("=== Test Succeeded. End Write, rewind, and re-read test ===\n\n"));
   stat = 1;

bail_out:
   free_record(rec);
   return stat;
}




/*
 * This test writes some records, then writes an end of file,
 *   rewinds the tape, moves to the end of the data and attepts
 *   to append to the tape.  This function is essential for
 *   Bacula to be able to write multiple jobs to the tape.
 */
static int append_test()
{
   Pmsg0(-1, _("\n\n=== Append files test ===\n\n"
               "This test is essential to Bacula.\n\n"
"I'm going to write one record  in file 0,\n"
"                   two records in file 1,\n"
"             and three records in file 2\n\n"));
   argc = 1;
   rewindcmd();
   wrcmd();
   weofcmd();      /* end file 0 */
   wrcmd();
   wrcmd();
   weofcmd();      /* end file 1 */
   wrcmd();
   wrcmd();
   wrcmd();
   weofcmd();     /* end file 2 */
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }
   dev->close();              /* release device */
   if (!open_the_device()) {
      return -1;
   }
   rewindcmd();
   Pmsg0(0, _("Now moving to end of medium.\n"));
   eodcmd();
   Pmsg2(-1, _("We should be in file 3. I am at file %d. %s\n"),
      dev->file, dev->file == 3 ? _("This is correct!") : _("This is NOT correct!!!!"));

   if (dev->file != 3) {
      return -1;
   }

   Pmsg0(-1, _("\nNow the important part, I am going to attempt to append to the tape.\n\n"));
   wrcmd();
   weofcmd();
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }
   rewindcmd();
   Pmsg0(-1, _("Done appending, there should be no I/O errors\n\n"));
   Pmsg0(-1, _("Doing Bacula scan of blocks:\n"));
   scan_blocks();
   Pmsg0(-1, _("End scanning the tape.\n"));
   Pmsg2(-1, _("We should be in file 4. I am at file %d. %s\n"),
      dev->file, dev->file == 4 ? _("This is correct!") : _("This is NOT correct!!!!"));

   if (dev->file != 4) {
      return -2;
   }
   return 1;
}


/*
 * This test exercises the autochanger
 */
static int autochanger_test()
{
   POOLMEM *results, *changer;
   int slot, status, loaded;
   int timeout = dcr->device->max_changer_wait;
   int sleep_time = 0;

   Dmsg1(100, "Max changer wait = %d sec\n", timeout);
   if (!dev_cap(dev, CAP_AUTOCHANGER)) {
      return 1;
   }
   if (!(dcr->device && dcr->device->changer_name && dcr->device->changer_command)) {
      Pmsg0(-1, _("\nAutochanger enabled, but no name or no command device specified.\n"));
      return 1;
   }

   Pmsg0(-1, _("\nAh, I see you have an autochanger configured.\n"
             "To test the autochanger you must have a blank tape\n"
             " that I can write on in Slot 1.\n"));
   if (!get_cmd(_("\nDo you wish to continue with the Autochanger test? (y/n): "))) {
      return 0;
   }
   if (cmd[0] != 'y' && cmd[0] != 'Y') {
      return 0;
   }

   Pmsg0(-1, _("\n\n=== Autochanger test ===\n\n"));

   results = get_pool_memory(PM_MESSAGE);
   changer = get_pool_memory(PM_FNAME);

try_again:
   slot = 1;
   dcr->VolCatInfo.Slot = slot;
   /* Find out what is loaded, zero means device is unloaded */
   Pmsg0(-1, _("3301 Issuing autochanger \"loaded\" command.\n"));
   changer = edit_device_codes(dcr, changer, 
                dcr->device->changer_command, "loaded");
   status = run_program(changer, timeout, results);
   Dmsg3(100, "run_prog: %s stat=%d result=\"%s\"\n", changer, status, results);
   if (status == 0) {
      loaded = atoi(results);
   } else {
      berrno be;
      Pmsg1(-1, _("3991 Bad autochanger command: %s\n"), changer);
      Pmsg2(-1, _("3991 result=\"%s\": ERR=%s\n"), results, be.strerror(status));
      goto bail_out;
   }
   if (loaded) {
      Pmsg1(-1, _("Slot %d loaded. I am going to unload it.\n"), loaded);
   } else {
      Pmsg0(-1, _("Nothing loaded in the drive. OK.\n"));
   }
   Dmsg1(100, "Results from loaded query=%s\n", results);
   if (loaded) {
      dcr->VolCatInfo.Slot = loaded;
      /* We are going to load a new tape, so close the device */
      dev->close();
      Pmsg2(-1, _("3302 Issuing autochanger \"unload %d %d\" command.\n"),
         loaded, dev->drive_index);
      changer = edit_device_codes(dcr, changer, 
                   dcr->device->changer_command, "unload");
      status = run_program(changer, timeout, results);
      Pmsg2(-1, _("unload status=%s %d\n"), status==0?_("OK"):_("Bad"), status);
      if (status != 0) {
         berrno be;
         Pmsg1(-1, _("3992 Bad autochanger command: %s\n"), changer);
         Pmsg2(-1, _("3992 result=\"%s\": ERR=%s\n"), results, be.strerror(status));
      }
   }

   /*
    * Load the Slot 1
    */

   slot = 1;
   dcr->VolCatInfo.Slot = slot;
   Pmsg2(-1, _("3303 Issuing autochanger \"load %d %d\" command.\n"),
      slot, dev->drive_index);
   changer = edit_device_codes(dcr, changer, 
                dcr->device->changer_command, "load");
   Dmsg1(100, "Changer=%s\n", changer);
   dev->close();
   status = run_program(changer, timeout, results);
   if (status == 0) {
      Pmsg2(-1,  _("3303 Autochanger \"load %d %d\" status is OK.\n"),
         slot, dev->drive_index);
   } else {
      berrno be;
      Pmsg1(-1, _("3993 Bad autochanger command: %s\n"), changer);
      Pmsg2(-1, _("3993 result=\"%s\": ERR=%s\n"), results, be.strerror(status));
      goto bail_out;
   }

   if (!open_the_device()) {
      goto bail_out;
   }
   /*
    * Start with sleep_time 0 then increment by 30 seconds if we get
    * a failure.
    */
   bmicrosleep(sleep_time, 0);
   if (!dev->rewind(dcr) || !dev->weof(1)) {
      Pmsg1(0, _("Bad status from rewind. ERR=%s\n"), dev->bstrerror());
      dev->clrerror(-1);
      Pmsg0(-1, _("\nThe test failed, probably because you need to put\n"
                "a longer sleep time in the mtx-script in the load) case.\n"
                "Adding a 30 second sleep and trying again ...\n"));
      sleep_time += 30;
      goto try_again;
   } else {
      Pmsg1(0, _("Rewound %s\n"), dev->print_name());
   }

   if (!dev->weof(1)) {
      Pmsg1(0, _("Bad status from weof. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   } else {
      Pmsg1(0, _("Wrote EOF to %s\n"), dev->print_name());
   }

   if (sleep_time) {
      Pmsg1(-1, _("\nThe test worked this time. Please add:\n\n"
                "   sleep %d\n\n"
                "to your mtx-changer script in the load) case.\n\n"),
                sleep_time);
   } else {
      Pmsg0(-1, _("\nThe test autochanger worked!!\n\n"));
   }

   free_pool_memory(changer);
   free_pool_memory(results);
   return 1;


bail_out:
   free_pool_memory(changer);
   free_pool_memory(results);
   Pmsg0(-1, _("You must correct this error or the Autochanger will not work.\n"));
   return -2;
}

static void autochangercmd()
{
   autochanger_test();
}


/*
 * This test assumes that the append test has been done,
 *   then it tests the fsf function.
 */
static int fsf_test()
{
   bool set_off = false;

   Pmsg0(-1, _("\n\n=== Forward space files test ===\n\n"
               "This test is essential to Bacula.\n\n"
               "I'm going to write five files then test forward spacing\n\n"));
   argc = 1;
   rewindcmd();
   wrcmd();
   weofcmd();      /* end file 0 */
   wrcmd();
   wrcmd();
   weofcmd();      /* end file 1 */
   wrcmd();
   wrcmd();
   wrcmd();
   weofcmd();     /* end file 2 */
   wrcmd();
   wrcmd();
   weofcmd();     /* end file 3 */
   wrcmd();
   weofcmd();     /* end file 4 */
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }

test_again:
   rewindcmd();
   Pmsg0(0, _("Now forward spacing 1 file.\n"));
   if (!dev->fsf(1)) {
      Pmsg1(0, _("Bad status from fsr. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg2(-1, _("We should be in file 1. I am at file %d. %s\n"),
      dev->file, dev->file == 1 ? _("This is correct!") : _("This is NOT correct!!!!"));

   if (dev->file != 1) {
      goto bail_out;
   }

   Pmsg0(0, _("Now forward spacing 2 files.\n"));
   if (!dev->fsf(2)) {
      Pmsg1(0, _("Bad status from fsr. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg2(-1, _("We should be in file 3. I am at file %d. %s\n"),
      dev->file, dev->file == 3 ? _("This is correct!") : _("This is NOT correct!!!!"));

   if (dev->file != 3) {
      goto bail_out;
   }

   rewindcmd();
   Pmsg0(0, _("Now forward spacing 4 files.\n"));
   if (!dev->fsf(4)) {
      Pmsg1(0, _("Bad status from fsr. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg2(-1, _("We should be in file 4. I am at file %d. %s\n"),
      dev->file, dev->file == 4 ? _("This is correct!") : _("This is NOT correct!!!!"));

   if (dev->file != 4) {
      goto bail_out;
   }
   if (set_off) {
      Pmsg0(-1, _("The test worked this time. Please add:\n\n"
                "   Fast Forward Space File = no\n\n"
                "to your Device resource for this drive.\n"));
   }

   Pmsg0(-1, "\n");
   Pmsg0(0, _("Now forward spacing 1 more file.\n"));
   if (!dev->fsf(1)) {
      Pmsg1(0, _("Bad status from fsr. ERR=%s\n"), dev->bstrerror());
   }
   Pmsg2(-1, _("We should be in file 5. I am at file %d. %s\n"),
      dev->file, dev->file == 5 ? _("This is correct!") : _("This is NOT correct!!!!"));
   if (dev->file != 5) {
      goto bail_out;
   }
   Pmsg0(-1, _("\n=== End Forward space files test ===\n\n"));
   return 1;

bail_out:
   Pmsg0(-1, _("\nThe forward space file test failed.\n"));
   if (dev_cap(dev, CAP_FASTFSF)) {
      Pmsg0(-1, _("You have Fast Forward Space File enabled.\n"
              "I am turning it off then retrying the test.\n"));
      dev->capabilities &= ~CAP_FASTFSF;
      set_off = true;
      goto test_again;
   }
   Pmsg0(-1, _("You must correct this error or Bacula will not work.\n"
            "Some systems, e.g. OpenBSD, require you to set\n"
            "   Use MTIOCGET= no\n"
            "in your device resource. Use with caution.\n"));
   return -2;
}





/*
 * This is a general test of Bacula's functions
 *   needed to read and write the tape.
 */
static void testcmd()
{
   int stat;

   if (!write_read_test()) {
      return;
   }
   if (!position_test()) {
      return;
   }

   stat = append_test();
   if (stat == 1) {                   /* OK get out */
      goto all_done;
   }
   if (stat == -1) {                  /* first test failed */
      if (dev_cap(dev, CAP_EOM) || dev_cap(dev, CAP_FASTFSF)) {
         Pmsg0(-1, _("\nAppend test failed. Attempting again.\n"
                   "Setting \"Hardware End of Medium = no\n"
                   "    and \"Fast Forward Space File = no\n"
                   "and retrying append test.\n\n"));
         dev->capabilities &= ~CAP_EOM; /* turn off eom */
         dev->capabilities &= ~CAP_FASTFSF; /* turn off fast fsf */
         stat = append_test();
         if (stat == 1) {
            Pmsg0(-1, _("\n\nIt looks like the test worked this time, please add:\n\n"
                     "    Hardware End of Medium = No\n\n"
                     "    Fast Forward Space File = No\n"
                     "to your Device resource in the Storage conf file.\n"));
            goto all_done;
         }
         if (stat == -1) {
            Pmsg0(-1, _("\n\nThat appears *NOT* to have corrected the problem.\n"));
            goto failed;
         }
         /* Wrong count after append */
         if (stat == -2) {
            Pmsg0(-1, _("\n\nIt looks like the append failed. Attempting again.\n"
                     "Setting \"BSF at EOM = yes\" and retrying append test.\n"));
            dev->capabilities |= CAP_BSFATEOM; /* backspace on eom */
            stat = append_test();
            if (stat == 1) {
               Pmsg0(-1, _("\n\nIt looks like the test worked this time, please add:\n\n"
                     "    Hardware End of Medium = No\n"
                     "    Fast Forward Space File = No\n"
                     "    BSF at EOM = yes\n\n"
                     "to your Device resource in the Storage conf file.\n"));
               goto all_done;
            }
         }

      }
failed:
      Pmsg0(-1, _("\nAppend test failed.\n\n"
            "\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
            "Unable to correct the problem. You MUST fix this\n"
             "problem before Bacula can use your tape drive correctly\n"
            "\nPerhaps running Bacula in fixed block mode will work.\n"
            "Do so by setting:\n\n"
            "Minimum Block Size = nnn\n"
            "Maximum Block Size = nnn\n\n"
            "in your Storage daemon's Device definition.\n"
            "nnn must match your tape driver's block size, which\n"
            "can be determined by reading your tape manufacturers\n"
            "information, and the information on your kernel dirver.\n"
            "Fixed block sizes, however, are not normally an ideal solution.\n"
            "\n"
            "Some systems, e.g. OpenBSD, require you to set\n"
            "   Use MTIOCGET= no\n"
            "in your device resource. Use with caution.\n"));
       return;
   }

all_done:
   Pmsg0(-1, _("\nThe above Bacula scan should have output identical to what follows.\n"
        "Please double check it ...\n"
        "=== Sample correct output ===\n"
        "1 block of 64448 bytes in file 1\n"
        "End of File mark.\n"
        "2 blocks of 64448 bytes in file 2\n"
        "End of File mark.\n"
        "3 blocks of 64448 bytes in file 3\n"
        "End of File mark.\n"
        "1 block of 64448 bytes in file 4\n"
        "End of File mark.\n"
        "Total files=4, blocks=7, bytes = 451,136\n"
        "=== End sample correct output ===\n\n"
        "If the above scan output is not identical to the\n"
        "sample output, you MUST correct the problem\n"
        "or Bacula will not be able to write multiple Jobs to \n"
        "the tape.\n\n"));

   if (stat == 1) {
      re_read_block_test();
   }

   fsf_test();                        /* do fast forward space file test */

   autochanger_test();                /* do autochanger test */

}

/* Forward space a file */
static void fsfcmd()
{
   int num = 1;
   if (argc > 1) {
      num = atoi(argk[1]);
   }
   if (num <= 0) {
      num = 1;
   }
   if (!dev->fsf(num)) {
      Pmsg1(0, _("Bad status from fsf. ERR=%s\n"), dev->bstrerror());
      return;
   }
   if (num == 1) {
      Pmsg0(0, _("Forward spaced 1 file.\n"));
   }
   else {
      Pmsg1(0, _("Forward spaced %d files.\n"), num);
   }
}

/* Forward space a record */
static void fsrcmd()
{
   int num = 1;
   if (argc > 1) {
      num = atoi(argk[1]);
   }
   if (num <= 0) {
      num = 1;
   }
   if (!dev->fsr(num)) {
      Pmsg1(0, _("Bad status from fsr. ERR=%s\n"), dev->bstrerror());
      return;
   }
   if (num == 1) {
      Pmsg0(0, _("Forward spaced 1 record.\n"));
   }
   else {
      Pmsg1(0, _("Forward spaced %d records.\n"), num);
   }
}


/*
 * Write a Bacula block to the tape
 */
static void wrcmd()
{
   DEV_BLOCK *block = dcr->block;
   DEV_RECORD *rec = dcr->rec;
   int i;

   sm_check(__FILE__, __LINE__, false);
   empty_block(block);
   if (verbose > 1) {
      dump_block(block, "test");
   }

   i = block->buf_len - 100;
   ASSERT (i > 0);
   rec->data = check_pool_memory_size(rec->data, i);
   memset(rec->data, i & 0xFF, i);
   rec->data_len = i;
   sm_check(__FILE__, __LINE__, false);
   if (!write_record_to_block(block, rec)) {
      Pmsg0(0, _("Error writing record to block.\n"));
      goto bail_out;
   }
   if (!write_block_to_dev(dcr)) {
      Pmsg0(0, _("Error writing block to device.\n"));
      goto bail_out;
   } else {
      Pmsg1(0, _("Wrote one record of %d bytes.\n"), i);
   }
   Pmsg0(0, _("Wrote block to device.\n"));

bail_out:
   sm_check(__FILE__, __LINE__, false);
   sm_check(__FILE__, __LINE__, false);
}

/*
 * Read a record from the tape
 */
static void rrcmd()
{
   char *buf;
   int stat, len;

   if (!get_cmd(_("Enter length to read: "))) {
      return;
   }
   len = atoi(cmd);
   if (len < 0 || len > 1000000) {
      Pmsg0(0, _("Bad length entered, using default of 1024 bytes.\n"));
      len = 1024;
   }
   buf = (char *)malloc(len);
   stat = read(dev->fd, buf, len);
   if (stat > 0 && stat <= len) {
      errno = 0;
   }
   berrno be;
   Pmsg3(0, _("Read of %d bytes gives stat=%d. ERR=%s\n"),
      len, stat, be.strerror());
   free(buf);
}


/*
 * Scan tape by reading block by block. Report what is
 * on the tape.  Note, this command does raw reads, and as such
 * will not work with fixed block size devices.
 */
static void scancmd()
{
   int stat;
   int blocks, tot_blocks, tot_files;
   int block_size;
   uint64_t bytes;
   char ec1[50];


   blocks = block_size = tot_blocks = 0;
   bytes = 0;
   if (dev->state & ST_EOT) {
      Pmsg0(0, _("End of tape\n"));
      return;
   }
   update_pos_dev(dev);
   tot_files = dev->file;
   Pmsg1(0, _("Starting scan at file %u\n"), dev->file);
   for (;;) {
      if ((stat = read(dev->fd, buf, sizeof(buf))) < 0) {
         berrno be;
         dev->clrerror(-1);
         Mmsg2(dev->errmsg, _("read error on %s. ERR=%s.\n"),
            dev->dev_name, be.strerror());
         Pmsg2(0, _("Bad status from read %d. ERR=%s\n"), stat, dev->bstrerror());
         if (blocks > 0) {
            if (blocks==1) {
               printf(_("1 block of %d bytes in file %d\n"), block_size, dev->file);
            }
            else {
               printf(_("%d blocks of %d bytes in file %d\n"), blocks, block_size, dev->file);
            }
         }
         return;
      }
      Dmsg1(200, "read status = %d\n", stat);
/*    sleep(1); */
      if (stat != block_size) {
         update_pos_dev(dev);
         if (blocks > 0) {
            if (blocks==1) {
               printf(_("1 block of %d bytes in file %d\n"), block_size, dev->file);
            }
            else {
               printf(_("%d blocks of %d bytes in file %d\n"), blocks, block_size, dev->file);
            }
            blocks = 0;
         }
         block_size = stat;
      }
      if (stat == 0) {                /* EOF */
         update_pos_dev(dev);
         printf(_("End of File mark.\n"));
         /* Two reads of zero means end of tape */
         if (dev->state & ST_EOF)
            dev->state |= ST_EOT;
         else {
            dev->state |= ST_EOF;
            dev->file++;
         }
         if (dev->state & ST_EOT) {
            printf(_("End of tape\n"));
            break;
         }
      } else {                        /* Got data */
         dev->state &= ~ST_EOF;
         blocks++;
         tot_blocks++;
         bytes += stat;
      }
   }
   update_pos_dev(dev);
   tot_files = dev->file - tot_files;
   printf(_("Total files=%d, blocks=%d, bytes = %s\n"), tot_files, tot_blocks,
      edit_uint64_with_commas(bytes, ec1));
}


/*
 * Scan tape by reading Bacula block by block. Report what is
 * on the tape.  This function reads Bacula blocks, so if your
 * Device resource is correctly defined, it should work with
 * either variable or fixed block sizes.
 */
static void scan_blocks()
{
   int blocks, tot_blocks, tot_files;
   uint32_t block_size;
   uint64_t bytes;
   DEV_BLOCK *block = dcr->block;
   char ec1[50];
   char buf1[100], buf2[100];

   blocks = block_size = tot_blocks = 0;
   bytes = 0;

   empty_block(block);
   update_pos_dev(dev);
   tot_files = dev->file;
   for (;;) {
      if (!read_block_from_device(dcr, NO_BLOCK_NUMBER_CHECK)) {
         Dmsg1(100, "!read_block(): ERR=%s\n", dev->bstrerror());
         if (dev->state & ST_EOT) {
            if (blocks > 0) {
               if (blocks==1) {
                  printf(_("1 block of %d bytes in file %d\n"), block_size, dev->file);
               }
               else {
                  printf(_("%d blocks of %d bytes in file %d\n"), blocks, block_size, dev->file);
               }
               blocks = 0;
            }
            goto bail_out;
         }
         if (dev->state & ST_EOF) {
            if (blocks > 0) {
               if (blocks==1) {
                  printf(_("1 block of %d bytes in file %d\n"), block_size, dev->file);
               }
               else {
                  printf(_("%d blocks of %d bytes in file %d\n"), blocks, block_size, dev->file);
               }
               blocks = 0;
            }
            printf(_("End of File mark.\n"));
            continue;
         }
         if (dev->state & ST_SHORT) {
            if (blocks > 0) {
               if (blocks==1) {
                  printf(_("1 block of %d bytes in file %d\n"), block_size, dev->file);
               }
               else {
                  printf(_("%d blocks of %d bytes in file %d\n"), blocks, block_size, dev->file);
               }
               blocks = 0;
            }
            printf(_("Short block read.\n"));
            continue;
         }
         printf(_("Error reading block. ERR=%s\n"), dev->bstrerror());
         goto bail_out;
      }
      if (block->block_len != block_size) {
         if (blocks > 0) {
            if (blocks==1) {
               printf(_("1 block of %d bytes in file %d\n"), block_size, dev->file);
            }
            else {
               printf(_("%d blocks of %d bytes in file %d\n"), blocks, block_size, dev->file);
            }
            blocks = 0;
         }
         block_size = block->block_len;
      }
      blocks++;
      tot_blocks++;
      bytes += block->block_len;
      Dmsg6(100, "Blk_blk=%u dev_blk=%u blen=%u bVer=%d SessId=%u SessTim=%u\n",
         block->BlockNumber, dev->block_num, block->block_len, block->BlockVer,
         block->VolSessionId, block->VolSessionTime);
      if (verbose == 1) {
         DEV_RECORD *rec = new_record();
         read_record_from_block(block, rec);
         Pmsg8(-1, _("Blk_block: %u dev_blk=%u blen=%u First rec FI=%s SessId=%u SessTim=%u Strm=%s rlen=%d\n"),
              block->BlockNumber, dev->block_num, block->block_len,
              FI_to_ascii(buf1, rec->FileIndex), rec->VolSessionId, rec->VolSessionTime,
              stream_to_ascii(buf2, rec->Stream, rec->FileIndex), rec->data_len);
         rec->remainder = 0;
         free_record(rec);
      } else if (verbose > 1) {
         dump_block(block, "");
      }

   }
bail_out:
   tot_files = dev->file - tot_files;
   printf(_("Total files=%d, blocks=%d, bytes = %s\n"), tot_files, tot_blocks,
      edit_uint64_with_commas(bytes, ec1));
}


static void statcmd()
{
   int debug = debug_level;
   debug_level = 30;
   Pmsg2(0, _("Device status: %u. ERR=%s\n"), status_dev(dev), dev->bstrerror());
#ifdef xxxx
   dump_volume_label(dev);
#endif
   debug_level = debug;
}


/*
 * First we label the tape, then we fill
 *  it with data get a new tape and write a few blocks.
 */
static void fillcmd()
{
   DEV_RECORD rec;
   DEV_BLOCK  *block = dcr->block;
   char ec1[50];
   char buf1[100], buf2[100];
   int fd;
   uint32_t i;
   uint32_t min_block_size;
   struct tm tm;

   ok = true;
   stop = 0;
   vol_num = 0;
   last_file = 0;
   last_block_num = 0;
   BlockNumber = 0;

   Pmsg0(-1, _("\n"
"This command simulates Bacula writing to a tape.\n"
"It requires either one or two blank tapes, which it\n"
"will label and write.\n\n"
"If you have an autochanger configured, it will use\n"
"the tapes that are in slots 1 and 2, otherwise, you will\n"
"be prompted to insert the tapes when necessary.\n\n"
"It will print a status approximately\n"
"every 322 MB, and write an EOF every 3.2 GB.  If you have\n"
"selected the simple test option, after writing the first tape\n"
"it will rewind it and re-read the last block written.\n\n"
"If you have selected the multiple tape test, when the first tape\n"
"fills, it will ask for a second, and after writing a few more \n"
"blocks, it will stop.  Then it will begin re-reading the\n"
"two tapes.\n\n"
"This may take a long time -- hours! ...\n\n"));

   get_cmd(_("Do you want to run the simplified test (s) with one tape\n"
           "or the complete multiple tape (m) test: (s/m) "));
   if (cmd[0] == 's') {
      Pmsg0(-1, _("Simple test (single tape) selected.\n"));
      simple = true;
   } else if (cmd[0] == 'm') {
      Pmsg0(-1, _("Multiple tape test selected.\n"));
      simple = false;
   } else {
      Pmsg0(000, _("Command aborted.\n"));
      return;
   }

   Dmsg1(20, "Begin append device=%s\n", dev->print_name());
   Dmsg1(20, "MaxVolSize=%s\n", edit_uint64(dev->max_volume_size, ec1));

   /* Use fixed block size to simplify read back */
   min_block_size = dev->min_block_size;
   dev->min_block_size = dev->max_block_size;
   set_volume_name("TestVolume1", 1);

   if (!dev->rewind(dcr)) {
      Pmsg0(000, _("Rewind failed.\n"));
   }
   if (!dev->weof(1)) {
      Pmsg0(000, _("Write EOF failed.\n"));
   }
   labelcmd();
   dev->set_append();                 /* force volume to be relabeled */

   /*
    * Acquire output device for writing.  Note, after acquiring a
    *   device, we MUST release it, which is done at the end of this
    *   subroutine.
    */
   Dmsg0(100, "just before acquire_device\n");
   if (!acquire_device_for_append(dcr)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return;
   }
   block = jcr->dcr->block;

   Dmsg0(100, "Just after acquire_device_for_append\n");
   /*
    * Write Begin Session Record
    */
   if (!write_session_label(dcr, SOS_LABEL)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      Jmsg1(jcr, M_FATAL, 0, _("Write session label failed. ERR=%s\n"),
         dev->bstrerror());
      ok = false;
   }
   Pmsg0(-1, _("Wrote Start of Session label.\n"));

   memset(&rec, 0, sizeof(rec));
   rec.data = get_memory(100000);     /* max record size */

#define REC_SIZE 32768
   rec.data_len = REC_SIZE;

   /*
    * Put some random data in the record
    */
   fd = open("/dev/urandom", O_RDONLY);
   if (fd) {
      read(fd, rec.data, rec.data_len);
      close(fd);
   } else {
      uint32_t *p = (uint32_t *)rec.data;
      srandom(time(NULL));
      for (i=0; i<rec.data_len/sizeof(uint32_t); i++) {
         p[i] = random();
      }
   }

   /*
    * Generate data as if from File daemon, write to device
    */
   jcr->dcr->VolFirstIndex = 0;
   time(&jcr->run_time);              /* start counting time for rates */
   (void)localtime_r(&jcr->run_time, &tm);
   strftime(buf1, sizeof(buf1), "%H:%M:%S", &tm);
   if (simple) {
      Pmsg1(-1, _("%s Begin writing Bacula records to tape ...\n"), buf1);
   } else {
      Pmsg1(-1, _("%s Begin writing Bacula records to first tape ...\n"), buf1);
   }
   for (file_index = 0; ok && !job_canceled(jcr); ) {
      rec.VolSessionId = jcr->VolSessionId;
      rec.VolSessionTime = jcr->VolSessionTime;
      rec.FileIndex = ++file_index;
      rec.Stream = STREAM_FILE_DATA;

      /* Mix up the data just a bit */
      uint32_t *lp = (uint32_t *)rec.data;
      lp[0] += lp[13];
      for (i=1; i < (rec.data_len-sizeof(uint32_t))/sizeof(uint32_t)-1; i++) {
         lp[i] += lp[i-1];
      }

      Dmsg4(250, "before write_rec FI=%d SessId=%d Strm=%s len=%d\n",
         rec.FileIndex, rec.VolSessionId, 
         stream_to_ascii(buf1, rec.Stream, rec.FileIndex),
         rec.data_len);

      while (!write_record_to_block(block, &rec)) {
         /*
          * When we get here we have just filled a block
          */
         Dmsg2(150, "!write_record_to_block data_len=%d rem=%d\n", rec.data_len,
                    rec.remainder);

         /* Write block to tape */
         if (!flush_block(block, 1)) {
            break;
         }

         /* Every 5000 blocks (approx 322MB) report where we are.
          */
         if ((block->BlockNumber % 5000) == 0) {
            now = time(NULL);
            now -= jcr->run_time;
            if (now <= 0) {
               now = 1;          /* prevent divide error */
            }
            kbs = (double)dev->VolCatInfo.VolCatBytes / (1000.0 * (double)now);
            Pmsg4(-1, _("Wrote blk_block=%u, dev_blk_num=%u VolBytes=%s rate=%.1f KB/s\n"),
               block->BlockNumber, dev->block_num,
               edit_uint64_with_commas(dev->VolCatInfo.VolCatBytes, ec1), (float)kbs);
         }
         /* Every 32000 blocks (approx 2GB) write an EOF.
          */
         if ((block->BlockNumber % 32000) == 0) {
            now = time(NULL);
            (void)localtime_r(&now, &tm);
            strftime(buf1, sizeof(buf1), "%H:%M:%S", &tm);
            Pmsg1(-1, _("%s Flush block, write EOF\n"), buf1);
            flush_block(block, 0);
            dev->weof(1);
         }

         /* Get out after writing 10 blocks to the second tape */
         if (++BlockNumber > 10 && stop != 0) {      /* get out */
            break;
         }
      }
      if (!ok) {
         Pmsg0(000, _("Not OK\n"));
         break;
      }
      jcr->JobBytes += rec.data_len;   /* increment bytes this job */
      Dmsg4(190, "write_record FI=%s SessId=%d Strm=%s len=%d\n",
         FI_to_ascii(buf1, rec.FileIndex), rec.VolSessionId,
         stream_to_ascii(buf2, rec.Stream, rec.FileIndex), rec.data_len);

      /* Get out after writing 10 blocks to the second tape */
      if (BlockNumber > 10 && stop != 0) {      /* get out */
         char ed1[50];
         Pmsg1(-1, "Done writing %s records ...\n", 
             edit_uint64_with_commas(write_count, ed1));
         break;
      }
   }
   if (vol_num > 1) {
      Dmsg0(100, "Write_end_session_label()\n");
      /* Create Job status for end of session label */
      if (!job_canceled(jcr) && ok) {
         set_jcr_job_status(jcr, JS_Terminated);
      } else if (!ok) {
         set_jcr_job_status(jcr, JS_ErrorTerminated);
      }
      if (!write_session_label(dcr, EOS_LABEL)) {
         Pmsg1(000, _("Error writting end session label. ERR=%s\n"), dev->bstrerror());
         ok = false;
      }
      /* Write out final block of this session */
      if (!write_block_to_device(dcr)) {
         Pmsg0(-1, _("Set ok=false after write_block_to_device.\n"));
         ok = false;
      }
      Pmsg0(-1, _("Wrote End of Session label.\n"));

      /* Save last block info for second tape */
      last_block_num2 = last_block_num;
      last_file2 = last_file;
      if (last_block2) {
         free_block(last_block2);
      }
      last_block2 = dup_block(last_block);
   }

   sprintf(buf, "%s/btape.state", working_directory);
   fd = open(buf, O_CREAT|O_TRUNC|O_WRONLY, 0640);
   if (fd >= 0) {
      write(fd, &btape_state_level, sizeof(btape_state_level));
      write(fd, &simple, sizeof(simple));
      write(fd, &last_block_num1, sizeof(last_block_num1));
      write(fd, &last_block_num2, sizeof(last_block_num2));
      write(fd, &last_file1, sizeof(last_file1));
      write(fd, &last_file2, sizeof(last_file2));
      write(fd, last_block1->buf, last_block1->buf_len);
      write(fd, last_block2->buf, last_block2->buf_len);
      write(fd, first_block->buf, first_block->buf_len);
      close(fd);
      Pmsg2(-1, _("Wrote state file last_block_num1=%d last_block_num2=%d\n"),
         last_block_num1, last_block_num2);
   } else {
      berrno be;
      Pmsg2(-1, _("Could not create state file: %s ERR=%s\n"), buf,
                 be.strerror());
   }

   now = time(NULL);
   (void)localtime_r(&now, &tm);
   strftime(buf1, sizeof(buf1), "%H:%M:%S", &tm);
   if (simple) {
      Pmsg3(-1, _("\n\n%s Done filling tape at %d:%d. Now beginning re-read of tape ...\n"),
         buf1, jcr->dcr->dev->file, jcr->dcr->dev->block_num);
   }
   else {
      Pmsg3(-1, _("\n\n%s Done filling tapes at %d:%d. Now beginning re-read of first tape ...\n"),
         buf1, jcr->dcr->dev->file, jcr->dcr->dev->block_num);
   }

   jcr->dcr->block = block;
   do_unfill();

   dev->min_block_size = min_block_size;
   free_memory(rec.data);
}

/*
 * Read two tapes written by the "fill" command and ensure
 *  that the data is valid.  If stop==1 we simulate full read back
 *  of two tapes.  If stop==-1 we simply read the last block and
 *  verify that it is correct.
 */
static void unfillcmd()
{
   int fd;

   last_block1 = new_block(dev);
   last_block2 = new_block(dev);
   first_block = new_block(dev);
   sprintf(buf, "%s/btape.state", working_directory);
   fd = open(buf, O_RDONLY);
   if (fd >= 0) {
      uint32_t state_level;
      read(fd, &state_level, sizeof(btape_state_level));
      read(fd, &simple, sizeof(simple));
      read(fd, &last_block_num1, sizeof(last_block_num1));
      read(fd, &last_block_num2, sizeof(last_block_num2));
      read(fd, &last_file1, sizeof(last_file1));
      read(fd, &last_file2, sizeof(last_file2));
      read(fd, last_block1->buf, last_block1->buf_len);
      read(fd, last_block2->buf, last_block2->buf_len);
      read(fd, first_block->buf, first_block->buf_len);
      close(fd);
      if (state_level != btape_state_level) {
          Pmsg0(-1, _("\nThe state file level has changed. You must redo\n"
                  "the fill command.\n"));
          return;
       }
   } else {
      berrno be;
      Pmsg2(-1, _("\nCould not find the state file: %s ERR=%s\n"
             "You must redo the fill command.\n"), buf, be.strerror());
      return;
   }
   do_unfill();
   this_block = NULL;
}

static void do_unfill()
{
   DEV_BLOCK *block = dcr->block;
   bool autochanger;

   dumped = 0;
   VolBytes = 0;
   LastBlock = 0;

   Dmsg0(20, "Enter do_unfill\n");
   dev->capabilities |= CAP_ANONVOLS; /* allow reading any volume */
   dev->capabilities &= ~CAP_LABEL;   /* don't label anything here */

   end_of_tape = 0;

   time(&jcr->run_time);              /* start counting time for rates */
   stop = 0;
   file_index = 0;
   if (last_block) {
      free_block(last_block);
      last_block = NULL;
   }
   last_block_num = last_block_num1;
   last_file = last_file1;
   last_block = last_block1;

   if (!simple) {
      /* Multiple Volume tape */
      /* Close device so user can use autochanger if desired */
      if (dev_cap(dev, CAP_OFFLINEUNMOUNT)) {
         dev->offline();
      }
      autochanger = autoload_device(dcr, 1, NULL);
      if (!autochanger) {
         dev->close();
         get_cmd(_("Mount first tape. Press enter when ready: "));
      }
   }

   free_restore_volume_list(jcr);
   jcr->dcr = new_dcr(jcr, dev);
   set_volume_name("TestVolume1", 1);
   jcr->bsr = NULL;
   create_restore_volume_list(jcr);
   dev->close();
   dev->num_writers = 0;
   if (!acquire_device_for_read(dcr)) {
      Pmsg1(-1, "%s", dev->errmsg);
      goto bail_out;
   }
   /*
    * We now have the first tape mounted.
    * Note, re-reading last block may have caused us to
    *   loose track of where we are (block number unknown).
    */
   Pmsg0(-1, _("Rewinding.\n"));
   if (!dev->rewind(dcr)) {                /* get to a known place on tape */
      goto bail_out;
   }
   /* Read the first 10000 records */
   Pmsg2(-1, _("Reading the first 10000 records from %u:%u.\n"),
      dev->file, dev->block_num);
   quickie_count = 0;
   read_records(dcr, quickie_cb, my_mount_next_read_volume);
   Pmsg4(-1, _("Reposition from %u:%u to %u:%u\n"), dev->file, dev->block_num,
         last_file, last_block_num);
   if (!dev->reposition(last_file, last_block_num)) {
      Pmsg1(-1, _("Reposition error. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg1(-1, _("Reading block %u.\n"), last_block_num);
   if (!read_block_from_device(dcr, NO_BLOCK_NUMBER_CHECK)) {
      Pmsg1(-1, _("Error reading block: ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   if (compare_blocks(last_block, block)) {
      if (simple) {
         Pmsg0(-1, _("\nThe last block on the tape matches. Test succeeded.\n\n"));
      } else {
         Pmsg0(-1, _("\nThe last block of the first tape matches.\n\n"));
      }
   }
   if (simple) {
      goto bail_out;
   }

   /* restore info for last block on second Volume */
   last_block_num = last_block_num2;
   last_file = last_file2;
   last_block = last_block2;

   /* Multiple Volume tape */
   /* Close device so user can use autochanger if desired */
   if (dev_cap(dev, CAP_OFFLINEUNMOUNT)) {
      dev->offline();
   }

   free_restore_volume_list(jcr);
   set_volume_name("TestVolume2", 2);
   jcr->bsr = NULL;
   create_restore_volume_list(jcr);
   autochanger = autoload_device(dcr, 1, NULL);
   if (!autochanger) {
      dev->close();
      get_cmd(_("Mount second tape. Press enter when ready: "));
   }

   dev->clear_read();
   if (!acquire_device_for_read(dcr)) {
      Pmsg1(-1, "%s", dev->errmsg);
      goto bail_out;
   }

   /* Space to "first" block which is last block not written
    * on the previous tape.
    */
   Pmsg2(-1, _("Reposition from %u:%u to 0:1\n"), dev->file, dev->block_num);
   if (!dev->reposition(0, 1)) {
      Pmsg1(-1, _("Reposition error. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg1(-1, _("Reading block %d.\n"), dev->block_num);
   if (!read_block_from_device(dcr, NO_BLOCK_NUMBER_CHECK)) {
      Pmsg1(-1, _("Error reading block: ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   if (compare_blocks(first_block, block)) {
      Pmsg0(-1, _("\nThe first block on the second tape matches.\n\n"));
   }

   /* Now find and compare the last block */
   Pmsg4(-1, _("Reposition from %u:%u to %u:%u\n"), dev->file, dev->block_num,
         last_file, last_block_num);
   if (!dev->reposition(last_file, last_block_num)) {
      Pmsg1(-1, _("Reposition error. ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   Pmsg1(-1, _("Reading block %d.\n"), dev->block_num);
   if (!read_block_from_device(dcr, NO_BLOCK_NUMBER_CHECK)) {
      Pmsg1(-1, _("Error reading block: ERR=%s\n"), dev->bstrerror());
      goto bail_out;
   }
   if (compare_blocks(last_block, block)) {
      Pmsg0(-1, _("\nThe last block on the second tape matches. Test succeeded.\n\n"));
   }

bail_out:
   free_block(last_block1);
   free_block(last_block2);
   free_block(first_block);
}

/* Read 10000 records then stop */
static bool quickie_cb(DCR *dcr, DEV_RECORD *rec)
{
   DEVICE *dev = dcr->dev;
   quickie_count++;
   if (quickie_count == 10000) {
      Pmsg2(-1, _("10000 records read now at %d:%d\n"), dev->file, dev->block_num);
   }
   return quickie_count < 10000;
}

static bool compare_blocks(DEV_BLOCK *last_block, DEV_BLOCK *block)
{
   char *p, *q;
   uint32_t CheckSum, block_len;
   ser_declare;

   p = last_block->buf;
   q = block->buf;
   unser_begin(q, BLKHDR2_LENGTH);
   unser_uint32(CheckSum);
   unser_uint32(block_len);
   while (q < (block->buf+block_len)) {
      if (*p == *q) {
         p++;
         q++;
         continue;
      }
      Pmsg0(-1, "\n");
      dump_block(last_block, _("Last block written"));
      Pmsg0(-1, "\n");
      dump_block(block, _("Block read back"));
      Pmsg1(-1, _("\n\nThe blocks differ at byte %u\n"), p - last_block->buf);
      Pmsg0(-1, _("\n\n!!!! The last block written and the block\n"
                "that was read back differ. The test FAILED !!!!\n"
                "This must be corrected before you use Bacula\n"
                "to write multi-tape Volumes.!!!!\n"));
      return false;
   }
   if (verbose) {
      dump_block(last_block, _("Last block written"));
      dump_block(block, _("Block read back"));
   }
   return true;
}





/*
 * Write current block to tape regardless of whether or
 *   not it is full. If the tape fills, attempt to
 *   acquire another tape.
 */
static int flush_block(DEV_BLOCK *block, int dump)
{
   char ec1[50];
   DEV_BLOCK *tblock;
   uint32_t this_file, this_block_num;

   lock_device(dev);
   if (!this_block) {
      this_block = new_block(dev);
   }
   if (!last_block) {
      last_block = new_block(dev);
   }
   /* Copy block */
   this_file = dev->file;
   this_block_num = dev->block_num;
   if (!write_block_to_dev(dcr)) {
      Pmsg3(000, _("Last block at: %u:%u this_dev_block_num=%d\n"),
                  last_file, last_block_num, this_block_num);
      if (vol_num == 1) {
         /*
          * This is 1st tape, so save first tape info separate
          *  from second tape info
          */
         last_block_num1 = last_block_num;
         last_file1 = last_file;
         last_block1 = dup_block(last_block);
         last_block2 = dup_block(last_block);
         first_block = dup_block(block); /* first block second tape */
      }
      if (verbose) {
         Pmsg3(000, _("Block not written: FileIndex=%u blk_block=%u Size=%u\n"),
            (unsigned)file_index, block->BlockNumber, block->block_len);
         dump_block(last_block, _("Last block written"));
         Pmsg0(-1, "\n");
         dump_block(block, _("Block not written"));
      }
      if (stop == 0) {
         eot_block = block->BlockNumber;
         eot_block_len = block->block_len;
         eot_FileIndex = file_index;
         stop = 1;
      }
      now = time(NULL);
      now -= jcr->run_time;
      if (now <= 0) {
         now = 1;                     /* don't divide by zero */
      }
      kbs = (double)dev->VolCatInfo.VolCatBytes / (1000 * now);
      vol_size = dev->VolCatInfo.VolCatBytes;
      Pmsg4(000, _("End of tape %d:%d. VolumeCapacity=%s. Write rate = %.1f KB/s\n"),
         dev->file, dev->block_num,
         edit_uint64_with_commas(dev->VolCatInfo.VolCatBytes, ec1), kbs);

      if (simple) {
         stop = -1;                   /* stop, but do simplified test */
      } else {
         /* Full test in progress */
         if (!fixup_device_block_write_error(jcr->dcr)) {
            Pmsg1(000, _("Cannot fixup device error. %s\n"), dev->bstrerror());
            ok = false;
            unlock_device(dev);
            return 0;
         }
         BlockNumber = 0;             /* start counting for second tape */
      }
      unlock_device(dev);
      return 1;                       /* end of tape reached */
   }

   /* Save contents after write so that the header is serialized */
   memcpy(this_block->buf, block->buf, this_block->buf_len);

   /*
    * Note, we always read/write to block, but we toggle
    *  copying it to one or another of two allocated blocks.
    * Switch blocks so that the block just successfully written is
    *  always in last_block.
    */
   tblock = last_block;
   last_block = this_block;
   this_block = tblock;
   last_file = this_file;
   last_block_num = this_block_num;

   unlock_device(dev);
   return 1;
}


/*
 * First we label the tape, then we fill
 *  it with data get a new tape and write a few blocks.
 */
static void qfillcmd()
{
   DEV_BLOCK *block = dcr->block;
   DEV_RECORD *rec = dcr->rec;
   int i, count;

   Pmsg0(0, _("Test writing blocks of 64512 bytes to tape.\n"));

   get_cmd(_("How many blocks do you want to write? (1000): "));

   count = atoi(cmd);
   if (count <= 0) {
      count = 1000;
   }

   sm_check(__FILE__, __LINE__, false);

   i = block->buf_len - 100;
   ASSERT (i > 0);
   rec->data = check_pool_memory_size(rec->data, i);
   memset(rec->data, i & 0xFF, i);
   rec->data_len = i;
   rewindcmd();
   Pmsg1(0, _("Begin writing %d Bacula blocks to tape ...\n"), count);
   for (i=0; i < count; i++) {
      if (i % 100 == 0) {
         printf("+");
         fflush(stdout);
      }
      if (!write_record_to_block(block, rec)) {
         Pmsg0(0, _("Error writing record to block.\n"));
         goto bail_out;
      }
      if (!write_block_to_dev(dcr)) {
         Pmsg0(0, _("Error writing block to device.\n"));
         goto bail_out;
      }
   }
   printf("\n");
   weofcmd();
   if (dev_cap(dev, CAP_TWOEOF)) {
      weofcmd();
   }
   rewindcmd();
   scan_blocks();

bail_out:
   sm_check(__FILE__, __LINE__, false);
}

/*
 * Fill a tape using raw write() command
 */
static void rawfill_cmd()
{
   DEV_BLOCK *block = dcr->block;
   int stat;
   int fd;
   uint32_t block_num = 0;
   uint32_t *p;
   int my_errno;
   uint32_t i;

   fd = open("/dev/urandom", O_RDONLY);
   if (fd) {
      read(fd, block->buf, block->buf_len);
      close(fd);
   } else {
      uint32_t *p = (uint32_t *)block->buf;
      srandom(time(NULL));
      for (i=0; i<block->buf_len/sizeof(uint32_t); i++) {
         p[i] = random();
      }
   }
   p = (uint32_t *)block->buf;
   Pmsg1(0, _("Begin writing raw blocks of %u bytes.\n"), block->buf_len);
   for ( ;; ) {
      *p = block_num;
      if (dev->is_tape()) {
         stat = tape_write(dev->fd, block->buf, block->buf_len);
      } else {
         stat = write(dev->fd, block->buf, block->buf_len);
      }
      if (stat == (int)block->buf_len) {
         if ((block_num++ % 100) == 0) {
            printf("+");
            fflush(stdout);
         }
         p[0] += p[13];
         for (i=1; i<(block->buf_len-sizeof(uint32_t))/sizeof(uint32_t)-1; i++) {
            p[i] += p[i-1];
         }
         continue;
      }
      break;
   }
   my_errno = errno;
   printf("\n");
   berrno be;
   printf(_("Write failed at block %u. stat=%d ERR=%s\n"), block_num, stat,
      be.strerror(my_errno));
   weofcmd();
}


/*
 * Fill a tape using Bacula block writes
 */
static void bfill_cmd()
{
   DEV_BLOCK *block = dcr->block;
   uint32_t block_num = 0;
   uint32_t *p;
   int my_errno;
   int fd;
   uint32_t i;

   fd = open("/dev/urandom", O_RDONLY);
   if (fd) {
      read(fd, block->buf, block->buf_len);
      close(fd);
   } else {
      uint32_t *p = (uint32_t *)block->buf;
      srandom(time(NULL));
      for (i=0; i<block->buf_len/sizeof(uint32_t); i++) {
         p[i] = random();
      }
   }
   p = (uint32_t *)block->buf;
   Pmsg1(0, _("Begin writing Bacula blocks of %u bytes.\n"), block->buf_len);
   for ( ;; ) {
      *p = block_num;
      block->binbuf = block->buf_len;
      block->bufp = block->buf + block->binbuf;
      if (!write_block_to_dev(dcr)) {
         break;
      }
      if ((block_num++ % 100) == 0) {
         printf("+");
         fflush(stdout);
      }
      p[0] += p[13];
      for (i=1; i<(block->buf_len/sizeof(uint32_t)-1); i++) {
         p[i] += p[i-1];
      }
   }
   my_errno = errno;
   printf("\n");
   printf(_("Write failed at block %u.\n"), block_num);
   weofcmd();
}


struct cmdstruct { const char *key; void (*func)(); const char *help; };
static struct cmdstruct commands[] = {
 {NT_("autochanger"),autochangercmd, _("test autochanger")},
 {NT_("bsf"),       bsfcmd,       _("backspace file")},
 {NT_("bsr"),       bsrcmd,       _("backspace record")},
 {NT_("bfill"),     bfill_cmd,    _("fill tape using Bacula writes")},
 {NT_("cap"),       capcmd,       _("list device capabilities")},
 {NT_("clear"),     clearcmd,     _("clear tape errors")},
 {NT_("eod"),       eodcmd,       _("go to end of Bacula data for append")},
 {NT_("eom"),       eomcmd,       _("go to the physical end of medium")},
 {NT_("fill"),      fillcmd,      _("fill tape, write onto second volume")},
 {NT_("unfill"),    unfillcmd,    _("read filled tape")},
 {NT_("fsf"),       fsfcmd,       _("forward space a file")},
 {NT_("fsr"),       fsrcmd,       _("forward space a record")},
 {NT_("help"),      helpcmd,      _("print this command")},
 {NT_("label"),     labelcmd,     _("write a Bacula label to the tape")},
 {NT_("load"),      loadcmd,      _("load a tape")},
 {NT_("quit"),      quitcmd,      _("quit btape")},
 {NT_("rawfill"),   rawfill_cmd,  _("use write() to fill tape")},
 {NT_("readlabel"), readlabelcmd, _("read and print the Bacula tape label")},
 {NT_("rectest"),   rectestcmd,   _("test record handling functions")},
 {NT_("rewind"),    rewindcmd,    _("rewind the tape")},
 {NT_("scan"),      scancmd,      _("read() tape block by block to EOT and report")},
 {NT_("scanblocks"),scan_blocks,  _("Bacula read block by block to EOT and report")},
 {NT_("status"),    statcmd,      _("print tape status")},
 {NT_("test"),      testcmd,      _("General test Bacula tape functions")},
 {NT_("weof"),      weofcmd,      _("write an EOF on the tape")},
 {NT_("wr"),        wrcmd,        _("write a single Bacula block")},
 {NT_("rr"),        rrcmd,        _("read a single record")},
 {NT_("qfill"),     qfillcmd,     _("quick fill command")}
             };
#define comsize (sizeof(commands)/sizeof(struct cmdstruct))

static void
do_tape_cmds()
{
   unsigned int i;
   bool found;

   while (!quit && get_cmd("*")) {
      sm_check(__FILE__, __LINE__, false);
      found = false;
      parse_args(cmd, &args, &argc, argk, argv, MAX_CMD_ARGS);
      for (i=0; i<comsize; i++)       /* search for command */
         if (argc > 0 && fstrsch(argk[0],  commands[i].key)) {
            (*commands[i].func)();    /* go execute command */
            found = true;
            break;
         }
      if (!found) {
         Pmsg1(0, _("\"%s\" is an illegal command\n"), cmd);
      }
   }
}

static void helpcmd()
{
   unsigned int i;
   usage();
   printf(_("Interactive commands:\n"));
   printf(_("  Command    Description\n  =======    ===========\n"));
   for (i=0; i<comsize; i++)
      printf("  %-10s %s\n", commands[i].key, commands[i].help);
   printf("\n");
}

static void usage()
{
   fprintf(stderr, _(
"Copyright (C) 2000-%s Kern Sibbald.\n"
"\nVersion: %s (%s)\n\n"
"Usage: btape <options> <device_name>\n"
"       -b <file>   specify bootstrap file\n"
"       -c <file>   set configuration file to file\n"
"       -d <nn>     set debug level to nn\n"
"       -p          proceed inspite of I/O errors\n"
"       -s          turn off signals\n"
"       -v          be verbose\n"
"       -?          print this message.\n"
"\n"), BYEAR, VERSION, BDATE);

}

/*
 * Get next input command from terminal.  This
 * routine is REALLY primitive, and should be enhanced
 * to have correct backspacing, etc.
 */
int
get_cmd(const char *prompt)
{
   int i = 0;
   int ch;
   fprintf(stdout, prompt);

   /* We really should turn off echoing and pretty this
    * up a bit.
    */
   cmd[i] = 0;
   while ((ch = fgetc(stdin)) != EOF) {
      if (ch == '\n') {
         strip_trailing_junk(cmd);
         return 1;
      } else if (ch == 4 || ch == 0xd3 || ch == 0x8) {
         if (i > 0)
            cmd[--i] = 0;
         continue;
      }

      cmd[i++] = ch;
      cmd[i] = 0;
   }
   quit = 1;
   return 0;
}

/* Dummies to replace askdir.c */
bool    dir_update_file_attributes(DCR *dcr, DEV_RECORD *rec) { return 1;}
bool    dir_send_job_status(JCR *jcr) {return 1;}

bool dir_update_volume_info(DCR *dcr, bool relabel)
{
   return 1;
}


bool dir_get_volume_info(DCR *dcr, enum get_vol_info_rw  writing)
{
   Dmsg0(20, "Enter dir_get_volume_info\n");
   bstrncpy(dcr->VolCatInfo.VolCatName, dcr->VolumeName, sizeof(dcr->VolCatInfo.VolCatName));
   return 1;
}

bool dir_create_jobmedia_record(DCR *dcr)
{
   dcr->WroteVol = false;
   return 1;
}


bool dir_find_next_appendable_volume(DCR *dcr)
{
   Dmsg1(20, "Enter dir_find_next_appendable_volume. stop=%d\n", stop);
   return dcr->VolumeName[0] != 0;
}

bool dir_ask_sysop_to_mount_volume(DCR *dcr)
{
   DEVICE *dev = dcr->dev;
   Dmsg0(20, "Enter dir_ask_sysop_to_mount_volume\n");
   if (dcr->VolumeName[0] == 0) {
      return dir_ask_sysop_to_create_appendable_volume(dcr);
   }
   dev->close();
   Pmsg1(-1, "%s", dev->errmsg);           /* print reason */
   if (dcr->VolumeName[0] == 0 || strcmp(dcr->VolumeName, "TestVolume2") == 0) {
      fprintf(stderr, _("Mount second Volume on device %s and press return when ready: "),
         dev->print_name());
   } else {
      fprintf(stderr, _("Mount Volume \"%s\" on device %s and press return when ready: "),
         dcr->VolumeName, dev->print_name());
   }
   getchar();
   return true;
}

bool dir_ask_sysop_to_create_appendable_volume(DCR *dcr)
{
   bool autochanger;
   DEVICE *dev = dcr->dev;
   Dmsg0(20, "Enter dir_ask_sysop_to_create_appendable_volume\n");
   if (stop == 0) {
      set_volume_name("TestVolume1", 1);
   } else {
      set_volume_name("TestVolume2", 2);
   }
   /* Close device so user can use autochanger if desired */
   if (dev_cap(dev, CAP_OFFLINEUNMOUNT)) {
      dev->offline();
   }
   autochanger = autoload_device(dcr, 1, NULL);
   if (!autochanger) {
      dev->close();
      fprintf(stderr, _("Mount blank Volume on device %s and press return when ready: "),
         dev->print_name());
      getchar();
   }
   open_device(dcr);
   labelcmd();
   VolumeName = NULL;
   BlockNumber = 0;
   return true;
}

static bool my_mount_next_read_volume(DCR *dcr)
{
   char ec1[50];
   JCR *jcr = dcr->jcr;
   DEV_BLOCK *block = dcr->block;

   Dmsg0(20, "Enter my_mount_next_read_volume\n");
   Pmsg2(000, _("End of Volume \"%s\" %d records.\n"), dcr->VolumeName,
      quickie_count);

   if (LastBlock != block->BlockNumber) {
      VolBytes += block->block_len;
   }
   LastBlock = block->BlockNumber;
   now = time(NULL);
   now -= jcr->run_time;
   if (now <= 0) {
      now = 1;
   }
   kbs = (double)VolBytes / (1000.0 * (double)now);
   Pmsg3(-1, _("Read block=%u, VolBytes=%s rate=%.1f KB/s\n"), block->BlockNumber,
            edit_uint64_with_commas(VolBytes, ec1), (float)kbs);

   if (strcmp(dcr->VolumeName, "TestVolume2") == 0) {
      end_of_tape = 1;
      return false;
   }

   free_restore_volume_list(jcr);
   set_volume_name("TestVolume2", 2);
   jcr->bsr = NULL;
   create_restore_volume_list(jcr);
   dev->close();
   if (!acquire_device_for_read(dcr)) {
      Pmsg2(0, _("Cannot open Dev=%s, Vol=%s\n"), dev->print_name(), dcr->VolumeName);
      return false;
   }
   return true;                    /* next volume mounted */
}

static void set_volume_name(const char *VolName, int volnum)
{
   DCR *dcr = jcr->dcr;
   VolumeName = VolName;
   vol_num = volnum;
   bstrncpy(dev->VolCatInfo.VolCatName, VolName, sizeof(dev->VolCatInfo.VolCatName));
   bstrncpy(dcr->VolCatInfo.VolCatName, VolName, sizeof(dcr->VolCatInfo.VolCatName));
   bstrncpy(dcr->VolumeName, VolName, sizeof(dcr->VolumeName));
   dcr->VolCatInfo.Slot = volnum;
}
