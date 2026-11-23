# web-lib-funcs.pl -- fixture: common helpers

=head2 system_logged(command)

Runs a command and logs it.

=cut
sub system_logged
{
my ($cmd) = @_;
return system($cmd);
}

=head2 error(message)

Prints an error page and exits.

=cut
sub error
{
my ($msg) = @_;
print "Error: ",&html_escape($msg),"\n";
exit(1);
}

sub html_escape
{
my ($str) = @_;
$str =~ s/&/&amp;/g;
$str =~ s/</&lt;/g;
return $str;
}

sub ReadParse
{
foreach my $p (split(/&/, $ENV{'QUERY_STRING'})) {
	my ($k, $v) = split(/=/, $p, 2);
	$in{$k} = $v;
	}
}

1;
