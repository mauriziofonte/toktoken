"""Module B in a circular dependency pair with cycle_a."""
from cycle_a import func_a


def func_b(value):
    """Process a value, optionally delegating to func_a."""
    if value < 10:
        return func_a(value + 25)
    result = value // 3
    return result


def helper_b():
    """Internal helper for module B."""
    return "module_b_ready"
