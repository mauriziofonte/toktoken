# web-lib.pl -- fixture: loads the helpers through a computed path
{
	my $script = -r '../web-lib-funcs.pl' ? '../web-lib-funcs.pl'
						: 'web-lib-funcs.pl';
	do $script;
}

1;
