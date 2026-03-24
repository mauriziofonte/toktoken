<x-alert />
<x-forms.input />
<x-slot:header>Header content</x-slot>
<x-dynamic-component :component="$name" />

@section('content')
    <x-navigation.breadcrumb />
    <x-ui.card>
        <x-slot:footer>Footer</x-slot>
    </x-ui.card>
@endsection
