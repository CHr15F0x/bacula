/*************************************************************************************************
 * Popular encoders and decoders of the utility API
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


/* global variables */
const char *g_progname;                  // program name


/* function prototypes */
int main(int argc, char **argv);
static void usage(void);
static void eprintf(const char *format, ...);
static int runurl(int argc, char **argv);
static int runbase(int argc, char **argv);
static int runquote(int argc, char **argv);
static int runmime(int argc, char **argv);
static int runpack(int argc, char **argv);
static int runtcbs(int argc, char **argv);
static int runzlib(int argc, char **argv);
static int runxml(int argc, char **argv);
static int runucs(int argc, char **argv);
static int rundate(int argc, char **argv);
static int runconf(int argc, char **argv);
static int procurl(const char *ibuf, int isiz, bool dec, bool br, const char *base);
static int procbase(const char *ibuf, int isiz, bool dec);
static int procquote(const char *ibuf, int isiz, bool dec);
static int procmime(const char *ibuf, int isiz, bool dec, const char *ename, bool qb, bool on);
static int procpack(const char *ibuf, int isiz, bool dec, bool bwt);
static int proctcbs(const char *ibuf, int isiz, bool dec);
static int proczlib(const char *ibuf, int isiz, bool dec, bool gz);
static int procxml(const char *ibuf, int isiz, bool dec, bool br);
static int procucs(const char *ibuf, int isiz, bool dec);
static int procdate(const char *str, int jl, bool wf, bool rf);
static int procconf(int mode);


/* main routine */
int main(int argc, char **argv){
  g_progname = argv[0];
  if(argc < 2) usage();
  int rv = 0;
  if(!strcmp(argv[1], "url")){
    rv = runurl(argc, argv);
  } else if(!strcmp(argv[1], "base")){
    rv = runbase(argc, argv);
  } else if(!strcmp(argv[1], "quote")){
    rv = runquote(argc, argv);
  } else if(!strcmp(argv[1], "mime")){
    rv = runmime(argc, argv);
  } else if(!strcmp(argv[1], "pack")){
    rv = runpack(argc, argv);
  } else if(!strcmp(argv[1], "tcbs")){
    rv = runtcbs(argc, argv);
  } else if(!strcmp(argv[1], "zlib")){
    rv = runzlib(argc, argv);
  } else if(!strcmp(argv[1], "xml")){
    rv = runxml(argc, argv);
  } else if(!strcmp(argv[1], "ucs")){
    rv = runucs(argc, argv);
  } else if(!strcmp(argv[1], "date")){
    rv = rundate(argc, argv);
  } else if(!strcmp(argv[1], "conf")){
    rv = runconf(argc, argv);
  } else {
    usage();
  }
  return rv;
}


/* print the usage and exit */
static void usage(void){
  fprintf(stderr, "%s: popular encoders and decoders of Tokyo Cabinet\n", g_progname);
  fprintf(stderr, "\n");
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s url [-d] [-br] [-rs base] [file]\n", g_progname);
  fprintf(stderr, "  %s base [-d] [file]\n", g_progname);
  fprintf(stderr, "  %s quote [-d] [file]\n", g_progname);
  fprintf(stderr, "  %s mime [-d] [-en name] [-q] [file]\n", g_progname);
  fprintf(stderr, "  %s pack [-d] [-bwt] [file]\n", g_progname);
  fprintf(stderr, "  %s tcbs [-d] [file]\n", g_progname);
  fprintf(stderr, "  %s zlib [-d] [-gz] [file]\n", g_progname);
  fprintf(stderr, "  %s xml [-d] [-br] [file]\n", g_progname);
  fprintf(stderr, "  %s ucs [-d] [file]\n", g_progname);
  fprintf(stderr, "  %s date [-ds str] [-jl num] [-wf] [-rf]\n", g_progname);
  fprintf(stderr, "  %s conf [-v|-i|-l|-p]\n", g_progname);
  fprintf(stderr, "\n");
  exit(1);
}


/* print formatted error string */
static void eprintf(const char *format, ...){
  va_list ap;
  va_start(ap, format);
  fprintf(stderr, "%s: ", g_progname);
  vfprintf(stderr, format, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}


/* parse arguments of url command */
static int runurl(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  bool br = false;
  char *base = NULL;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else if(!strcmp(argv[i], "-br")){
        br = true;
      } else if(!strcmp(argv[i], "-rs")){
        if(++i >= argc) usage();
        base = argv[i];
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = procurl(ibuf, isiz, dec, br, base);
  if(path && path[0] == '@' && !br) printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of base command */
static int runbase(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = procbase(ibuf, isiz, dec);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of quote command */
static int runquote(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = procquote(ibuf, isiz, dec);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of mime command */
static int runmime(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  char *ename = NULL;
  bool qb = false;
  bool on = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else if(!strcmp(argv[i], "-en")){
        if(++i >= argc) usage();
        ename = argv[i];
      } else if(!strcmp(argv[i], "-q")){
        qb = true;
      } else if(!strcmp(argv[i], "-on")){
        on = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  if(!ename) ename = "UTF-8";
  int rv = procmime(ibuf, isiz, dec, ename, qb, on);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of pack command */
static int runpack(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  bool bwt = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else if(!strcmp(argv[i], "-bwt")){
        bwt = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = procpack(ibuf, isiz, dec, bwt);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of tcbs command */
static int runtcbs(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = proctcbs(ibuf, isiz, dec);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of zlib command */
static int runzlib(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  bool gz = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else if(!strcmp(argv[i], "-gz")){
        gz = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = proczlib(ibuf, isiz, dec, gz);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of xml command */
static int runxml(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  bool br = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else if(!strcmp(argv[i], "-br")){
        br = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = procxml(ibuf, isiz, dec, br);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of ucs command */
static int runucs(int argc, char **argv){
  char *path = NULL;
  bool dec = false;
  for(int i = 2; i < argc; i++){
    if(!path && argv[i][0] == '-'){
      if(!strcmp(argv[i], "-d")){
        dec = true;
      } else {
        usage();
      }
    } else if(!path){
      path = argv[i];
    } else {
      usage();
    }
  }
  char *ibuf;
  int isiz;
  if(path && path[0] == '@'){
    isiz = strlen(path) - 1;
    ibuf = tcmemdup(path + 1, isiz);
  } else {
    ibuf = tcreadfile(path, -1, &isiz);
  }
  if(!ibuf){
    eprintf("%s: cannot open", path ? path : "(stdin)");
    return 1;
  }
  int rv = procucs(ibuf, isiz, dec);
  if(path && path[0] == '@') printf("\n");
  free(ibuf);
  return rv;
}


/* parse arguments of date command */
static int rundate(int argc, char **argv){
  char *str = NULL;
  int jl = INT_MAX;
  bool wf = false;
  bool rf = false;
  for(int i = 2; i < argc; i++){
    if(argv[i][0] == '-'){
      if(!strcmp(argv[i], "-ds")){
        if(++i >= argc) usage();
        str = argv[i];
      } else if(!strcmp(argv[i], "-jl")){
        if(++i >= argc) usage();
        jl = atoi(argv[i]);
      } else if(!strcmp(argv[i], "-wf")){
        wf = true;
      } else if(!strcmp(argv[i], "-rf")){
        rf = true;
      } else {
        usage();
      }
    } else {
      usage();
    }
  }
  int rv = procdate(str, jl, wf, rf);
  return rv;
}


/* parse arguments of conf command */
static int runconf(int argc, char **argv){
  int mode = 0;
  for(int i = 2; i < argc; i++){
    if(argv[i][0] == '-'){
      if(!strcmp(argv[i], "-v")){
        mode = 'v';
      } else if(!strcmp(argv[i], "-i")){
        mode = 'i';
      } else if(!strcmp(argv[i], "-l")){
        mode = 'l';
      } else if(!strcmp(argv[i], "-p")){
        mode = 'p';
      } else {
        usage();
      }
    } else {
      usage();
    }
  }
  int rv = procconf(mode);
  return rv;
}


/* perform url command */
static int procurl(const char *ibuf, int isiz, bool dec, bool br, const char *base){
  if(base){
    char *obuf = tcurlresolve(base, ibuf);
    printf("%s", obuf);
    free(obuf);
  } else if(br){
    TCMAP *elems = tcurlbreak(ibuf);
    const char *elem;
    if((elem = tcmapget2(elems, "self")) != NULL) printf("self: %s\n", elem);
    if((elem = tcmapget2(elems, "scheme")) != NULL) printf("scheme: %s\n", elem);
    if((elem = tcmapget2(elems, "host")) != NULL) printf("host: %s\n", elem);
    if((elem = tcmapget2(elems, "port")) != NULL) printf("port: %s\n", elem);
    if((elem = tcmapget2(elems, "authority")) != NULL) printf("authority: %s\n", elem);
    if((elem = tcmapget2(elems, "path")) != NULL) printf("path: %s\n", elem);
    if((elem = tcmapget2(elems, "file")) != NULL) printf("file: %s\n", elem);
    if((elem = tcmapget2(elems, "query")) != NULL) printf("query: %s\n", elem);
    if((elem = tcmapget2(elems, "fragment")) != NULL) printf("fragment: %s\n", elem);
    tcmapdel(elems);
  } else if(dec){
    int osiz;
    char *obuf = tcurldecode(ibuf, &osiz);
    fwrite(obuf, osiz, 1, stdout);
    free(obuf);
  } else {
    char *obuf = tcurlencode(ibuf, isiz);
    fwrite(obuf, strlen(obuf), 1, stdout);
    free(obuf);
  }
  return 0;
}


/* perform base command */
static int procbase(const char *ibuf, int isiz, bool dec){
  if(dec){
    int osiz;
    char *obuf = tcbasedecode(ibuf, &osiz);
    fwrite(obuf, osiz, 1, stdout);
    free(obuf);
  } else {
    char *obuf = tcbaseencode(ibuf, isiz);
    fwrite(obuf, strlen(obuf), 1, stdout);
    free(obuf);
  }
  return 0;
}


/* perform quote command */
static int procquote(const char *ibuf, int isiz, bool dec){
  if(dec){
    int osiz;
    char *obuf = tcquotedecode(ibuf, &osiz);
    fwrite(obuf, osiz, 1, stdout);
    free(obuf);
  } else {
    char *obuf = tcquoteencode(ibuf, isiz);
    fwrite(obuf, strlen(obuf), 1, stdout);
    free(obuf);
  }
  return 0;
}


/* perform mime command */
static int procmime(const char *ibuf, int isiz, bool dec, const char *ename, bool qb, bool on){
  if(dec){
    char ebuf[32];
    char *obuf = tcmimedecode(ibuf, ebuf);
    if(on){
      fwrite(ebuf, strlen(ebuf), 1, stdout);
    } else {
      fwrite(obuf, strlen(obuf), 1, stdout);
    }
    free(obuf);
  } else {
    char *obuf = tcmimeencode(ibuf, ename, !qb);
    fwrite(obuf, strlen(obuf), 1, stdout);
    free(obuf);
  }
  return 0;
}


/* perform pack command */
static int procpack(const char *ibuf, int isiz, bool dec, bool bwt){
  if(dec){
    int osiz;
    char *obuf = tcpackdecode(ibuf, isiz, &osiz);
    if(bwt && osiz > 0){
      int idx, step;
      TCREADVNUMBUF(obuf, idx, step);
      char *tbuf = tcbwtdecode(obuf + step, osiz - step, idx);
      fwrite(tbuf, osiz - step, 1, stdout);
      free(tbuf);
    } else {
      fwrite(obuf, osiz, 1, stdout);
    }
    free(obuf);
  } else {
    char *tbuf = NULL;
    if(bwt){
      int idx;
      tbuf = tcbwtencode(ibuf, isiz, &idx);
      char vnumbuf[sizeof(int)+1];
      int step;
      TCSETVNUMBUF(step, vnumbuf, idx);
      tbuf = tcrealloc(tbuf, isiz + step + 1);
      memmove(tbuf + step, tbuf, isiz);
      memcpy(tbuf, vnumbuf, step);
      isiz += step;
      ibuf = tbuf;
    }
    int osiz;
    char *obuf = tcpackencode(ibuf, isiz, &osiz);
    fwrite(obuf, osiz, 1, stdout);
    free(obuf);
    free(tbuf);
  }
  return 0;
}


/* perform tcbs command */
static int proctcbs(const char *ibuf, int isiz, bool dec){
  if(dec){
    int osiz;
    char *obuf = tcbsdecode(ibuf, isiz, &osiz);
    fwrite(obuf, osiz, 1, stdout);
    free(obuf);
  } else {
    int osiz;
    char *obuf = tcbsencode(ibuf, isiz, &osiz);
    fwrite(obuf, osiz, 1, stdout);
    free(obuf);
  }
  return 0;
}


/* perform zlib command */
static int proczlib(const char *ibuf, int isiz, bool dec, bool gz){
  if(dec){
    int osiz;
    char *obuf = gz ? tcgzipdecode(ibuf, isiz, &osiz) : tcinflate(ibuf, isiz, &osiz);
    if(obuf){
      fwrite(obuf, osiz, 1, stdout);
      free(obuf);
    } else {
      eprintf("inflate failure");
    }
  } else {
    int osiz;
    char *obuf = gz ? tcgzipencode(ibuf, isiz, &osiz) : tcdeflate(ibuf, isiz, &osiz);
    if(obuf){
      fwrite(obuf, osiz, 1, stdout);
      free(obuf);
    } else {
      eprintf("deflate failure");
    }
  }
  return 0;
}


/* perform xml command */
static int procxml(const char *ibuf, int isiz, bool dec, bool br){
  if(br){
    TCLIST *elems = tcxmlbreak(ibuf);
    for(int i = 0; i < tclistnum(elems); i++){
      int esiz;
      const char *elem = tclistval(elems, i, &esiz);
      char *estr = tcmemdup(elem, esiz);
      tcstrsubchr(estr, "\t\n\r", "  ");
      tcstrtrim(estr);
      if(*estr != '\0'){
        if(*elem == '<'){
          if(tcstrfwm(estr, "<!--")){
            printf("COMMENT\t%s\n", estr);
          } else if(tcstrfwm(estr, "<![CDATA[")){
            printf("CDATA\t%s\n", estr);
          } else if(tcstrfwm(estr, "<!")){
            printf("DECL\t%s\n", estr);
          } else if(tcstrfwm(estr, "<?")){
            printf("XMLDECL\t%s\n", estr);
          } else {
            TCMAP *attrs = tcxmlattrs(estr);
            tcmapiterinit(attrs);
            const char *name;
            if((name = tcmapget2(attrs, "")) != NULL){
              if(tcstrfwm(estr, "</")){
                printf("END");
              } else if(tcstrbwm(estr, "/>")){
                printf("EMPTY");
              } else {
                printf("BEGIN");
              }
              printf("\t%s", name);
              while((name = tcmapiternext2(attrs)) != NULL){
                if(*name == '\0') continue;
                printf("\t%s\t%s", name, tcmapiterval2(name));
              }
              printf("\n");
            }
            tcmapdel(attrs);
          }
        } else {
          char *dstr = tcxmlunescape(estr);
          printf("TEXT\t%s\n", dstr);
          free(dstr);
        }
      }
      free(estr);
    }
    tclistdel(elems);
  } else if(dec){
    char *obuf = tcxmlunescape(ibuf);
    fwrite(obuf, strlen(obuf), 1, stdout);
    free(obuf);
  } else {
    char *obuf = tcxmlescape(ibuf);
    fwrite(obuf, strlen(obuf), 1, stdout);
    free(obuf);
  }
  return 0;
}


/* perform ucs command */
static int procucs(const char *ibuf, int isiz, bool dec){
  if(dec){
    uint16_t *ary = tcmalloc(isiz + 1);
    int anum = 0;
    for(int i = 0; i < isiz; i += 2){
      ary[anum++] = (((unsigned char *)ibuf)[i] << 8) + ((unsigned char *)ibuf)[i+1];
    }
    char *str = tcmalloc(isiz * 3 + 1);
    tcstrucstoutf(ary, anum, str);
    printf("%s", str);
    free(str);
    free(ary);
  } else {
    uint16_t *ary = tcmalloc(isiz * sizeof(uint16_t) + 1);
    int anum;
    tcstrutftoucs(ibuf, ary, &anum);
    for(int i = 0; i < anum; i++){
      int c = ary[i];
      putchar(c >> 8);
      putchar(c & 0xff);
    }
    free(ary);
  }
  return 0;
}


/* perform date command */
static int procdate(const char *str, int jl, bool wf, bool rf){
  int64_t t = str ? tcstrmktime(str) : time(NULL);
  if(wf){
    char buf[48];
    tcdatestrwww(t, jl, buf);
    printf("%s\n", buf);
  } else if(rf){
    char buf[48];
    tcdatestrhttp(t, jl, buf);
    printf("%s\n", buf);
  } else {
    printf("%lld\n", (long long int)t);
  }
  return 0;
}


/* perform conf command */
static int procconf(int mode){
  switch(mode){
  case 'v':
    printf("%s\n", tcversion);
    break;
  case 'i':
    printf("%s\n", _TC_APPINC);
    break;
  case 'l':
    printf("%s\n", _TC_APPLIBS);
    break;
  case 'p':
    printf("%s\n", _TC_BINDIR);
    break;
  default:
    printf("myconf(version): %s\n", tcversion);
    printf("myconf(libver): %d\n", _TC_LIBVER);
    printf("myconf(formatver): %s\n", _TC_FORMATVER);
    printf("myconf(prefix): %s\n", _TC_PREFIX);
    printf("myconf(includedir): %s\n", _TC_INCLUDEDIR);
    printf("myconf(libdir): %s\n", _TC_LIBDIR);
    printf("myconf(bindir): %s\n", _TC_BINDIR);
    printf("myconf(libexecdir): %s\n", _TC_LIBEXECDIR);
    printf("myconf(appinc): %s\n", _TC_APPINC);
    printf("myconf(applibs): %s\n", _TC_APPLIBS);
    printf("myconf(bigend): %d\n", TCBIGEND);
    printf("myconf(usezlib): %d\n", TCUSEZLIB);
    printf("sizeof(bool): %d\n", sizeof(bool));
    printf("sizeof(char): %d\n", sizeof(char));
    printf("sizeof(short): %d\n", sizeof(short));
    printf("sizeof(int): %d\n", sizeof(int));
    printf("sizeof(long): %d\n", sizeof(long));
    printf("sizeof(long long): %d\n", sizeof(long long));
    printf("sizeof(float): %d\n", sizeof(float));
    printf("sizeof(double): %d\n", sizeof(double));
    printf("sizeof(long double): %d\n", sizeof(long double));
    printf("sizeof(size_t): %d\n", sizeof(size_t));
    printf("sizeof(time_t): %d\n", sizeof(time_t));
    printf("sizeof(off_t): %d\n", sizeof(off_t));
    printf("sizeof(ino_t): %d\n", sizeof(ino_t));
    printf("sizeof(intptr_t): %d\n", sizeof(intptr_t));
    printf("sizeof(void *): %d\n", sizeof(void *));
    printf("macro(CHAR_MAX): %llu\n", (unsigned long long)CHAR_MAX);
    printf("macro(SHRT_MAX): %llu\n", (unsigned long long)SHRT_MAX);
    printf("macro(INT_MAX): %llu\n", (unsigned long long)INT_MAX);
    printf("macro(LONG_MAX): %llu\n", (unsigned long long)LONG_MAX);
    printf("macro(LLONG_MAX): %llu\n", (unsigned long long)LLONG_MAX);
    printf("macro(PATH_MAX): %llu\n", (unsigned long long)PATH_MAX);
    printf("macro(RAND_MAX): %llu\n", (unsigned long long)RAND_MAX);
    printf("sysconf(_SC_CLK_TCK): %ld\n", sysconf(_SC_CLK_TCK));
    printf("sysconf(_SC_OPEN_MAX): %ld\n", sysconf(_SC_OPEN_MAX));
    printf("sysconf(_SC_PAGESIZE): %ld\n", sysconf(_SC_PAGESIZE));
    struct stat sbuf;
    if(stat(MYCDIRSTR, &sbuf) == 0){
      printf("stat(st_uid): %d\n", (int)sbuf.st_uid);
      printf("stat(st_gid): %d\n", (int)sbuf.st_gid);
      printf("stat(st_blksize): %d\n", (int)sbuf.st_blksize);
    }
  }
  return 0;
}



// END OF FILE
