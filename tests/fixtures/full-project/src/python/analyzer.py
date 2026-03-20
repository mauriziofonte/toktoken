"""Statistical analyzer for structured data points."""

import statistics
from typing import Optional
from models import DataPoint


class Analyzer:
    """Processes collections of data points and produces statistical summaries."""

    def __init__(self, batch_size: int = 100, threshold: float = 0.75) -> None:
        self.batch_size = batch_size
        self.threshold = threshold
        self._results: list[dict] = []

    def run(self, input_files: list[str]) -> list[DataPoint]:
        """Load and validate data points from all input files."""
        points: list[DataPoint] = []
        for path in input_files:
            with open(path, "r") as fh:
                for line_num, line in enumerate(fh, start=1):
                    parts = line.strip().split(",")
                    if len(parts) >= 3:
                        dp = DataPoint(label=parts[0], value=float(parts[1]), weight=float(parts[2]))
                        if dp.validate():
                            points.append(dp)
        return points

    def evaluate(self, points: list[DataPoint], min_weight: float = 0.0) -> list[float]:
        """Filter points by weight and return weighted values."""
        weighted = [
            p.value * p.weight
            for p in points
            if p.weight >= min_weight and p.validate()
        ]
        return weighted

    def summarize(self, points: list[DataPoint]) -> dict:
        """Compute aggregate statistics over a collection of data points."""
        if not points:
            return {"count": 0, "mean": 0.0, "stdev": 0.0, "success_rate": 0.0}
        values = self.evaluate(points)
        valid_count = len(values)
        success_count = sum(1 for v in values if v >= self.threshold)
        return {
            "count": valid_count,
            "mean": round(statistics.mean(values), 4) if values else 0.0,
            "stdev": round(statistics.stdev(values), 4) if len(values) > 1 else 0.0,
            "median": round(statistics.median(values), 4) if values else 0.0,
            "success_rate": round(success_count / valid_count, 4) if valid_count else 0.0,
        }
