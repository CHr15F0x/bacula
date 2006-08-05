#!/bin/sh

# shell script wizard to build bacula rpm using gnome dialogs
# requires zenity to be installed
# 30 Jul 2006 D. Scott Barninger

# Copyright (C) 2006 Kern Sibbald
# licensed under GPL-v2

# usage ./rpm_wizard.sh

# check for zenity
HAVE_ZENITY=`which zenity`
if [ -z $HAVE_ZENITY ];
then
	echo You need zenity installed to run this script;
	exit;
fi

zenity --question --text "Bacula rpm rebuilding wizard. Do you wish to continue?"

RESULT="$?"
if [ "$RESULT" == "1" ];
then
	exit;
fi

# get packager name and email adddress
PACKAGER=`zenity --text-info --editable --height=25 --width=300 --title="Enter Your Name <your_email_address>"`

# get location of src rpm
SELECTED_FILE=`zenity --file-selection --title "Choose SRPM file to rebuild"`

RESULT="$?"
if [ "$RESULT" == "1" ];
then
	exit;
fi

# select build platform
PLATFORM=`zenity --title "Select Platform" --text "Please choose a build platform." --list --radiolist --column "Select" --column "Platform" False rh7 False rh8 False rh9 False fc1 False fc3 False fc4 False fc5 False wb3 False rhel3 False rhel4 False centos3 False centos4 False su9 False su10 False mdk False mdv`

RESULT="$?"
if [ "$RESULT" == "1" ];
then
	exit;
fi

# select database support
DATABASE=`zenity --title "Select Database" --text "Please choose database support." --list --radiolist --column "Select" --column "Platform" False sqlite False mysql False mysql4 False mysql5 False postgresql False client_only`

RESULT="$?"
if [ "$RESULT" == "1" ];
then
	exit;
fi

# select other build options
OPTIONS=`zenity --title "Select Options" --text "Please choose other options." --list --checklist --column "Select" --column "Platform" False build_wxconsole False nobuild_gconsole False build_x86_64 False build_python`

RESULT="$?"
if [ "$RESULT" == "1" ];
then
	exit;
fi

OPTION1=`echo $OPTIONS|cut --delimiter=\| -f1`
OPTION2=`echo $OPTIONS|cut --delimiter=\| -f2`
OPTION3=`echo $OPTIONS|cut --delimiter=\| -f3`
OPTION4=`echo $OPTIONS|cut --delimiter=\| -f4`

# construct rpmbuild command
COMMAND="rpmbuild --rebuild --define 'build_$PLATFORM 1' --define 'build_$DATABASE 1' --define 'contrib_packager ${PACKAGER}'"

if [ ! -z $OPTION1 ];
then
	COMMAND="${COMMAND} --define '$OPTION1 1'";
fi
if [ ! -z $OPTION2 ];
then
	COMMAND="${COMMAND} --define '$OPTION2 1'";
fi
if [ ! -z $OPTION3 ];
then
	COMMAND="${COMMAND} --define '$OPTION3 1'";
fi
if [ ! -z $OPTION4 ];
then
	COMMAND="${COMMAND} --define '$OPTION4 1'";
fi

COMMAND="${COMMAND} ${SELECTED_FILE}"

zenity --question --text "Ready to rebuild the src rpm with $COMMAND. Do you wish to continue?"

RESULT="$?"
if [ "$RESULT" == "1" ];
then
	exit;
fi

# execute the build
echo $COMMAND | sh

# ChangeLog
# 30 Jul 2006 initial release
# 05 Aug 2006 add option for build_python

