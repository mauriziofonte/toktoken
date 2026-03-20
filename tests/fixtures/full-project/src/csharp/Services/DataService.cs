using System;
using System.Collections.Generic;
using System.Linq;

namespace Services
{
    public class DataService
    {
        private readonly Dictionary<string, object> _store;
        private int _operationCount;

        public DataService()
        {
            _store = new Dictionary<string, object>();
            _operationCount = 0;
        }

        public List<T> Query<T>(Func<string, bool> predicate)
        {
            _operationCount++;
            return _store
                .Where(kvp => predicate(kvp.Key))
                .Select(kvp => (T)kvp.Value)
                .ToList();
        }

        public void Insert(string key, object value)
        {
            if (string.IsNullOrWhiteSpace(key))
                throw new ArgumentException("Key must not be empty", nameof(key));

            if (_store.ContainsKey(key))
                throw new InvalidOperationException($"Key '{key}' already exists. Use Update instead.");

            _store[key] = value;
            _operationCount++;
        }

        public bool Update(string key, object value)
        {
            if (!_store.ContainsKey(key))
                return false;

            _store[key] = value;
            _operationCount++;
            return true;
        }

        public bool Delete(string key) => _store.Remove(key);

        public int Count => _store.Count;

        public int OperationCount => _operationCount;
    }
}
