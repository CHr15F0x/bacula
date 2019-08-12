/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2014 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from many
   others, a complete list can be found in the file AUTHORS.

   You may use this file and others of this release according to the
   license defined in the LICENSE file, which includes the Affero General
   Public License, v3.0 ("AGPLv3") and some additional permissions and
   terms pursuant to its AGPLv3 Section 7.

   Bacula® is a registered trademark of Kern Sibbald.
*/
/*
 * Append code for Storage daemon
 *  Kern Sibbald, May MM
 *
 */

#include "bacula.h"
#include "stored.h"



#define KLDEBUG 0
#define KLDEBUG_FILE_IDX 0


static const char *KL_stream_to_ascii(int stream)
{
   static char buf[20];

   switch (stream & STREAMMASK_TYPE) {
   case STREAM_UNIX_ATTRIBUTES:
      return _("Unix attributes");
   case STREAM_FILE_DATA:
      return _("File data");
   case STREAM_MD5_DIGEST:
      return _("MD5 digest");
   case STREAM_GZIP_DATA:
      return _("GZIP data");
   case STREAM_COMPRESSED_DATA:
      return _("Compressed data");
   case STREAM_UNIX_ATTRIBUTES_EX:
      return _("Extended attributes");
   case STREAM_SPARSE_DATA:
      return _("Sparse data");
   case STREAM_SPARSE_GZIP_DATA:
      return _("GZIP sparse data");
   case STREAM_SPARSE_COMPRESSED_DATA:
      return _("Compressed sparse data");
   case STREAM_PROGRAM_NAMES:
      return _("Program names");
   case STREAM_PROGRAM_DATA:
      return _("Program data");
   case STREAM_SHA1_DIGEST:
      return _("SHA1 digest");
   case STREAM_WIN32_DATA:
      return _("Win32 data");
   case STREAM_WIN32_GZIP_DATA:
      return _("Win32 GZIP data");
   case STREAM_WIN32_COMPRESSED_DATA:
      return _("Win32 compressed data");
   case STREAM_MACOS_FORK_DATA:
      return _("MacOS Fork data");
   case STREAM_HFSPLUS_ATTRIBUTES:
      return _("HFS+ attribs");
   case STREAM_UNIX_ACCESS_ACL:
      return _("Standard Unix ACL attribs");
   case STREAM_UNIX_DEFAULT_ACL:
      return _("Default Unix ACL attribs");
   case STREAM_SHA256_DIGEST:
      return _("SHA256 digest");
   case STREAM_SHA512_DIGEST:
      return _("SHA512 digest");
   case STREAM_SIGNED_DIGEST:
      return _("Signed digest");
   case STREAM_ENCRYPTED_FILE_DATA:
      return _("Encrypted File data");
   case STREAM_ENCRYPTED_WIN32_DATA:
      return _("Encrypted Win32 data");
   case STREAM_ENCRYPTED_SESSION_DATA:
      return _("Encrypted session data");
   case STREAM_ENCRYPTED_FILE_GZIP_DATA:
      return _("Encrypted GZIP data");
   case STREAM_ENCRYPTED_FILE_COMPRESSED_DATA:
      return _("Encrypted compressed data");
   case STREAM_ENCRYPTED_WIN32_GZIP_DATA:
      return _("Encrypted Win32 GZIP data");
   case STREAM_ENCRYPTED_WIN32_COMPRESSED_DATA:
      return _("Encrypted Win32 Compressed data");
   case STREAM_ENCRYPTED_MACOS_FORK_DATA:
      return _("Encrypted MacOS fork data");
   case STREAM_PLUGIN_NAME:
      return _("Plugin Name");
   case STREAM_PLUGIN_DATA:
      return _("Plugin Data");
   case STREAM_RESTORE_OBJECT:
      return _("Restore Object");
   case STREAM_ACL_AIX_TEXT:
      return _("AIX Specific ACL attribs");
   case STREAM_ACL_DARWIN_ACCESS_ACL:
      return _("Darwin Specific ACL attribs");
   case STREAM_ACL_FREEBSD_DEFAULT_ACL:
      return _("FreeBSD Specific Default ACL attribs");
   case STREAM_ACL_FREEBSD_ACCESS_ACL:
      return _("FreeBSD Specific Access ACL attribs");
   case STREAM_ACL_HPUX_ACL_ENTRY:
      return _("HPUX Specific ACL attribs");
   case STREAM_ACL_IRIX_DEFAULT_ACL:
      return _("Irix Specific Default ACL attribs");
   case STREAM_ACL_IRIX_ACCESS_ACL:
      return _("Irix Specific Access ACL attribs");
   case STREAM_ACL_LINUX_DEFAULT_ACL:
      return _("Linux Specific Default ACL attribs");
   case STREAM_ACL_LINUX_ACCESS_ACL:
      return _("Linux Specific Access ACL attribs");
   case STREAM_ACL_TRU64_DEFAULT_ACL:
      return _("TRU64 Specific Default ACL attribs");
   case STREAM_ACL_TRU64_ACCESS_ACL:
      return _("TRU64 Specific Access ACL attribs");
   case STREAM_ACL_SOLARIS_ACLENT:
      return _("Solaris Specific POSIX ACL attribs");
   case STREAM_ACL_SOLARIS_ACE:
      return _("Solaris Specific NFSv4/ZFS ACL attribs");
   case STREAM_ACL_AFS_TEXT:
      return _("AFS Specific ACL attribs");
   case STREAM_ACL_AIX_AIXC:
      return _("AIX Specific POSIX ACL attribs");
   case STREAM_ACL_AIX_NFS4:
      return _("AIX Specific NFSv4 ACL attribs");
   case STREAM_ACL_FREEBSD_NFS4_ACL:
      return _("FreeBSD Specific NFSv4/ZFS ACL attribs");
   case STREAM_ACL_HURD_DEFAULT_ACL:
      return _("GNU Hurd Specific Default ACL attribs");
   case STREAM_ACL_HURD_ACCESS_ACL:
      return _("GNU Hurd Specific Access ACL attribs");
   case STREAM_XATTR_HURD:
      return _("GNU Hurd Specific Extended attribs");
   case STREAM_XATTR_IRIX:
      return _("IRIX Specific Extended attribs");
   case STREAM_XATTR_TRU64:
      return _("TRU64 Specific Extended attribs");
   case STREAM_XATTR_AIX:
      return _("AIX Specific Extended attribs");
   case STREAM_XATTR_OPENBSD:
      return _("OpenBSD Specific Extended attribs");
   case STREAM_XATTR_SOLARIS_SYS:
      return _("Solaris Specific Extensible attribs or System Extended attribs");
   case STREAM_XATTR_SOLARIS:
      return _("Solaris Specific Extended attribs");
   case STREAM_XATTR_DARWIN:
      return _("Darwin Specific Extended attribs");
   case STREAM_XATTR_FREEBSD:
      return _("FreeBSD Specific Extended attribs");
   case STREAM_XATTR_LINUX:
      return _("Linux Specific Extended attribs");
   case STREAM_XATTR_NETBSD:
      return _("NetBSD Specific Extended attribs");
   default:
      sprintf(buf, "%d", stream);
      return (const char *)buf;
   }
}







/* Responses sent to the File daemon */
static char OK_data[]    = "3000 OK data\n";
static char OK_append[]  = "3000 OK append data\n";

/*
 *  Append Data sent from Client (FD/SD)
 *
 */
bool do_append_data(JCR *jcr)
{
   int32_t n;
   int32_t file_index, stream, last_file_index;
   uint64_t stream_len;
   BSOCK *fd = jcr->file_bsock;
   bool ok = true;
   DEV_RECORD rec;
   char buf1[100], buf2[100];
   DCR *dcr = jcr->dcr;
   DEVICE *dev;
   char ec[50];


   uint64_t total_stream_len;



   if (!dcr) {
      pm_strcpy(jcr->errmsg, _("DCR is NULL!!!\n"));
      Jmsg0(jcr, M_FATAL, 0, jcr->errmsg);

#if KLDEBUG
      Pmsg0(50, "\t\t\t!!!! DCR is NULL!!!\n");
#endif

      return false;
   }
   dev = dcr->dev;
   if (!dev) {
      pm_strcpy(jcr->errmsg, _("DEVICE is NULL!!!\n"));
      Jmsg0(jcr, M_FATAL, 0, jcr->errmsg);

#if KLDEBUG
      Pmsg0(50, "\t\t\t!!!! DEVICE is NULL!!!\n");
#endif

      return false;
   }

   Dmsg1(100, "Start append data. res=%d\n", dev->num_reserved());

#if KLDEBUG
   Pmsg1(50, "\t\t\t!!!! Start append data. res=%d\n", dev->num_reserved());
#endif

   memset(&rec, 0, sizeof(rec));

   if (!fd->set_buffer_size(dcr->device->max_network_buffer_size, BNET_SETBUF_WRITE)) {
      jcr->setJobStatus(JS_ErrorTerminated);
      pm_strcpy(jcr->errmsg, _("Unable to set network buffer size.\n"));
      Jmsg0(jcr, M_FATAL, 0, jcr->errmsg);

#if KLDEBUG
      Pmsg0(50, "\t\t\t!!!! Unable to set network buffer size.\n");
#endif

      return false;
   }

   if (!acquire_device_for_append(dcr)) {
      jcr->setJobStatus(JS_ErrorTerminated);
      return false;
   }

   jcr->sendJobStatus(JS_Running);

   //ASSERT(dev->VolCatInfo.VolCatName[0]);
   if (dev->VolCatInfo.VolCatName[0] == 0) {
      Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));

#if KLDEBUG
      Pmsg0(50, "\t\t\t!!!! NULL Volume name. This shouldn't happen!!!\n");
#endif

   }
   Dmsg1(50, "Begin append device=%s\n", dev->print_name());

#if KLDEBUG
   Pmsg1(50, "\t\t\t!!!! Begin append device=%s\n", dev->print_name());
#endif

   begin_data_spool(dcr);
   begin_attribute_spool(jcr);

   Dmsg0(100, "Just after acquire_device_for_append\n");
   //ASSERT(dev->VolCatInfo.VolCatName[0]);
   if (dev->VolCatInfo.VolCatName[0] == 0) {
      Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));

#if KLDEBUG
      Pmsg0(50, "\t\t\t!!!! NULL Volume name. This shouldn't happen!!!\n");
#endif
   }
   /*
    * Write Begin Session Record
    */
   if (!write_session_label(dcr, SOS_LABEL)) {
      Jmsg1(jcr, M_FATAL, 0, _("Write session label failed. ERR=%s\n"),
         dev->bstrerror());
      jcr->setJobStatus(JS_ErrorTerminated);

#if KLDEBUG
      Pmsg1(50, "\t\t\t!!!! Write session label failed. ERR=%s\n", dev->bstrerror());
#endif

      ok = false;
   }
   //ASSERT(dev->VolCatInfo.VolCatName[0]);
   if (dev->VolCatInfo.VolCatName[0] == 0) {
      Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));

#if KLDEBUG
      Pmsg0(50, "\t\t\t!!!! NULL Volume name. This shouldn't happen!!!\n");
#endif
   }

   /* Tell File daemon to send data */
   if (!fd->fsend(OK_data)) {
      berrno be;
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to FD. ERR=%s\n"),
            be.bstrerror(fd->b_errno));

#if KLDEBUG
      Pmsg1(50, "\t\t\t!!!! Network send error to FD. ERR=%s\n", be.bstrerror(fd->b_errno));
#endif

      ok = false;
   }

   /*
    * Get Data from File daemon, write to device.  To clarify what is
    *   going on here.  We expect:
    *     - A stream header
    *     - Multiple records of data
    *     - EOD record
    *
    *    The Stream header is just used to sychronize things, and
    *    none of the stream header is written to tape.
    *    The Multiple records of data, contain first the Attributes,
    *    then after another stream header, the file data, then
    *    after another stream header, the MD5 data if any.
    *
    *   So we get the (stream header, data, EOD) three time for each
    *   file. 1. for the Attributes, 2. for the file data if any,
    *   and 3. for the MD5 if any.
    */
   dcr->VolFirstIndex = dcr->VolLastIndex = 0;
   jcr->run_time = time(NULL);              /* start counting time for rates */
   for (last_file_index = 0; ok && !jcr->is_job_canceled(); ) {

      /* Read Stream header from the File daemon.
       *  The stream header consists of the following:
       *    file_index (sequential Bacula file index, base 1)
       *    stream     (Bacula number to distinguish parts of data)
       *    stream_len (Expected length of this stream. This
       *       will be the size backed up if the file does not
       *       grow during the backup.
       */
     if ((n=bget_msg(fd)) <= 0) {


    	 total_stream_len = 0;


         if (n == BNET_SIGNAL && fd->msglen == BNET_EOD) {
            Dmsg0(200, "Got EOD on reading header.\n");

#if KLDEBUG
            Pmsg0(50, "\t\t\t!!!! Got EOD on reading header.\n");
#endif

            break;                    /* end of data */
         }
         Jmsg3(jcr, M_FATAL, 0, _("Error reading data header from FD. n=%d msglen=%d ERR=%s\n"),
               n, fd->msglen, fd->bstrerror());

#if KLDEBUG
         Pmsg3(50, "\t\t\t!!!! Error reading data header from FD. n=%d msglen=%d ERR=%s\n", n, fd->msglen, fd->bstrerror());
#endif

         ok = false;
         break;
      }

      if (sscanf(fd->msg, "%ld %ld %lld", &file_index, &stream, &stream_len) != 3) {
         Jmsg1(jcr, M_FATAL, 0, _("Malformed data header from FD: %s\n"), fd->msg);

#if KLDEBUG
         Pmsg1(50, "\t\t\t!!!! Malformed data header from FD: %s\n", fd->msg);
#endif

         ok = false;
         break;
      }

      Dmsg3(890, "<filed: Header FilInx=%d stream=%d stream_len=%lld\n",
         file_index, stream, stream_len);


#if KLDEBUG_FILE_IDX

      total_stream_len += stream_len;

      Pmsg6(50, ">>>> APPEND Files=%d FilInx=%d stream=%d stream_len=%lld tot_stream_len=%lld %s\n",
    	jcr->JobFiles, file_index, stream, stream_len, total_stream_len, KL_stream_to_ascii(stream));
#endif


#if KLDEBUG
      Pmsg3(50, "\t\t\t!!!! <filed: Header FilInx=%d stream=%d stream_len=%lld\n",
         file_index, stream, stream_len);

      // KLIS
      Pmsg1(50, "\t\t!!!! APPEND last_file_index: %d\n", last_file_index);
#endif

      /*
       * We make sure the file_index is advancing sequentially.
       */
      if (jcr->rerunning && file_index > 0 && last_file_index == 0) {
         goto fi_checked;
      }
      Dmsg2(400, "file_index=%d last_file_index=%d\n", file_index, last_file_index);

#if KLDEBUG
      Pmsg2(50, "\t\t\t!!!! file_index=%d last_file_index=%d\n", file_index, last_file_index);
#endif

      if (file_index > 0 && (file_index == last_file_index ||
          file_index == last_file_index + 1)) {
         goto fi_checked;
      }

     // TODO KLIS file indexes not coming in order right now
     // TODO REMOVE IF 0
      Pmsg2(50, "\t\t\t!!!! FI=%d from FD not positive or last_FI=%d\n", file_index, last_file_index);

#if 1
      Jmsg2(jcr, M_FATAL, 0, _("FI=%d from FD not positive or last_FI=%d\n"),
            file_index, last_file_index);
      ok = false;
      break;
#endif

fi_checked:
      if (file_index != last_file_index) {
         jcr->JobFiles = file_index;
         last_file_index = file_index;
      }

      /* Read data stream from the File daemon.
       *  The data stream is just raw bytes
       */
      while ((n=bget_msg(fd)) > 0 && !jcr->is_job_canceled()) {
         rec.VolSessionId = jcr->VolSessionId;
         rec.VolSessionTime = jcr->VolSessionTime;
         rec.FileIndex = file_index;
         rec.Stream = stream;
         rec.StreamLen = stream_len;
         rec.maskedStream = stream & STREAMMASK_TYPE;   /* strip high bits */
         rec.data_len = fd->msglen;
         rec.data = fd->msg;            /* use message buffer */

         Dmsg4(850, "before writ_rec FI=%d SessId=%d Strm=%s len=%d\n",
            rec.FileIndex, rec.VolSessionId,
            stream_to_ascii(buf1, rec.Stream,rec.FileIndex),
            rec.data_len);

#if KLDEBUG
         Pmsg4(50, "\t\t\t!!!! before writ_rec FI=%d SessId=%d Strm=%s len=%d\n",
            rec.FileIndex, rec.VolSessionId,
            stream_to_ascii(buf1, rec.Stream,rec.FileIndex),
            rec.data_len);
#endif

         ok = dcr->write_record(&rec);
         if (!ok) {
            Dmsg2(90, "Got write_block_to_dev error on device %s. %s\n",
                  dcr->dev->print_name(), dcr->dev->bstrerror());

#if KLDEBUG
            Pmsg2(50, "\t\t\t!!!! Got write_block_to_dev error on device %s. %s\n",
                  dcr->dev->print_name(), dcr->dev->bstrerror());
#endif

            break;
         }
         jcr->JobBytes += rec.data_len;   /* increment bytes this job */
         Dmsg4(850, "write_record FI=%s SessId=%d Strm=%s len=%d\n",
            FI_to_ascii(buf1, rec.FileIndex), rec.VolSessionId,
            stream_to_ascii(buf2, rec.Stream, rec.FileIndex), rec.data_len);

#if KLDEBUG
         Pmsg4(50, "\t\t\t!!!! write_record FI=%s SessId=%d Strm=%s len=%d\n",
            FI_to_ascii(buf1, rec.FileIndex), rec.VolSessionId,
            stream_to_ascii(buf2, rec.Stream, rec.FileIndex), rec.data_len);
#endif

         send_attrs_to_dir(jcr, &rec);
         Dmsg0(650, "Enter bnet_get\n");

#if KLDEBUG
         Pmsg0(50, "\t\t\t!!!! Enter bnet_get\n");
#endif
      }
      Dmsg2(650, "End read loop with FD. JobFiles=%d Stat=%d\n", jcr->JobFiles, n);

#if KLDEBUG
      Pmsg2(50, "\t\t\t!!!! End read loop with FD. JobFiles=%d Stat=%d\n", jcr->JobFiles, n);
#endif

      if (fd->is_error()) {
         if (!jcr->is_job_canceled()) {
            Dmsg1(350, "Network read error from FD. ERR=%s\n", fd->bstrerror());

#if KLDEBUG
            Pmsg1(50, "\t\t\t!!!! Network read error from FD. ERR=%s\n", fd->bstrerror());
#endif

            Jmsg1(jcr, M_FATAL, 0, _("Network error reading from FD. ERR=%s\n"),
                  fd->bstrerror());
         }
         ok = false;
         break;
      }
   }

#if KLDEBUG
            Pmsg1(50, "\t\t\t!!!! BEGIN setJobStatus ok=%d\n", ok);
#endif

   /* Create Job status for end of session label */
   jcr->setJobStatus(ok?JS_Terminated:JS_ErrorTerminated);


#if KLDEBUG
            Pmsg1(50, "\t\t\t!!!! END setJobStatus ok=%d\n", ok);
#endif

   if (ok) {
      /* Terminate connection with Client */
#if KLDEBUG
            Pmsg0(50, "\t\t\t!!!! fd->fsend(OK_append)\n");
#endif

      fd->fsend(OK_append);

#if KLDEBUG
            Pmsg0(50, "\t\t\t!!!! do_client_commands(jcr)\n");
#endif

      do_client_commands(jcr);            /* finish dialog with Client */
   } else {
      fd->fsend("3999 Failed append\n");
   }

   Dmsg1(200, "Write EOS label JobStatus=%c\n", jcr->JobStatus);

#if KLDEBUG
   Pmsg1(200, "\t\t\t!!!! Write EOS label JobStatus=%c\n", jcr->JobStatus);
#endif

   /*
    * Check if we can still write. This may not be the case
    *  if we are at the end of the tape or we got a fatal I/O error.
    */
   if (ok || dev->can_write()) {
      if (!write_session_label(dcr, EOS_LABEL)) {
         /* Print only if ok and not cancelled to avoid spurious messages */
         if (ok && !jcr->is_job_canceled()) {
            Jmsg1(jcr, M_FATAL, 0, _("Error writing end session label. ERR=%s\n"),
                  dev->bstrerror());

#if KLDEBUG
            Pmsg1(50, "\t\t\t!!!! Error writing end session label. ERR=%s\n", dev->bstrerror());
#endif
         }
         jcr->setJobStatus(JS_ErrorTerminated);
         ok = false;
      }
      /* Flush out final partial block of this session */
      if (!dcr->write_block_to_device()) {
         /* Print only if ok and not cancelled to avoid spurious messages */
         if (ok && !jcr->is_job_canceled()) {
            Jmsg2(jcr, M_FATAL, 0, _("Fatal append error on device %s: ERR=%s\n"),
                  dev->print_name(), dev->bstrerror());
            Dmsg0(100, _("Set ok=FALSE after write_block_to_device.\n"));

#if KLDEBUG
            Pmsg0(50, "\t\t\t!!!! Set ok=FALSE after write_block_to_device.\n");
#endif
         }
         jcr->setJobStatus(JS_ErrorTerminated);
         ok = false;
      }
   }

   if (!ok) {
      discard_data_spool(dcr);
   } else {
      /* Note: if commit is OK, the device will remain blocked */
      commit_data_spool(dcr);
   }

   /*
    * Don't use time_t for job_elapsed as time_t can be 32 or 64 bits,
    *   and the subsequent Jmsg() editing will break
    */
   int32_t job_elapsed = time(NULL) - jcr->run_time;

   if (job_elapsed <= 0) {
      job_elapsed = 1;
   }

   Jmsg(dcr->jcr, M_INFO, 0, _("Elapsed time=%02d:%02d:%02d, Transfer rate=%s Bytes/second\n"),
         job_elapsed / 3600, job_elapsed % 3600 / 60, job_elapsed % 60,
         edit_uint64_with_suffix(jcr->JobBytes / job_elapsed, ec));

   /*
    * Release the device -- and send final Vol info to DIR
    *  and unlock it.
    */
   release_device(dcr);

   if (!ok || jcr->is_job_canceled()) {
      discard_attribute_spool(jcr);
   } else {
      commit_attribute_spool(jcr);
   }

   jcr->sendJobStatus();          /* update director */

   Dmsg1(100, "return from do_append_data() ok=%d\n", ok);

#if KLDEBUG
   Pmsg1(50, "\t\t\t!!!! return from do_append_data() ok=%d\n", ok);
#endif

   return ok;
}


/* Send attributes and digest to Director for Catalog */
bool send_attrs_to_dir(JCR *jcr, DEV_RECORD *rec)
{
   if (rec->maskedStream == STREAM_UNIX_ATTRIBUTES    ||
       rec->maskedStream == STREAM_UNIX_ATTRIBUTES_EX ||
       rec->maskedStream == STREAM_RESTORE_OBJECT     ||
       crypto_digest_stream_type(rec->maskedStream) != CRYPTO_DIGEST_NONE) {
      if (!jcr->no_attributes) {
         BSOCK *dir = jcr->dir_bsock;
         if (are_attributes_spooled(jcr)) {
            dir->set_spooling();
         }
         Dmsg1(850, "Send attributes to dir. FI=%d\n", rec->FileIndex);
         if (!dir_update_file_attributes(jcr->dcr, rec)) {
            Jmsg(jcr, M_FATAL, 0, _("Error updating file attributes. ERR=%s\n"),
               dir->bstrerror());
            dir->clear_spooling();
            return false;
         }
         dir->clear_spooling();
      }
   }
   return true;
}
