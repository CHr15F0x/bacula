/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2000-2007 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation plus additions
   that are listed in the file LICENSE.

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
 *   Version $Id: medialist.cpp 4230 2007-02-21 20:07:37Z kerns $
 *
 *  MediaList Class
 *
 *   Dirk Bartley, March 2007
 *
 */ 

#include <QAbstractEventDispatcher>
#include <QMenu>
#include "bat.h"
#include "medialist.h"
#include "mediaedit/mediaedit.h"
#include "joblist/joblist.h"
#include "relabel/relabel.h"
#include "run/run.h"

MediaList::MediaList()
{
   setupUi(this);
   m_name = "Media";
   pgInitialize();
   QTreeWidgetItem* thisitem = mainWin->getFromHash(this);
   thisitem->setIcon(0,QIcon(QString::fromUtf8(":images/cartridge.png")));

   /* mp_treeWidget, Storage Tree Tree Widget inherited from ui_medialist.h */
   m_populated = false;
   m_checkcurwidget = true;
   m_closeable = false;
   /* add context sensitive menu items specific to this classto the page
    * selector tree. m_contextActions is QList of QActions */
   m_contextActions.append(actionRefreshMediaList);
}

MediaList::~MediaList()
{
}

/*
 * The main meat of the class!!  The function that querries the director and 
 * creates the widgets with appropriate values.
 */
void MediaList::populateTree()
{
   QTreeWidgetItem *mediatreeitem, *pooltreeitem, *topItem;

   if (!m_console->preventInUseConnect())
       return;

   QStringList headerlist = (QStringList()
      << "Volume Name" << "Id" << "Status" << "Enabled"
      << "Bytes" << "Files" << "Jobs" << "Retention" 
      << "Media Type" << "Slot" << "Last Written");
   int statusIndex = headerlist.indexOf("Status");

   m_checkcurwidget = false;
   mp_treeWidget->clear();
   m_checkcurwidget = true;
   mp_treeWidget->setColumnCount(headerlist.count());
   topItem = new QTreeWidgetItem(mp_treeWidget);
   topItem->setText(0, "Pools");
   topItem->setData(0, Qt::UserRole, 0);
   topItem->setExpanded(true);
   
   mp_treeWidget->setHeaderLabels(headerlist);

   QString query;

   foreach (QString pool_listItem, m_console->pool_list) {
      pooltreeitem = new QTreeWidgetItem(topItem);
      pooltreeitem->setText(0, pool_listItem);
      pooltreeitem->setData(0, Qt::UserRole, 1);
      pooltreeitem->setExpanded(true);

      query =  "SELECT Media.VolumeName AS Media, "
         " Media.MediaId AS Id, Media.VolStatus AS VolStatus,"
         " Media.Enabled AS Enabled, Media.VolBytes AS Bytes,"
         " Media.VolFiles AS FileCount, Media.VolJobs AS JobCount,"
         " Media.VolRetention AS VolumeRetention, Media.MediaType AS MediaType,"
         " Media.Slot AS Slot, Media.LastWritten AS LastWritten"
         " FROM Media, Pool"
         " WHERE Media.PoolId=Pool.PoolId";
      query += " AND Pool.Name='" + pool_listItem + "'";
      query += " ORDER BY Media";
   
      if (mainWin->m_sqlDebug) {
         Pmsg1(000, "MediaList query cmd : %s\n",query.toUtf8().data());
      }
      QStringList results;
      if (m_console->sql_cmd(query, results)) {
         QString field;
         QStringList fieldlist;

         /* Iterate through the lines of results. */
         foreach (QString resultline, results) {
            fieldlist = resultline.split("\t");
            int index = 0;
            mediatreeitem = new QTreeWidgetItem(pooltreeitem);

            /* Iterate through fields in the record */
            foreach (field, fieldlist) {
               field = field.trimmed();  /* strip leading & trailing spaces */
               if (field != "") {
                  mediatreeitem->setData(index, Qt::UserRole, 2);
                  mediatreeitem->setData(index, Qt::UserRole, 2);
                  mediatreeitem->setText(index, field);
                  if (index == statusIndex) {
                     setStatusColor(mediatreeitem, field, index);
                  }
               }
               index++;
            } /* foreach field */
         } /* foreach resultline */
      } /* if results from query */
   } /* foreach pool_listItem */
   /* Resize the columns */
   for(int cnter=0; cnter<headerlist.count(); cnter++) {
      mp_treeWidget->resizeColumnToContents(cnter);
   }
}

void MediaList::setStatusColor(QTreeWidgetItem *item, QString &field, int &index)
{
   if (field == "Append" ) {
      item->setBackground(index, Qt::green);
   } else if (field == "Error") {
      item->setBackground(index, Qt::red);
   } else if ((field == "Used") || ("Full")){
      item->setBackground(index, Qt::yellow);
   }
}

/*
 * Called from the signal of the context sensitive menu!
 */
void MediaList::editVolume()
{
   MediaEdit* edit = new MediaEdit(m_console, m_currentVolumeId);
   connect(edit, SIGNAL(destroyed()), this, SLOT(populateTree()));
}

/*
 * Called from the signal of the context sensitive menu!
 */
void MediaList::showJobs()
{
   QString emptyclient("");
   QTreeWidgetItem *parentItem = mainWin->getFromHash(this);
   mainWin->createPageJobList(m_currentVolumeName, emptyclient, parentItem);
}

/*
 * When the treeWidgetItem in the page selector tree is singleclicked, Make sure
 * The tree has been populated.
 */
void MediaList::PgSeltreeWidgetClicked()
{
   if (!m_populated) {
      populateTree();
      createContextMenu();
      m_populated=true;
   }
}

/*
 * Added to set the context menu policy based on currently active treeWidgetItem
 * signaled by currentItemChanged
 */
void MediaList::treeItemChanged(QTreeWidgetItem *currentwidgetitem, QTreeWidgetItem *previouswidgetitem )
{
   /* m_checkcurwidget checks to see if this is during a refresh, which will segfault */
   if (m_checkcurwidget) {
      /* The Previous item */
      if (previouswidgetitem) { /* avoid a segfault if first time */
         int treedepth = previouswidgetitem->data(0, Qt::UserRole).toInt();
         if (treedepth == 2){
            mp_treeWidget->removeAction(actionEditVolume);
            mp_treeWidget->removeAction(actionListJobsOnVolume);
            mp_treeWidget->removeAction(actionDeleteVolume);
            mp_treeWidget->removeAction(actionPruneVolume);
            mp_treeWidget->removeAction(actionPurgeVolume);
            mp_treeWidget->removeAction(actionRelabelVolume);
         }
      }

      int treedepth = currentwidgetitem->data(0, Qt::UserRole).toInt();
      if (treedepth == 2){
         m_currentVolumeName=currentwidgetitem->text(0);
         m_currentVolumeId=currentwidgetitem->text(1);
         mp_treeWidget->addAction(actionEditVolume);
         mp_treeWidget->addAction(actionListJobsOnVolume);
         mp_treeWidget->addAction(actionDeleteVolume);
         mp_treeWidget->addAction(actionPruneVolume);
         mp_treeWidget->addAction(actionPurgeVolume);
         mp_treeWidget->addAction(actionRelabelVolume);
      }
   }
}

/* 
 * Setup a context menu 
 * Made separate from populate so that it would not create context menu over and
 * over as the tree is repopulated.
 */
void MediaList::createContextMenu()
{
   mp_treeWidget->setContextMenuPolicy(Qt::ActionsContextMenu);
   mp_treeWidget->addAction(actionRefreshMediaList);
   connect(actionEditVolume, SIGNAL(triggered()), this, SLOT(editVolume()));
   connect(actionListJobsOnVolume, SIGNAL(triggered()), this, SLOT(showJobs()));
   connect(actionDeleteVolume, SIGNAL(triggered()), this, SLOT(deleteVolume()));
   connect(actionPurgeVolume, SIGNAL(triggered()), this, SLOT(purgeVolume()));
   connect(actionPruneVolume, SIGNAL(triggered()), this, SLOT(pruneVolume()));
   connect(actionRelabelVolume, SIGNAL(triggered()), this, SLOT(relabelVolume()));
   connect(mp_treeWidget, SIGNAL(
           currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)),
           this, SLOT(treeItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)));
   /* connect to the action specific to this pages class */
   connect(actionRefreshMediaList, SIGNAL(triggered()), this,
                SLOT(populateTree()));
}

/*
 * Virtual function which is called when this page is visible on the stack
 */
void MediaList::currentStackItem()
{
   if(!m_populated) {
      populateTree();
      /* Create the context menu for the medialist tree */
      createContextMenu();
      m_populated=true;
   }
}

/*
 * Called from the signal of the context sensitive menu to delete a volume!
 */
void MediaList::deleteVolume()
{
   if (QMessageBox::warning(this, tr("Bat"),
      tr("Are you sure you want to delete??  !!!.\n"
"This delete command is used to delete a Volume record and all associated catalog"
" records that were created. This command operates only on the Catalog"
" database and has no effect on the actual data written to a Volume. This"
" command can be dangerous and we strongly recommend that you do not use"
" it unless you know what you are doing.  All Jobs and all associated"
" records (File and JobMedia) will be deleted from the catalog."
      "Press OK to proceed with delete operation.?"),
      QMessageBox::Ok | QMessageBox::Cancel)
      == QMessageBox::Cancel) { return; }

   QString cmd("delete volume=");
   cmd += m_currentVolumeName;
   consoleCommand(cmd);
}

/*
 * Called from the signal of the context sensitive menu to purge!
 */
void MediaList::purgeVolume()
{
   if (QMessageBox::warning(this, tr("Bat"),
      tr("Are you sure you want to purge ??  !!!.\n"
"The Purge command will delete associated Catalog database records from Jobs and"
" Volumes without considering the retention period. Purge  works only on the"
" Catalog database and does not affect data written to Volumes. This command can"
" be dangerous because you can delete catalog records associated with current"
" backups of files, and we recommend that you do not use it unless you know what"
" you are doing.\n"
      "Press OK to proceed with the purge operation?"),
      QMessageBox::Ok | QMessageBox::Cancel)
      == QMessageBox::Cancel) { return; }

   QString cmd("purge volume=");
   cmd += m_currentVolumeName;
   consoleCommand(cmd);
   populateTree();
}

/*
 * Called from the signal of the context sensitive menu to prune!
 */
void MediaList::pruneVolume()
{
   new prunePage(m_currentVolumeName, "");
}

/*
 * Called from the signal of the context sensitive menu to relabel!
 */
void MediaList::relabelVolume()
{
   setConsoleCurrent();
   new relabelDialog(m_console, m_currentVolumeName);
}
