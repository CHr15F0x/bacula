/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2001-2008 Free Software Foundation Europe e.V.

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
 *
 *   Bacula Director -- catreq.c -- handles the message channel
 *    catalog request from the Storage daemon.
 *
 *     Kern Sibbald, March MMI
 *
 *    This routine runs as a thread and must be thread reentrant.
 *
 *  Basic tasks done here:
 *      Handle Catalog services.
 *
 *   Version $Id$
 */

#include "bacula.h"
#include "dird.h"
#include "findlib/find.h"

/*
 * Handle catalog request
 *  For now, we simply return next Volume to be used
 */

/* Requests from the Storage daemon */
static char Find_media[] = "CatReq Job=%127s FindMedia=%d pool_name=%127s media_type=%127s\n";
static char Get_Vol_Info[] = "CatReq Job=%127s GetVolInfo VolName=%127s write=%d\n";

static char Update_media[] = "CatReq Job=%127s UpdateMedia VolName=%s"
   " VolJobs=%u VolFiles=%u VolBlocks=%u VolBytes=%lld VolMounts=%u"
   " VolErrors=%u VolWrites=%u MaxVolBytes=%lld EndTime=%lld VolStatus=%10s"
   " Slot=%d relabel=%d InChanger=%d VolReadTime=%lld VolWriteTime=%lld"
   " VolFirstWritten=%lld VolParts=%u\n";

static char Create_job_media[] = "CatReq Job=%127s CreateJobMedia "
   " FirstIndex=%u LastIndex=%u StartFile=%u EndFile=%u "
   " StartBlock=%u EndBlock=%u Copy=%d Strip=%d MediaId=%" lld "\n";


/* Responses  sent to Storage daemon */
static char OK_media[] = "1000 OK VolName=%s VolJobs=%u VolFiles=%u"
   " VolBlocks=%u VolBytes=%s VolMounts=%u VolErrors=%u VolWrites=%u"
   " MaxVolBytes=%s VolCapacityBytes=%s VolStatus=%s Slot=%d"
   " MaxVolJobs=%u MaxVolFiles=%u InChanger=%d VolReadTime=%s"
   " VolWriteTime=%s EndFile=%u EndBlock=%u VolParts=%u LabelType=%d"
   " MediaId=%s\n";

static char OK_create[] = "1000 OK CreateJobMedia\n";


static int send_volume_info_to_storage_daemon(JCR *jcr, BSOCK *sd, MEDIA_DBR *mr)
{
   int stat;
   char ed1[50], ed2[50], ed3[50], ed4[50], ed5[50], ed6[50];

   jcr->MediaId = mr->MediaId;
   pm_strcpy(jcr->VolumeName, mr->VolumeName);
   bash_spaces(mr->VolumeName);
   stat = sd->fsend(OK_media, mr->VolumeName, mr->VolJobs,
      mr->VolFiles, mr->VolBlocks, edit_uint64(mr->VolBytes, ed1),
      mr->VolMounts, mr->VolErrors, mr->VolWrites,
      edit_uint64(mr->MaxVolBytes, ed2),
      edit_uint64(mr->VolCapacityBytes, ed3),
      mr->VolStatus, mr->Slot, mr->MaxVolJobs, mr->MaxVolFiles,
      mr->InChanger,
      edit_int64(mr->VolReadTime, ed4),
      edit_int64(mr->VolWriteTime, ed5),
      mr->EndFile, mr->EndBlock,
      mr->VolParts,
      mr->LabelType,
      edit_uint64(mr->MediaId, ed6));
   unbash_spaces(mr->VolumeName);
   Dmsg2(100, "Vol Info for %s: %s", jcr->Job, sd->msg);
   return stat;
}

void catalog_request(JCR *jcr, BSOCK *bs)
{
   MEDIA_DBR mr, sdmr;
   JOBMEDIA_DBR jm;
   char Job[MAX_NAME_LENGTH];
   char pool_name[MAX_NAME_LENGTH];
   int index, ok, label, writing;
   POOLMEM *omsg;
   POOL_DBR pr;
   uint32_t Stripe, Copy;
   uint64_t MediaId;
   utime_t VolFirstWritten;
   utime_t VolLastWritten;

   memset(&mr, 0, sizeof(mr));
   memset(&sdmr, 0, sizeof(sdmr));
   memset(&jm, 0, sizeof(jm));
   Dsm_check(100);      

   /*
    * Request to find next appendable Volume for this Job
    */
   Dmsg1(100, "catreq %s", bs->msg);
   if (!jcr->db) {
      omsg = get_memory(bs->msglen+1);
      pm_strcpy(omsg, bs->msg);
      bs->fsend(_("1990 Invalid Catalog Request: %s"), omsg);    
      Jmsg1(jcr, M_FATAL, 0, _("Invalid Catalog request; DB not open: %s"), omsg);
      free_memory(omsg);
      return;
   }
   /*
    * Find next appendable medium for SD
    */
   if (sscanf(bs->msg, Find_media, &Job, &index, &pool_name, &mr.MediaType) == 4) {
      memset(&pr, 0, sizeof(pr));
      bstrncpy(pr.Name, pool_name, sizeof(pr.Name));
      unbash_spaces(pr.Name);
      ok = db_get_pool_record(jcr, jcr->db, &pr);
      if (ok) {
         mr.PoolId = pr.PoolId;
         mr.StorageId = jcr->wstore->StorageId;
         mr.ScratchPoolId = pr.ScratchPoolId;
         ok = find_next_volume_for_append(jcr, &mr, index, fnv_create_vol, fnv_prune);
         Dmsg3(050, "find_media ok=%d idx=%d vol=%s\n", ok, index, mr.VolumeName);
      }
      /*
       * Send Find Media response to Storage daemon
       */
      if (ok) {
         send_volume_info_to_storage_daemon(jcr, bs, &mr);
      } else {
         bs->fsend(_("1901 No Media.\n"));
         Dmsg0(500, "1901 No Media.\n");
      }

   /*
    * Request to find specific Volume information
    */
   } else if (sscanf(bs->msg, Get_Vol_Info, &Job, &mr.VolumeName, &writing) == 3) {
      Dmsg1(100, "CatReq GetVolInfo Vol=%s\n", mr.VolumeName);
      /*
       * Find the Volume
       */
      unbash_spaces(mr.VolumeName);
      if (db_get_media_record(jcr, jcr->db, &mr)) {
         const char *reason = NULL;           /* detailed reason for rejection */
         /*
          * If we are reading, accept any volume (reason == NULL)
          * If we are writing, check if the Volume is valid
          *   for this job, and do a recycle if necessary
          */
         if (writing) {
            /*
             * SD wants to write this Volume, so make
             *   sure it is suitable for this job, i.e.
             *   Pool matches, and it is either Append or Recycle
             *   and Media Type matches and Pool allows any volume.
             */
            if (mr.PoolId != jcr->jr.PoolId) {
               reason = _("not in Pool");
            } else if (strcmp(mr.MediaType, jcr->wstore->media_type) != 0) {
               reason = _("not correct MediaType");
            } else {
               /*
                * Now try recycling if necessary
                *   reason set non-NULL if we cannot use it
                */
               check_if_volume_valid_or_recyclable(jcr, &mr, &reason);
            }
         }
         if (!reason && mr.Enabled != 1) {
            reason = _("is not Enabled");
         }
         if (reason == NULL) {
            /*
             * Send Find Media response to Storage daemon
             */
            send_volume_info_to_storage_daemon(jcr, bs, &mr);
         } else {
            /* Not suitable volume */
            bs->fsend(_("1998 Volume \"%s\" status is %s, %s.\n"), mr.VolumeName,
               mr.VolStatus, reason);
         }

      } else {
         bs->fsend(_("1997 Volume \"%s\" not in catalog.\n"), mr.VolumeName);
         Dmsg1(100, "1997 Volume \"%s\" not in catalog.\n", mr.VolumeName);
      }

   /*
    * Request to update Media record. Comes typically at the end
    *  of a Storage daemon Job Session, when labeling/relabeling a
    *  Volume, or when an EOF mark is written.
    */
   } else if (sscanf(bs->msg, Update_media, &Job, &sdmr.VolumeName,
      &sdmr.VolJobs, &sdmr.VolFiles, &sdmr.VolBlocks, &sdmr.VolBytes,
      &sdmr.VolMounts, &sdmr.VolErrors, &sdmr.VolWrites, &sdmr.MaxVolBytes,
      &VolLastWritten, &sdmr.VolStatus, &sdmr.Slot, &label, &sdmr.InChanger,
      &sdmr.VolReadTime, &sdmr.VolWriteTime, &VolFirstWritten,
      &sdmr.VolParts) == 19) {

      db_lock(jcr->db);
      Dmsg3(400, "Update media %s oldStat=%s newStat=%s\n", sdmr.VolumeName,
         mr.VolStatus, sdmr.VolStatus);
      bstrncpy(mr.VolumeName, sdmr.VolumeName, sizeof(mr.VolumeName)); /* copy Volume name */
      unbash_spaces(mr.VolumeName);
      if (!db_get_media_record(jcr, jcr->db, &mr)) {
         Jmsg(jcr, M_ERROR, 0, _("Unable to get Media record for Volume %s: ERR=%s\n"),
              mr.VolumeName, db_strerror(jcr->db));
         bs->fsend(_("1991 Catalog Request for vol=%s failed: %s"),
            mr.VolumeName, db_strerror(jcr->db));
         db_unlock(jcr->db);
         return;
      }
      /* Set first written time if this is first job */
      if (mr.FirstWritten == 0) {
         if (VolFirstWritten == 0) {
            mr.FirstWritten = jcr->start_time;   /* use Job start time as first write */
         } else {
            mr.FirstWritten = VolFirstWritten;
         }
         mr.set_first_written = true;
      }
      /* If we just labeled the tape set time */
      if (label || mr.LabelDate == 0) {
         mr.LabelDate = jcr->start_time;
         mr.set_label_date = true;
         if (mr.InitialWrite == 0) {
            mr.InitialWrite = jcr->start_time;
         }
         Dmsg2(400, "label=%d labeldate=%d\n", label, mr.LabelDate);
      } else {
         /*
          * Insanity check for VolFiles get set to a smaller value
          */
         if (sdmr.VolFiles < mr.VolFiles) {
            Jmsg(jcr, M_FATAL, 0, _("Volume Files at %u being set to %u"
                 " for Volume \"%s\". This is incorrect.\n"),
               mr.VolFiles, sdmr.VolFiles, mr.VolumeName);
            bs->fsend(_("1992 Update Media error. VolFiles=%u, CatFiles=%u\n"),
               sdmr.VolFiles, mr.VolFiles);
            db_unlock(jcr->db);
            return;
         }
      }
      Dmsg2(400, "Update media: BefVolJobs=%u After=%u\n", mr.VolJobs, sdmr.VolJobs);

      /*
       * Check if the volume has been written by the job, 
       * and update the LastWritten field if needed.
       */
      if (mr.VolBlocks != sdmr.VolBlocks && VolLastWritten != 0) {
         mr.LastWritten = VolLastWritten;
      }

      /*
       * Update to point to the last device used to write the Volume.
       *   However, do so only if we are writing the tape, i.e.
       *   the number of VolWrites has increased.
       */
      if (jcr->wstore && jcr->wstore->StorageId && sdmr.VolWrites > mr.VolWrites) {
         mr.StorageId = jcr->wstore->StorageId;
      }

      /* Copy updated values to original media record */
      mr.VolJobs      = sdmr.VolJobs;
      mr.VolFiles     = sdmr.VolFiles;
      mr.VolBlocks    = sdmr.VolBlocks;
      mr.VolBytes     = sdmr.VolBytes;
      mr.VolMounts    = sdmr.VolMounts;
      mr.VolErrors    = sdmr.VolErrors;
      mr.VolWrites    = sdmr.VolWrites;
      mr.Slot         = sdmr.Slot;
      mr.InChanger    = sdmr.InChanger;
      mr.VolParts     = sdmr.VolParts;
      bstrncpy(mr.VolStatus, sdmr.VolStatus, sizeof(mr.VolStatus));
      if (sdmr.VolReadTime >= 0) { 
         mr.VolReadTime  = sdmr.VolReadTime;
      }
      if (sdmr.VolWriteTime >= 0) {
         mr.VolWriteTime = sdmr.VolWriteTime;
      }

      Dmsg2(400, "db_update_media_record. Stat=%s Vol=%s\n", mr.VolStatus, mr.VolumeName);
      /*
       * Update the database, then before sending the response to the
       *  SD, check if the Volume has expired.
       */
      if (!db_update_media_record(jcr, jcr->db, &mr)) {
         Jmsg(jcr, M_FATAL, 0, _("Catalog error updating Media record. %s"),
            db_strerror(jcr->db));
         bs->fsend(_("1993 Update Media error\n"));
         Dmsg0(400, "send error\n");
      } else {
         (void)has_volume_expired(jcr, &mr);
         send_volume_info_to_storage_daemon(jcr, bs, &mr);
      }
      db_unlock(jcr->db);

   /*
    * Request to create a JobMedia record
    */
   } else if (sscanf(bs->msg, Create_job_media, &Job,
      &jm.FirstIndex, &jm.LastIndex, &jm.StartFile, &jm.EndFile,
      &jm.StartBlock, &jm.EndBlock, &Copy, &Stripe, &MediaId) == 10) {

      if (jcr->mig_jcr) {
         jm.JobId = jcr->mig_jcr->JobId;
      } else {
         jm.JobId = jcr->JobId;
      }
      jm.MediaId = MediaId;
      Dmsg6(400, "create_jobmedia JobId=%d MediaId=%d SF=%d EF=%d FI=%d LI=%d\n",
         jm.JobId, jm.MediaId, jm.StartFile, jm.EndFile, jm.FirstIndex, jm.LastIndex);
      if (!db_create_jobmedia_record(jcr, jcr->db, &jm)) {
         Jmsg(jcr, M_FATAL, 0, _("Catalog error creating JobMedia record. %s"),
            db_strerror(jcr->db));
         bs->fsend(_("1992 Create JobMedia error\n"));
      } else {
         Dmsg0(400, "JobMedia record created\n");
         bs->fsend(OK_create);
      }

   } else {
      omsg = get_memory(bs->msglen+1);
      pm_strcpy(omsg, bs->msg);
      bs->fsend(_("1990 Invalid Catalog Request: %s"), omsg);
      Jmsg1(jcr, M_FATAL, 0, _("Invalid Catalog request: %s"), omsg);
      free_memory(omsg);
   }
   Dmsg1(400, ">CatReq response: %s", bs->msg);
   Dmsg1(400, "Leave catreq jcr 0x%x\n", jcr);
   return;
}

/*
 * Note, we receive the whole attribute record, but we select out only the stat
 * packet, VolSessionId, VolSessionTime, FileIndex, file type, and file name to
 * store in the catalog.
 */
static void update_attribute(JCR *jcr, char *msg, int32_t msglen)
{
   unser_declare;
   uint32_t VolSessionId, VolSessionTime;
   int32_t Stream;
   uint32_t FileIndex;
   uint32_t data_len;
   char *p;
   int len;
   char *fname, *attr;
   ATTR_DBR *ar = NULL;

   /* Start transaction allocates jcr->attr and jcr->ar if needed */
   db_start_transaction(jcr, jcr->db);     /* start transaction if not already open */
   ar = jcr->ar;      

   /* Start by scanning directly in the message buffer to get Stream   
    *  there may be a cached attr so we cannot yet write into
    *  jcr->attr or jcr->ar  
    */
   p = msg;
   skip_nonspaces(&p);                /* UpdCat */
   skip_spaces(&p);
   skip_nonspaces(&p);                /* Job=nnn */
   skip_spaces(&p);
   skip_nonspaces(&p);                /* FileAttributes */
   p += 1;
   unser_begin(p, 0);
   unser_uint32(VolSessionId);
   unser_uint32(VolSessionTime);
   unser_int32(FileIndex);
   unser_int32(Stream);
   unser_uint32(data_len);
   p += unser_length(p);

   Dmsg1(400, "UpdCat msg=%s\n", msg);
   Dmsg5(400, "UpdCat VolSessId=%d VolSessT=%d FI=%d Strm=%d data_len=%d\n",
      VolSessionId, VolSessionTime, FileIndex, Stream, data_len);

   if (Stream == STREAM_UNIX_ATTRIBUTES || Stream == STREAM_UNIX_ATTRIBUTES_EX) {
      if (jcr->cached_attribute) {
         Dmsg2(400, "Cached attr. Stream=%d fname=%s\n", ar->Stream, ar->fname);
         if (!db_create_attributes_record(jcr, jcr->db, ar)) {
            Jmsg1(jcr, M_FATAL, 0, _("Attribute create error. %s"), db_strerror(jcr->db));
         }
      }
      /* Any cached attr is flushed so we can reuse jcr->attr and jcr->ar */
      jcr->attr = check_pool_memory_size(jcr->attr, msglen);
      memcpy(jcr->attr, msg, msglen);
      p = jcr->attr - msg + p;    /* point p into jcr->attr */
      skip_nonspaces(&p);             /* skip FileIndex */
      skip_spaces(&p);
      ar->FileType = str_to_int32(p);     /* TODO: choose between unserialize and str_to_int32 */
      skip_nonspaces(&p);             /* skip FileType */
      skip_spaces(&p);
      fname = p;
      len = strlen(fname);        /* length before attributes */
      attr = &fname[len+1];

      Dmsg2(400, "dird<stored: stream=%d %s\n", Stream, fname);
      Dmsg1(400, "dird<stored: attr=%s\n", attr);
      ar->attr = attr;
      ar->fname = fname;
      if (ar->FileType == FT_DELETED) {
         ar->FileIndex = 0;     /* special value */
      } else {
         ar->FileIndex = FileIndex;
      }
      ar->Stream = Stream;
      ar->link = NULL;
      if (jcr->mig_jcr) {
         ar->JobId = jcr->mig_jcr->JobId;
      } else {
         ar->JobId = jcr->JobId;
      }
      ar->Digest = NULL;
      ar->DigestType = CRYPTO_DIGEST_NONE;
      jcr->cached_attribute = true;

      Dmsg2(400, "dird<filed: stream=%d %s\n", Stream, fname);
      Dmsg1(400, "dird<filed: attr=%s\n", attr);

   } else if (crypto_digest_stream_type(Stream) != CRYPTO_DIGEST_NONE) {
      fname = p;
      if (ar->FileIndex != FileIndex) {
         Jmsg(jcr, M_WARNING, 0, _("Got %s but not same File as attributes\n"), stream_to_ascii(Stream));
      } else {
         /* Update digest in catalog */
         char digestbuf[BASE64_SIZE(CRYPTO_DIGEST_MAX_SIZE)];
         int len = 0;
         int type = CRYPTO_DIGEST_NONE;

         switch(Stream) {
         case STREAM_MD5_DIGEST:
            len = CRYPTO_DIGEST_MD5_SIZE;
            type = CRYPTO_DIGEST_MD5;
            break;
         case STREAM_SHA1_DIGEST:
            len = CRYPTO_DIGEST_SHA1_SIZE;
            type = CRYPTO_DIGEST_SHA1;
            break;
         case STREAM_SHA256_DIGEST:
            len = CRYPTO_DIGEST_SHA256_SIZE;
            type = CRYPTO_DIGEST_SHA256;
            break;
         case STREAM_SHA512_DIGEST:
            len = CRYPTO_DIGEST_SHA512_SIZE;
            type = CRYPTO_DIGEST_SHA512;
            break;
         default:
            /* Never reached ... */
            Jmsg(jcr, M_ERROR, 0, _("Catalog error updating file digest. Unsupported digest stream type: %d"),
                 Stream);
         }

         bin_to_base64(digestbuf, sizeof(digestbuf), fname, len, true);
         Dmsg3(400, "DigestLen=%d Digest=%s type=%d\n", strlen(digestbuf),
               digestbuf, Stream);
         if (jcr->cached_attribute) {
            ar->Digest = digestbuf;
            ar->DigestType = type;
            Dmsg2(400, "Cached attr with digest. Stream=%d fname=%s\n",
                  ar->Stream, ar->fname);

            /* Update BaseFile table */
            if (!db_create_attributes_record(jcr, jcr->db, ar)) {
               Jmsg1(jcr, M_FATAL, 0, _("attribute create error. %s"),
                        db_strerror(jcr->db));
            }
            jcr->cached_attribute = false; 
         } else {
            if (!db_add_digest_to_file_record(jcr, jcr->db, ar->FileId, digestbuf, type)) {
               Jmsg(jcr, M_ERROR, 0, _("Catalog error updating file digest. %s"),
                  db_strerror(jcr->db));
            }
         }
      }
   }
}

/*
 * Update File Attributes in the catalog with data
 *  sent by the Storage daemon.
 */
void catalog_update(JCR *jcr, BSOCK *bs)
{
   if (!jcr->pool->catalog_files) {
      return;                         /* user disabled cataloging */
   }
   if (job_canceled(jcr)) {
      goto bail_out;
   }
   if (!jcr->db) {
      POOLMEM *omsg = get_memory(bs->msglen+1);
      pm_strcpy(omsg, bs->msg);
      bs->fsend(_("1994 Invalid Catalog Update: %s"), omsg);    
      Jmsg1(jcr, M_FATAL, 0, _("Invalid Catalog Update; DB not open: %s"), omsg);
      free_memory(omsg);
      goto bail_out;
   }
   update_attribute(jcr, bs->msg, bs->msglen);

bail_out:
   if (job_canceled(jcr)) {
      cancel_storage_daemon_job(jcr);
   }
}

/*
 * Update File Attributes in the catalog with data read from
 * the storage daemon spool file. We receive the filename and
 * we try to read it.
 */
bool despool_attributes_from_file(JCR *jcr, const char *file)
{
   bool ret=false;
   int32_t pktsiz;
   size_t nbytes;
   ssize_t last = 0, size = 0;
   int count = 0;
   int32_t msglen;                    /* message length */
   POOLMEM *msg = get_pool_memory(PM_MESSAGE);
   FILE *spool_fd=NULL;

   Dmsg0(100, "Begin despool_attributes_from_file\n");

   if (job_canceled(jcr) || !jcr->pool->catalog_files || !jcr->db) {
      goto bail_out;                  /* user disabled cataloging */
   }

   spool_fd = fopen(file, "rb");
   if (!spool_fd) {
      Dmsg0(100, "cancel despool_attributes_from_file\n");
      /* send an error message */
      goto bail_out;
   }
#if defined(HAVE_POSIX_FADVISE) && defined(POSIX_FADV_WILLNEED)
   posix_fadvise(fileno(spool_fd), 0, 0, POSIX_FADV_WILLNEED);
#endif

   while (fread((char *)&pktsiz, 1, sizeof(int32_t), spool_fd) ==
          sizeof(int32_t)) {
      size += sizeof(int32_t);
      msglen = ntohl(pktsiz);
      if (msglen > 0) {
         if (msglen > (int32_t) sizeof_pool_memory(msg)) {
            msg = realloc_pool_memory(msg, msglen + 1);
         }
         nbytes = fread(msg, 1, msglen, spool_fd);
         if (nbytes != (size_t) msglen) {
            berrno be;
            Dmsg2(400, "nbytes=%d msglen=%d\n", nbytes, msglen);
            Qmsg1(jcr, M_FATAL, 0, _("fread attr spool error. ERR=%s\n"),
                  be.bstrerror());
            goto bail_out;
         }
         size += nbytes;
         if ((++count & 0x3F) == 0) {
            last = size;
         }
      }
      if (!job_canceled(jcr)) {
         update_attribute(jcr, msg, msglen);
         if (job_canceled(jcr)) {
            goto bail_out;
         }
      }
   }
   if (ferror(spool_fd)) {
      berrno be;
      Qmsg1(jcr, M_FATAL, 0, _("fread attr spool error. ERR=%s\n"),
            be.bstrerror());
      goto bail_out;
   }
   ret = true;

bail_out:
   if (spool_fd) {
      fclose(spool_fd);
   }

   if (job_canceled(jcr)) {
      cancel_storage_daemon_job(jcr);
   }

   free_pool_memory(msg);
   Dmsg1(100, "End despool_attributes_from_file ret=%i\n", ret);
   return ret;
}
