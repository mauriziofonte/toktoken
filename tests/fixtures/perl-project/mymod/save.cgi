#!/usr/bin/perl
# save.cgi -- fixture: saves a job

require './mymod-lib.pl';
&ReadParse();
&error("Not allowed") if (!&can_edit_thing($in{'dir'}));
&run_backup($in{'dir'});
&helper_exec($in{'cmd'}, $in{'user'});
&safe_list($in{'list'});
&foreign_require("other", "other-lib.pl");
&foreign_call("other", "do_thing", $in{'pipe'});
&unknown_helper($in{'z'});
system("echo $in{'x'}");
