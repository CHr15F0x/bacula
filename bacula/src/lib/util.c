/*
 *   util.c  miscellaneous utility subroutines for Bacula
 * 
 *    Kern Sibbald, MM
 *
 *   Version $Id$
 */

/*
   Copyright (C) 2000, 2001, 2002 Kern Sibbald and John Walker

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
#include "jcr.h"
#include "findlib/find.h"

/*
 * Various Bacula Utility subroutines
 *
 */

/* Return true of buffer has all zero bytes */
int is_buf_zero(char *buf, int len)
{
   uint64_t *ip;
   char *p;
   int i, len64, done, rem;

   if (buf[0] != 0) {
      return 0;
   }
   ip = (uint64_t *)buf;
   /* Optimize by checking uint64_t for zero */
   len64 = len / sizeof(uint64_t);
   for (i=0; i < len64; i++) {
      if (ip[i] != 0) {
	 return 0;
      }
   }
   done = len64 * sizeof(uint64_t);  /* bytes already checked */
   p = buf + done;
   rem = len - done;
   for (i = 0; i < rem; i++) {
      if (p[i] != 0) {
	 return 0;
      }
   }
   return 1;
}


/* Convert a string in place to lower case */
void lcase(char *str)
{
   while (*str) {
      if (B_ISUPPER(*str))
	 *str = tolower((int)(*str));
       str++;
   }
}

/* Convert spaces to non-space character. 
 * This makes scanf of fields containing spaces easier.
 */
void
bash_spaces(char *str)
{
   while (*str) {
      if (*str == ' ')
	 *str = 0x1;
      str++;
   }
}

/* Convert non-space characters (0x1) back into spaces */
void
unbash_spaces(char *str)
{
   while (*str) {
     if (*str == 0x1)
        *str = ' ';
     str++;
   }
}


char *encode_time(time_t time, char *buf)
{
   struct tm tm;
   int n = 0;

   if (localtime_r(&time, &tm)) {
      n = sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
		   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		   tm.tm_hour, tm.tm_min, tm.tm_sec);
   }
   return buf+n;
}

/*
 * Concatenate a string (str) onto a pool memory buffer pm
 *   Returns: length of concatenated string
 */
int pm_strcat(POOLMEM **pm, const char *str)
{
   int pmlen = strlen(*pm);
   int len = strlen(str) + 1;

   *pm = check_pool_memory_size(*pm, pmlen + len);
   memcpy(*pm+pmlen, str, len);
   return pmlen + len - 1;
}


/*
 * Copy a string (str) into a pool memory buffer pm
 *   Returns: length of string copied
 */
int pm_strcpy(POOLMEM **pm, const char *str)
{
   int len = strlen(str) + 1;

   *pm = check_pool_memory_size(*pm, len);
   memcpy(*pm, str, len);
   return len - 1;
}


/*
 * Convert a JobStatus code into a human readable form
 */
void jobstatus_to_ascii(int JobStatus, char *msg, int maxlen)
{
   char *jobstat;
   char buf[100];

   switch (JobStatus) {
   case JS_Created:
      jobstat = _("Created");
      break;
   case JS_Running:
      jobstat = _("Running");
      break;
   case JS_Blocked:
      jobstat = _("Blocked");
      break;
   case JS_Terminated:
      jobstat = _("OK");
      break;
   case JS_FatalError:
   case JS_ErrorTerminated:
      jobstat = _("Error");
      break;
   case JS_Error:
      jobstat = _("Non-fatal error");
      break;
   case JS_Canceled:
      jobstat = _("Canceled");
      break;
   case JS_Differences:
      jobstat = _("Verify differences");
      break;
   case JS_WaitFD:
      jobstat = _("Waiting on FD");
      break;
   case JS_WaitSD:
      jobstat = _("Wait on SD");
      break;
   case JS_WaitMedia:
      jobstat = _("Wait for new Volume");
      break;
   case JS_WaitMount:
      jobstat = _("Waiting for mount");
      break;
   case JS_WaitStoreRes:
      jobstat = _("Waiting for Storage resource");
      break;
   case JS_WaitJobRes:
      jobstat = _("Waiting for Job resource");
      break;
   case JS_WaitClientRes:
      jobstat = _("Waiting for Client resource");
      break;
   case JS_WaitMaxJobs:
      jobstat = _("Waiting on Max Jobs");
      break;
   case JS_WaitStartTime:
      jobstat = _("Waiting for Start Time");
      break;
   case JS_WaitPriority:
      jobstat = _("Waiting on Priority");
      break;

   default:
      if (JobStatus == 0) {
	 buf[0] = 0;
      } else {
         bsnprintf(buf, sizeof(buf), _("Unknown Job termination status=%d"), JobStatus);
      }
      jobstat = buf;
      break;
   }
   bstrncpy(msg, jobstat, maxlen);
}

/*
 * Convert Job Termination Status into a string
 */
char *job_status_to_str(int stat) 
{
   char *str;

   switch (stat) {
   case JS_Terminated:
      str = _("OK");
      break;
   case JS_ErrorTerminated:
   case JS_Error:
      str = _("Error");
      break;
   case JS_FatalError:
      str = _("Fatal Error");
      break;
   case JS_Canceled:
      str = _("Canceled");
      break;
   case JS_Differences:
      str = _("Differences");
      break;
   default:
      str = _("Unknown term code");
      break;
   }
   return str;
}


/*
 * Convert Job Type into a string
 */
char *job_type_to_str(int type) 
{
   char *str;

   switch (type) {
   case JT_BACKUP:
      str = _("Backup");
      break;
   case JT_VERIFY:
      str = _("Verify");
      break;
   case JT_RESTORE:
      str = _("Restore");
      break;
   case JT_ADMIN:
      str = _("Admin");
      break;
   default:
      str = _("Unknown Type");
      break;
   }
   return str;
}

/*
 * Convert Job Level into a string
 */
char *job_level_to_str(int level) 
{
   char *str;

   switch (level) {
   case L_BASE:
      str = _("Base");
   case L_FULL:
      str = _("Full");
      break;
   case L_INCREMENTAL:
      str = _("Incremental");
      break;
   case L_DIFFERENTIAL:
      str = _("Differential");
      break;
   case L_SINCE:
      str = _("Since");
      break;
   case L_VERIFY_CATALOG:
      str = _("Verify Catalog");
      break;
   case L_VERIFY_INIT:
      str = _("Verify Init Catalog");
      break;
   case L_VERIFY_VOLUME_TO_CATALOG:
      str = _("Verify Volume to Catalog");
      break;
   case L_VERIFY_DISK_TO_CATALOG:
      str = _("Verify Disk to Catalog");
      break;
   case L_VERIFY_DATA:
      str = _("Verify Data");
      break;
   case L_NONE:
      str = " ";
      break;
   default:
      str = _("Unknown Job Level");
      break;
   }
   return str;
}


/***********************************************************************
 * Encode the mode bits into a 10 character string like LS does
 ***********************************************************************/

char *encode_mode(mode_t mode, char *buf)
{
  char *cp = buf;  

  *cp++ = S_ISDIR(mode) ? 'd' : S_ISBLK(mode)  ? 'b' : S_ISCHR(mode)  ? 'c' :
          S_ISLNK(mode) ? 'l' : S_ISFIFO(mode) ? 'f' : S_ISSOCK(mode) ? 's' : '-';
  *cp++ = mode & S_IRUSR ? 'r' : '-';
  *cp++ = mode & S_IWUSR ? 'w' : '-';
  *cp++ = (mode & S_ISUID
               ? (mode & S_IXUSR ? 's' : 'S')
               : (mode & S_IXUSR ? 'x' : '-'));
  *cp++ = mode & S_IRGRP ? 'r' : '-';
  *cp++ = mode & S_IWGRP ? 'w' : '-';
  *cp++ = (mode & S_ISGID
               ? (mode & S_IXGRP ? 's' : 'S')
               : (mode & S_IXGRP ? 'x' : '-'));
  *cp++ = mode & S_IROTH ? 'r' : '-';
  *cp++ = mode & S_IWOTH ? 'w' : '-';
  *cp++ = (mode & S_ISVTX
               ? (mode & S_IXOTH ? 't' : 'T')
               : (mode & S_IXOTH ? 'x' : '-'));
  *cp = '\0';
  return cp;
}


int do_shell_expansion(char *name, int name_len)
{
   static char meta[] = "~\\$[]*?`'<>\"";
   bool found = false;
   int len, i, stat;
   POOLMEM *cmd;
   BPIPE *bpipe;
   char line[MAXSTRING];
   char *shellcmd;

   /* Check if any meta characters are present */
   len = strlen(meta);
   for (i = 0; i < len; i++) {
      if (strchr(name, meta[i])) {
	 found = true;
	 break;
      }
   }
   if (found) {
      cmd =  get_pool_memory(PM_FNAME);
      /* look for shell */
      if ((shellcmd = getenv("SHELL")) == NULL) {
         shellcmd = "/bin/sh";
      }
      pm_strcpy(&cmd, shellcmd);
      pm_strcat(&cmd, " -c \"echo ");
      pm_strcat(&cmd, name);
      pm_strcat(&cmd, "\"");
      Dmsg1(400, "Send: %s\n", cmd);
      bpipe = open_bpipe(cmd, 0, "r");
      *line = 0;
      fgets(line, sizeof(line), bpipe->rfd);
      strip_trailing_junk(line);
      stat = close_bpipe(bpipe);
      Dmsg2(400, "stat=%d got: %s\n", stat, line);
      free_pool_memory(cmd);
      if (stat == 0) {
	 bstrncpy(name, line, name_len);
      }
   }
   return 1;
}


/*  MAKESESSIONKEY  --	Generate session key with optional start
			key.  If mode is TRUE, the key will be
			translated to a string, otherwise it is
			returned as 16 binary bytes.

    from SpeakFreely by John Walker */

void make_session_key(char *key, char *seed, int mode)
{
     int j, k;
     struct MD5Context md5c;
     unsigned char md5key[16], md5key1[16];
     char s[1024];

     s[0] = 0;
     if (seed != NULL) {
	strcat(s, seed);
     }

     /* The following creates a seed for the session key generator
	based on a collection of volatile and environment-specific
	information unlikely to be vulnerable (as a whole) to an
        exhaustive search attack.  If one of these items isn't
	available on your machine, replace it with something
	equivalent or, if you like, just delete it. */

     sprintf(s + strlen(s), "%lu", (unsigned long) getpid());
     sprintf(s + strlen(s), "%lu", (unsigned long) getppid());
     getcwd(s + strlen(s), 256);
     sprintf(s + strlen(s), "%lu", (unsigned long) clock());
     sprintf(s + strlen(s), "%lu", (unsigned long) time(NULL));
#ifdef Solaris
     sysinfo(SI_HW_SERIAL,s + strlen(s), 12);
#endif
#ifdef HAVE_GETHOSTID
     sprintf(s + strlen(s), "%lu", (unsigned long) gethostid());
#endif
#ifdef HAVE_GETDOMAINNAME
     getdomainname(s + strlen(s), 256);
#endif
     gethostname(s + strlen(s), 256);
     sprintf(s + strlen(s), "%u", (unsigned)getuid());
     sprintf(s + strlen(s), "%u", (unsigned)getgid());
     MD5Init(&md5c);
     MD5Update(&md5c, (unsigned char *)s, strlen(s));
     MD5Final(md5key, &md5c);
     sprintf(s + strlen(s), "%lu", (unsigned long) ((time(NULL) + 65121) ^ 0x375F));
     MD5Init(&md5c);
     MD5Update(&md5c, (unsigned char *)s, strlen(s));
     MD5Final(md5key1, &md5c);
#define nextrand    (md5key[j] ^ md5key1[j])
     if (mode) {
	for (j = k = 0; j < 16; j++) {
	    unsigned char rb = nextrand;

#define Rad16(x) ((x) + 'A')
	    key[k++] = Rad16((rb >> 4) & 0xF);
	    key[k++] = Rad16(rb & 0xF);
#undef Rad16
	    if (j & 1) {
                 key[k++] = '-';
	    }
	}
	key[--k] = 0;
     } else {
	for (j = 0; j < 16; j++) {
	    key[j] = nextrand;
	}
     }
}
#undef nextrand



/*
 * Edit job codes into main command line
 *  %% = %
 *  %c = Client's name
 *  %d = Director's name
 *  %e = Job Exit code
 *  %i = JobId
 *  %j = Unique Job name
 *  %l = job level
 *  %n = Unadorned Job name
 *  %t = Job type (Backup, ...)
 *  %r = Recipients
 *  %v = Volume name
 *
 *  omsg = edited output message
 *  imsg = input string containing edit codes (%x)
 *  to = recepients list 
 *
 */
POOLMEM *edit_job_codes(JCR *jcr, char *omsg, char *imsg, const char *to)   
{
   char *p, *q;
   const char *str;
   char add[20];
   char name[MAX_NAME_LENGTH];
   int i;

   *omsg = 0;
   Dmsg1(200, "edit_job_codes: %s\n", imsg);
   for (p=imsg; *p; p++) {
      if (*p == '%') {
	 switch (*++p) {
         case '%':
            str = "%";
	    break;
         case 'c':
	    str = jcr->client_name;
	    if (!str) {
               str = "";
	    }
	    break;
         case 'd':
            str = my_name;            /* Director's name */
	    break;
         case 'e':
	    str = job_status_to_str(jcr->JobStatus); 
	    break;
         case 'i':
            bsnprintf(add, sizeof(add), "%d", jcr->JobId);
	    str = add;
	    break;
         case 'j':                    /* Job name */
	    str = jcr->Job;
	    break;
         case 'l':
	    str = job_level_to_str(jcr->JobLevel);
	    break;
         case 'n':
	     bstrncpy(name, jcr->Job, sizeof(name));
	     /* There are three periods after the Job name */
	     for (i=0; i<3; i++) {
                if ((q=strrchr(name, '.')) != NULL) {
		    *q = 0;
		}
	     }
	     str = name;
	     break;
         case 'r':
	    str = to;
	    break;
         case 't':
	    str = job_type_to_str(jcr->JobType);
	    break;
         case 'v':
	    if (jcr->VolumeName && jcr->VolumeName[0]) {
	       str = jcr->VolumeName;
	    } else {
               str = "";
	    }
	    break;
	 default:
            add[0] = '%';
	    add[1] = *p;
	    add[2] = 0;
	    str = add;
	    break;
	 }
      } else {
	 add[0] = *p;
	 add[1] = 0;
	 str = add;
      }
      Dmsg1(1200, "add_str %s\n", str);
      pm_strcat(&omsg, str);
      Dmsg1(1200, "omsg=%s\n", omsg);
   }
   return omsg;
}

void set_working_directory(char *wd)
{
   struct stat stat_buf; 

   if (wd == NULL) {
      Emsg0(M_ERROR_TERM, 0, _("Working directory not defined. Cannot continue.\n"));
   }
   if (stat(wd, &stat_buf) != 0) {
      Emsg1(M_ERROR_TERM, 0, _("Working Directory: \"%s\" not found. Cannot continue.\n"),
	 wd);
   }
   if (!S_ISDIR(stat_buf.st_mode)) {
      Emsg1(M_ERROR_TERM, 0, _("Working Directory: \"%s\" is not a directory. Cannot continue.\n"),
	 wd);
   }
   working_directory = wd;	      /* set global */
}
