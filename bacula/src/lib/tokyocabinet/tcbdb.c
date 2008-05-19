/*************************************************************************************************
 * The B+ tree database API of Tokyo Cabinet
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


#include "tcutil.h"
#include "tchdb.h"
#include "tcbdb.h"
#include "myconf.h"

#define BDBFILEMODE    00644             // permission of created files
#define BDBOPAQUESIZ   64                // size of using opaque field
#define BDBPAGEBUFSIZ  32768             // size of a buffer to read each page
#define BDBNODEIDBASE   ((1LL<<48)+1)    // base number of node ID
#define BDBLEVELMAX    64                // max level of B+ tree
#define BDBCACHEOUT    8                 // number of pages in a process of cacheout

#define BDBDEFLMEMB    128               // default number of members in each leaf
#define BDBMINLMEMB    4                 // minimum number of members in each leaf
#define BDBDEFNMEMB    256               // default number of members in each node
#define BDBMINNMEMB    4                 // minimum number of members in each node
#define BDBDEFBNUM     16381             // default bucket number
#define BDBDEFAPOW     8                 // default alignment power
#define BDBDEFFPOW     10                // default free block pool power
#define BDBDEFLCNUM    1024              // default number of leaf cache
#define BDBDEFNCNUM    512               // default number of node cache

typedef struct {                         // type of structure for a record
  char *kbuf;                            // pointer to the key region
  int ksiz;                              // size of the key region
  char *vbuf;                            // pointer to the value region
  int vsiz;                              // size of the value region
  TCLIST *rest;                          // list of value objects
} BDBREC;

typedef struct {                         // type of structure for a leaf page
  uint64_t id;                           // ID number of the leaf
  TCLIST *recs;                          // list of records
  uint64_t prev;                         // ID number of the previous leaf
  uint64_t next;                         // ID number of the next leaf
  bool dirty;                            // whether to be written back
  bool dead;                             // whether to be removed
} BDBLEAF;

typedef struct {                         // type of structure for a page index
  uint64_t pid;                          // ID number of the referring page
  char *kbuf;                            // pointer to the key region
  int ksiz;                              // size of the key region
} BDBIDX;

typedef struct {                         // type of structure for a node page
  uint64_t id;                           // ID number of the node
  uint64_t heir;                         // ID of the child before the first index
  TCLIST *idxs;                          // list of indexes
  bool dirty;                            // whether to be written back
} BDBNODE;

enum {                                   // enumeration for duplication behavior
  BDBPDOVER,                             // overwrite an existing value
  BDBPDKEEP,                             // keep the existing value
  BDBPDCAT,                              // concatenate values
  BDBPDDUP,                              // allow duplication of keys
  BDBPDDUPB,                             // allow backward duplication
};


/* private macros */
#define BDBLOCKMETHOD(TC_bdb, TC_wr) \
  ((TC_bdb)->mmtx ? tcbdblockmethod((TC_bdb), (TC_wr)) : true)
#define BDBUNLOCKMETHOD(TC_bdb) \
  ((TC_bdb)->mmtx ? tcbdbunlockmethod(TC_bdb) : true)
#define BDBLOCKCACHE(TC_bdb) \
  ((TC_bdb)->mmtx ? tcbdblockcache(TC_bdb) : true)
#define BDBUNLOCKCACHE(TC_bdb) \
  ((TC_bdb)->mmtx ? tcbdbunlockcache(TC_bdb) : true)
#define BDBLOCKTRAN(TC_bdb) \
  ((TC_bdb)->mmtx ? tcbdblocktran(TC_bdb) : true)
#define BDBUNLOCKTRAN(TC_bdb) \
  ((TC_bdb)->mmtx ? tcbdbunlocktran(TC_bdb) : true)


/* private function prototypes */
static void tcbdbclear(TCBDB *bdb);
static void tcdumpmeta(TCBDB *bdb);
static void tcloadmeta(TCBDB *bdb);
static BDBLEAF *tcbdbleafnew(TCBDB *bdb, uint64_t prev, uint64_t next);
static bool tcbdbleafcacheout(TCBDB *bdb, BDBLEAF *leaf);
static bool tcbdbleafsave(TCBDB *bdb, BDBLEAF *leaf);
static BDBLEAF *tcbdbleafload(TCBDB *bdb, uint64_t id);
static BDBLEAF *tcbdbgethistleaf(TCBDB *bdb, const char *kbuf, int ksiz);
static bool tcbdbleafaddrec(TCBDB *bdb, BDBLEAF *leaf, int dmode,
                            const char *kbuf, int ksiz, const char *vbuf, int vsiz);
static int tcbdbleafdatasize(BDBLEAF *leaf);
static BDBLEAF *tcbdbleafdivide(TCBDB *bdb, BDBLEAF *leaf);
static bool tcbdbleafkill(TCBDB *bdb, BDBLEAF *leaf);
static BDBNODE *tcbdbnodenew(TCBDB *bdb, uint64_t heir);
static bool tcbdbnodecacheout(TCBDB *bdb, BDBNODE *node);
static bool tcbdbnodesave(TCBDB *bdb, BDBNODE *node);
static BDBNODE *tcbdbnodeload(TCBDB *bdb, uint64_t id);
static void tcbdbnodeaddidx(TCBDB *bdb, BDBNODE *node, bool order, uint64_t pid,
                            const char *kbuf, int ksiz);
static bool tcbdbnodesubidx(TCBDB *bdb, BDBNODE *node, uint64_t pid);
static uint64_t tcbdbsearchleaf(TCBDB *bdb, const char *kbuf, int ksiz);
static BDBREC *tcbdbsearchrec(TCBDB *bdb, BDBLEAF *leaf, const char *kbuf, int ksiz, int *ip);
static bool tcbdbcacheadjust(TCBDB *bdb);
static void tcbdbcachepurge(TCBDB *bdb);
static bool tcbdbopenimpl(TCBDB *bdb, const char *path, int omode);
static bool tcbdbcloseimpl(TCBDB *bdb);
static bool tcbdbputimpl(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz,
                         int dmode);
static bool tcbdboutimpl(TCBDB *bdb, const char *kbuf, int ksiz);
static bool tcbdboutlist(TCBDB *bdb, const char *kbuf, int ksiz);
static const char *tcbdbgetimpl(TCBDB *bdb, const char *kbuf, int ksiz, int *sp);
static int tcbdbgetnum(TCBDB *bdb, const char *kbuf, int ksiz);
static TCLIST *tcbdbgetlist(TCBDB *bdb, const char *kbuf, int ksiz);
static bool tcbdbrangeimpl(TCBDB *bdb, const char *bkbuf, int bksiz, bool binc,
                           const char *ekbuf, int eksiz, bool einc, int max, TCLIST *keys);
static bool tcbdbrangefwm(TCBDB *bdb, const char *pbuf, int psiz, int max, TCLIST *keys);
static bool tcbdboptimizeimpl(TCBDB *bdb, int32_t lmemb, int32_t nmemb,
                              int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts);
static bool tcbdbvanishimpl(TCBDB *bdb);
static bool tcbdblockmethod(TCBDB *bdb, bool wr);
static bool tcbdbunlockmethod(TCBDB *bdb);
static bool tcbdblockcache(TCBDB *bdb);
static bool tcbdbunlockcache(TCBDB *bdb);
static bool tcbdblocktran(TCBDB *bdb);
static bool tcbdbunlocktran(TCBDB *bdb);
static bool tcbdbcurfirstimpl(BDBCUR *cur);
static bool tcbdbcurlastimpl(BDBCUR *cur);
static bool tcbdbcurjumpimpl(BDBCUR *cur, const char *kbuf, int ksiz, bool forward);
static bool tcbdbcuradjust(BDBCUR *cur, bool forward);
static bool tcbdbcurprevimpl(BDBCUR *cur);
static bool tcbdbcurnextimpl(BDBCUR *cur);
static bool tcbdbcurputimpl(BDBCUR *cur, const char *vbuf, int vsiz, int mode);
static bool tcbdbcuroutimpl(BDBCUR *cur);
static bool tcbdbcurrecimpl(BDBCUR *cur, const char **kbp, int *ksp, const char **vbp, int *vsp);


/* debugging function prototypes */
void tcbdbprintmeta(TCBDB *bdb);
void tcbdbprintleaf(TCBDB *bdb, BDBLEAF *leaf);
void tcbdbprintnode(TCBDB *bdb, BDBNODE *node);



/*************************************************************************************************
 * API
 *************************************************************************************************/


/* Get the message string corresponding to an error code. */
const char *tcbdberrmsg(int ecode){
  return tchdberrmsg(ecode);
}


/* Create a B+ tree database object. */
TCBDB *tcbdbnew(void){
  TCBDB *bdb;
  TCMALLOC(bdb, sizeof(*bdb));
  tcbdbclear(bdb);
  bdb->hdb = tchdbnew();
  TCMALLOC(bdb->hist, sizeof(*bdb->hist) * BDBLEVELMAX);
  return bdb;
}


/* Delete a B+ tree database object. */
void tcbdbdel(TCBDB *bdb){
  assert(bdb);
  if(bdb->open) tcbdbclose(bdb);
  free(bdb->hist);
  tchdbdel(bdb->hdb);
  if(bdb->mmtx){
    pthread_mutex_destroy(bdb->tmtx);
    pthread_mutex_destroy(bdb->cmtx);
    pthread_rwlock_destroy(bdb->mmtx);
    free(bdb->tmtx);
    free(bdb->cmtx);
    free(bdb->mmtx);
  }
  free(bdb);
}


/* Get the last happened error code of a B+ tree database object. */
int tcbdbecode(TCBDB *bdb){
  assert(bdb);
  return tchdbecode(bdb->hdb);
}


/* Set mutual exclusion control of a B+ tree database object for threading. */
bool tcbdbsetmutex(TCBDB *bdb){
  assert(bdb);
  if(!TCUSEPTHREAD) return true;
  if(!tcglobalmutexlock()) return false;
  if(bdb->mmtx || bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    tcglobalmutexunlock();
    return false;
  }
  TCMALLOC(bdb->mmtx, sizeof(pthread_rwlock_t));
  TCMALLOC(bdb->cmtx, sizeof(pthread_mutex_t));
  TCMALLOC(bdb->tmtx, sizeof(pthread_mutex_t));
  if(pthread_rwlock_init(bdb->mmtx, NULL) != 0 || pthread_mutex_init(bdb->cmtx, NULL) != 0 ||
     pthread_mutex_init(bdb->tmtx, NULL) != 0){
    free(bdb->tmtx);
    free(bdb->cmtx);
    free(bdb->mmtx);
    bdb->tmtx = NULL;
    bdb->cmtx = NULL;
    bdb->mmtx = NULL;
    tcglobalmutexunlock();
    return false;
  }
  tcglobalmutexunlock();
  return tchdbsetmutex(bdb->hdb);
}


/* Set the custom comparison function of a B+ tree database object. */
bool tcbdbsetcmpfunc(TCBDB *bdb, BDBCMP cmp, void *cmpop){
  assert(bdb && cmp);
  if(bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  bdb->cmp = cmp;
  bdb->cmpop = cmpop;
  return true;
}


/* Set the tuning parameters of a B+ tree database object. */
bool tcbdbtune(TCBDB *bdb, int32_t lmemb, int32_t nmemb,
               int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts){
  assert(bdb);
  if(bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  bdb->lmemb = (lmemb > 0) ? tclmax(lmemb, BDBMINLMEMB) : BDBDEFLMEMB;
  bdb->nmemb = (nmemb > 0) ? tclmax(nmemb, BDBMINNMEMB) : BDBDEFNMEMB;
  bdb->opts = opts;
  uint8_t hopts = 0;
  if(opts & BDBTLARGE) hopts |= HDBTLARGE;
  if(opts & BDBTDEFLATE) hopts |= HDBTDEFLATE;
  if(opts & BDBTTCBS) hopts |= HDBTTCBS;
  bnum = (bnum > 0) ? bnum : BDBDEFBNUM;
  apow = (apow >= 0) ? apow : BDBDEFAPOW;
  fpow = (fpow >= 0) ? fpow : BDBDEFFPOW;
  return tchdbtune(bdb->hdb, bnum, apow, fpow, hopts);
}


/* Set the caching parameters of a B+ tree database object. */
bool tcbdbsetcache(TCBDB *bdb, int32_t lcnum, int32_t ncnum){
  assert(bdb);
  if(bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  if(lcnum > 0) bdb->lcnum = tclmax(lcnum, BDBLEVELMAX);
  if(ncnum > 0) bdb->ncnum = tclmax(ncnum, BDBLEVELMAX);
  return true;
}


/* Open a database file and connect a B+ tree database object. */
bool tcbdbopen(TCBDB *bdb, const char *path, int omode){
  assert(bdb && path);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbopenimpl(bdb, path, omode);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Close a B+ tree database object. */
bool tcbdbclose(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcloseimpl(bdb);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Store a record into a B+ tree database object. */
bool tcbdbput(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(bdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbputimpl(bdb, kbuf, ksiz, vbuf, vsiz, BDBPDOVER);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Store a string record into a B+ tree database object. */
bool tcbdbput2(TCBDB *bdb, const char *kstr, const char *vstr){
  assert(bdb && kstr && vstr);
  return tcbdbput(bdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Store a new record into a B+ tree database object. */
bool tcbdbputkeep(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(bdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbputimpl(bdb, kbuf, ksiz, vbuf, vsiz, BDBPDKEEP);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Store a new string record into a B+ tree database object. */
bool tcbdbputkeep2(TCBDB *bdb, const char *kstr, const char *vstr){
  assert(bdb && kstr && vstr);
  return tcbdbputkeep(bdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Concatenate a value at the end of the existing record in a B+ tree database object. */
bool tcbdbputcat(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(bdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbputimpl(bdb, kbuf, ksiz, vbuf, vsiz, BDBPDCAT);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Concatenate a string value at the end of the existing record in a B+ tree database object. */
bool tcbdbputcat2(TCBDB *bdb, const char *kstr, const char *vstr){
  assert(bdb && kstr && vstr);
  return tcbdbputcat(bdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Store a record into a B+ tree database object with allowing duplication of keys. */
bool tcbdbputdup(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(bdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbputimpl(bdb, kbuf, ksiz, vbuf, vsiz, BDBPDDUP);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Store a string record into a B+ tree database object with allowing duplication of keys. */
bool tcbdbputdup2(TCBDB *bdb, const char *kstr, const char *vstr){
  assert(bdb && kstr && vstr);
  return tcbdbputdup(bdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Store records into a B+ tree database object with allowing duplication of keys. */
bool tcbdbputdup3(TCBDB *bdb, const void *kbuf, int ksiz, const TCLIST *vals){
  assert(bdb && kbuf && ksiz >= 0 && vals);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool err = false;
  int ln = TCLISTNUM(vals);
  for(int i = 0; i < ln; i++){
    int vsiz;
    const char *vbuf = tclistval(vals, i, &vsiz);
    if(!tcbdbputimpl(bdb, kbuf, ksiz, vbuf, vsiz, BDBPDDUP)) err = true;
  }
  BDBUNLOCKMETHOD(bdb);
  return !err;
}


/* Remove a record of a B+ tree database object. */
bool tcbdbout(TCBDB *bdb, const void *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdboutimpl(bdb, kbuf, ksiz);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Remove a string record of a B+ tree database object. */
bool tcbdbout2(TCBDB *bdb, const char *kstr){
  assert(bdb && kstr);
  return tcbdbout(bdb, kstr, strlen(kstr));
}


/* Remove records of a B+ tree database object. */
bool tcbdbout3(TCBDB *bdb, const void *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdboutlist(bdb, kbuf, ksiz);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Retrieve a record in a B+ tree database object. */
void *tcbdbget(TCBDB *bdb, const void *kbuf, int ksiz, int *sp){
  assert(bdb && kbuf && ksiz >= 0 && sp);
  if(!BDBLOCKMETHOD(bdb, false)) return NULL;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return NULL;
  }
  const char *vbuf = tcbdbgetimpl(bdb, kbuf, ksiz, sp);
  char *rv;
  if(vbuf){
    TCMEMDUP(rv, vbuf, *sp);
  } else {
    rv = NULL;
  }
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)){
      free(rv);
      rv = NULL;
    }
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Retrieve a string record in a B+ tree database object. */
char *tcbdbget2(TCBDB *bdb, const char *kstr){
  assert(bdb && kstr);
  int vsiz;
  return tcbdbget(bdb, kstr, strlen(kstr), &vsiz);
}


/* Retrieve a record in a B+ tree database object and write the value into a buffer. */
const void *tcbdbget3(TCBDB *bdb, const void *kbuf, int ksiz, int *sp){
  assert(bdb && kbuf && ksiz >= 0 && sp);
  if(!BDBLOCKMETHOD(bdb, false)) return NULL;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return NULL;
  }
  const char *rv = tcbdbgetimpl(bdb, kbuf, ksiz, sp);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = NULL;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Retrieve records in a B+ tree database object. */
TCLIST *tcbdbget4(TCBDB *bdb, const void *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  if(!BDBLOCKMETHOD(bdb, false)) return NULL;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return NULL;
  }
  TCLIST *rv = tcbdbgetlist(bdb, kbuf, ksiz);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)){
      if(rv) tclistdel(rv);
      rv = NULL;
    }
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Get the number of records corresponding a key in a B+ tree database object. */
int tcbdbvnum(TCBDB *bdb, const void *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  if(!BDBLOCKMETHOD(bdb, false)) return 0;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return 0;
  }
  int rv = tcbdbgetnum(bdb, kbuf, ksiz);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = 0;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Get the number of records corresponding a string key in a B+ tree database object. */
int tcbdbvnum2(TCBDB *bdb, const char *kstr){
  assert(bdb && kstr);
  return tcbdbvnum(bdb, kstr, strlen(kstr));
}


/* Get the size of the value of a record in a B+ tree database object. */
int tcbdbvsiz(TCBDB *bdb, const void *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  int vsiz;
  if(!tcbdbget3(bdb, kbuf, ksiz, &vsiz)) return -1;
  return vsiz;
}


/* Get the size of the value of a string record in a B+ tree database object. */
int tcbdbvsiz2(TCBDB *bdb, const char *kstr){
  assert(bdb && kstr);
  return tcbdbvsiz(bdb, kstr, strlen(kstr));
}


/* Get keys of ranged records in a B+ tree database object. */
TCLIST *tcbdbrange(TCBDB *bdb, const void *bkbuf, int bksiz, bool binc,
                   const void *ekbuf, int eksiz, bool einc, int max){
  assert(bdb);
  TCLIST *keys = tclistnew();
  if(!BDBLOCKMETHOD(bdb, false)) return keys;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return keys;
  }
  tcbdbrangeimpl(bdb, bkbuf, bksiz, binc, ekbuf, eksiz, einc, max, keys);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    tcbdbcacheadjust(bdb);
    BDBUNLOCKMETHOD(bdb);
  }
  return keys;
}


/* Get string keys of ranged records in a B+ tree database object. */
TCLIST *tcbdbrange2(TCBDB *bdb, const char *bkstr, bool binc,
                    const char *ekstr, bool einc, int max){
  return tcbdbrange(bdb, bkstr, bkstr ? strlen(bkstr) : 0, binc,
                    ekstr, ekstr ? strlen(ekstr) : 0, einc, max);
}


/* Get forward matching keys in a B+ tree database object. */
TCLIST *tcbdbfwmkeys(TCBDB *bdb, const void *pbuf, int psiz, int max){
  assert(bdb && pbuf && psiz >= 0);
  TCLIST *keys = tclistnew();
  if(!BDBLOCKMETHOD(bdb, false)) return keys;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return keys;
  }
  tcbdbrangefwm(bdb, pbuf, psiz, max, keys);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    tcbdbcacheadjust(bdb);
    BDBUNLOCKMETHOD(bdb);
  }
  return keys;
}


/* Get forward matching string keys in a B+ tree database object. */
TCLIST *tcbdbfwmkeys2(TCBDB *bdb, const char *pstr, int max){
  assert(bdb && pstr);
  return tcbdbfwmkeys(bdb, pstr, strlen(pstr), max);
}



/* Synchronize updated contents of a B+ tree database object with the file and the device. */
bool tcbdbsync(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode || bdb->tran){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbmemsync(bdb, true);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Optimize the file of a B+ tree database object. */
bool tcbdboptimize(TCBDB *bdb, int32_t lmemb, int32_t nmemb,
                   int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode || bdb->tran){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdboptimizeimpl(bdb, lmemb, nmemb, bnum, apow, fpow, opts);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Remove all records of a B+ tree database object. */
bool tcbdbvanish(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode || bdb->tran){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbvanishimpl(bdb);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Copy the database file of a B+ tree database object. */
bool tcbdbcopy(TCBDB *bdb, const char *path){
  assert(bdb && path);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  TCLIST *lids = tclistnew();
  TCLIST *nids = tclistnew();
  const char *vbuf;
  int vsiz;
  TCMAP *leafc = bdb->leafc;
  tcmapiterinit(leafc);
  while((vbuf = tcmapiternext(leafc, &vsiz)) != NULL){
    tclistpush(lids, vbuf, vsiz);
  }
  TCMAP *nodec = bdb->nodec;
  tcmapiterinit(nodec);
  while((vbuf = tcmapiternext(nodec, &vsiz)) != NULL){
    tclistpush(nids, vbuf, vsiz);
  }
  BDBUNLOCKMETHOD(bdb);
  bool err = false;
  int ln = TCLISTNUM(lids);
  for(int i = 0; i < ln; i++){
    vbuf = TCLISTVALPTR(lids, i);
    if(BDBLOCKMETHOD(bdb, true)){
      if(bdb->open){
        int rsiz;
        BDBLEAF *leaf = (BDBLEAF *)tcmapget(bdb->leafc, vbuf, sizeof(leaf->id), &rsiz);
        if(leaf && leaf->dirty && !tcbdbleafsave(bdb, leaf)) err = true;
      } else {
        err = true;
      }
      BDBUNLOCKMETHOD(bdb);
    } else {
      err = true;
    }
  }
  ln = TCLISTNUM(nids);
  for(int i = 0; i < ln; i++){
    vbuf = TCLISTVALPTR(nids, i);
    if(BDBLOCKMETHOD(bdb, true)){
      if(bdb->open){
        int rsiz;
        BDBNODE *node = (BDBNODE *)tcmapget(bdb->nodec, vbuf, sizeof(node->id), &rsiz);
        if(node && node->dirty && !tcbdbnodesave(bdb, node)) err = true;
      } else {
        err = true;
      }
      BDBUNLOCKMETHOD(bdb);
    } else {
      err = true;
    }
  }
  tclistdel(nids);
  tclistdel(lids);
  if(!tcbdbtranbegin(bdb)) err = true;
  if(BDBLOCKMETHOD(bdb, false)){
    if(!tchdbcopy(bdb->hdb, path)) err = true;
    BDBUNLOCKMETHOD(bdb);
  } else {
    err = true;
  }
  if(!tcbdbtrancommit(bdb)) err = true;
  return !err;
}


/* Begin the transaction of a B+ tree database object. */
bool tcbdbtranbegin(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode || bdb->tran){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(!tcbdbmemsync(bdb, false)){
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(!BDBLOCKTRAN(bdb)){
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bdb->tran = true;
  TCMEMDUP(bdb->rbopaque, bdb->opaque, BDBOPAQUESIZ);
  BDBUNLOCKMETHOD(bdb);
  return true;
}


/* Commit the transaction of a B+ tree database object. */
bool tcbdbtrancommit(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode || !bdb->tran){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  free(bdb->rbopaque);
  bdb->tran = false;
  bdb->rbopaque = NULL;
  bool err = false;
  if(!tcbdbmemsync(bdb, false)) err = true;
  BDBUNLOCKTRAN(bdb);
  BDBUNLOCKMETHOD(bdb);
  return !err;
}


/* Abort the transaction of a B+ tree database object. */
bool tcbdbtranabort(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode || !bdb->tran){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  tcbdbcachepurge(bdb);
  memcpy(bdb->opaque, bdb->rbopaque, BDBOPAQUESIZ);
  tcloadmeta(bdb);
  free(bdb->rbopaque);
  bdb->tran = false;
  bdb->rbopaque = NULL;
  BDBUNLOCKTRAN(bdb);
  BDBUNLOCKMETHOD(bdb);
  return true;
}


/* Get the file path of a B+ tree database object. */
const char *tcbdbpath(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, false)) return NULL;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return NULL;
  }
  const char *rv = tchdbpath(bdb->hdb);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the number of records of a B+ tree database object. */
uint64_t tcbdbrnum(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, false)) return 0;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return 0;
  }
  uint64_t rv = bdb->rnum;
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the size of the database file of a B+ tree database object. */
uint64_t tcbdbfsiz(TCBDB *bdb){
  assert(bdb);
  if(!BDBLOCKMETHOD(bdb, false)) return 0;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return 0;
  }
  uint64_t rv = tchdbfsiz(bdb->hdb);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Create a cursor object. */
BDBCUR *tcbdbcurnew(TCBDB *bdb){
  assert(bdb);
  BDBCUR *cur;
  TCMALLOC(cur, sizeof(*cur));
  cur->bdb = bdb;
  cur->id = 0;
  cur->kidx = 0;
  cur->vidx = 0;
  return cur;
}


/* Delete a cursor object. */
void tcbdbcurdel(BDBCUR *cur){
  assert(cur);
  free(cur);
}


/* Move a cursor object to the first record. */
bool tcbdbcurfirst(BDBCUR *cur){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurfirstimpl(cur);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = false;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Move a cursor object to the last record. */
bool tcbdbcurlast(BDBCUR *cur){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurlastimpl(cur);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = false;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Move a cursor object to the front of records corresponding a key. */
bool tcbdbcurjump(BDBCUR *cur, const void *kbuf, int ksiz){
  assert(cur && kbuf && ksiz >= 0);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurjumpimpl(cur, kbuf, ksiz, true);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = false;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Move a cursor object to the front of records corresponding a key string. */
bool tcbdbcurjump2(BDBCUR *cur, const char *kstr){
  assert(cur && kstr);
  return tcbdbcurjump(cur, kstr, strlen(kstr));
}


/* Move a cursor object to the previous record. */
bool tcbdbcurprev(BDBCUR *cur){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurprevimpl(cur);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = false;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Move a cursor object to the next record. */
bool tcbdbcurnext(BDBCUR *cur){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurnextimpl(cur);
  bool adj = TCMAPRNUM(bdb->leafc) > bdb->lcnum || TCMAPRNUM(bdb->nodec) > bdb->ncnum;
  BDBUNLOCKMETHOD(bdb);
  if(adj && BDBLOCKMETHOD(bdb, true)){
    if(!bdb->tran && !tcbdbcacheadjust(bdb)) rv = false;
    BDBUNLOCKMETHOD(bdb);
  }
  return rv;
}


/* Insert a record around a cursor object. */
bool tcbdbcurput(BDBCUR *cur, const void *vbuf, int vsiz, int cpmode){
  assert(cur && vbuf && vsiz >= 0);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurputimpl(cur, vbuf, vsiz, cpmode);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Insert a string record around a cursor object. */
bool tcbdbcurput2(BDBCUR *cur, const char *vstr, int cpmode){
  assert(cur && vstr);
  return tcbdbcurput(cur, vstr, strlen(vstr), cpmode);
}


/* Delete the record where a cursor object is. */
bool tcbdbcurout(BDBCUR *cur){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcuroutimpl(cur);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the key of the record where the cursor object is. */
char *tcbdbcurkey(BDBCUR *cur, int *sp){
  assert(cur && sp);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  const char *kbuf, *vbuf;
  int ksiz, vsiz;
  char *rv;
  if(tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
    TCMEMDUP(rv, kbuf, ksiz);
    *sp = ksiz;
  } else {
    rv = NULL;
  }
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the key string of the record where the cursor object is. */
char *tcbdbcurkey2(BDBCUR *cur){
  assert(cur);
  int ksiz;
  return tcbdbcurkey(cur, &ksiz);
}


/* Get the key of the record where the cursor object is, as a volatile buffer. */
const char *tcbdbcurkey3(BDBCUR *cur, int *sp){
  assert(cur && sp);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  const char *kbuf, *vbuf;
  int ksiz, vsiz;
  const char *rv;
  if(tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
    rv = kbuf;
    *sp = ksiz;
  } else {
    rv = NULL;
  }
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the value of the record where the cursor object is. */
char *tcbdbcurval(BDBCUR *cur, int *sp){
  assert(cur && sp);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  const char *kbuf, *vbuf;
  int ksiz, vsiz;
  char *rv;
  if(tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
    TCMEMDUP(rv, vbuf, vsiz);
    *sp = vsiz;
  } else {
    rv = NULL;
  }
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the value string of the record where the cursor object is. */
char *tcbdbcurval2(BDBCUR *cur){
  assert(cur);
  int vsiz;
  return tcbdbcurval(cur, &vsiz);
}


/* Get the value of the record where the cursor object is, as a volatile buffer. */
const char *tcbdbcurval3(BDBCUR *cur, int *sp){
  assert(cur && sp);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  const char *kbuf, *vbuf;
  int ksiz, vsiz;
  const char *rv;
  if(tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
    rv = vbuf;
    *sp = vsiz;
  } else {
    rv = NULL;
  }
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Get the key and the value of the record where the cursor object is. */
bool tcbdbcurrec(BDBCUR *cur, TCXSTR *kxstr, TCXSTR *vxstr){
  assert(cur && kxstr && vxstr);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  if(cur->id < 1){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  const char *kbuf, *vbuf;
  int ksiz, vsiz;
  bool rv;
  if(tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
    tcxstrclear(kxstr);
    TCXSTRCAT(kxstr, kbuf, ksiz);
    tcxstrclear(vxstr);
    TCXSTRCAT(vxstr, vbuf, vsiz);
    rv = true;
  } else {
    rv = false;
  }
  BDBUNLOCKMETHOD(bdb);
  return rv;
}



/*************************************************************************************************
 * features for experts
 *************************************************************************************************/


/* Set the error code of a B+ tree database object. */
void tcbdbsetecode(TCBDB *bdb, int ecode, const char *filename, int line, const char *func){
  assert(bdb && filename && line >= 1 && func);
  tchdbsetecode(bdb->hdb, ecode, filename, line, func);
}


/* Set the file descriptor for debugging output. */
void tcbdbsetdbgfd(TCBDB *bdb, int fd){
  assert(bdb && fd >= 0);
  tchdbsetdbgfd(bdb->hdb, fd);
}


/* Get the file descriptor for debugging output. */
int tcbdbdbgfd(TCBDB *bdb){
  assert(bdb);
  return tchdbdbgfd(bdb->hdb);
}


/* Synchronize updating contents on memory. */
bool tcbdbmemsync(TCBDB *bdb, bool phys){
  assert(bdb);
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  bool err = false;
  bool clk = BDBLOCKCACHE(bdb);
  const char *vbuf;
  int vsiz;
  TCMAP *leafc = bdb->leafc;
  tcmapiterinit(leafc);
  while((vbuf = tcmapiternext(leafc, &vsiz)) != NULL){
    int rsiz;
    BDBLEAF *leaf = (BDBLEAF *)tcmapiterval(vbuf, &rsiz);
    if(leaf->dirty && !tcbdbleafsave(bdb, leaf)) err = true;
  }
  TCMAP *nodec = bdb->nodec;
  tcmapiterinit(nodec);
  while((vbuf = tcmapiternext(nodec, &vsiz)) != NULL){
    int rsiz;
    BDBNODE *node = (BDBNODE *)tcmapiterval(vbuf, &rsiz);
    if(node->dirty && !tcbdbnodesave(bdb, node)) err = true;
  }
  if(clk) BDBUNLOCKCACHE(bdb);
  tcdumpmeta(bdb);
  if(!tchdbmemsync(bdb->hdb, phys)) err = true;
  return !err;
}


/* Get the comparison function of a B+ tree database object. */
BDBCMP tcbdbcmpfunc(TCBDB *bdb){
  assert(bdb);
  return bdb->cmp;
}


/* Get the opaque object for the comparison function of a B+ tree database object. */
void *tcbdbcmpop(TCBDB *bdb){
  assert(bdb);
  return bdb->cmpop;
}


/* Get the maximum number of cached leaf nodes of a B+ tree database object. */
uint32_t tcbdblmemb(TCBDB *bdb){
  assert(bdb);
  return bdb->lmemb;
}


/* Get the maximum number of cached non-leaf nodes of a B+ tree database object. */
uint32_t tcbdbnmemb(TCBDB *bdb){
  assert(bdb);
  return bdb->nmemb;
}


/* Get the number of the leaf nodes of B+ tree database object. */
uint64_t tcbdblnum(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return bdb->lnum;
}


/* Get the number of the non-leaf nodes of B+ tree database object. */
uint64_t tcbdbnnum(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return bdb->nnum;
}


/* Get the number of elements of the bucket array of a B+ tree database object. */
uint64_t tcbdbbnum(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbbnum(bdb->hdb);
}


/* Get the record alignment of a B+ tree database object. */
uint32_t tcbdbalign(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbalign(bdb->hdb);
}


/* Get the maximum number of the free block pool of a B+ tree database object. */
uint32_t tcbdbfbpmax(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbfbpmax(bdb->hdb);
}


/* Get the inode number of the database file of a B+ tree database object. */
uint64_t tcbdbinode(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbinode(bdb->hdb);
}


/* Get the modification time of the database file of a B+ tree database object. */
time_t tcbdbmtime(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbmtime(bdb->hdb);
}


/* Get the additional flags of a B+ tree database object. */
uint8_t tcbdbflags(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbflags(bdb->hdb);
}


/* Get the options of a B+ tree database object. */
uint8_t tcbdbopts(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return bdb->opts;
}


/* Get the pointer to the opaque field of a B+ tree database object. */
char *tcbdbopaque(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbopaque(bdb->hdb) + BDBOPAQUESIZ;
}


/* Get the number of used elements of the bucket array of a B+ tree database object. */
uint64_t tcbdbbnumused(TCBDB *bdb){
  assert(bdb);
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return tchdbbnumused(bdb->hdb);
}


/* Set the maximum size of each leaf node. */
bool tcbdbsetlsmax(TCBDB *bdb, uint32_t lsmax){
  assert(bdb);
  if(bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  bdb->lsmax = lsmax;
  return true;
}


/* Set the capacity number of records. */
bool tcbdbsetcapnum(TCBDB *bdb, uint64_t capnum){
  assert(bdb);
  if(bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  bdb->capnum = capnum;
  return true;
}


/* Store a new record into a B+ tree database object with backward duplication. */
bool tcbdbputdupback(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(bdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!BDBLOCKMETHOD(bdb, true)) return false;
  if(!bdb->open || !bdb->wmode){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbputimpl(bdb, kbuf, ksiz, vbuf, vsiz, BDBPDDUPB);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Store a new string record into a B+ tree database object with backward duplication. */
bool tcbdbputdupback2(TCBDB *bdb, const char *kstr, const char *vstr){
  assert(bdb && kstr && vstr);
  return tcbdbputdupback(bdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Move a cursor object to the rear of records corresponding a key. */
bool tcbdbcurjumpback(BDBCUR *cur, const void *kbuf, int ksiz){
  assert(cur && kbuf && ksiz >= 0);
  TCBDB *bdb = cur->bdb;
  if(!BDBLOCKMETHOD(bdb, false)) return false;
  if(!bdb->open){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    BDBUNLOCKMETHOD(bdb);
    return false;
  }
  bool rv = tcbdbcurjumpimpl(cur, kbuf, ksiz, false);
  BDBUNLOCKMETHOD(bdb);
  return rv;
}


/* Move a cursor object to the rear of records corresponding a key string. */
bool tcbdbcurjumpback2(BDBCUR *cur, const char *kstr){
  assert(cur && kstr);
  return tcbdbcurjumpback(cur, kstr, strlen(kstr));
}


/* Compare keys of two records by lexical order. */
int tcbdbcmplexical(const char *aptr, int asiz, const char *bptr, int bsiz, void *op){
  assert(aptr && asiz >= 0 && bptr && bsiz >= 0);
  int rv;
  TCCMPLEXICAL(rv, aptr, asiz, bptr, bsiz);
  return rv;
}


/* Compare two keys as decimal strings of real numbers. */
int tcbdbcmpdecimal(const char *aptr, int asiz, const char *bptr, int bsiz, void *op){
  assert(aptr && asiz >= 0 && bptr && bsiz >= 0);
  int sign;
  int64_t anum = 0;
  sign = 1;
  if(asiz > 0 && *aptr == '-'){
    aptr++;
    asiz--;
    sign = -1;
  }
  for(int i = 0; i < asiz; i++){
    int c = aptr[i];
    if(c < '0' || c > '9') continue;
    anum = anum * 10 + c - '0';
  }
  anum *= sign;
  int64_t bnum = 0;
  sign = 1;
  if(bsiz > 0 && *bptr == '-'){
    bptr++;
    bsiz--;
    sign = -1;
  }
  for(int i = 0; i < bsiz; i++){
    int c = bptr[i];
    if(c < '0' || c > '9') continue;
    bnum = bnum * 10 + c - '0';
  }
  bnum *= sign;
  return (anum < bnum) ? -1 : anum > bnum;
}


/* Compare two keys as 32-bit integers in the native byte order. */
int tcbdbcmpint32(const char *aptr, int asiz, const char *bptr, int bsiz, void *op){
  assert(aptr && bptr);
  int32_t anum, bnum;
  if(asiz == sizeof(int32_t)){
    memcpy(&anum, aptr, sizeof(int32_t));
  } else if(asiz < sizeof(int32_t)){
    memset(&anum, 0, sizeof(int32_t));
    memcpy(&anum, aptr, asiz);
  } else {
    memcpy(&anum, aptr, sizeof(int32_t));
  }
  if(bsiz == sizeof(int32_t)){
    memcpy(&bnum, bptr, sizeof(int32_t));
  } else if(bsiz < sizeof(int32_t)){
    memset(&bnum, 0, sizeof(int32_t));
    memcpy(&bnum, bptr, bsiz);
  } else {
    memcpy(&bnum, bptr, sizeof(int32_t));
  }
  return (anum < bnum) ? -1 : anum > bnum;
}


/* Compare two keys as 64-bit integers in the native byte order. */
int tcbdbcmpint64(const char *aptr, int asiz, const char *bptr, int bsiz, void *op){
  assert(aptr && bptr);
  int64_t anum, bnum;
  if(asiz == sizeof(int64_t)){
    memcpy(&anum, aptr, sizeof(int64_t));
  } else if(asiz < sizeof(int64_t)){
    memset(&anum, 0, sizeof(int64_t));
    memcpy(&anum, aptr, asiz);
  } else {
    memcpy(&anum, aptr, sizeof(int64_t));
  }
  if(bsiz == sizeof(int64_t)){
    memcpy(&bnum, bptr, sizeof(int64_t));
  } else if(bsiz < sizeof(int64_t)){
    memset(&bnum, 0, sizeof(int64_t));
    memcpy(&bnum, bptr, bsiz);
  } else {
    memcpy(&bnum, bptr, sizeof(int64_t));
  }
  return (anum < bnum) ? -1 : anum > bnum;
}



/*************************************************************************************************
 * private features
 *************************************************************************************************/


/* Clear all members.
   `bdb' specifies the B+ tree database object. */
static void tcbdbclear(TCBDB *bdb){
  assert(bdb);
  bdb->mmtx = NULL;
  bdb->cmtx = NULL;
  bdb->tmtx = NULL;
  bdb->hdb = NULL;
  bdb->opaque = NULL;
  bdb->open = false;
  bdb->wmode = false;
  bdb->lmemb = BDBDEFLMEMB;
  bdb->nmemb = BDBDEFNMEMB;
  bdb->opts = 0;
  bdb->root = 0;
  bdb->first = 0;
  bdb->last = 0;
  bdb->lnum = 0;
  bdb->nnum = 0;
  bdb->rnum = 0;
  bdb->leafc = NULL;
  bdb->nodec = NULL;
  bdb->cmp = NULL;
  bdb->cmpop = NULL;
  bdb->lcnum = BDBDEFLCNUM;
  bdb->ncnum = BDBDEFNCNUM;
  bdb->lsmax = 0;
  bdb->lschk = 0;
  bdb->capnum = 0;
  bdb->hist = NULL;
  bdb->hnum = 0;
  bdb->hleaf = 0;
  bdb->lleaf = 0;
  bdb->tran = false;
  bdb->rbopaque = NULL;
  bdb->cnt_saveleaf = -1;
  bdb->cnt_loadleaf = -1;
  bdb->cnt_killleaf = -1;
  bdb->cnt_adjleafc = -1;
  bdb->cnt_savenode = -1;
  bdb->cnt_loadnode = -1;
  bdb->cnt_adjnodec = -1;
  TCDODEBUG(bdb->cnt_saveleaf = 0);
  TCDODEBUG(bdb->cnt_loadleaf = 0);
  TCDODEBUG(bdb->cnt_killleaf = 0);
  TCDODEBUG(bdb->cnt_adjleafc = 0);
  TCDODEBUG(bdb->cnt_savenode = 0);
  TCDODEBUG(bdb->cnt_loadnode = 0);
  TCDODEBUG(bdb->cnt_adjnodec = 0);
}


/* Serialize meta data into the opaque field.
   `bdb' specifies the B+ tree database object. */
static void tcdumpmeta(TCBDB *bdb){
  assert(bdb);
  memset(bdb->opaque, 0, 64);
  char *wp = bdb->opaque;
  if(bdb->cmp == tcbdbcmplexical){
    *(uint8_t *)(wp++) = 0x0;
  } else if(bdb->cmp == tcbdbcmpdecimal){
    *(uint8_t *)(wp++) = 0x1;
  } else if(bdb->cmp == tcbdbcmpint32){
    *(uint8_t *)(wp++) = 0x2;
  } else if(bdb->cmp == tcbdbcmpint64){
    *(uint8_t *)(wp++) = 0x3;
  } else {
    *(uint8_t *)(wp++) = 0xff;
  }
  wp += 7;
  uint32_t lnum;
  lnum = bdb->lmemb;
  lnum = TCHTOIL(lnum);
  memcpy(wp, &lnum, sizeof(lnum));
  wp += sizeof(lnum);
  lnum = bdb->nmemb;
  lnum = TCHTOIL(lnum);
  memcpy(wp, &lnum, sizeof(lnum));
  wp += sizeof(lnum);
  uint64_t llnum;
  llnum = bdb->root;
  llnum = TCHTOILL(llnum);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
  llnum = bdb->first;
  llnum = TCHTOILL(llnum);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
  llnum = bdb->last;
  llnum = TCHTOILL(llnum);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
  llnum = bdb->lnum;
  llnum = TCHTOILL(llnum);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
  llnum = bdb->nnum;
  llnum = TCHTOILL(llnum);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
  llnum = bdb->rnum;
  llnum = TCHTOILL(llnum);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
}


/* Deserialize meta data from the opaque field.
   `bdb' specifies the B+ tree database object. */
static void tcloadmeta(TCBDB *bdb){
  const char *rp = bdb->opaque;
  uint8_t cnum = *(uint8_t *)(rp++);
  if(cnum == 0x0){
    bdb->cmp = tcbdbcmplexical;
  } else if(cnum == 0x1){
    bdb->cmp = tcbdbcmpdecimal;
  } else if(cnum == 0x2){
    bdb->cmp = tcbdbcmpint32;
  } else if(cnum == 0x3){
    bdb->cmp = tcbdbcmpint64;
  }
  rp += 7;
  uint32_t lnum;
  memcpy(&lnum, rp, sizeof(lnum));
  rp += sizeof(lnum);
  bdb->lmemb = TCITOHL(lnum);
  memcpy(&lnum, rp, sizeof(lnum));
  rp += sizeof(lnum);
  bdb->nmemb = TCITOHL(lnum);
  uint64_t llnum;
  memcpy(&llnum, rp, sizeof(llnum));
  bdb->root = TCITOHLL(llnum);
  rp += sizeof(llnum);
  memcpy(&llnum, rp, sizeof(llnum));
  bdb->first = TCITOHLL(llnum);
  rp += sizeof(llnum);
  memcpy(&llnum, rp, sizeof(llnum));
  bdb->last = TCITOHLL(llnum);
  rp += sizeof(llnum);
  memcpy(&llnum, rp, sizeof(llnum));
  bdb->lnum = TCITOHLL(llnum);
  rp += sizeof(llnum);
  memcpy(&llnum, rp, sizeof(llnum));
  bdb->nnum = TCITOHLL(llnum);
  rp += sizeof(llnum);
  memcpy(&llnum, rp, sizeof(llnum));
  bdb->rnum = TCITOHLL(llnum);
  rp += sizeof(llnum);
}


/* Create a new leaf.
   `bdb' specifies the B+ tree database object.
   `prev' specifies the ID number of the previous leaf.
   `next' specifies the ID number of the next leaf.
   The return value is the new leaf object. */
static BDBLEAF *tcbdbleafnew(TCBDB *bdb, uint64_t prev, uint64_t next){
  assert(bdb);
  BDBLEAF lent;
  lent.id = ++bdb->lnum;
  lent.recs = tclistnew2(bdb->lmemb + 1);
  lent.prev = prev;
  lent.next = next;
  lent.dirty = true;
  lent.dead = false;
  tcmapputkeep(bdb->leafc, &(lent.id), sizeof(lent.id), &lent, sizeof(lent));
  int rsiz;
  return (BDBLEAF *)tcmapget(bdb->leafc, &(lent.id), sizeof(lent.id), &rsiz);
}


/* Remove a leaf from the cache.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbleafcacheout(TCBDB *bdb, BDBLEAF *leaf){
  assert(bdb && leaf);
  bool err = false;
  if(leaf->dirty && !tcbdbleafsave(bdb, leaf)) err = true;
  TCLIST *recs = leaf->recs;
  int ln = TCLISTNUM(recs);
  for(int i = 0; i < ln; i++){
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    free(recp->kbuf);
    free(recp->vbuf);
    if(recp->rest) tclistdel(recp->rest);
  }
  tclistdel(recs);
  tcmapout(bdb->leafc, &(leaf->id), sizeof(leaf->id));
  return !err;
}


/* Save a leaf into the internal database.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbleafsave(TCBDB *bdb, BDBLEAF *leaf){
  assert(bdb && leaf);
  TCDODEBUG(bdb->cnt_saveleaf++);
  TCXSTR *rbuf = tcxstrnew3(BDBPAGEBUFSIZ);
  char hbuf[(sizeof(uint64_t)+1)*3];
  char *wp = hbuf;
  uint64_t llnum;
  int step;
  llnum = leaf->prev;
  TCSETVNUMBUF64(step, wp, llnum);
  wp += step;
  llnum = leaf->next;
  TCSETVNUMBUF64(step, wp, llnum);
  wp += step;
  TCXSTRCAT(rbuf, hbuf, wp - hbuf);
  TCLIST *recs = leaf->recs;
  int ln = TCLISTNUM(recs);
  for(int i = 0; i < ln; i++){
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    int lnum;
    wp = hbuf;
    lnum = recp->ksiz;
    TCSETVNUMBUF(step, wp, lnum);
    wp += step;
    lnum = recp->vsiz;
    TCSETVNUMBUF(step, wp, lnum);
    wp += step;
    TCLIST *rest = recp->rest;
    int rnum = rest ? TCLISTNUM(rest) : 0;
    TCSETVNUMBUF(step, wp, rnum);
    wp += step;
    TCXSTRCAT(rbuf, hbuf, wp - hbuf);
    TCXSTRCAT(rbuf, recp->kbuf, recp->ksiz);
    TCXSTRCAT(rbuf, recp->vbuf, recp->vsiz);
    for(int j = 0; j < rnum; j++){
      int vsiz;
      const char *vbuf = tclistval(rest, j, &vsiz);
      TCSETVNUMBUF(step, hbuf, vsiz);
      TCXSTRCAT(rbuf, hbuf, step);
      TCXSTRCAT(rbuf, vbuf, vsiz);
    }
  }
  bool err = false;
  step = sprintf(hbuf, "%llx", (unsigned long long)leaf->id);
  if(ln < 1 && !tchdbout(bdb->hdb, hbuf, step) && tchdbecode(bdb->hdb) != TCENOREC)
    err = true;
  if(!leaf->dead && !tchdbput(bdb->hdb, hbuf, step, TCXSTRPTR(rbuf), TCXSTRSIZE(rbuf)))
    err = true;
  tcxstrdel(rbuf);
  leaf->dirty = false;
  return !err;
}


/* Load a leaf from the internal database.
   `bdb' specifies the B+ tree database object.
   `id' specifies the ID number of the leaf.
   The return value is the leaf object or `NULL' on failure. */
static BDBLEAF *tcbdbleafload(TCBDB *bdb, uint64_t id){
  assert(bdb && id > 0);
  bool clk = BDBLOCKCACHE(bdb);
  int rsiz;
  BDBLEAF *leaf = (BDBLEAF *)tcmapget3(bdb->leafc, &id, sizeof(id), &rsiz);
  if(leaf){
    if(clk) BDBUNLOCKCACHE(bdb);
    return leaf;
  }
  if(clk) BDBUNLOCKCACHE(bdb);
  TCDODEBUG(bdb->cnt_loadleaf++);
  char hbuf[(sizeof(uint64_t)+1)*3];
  int step;
  step = sprintf(hbuf, "%llx", (unsigned long long)id);
  char *rbuf = NULL;
  char wbuf[BDBPAGEBUFSIZ];
  const char *rp = NULL;
  rsiz = tchdbget3(bdb->hdb, hbuf, step, wbuf, BDBPAGEBUFSIZ);
  if(rsiz < 1){
    tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
    return false;
  } else if(rsiz < BDBPAGEBUFSIZ){
    rp = wbuf;
  } else {
    if(!(rbuf = tchdbget(bdb->hdb, hbuf, step, &rsiz))){
      tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
      return false;
    }
    rp = rbuf;
  }
  BDBLEAF lent;
  lent.id = id;
  uint64_t llnum;
  TCREADVNUMBUF64(rp, llnum, step);
  lent.prev = llnum;
  rp += step;
  rsiz -= step;
  TCREADVNUMBUF64(rp, llnum, step);
  lent.next = llnum;
  rp += step;
  rsiz -= step;
  lent.dirty = false;
  lent.dead = false;
  lent.recs = tclistnew2(bdb->lmemb + 1);
  bool err = false;
  while(rsiz >= 3){
    BDBREC rec;
    TCREADVNUMBUF(rp, rec.ksiz, step);
    rp += step;
    rsiz -= step;
    TCREADVNUMBUF(rp, rec.vsiz, step);
    rp += step;
    rsiz -= step;
    int rnum;
    TCREADVNUMBUF(rp, rnum, step);
    rp += step;
    rsiz -= step;
    if(rsiz < rec.ksiz + rec.vsiz + rnum){
      err = true;
      break;
    }
    TCMEMDUP(rec.kbuf, rp, rec.ksiz);
    rp += rec.ksiz;
    rsiz -= rec.ksiz;
    TCMEMDUP(rec.vbuf, rp, rec.vsiz);
    rp += rec.vsiz;
    rsiz -= rec.vsiz;
    if(rnum > 0){
      rec.rest = tclistnew2(rnum);
      while(rnum-- > 0 && rsiz > 0){
        int vsiz;
        TCREADVNUMBUF(rp, vsiz, step);
        rp += step;
        rsiz -= step;
        if(rsiz < vsiz){
          err = true;
          break;
        }
        TCLISTPUSH(rec.rest, rp, vsiz);
        rp += vsiz;
        rsiz -= vsiz;
      }
    } else {
      rec.rest = NULL;
    }
    TCLISTPUSH(lent.recs, &rec, sizeof(rec));
  }
  free(rbuf);
  if(err || rsiz != 0){
    tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
    return NULL;
  }
  clk = BDBLOCKCACHE(bdb);
  tcmapputkeep(bdb->leafc, &(lent.id), sizeof(lent.id), &lent, sizeof(lent));
  leaf = (BDBLEAF *)tcmapget(bdb->leafc, &(lent.id), sizeof(lent.id), &rsiz);
  if(clk) BDBUNLOCKCACHE(bdb);
  return leaf;
}


/* Load the historical leaf from the internal database.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is the pointer to the leaf, else, it is `NULL'. */
static BDBLEAF *tcbdbgethistleaf(TCBDB *bdb, const char *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  BDBLEAF *leaf = tcbdbleafload(bdb, bdb->hleaf);
  if(!leaf) return NULL;
  int ln = TCLISTNUM(leaf->recs);
  if(ln < 2) return NULL;
  BDBREC *recp = (BDBREC *)TCLISTVALPTR(leaf->recs, 0);
  int rv;
  if(bdb->cmp == tcbdbcmplexical){
    TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
  } else {
    rv = bdb->cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, bdb->cmpop);
  }
  if(rv == 0) return leaf;
  if(rv < 0) return NULL;
  recp = (BDBREC *)TCLISTVALPTR(leaf->recs, ln - 1);
  if(bdb->cmp == tcbdbcmplexical){
    TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
  } else {
    rv = bdb->cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, bdb->cmpop);
  }
  if(rv <= 0 || leaf->next < 1) return leaf;
  return NULL;
}


/* Add a record to a leaf.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   `dmode' specifies behavior when the key overlaps.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   If successful, the return value is true, else, it is false. */
static bool tcbdbleafaddrec(TCBDB *bdb, BDBLEAF *leaf, int dmode,
                            const char *kbuf, int ksiz, const char *vbuf, int vsiz){
  assert(bdb && leaf && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  BDBCMP cmp = bdb->cmp;
  void *cmpop = bdb->cmpop;
  TCLIST *recs = leaf->recs;
  int ln = TCLISTNUM(recs);
  int left = 0;
  int right = ln;
  int i = (left + right) / 2;
  while(right >= left && i < ln){
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    int rv;
    if(cmp == tcbdbcmplexical){
      TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
    } else {
      rv = cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, cmpop);
    }
    if(rv == 0){
      break;
    } else if(rv <= 0){
      right = i - 1;
    } else {
      left = i + 1;
    }
    i = (left + right) / 2;
  }
  while(i < ln){
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    int rv;
    if(cmp == tcbdbcmplexical){
      TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
    } else {
      rv = cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, cmpop);
    }
    if(rv == 0){
      switch(dmode){
      case BDBPDKEEP:
        return false;
      case BDBPDCAT:
        TCREALLOC(recp->vbuf, recp->vbuf, recp->vsiz + vsiz + 1);
        memcpy(recp->vbuf + recp->vsiz, vbuf, vsiz);
        recp->vsiz += vsiz;
        recp->vbuf[recp->vsiz] = '\0';
        break;
      case BDBPDDUP:
        if(!recp->rest) recp->rest = tclistnew();
        TCLISTPUSH(recp->rest, vbuf, vsiz);
        bdb->rnum++;
        break;
      case BDBPDDUPB:
        if(!recp->rest) recp->rest = tclistnew();
        tclistunshift(recp->rest, recp->vbuf, recp->vsiz);
        if(vsiz > recp->vsiz) TCREALLOC(recp->vbuf, recp->vbuf, vsiz + 1);
        memcpy(recp->vbuf, vbuf, vsiz);
        recp->vbuf[vsiz] = '\0';
        recp->vsiz = vsiz;
        bdb->rnum++;
        break;
      default:
        if(vsiz > recp->vsiz) TCREALLOC(recp->vbuf, recp->vbuf, vsiz + 1);
        memcpy(recp->vbuf, vbuf, vsiz);
        recp->vbuf[vsiz] = '\0';
        recp->vsiz = vsiz;
        break;
      }
      break;
    } else if(rv < 0){
      BDBREC rec;
      TCMEMDUP(rec.kbuf, kbuf, ksiz);
      rec.ksiz = ksiz;
      TCMEMDUP(rec.vbuf, vbuf, vsiz);
      rec.vsiz = vsiz;
      rec.rest = NULL;
      tclistinsert(recs, i, &rec, sizeof(rec));
      bdb->rnum++;
      break;
    }
    i++;
  }
  if(i >= ln){
    BDBREC rec;
    TCMEMDUP(rec.kbuf, kbuf, ksiz);
    rec.ksiz = ksiz;
    TCMEMDUP(rec.vbuf, vbuf, vsiz);
    rec.vsiz = vsiz;
    rec.rest = NULL;
    TCLISTPUSH(recs, &rec, sizeof(rec));
    bdb->rnum++;
  }
  leaf->dirty = true;
  return true;
}


/* Calculate the size of data of a leaf object.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   The return value is size of data of the leaf. */
static int tcbdbleafdatasize(BDBLEAF *leaf){
  assert(leaf);
  int sum = 0;
  TCLIST *recs = leaf->recs;
  int ln = TCLISTNUM(recs);
  for(int i = 0; i < ln; i++){
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    sum += recp->ksiz + recp->vsiz;
    if(recp->rest){
      TCLIST *rest = recp->rest;
      int rnum = TCLISTNUM(rest);
      for(int j = 0; j < rnum; j++){
        int vsiz;
        tclistval(rest, j, &vsiz);
        sum += vsiz;
      }
    }
  }
  return sum;
}


/* Divide a leaf into two.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   The return value is the new leaf object or `NULL' on failure. */
static BDBLEAF *tcbdbleafdivide(TCBDB *bdb, BDBLEAF *leaf){
  assert(bdb && leaf);
  bdb->hleaf = 0;
  TCLIST *recs = leaf->recs;
  int mid = TCLISTNUM(recs) / 2;
  BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, mid);
  BDBLEAF *newleaf = tcbdbleafnew(bdb, leaf->id, leaf->next);
  if(newleaf->next > 0){
    BDBLEAF *nextleaf = tcbdbleafload(bdb, newleaf->next);
    if(!nextleaf) return NULL;
    nextleaf->prev = newleaf->id;
    nextleaf->dirty = true;
  }
  leaf->next = newleaf->id;
  leaf->dirty = true;
  int ln = TCLISTNUM(recs);
  TCLIST *newrecs = newleaf->recs;
  for(int i = mid; i < ln; i++){
    recp = (BDBREC *)TCLISTVALPTR(recs, i);
    TCLISTPUSH(newrecs, recp, sizeof(*recp));
  }
  ln = TCLISTNUM(newrecs);
  for(int i = 0; i < ln; i++){
    int rsiz;
    free(tclistpop(recs, &rsiz));
  }
  return newleaf;
}


/* Cut off the path to a leaf and mark it dead.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbleafkill(TCBDB *bdb, BDBLEAF *leaf){
  assert(bdb && leaf);
  BDBNODE *node = tcbdbnodeload(bdb, bdb->hist[bdb->hnum-1]);
  if(!node) return false;
  if(tcbdbnodesubidx(bdb, node, leaf->id)){
    TCDODEBUG(bdb->cnt_killleaf++);
    if(bdb->hleaf == leaf->id) bdb->hleaf = 0;
    if(leaf->prev > 0){
      BDBLEAF *tleaf = tcbdbleafload(bdb, leaf->prev);
      if(!tleaf) return false;
      tleaf->next = leaf->next;
      tleaf->dirty = true;
      if(bdb->last == leaf->id) bdb->last = leaf->prev;
    }
    if(leaf->next > 0){
      BDBLEAF *tleaf = tcbdbleafload(bdb, leaf->next);
      if(!tleaf) return false;
      tleaf->prev = leaf->prev;
      tleaf->dirty = true;
      if(bdb->first == leaf->id) bdb->first = leaf->next;
    }
    leaf->dead = true;
  }
  return true;
}


/* Create a new node.
   `bdb' specifies the B+ tree database object.
   `heir' specifies the ID of the child before the first index.
   The return value is the new node object. */
static BDBNODE *tcbdbnodenew(TCBDB *bdb, uint64_t heir){
  assert(bdb && heir > 0);
  BDBNODE nent;
  nent.id = ++bdb->nnum + BDBNODEIDBASE;
  nent.idxs = tclistnew2(bdb->nmemb + 1);
  nent.heir = heir;
  nent.dirty = true;
  tcmapputkeep(bdb->nodec, &(nent.id), sizeof(nent.id), &nent, sizeof(nent));
  int rsiz;
  return (BDBNODE *)tcmapget(bdb->nodec, &(nent.id), sizeof(nent.id), &rsiz);
}


/* Remove a node from the cache.
   `bdb' specifies the B+ tree database object.
   `node' specifies the node object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbnodecacheout(TCBDB *bdb, BDBNODE *node){
  assert(bdb && node);
  bool err = false;
  if(node->dirty && !tcbdbnodesave(bdb, node)) err = true;
  TCLIST *idxs = node->idxs;
  int ln = TCLISTNUM(idxs);
  for(int i = 0; i < ln; i++){
    BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
    free(idxp->kbuf);
  }
  tclistdel(idxs);
  tcmapout(bdb->nodec, &(node->id), sizeof(node->id));
  return !err;
}


/* Save a node into the internal database.
   `bdb' specifies the B+ tree database object.
   `node' specifies the node object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbnodesave(TCBDB *bdb, BDBNODE *node){
  assert(bdb && node);
  TCDODEBUG(bdb->cnt_savenode++);
  TCXSTR *rbuf = tcxstrnew3(BDBPAGEBUFSIZ);
  char hbuf[(sizeof(uint64_t)+1)*2];
  uint64_t llnum;
  int step;
  llnum = node->heir;
  TCSETVNUMBUF64(step, hbuf, llnum);
  TCXSTRCAT(rbuf, hbuf, step);
  TCLIST *idxs = node->idxs;
  int ln = TCLISTNUM(idxs);
  for(int i = 0; i < ln; i++){
    BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
    char *wp = hbuf;
    llnum = idxp->pid;
    TCSETVNUMBUF64(step, wp, llnum);
    wp += step;
    uint32_t lnum = idxp->ksiz;
    TCSETVNUMBUF(step, wp, lnum);
    wp += step;
    TCXSTRCAT(rbuf, hbuf, wp - hbuf);
    TCXSTRCAT(rbuf, idxp->kbuf, idxp->ksiz);
  }
  bool err = false;
  step = sprintf(hbuf, "#%llx", (unsigned long long)(node->id - BDBNODEIDBASE));
  if(!tchdbput(bdb->hdb, hbuf, step, TCXSTRPTR(rbuf), TCXSTRSIZE(rbuf))) err = true;
  tcxstrdel(rbuf);
  node->dirty = false;
  return !err;
}


/* Load a node from the internal database.
   `bdb' specifies the B+ tree database object.
   `id' specifies the ID number of the node.
   The return value is the node object or `NULL' on failure. */
static BDBNODE *tcbdbnodeload(TCBDB *bdb, uint64_t id){
  assert(bdb && id > BDBNODEIDBASE);
  bool clk = BDBLOCKCACHE(bdb);
  int rsiz;
  BDBNODE *node = (BDBNODE *)tcmapget3(bdb->nodec, &id, sizeof(id), &rsiz);
  if(node){
    if(clk) BDBUNLOCKCACHE(bdb);
    return node;
  }
  if(clk) BDBUNLOCKCACHE(bdb);
  TCDODEBUG(bdb->cnt_loadnode++);
  char hbuf[(sizeof(uint64_t)+1)*2];
  int step;
  step = sprintf(hbuf, "#%llx", (unsigned long long)(id - BDBNODEIDBASE));
  char *rbuf = NULL;
  char wbuf[BDBPAGEBUFSIZ];
  const char *rp = NULL;
  rsiz = tchdbget3(bdb->hdb, hbuf, step, wbuf, BDBPAGEBUFSIZ);
  if(rsiz < 1){
    tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
    return false;
  } else if(rsiz < BDBPAGEBUFSIZ){
    rp = wbuf;
  } else {
    if(!(rbuf = tchdbget(bdb->hdb, hbuf, step, &rsiz))){
      tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
      return false;
    }
    rp = rbuf;
  }
  BDBNODE nent;
  nent.id = id;
  uint64_t llnum;
  TCREADVNUMBUF64(rp, llnum, step);
  nent.heir = llnum;
  rp += step;
  rsiz -= step;
  nent.dirty = false;
  nent.idxs = tclistnew2(bdb->nmemb + 1);
  bool err = false;
  while(rsiz >= 2){
    BDBIDX idx;
    TCREADVNUMBUF64(rp, idx.pid, step);
    rp += step;
    rsiz -= step;
    TCREADVNUMBUF(rp, idx.ksiz, step);
    rp += step;
    rsiz -= step;
    if(rsiz < idx.ksiz){
      err = true;
      break;
    }
    TCMEMDUP(idx.kbuf, rp, idx.ksiz);
    rp += idx.ksiz;
    rsiz -= idx.ksiz;
    TCLISTPUSH(nent.idxs, &idx, sizeof(idx));
  }
  free(rbuf);
  if(err || rsiz != 0){
    tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
    return NULL;
  }
  clk = BDBLOCKCACHE(bdb);
  tcmapputkeep(bdb->nodec, &(nent.id), sizeof(nent.id), &nent, sizeof(nent));
  node = (BDBNODE *)tcmapget(bdb->nodec, &(nent.id), sizeof(nent.id), &rsiz);
  if(clk) BDBUNLOCKCACHE(bdb);
  return node;
}


/* Add an index to a node.
   `bdb' specifies the B+ tree database object.
   `node' specifies the node object.
   `order' specifies whether the calling sequence is orderd or not.
   `pid' specifies the ID number of referred page.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key. */
static void tcbdbnodeaddidx(TCBDB *bdb, BDBNODE *node, bool order, uint64_t pid,
                            const char *kbuf, int ksiz){
  assert(bdb && node && pid > 0 && kbuf && ksiz >= 0);
  BDBIDX idx;
  idx.pid = pid;
  TCMEMDUP(idx.kbuf, kbuf, ksiz);
  idx.ksiz = ksiz;
  BDBCMP cmp = bdb->cmp;
  void *cmpop = bdb->cmpop;
  TCLIST *idxs = node->idxs;
  if(order){
    TCLISTPUSH(idxs, &idx, sizeof(idx));
  } else {
    int ln = TCLISTNUM(idxs);
    int left = 0;
    int right = ln;
    int i = (left + right) / 2;
    while(right >= left && i < ln){
      BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
      int rv;
      if(cmp == tcbdbcmplexical){
        TCCMPLEXICAL(rv, kbuf, ksiz, idxp->kbuf, idxp->ksiz);
      } else {
        rv = cmp(kbuf, ksiz, idxp->kbuf, idxp->ksiz, cmpop);
      }
      if(rv == 0){
        break;
      } else if(rv <= 0){
        right = i - 1;
      } else {
        left = i + 1;
      }
      i = (left + right) / 2;
    }
    while(i < ln){
      BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
      int rv;
      if(cmp == tcbdbcmplexical){
        TCCMPLEXICAL(rv, kbuf, ksiz, idxp->kbuf, idxp->ksiz);
      } else {
        rv = cmp(kbuf, ksiz, idxp->kbuf, idxp->ksiz, cmpop);
      }
      if(rv < 0){
        tclistinsert(idxs, i, &idx, sizeof(idx));
        break;
      }
      i++;
    }
    if(i >= ln) TCLISTPUSH(idxs, &idx, sizeof(idx));
  }
  node->dirty = true;
}


/* Subtract an index from a node.
   `bdb' specifies the B+ tree database object.
   `node' specifies the node object.
   `pid' specifies the ID number of referred page.
   The return value is whether the subtraction is completed. */
static bool tcbdbnodesubidx(TCBDB *bdb, BDBNODE *node, uint64_t pid){
  assert(bdb && node && pid > 0);
  TCLIST *idxs = node->idxs;
  int ln = TCLISTNUM(idxs);
  if(ln < 2) return false;
  if(node->heir == pid){
    int rsiz;
    BDBIDX *idxp = (BDBIDX *)tclistshift(idxs, &rsiz);
    node->heir = idxp->pid;
    free(idxp->kbuf);
    free(idxp);
    node->dirty = true;
    return true;
  } else {
    int ln = TCLISTNUM(idxs);
    for(int i = 0; i < ln; i++){
      BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
      if(idxp->pid == pid){
        int rsiz;
        free(idxp->kbuf);
        free(tclistremove(idxs, i, &rsiz));
        node->dirty = true;
        return true;
      }
    }
  }
  return false;
}


/* Search the leaf object corresponding to a key.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   The return value is the ID number of the leaf object or 0 on failure. */
static uint64_t tcbdbsearchleaf(TCBDB *bdb, const char *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  BDBCMP cmp = bdb->cmp;
  void *cmpop = bdb->cmpop;
  uint64_t *hist = bdb->hist;
  uint64_t pid = bdb->root;
  int hnum = 0;
  bdb->hleaf = 0;
  while(pid > BDBNODEIDBASE){
    BDBNODE *node = tcbdbnodeload(bdb, pid);
    if(!node){
      tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
      return 0;
    }
    TCLIST *idxs = node->idxs;
    int ln = TCLISTNUM(idxs);
    if(ln < 1){
      tcbdbsetecode(bdb, TCEMISC, __FILE__, __LINE__, __func__);
      return 0;
    }
    hist[hnum++] = node->id;
    int left = 0;
    int right = ln;
    int i = (left + right) / 2;
    BDBIDX *idxp = NULL;
    while(right >= left && i < ln){
      idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
      int rv;
      if(cmp == tcbdbcmplexical){
        TCCMPLEXICAL(rv, kbuf, ksiz, idxp->kbuf, idxp->ksiz);
      } else {
        rv = cmp(kbuf, ksiz, idxp->kbuf, idxp->ksiz, cmpop);
      }
      if(rv == 0){
        break;
      } else if(rv <= 0){
        right = i - 1;
      } else {
        left = i + 1;
      }
      i = (left + right) / 2;
    }
    if(i > 0) i--;
    while(i < ln){
      idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
      int rv;
      if(cmp == tcbdbcmplexical){
        TCCMPLEXICAL(rv, kbuf, ksiz, idxp->kbuf, idxp->ksiz);
      } else {
        rv = cmp(kbuf, ksiz, idxp->kbuf, idxp->ksiz, cmpop);
      }
      if(rv < 0){
        if(i == 0){
          pid = node->heir;
          break;
        }
        idxp = (BDBIDX *)TCLISTVALPTR(idxs, i - 1);
        pid = idxp->pid;
        break;
      }
      i++;
    }
    if(i >= ln) pid = idxp->pid;
  }
  if(!bdb->mmtx){
    if(bdb->lleaf == pid) bdb->hleaf = pid;
    bdb->lleaf = pid;
  }
  bdb->hnum = hnum;
  return pid;
}


/* Search a record of a leaf.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `ip' specifies the pointer to a variable to fetch the index of the correspnding record.
   The return value is the pointer to a corresponding record or `NULL' on failure. */
static BDBREC *tcbdbsearchrec(TCBDB *bdb, BDBLEAF *leaf, const char *kbuf, int ksiz, int *ip){
  assert(bdb && leaf && kbuf && ksiz >= 0);
  BDBCMP cmp = bdb->cmp;
  void *cmpop = bdb->cmpop;
  TCLIST *recs = leaf->recs;
  int ln = TCLISTNUM(recs);
  int left = 0;
  int right = ln;
  int i = (left + right) / 2;
  while(right >= left && i < ln){
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    int rv;
    if(cmp == tcbdbcmplexical){
      TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
    } else {
      rv = cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, cmpop);
    }
    if(rv == 0){
      if(ip) *ip = i;
      return recp;
    } else if(rv <= 0){
      right = i - 1;
    } else {
      left = i + 1;
    }
    i = (left + right) / 2;
  }
  if(ip) *ip = i;
  return NULL;
}


/* Adjust the caches for leaves and nodes.
   `bdb' specifies the B+ tree database object.
   The return value is true if successful, else, it is false. */
static bool tcbdbcacheadjust(TCBDB *bdb){
  bool err = false;
  if(TCMAPRNUM(bdb->leafc) > bdb->lcnum){
    TCDODEBUG(bdb->cnt_adjleafc++);
    bool clk = BDBLOCKCACHE(bdb);
    TCMAP *leafc = bdb->leafc;
    tcmapiterinit(leafc);
    for(int i = 0; i < BDBCACHEOUT; i++){
      int rsiz;
      if(!tcbdbleafcacheout(bdb, (BDBLEAF *)tcmapiterval(tcmapiternext(leafc, &rsiz), &rsiz)))
        err = true;
    }
    if(clk) BDBUNLOCKCACHE(bdb);
  }
  if(TCMAPRNUM(bdb->nodec) > bdb->ncnum){
    TCDODEBUG(bdb->cnt_adjnodec++);
    bool clk = BDBLOCKCACHE(bdb);
    TCMAP *nodec = bdb->nodec;
    tcmapiterinit(nodec);
    for(int i = 0; i < BDBCACHEOUT; i++){
      int rsiz;
      if(!tcbdbnodecacheout(bdb, (BDBNODE *)tcmapiterval(tcmapiternext(nodec, &rsiz), &rsiz)))
        err = true;
    }
    if(clk) BDBUNLOCKCACHE(bdb);
  }
  return !err;
}


/* Purge dirty pages of caches for leaves and nodes.
   `bdb' specifies the B+ tree database object. */
static void tcbdbcachepurge(TCBDB *bdb){
  bool clk = BDBLOCKCACHE(bdb);
  int tsiz;
  const char *tmp;
  tcmapiterinit(bdb->leafc);
  while((tmp = tcmapiternext(bdb->leafc, &tsiz)) != NULL){
    int lsiz;
    BDBLEAF *leaf = (BDBLEAF *)tcmapiterval(tmp, &lsiz);
    if(!leaf->dirty) continue;
    TCLIST *recs = leaf->recs;
    int ln = TCLISTNUM(recs);
    for(int i = 0; i < ln; i++){
      BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
      free(recp->kbuf);
      free(recp->vbuf);
      if(recp->rest) tclistdel(recp->rest);
    }
    tclistdel(recs);
    tcmapout(bdb->leafc, tmp, tsiz);
  }
  tcmapiterinit(bdb->nodec);
  while((tmp = tcmapiternext(bdb->nodec, &tsiz)) != NULL){
    int nsiz;
    BDBNODE *node = (BDBNODE *)tcmapiterval(tmp, &nsiz);
    if(!node->dirty) continue;
    TCLIST *idxs = node->idxs;
    int ln = TCLISTNUM(idxs);
    for(int i = 0; i < ln; i++){
      BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
      free(idxp->kbuf);
    }
    tclistdel(idxs);
    tcmapout(bdb->nodec, tmp, tsiz);
  }
  if(clk) BDBUNLOCKCACHE(bdb);
}


/* Open a database file and connect a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `path' specifies the path of the internal database file.
   `omode' specifies the connection mode.
   If successful, the return value is true, else, it is false. */
static bool tcbdbopenimpl(TCBDB *bdb, const char *path, int omode){
  assert(bdb && path);
  int homode = HDBOREADER;
  if(omode & BDBOWRITER){
    homode = HDBOWRITER;
    if(omode & BDBOCREAT) homode |= HDBOCREAT;
    if(omode & BDBOTRUNC) homode |= HDBOTRUNC;
    bdb->wmode = true;
  } else {
    bdb->wmode = false;
  }
  if(omode & BDBONOLCK) homode |= HDBONOLCK;
  if(omode & BDBOLCKNB) homode |= HDBOLCKNB;
  tchdbsettype(bdb->hdb, HDBTBTREE);
  if(!tchdbopen(bdb->hdb, path, homode)) return false;
  bdb->root = 0;
  bdb->first = 0;
  bdb->last = 0;
  bdb->lnum = 0;
  bdb->nnum = 0;
  bdb->rnum = 0;
  bdb->opaque = tchdbopaque(bdb->hdb);
  bdb->leafc = tcmapnew2(bdb->lcnum * 2 + 1);
  bdb->nodec = tcmapnew2(bdb->ncnum * 2 + 1);
  if(bdb->wmode && tchdbrnum(bdb->hdb) < 1){
    BDBLEAF *leaf = tcbdbleafnew(bdb, 0, 0);
    bdb->root = leaf->id;
    bdb->first = leaf->id;
    bdb->last = leaf->id;
    bdb->lnum = 1;
    bdb->nnum = 0;
    bdb->rnum = 0;
    if(!bdb->cmp){
      bdb->cmp = tcbdbcmplexical;
      bdb->cmpop = NULL;
    }
    tcdumpmeta(bdb);
    if(!tcbdbleafsave(bdb, leaf)){
      tcmapdel(bdb->nodec);
      tcmapdel(bdb->leafc);
      tchdbclose(bdb->hdb);
      return false;
    }
  }
  tcloadmeta(bdb);
  if(!bdb->cmp){
    tcbdbsetecode(bdb, TCEINVALID, __FILE__, __LINE__, __func__);
    tcmapdel(bdb->nodec);
    tcmapdel(bdb->leafc);
    tchdbclose(bdb->hdb);
    return false;
  }
  if(bdb->lmemb < BDBMINLMEMB || bdb->nmemb < BDBMINNMEMB ||
     bdb->root < 1 || bdb->first < 1 || bdb->last < 1 ||
     bdb->lnum < 0 || bdb->nnum < 0 || bdb->rnum < 0){
    tcbdbsetecode(bdb, TCEMETA, __FILE__, __LINE__, __func__);
    tcmapdel(bdb->nodec);
    tcmapdel(bdb->leafc);
    tchdbclose(bdb->hdb);
    return false;
  }
  bdb->open = true;
  uint8_t hopts = tchdbopts(bdb->hdb);
  uint8_t opts = 0;
  if(hopts & HDBTLARGE) opts |= BDBTLARGE;
  if(hopts & HDBTDEFLATE) opts |= BDBTDEFLATE;
  if(hopts & HDBTTCBS) opts |= BDBTTCBS;
  bdb->opts = opts;
  bdb->hleaf = 0;
  bdb->lleaf = 0;
  bdb->tran = false;
  bdb->rbopaque = NULL;
  return true;
}


/* Close a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcloseimpl(TCBDB *bdb){
  assert(bdb);
  if(bdb->tran){
    tcbdbcachepurge(bdb);
    memcpy(bdb->opaque, bdb->rbopaque, BDBOPAQUESIZ);
    tcloadmeta(bdb);
    free(bdb->rbopaque);
    bdb->tran = false;
    bdb->rbopaque = NULL;
    BDBUNLOCKTRAN(bdb);
  }
  bool err = false;
  bdb->open = false;
  const char *vbuf;
  int vsiz;
  TCMAP *leafc = bdb->leafc;
  tcmapiterinit(leafc);
  while((vbuf = tcmapiternext(leafc, &vsiz)) != NULL){
    if(!tcbdbleafcacheout(bdb, (BDBLEAF *)tcmapiterval(vbuf, &vsiz))) err = true;
  }
  TCMAP *nodec = bdb->nodec;
  tcmapiterinit(nodec);
  while((vbuf = tcmapiternext(nodec, &vsiz)) != NULL){
    if(!tcbdbnodecacheout(bdb, (BDBNODE *)tcmapiterval(vbuf, &vsiz))) err = true;
  }
  if(bdb->wmode) tcdumpmeta(bdb);
  tcmapdel(bdb->nodec);
  tcmapdel(bdb->leafc);
  if(!tchdbclose(bdb->hdb)) err = true;
  return !err;
}


/* Store a record into a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   `dmode' specifies behavior when the key overlaps.
   If successful, the return value is true, else, it is false. */
static bool tcbdbputimpl(TCBDB *bdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz,
                         int dmode){
  assert(bdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  BDBLEAF *leaf = NULL;
  if(bdb->hleaf < 1 || !(leaf = tcbdbgethistleaf(bdb, kbuf, ksiz))){
    uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
    if(pid < 1) return false;
    if(!(leaf = tcbdbleafload(bdb, pid))) return false;
  }
  if(!tcbdbleafaddrec(bdb, leaf, dmode, kbuf, ksiz, vbuf, vsiz)){
    tcbdbsetecode(bdb, TCEKEEP, __FILE__, __LINE__, __func__);
    return false;
  }
  int rnum = TCLISTNUM(leaf->recs);
  if(rnum > bdb->lmemb ||
     (bdb->lsmax > 0 && rnum > BDBMINLMEMB && (bdb->lschk++ & (0x8 - 1)) == 0 &&
      tcbdbleafdatasize(leaf) > bdb->lsmax)){
    bdb->lschk = 0;
    BDBLEAF *newleaf = tcbdbleafdivide(bdb, leaf);
    if(!newleaf) return false;
    if(leaf->id == bdb->last) bdb->last = newleaf->id;
    uint64_t heir = leaf->id;
    uint64_t pid = newleaf->id;
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(newleaf->recs, 0);
    int ksiz = recp->ksiz;
    char *kbuf;
    TCMEMDUP(kbuf, recp->kbuf, ksiz);
    while(true){
      BDBNODE *node;
      if(bdb->hnum < 1){
        node = tcbdbnodenew(bdb, heir);
        tcbdbnodeaddidx(bdb, node, true, pid, kbuf, ksiz);
        bdb->root = node->id;
        free(kbuf);
        break;
      }
      uint64_t parent = bdb->hist[--bdb->hnum];
      if(!(node = tcbdbnodeload(bdb, parent))){
        free(kbuf);
        return false;
      }
      tcbdbnodeaddidx(bdb, node, false, pid, kbuf, ksiz);
      free(kbuf);
      TCLIST *idxs = node->idxs;
      int ln = TCLISTNUM(idxs);
      if(ln <= bdb->nmemb) break;
      int mid = ln / 2;
      BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, mid);
      BDBNODE *newnode = tcbdbnodenew(bdb, idxp->pid);
      heir = node->id;
      pid = newnode->id;
      TCMEMDUP(kbuf, idxp->kbuf, idxp->ksiz);
      ksiz = idxp->ksiz;
      for(int i = mid + 1; i < ln; i++){
        idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
        tcbdbnodeaddidx(bdb, newnode, true, idxp->pid, idxp->kbuf, idxp->ksiz);
      }
      ln = TCLISTNUM(newnode->idxs);
      for(int i = 0; i < ln; i++){
        int rsiz;
        idxp = (BDBIDX *)tclistpop(idxs, &rsiz);
        free(idxp->kbuf);
        free(idxp);
      }
      node->dirty = true;
    }
    if(bdb->capnum > 0 && bdb->rnum > bdb->capnum){
      uint64_t xnum = bdb->rnum - bdb->capnum;
      BDBCUR *cur = tcbdbcurnew(bdb);
      tcbdbcurfirstimpl(cur);
      while((xnum--) > 0){
        if(!tcbdbcuroutimpl(cur)){
          tcbdbcurdel(cur);
          return false;
        }
      }
      tcbdbcurdel(cur);
    }
  }
  if(!bdb->tran && !tcbdbcacheadjust(bdb)) return false;
  return true;
}


/* Remove a record of a B+ tree database object.
   `hdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is true, else, it is false. */
static bool tcbdboutimpl(TCBDB *bdb, const char *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  BDBLEAF *leaf = NULL;
  if(bdb->hleaf < 1 || !(leaf = tcbdbgethistleaf(bdb, kbuf, ksiz))){
    uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
    if(pid < 1) return false;
    if(!(leaf = tcbdbleafload(bdb, pid))) return false;
  }
  int ri;
  BDBREC *recp = tcbdbsearchrec(bdb, leaf, kbuf, ksiz, &ri);
  if(!recp){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  if(recp->rest){
    free(recp->vbuf);
    recp->vbuf = tclistshift(recp->rest, &(recp->vsiz));
    if(TCLISTNUM(recp->rest) < 1){
      tclistdel(recp->rest);
      recp->rest = NULL;
    }
  } else {
    free(recp->vbuf);
    free(recp->kbuf);
    int rsiz;
    free(tclistremove(leaf->recs, ri, &rsiz));
  }
  leaf->dirty = true;
  bdb->rnum--;
  if(TCLISTNUM(leaf->recs) < 1 && bdb->hnum > 0 && !tcbdbleafkill(bdb, leaf)) return false;
  if(!bdb->tran && !tcbdbcacheadjust(bdb)) return false;
  return true;
}


/* Remove records of a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is true, else, it is false. */
static bool tcbdboutlist(TCBDB *bdb, const char *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  BDBLEAF *leaf = NULL;
  if(bdb->hleaf < 1 || !(leaf = tcbdbgethistleaf(bdb, kbuf, ksiz))){
    uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
    if(pid < 1) return false;
    if(!(leaf = tcbdbleafload(bdb, pid))) return false;
  }
  int ri;
  BDBREC *recp = tcbdbsearchrec(bdb, leaf, kbuf, ksiz, &ri);
  if(!recp){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  int rnum = 1;
  if(recp->rest){
    rnum += TCLISTNUM(recp->rest);
    tclistdel(recp->rest);
  }
  free(recp->vbuf);
  free(recp->kbuf);
  int rsiz;
  free(tclistremove(leaf->recs, ri, &rsiz));
  leaf->dirty = true;
  bdb->rnum -= rnum;
  if(TCLISTNUM(leaf->recs) < 1 && bdb->hnum > 0 && !tcbdbleafkill(bdb, leaf)) return false;
  if(!bdb->tran && !tcbdbcacheadjust(bdb)) return false;
  return true;
}


/* Retrieve a record in a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `sp' specifies the pointer to the variable into which the size of the region of the return
   value is assigned.
   If successful, the return value is the pointer to the region of the value of the corresponding
   record. */
static const char *tcbdbgetimpl(TCBDB *bdb, const char *kbuf, int ksiz, int *sp){
  assert(bdb && kbuf && ksiz >= 0 && sp);
  BDBLEAF *leaf = NULL;
  if(bdb->hleaf < 1 || !(leaf = tcbdbgethistleaf(bdb, kbuf, ksiz))){
    uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
    if(pid < 1) return NULL;
    if(!(leaf = tcbdbleafload(bdb, pid))) return NULL;
  }
  BDBREC *recp = tcbdbsearchrec(bdb, leaf, kbuf, ksiz, NULL);
  if(!recp){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return NULL;
  }
  *sp = recp->vsiz;
  return recp->vbuf;
}


/* Get the number of records corresponding a key in a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is the number of the corresponding records, else, it is 0. */
static int tcbdbgetnum(TCBDB *bdb, const char *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  BDBLEAF *leaf = NULL;
  if(bdb->hleaf < 1 || !(leaf = tcbdbgethistleaf(bdb, kbuf, ksiz))){
    uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
    if(pid < 1) return 0;
    if(!(leaf = tcbdbleafload(bdb, pid))) return 0;
  }
  BDBREC *recp = tcbdbsearchrec(bdb, leaf, kbuf, ksiz, NULL);
  if(!recp){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return 0;
  }
  return recp->rest ? TCLISTNUM(recp->rest) + 1 : 1;
}


/* Retrieve records in a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is a list object of the values of the corresponding records. */
static TCLIST *tcbdbgetlist(TCBDB *bdb, const char *kbuf, int ksiz){
  assert(bdb && kbuf && ksiz >= 0);
  BDBLEAF *leaf = NULL;
  if(bdb->hleaf < 1 || !(leaf = tcbdbgethistleaf(bdb, kbuf, ksiz))){
    uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
    if(pid < 1) return NULL;
    if(!(leaf = tcbdbleafload(bdb, pid))) return NULL;
  }
  BDBREC *recp = tcbdbsearchrec(bdb, leaf, kbuf, ksiz, NULL);
  if(!recp){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return NULL;
  }
  TCLIST *vals;
  TCLIST *rest = recp->rest;
  if(rest){
    int ln = TCLISTNUM(rest);
    vals = tclistnew2(ln + 1);
    TCLISTPUSH(vals, recp->vbuf, recp->vsiz);
    for(int i = 0; i < ln; i++){
      int vsiz;
      const char *vbuf = tclistval(rest, i, &vsiz);
      TCLISTPUSH(vals, vbuf, vsiz);
    }
  } else {
    vals = tclistnew2(1);
    TCLISTPUSH(vals, recp->vbuf, recp->vsiz);
  }
  return vals;
}


/* Get keys of ranged records in a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `bkbuf' specifies the pointer to the region of the key of the beginning border.
   `bksiz' specifies the size of the region of the beginning key.
   `binc' specifies whether the beginning border is inclusive or not.
   `ekbuf' specifies the pointer to the region of the key of the ending border.
   `eksiz' specifies the size of the region of the ending key.
   `einc' specifies whether the ending border is inclusive or not.
   `max' specifies the maximum number of keys to be fetched.
   `keys' specifies a list object to store the result.
   If successful, the return value is true, else, it is false. */
static bool tcbdbrangeimpl(TCBDB *bdb, const char *bkbuf, int bksiz, bool binc,
                           const char *ekbuf, int eksiz, bool einc, int max, TCLIST *keys){
  assert(bdb && keys);
  bool err = false;
  BDBCUR *cur = tcbdbcurnew(bdb);
  if(bkbuf){
    tcbdbcurjumpimpl(cur, bkbuf, bksiz, true);
  } else {
    tcbdbcurfirstimpl(cur);
  }
  BDBCMP cmp = bdb->cmp;
  void *cmpop = bdb->cmpop;
  const char *lbuf = NULL;
  int lsiz = 0;
  while(cur->id > 0){
    const char *kbuf, *vbuf;
    int ksiz, vsiz;
    if(!tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
      if(tchdbecode(bdb->hdb) != TCEINVALID && tchdbecode(bdb->hdb) != TCENOREC) err = true;
      break;
    }
    if(bkbuf && !binc){
      if(cmp(kbuf, ksiz, bkbuf, bksiz, cmpop) == 0){
        tcbdbcurnext(cur);
        continue;
      }
      bkbuf = NULL;
    }
    if(ekbuf){
      if(einc){
        if(cmp(kbuf, ksiz, ekbuf, eksiz, cmpop) > 0) break;
      } else {
        if(cmp(kbuf, ksiz, ekbuf, eksiz, cmpop) >= 0) break;
      }
    }
    if(!lbuf || lsiz != ksiz || memcmp(kbuf, lbuf, ksiz)){
      TCLISTPUSH(keys, kbuf, ksiz);
      if(max >= 0 && TCLISTNUM(keys) >= max) break;
      lbuf = kbuf;
      lsiz = ksiz;
    }
    tcbdbcurnextimpl(cur);
  }
  tcbdbcurdel(cur);
  return !err;
}


/* Get forward matching keys in a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `pbuf' specifies the pointer to the region of the prefix.
   `psiz' specifies the size of the region of the prefix.
   `max' specifies the maximum number of keys to be fetched.
   `keys' specifies a list object to store the result.
   If successful, the return value is true, else, it is false. */
static bool tcbdbrangefwm(TCBDB *bdb, const char *pbuf, int psiz, int max, TCLIST *keys){
  assert(bdb && pbuf && psiz >= 0 && keys);
  bool err = false;
  if(max < 0) max = INT_MAX;
  BDBCUR *cur = tcbdbcurnew(bdb);
  tcbdbcurjumpimpl(cur, pbuf, psiz, true);
  const char *lbuf = NULL;
  int lsiz = 0;
  while(cur->id > 0){
    const char *kbuf, *vbuf;
    int ksiz, vsiz;
    if(!tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
      if(tchdbecode(bdb->hdb) != TCEINVALID && tchdbecode(bdb->hdb) != TCENOREC) err = true;
      break;
    }
    if(ksiz < psiz || memcmp(kbuf, pbuf, psiz)) break;
    if(!lbuf || lsiz != ksiz || memcmp(kbuf, lbuf, ksiz)){
      TCLISTPUSH(keys, kbuf, ksiz);
      if(TCLISTNUM(keys) >= max) break;
      lbuf = kbuf;
      lsiz = ksiz;
    }
    tcbdbcurnextimpl(cur);
  }
  tcbdbcurdel(cur);
  return !err;
}


/* Optimize the file of a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `lmemb' specifies the number of members in each leaf page.
   `nmemb' specifies the number of members in each non-leaf page.
   `bnum' specifies the number of elements of the bucket array.
   `apow' specifies the size of record alignment by power of 2.
   `fpow' specifies the maximum number of elements of the free block pool by power of 2.
   `opts' specifies options by bitwise or.
   If successful, the return value is true, else, it is false. */
static bool tcbdboptimizeimpl(TCBDB *bdb, int32_t lmemb, int32_t nmemb,
                              int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts){
  assert(bdb);
  if(lmemb < 1) lmemb = bdb->lmemb;
  if(nmemb < 1) nmemb = bdb->nmemb;
  if(bnum < 1) bnum = tchdbrnum(bdb->hdb) * 2 + 1;
  if(opts == UINT8_MAX) opts = bdb->opts;
  const char *path = tchdbpath(bdb->hdb);
  char *tpath = tcsprintf("%s%ctmp%c%llu", path, MYEXTCHR, MYEXTCHR, tchdbinode(bdb->hdb));
  TCBDB *tbdb = tcbdbnew();
  tcbdbsetcmpfunc(tbdb, bdb->cmp, bdb->cmpop);
  tcbdbtune(tbdb, lmemb, nmemb, bnum, apow, fpow, opts);
  tcbdbsetlsmax(tbdb, bdb->lsmax);
  if(!tcbdbopen(tbdb, tpath, BDBOWRITER | BDBOCREAT | BDBOTRUNC)){
    tcbdbsetecode(bdb, tcbdbecode(tbdb), __FILE__, __LINE__, __func__);
    tcbdbdel(tbdb);
    free(tpath);
    return false;
  }
  bool err = false;
  BDBCUR *cur = tcbdbcurnew(bdb);
  tcbdbcurfirstimpl(cur);
  const char *kbuf, *vbuf;
  int ksiz, vsiz;
  while(!err && cur->id > 0 && tcbdbcurrecimpl(cur, &kbuf, &ksiz, &vbuf, &vsiz)){
    if(!tcbdbputdup(tbdb, kbuf, ksiz, vbuf, vsiz)){
      tcbdbsetecode(bdb, tcbdbecode(tbdb), __FILE__, __LINE__, __func__);
      err = true;
    }
    tcbdbcurnextimpl(cur);
  }
  tcbdbcurdel(cur);
  if(!tcbdbclose(tbdb)){
    tcbdbsetecode(bdb, tcbdbecode(tbdb), __FILE__, __LINE__, __func__);
    err = true;
  }
  tcbdbdel(tbdb);
  if(unlink(path) == -1){
    tcbdbsetecode(bdb, TCEUNLINK, __FILE__, __LINE__, __func__);
    err = true;
  }
  if(rename(tpath, path) == -1){
    tcbdbsetecode(bdb, TCERENAME, __FILE__, __LINE__, __func__);
    err = true;
  }
  free(tpath);
  if(err) return false;
  tpath = tcstrdup(path);
  int omode = (tchdbomode(bdb->hdb) & ~BDBOCREAT) & ~BDBOTRUNC;
  if(!tcbdbcloseimpl(bdb)){
    free(tpath);
    return false;
  }
  bool rv = tcbdbopenimpl(bdb, tpath, omode);
  free(tpath);
  return rv;
}


/* Remove all records of a B+ tree database object.
   `bdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbvanishimpl(TCBDB *bdb){
  assert(bdb);
  char *path = tcstrdup(tchdbpath(bdb->hdb));
  int omode = tchdbomode(bdb->hdb);
  bool err = false;
  if(!tcbdbcloseimpl(bdb)) err = true;
  if(!tcbdbopenimpl(bdb, path, BDBOTRUNC | omode)) err = true;
  free(path);
  return !err;
}


/* Lock a method of the B+ tree database object.
   `bdb' specifies the B+ tree database object.
   `wr' specifies whether the lock is writer or not.
   If successful, the return value is true, else, it is false. */
static bool tcbdblockmethod(TCBDB *bdb, bool wr){
  assert(bdb);
  if(wr ? pthread_rwlock_wrlock(bdb->mmtx) != 0 : pthread_rwlock_rdlock(bdb->mmtx) != 0){
    tcbdbsetecode(bdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock a method of the B+ tree database object.
   `hdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbunlockmethod(TCBDB *bdb){
  assert(bdb);
  if(pthread_rwlock_unlock(bdb->mmtx) != 0){
    tcbdbsetecode(bdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Lock the cache of the B+ tree database object.
   `hdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdblockcache(TCBDB *bdb){
  assert(bdb);
  if(pthread_mutex_lock(bdb->cmtx) != 0){
    tcbdbsetecode(bdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock the cache of the B+ tree database object.
   `hdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbunlockcache(TCBDB *bdb){
  assert(bdb);
  if(pthread_mutex_unlock(bdb->cmtx) != 0){
    tcbdbsetecode(bdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Lock the transaction of the B+ tree database object.
   `hdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdblocktran(TCBDB *bdb){
  assert(bdb);
  if(pthread_mutex_lock(bdb->tmtx) != 0){
    tcbdbsetecode(bdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock the transaction of the B+ tree database object.
   `hdb' specifies the B+ tree database object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbunlocktran(TCBDB *bdb){
  assert(bdb);
  if(pthread_mutex_unlock(bdb->tmtx) != 0){
    tcbdbsetecode(bdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Move a cursor object to the first record.
   `cur' specifies the cursor object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcurfirstimpl(BDBCUR *cur){
  assert(cur);
  cur->id = cur->bdb->first;
  cur->kidx = 0;
  cur->vidx = 0;
  return tcbdbcuradjust(cur, true);
}


/* Move a cursor object to the last record.
   `cur' specifies the cursor object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcurlastimpl(BDBCUR *cur){
  assert(cur);
  cur->id = cur->bdb->last;
  cur->kidx = INT_MAX;
  cur->vidx = INT_MAX;
  return tcbdbcuradjust(cur, false);
}


/* Move a cursor object to around records corresponding a key.
   `cur' specifies the cursor object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `forward' specifies whether the cursor is to be the front of records.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcurjumpimpl(BDBCUR *cur, const char *kbuf, int ksiz, bool forward){
  assert(cur && kbuf && ksiz >= 0);
  TCBDB *bdb = cur->bdb;
  uint64_t pid = tcbdbsearchleaf(bdb, kbuf, ksiz);
  if(pid < 1){
    cur->id = 0;
    cur->kidx = 0;
    cur->vidx = 0;
    return false;
  }
  BDBLEAF *leaf = tcbdbleafload(bdb, pid);
  if(!leaf){
    cur->id = 0;
    cur->kidx = 0;
    cur->vidx = 0;
    return false;
  }
  if(TCLISTNUM(leaf->recs) < 1){
    cur->id = pid;
    cur->kidx = 0;
    cur->vidx = 0;
    return forward ? tcbdbcurnextimpl(cur) : tcbdbcurprevimpl(cur);
  }
  int ri;
  BDBREC *recp = tcbdbsearchrec(bdb, leaf, kbuf, ksiz, &ri);
  if(recp){
    cur->id = pid;
    cur->kidx = ri;
    if(forward){
      cur->vidx = 0;
    } else {
      cur->vidx = recp->rest ? TCLISTNUM(recp->rest) : 0;
    }
    return true;
  }
  cur->id = leaf->id;
  if(ri > 0 && ri >= TCLISTNUM(leaf->recs)) ri = TCLISTNUM(leaf->recs) - 1;
  cur->kidx = ri;
  recp = (BDBREC *)TCLISTVALPTR(leaf->recs, ri);
  if(forward){
    int rv;
    if(bdb->cmp == tcbdbcmplexical){
      TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
    } else {
      rv = bdb->cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, bdb->cmpop);
    }
    if(rv < 0) return true;
    cur->vidx = recp->rest ? TCLISTNUM(recp->rest) : 0;
    return tcbdbcurnextimpl(cur);
  }
  int rv;
  if(bdb->cmp == tcbdbcmplexical){
    TCCMPLEXICAL(rv, kbuf, ksiz, recp->kbuf, recp->ksiz);
  } else {
    rv = bdb->cmp(kbuf, ksiz, recp->kbuf, recp->ksiz, bdb->cmpop);
  }
  if(rv > 0) return true;
  cur->vidx = 0;
  return tcbdbcurprevimpl(cur);
}


/* Adjust a cursor object forward to the suitable record.
   `cur' specifies the cursor object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcuradjust(BDBCUR *cur, bool forward){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  while(true){
    if(cur->id < 1){
      tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
      cur->id = 0;
      cur->kidx = 0;
      cur->vidx = 0;
      return false;
    }
    BDBLEAF *leaf = tcbdbleafload(bdb, cur->id);
    if(!leaf) return false;
    TCLIST *recs = leaf->recs;
    int knum = TCLISTNUM(recs);
    if(cur->kidx < 0){
      if(forward){
        cur->kidx = 0;
        cur->vidx = 0;
      } else {
        cur->id = leaf->prev;
        cur->kidx = INT_MAX;
        cur->vidx = INT_MAX;
      }
    } else if(cur->kidx >= knum){
      if(forward){
        cur->id = leaf->next;
        cur->kidx = 0;
        cur->vidx = 0;
      } else {
        cur->kidx = knum - 1;
        cur->vidx = INT_MAX;
      }
    } else {
      BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, cur->kidx);
      int vnum = recp->rest ? TCLISTNUM(recp->rest) + 1 : 1;
      if(cur->vidx < 0){
        if(forward){
          cur->vidx = 0;
        } else {
          cur->kidx--;
          cur->vidx = INT_MAX;
        }
      } else if(cur->vidx >= vnum){
        if(forward){
          cur->kidx++;
          cur->vidx = 0;
          if(cur->kidx >= knum){
            cur->id = leaf->next;
            cur->kidx = 0;
            cur->vidx = 0;
          } else {
            break;
          }
        } else {
          cur->vidx = vnum - 1;
          if(cur->vidx >= 0) break;
        }
      } else {
        break;
      }
    }
  }
  return true;
}


/* Move a cursor object to the previous record.
   `cur' specifies the cursor object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcurprevimpl(BDBCUR *cur){
  assert(cur);
  cur->vidx--;
  return tcbdbcuradjust(cur, false);
}


/* Move a cursor object to the next record.
   `cur' specifies the cursor object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcurnextimpl(BDBCUR *cur){
  assert(cur);
  cur->vidx++;
  return tcbdbcuradjust(cur, true);
}


/* Insert a record around a cursor object.
   `cur' specifies the cursor object.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   `cpmode' specifies detail adjustment.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcurputimpl(BDBCUR *cur, const char *vbuf, int vsiz, int cpmode){
  assert(cur && vbuf && vsiz >= 0);
  TCBDB *bdb = cur->bdb;
  BDBLEAF *leaf = tcbdbleafload(bdb, cur->id);
  if(!leaf) return false;
  TCLIST *recs = leaf->recs;
  if(cur->kidx >= TCLISTNUM(recs)){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, cur->kidx);
  int vnum = recp->rest ? TCLISTNUM(recp->rest) + 1 : 1;
  if(cur->vidx >= vnum){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  switch(cpmode){
  case BDBCPCURRENT:
    if(cur->vidx < 1){
      if(vsiz > recp->vsiz) TCREALLOC(recp->vbuf, recp->vbuf, vsiz + 1);
      memcpy(recp->vbuf, vbuf, vsiz);
      recp->vbuf[vsiz] = '\0';
      recp->vsiz = vsiz;
    } else {
      tclistover(recp->rest, cur->vidx - 1, vbuf, vsiz);
    }
    break;
  case BDBCPBEFORE:
    if(cur->vidx < 1){
      if(!recp->rest) recp->rest = tclistnew();
      tclistunshift(recp->rest, recp->vbuf, recp->vsiz);
      if(vsiz > recp->vsiz) TCREALLOC(recp->vbuf, recp->vbuf, vsiz + 1);
      memcpy(recp->vbuf, vbuf, vsiz);
      recp->vbuf[vsiz] = '\0';
      recp->vsiz = vsiz;
    } else {
      tclistinsert(recp->rest, cur->vidx - 1, vbuf, vsiz);
    }
    bdb->rnum++;
    break;
  case BDBCPAFTER:
    if(!recp->rest) recp->rest = tclistnew();
    tclistinsert(recp->rest, cur->vidx, vbuf, vsiz);
    cur->vidx++;
    bdb->rnum++;
    break;
  }
  leaf->dirty = true;
  return true;
}


/* Delete the record where a cursor object is.
   `cur' specifies the cursor object.
   If successful, the return value is true, else, it is false. */
static bool tcbdbcuroutimpl(BDBCUR *cur){
  assert(cur);
  TCBDB *bdb = cur->bdb;
  BDBLEAF *leaf = tcbdbleafload(bdb, cur->id);
  if(!leaf) return false;
  TCLIST *recs = leaf->recs;
  if(cur->kidx >= TCLISTNUM(recs)){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, cur->kidx);
  int vnum = recp->rest ? TCLISTNUM(recp->rest) + 1 : 1;
  if(cur->vidx >= vnum){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  if(recp->rest){
    if(cur->vidx < 1){
      free(recp->vbuf);
      recp->vbuf = tclistshift(recp->rest, &(recp->vsiz));
    } else {
      int vsiz;
      free(tclistremove(recp->rest, cur->vidx - 1, &vsiz));
    }
    if(TCLISTNUM(recp->rest) < 1){
      tclistdel(recp->rest);
      recp->rest = NULL;
    }
  } else {
    if(TCLISTNUM(recs) < 2 && bdb->hnum > 0){
      uint64_t pid = tcbdbsearchleaf(bdb, recp->kbuf, recp->ksiz);
      if(pid < 1) return false;
      if(!(leaf = tcbdbleafload(bdb, pid))) return false;
      if(!tcbdbleafkill(bdb, leaf)) return false;
    }
    free(recp->vbuf);
    free(recp->kbuf);
    int rsiz;
    free(tclistremove(leaf->recs, cur->kidx, &rsiz));
  }
  bdb->rnum--;
  leaf->dirty = true;
  return tcbdbcuradjust(cur, true) || tchdbecode(bdb->hdb) == TCENOREC;
}


/* Get the key and the value of the current record of the cursor object.
   `cur' specifies the cursor object.
   `kbp' specifies the pointer to the variable into which the pointer to the region of the key is
   assgined.
   `ksp' specifies the pointer to the variable into which the size of the key region is assigned.
   `vbp' specifies the pointer to the variable into which the pointer to the region of the value
   is assgined.
   `vsp' specifies the pointer to the variable into which the size of the value region is
   assigned. */
static bool tcbdbcurrecimpl(BDBCUR *cur, const char **kbp, int *ksp, const char **vbp, int *vsp){
  assert(cur && kbp && ksp && vbp && vsp);
  TCBDB *bdb = cur->bdb;
  BDBLEAF *leaf = tcbdbleafload(bdb, cur->id);
  if(!leaf) return false;
  TCLIST *recs = leaf->recs;
  if(cur->kidx >= TCLISTNUM(recs)){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, cur->kidx);
  int vnum = recp->rest ? TCLISTNUM(recp->rest) + 1 : 1;
  if(cur->vidx >= vnum){
    tcbdbsetecode(bdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  *kbp = recp->kbuf;
  *ksp = recp->ksiz;
  if(cur->vidx > 0){
    *vbp = tclistval(recp->rest, cur->vidx - 1, vsp);
  } else {
    *vbp = recp->vbuf;
    *vsp = recp->vsiz;
  }
  return true;
}



/*************************************************************************************************
 * debugging functions
 *************************************************************************************************/


/* Print meta data of the header into the debugging output.
   `bdb' specifies the B+ tree database object. */
void tcbdbprintmeta(TCBDB *bdb){
  assert(bdb);
  int dbgfd = tchdbdbgfd(bdb->hdb);
  if(dbgfd < 0) return;
  char buf[BDBPAGEBUFSIZ];
  char *wp = buf;
  wp += sprintf(wp, "META:");
  wp += sprintf(wp, " mmtx=%p", (void *)bdb->mmtx);
  wp += sprintf(wp, " cmtx=%p", (void *)bdb->cmtx);
  wp += sprintf(wp, " tmtx=%p", (void *)bdb->tmtx);
  wp += sprintf(wp, " hdb=%p", (void *)bdb->hdb);
  wp += sprintf(wp, " opaque=%p", (void *)bdb->opaque);
  wp += sprintf(wp, " open=%d", bdb->open);
  wp += sprintf(wp, " wmode=%d", bdb->wmode);
  wp += sprintf(wp, " lmemb=%u", bdb->lmemb);
  wp += sprintf(wp, " nmemb=%u", bdb->nmemb);
  wp += sprintf(wp, " opts=%u", bdb->opts);
  wp += sprintf(wp, " root=%llu", (unsigned long long)bdb->root);
  wp += sprintf(wp, " first=%llu", (unsigned long long)bdb->first);
  wp += sprintf(wp, " last=%llu", (unsigned long long)bdb->last);
  wp += sprintf(wp, " lnum=%llu", (unsigned long long)bdb->lnum);
  wp += sprintf(wp, " nnum=%llu", (unsigned long long)bdb->nnum);
  wp += sprintf(wp, " rnum=%llu", (unsigned long long)bdb->rnum);
  wp += sprintf(wp, " leafc=%p", (void *)bdb->leafc);
  wp += sprintf(wp, " nodec=%p", (void *)bdb->nodec);
  wp += sprintf(wp, " cmp=%p", (void *)(intptr_t)bdb->cmp);
  wp += sprintf(wp, " cmpop=%p", (void *)bdb->cmpop);
  wp += sprintf(wp, " lcnum=%u", bdb->lcnum);
  wp += sprintf(wp, " ncnum=%u", bdb->ncnum);
  wp += sprintf(wp, " lsmax=%u", bdb->lsmax);
  wp += sprintf(wp, " lschk=%u", bdb->lschk);
  wp += sprintf(wp, " capnum=%llu", (unsigned long long)bdb->capnum);
  wp += sprintf(wp, " hist=%p", (void *)bdb->hist);
  wp += sprintf(wp, " hnum=%d", bdb->hnum);
  wp += sprintf(wp, " hleaf=%llu", (unsigned long long)bdb->hleaf);
  wp += sprintf(wp, " lleaf=%llu", (unsigned long long)bdb->lleaf);
  wp += sprintf(wp, " tran=%d", bdb->tran);
  wp += sprintf(wp, " rbopaque=%p", (void *)bdb->rbopaque);
  wp += sprintf(wp, " cnt_saveleaf=%lld", (long long)bdb->cnt_saveleaf);
  wp += sprintf(wp, " cnt_loadleaf=%lld", (long long)bdb->cnt_loadleaf);
  wp += sprintf(wp, " cnt_killleaf=%lld", (long long)bdb->cnt_killleaf);
  wp += sprintf(wp, " cnt_adjleafc=%lld", (long long)bdb->cnt_adjleafc);
  wp += sprintf(wp, " cnt_savenode=%lld", (long long)bdb->cnt_savenode);
  wp += sprintf(wp, " cnt_loadnode=%lld", (long long)bdb->cnt_loadnode);
  wp += sprintf(wp, " cnt_adjnodec=%lld", (long long)bdb->cnt_adjnodec);
  *(wp++) = '\n';
  tcwrite(dbgfd, buf, wp - buf);
}


/* Print records of a leaf object into the debugging output.
   `bdb' specifies the B+ tree database object.
   `leaf' specifies the leaf object. */
void tcbdbprintleaf(TCBDB *bdb, BDBLEAF *leaf){
  assert(bdb && leaf);
  int dbgfd = tchdbdbgfd(bdb->hdb);
  TCLIST *recs = leaf->recs;
  if(dbgfd < 0) return;
  char buf[BDBPAGEBUFSIZ];
  char *wp = buf;
  wp += sprintf(wp, "LEAF:");
  wp += sprintf(wp, " id:%llx", (unsigned long long)leaf->id);
  wp += sprintf(wp, " prev:%llx", (unsigned long long)leaf->prev);
  wp += sprintf(wp, " next:%llx", (unsigned long long)leaf->next);
  wp += sprintf(wp, " dirty:%d", leaf->dirty);
  wp += sprintf(wp, " dead:%d", leaf->dead);
  wp += sprintf(wp, " rnum:%d", TCLISTNUM(recs));
  *(wp++) = ' ';
  for(int i = 0; i < TCLISTNUM(recs); i++){
    tcwrite(dbgfd, buf, wp - buf);
    wp = buf;
    BDBREC *recp = (BDBREC *)TCLISTVALPTR(recs, i);
    wp += sprintf(wp, " [%s:%s]", recp->kbuf, recp->vbuf);
    TCLIST *rest = recp->rest;
    if(rest){
      for(int j = 0; j < TCLISTNUM(rest); j++){
        wp += sprintf(wp, ":%s", (char *)TCLISTVALPTR(rest, j));
      }
    }
  }
  *(wp++) = '\n';
  tcwrite(dbgfd, buf, wp - buf);
}


/* Print indexes of a node object into the debugging output.
   `bdb' specifies the B+ tree database object.
   `node' specifies the node object. */
void tcbdbprintnode(TCBDB *bdb, BDBNODE *node){
  assert(bdb && node);
  int dbgfd = tchdbdbgfd(bdb->hdb);
  TCLIST *idxs = node->idxs;
  if(dbgfd < 0) return;
  char buf[BDBPAGEBUFSIZ];
  char *wp = buf;
  wp += sprintf(wp, "NODE:");
  wp += sprintf(wp, " id:%llx", (unsigned long long)node->id);
  wp += sprintf(wp, " heir:%llx", (unsigned long long)node->heir);
  wp += sprintf(wp, " dirty:%d", node->dirty);
  wp += sprintf(wp, " rnum:%d", TCLISTNUM(idxs));
  *(wp++) = ' ';
  for(int i = 0; i < TCLISTNUM(idxs); i++){
    tcwrite(dbgfd, buf, wp - buf);
    wp = buf;
    BDBIDX *idxp = (BDBIDX *)TCLISTVALPTR(idxs, i);
    wp += sprintf(wp, " [%llx:%s]", (unsigned long long)idxp->pid, idxp->kbuf);
  }
  *(wp++) = '\n';
  tcwrite(dbgfd, buf, wp - buf);
}



// END OF FILE
