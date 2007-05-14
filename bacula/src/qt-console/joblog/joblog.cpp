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
 *   Version $Id: joblog.cpp 4230 2007-02-21 20:07:37Z kerns $
 *
 *  JobLog Class
 *
 *   Dirk Bartley, March 2007
 *
 */ 

#include "bat.h"
#include "joblog.h"

JobLog::JobLog(QString &jobId, QTreeWidgetItem *parentTreeWidgetItem)
{
   setupUi(this);
   m_name = "JobLog";
   m_closeable = true;
   pgInitialize(parentTreeWidgetItem);
   m_cursor = new QTextCursor(textEdit->document());

   m_jobId = jobId;
   getFont();
   populateText();

   dockPage();
   setCurrent();
}

void JobLog::getFont()
{
   QFont font = textEdit->font();

   QString dirname;
   m_console->getDirResName(dirname);
   QSettings settings(dirname, "bat");
   settings.beginGroup("Console");
   font.setFamily(settings.value("consoleFont", "Courier").value<QString>());
   font.setPointSize(settings.value("consolePointSize", 10).toInt());
   font.setFixedPitch(settings.value("consoleFixedPitch", true).toBool());
   settings.endGroup();
   textEdit->setFont(font);
}

/*
 * Populate the text in the window
 */
void JobLog::populateText()
{
   QString heading("<A href=\"#top\">Log records for job ");
   heading += m_jobId + "</A>\n";
   textEdit->insertHtml(heading);
   
   QString query("");
   query = "SELECT Time, LogText FROM Log WHERE JobId='" + m_jobId + "'";

   QStringList results;
   if (m_console->sql_cmd(query, results)) {
      QString field;
      QStringList fieldlist;
      int resultcount = 0;

      /* Iterate through the lines of results. */
      foreach (QString resultline, results) {
         fieldlist = resultline.split("\t");

         int column = 0;
         /* Iterate through fields in the record */
         foreach (field, fieldlist) {
            display_text(field);
            if (column <= 1) display_text("\n");
            column += 1;
         } /* foreach field */
         resultcount += 1;
      } /* foreach resultline */
      if (resultcount == 0) {
         /* show a message about configuration item */
         QMessageBox::warning(this, tr("Bat"),
            tr("There were no results ??  !!!.\n"
"It is possible you may need to add \"catalog = all\" to the Messages stanza"
" for this job.\n"), QMessageBox::Ok);
      }
   } /* if results from query */
   textEdit->scrollToAnchor("top");
}

/*
 * Put text into the joblog window with an overload
 */
void JobLog::display_text(const QString buf)
{
   m_cursor->movePosition(QTextCursor::End);
   m_cursor->insertText(buf);
}

void JobLog::display_text(const char *buf)
{
   m_cursor->movePosition(QTextCursor::End);
   m_cursor->insertText(buf);
}
