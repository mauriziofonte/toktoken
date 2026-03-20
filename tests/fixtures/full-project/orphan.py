"""Orphan module — not imported by any other file in the project.
Used to test find:dead detection of unreferenced code."""


def orphan_function(data):
    """Process data that nobody calls."""
    if not isinstance(data, list):
        return []
    cleaned = [item.strip() for item in data if isinstance(item, str)]
    unique = list(set(cleaned))
    unique.sort()
    return unique


def unused_helper(value, multiplier=2):
    """Another function that is never imported or called."""
    if value is None:
        return 0
    return int(value) * multiplier


class DeadClass:
    """A class that exists but is never instantiated anywhere."""

    def __init__(self, name):
        self.name = name
        self._cache = {}

    def compute(self, key):
        if key in self._cache:
            return self._cache[key]
        result = hash(f"{self.name}:{key}") % 1000
        self._cache[key] = result
        return result

    def reset(self):
        self._cache.clear()
