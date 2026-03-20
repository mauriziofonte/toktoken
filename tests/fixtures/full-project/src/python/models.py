"""Data models for the analysis pipeline."""

from typing import Optional, Callable


class DataPoint:
    """Represents a single labeled measurement with an associated weight."""

    def __init__(self, label: str, value: float, weight: float = 1.0) -> None:
        self._label = label
        self._value = value
        self._weight = weight

    @property
    def label(self) -> str:
        return self._label

    @property
    def value(self) -> float:
        return self._value

    @property
    def weight(self) -> float:
        return self._weight

    def validate(self) -> bool:
        """Return True if the data point has valid, usable values."""
        return len(self._label) > 0 and self._weight > 0.0

    def to_dict(self) -> dict:
        """Serialize the data point to a plain dictionary."""
        return {"label": self._label, "value": self._value, "weight": self._weight}


class DataSet:
    """An ordered collection of DataPoint instances with filtering and aggregation."""

    def __init__(self) -> None:
        self._points: list[DataPoint] = []

    def add(self, point: DataPoint) -> None:
        """Append a data point to the set."""
        self._points.append(point)

    def filter(self, predicate: Callable[[DataPoint], bool]) -> list[DataPoint]:
        """Return all points matching the given predicate function."""
        return [p for p in self._points if predicate(p)]

    def aggregate(self) -> dict:
        """Compute total value and average weight across all points."""
        if not self._points:
            return {"total": 0.0, "avg_weight": 0.0, "count": 0}
        total = sum(p.value for p in self._points)
        avg_w = sum(p.weight for p in self._points) / len(self._points)
        return {"total": round(total, 4), "avg_weight": round(avg_w, 4), "count": len(self._points)}


def validate_input(raw: str, min_length: int = 1) -> Optional[str]:
    """Sanitize and validate a raw input string, returning None on failure."""
    cleaned = raw.strip()
    if len(cleaned) < min_length:
        return None
    if any(ch in cleaned for ch in ("\x00", "\n", "\r")):
        return None
    return cleaned
