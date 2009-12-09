/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.

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

   Bacula® is a registered trademark of Kern Sibbald.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
/*
 *   Volume management functions for Storage Daemon
 *
 *   Kern Sibbald, MM
 *
 *   Split from reserve.c October 2008
 *
 *   Version $Id: reserve.c 7380 2008-07-14 10:42:59Z kerns $
 *
 */

#include "bacula.h"
#include "stored.h"

const int dbglvl =  150;

static dlist *vol_list = NULL;
static brwlock_t vol_list_lock;
static dlist *read_vol_list = NULL;
static pthread_mutex_t read_vol_lock = PTHREAD_MUTEX_INITIALIZER;

/* Forward referenced functions */
static void free_vol_item(VOLRES *vol);
static VOLRES *new_vol_item(DCR *dcr, const char *VolumeName);
static void debug_list_volumes(const char *imsg);

/*
 * For append volumes the key is the VolumeName.
 */
static int my_compare(void *item1, void *item2)
{
   return strcmp(((VOLRES *)item1)->vol_name, ((VOLRES *)item2)->vol_name);
}

/*
 * For read volumes the key is JobId, VolumeName.
 */
static int read_compare(void *item1, void *item2)
{
   VOLRES *vol1 = (VOLRES *)item1;
   VOLRES *vol2 = (VOLRES *)item2;

   if (vol1->get_jobid() == vol2->get_jobid()) {
      return strcmp(vol1->vol_name, vol2->vol_name);
   }
   if (vol1->get_jobid() < vol2->get_jobid()) {
      return -1;
   }
   return 1;
}


bool is_vol_list_empty() 
{
   return vol_list->empty();
}

int vol_list_lock_count = 0;

/*
 *  Initialized the main volume list. Note, we are using a recursive lock.
 */
void init_vol_list_lock()
{
   int errstat;
   if ((errstat=rwl_init(&vol_list_lock, PRIO_SD_VOL_LIST)) != 0) {
      berrno be;
      Emsg1(M_ABORT, 0, _("Unable to initialize volume list lock. ERR=%s\n"),
            be.bstrerror(errstat));
   }
}

void term_vol_list_lock()
{
   rwl_destroy(&vol_list_lock);
}

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

void lock_read_volumes()
{
   P(read_vol_lock);
}

void unlock_read_volumes()
{
   V(read_vol_lock);
}

/*
 * Add a volume to the read list.
 * Note, we use VOLRES because it simplifies the code
 *   even though, the only part of VOLRES that we need is
 *   the volume name.  The same volume may be in the list
 *   multiple times, but each one is distinguished by the 
 *   JobId.  We use JobId, VolumeName as the key.
 * We can get called multiple times for the same volume because
 *   when parsing the bsr, the volume name appears multiple times.
 */
void add_read_volume(JCR *jcr, const char *VolumeName)
{
   VOLRES *nvol, *vol;

   lock_read_volumes();
   nvol = new_vol_item(NULL, VolumeName);
   nvol->set_jobid(jcr->JobId);
   vol = (VOLRES *)read_vol_list->binary_insert(nvol, read_compare);
   if (vol != nvol) {
      free_vol_item(nvol);
      Dmsg2(dbglvl, "read_vol=%s JobId=%d already in list.\n", VolumeName, jcr->JobId);
   } else {
      Dmsg2(dbglvl, "add read_vol=%s JobId=%d\n", VolumeName, jcr->JobId);
   }
   unlock_read_volumes();
}

/*
 * Remove a given volume name from the read list.
 */
void remove_read_volume(JCR *jcr, const char *VolumeName)
{
   VOLRES vol, *fvol;
   lock_read_volumes();
   vol.vol_name = bstrdup(VolumeName);
   vol.set_jobid(jcr->JobId);
   fvol = (VOLRES *)read_vol_list->binary_search(&vol, read_compare);
   free(vol.vol_name);
   if (fvol) {
      Dmsg3(dbglvl, "remove_read_vol=%s JobId=%d found=%d\n", VolumeName, jcr->JobId, fvol!=NULL);
   }
   debug_list_volumes("remove_read_volume");
   if (fvol) {
      read_vol_list->remove(fvol);
      free_vol_item(fvol);
   }
   unlock_read_volumes();
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
         Mmsg(msg, "List %s: %s in_use=%d on device %s\n", imsg, 
              vol->vol_name, vol->is_in_use(), vol->dev->print_name());
      } else {
         Mmsg(msg, "List %s: %s in_use=%d no dev\n", imsg, vol->vol_name, 
              vol->is_in_use());
      }
      Dmsg1(dbglvl, "%s", msg.c_str());
   }

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
         len = Mmsg(msg, "    Reader=%d writers=%d devres=%d volinuse=%d\n", 
            dev->can_read()?1:0, dev->num_writers, dev->num_reserved(),   
            vol->is_in_use());
         sendit(msg.c_str(), len, arg);
      } else {
         len = Mmsg(msg, "%s no device. volinuse= %d\n", vol->vol_name, 
            vol->is_in_use());
         sendit(msg.c_str(), len, arg);
      }
   }
   unlock_volumes();

   lock_read_volumes();
   foreach_dlist(vol, read_vol_list) {
      len = Mmsg(msg, "%s read volume JobId=%d\n", vol->vol_name, 
            vol->get_jobid());
      sendit(msg.c_str(), len, arg);
   }
   unlock_read_volumes();

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
   if (dcr) {
      vol->dev = dcr->dev;
      Dmsg3(dbglvl, "new Vol=%s at %p dev=%s\n",
            VolumeName, vol->vol_name, vol->dev->print_name());
   }
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
 *       the volume (in use, mounted)
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

   if (job_canceled(dcr->jcr)) {
      return NULL;
   }
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
      Dmsg4(dbglvl, "Vol attached=%s, newvol=%s volinuse=%d on %s\n",
         vol->vol_name, VolumeName, vol->is_in_use(), dev->print_name());
      /*
       * Make sure we don't remove the current volume we are inserting
       *  because it was probably inserted by another job, or it
       *  is not being used and is marked as not reserved.
       */
      if (strcmp(vol->vol_name, VolumeName) == 0) {
         Dmsg2(dbglvl, "=== set reserved vol=%s dev=%s\n", VolumeName,
               vol->dev->print_name());
         goto get_out;                  /* Volume already on this device */
      } else {
         /* Don't release a volume if it was reserved by someone other than us */
         if (vol->is_in_use() && !dcr->reserved_volume) { 
            Dmsg1(dbglvl, "Cannot free vol=%s. It is reserved.\n", vol->vol_name);
            vol = NULL;                  /* vol in use */
            goto get_out;
         }
         Dmsg2(dbglvl, "reserve_vol free vol=%s at %p\n", vol->vol_name, vol->vol_name);
         /* If old Volume is still mounted, must unload it */
         if (strcmp(vol->vol_name, dev->VolHdr.VolumeName) == 0) {
            Dmsg0(50, "set_unload\n");
            dev->set_unload();          /* have to unload current volume */
         }
         free_volume(dev);              /* Release old volume entry */
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

      if (vol->dev) {
         Dmsg2(dbglvl, "dev=%s vol->dev=%s\n", dev->print_name(), vol->dev->print_name());
      }
         
      /*
       * Check if we are trying to use the Volume on a different drive
       *  dev      is our device
       *  vol->dev is where the Volume we want is
       */
      if (dev != vol->dev) {
         /* Caller wants to switch Volume to another device */
         if (!vol->dev->is_busy() && !vol->is_swapping()) {
            int32_t slot;
            Dmsg3(dbglvl, "==== Swap vol=%s from dev=%s to %s\n", 
               VolumeName, vol->dev->print_name(), dev->print_name());
            free_volume(dev);            /* free any volume attached to our drive */
            Dmsg0(50, "set_unload\n");
            dev->set_unload();           /* Unload any volume that is on our drive */
            dcr->dev = vol->dev;         /* temp point to other dev */
            slot = get_autochanger_loaded_slot(dcr);  /* get slot on other drive */
            dcr->dev = dev;              /* restore dev */
            vol->set_slot(slot);         /* save slot */
            vol->dev->set_unload();      /* unload the other drive */
            vol->set_swapping();         /* swap from other drive */
            dev->swap_dev = vol->dev;    /* remember to get this vol */
            dev->set_load();             /* then reload on our drive */
            vol->dev->vol = NULL;        /* remove volume from other drive */
            vol->dev = dev;              /* point the Volume at our drive */
            dev->vol = vol;              /* point our drive at the Volume */
         } else {
            Dmsg5(dbglvl, "==== Swap not possible Vol busy=%d swap=%d vol=%s from dev=%s to %s\n", 
               vol->dev->is_busy(), vol->is_swapping(),
               VolumeName, vol->dev->print_name(), dev->print_name());
            if (vol->is_swapping() && dev->swap_dev) {
               Dmsg2(dbglvl, "Swap vol=%s dev=%s\n", vol->vol_name, dev->swap_dev->print_name());
            } else {
               Dmsg1(dbglvl, "swap_dev=%p\n", dev->swap_dev);
            }
            debug_list_volumes("failed swap");
            vol = NULL;                  /* device busy */
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
      Dmsg2(dbglvl, "=== set in_use. vol=%s dev=%s\n", vol->vol_name,
            vol->dev->print_name());
      vol->set_in_use();
      dcr->reserved_volume = true;
      bstrncpy(dcr->VolumeName, vol->vol_name, sizeof(dcr->VolumeName));
   }
   debug_list_volumes("end new volume");
   unlock_volumes();
   return vol;
}

/* 
 * Switch from current device to given device  
 *   (not yet used) 
 */
#ifdef xxx
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

// dcr->set_reserved();

   dev->dunlock();
}
#endif

/*
 * Search for a Volume name in the Volume list.
 *
 *  Returns: VOLRES entry on success
 *           NULL if the Volume is not in the list
 */
VOLRES *find_volume(const char *VolumeName) 
{
   VOLRES vol, *fvol;

   if (vol_list->empty()) {
      return NULL;
   }
   /* Do not lock reservations here */
   lock_volumes();
   vol.vol_name = bstrdup(VolumeName);
   fvol = (VOLRES *)vol_list->binary_search(&vol, my_compare);
   free(vol.vol_name);
   Dmsg2(dbglvl, "find_vol=%s found=%d\n", VolumeName, fvol!=NULL);
   debug_list_volumes("find_volume");
   unlock_volumes();
   return fvol;
}

/*
 * Search for a Volume name in the read Volume list.
 *
 *  Returns: VOLRES entry on success
 *           NULL if the Volume is not in the list
 */
static VOLRES *find_read_volume(const char *VolumeName) 
{
   VOLRES vol, *fvol;

   if (read_vol_list->empty()) {
      Dmsg0(dbglvl, "find_read_vol: read_vol_list empty.\n");
      return NULL;
   }
   /* Do not lock reservations here */
   lock_read_volumes();
   vol.vol_name = bstrdup(VolumeName);
   /* Note, we do want a simple my_compare on volume name only here */
   fvol = (VOLRES *)read_vol_list->binary_search(&vol, my_compare);
   free(vol.vol_name);
   Dmsg2(dbglvl, "find_read_vol=%s found=%d\n", VolumeName, fvol!=NULL);
   unlock_read_volumes();
   return fvol;
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
      Dmsg1(dbglvl, "=== clear in_use vol=%s\n", dev->vol->vol_name);
      dev->vol->clear_in_use();
      debug_list_volumes("swapping vol cannot free_volume");
      return false;
   }

   /*  
    * If this is a tape, we do not free the volume, rather we wait
    *  until the autoloader unloads it, or until another tape is
    *  explicitly read in this drive. This allows the SD to remember
    *  where the tapes are or last were.
    */
   Dmsg4(dbglvl, "=== set not reserved vol=%s num_writers=%d dev_reserved=%d dev=%s\n",
      dev->vol->vol_name, dev->num_writers, dev->num_reserved(), dev->print_name());
   Dmsg1(dbglvl, "=== clear in_use vol=%s\n", dev->vol->vol_name);
   dev->vol->clear_in_use();
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
   /* Don't free a volume while it is being swapped */
   if (!vol->is_swapping()) {
      Dmsg1(dbglvl, "=== clear in_use vol=%s\n", vol->vol_name);
      dev->vol = NULL;
      vol_list->remove(vol);
      Dmsg2(dbglvl, "=== remove volume %s dev=%s\n", vol->vol_name, dev->print_name());
      free_vol_item(vol);
      debug_list_volumes("free_volume");
   } else {
      Dmsg1(dbglvl, "=== cannot clear swapping vol=%s\n", vol->vol_name);
   }
   unlock_volumes();
   return true;
}

      
/* Create the Volume list */
void create_volume_lists()
{
   VOLRES *vol = NULL;
   if (vol_list == NULL) {
      vol_list = New(dlist(vol, &vol->link));
   }
   if (read_vol_list == NULL) {
      read_vol_list = New(dlist(vol, &vol->link));
   }
}

/*
 * Free normal append volumes list
 */
static void free_volume_list()
{
   VOLRES *vol;
   if (vol_list) {
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
}

/* Release all Volumes from the list */
void free_volume_lists()
{
   VOLRES *vol;

   free_volume_list();           /* normal append list */

   if (read_vol_list) {
      lock_read_volumes();
      foreach_dlist(vol, read_vol_list) {
         if (vol->dev) {
            Dmsg2(dbglvl, "free read_vol_list Volume=%s dev=%s\n", vol->vol_name, vol->dev->print_name());
         } else {
            Dmsg1(dbglvl, "free read_vol_list Volume=%s No dev\n", vol->vol_name);
         }
         free(vol->vol_name);
         vol->vol_name = NULL;
      }
      delete read_vol_list;
      read_vol_list = NULL;
      unlock_read_volumes();
   }
}

/* 
 * Determine if caller can write on volume
 */
bool DCR::can_i_write_volume()
{
   VOLRES *vol;

   vol = find_read_volume(VolumeName);
   if (vol) {
      Dmsg1(100, "Found in read list; cannot write vol=%s\n", VolumeName);
      return false;
   }
   return can_i_use_volume();
}

/*
 * Determine if caller can read or write volume
 */
bool DCR::can_i_use_volume()
{
   bool rtn = true;
   VOLRES *vol;

   if (job_canceled(jcr)) {
      return false;
   }
   lock_volumes();
   vol = find_volume(VolumeName);
   if (!vol) {
      Dmsg1(dbglvl, "Vol=%s not in use.\n", VolumeName);
      goto get_out;                   /* vol not in list */
   }
   ASSERT(vol->dev != NULL);

   if (dev == vol->dev) {        /* same device OK */
      Dmsg1(dbglvl, "Vol=%s on same dev.\n", VolumeName);
      goto get_out;
   } else {
      Dmsg3(dbglvl, "Vol=%s on %s we have %s\n", VolumeName,
            vol->dev->print_name(), dev->print_name());
   }
   /* ***FIXME*** check this ... */
   if (!vol->dev->is_busy()) {
      Dmsg2(dbglvl, "Vol=%s dev=%s not busy.\n", VolumeName, vol->dev->print_name());
      goto get_out;
   } else {
      Dmsg2(dbglvl, "Vol=%s dev=%s busy.\n", VolumeName, vol->dev->print_name());
   }
   Dmsg2(dbglvl, "Vol=%s in use by %s.\n", VolumeName, vol->dev->print_name());
   rtn = false;

get_out:
   unlock_volumes();
   return rtn;

}

/*  
 * Create a temporary copy of the volume list.  We do this,
 *   to avoid having the volume list locked during the
 *   call to reserve_device(), which would cause a deadlock.
 * Note, we may want to add an update counter on the vol_list
 *   so that if it is modified while we are traversing the copy
 *   we can take note and act accordingly (probably redo the 
 *   search at least a few times).
 */
dlist *dup_vol_list(JCR *jcr)
{
   dlist *temp_vol_list;
   VOLRES *vol = NULL;

   lock_volumes();
   Dmsg0(dbglvl, "lock volumes\n");                           

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
   return temp_vol_list;
}

/*
 * Free the specified temp list.
 */
void free_temp_vol_list(dlist *temp_vol_list)
{
   dlist *save_vol_list;
   
   lock_volumes();
   save_vol_list = vol_list;
   vol_list = temp_vol_list;
   free_volume_list();                  /* release temp_vol_list */
   vol_list = save_vol_list;
   Dmsg0(dbglvl, "deleted temp vol list\n");
   Dmsg0(dbglvl, "unlock volumes\n");
   unlock_volumes();
   debug_list_volumes("after free temp table");
}
