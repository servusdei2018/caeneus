#!/usr/bin/env python3
"""Validate that a release tag matches every published binding."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_pyproject_version() -> str:
    contents = (ROOT / "ext" / "python" / "pyproject.toml").read_text()
    match = re.search(r"^version\s*=\s*[\"']([^\"']+)[\"']\s*$", contents, re.MULTILINE)
    if match is None:
        raise ValueError("could not find Python project version")
    return match.group(1)


def read_node_version() -> str:
    package = json.loads((ROOT / "ext" / "node" / "package.json").read_text())
    version = package.get("version")
    if not isinstance(version, str):
        raise ValueError("could not find Node project version")
    return version


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tag", help="release tag, for example v0.1.0")
    args = parser.parse_args()

    tag_version = args.tag.removeprefix("v")
    manifest = json.loads((ROOT / "tools" / "native-targets.json").read_text())
    versions = {
        "release tag": tag_version,
        "native target manifest": manifest["version"],
        "Python package": read_pyproject_version(),
        "Node package": read_node_version(),
    }
    mismatches = {
        name: version for name, version in versions.items() if version != tag_version
    }
    if mismatches:
        details = ", ".join(f"{name}={version}" for name, version in versions.items())
        print(f"release version mismatch: {details}", file=sys.stderr)
        return 1

    print(f"release version {tag_version} is consistent")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
