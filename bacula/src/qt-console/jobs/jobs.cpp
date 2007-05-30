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
 *   Version $Id: client.cpp 4230 2007-02-21 20:07:37Z kerns $
 *
 *  Jobs Class
 *
 *   Dirk Bartley, March 2007
 *
 */ 

//#include <QAbstractEventDispatcher>
//#include <QMenu>
#include "bat.h"
#include "jobs/jobs.h"

Jobs::Jobs()
{
   setupUi(this);
   m_name = "Jobs";
   pgInitialize();
   QTreeWidgetItem* thisitem = mainWin->getFromHash(this);
   thisitem->setIcon(0,QIcon(QString::fromUtf8(":images/emblem-system.svg")));

   /* mp_treeWidget, Storage Tree Tree Widget inherited from ui_client.h */
   m_populated = false;
   m_checkcurwidget = true;
   m_closeable = false;
   /* add context sensitive menu items specific to this classto the page
    * selector tree. m_contextActions is QList of QActions */
   m_contextActions.append(actionRefreshJobs);
   createContextMenu();
   dockPage();
}

Jobs::~Jobs()
{
}

/*
 * The main meat of the class!!  The function that querries the director and 
 * creates the widgets with appropriate values.
 */
void Jobs::populateTree()
{
   QTreeWidgetItem *jobsItem, *topItem;

   if (!m_console->preventInUseConnect())
      return;
   m_checkcurwidget = false;
   mp_treeWidget->clear();
   m_checkcurwidget = true;
   QStringList headerlist = (QStringList() << "Job Name" << "Pool" << "Messages"
      << "Client" << "Storage" << "Where" << "Level" << "Type" << "FileSet" 
      << "Catalog" << "Enabled");

   topItem = new QTreeWidgetItem(mp_treeWidget);
   topItem->setText(0, "Jobs");
   topItem->setData(0, Qt::UserRole, 0);
   topItem->setExpanded(true);

   mp_treeWidget->setColumnCount(headerlist.count());
   mp_treeWidget->setHeaderLabels(headerlist);

   foreach (QString jobName, m_console->job_list){
      jobsItem = new QTreeWidgetItem(topItem);
      jobsItem->setText(0, jobName);
      //jobsItem->setExpanded(true);

      for (int i=0; i<headerlist.count(); i++)
         jobsItem->setData(i, Qt::UserRole, 1);

      job_defaults job_defs;
      job_defs.job_name = jobName;
      if (m_console->get_job_defaults(job_defs)) {
         int col = 1;
         jobsItem->setText(col++, job_defs.pool_name);
         jobsItem->setText(col++, job_defs.messages_name);
         jobsItem->setText(col++, job_defs.client_name);
         jobsItem->setText(col++, job_defs.store_name);
         jobsItem->setText(col++, job_defs.where);
         jobsItem->setText(col++, job_defs.level);
         jobsItem->setText(col++, job_defs.type);
         jobsItem->setText(col++, job_defs.fileset_name);
         jobsItem->setText(col++, job_defs.catalog_name);
         if (job_defs.enabled) {
            jobsItem->setText(col++, "Yes");
         } else {
            jobsItem->setText(col++, "No");
         }
      }
   }
   /* Resize the columns */
   for(int cnter=0; cnter<headerlist.size(); cnter++) {
      mp_treeWidget->resizeColumnToContents(cnter);
   }
}

/*
 * When the treeWidgetItem in the page selector tree is singleclicked, Make sure
 * The tree has been populated.
 */
void Jobs::PgSeltreeWidgetClicked()
{
   if(!m_populated) {
      populateTree();
      m_populated=true;
   }
}

/*
 * Added to set the context menu policy based on currently active treeWidgetItem
 * signaled by currentItemChanged
 */
void Jobs::treeItemChanged(QTreeWidgetItem *currentwidgetitem, QTreeWidgetItem *previouswidgetitem )
{
   /* m_checkcurwidget checks to see if this is during a refresh, which will segfault */
   if (m_checkcurwidget) {
      /* The Previous item */
      if (previouswidgetitem) { /* avoid a segfault if first time */
         int treedepth = previouswidgetitem->data(0, Qt::UserRole).toInt();
         if (treedepth == 1){
            mp_treeWidget->removeAction(actionConsoleListFiles);
            mp_treeWidget->removeAction(actionConsoleListVolumes);
            mp_treeWidget->removeAction(actionConsoleListNextVolume);
            mp_treeWidget->removeAction(actionConsoleEnableJob);
            mp_treeWidget->removeAction(actionConsoleDisableJob);
            mp_treeWidget->removeAction(actionConsoleCancel);
            mp_treeWidget->removeAction(actionJobListQuery);
         }
      }

      int treedepth = currentwidgetitem->data(0, Qt::UserRole).toInt();
      if (treedepth == 1){
         /* set a hold variable to the client name in case the context sensitive
          * menu is used */
         m_currentlyselected=currentwidgetitem->text(0);
         mp_treeWidget->addAction(actionConsoleListFiles);
         mp_treeWidget->addAction(actionConsoleListVolumes);
         mp_treeWidget->addAction(actionConsoleListNextVolume);
         mp_treeWidget->addAction(actionConsoleEnableJob);
         mp_treeWidget->addAction(actionConsoleDisableJob);
         mp_treeWidget->addAction(actionConsoleCancel);
         mp_treeWidget->addAction(actionJobListQuery);
      }
   }
}

/* 
 * Setup a context menu 
 * Made separate from populate so that it would not create context menu over and
 * over as the tree is repopulated.
 */
void Jobs::createContextMenu()
{
   mp_treeWidget->setContextMenuPolicy(Qt::ActionsContextMenu);
   mp_treeWidget->addAction(actionRefreshJobs);
   connect(mp_treeWidget, SIGNAL(
           currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)),
           this, SLOT(treeItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)));
   /* connect to the action specific to this pages class */
   connect(actionRefreshJobs, SIGNAL(triggered()), this,
                SLOT(populateTree()));
   connect(actionConsoleListFiles, SIGNAL(triggered()), this, SLOT(consoleListFiles()));
   connect(actionConsoleListVolumes, SIGNAL(triggered()), this, SLOT(consoleListVolume()));
   connect(actionConsoleListNextVolume, SIGNAL(triggered()), this, SLOT(consoleListNextVolume()));
   connect(actionConsoleEnableJob, SIGNAL(triggered()), this, SLOT(consoleEnable()));
   connect(actionConsoleDisableJob, SIGNAL(triggered()), this, SLOT(consoleDisable()));
   connect(actionConsoleCancel, SIGNAL(triggered()), this, SLOT(consoleCancel()));
   connect(actionJobListQuery, SIGNAL(triggered()), this, SLOT(listJobs()));
}

/*
 * Virtual function which is called when this page is visible on the stack
 */
void Jobs::currentStackItem()
{
   populateTree();
   if(!m_populated) {
      /* Create the context menu for the client tree */
      m_populated=true;
   }
}

/*
 * The following functions are slots responding to users clicking on the context
 * sensitive menu
 */

void Jobs::consoleListFiles()
{
   QString cmd("list files job=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
}

void Jobs::consoleListVolume()
{
   QString cmd("list volumes job=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
}

void Jobs::consoleListNextVolume()
{
   QString cmd("list nextvolume job=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
}

void Jobs::consoleEnable()
{
   QString cmd("enable job=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
}

void Jobs::consoleDisable()
{
   QString cmd("disable job=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
}

void Jobs::consoleCancel()
{
   QString cmd("cancel job=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
}

void Jobs::listJobs()
{
   printf("In Jobs::listJobs\n");
   QTreeWidgetItem *parentItem = mainWin->getFromHash(this);
   mainWin->createPageJobList("", "", m_currentlyselected, parentItem);
}
