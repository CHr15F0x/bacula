/*
 * Functions to handle ACL for bacula.
 *
 * We handle two different typers of ACLs: access and default ACLS.
 * Default ACLs only apply to directories.
 *
 * On some systems (eg. linux and FreeBSD) we must obtain the two ACLs
 * independently, while others (eg. Solaris) provide both in one call.
 *
 * As for the streams to use, we have two choices:
 *
 * 1. Build a generic framework.
 *    With two different types of ACLs, supported differently, we
 *    probably end up encoding and decoding everything ourselves.
 *
 * 2. Take the easy way out.
 *    Just handle each platform individually, assuming that backups
 *    and restores are done on the same (kind of) client.
 *
 * Currently we take the easy way out. We use two kinds of streams, one
 * for access ACLs and one for default ACLs. If an OS mixes the two, we
 * send the mix in the access ACL stream.
 *
 * Looking at more man pages, supporting a framework seems really hard
 * if we want to support HP-UX. Deity knows what AIX is up to.
 *
 *   Written by Preben 'Peppe' Guldberg, December MMIV
 *
 *   Version $Id$
 */

#ifndef TEST_PROGRAM

#include "bacula.h"
#include "filed.h"
/* So we can free system allocated memory */
#undef free
#undef malloc
#define malloc &* dont use malloc in this routine

#else
/*
 * Test program setup 
 *
 * Compile and set up with eg. with eg.
 *
 *    $ cc -DTEST_PROGRAM -DHAVE_SUN_OS -lsec -o acl acl.c
 *    $ ln -s acl aclcp
 *
 * You can then list ACLs with acl and copy them with aclcp.
 *
 * For a list of compiler flags, see the list preceding the big #if below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "acl.h"

#define BACLLEN 65535
#define pm_strcpy(d,s)	   (strncpy(d, s, BACLLEN - 1) == NULL ? -1 : (int)strlen(d))
#define Dmsg0(n,s)	   fprintf(stderr, s)
#define Dmsg1(n,s,a1)	   fprintf(stderr, s, a1)
#define Dmsg2(n,s,a1,a2)   fprintf(stderr, s, a1, a2)

int aclls(char *fname);
int aclcp(char *src, char *dst);

struct JCRstruct {
   char *last_fname;
   char acl_text[BACLLEN];
};
typedef struct JCRstruct JCR;
JCR jcr;
#endif

/*
 * List of supported OSs.
 * Not sure if all the HAVE_XYZ_OS are correct for autoconf.
 * The ones that says man page, are coded according to man pages only.
 */
#if !defined(HAVE_ACL)              /* ACL support is required, of course */ \
   || !( defined(HAVE_AIX_OS)       /* man page -- may need flags         */ \
      || defined(HAVE_FREEBSD_OS)   /* tested   -- compile wihtout flags  */ \
      || defined(HAVE_IRIX_OS)      /* man page -- compile without flags  */ \
      || defined(HAVE_OSF1_OS)      /* man page -- may need -lpacl        */ \
      || defined(HAVE_LINUX_OS)     /* tested   -- compile with -lacl     */ \
      || defined(HAVE_HPUX_OS)      /* man page -- may need flags         */ \
      || defined(HAVE_SUN_OS)       /* tested   -- compile with -lsec     */ \
       )
/*
 * ***FIXME***
 * For now we abandon this test and only test for Linux:
 * 1. This is backwards compatible.
 * 2. If we allow any of the other now, we may have to provide conversion
 *    routines if we ever want to distinguish them. Or just do our best
 *    with what we have and give all ACL streams a new number/type.
 */
#endif
#if !defined(HAVE_ACL) || !defined(HAVE_LINUX_OS)

/* bacl_get() returns the lenght of the string, or -1 on error. */
int bacl_get(JCR *jcr, int acltype)
{
   return -1;
}

int bacl_set(JCR *jcr, int acltype)
{
   return -1;
}

#elif defined(HAVE_AIX_OS)

#include <sys/access.h>

int bacl_get(JCR *jcr, int acltype)
{
   char *acl_text;
   int len;

   if ((acl_text = acl_get(jcr->last_fname)) != NULL) {
      len = pm_strcpy(jcr->acl_text, acl_text);
      free(acl_text);
      return len;
   }
   return -1;
}

int bacl_set(JCR *jcr, int acltype)
{
   if (acl_put(jcr->last_fname, jcr->acl_text, 0) != 0) {
      return -1;
   }
   return 0;
}

#elif defined(HAVE_FREEBSD_OS) \
   || defined(HAVE_IRIX_OS) \
   || defined(HAVE_OSF1_OS) \
   || defined(HAVE_LINUX_OS)

#include <sys/types.h>
#include <sys/acl.h>

/* On IRIX we can get shortened ACLs */
#if defined(HAVE_IRIX_OS) && defined(BACL_WANT_SHORT_ACLS)
#define acl_to_text(acl,len)	 acl_to_short_text((acl), (len))
#endif

/* In Linux we can get numeric and/or shorted ACLs */
#if defined(HAVE_LINUX_OS)
#if defined(BACL_WANT_SHORT_ACLS) && defined(BACL_WANT_NUMERIC_IDS)
#define BACL_ALTERNATE_TEXT	       (TEXT_ABBREVIATE|TEXT_NUMERIC_IDS)
#elif defined(BACL_WANT_SHORT_ACLS)
#define BACL_ALTERNATE_TEXT	       TEXT_ABBREVIATE
#elif defined(BACL_WANT_NUMERIC_IDS)
#define BACL_ALTERNATE_TEXT	       TEXT_NUMERIC_IDS
#endif
#ifdef BACL_ALTERNATE_TEXT
#include <acl/libacl.h>
#define acl_to_text(acl,len)     ((len), acl_to_any_text((acl), NULL, ',', BACL_ALTERNATE_TEXT))
#endif
#endif

int bacl_get(JCR *jcr, int acltype)
{
   acl_t acl;
   int len, ostype;
   char *acl_text;

   ostype = (acltype & BACL_TYPE_DEFAULT) ? ACL_TYPE_DEFAULT : ACL_TYPE_ACCESS;

   acl = acl_get_file(jcr->last_fname, ostype);
   if (acl) {
      if ((acl_text = acl_to_text(acl, NULL)) != NULL) {
	 len = pm_strcpy(jcr->acl_text, acl_text);
	 acl_free(acl_text);
	 return len;
      }
   }
   /***** Do we really want to silently ignore errors from acl_get_file
     and acl_to_text?  *****/
   return 0;
}

int bacl_set(JCR *jcr, int acltype)
{
   acl_t acl;
   int ostype;

   ostype = (acltype & BACL_TYPE_DEFAULT) ? ACL_TYPE_DEFAULT : ACL_TYPE_ACCESS;

   /* If we get empty default ACLs, clear ACLs now */
   if (ostype == ACL_TYPE_DEFAULT && strlen(jcr->acl_text) == 0) {
      if (acl_delete_def_file(jcr->last_fname) == 0) {
	 return 0;
      }
      return -1;
   }

   acl = acl_from_text(jcr->acl_text);
   if (acl == NULL) {
      return -1;
   }

   /*
    * FreeBSD always fails acl_valid() - at least on valid input...
    * As it does the right thing, given valid input, just ignore acl_valid().
    */
#ifndef HAVE_FREEBSD_OS
   if (acl_valid(acl) != 0) {
      acl_free(acl);
      return -1;
   }
#endif

   if (acl_set_file(jcr->last_fname, ostype, acl) != 0) {
      acl_free(acl);
      return -1;
   }
   acl_free(acl);
   return 0;
}

#elif defined(HAVE_HPUX_OS)
#include <sys/acl.h>
#include <acllib.h>

int bacl_get(JCR *jcr, int acltype)
{
   int n, len;
   struct acl_entry acls[NACLENTRIES];
   char *acl_text;

   if ((n = getacl(jcr->last_fname, 0, acls)) <= 0) {
      return -1;
   }
   if ((n = getacl(jcr->last_fname, n, acls)) > 0) {
      if ((acl_text = acltostr(n, acls, FORM_SHORT)) != NULL) {
	 len = pm_strcpy(jcr->acl_text, acl_text);
	 free(acl_text);
	 return len;
      }
   }
   return -1;
}

int bacl_set(JCR *jcr, int acltype)
{
   int n, stat;
   struct acl_entry acls[NACLENTRIES];

   n = strtoacl(jcr->acl_text, 0, NACLENTRIES, acls, ACL_FILEOWNER, ACL_FILEGROUP);
   if (n <= 0) {
      return -1;
   }
   if (strtoacl(jcr->acl_text, n, NACLENTRIES, acls, ACL_FILEOWNER, ACL_FILEGROUP) != n) {
      return -1;
   }
   if (setacl(jcr->last_fname, n, acls) != 0) {
      return -1;
   }
   return 0;
}

#elif defined(HAVE_SUN_OS)
#include <sys/acl.h>

int bacl_get(JCR *jcr, int acltype)
{
   int n, len;
   aclent_t *acls;
   char *acl_text;

   n = acl(jcr->last_fname, GETACLCNT, 0, NULL);
   if (n <= 0) {
      return -1;
   }
   if ((acls = (aclent_t *)malloc(n * sizeof(aclent_t))) == NULL) {
      return -1;
   }
   if (acl(jcr->last_fname, GETACL, n, acls) == n) {
      if ((acl_text = acltotext(acls, n)) != NULL) {
	 pm_strcpy(jcr->acl_text, acl_text);
	 free(acl_text);
	 free(acls);
	 return len;
      }
   }
   free(acls);
   return -1;
}

int bacl_set(JCR *jcr, int acltype)
{
   int n;
   aclent_t *acls;

   acls = aclfromtext(jcr->acl_text, &n);
   if (!acls) {
      return -1;
   }
   if (acl(jcr->last_fname, SETACL, n, acls) != 0) {
      free(acls);
      return -1;
   }
   free(acls);
   return 0;
}

#endif


#ifdef TEST_PROGRAM
int main(int argc, char **argv)
{
   char *prgname;
   int status = 0;

   if (argc < 1) {
      Dmsg0(200, "Cannot determine my own name\n");
      return EXIT_FAILURE;
   }

   prgname = strrchr(argv[0], '/');
   if (prgname == NULL || *++prgname == '\0') {
      prgname = argv[0];
   }
   --argc;
   ++argv;

   /* aclcp "copies" ACLs - set ACLs on destination equal to ACLs on source */
   if (strcmp(prgname, "aclcp") == 0) {
      int verbose = 0;
      if (strcmp(*argv, "-v") == 0) {
	 ++verbose;
	 --argc;
	 ++argv;
      }
      if (argc != 2) {
         Dmsg2(200, "%s: wrong number of arguments\n"
               "usage:\t%s [-v] source destination\n"
               "\tCopies ACLs from source to destination.\n"
               "\tSpecify -v to show ACLs after copy for verification.\n",
	       prgname, prgname);
	 return EXIT_FAILURE;
      }
      if (strcmp(argv[0], argv[1]) == 0) {
         Dmsg2(200, "%s: identical source and destination.\n"
               "usage:\t%s [-v] source destination\n"
               "\tCopies ACLs from source to destination.\n"
               "\tSpecify -v to show ACLs after copy for verification.\n",
	       prgname, prgname);
	 return EXIT_FAILURE;
      }
      if (verbose) {
	 aclls(argv[0]);
      }
      status = aclcp(argv[0], argv[1]);
      if (verbose && status == 0) {
	 aclls(argv[1]);
      }
      return status;
   }

   /* Default: just list ACLs */
   if (argc < 1) {
      Dmsg2(200, "%s: missing arguments\n"
            "usage:\t%s file ...\n"
            "\tLists ACLs of specified files or directories.\n",
	    prgname, prgname);
      return EXIT_FAILURE;
   }
   while (argc--) {
      if (!aclls(*argv++)) {
	 status = EXIT_FAILURE;
      }
   }

   return status;
}

/**** Test program *****/
int aclcp(char *src, char *dst)
{
   struct stat st;

   if (lstat(dst, &st) != 0) {
      Dmsg0(200, "aclcp: destination does not exist\n");
      return EXIT_FAILURE;
   }
   if (S_ISLNK(st.st_mode)) {
      Dmsg0(200, "aclcp: cannot set ACL on symlinks\n");
      return EXIT_FAILURE;
   }
   if (lstat(src, &st) != 0) {
      Dmsg0(200, "aclcp: source does not exist\n");
      return EXIT_FAILURE;
   }
   if (S_ISLNK(st.st_mode)) {
      Dmsg0(200, "aclcp: will not read ACL from symlinks\n");
      return EXIT_FAILURE;
   }

   jcr.last_fname = src;
   if (bacl_get(&jcr, BACL_TYPE_ACCESS) < 0) {
      Dmsg1(200, "aclcp: could not read ACLs for %s\n", jcr.last_fname);
      return EXIT_FAILURE;
   } else {
      jcr.last_fname = dst;
      if (bacl_set(&jcr, BACL_TYPE_ACCESS) < 0) {
         Dmsg1(200, "aclcp: could not set ACLs on %s\n", jcr.last_fname);
	 return EXIT_FAILURE;
      }
   }

   if (S_ISDIR(st.st_mode) && (BACL_CAP & BACL_CAP_DEFAULTS_DIR)) {
      jcr.last_fname = src;
      if (bacl_get(&jcr, BACL_TYPE_DEFAULT) < 0) {
         Dmsg1(200, "aclcp: could not read default ACLs for %s\n", jcr.last_fname);
	 return EXIT_FAILURE;
      } else {
	 jcr.last_fname = dst;
	 if (bacl_set(&jcr, BACL_TYPE_DEFAULT) < 0) {
            Dmsg1(200, "aclcp: could not set default ACLs on %s\n", jcr.last_fname);
	    return EXIT_FAILURE;
	 }
      }
   }

   return 0;
}

/**** Test program *****/
int aclls(char *fname)
{
   struct stat st;

   if (lstat(fname, &st) != 0) {
      Dmsg0(200, "acl: source does not exist\n");
      return EXIT_FAILURE;
   }
   if (S_ISLNK(st.st_mode)) {
      Dmsg0(200, "acl: will not read ACL from symlinks\n");
      return EXIT_FAILURE;
   }

   jcr.last_fname = fname;

   if (bacl_get(&jcr, BACL_TYPE_ACCESS) < 0) {
      Dmsg1(200, "acl: could not read ACLs for %s\n", jcr.last_fname);
      return EXIT_FAILURE;
   }
   printf("#file: %s\n%s\n", jcr.last_fname, jcr.acl_text);

   if (S_ISDIR(st.st_mode) && (BACL_CAP & BACL_CAP_DEFAULTS_DIR)) {
      if (bacl_get(&jcr, BACL_TYPE_DEFAULT) < 0) {
         Dmsg1(200, "acl: could not read default ACLs for %s\n", jcr.last_fname);
	 return EXIT_FAILURE;
      }
      printf("#file: %s [default]\n%s\n", jcr.last_fname, jcr.acl_text);
   }

   return 0;
}
#endif
