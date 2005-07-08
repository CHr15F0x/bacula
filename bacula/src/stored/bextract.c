/*
 *
 *  Dumb program to extract files from a Bacula backup.
 *
 *   Kern E. Sibbald, MM
 *
 *   Version $Id$
 *
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

#include "bacula.h"
#include "stored.h"
#include "findlib/find.h"

#if defined(HAVE_CYGWIN) || defined(HAVE_WIN32)
int win32_client = 1;
#else
int win32_client = 0;
#endif

static void do_extract(char *fname);
static bool record_cb(DCR *dcr, DEV_RECORD *rec);

static DEVICE *dev = NULL;
static DCR *dcr;
static BFILE bfd;
static JCR *jcr;
static FF_PKT *ff;
static BSR *bsr = NULL;
static bool extract = false;
static int non_support_data = 0;
static long total = 0;
static ATTR *attr;
static char *where;
static uint32_t num_files = 0;
static uint32_t compress_buf_size = 70000;
static POOLMEM *compress_buf;
static int prog_name_msg = 0;
static int win32_data_msg = 0;
static char *VolumeName = NULL;

static char *wbuf;                    /* write buffer address */
static uint32_t wsize;                /* write size */
static uint64_t fileAddr = 0;         /* file write address */

#define CONFIG_FILE "bacula-sd.conf"
char *configfile = NULL;
STORES *me = NULL;                    /* our Global resource */
bool forge_on = false;
pthread_mutex_t device_release_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wait_device_release = PTHREAD_COND_INITIALIZER;

static void usage()
{
   fprintf(stderr,
"Copyright (C) 2000-2005 Kern Sibbald.\n"
"\nVersion: " VERSION " (" BDATE ")\n\n"
"Usage: bextract <options> <bacula-archive-device-name> <directory-to-store-files>\n"
"       -b <file>       specify a bootstrap file\n"
"       -c <file>       specify a configuration file\n"
"       -d <nn>         set debug level to nn\n"
"       -e <file>       exclude list\n"
"       -i <file>       include list\n"
"       -p              proceed inspite of I/O errors\n"
"       -v              verbose\n"
"       -V <volumes>    specify Volume names (separated by |)\n"
"       -?              print this message\n\n");
   exit(1);
}


int main (int argc, char *argv[])
{
   int ch;
   FILE *fd;
   char line[1000];
   bool got_inc = false;

   working_directory = "/tmp";
   my_name_is(argc, argv, "bextract");
   init_msg(NULL, NULL);              /* setup message handler */

   ff = init_find_files();
   binit(&bfd);

   while ((ch = getopt(argc, argv, "b:c:d:e:i:pvV:?")) != -1) {
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

      case 'd':                    /* debug level */
         debug_level = atoi(optarg);
         if (debug_level <= 0)
            debug_level = 1;
         break;

      case 'e':                    /* exclude list */
         if ((fd = fopen(optarg, "r")) == NULL) {
            berrno be;
            Pmsg2(0, "Could not open exclude file: %s, ERR=%s\n",
               optarg, be.strerror());
            exit(1);
         }
         while (fgets(line, sizeof(line), fd) != NULL) {
            strip_trailing_junk(line);
            Dmsg1(900, "add_exclude %s\n", line);
            add_fname_to_exclude_list(ff, line);
         }
         fclose(fd);
         break;

      case 'i':                    /* include list */
         if ((fd = fopen(optarg, "r")) == NULL) {
            berrno be;
            Pmsg2(0, "Could not open include file: %s, ERR=%s\n",
               optarg, be.strerror());
            exit(1);
         }
         while (fgets(line, sizeof(line), fd) != NULL) {
            strip_trailing_junk(line);
            Dmsg1(900, "add_include %s\n", line);
            add_fname_to_include_list(ff, 0, line);
         }
         fclose(fd);
         got_inc = true;
         break;

      case 'p':
         forge_on = true;
         break;

      case 'v':
         verbose++;
         break;

      case 'V':                    /* Volume name */
         VolumeName = optarg;
         break;

      case '?':
      default:
         usage();

      } /* end switch */
   } /* end while */
   argc -= optind;
   argv += optind;

   if (argc != 2) {
      Pmsg0(0, "Wrong number of arguments: \n");
      usage();
   }

   if (configfile == NULL) {
      configfile = bstrdup(CONFIG_FILE);
   }

   parse_config(configfile);

   if (!got_inc) {                            /* If no include file, */
      add_fname_to_include_list(ff, 0, "/");  /*   include everything */
   }

   where = argv[1];
   do_extract(argv[0]);

   if (bsr) {
      free_bsr(bsr);
   }
   if (prog_name_msg) {
      Pmsg1(000, "%d Program Name and/or Program Data Stream records ignored.\n",
         prog_name_msg);
   }
   if (win32_data_msg) {
      Pmsg1(000, "%d Win32 data or Win32 gzip data stream records. Ignored.\n",
         win32_data_msg);
   }
   term_include_exclude_files(ff);
   term_find_files(ff);
   return 0;
}

static void do_extract(char *devname)
{
   struct stat statp;
   jcr = setup_jcr("bextract", devname, bsr, VolumeName, 1); /* acquire for read */
   if (!jcr) {
      exit(1);
   }
   dev = jcr->dcr->dev;
   if (!dev) {
      exit(1);
   }
   dcr = jcr->dcr;

   /* Make sure where directory exists and that it is a directory */
   if (stat(where, &statp) < 0) {
      berrno be;
      Emsg2(M_ERROR_TERM, 0, "Cannot stat %s. It must exist. ERR=%s\n",
         where, be.strerror());
   }
   if (!S_ISDIR(statp.st_mode)) {
      Emsg1(M_ERROR_TERM, 0, "%s must be a directory.\n", where);
   }

   free(jcr->where);
   jcr->where = bstrdup(where);
   attr = new_attr();

   compress_buf = get_memory(compress_buf_size);

   read_records(dcr, record_cb, mount_next_read_volume);
   /* If output file is still open, it was the last one in the
    * archive since we just hit an end of file, so close the file.
    */
   if (is_bopen(&bfd)) {
      set_attributes(jcr, attr, &bfd);
   }
   release_device(dcr);
   free_attr(attr);
   free_jcr(jcr);
   term_dev(dev);

   printf("%u files restored.\n", num_files);
   return;
}

/*
 * Called here for each record from read_records()
 */
static bool record_cb(DCR *dcr, DEV_RECORD *rec)
{
   int stat;
   JCR *jcr = dcr->jcr;

   if (rec->FileIndex < 0) {
      return true;                    /* we don't want labels */
   }

   /* File Attributes stream */

   switch (rec->Stream) {
   case STREAM_UNIX_ATTRIBUTES:
   case STREAM_UNIX_ATTRIBUTES_EX:

      /* If extracting, it was from previous stream, so
       * close the output file.
       */
      if (extract) {
         if (!is_bopen(&bfd)) {
            Emsg0(M_ERROR, 0, _("Logic error output file should be open but is not.\n"));
         }
         set_attributes(jcr, attr, &bfd);
         extract = false;
      }

      if (!unpack_attributes_record(jcr, rec->Stream, rec->data, attr)) {
         Emsg0(M_ERROR_TERM, 0, _("Cannot continue.\n"));
      }

      if (attr->file_index != rec->FileIndex) {
         Emsg2(M_ERROR_TERM, 0, _("Record header file index %ld not equal record index %ld\n"),
            rec->FileIndex, attr->file_index);
      }

      if (file_is_included(ff, attr->fname) && !file_is_excluded(ff, attr->fname)) {

         attr->data_stream = decode_stat(attr->attr, &attr->statp, &attr->LinkFI);
         if (!is_stream_supported(attr->data_stream)) {
            if (!non_support_data++) {
               Jmsg(jcr, M_ERROR, 0, _("%s stream not supported on this Client.\n"),
                  stream_to_ascii(attr->data_stream));
            }
            extract = false;
            return true;
         }


         build_attr_output_fnames(jcr, attr);

         extract = false;
         stat = create_file(jcr, attr, &bfd, REPLACE_ALWAYS);
         switch (stat) {
         case CF_ERROR:
         case CF_SKIP:
            break;
         case CF_EXTRACT:
            extract = true;
            print_ls_output(jcr, attr);
            num_files++;
            fileAddr = 0;
            break;
         case CF_CREATED:
            set_attributes(jcr, attr, &bfd);
            print_ls_output(jcr, attr);
            num_files++;
            fileAddr = 0;
            break;
         }
      }
      break;

   /* Data stream and extracting */
   case STREAM_FILE_DATA:
   case STREAM_SPARSE_DATA:
   case STREAM_WIN32_DATA:

      if (extract) {
         if (rec->Stream == STREAM_SPARSE_DATA) {
            ser_declare;
            uint64_t faddr;
            wbuf = rec->data + SPARSE_FADDR_SIZE;
            wsize = rec->data_len - SPARSE_FADDR_SIZE;
            ser_begin(rec->data, SPARSE_FADDR_SIZE);
            unser_uint64(faddr);
            if (fileAddr != faddr) {
               fileAddr = faddr;
               if (blseek(&bfd, (off_t)fileAddr, SEEK_SET) < 0) {
                  berrno be;
                  Emsg2(M_ERROR_TERM, 0, _("Seek error on %s: %s\n"),
                     attr->ofname, be.strerror());
               }
            }
         } else {
            wbuf = rec->data;
            wsize = rec->data_len;
         }
         total += wsize;
         Dmsg2(8, "Write %u bytes, total=%u\n", wsize, total);
         if ((uint32_t)bwrite(&bfd, wbuf, wsize) != wsize) {
            berrno be;
            Emsg2(M_ERROR_TERM, 0, _("Write error on %s: %s\n"),
               attr->ofname, be.strerror());
         }
         fileAddr += wsize;
      }
      break;

   /* GZIP data stream */
   case STREAM_GZIP_DATA:
   case STREAM_SPARSE_GZIP_DATA:
   case STREAM_WIN32_GZIP_DATA:
#ifdef HAVE_LIBZ
      if (extract) {
         uLong compress_len;
         int stat;

         if (rec->Stream == STREAM_SPARSE_GZIP_DATA) {
            ser_declare;
            uint64_t faddr;
            char ec1[50];
            wbuf = rec->data + SPARSE_FADDR_SIZE;
            wsize = rec->data_len - SPARSE_FADDR_SIZE;
            ser_begin(rec->data, SPARSE_FADDR_SIZE);
            unser_uint64(faddr);
            if (fileAddr != faddr) {
               fileAddr = faddr;
               if (blseek(&bfd, (off_t)fileAddr, SEEK_SET) < 0) {
                  berrno be;
                  Emsg3(M_ERROR, 0, _("Seek to %s error on %s: ERR=%s\n"),
                     edit_uint64(fileAddr, ec1), attr->ofname, be.strerror());
                  extract = false;
                  return true;
               }
            }
         } else {
            wbuf = rec->data;
            wsize = rec->data_len;
         }
         compress_len = compress_buf_size;
         if ((stat=uncompress((Bytef *)compress_buf, &compress_len,
               (const Bytef *)wbuf, (uLong)wsize) != Z_OK)) {
            Emsg1(M_ERROR, 0, _("Uncompression error. ERR=%d\n"), stat);
            extract = false;
            return true;
         }

         Dmsg2(100, "Write uncompressed %d bytes, total before write=%d\n", compress_len, total);
         if ((uLongf)bwrite(&bfd, compress_buf, (size_t)compress_len) != compress_len) {
            berrno be;
            Pmsg0(0, "===Write error===\n");
            Emsg2(M_ERROR, 0, _("Write error on %s: %s\n"),
               attr->ofname, be.strerror());
            extract = false;
            return true;
         }
         total += compress_len;
         fileAddr += compress_len;
         Dmsg2(100, "Compress len=%d uncompressed=%d\n", rec->data_len,
            compress_len);
      }
#else
      if (extract) {
         Emsg0(M_ERROR, 0, "GZIP data stream found, but GZIP not configured!\n");
         extract = false;
         return true;
      }
#endif
      break;

   case STREAM_MD5_SIGNATURE:
   case STREAM_SHA1_SIGNATURE:
      break;

   case STREAM_PROGRAM_NAMES:
   case STREAM_PROGRAM_DATA:
      if (!prog_name_msg) {
         Pmsg0(000, "Got Program Name or Data Stream. Ignored.\n");
         prog_name_msg++;
      }
      break;

   default:
      /* If extracting, wierd stream (not 1 or 2), close output file anyway */
      if (extract) {
         if (!is_bopen(&bfd)) {
            Emsg0(M_ERROR, 0, "Logic error output file should be open but is not.\n");
         }
         set_attributes(jcr, attr, &bfd);
         extract = false;
      }
      Jmsg(jcr, M_ERROR, 0, _("Unknown stream=%d ignored. This shouldn't happen!\n"),
         rec->Stream);
      break;

   } /* end switch */
   return true;
}

/* Dummies to replace askdir.c */
bool    dir_get_volume_info(DCR *dcr, enum get_vol_info_rw  writing) { return 1;}
bool    dir_find_next_appendable_volume(DCR *dcr) { return 1;}
bool    dir_update_volume_info(DCR *dcr, bool relabel) { return 1; }
bool    dir_create_jobmedia_record(DCR *dcr) { return 1; }
bool    dir_ask_sysop_to_create_appendable_volume(DCR *dcr) { return 1; }
bool    dir_update_file_attributes(DCR *dcr, DEV_RECORD *rec) { return 1;}
bool    dir_send_job_status(JCR *jcr) {return 1;}
VOLRES *new_volume(DCR *dcr, const char *VolumeName) { return NULL; }
bool    free_volume(DEVICE *dev) { return true; }
void    free_unused_volume(DCR *dcr) { }


bool dir_ask_sysop_to_mount_volume(DCR *dcr)
{
   DEVICE *dev = dcr->dev;
   fprintf(stderr, "Mount Volume \"%s\" on device %s and press return when ready: ",
      dcr->VolumeName, dev->print_name());
   getchar();
   return true;
}
