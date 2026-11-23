package Fixture::Widget;
use strict;
use warnings;
use constant DEFAULT_SIZE => 10;

=head1 NAME

Fixture::Widget - fixture class

=head2 new(%args)

Constructor.

=cut

sub new {
    my ($class, %args) = @_;
    my $self = { size => $args{size} || DEFAULT_SIZE };
    return bless $self, $class;
}

sub render {
    my ($self) = @_;
    my $pat = qr{\{(\w+)\}};
    my $text = "size={size}";
    $text =~ s{\{size\}}{$self->{size}}g;
    return $text;
}

1;
__END__

=head1 SEE ALSO

sub hidden_in_pod { }

=cut
