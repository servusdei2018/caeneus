# Caeneus

Caeneus is a concurrent in-memory cache engine written in Zig. It is designed
for high throughput and predictable latency under concurrent reads, writes, and
eviction.

## Get Started

```bash
zig build -Doptimize=ReleaseFast
zig build test
```

## Usage

```zig
const std = @import("std");
const caeneus = @import("caeneus");
const Engine = caeneus.Engine;

// Caeneus is awesome.
pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // 64 shards for concurrency, 1k slots each, and a 1MB slab pool per shard.
    // Shard count must be a power of two for fast bitwise masking.
    var cache = try Engine.init(allocator, 64, 1024, 1024 * 1024);
    defer cache.deinit();

    // Writes are allocation-free.
    try cache.set("session_user_42", "{\"name\":\"Nate\",\"role\":\"necromancer\"}");

    // Lookups are lock-free and safe from concurrent eviction overwrites.
    var buffer: [256]u8 = undefined;
    if (try cache.get("session_user_42", &buffer)) |value| {
        std.debug.print("Retrieved value: {s}\n", .{value});
    } else {
        std.debug.print("Session not found (expired or evicted)\n", .{});
    }
}
```

## Bindings

- Go, Python, and Node.js bindings are in [`ext/`](ext/).
- Python and Node packages can be installed directly from their registries on
  supported platforms:

  ```bash
  uv add caeneus-native
  npm install caeneus-native
  ```

- Setup and contributor source-build instructions are in
  [`GETTING_STARTED.md`](GETTING_STARTED.md).
- Binding-specific build and API notes are in the corresponding README.

## Benchmarks

Run the quick integration suite with:

```bash
./benchmarks/run-all.sh --quick
```

See [`benchmarks/README.md`](benchmarks/README.md) for all runners and
requirements.

## Contributing

Contributions are welcome! Please feel free to open issues or submit pull requests. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the [MIT License](LICENSE).
