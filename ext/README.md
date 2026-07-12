# Language extensions

These directories contain compiled bindings over the public C ABI:

- `go/` — cgo client and native staging command.
- `python/` — self-contained PyPI wheel and CPython extension.
- `node/` — npm/Bun package with Node-API prebuilds and TypeScript API.

Use `uv add caeneus-native`, `npm install caeneus-native`, or
`bun add caeneus-native` on a supported platform. See
[`../GETTING_STARTED.md`](../GETTING_STARTED.md) for contributor source-build
instructions.
Each binding README has additional build and API details.
