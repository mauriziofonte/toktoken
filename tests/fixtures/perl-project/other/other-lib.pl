# other-lib.pl -- fixture: second module (import cycle with mymod)
require '../mymod/mymod-lib.pl';

sub do_thing
{
my ($v) = @_;
open(my $fh, "|$v");
close($fh);
}

1;
