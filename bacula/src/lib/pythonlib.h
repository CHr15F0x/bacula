/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2004-2008 Free Software Foundation Europe e.V.

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
 *
 * Interface definitions of bacula <--> python interface
 *
 * Version $Id:$
 *
 * Marco van Wieringen, October MMVIII
 *
 */

/*
 * This is the argument struct passed to init_python_interpreter
 * which initializes the python interpreter and makes sure we don't
 * depend on global variables.
 */
typedef struct {
   const char *progname;
   const char *scriptdir;
   const char *modulename;
   const char *configfile;
   const char *workingdir;
   PyObject *(*job_getattr)(PyObject *, char *);
   int (*job_setattr)(PyObject *, char *, PyObject *);
} init_python_interpreter_args;

void init_python_interpreter(init_python_interpreter_args *args);
void term_python_interpreter();
void lock_python();
void unlock_python();

JCR *get_jcr_from_PyObject(PyObject *self);
PyObject *find_method(PyObject *eventsObject, PyObject *method, const char *name);

