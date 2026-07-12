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

`Cache.get()` returns a new Python `bytes` object. Native calls run outside the
GIL, and repeated reads reuse the learned value capacity.

`Cache.get_into()` writes into a caller-owned `bytearray` and avoids allocating
`bytes` on the read path:

```python
scratch = bytearray(128)
length = cache.get_into("key", scratch)
value = None if length is None else bytes(scratch[:length])
```
