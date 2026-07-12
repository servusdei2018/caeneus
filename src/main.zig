const std = @import("std");
const caeneus = @import("caeneus");
const Engine = caeneus.Engine;
const Shard = caeneus.Shard;
const SlabPool = caeneus.SlabPool;

fn nanoTime() u64 {
    var ts: std.posix.timespec = undefined;
    _ = std.posix.system.clock_gettime(.MONOTONIC, &ts);
    return @as(u64, @intCast(ts.sec)) * std.time.ns_per_s + @as(u64, @intCast(ts.nsec));
}

const Timer = struct {
    start_time: u64,

    pub fn start() !Timer {
        return Timer{ .start_time = nanoTime() };
    }

    pub fn read(self: Timer) u64 {
        return nanoTime() - self.start_time;
    }
};

/// Custom Tracking Allocator to verify zero runtime heap allocations
const TrackingAllocator = struct {
    backing: std.mem.Allocator,
    alloc_count: usize = 0,
    resize_count: usize = 0,
    free_count: usize = 0,

    pub fn allocator(self: *TrackingAllocator) std.mem.Allocator {
        return .{
            .ptr = self,
            .vtable = &.{
                .alloc = alloc,
                .resize = resize,
                .remap = remap,
                .free = free,
            },
        };
    }

    fn alloc(ctx: *anyopaque, len: usize, ptr_align: std.mem.Alignment, ret_addr: usize) ?[*]u8 {
        const self: *TrackingAllocator = @ptrCast(@alignCast(ctx));
        self.alloc_count += 1;
        return self.backing.vtable.alloc(self.backing.ptr, len, ptr_align, ret_addr);
    }

    fn resize(ctx: *anyopaque, buf: []u8, buf_align: std.mem.Alignment, new_len: usize, ret_addr: usize) bool {
        const self: *TrackingAllocator = @ptrCast(@alignCast(ctx));
        self.resize_count += 1;
        return self.backing.vtable.resize(self.backing.ptr, buf, buf_align, new_len, ret_addr);
    }

    fn remap(ctx: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, ret_addr: usize) ?[*]u8 {
        const self: *TrackingAllocator = @ptrCast(@alignCast(ctx));
        self.alloc_count += 1; // remapping acts as allocation/resize
        return self.backing.vtable.remap(self.backing.ptr, memory, alignment, new_len, ret_addr);
    }

    fn free(ctx: *anyopaque, buf: []u8, buf_align: std.mem.Alignment, ret_addr: usize) void {
        const self: *TrackingAllocator = @ptrCast(@alignCast(ctx));
        self.free_count += 1;
        self.backing.vtable.free(self.backing.ptr, buf, buf_align, ret_addr);
    }
};

fn nextPowerLaw(random: std.Random, n: usize, skew_power: usize) usize {
    const u = random.float(f64);
    var p = u;
    var j: usize = 1;
    while (j < skew_power) : (j += 1) {
        p *= u;
    }
    const val = @as(usize, @intFromFloat(@as(f64, @floatFromInt(n)) * p));
    return val % n;
}

pub fn main() !void {
    std.debug.print("==================================================\n", .{});
    std.debug.print("   Caeneus Benchmark\n", .{});
    std.debug.print("==================================================\n\n", .{});

    const allocator = std.heap.page_allocator;

    // --- BENCHMARK CONFIGURATION ---
    const num_threads = 4;
    const ops_per_thread = 200_000;
    const read_ratio = 0.95; // 95% reads, 5% writes (skewed read-heavy cache workload)
    const num_keys = 150_000; // Key cardinality
    const skew_power = 3; // Power-law exponent for skewing access frequency
    const num_shards = 64;
    const slots_per_shard = 2048;
    const slab_size_per_shard = 2 * 1024 * 1024; // 2MB slab per shard

    std.debug.print("Workload Profile:\n", .{});
    std.debug.print("  Threads:             {}\n", .{num_threads});
    std.debug.print("  Ops per Thread:      {}\n", .{ops_per_thread});
    std.debug.print("  Read Ratio:          {d:.0}%\n", .{read_ratio * 100.0});
    std.debug.print("  Key Cardinality:     {}\n", .{num_keys});
    std.debug.print("  Access Skew (Power): {}\n", .{skew_power});
    std.debug.print("  Cache Config:        {} shards, {} slots/shard, {d:.1} MB slab/shard\n", .{
        num_shards,
        slots_per_shard,
        @as(f64, @floatFromInt(slab_size_per_shard)) / 1024.0 / 1024.0,
    });
    std.debug.print("--------------------------------------------------\n\n", .{});

    std.debug.print("Running Benchmarks...\n", .{});
    const result = try runEngineBenchmark(Engine, allocator, num_threads, ops_per_thread, read_ratio, num_keys, skew_power, num_shards, slots_per_shard, slab_size_per_shard);

    std.debug.print("==================================================\n", .{});
    std.debug.print("               PERFORMANCE METRICS                \n", .{});
    std.debug.print("==================================================\n", .{});
    std.debug.print("Throughput (ops/s):  {d:.2}\n", .{result.throughput});
    std.debug.print("Hit Ratio:           {d:.2}%\n", .{result.hit_ratio * 100.0});
    std.debug.print("Latency (p50):       {d:.3} us\n", .{result.p50});
    std.debug.print("Latency (p99):       {d:.3} us\n", .{result.p99});
    std.debug.print("Latency (p99.9):     {d:.3} us\n", .{result.p999});
    std.debug.print("Latency (p99.99):    {d:.3} us\n", .{result.p9999});
    std.debug.print("==================================================\n", .{});
}

const BenchResult = struct {
    throughput: f64,
    hit_ratio: f64,
    p50: f64,
    p99: f64,
    p999: f64,
    p9999: f64,
};

fn runEngineBenchmark(
    comptime EngineType: type,
    allocator: std.mem.Allocator,
    num_threads: usize,
    ops_per_thread: usize,
    read_ratio: f64,
    num_keys: usize,
    skew_power: usize,
    num_shards: u32,
    slots_per_shard: u32,
    slab_size_per_shard: usize,
) !BenchResult {
    var engine = try EngineType.init(allocator, num_shards, slots_per_shard, slab_size_per_shard);
    defer engine.deinit();

    // Populate cache first to reach steady state eviction
    {
        var key_buf: [32]u8 = undefined;
        var val_buf: [64]u8 = undefined;
        @memset(&val_buf, 'P');
        var prng = std.Random.DefaultPrng.init(12345);
        const rand = prng.random();
        var i: usize = 0;
        const warmup_keys = num_shards * slots_per_shard;
        while (i < warmup_keys) : (i += 1) {
            const key = try std.fmt.bufPrint(&key_buf, "key_{}", .{i});
            const val_len = rand.intRangeAtMost(usize, 10, 60);
            try engine.set(key, val_buf[0..val_len]);
        }
    }

    const sample_size = 10_000;
    var threads = try allocator.alloc(std.Thread, num_threads);
    defer allocator.free(threads);

    var latency_samples = try allocator.alloc(u32, num_threads * sample_size);
    defer allocator.free(latency_samples);
    @memset(latency_samples, 0);

    var num_gets = try allocator.alloc(usize, num_threads);
    defer allocator.free(num_gets);
    @memset(num_gets, 0);

    var num_hits = try allocator.alloc(usize, num_threads);
    defer allocator.free(num_hits);
    @memset(num_hits, 0);

    var timer = try Timer.start();

    var i: usize = 0;
    while (i < num_threads) : (i += 1) {
        threads[i] = try std.Thread.spawn(.{}, benchmarkWorker, .{
            EngineType,
            &engine,
            i,
            ops_per_thread,
            read_ratio,
            num_keys,
            skew_power,
            latency_samples[i * sample_size .. (i + 1) * sample_size],
            &num_gets[i],
            &num_hits[i],
        });
    }

    for (threads) |t| {
        t.join();
    }

    const elapsed_ns = timer.read();

    const total_ops = num_threads * ops_per_thread;
    const throughput = (@as(f64, @floatFromInt(total_ops)) / @as(f64, @floatFromInt(elapsed_ns))) * 1e9;

    var total_gets: usize = 0;
    var total_hits: usize = 0;
    for (num_gets, num_hits) |g, h| {
        total_gets += g;
        total_hits += h;
    }
    const hit_ratio = if (total_gets > 0) @as(f64, @floatFromInt(total_hits)) / @as(f64, @floatFromInt(total_gets)) else 0.0;
    std.mem.sort(u32, latency_samples, {}, std.sort.asc(u32));

    const p50 = @as(f64, @floatFromInt(latency_samples[latency_samples.len * 50 / 100])) / 1000.0;
    const p99 = @as(f64, @floatFromInt(latency_samples[latency_samples.len * 99 / 100])) / 1000.0;
    const p999 = @as(f64, @floatFromInt(latency_samples[latency_samples.len * 999 / 1000])) / 1000.0;
    const p9999 = @as(f64, @floatFromInt(latency_samples[latency_samples.len * 9999 / 10000])) / 1000.0;

    std.debug.print("  Completed in {d:.3} ms, Throughput: {d:.2} ops/sec, Hit Ratio: {d:.2}%\n\n", .{
        @as(f64, @floatFromInt(elapsed_ns)) / 1e6,
        throughput,
        hit_ratio * 100.0,
    });

    return BenchResult{
        .throughput = throughput,
        .hit_ratio = hit_ratio,
        .p50 = p50,
        .p99 = p99,
        .p999 = p999,
        .p9999 = p9999,
    };
}

fn benchmarkWorker(
    comptime EngineType: type,
    engine: *EngineType,
    thread_id: usize,
    ops_count: usize,
    read_ratio: f64,
    num_keys: usize,
    skew_power: usize,
    latency_samples: []u32,
    num_gets: *usize,
    num_hits: *usize,
) void {
    var key_buf: [32]u8 = undefined;
    var val_buf: [64]u8 = undefined;
    var read_buf: [64]u8 = undefined;

    var prng = std.Random.DefaultPrng.init(thread_id * 54321 + 9876);
    const rand = prng.random();

    @memset(&val_buf, 'V');

    const sample_interval = ops_count / latency_samples.len;
    var sample_idx: usize = 0;

    var gets: usize = 0;
    var hits: usize = 0;

    var timer = Timer.start() catch unreachable;

    var i: usize = 0;
    while (i < ops_count) : (i += 1) {
        const is_read = rand.float(f64) < read_ratio;
        const key_num = nextPowerLaw(rand, num_keys, skew_power);
        const key = std.fmt.bufPrint(&key_buf, "key_{}", .{key_num}) catch unreachable;

        const record_latency = (sample_interval > 0) and (i % sample_interval == 0) and (sample_idx < latency_samples.len);
        const start = if (record_latency) timer.read() else 0;

        if (is_read) {
            gets += 1;
            const res = engine.get(key, &read_buf, null) catch |err| {
                std.debug.print("Thread {} Get error: {}\n", .{ thread_id, err });
                return;
            };
            if (res != null) {
                hits += 1;
            }
        } else {
            const val_len = rand.intRangeAtMost(usize, 10, 60);
            const val = val_buf[0..val_len];
            engine.set(key, val) catch |err| {
                std.debug.print("Thread {} Set error: {}\n", .{ thread_id, err });
                return;
            };
        }

        if (record_latency) {
            const end = timer.read();
            latency_samples[sample_idx] = @intCast(end - start);
            sample_idx += 1;
        }
    }

    num_gets.* = gets;
    num_hits.* = hits;
}

// ============================================================================
// UNIT TESTS
// ============================================================================

test "basic operations" {
    const allocator = std.testing.allocator;
    var engine = try Engine.init(allocator, 4, 128, 32 * 1024);
    defer engine.deinit();

    var read_buf: [128]u8 = undefined;

    const missing = try engine.get("nonexistent", &read_buf, null);
    try std.testing.expect(missing == null);

    try engine.set("hello", "world");
    const got = try engine.get("hello", &read_buf, null);
    try std.testing.expect(got != null);
    try std.testing.expectEqualSlices(u8, "world", got.?);

    try engine.set("hello", "antigravity");
    const updated = try engine.get("hello", &read_buf, null);
    try std.testing.expect(updated != null);
    try std.testing.expectEqualSlices(u8, "antigravity", updated.?);

    try engine.set("hello", "low-latency");
    const got_short = try engine.get("hello", &read_buf, null);
    try std.testing.expect(got_short != null);
    try std.testing.expectEqualSlices(u8, "low-latency", got_short.?);
}

test "eviction correctness" {
    const allocator = std.testing.allocator;
    var engine = try Engine.init(allocator, 1, 64, 16 * 1024);
    defer engine.deinit();

    var read_buf: [32]u8 = undefined;

    try engine.set("item_0", "val_0");
    try engine.set("item_1", "val_1");
    try engine.set("item_2", "val_2");
    try engine.set("item_3", "val_3");
    try engine.set("item_4", "val_4");
    try engine.set("item_5", "val_5");

    _ = try engine.get("item_0", &read_buf, null);
    _ = try engine.get("item_1", &read_buf, null);

    try engine.set("item_6", "val_6");

    const got_item_0 = try engine.get("item_0", &read_buf, null);
    try std.testing.expect(got_item_0 != null);
    try std.testing.expectEqualSlices(u8, "val_0", got_item_0.?);

    const got_item_6 = try engine.get("item_6", &read_buf, null);
    try std.testing.expect(got_item_6 != null);
    try std.testing.expectEqualSlices(u8, "val_6", got_item_6.?);

    try engine.set("item_7", "val_7");

    const got_item_1 = try engine.get("item_1", &read_buf, null);
    try std.testing.expect(got_item_1 != null);
    try std.testing.expectEqualSlices(u8, "val_1", got_item_1.?);

    try engine.set("item_8", "val_8");

    const got_item_2 = try engine.get("item_2", &read_buf, null);
    try std.testing.expect(got_item_2 == null);
}

test "concurrency safety under heavy contention" {
    const allocator = std.testing.allocator;
    var engine = try Engine.init(allocator, 16, 256, 64 * 1024);
    defer engine.deinit();

    const num_threads = 4;
    const ops = 5000;
    var threads: [num_threads]std.Thread = undefined;

    var i: usize = 0;
    while (i < num_threads) : (i += 1) {
        threads[i] = try std.Thread.spawn(.{}, benchmarkWorkerSimple, .{ &engine, i, ops });
    }

    for (threads) |t| {
        t.join();
    }
}

fn benchmarkWorkerSimple(engine: *Engine, thread_id: usize, ops_count: usize) void {
    var key_buf: [32]u8 = undefined;
    var val_buf: [64]u8 = undefined;
    var read_buf: [64]u8 = undefined;
    var prng = std.Random.DefaultPrng.init(thread_id * 54321);
    const rand = prng.random();
    @memset(&val_buf, 'V');
    var i: usize = 0;
    while (i < ops_count) : (i += 1) {
        const key = std.fmt.bufPrint(&key_buf, "t_{}_k_{}", .{ thread_id, i }) catch unreachable;
        const val_len = rand.intRangeAtMost(usize, 10, 60);
        const val = val_buf[0..val_len];
        engine.set(key, val) catch return;
        _ = engine.get(key, &read_buf, null) catch return;
    }
}

test "tail latency" {
    const allocator = std.testing.allocator;
    var engine = try Engine.init(allocator, 4, 64, 16 * 1024);
    defer engine.deinit();

    var key_buf: [32]u8 = undefined;
    var val_buf: [64]u8 = undefined;
    @memset(&val_buf, 'K');

    var i: usize = 0;
    while (i < 1000) : (i += 1) {
        const key = try std.fmt.bufPrint(&key_buf, "k_{}", .{i});
        try engine.set(key, &val_buf);
    }

    var latencies: std.ArrayList(u64) = .empty;
    defer latencies.deinit(allocator);
    try latencies.ensureTotalCapacity(allocator, 1000);

    var timer = try Timer.start();
    i = 1000;
    while (i < 2000) : (i += 1) {
        const key = try std.fmt.bufPrint(&key_buf, "k_{}", .{i});
        const start = timer.read();
        try engine.set(key, &val_buf);
        const end = timer.read();
        try latencies.append(allocator, @intCast(end - start));
    }

    std.mem.sort(u64, latencies.items, {}, std.sort.asc(u64));
    const p99 = latencies.items[latencies.items.len * 99 / 100];
    try std.testing.expect(p99 < 500_000);
}

test "zero post-initialization allocations" {
    const base_allocator = std.testing.allocator;
    var tracker = TrackingAllocator{ .backing = base_allocator };
    const tracking_alloc = tracker.allocator();

    var engine = try Engine.init(tracking_alloc, 4, 128, 32 * 1024);
    defer engine.deinit();
    tracker.alloc_count = 0;
    tracker.resize_count = 0;
    tracker.free_count = 0;

    var key_buf: [32]u8 = undefined;
    var val_buf: [64]u8 = undefined;
    var read_buf: [64]u8 = undefined;
    @memset(&val_buf, 'A');

    var i: usize = 0;
    while (i < 1000) : (i += 1) {
        const key = try std.fmt.bufPrint(&key_buf, "key_{}", .{i});
        try engine.set(key, &val_buf);
        _ = try engine.get(key, &read_buf, null);
    }

    try std.testing.expectEqual(@as(usize, 0), tracker.alloc_count);
    try std.testing.expectEqual(@as(usize, 0), tracker.resize_count);
    try std.testing.expectEqual(@as(usize, 0), tracker.free_count);
}

test "invalid parameters validation" {
    const allocator = std.testing.allocator;

    // Slots too small
    const err_slots = Engine.init(allocator, 4, 32, 32 * 1024);
    try std.testing.expectError(error.SlotsPerShardTooSmall, err_slots);

    // Slab too small
    const err_slab = Engine.init(allocator, 4, 128, 1024);
    try std.testing.expectError(error.SlabSizeTooSmall, err_slab);
}
