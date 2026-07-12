# Caeneus

Caeneus is a local, high-performance, in-memory cache engine written in Zig. It prioritizes concurrency, throughput, and maintaining a flat tail latency under concurrent write and eviction pressure.

## Get Started

```bash
# Build
zig build -Doptimize=ReleaseFast

# Benchmark
zig build run -Doptimize=ReleaseFast

# Test
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

## Contributing

Contributions are welcome! Please feel free to open issues or submit pull requests. See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the [MIT License](LICENSE).