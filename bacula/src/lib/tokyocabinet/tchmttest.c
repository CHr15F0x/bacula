/*************************************************************************************************
 * The test cases of the hash database API
 *                                                      Copyright (C) 2006-2008 Mikio Hirabayashi
 * This file is part of Tokyo Cabinet.
 * Tokyo Cabinet is free software; you can redistribute it and/or modify it under the terms of
 * the GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License or any later version.  Tokyo Cabinet is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 * You should have received a copy of the GNU Lesser General Public License along with Tokyo
 * Cabinet; if not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA.
 *************************************************************************************************/


#include <tcutil.h>
#include <tchdb.h>
#include "myconf.h"

#define RECBUFSIZ      32                // buffer for records

typedef struct {                         // type of structure for write thread
  TCHDB *hdb;
  int rnum;
  bool as;
  bool rnd;
  int id;
} TARGWRITE;

typedef struct {                         // type of structure for read thread
  TCHDB *hdb;
  int rnum;
  bool wb;
  bool rnd;
  int id;
} TARGREAD;

typedef struct {                         // type of structure for remove thread
  TCHDB *hdb;
  int rnum;
  bool rnd;
  int id;
} TARGREMOVE;

typedef struct {                         // type of structure for wicked thread
  TCHDB *hdb;
  int rnum;
  bool nc;
  int id;
  TCMAP *map;
} TARGWICKED;

typedef struct {                         // type of structure for typical thread
  TCHDB *hdb;
  int rnum;
  bool nc;
  int rratio;
  int id;
} TARGTYPICAL;


/* global variables */
const char *g_progname;                  // program name
int g_dbgfd;                             // debugging output


/* function prototypes */
int main(int argc, char **argv);
static void usage(void);
static void iprintf(const char *format, ...);
static void eprint(TCHDB *hdb, const char *func);
static void mprint(TCHDB *hdb);
static int myrand(int range);
static int myrandnd(int range);
static int runwrite(int argc, char **argv);
static int runread(int argc, char **argv);
static int runremove(int argc, char **argv);
static int runwicked(int argc, char **argv);
static int runtypical(int argc, char **argv);
static int procwrite(const char *path, int tnum, int rnum, int bnum, int apow, int fpow,
                     int opts, int omode, bool as, bool rnd);
static int procread(const char *path, int tnum, int rcnum, int omode, bool wb, bool rnd);
static int procremove(const char *path, int tnum, int omode, bool rnd);
static int procwicked(const char *path, int tnum, int rnum, int opts, int omode, bool nc);
static int proctypical(const char *path, int tnum, int rnum, int bnum, int apow, int fpow,
                       int opts, int rcnum, int omode, bool nc, int rratio);
static void *threadwrite(void *targ);
static void *threadread(void *targ);
static void *threadremove(void *targ);
static void *threadwicked(void *targ);
static void *threadtypical(void *targ);


/* main routine */
int main(int argc, char **argv){
  g_progname = argv[0];
  g_dbgfd = -1;
  const char *ebuf = getenv("TCDBGFD");
  if(ebuf) g_dbgfd = atoi(ebuf);
  srand((unsigned int)(tctime() * 1000) % UINT_MAX);
  if(argc < 2) usage();
  int rv = 0;
  if(!strcmp(argv[1], "write")){
    rv = runwrite(argc, argv);
  } else if(!strcmp(argv[1], "read")){
    rv = runread(argc, argv);
  } else if(!strcmp(argv[1], "remove")){
    rv = runremove(argc, argv);
  } else if(!strcmp(argv[1], "wicked")){
    rv = runwicked(argc, argv);
  } else if(!strcmp(argv[1], "typical")){
    rv = runtypical(argc, argv);
  } else {
    usage();
  }
  return rv;
}


/* print the usage and exit */
static void usage(void){
  fprintf(stderr, "%s: test cases of the hash database API of Tokyo Cabinet\n", g_progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s write [-tl] [-td|-tb] [-nl|-nb] [-as] [-rnd] path tnum rnum"
          " [bnum [apow [fpow]]]\n", g_progname);
  fprintf(stderr, "  %s read [-rc num] [-nl|-nb] [-wb] [-rnd] path tnum\n", g_progname);
  fprintf(stderr, "  %s remove [-nl|-nb] [-wb] [-rnd] path tnum\n", g_progname);
  fprintf(stderr, "  %s wicked [-tl] [-td|-tb] [-nl|-nb] [-nc] path tnum rnum\n",
          g_progname);
  fprintf(stderr, "  %s typical [-tl] [-td|-tb] [-rc num] [-nl|-nb] [-nc] [-rr num]"
          " path tnum rnum [bnum [apow [fpow]]]\n", g_progname);
  fprintf(stderr, "\n");
  exit(1);
}


/* print formatted information string and flush the buffer */
static void iprintf(const char *format, ...){
  va_list ap;
  va_start(ap, format);
  vprintf(format, ap);
  fflush(stdout);
  va_end(ap);
}


/* print error message of hash database */
static void eprint(TCHDB *hdb, const char *func){
  const char *path = tchdbpath(hdb);
  int ecode = tchdbecode(hdb);
  fprintf(stderr, "%s: %s: %s: error: %d: %s\n",
          g_progname, path ? path : "-", func, ecode, tchdberrmsg(ecode));
}


/* print members of hash database */
static void mprint(TCHDB *hdb){
  if(hdb->cnt_writerec < 0) return;
  iprintf("bucket number: %lld\n", (long long)tchdbbnum(hdb));
  iprintf("used bucket number: %lld\n", (long long)tchdbbnumused(hdb));
  iprintf("cnt_writerec: %lld\n", (long long)hdb->cnt_writerec);
  iprintf("cnt_reuserec: %lld\n", (long long)hdb->cnt_reuserec);
  iprintf("cnt_moverec: %lld\n", (long long)hdb->cnt_moverec);
  iprintf("cnt_readrec: %lld\n", (long long)hdb->cnt_readrec);
  iprintf("cnt_searchfbp: %lld\n", (long long)hdb->cnt_searchfbp);
  iprintf("cnt_insertfbp: %lld\n", (long long)hdb->cnt_insertfbp);
  iprintf("cnt_splicefbp: %lld\n", (long long)hdb->cnt_splicefbp);
  iprintf("cnt_dividefbp: %lld\n", (long long)hdb->cnt_dividefbp);
  iprintf("cnt_mergefbp: %lld\n", (long long)hdb->cnt_mergefbp);
  iprintf("cnt_reducefbp: %lld\n", (long long)hdb->cnt_reducefbp);
  iprintf("cnt_appenddrp: %lld\n", (long long)hdb->cnt_appenddrp);
  iprintf("cnt_deferdrp: %lld\n", (long long)hdb->cnt_deferdrp);
  iprintf("cnt_flushdrp: %lld\n", (long long)hdb->cnt_flushdrp);
  iprintf("cnt_adjrecc: %lld\n", (long long)hdb->cnt_adjrecc);
}


/* get a random number */
static int myrand(int range){
  return (int)((double)range * rand() / (RAND_MAX + 1.0));
}


/* get a random number based on normal distribution */
static int myrandnd(int range){
  int num = (int)tcdrandnd(range >> 1, range / 10);
  return (num < 0 || num >= range) ? 0 : num;
}


/* parse arguments of write command */
static int runwrite(int argc, char **argv){
  char *path = NULL;
  char *tstr = NULL;
  char *rstr = NULL;
  char *bstr = NULL;
  char *astr = NULL;
  char *fstr = NULL;
  int opts = 0;
  int omode = 0;
  bool as = false;
  bool rnd = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-tl")){
        opts |= HDBTLARGE;
      } else if(!strcmp(argv[i], "-td")){
        opts |= HDBTDEFLATE;
      } else if(!strcmp(argv[i], "-tb")){
        opts |= HDBTTCBS;
      } else if(!strcmp(argv[i], "-nl")){
        omode |= HDBONOLCK;
      } else if(!strcmp(argv[i], "-nb")){
        omode |= HDBOLCKNB;
      } else if(!strcmp(argv[i], "-as")){
        as = true;
      } else if(!strcmp(argv[i], "-rnd")){
        rnd = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else if(!tstr){
      tstr = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else if(!bstr){
      bstr = argv[i];
    } else if(!astr){
      astr = argv[i];
    } else if(!fstr){
      fstr = argv[i];
    } else {
      usage();
    }
  }
  if(!path || !tstr || !rstr) usage();
  int tnum = atoi(tstr);
  int rnum = atoi(rstr);
  if(tnum < 1 || rnum < 1) usage();
  int bnum = bstr ? atoi(bstr) : -1;
  int apow = astr ? atoi(astr) : -1;
  int fpow = fstr ? atoi(fstr) : -1;
  int rv = procwrite(path, tnum, rnum, bnum, apow, fpow, opts, omode, as, rnd);
  return rv;
}


/* parse arguments of read command */
static int runread(int argc, char **argv){
  char *path = NULL;
  char *tstr = NULL;
  int rcnum = 0;
  int omode = 0;
  bool wb = false;
  bool rnd = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-rc")){
        if(++i >= argc) usage();
        rcnum = atoi(argv[i]);
      } else if(!strcmp(argv[i], "-nl")){
        omode |= HDBONOLCK;
      } else if(!strcmp(argv[i], "-nb")){
        omode |= HDBOLCKNB;
      } else if(!strcmp(argv[i], "-wb")){
        wb = true;
      } else if(!strcmp(argv[i], "-rnd")){
        rnd = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else if(!tstr){
      tstr = argv[i];
    } else {
      usage();
    }
  }
  if(!path || !tstr) usage();
  int tnum = atoi(tstr);
  if(tnum < 1) usage();
  int rv = procread(path, tnum, rcnum, omode, wb, rnd);
  return rv;
}


/* parse arguments of remove command */
static int runremove(int argc, char **argv){
  char *path = NULL;
  char *tstr = NULL;
  int omode = 0;
  bool rnd = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-nl")){
        omode |= HDBONOLCK;
      } else if(!strcmp(argv[i], "-nb")){
        omode |= HDBOLCKNB;
      } else if(!strcmp(argv[i], "-rnd")){
        rnd = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else if(!tstr){
      tstr = argv[i];
    } else {
      usage();
    }
  }
  if(!path || !tstr) usage();
  int tnum = atoi(tstr);
  if(tnum < 1) usage();
  int rv = procremove(path, tnum, omode, rnd);
  return rv;
}


/* parse arguments of wicked command */
static int runwicked(int argc, char **argv){
  char *path = NULL;
  char *tstr = NULL;
  char *rstr = NULL;
  int opts = 0;
  int omode = 0;
  bool nc = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-tl")){
        opts |= HDBTLARGE;
      } else if(!strcmp(argv[i], "-td")){
        opts |= HDBTDEFLATE;
      } else if(!strcmp(argv[i], "-tb")){
        opts |= HDBTTCBS;
      } else if(!strcmp(argv[i], "-nl")){
        omode |= HDBONOLCK;
      } else if(!strcmp(argv[i], "-nb")){
        omode |= HDBOLCKNB;
      } else if(!strcmp(argv[i], "-nc")){
        nc = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else if(!tstr){
      tstr = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else {
      usage();
    }
  }
  if(!path || !tstr || !rstr) usage();
  int tnum = atoi(tstr);
  int rnum = atoi(rstr);
  if(tnum < 1 || rnum < 1) usage();
  int rv = procwicked(path, tnum, rnum, opts, omode, nc);
  return rv;
}


/* parse arguments of typical command */
static int runtypical(int argc, char **argv){
  char *path = NULL;
  char *tstr = NULL;
  char *rstr = NULL;
  char *bstr = NULL;
  char *astr = NULL;
  char *fstr = NULL;
  int opts = 0;
  int rcnum = 0;
  int omode = 0;
  int rratio = -1;
  bool nc = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-tl")){
        opts |= HDBTLARGE;
      } else if(!strcmp(argv[i], "-td")){
        opts |= HDBTDEFLATE;
      } else if(!strcmp(argv[i], "-tb")){
        opts |= HDBTTCBS;
      } else if(!strcmp(argv[i], "-rc")){
        if(++i >= argc) usage();
        rcnum = atoi(argv[i]);
      } else if(!strcmp(argv[i], "-nl")){
        omode |= HDBONOLCK;
      } else if(!strcmp(argv[i], "-nb")){
        omode |= HDBOLCKNB;
      } else if(!strcmp(argv[i], "-nc")){
        nc = true;
      } else if(!strcmp(argv[i], "-rr")){
        if(++i >= argc) usage();
        rratio = atoi(argv[i]);
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else if(!tstr){
      tstr = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else if(!bstr){
      bstr = argv[i];
    } else if(!astr){
      astr = argv[i];
    } else if(!fstr){
      fstr = argv[i];
    } else {
      usage();
    }
  }
  if(!path || !tstr || !rstr) usage();
  int tnum = atoi(tstr);
  int rnum = atoi(rstr);
  if(tnum < 1 || rnum < 1) usage();
  int bnum = bstr ? atoi(bstr) : -1;
  int apow = astr ? atoi(astr) : -1;
  int fpow = fstr ? atoi(fstr) : -1;
  int rv = proctypical(path, tnum, rnum, bnum, apow, fpow, opts, rcnum, omode, nc, rratio);
  return rv;
}


/* perform write command */
static int procwrite(const char *path, int tnum, int rnum, int bnum, int apow, int fpow,
                     int opts, int omode, bool as, bool rnd){
  iprintf("<Writing Test>\n  path=%s  tnum=%d  rnum=%d  bnum=%d  apow=%d  fpow=%d"
          "  opts=%d  omode=%d  as=%d  rnd=%d\n\n",
          path, tnum, rnum, bnum, apow, fpow, opts, omode, as, rnd);
  bool err = false;
  double stime = tctime();
  TCHDB *hdb = tchdbnew();
  if(g_dbgfd >= 0) tchdbsetdbgfd(hdb, g_dbgfd);
  if(!tchdbsetmutex(hdb)){
    eprint(hdb, "tchdbsetmutex");
    err = true;
  }
  if(!tchdbtune(hdb, bnum, apow, fpow, opts)){
    eprint(hdb, "tchdbtune");
    err = true;
  }
  if(!tchdbopen(hdb, path, HDBOWRITER | HDBOCREAT | HDBOTRUNC | omode)){
    eprint(hdb, "tchdbopen");
    err = true;
  }
  TARGWRITE targs[tnum];
  pthread_t threads[tnum];
  if(tnum == 1){
    targs[0].hdb = hdb;
    targs[0].rnum = rnum;
    targs[0].as = as;
    targs[0].rnd = rnd;
    targs[0].id = 0;
    if(threadwrite(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].hdb = hdb;
      targs[i].rnum = rnum;
      targs[i].as = as;
      targs[i].rnd = rnd;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadwrite, targs + i) != 0){
        eprint(hdb, "pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint(hdb, "pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  iprintf("record number: %llu\n", (unsigned long long)tchdbrnum(hdb));
  iprintf("size: %llu\n", (unsigned long long)tchdbfsiz(hdb));
  mprint(hdb);
  if(!tchdbclose(hdb)){
    eprint(hdb, "tchdbclose");
    err = true;
  }
  tchdbdel(hdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* perform read command */
static int procread(const char *path, int tnum, int rcnum, int omode, bool wb, bool rnd){
  iprintf("<Reading Test>\n  path=%s  tnum=%d  rcnum=%d  omode=%d  wb=%d  rnd=%d\n\n",
          path, tnum, rcnum, omode, wb, rnd);
  bool err = false;
  double stime = tctime();
  TCHDB *hdb = tchdbnew();
  if(g_dbgfd >= 0) tchdbsetdbgfd(hdb, g_dbgfd);
  if(!tchdbsetmutex(hdb)){
    eprint(hdb, "tchdbsetmutex");
    err = true;
  }
  if(!tchdbsetcache(hdb, rcnum)){
    eprint(hdb, "tchdbsetcache");
    err = true;
  }
  if(!tchdbopen(hdb, path, HDBOREADER | omode)){
    eprint(hdb, "tchdbopen");
    err = true;
  }
  int rnum = tchdbrnum(hdb) / tnum;
  TARGREAD targs[tnum];
  pthread_t threads[tnum];
  if(tnum == 1){
    targs[0].hdb = hdb;
    targs[0].rnum = rnum;
    targs[0].wb = wb;
    targs[0].rnd = rnd;
    targs[0].id = 0;
    if(threadread(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].hdb = hdb;
      targs[i].rnum = rnum;
      targs[i].wb = wb;
      targs[i].rnd = rnd;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadread, targs + i) != 0){
        eprint(hdb, "pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint(hdb, "pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  iprintf("record number: %llu\n", (unsigned long long)tchdbrnum(hdb));
  iprintf("size: %llu\n", (unsigned long long)tchdbfsiz(hdb));
  mprint(hdb);
  if(!tchdbclose(hdb)){
    eprint(hdb, "tchdbclose");
    err = true;
  }
  tchdbdel(hdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* perform remove command */
static int procremove(const char *path, int tnum, int omode, bool rnd){
  iprintf("<Removing Test>\n  path=%s  tnum=%d  omode=%d  rnd=%d\n\n", path, tnum, omode, rnd);
  bool err = false;
  double stime = tctime();
  TCHDB *hdb = tchdbnew();
  if(g_dbgfd >= 0) tchdbsetdbgfd(hdb, g_dbgfd);
  if(!tchdbsetmutex(hdb)){
    eprint(hdb, "tchdbsetmutex");
    err = true;
  }
  if(!tchdbopen(hdb, path, HDBOWRITER | omode)){
    eprint(hdb, "tchdbopen");
    err = true;
  }
  int rnum = tchdbrnum(hdb) / tnum;
  TARGREMOVE targs[tnum];
  pthread_t threads[tnum];
  if(tnum == 1){
    targs[0].hdb = hdb;
    targs[0].rnum = rnum;
    targs[0].rnd = rnd;
    targs[0].id = 0;
    if(threadremove(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].hdb = hdb;
      targs[i].rnum = rnum;
      targs[i].rnd = rnd;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadremove, targs + i) != 0){
        eprint(hdb, "pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint(hdb, "pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  iprintf("record number: %llu\n", (unsigned long long)tchdbrnum(hdb));
  iprintf("size: %llu\n", (unsigned long long)tchdbfsiz(hdb));
  mprint(hdb);
  if(!tchdbclose(hdb)){
    eprint(hdb, "tchdbclose");
    err = true;
  }
  tchdbdel(hdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* perform wicked command */
static int procwicked(const char *path, int tnum, int rnum, int opts, int omode, bool nc){
  iprintf("<Writing Test>\n  path=%s  tnum=%d  rnum=%d  opts=%d  omode=%d  nc=%d\n\n",
          path, tnum, rnum, opts, omode, nc);
  bool err = false;
  double stime = tctime();
  TCHDB *hdb = tchdbnew();
  if(g_dbgfd >= 0) tchdbsetdbgfd(hdb, g_dbgfd);
  if(!tchdbsetmutex(hdb)){
    eprint(hdb, "tchdbsetmutex");
    err = true;
  }
  if(!tchdbtune(hdb, rnum / 50, 2, -1, opts)){
    eprint(hdb, "tchdbtune");
    err = true;
  }
  if(!tchdbsetcache(hdb, rnum / 2)){
    eprint(hdb, "tchdbsetcache");
    err = true;
  }
  if(!tchdbopen(hdb, path, HDBOWRITER | HDBOCREAT | HDBOTRUNC | omode)){
    eprint(hdb, "tchdbopen");
    err = true;
  }
  if(!tchdbiterinit(hdb)){
    eprint(hdb, "tchdbiterinit");
    err = true;
  }
  TARGWICKED targs[tnum];
  pthread_t threads[tnum];
  TCMAP *map = tcmapnew();
  if(tnum == 1){
    targs[0].hdb = hdb;
    targs[0].rnum = rnum;
    targs[0].nc = nc;
    targs[0].id = 0;
    targs[0].map = map;
    if(threadwicked(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].hdb = hdb;
      targs[i].rnum = rnum;
      targs[i].nc = nc;
      targs[i].id = i;
      targs[i].map = map;
      targs[i].map = map;
      if(pthread_create(threads + i, NULL, threadwicked, targs + i) != 0){
        eprint(hdb, "pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint(hdb, "pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  if(!nc){
    if(!tchdbsync(hdb)){
      eprint(hdb, "tchdbsync");
      err = true;
    }
    if(tchdbrnum(hdb) != tcmaprnum(map)){
      eprint(hdb, "(validation)");
      err = true;
    }
    int end = rnum * tnum;
    for(int i = 1; i <= end && !err; i++){
      char kbuf[RECBUFSIZ];
      int ksiz = sprintf(kbuf, "%d", i - 1);
      int vsiz;
      const char *vbuf = tcmapget(map, kbuf, ksiz, &vsiz);
      int rsiz;
      char *rbuf = tchdbget(hdb, kbuf, ksiz, &rsiz);
      if(vbuf){
        putchar('.');
        if(!rbuf){
          eprint(hdb, "tchdbget");
          err = true;
        } else if(rsiz != vsiz || memcmp(rbuf, vbuf, rsiz)){
          eprint(hdb, "(validation)");
          err = true;
        }
      } else {
        putchar('*');
        if(rbuf || tchdbecode(hdb) != TCENOREC){
          eprint(hdb, "(validation)");
          err = true;
        }
      }
      free(rbuf);
      if(i % 50 == 0) iprintf(" (%08d)\n", i);
    }
    if(rnum % 50 > 0) iprintf(" (%08d)\n", rnum);
  }
  tcmapdel(map);
  iprintf("record number: %llu\n", (unsigned long long)tchdbrnum(hdb));
  iprintf("size: %llu\n", (unsigned long long)tchdbfsiz(hdb));
  mprint(hdb);
  if(!tchdbclose(hdb)){
    eprint(hdb, "tchdbclose");
    err = true;
  }
  tchdbdel(hdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* perform typical command */
static int proctypical(const char *path, int tnum, int rnum, int bnum, int apow, int fpow,
                       int opts, int rcnum, int omode, bool nc, int rratio){
  iprintf("<Typical Access Test>\n  path=%s  tnum=%d  rnum=%d  bnum=%d  apow=%d  fpow=%d"
          "  opts=%d  rcnum=%d  omode=%d  nc=%d  rratio=%d\n\n",
          path, tnum, rnum, bnum, apow, fpow, opts, rcnum, omode, nc, rratio);
  bool err = false;
  double stime = tctime();
  TCHDB *hdb = tchdbnew();
  if(g_dbgfd >= 0) tchdbsetdbgfd(hdb, g_dbgfd);
  if(!tchdbsetmutex(hdb)){
    eprint(hdb, "tchdbsetmutex");
    err = true;
  }
  if(!tchdbtune(hdb, bnum, apow, fpow, opts)){
    eprint(hdb, "tchdbtune");
    err = true;
  }
  if(!tchdbsetcache(hdb, rcnum)){
    eprint(hdb, "tchdbsetcache");
    err = true;
  }
  if(!tchdbopen(hdb, path, HDBOWRITER | HDBOCREAT | HDBOTRUNC | omode)){
    eprint(hdb, "tchdbopen");
    err = true;
  }
  TARGTYPICAL targs[tnum];
  pthread_t threads[tnum];
  if(tnum == 1){
    targs[0].hdb = hdb;
    targs[0].rnum = rnum;
    targs[0].nc = nc;
    targs[0].rratio = rratio;
    targs[0].id = 0;
    if(threadtypical(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].hdb = hdb;
      targs[i].rnum = rnum;
      targs[i].nc = nc;
      targs[i].rratio= rratio;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadtypical, targs + i) != 0){
        eprint(hdb, "pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint(hdb, "pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  iprintf("record number: %llu\n", (unsigned long long)tchdbrnum(hdb));
  iprintf("size: %llu\n", (unsigned long long)tchdbfsiz(hdb));
  mprint(hdb);
  if(!tchdbclose(hdb)){
    eprint(hdb, "tchdbclose");
    err = true;
  }
  tchdbdel(hdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* thread the write function */
static void *threadwrite(void *targ){
  TCHDB *hdb = ((TARGWRITE *)targ)->hdb;
  int rnum = ((TARGWRITE *)targ)->rnum;
  bool as = ((TARGWRITE *)targ)->as;
  bool rnd = ((TARGWRITE *)targ)->rnd;
  int id = ((TARGWRITE *)targ)->id;
  bool err = false;
  int base = id * rnum;
  for(int i = 1; i <= rnum; i++){
    char buf[RECBUFSIZ];
    int len = sprintf(buf, "%08d", base + (rnd ? myrand(i) : i));
    if(as){
      if(!tchdbputasync(hdb, buf, len, buf, len)){
        eprint(hdb, "tchdbput");
        err = true;
        break;
      }
    } else {
      if(!tchdbput(hdb, buf, len, buf, len)){
        eprint(hdb, "tchdbput");
        err = true;
        break;
      }
    }
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  return err ? "error" : NULL;
}


/* thread the read function */
static void *threadread(void *targ){
  TCHDB *hdb = ((TARGREAD *)targ)->hdb;
  int rnum = ((TARGREAD *)targ)->rnum;
  bool wb = ((TARGREAD *)targ)->wb;
  bool rnd = ((TARGREAD *)targ)->rnd;
  int id = ((TARGREAD *)targ)->id;
  bool err = false;
  int base = id * rnum;
  for(int i = 1; i <= rnum && !err; i++){
    char kbuf[RECBUFSIZ];
    int ksiz = sprintf(kbuf, "%08d", base + (rnd ? myrandnd(i) : i));
    int vsiz;
    if(wb){
      char vbuf[RECBUFSIZ];
      int vsiz = tchdbget3(hdb, kbuf, ksiz, vbuf, RECBUFSIZ);
      if(vsiz < 0 && (!rnd || tchdbecode(hdb) != TCENOREC)){
        eprint(hdb, "tchdbget3");
        err = true;
      }
    } else {
      char *vbuf = tchdbget(hdb, kbuf, ksiz, &vsiz);
      if(!vbuf && (!rnd || tchdbecode(hdb) != TCENOREC)){
        eprint(hdb, "tchdbget");
        err = true;
      }
      free(vbuf);
    }
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  return err ? "error" : NULL;
}


/* thread the remove function */
static void *threadremove(void *targ){
  TCHDB *hdb = ((TARGREMOVE *)targ)->hdb;
  int rnum = ((TARGREMOVE *)targ)->rnum;
  bool rnd = ((TARGREMOVE *)targ)->rnd;
  int id = ((TARGREMOVE *)targ)->id;
  bool err = false;
  int base = id * rnum;
  for(int i = 1; i <= rnum; i++){
    char kbuf[RECBUFSIZ];
    int ksiz = sprintf(kbuf, "%08d", base + (rnd ? myrand(i + 1) : i));
    if(!tchdbout(hdb, kbuf, ksiz) && (!rnd || tchdbecode(hdb) != TCENOREC)){
      eprint(hdb, "tchdbout");
      err = true;
      break;
    }
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  return err ? "error" : NULL;
}


/* thread the wicked function */
static void *threadwicked(void *targ){
  TCHDB *hdb = ((TARGWICKED *)targ)->hdb;
  int rnum = ((TARGWICKED *)targ)->rnum;
  bool nc = ((TARGWICKED *)targ)->nc;
  int id = ((TARGWICKED *)targ)->id;
  TCMAP *map = ((TARGWICKED *)targ)->map;
  bool err = false;
  for(int i = 1; i <= rnum && !err; i++){
    char kbuf[RECBUFSIZ];
    int ksiz = sprintf(kbuf, "%d", myrand(rnum * (id + 1)));
    char vbuf[RECBUFSIZ];
    int vsiz = myrand(RECBUFSIZ);
    memset(vbuf, '*', vsiz);
    vbuf[vsiz] = '\0';
    char *rbuf;
    if(!nc) tcglobalmutexlock();
    switch(myrand(16)){
    case 0:
      if(id == 0) putchar('0');
      if(!tchdbput(hdb, kbuf, ksiz, vbuf, vsiz)){
        eprint(hdb, "tchdbput");
        err = true;
      }
      if(!nc) tcmapput(map, kbuf, ksiz, vbuf, vsiz);
      break;
    case 1:
      if(id == 0) putchar('1');
      if(!tchdbput2(hdb, kbuf, vbuf)){
        eprint(hdb, "tchdbput2");
        err = true;
      }
      if(!nc) tcmapput2(map, kbuf, vbuf);
      break;
    case 2:
      if(id == 0) putchar('2');
      if(!tchdbputkeep(hdb, kbuf, ksiz, vbuf, vsiz) && tchdbecode(hdb) != TCEKEEP){
        eprint(hdb, "tchdbputkeep");
        err = true;
      }
      if(!nc) tcmapputkeep(map, kbuf, ksiz, vbuf, vsiz);
      break;
    case 3:
      if(id == 0) putchar('3');
      if(!tchdbputkeep2(hdb, kbuf, vbuf) && tchdbecode(hdb) != TCEKEEP){
        eprint(hdb, "tchdbputkeep2");
        err = true;
      }
      if(!nc) tcmapputkeep2(map, kbuf, vbuf);
      break;
    case 4:
      if(id == 0) putchar('4');
      if(!tchdbputcat(hdb, kbuf, ksiz, vbuf, vsiz)){
        eprint(hdb, "tchdbputcat");
        err = true;
      }
      if(!nc) tcmapputcat(map, kbuf, ksiz, vbuf, vsiz);
      break;
    case 5:
      if(id == 0) putchar('5');
      if(!tchdbputcat2(hdb, kbuf, vbuf)){
        eprint(hdb, "tchdbputcat2");
        err = true;
      }
      if(!nc) tcmapputcat2(map, kbuf, vbuf);
      break;
    case 6:
      if(id == 0) putchar('6');
      if(i > rnum / 4 * 3){
        if(!tchdbputasync(hdb, kbuf, ksiz, vbuf, vsiz)){
          eprint(hdb, "tchdbputasync");
          err = true;
        }
      } else {
        if(!tchdbput(hdb, kbuf, ksiz, vbuf, vsiz)){
          eprint(hdb, "tchdbput");
          err = true;
        }
      }
      if(!nc) tcmapput(map, kbuf, ksiz, vbuf, vsiz);
      break;
    case 7:
      if(id == 0) putchar('7');
      if(i > rnum / 4 * 3){
        if(!tchdbputasync2(hdb, kbuf, vbuf)){
          eprint(hdb, "tchdbputasync2");
          err = true;
        }
      } else {
        if(!tchdbput2(hdb, kbuf, vbuf)){
          eprint(hdb, "tchdbput2");
          err = true;
        }
      }
      if(!nc) tcmapput2(map, kbuf, vbuf);
      break;
    case 8:
      if(id == 0) putchar('8');
      if(myrand(10) == 0){
        if(!tchdbout(hdb, kbuf, ksiz) && tchdbecode(hdb) != TCENOREC){
          eprint(hdb, "tchdbout");
          err = true;
        }
        if(!nc) tcmapout(map, kbuf, ksiz);
      }
      break;
    case 9:
      if(id == 0) putchar('9');
      if(myrand(10) == 0){
        if(!tchdbout2(hdb, kbuf) && tchdbecode(hdb) != TCENOREC){
          eprint(hdb, "tchdbout2");
          err = true;
        }
        if(!nc) tcmapout2(map, kbuf);
      }
      break;
    case 10:
      if(id == 0) putchar('A');
      if(!(rbuf = tchdbget(hdb, kbuf, ksiz, &vsiz))){
        if(tchdbecode(hdb) != TCENOREC){
          eprint(hdb, "tchdbget");
          err = true;
        }
        rbuf = tcsprintf("[%d]", myrand(i + 1));
        vsiz = strlen(rbuf);
      }
      vsiz += myrand(vsiz);
      if(myrand(3) == 0) vsiz += PATH_MAX;
      rbuf = tcrealloc(rbuf, vsiz + 1);
      for(int j = 0; j < vsiz; j++){
        rbuf[j] = myrand(0x100);
      }
      if(!tchdbput(hdb, kbuf, ksiz, rbuf, vsiz)){
        eprint(hdb, "tchdbput");
        err = true;
      }
      if(!nc) tcmapput(map, kbuf, ksiz, rbuf, vsiz);
      free(rbuf);
      break;
    case 11:
      if(id == 0) putchar('B');
      if(!(rbuf = tchdbget(hdb, kbuf, ksiz, &vsiz)) && tchdbecode(hdb) != TCENOREC){
        eprint(hdb, "tchdbget");
        err = true;
      }
      free(rbuf);
      break;
    case 12:
      if(id == 0) putchar('C');
      if(!(rbuf = tchdbget2(hdb, kbuf)) && tchdbecode(hdb) != TCENOREC){
        eprint(hdb, "tchdbget");
        err = true;
      }
      free(rbuf);
      break;
    case 13:
      if(id == 0) putchar('D');
      if(myrand(1) == 0) vsiz = 1;
      if((vsiz = tchdbget3(hdb, kbuf, ksiz, vbuf, vsiz)) < 0 && tchdbecode(hdb) != TCENOREC){
        eprint(hdb, "tchdbget3");
        err = true;
      }
      break;
    case 14:
      if(id == 0) putchar('E');
      if(myrand(rnum / 50) == 0){
        if(!tchdbiterinit(hdb)){
          eprint(hdb, "tchdbiterinit");
          err = true;
        }
      }
      TCXSTR *ikey = tcxstrnew();
      TCXSTR *ival = tcxstrnew();
      for(int j = myrand(rnum) / 1000 + 1; j >= 0; j--){
        if(j % 3 == 0){
          if(!tchdbiternext3(hdb, ikey, ival)){
            int ecode = tchdbecode(hdb);
            if(ecode != TCEINVALID && ecode != TCENOREC){
              eprint(hdb, "tchdbiternext3");
              err = true;
            }
          }
        } else {
          int iksiz;
          char *ikbuf = tchdbiternext(hdb, &iksiz);
          if(ikbuf){
            free(ikbuf);
          } else {
            int ecode = tchdbecode(hdb);
            if(ecode != TCEINVALID && ecode != TCENOREC){
              eprint(hdb, "tchdbiternext");
              err = true;
            }
          }
        }
      }
      tcxstrdel(ival);
      tcxstrdel(ikey);
      break;
    default:
      if(id == 0) putchar('@');
      if(myrand(10000) == 0) srand((unsigned int)(tctime() * 1000) % UINT_MAX);
      break;
    }
    if(!nc) tcglobalmutexunlock();
    if(id == 0){
      if(i % 50 == 0) iprintf(" (%08d)\n", i);
      if(id == 0 && i == rnum / 4){
        if(!tchdboptimize(hdb, rnum / 50, -1, -1, -1)){
          eprint(hdb, "tchdboptimize");
          err = true;
        }
        if(!tchdbiterinit(hdb)){
          eprint(hdb, "tchdbiterinit");
          err = true;
        }
      }
    }
  }
  return err ? "error" : NULL;
}


/* thread the typical function */
static void *threadtypical(void *targ){
  TCHDB *hdb = ((TARGTYPICAL *)targ)->hdb;
  int rnum = ((TARGTYPICAL *)targ)->rnum;
  bool nc = ((TARGTYPICAL *)targ)->nc;
  int rratio = ((TARGTYPICAL *)targ)->rratio;
  int id = ((TARGTYPICAL *)targ)->id;
  bool err = false;
  TCMAP *map = (!nc && id == 0) ? tcmapnew2(rnum + 1) : NULL;
  int base = id * rnum;
  int mrange = tclmax(50 + rratio, 100);
  for(int i = 1; !err && i <= rnum; i++){
    char buf[RECBUFSIZ];
    int len = sprintf(buf, "%08d", base + myrandnd(i));
    int rnd = myrand(mrange);
    if(rnd < 10){
      if(!tchdbput(hdb, buf, len, buf, len)){
        eprint(hdb, "tchdbput");
        err = true;
      }
      if(map) tcmapput(map, buf, len, buf, len);
    } else if(rnd < 15){
      if(!tchdbputkeep(hdb, buf, len, buf, len) && tchdbecode(hdb) != TCEKEEP){
        eprint(hdb, "tchdbputkeep");
        err = true;
      }
      if(map) tcmapputkeep(map, buf, len, buf, len);
    } else if(rnd < 20){
      if(!tchdbputcat(hdb, buf, len, buf, len)){
        eprint(hdb, "tchdbputcat");
        err = true;
      }
      if(map) tcmapputcat(map, buf, len, buf, len);
    } else if(rnd < 25){
      if(i > rnum / 10 * 9){
        if(!tchdbputasync(hdb, buf, len, buf, len)){
          eprint(hdb, "tchdbputasync");
          err = true;
        }
      } else {
        if(!tchdbput(hdb, buf, len, buf, len)){
          eprint(hdb, "tchdbput");
          err = true;
        }
      }
      if(map) tcmapput(map, buf, len, buf, len);
    } else if(rnd < 30){
      if(!tchdbout(hdb, buf, len) && tchdbecode(hdb) && tchdbecode(hdb) != TCENOREC){
        eprint(hdb, "tchdbout");
        err = true;
      }
      if(map) tcmapout(map, buf, len);
    } else if(rnd < 31){
      if(myrand(10) == 0 && !tchdbiterinit(hdb) && tchdbecode(hdb) != TCENOREC){
        eprint(hdb, "tchdbiterinit");
        err = true;
      }
      for(int j = 0; !err && j < 10; j++){
        int ksiz;
        char *kbuf = tchdbiternext(hdb, &ksiz);
        if(kbuf){
          free(kbuf);
        } else if(tchdbecode(hdb) != TCEINVALID && tchdbecode(hdb) != TCENOREC){
          eprint(hdb, "tchdbiternext");
          err = true;
        }
      }
    } else {
      int vsiz;
      char *vbuf = tchdbget(hdb, buf, len, &vsiz);
      if(vbuf){
        if(map){
          int msiz;
          const char *mbuf = tcmapget(map, buf, len, &msiz);
          if(msiz != vsiz || memcmp(mbuf, vbuf, vsiz)){
            eprint(hdb, "(validation)");
            err = true;
          }
        }
        free(vbuf);
      } else {
        if(tchdbecode(hdb) != TCENOREC){
          eprint(hdb, "tchdbget");
          err = true;
        }
        if(map && tcmapget(map, buf, len, &vsiz)){
          eprint(hdb, "(validation)");
          err = true;
        }
      }
    }
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  if(map){
    tcmapiterinit(map);
    int ksiz;
    const char *kbuf;
    while(!err && (kbuf = tcmapiternext(map, &ksiz)) != NULL){
      int vsiz;
      char *vbuf = tchdbget(hdb, kbuf, ksiz, &vsiz);
      if(vbuf){
        int msiz;
        const char *mbuf = tcmapget(map, kbuf, ksiz, &msiz);
        if(!mbuf || msiz != vsiz || memcmp(mbuf, vbuf, vsiz)){
          eprint(hdb, "(validation)");
          err = true;
        }
        free(vbuf);
      } else {
        eprint(hdb, "(validation)");
        err = true;
      }
    }
    tcmapdel(map);
  }
  return err ? "error" : NULL;
}



// END OF FILE
