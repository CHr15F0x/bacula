#ifndef _MEDIALIST_H_
#define _MEDIALIST_H_
/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2007-2007 Free Software Foundation Europe e.V.

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
 *   Version $Id$
 *
 *   Dirk Bartley, March 2007
 */

#include <QtGui>
#include "ui_medialist.h"
#include "console.h"
#include <qstringlist.h>

class MediaList : public Pages, public Ui::MediaListForm
{
   Q_OBJECT 

public:
   MediaList();
   ~MediaList();
   virtual void PgSeltreeWidgetClicked();
   virtual void currentStackItem();

public slots:
   void treeItemChanged(QTreeWidgetItem *, QTreeWidgetItem *);

private slots:
   void populateTree();
   void showJobs();
   void editVolume();
   void deleteVolume();
   void purgeVolume();
   void pruneVolume();
   void relabelVolume();
   void allVolumesFromPool();
   void allVolumes();
   void volumeFromPool();

private:
   void createContextMenu();
   void setStatusColor(QTreeWidgetItem *, QString &, int &);
   QString m_currentVolumeName;
   QString m_currentVolumeId;
   bool m_populated;
   bool m_checkcurwidget;
};

#endif /* _MEDIALIST_H_ */
