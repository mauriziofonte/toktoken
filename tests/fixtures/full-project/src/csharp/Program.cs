using System;
using System.Collections.Generic;
using Services;

namespace MyApp
{
    class Program
    {
        private static readonly string Version = "3.1.0";
        private static DataService _dataService;
        private static Dictionary<string, string> _settings;

        static void Main(string[] args)
        {
            Console.WriteLine($"MyApp v{Version} initializing...");
            try
            {
                Initialize(args);
                _dataService = new DataService();

                _dataService.Insert("app.version", Version);
                _dataService.Insert("app.startedAt", DateTime.UtcNow.ToString("o"));

                var records = _dataService.Query<string>(r => r.StartsWith("app."));
                Console.WriteLine($"Loaded {records.Count} app records");

                foreach (var arg in args)
                {
                    Console.WriteLine($"  arg: {arg}");
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Fatal error: {ex.Message}");
                Environment.Exit(1);
            }
        }

        static void Initialize(string[] args)
        {
            _settings = new Dictionary<string, string>
            {
                { "environment", args.Length > 0 ? args[0] : "production" },
                { "logLevel", "info" },
                { "maxConnections", "100" }
            };
            Console.WriteLine($"Environment: {_settings["environment"]}");
        }

        public static string GetVersion() => Version;
    }
}
