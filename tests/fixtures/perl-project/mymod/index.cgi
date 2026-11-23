#!/usr/bin/perl
# index.cgi -- fixture: lists jobs

require './mymod-lib.pl';
&ReadParse();
print "Max: ",MAX_JOBS,"\n";
