/*
 * General header file configurations that apply to
 * all daemons.  System dependent stuff goes here.
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


#ifndef _BACONFIG_H
#define _BACONFIG_H 1

/* Bacula common configuration defines */

#undef  TRUE
#undef  FALSE
#define TRUE  1
#define FALSE 0

#ifdef HAVE_TLS
#define have_tls 1
#else
#define have_tls 0
#endif

#ifndef ETIME
#define ETIME ETIMEDOUT
#endif

#ifdef PROTOTYPES
# define __PROTO(p)     p
#else
# define __PROTO(p)     ()
#endif

#ifdef DEBUG
#define ASSERT(x) if (!(x)) { \
   char *jcr = NULL; \
   Emsg1(M_ERROR, 0, _("Failed ASSERT: %s\n"), #x); \
   jcr[0] = 0; }
#else
#define ASSERT(x)
#endif

/* Allow printing of NULL pointers */
#define NPRT(x) (x)?(x):_("*None*")
 
#ifdef WIN32
#undef ENABLE_NLS
#endif

#ifdef ENABLE_NLS
   #include <libintl.h>
   #include <locale.h>
   #ifndef _
      #define _(s) gettext((s))
   #endif /* _ */
   #ifndef N_
      #define N_(s) (s)
   #endif /* N_ */
#else /* !ENABLE_NLS */
   #ifndef _
      #define _(s) (s)
   #endif
   #ifndef N_
      #define N_(s) (s)
   #endif
   #ifndef textdomain
      #define textdomain(d)
   #endif
   #ifndef bindtextdomain
      #define bindtextdomain(p, d)
   #endif
   #ifndef setlocale
      #define setlocale(p, d)
   #endif
#endif /* ENABLE_NLS */

/* This should go away! ****FIXME***** */
#define MAXSTRING 500

/* Maximum length to edit time/date */
#define MAX_TIME_LENGTH 50

/* Maximum Name length including EOS */
#define MAX_NAME_LENGTH 128

/* Maximume number of user entered command args */
#define MAX_CMD_ARGS 30

/* All tape operations MUST be a multiple of this */
#define TAPE_BSIZE 1024

#ifdef DEV_BSIZE 
#define B_DEV_BSIZE DEV_BSIZE
#endif

#if !defined(B_DEV_BSIZE) & defined(BSIZE)
#define B_DEV_BSIZE BSIZE
#endif

#ifndef B_DEV_BSIZE
#define B_DEV_BSIZE 512
#endif

/*
 * Set to time limit for other end to respond to
 *  authentication.  Normally 10 minutes is *way*
 *  more than enough. The idea is to keep the Director
 *  from hanging because there is a dead connection on
 *  the other end.
 */
#define AUTH_TIMEOUT 60 * 10

/*
 * Default network buffer size
 */
#define DEFAULT_NETWORK_BUFFER_SIZE (64 * 1024)

/*
 * Stream definitions.  Once defined these must NEVER
 *   change as they go on the storage media.
 * Note, the following streams are passed from the SD to the DIR
 *   so that they may be put into the catalog (actually only the
 *   stat packet part of the attr record is put in the catalog.
 *
 *   STREAM_UNIX_ATTRIBUTES
 *   STREAM_UNIX_ATTRIBUTES_EX
 *   STREAM_MD5_DIGEST
 *   STREAM_SHA1_DIGEST
 *   STREAM_SHA256_DIGEST
 *   STREAM_SHA512_DIGEST
 */
#define STREAM_NONE               0    /* Reserved Non-Stream */
#define STREAM_UNIX_ATTRIBUTES    1    /* Generic Unix attributes */
#define STREAM_FILE_DATA          2    /* Standard uncompressed data */
#define STREAM_MD5_DIGEST         3    /* MD5 digest for the file */
#define STREAM_GZIP_DATA          4    /* GZip compressed file data */
/* Extended Unix attributes with Win32 Extended data.  Deprecated. */
#define STREAM_UNIX_ATTRIBUTES_EX 5    /* Extended Unix attr for Win32 EX */
#define STREAM_SPARSE_DATA        6    /* Sparse data stream */
#define STREAM_SPARSE_GZIP_DATA   7
#define STREAM_PROGRAM_NAMES      8    /* program names for program data */
#define STREAM_PROGRAM_DATA       9    /* Data needing program */
#define STREAM_SHA1_DIGEST       10    /* SHA1 digest for the file */
#define STREAM_WIN32_DATA        11    /* Win32 BackupRead data */
#define STREAM_WIN32_GZIP_DATA   12    /* Gzipped Win32 BackupRead data */
#define STREAM_MACOS_FORK_DATA   13    /* Mac resource fork */
#define STREAM_HFSPLUS_ATTRIBUTES 14   /* Mac OS extra attributes */
/*** FIXME ***/
#define STREAM_UNIX_ATTRIBUTES_ACCESS_ACL 15 /* Standard ACL attributes on UNIX */
#define STREAM_UNIX_ATTRIBUTES_DEFAULT_ACL 16 /* Default ACL attributes on UNIX */
/*** FIXME ***/
#define STREAM_SHA256_DIGEST     17    /* SHA-256 digest for the file */
#define STREAM_SHA512_DIGEST     18    /* SHA-512 digest for the file */
#define STREAM_SIGNED_DIGEST     19    /* Signed File Digest, ASN.1, DER Encoded */
#define STREAM_ENCRYPTED_FILE_DATA 20  /* Encrypted, uncompressed data */
#define STREAM_ENCRYPTED_WIN32_DATA 21 /* Encrypted, uncompressed Win32 BackupRead data */
#define STREAM_ENCRYPTED_SESSION_DATA 22 /* Encrypted Session Data, ASN.1, DER Encoded */


/*
 *  File type (Bacula defined).
 *  NOTE!!! These are saved in the Attributes record on the tape, so
 *          do not change them. If need be, add to them.
 *
 *  This is stored as 32 bits on tape, but only FT_MASK bits are
 *    used for the file type. The upper bits are used to indicate
 *    additional optional fields in the attribute record.
 */
#define FT_MASK       0xFFFF          /* Bits used by FT (type) */
#define FT_LNKSAVED   1               /* hard link to file already saved */
#define FT_REGE       2               /* Regular file but empty */
#define FT_REG        3               /* Regular file */
#define FT_LNK        4               /* Soft Link */
#define FT_DIREND     5               /* Directory at end (saved) */
#define FT_SPEC       6               /* Special file -- chr, blk, fifo, sock */
#define FT_NOACCESS   7               /* Not able to access */
#define FT_NOFOLLOW   8               /* Could not follow link */
#define FT_NOSTAT     9               /* Could not stat file */
#define FT_NOCHG     10               /* Incremental option, file not changed */
#define FT_DIRNOCHG  11               /* Incremental option, directory not changed */
#define FT_ISARCH    12               /* Trying to save archive file */
#define FT_NORECURSE 13               /* No recursion into directory */
#define FT_NOFSCHG   14               /* Different file system, prohibited */
#define FT_NOOPEN    15               /* Could not open directory */
#define FT_RAW       16               /* Raw block device */
#define FT_FIFO      17               /* Raw fifo device */
/* This directory packet is sent to the FD file processing routine so
 * that it can filter packets, but otherwise, it is not used
 * or saved */
#define FT_DIRBEGIN  18               /* Directory at beginning (not saved) */
#define FT_INVALIDFS 19               /* File system not allowed for */

/* Definitions for upper part of type word (see above). */
#define AR_DATA_STREAM (1<<16)        /* Data stream id present */

/*
 * Tape label types -- stored in catalog
 */
#define B_BACULA_LABEL 0
#define B_ANSI_LABEL   1
#define B_IBM_LABEL    2

/* Size of File Address stored in STREAM_SPARSE_DATA. Do NOT change! */
#define SPARSE_FADDR_SIZE (sizeof(uint64_t))


/* This is for dumb compilers/libraries like Solaris. Linux GCC
 * does it correctly, so it might be worthwhile
 * to remove the isascii(c) with ifdefs on such
 * "smart" systems.
 */
#define B_ISSPACE(c) (isascii((int)(c)) && isspace((int)(c)))
#define B_ISALPHA(c) (isascii((int)(c)) && isalpha((int)(c)))
#define B_ISUPPER(c) (isascii((int)(c)) && isupper((int)(c)))
#define B_ISDIGIT(c) (isascii((int)(c)) && isdigit((int)(c)))

/* For multiplying by 10 with shift and addition */
#define B_TIMES10(d) ((d<<3)+(d<<1))


typedef void (HANDLER)();
typedef int (INTHANDLER)();

#ifdef SETPGRP_VOID
# define SETPGRP_ARGS(x, y) /* No arguments */
#else
# define SETPGRP_ARGS(x, y) (x, y)
#endif

#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFM) == S_IFLNK)
#endif

/* Added by KES to deal with Win32 systems */
#ifndef S_ISWIN32
#define S_ISWIN32 020000
#endif

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long) -1)
#endif

#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef MODE_RW
#define MODE_RW 0666
#endif

#ifdef DEBUG_MUTEX
extern void _p(char *file, int line, pthread_mutex_t *m);
extern void _v(char *file, int line, pthread_mutex_t *m);

#define P(x) _p(__FILE__, __LINE__, &(x))
#define V(x) _v(__FILE__, __LINE__, &(x))

#else
extern void _p(pthread_mutex_t *m);
extern void _v(pthread_mutex_t *m);

#define P(x) _p(&(x))
#define V(x) _v(&(x))

#endif /* DEBUG_MUTEX */

/* These probably should be subroutines */
#define Pw(x) \
   do { int errstat; if ((errstat=rwl_writelock(&(x)))) \
      e_msg(__FILE__, __LINE__, M_ABORT, 0, "Write lock lock failure. ERR=%s\n",\
           strerror(errstat)); \
   } while(0)

#define Vw(x) \
   do { int errstat; if ((errstat=rwl_writeunlock(&(x)))) \
         e_msg(__FILE__, __LINE__, M_ABORT, 0, "Write lock unlock failure. ERR=%s\n",\
           strerror(errstat)); \
   } while(0)

#define LockRes()   b_LockRes(__FILE__, __LINE__)
#define UnlockRes() b_UnlockRes(__FILE__, __LINE__)

#ifdef DEBUG_MEMSET
#define memset(a, v, n) b_memset(__FILE__, __LINE__, a, v, n)
void b_memset(const char *file, int line, void *mem, int val, size_t num);
#endif


/*
 * The digit following Dmsg and Emsg indicates the number of substitutions in
 * the message string. We need to do this kludge because non-GNU compilers
 * do not handle varargs #defines.
 */
/* Debug Messages that are printed */
#ifdef DEBUG
#define Dmsg0(lvl, msg)             if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg)
#define Dmsg1(lvl, msg, a1)         if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1)
#define Dmsg2(lvl, msg, a1, a2)     if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1, a2)
#define Dmsg3(lvl, msg, a1, a2, a3) if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3)
#define Dmsg4(lvl, msg, arg1, arg2, arg3, arg4) if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, arg1, arg2, arg3, arg4)
#define Dmsg5(lvl, msg, a1, a2, a3, a4, a5) if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5)
#define Dmsg6(lvl, msg, a1, a2, a3, a4, a5, a6) if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6)
#define Dmsg7(lvl, msg, a1, a2, a3, a4, a5, a6, a7) if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6, a7)
#define Dmsg8(lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8) if ((lvl)<=debug_level) d_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8)
#define Dmsg9(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9) if ((lvl)<=debug_level) d_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9)
#define Dmsg10(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10) if ((lvl)<=debug_level) d_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10)
#define Dmsg11(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11) if ((lvl)<=debug_level) d_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11)
#define Dmsg12(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12) if ((lvl)<=debug_level) d_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12)
#define Dmsg13(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13) if ((lvl)<=debug_level) d_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13)
#else
#define Dmsg0(lvl, msg)
#define Dmsg1(lvl, msg, a1)
#define Dmsg2(lvl, msg, a1, a2)
#define Dmsg3(lvl, msg, a1, a2, a3)
#define Dmsg4(lvl, msg, arg1, arg2, arg3, arg4)
#define Dmsg5(lvl, msg, a1, a2, a3, a4, a5)
#define Dmsg6(lvl, msg, a1, a2, a3, a4, a5, a6)
#define Dmsg7(lvl, msg, a1, a2, a3, a4, a5, a6, a7)
#define Dmsg8(lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8)
#define Dmsg11(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11)
#define Dmsg12(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12)
#define Dmsg13(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13)
#endif /* DEBUG */

#ifdef TRACE_FILE
#define Tmsg0(lvl, msg)             t_msg(__FILE__, __LINE__, lvl, msg)
#define Tmsg1(lvl, msg, a1)         t_msg(__FILE__, __LINE__, lvl, msg, a1)
#define Tmsg2(lvl, msg, a1, a2)     t_msg(__FILE__, __LINE__, lvl, msg, a1, a2)
#define Tmsg3(lvl, msg, a1, a2, a3) t_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3)
#define Tmsg4(lvl, msg, arg1, arg2, arg3, arg4) t_msg(__FILE__, __LINE__, lvl, msg, arg1, arg2, arg3, arg4)
#define Tmsg5(lvl, msg, a1, a2, a3, a4, a5) t_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5)
#define Tmsg6(lvl, msg, a1, a2, a3, a4, a5, a6) t_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6)
#define Tmsg7(lvl, msg, a1, a2, a3, a4, a5, a6, a7) t_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6, a7)
#define Tmsg8(lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8) t_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8)
#define Tmsg9(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9) t_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9)
#define Tmsg10(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10) t_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10)
#define Tmsg11(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11) t_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11)
#define Tmsg12(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12) t_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12)
#define Tmsg13(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13) t_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13)
#else
#define Tmsg0(lvl, msg)
#define Tmsg1(lvl, msg, a1)
#define Tmsg2(lvl, msg, a1, a2)
#define Tmsg3(lvl, msg, a1, a2, a3)
#define Tmsg4(lvl, msg, arg1, arg2, arg3, arg4)
#define Tmsg5(lvl, msg, a1, a2, a3, a4, a5)
#define Tmsg6(lvl, msg, a1, a2, a3, a4, a5, a6)
#define Tmsg7(lvl, msg, a1, a2, a3, a4, a5, a6, a7)
#define Tmsg8(lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8)
#define Tmsg11(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11)
#define Tmsg12(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12)
#define Tmsg13(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13)
#endif /* TRACE_FILE */



/* Messages that are printed (uses d_msg) */
#define Pmsg0(lvl, msg)             p_msg(__FILE__, __LINE__, lvl, msg)
#define Pmsg1(lvl, msg, a1)         p_msg(__FILE__, __LINE__, lvl, msg, a1)
#define Pmsg2(lvl, msg, a1, a2)     p_msg(__FILE__, __LINE__, lvl, msg, a1, a2)
#define Pmsg3(lvl, msg, a1, a2, a3) p_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3)
#define Pmsg4(lvl, msg, arg1, arg2, arg3, arg4) p_msg(__FILE__, __LINE__, lvl, msg, arg1, arg2, arg3, arg4)
#define Pmsg5(lvl, msg, a1, a2, a3, a4, a5) p_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5)
#define Pmsg6(lvl, msg, a1, a2, a3, a4, a5, a6) p_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6)
#define Pmsg7(lvl, msg, a1, a2, a3, a4, a5, a6, a7) p_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6, a7)
#define Pmsg8(lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8) p_msg(__FILE__, __LINE__, lvl, msg, a1, a2, a3, a4, a5, a6, a7, a8)
#define Pmsg9(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9) p_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9)
#define Pmsg10(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10) p_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10)
#define Pmsg11(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11) p_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11)
#define Pmsg12(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12) p_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12)
#define Pmsg13(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13) p_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13)
#define Pmsg14(lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14) p_msg(__FILE__,__LINE__,lvl,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14)


/* Daemon Error Messages that are delivered according to the message resource */
#define Emsg0(typ, lvl, msg)             e_msg(__FILE__, __LINE__, typ, lvl, msg)
#define Emsg1(typ, lvl, msg, a1)         e_msg(__FILE__, __LINE__, typ, lvl, msg, a1)
#define Emsg2(typ, lvl, msg, a1, a2)     e_msg(__FILE__, __LINE__, typ, lvl, msg, a1, a2)
#define Emsg3(typ, lvl, msg, a1, a2, a3) e_msg(__FILE__, __LINE__, typ, lvl, msg, a1, a2, a3)
#define Emsg4(typ, lvl, msg, a1, a2, a3, a4) e_msg(__FILE__, __LINE__, typ, lvl, msg, a1, a2, a3, a4)
#define Emsg5(typ, lvl, msg, a1, a2, a3, a4, a5) e_msg(__FILE__, __LINE__, typ, lvl, msg, a1, a2, a3, a4, a5)
#define Emsg6(typ, lvl, msg, a1, a2, a3, a4, a5, a6) e_msg(__FILE__, __LINE__, typ, lvl, msg, a1, a2, a3, a4, a5, a6)

/* Job Error Messages that are delivered according to the message resource */
#define Jmsg0(jcr, typ, lvl, msg)             j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg)
#define Jmsg1(jcr, typ, lvl, msg, a1)         j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1)
#define Jmsg2(jcr, typ, lvl, msg, a1, a2)     j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2)
#define Jmsg3(jcr, typ, lvl, msg, a1, a2, a3) j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3)
#define Jmsg4(jcr, typ, lvl, msg, a1, a2, a3, a4) j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3, a4)
#define Jmsg5(jcr, typ, lvl, msg, a1, a2, a3, a4, a5) j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3, a4, a5)
#define Jmsg6(jcr, typ, lvl, msg, a1, a2, a3, a4, a5, a6) j_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3, a4, a5, a6)

/* Queued Job Error Messages that are delivered according to the message resource */
#define Qmsg0(jcr, typ, lvl, msg)             q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg)
#define Qmsg1(jcr, typ, lvl, msg, a1)         q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1)
#define Qmsg2(jcr, typ, lvl, msg, a1, a2)     q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2)
#define Qmsg3(jcr, typ, lvl, msg, a1, a2, a3) q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3)
#define Qmsg4(jcr, typ, lvl, msg, a1, a2, a3, a4) q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3, a4)
#define Qmsg5(jcr, typ, lvl, msg, a1, a2, a3, a4, a5) q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3, a4, a5)
#define Qmsg6(jcr, typ, lvl, msg, a1, a2, a3, a4, a5, a6) q_msg(__FILE__, __LINE__, jcr, typ, lvl, msg, a1, a2, a3, a4, a5, a6)


/* Memory Messages that are edited into a Pool Memory buffer */
#define Mmsg0(buf, msg)             m_msg(__FILE__, __LINE__, buf, msg)
#define Mmsg1(buf, msg, a1)         m_msg(__FILE__, __LINE__, buf, msg, a1)
#define Mmsg2(buf, msg, a1, a2)     m_msg(__FILE__, __LINE__, buf, msg, a1, a2)
#define Mmsg3(buf, msg, a1, a2, a3) m_msg(__FILE__, __LINE__, buf, msg, a1, a2, a3)
#define Mmsg4(buf, msg, a1, a2, a3, a4) m_msg(__FILE__, __LINE__, buf, msg, a1, a2, a3, a4)
#define Mmsg5(buf, msg, a1, a2, a3, a4, a5) m_msg(__FILE__, __LINE__, buf, msg, a1, a2, a3, a4, a5)
#define Mmsg6(buf, msg, a1, a2, a3, a4, a5, a6) m_msg(__FILE__, __LINE__, buf, msg, a1, a2, a3, a4, a5, a6)
#define Mmsg7(buf, msg, a1, a2, a3, a4, a5, a6, a7) m_msg(__FILE__, __LINE__, buf, msg, a1, a2, a3, a4, a5, a6)
#define Mmsg8(buf,msg,a1,a2,a3,a4,a5,a6,a7,a8) m_msg(__FILE__,__LINE__,buf,msg,a1,a2,a3,a4,a5,a6)
#define Mmsg11(buf,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11) m_msg(__FILE__,__LINE__,buf,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11)
#define Mmsg15(buf,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15) m_msg(__FILE__,__LINE__,buf,msg,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15)

class POOL_MEM;
/* Edit message into Pool Memory buffer -- no __FILE__ and __LINE__ */
int  Mmsg(POOLMEM **msgbuf, const char *fmt,...);
int  Mmsg(POOLMEM *&msgbuf, const char *fmt,...);
int  Mmsg(POOL_MEM &msgbuf, const char *fmt,...);


class JCR;
void d_msg(const char *file, int line, int level, const char *fmt,...);
void p_msg(const char *file, int line, int level, const char *fmt,...);
void e_msg(const char *file, int line, int type, int level, const char *fmt,...);
void j_msg(const char *file, int line, JCR *jcr, int type, time_t mtime, const char *fmt,...);
void q_msg(const char *file, int line, JCR *jcr, int type, time_t mtime, const char *fmt,...);
int  m_msg(const char *file, int line, POOLMEM **msgbuf, const char *fmt,...);
int  m_msg(const char *file, int line, POOLMEM *&pool_buf, const char *fmt, ...);


/* Use our strdup with smartalloc */
#ifndef __WXGTK__
#undef strdup
#define strdup(buf) bad_call_on_strdup_use_bstrdup(buf)
#endif

/* Use our fgets which handles interrupts */
#undef fgets
#define fgets(x,y,z) bfgets((x), (y), (z))

/* Use our sscanf, which is safer and works with known sizes */
#define sscanf bsscanf

#ifdef DEBUG
#define bstrdup(str) strcpy((char *)b_malloc(__FILE__,__LINE__,strlen((str))+1),(str))
#else
#define bstrdup(str) strcpy((char *)bmalloc(strlen((str))+1),(str))
#endif

#ifdef DEBUG
#define bmalloc(size) b_malloc(__FILE__, __LINE__, (size))
#endif

/*
 * Replace codes needed in both file routines and non-file routines
 * Job replace codes -- in "replace"
 */
#define REPLACE_ALWAYS   'a'
#define REPLACE_IFNEWER  'w'
#define REPLACE_NEVER    'n'
#define REPLACE_IFOLDER  'o'

/* This probably should be done on a machine by machine basic, but it works */
/* This is critical for the smartalloc routines to properly align memory */
#define ALIGN_SIZE (sizeof(double))
#define BALIGN(x) (((x) + ALIGN_SIZE - 1) & ~(ALIGN_SIZE -1))


/* =============================================================
 *               OS Dependent defines
 * ============================================================= 
 */

#ifndef HAVE_FSEEKO
/* Bad news. This OS cannot handle 64 bit fseeks and ftells */
#define fseeko fseek
#define ftello ftell
#endif

#if defined (__digital__) && defined (__unix__)
/* Tru64 - it does have fseeko and ftello , but since ftell/fseek are also 64 bit */
/* take this 'shortcut' */
#define fseeko fseek
#define ftello ftell
#endif


#ifdef __alpha__
#define OSF 1
#endif

#ifdef HAVE_SUN_OS
   /*
    * On Solaris 2.5, threads are not timesliced by default, so we need to
    * explictly increase the conncurrency level.
    */
#include <thread.h>
#define set_thread_concurrency(x)  thr_setconcurrency(x)
extern int thr_setconcurrency(int);
#define SunOS 1

#else


/* Not needed on most systems */
#define set_thread_concurrency(x)

#endif

#if defined(HAVE_DARWIN_OS) || defined(HAVE_OSF1_OS)
/* Apparently someone forgot to wrap getdomainname as a C function */
extern "C" int getdomainname(char *name, int len);
#endif

#ifdef HAVE_OSF1_OS
extern "C" int mknod ( const char *path, int mode, dev_t device );
#endif

#ifdef HAVE_CYGWIN
/* They don't really have it */
#undef HAVE_GETDOMAINNAME
#endif

/* Define Winsock functions if we aren't on Windows */
#if (!defined HAVE_WIN32) || (defined HAVE_CYGWIN)
#define WSA_Init() 0 /* 0 = success */
#define WSACleanup() 0 /* 0 = success */
#endif

#ifdef HAVE_AIX_OS
#endif

/* HP-UX 11 specific workarounds */

#ifdef HAVE_HPUX_OS
# undef h_errno
extern int h_errno;
/* the {get,set}domainname() functions exist in HPUX's libc.
 * the configure script detects that correctly.
 * the problem is no system headers declares the prototypes for these functions
 * this is done below
 */
extern "C" int getdomainname(char *name, int namelen);
extern "C" int setdomainname(char *name, int namelen);
#define uLong unsigned long
#endif /* HAVE_HPUX_OS */


#ifdef HAVE_OSF1_OS
#undef HAVE_CHFLAGS  /* chflags is incorrectly detected */
extern "C" int fchdir(int filedes);
extern "C" long gethostid(void);
#endif


/* Added by KES to deal with Win32 systems */
#ifndef S_ISWIN32
#define S_ISWIN32 020000
#endif


/* Disabled because it breaks internationalisation...
#undef HAVE_SETLOCALE
#ifdef HAVE_SETLOCALE
#include <locale.h>
#else
#define setlocale(x, y) ("ANSI_X3.4-1968")
#endif
#ifdef HAVE_NL_LANGINFO
#include <langinfo.h>
#else
#define nl_langinfo(x) ("ANSI_X3.4-1968")
#endif
*/

/* Fake entry points if regex does not exist */
#ifndef HAVE_REGEX_H
#define regcomp(x, y, z) 1
#define regfree(x)
#define regerror(rc, preg, prbuf, len) bstrncpy(prbuf, "REGEX not available on this system.", len)
#define regex_t int
#define regmatch_t char
#endif

#endif /* _BACONFIG_H */
