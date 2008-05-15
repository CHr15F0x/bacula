/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2007-2008 Free Software Foundation Europe e.V.

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
 
#include <QAbstractEventDispatcher>
#include <QTableWidgetItem>
#include "bat.h"
#include "joblist.h"
#include "restore.h"
#include "joblog/joblog.h"
#ifdef HAVE_QWT
#include "jobgraphs/jobplot.h"
#endif
#include "util/fmtwidgetitem.h"

/*
 * Constructor for the class
 */
JobList::JobList(const QString &mediaName, const QString &clientName,
          const QString &jobName, const QString &filesetName, QTreeWidgetItem *parentTreeWidgetItem)
{
   setupUi(this);
   m_name = ""; /* treeWidgetName has a virtual override in this class */
   m_mediaName = mediaName;
   m_clientName = clientName;
   m_jobName = jobName;
   m_filesetName = filesetName;
   m_filesetName = filesetName;
   pgInitialize(parentTreeWidgetItem);
   QTreeWidgetItem* thisitem = mainWin->getFromHash(this);
   thisitem->setIcon(0,QIcon(QString::fromUtf8(":images/emblem-system.png")));

   m_resultCount = 0;
   m_populated = false;
   m_closeable = false;
   if ((m_mediaName != "") || (m_clientName != "") || (m_jobName != "") || (m_filesetName != ""))
      m_closeable=true;
   m_checkCurrentWidget = true;
   createConnections();

   /* Set Defaults for check and spin for limits */
   limitCheckBox->setCheckState(mainWin->m_recordLimitCheck ? Qt::Checked : Qt::Unchecked);
   limitSpinBox->setValue(mainWin->m_recordLimitVal);
   daysCheckBox->setCheckState(mainWin->m_daysLimitCheck ? Qt::Checked : Qt::Unchecked);
   daysSpinBox->setValue(mainWin->m_daysLimitVal);
   dockPage();

   QGridLayout *gridLayout = new QGridLayout(this);
   gridLayout->setSpacing(6);
   gridLayout->setMargin(9);
   gridLayout->setObjectName(QString::fromUtf8("gridLayout"));

   m_splitter = new QSplitter(Qt::Vertical, this);
   QScrollArea *area = new QScrollArea();
   area->setObjectName(QString::fromUtf8("area"));
   area->setWidget(frame);
   area->setWidgetResizable(true);
   m_splitter->addWidget(mp_tableWidget);
   m_splitter->addWidget(area);

   gridLayout->addWidget(m_splitter, 0, 0, 1, 1);
   readSettings();
}

/*
 * Write the m_splitter settings in the destructor
 */
JobList::~JobList()
{
   writeSettings();
}

/*
 * The Meat of the class.
 * This function will populate the QTableWidget, mp_tablewidget, with
 * QTableWidgetItems representing the results of a query for what jobs exist on
 * the media name passed from the constructor stored in m_mediaName.
 */
void JobList::populateTable()
{
   if (!m_console->preventInUseConnect())
       return;

   /* Can't do this in constructor because not neccesarily conected in constructor */
   prepareFilterWidgets();

   /* Set up query */
   QString query;
   fillQueryString(query);

   /* Set up the Header for the table */
   QStringList headerlist = (QStringList()
      << tr("Job Id") << tr("Job Name") << tr("Client") << tr("Job Starttime") 
      << tr("Job Type") << tr("Job Level") << tr("Job Files") 
      << tr("Job Bytes") << tr("Job Status")  << tr("Purged") << tr("File Set"));

   m_jobIdIndex = headerlist.indexOf(tr("Job Id"));
   m_purgedIndex = headerlist.indexOf(tr("Purged"));
   m_typeIndex = headerlist.indexOf(tr("Job Type"));
   m_statusIndex = headerlist.indexOf(tr("Job Status"));
   m_startIndex = headerlist.indexOf(tr("Job Starttime"));
   m_filesIndex = headerlist.indexOf(tr("Job Files"));
   m_bytesIndex = headerlist.indexOf(tr("Job Bytes"));

   /* Initialize the QTableWidget */
   m_checkCurrentWidget = false;
   mp_tableWidget->clear();
   m_checkCurrentWidget = true;
   mp_tableWidget->setColumnCount(headerlist.size());
   mp_tableWidget->setHorizontalHeaderLabels(headerlist);
   mp_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
   mp_tableWidget->setSelectionMode(QAbstractItemView::SingleSelection);

   if (mainWin->m_sqlDebug) {
      Pmsg1(000, "Query cmd : %s\n",query.toUtf8().data());
   }

   QStringList results;
   if (m_console->sql_cmd(query, results)) {
      m_resultCount = results.count();

      QStringList fieldlist;
      mp_tableWidget->setRowCount(results.size());

      int row = 0;
      /* Iterate through the record returned from the query */
      QString resultline;
      foreach (resultline, results) {
         fieldlist = resultline.split("\t");
         if (fieldlist.size() < 12)
	    continue; // some fields missing, ignore row

	 TableItemFormatter jobitem(*mp_tableWidget, row);
  
         /* Iterate through fields in the record */
	 QStringListIterator fld(fieldlist);
         int col = 0;

	 /* job id */
         jobitem.setNumericFld(col++, fld.next());

	 /* job name */
         jobitem.setTextFld(col++, fld.next());

	 /* client */
         jobitem.setTextFld(col++, fld.next());

	 /* job starttime */
         jobitem.setTextFld(col++, fld.next(), true);

	 /* job type */
         jobitem.setJobTypeFld(col++, fld.next());

	 /* job level */
         jobitem.setJobLevelFld(col++, fld.next());

	 /* job files */
         jobitem.setNumericFld(col++, fld.next());

	 /* job bytes */
         jobitem.setBytesFld(col++, fld.next());

	 /* job status */
	 QString shortstatus(fld.next());
	 QString longstatus(fld.next());
         jobitem.setJobStatusFld(col++, shortstatus, longstatus);

	 /* purged */
         if (fld.next().toInt())
	    jobitem.setTextFld(col++, tr("IS"), true);
	 else
	    jobitem.setTextFld(col++, tr("NOT"), true);

	 /* fileset */
         jobitem.setTextFld(col++, fld.next());

         row++;
      }
   } 
   /* Resize the columns */
   mp_tableWidget->resizeColumnsToContents();
   mp_tableWidget->resizeRowsToContents();
   mp_tableWidget->verticalHeader()->hide();
   if ((m_mediaName != tr("Any")) && (m_resultCount == 0)){
      /* for context sensitive searches, let the user know if there were no
       * results */
      QMessageBox::warning(this, "Bat",
          tr("The Jobs query returned no results.\n"
         "Press OK to continue?"), QMessageBox::Ok );
   }
}

void JobList::prepareFilterWidgets()
{
   if (!m_populated) {
      clientComboBox->addItem(tr("Any"));
      clientComboBox->addItems(m_console->client_list);
      int clientIndex = clientComboBox->findText(m_clientName, Qt::MatchExactly);
      if (clientIndex != -1)
         clientComboBox->setCurrentIndex(clientIndex);

      QStringList volumeList;
      m_console->getVolumeList(volumeList);
      volumeComboBox->addItem(tr("Any"));
      volumeComboBox->addItems(volumeList);
      int volumeIndex = volumeComboBox->findText(m_mediaName, Qt::MatchExactly);
      if (volumeIndex != -1) {
         volumeComboBox->setCurrentIndex(volumeIndex);
      }
      jobComboBox->addItem(tr("Any"));
      jobComboBox->addItems(m_console->job_list);
      int jobIndex = jobComboBox->findText(m_jobName, Qt::MatchExactly);
      if (jobIndex != -1) {
         jobComboBox->setCurrentIndex(jobIndex);
      }
      levelComboBox->addItem(tr("Any"));
      levelComboBox->addItems( QStringList() << "F" << "D" << "I");
      purgedComboBox->addItem(tr("Any"));
      purgedComboBox->addItems( QStringList() << "0" << "1");
      fileSetComboBox->addItem(tr("Any"));
      fileSetComboBox->addItems(m_console->fileset_list);
      int filesetIndex = fileSetComboBox->findText(m_filesetName, Qt::MatchExactly);
      if (filesetIndex != -1) {
         fileSetComboBox->setCurrentIndex(filesetIndex);
      }
      QStringList statusLongList;
      m_console->getStatusList(statusLongList);
      statusComboBox->addItem(tr("Any"));
      statusComboBox->addItems(statusLongList);
   }
}

void JobList::fillQueryString(QString &query)
{
   query = "";
   int volumeIndex = volumeComboBox->currentIndex();
   if (volumeIndex != -1)
      m_mediaName = volumeComboBox->itemText(volumeIndex);
   QString distinct = "";
   if (m_mediaName != tr("Any")) { distinct = "DISTINCT "; }
   query += "SELECT " + distinct + "Job.Jobid AS Id, Job.Name AS JobName, " 
            " Client.Name AS Client,"
            " Job.Starttime AS JobStart, Job.Type AS JobType,"
            " Job.Level AS BackupLevel, Job.Jobfiles AS FileCount,"
            " Job.JobBytes AS Bytes,"
            " Job.JobStatus AS Status, Status.JobStatusLong AS StatusLong,"
            " Job.PurgedFiles AS Purged, FileSet.FileSet"
            " FROM Job"
            " JOIN Client ON (Client.ClientId=Job.ClientId)"
            " JOIN Status ON (Job.JobStatus=Status.JobStatus)"
            " LEFT OUTER JOIN FileSet ON (FileSet.FileSetId=Job.FileSetId) ";
   QStringList conditions;
   if (m_mediaName != tr("Any")) {
      query += " LEFT OUTER JOIN JobMedia ON (JobMedia.JobId=Job.JobId) "
               " LEFT OUTER JOIN Media ON (JobMedia.MediaId=Media.MediaId) ";
      conditions.append("Media.VolumeName='" + m_mediaName + "'");
   }
   int clientIndex = clientComboBox->currentIndex();
   if (clientIndex != -1)
      m_clientName = clientComboBox->itemText(clientIndex);
   if (m_clientName != tr("Any")) {
      conditions.append("Client.Name='" + m_clientName + "'");
   }
   int jobIndex = jobComboBox->currentIndex();
   if (jobIndex != -1)
      m_jobName = jobComboBox->itemText(jobIndex);
   if ((jobIndex != -1) && (jobComboBox->itemText(jobIndex) != tr("Any"))) {
      conditions.append("Job.Name='" + jobComboBox->itemText(jobIndex) + "'");
   }
   int levelIndex = levelComboBox->currentIndex();
   if ((levelIndex != -1) && (levelComboBox->itemText(levelIndex) != tr("Any"))) {
      conditions.append("Job.Level='" + levelComboBox->itemText(levelIndex) + "'");
   }
   int statusIndex = statusComboBox->currentIndex();
   if ((statusIndex != -1) && (statusComboBox->itemText(statusIndex) != tr("Any"))) {
      conditions.append("Status.JobStatusLong='" + statusComboBox->itemText(statusIndex) + "'");
   }
   int purgedIndex = purgedComboBox->currentIndex();
   if ((purgedIndex != -1) && (purgedComboBox->itemText(purgedIndex) != tr("Any"))) {
      conditions.append("Job.PurgedFiles='" + purgedComboBox->itemText(purgedIndex) + "'");
   }
   int fileSetIndex = fileSetComboBox->currentIndex();
   if (fileSetIndex != -1)
      m_filesetName = fileSetComboBox->itemText(fileSetIndex);
   if ((fileSetIndex != -1) && (fileSetComboBox->itemText(fileSetIndex) != tr("Any"))) {
      conditions.append("FileSet.FileSet='" + fileSetComboBox->itemText(fileSetIndex) + "'");
   }
   /* If Limit check box For limit by days is checked  */
   if (daysCheckBox->checkState() == Qt::Checked) {
      QDateTime stamp = QDateTime::currentDateTime().addDays(-daysSpinBox->value());
      QString since = stamp.toString(Qt::ISODate);
      conditions.append("Job.Starttime>'" + since + "'");
   }
   bool first = true;
   foreach (QString condition, conditions) {
      if (first) {
         query += " WHERE " + condition;
         first = false;
      } else {
         query += " AND " + condition;
      }
   }
   /* Descending */
   query += " ORDER BY Job.Starttime=0 DESC, Job.Starttime DESC, Job.JobId DESC";
   /* If Limit check box for limit records returned is checked  */
   if (limitCheckBox->checkState() == Qt::Checked) {
      QString limit;
      limit.setNum(limitSpinBox->value());
      query += " LIMIT " + limit;
   }
}

/*
 * When the treeWidgetItem in the page selector tree is singleclicked, Make sure
 * The tree has been populated.
 */
void JobList::PgSeltreeWidgetClicked()
{
   if (!m_populated) {
      populateTable();
      m_populated=true;
   }
}

/*
 *  Virtual function override of pages function which is called when this page
 *  is visible on the stack
 */
void JobList::currentStackItem()
{
   populateTable();
   if (!m_populated) {
      m_populated=true;
   }
}

/*
 * Virtual Function to return the name for the medialist tree widget
 */
void JobList::treeWidgetName(QString &desc)
{
   if ((m_mediaName == "") && (m_clientName == "") && (m_jobName == "") && (m_filesetName == "")) {
      desc = "JobList";
   } else {
      desc = "JobList ";
      if (m_mediaName != "" ) {
         desc += "of Volume " + m_mediaName;
      }
      if (m_clientName != "" ) {
         desc += "of Client " + m_clientName;
      }
      if (m_jobName != "" ) {
         desc += "of Job " + m_jobName;
      }
      if (m_filesetName != "" ) {
         desc += "of fileset " + m_filesetName;
      }
   }
}

/*
 * This functions much line tableItemChanged for trees like the page selector,
 * but I will do much less here
 */
void JobList::tableItemChanged(QTableWidgetItem *currentItem, QTableWidgetItem * /*previousItem*/)
{
   if (m_checkCurrentWidget) {
      int row = currentItem->row();
      QTableWidgetItem* jobitem = mp_tableWidget->item(row, 0);
      m_currentJob = jobitem->text();
      selectedJobsGet();

      /* include purged action or not */
      jobitem = mp_tableWidget->item(row, m_purgedIndex);
      QString purged = jobitem->text();
      mp_tableWidget->removeAction(actionPurgeFiles);
      if (purged == "NOT") {
         mp_tableWidget->addAction(actionPurgeFiles);
      }
      /* include restore from time and job action or not */
      jobitem = mp_tableWidget->item(row, m_typeIndex);
      QString type = jobitem->text();
      mp_tableWidget->removeAction(actionRestoreFromJob);
      mp_tableWidget->removeAction(actionRestoreFromTime);
      if (type == "Backup") {
         mp_tableWidget->addAction(actionRestoreFromJob);
         mp_tableWidget->addAction(actionRestoreFromTime);
      }
      /* include cancel action or not */
      jobitem = mp_tableWidget->item(row, m_statusIndex);
      QString status = jobitem->text();
      mp_tableWidget->removeAction(actionCancelJob);
      if (status == "Running") {
         mp_tableWidget->addAction(actionCancelJob);
      }
   }
}

/*
 * Function to create connections for context sensitive menu for this and
 * the page selector
 */
void JobList::createConnections()
{
   /* connect to the action specific to this pages class that shows up in the 
    * page selector tree */
   connect(actionRefreshJobList, SIGNAL(triggered()), this,
                SLOT(populateTable()));
   connect(refreshButton, SIGNAL(pressed()), this, SLOT(populateTable()));
#ifdef HAVE_QWT
   connect(graphButton, SIGNAL(pressed()), this, SLOT(graphTable()));
#else
   graphButton->setEnabled(false);
#endif
   /* for the tableItemChanged to maintain m_currentJob */
   connect(mp_tableWidget, SIGNAL(
           currentItemChanged(QTableWidgetItem *, QTableWidgetItem *)),
           this, SLOT(tableItemChanged(QTableWidgetItem *, QTableWidgetItem *)));

   /* Do what is required for the local context sensitive menu */


   /* setContextMenuPolicy is required */
   mp_tableWidget->setContextMenuPolicy(Qt::ActionsContextMenu);

   /* Add Actions */
   mp_tableWidget->addAction(actionRefreshJobList);
   mp_tableWidget->addAction(actionListJobid);
   mp_tableWidget->addAction(actionListFilesOnJob);
   mp_tableWidget->addAction(actionListJobMedia);
   mp_tableWidget->addAction(actionListVolumes);
   mp_tableWidget->addAction(actionDeleteJob);
   mp_tableWidget->addAction(actionPurgeFiles);
   mp_tableWidget->addAction(actionRestoreFromJob);
   mp_tableWidget->addAction(actionRestoreFromTime);
   mp_tableWidget->addAction(actionShowLogForJob);

   /* Make Connections */
   connect(actionListJobid, SIGNAL(triggered()), this,
                SLOT(consoleListJobid()));
   connect(actionListFilesOnJob, SIGNAL(triggered()), this,
                SLOT(consoleListFilesOnJob()));
   connect(actionListJobMedia, SIGNAL(triggered()), this,
                SLOT(consoleListJobMedia()));
   connect(actionListVolumes, SIGNAL(triggered()), this,
                SLOT(consoleListVolumes()));
   connect(actionDeleteJob, SIGNAL(triggered()), this,
                SLOT(consoleDeleteJob()));
   connect(actionPurgeFiles, SIGNAL(triggered()), this,
                SLOT(consolePurgeFiles()));
   connect(actionRestoreFromJob, SIGNAL(triggered()), this,
                SLOT(preRestoreFromJob()));
   connect(actionRestoreFromTime, SIGNAL(triggered()), this,
                SLOT(preRestoreFromTime()));
   connect(actionShowLogForJob, SIGNAL(triggered()), this,
                SLOT(showLogForJob()));
   connect(actionCancelJob, SIGNAL(triggered()), this,
                SLOT(consoleCancelJob()));
   connect(actionListJobTotals, SIGNAL(triggered()), this,
                SLOT(consoleListJobTotals()));

   m_contextActions.append(actionRefreshJobList);
   m_contextActions.append(actionListJobTotals);
}

/*
 * Functions to respond to local context sensitive menu sending console commands
 * If I could figure out how to make these one function passing a string, Yaaaaaa
 */
void JobList::consoleListJobid()
{
   QString cmd("list jobid=");
   cmd += m_currentJob;
   if (mainWin->m_longList) { cmd.prepend("l"); }
   consoleCommand(cmd);
}
void JobList::consoleListFilesOnJob()
{
   QString cmd("list files jobid=");
   cmd += m_currentJob;
   if (mainWin->m_longList) { cmd.prepend("l"); }
   consoleCommand(cmd);
}
void JobList::consoleListJobMedia()
{
   QString cmd("list jobmedia jobid=");
   cmd += m_currentJob;
   if (mainWin->m_longList) { cmd.prepend("l"); }
   consoleCommand(cmd);
}
void JobList::consoleListVolumes()
{
   QString cmd("list volumes jobid=");
   cmd += m_currentJob;
   if (mainWin->m_longList) { cmd.prepend("l"); }
   consoleCommand(cmd);
}
void JobList::consoleListJobTotals()
{
   QString cmd("list jobtotals");
   if (mainWin->m_longList) { cmd.prepend("l"); }
   consoleCommand(cmd);
}
void JobList::consoleDeleteJob()
{
   if (QMessageBox::warning(this, "Bat",
      tr("Are you sure you want to delete??  !!!.\n"
"This delete command is used to delete a Job record and all associated catalog"
" records that were created. This command operates only on the Catalog"
" database and has no effect on the actual data written to a Volume. This"
" command can be dangerous and we strongly recommend that you do not use"
" it unless you know what you are doing.  The Job and all its associated"
" records (File and JobMedia) will be deleted from the catalog."
      "Press OK to proceed with delete operation.?"),
      QMessageBox::Ok | QMessageBox::Cancel)
      == QMessageBox::Cancel) { return; }

   QString cmd("delete job jobid=");
   cmd += m_selectedJobs;
   consoleCommand(cmd);
}
void JobList::consolePurgeFiles()
{
   if (QMessageBox::warning(this, "Bat",
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

   QString cmd("purge files jobid=");
   cmd += m_currentJob;
   consoleCommand(cmd);
}

/*
 * Subroutine to call preRestore to restore from a select job
 */
void JobList::preRestoreFromJob()
{
   new prerestorePage(m_currentJob, R_JOBIDLIST);
}

/*
 * Subroutine to call preRestore to restore from a select job
 */
void JobList::preRestoreFromTime()
{
   new prerestorePage(m_currentJob, R_JOBDATETIME);
}

/*
 * Subroutine to call class to show the log in the database from that job
 */
void JobList::showLogForJob()
{
   QTreeWidgetItem* pageSelectorTreeWidgetItem = mainWin->getFromHash(this);
   new JobLog(m_currentJob, pageSelectorTreeWidgetItem);
}

/*
 * Cancel a running job
 */
void JobList::consoleCancelJob()
{
   QString cmd("cancel jobid=");
   cmd += m_currentJob;
   consoleCommand(cmd);
}

/*
 * Graph this table
 */
#ifdef HAVE_QWT
void JobList::graphTable()
{
   JobPlotPass pass;
   pass.recordLimitCheck = limitCheckBox->checkState();
   pass.daysLimitCheck = daysCheckBox->checkState();
   pass.recordLimitSpin = limitSpinBox->value();
   pass.daysLimitSpin = daysSpinBox->value();
   pass.jobCombo = jobComboBox->currentText();
   pass.clientCombo = clientComboBox->currentText();
   pass.volumeCombo = volumeComboBox->currentText();
   pass.fileSetCombo = fileSetComboBox->currentText();
   pass.purgedCombo = purgedComboBox->currentText();
   pass.levelCombo = levelComboBox->currentText();
   pass.statusCombo = statusComboBox->currentText();
   pass.use = true;
   QTreeWidgetItem* pageSelectorTreeWidgetItem = mainWin->getFromHash(this);
   new JobPlot(pageSelectorTreeWidgetItem, pass);
}
#endif

/*
 * Save user settings associated with this page
 */
void JobList::writeSettings()
{
   QSettings settings(m_console->m_dir->name(), "bat");
   settings.beginGroup(m_groupText);
   settings.setValue(m_splitText, m_splitter->saveState());
   settings.endGroup();
}

/*
 * Read and restore user settings associated with this page
 */
void JobList::readSettings()
{
   m_groupText = "JobListPage";
   m_splitText = "splitterSizes_1";
   QSettings settings(m_console->m_dir->name(), "bat");
   settings.beginGroup(m_groupText);
   m_splitter->restoreState(settings.value(m_splitText).toByteArray());
   settings.endGroup();
}

/*
 * Function to fill m_selectedJobsCount and m_selectedJobs with selected values
 */
void JobList::selectedJobsGet()
{
   QList<int> rowList;
   QList<QTableWidgetItem *> sitems = mp_tableWidget->selectedItems();
   foreach (QTableWidgetItem *sitem, sitems) {
      int row = sitem->row();
      if (!rowList.contains(row)) {
         rowList.append(row);
      }
   }

   m_selectedJobs = "";
   bool first = true;
   foreach(int row, rowList) {
      QTableWidgetItem * sitem = mp_tableWidget->item(row, m_jobIdIndex);
      if (!first) m_selectedJobs.append(",");
      else first = false;
      m_selectedJobs.append(sitem->text());
   }
   m_selectedJobsCount = rowList.count();
   if (m_selectedJobsCount > 1) {
      QString text = QString("Delete list of %1 Jobs").arg(m_selectedJobsCount);
      actionDeleteJob->setText(text);
   } else {
      actionDeleteJob->setText("Delete Single Job");
   }
}
