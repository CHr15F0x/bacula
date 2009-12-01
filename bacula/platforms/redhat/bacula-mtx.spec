# Bacula RPM spec file
#
# Copyright (C) 2000-2009 Free Software Foundation Europe e.V.

# Platform Build Configuration

# basic defines for every build
%define _release           1
%define _version           3.0.3
%define depkgs_version     18Feb09
%define _packager D. Scott Barninger <barninger@fairfieldcomputers.com>

%define manpage_ext gz

%define single_dir 0
%{?single_dir_install:%define single_dir 1}

# Installation Directory locations
%define _prefix        /usr
%define _sbindir       %_prefix/sbin
%define _bindir        %_prefix/bin
%define _subsysdir     /var/lock/subsys
%define sqlite_bindir  %_libdir/bacula/sqlite
%define _mandir        %_prefix/share/man
%define sysconf_dir    /etc/bacula
%define script_dir     %_libdir/bacula
%define working_dir    /var/lib/bacula
%define pid_dir        /var/run
%define plugin_dir     %_libdir/bacula/plugins
%define lib_dir        %_libdir/bacula/lib

#--------------------------------------------------------------------------
# it should not be necessary to change anything below here for a release
# except for patch macros in the setup section
#--------------------------------------------------------------------------

%{?contrib_packager:%define _packager %{contrib_packager}}

Summary: Bacula - The Network Backup Solution
Name: bacula-mtx
Version: %{_version}
Release: %{_release}
Group: System Environment/Daemons
License: GPL v2
BuildRoot: %{_tmppath}/%{name}-root
URL: http://www.bacula.org/
Vendor: The Bacula Team
Packager: %{_packager}
Prefix: %{_prefix}
Distribution: Bacula Bat

Source: http://www.prdownloads.sourceforge.net/bacula/depkgs-%{depkgs_version}.tar.gz

# define the basic package description
%define blurb Bacula - It comes by night and sucks the vital essence from your computers.
%define blurb2 Bacula is a set of computer programs that permit you (or the system
%define blurb3 administrator) to manage backup, recovery, and verification of computer
%define blurb4 data across a network of computers of different kinds. In technical terms,
%define blurb5 it is a network client/server based backup program. Bacula is relatively
%define blurb6 easy to use and efficient, while offering many advanced storage management
%define blurb7 features that make it easy to find and recover lost or damaged files.
%define blurb8 Bacula source code has been released under the GPL version 2 license.

Summary: Bacula - The Network Backup Solution
Group: System Environment/Daemons

%description
%{blurb}

%{blurb2}
%{blurb3}
%{blurb4}
%{blurb5}
%{blurb6}
%{blurb7}
%{blurb8}

This is Bacula's version of mtx tape utilities for Linux distributions that
do not provide their own mtx package

%prep
%setup -T -n depkgs -b 0

%build

make mtx

%install
make \
        prefix=$RPM_BUILD_ROOT%{_prefix} \
        sbindir=$RPM_BUILD_ROOT%{_sbindir} \
        sysconfdir=$RPM_BUILD_ROOT%{sysconf_dir} \
        scriptdir=$RPM_BUILD_ROOT%{script_dir} \
        working_dir=$RPM_BUILD_ROOT%{working_dir} \
        piddir=$RPM_BUILD_ROOT%{pid_dir} \
        mandir=$RPM_BUILD_ROOT%{_mandir} \
        mtx-install

%files
%defattr(-,root,root)
%attr(-, root, %{storage_daemon_group}) %{_sbindir}/loaderinfo
%attr(-, root, %{storage_daemon_group}) %{_sbindir}/mtx
%attr(-, root, %{storage_daemon_group}) %{_sbindir}/scsitape
%attr(-, root, %{storage_daemon_group}) %{_sbindir}/tapeinfo
%attr(-, root, %{storage_daemon_group}) %{_sbindir}/scsieject
%{_mandir}/man1/loaderinfo.1.%{manpage_ext}
%{_mandir}/man1/mtx.1.%{manpage_ext}
%{_mandir}/man1/scsitape.1.%{manpage_ext}
%{_mandir}/man1/tapeinfo.1.%{manpage_ext}
%{_mandir}/man1/scsieject.1.%{manpage_ext}


%clean
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf "$RPM_BUILD_ROOT"
rm -rf $RPM_BUILD_DIR/depkgs

%changelog
* Sat Aug 1 2009 Kern Sibbald <kern@sibbald.com>
- Split mtx out into bacula-mtx.spec
