# Bacula RPM spec file
#
# Copyright (C) 2000-2009 Free Software Foundation Europe e.V.

# Platform Build Configuration

# basic defines for every build
%define _release           1
%define _version           3.0.3
%define _packager D. Scott Barninger <barninger@fairfieldcomputers.com>

%define _prefix        /usr
%define _sbindir       %_prefix/sbin
%define _mandir        %_prefix/share/man


#--------------------------------------------------------------------------
# it should not be necessary to change anything below here for a release
# except for patch macros in the setup section
#--------------------------------------------------------------------------

%{?contrib_packager:%define _packager %{contrib_packager}}

Summary: Bacula - The Network Backup Solution
Name: bacula-docs
Version: %{_version}
Release: %{_release}
Group: System Environment/Daemons
License: GPL v2
BuildRoot: %{_tmppath}/%{name}-root
URL: http://www.bacula.org/
Vendor: The Bacula Team
Packager: %{_packager}
Prefix: %{_prefix}
Distribution: Bacula Documentation

Source: %{name}-%{_version}.tar.gz

# Source directory locations
%define _docsrc .

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

This package installs the Bacula pdf and html documentation.

%prep
%setup


%clean
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf "$RPM_BUILD_ROOT"
rm -rf $RPM_BUILD_DIR/%{name}-%{_version}

%files
%doc %{_docsrc}/manuals/en/catalog/catalog %{_docsrc}/manuals/en/catalog/catalog.pdf
%doc %{_docsrc}/manuals/en/concepts/concepts %{_docsrc}/manuals/en/concepts/concepts.pdf
%doc %{_docsrc}/manuals/en/console/console %{_docsrc}/manuals/en/console/console.pdf
%doc %{_docsrc}/manuals/en/developers/developers %{_docsrc}/manuals/en/developers/developers.pdf
%doc %{_docsrc}/manuals/en/install/install %{_docsrc}/manuals/en/install/install.pdf
%doc %{_docsrc}/manuals/en/problems/problems %{_docsrc}/manuals/en/problems/problems.pdf
%doc %{_docsrc}/manuals/en/utility/utility %{_docsrc}/manuals/en/utility/utility.pdf

%changelog
* Sat Aug 1 2009 Kern Sibbald <kern@sibbald.com>
- Split docs into separate bacula-docs.spec
