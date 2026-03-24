@verbatim
    @extends('should-not-extract')
    <x-should-not-extract />
    @include('also-not-extracted')
@endverbatim

@section('real-content')
    <x-real-component />
@endsection
