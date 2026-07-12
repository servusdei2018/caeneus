# Benchmarks

Run benchmarks from the repository root:

```bash
# Fast integration check.
./benchmarks/run-all.sh --quick

# Full comparison run.
./benchmarks/run-all.sh --full

# Full run without competitor libraries.
./benchmarks/run-all.sh --full --caeneus-only
```

The full Go suite runs each benchmark three times by default. Change that with
`--count N`. Run one suite with `run-zig.sh`, `run-go.sh`, `run-python.sh`, or
`run-node.sh`.

`--quick` uses smaller workloads and runs only Caeneus for the bindings. It is
for checking builds and integrations, not for publishing performance results.
The full Go eviction profile uses a large key set and may need several minutes
and substantial memory.

## Requirements

- Zig 0.16.0
- Go 1.26 and a C compiler
- Python 3.9 or newer and `uv`
- Node.js, npm, and the native addon build tools

The runners build the native library with:

```bash
zig build -Doptimize=ReleaseFast
```

The native Zig benchmark and these shell runners currently support Linux and
macOS.

## Suites

- Zig measures the engine directly.
- Go compares Caeneus with BigCache, Ristretto, Otter, and go-cache.
- Python compares Caeneus with `dict`, `cachetools`, and `lru-dict`.
- Node.js compares Caeneus with `Map`, `lru-cache`, and `node-cache`.
- Python also measures shared-cache contention with `get_into`.

Profile A is read-heavy and uses skewed access. Profile B mixes reads and
writes under bounded-cache pressure. The exact operation counts and cache
sizes differ by runtime, so compare implementations within one language first.

The bindings use different read APIs: Go writes to a caller-provided slice,
Python offers allocating `get` and reusable-buffer `get_into`, and Node.js
returns a view over reusable native storage. Include the implementation,
profile, operation count, worker count, value size, hit/miss counts, and
runtime details with saved results. Exclude warmup and setup from steady-state
measurements unless the benchmark reports them explicitly.
