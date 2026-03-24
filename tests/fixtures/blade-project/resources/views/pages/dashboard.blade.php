@extends('layouts.app')

@section('title', 'Dashboard')

{{-- This comment should be skipped
    @include('should-not-appear')
--}}

@section('content')
    @include('partials.header')
    @includeIf('partials.optional-banner')
    @includeWhen($showSidebar, 'partials.sidebar')
    @includeFirst(['partials.custom-footer', 'partials.footer'])

    @component('components.alert')
        @slot('title', 'Welcome')
        Dashboard content here.
    @endcomponent

    @livewire('notifications')

    @push('scripts')
        <script src="/js/dashboard.js"></script>
    @endpush

    @prepend('styles')
        <link rel="stylesheet" href="/css/dashboard.css">
    @endprepend

    @inject('metrics', 'App\Services\MetricsService')

    @each('partials.item', $items, 'item')

    <x-alert type="danger" />
    <x-inputs.text-field :value="$name" />
    <x-navigation.breadcrumb />
@endsection

@use('App\Models\User')
