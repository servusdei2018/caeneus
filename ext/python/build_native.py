#!/usr/bin/env python3
"""Download the tagged Caeneus native archive for a wheel build."""

from __future__ import annotations

import argparse
import hashlib
import io
import platform
import os
import tarfile
import urllib.request
from pathlib import Path


REPOSITORY = os.environ.get("GITHUB_REPOSITORY", "servusdei2018/caeneus")


def target() -> tuple[str, str]:
    system = platform.system().lower()
    machine = platform.machine().lower()
    if system == "linux" and machine in {"x86_64", "amd64"}:
        return "linux_amd64", "libcaeneus.a"
    if system == "linux" and machine in {"aarch64", "arm64"}:
        return "linux_arm64", "libcaeneus.a"
    if system == "darwin" and machine in {"arm64", "aarch64"}:
        return "darwin_arm64", "libcaeneus.a"
    if system == "windows" and machine in {"amd64", "x86_64"}:
        return "windows_amd64_msvc", "caeneus-static.lib"
    raise RuntimeError(f"unsupported wheel target: {platform.system()} {platform.machine()}")


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "caeneus-wheel-build"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def checksum_for(contents: bytes, filename: str) -> str:
    for line in contents.decode().splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[1] == filename:
            return fields[0].lower()
    raise RuntimeError(f"{filename} is missing from checksums.txt")


def stage(version: str, destination: Path) -> None:
    release_platform, library_name = target()
    asset = f"caeneus-{version}-{release_platform}.tar.gz"
    base = f"https://github.com/{REPOSITORY}/releases/download/{version}"
    archive = download(f"{base}/{asset}")
    expected = checksum_for(download(f"{base}/checksums.txt"), asset)
    actual = hashlib.sha256(archive).hexdigest()
    if actual != expected:
        raise RuntimeError(f"checksum mismatch for {asset}")

    destination.mkdir(parents=True, exist_ok=True)
    wanted = {"caeneus.h", library_name}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.isdir():
                continue
            name = Path(member.name).name
            if not member.isfile() or name not in wanted:
                raise RuntimeError(f"unexpected file in native archive: {member.name}")
            source = tar.extractfile(member)
            if source is None:
                raise RuntimeError(f"could not read archive member: {member.name}")
            with source:
                output = destination / name
                output.write_bytes(source.read())
            wanted.remove(name)
    if wanted:
        raise RuntimeError(f"native archive is missing: {', '.join(sorted(wanted))}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--dest", required=True, type=Path)
    args = parser.parse_args()
    stage(args.version, args.dest / "lib")
    header = args.dest / "lib" / "caeneus.h"
    header.replace(args.dest / "caeneus.h")
    print(f"staged {args.version} native archive in {args.dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
