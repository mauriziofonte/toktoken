@extends('layouts.app')

@section('title', 'Page Title')

@section('content')
    @include('components.nav')

    <x-page-header title="My Page" />

    @livewire('search-bar')

    @push('scripts')
        <script src="/js/page.js"></script>
    @endpush
@endsection
