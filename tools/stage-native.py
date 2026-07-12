#!/usr/bin/env python3
"""Stage a Caeneus static library and header for binding builds."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import platform
import shutil
import subprocess
import sys
import tarfile
import os
import tempfile
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GITHUB_REPOSITORY = os.environ.get("GITHUB_REPOSITORY", "servusdei2018/caeneus")


def host_target() -> dict[str, object]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "darwin":
        system = "darwin"
    elif system == "windows":
        system = "windows"
    elif system == "linux":
        system = "linux"
    else:
        raise RuntimeError(f"unsupported host operating system: {platform.system()}")

    manifest = json.loads((ROOT / "tools" / "native-targets.json").read_text())
    for target in manifest["targets"]:
        if target["os"] == system and machine in {
            value.lower() for value in target["architectures"]
        }:
            return target
    raise RuntimeError(
        f"unsupported host target: {platform.system()} {platform.machine()}"
    )


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "caeneus-release"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def checksum_for(contents: bytes, filename: str) -> str:
    for line in contents.decode().splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1] == filename:
            return fields[0].lower()
    raise RuntimeError(f"{filename} is missing from checksums.txt")


def extract_archive(archive: bytes, destination: Path, library_name: str) -> None:
    expected = {"caeneus.h", library_name}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.isdir():
                continue
            name = Path(member.name).name
            if not member.isfile() or name not in expected:
                raise RuntimeError(f"unexpected file in native archive: {member.name}")
            output = destination / ("lib" if name == library_name else "include") / name
            output.parent.mkdir(parents=True, exist_ok=True)
            source = tar.extractfile(member)
            if source is None:
                raise RuntimeError(f"could not read native archive member: {name}")
            with source, output.open("wb") as target:
                shutil.copyfileobj(source, target)
            expected.remove(name)
    if expected:
        raise RuntimeError(
            f"native archive is missing: {', '.join(sorted(expected))}"
        )


def stage_local(destination: Path, library_name: str) -> None:
    with tempfile.TemporaryDirectory(prefix="caeneus-native-build-") as temporary:
        subprocess.run(
            [
                "zig",
                "build",
                "-Doptimize=ReleaseFast",
                "-p",
                temporary,
            ],
            cwd=ROOT,
            check=True,
        )
        library = Path(temporary) / "lib" / library_name
        if not library.is_file():
            raise RuntimeError(f"Zig build did not produce {library}")
        (destination / "lib").mkdir(parents=True, exist_ok=True)
        shutil.copy2(library, destination / "lib" / library_name)
    (destination / "include").mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "include" / "caeneus.h", destination / "include" / "caeneus.h")


def stage_release(destination: Path, version: str, target: dict[str, object]) -> None:
    release_platform = target["binding_release_platform"]
    library_name = target["static_library"]
    asset = f"caeneus-{version}-{release_platform}.tar.gz"
    base = f"https://github.com/{GITHUB_REPOSITORY}/releases/download/{version}"
    archive = download(f"{base}/{asset}")
    checksums = download(f"{base}/checksums.txt")
    expected = checksum_for(checksums, asset)
    actual = hashlib.sha256(archive).hexdigest()
    if actual != expected:
        raise RuntimeError(f"checksum mismatch for {asset}")
    extract_archive(archive, destination, library_name)


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--local", action="store_true")
    source.add_argument("--version", help="release tag, for example v0.1.0")
    parser.add_argument("--dest", required=True, type=Path)
    args = parser.parse_args()

    target = host_target()
    destination = args.dest.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    library_name = target["static_library"]
    if args.local:
        stage_local(destination, library_name)
    else:
        stage_release(destination, args.version, target)

    print(f"staged {target['release_platform']} native library in {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
