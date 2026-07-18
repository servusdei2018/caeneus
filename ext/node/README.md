# Caeneus Node.js binding

This package provides a V8 Fast API addon for Node and a Bun-compatible addon
that link the Caeneus C ABI directly.

On supported platforms, install the package from npm or Bun:

```bash
npm install caeneus-native
# or
bun add caeneus-native
```

The package includes statically linked native prebuilds, so normal installs do
not require a Caeneus checkout, Zig, or a C++ compiler.
Node 22 and newer use the V8 Fast API addon, with prebuilds selected for the
Node module version.

For contributor source builds, build the native archive from the repository
root, install the Node build tools, then run:

```bash
zig build -Doptimize=ReleaseFast
cd ext/node
npm install
npm test
```

`binding.gyp` links `zig-out/lib/libcaeneus.a` by default for a checkout build.
`npm run build:native` builds the Node Fast API addon. Build the Bun addon with
`npm run build:native:bun`.
Override the repository and native library locations with:

```bash
node-gyp rebuild \
  --caeneus_root=/path/to/caeneus \
  --caeneus_lib_dir=/path/to/caeneus/zig-out/lib
```

For a dynamic build, use:

```bash
node-gyp rebuild --caeneus_shared=1
```

On Linux this links `libcaeneus.so` and uses the configured library directory
at runtime.

Published prebuilds currently cover Linux x64/arm64, macOS arm64, and Windows
x64. An unsupported target reports an actionable error. To build from source
with an external native archive, set `CAENEUS_BUILD_FROM_SOURCE=1`,
`CAENEUS_ROOT`, `CAENEUS_INCLUDE_DIR`, and `CAENEUS_LIBRARY_DIR` before
installing.

The Node addon is tied to the Node/V8 module version. The package selects a
matching Node prebuild at runtime. Bun uses its separate Node-API prebuild.

`Cache.get()` returns a `Buffer` view over reusable native scratch storage.
Consume the view before the next `get()` call — a later read may overwrite the
same backing memory (same-length hits reuse the same Buffer object). Keys may
be strings or `Buffer`s; prefer `Buffer` keys when you already have bytes to
skip string conversion.

Default published charts measure `--api get`. For reusable caller-owned output
with no Buffer wrapper on the hot path, use `Cache.getInto(key, output)`: it
writes into a caller-owned `Buffer` and returns the byte count, or `null` for a
miss. The output buffer must be large enough for the value.
