#!/usr/bin/env perl
use strict;
use warnings;
use Carp qw(croak);

my $DELIMITER = ',';
my $MAX_FIELD_LENGTH = 1024;

sub parse_input {
    my ($filename) = @_;
    croak "Filename required" unless defined $filename;

    open my $fh, '<', $filename
        or croak "Cannot open '$filename': $!";

    my @records;
    my $line_num = 0;

    while (my $line = <$fh>) {
        $line_num++;
        chomp $line;
        next if $line =~ /^\s*#/;
        next if $line =~ /^\s*$/;

        my @fields = split /\Q$DELIMITER\E/, $line;
        for my $field (@fields) {
            $field =~ s/^\s+|\s+$//g;
            if (length($field) > $MAX_FIELD_LENGTH) {
                warn "Field truncated at line $line_num";
                $field = substr($field, 0, $MAX_FIELD_LENGTH);
            }
        }

        push @records, {
            line   => $line_num,
            fields => \@fields,
            raw    => $line,
        };
    }

    close $fh;
    return \@records;
}

sub transform_data {
    my ($records, $transform_fn) = @_;
    croak "Records arrayref required" unless ref $records eq 'ARRAY';
    $transform_fn //= sub { return $_[0] };

    my @transformed;
    for my $record (@$records) {
        my $result = eval { $transform_fn->($record) };
        if ($@) {
            warn "Transform failed at line $record->{line}: $@";
            next;
        }
        push @transformed, $result if defined $result;
    }

    return \@transformed;
}

sub write_output {
    my ($filename, $records, $options) = @_;
    $options //= {};

    my $separator = $options->{separator} // $DELIMITER;
    my $header    = $options->{header};

    open my $fh, '>', $filename
        or croak "Cannot write to '$filename': $!";

    if ($header && ref $header eq 'ARRAY') {
        print $fh join($separator, @$header), "\n";
    }

    for my $record (@$records) {
        my $fields = ref $record eq 'HASH' ? $record->{fields} : $record;
        next unless ref $fields eq 'ARRAY';
        print $fh join($separator, @$fields), "\n";
    }

    close $fh;
    return scalar @$records;
}

sub validate {
    my ($records, $rules) = @_;
    croak "Records and rules required" unless $records && $rules;

    my @errors;
    for my $record (@$records) {
        for my $rule (@$rules) {
            my $field_idx = $rule->{field};
            my $pattern   = $rule->{pattern};
            my $required  = $rule->{required} // 0;

            my $value = $record->{fields}[$field_idx] // '';

            if ($required && $value eq '') {
                push @errors, "Line $record->{line}: field $field_idx is required";
            }
            if ($pattern && $value !~ /$pattern/) {
                push @errors, "Line $record->{line}: field $field_idx invalid format";
            }
        }
    }

    return @errors ? \@errors : undef;
}

1;
