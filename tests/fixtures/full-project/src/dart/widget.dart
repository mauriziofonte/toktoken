import 'dart:async';
import 'dart:convert';

const String appTitle = 'Full Project';
const int maxRetries = 3;

void main() {
  final app = AppWidget(title: appTitle);
  app.run();
}

class AppWidget {
  final String title;
  final List<String> _logs = [];
  bool _isRunning = false;

  AppWidget({required this.title});

  void run() {
    _isRunning = true;
    _logs.add('Application started: $title');
    final state = AppState(widget: this);
    state.initialize();
  }

  void stop() {
    _isRunning = false;
    _logs.add('Application stopped');
  }

  bool get isRunning => _isRunning;
  List<String> get logs => List.unmodifiable(_logs);
}

class AppState {
  final AppWidget widget;
  final Map<String, dynamic> _data = {};
  int _counter = 0;
  Timer? _refreshTimer;

  AppState({required this.widget});

  void initialize() {
    _data['initialized'] = true;
    _data['timestamp'] = DateTime.now().toIso8601String();
    _counter = 0;
    _startRefreshCycle();
  }

  Widget build() {
    return Widget(
      type: 'column',
      children: [
        Widget(type: 'text', value: widget.title),
        Widget(type: 'text', value: 'Count: $_counter'),
        Widget(type: 'button', value: 'Increment'),
      ],
    );
  }

  void increment() {
    _counter++;
    _data['lastUpdate'] = DateTime.now().toIso8601String();
  }

  void dispose() {
    _refreshTimer?.cancel();
    _refreshTimer = null;
    _data.clear();
  }

  void _startRefreshCycle() {
    _refreshTimer = Timer.periodic(
      const Duration(seconds: 30),
      (_) => _refreshData(),
    );
  }

  Future<void> _refreshData() async {
    try {
      await Future.delayed(const Duration(milliseconds: 100));
      _data['refreshCount'] = (_data['refreshCount'] ?? 0) + 1;
    } catch (e) {
      _data['lastError'] = e.toString();
    }
  }

  Map<String, dynamic> toJson() => Map.from(_data)..['counter'] = _counter;
}

class Widget {
  final String type;
  final String? value;
  final List<Widget>? children;

  Widget({required this.type, this.value, this.children});

  @override
  String toString() => 'Widget($type${value != null ? ': $value' : ''})';
}
