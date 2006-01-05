/*
 *   Drive reservation functions for Storage Daemon
 *
 *   Kern Sibbald, MM
 *
 *   Split from job.c and acquire.c June 2005
 *
 *   Version $Id$
 *
 */
/*
   Copyright (C) 2000-2006 Kern Sibbald

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   version 2 as amended with additional clauses defined in the
   file LICENSE in the main source directory.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
   the file LICENSE for additional details.

 */

#include "bacula.h"
#include "stored.h"

/*
 *   Use Device command from Director
 *   He tells is what Device Name to use, the Media Type,
 *      the Pool Name, and the Pool Type.
 *
 *    Ensure that the device exists and is opened, then store
 *      the media and pool info in the JCR.  This class is used
 *      only temporarily in this file.
 */
class DIRSTORE {
public:
   alist *device;
   bool append;
   char name[MAX_NAME_LENGTH];
   char media_type[MAX_NAME_LENGTH];
   char pool_name[MAX_NAME_LENGTH];
   char pool_type[MAX_NAME_LENGTH];
};

/* Reserve context */
class RCTX {
public:
   JCR *jcr;
   char *device_name;
   DIRSTORE *store;
   DEVRES   *device;
   DEVICE *low_use_drive;             /* Low use drive candidate */
   int num_writers;                   /* for selecting low use drive */
   bool try_low_use_drive;            /* see if low use drive available */
   bool any_drive;                    /* Accept any drive if set */
   bool PreferMountedVols;            /* Prefer volumes already mounted */
   bool exact_match;                  /* Want exact volume */
   bool have_volume;                  /* Have DIR suggested vol name */
   bool suitable_device;              /* at least one device is suitable */
   bool autochanger_only;             /* look at autochangers only */
   char VolumeName[MAX_NAME_LENGTH];  /* Vol name suggested by DIR */
};

static dlist *vol_list = NULL;
static pthread_mutex_t vol_list_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t search_lock = PTHREAD_MUTEX_INITIALIZER;

/* Forward referenced functions */
static int can_reserve_drive(DCR *dcr, RCTX &rctx);
static int search_res_for_device(RCTX &rctx);
static int reserve_device(RCTX &rctx);
static bool reserve_device_for_read(DCR *dcr);
static bool reserve_device_for_append(DCR *dcr, RCTX &rctx);
static bool use_storage_cmd(JCR *jcr);
bool find_suitable_device_for_job(JCR *jcr, RCTX &rctx);

/* Requests from the Director daemon */
static char use_storage[]  = "use storage=%127s media_type=%127s "
   "pool_name=%127s pool_type=%127s append=%d copy=%d stripe=%d\n";
static char use_device[]  = "use device=%127s\n";

/* Responses sent to Director daemon */
static char OK_device[] = "3000 OK use device device=%s\n";
static char NO_device[] = "3924 Device \"%s\" not in SD Device resources.\n";
static char BAD_use[]   = "3913 Bad use command: %s\n";

bool use_cmd(JCR *jcr) 
{
   /*
    * Get the device, media, and pool information
    */
   if (!use_storage_cmd(jcr)) {
      set_jcr_job_status(jcr, JS_ErrorTerminated);
      memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));
      return false;
   }
   return true;
}

static int my_compare(void *item1, void *item2)
{
   return strcmp(((VOLRES *)item1)->vol_name, ((VOLRES *)item2)->vol_name);
}


/*
 * Put a new Volume entry in the Volume list. This
 *  effectively reserves the volume so that it will
 *  not be mounted again.
 *
 *  Return: VOLRES entry on success
 *          NULL if the Volume is already in the list
 */
VOLRES *new_volume(DCR *dcr, const char *VolumeName)
{
   VOLRES *vol, *nvol;

   Dmsg1(400, "new_volume %s\n", VolumeName);
   P(vol_list_lock);
   if (dcr->dev) {
again:
      foreach_dlist(vol, vol_list) {
         if (vol && vol->dev == dcr->dev) {
            vol_list->remove(vol);
            if (vol->vol_name) {
               free(vol->vol_name);
            }
            free(vol);
            goto again;
         }
      }
   }
   vol = (VOLRES *)malloc(sizeof(VOLRES));
   memset(vol, 0, sizeof(VOLRES));
   vol->vol_name = bstrdup(VolumeName);
   vol->dev = dcr->dev;
   vol->dcr = dcr;
   nvol = (VOLRES *)vol_list->binary_insert(vol, my_compare);
   if (nvol != vol) {
      free(vol->vol_name);
      free(vol);
      vol = NULL;
      if (dcr->dev) {
         nvol->dev = dcr->dev;
      }
   }
   V(vol_list_lock);
   return vol;
}

/*
 * Search for a Volume name in the Volume list.
 *
 *  Returns: VOLRES entry on success
 *           NULL if the Volume is not in the list
 */
VOLRES *find_volume(const char *VolumeName)
{
   VOLRES vol, *fvol;
   P(vol_list_lock);
   vol.vol_name = bstrdup(VolumeName);
   fvol = (VOLRES *)vol_list->binary_search(&vol, my_compare);
   free(vol.vol_name);
   V(vol_list_lock);
   return fvol;
}

/*  
 * Free a Volume from the Volume list
 *
 *  Returns: true if the Volume found and removed from the list
 *           false if the Volume is not in the list
 */
bool free_volume(DEVICE *dev)
{
   VOLRES vol, *fvol;

  P(vol_list_lock);
   if (dev->VolHdr.VolumeName[0] == 0) {
      /*
       * Our device has no VolumeName listed, but
       *  search the list for any Volume attached to
       *  this device and remove it.
       */
      foreach_dlist(fvol, vol_list) {
         if (fvol && fvol->dev == dev) {
            vol_list->remove(fvol);
            if (fvol->vol_name) {
               free(fvol->vol_name);
            }
            free(fvol);
            break;
         }
      }
      goto bail_out;
   }
   Dmsg1(400, "free_volume %s\n", dev->VolHdr.VolumeName);
   vol.vol_name = bstrdup(dev->VolHdr.VolumeName);
   fvol = (VOLRES *)vol_list->binary_search(&vol, my_compare);
   if (fvol) {
      vol_list->remove(fvol);
      free(fvol->vol_name);
      free(fvol);
   }
   free(vol.vol_name);
   dev->VolHdr.VolumeName[0] = 0;
bail_out:
   V(vol_list_lock);
   return fvol != NULL;
}

/* Free volume reserved by this dcr but not attached to a dev */
void free_unused_volume(DCR *dcr)
{
   VOLRES *vol;
   P(vol_list_lock);
   for (vol=(VOLRES *)vol_list->first(); vol; vol=(VOLRES *)vol_list->next(vol)) {
      if (vol->dcr == dcr && (vol->dev == NULL || 
          strcmp(vol->vol_name, vol->dev->VolHdr.VolumeName) != 0)) {
         vol_list->remove(vol);
         free(vol->vol_name);
         free(vol);
         break;
      }
   }
   V(vol_list_lock);
}

/*
 * List Volumes -- this should be moved to status.c
 */
void list_volumes(BSOCK *user)  
{
   VOLRES *vol;
   for (vol=(VOLRES *)vol_list->first(); vol; vol=(VOLRES *)vol_list->next(vol)) {
      if (vol->dev) {
         bnet_fsend(user, "%s on device %s\n", vol->vol_name, vol->dev->print_name());
      } else {
         bnet_fsend(user, "%s\n", vol->vol_name);
      }
   }
}
      
/* Create the Volume list */
void create_volume_list()
{
   VOLRES *dummy = NULL;
   if (vol_list == NULL) {
      vol_list = New(dlist(dummy, &dummy->link));
   }
}

/* Release all Volumes from the list */
void free_volume_list()
{
   VOLRES *vol;
   if (!vol_list) {
      return;
   }
   for (vol=(VOLRES *)vol_list->first(); vol; vol=(VOLRES *)vol_list->next(vol)) {
      Dmsg3(000, "Unreleased Volume=%s dcr=0x%x dev=0x%x\n", vol->vol_name,
         vol->dcr, vol->dev);
   }
   delete vol_list;
   vol_list = NULL;
}

bool is_volume_in_use(DCR *dcr)
{
   VOLRES *vol = find_volume(dcr->VolumeName);
   if (!vol) {
      return false;                   /* vol not in list */
   }
   if (!vol->dev) {                   /* vol not attached to device */
      return false;
   }
   if (dcr->dev == vol->dev) {        /* same device OK */
      return false;
   }
   if (!vol->dev->is_busy()) {
      return false;
   }
   return true;
}


/*
 * We get the following type of information:
 *
 * use storage=xxx media_type=yyy pool_name=xxx pool_type=yyy append=1 copy=0 strip=0
 *  use device=zzz
 *  use device=aaa
 *  use device=bbb
 * use storage=xxx media_type=yyy pool_name=xxx pool_type=yyy append=0 copy=0 strip=0
 *  use device=bbb
 *
 */
static bool use_storage_cmd(JCR *jcr)
{
   POOL_MEM store_name, dev_name, media_type, pool_name, pool_type;
   BSOCK *dir = jcr->dir_bsock;
   int append;
   bool ok;       
   int Copy, Stripe;
   DIRSTORE *store;
   RCTX rctx;

   memset(&rctx, 0, sizeof(RCTX));
   rctx.jcr = jcr;
   /*
    * If there are multiple devices, the director sends us
    *   use_device for each device that it wants to use.
    */
   jcr->dirstore = New(alist(10, not_owned_by_alist));
   do {
      Dmsg1(100, "<dird: %s", dir->msg);
      ok = sscanf(dir->msg, use_storage, store_name.c_str(), 
                  media_type.c_str(), pool_name.c_str(), 
                  pool_type.c_str(), &append, &Copy, &Stripe) == 7;
      if (!ok) {
         break;
      }
      unbash_spaces(store_name);
      unbash_spaces(media_type);
      unbash_spaces(pool_name);
      unbash_spaces(pool_type);
      store = new DIRSTORE;
      jcr->dirstore->append(store);
      memset(store, 0, sizeof(DIRSTORE));
      store->device = New(alist(10));
      bstrncpy(store->name, store_name, sizeof(store->name));
      bstrncpy(store->media_type, media_type, sizeof(store->media_type));
      bstrncpy(store->pool_name, pool_name, sizeof(store->pool_name));
      bstrncpy(store->pool_type, pool_type, sizeof(store->pool_type));
      store->append = append;

      /* Now get all devices */
      while (bnet_recv(dir) >= 0) {
         Dmsg1(100, "<dird device: %s", dir->msg);
         ok = sscanf(dir->msg, use_device, dev_name.c_str()) == 1;
         if (!ok) {
            break;
         }
         unbash_spaces(dev_name);
         store->device->append(bstrdup(dev_name.c_str()));
      }
   }  while (ok && bnet_recv(dir) >= 0);

#ifdef DEVELOPER
   /* This loop is debug code and can be removed */
   /* ***FIXME**** remove after 1.38 release */
   char *device_name;
   foreach_alist(store, jcr->dirstore) {
      Dmsg4(100, "Storage=%s media_type=%s pool=%s pool_type=%s\n", 
         store->name, store->media_type, store->pool_name, 
         store->pool_type);
      foreach_alist(device_name, store->device) {
         Dmsg1(100, "   Device=%s\n", device_name);
      }
   }
#endif

   /*                    
    * At this point, we have a list of all the Director's Storage
    *  resources indicated for this Job, which include Pool, PoolType,
    *  storage name, and Media type.     
    * Then for each of the Storage resources, we have a list of
    *  device names that were given.
    *
    * Wiffle through them and find one that can do the backup.
    */
   if (ok) {
      bool first = true;           /* print wait message once */
      for ( ; !job_canceled(jcr); ) {
         rctx.suitable_device = false;
         rctx.have_volume = false;
         rctx.any_drive = false;
         if (!jcr->PreferMountedVols) {
            /* Look for unused drives in autochangers */
            rctx.num_writers = 20000000;   /* start with impossible number */
            rctx.low_use_drive = NULL;
            rctx.PreferMountedVols = false;                
            rctx.exact_match = false;
            rctx.autochanger_only = true;
            Dmsg5(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
               rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
               rctx.autochanger_only, rctx.any_drive);
            if ((ok = find_suitable_device_for_job(jcr, rctx))) {
               goto done;
            }
            /* Look through all drives possibly for low_use drive */
            if (rctx.low_use_drive) {
               rctx.try_low_use_drive = true;
               if ((ok = find_suitable_device_for_job(jcr, rctx))) {
                  goto done;
               }
               rctx.try_low_use_drive = false;
            }
            rctx.autochanger_only = false;
            Dmsg5(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
               rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
               rctx.autochanger_only, rctx.any_drive);
            if ((ok = find_suitable_device_for_job(jcr, rctx))) {
               goto done;
            }
         }
         /* Look for an exact match all drives */
         rctx.PreferMountedVols = true;
         rctx.exact_match = true;
         rctx.autochanger_only = false;
         Dmsg5(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
            rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
            rctx.autochanger_only, rctx.any_drive);
         if ((ok = find_suitable_device_for_job(jcr, rctx))) {
            goto done;
         }
         /* Look for any mounted drive */
         rctx.exact_match = false;
         Dmsg5(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
            rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
            rctx.autochanger_only, rctx.any_drive);
         if ((ok = find_suitable_device_for_job(jcr, rctx))) {
            goto done;
         }
         /* Try any drive */
         rctx.any_drive = true;
         Dmsg5(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
            rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
            rctx.autochanger_only, rctx.any_drive);
         if ((ok = find_suitable_device_for_job(jcr, rctx))) {
            goto done;
         }
         if (!rctx.suitable_device || !wait_for_device(jcr, first)) {
            break;       /* Get out, failure ... */
         }   
         first = false;
         bnet_sig(dir, BNET_HEARTBEAT);  /* Inform Dir that we are alive */
      }

      /*
       * If we get here, there are no suitable devices available, which
       *  means nothing configured.  If a device is suitable but busy
       *  with another Volume, we will not come here.
       */
      if (verbose) {
         unbash_spaces(dir->msg);
         pm_strcpy(jcr->errmsg, dir->msg);
         Jmsg(jcr, M_INFO, 0, _("Failed command: %s\n"), jcr->errmsg);
      }
      Jmsg(jcr, M_FATAL, 0, _("\n"
         "     Device \"%s\" with MediaType \"%s\" requested by DIR not found in SD Device resources.\n"),
           dev_name.c_str(), media_type.c_str());
      bnet_fsend(dir, NO_device, dev_name.c_str());

      Dmsg1(100, ">dird: %s", dir->msg);
   } else {
      unbash_spaces(dir->msg);
      pm_strcpy(jcr->errmsg, dir->msg);
      if (verbose) {
         Jmsg(jcr, M_INFO, 0, _("Failed command: %s\n"), jcr->errmsg);
      }
      bnet_fsend(dir, BAD_use, jcr->errmsg);
      Dmsg1(100, ">dird: %s", dir->msg);
   }

done:
   foreach_alist(store, jcr->dirstore) {
      delete store->device;
      delete store;
   }
   delete jcr->dirstore;
#ifdef implemented
   while (error=(char*)rctx->errors.first()) {
      free(error);
   }
#endif
   return ok;
}


/*
 * Search for a device suitable for this job.
 */
bool find_suitable_device_for_job(JCR *jcr, RCTX &rctx)
{
   bool ok;
   DIRSTORE *store;
   char *device_name;

   /* 
    * For each storage device that the user specified, we
    *  search and see if there is a resource for that device.
    */
   Dmsg4(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d\n",
      rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
      rctx.autochanger_only);
   init_jcr_device_wait_timers(jcr);
   ok = false;
   P(search_lock);
   foreach_alist(store, jcr->dirstore) {
      rctx.store = store;
      foreach_alist(device_name, store->device) {
         int stat;
         rctx.device_name = device_name;
         stat = search_res_for_device(rctx); 
         if (stat == 1) {             /* found available device */
            Dmsg1(100, "Suitable device found=%s\n", device_name);
            ok = true;
            break;
         } else if (stat == 0) {      /* device busy */
            Dmsg1(100, "Suitable busy device found=%s\n", device_name);
         } else {
            /* otherwise error */
            Dmsg0(100, "No suitable device found.\n");
         }
      }
      if (ok) {
         break;
      }
   }
   V(search_lock);

   return ok;
}

/*
 * Search for a particular storage device with particular storage
 *  characteristics (MediaType).
 */
static int search_res_for_device(RCTX &rctx) 
{
   AUTOCHANGER *changer;
   BSOCK *dir = rctx.jcr->dir_bsock;
   bool ok;
   int stat;

   Dmsg1(100, "Search res for %s\n", rctx.device_name);
   /* Look through Autochangers first */
   foreach_res(changer, R_AUTOCHANGER) {
      Dmsg1(150, "Try match changer res=%s\n", changer->hdr.name);
      /* Find resource, and make sure we were able to open it */
      if (fnmatch(rctx.device_name, changer->hdr.name, 0) == 0) {
         /* Try each device in this AutoChanger */
         foreach_alist(rctx.device, changer->device) {
            Dmsg1(100, "Try changer device %s\n", rctx.device->hdr.name);
            stat = reserve_device(rctx);
            if (stat != 1) {             /* try another device */
               continue;
            }
            POOL_MEM dev_name;
            if (rctx.store->append == SD_APPEND) {
               Dmsg2(100, "Device %s reserved=%d.\n", rctx.device_name,
                  rctx.jcr->dcr->dev->reserved_device);
            } else {
               Dmsg2(100, "Device %s reserved=%d.\n", rctx.device_name,
                  rctx.jcr->read_dcr->dev->reserved_device);
            }
            pm_strcpy(dev_name, rctx.device->hdr.name);
            bash_spaces(dev_name);
            ok = bnet_fsend(dir, OK_device, dev_name.c_str());  /* Return real device name */
            Dmsg1(100, ">dird changer: %s", dir->msg);
            return ok ? 1 : -1;
         }
      }
   }

   /* Now if requested look through regular devices */
   if (!rctx.autochanger_only) {
      foreach_res(rctx.device, R_DEVICE) {
         Dmsg1(150, "Try match res=%s\n", rctx.device->hdr.name);
         /* Find resource, and make sure we were able to open it */
         if (fnmatch(rctx.device_name, rctx.device->hdr.name, 0) == 0) {
            stat = reserve_device(rctx);
            if (stat != 1) {
               return stat;
            }
            Dmsg1(220, "Got: %s", dir->msg);
            bash_spaces(rctx.device_name);
            ok = bnet_fsend(dir, OK_device, rctx.device_name);
            Dmsg1(100, ">dird dev: %s", dir->msg);
            return ok ? 1 : -1;
         }
      }
   }
   return -1;                    /* nothing found */
}

/*
 *  Try to reserve a specific device.
 *
 *  Returns: 1 -- OK, have DCR
 *           0 -- must wait
 *          -1 -- fatal error
 */
static int reserve_device(RCTX &rctx)
{
   bool ok;
   DCR *dcr;
   const int name_len = MAX_NAME_LENGTH;

   /* Make sure MediaType is OK */
   if (strcmp(rctx.device->media_type, rctx.store->media_type) != 0) {
      return -1;
   }

   /* Make sure device exists -- i.e. we can stat() it */
   if (!rctx.device->dev) {
      rctx.device->dev = init_dev(rctx.jcr, rctx.device);
   }
   if (!rctx.device->dev) {
      if (rctx.device->changer_res) {
        Jmsg(rctx.jcr, M_WARNING, 0, _("\n"
           "     Device \"%s\" in changer \"%s\" requested by DIR could not be opened or does not exist.\n"),
             rctx.device->hdr.name, rctx.device_name);
      } else {
         Jmsg(rctx.jcr, M_WARNING, 0, _("\n"
            "     Device \"%s\" requested by DIR could not be opened or does not exist.\n"),
              rctx.device_name);
      }
      return -1;  /* no use waiting */
   }  

   rctx.suitable_device = true;
   Dmsg2(100, "Try reserve %s jobid=%d\n", rctx.device->hdr.name,
         rctx.jcr->JobId);
   dcr = new_dcr(rctx.jcr, rctx.device->dev);
   if (!dcr) {
      BSOCK *dir = rctx.jcr->dir_bsock;
      bnet_fsend(dir, _("3926 Could not get dcr for device: %s\n"), rctx.device_name);
      Dmsg1(100, ">dird: %s", dir->msg);
      return -1;
   }
   bstrncpy(dcr->pool_name, rctx.store->pool_name, name_len);
   bstrncpy(dcr->pool_type, rctx.store->pool_type, name_len);
   bstrncpy(dcr->media_type, rctx.store->media_type, name_len);
   bstrncpy(dcr->dev_name, rctx.device_name, name_len);
   if (rctx.store->append == SD_APPEND) {
      if (rctx.exact_match && !rctx.have_volume) {
         dcr->any_volume = true;
         if (dir_find_next_appendable_volume(dcr)) {
            Dmsg1(100, "Looking for Volume=%s\n", dcr->VolumeName);
            bstrncpy(rctx.VolumeName, dcr->VolumeName, sizeof(rctx.VolumeName));
            rctx.have_volume = true;
         } else {
            Dmsg0(100, "No next volume found\n");
            rctx.VolumeName[0] = 0;
        }
      }
      ok = reserve_device_for_append(dcr, rctx);
      if (ok) {
         rctx.jcr->dcr = dcr;
         Dmsg5(100, "Reserved=%d dev_name=%s mediatype=%s pool=%s ok=%d\n",
               dcr->dev->reserved_device,
               dcr->dev_name, dcr->media_type, dcr->pool_name, ok);
      }
   } else {
      ok = reserve_device_for_read(dcr);
      if (ok) {
         rctx.jcr->read_dcr = dcr;
         Dmsg5(100, "Read reserved=%d dev_name=%s mediatype=%s pool=%s ok=%d\n",
               dcr->dev->reserved_device,
               dcr->dev_name, dcr->media_type, dcr->pool_name, ok);
      }
   }
   if (!ok) {
      free_dcr(dcr);
      Dmsg0(100, "Not OK.\n");
      return 0;
   }
   return 1;
}

/*
 * We "reserve" the drive by setting the ST_READ bit. No one else
 *  should touch the drive until that is cleared.
 *  This allows the DIR to "reserve" the device before actually
 *  starting the job. 
 */
static bool reserve_device_for_read(DCR *dcr)
{
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;
   bool ok = false;

   ASSERT(dcr);

   P(dev->mutex);

   if (is_device_unmounted(dev)) {             
      Dmsg1(200, "Device %s is BLOCKED due to user unmount.\n", dev->print_name());
      Mmsg(jcr->errmsg, _("Device %s is BLOCKED due to user unmount.\n"),
           dev->print_name());
      goto bail_out;
   }

   if (dev->is_busy()) {
      Dmsg4(200, "Device %s is busy ST_READ=%d num_writers=%d reserved=%d.\n", dev->print_name(),
         dev->state & ST_READ?1:0, dev->num_writers, dev->reserved_device);
      Mmsg1(jcr->errmsg, _("Device %s is busy.\n"),
            dev->print_name());
      goto bail_out;
   }

   dev->clear_append();
   dev->set_read();
   ok = true;

bail_out:
   V(dev->mutex);
   return ok;
}


/*
 * We reserve the device for appending by incrementing the 
 *  reserved_device. We do virtually all the same work that
 *  is done in acquire_device_for_append(), but we do
 *  not attempt to mount the device. This routine allows
 *  the DIR to reserve multiple devices before *really* 
 *  starting the job. It also permits the SD to refuse 
 *  certain devices (not up, ...).
 *
 * Note, in reserving a device, if the device is for the
 *  same pool and the same pool type, then it is acceptable.
 *  The Media Type has already been checked. If we are
 *  the first tor reserve the device, we put the pool
 *  name and pool type in the device record.
 */
static bool reserve_device_for_append(DCR *dcr, RCTX &rctx)
{
   JCR *jcr = dcr->jcr;
   DEVICE *dev = dcr->dev;
   bool ok = false;

   ASSERT(dcr);

   P(dev->mutex);

   /* If device is being read, we cannot write it */
   if (dev->can_read()) {
      Mmsg1(jcr->errmsg, _("Device %s is busy reading.\n"), dev->print_name());
      Dmsg1(100, "%s", jcr->errmsg);
      goto bail_out;
   }

   /* If device is unmounted, we are out of luck */
   if (is_device_unmounted(dev)) {
      Mmsg(jcr->errmsg, _("Device %s is BLOCKED due to user unmount.\n"), dev->print_name());
      Dmsg1(100, "%s", jcr->errmsg);
      goto bail_out;
   }

   Dmsg1(100, "reserve_append device is %s\n", dev->is_tape()?"tape":"disk");

   /* Now do detailed tests ... */
   if (can_reserve_drive(dcr, rctx) != 1) {
      Dmsg0(100, "can_reserve_drive!=1\n");
      goto bail_out;
   }

   dev->reserved_device++;
   Dmsg3(100, "Inc reserve=%d dev=%s %p\n", dev->reserved_device, 
      dev->print_name(), dev);
   dcr->reserved_device = true;
   ok = true;

bail_out:
   V(dev->mutex);
   return ok;
}

/*
 * Returns: 1 if drive can be reserved
 *          0 if we should wait
 *         -1 on error or impossibility
 */
static int can_reserve_drive(DCR *dcr, RCTX &rctx) 
{
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;

   Dmsg5(100, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
         rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
         rctx.autochanger_only, rctx.any_drive);

   /* setting any_drive overrides PreferMountedVols flag */
   if (!rctx.any_drive) {
      /*
       * When PreferMountedVols is set, we keep track of the 
       *  drive in use that has the least number of writers, then if
       *  no unmounted drive is found, we try that drive. This   
       *  helps spread the load to the least used drives.  
       */
      if (rctx.try_low_use_drive && dev == rctx.low_use_drive) {
         Dmsg3(100, "OK dev=%s == low_drive=%s. JobId=%d\n",
            dev->print_name(), rctx.low_use_drive->print_name(), jcr->JobId);
         return 1;
      }
      /* If he wants a free drive, but this one is busy, no go */
      if (!rctx.PreferMountedVols && dev->is_busy()) {
         /* Save least used drive */
         if ((dev->num_writers + dev->reserved_device) < rctx.num_writers) {
            rctx.num_writers = dev->num_writers + dev->reserved_device;
            rctx.low_use_drive = dev;
            Dmsg2(100, "set low use drive=%s num_writers=%d\n", dev->print_name(),
               rctx.num_writers);
         } else {
            Dmsg1(100, "not low use num_writers=%d\n", dev->num_writers+ 
               dev->reserved_device);
         }
         Dmsg1(100, "failed: !prefMnt && busy. JobId=%d\n", jcr->JobId);
         return 0;
      }

      /* Check for prefer mounted volumes */
      if (rctx.PreferMountedVols && !dev->VolHdr.VolumeName[0] && dev->is_tape()) {
         Dmsg1(100, "failed: want mounted -- no vol JobId=%d\n", jcr->JobId);
         return 0;                 /* No volume mounted */
      }

      /* Check for exact Volume name match */
      if (rctx.exact_match && rctx.have_volume &&
          strcmp(dev->VolHdr.VolumeName, rctx.VolumeName) != 0) {
         Dmsg2(100, "failed: Not exact match have=%s want=%s\n",
               dev->VolHdr.VolumeName, rctx.VolumeName);
         return 0;
      }
   }

   /* Check for unused autochanger drive */
   if (rctx.autochanger_only && dev->num_writers == 0 &&
       dev->VolHdr.VolumeName[0] == 0) {
      /* Device is available but not yet reserved, reserve it for us */
      Dmsg2(100, "OK Res Unused autochanger %s JobId=%d.\n",
         dev->print_name(), jcr->JobId);
      bstrncpy(dev->pool_name, dcr->pool_name, sizeof(dev->pool_name));
      bstrncpy(dev->pool_type, dcr->pool_type, sizeof(dev->pool_type));
      return 1;                       /* reserve drive */
   }

   /*
    * Handle the case that there are no writers
    */
   if (dev->num_writers == 0) {
      /* Now check if there are any reservations on the drive */
      if (dev->reserved_device) {           
         /* Now check if we want the same Pool and pool type */
         if (strcmp(dev->pool_name, dcr->pool_name) == 0 &&
             strcmp(dev->pool_type, dcr->pool_type) == 0) {
            /* OK, compatible device */
            Dmsg2(100, "OK dev: %s num_writers=0, reserved, pool matches JobId=%d\n",
               dev->print_name(), jcr->JobId);
            return 1;
         } else {
            /* Drive not suitable for us */
            Dmsg2(100, "failed: busy num_writers=0, reserved, pool=%s wanted=%s\n",
               dev->pool_name, dcr->pool_name);
            return 0;                 /* wait */
         }
      } else if (dev->can_append()) {
         /* Device in append mode, check if changing pool */
         if (strcmp(dev->pool_name, dcr->pool_name) == 0 &&
             strcmp(dev->pool_type, dcr->pool_type) == 0) {
            Dmsg2(100, "OK dev: %s num_writers=0, can_append, pool matches. JobId=%d\n",
               dev->print_name(), jcr->JobId);
            /* OK, compatible device */
            return 1;
         } else {
            /* Changing pool, unload old tape if any in drive */
            Dmsg0(100, "OK dev: num_writers=0, not reserved, pool change, unload changer\n");
            unload_autochanger(dcr, 0);
         }
      }
      /* Device is available but not yet reserved, reserve it for us */
      Dmsg2(100, "OK Dev avail reserved %s JobId=%d\n", dev->print_name(),
         jcr->JobId);
      bstrncpy(dev->pool_name, dcr->pool_name, sizeof(dev->pool_name));
      bstrncpy(dev->pool_type, dcr->pool_type, sizeof(dev->pool_type));
      return 1;                       /* reserve drive */
   }

   /*
    * Check if the device is in append mode with writers (i.e.
    *  available if pool is the same).
    */
   if (dev->can_append() || dev->num_writers > 0) {
      /* Yes, now check if we want the same Pool and pool type */
      if (strcmp(dev->pool_name, dcr->pool_name) == 0 &&
          strcmp(dev->pool_type, dcr->pool_type) == 0) {
         Dmsg2(100, "OK dev: %s num_writers>=0, can_append, pool matches. JobId=%d\n",
            dev->print_name(), jcr->JobId);
         /* OK, compatible device */
         return 1;
      } else {
         /* Drive not suitable for us */
         Dmsg2(100, "failed: busy num_writers>0, can_append, pool=%s wanted=%s\n",
            dev->pool_name, dcr->pool_name);
         return 0;                    /* wait */
      }
   } else {
      Pmsg0(000, _("Logic error!!!! Should not get here.\n"));
      Jmsg0(jcr, M_FATAL, 0, _("Logic error!!!! Should not get here.\n"));
      return -1;                      /* error, should not get here */
   }
   Dmsg2(100, "failed: No reserve %s JobId=%d\n", dev->print_name(), jcr->JobId);
   return 0;
}
