# miniserv-lib.pl -- fixture: framework defaults (Webmin harness)
package miniserv;

sub update_vital_config
{
my %vital = ("port", 10000,
	     "unauthcgi", "^/forgot.cgi\$",
	     "max_post", 10000);
foreach my $v (keys %vital) {
	$config{$v} = $vital{$v} if (!defined($config{$v}));
	}
}

1;
