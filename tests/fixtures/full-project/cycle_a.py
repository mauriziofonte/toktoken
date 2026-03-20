"""Module A in a circular dependency pair with cycle_b."""
from cycle_b import func_b


def func_a(value):
    """Process a value, optionally delegating to func_b."""
    if value > 100:
        return func_b(value - 50)
    result = value * 2 + 1
    return result


def helper_a():
    """Internal helper for module A."""
    return "module_a_ready"
