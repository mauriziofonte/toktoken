"""Pipeline orchestrator for data analysis."""

import argparse
import sys
from analyzer import Analyzer


def parse_config(config_path: str) -> dict:
    """Parse configuration from file path into a settings dictionary."""
    defaults = {
        "batch_size": 100,
        "threshold": 0.75,
        "output_format": "json",
        "verbose": False,
        "max_retries": 3,
    }
    config = dict(defaults)
    with open(config_path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("=")
            config[key.strip()] = value.strip()
    return config


def run_pipeline(config: dict, input_files: list[str]) -> int:
    """Execute the full analysis pipeline and return exit code."""
    analyzer = Analyzer(
        batch_size=int(config["batch_size"]),
        threshold=float(config["threshold"]),
    )
    results = analyzer.run(input_files)
    summary = analyzer.summarize(results)
    if config["verbose"]:
        for key, value in summary.items():
            print(f"  {key}: {value}")
    return 0 if summary.get("success_rate", 0) >= float(config["threshold"]) else 1


def main() -> int:
    """Entry point: parse arguments and launch the pipeline."""
    parser = argparse.ArgumentParser(description="Data analysis pipeline")
    parser.add_argument("inputs", nargs="+", help="Input data files")
    parser.add_argument("-c", "--config", default="config.ini", help="Config file path")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose output")
    args = parser.parse_args()

    config = parse_config(args.config)
    config["verbose"] = args.verbose or config["verbose"]
    return run_pipeline(config, args.inputs)


if __name__ == "__main__":
    sys.exit(main())
