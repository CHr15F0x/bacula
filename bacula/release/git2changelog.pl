#!/usr/bin/perl -w
#
=head USAGE
    
    ./git2changelog.pl Release-3.0.1..Release-3.0.2

=cut

use strict;
use Time::CTime;

my $d='';
my $last_txt='';
my %bugs;
my $refs = shift || '';
open(FP, "git log --no-merges --pretty=format:'%ct: %s' $refs|") or die "Can't run git log $!";
while (my $l = <FP>) {

    # remove non useful messages
    next if ($l =~ /(tweak|typo|cleanup|bweb:|regress:|again|.gitignore|fix compilation|technotes)/ixs);
    next if ($l =~ /update (version|technotes|kernstodo|projects|releasenotes|version|home|release|todo|notes|changelog)/i);

    # keep list of fixed bugs
    if ($l =~ /#(\d+)/) {
        $bugs{$1}=1;
    }

    # remove old commit format
    $l =~ s/^(\d+): (kes|ebl)  /$1: /;

    if ($l =~ /(\d+): (.+)/) {
        # use date as 01Jan70
        my $dnow = strftime('%d%b%y', localtime($1));
        my $txt = $2;

        # avoid identical multiple commit message
        next if ($last_txt eq $txt);
        $last_txt = $txt;

        # We format the string on 79 caracters
        $txt =~ s/\s\s+/ /g;
        $txt =~ s/.{70,77} /$&\n  /g;

        # if we are the same day, just add entry
        if ($dnow ne $d) {
            print "\n$dnow\n";
            $d = $dnow;
        }
        print "- $txt\n";

    } else {
        print STDERR "invalid format: $l\n";
    }
}

close(FP);

print "\nBug fixes\n";
print join(" ", sort keys %bugs), "\n";
