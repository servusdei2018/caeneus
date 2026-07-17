from __future__ import annotations

import os
import platform
from pathlib import Path

from setuptools import Extension, setup


PYTHON_DIR = Path(__file__).resolve().parent
# Build frontends may invoke setup.py from the repository root when the
# project is supplied by absolute path. Anchor every source path to this
# extension directory instead of relying on the process cwd.
NATIVE_STAGE = os.environ.get("CAENEUS_NATIVE_STAGE")
REPOSITORY_ROOT = Path(
    os.environ.get("CAENEUS_ROOT", NATIVE_STAGE or PYTHON_DIR.parents[1])
).resolve()
INCLUDE_DIR = Path(
    os.environ.get(
        "CAENEUS_INCLUDE_DIR",
        Path(NATIVE_STAGE).resolve() if NATIVE_STAGE else REPOSITORY_ROOT / "include",
    )
).resolve()
LIBRARY_DIR = Path(
    os.environ.get(
        "CAENEUS_LIBRARY_DIR",
        Path(NATIVE_STAGE).resolve() / "lib"
        if NATIVE_STAGE
        else REPOSITORY_ROOT / "zig-out" / "lib",
    )
).resolve()


def native_library() -> tuple[list[str], list[str], list[str]]:
    explicit = os.environ.get("CAENEUS_LIBRARY")
    if explicit:
        candidates = [Path(explicit).resolve()]
    elif platform.system() == "Windows":
        candidates = [LIBRARY_DIR / "caeneus-static.lib", LIBRARY_DIR / "caeneus.lib"]
    elif platform.system() == "Darwin":
        candidates = [LIBRARY_DIR / "libcaeneus.a", LIBRARY_DIR / "libcaeneus.dylib"]
    else:
        candidates = [LIBRARY_DIR / "libcaeneus.a", LIBRARY_DIR / "libcaeneus.so"]

    for candidate in candidates:
        if candidate.is_file():
            if candidate.suffix in {".a", ".lib"}:
                return [str(candidate)], [], []
            return [], ["caeneus"], [str(candidate.parent)]

    expected = ", ".join(str(candidate) for candidate in candidates)
    raise RuntimeError(
        "Caeneus native library not found. Build it with "
        "`zig build -Doptimize=ReleaseFast`, stage a release archive, or set "
        "CAENEUS_LIBRARY. "
        f"Checked: {expected}"
    )


extra_objects, libraries, library_dirs = native_library()

system_libraries: list[str] = []
if platform.system() == "Linux":
    system_libraries = ["dl", "pthread", "m"]
elif platform.system() == "Darwin":
    system_libraries = ["pthread"]
elif platform.system() == "Windows":
    system_libraries = ["ntdll"]

extension = Extension(
    "caeneus",
    sources=[str(PYTHON_DIR / "caeneusmodule.c")],
    include_dirs=[str(INCLUDE_DIR)],
    library_dirs=library_dirs,
    libraries=libraries + system_libraries,
    extra_objects=extra_objects,
    extra_compile_args=[] if platform.system() == "Windows" else ["-std=c11", "-O3", "-fomit-frame-pointer", "-march=native", "-flto", "-fno-plt", "-fno-stack-protector", "-funroll-loops"],
    extra_link_args=[] if platform.system() == "Windows" else ["-flto", "-fno-plt"],
)

setup(
    name="caeneus-native",
    version="0.1.2",
    description="Compiled CPython extension for the Caeneus cache engine",
    packages=[],
    py_modules=[],
    ext_modules=[extension],
)
