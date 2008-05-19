/*************************************************************************************************
 * The command line utility of the abstract database API
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
#include <tcadb.h>
#include "myconf.h"


/* global variables */
const char *g_progname;                  // program name


/* function prototypes */
int main(int argc, char **argv);
static void usage(void);
static void printerr(TCADB *adb);
static int printdata(const char *ptr, int size, bool px);
static char *hextoobj(const char *str, int *sp);
static int runcreate(int argc, char **argv);
static int runinform(int argc, char **argv);
static int runput(int argc, char **argv);
static int runout(int argc, char **argv);
static int runget(int argc, char **argv);
static int runlist(int argc, char **argv);
static int runversion(int argc, char **argv);
static int proccreate(const char *name);
static int procinform(const char *name);
static int procput(const char *name, const char *kbuf, int ksiz, const char *vbuf, int vsiz,
                   int dmode);
static int procout(const char *name, const char *kbuf, int ksiz);
static int procget(const char *name, const char *kbuf, int ksiz, bool px, bool pz);
static int proclist(const char *name, int max, bool pv, bool px, const char *fmstr);
static int procversion(void);


/* main routine */
int main(int argc, char **argv){
  g_progname = argv[0];
  if(argc < 2) usage();
  int rv = 0;
  if(!strcmp(argv[1], "create")){
    rv = runcreate(argc, argv);
  } else if(!strcmp(argv[1], "inform")){
    rv = runinform(argc, argv);
  } else if(!strcmp(argv[1], "put")){
    rv = runput(argc, argv);
  } else if(!strcmp(argv[1], "out")){
    rv = runout(argc, argv);
  } else if(!strcmp(argv[1], "get")){
    rv = runget(argc, argv);
  } else if(!strcmp(argv[1], "list")){
    rv = runlist(argc, argv);
  } else if(!strcmp(argv[1], "version") || !strcmp(argv[1], "--version")){
    rv = runversion(argc, argv);
  } else {
    usage();
  }
  return rv;
}


/* print the usage and exit */
static void usage(void){
  fprintf(stderr, "%s: the command line utility of the abstract database API\n", g_progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s create name\n", g_progname);
  fprintf(stderr, "  %s inform name\n", g_progname);
  fprintf(stderr, "  %s put [-sx] [-dk|-dc] name key value\n", g_progname);
  fprintf(stderr, "  %s out [-sx] name key\n", g_progname);
  fprintf(stderr, "  %s get [-sx] [-px] [-pz] name key\n", g_progname);
  fprintf(stderr, "  %s list [-m num] [-pv] [-px] [-fm str] name\n", g_progname);
  fprintf(stderr, "  %s version\n", g_progname);
  fprintf(stderr, "\n");
  exit(1);
}


/* print error information */
static void printerr(TCADB *adb){
  fprintf(stderr, "%s: error\n", g_progname);
}


/* print record data */
static int printdata(const char *ptr, int size, bool px){
  int len = 0;
  while(size-- > 0){
    if(px){
      if(len > 0) putchar(' ');
      len += printf("%02X", *(unsigned char *)ptr);
    } else {
      putchar(*ptr);
      len++;
    }
    ptr++;
  }
  return len;
}


/* create a binary object from a hexadecimal string */
static char *hextoobj(const char *str, int *sp){
  int len = strlen(str);
  char *buf = tcmalloc(len + 1);
  int j = 0;
  for(int i = 0; i < len; i += 2){
    while(strchr(" \n\r\t\f\v", str[i])){
      i++;
    }
    char mbuf[3];
    if((mbuf[0] = str[i]) == '\0') break;
    if((mbuf[1] = str[i+1]) == '\0') break;
    mbuf[2] = '\0';
    buf[j++] = (char)strtol(mbuf, NULL, 16);
  }
  buf[j] = '\0';
  *sp = j;
  return buf;
}


/* parse arguments of create command */
static int runcreate(int argc, char **argv){
  char *name = NULL;
  for(int i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      usage();
    } else if(!name){
      name = argv[i];
    } else {
      usage();
    }
  }
  if(!name) usage();
  int rv = proccreate(name);
  return rv;
}


/* parse arguments of inform command */
static int runinform(int argc, char **argv){
  char *name = NULL;
  for(int i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      usage();
    } else if(!name){
      name = argv[i];
    } else {
      usage();
    }
  }
  if(!name) usage();
  name = tcsprintf("%s#mode=r", name);
  int rv = procinform(name);
  free(name);
  return rv;
}


/* parse arguments of put command */
static int runput(int argc, char **argv){
  char *name = NULL;
  char *key = NULL;
  char *value = NULL;
  int dmode = 0;
  bool sx = false;
  for(int i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-dk")){
        dmode = -1;
      } else if(!strcmp(argv[i], "-dc")){
        dmode = 1;
      } else if(!strcmp(argv[i], "-sx")){
        sx = true;
      } else {
        usage();
      }
    } else if(!name){
      name = argv[i];
    } else if(!key){
      key = argv[i];
    } else if(!value){
      value = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !key || !value) usage();
  int ksiz, vsiz;
  char *kbuf, *vbuf;
  if(sx){
    kbuf = hextoobj(key, &ksiz);
    vbuf = hextoobj(value, &vsiz);
  } else {
    ksiz = strlen(key);
    kbuf = tcmemdup(key, ksiz);
    vsiz = strlen(value);
    vbuf = tcmemdup(value, vsiz);
  }
  int rv = procput(name, kbuf, ksiz, vbuf, vsiz, dmode);
  free(vbuf);
  free(kbuf);
  return rv;
}


/* parse arguments of out command */
static int runout(int argc, char **argv){
  char *name = NULL;
  char *key = NULL;
  bool sx = false;
  for(int i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-sx")){
        sx = true;
      } else {
        usage();
      }
    } else if(!name){
      name = argv[i];
    } else if(!key){
      key = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !key) usage();
  int ksiz;
  char *kbuf;
  if(sx){
    kbuf = hextoobj(key, &ksiz);
  } else {
    ksiz = strlen(key);
    kbuf = tcmemdup(key, ksiz);
  }
  int rv = procout(name, kbuf, ksiz);
  free(kbuf);
  return rv;
}


/* parse arguments of get command */
static int runget(int argc, char **argv){
  char *name = NULL;
  char *key = NULL;
  bool sx = false;
  bool px = false;
  bool pz = false;
  for(int i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-sx")){
        sx = true;
      } else if(!strcmp(argv[i], "-px")){
        px = true;
      } else if(!strcmp(argv[i], "-pz")){
        pz = true;
      } else {
        usage();
      }
    } else if(!name){
      name = argv[i];
    } else if(!key){
      key = argv[i];
    } else {
      usage();
    }
  }
  if(!name || !key) usage();
  int ksiz;
  char *kbuf;
  if(sx){
    kbuf = hextoobj(key, &ksiz);
  } else {
    ksiz = strlen(key);
    kbuf = tcmemdup(key, ksiz);
  }
  name = tcsprintf("%s#mode=r", name);
  int rv = procget(name, kbuf, ksiz, px, pz);
  free(name);
  free(kbuf);
  return rv;
}


/* parse arguments of list command */
static int runlist(int argc, char **argv){
  char *name = NULL;
  int max = -1;
  bool pv = false;
  bool px = false;
  char *fmstr = NULL;
  for(int i = 2; i < argc; i++){
    if(!name && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-m")){
        if(++i >= argc) usage();
        max = atoi(argv[i]);
      } else if(!strcmp(argv[i], "-pv")){
        pv = true;
      } else if(!strcmp(argv[i], "-px")){
        px = true;
      } else if(!strcmp(argv[i], "-fm")){
        if(++i >= argc) usage();
        fmstr = argv[i];
      } else {
        usage();
      }
    } else if(!name){
      name = argv[i];
    } else {
      usage();
    }
  }
  if(!name) usage();
  name = tcsprintf("%s#mode=r", name);
  int rv = proclist(name, max, pv, px, fmstr);
  free(name);
  return rv;
}


/* parse arguments of version command */
static int runversion(int argc, char **argv){
  int rv = procversion();
  return rv;
}


/* perform create command */
static int proccreate(const char *name){
  TCADB *adb = tcadbnew();
  if(!tcadbopen(adb, name)){
    printerr(adb);
    tcadbdel(adb);
    return 1;
  }
  bool err = false;
  if(!tcadbclose(adb)){
    printerr(adb);
    err = true;
  }
  tcadbdel(adb);
  return err ? 1 : 0;
}


/* perform inform command */
static int procinform(const char *name){
  TCADB *adb = tcadbnew();
  if(!tcadbopen(adb, name)){
    printerr(adb);
    tcadbdel(adb);
    return 1;
  }
  bool err = false;
  printf("record number: %llu\n", (unsigned long long)tcadbrnum(adb));
  printf("size: %llu\n", (unsigned long long)tcadbsize(adb));
  if(!tcadbclose(adb)){
    printerr(adb);
    err = true;
  }
  tcadbdel(adb);
  return err ? 1 : 0;
}


/* perform put command */
static int procput(const char *name, const char *kbuf, int ksiz, const char *vbuf, int vsiz,
                   int dmode){
  TCADB *adb = tcadbnew();
  if(!tcadbopen(adb, name)){
    printerr(adb);
    tcadbdel(adb);
    return 1;
  }
  bool err = false;
  switch(dmode){
  case -1:
    if(!tcadbputkeep(adb, kbuf, ksiz, vbuf, vsiz)){
      printerr(adb);
      err = true;
    }
    break;
  case 1:
    if(!tcadbputcat(adb, kbuf, ksiz, vbuf, vsiz)){
      printerr(adb);
      err = true;
    }
    break;
  default:
    if(!tcadbput(adb, kbuf, ksiz, vbuf, vsiz)){
      printerr(adb);
      err = true;
    }
    break;
  }
  if(!tcadbclose(adb)){
    if(!err) printerr(adb);
    err = true;
  }
  tcadbdel(adb);
  return err ? 1 : 0;
}


/* perform out command */
static int procout(const char *name, const char *kbuf, int ksiz){
  TCADB *adb = tcadbnew();
  if(!tcadbopen(adb, name)){
    printerr(adb);
    tcadbdel(adb);
    return 1;
  }
  bool err = false;
  if(!tcadbout(adb, kbuf, ksiz)){
    printerr(adb);
    err = true;
  }
  if(!tcadbclose(adb)){
    if(!err) printerr(adb);
    err = true;
  }
  tcadbdel(adb);
  return err ? 1 : 0;
}


/* perform get command */
static int procget(const char *name, const char *kbuf, int ksiz, bool px, bool pz){
  TCADB *adb = tcadbnew();
  if(!tcadbopen(adb, name)){
    printerr(adb);
    tcadbdel(adb);
    return 1;
  }
  bool err = false;
  int vsiz;
  char *vbuf = tcadbget(adb, kbuf, ksiz, &vsiz);
  if(vbuf){
    printdata(vbuf, vsiz, px);
    if(!pz) putchar('\n');
    free(vbuf);
  } else {
    printerr(adb);
    err = true;
  }
  if(!tcadbclose(adb)){
    if(!err) printerr(adb);
    err = true;
  }
  tcadbdel(adb);
  return err ? 1 : 0;
}


/* perform list command */
static int proclist(const char *name, int max, bool pv, bool px, const char *fmstr){
  TCADB *adb = tcadbnew();
  if(!tcadbopen(adb, name)){
    printerr(adb);
    tcadbdel(adb);
    return 1;
  }
  bool err = false;
  if(fmstr){
    TCLIST *keys = tcadbfwmkeys2(adb, fmstr, max);
    for(int i = 0; i < tclistnum(keys); i++){
      int ksiz;
      const char *kbuf = tclistval(keys, i, &ksiz);
      printdata(kbuf, ksiz, px);
      if(pv){
        int vsiz;
        char *vbuf = tcadbget(adb, kbuf, ksiz, &vsiz);
        if(vbuf){
          putchar('\t');
          printdata(vbuf, vsiz, px);
          free(vbuf);
        }
      }
      putchar('\n');
    }
    tclistdel(keys);
  } else {
    if(!tcadbiterinit(adb)){
      printerr(adb);
      err = true;
    }
    int ksiz;
    char *kbuf;
    int cnt = 0;
    while((kbuf = tcadbiternext(adb, &ksiz)) != NULL){
      printdata(kbuf, ksiz, px);
      if(pv){
        int vsiz;
        char *vbuf = tcadbget(adb, kbuf, ksiz, &vsiz);
        if(vbuf){
          putchar('\t');
          printdata(vbuf, vsiz, px);
          free(vbuf);
        }
      }
      putchar('\n');
      free(kbuf);
      if(max >= 0 && ++cnt >= max) break;
    }
  }
  if(!tcadbclose(adb)){
    if(!err) printerr(adb);
    err = true;
  }
  tcadbdel(adb);
  return err ? 1 : 0;
}


/* perform version command */
static int procversion(void){
  printf("Tokyo Cabinet version %s (%d:%s)\n", tcversion, _TC_LIBVER, _TC_FORMATVER);
  printf("Copyright (C) 2006-2008 Mikio Hirabayashi\n");
  return 0;
}



// END OF FILE
