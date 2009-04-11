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
 * Read code for Storage daemon
 *
 *     Kern Sibbald, November MM
 *
 *   Version $Id$
 */

#include "bacula.h"
#include "stored.h"

/* Forward referenced subroutines */
static bool record_cb(DCR *dcr, DEV_RECORD *rec);


/* Responses sent to the File daemon */
static char OK_data[]    = "3000 OK data\n";
static char FD_error[]   = "3000 error\n";
static char rec_header[] = "rechdr %ld %ld %ld %ld %ld";

/*
 *  Read Data and send to File Daemon
 *   Returns: false on failure
 *            true  on success
 */
bool do_read_data(JCR *jcr)
{
   BSOCK *fd = jcr->file_bsock;
   bool ok = true;
   DCR *dcr = jcr->read_dcr;

   Dmsg0(20, "Start read data.\n");

   if (!bnet_set_buffer_size(fd, dcr->device->max_network_buffer_size, BNET_SETBUF_WRITE)) {
      return false;
   }

   if (jcr->NumReadVolumes == 0) {
      Jmsg(jcr, M_FATAL, 0, _("No Volume names found for restore.\n"));
      fd->fsend(FD_error);
      return false;
   }

   Dmsg2(200, "Found %d volumes names to restore. First=%s\n", jcr->NumReadVolumes,
      jcr->VolList->VolumeName);

   /* Ready device for reading */
   if (!acquire_device_for_read(dcr)) {
      fd->fsend(FD_error);
      return false;
   }

   /* Tell File daemon we will send data */
   fd->fsend(OK_data);
   ok = read_records(dcr, record_cb, mount_next_read_volume);

   /* Send end of data to FD */
   fd->signal(BNET_EOD);

   if (!release_device(jcr->read_dcr)) {
      ok = false;
   }

   Dmsg0(30, "Done reading.\n");
   return ok;
}

/*
 * Called here for each record from read_records()
 *  Returns: true if OK
 *           false if error
 */
static bool record_cb(DCR *dcr, DEV_RECORD *rec)
{
   JCR *jcr = dcr->jcr;
   BSOCK *fd = jcr->file_bsock;
   bool ok = true;
   POOLMEM *save_msg;
   char ec1[50], ec2[50];

   if (rec->FileIndex < 0) {
      return true;
   }
   Dmsg5(400, "Send to FD: SessId=%u SessTim=%u FI=%s Strm=%s, len=%d\n",
      rec->VolSessionId, rec->VolSessionTime, 
      FI_to_ascii(ec1, rec->FileIndex),
      stream_to_ascii(ec2, rec->Stream, rec->FileIndex),
      rec->data_len);

   /* Send record header to File daemon */
   if (!fd->fsend(rec_header, rec->VolSessionId, rec->VolSessionTime,
          rec->FileIndex, rec->Stream, rec->data_len)) {
      Pmsg1(000, _(">filed: Error Hdr=%s\n"), fd->msg);
      Jmsg1(jcr, M_FATAL, 0, _("Error sending to File daemon. ERR=%s\n"),
         fd->bstrerror());
      return false;
   } else {
      Dmsg1(400, ">filed: Hdr=%s\n", fd->msg);
   }


   /* Send data record to File daemon */
   save_msg = fd->msg;          /* save fd message pointer */
   fd->msg = rec->data;         /* pass data directly to the FD */
   fd->msglen = rec->data_len;
   Dmsg1(400, ">filed: send %d bytes data.\n", fd->msglen);
   if (!fd->send()) {
      Pmsg1(000, _("Error sending to FD. ERR=%s\n"), fd->bstrerror());
      Jmsg1(jcr, M_FATAL, 0, _("Error sending to File daemon. ERR=%s\n"),
         fd->bstrerror());

      ok = false;
   }
   fd->msg = save_msg;                /* restore fd message pointer */
   return ok;
}
