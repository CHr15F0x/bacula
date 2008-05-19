/*************************************************************************************************
 * The test cases of the on-memory database API
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
#include "myconf.h"

#define RECBUFSIZ      32                // buffer for records

typedef struct {                         // type of structure for combo thread
  TCMDB *mdb;
  int rnum;
  bool rnd;
  int id;
} TARGCOMBO;

typedef struct {                         // type of structure for typical thread
  TCMDB *mdb;
  int rnum;
  bool nc;
  int rratio;
  int id;
} TARGTYPICAL;


/* global variables */
const char *g_progname;                  // program name


/* function prototypes */
int main(int argc, char **argv);
static void usage(void);
static void iprintf(const char *format, ...);
static void eprint(const char *func);
static int myrand(int range);
static int myrandnd(int range);
static int runcombo(int argc, char **argv);
static int runtypical(int argc, char **argv);
static int proccombo(int tnum, int rnum, int bnum, bool rnd);
static int proctypical(int tnum, int rnum, int bnum, bool nc, int rratio);
static void *threadwrite(void *targ);
static void *threadread(void *targ);
static void *threadremove(void *targ);
static void *threadtypical(void *targ);


/* main routine */
int main(int argc, char **argv){
  g_progname = argv[0];
  srand((unsigned int)(tctime() * 1000) % UINT_MAX);
  if(argc < 2) usage();
  int rv = 0;
  if(!strcmp(argv[1], "combo")){
    rv = runcombo(argc, argv);
  } else if(!strcmp(argv[1], "typical")){
    rv = runtypical(argc, argv);
  } else {
    usage();
  }
  return rv;
}


/* print the usage and exit */
static void usage(void){
  fprintf(stderr, "%s: test cases of the on-memory database API of Tokyo Cabinet\n", g_progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s combo [-rnd] tnum rnum [bnum]\n", g_progname);
  fprintf(stderr, "  %s typical [-nc] [-rr num] tnum rnum [bnum]\n", g_progname);
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


/* print error message of on-memory database */
static void eprint(const char *func){
  fprintf(stderr, "%s: %s: error\n", g_progname, func);
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


/* parse arguments of combo command */
static int runcombo(int argc, char **argv){
  char *tstr = NULL;
  char *rstr = NULL;
  char *bstr = NULL;
  bool rnd = false;
  for(int i = 2; i < argc; i++){
    if(!tstr && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-rnd")){
        rnd = true;
      } else {
        usage();
      }
    } else if(!tstr){
      tstr = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else if(!bstr){
      bstr = argv[i];
    } else {
      usage();
    }
  }
  if(!tstr || !rstr) usage();
  int tnum = atoi(tstr);
  int rnum = atoi(rstr);
  if(tnum < 1 || rnum < 1) usage();
  int bnum = bstr ? atoi(bstr) : -1;
  int rv = proccombo(tnum, rnum, bnum, rnd);
  return rv;
}


/* parse arguments of typical command */
static int runtypical(int argc, char **argv){
  char *tstr = NULL;
  char *rstr = NULL;
  char *bstr = NULL;
  int rratio = -1;
  bool nc = false;
  for(int i = 2; i < argc; i++){
    if(!tstr && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-nc")){
        nc = true;
      } else if(!strcmp(argv[i], "-rr")){
        if(++i >= argc) usage();
        rratio = atoi(argv[i]);
      } else {
        usage();
      }
    } else if(!tstr){
      tstr = argv[i];
    } else if(!rstr){
      rstr = argv[i];
    } else if(!bstr){
      bstr = argv[i];
    } else {
      usage();
    }
  }
  if(!tstr || !rstr) usage();
  int tnum = atoi(tstr);
  int rnum = atoi(rstr);
  if(tnum < 1 || rnum < 1) usage();
  int bnum = bstr ? atoi(bstr) : -1;
  int rv = proctypical(tnum, rnum, bnum, nc, rratio);
  return rv;
}


/* perform combo command */
static int proccombo(int tnum, int rnum, int bnum, bool rnd){
  iprintf("<Combination Test>\n  tnum=%d  rnum=%d  bnum=%d  rnd=%d\n\n", tnum, rnum, bnum, rnd);
  bool err = false;
  double stime = tctime();
  TCMDB *mdb = (bnum > 0) ? tcmdbnew2(bnum) : tcmdbnew();
  TARGCOMBO targs[tnum];
  pthread_t threads[tnum];
  if(tnum == 1){
    targs[0].mdb = mdb;
    targs[0].rnum = rnum;
    targs[0].rnd = rnd;
    targs[0].id = 0;
    if(threadwrite(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].mdb = mdb;
      targs[i].rnum = rnum;
      targs[i].rnd = rnd;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadwrite, targs + i) != 0){
        eprint("pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint("pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  if(tnum == 1){
    targs[0].mdb = mdb;
    targs[0].rnum = rnum;
    targs[0].rnd = rnd;
    targs[0].id = 0;
    if(threadread(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].mdb = mdb;
      targs[i].rnum = rnum;
      targs[i].rnd = rnd;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadread, targs + i) != 0){
        eprint("pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint("pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  if(tnum == 1){
    targs[0].mdb = mdb;
    targs[0].rnum = rnum;
    targs[0].rnd = rnd;
    targs[0].id = 0;
    if(threadremove(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].mdb = mdb;
      targs[i].rnum = rnum;
      targs[i].rnd = rnd;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadremove, targs + i) != 0){
        eprint("pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint("pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  iprintf("record number: %llu\n", (unsigned long long)tcmdbrnum(mdb));
  tcmdbdel(mdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* perform typical command */
static int proctypical(int tnum, int rnum, int bnum, bool nc, int rratio){
  iprintf("<Typical Access Test>\n  tnum=%d  rnum=%d  bnum=%d  nc=%d  rratio=%d\n\n",
          tnum, rnum, bnum, nc, rratio);
  bool err = false;
  double stime = tctime();
  TCMDB *mdb = (bnum > 0) ? tcmdbnew2(bnum) : tcmdbnew();
  TARGTYPICAL targs[tnum];
  pthread_t threads[tnum];
  if(tnum == 1){
    targs[0].mdb = mdb;
    targs[0].rnum = rnum;
    targs[0].nc = nc;
    targs[0].rratio = rratio;
    targs[0].id = 0;
    if(threadtypical(targs) != NULL) err = true;
  } else {
    for(int i = 0; i < tnum; i++){
      targs[i].mdb = mdb;
      targs[i].rnum = rnum;
      targs[i].nc = nc;
      targs[i].rratio= rratio;
      targs[i].id = i;
      if(pthread_create(threads + i, NULL, threadtypical, targs + i) != 0){
        eprint("pthread_create");
        targs[i].id = -1;
        err = true;
      }
    }
    for(int i = 0; i < tnum; i++){
      if(targs[i].id == -1) continue;
      void *rv;
      if(pthread_join(threads[i], &rv) != 0){
        eprint("pthread_join");
        err = true;
      } else if(rv){
        err = true;
      }
    }
  }
  iprintf("record number: %llu\n", (unsigned long long)tcmdbrnum(mdb));
  tcmdbdel(mdb);
  iprintf("time: %.3f\n", tctime() - stime);
  iprintf("%s\n\n", err ? "error" : "ok");
  return err ? 1 : 0;
}


/* thread the write function */
static void *threadwrite(void *targ){
  TCMDB *mdb = ((TARGCOMBO *)targ)->mdb;
  int rnum = ((TARGCOMBO *)targ)->rnum;
  bool rnd = ((TARGCOMBO *)targ)->rnd;
  int id = ((TARGCOMBO *)targ)->id;
  double stime = tctime();
  if(id == 0) iprintf("writing:\n");
  int base = id * rnum;
  for(int i = 1; i <= rnum; i++){
    char buf[RECBUFSIZ];
    int len = sprintf(buf, "%08d", base + (rnd ? myrand(i) : i));
    tcmdbput(mdb, buf, len, buf, len);
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  if(id == 0) iprintf("time: %.3f\n", tctime() - stime);
  return NULL;
}


/* thread the read function */
static void *threadread(void *targ){
  TCMDB *mdb = ((TARGCOMBO *)targ)->mdb;
  int rnum = ((TARGCOMBO *)targ)->rnum;
  bool rnd = ((TARGCOMBO *)targ)->rnd;
  int id = ((TARGCOMBO *)targ)->id;
  double stime = tctime();
  if(id == 0) iprintf("reading:\n");
  int base = id * rnum;
  for(int i = 1; i <= rnum; i++){
    char kbuf[RECBUFSIZ];
    int ksiz = sprintf(kbuf, "%08d", base + (rnd ? myrand(i) : i));
    int vsiz;
    char *vbuf = tcmdbget(mdb, kbuf, ksiz, &vsiz);
    if(vbuf) free(vbuf);
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  if(id == 0) iprintf("time: %.3f\n", tctime() - stime);
  return NULL;
}


/* thread the remove function */
static void *threadremove(void *targ){
  TCMDB *mdb = ((TARGCOMBO *)targ)->mdb;
  int rnum = ((TARGCOMBO *)targ)->rnum;
  bool rnd = ((TARGCOMBO *)targ)->rnd;
  int id = ((TARGCOMBO *)targ)->id;
  double stime = tctime();
  if(id == 0) iprintf("removing:\n");
  int base = id * rnum;
  for(int i = 1; i <= rnum; i++){
    char buf[RECBUFSIZ];
    int len = sprintf(buf, "%08d", base + (rnd ? myrand(i) : i));
    tcmdbout(mdb, buf, len);
    if(id == 0 && rnum > 250 && i % (rnum / 250) == 0){
      putchar('.');
      fflush(stdout);
      if(i == rnum || i % (rnum / 10) == 0) iprintf(" (%08d)\n", i);
    }
  }
  if(id == 0) iprintf("time: %.3f\n", tctime() - stime);
  return NULL;
}


/* thread the typical function */
static void *threadtypical(void *targ){
  TCMDB *mdb = ((TARGTYPICAL *)targ)->mdb;
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
      tcmdbput(mdb, buf, len, buf, len);
      if(map) tcmapput(map, buf, len, buf, len);
    } else if(rnd < 15){
      tcmdbputkeep(mdb, buf, len, buf, len);
      if(map) tcmapputkeep(map, buf, len, buf, len);
    } else if(rnd < 20){
      tcmdbputcat(mdb, buf, len, buf, len);
      if(map) tcmapputcat(map, buf, len, buf, len);
    } else if(rnd < 30){
      tcmdbout(mdb, buf, len);
      if(map) tcmapout(map, buf, len);
    } else if(rnd < 31){
      if(myrand(10) == 0) tcmdbiterinit(mdb);
      for(int j = 0; !err && j < 10; j++){
        int ksiz;
        char *kbuf = tcmdbiternext(mdb, &ksiz);
        if(kbuf) free(kbuf);
      }
    } else {
      int vsiz;
      char *vbuf = tcmdbget(mdb, buf, len, &vsiz);
      if(vbuf){
        if(map){
          int msiz;
          const char *mbuf = tcmapget(map, buf, len, &msiz);
          if(msiz != vsiz || memcmp(mbuf, vbuf, vsiz)){
            eprint("(validation)");
            err = true;
          }
        }
        free(vbuf);
      } else {
        if(map && tcmapget(map, buf, len, &vsiz)){
          eprint("(validation)");
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
      char *vbuf = tcmdbget(mdb, kbuf, ksiz, &vsiz);
      if(vbuf){
        int msiz;
        const char *mbuf = tcmapget(map, kbuf, ksiz, &msiz);
        if(!mbuf || msiz != vsiz || memcmp(mbuf, vbuf, vsiz)){
          eprint("(validation)");
          err = true;
        }
        free(vbuf);
      } else {
        eprint("(validation)");
        err = true;
      }
    }
    tcmapdel(map);
  }
  return err ? "error" : NULL;
}



// END OF FILE
