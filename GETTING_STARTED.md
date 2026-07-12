# Getting Started

## Build

The public C header is [`include/caeneus.h`](include/caeneus.h). Build the
native library with:

```bash
zig build -Doptimize=ReleaseFast
```

## Go

The Go client uses cgo. From `ext/go`:

```bash
cd ext/go
go get github.com/servusdei2018/caeneus@v0.1.0
eval "$(go run github.com/servusdei2018/caeneus/cmd/caeneus-native@v0.1.0 --version v0.1.0)"
go test ./...
```

The staging command downloads and caches the native archive. It requires a C
compiler for cgo.

## Python

Install the published wheel on a supported platform:

```bash
uv add caeneus-native
```

The package imports as `caeneus` and contains the native library, so this does
not require a Caeneus checkout, Zig, or a local compiler.

For contributor source builds, build the native archive from the repository
root and then install the local extension:

```bash
zig build -Doptimize=ReleaseFast
uv sync --project ext/python
uv run --project ext/python python ext/python/test_caeneus.py
```

See [`ext/python/README.md`](ext/python/README.md) for binding details.

## Node.js

Install the published N-API package on a supported platform:

```bash
npm install caeneus-native
# or
bun add caeneus-native
```

The package includes a prebuilt addon and does not require cloning Caeneus,
installing Zig, or compiling native code.

For contributor source builds, install the native addon build tools and run:

```bash
zig build -Doptimize=ReleaseFast
cd ext/node
npm install
npm test
```

If building from a published source package, set `CAENEUS_ROOT`,
`CAENEUS_INCLUDE_DIR`, and `CAENEUS_LIBRARY_DIR`, then use
`CAENEUS_BUILD_FROM_SOURCE=1 npm install`.

See [`ext/node/README.md`](ext/node/README.md) for binding details.