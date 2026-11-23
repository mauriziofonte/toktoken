#!/usr/bin/perl
# forgot.cgi -- fixture: reachable before authentication (unauthcgi)

require './web-lib.pl';
&ReadParse();
system("mail -s reset $in{'user'} </dev/null");
