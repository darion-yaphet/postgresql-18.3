#!/usr/bin/perl
#
#!/usr/bin/perl
#
#!/usr/bin/perl
#
#!/usr/bin/perl

# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# this script will sort any table with the segment data type in its last column
#
# 该脚本将对最后一列中具有段数据类型的任何表进行排序

use strict;
use warnings FATAL => 'all';

my @rows;

while (<>)
{
	chomp;
	push @rows, $_;
}

foreach (
	sort {
		my @ar = split("\t", $a);
		my $valA = pop @ar;
		$valA =~ s/[~<> ]+//g;
		@ar = split("\t", $b);
		my $valB = pop @ar;
		$valB =~ s/[~<> ]+//g;
		$valA <=> $valB
	} @rows)
{
	print "$_\n";
}
