{{-- Base application layout --}}
<!DOCTYPE html>
<html>
<head>
    <title>@yield('title', 'Default')</title>
    @stack('styles')
</head>
<body>
    <div id="app">
        @section('navigation')
            @include('partials.nav')
        @show

        @yield('content')
    </div>

    @stack('scripts')
</body>
</html>
