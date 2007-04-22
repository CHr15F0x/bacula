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
 *   Version $Id: fileset.cpp 4230 2007-02-21 20:07:37Z kerns $
 *
 *  FileSet Class
 *
 *   Dirk Bartley, March 2007
 *
 */ 

#include <QAbstractEventDispatcher>
#include <QMenu>
#include "bat.h"
#include "fileset/fileset.h"

FileSet::FileSet()
{
   setupUi(this);
   m_name = "FileSets";
   pgInitialize();

   /* mp_treeWidget, FileSet Tree Tree Widget inherited from ui_fileset.h */
   m_populated = false;
   m_checkcurwidget = true;
   m_closeable = false;
   setTitle();
}

FileSet::~FileSet()
{
}

/*
 * The main meat of the class!!  The function that querries the director and 
 * creates the widgets with appropriate values.
 */
void FileSet::populateTree()
{
   QTreeWidgetItem *filesetItem, *topItem;

   m_checkcurwidget = false;
   mp_treeWidget->clear();
   m_checkcurwidget = true;

   QStringList headerlist = (QStringList() << "FileSet Name" << "FileSet Id"
       << "Create Time");

   topItem = new QTreeWidgetItem(mp_treeWidget);
   topItem->setText(0, "FileSet");
   topItem->setData(0, Qt::UserRole, 0);
   topItem->setExpanded(true);

   mp_treeWidget->setColumnCount(headerlist.count());
   mp_treeWidget->setHeaderLabels(headerlist);
   /* This could be a log item */
   //printf("In FileSet::populateTree()\n");

   foreach(QString filesetName, m_console->fileset_list){
      filesetItem = new QTreeWidgetItem(topItem);
      filesetItem->setText(0, filesetName);
      filesetItem->setData(0, Qt::UserRole, 1);
      filesetItem->setExpanded(true);

      /* Set up query QString and header QStringList */
      QString query("");
      query += "SELECT FileSet AS Name, FileSetId AS Id, CreateTime"
           " FROM FileSet"
           " WHERE ";
      query += " FileSet='" + filesetName + "'";
      query += " ORDER BY FileSet";

      QStringList results;
      /* This could be a log item */
      //printf("FileSet query cmd : %s\n",query.toUtf8().data());
      if (m_console->sql_cmd(query, results)) {
         int resultCount = results.count();
         if (resultCount == 1){
            QString resultline;
            QString field;
            QStringList fieldlist;
            /* there will only be one of these */
            foreach (resultline, results) {
               fieldlist = resultline.split("\t");
               int index = 0;
               /* Iterate through fields in the record */
               foreach (field, fieldlist) {
                  field = field.trimmed();  /* strip leading & trailing spaces */
                  filesetItem->setData(index+1, Qt::UserRole, 1);
                  /* Put media fields under the pool tree item */
                  filesetItem->setData(index+1, Qt::UserRole, 1);
                  filesetItem->setText(index+1, field);
                  index++;
               }
            }
         }
      }
   }
   /* Resize the columns */
   for(int cnter=1; cnter<headerlist.size(); cnter++) {
      mp_treeWidget->resizeColumnToContents(cnter);
   }

}

/*
 * When the treeWidgetItem in the page selector tree is singleclicked, Make sure
 * The tree has been populated.
 */
void FileSet::PgSeltreeWidgetClicked()
{
   if(!m_populated) {
      populateTree();
      createContextMenu();
      m_populated=true;
   }
}

/*
 * Added to set the context menu policy based on currently active treeWidgetItem
 * signaled by currentItemChanged
 */
void FileSet::treeItemChanged(QTreeWidgetItem *currentwidgetitem, QTreeWidgetItem *previouswidgetitem )
{
   /* m_checkcurwidget checks to see if this is during a refresh, which will segfault */
   if (m_checkcurwidget) {
      /* The Previous item */
      if (previouswidgetitem) { /* avoid a segfault if first time */
         int treedepth = previouswidgetitem->data(0, Qt::UserRole).toInt();
         if (treedepth == 1){
            mp_treeWidget->removeAction(actionStatusFileSetInConsole);
         }
      }

      int treedepth = currentwidgetitem->data(0, Qt::UserRole).toInt();
      if (treedepth == 1){
         /* set a hold variable to the fileset name in case the context sensitive
          * menu is used */
         m_currentlyselected=currentwidgetitem->text(0);
         mp_treeWidget->addAction(actionStatusFileSetInConsole);
      }
   }
}

/* 
 * Setup a context menu 
 * Made separate from populate so that it would not create context menu over and
 * over as the tree is repopulated.
 */
void FileSet::createContextMenu()
{
   mp_treeWidget->setContextMenuPolicy(Qt::ActionsContextMenu);
   mp_treeWidget->addAction(actionRefreshFileSet);
   connect(mp_treeWidget, SIGNAL(
           currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)),
           this, SLOT(treeItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)));
   /* connect to the action specific to this pages class */
   connect(actionRefreshFileSet, SIGNAL(triggered()), this,
                SLOT(populateTree()));
   connect(actionStatusFileSetInConsole, SIGNAL(triggered()), this,
                SLOT(consoleStatusFileSet()));
}

/*
 * Function responding to actionListJobsofFileSet which calls mainwin function
 * to create a window of a list of jobs of this fileset.
 */
void FileSet::consoleStatusFileSet()
{
   QString cmd("status fileset=");
   cmd += m_currentlyselected;
   consoleCommand(cmd);
//   m_console->write_dir(cmd.toUtf8().data());
//   m_console->displayToPrompt();
   /* Bring this directors console to the front of the stack */
//   mainWin->treeWidget->setCurrentItem(mainWin->getFromHash(m_console));
}

/*
 * Virtual function which is called when this page is visible on the stack
 */
void FileSet::currentStackItem()
{
   if(!m_populated) {
      populateTree();
      /* add context sensitive menu items specific to this classto the page
       * selector tree. m_m_contextActions is QList of QActions, so this is 
       * only done once with the first population. */
      m_contextActions.append(actionRefreshFileSet);
      /* Create the context menu for the fileset tree */
      createContextMenu();
      m_populated=true;
   }
}
