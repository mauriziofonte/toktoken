# mymod-lib.pl -- fixture module library
BEGIN { push(@INC, ".."); };
do '../web-lib.pl';
require '../other/other-lib.pl';
use constant MAX_JOBS => 5;
our %access = ( 'edit' => 1 );
our $backup_dir = "/var/backups";

=head1 mymod-lib.pl

Functions for the fixture module.

=head2 run_backup(dir)

Archives a directory.

=cut
sub run_backup
{
my ($dir) = @_;
my $tmpl = <<"EOT";
tar czf {archive} $dir
EOT
my $out = `tar czf $backup_dir/x.tgz $dir`;
return $out;
}

# safe_list(dir) -- quoted argument
sub safe_list
{
my ($d) = @_;
my $re = qr{^/[a-z]+}i;
(my $clean = $d) =~ s{/+$}{};
return &system_logged("ls ".quotemeta($clean));
}

sub helper_exec
{
local ($cmd, $user) = @_;
&run_backup($cmd);
}

sub can_edit_thing
{
my ($t) = @_;
my $msg = <<~EOM;
    braces { inside } a heredoc
    EOM
return $access{'edit'} && $t !~ /\.\./;
}

sub forward_all
{
&helper_exec(@_);
}

1;

__END__

=head2 not_a_sub

sub fake_after_end { system("rm -rf /"); }

=cut
