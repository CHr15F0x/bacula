/*
   BaculaÂ® - The Network Backup Solution

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   BaculaÂ® is a registered trademark of Kern Sibbald.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 ZÃ¼rich,
   Switzerland, email:ftf <at> fsfeurope.org.
*/
/*
 * Bacula zlib compression wrappers
 *
 */

#include "bacula.h"
#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

/*
 * Deflate or compress and input buffer.  You must supply an
 *  output buffer sufficiently long and the length of the
 *  output buffer. Generally, if the output buffer is the
 *  same size as the input buffer, it should work (at least
 *  for text).
 */
int Zdeflate(char *in, int in_len, char *out, int &out_len)
{
#ifdef HAVE_LIBZ
   z_stream strm;
   int ret;

   /* allocate deflate state */
   strm.zalloc = Z_NULL;
   strm.zfree = Z_NULL;
   strm.opaque = Z_NULL;
   ret = deflateInit(&strm, 9);
   if (ret != Z_OK) {
      Dmsg0(200, "deflateInit error\n");
      (void)deflateEnd(&strm);
      return ret;
   }

   strm.next_in = (Bytef *)in;
   strm.avail_in = in_len;
   Dmsg1(200, "In: %d bytes\n", strm.avail_in);
   strm.avail_out = out_len;
   strm.next_out = (Bytef *)out;
   ret = deflate(&strm, Z_FINISH);
   out_len = out_len - strm.avail_out;
   Dmsg1(200, "compressed=%d\n", out_len);
   (void)deflateEnd(&strm);
   return ret;
#else
   return 1;
#endif
}

/*
 * Inflate or uncompress an input buffer.  You must supply
 *  and output buffer and an output length sufficiently long
 *  or there will be an error.  This uncompresses in one call.
 */
int Zinflate(char *in, int in_len, char *out, int &out_len)
{
#ifdef HAVE_LIBZ
   z_stream strm;
   int ret;

   /* allocate deflate state */
   strm.zalloc = Z_NULL;
   strm.zfree = Z_NULL;
   strm.opaque = Z_NULL;
   strm.next_in = (Bytef *)in;
   strm.avail_in = in_len;
   ret = inflateInit(&strm);
   if (ret != Z_OK) {
      Dmsg0(200, "inflateInit error\n");
      (void)inflateEnd(&strm);
      return ret;
   }

   Dmsg1(200, "In len: %d bytes\n", strm.avail_in);
   strm.avail_out = out_len;
   strm.next_out = (Bytef *)out;
   ret = inflate(&strm, Z_FINISH);
   out_len -= strm.avail_out;
   Dmsg1(200, "Uncompressed=%d\n", out_len);
   (void)inflateEnd(&strm);
   return ret;
#else
   return 1;
#endif
}

