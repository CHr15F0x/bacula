/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2008 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of John Walker.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
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

#include "bacula.h"
#include "stored.h"

const int dbglvl =  50;

static dlist *vol_list = NULL;
static brwlock_t reservation_lock;
static brwlock_t vol_list_lock;

/* Forward referenced functions */
static int can_reserve_drive(DCR *dcr, RCTX &rctx);
static int reserve_device(RCTX &rctx);
static bool reserve_device_for_read(DCR *dcr);
static bool reserve_device_for_append(DCR *dcr, RCTX &rctx);
static bool use_storage_cmd(JCR *jcr);
static void queue_reserve_message(JCR *jcr);
static void pop_reserve_messages(JCR *jcr);
void switch_device(DCR *dcr, DEVICE *dev);

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
 * This allows a given thread to recursively call lock_reservations.
 *   It must, of course, call unlock_... the same number of times.
 */
void init_reservations_lock()
{
   int errstat;
   if ((errstat=rwl_init(&reservation_lock)) != 0) {
      berrno be;
      Emsg1(M_ABORT, 0, _("Unable to initialize reservation lock. ERR=%s\n"),
            be.bstrerror(errstat));
   }

   if ((errstat=rwl_init(&vol_list_lock)) != 0) {
      berrno be;
      Emsg1(M_ABORT, 0, _("Unable to initialize volume list lock. ERR=%s\n"),
            be.bstrerror(errstat));
   }
}

void term_reservations_lock()
{
   rwl_destroy(&reservation_lock);
   rwl_destroy(&vol_list_lock);
}

int reservations_lock_count = 0;

/* This applies to a drive and to Volumes */
void _lock_reservations()
{
   int errstat;
   reservations_lock_count++;
   if ((errstat=rwl_writelock(&reservation_lock)) != 0) {
      berrno be;
      Emsg2(M_ABORT, 0, "rwl_writelock failure. stat=%d: ERR=%s\n",
           errstat, be.bstrerror(errstat));
   }
}

void _unlock_reservations()
{
   int errstat;
   reservations_lock_count--;
   if ((errstat=rwl_writeunlock(&reservation_lock)) != 0) {
      berrno be;
      Emsg2(M_ABORT, 0, "rwl_writeunlock failure. stat=%d: ERR=%s\n",
           errstat, be.bstrerror(errstat));
   }
}

int vol_list_lock_count = 0;

/* 
 * This allows a given thread to recursively call to lock_volumes()
 */
void _lock_volumes()
{
   int errstat;
   vol_list_lock_count++;
   if ((errstat=rwl_writelock(&vol_list_lock)) != 0) {
      berrno be;
      Emsg2(M_ABORT, 0, "rwl_writelock failure. stat=%d: ERR=%s\n",
           errstat, be.bstrerror(errstat));
   }
}

void _unlock_volumes()
{
   int errstat;
   vol_list_lock_count--;
   if ((errstat=rwl_writeunlock(&vol_list_lock)) != 0) {
      berrno be;
      Emsg2(M_ABORT, 0, "rwl_writeunlock failure. stat=%d: ERR=%s\n",
           errstat, be.bstrerror(errstat));
   }
}


/*
 * List Volumes -- this should be moved to status.c
 */
enum {
   debug_lock = true,
   debug_nolock = false
};

static void debug_list_volumes(const char *imsg)
{
   VOLRES *vol;
   POOL_MEM msg(PM_MESSAGE);

   lock_volumes();
   foreach_dlist(vol, vol_list) {
      if (vol->dev) {
         Mmsg(msg, "List %s: %s reserved=%d on device %s\n", imsg, 
              vol->vol_name, vol->is_reserved(), vol->dev->print_name());
      } else {
         Mmsg(msg, "List %s: %s reserved=%d no dev\n", imsg, vol->vol_name, 
              vol->is_reserved());
      }
      Dmsg1(dbglvl, "%s", msg.c_str());
   }

#ifdef xxx
   DEVICE *dev = NULL;
   foreach_dlist(vol, vol_list) {
      if (vol->dev == dev) {
         Dmsg0(dbglvl, "Two Volumes on same device.\n");
         ASSERT(0);
         dev = vol->dev;
      }
   }
#endif

   unlock_volumes();
}


/*
 * List Volumes -- this should be moved to status.c
 */
void list_volumes(void sendit(const char *msg, int len, void *sarg), void *arg)
{
   VOLRES *vol;
   POOL_MEM msg(PM_MESSAGE);
   int len;

   lock_volumes();
   foreach_dlist(vol, vol_list) {
      DEVICE *dev = vol->dev;
      if (dev) {
         len = Mmsg(msg, "%s on device %s\n", vol->vol_name, dev->print_name());
         sendit(msg.c_str(), len, arg);
         len = Mmsg(msg, "    Reader=%d writers=%d devres=%d volres=%d\n", 
            dev->can_read()?1:0, dev->num_writers, dev->reserved_device, 
            vol->is_reserved());
         sendit(msg.c_str(), len, arg);
      } else {
         len = Mmsg(msg, "%s no device. volres= %d\n", vol->vol_name, 
            vol->is_reserved());
         sendit(msg.c_str(), len, arg);
      }
   }
   unlock_volumes();
}

/*
 * Create a Volume item to put in the Volume list
 *   Ensure that the device points to it.
 */
static VOLRES *new_vol_item(DCR *dcr, const char *VolumeName)
{
   VOLRES *vol;
   vol = (VOLRES *)malloc(sizeof(VOLRES));
   memset(vol, 0, sizeof(VOLRES));
   vol->vol_name = bstrdup(VolumeName);
   vol->dev = dcr->dev;
   Dmsg3(dbglvl, "new Vol=%s at %p dev=%s\n",
         VolumeName, vol->vol_name, vol->dev->print_name());
   return vol;
}

static void free_vol_item(VOLRES *vol)
{
   DEVICE *dev = NULL;

   free(vol->vol_name);
   if (vol->dev) {
      dev = vol->dev;
   }
   free(vol);
   if (dev) {
      dev->vol = NULL;
   }
}


/*
 * Put a new Volume entry in the Volume list. This
 *  effectively reserves the volume so that it will
 *  not be mounted again.
 *
 * If the device has any current volume associated with it,
 *  and it is a different Volume, and the device is not busy,
 *  we release the old Volume item and insert the new one.
 * 
 * It is assumed that the device is free and locked so that
 *  we can change the device structure.
 *
 * Some details of the Volume list handling:
 *
 *  1. The Volume list entry must be attached to the drive (rather than 
 *       attached to a job as it currently is. I.e. the drive that "owns" 
 *       the volume (reserved, in use, mounted)
 *       must point to the volume (still to be maintained in a list).
 *
 *  2. The Volume is entered in the list when a drive is reserved.  
 *
 *  3. When a drive is in use, the device code must appropriately update the
 *      volume name as it changes (currently the list is static -- an entry is
 *      removed when the Volume is no longer reserved, in use or mounted).  
 *      The new code must keep the same list entry as long as the drive
 *       has any volume associated with it but the volume name in the list
 *       must be updated when the drive has a different volume mounted.
 *
 *  4. A job that has reserved a volume, can un-reserve the volume, and if the 
 *      volume is not mounted, and not reserved, and not in use, it will be
 *      removed from the list.
 *
 *  5. If a job wants to reserve a drive with a different Volume from the one on
 *      the drive, it can re-use the drive for the new Volume.
 *
 *  6. If a job wants a Volume that is in a different drive, it can either use the
 *      other drive or take the volume, only if the other drive is not in use or
 *      not reserved.
 *
 *  One nice aspect of this is that the reserve use count and the writer use count 
 *  already exist and are correctly programmed and will need no changes -- use 
 *  counts are always very tricky.
 *
 *  The old code had a concept of "reserving" a Volume, but was changed 
 *  to reserving and using a drive.  A volume is must be attached to (owned by) a 
 *  drive and can move from drive to drive or be unused given certain specific 
 *  conditions of the drive.  The key is that the drive must "own" the Volume.  
 *  The old code had the job (dcr) owning the volume (more or less).  The job was
 *  to change the insertion and removal of the volumes from the list to be based 
 *  on the drive rather than the job.  
 *
 *  Return: VOLRES entry on success
 *          NULL volume busy on another drive
 */
VOLRES *reserve_volume(DCR *dcr, const char *VolumeName)
{
   VOLRES *vol, *nvol;
   DEVICE * volatile dev = dcr->dev;

   ASSERT(dev != NULL);

   Dmsg2(dbglvl, "enter reserve_volume=%s drive=%s\n", VolumeName, 
      dcr->dev->print_name());
   /* 
    * We lock the reservations system here to ensure
    *  when adding a new volume that no newly scheduled
    *  job can reserve it.
    */
   lock_volumes();
   debug_list_volumes("begin reserve_volume");
   /* 
    * First, remove any old volume attached to this device as it
    *  is no longer used.
    */
   if (dev->vol) {
      vol = dev->vol;
      Dmsg4(dbglvl, "Vol attached=%s, newvol=%s volres=%d on %s\n",
         vol->vol_name, VolumeName, vol->is_reserved(), dev->print_name());
      /*
       * Make sure we don't remove the current volume we are inserting
       *  because it was probably inserted by another job, or it
       *  is not being used and is marked as not reserved.
       */
      if (strcmp(vol->vol_name, VolumeName) == 0) {
         Dmsg2(dbglvl, "=== set reserved vol=%s dev=%s\n", VolumeName,
               vol->dev->print_name());
         vol->set_reserved();           /* retake vol if released previously */
         dcr->reserved_volume = true;   /* reserved volume */
         goto get_out;                  /* Volume already on this device */
      } else {
         /* Don't release a volume if it was reserved by someone other than us */
         if (vol->is_reserved() && !dcr->reserved_volume) { 
            Dmsg1(dbglvl, "Cannot free vol=%s. It is reserved.\n", vol->vol_name);
            vol = NULL;                  /* vol in use */
            goto get_out;
         }
         Dmsg2(dbglvl, "reserve_vol free vol=%s at %p\n", vol->vol_name, vol->vol_name);
         free_volume(dev);
         dcr->unload_device = true;     /* have to unload current volume */
         debug_list_volumes("reserve_vol free");
      }
   }

   /* Create a new Volume entry */
   nvol = new_vol_item(dcr, VolumeName);

   /*
    * Now try to insert the new Volume
    */
   vol = (VOLRES *)vol_list->binary_insert(nvol, my_compare);
   if (vol != nvol) {
      Dmsg2(dbglvl, "Found vol=%s dev-same=%d\n", vol->vol_name, dev==vol->dev);
      /*
       * At this point, a Volume with this name already is in the list,
       *   so we simply release our new Volume entry. Note, this should
       *   only happen if we are moving the volume from one drive to another.
       */
      Dmsg2(dbglvl, "reserve_vol free-tmp vol=%s at %p\n", 
            vol->vol_name, vol->vol_name);
      /*
       * Clear dev pointer so that free_vol_item() doesn't 
       *  take away our volume. 
       */
      nvol->dev = NULL;                  /* don't zap dev entry */
      free_vol_item(nvol);

      /* Check if we are trying to use the Volume on a different drive */
      if (dev != vol->dev) {
         /* Caller wants to switch Volume to another device */
         if (!vol->dev->is_busy() && !vol->is_swapping()) {
            vol->set_swapping();
            dcr->swap_dev = vol->dev;   /* remember to get this vol */
            Dmsg3(dbglvl, "==== Swap vol=%s from dev=%s to %s\n", 
               VolumeName, vol->dev->print_name(), dev->print_name());
         } else {
            Dmsg3(dbglvl, "==== Swap not possible Vol busy vol=%s from dev=%s to %s\n", 
               VolumeName, vol->dev->print_name(), dev->print_name());
            vol = NULL;                 /* device busy */
            goto get_out;
         }
      } else {
         dev->vol = vol;
      }
   } else {
      dev->vol = vol;                    /* point to newly inserted volume */
   }

get_out:
   if (vol) {
      Dmsg2(dbglvl, "=== set reserved. vol=%s dev=%s\n", vol->vol_name,
            vol->dev->print_name());
      vol->set_reserved();
      dcr->reserved_volume = true;
   }
   debug_list_volumes("end new volume");
   unlock_volumes();
   return vol;
}

/* 
 * Switch from current device to given device  
 *   (not yet used) 
 */
void switch_device(DCR *dcr, DEVICE *dev)
{
   DCR save_dcr;

   dev->dlock();
   memcpy(&save_dcr, dcr, sizeof(save_dcr));
   clean_device(dcr);                  /* clean up the dcr */

   dcr->dev = dev;                     /* get new device pointer */
   Jmsg(dcr->jcr, M_INFO, 0, _("Device switch. New device %s chosen.\n"),
      dcr->dev->print_name());

   bstrncpy(dcr->VolumeName, save_dcr.VolumeName, sizeof(dcr->VolumeName));
   bstrncpy(dcr->media_type, save_dcr.media_type, sizeof(dcr->media_type));
   dcr->VolCatInfo.Slot = save_dcr.VolCatInfo.Slot;
   bstrncpy(dcr->pool_name, save_dcr.pool_name, sizeof(dcr->pool_name));
   bstrncpy(dcr->pool_type, save_dcr.pool_type, sizeof(dcr->pool_type));
   bstrncpy(dcr->dev_name, dev->dev_name, sizeof(dcr->dev_name));

   dev->reserved_device++;
   dcr->reserved_device = true;

   dev->dunlock();
}

/*
 * Search for a Volume name in the Volume list.
 *
 *  Returns: VOLRES entry on success
 *           NULL if the Volume is not in the list
 */
VOLRES *find_volume(DCR *dcr)
{
   VOLRES vol, *fvol;
   /* Do not lock reservations here */
   lock_volumes();
   vol.vol_name = bstrdup(dcr->VolumeName);
   fvol = (VOLRES *)vol_list->binary_search(&vol, my_compare);
   free(vol.vol_name);
   Dmsg2(dbglvl, "find_vol=%s found=%d\n", dcr->VolumeName, fvol!=NULL);
   debug_list_volumes("find_volume");
   unlock_volumes();
   return fvol;
}

/* 
 * Remove any reservation from a drive and tell the system
 *  that the volume is unused at least by us.
 */
void unreserve_device(DCR *dcr)
{
   DEVICE *dev = dcr->dev;
   lock_volumes();
   if (dcr->reserved_device) {
      dcr->reserved_device = false;
      dcr->reserved_volume = false;
      dev->reserved_device--;
      Dmsg2(dbglvl, "Dec reserve=%d dev=%s\n", dev->reserved_device, dev->print_name());
      /* If we set read mode in reserving, remove it */
      if (dev->can_read()) {
         dev->clear_read();
      }
      if (dev->num_writers < 0) {
         Jmsg1(dcr->jcr, M_ERROR, 0, _("Hey! num_writers=%d!!!!\n"), dev->num_writers);
         dev->num_writers = 0;
      }
      if (dev->reserved_device == 0 && dev->num_writers == 0) {
         volume_unused(dcr);
      }
   }
   unlock_volumes();
}

/*  
 * Free a Volume from the Volume list if it is no longer used
 *   Note, for tape drives we want to remember where the Volume
 *   was when last used, so rather than free the volume entry,
 *   we simply mark it "not reserved" so when the drive is really
 *   needed for another volume, we can reuse it.
 *
 *  Returns: true if the Volume found and "removed" from the list
 *           false if the Volume is not in the list or is in use
 */
bool volume_unused(DCR *dcr)
{
   DEVICE *dev = dcr->dev;

   if (!dev->vol) {
      Dmsg1(dbglvl, "vol_unused: no vol on %s\n", dev->print_name());
      debug_list_volumes("null vol cannot unreserve_volume");
      return false;
   }
   if (dev->vol->is_swapping()) {
      Dmsg1(dbglvl, "vol_unused: vol being swapped on %s\n", dev->print_name());
      debug_list_volumes("swapping vol cannot unreserve_volume");
      return false;
   }

#ifdef xxx
   if (dev->is_busy()) {
      Dmsg1(dbglvl, "vol_unused: busy on %s\n", dev->print_name());
      debug_list_volumes("dev busy cannot unreserve_volume");
      return false;
   }
#endif
#ifdef xxx
   if (dev->num_writers > 0 || dev->reserved_device > 0) {
      ASSERT(0);
   }
#endif

   /*  
    * If this is a tape, we do not free the volume, rather we wait
    *  until the autoloader unloads it, or until another tape is
    *  explicitly read in this drive. This allows the SD to remember
    *  where the tapes are or last were.
    */
   Dmsg3(dbglvl, "=== mark not reserved vol=%s num_writers=%d dev_reserved=%d\n",
      dev->vol->vol_name, dev->num_writers, dev->reserved_device);
   dev->vol->clear_reserved();
   Dmsg2(dbglvl, "=== set not reserved. Vol=%s dev=%s\n", dev->vol->vol_name,
         dev->print_name());
   if (dev->is_tape() || dev->is_autochanger()) {
      return true;
   } else {
      /*
       * Note, this frees the volume reservation entry, but the 
       *   file descriptor remains open with the OS.
       */
      return free_volume(dev);
   }
}

/*
 * Unconditionally release the volume entry
 */
bool free_volume(DEVICE *dev)
{
   VOLRES *vol;

   if (dev->vol == NULL) {
      Dmsg1(dbglvl, "No vol on dev %s\n", dev->print_name());
      return false;
   }
   lock_volumes();
   vol = dev->vol;
   dev->vol = NULL;
   vol_list->remove(vol);
   Dmsg2(dbglvl, "=== free_volume %s dev=%s\n", vol->vol_name, dev->print_name());
   free_vol_item(vol);
   debug_list_volumes("free_volume");
   unlock_volumes();
   return true;
}

      
/* Create the Volume list */
void create_volume_list()
{
   VOLRES *vol = NULL;
   if (vol_list == NULL) {
      vol_list = New(dlist(vol, &vol->link));
   }
}

/* Release all Volumes from the list */
void free_volume_list()
{
   VOLRES *vol;
   if (!vol_list) {
      return;
   }
   lock_volumes();
   foreach_dlist(vol, vol_list) {
      if (vol->dev) {
         Dmsg2(dbglvl, "free vol_list Volume=%s dev=%s\n", vol->vol_name, vol->dev->print_name());
      } else {
         Dmsg1(dbglvl, "free vol_list Volume=%s No dev\n", vol->vol_name);
      }
      free(vol->vol_name);
      vol->vol_name = NULL;
   }
   delete vol_list;
   vol_list = NULL;
   unlock_volumes();
}

bool is_volume_in_use(DCR *dcr)
{
   bool rtn = false;
   VOLRES *vol;

   lock_volumes();
   vol = find_volume(dcr);
   if (!vol) {
      Dmsg1(dbglvl, "Vol=%s not in use.\n", dcr->VolumeName);
      goto get_out;                   /* vol not in list */
   }
   ASSERT(vol->dev != NULL);

   if (dcr->dev == vol->dev) {        /* same device OK */
      Dmsg1(dbglvl, "Vol=%s on same dev.\n", dcr->VolumeName);
      goto get_out;
   } else {
      Dmsg3(dbglvl, "Vol=%s on %s we have %s\n", dcr->VolumeName,
            vol->dev->print_name(), dcr->dev->print_name());
   }
   if (!vol->dev->is_busy()) {
      Dmsg2(dbglvl, "Vol=%s dev=%s not busy.\n", dcr->VolumeName, vol->dev->print_name());
      goto get_out;
   } else {
      Dmsg2(dbglvl, "Vol=%s dev=%s busy.\n", dcr->VolumeName, vol->dev->print_name());
   }
   Dmsg2(dbglvl, "Vol=%s in use by %s.\n", dcr->VolumeName, vol->dev->print_name());
   rtn = true;

get_out:
   unlock_volumes();
   return rtn;

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
   alist *dirstore;

   memset(&rctx, 0, sizeof(RCTX));
   rctx.jcr = jcr;
   /*
    * If there are multiple devices, the director sends us
    *   use_device for each device that it wants to use.
    */
   dirstore = New(alist(10, not_owned_by_alist));
   jcr->reserve_msgs = New(alist(10, not_owned_by_alist));  
   do {
      Dmsg1(dbglvl, "<dird: %s", dir->msg);
      ok = sscanf(dir->msg, use_storage, store_name.c_str(), 
                  media_type.c_str(), pool_name.c_str(), 
                  pool_type.c_str(), &append, &Copy, &Stripe) == 7;
      if (!ok) {
         break;
      }
      if (append) {
         jcr->write_store = dirstore;
      } else {
         jcr->read_store = dirstore;
      }
      rctx.append = append;
      unbash_spaces(store_name);
      unbash_spaces(media_type);
      unbash_spaces(pool_name);
      unbash_spaces(pool_type);
      store = new DIRSTORE;
      dirstore->append(store);
      memset(store, 0, sizeof(DIRSTORE));
      store->device = New(alist(10));
      bstrncpy(store->name, store_name, sizeof(store->name));
      bstrncpy(store->media_type, media_type, sizeof(store->media_type));
      bstrncpy(store->pool_name, pool_name, sizeof(store->pool_name));
      bstrncpy(store->pool_type, pool_type, sizeof(store->pool_type));
      store->append = append;

      /* Now get all devices */
      while (dir->recv() >= 0) {
         Dmsg1(dbglvl, "<dird device: %s", dir->msg);
         ok = sscanf(dir->msg, use_device, dev_name.c_str()) == 1;
         if (!ok) {
            break;
         }
         unbash_spaces(dev_name);
         store->device->append(bstrdup(dev_name.c_str()));
      }
   }  while (ok && dir->recv() >= 0);

   /* Developer debug code */
   char *device_name;
   if (debug_level >= dbglvl) {
      foreach_alist(store, dirstore) {
         Dmsg5(dbglvl, "Storage=%s media_type=%s pool=%s pool_type=%s append=%d\n", 
            store->name, store->media_type, store->pool_name, 
            store->pool_type, store->append);
         foreach_alist(device_name, store->device) {
            Dmsg1(dbglvl, "     Device=%s\n", device_name);
         }
      }
   }

   init_jcr_device_wait_timers(jcr);
   jcr->dcr = new_dcr(jcr, NULL, NULL);         /* get a dcr */
   if (!jcr->dcr) {
      BSOCK *dir = jcr->dir_bsock;
      dir->fsend(_("3939 Could not get dcr\n"));
      Dmsg1(dbglvl, ">dird: %s", dir->msg);
      ok = false;
   }
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
      int wait_for_device_retries = 0;  
      int repeat = 0;
      bool fail = false;
      rctx.notify_dir = true;

      lock_reservations();
      for ( ; !fail && !job_canceled(jcr); ) {
         pop_reserve_messages(jcr);
         rctx.suitable_device = false;
         rctx.have_volume = false;
         rctx.VolumeName[0] = 0;
         rctx.any_drive = false;
         if (!jcr->PreferMountedVols) {
            /*
             * Here we try to find a drive that is not used.
             * This will maximize the use of available drives.
             *
             */
            rctx.num_writers = 20000000;   /* start with impossible number */
            rctx.low_use_drive = NULL;
            rctx.PreferMountedVols = false;                
            rctx.exact_match = false;
            rctx.autochanger_only = true;
            Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
               rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
               rctx.autochanger_only, rctx.any_drive);
            if ((ok = find_suitable_device_for_job(jcr, rctx))) {
               break;
            }
            /* Look through all drives possibly for low_use drive */
            if (rctx.low_use_drive) {
               rctx.try_low_use_drive = true;
               if ((ok = find_suitable_device_for_job(jcr, rctx))) {
                  break;
               }
               rctx.try_low_use_drive = false;
            }
            rctx.autochanger_only = false;
            Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
               rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
               rctx.autochanger_only, rctx.any_drive);
            if ((ok = find_suitable_device_for_job(jcr, rctx))) {
               break;
            }
         }
         /*
          * Now we look for a drive that may or may not be in
          *  use.
          */
         /* Look for an exact Volume match all drives */
         rctx.PreferMountedVols = true;
         rctx.exact_match = true;
         rctx.autochanger_only = false;
         Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
            rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
            rctx.autochanger_only, rctx.any_drive);
         if ((ok = find_suitable_device_for_job(jcr, rctx))) {
            break;
         }
         /* Look for any mounted drive */
         rctx.exact_match = false;
         Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
            rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
            rctx.autochanger_only, rctx.any_drive);
         if ((ok = find_suitable_device_for_job(jcr, rctx))) {
            break;
         }
         /* Try any drive */
         rctx.any_drive = true;
         Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
            rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
            rctx.autochanger_only, rctx.any_drive);
         if ((ok = find_suitable_device_for_job(jcr, rctx))) {
            break;
         }
         /* Keep reservations locked *except* during wait_for_device() */
         unlock_reservations();
         /*     
          * The idea of looping on repeat a few times it to ensure
          * that if there is some subtle timing problem between two
          * jobs, we will simply try again, and most likely succeed.
          * This can happen if one job reserves a drive or finishes using
          * a drive at the same time a second job wants it.
          */
         if (repeat++ > 1) {              /* try algorithm 3 times */
            bmicrosleep(30, 0);           /* wait a bit */
            Dmsg0(dbglvl, "repeat reserve algorithm\n");
         } else if (!rctx.suitable_device || !wait_for_device(jcr, wait_for_device_retries)) {
            Dmsg0(dbglvl, "Fail. !suitable_device || !wait_for_device\n");
            fail = true;
         }   
         lock_reservations();
         dir->signal(BNET_HEARTBEAT);  /* Inform Dir that we are alive */
      }
      unlock_reservations();
      if (!ok) {
         /*
          * If we get here, there are no suitable devices available, which
          *  means nothing configured.  If a device is suitable but busy
          *  with another Volume, we will not come here.
          */
         unbash_spaces(dir->msg);
         pm_strcpy(jcr->errmsg, dir->msg);
         Jmsg(jcr, M_INFO, 0, _("Failed command: %s\n"), jcr->errmsg);
         Jmsg(jcr, M_FATAL, 0, _("\n"
            "     Device \"%s\" with MediaType \"%s\" requested by DIR not found in SD Device resources.\n"),
              dev_name.c_str(), media_type.c_str());
         dir->fsend(NO_device, dev_name.c_str());

         Dmsg1(dbglvl, ">dird: %s", dir->msg);
      }
   } else {
      unbash_spaces(dir->msg);
      pm_strcpy(jcr->errmsg, dir->msg);
      Jmsg(jcr, M_FATAL, 0, _("Failed command: %s\n"), jcr->errmsg);
      dir->fsend(BAD_use, jcr->errmsg);
      Dmsg1(dbglvl, ">dird: %s", dir->msg);
   }

   release_reserve_messages(jcr);
   return ok;
}


/*
 * Walk through the autochanger resources and check if
 *  the volume is in one of them.
 * 
 * Returns:  true  if volume is in device
 *           false otherwise
 */
static bool is_vol_in_autochanger(RCTX &rctx, VOLRES *vol)
{
   AUTOCHANGER *changer = vol->dev->device->changer_res;

   /* Find resource, and make sure we were able to open it */
   if (strcmp(rctx.device_name, changer->hdr.name) == 0) {
      Dmsg1(dbglvl, "Found changer device %s\n", vol->dev->device->hdr.name);
      return true;
   }  
   Dmsg1(dbglvl, "Incorrect changer device %s\n", changer->hdr.name);
   return false;
}

/*
 * Search for a device suitable for this job.
 * Note, this routine sets sets rctx.suitable_device if any 
 *   device exists within the SD.  The device may not be actually
 *   useable.
 * It also returns if it finds a useable device.  
 */
bool find_suitable_device_for_job(JCR *jcr, RCTX &rctx)
{
   bool ok = false;
   DIRSTORE *store;
   char *device_name;
   alist *dirstore;
   DCR *dcr = jcr->dcr;

   if (rctx.append) {
      dirstore = jcr->write_store;
   } else {
      dirstore = jcr->read_store;
   }
   Dmsg4(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d\n",
      rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
      rctx.autochanger_only);

   /* 
    * If the appropriate conditions of this if are met, namely that
    *  we are appending and the user wants mounted drive (or we
    *  force try a mounted drive because they are all busy), we
    *  start by looking at all the Volumes in the volume list.
    */
   if (!vol_list->empty() && rctx.append && rctx.PreferMountedVols) {
      dlist *temp_vol_list, *save_vol_list;
      VOLRES *vol = NULL;
      lock_volumes();
      Dmsg0(dbglvl, "lock volumes\n");                           

      /*  
       * Create a temporary copy of the volume list.  We do this,
       *   to avoid having the volume list locked during the
       *   call to reserve_device(), which would cause a deadlock.
       * Note, we may want to add an update counter on the vol_list
       *   so that if it is modified while we are traversing the copy
       *   we can take note and act accordingly (probably redo the 
       *   search at least a few times).
       */
      Dmsg0(dbglvl, "duplicate vol list\n");
      temp_vol_list = New(dlist(vol, &vol->link));
      foreach_dlist(vol, vol_list) {
         VOLRES *nvol;
         VOLRES *tvol = (VOLRES *)malloc(sizeof(VOLRES));
         memset(tvol, 0, sizeof(VOLRES));
         tvol->vol_name = bstrdup(vol->vol_name);
         tvol->dev = vol->dev;
         nvol = (VOLRES *)temp_vol_list->binary_insert(tvol, my_compare);
         if (tvol != nvol) {
            tvol->dev = NULL;                   /* don't zap dev entry */
            free_vol_item(tvol);
            Pmsg0(000, "Logic error. Duplicating vol list hit duplicate.\n");
            Jmsg(jcr, M_WARNING, 0, "Logic error. Duplicating vol list hit duplicate.\n");
         }
      }
      Dmsg0(dbglvl, "unlock volumes\n");
      unlock_volumes();

      /* Look through reserved volumes for one we can use */
      Dmsg0(dbglvl, "look for vol in vol list\n");
      foreach_dlist(vol, temp_vol_list) {
         if (!vol->dev) {
            Dmsg1(dbglvl, "vol=%s no dev\n", vol->vol_name);
            continue;
         }
         /* Check with Director if this Volume is OK */
         bstrncpy(dcr->VolumeName, vol->vol_name, sizeof(dcr->VolumeName));
         if (!dir_get_volume_info(dcr, GET_VOL_INFO_FOR_WRITE)) {
            continue;
         }

         Dmsg1(dbglvl, "vol=%s OK for this job\n", vol->vol_name);
         foreach_alist(store, dirstore) {
            int stat;
            rctx.store = store;
            foreach_alist(device_name, store->device) {
               /* Found a device, try to use it */
               rctx.device_name = device_name;
               rctx.device = vol->dev->device;

               if (vol->dev->is_autochanger()) {
                  Dmsg1(dbglvl, "vol=%s is in changer\n", vol->vol_name);
                  if (!is_vol_in_autochanger(rctx, vol)) {
                     continue;
                  }
               } else if (strcmp(device_name, vol->dev->device->hdr.name) != 0) {
                  Dmsg2(dbglvl, "device=%s not suitable want %s\n",
                        vol->dev->device->hdr.name, device_name);
                  continue;
               }

               bstrncpy(rctx.VolumeName, vol->vol_name, sizeof(rctx.VolumeName));
               rctx.have_volume = true;
               /* Try reserving this device and volume */
               Dmsg2(dbglvl, "try vol=%s on device=%s\n", rctx.VolumeName, device_name);
               stat = reserve_device(rctx);
               if (stat == 1) {             /* found available device */
                  Dmsg1(dbglvl, "Suitable device found=%s\n", device_name);
                  ok = true;
                  break;
               } else if (stat == 0) {      /* device busy */
                  Dmsg1(dbglvl, "Suitable device=%s, busy: not use\n", device_name);
               } else {
                  /* otherwise error */
                  Dmsg0(dbglvl, "No suitable device found.\n");
               }
               rctx.have_volume = false;
               rctx.VolumeName[0] = 0;
            }
            if (ok) {
               break;
            }
         }
         if (ok) {
            break;
         }
      } /* end for loop over reserved volumes */

      Dmsg0(dbglvl, "lock volumes\n");
      lock_volumes();
      save_vol_list = vol_list;
      vol_list = temp_vol_list;
      free_volume_list();                  /* release temp_vol_list */
      vol_list = save_vol_list;
      Dmsg0(dbglvl, "deleted temp vol list\n");
      Dmsg0(dbglvl, "unlock volumes\n");
      unlock_volumes();
      debug_list_volumes("=== After free temp table\n");
   }
   if (ok) {
      Dmsg1(dbglvl, "Usable dev found. Vol=%s from in-use vols list\n", rctx.VolumeName);
      return true;
   }

   /* 
    * No reserved volume we can use, so now search for an available device.  
    *
    * For each storage device that the user specified, we
    *  search and see if there is a resource for that device.
    */
   foreach_alist(store, dirstore) {
      rctx.store = store;
      foreach_alist(device_name, store->device) {
         int stat;
         rctx.device_name = device_name;
         stat = search_res_for_device(rctx); 
         if (stat == 1) {             /* found available device */
            Dmsg1(dbglvl, "available device found=%s\n", device_name);
            ok = true;
            break;
         } else if (stat == 0) {      /* device busy */
            Dmsg1(dbglvl, "No usable device=%s, busy: not use\n", device_name);
         } else {
            /* otherwise error */
            Dmsg0(dbglvl, "No usable device found.\n");
         }
      }
      if (ok) {
         break;
      }
   }
   if (ok) {
      Dmsg1(dbglvl, "Usable dev found. Vol=%s\n", rctx.VolumeName);
   }
   return ok;
}

/*
 * Search for a particular storage device with particular storage
 *  characteristics (MediaType).
 */
int search_res_for_device(RCTX &rctx) 
{
   AUTOCHANGER *changer;
   int stat;

   Dmsg1(dbglvl, "search res for %s\n", rctx.device_name);
   /* Look through Autochangers first */
   foreach_res(changer, R_AUTOCHANGER) {
      Dmsg1(dbglvl, "Try match changer res=%s\n", changer->hdr.name);
      /* Find resource, and make sure we were able to open it */
      if (strcmp(rctx.device_name, changer->hdr.name) == 0) {
         /* Try each device in this AutoChanger */
         foreach_alist(rctx.device, changer->device) {
            Dmsg1(dbglvl, "Try changer device %s\n", rctx.device->hdr.name);
            stat = reserve_device(rctx);
            if (stat != 1) {             /* try another device */
               continue;
            }
            /* Debug code */
            if (rctx.store->append == SD_APPEND) {
               Dmsg2(dbglvl, "Device %s reserved=%d for append.\n", 
                  rctx.device->hdr.name, rctx.jcr->dcr->dev->reserved_device);
            } else {
               Dmsg2(dbglvl, "Device %s reserved=%d for read.\n", 
                  rctx.device->hdr.name, rctx.jcr->read_dcr->dev->reserved_device);
            }
            return stat;
         }
      }
   }

   /* Now if requested look through regular devices */
   if (!rctx.autochanger_only) {
      foreach_res(rctx.device, R_DEVICE) {
         Dmsg1(dbglvl, "Try match res=%s\n", rctx.device->hdr.name);
         /* Find resource, and make sure we were able to open it */
         if (strcmp(rctx.device_name, rctx.device->hdr.name) == 0) {
            stat = reserve_device(rctx);
            if (stat != 1) {             /* try another device */
               continue;
            }
            /* Debug code */
            if (rctx.store->append == SD_APPEND) {
               Dmsg2(dbglvl, "Device %s reserved=%d for append.\n", 
                  rctx.device->hdr.name, rctx.jcr->dcr->dev->reserved_device);
            } else {
               Dmsg2(dbglvl, "Device %s reserved=%d for read.\n", 
                  rctx.device->hdr.name, rctx.jcr->read_dcr->dev->reserved_device);
            }
            return stat;
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
   Dmsg2(dbglvl, "chk MediaType device=%s request=%s\n",
         rctx.device->media_type, rctx.store->media_type);
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
   Dmsg1(dbglvl, "try reserve %s\n", rctx.device->hdr.name);
   rctx.jcr->dcr = dcr = new_dcr(rctx.jcr, rctx.jcr->dcr, rctx.device->dev);
   if (!dcr) {
      BSOCK *dir = rctx.jcr->dir_bsock;
      dir->fsend(_("3926 Could not get dcr for device: %s\n"), rctx.device_name);
      Dmsg1(dbglvl, ">dird: %s", dir->msg);
      return -1;
   }
   bstrncpy(dcr->pool_name, rctx.store->pool_name, name_len);
   bstrncpy(dcr->pool_type, rctx.store->pool_type, name_len);
   bstrncpy(dcr->media_type, rctx.store->media_type, name_len);
   bstrncpy(dcr->dev_name, rctx.device_name, name_len);
   if (rctx.store->append == SD_APPEND) {
      Dmsg2(dbglvl, "call reserve for append: have_vol=%d vol=%s\n", rctx.have_volume, rctx.VolumeName);                                   
      ok = reserve_device_for_append(dcr, rctx);
      if (!ok) {
         goto bail_out;
      }

      rctx.jcr->dcr = dcr;
      Dmsg5(dbglvl, "Reserved=%d dev_name=%s mediatype=%s pool=%s ok=%d\n",
               dcr->dev->reserved_device,
               dcr->dev_name, dcr->media_type, dcr->pool_name, ok);
      Dmsg3(dbglvl, "Vol=%s num_writers=%d, have_vol=%d\n", 
         rctx.VolumeName, dcr->dev->num_writers, rctx.have_volume);
      if (rctx.have_volume) {
         Dmsg0(dbglvl, "Call reserve_volume\n");
         if (reserve_volume(dcr, rctx.VolumeName)) {
            Dmsg1(dbglvl, "Reserved vol=%s\n", rctx.VolumeName);
         } else {
            Dmsg1(dbglvl, "Could not reserve vol=%s\n", rctx.VolumeName);
            goto bail_out;
         }
      } else {
         dcr->any_volume = true;
         Dmsg0(dbglvl, "no vol, call find_next_appendable_vol.\n");
         if (dir_find_next_appendable_volume(dcr)) {
            bstrncpy(rctx.VolumeName, dcr->VolumeName, sizeof(rctx.VolumeName));
            rctx.have_volume = true;
            Dmsg1(dbglvl, "looking for Volume=%s\n", rctx.VolumeName);
         } else {
            Dmsg0(dbglvl, "No next volume found\n");
            rctx.have_volume = false;
            rctx.VolumeName[0] = 0;
            /*
             * If there is at least one volume that is valid and in use,
             *   but we get here, check if we are running with prefers
             *   non-mounted drives.  In that case, we have selected a
             *   non-used drive and our one and only volume is mounted
             *   elsewhere, so we bail out and retry using that drive.
             */
            if (dcr->volume_in_use && !rctx.PreferMountedVols) {
               rctx.PreferMountedVols = true;
               if (dcr->VolumeName[0]) {
                  unreserve_device(dcr);
               }
               goto bail_out;
            }
            /*
             * Note. Under some circumstances, the Director can hand us
             *  a Volume name that is not the same as the one on the current
             *  drive, and in that case, the call above to find the next
             *  volume will fail because in attempting to reserve the Volume
             *  the code will realize that we already have a tape mounted,
             *  and it will fail.  This *should* only happen if there are 
             *  writers, thus the following test.  In that case, we simply
             *  bail out, and continue waiting, rather than plunging on
             *  and hoping that the operator can resolve the problem. 
             */
            if (dcr->dev->num_writers != 0) {
               if (dcr->VolumeName[0]) {
                  unreserve_device(dcr);
               }
               goto bail_out;
            }
         }
      }
   } else {
      ok = reserve_device_for_read(dcr);
      if (ok) {
         rctx.jcr->read_dcr = dcr;
         Dmsg5(dbglvl, "Read reserved=%d dev_name=%s mediatype=%s pool=%s ok=%d\n",
               dcr->dev->reserved_device,
               dcr->dev_name, dcr->media_type, dcr->pool_name, ok);
      }
   }
   if (!ok) {
      goto bail_out;
   }

   if (rctx.notify_dir) {
      POOL_MEM dev_name;
      BSOCK *dir = rctx.jcr->dir_bsock;
      pm_strcpy(dev_name, rctx.device->hdr.name);
      bash_spaces(dev_name);
      ok = dir->fsend(OK_device, dev_name.c_str());  /* Return real device name */
      Dmsg1(dbglvl, ">dird: %s", dir->msg);
   } else {
      ok = true;
   }
   return ok ? 1 : -1;

bail_out:
   rctx.have_volume = false;
   rctx.VolumeName[0] = 0;
   Dmsg0(dbglvl, "Not OK.\n");
   return 0;
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

   dev->dlock();  

   if (is_device_unmounted(dev)) {             
      Dmsg1(dbglvl, "Device %s is BLOCKED due to user unmount.\n", dev->print_name());
      Mmsg(jcr->errmsg, _("3601 JobId=%u device %s is BLOCKED due to user unmount.\n"),
           jcr->JobId, dev->print_name());
      queue_reserve_message(jcr);
      goto bail_out;
   }

   if (dev->is_busy()) {
      Dmsg4(dbglvl, "Device %s is busy ST_READ=%d num_writers=%d reserved=%d.\n", 
         dev->print_name(),
         dev->state & ST_READ?1:0, dev->num_writers, dev->reserved_device);
      Mmsg(jcr->errmsg, _("3602 JobId=%u device %s is busy (already reading/writing).\n"),
            jcr->JobId, dev->print_name());
      queue_reserve_message(jcr);
      goto bail_out;
   }

   dev->clear_append();
   dev->set_read();
   ok = true;
   dev->reserved_device++;
   Dmsg3(dbglvl, "Inc reserve=%d dev=%s %p\n", dev->reserved_device, dev->print_name(), dev);
   dcr->reserved_device = true;

bail_out:
   dev->dunlock();
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

   dev->dlock();

   /* If device is being read, we cannot write it */
   if (dev->can_read()) {
      Mmsg(jcr->errmsg, _("3603 JobId=%u device %s is busy reading.\n"), 
         jcr->JobId, dev->print_name());
      Dmsg1(dbglvl, "%s", jcr->errmsg);
      queue_reserve_message(jcr);
      goto bail_out;
   }

   /* If device is unmounted, we are out of luck */
   if (is_device_unmounted(dev)) {
      Mmsg(jcr->errmsg, _("3604 JobId=%u device %s is BLOCKED due to user unmount.\n"), 
         jcr->JobId, dev->print_name());
      Dmsg1(dbglvl, "%s", jcr->errmsg);
      queue_reserve_message(jcr);
      goto bail_out;
   }

   Dmsg1(dbglvl, "reserve_append device is %s\n", dev->print_name());

   /* Now do detailed tests ... */
   if (can_reserve_drive(dcr, rctx) != 1) {
      Dmsg0(dbglvl, "can_reserve_drive!=1\n");
      goto bail_out;
   }

   dev->reserved_device++;
   Dmsg3(dbglvl, "Inc reserve=%d dev=%s %p\n", dev->reserved_device, 
      dev->print_name(), dev);
   dcr->reserved_device = true;
   ok = true;

bail_out:
   dev->dunlock();
   return ok;
}

static int is_pool_ok(DCR *dcr)
{
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;

   /* Now check if we want the same Pool and pool type */
   if (strcmp(dev->pool_name, dcr->pool_name) == 0 &&
       strcmp(dev->pool_type, dcr->pool_type) == 0) {
      /* OK, compatible device */
      Dmsg1(dbglvl, "OK dev: %s num_writers=0, reserved, pool matches\n", dev->print_name());
      return 1;
   } else {
      /* Drive Pool not suitable for us */
      Mmsg(jcr->errmsg, _(
"3608 JobId=%u wants Pool=\"%s\" but have Pool=\"%s\" nreserve=%d on drive %s.\n"), 
            (uint32_t)jcr->JobId, dcr->pool_name, dev->pool_name,
            dev->reserved_device, dev->print_name());
      queue_reserve_message(jcr);
      Dmsg2(dbglvl, "failed: busy num_writers=0, reserved, pool=%s wanted=%s\n",
         dev->pool_name, dcr->pool_name);
   }
   return 0;
}

static bool is_max_jobs_ok(DCR *dcr) 
{
   DEVICE *dev = dcr->dev;
   JCR *jcr = dcr->jcr;

   Dmsg4(dbglvl, "MaxJobs=%d Jobs=%d reserves=%d Vol=%s\n",
         dcr->VolCatInfo.VolCatMaxJobs,
         dcr->VolCatInfo.VolCatJobs, dev->reserved_device,
         dcr->VolumeName);
   if (dcr->VolCatInfo.VolCatMaxJobs > 0 && dcr->VolCatInfo.VolCatMaxJobs <=
        (dcr->VolCatInfo.VolCatJobs + dev->reserved_device)) {
      /* Max Job Vols depassed or already reserved */
      Mmsg(jcr->errmsg, _("3610 JobId=%u Volume max jobs exceeded on drive %s.\n"), 
            (uint32_t)jcr->JobId, dev->print_name());
      queue_reserve_message(jcr);
      Dmsg1(dbglvl, "reserve dev failed: %s", jcr->errmsg);
      return false;                /* wait */
   }
   return true;
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

   Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
         rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
         rctx.autochanger_only, rctx.any_drive);

   /* Check for max jobs on this Volume */
   if (!is_max_jobs_ok(dcr)) {
      return 0;
   }

   /* setting any_drive overrides PreferMountedVols flag */
   if (!rctx.any_drive) {
      /*
       * When PreferMountedVols is set, we keep track of the 
       *  drive in use that has the least number of writers, then if
       *  no unmounted drive is found, we try that drive. This   
       *  helps spread the load to the least used drives.  
       */
      if (rctx.try_low_use_drive && dev == rctx.low_use_drive) {
         Dmsg2(dbglvl, "OK dev=%s == low_drive=%s.\n",
            dev->print_name(), rctx.low_use_drive->print_name());
         return 1;
      }
      /* If he wants a free drive, but this one is busy, no go */
      if (!rctx.PreferMountedVols && dev->is_busy()) {
         /* Save least used drive */
         if ((dev->num_writers + dev->reserved_device) < rctx.num_writers) {
            rctx.num_writers = dev->num_writers + dev->reserved_device;
            rctx.low_use_drive = dev;
            Dmsg2(dbglvl, "set low use drive=%s num_writers=%d\n", 
               dev->print_name(), rctx.num_writers);
         } else {
            Dmsg1(dbglvl, "not low use num_writers=%d\n", dev->num_writers+dev->reserved_device);
         }
         Dmsg0(dbglvl, "failed: !prefMnt && busy.\n");
         Mmsg(jcr->errmsg, _("3605 JobId=%u wants free drive but device %s is busy.\n"), 
            jcr->JobId, dev->print_name());
         queue_reserve_message(jcr);
         return 0;
      }

      /* Check for prefer mounted volumes */
      if (rctx.PreferMountedVols && !dev->vol && dev->is_tape()) {
         Mmsg(jcr->errmsg, _("3606 JobId=%u prefers mounted drives, but drive %s has no Volume.\n"), 
            jcr->JobId, dev->print_name());
         queue_reserve_message(jcr);
         Dmsg0(dbglvl, "failed: want mounted -- no vol\n");
         return 0;                 /* No volume mounted */
      }

      /* Check for exact Volume name match */
      /* ***FIXME*** for Disk, we can accept any volume that goes with this
       *    drive.
       */
      if (rctx.exact_match && rctx.have_volume) {
         bool ok;
         Dmsg5(dbglvl, "PrefMnt=%d exact=%d suitable=%d chgronly=%d any=%d\n",
               rctx.PreferMountedVols, rctx.exact_match, rctx.suitable_device,
               rctx.autochanger_only, rctx.any_drive);
         Dmsg4(dbglvl, "have_vol=%d have=%s resvol=%s want=%s\n",
                  rctx.have_volume, dev->VolHdr.VolumeName, 
                  dev->vol?dev->vol->vol_name:"*none*", rctx.VolumeName);
         ok = strcmp(dev->VolHdr.VolumeName, rctx.VolumeName) == 0 ||
                 (dev->vol && strcmp(dev->vol->vol_name, rctx.VolumeName) == 0);
         if (!ok) {
            Mmsg(jcr->errmsg, _("3607 JobId=%u wants Vol=\"%s\" drive has Vol=\"%s\" on drive %s.\n"), 
               jcr->JobId, rctx.VolumeName, dev->VolHdr.VolumeName, 
               dev->print_name());
            queue_reserve_message(jcr);
            Dmsg3(dbglvl, "not OK: dev have=%s resvol=%s want=%s\n",
                  dev->VolHdr.VolumeName, dev->vol?dev->vol->vol_name:"*none*", rctx.VolumeName);
            return 0;
         }
         if (is_volume_in_use(dcr)) {
            return 0;              /* fail if volume on another drive */
         }
      }
   }

   /* Check for unused autochanger drive */
   if (rctx.autochanger_only && !dev->is_busy() &&
       dev->VolHdr.VolumeName[0] == 0) {
      /* Device is available but not yet reserved, reserve it for us */
      Dmsg1(dbglvl, "OK Res Unused autochanger %s.\n", dev->print_name());
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
         return is_pool_ok(dcr);
      } else if (dev->can_append()) {
         if (is_pool_ok(dcr)) {
            return 1; 
         } else {
            /* Changing pool, unload old tape if any in drive */
            Dmsg0(dbglvl, "OK dev: num_writers=0, not reserved, pool change, unload changer\n");
            unload_autochanger(dcr, 0);
         }
      }
      /* Device is available but not yet reserved, reserve it for us */
      Dmsg1(dbglvl, "OK Dev avail reserved %s\n", dev->print_name());
      bstrncpy(dev->pool_name, dcr->pool_name, sizeof(dev->pool_name));
      bstrncpy(dev->pool_type, dcr->pool_type, sizeof(dev->pool_type));
      return 1;                       /* reserve drive */
   }

   /*
    * Check if the device is in append mode with writers (i.e.
    *  available if pool is the same).
    */
   if (dev->can_append() || dev->num_writers > 0) {
      return is_pool_ok(dcr);
   } else {
      Pmsg1(000, _("Logic error!!!! JobId=%u Should not get here.\n"), (int)jcr->JobId);
      Mmsg(jcr->errmsg, _("3910 JobId=%u Logic error!!!! drive %s Should not get here.\n"),
            jcr->JobId, dev->print_name());
      queue_reserve_message(jcr);
      Jmsg0(jcr, M_FATAL, 0, _("Logic error!!!! Should not get here.\n"));
      return -1;                      /* error, should not get here */
   }
   Mmsg(jcr->errmsg, _("3911 JobId=%u failed reserve drive %s.\n"), 
         jcr->JobId, dev->print_name());
   queue_reserve_message(jcr);
   Dmsg1(dbglvl, "failed: No reserve %s\n", dev->print_name());
   return 0;
}




/*
 * Queue a reservation error or failure message for this jcr
 */
static void queue_reserve_message(JCR *jcr)
{
   int i;   
   alist *msgs;
   char *msg;

   jcr->lock();

   msgs = jcr->reserve_msgs;
   if (!msgs) {
      goto bail_out;
   }
   /*
    * Look for duplicate message.  If found, do
    * not insert
    */
   for (i=msgs->size()-1; i >= 0; i--) {
      msg = (char *)msgs->get(i);
      if (!msg) {
         goto bail_out;
      }
      /* Comparison based on 4 digit message number */
      if (strncmp(msg, jcr->errmsg, 4) == 0) {
         goto bail_out;
      }
   }      
   /* Message unique, so insert it */
   jcr->reserve_msgs->push(bstrdup(jcr->errmsg));

bail_out:
   jcr->unlock();
}

/*
 * Send any reservation messages queued for this jcr
 */
void send_drive_reserve_messages(JCR *jcr, void sendit(const char *msg, int len, void *sarg), void *arg)
{
   int i;
   alist *msgs;
   char *msg;

   jcr->lock();
   msgs = jcr->reserve_msgs;
   if (!msgs || msgs->size() == 0) {
      goto bail_out;
   }
   for (i=msgs->size()-1; i >= 0; i--) {
      msg = (char *)msgs->get(i);
      if (msg) {
         sendit("   ", 3, arg);
         sendit(msg, strlen(msg), arg);
      } else {
         break;
      }
   }

bail_out:
   jcr->unlock();
}

/*
 * Pop and release any reservations messages
 */
static void pop_reserve_messages(JCR *jcr)
{
   alist *msgs;
   char *msg;

   jcr->lock();
   msgs = jcr->reserve_msgs;
   if (!msgs) {
      goto bail_out;
   }
   while ((msg = (char *)msgs->pop())) {
      free(msg);
   }
bail_out:
   jcr->unlock();
}

/*
 * Also called from acquire.c 
 */
void release_reserve_messages(JCR *jcr)
{
   pop_reserve_messages(jcr);
   jcr->lock();
   if (!jcr->reserve_msgs) {
      goto bail_out;
   }
   delete jcr->reserve_msgs;
   jcr->reserve_msgs = NULL;

bail_out:
   jcr->unlock();
}
