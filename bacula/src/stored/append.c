/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

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
 * Append code for Storage daemon
 *  Kern Sibbald, May MM
 *
 *  Version $Id$
 */

#include "bacula.h"
#include "stored.h"


/* Responses sent to the File daemon */
static char OK_data[]    = "3000 OK data\n";
static char OK_append[]  = "3000 OK append data\n";

/* Forward referenced functions */

/*
 *  Append Data sent from File daemon
 *
 */
bool do_append_data(JCR *jcr)
{
   int32_t n;
   int32_t file_index, stream, last_file_index;
   BSOCK *fd = jcr->file_bsock;
   bool ok = true;
   DEV_RECORD rec;
   char buf1[100], buf2[100];
   DCR *dcr = jcr->dcr;
   DEVICE *dev;
   char ec[50];


   if (!dcr) { 
      Jmsg0(jcr, M_FATAL, 0, _("DCR is NULL!!!\n"));
      return false;
   }                                              
   dev = dcr->dev;
   if (!dev) { 
      Jmsg0(jcr, M_FATAL, 0, _("DEVICE is NULL!!!\n"));
      return false;
   }                                              

   Dmsg1(100, "Start append data. res=%d\n", dev->num_reserved());

   memset(&rec, 0, sizeof(rec));

   if (!fd->set_buffer_size(dcr->device->max_network_buffer_size, BNET_SETBUF_WRITE)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      Jmsg0(jcr, M_FATAL, 0, _("Unable to set network buffer size.\n"));
      return false;
   }

   if (!acquire_device_for_append(dcr)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return false;
   }

   set_jcr_job_status(jcr, JS_Running);
   dir_send_job_status(jcr);

   if (dev->VolCatInfo.VolCatName[0] == 0) {
      Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
   }
   Dmsg1(50, "Begin append device=%s\n", dev->print_name());

   begin_data_spool(dcr);
   begin_attribute_spool(jcr);

   Dmsg0(100, "Just after acquire_device_for_append\n");
   if (dev->VolCatInfo.VolCatName[0] == 0) {
      Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
   }
   /*
    * Write Begin Session Record
    */
   if (!write_session_label(dcr, SOS_LABEL)) {
      Jmsg1(jcr, M_FATAL, 0, _("Write session label failed. ERR=%s\n"),
         dev->bstrerror());
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      ok = false;
   }
   if (dev->VolCatInfo.VolCatName[0] == 0) {
      Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
   }

   /* Tell File daemon to send data */
   if (!fd->fsend(OK_data)) {
      berrno be;
      Jmsg1(jcr, M_FATAL, 0, _("Network send error to FD. ERR=%s\n"),
            be.bstrerror(fd->b_errno));
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
   for (last_file_index = 0; ok && !job_canceled(jcr); ) {

      /* Read Stream header from the File daemon.
       *  The stream header consists of the following:
       *    file_index (sequential Bacula file index, base 1)
       *    stream     (Bacula number to distinguish parts of data)
       *    info       (Info for Storage daemon -- compressed, encrypted, ...)
       *       info is not currently used, so is read, but ignored!
       */
     if ((n=bget_msg(fd)) <= 0) {
         if (n == BNET_SIGNAL && fd->msglen == BNET_EOD) {
            break;                    /* end of data */
         }
         Jmsg1(jcr, M_FATAL, 0, _("Error reading data header from FD. ERR=%s\n"),
               fd->bstrerror());
         ok = false;
         break;
      }

      if (sscanf(fd->msg, "%ld %ld", &file_index, &stream) != 2) {
         Jmsg1(jcr, M_FATAL, 0, _("Malformed data header from FD: %s\n"), fd->msg);
         ok = false;
         break;
      }

      Dmsg2(890, "<filed: Header FilInx=%d stream=%d\n", file_index, stream);

      if (!(file_index > 0 && (file_index == last_file_index ||
          file_index == last_file_index + 1))) {
         Jmsg0(jcr, M_FATAL, 0, _("File index from FD not positive or sequential\n"));
         ok = false;
         break;
      }
      if (file_index != last_file_index) {
         jcr->JobFiles = file_index;
         last_file_index = file_index;
      }

      /* Read data stream from the File daemon.
       *  The data stream is just raw bytes
       */
      while ((n=bget_msg(fd)) > 0 && !job_canceled(jcr)) {
         rec.VolSessionId = jcr->VolSessionId;
         rec.VolSessionTime = jcr->VolSessionTime;
         rec.FileIndex = file_index;
         rec.Stream = stream;
         rec.data_len = fd->msglen;
         rec.data = fd->msg;            /* use message buffer */

         Dmsg4(850, "before writ_rec FI=%d SessId=%d Strm=%s len=%d\n",
            rec.FileIndex, rec.VolSessionId, 
            stream_to_ascii(buf1, rec.Stream,rec.FileIndex),
            rec.data_len);

         while (!write_record_to_block(dcr->block, &rec)) {
            Dmsg2(850, "!write_record_to_block data_len=%d rem=%d\n", rec.data_len,
                       rec.remainder);
            if (!write_block_to_device(dcr)) {
               Dmsg2(90, "Got write_block_to_dev error on device %s. %s\n",
                  dev->print_name(), dev->bstrerror());
               ok = false;
               break;
            }
         }
         if (!ok) {
            Dmsg0(400, "Not OK\n");
            break;
         }
         jcr->JobBytes += rec.data_len;   /* increment bytes this job */
         Dmsg4(850, "write_record FI=%s SessId=%d Strm=%s len=%d\n",
            FI_to_ascii(buf1, rec.FileIndex), rec.VolSessionId,
            stream_to_ascii(buf2, rec.Stream, rec.FileIndex), rec.data_len);

         /* Send attributes and digest to Director for Catalog */
         if (stream == STREAM_UNIX_ATTRIBUTES || stream == STREAM_UNIX_ATTRIBUTES_EX ||
             crypto_digest_stream_type(stream) != CRYPTO_DIGEST_NONE) {
            if (!jcr->no_attributes) {
               BSOCK *dir = jcr->dir_bsock;
               if (are_attributes_spooled(jcr)) {
                  dir->set_spooling();
               }
               Dmsg0(850, "Send attributes to dir.\n");
               if (!dir_update_file_attributes(dcr, &rec)) {
                  dir->clear_spooling();
                  Jmsg(jcr, M_FATAL, 0, _("Error updating file attributes. ERR=%s\n"),
                     dir->bstrerror());
                  ok = false;
                  break;
               }
               dir->clear_spooling();
            }
         }
         Dmsg0(650, "Enter bnet_get\n");
      }
      Dmsg1(650, "End read loop with FD. Stat=%d\n", n);

      if (fd->is_error()) {
         if (!job_canceled(jcr)) {
            Dmsg1(350, "Network read error from FD. ERR=%s\n", fd->bstrerror());
            Jmsg1(jcr, M_FATAL, 0, _("Network error reading from FD. ERR=%s\n"),
                  fd->bstrerror());
         }
         ok = false;
         break;
      }
   }

   /* Create Job status for end of session label */
   set_jcr_job_status(jcr, ok?JS_Terminated:JS_ErrorTerminated);

   if (ok) {
      /* Terminate connection with FD */
      fd->fsend(OK_append);
      do_fd_commands(jcr);               /* finish dialog with FD */
   } else {
      fd->fsend("3999 Failed append\n");
   }

   /*
    * Don't use time_t for job_elapsed as time_t can be 32 or 64 bits,
    *   and the subsequent Jmsg() editing will break
    */
   int32_t job_elapsed = time(NULL) - jcr->run_time;

   if (job_elapsed <= 0) {
      job_elapsed = 1;
   }

   Jmsg(dcr->jcr, M_INFO, 0, _("Job write elapsed time = %02d:%02d:%02d, Transfer rate = %s Bytes/second\n"),
         job_elapsed / 3600, job_elapsed % 3600 / 60, job_elapsed % 60,
         edit_uint64_with_suffix(jcr->JobBytes / job_elapsed, ec));


   Dmsg1(200, "Write EOS label JobStatus=%c\n", jcr->JobStatus);

   /*
    * Check if we can still write. This may not be the case
    *  if we are at the end of the tape or we got a fatal I/O error.
    */
   if (ok || dev->can_write()) {
      if (!write_session_label(dcr, EOS_LABEL)) {
         /* Print only if ok and not cancelled to avoid spurious messages */
         if (ok && !job_canceled(jcr)) {
            Jmsg1(jcr, M_FATAL, 0, _("Error writing end session label. ERR=%s\n"),
                  dev->bstrerror());
         }
         set_jcr_job_status(jcr, JS_ErrorTerminated);
         ok = false;
      }
      if (dev->VolCatInfo.VolCatName[0] == 0) {
         Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
         Dmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
      }
      Dmsg0(90, "back from write_end_session_label()\n");
      /* Flush out final partial block of this session */
      if (!write_block_to_device(dcr)) {
         /* Print only if ok and not cancelled to avoid spurious messages */
         if (ok && !job_canceled(jcr)) {
            Jmsg2(jcr, M_FATAL, 0, _("Fatal append error on device %s: ERR=%s\n"),
                  dev->print_name(), dev->bstrerror());
            Dmsg0(100, _("Set ok=FALSE after write_block_to_device.\n"));
         }
         set_jcr_job_status(jcr, JS_ErrorTerminated);
         ok = false;
      }
      if (dev->VolCatInfo.VolCatName[0] == 0) {
         Pmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
         Dmsg0(000, _("NULL Volume name. This shouldn't happen!!!\n"));
      }
   }


   if (!ok) {
      discard_data_spool(dcr);
   } else {
      /* Note: if commit is OK, the device will remain blocked */
      commit_data_spool(dcr);
   }

   if (ok) {
      ok = dvd_close_job(dcr);  /* do DVD cleanup if any */
   }
   
   /*
    * Release the device -- and send final Vol info to DIR
    *  and unlock it.
    */
   release_device(dcr);

   if (!ok || job_canceled(jcr)) {
      discard_attribute_spool(jcr);
   } else {
      commit_attribute_spool(jcr);
   }

   dir_send_job_status(jcr);          /* update director */

   Dmsg1(100, "return from do_append_data() ok=%d\n", ok);
   return ok;
}
