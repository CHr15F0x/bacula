/*
 *
 *  label.c  Bacula routines to handle labels
 *
 *   Kern Sibbald, MM
 *
 *
 *   Version $Id$
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

#include "bacula.h"                   /* pull in global headers */
#include "stored.h"                   /* pull in Storage Deamon headers */

/* Forward referenced functions */
static void create_volume_label_record(DCR *dcr, DEV_RECORD *rec);

extern char my_name[];
extern int debug_level;

/*
 * Read the volume label
 *
 *  If dcr->VolumeName == NULL, we accept any Bacula Volume
 *  If dcr->VolumeName[0] == 0, we accept any Bacula Volume
 *  otherwise dcr->VolumeName must match the Volume.
 *
 *  If VolName given, ensure that it matches
 *
 *  Returns VOL_  code as defined in record.h
 *    VOL_NOT_READ
 *    VOL_OK                          good label found
 *    VOL_NO_LABEL                    volume not labeled
 *    VOL_IO_ERROR                    I/O error reading tape
 *    VOL_NAME_ERROR                  label has wrong name
 *    VOL_CREATE_ERROR                Error creating label
 *    VOL_VERSION_ERROR               label has wrong version
 *    VOL_LABEL_ERROR                 bad label type
 *    VOL_NO_MEDIA                    no media in drive
 *
 *  The dcr block is emptied on return, and the Volume is
 *    rewound.
 */
int read_dev_volume_label(DCR *dcr)
{
   JCR *jcr = dcr->jcr;
   DEVICE *dev = dcr->dev;
   char *VolName = dcr->VolumeName;
   DEV_RECORD *record;
   bool ok = false;
   DEV_BLOCK *block = dcr->block;
   int stat;
   bool want_ansi_label;
   bool have_ansi_label = false;

   Dmsg3(100, "Enter read_volume_label device=%s vol=%s dev_Vol=%s\n",
      dev->print_name(), VolName, dev->VolHdr.VolumeName[0]?dev->VolHdr.VolumeName:
      "*NULL*");

   if (!dev->is_open()) {
      Emsg0(M_ABORT, 0, _("BAD call to read_dev_volume_label\n"));
   }
   if (dev->is_labeled()) {              /* did we already read label? */
      /* Compare Volume Names allow special wild card */
      if (VolName && *VolName && *VolName != '*' && strcmp(dev->VolHdr.VolumeName, VolName) != 0) {
         Mmsg(jcr->errmsg, _("Wrong Volume mounted on device %s: Wanted %s have %s\n"),
            dev->print_name(), VolName, dev->VolHdr.VolumeName);
         /*
          * Cancel Job if too many label errors
          *  => we are in a loop
          */
         if (!dev->poll && jcr->label_errors++ > 100) {
            Jmsg(jcr, M_FATAL, 0, "Too many tries: %s", jcr->errmsg);
         }
         Dmsg0(100, "return VOL_NAME_ERROR\n");
         stat = VOL_NAME_ERROR;
         goto bail_out;
      }
      Dmsg0(30, "Leave read_volume_label() VOL_OK\n");
      return VOL_OK;       /* label already read */
   }

   dev->clear_labeled();
   dev->clear_append();
   dev->clear_read();
   dev->label_type = B_BACULA_LABEL;

   if (!rewind_dev(dev)) {
      Mmsg(jcr->errmsg, _("Couldn't rewind device %s: ERR=%s\n"), 
         dev->print_name(), strerror_dev(dev));
      Dmsg1(30, "return VOL_NO_MEDIA: %s", jcr->errmsg);
      return VOL_NO_MEDIA;
   }
   bstrncpy(dev->VolHdr.Id, "**error**", sizeof(dev->VolHdr.Id));

  /* Read ANSI/IBM label if so requested */
  
  want_ansi_label = dcr->VolCatInfo.LabelType != B_BACULA_LABEL ||
                    dcr->device->label_type != B_BACULA_LABEL;
  if (want_ansi_label || dev_cap(dev, CAP_CHECKLABELS)) {
      stat = read_ansi_ibm_label(dcr);            
      /* If we want a label and didn't find it, return error */
      if (want_ansi_label && stat != VOL_OK) {
         goto bail_out;
      }
      if (stat == VOL_NAME_ERROR || stat == VOL_LABEL_ERROR) {
         Mmsg(jcr->errmsg, _("Wrong Volume mounted on device %s: Wanted %s have %s\n"),
              dev->print_name(), VolName, dev->VolHdr.VolumeName);
         if (!dev->poll && jcr->label_errors++ > 100) {
            Jmsg(jcr, M_FATAL, 0, "Too many tries: %s", jcr->errmsg);
         }
         goto bail_out;
      }
      if (stat != VOL_OK) {           /* Not an ANSI/IBM label, so re-read */
         rewind_dev(dev);
      } else {
         have_ansi_label = true;
      }
   }
  
   /* Read the Bacula Volume label block */
   record = new_record();
   empty_block(block);

   Dmsg0(90, "Big if statement in read_volume_label\n");
   if (!read_block_from_dev(dcr, NO_BLOCK_NUMBER_CHECK)) {
      Mmsg(jcr->errmsg, _("Requested Volume \"%s\" on %s is not a Bacula "
           "labeled Volume, because: ERR=%s"), NPRT(VolName), 
           dev->print_name(), strerror_dev(dev));
      Dmsg1(30, "%s", jcr->errmsg);
   } else if (!read_record_from_block(block, record)) {
      Mmsg(jcr->errmsg, _("Could not read Volume label from block.\n"));
      Dmsg1(30, "%s", jcr->errmsg);
   } else if (!unser_volume_label(dev, record)) {
      Mmsg(jcr->errmsg, _("Could not unserialize Volume label: ERR=%s\n"),
         strerror_dev(dev));
      Dmsg1(30, "%s", jcr->errmsg);
   } else if (strcmp(dev->VolHdr.Id, BaculaId) != 0 &&
              strcmp(dev->VolHdr.Id, OldBaculaId) != 0) {
      Mmsg(jcr->errmsg, _("Volume Header Id bad: %s\n"), dev->VolHdr.Id);
      Dmsg1(30, "%s", jcr->errmsg);
   } else {
      ok = true;
   }
   free_record(record);               /* finished reading Volume record */

   if (!ok) {
      if (forge_on || jcr->ignore_label_errors) {
         dev->set_labeled();         /* set has Bacula label */
         Jmsg(jcr, M_ERROR, 0, "%s", jcr->errmsg);
         empty_block(block);
         return VOL_OK;
      }
      stat = VOL_NO_LABEL;
      goto bail_out;
   }

   /* At this point, we have read the first Bacula block, and
    * then read the Bacula Volume label. Now we need to
    * make sure we have the right Volume.
    */


   if (dev->VolHdr.VerNum != BaculaTapeVersion &&
       dev->VolHdr.VerNum != OldCompatibleBaculaTapeVersion1 &&
       dev->VolHdr.VerNum != OldCompatibleBaculaTapeVersion2) {
      Mmsg(jcr->errmsg, _("Volume on %s has wrong Bacula version. Wanted %d got %d\n"),
         dev->print_name(), BaculaTapeVersion, dev->VolHdr.VerNum);
      Dmsg1(30, "VOL_VERSION_ERROR: %s", jcr->errmsg);
      stat = VOL_VERSION_ERROR;
      goto bail_out;
   }

   /* We are looking for either an unused Bacula tape (PRE_LABEL) or
    * a Bacula volume label (VOL_LABEL)
    */
   if (dev->VolHdr.LabelType != PRE_LABEL && dev->VolHdr.LabelType != VOL_LABEL) {
      Mmsg(jcr->errmsg, _("Volume on %s has bad Bacula label type: %x\n"),
          dev->print_name(), dev->VolHdr.LabelType);
      Dmsg1(30, "%s", jcr->errmsg);
      if (!dev->poll && jcr->label_errors++ > 100) {
         Jmsg(jcr, M_FATAL, 0, "Too many tries: %s", jcr->errmsg);
      }
      Dmsg0(100, "return VOL_LABEL_ERROR\n");
      stat = VOL_LABEL_ERROR;
      goto bail_out;
   }

   dev->set_labeled();               /* set has Bacula label */
   new_volume(dcr, dev->VolHdr.VolumeName);

   /* Compare Volume Names */
   Dmsg2(30, "Compare Vol names: VolName=%s hdr=%s\n", VolName?VolName:"*", dev->VolHdr.VolumeName);
   if (VolName && *VolName && *VolName != '*' && strcmp(dev->VolHdr.VolumeName, VolName) != 0) {
      Mmsg(jcr->errmsg, _("Wrong Volume mounted on device %s: Wanted %s have %s\n"),
           dev->print_name(), VolName, dev->VolHdr.VolumeName);
      Dmsg1(30, "%s", jcr->errmsg);
      /*
       * Cancel Job if too many label errors
       *  => we are in a loop
       */
      if (!dev->poll && jcr->label_errors++ > 100) {
         Jmsg(jcr, M_FATAL, 0, "Too many tries: %s", jcr->errmsg);
      }
      Dmsg0(100, "return VOL_NAME_ERROR\n");
      stat = VOL_NAME_ERROR;
      goto bail_out;
   }
   Dmsg1(30, "Copy vol_name=%s\n", dev->VolHdr.VolumeName);

   if (debug_level >= 10) {
      dump_volume_label(dev);
   }
   Dmsg0(30, "Leave read_volume_label() VOL_OK\n");
   /* If we are a streaming device, we only get one chance to read */
   if (!dev_cap(dev, CAP_STREAM)) {
      rewind_dev(dev);
      if (have_ansi_label) {
         stat = read_ansi_ibm_label(dcr);            
         /* If we want a label and didn't find it, return error */
         if (stat != VOL_OK) {
            goto bail_out;
         }
      }
   }
   empty_block(block);
   return VOL_OK;

bail_out:
   empty_block(block);
   rewind_dev(dev);
   Dmsg1(100, "return %d\n", stat);
   return stat;
}

/*
 * Put a volume label into the block
 *
 *  Returns: false on failure
 *           true  on success
 */
bool write_volume_label_to_block(DCR *dcr)
{
   DEV_RECORD rec;
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;
   DEV_BLOCK *block = dcr->block;

   Dmsg0(20, "write Label in write_volume_label_to_block()\n");
   memset(&rec, 0, sizeof(rec));
   rec.data = get_memory(SER_LENGTH_Volume_Label);
   empty_block(block);                /* Volume label always at beginning */

   create_volume_label_record(dcr, &rec);

   block->BlockNumber = 0;
   if (!write_record_to_block(block, &rec)) {
      free_pool_memory(rec.data);
      Jmsg1(jcr, M_FATAL, 0, _("Cannot write Volume label to block for device %s\n"),
         dev->print_name());
      return false;
   } else {
      Dmsg1(90, "Wrote label of %d bytes to block\n", rec.data_len);
   }
   free_pool_memory(rec.data);
   return true;
}


/*
 * Write a Volume Label
 *  !!! Note, this is ONLY used for writing
 *            a fresh volume label.  Any data
 *            after the label will be destroyed,
 *            in fact, we write the label 5 times !!!!
 *
 *  This routine expects that open_device() was previously called.
 *
 *  This routine should be used only when labeling a blank tape.
 */
bool write_new_volume_label_to_dev(DCR *dcr, const char *VolName, const char *PoolName)
{
   DEVICE *dev = dcr->dev;


   Dmsg0(99, "write_volume_label()\n");
   empty_block(dcr->block);

   Dmsg1(100, "Label type=%d\n", dev->label_type);
   if (!rewind_dev(dev)) {
      memset(&dev->VolHdr, 0, sizeof(dev->VolHdr));
      Dmsg2(30, "Bad status on %s from rewind: ERR=%s\n", dev->print_name(), strerror_dev(dev));
      if (!forge_on) {
         goto bail_out;
      }
   }

   /* Create PRE_LABEL */
   create_volume_label(dev, VolName, PoolName);

   /*
    * If we have already detected an ANSI label, re-read it
    *   to skip past it. Otherwise, we write a new one if 
    *   so requested.  
    */
   if (dev->label_type != B_BACULA_LABEL) {
      if (read_ansi_ibm_label(dcr) != VOL_OK) {
         rewind_dev(dev);
         goto bail_out;
      }
   } else if (!write_ansi_ibm_labels(dcr, ANSI_VOL_LABEL, VolName)) {
      goto bail_out;
   }

   create_volume_label_record(dcr, dcr->rec);
   dcr->rec->Stream = 0;

   /* Temporarily mark in append state to enable writing */
   dev->set_append();
   if (!write_record_to_block(dcr->block, dcr->rec)) {
      Dmsg2(30, "Bad Label write on %s: ERR=%s\n", dev->print_name(), strerror_dev(dev));
      goto bail_out;
   } else {
      Dmsg2(30, "Wrote label of %d bytes to %s\n", dcr->rec->data_len, dev->print_name());
   }

   Dmsg0(99, "Call write_block_to_dev()\n");
   if (!write_block_to_dev(dcr)) {
      Dmsg2(30, "Bad Label write on %s: ERR=%s\n", dev->print_name(), strerror_dev(dev));
      goto bail_out;
   }
   Dmsg0(99, " Wrote block to device\n");

   if (weof_dev(dev, 1) == 0) {
      dev->set_labeled();
      write_ansi_ibm_labels(dcr, ANSI_EOF_LABEL, dev->VolHdr.VolumeName);
   }

   if (debug_level >= 20)  {
      dump_volume_label(dev);
   }
   dev->clear_append();               /* remove append since this is PRE_LABEL */
   return true;

bail_out:
   memset(&dev->VolHdr, 0, sizeof(dev->VolHdr));
   dev->clear_append();               /* remove append since this is PRE_LABEL */
   return false;
}

/*
 * Write a volume label. This is ONLY called if we have a valid Bacula
 *   label of type PRE_LABEL;
 *  Returns: true if OK
 *           false if unable to write it
 */
bool rewrite_volume_label(DCR *dcr, bool recycle)
{
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;

   Dmsg2(190, "set append found freshly labeled volume. fd=%d dev=%x\n", dev->fd, dev);
   dev->VolHdr.LabelType = VOL_LABEL; /* set Volume label */
   dev->set_append();
   if (!write_volume_label_to_block(dcr)) {
      Dmsg0(200, "Error from write volume label.\n");
      return false;
   }
   /*
    * If we are not dealing with a streaming device,
    *  write the block now to ensure we have write permission.
    *  It is better to find out now rather than later.
    * We do not write the block now if this is an ANSI label. This
    *  avoids re-writing the ANSI label, which we do not want to do.
    */
   if (!dev_cap(dev, CAP_STREAM)) {
      if (!rewind_dev(dev)) {
         Jmsg2(jcr, M_WARNING, 0, _("Rewind error on device %s: ERR=%s\n"),
               dev->print_name(), strerror_dev(dev));
      }
      if (recycle) {
         if (!truncate_dev(dcr)) {
            Jmsg2(jcr, M_WARNING, 0, _("Truncate error on device %s: ERR=%s\n"),
                  dev->print_name(), strerror_dev(dev));
         }
      }

      /*
       * If we have already detected an ANSI label, re-read it
       *   to skip past it. Otherwise, we write a new one if 
       *   so requested.  
       */
      if (dev->label_type != B_BACULA_LABEL) {
         if (read_ansi_ibm_label(dcr) != VOL_OK) {
            rewind_dev(dev);
            return false;
         }
      } else if (!write_ansi_ibm_labels(dcr, ANSI_VOL_LABEL, dev->VolHdr.VolumeName)) {
         return false;
      }

      /* Attempt write to check write permission */
      Dmsg1(200, "Attempt to write to device fd=%d.\n", dev->fd);
      if (!write_block_to_dev(dcr)) {
         Jmsg2(jcr, M_ERROR, 0, _("Unable to write device %s: ERR=%s\n"),
            dev->print_name(), strerror_dev(dev));
         Dmsg0(200, "===ERROR write block to dev\n");
         return false;
      }
   }
   /* Set or reset Volume statistics */
   dev->VolCatInfo.VolCatJobs = 0;
   dev->VolCatInfo.VolCatFiles = 0;
   dev->VolCatInfo.VolCatBytes = 1;
   dev->VolCatInfo.VolCatErrors = 0;
   dev->VolCatInfo.VolCatBlocks = 0;
   dev->VolCatInfo.VolCatRBytes = 0;
   if (recycle) {
      dev->VolCatInfo.VolCatMounts++;
      dev->VolCatInfo.VolCatRecycles++;
   } else {
      dev->VolCatInfo.VolCatMounts = 1;
      dev->VolCatInfo.VolCatRecycles = 0;
      dev->VolCatInfo.VolCatWrites = 1;
      dev->VolCatInfo.VolCatReads = 1;
   }
   Dmsg0(100, "dir_update_vol_info. Set Append\n");
   bstrncpy(dev->VolCatInfo.VolCatStatus, "Append", sizeof(dev->VolCatInfo.VolCatStatus));
   if (!dir_update_volume_info(dcr, true)) {  /* indicate doing relabel */
      return false;
   }
   if (recycle) {
      Jmsg(jcr, M_INFO, 0, _("Recycled volume \"%s\" on device %s, all previous data lost.\n"),
         dcr->VolumeName, dev->print_name());
   } else {
      Jmsg(jcr, M_INFO, 0, _("Wrote label to prelabeled Volume \"%s\" on device %s\n"),
         dcr->VolumeName, dev->print_name());
   }
   /*
    * End writing real Volume label (from pre-labeled tape), or recycling
    *  the volume.
    */
   Dmsg0(200, "OK from rewite vol label.\n");
   return true;
}


/*
 *  create_volume_label_record
 *   Serialize label (from dev->VolHdr structure) into device record.
 *   Assumes that the dev->VolHdr structure is properly
 *   initialized.
*/
static void create_volume_label_record(DCR *dcr, DEV_RECORD *rec)
{
   ser_declare;
   struct date_time dt;
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;

   /* Serialize the label into the device record. */

   rec->data = check_pool_memory_size(rec->data, SER_LENGTH_Volume_Label);
   ser_begin(rec->data, SER_LENGTH_Volume_Label);
   ser_string(dev->VolHdr.Id);

   ser_uint32(dev->VolHdr.VerNum);

   if (dev->VolHdr.VerNum >= 11) {
      ser_btime(dev->VolHdr.label_btime);
      dev->VolHdr.write_btime = get_current_btime();
      ser_btime(dev->VolHdr.write_btime);
      dev->VolHdr.write_date = 0;
      dev->VolHdr.write_time = 0;
   } else {
      /* OLD WAY DEPRECATED */
      ser_float64(dev->VolHdr.label_date);
      ser_float64(dev->VolHdr.label_time);
      get_current_time(&dt);
      dev->VolHdr.write_date = dt.julian_day_number;
      dev->VolHdr.write_time = dt.julian_day_fraction;
   }
   ser_float64(dev->VolHdr.write_date);   /* 0 if VerNum >= 11 */
   ser_float64(dev->VolHdr.write_time);   /* 0  if VerNum >= 11 */

   ser_string(dev->VolHdr.VolumeName);
   ser_string(dev->VolHdr.PrevVolumeName);
   ser_string(dev->VolHdr.PoolName);
   ser_string(dev->VolHdr.PoolType);
   ser_string(dev->VolHdr.MediaType);

   ser_string(dev->VolHdr.HostName);
   ser_string(dev->VolHdr.LabelProg);
   ser_string(dev->VolHdr.ProgVersion);
   ser_string(dev->VolHdr.ProgDate);

   ser_end(rec->data, SER_LENGTH_Volume_Label);
   rec->data_len = ser_length(rec->data);
   rec->FileIndex = dev->VolHdr.LabelType;
   rec->VolSessionId = jcr->VolSessionId;
   rec->VolSessionTime = jcr->VolSessionTime;
   rec->Stream = jcr->NumVolumes;
   Dmsg2(100, "Created Vol label rec: FI=%s len=%d\n", FI_to_ascii(rec->FileIndex),
      rec->data_len);
}


/*
 * Create a volume label in memory
 */
void create_volume_label(DEVICE *dev, const char *VolName, const char *PoolName)
{
   DEVRES *device = (DEVRES *)dev->device;

   Dmsg0(90, "Start create_volume_label()\n");

   ASSERT(dev != NULL);

   memset(&dev->VolHdr, 0, sizeof(dev->VolHdr));

   bstrncpy(dev->VolHdr.Id, BaculaId, sizeof(dev->VolHdr.Id));
   dev->VolHdr.VerNum = BaculaTapeVersion;
   dev->VolHdr.LabelType = PRE_LABEL;  /* Mark tape as unused */
   bstrncpy(dev->VolHdr.VolumeName, VolName, sizeof(dev->VolHdr.VolumeName));
   bstrncpy(dev->VolHdr.PoolName, PoolName, sizeof(dev->VolHdr.PoolName));
   bstrncpy(dev->VolHdr.MediaType, device->media_type, sizeof(dev->VolHdr.MediaType));

   bstrncpy(dev->VolHdr.PoolType, "Backup", sizeof(dev->VolHdr.PoolType));

   dev->VolHdr.label_btime = get_current_btime();
   dev->VolHdr.label_date = 0;
   dev->VolHdr.label_time = 0;

   if (gethostname(dev->VolHdr.HostName, sizeof(dev->VolHdr.HostName)) != 0) {
      dev->VolHdr.HostName[0] = 0;
   }
   bstrncpy(dev->VolHdr.LabelProg, my_name, sizeof(dev->VolHdr.LabelProg));
   sprintf(dev->VolHdr.ProgVersion, "Ver. %s %s", VERSION, BDATE);
   sprintf(dev->VolHdr.ProgDate, "Build %s %s", __DATE__, __TIME__);
   dev->set_labeled();               /* set has Bacula label */
   if (debug_level >= 90) {
      dump_volume_label(dev);
   }
}

/*
 * Create session label
 *  The pool memory must be released by the calling program
 */
void create_session_label(DCR *dcr, DEV_RECORD *rec, int label)
{
   JCR *jcr = dcr->jcr;
   ser_declare;

   rec->VolSessionId   = jcr->VolSessionId;
   rec->VolSessionTime = jcr->VolSessionTime;
   rec->Stream         = jcr->JobId;

   rec->data = check_pool_memory_size(rec->data, SER_LENGTH_Session_Label);
   ser_begin(rec->data, SER_LENGTH_Session_Label);
   ser_string(BaculaId);
   ser_uint32(BaculaTapeVersion);

   ser_uint32(jcr->JobId);

   /* Changed in VerNum 11 */
   ser_btime(get_current_btime());
   ser_float64(0);

   ser_string(dcr->pool_name);
   ser_string(dcr->pool_type);
   ser_string(jcr->job_name);         /* base Job name */
   ser_string(jcr->client_name);

   /* Added in VerNum 10 */
   ser_string(jcr->Job);              /* Unique name of this Job */
   ser_string(jcr->fileset_name);
   ser_uint32(jcr->JobType);
   ser_uint32(jcr->JobLevel);
   /* Added in VerNum 11 */
   ser_string(jcr->fileset_md5);

   if (label == EOS_LABEL) {
      ser_uint32(jcr->JobFiles);
      ser_uint64(jcr->JobBytes);
      ser_uint32(dcr->StartBlock);
      ser_uint32(dcr->EndBlock);
      ser_uint32(dcr->StartFile);
      ser_uint32(dcr->EndFile);
      ser_uint32(jcr->JobErrors);

      /* Added in VerNum 11 */
      ser_uint32(jcr->JobStatus);
   }
   ser_end(rec->data, SER_LENGTH_Session_Label);
   rec->data_len = ser_length(rec->data);
}

/* Write session label
 *  Returns: false on failure
 *           true  on success
 */
bool write_session_label(DCR *dcr, int label)
{
   JCR *jcr = dcr->jcr;
   DEVICE *dev = dcr->dev;
   DEV_RECORD *rec;
   DEV_BLOCK *block = dcr->block;

   rec = new_record();
   Dmsg1(90, "session_label record=%x\n", rec);
   switch (label) {
   case SOS_LABEL:
      if (dev->is_tape()) {
         dcr->StartBlock = dev->block_num;
         dcr->StartFile  = dev->file;
      } else {
         dcr->StartBlock = (uint32_t)dev->file_addr;
         dcr->StartFile = (uint32_t)(dev->file_addr >> 32);
      }
      break;
   case EOS_LABEL:
      if (dev->is_tape()) {
         dcr->EndBlock = dev->EndBlock;
         dcr->EndFile  = dev->EndFile;
      } else {
         dcr->EndBlock = (uint32_t)dev->file_addr;
         dcr->EndFile = (uint32_t)(dev->file_addr >> 32);
      }
      break;
   default:
      Jmsg1(jcr, M_ABORT, 0, _("Bad session label = %d\n"), label);
      break;
   }
   create_session_label(dcr, rec, label);
   rec->FileIndex = label;

   /*
    * We guarantee that the session record can totally fit
    *  into a block. If not, write the block, and put it in
    *  the next block. Having the sesssion record totally in
    *  one block makes reading them much easier (no need to
    *  read the next block).
    */
   if (!can_write_record_to_block(block, rec)) {
      Dmsg0(100, "Cannot write session label to block.\n");
      if (!write_block_to_device(dcr)) {
         Dmsg0(90, "Got session label write_block_to_dev error.\n");
         /* ****FIXME***** errno is not set here */
         Jmsg(jcr, M_FATAL, 0, _("Error writing Session label to %s: %s\n"),
                           dev_vol_name(dev), strerror(errno));
         free_record(rec);
         return false;
      }
   }
   if (!write_record_to_block(block, rec)) {
      Jmsg(jcr, M_FATAL, 0, _("Error writing Session label to %s: %s\n"),
                        dev_vol_name(dev), strerror(errno));
      free_record(rec);
      return false;
   }

   Dmsg6(20, "Write sesson_label record JobId=%d FI=%s SessId=%d Strm=%s len=%d "
             "remainder=%d\n", jcr->JobId,
      FI_to_ascii(rec->FileIndex), rec->VolSessionId,
      stream_to_ascii(rec->Stream, rec->FileIndex), rec->data_len,
      rec->remainder);

   free_record(rec);
   Dmsg2(20, "Leave write_session_label Block=%d File=%d\n",
      dev->block_num, dev->file);
   return true;
}

/*  unser_volume_label
 *
 * Unserialize the Bacula Volume label into the device Volume_Label
 * structure.
 *
 * Assumes that the record is already read.
 *
 * Returns: false on error
 *          true  on success
*/

bool unser_volume_label(DEVICE *dev, DEV_RECORD *rec)
{
   ser_declare;

   if (rec->FileIndex != VOL_LABEL && rec->FileIndex != PRE_LABEL) {
      Mmsg3(dev->errmsg, _("Expecting Volume Label, got FI=%s Stream=%s len=%d\n"),
              FI_to_ascii(rec->FileIndex),
              stream_to_ascii(rec->Stream, rec->FileIndex),
              rec->data_len);
      if (!forge_on) {
         return false;
      }
   }

   dev->VolHdr.LabelType = rec->FileIndex;
   dev->VolHdr.LabelSize = rec->data_len;


   /* Unserialize the record into the Volume Header */
   rec->data = check_pool_memory_size(rec->data, SER_LENGTH_Volume_Label);
   ser_begin(rec->data, SER_LENGTH_Volume_Label);
   unser_string(dev->VolHdr.Id);
   unser_uint32(dev->VolHdr.VerNum);

   if (dev->VolHdr.VerNum >= 11) {
      unser_btime(dev->VolHdr.label_btime);
      unser_btime(dev->VolHdr.write_btime);
   } else { /* old way */
      unser_float64(dev->VolHdr.label_date);
      unser_float64(dev->VolHdr.label_time);
   }
   unser_float64(dev->VolHdr.write_date);    /* Unused with VerNum >= 11 */
   unser_float64(dev->VolHdr.write_time);    /* Unused with VerNum >= 11 */

   unser_string(dev->VolHdr.VolumeName);
   unser_string(dev->VolHdr.PrevVolumeName);
   unser_string(dev->VolHdr.PoolName);
   unser_string(dev->VolHdr.PoolType);
   unser_string(dev->VolHdr.MediaType);

   unser_string(dev->VolHdr.HostName);
   unser_string(dev->VolHdr.LabelProg);
   unser_string(dev->VolHdr.ProgVersion);
   unser_string(dev->VolHdr.ProgDate);

   ser_end(rec->data, SER_LENGTH_Volume_Label);
   Dmsg0(90, "unser_vol_label\n");
   if (debug_level >= 90) {
      dump_volume_label(dev);
   }
   return true;
}


bool unser_session_label(SESSION_LABEL *label, DEV_RECORD *rec)
{
   ser_declare;

   rec->data = check_pool_memory_size(rec->data, SER_LENGTH_Session_Label);
   unser_begin(rec->data, SER_LENGTH_Session_Label);
   unser_string(label->Id);
   unser_uint32(label->VerNum);
   unser_uint32(label->JobId);
   if (label->VerNum >= 11) {
      unser_btime(label->write_btime);
   } else {
      unser_float64(label->write_date);
   }
   unser_float64(label->write_time);
   unser_string(label->PoolName);
   unser_string(label->PoolType);
   unser_string(label->JobName);
   unser_string(label->ClientName);
   if (label->VerNum >= 10) {
      unser_string(label->Job);          /* Unique name of this Job */
      unser_string(label->FileSetName);
      unser_uint32(label->JobType);
      unser_uint32(label->JobLevel);
   }
   if (label->VerNum >= 11) {
      unser_string(label->FileSetMD5);
   } else {
      label->FileSetMD5[0] = 0;
   }
   if (rec->FileIndex == EOS_LABEL) {
      unser_uint32(label->JobFiles);
      unser_uint64(label->JobBytes);
      unser_uint32(label->StartBlock);
      unser_uint32(label->EndBlock);
      unser_uint32(label->StartFile);
      unser_uint32(label->EndFile);
      unser_uint32(label->JobErrors);
      if (label->VerNum >= 11) {
         unser_uint32(label->JobStatus);
      } else {
         label->JobStatus = JS_Terminated; /* kludge */
      }
   }
   return true;
}

void dump_volume_label(DEVICE *dev)
{
   int dbl = debug_level;
   uint32_t File;
   const char *LabelType;
   char buf[30];
   struct tm tm;
   struct date_time dt;

   debug_level = 1;
   File = dev->file;
   switch (dev->VolHdr.LabelType) {
   case PRE_LABEL:
      LabelType = "PRE_LABEL";
      break;
   case VOL_LABEL:
      LabelType = "VOL_LABEL";
      break;
   case EOM_LABEL:
      LabelType = "EOM_LABEL";
      break;
   case SOS_LABEL:
      LabelType = "SOS_LABEL";
      break;
   case EOS_LABEL:
      LabelType = "EOS_LABEL";
      break;
   case EOT_LABEL:
      goto bail_out;
   default:
      LabelType = buf;
      sprintf(buf, "Unknown %d", dev->VolHdr.LabelType);
      break;
   }

   Pmsg11(-1, "\nVolume Label:\n"
"Id                : %s"
"VerNo             : %d\n"
"VolName           : %s\n"
"PrevVolName       : %s\n"
"VolFile           : %d\n"
"LabelType         : %s\n"
"LabelSize         : %d\n"
"PoolName          : %s\n"
"MediaType         : %s\n"
"PoolType          : %s\n"
"HostName          : %s\n"
"",
             dev->VolHdr.Id, dev->VolHdr.VerNum,
             dev->VolHdr.VolumeName, dev->VolHdr.PrevVolumeName,
             File, LabelType, dev->VolHdr.LabelSize,
             dev->VolHdr.PoolName, dev->VolHdr.MediaType,
             dev->VolHdr.PoolType, dev->VolHdr.HostName);

   if (dev->VolHdr.VerNum >= 11) {
      char dt[50];
      bstrftime(dt, sizeof(dt), btime_to_unix(dev->VolHdr.label_btime));
      Pmsg1(-1, "Date label written: %s\n", dt);
   } else {
   dt.julian_day_number   = dev->VolHdr.label_date;
   dt.julian_day_fraction = dev->VolHdr.label_time;
   tm_decode(&dt, &tm);
   Pmsg5(-1,
"Date label written: %04d-%02d-%02d at %02d:%02d\n",
      tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min);
   }

bail_out:
   debug_level = dbl;
}


static void dump_session_label(DEV_RECORD *rec, const char *type)
{
   int dbl;
   struct date_time dt;
   struct tm tm;
   SESSION_LABEL label;
   char ec1[30], ec2[30], ec3[30], ec4[30], ec5[30], ec6[30], ec7[30];

   unser_session_label(&label, rec);
   dbl = debug_level;
   debug_level = 1;
   Pmsg7(-1, "\n%s Record:\n"
"JobId             : %d\n"
"VerNum            : %d\n"
"PoolName          : %s\n"
"PoolType          : %s\n"
"JobName           : %s\n"
"ClientName        : %s\n"
"",    type, label.JobId, label.VerNum,
      label.PoolName, label.PoolType,
      label.JobName, label.ClientName);

   if (label.VerNum >= 10) {
      Pmsg4(-1, ""
"Job (unique name) : %s\n"
"FileSet           : %s\n"
"JobType           : %c\n"
"JobLevel          : %c\n"
"", label.Job, label.FileSetName, label.JobType, label.JobLevel);
   }

   if (rec->FileIndex == EOS_LABEL) {
      Pmsg8(-1, ""
"JobFiles          : %s\n"
"JobBytes          : %s\n"
"StartBlock        : %s\n"
"EndBlock          : %s\n"
"StartFile         : %s\n"
"EndFile           : %s\n"
"JobErrors         : %s\n"
"JobStatus         : %c\n"
"",
         edit_uint64_with_commas(label.JobFiles, ec1),
         edit_uint64_with_commas(label.JobBytes, ec2),
         edit_uint64_with_commas(label.StartBlock, ec3),
         edit_uint64_with_commas(label.EndBlock, ec4),
         edit_uint64_with_commas(label.StartFile, ec5),
         edit_uint64_with_commas(label.EndFile, ec6),
         edit_uint64_with_commas(label.JobErrors, ec7),
         label.JobStatus);
   }
   if (label.VerNum >= 11) {
      char dt[50];
      bstrftime(dt, sizeof(dt), btime_to_unix(label.write_btime));
      Pmsg1(-1, _("Date written      : %s\n"), dt);
   } else {
      dt.julian_day_number   = label.write_date;
      dt.julian_day_fraction = label.write_time;
      tm_decode(&dt, &tm);
      Pmsg5(-1, _(""
"Date written      : %04d-%02d-%02d at %02d:%02d\n"),
      tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min);
   }

   debug_level = dbl;
}

void dump_label_record(DEVICE *dev, DEV_RECORD *rec, int verbose)
{
   const char *type;
   int dbl;

   if (rec->FileIndex == 0 && rec->VolSessionId == 0 && rec->VolSessionTime == 0) {
      return;
   }
   dbl = debug_level;
   debug_level = 1;
   switch (rec->FileIndex) {
   case PRE_LABEL:
      type = _("Fresh Volume");
      break;
   case VOL_LABEL:
      type = _("Volume");
      break;
   case SOS_LABEL:
      type = _("Begin Job Session");
      break;
   case EOS_LABEL:
      type = _("End Job Session");
      break;
   case EOM_LABEL:
      type = _("End of Media");
      break;
   case EOT_LABEL:
      type = ("End of Tape");
      break;
   default:
      type = _("Unknown");
      break;
   }
   if (verbose) {
      switch (rec->FileIndex) {
      case PRE_LABEL:
      case VOL_LABEL:
         unser_volume_label(dev, rec);
         dump_volume_label(dev);
         break;
      case SOS_LABEL:
         dump_session_label(rec, type);
         break;
      case EOS_LABEL:
         dump_session_label(rec, type);
         break;
      case EOM_LABEL:
         Pmsg7(-1, "%s Record: File:blk=%u:%u SessId=%d SessTime=%d JobId=%d DataLen=%d\n",
            type, dev->file, dev->block_num, rec->VolSessionId, 
            rec->VolSessionTime, rec->Stream, rec->data_len);
         break;
      case EOT_LABEL:
         Pmsg0(-1, _("End of physical tape.\n"));
         break;
      default:
         Pmsg7(-1, "%s Record: File:blk=%u:%u SessId=%d SessTime=%d JobId=%d DataLen=%d\n",
            type, dev->file, dev->block_num, rec->VolSessionId, 
            rec->VolSessionTime, rec->Stream, rec->data_len);
         break;
      }
   } else {
      SESSION_LABEL label;
      char dt[50];
      switch (rec->FileIndex) {
      case SOS_LABEL:
         unser_session_label(&label, rec);
         bstrftimes(dt, sizeof(dt), btime_to_unix(label.write_btime));
         Pmsg6(-1, "%s Record: File:blk=%u:%u SessId=%d SessTime=%d JobId=%d\n",
            type, dev->file, dev->block_num, rec->VolSessionId, rec->VolSessionTime, label.JobId);
         Pmsg4(-1, "   Job=%s Date=%s Level=%c Type=%c\n",
            label.Job, dt, label.JobLevel, label.JobType);
         break;
      case EOS_LABEL:
         char ed1[30], ed2[30];
         unser_session_label(&label, rec);
         bstrftimes(dt, sizeof(dt), btime_to_unix(label.write_btime));
         Pmsg6(-1, "%s Record: File:blk=%u:%u SessId=%d SessTime=%d JobId=%d\n",
            type, dev->file, dev->block_num, rec->VolSessionId, rec->VolSessionTime, label.JobId);
         Pmsg7(-1, "   Date=%s Level=%c Type=%c Files=%s Bytes=%s Errors=%d Status=%c\n",
            dt, label.JobLevel, label.JobType,
            edit_uint64_with_commas(label.JobFiles, ed1),
            edit_uint64_with_commas(label.JobBytes, ed2),
            label.JobErrors, (char)label.JobStatus);
         break;
      case EOM_LABEL:
      case PRE_LABEL:
      case VOL_LABEL:
      default:
         Pmsg7(-1, "%s Record: File:blk=%u:%u SessId=%d SessTime=%d JobId=%d DataLen=%d\n",
            type, dev->file, dev->block_num, rec->VolSessionId, rec->VolSessionTime, 
            rec->Stream, rec->data_len);
         break;
      case EOT_LABEL:
         break;
      }
   }
   debug_level = dbl;
}
