# Caeneus CPython extension

This binding is a compiled CPython extension that links the C ABI directly.

On supported platforms, install the self-contained wheel from PyPI:

```bash
uv add caeneus-native
# or
python -m pip install caeneus-native
```

The wheel contains the statically linked native library. It does not require a
Caeneus checkout, Zig, or a C compiler.

For contributor source builds, build the native archive from the repository
root, then use `uv` to build and test the extension:

```bash
zig build -Doptimize=ReleaseFast
uv sync --project ext/python
uv run --project ext/python python ext/python/test_caeneus.py
```

The static archive is selected from `../../zig-out/lib/libcaeneus.a` by
default for a checkout build. Release wheel builds set
`CAENEUS_NATIVE_STAGE` to a staged header and archive. Set `CAENEUS_ROOT`,
`CAENEUS_INCLUDE_DIR`, `CAENEUS_LIBRARY_DIR`, or `CAENEUS_LIBRARY` to use
another native artifact.

Published wheels currently cover Linux x86_64/aarch64, macOS arm64, and
Windows x86_64. Other platforms can use the source-build path with a suitable
native archive.

`Cache.get()` returns a Python `bytes` object. By default it seeds a 128-byte
read buffer so typical values write in one copy; pass
`initial_value_capacity=0` to force a size probe. Contended and large native
operations run outside the GIL; once the cache observes concurrent callers,
small gets also release the GIL. A per-cache freelist reuses the prior `bytes`
object when callers do not retain it.

Prefer `Cache.get_into()` for read-heavy concurrent workloads: it writes into a
caller-owned `bytearray` and avoids allocating `bytes` on the read path.
Published multi-thread charts use `--api get_into`.

```python
scratch = bytearray(128)
length = cache.get_into("key", scratch)
value = None if length is None else bytes(scratch[:length])
```
