/*
 *  Bacula File Daemon	restore.c Restorefiles.
 *
 *    Kern Sibbald, November MM
 *
 *   Version $Id$
 *
 */
/*
   Copyright (C) 2000-2003 Kern Sibbald and John Walker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of
   the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
   MA 02111-1307, USA.

 */

#include "bacula.h"
#include "filed.h"

/* Data received from Storage Daemon */
static char rec_header[] = "rechdr %ld %ld %ld %ld %ld";

/* Forward referenced functions */

#define RETRY 10		      /* retry wait time */

/* 
 * Restore the requested files.
 * 
 */
void do_restore(JCR *jcr)
{
   int wherelen;
   BSOCK *sd;
   int32_t stream;
   uint32_t size;
   uint32_t VolSessionId, VolSessionTime;
   int32_t file_index;
   int extract = FALSE;
   BFILE bfd;
   int stat;
   uint32_t total = 0;		      /* Job total but only 32 bits for debug */
   char *wbuf;			      /* write buffer */
   uint32_t wsize;		      /* write size */
   uint64_t fileAddr = 0;	      /* file write address */
   int non_support_data = 0;
   int non_support_attr = 0;
   int prog_name_msg = 0;
   ATTR *attr;
   
   wherelen = strlen(jcr->where);

   binit(&bfd);
   sd = jcr->store_bsock;
   set_jcr_job_status(jcr, JS_Running);

   if (!bnet_set_buffer_size(sd, MAX_NETWORK_BUFFER_SIZE, BNET_SETBUF_READ)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      return;
   }
   jcr->buf_size = sd->msglen;

   attr = new_attr();

#ifdef HAVE_LIBZ
   uint32_t compress_buf_size = jcr->buf_size + 12 + ((jcr->buf_size+999) / 1000) + 100;
   jcr->compress_buf = (char *)bmalloc(compress_buf_size);
#endif

   /* 
    * Get a record from the Storage daemon. We are guaranteed to 
    *	receive records in the following order:
    *	1. Stream record header
    *	2. Stream data
    *	     a. Attributes (Unix or Win32)
    *	 or  b. File data for the file
    *	 or  c. Possibly MD5 or SHA1 record
    *	3. Repeat step 1
    */
   while (bget_msg(sd) >= 0 && !job_canceled(jcr)) {
      /*
       * First we expect a Stream Record Header 
       */
      if (sscanf(sd->msg, rec_header, &VolSessionId, &VolSessionTime, &file_index,
	  &stream, &size) != 5) {
         Jmsg1(jcr, M_FATAL, 0, _("Record header scan error: %s\n"), sd->msg);
	 goto bail_out;
      }
      Dmsg2(30, "Got hdr: FilInx=%d Stream=%d.\n", file_index, stream);

      /* 
       * Now we expect the Stream Data
       */
      if (bget_msg(sd) < 0) {
         Jmsg1(jcr, M_FATAL, 0, _("Data record error. ERR=%s\n"), bnet_strerror(sd));
	 goto bail_out;
      }
      if (size != (uint32_t)sd->msglen) {
         Jmsg2(jcr, M_FATAL, 0, _("Actual data size %d not same as header %d\n"), sd->msglen, size);
	 goto bail_out;
      }
      Dmsg1(30, "Got stream data, len=%d\n", sd->msglen);

      /* File Attributes stream */
      switch (stream) {
      case STREAM_UNIX_ATTRIBUTES:
      case STREAM_UNIX_ATTRIBUTES_EX:

         Dmsg1(30, "Stream=Unix Attributes. extract=%d\n", extract);
	 /* If extracting, it was from previous stream, so
	  * close the output file.
	  */
	 if (extract) {
	    if (!is_bopen(&bfd)) {
               Jmsg0(jcr, M_ERROR, 0, _("Logic error output file should be open\n"));
	    }
	    set_attributes(jcr, attr, &bfd);
	    extract = FALSE;
            Dmsg0(30, "Stop extracting.\n");
	 }

	 if (!unpack_attributes_record(jcr, stream, sd->msg, attr)) {
	    goto bail_out;
	 }
	 if (file_index != attr->file_index) {
            Jmsg(jcr, M_FATAL, 0, _("Record header file index %ld not equal record index %ld\n"),
		 file_index, attr->file_index);
            Dmsg0(100, "File index error\n");
	    goto bail_out;
	 }
	    
         Dmsg3(200, "File %s\nattrib=%s\nattribsEx=%s\n", attr->fname, 
	       attr->attr, attr->attrEx);

	 attr->data_stream = decode_stat(attr->attr, &attr->statp, &attr->LinkFI);

	 if (!is_stream_supported(attr->data_stream)) {
	    if (!non_support_data++) {
               Jmsg(jcr, M_ERROR, 0, _("%s stream not supported on this Client.\n"),
		  stream_to_ascii(attr->data_stream));
	    }
	    continue;
	 }

	 build_attr_output_fnames(jcr, attr);

	 jcr->num_files_examined++;

         Dmsg1(30, "Outfile=%s\n", attr->ofname);
	 extract = FALSE;
	 stat = create_file(jcr, attr, &bfd, jcr->replace);
	 switch (stat) {
	 case CF_ERROR:
	 case CF_SKIP:
	    break;
	 case CF_EXTRACT:
	    extract = TRUE;
	    P(jcr->mutex);
	    pm_strcpy(&jcr->last_fname, attr->ofname);
	    V(jcr->mutex);
	    jcr->JobFiles++;
	    fileAddr = 0;
	    print_ls_output(jcr, attr);
	    /* Set attributes after file extracted */
	    break;
	 case CF_CREATED:
	    P(jcr->mutex);
	    pm_strcpy(&jcr->last_fname, attr->ofname);
	    V(jcr->mutex);
	    jcr->JobFiles++;
	    fileAddr = 0;
	    print_ls_output(jcr, attr);
	    /* set attributes now because file will not be extracted */
	    set_attributes(jcr, attr, &bfd);
	    break;
	 }  
	 break;

      /* Data stream */
      case STREAM_FILE_DATA:
      case STREAM_SPARSE_DATA:	
      case STREAM_WIN32_DATA:  

	 if (extract) {
	    if (stream == STREAM_SPARSE_DATA) {
	       ser_declare;
	       uint64_t faddr;
	       char ec1[50];

	       wbuf = sd->msg + SPARSE_FADDR_SIZE;
	       wsize = sd->msglen - SPARSE_FADDR_SIZE;
	       ser_begin(sd->msg, SPARSE_FADDR_SIZE);
	       unser_uint64(faddr);
	       if (fileAddr != faddr) {
		  fileAddr = faddr;
		  if (blseek(&bfd, (off_t)fileAddr, SEEK_SET) < 0) {
                     Jmsg3(jcr, M_ERROR, 0, _("Seek to %s error on %s: ERR=%s\n"),
			 edit_uint64(fileAddr, ec1), attr->ofname, berror(&bfd));
		     extract = FALSE;
		     continue;
		  }
	       }
	    } else {
	       wbuf = sd->msg;
	       wsize = sd->msglen;
	    }
            Dmsg2(30, "Write %u bytes, total before write=%u\n", wsize, total);
	    if ((uint32_t)bwrite(&bfd, wbuf, wsize) != wsize) {
               Dmsg0(0, "===Write error===\n");
               Jmsg2(jcr, M_ERROR, 0, _("Write error on %s: ERR=%s\n"), attr->ofname, berror(&bfd));
	       extract = FALSE;
	       continue;
	    } 
	    total += wsize;
	    jcr->JobBytes += wsize;
	    jcr->ReadBytes += wsize;
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

	    if (stream == STREAM_SPARSE_GZIP_DATA) {
	       ser_declare;
	       uint64_t faddr;
	       char ec1[50];
	       wbuf = sd->msg + SPARSE_FADDR_SIZE;
	       wsize = sd->msglen - SPARSE_FADDR_SIZE;
	       ser_begin(sd->msg, SPARSE_FADDR_SIZE);
	       unser_uint64(faddr);
	       if (fileAddr != faddr) {
		  fileAddr = faddr;
		  if (blseek(&bfd, (off_t)fileAddr, SEEK_SET) < 0) {
                     Jmsg3(jcr, M_ERROR, 0, _("Seek to %s error on %s: ERR=%s\n"),
			 edit_uint64(fileAddr, ec1), attr->ofname, berror(&bfd));
		     extract = FALSE;
		     continue;
		  }
	       }
	    } else {
	       wbuf = sd->msg;
	       wsize = sd->msglen;
	    }
	    compress_len = compress_buf_size;
            Dmsg2(100, "Comp_len=%d msglen=%d\n", compress_len, wsize);
	    if ((stat=uncompress((Byte *)jcr->compress_buf, &compress_len, 
		  (const Byte *)wbuf, (uLong)wsize)) != Z_OK) {
               Jmsg(jcr, M_ERROR, 0, _("Uncompression error. ERR=%d\n"), stat);
	       extract = FALSE;
	       continue;
	    }

            Dmsg2(100, "Write uncompressed %d bytes, total before write=%d\n", compress_len, total);
	    if ((uLong)bwrite(&bfd, jcr->compress_buf, compress_len) != compress_len) {
               Dmsg0(0, "===Write error===\n");
               Jmsg2(jcr, M_ERROR, 0, _("Write error on %s: %s\n"), attr->ofname, berror(&bfd));
	       extract = FALSE;
	       continue;
	    }
	    total += compress_len;
	    jcr->JobBytes += compress_len;
	    jcr->ReadBytes += wsize;
	    fileAddr += compress_len;
	 }
#else
	 if (extract) {
            Jmsg(jcr, M_ERROR, 0, _("GZIP data stream found, but GZIP not configured!\n"));
	    extract = FALSE;
	    continue;
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
            Dmsg1(30, "Found wierd stream %d\n", stream);
	    if (!is_bopen(&bfd)) {
               Jmsg0(jcr, M_ERROR, 0, _("Logic error output file should be open but is not.\n"));
	    }
	    set_attributes(jcr, attr, &bfd);
	    extract = FALSE;
	 }
         Jmsg(jcr, M_ERROR, 0, _("Unknown stream=%d ignored. This shouldn't happen!\n"), stream);
         Dmsg2(0, "None of above!!! stream=%d data=%s\n", stream,sd->msg);
	 break;
      } /* end switch(stream) */

   } /* end while get_msg() */

   /* If output file is still open, it was the last one in the
    * archive since we just hit an end of file, so close the file. 
    */
   if (is_bopen(&bfd)) {
      set_attributes(jcr, attr, &bfd);
   }
   set_jcr_job_status(jcr, JS_Terminated);
   goto ok_out;

bail_out:
   set_jcr_job_status(jcr, JS_ErrorTerminated);
ok_out:
   if (jcr->compress_buf) {
      free(jcr->compress_buf);
      jcr->compress_buf = NULL;
   }
   free_attr(attr);
   Dmsg2(10, "End Do Restore. Files=%d Bytes=%" lld "\n", jcr->JobFiles,
      jcr->JobBytes);
   if (non_support_data > 1 || non_support_attr > 1) {
      Jmsg(jcr, M_ERROR, 0, _("%d non-supported data streams and %d non-supported attrib streams ignored.\n"),
	 non_support_data, non_support_attr);
   }
}	   
