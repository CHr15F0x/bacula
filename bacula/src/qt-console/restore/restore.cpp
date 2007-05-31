/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2007-2007 Free Software Foundation Europe e.V.

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
 *   Version $Id$
 *
 *  Restore Class 
 *
 *   Kern Sibbald, February MMVII
 *
 */ 

#include "bat.h"
#include "restore.h"

restorePage::restorePage()
{
   QStringList titles;

   setupUi(this);
   m_name = "Restore Select";
   pgInitialize();
   QTreeWidgetItem* thisitem = mainWin->getFromHash(this);
   thisitem->setIcon(0,QIcon(QString::fromUtf8(":images/restore.png")));

   m_console->notify(false);          /* this should already be off */
   m_closeable = true;

   connect(fileWidget, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)), 
           this, SLOT(fileDoubleClicked(QTreeWidgetItem *, int)));
   connect(directoryWidget, SIGNAL(
           currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)),
           this, SLOT(directoryItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)));
   connect(upButton, SIGNAL(pressed()), this, SLOT(upButtonPushed()));
   connect(markButton, SIGNAL(pressed()), this, SLOT(markButtonPushed()));
   connect(unmarkButton, SIGNAL(pressed()), this, SLOT(unmarkButtonPushed()));
   connect(okButton, SIGNAL(pressed()), this, SLOT(okButtonPushed()));
   connect(cancelButton, SIGNAL(pressed()), this, SLOT(cancelButtonPushed()));

   fileWidget->setContextMenuPolicy(Qt::ActionsContextMenu);
   fileWidget->addAction(actionMark);
   fileWidget->addAction(actionUnMark);
   connect(actionMark, SIGNAL(triggered()), this, SLOT(markButtonPushed()));
   connect(actionUnMark, SIGNAL(triggered()), this, SLOT(unmarkButtonPushed()));

   setFont(m_console->get_font());
   m_console->displayToPrompt();

   titles << "Mark" << "File" << "Mode" << "User" << "Group" << "Size" << "Date";
   fileWidget->setHeaderLabels(titles);

   get_cwd();

   readSettings();
   fillDirectory();
   dockPage();
   setCurrent();
   this->show();
}

restorePage::~restorePage()
{
   writeSettings();
}

/*
 * Fill the fileWidget box with the contents of the current directory
 */
void restorePage::fillDirectory()
{
   char modes[20], user[20], group[20], size[20], date[30];
   char marked[10];
   int pnl, fnl;
   POOLMEM *file = get_pool_memory(PM_FNAME);
   POOLMEM *path = get_pool_memory(PM_FNAME);

   fileWidget->clear();
   m_console->write_dir("dir");
   QList<QTreeWidgetItem *> treeItemList;
   QStringList item;
   while (m_console->read() > 0) {
      char *p = m_console->msg();
      char *l;
      strip_trailing_junk(p);
      if (*p == '$' || !*p) {
         continue;
      }
      l = p;
      skip_nonspaces(&p);             /* permissions */
      *p++ = 0;
      bstrncpy(modes, l, sizeof(modes));
      skip_spaces(&p);
      skip_nonspaces(&p);             /* link count */
      *p++ = 0;
      skip_spaces(&p);
      l = p;
      skip_nonspaces(&p);             /* user */
      *p++ = 0;
      skip_spaces(&p);
      bstrncpy(user, l, sizeof(user));
      l = p;
      skip_nonspaces(&p);             /* group */
      *p++ = 0;
      bstrncpy(group, l, sizeof(group));
      skip_spaces(&p);
      l = p;
      skip_nonspaces(&p);             /* size */
      *p++ = 0;
      bstrncpy(size, l, sizeof(size));
      skip_spaces(&p);
      l = p;
      skip_nonspaces(&p);             /* date/time */
      skip_spaces(&p);
      skip_nonspaces(&p);
      *p++ = 0;
      bstrncpy(date, l, sizeof(date));
      skip_spaces(&p);
      if (*p == '*') {
         bstrncpy(marked, "*", sizeof(marked));
         p++;
      } else {
         bstrncpy(marked, " ", sizeof(marked));
      }
      split_path_and_filename(p, &path, &pnl, &file, &fnl);
      item.clear();
      item << "" << file << modes << user << group << size << date;
      if (item[1].endsWith("/")) {
         addDirectory(item[1]);
      }
      QTreeWidgetItem *ti = new QTreeWidgetItem((QTreeWidget *)0, item);
      ti->setTextAlignment(5, Qt::AlignRight); /* right align size */
      if (strcmp(marked, "*") == 0) {
         ti->setIcon(0, QIcon(QString::fromUtf8(":images/check.png")));
         ti->setData(0, Qt::UserRole, true);
      } else {
         ti->setIcon(0, QIcon(QString::fromUtf8(":images/unchecked.png")));
         ti->setData(0, Qt::UserRole, false);
      }
      treeItemList.append(ti);
   }
   fileWidget->clear();
   fileWidget->insertTopLevelItems(0, treeItemList);
   for (int i=0; i<7; i++) {
      fileWidget->resizeColumnToContents(i);
   }

   free_pool_memory(file);
   free_pool_memory(path);
}

/*
 * Function called from fill directory when a directory is found to see if this
 * directory exists in the directory pane and then add it to the directory pane
 */
void restorePage::addDirectory(QString &newdirr)
{
   QString newdir = newdirr;
   QString fullpath = m_cwd + newdirr;
   QRegExp regex("^/[a-z]:/$");
   bool ok = true;
   bool windrive = false;

   if (mainWin->m_miscDebug) {
      QString msg = QString("In addDirectory cwd \"%1\" newdir \"%2\"\n")
                    .arg(m_cwd)
                    .arg(newdir);
      Pmsg0(000, msg.toUtf8().data());
   }

   /* add unix '/' directory first */
   if (m_dirPaths.empty() && (regex.indexIn(fullpath,0) == -1)) {
      QTreeWidgetItem *item = new QTreeWidgetItem(directoryWidget);
      item->setIcon(0,QIcon(QString::fromUtf8(":images/folder.svg")));
      
      QString text("/");
      item->setText(0, text.toUtf8().data());
      if (mainWin->m_miscDebug) {
         Pmsg1(000, "Pre Inserting %s\n",text.toUtf8().data());
      }
      m_dirPaths.insert(text, item);
      m_dirTreeItems.insert(item, text);
   }

   if (regex.indexIn(fullpath,0) == 0) {
      /* this is a windows drive */
      if (mainWin->m_miscDebug) {
         Pmsg0(000, "Need to do windows \"letter\":/\n");
      }
      fullpath.replace(0,1,"");
      windrive = true;
   }
 
   /* is it already existent ?? */
   if (!m_dirPaths.contains(fullpath)) {
      QTreeWidgetItem *item = NULL;
      if (windrive) {
         /* this is the base widget */
         item = new QTreeWidgetItem(directoryWidget);
         item->setText(0, fullpath.toUtf8().data());
         item->setIcon(0,QIcon(QString::fromUtf8(":images/folder.svg")));
      } else {
         QTreeWidgetItem *parent = m_dirPaths.value(m_cwd);
         if (parent) {
            /* new directories to add */
            item = new QTreeWidgetItem(parent);
            item->setText(0, newdir.toUtf8().data());
            item->setIcon(0,QIcon(QString::fromUtf8(":images/folder.svg")));
            directoryWidget->expandItem(parent);
         } else {
            ok = false;
            if (mainWin->m_miscDebug) {
               QString msg = QString("In else of if parent cwd \"%1\" newdir \"%2\"\n")
                    .arg(m_cwd)
                    .arg(newdir);
               Pmsg0(000, msg.toUtf8().data());
            }
         }
      }
      /* insert into both forward and reverse hash */
      if (ok) {
         if (mainWin->m_miscDebug) {
            Pmsg1(000, "Inserting %s\n",fullpath.toUtf8().data());
         }
         m_dirPaths.insert(fullpath, item);
         m_dirTreeItems.insert(item, fullpath);
      }
   }
}

/*
 * Executed when the tree item in the directory pane is changed.  This will
 * allow us to populate the file pane and make this the cwd.
 */
void restorePage::directoryItemChanged(QTreeWidgetItem *currentitem,
                                         QTreeWidgetItem * /*previousitem*/)
{
   QString fullpath = m_dirTreeItems.value(currentitem);
   if (fullpath != ""){
      cwd(fullpath.toUtf8().data());
      fillDirectory();
   }
}

void restorePage::okButtonPushed()
{
   this->hide();
   m_console->write("done");
   m_console->notify(true);
   setConsoleCurrent();
   closeStackPage();
   mainWin->resetFocus();
}


void restorePage::cancelButtonPushed()
{
   this->hide();
   m_console->write("quit");
   m_console->displayToPrompt();
   mainWin->set_status("Canceled");
   closeStackPage();
   m_console->notify(true);
   mainWin->resetFocus();
}

void restorePage::fileDoubleClicked(QTreeWidgetItem *item, int column)
{
   char cmd[1000];
   if (column == 0) {                 /* mark/unmark */
      if (item->data(0, Qt::UserRole).toBool()) {
         bsnprintf(cmd, sizeof(cmd), "unmark \"%s\"", item->text(1).toUtf8().data());
         item->setIcon(0, QIcon(QString::fromUtf8(":images/unchecked.png")));
         item->setData(0, Qt::UserRole, false);
      } else {
         bsnprintf(cmd, sizeof(cmd), "mark \"%s\"", item->text(1).toUtf8().data());
         item->setIcon(0, QIcon(QString::fromUtf8(":images/check.png")));
         item->setData(0, Qt::UserRole, true);
      }
      m_console->write_dir(cmd);
      if (m_console->read() > 0) {
         strip_trailing_junk(m_console->msg());
         statusLine->setText(m_console->msg());
      }
      m_console->displayToPrompt();
      return;
   }    
   /* 
    * Double clicking other than column 0 means to decend into
    *  the directory -- or nothing if it is not a directory.
    */
   if (item->text(1).endsWith("/")) {
      QString fullpath = m_cwd + item->text(1);
      /* check for fullpath = "/c:/" */
      QRegExp regex("^/[a-z]:/");
      if (regex.indexIn(fullpath,0) == 0)  /* remove leading '/' */
         fullpath.replace(0,1,"");
      QTreeWidgetItem *item = m_dirPaths.value(fullpath);
      if (item) {
         directoryWidget->setCurrentItem(item);
      } else {
         QString msg = QString("DoubleClick else of item column %1 fullpath %2\n")
              .arg(column,10)
              .arg(fullpath);
         Pmsg0(000, msg.toUtf8().data());
      }
   }
}

/*
 * If up button pushed, making the parent tree widget current will call fill
 * directory.
 */
void restorePage::upButtonPushed()
{
   cwd("..");
   QTreeWidgetItem *item = m_dirPaths.value(m_cwd);
   if (item) {
      directoryWidget->setCurrentItem(item);
   }
}

/*
 * Mark selected items
 */
void restorePage::markButtonPushed()
{
   QList<QTreeWidgetItem *> treeItemList = fileWidget->selectedItems();
   QTreeWidgetItem *item;
   char cmd[1000];
   foreach (item, treeItemList) {
      bsnprintf(cmd, sizeof(cmd), "mark \"%s\"", item->text(1).toUtf8().data());
      item->setIcon(0, QIcon(QString::fromUtf8(":images/check.png")));
      m_console->write_dir(cmd);
      if (m_console->read() > 0) {
         strip_trailing_junk(m_console->msg());
         statusLine->setText(m_console->msg());
      }
      Dmsg1(100, "cmd=%s\n", cmd);
      m_console->discardToPrompt();
   }
}

/*
 * Unmark selected items
 */
void restorePage::unmarkButtonPushed()
{
   QList<QTreeWidgetItem *> treeItemList = fileWidget->selectedItems();
   QTreeWidgetItem *item;
   char cmd[1000];
   foreach (item, treeItemList) {
      bsnprintf(cmd, sizeof(cmd), "unmark \"%s\"", item->text(1).toUtf8().data());
      item->setIcon(0, QIcon(QString::fromUtf8(":images/unchecked.png")));
      m_console->write_dir(cmd);
      if (m_console->read() > 0) {
         strip_trailing_junk(m_console->msg());
         statusLine->setText(m_console->msg());
      }
      Dmsg1(100, "cmd=%s\n", cmd);
      m_console->discardToPrompt();
   }
}

/*
 * Change current working directory 
 */
bool restorePage::cwd(const char *dir)
{
   int stat;
   char cd_cmd[MAXSTRING];

   bsnprintf(cd_cmd, sizeof(cd_cmd), "cd \"%s\"", dir);
   Dmsg2(100, "dir=%s cmd=%s\n", dir, cd_cmd);
   m_console->write_dir(cd_cmd);
   lineEdit->clear();
   if ((stat = m_console->read()) > 0) {
      m_cwd = m_console->msg();
      lineEdit->insert(m_cwd);
      Dmsg2(100, "cwd=%s msg=%s\n", m_cwd.toUtf8().data(), m_console->msg());
   } else {
      Dmsg1(000, "stat=%d\n", stat);
      QMessageBox::critical(this, "Error", "cd command failed", QMessageBox::Ok);
   }
   m_console->discardToPrompt();
   return true;  /* ***FIXME*** return real status */
}

/*
 * Return cwd when in tree restore mode 
 */
char *restorePage::get_cwd()
{
   int stat;
   m_console->write_dir(".pwd");
   Dmsg0(100, "send: .pwd\n");
   if ((stat = m_console->read()) > 0) {
      m_cwd = m_console->msg();
      Dmsg2(100, "cwd=%s msg=%s\n", m_cwd.toUtf8().data(), m_console->msg());
   } else {
      Dmsg1(000, "Something went wrong read stat=%d\n", stat);
      QMessageBox::critical(this, "Error", ".pwd command failed", QMessageBox::Ok);
   }
   m_console->discardToPrompt(); 
   return m_cwd.toUtf8().data();
}

/*
 * Save user settings associated with this page
 */
void restorePage::writeSettings()
{
   QSettings settings(m_console->m_dir->name(), "bat");
   settings.beginGroup("RestorePage");
   settings.setValue("splitterSizes", splitter->saveState());
   settings.endGroup();
}

/*
 * Read and restore user settings associated with this page
 */
void restorePage::readSettings()
{
   QSettings settings(m_console->m_dir->name(), "bat");
   settings.beginGroup("RestorePage");
   splitter->restoreState(settings.value("splitterSizes").toByteArray());
   settings.endGroup();
}
