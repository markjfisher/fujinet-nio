#!/usr/bin/env python3
"""Cheap manifest and dry-run checks for the Story 2.3 lab runner."""
import importlib.util
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("link_experiment", HERE / "link_experiment.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main():
    manifests = sorted(HERE.glob("L*/experiment.json"))
    assert len(manifests) == 10
    for expected, path in enumerate(manifests):
        manifest = runner.load_manifest(path)
        assert manifest["scenario"] == expected
        assert (path.parent / "run.sh").is_file()
        assert (path.parent / "README.md").is_file()
        assert manifest["wiring"]["rp2350"]["SCLK"] == 2
        assert manifest["wiring"]["esp32s3"]["SCLK"] == 12
    runner.show_plan(runner.load_manifest(manifests[0]))
    print("link experiment runner tests passed")


if __name__ == "__main__":
    main()
