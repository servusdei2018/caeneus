const std = @import("std");
const common = @import("common.zig");
const Shard = @import("shard.zig").Shard;
pub const StatsSnapshot = @import("shard.zig").StatsSnapshot;

pub const Engine = struct {
    allocator: std.mem.Allocator,
    shards: []Shard,
    num_shards: u32,
    shard_mask: u32,

    pub fn init(
        allocator: std.mem.Allocator,
        num_shards: u32,
        slots_per_shard: u32,
        slab_size_per_shard: usize,
    ) !Engine {
        if (num_shards == 0 or !std.math.isPowerOfTwo(num_shards)) {
            return error.NumShardsMustBePowerOfTwo;
        }

        if (slots_per_shard < 64) {
            return error.SlotsPerShardTooSmall;
        }

        if (slab_size_per_shard < std.heap.page_size_min) {
            return error.SlabSizeTooSmall;
        }

        const shards = try allocator.alloc(Shard, num_shards);

        var initialized_count: u32 = 0;
        errdefer {
            var j: u32 = 0;
            while (j < initialized_count) : (j += 1) {
                shards[j].deinit(allocator);
            }
            allocator.free(shards);
        }

        while (initialized_count < num_shards) : (initialized_count += 1) {
            shards[initialized_count] = try Shard.init(allocator, slots_per_shard, slab_size_per_shard);
            shards[initialized_count].slab.shard = &shards[initialized_count];
        }

        return Engine{
            .allocator = allocator,
            .shards = shards,
            .num_shards = num_shards,
            .shard_mask = num_shards - 1,
        };
    }

    pub fn deinit(self: *Engine) void {
        for (self.shards) |*shard| {
            shard.deinit(self.allocator);
        }
        self.allocator.free(self.shards);
    }

    pub fn get(self: *Engine, key: []const u8, buf: []u8, required_len: ?*usize) !?[]const u8 {
        const hash = common.hashKey(key);
        const shard_idx = hash & self.shard_mask;
        const shard = &self.shards[shard_idx];

        // Prefetch the hash table entry in the shard before we do shard.get
        const hash_low = @as(u32, @intCast(hash & 0xFFFFFFFF));
        const table_idx = @as(usize, hash_low) & shard.index_mask;
        @prefetch(&shard.table[table_idx], .{ .rw = .read, .locality = 3, .cache = .data });

        return shard.get(key, hash, buf, required_len);
    }

    pub fn set(self: *Engine, key: []const u8, value: []const u8) !void {
        const hash = common.hashKey(key);
        const shard_idx = hash & self.shard_mask;
        const shard = &self.shards[shard_idx];

        // Prefetch the hash table entry in the shard before we do shard.set
        const hash_low = @as(u32, @intCast(hash & 0xFFFFFFFF));
        const table_idx = @as(usize, hash_low) & shard.index_mask;
        @prefetch(&shard.table[table_idx], .{ .rw = .read, .locality = 3, .cache = .data });

        try shard.set(key, value, hash);
    }

    /// Returns the exact byte length of the value stored under `key`, or null
    /// if the key is not present. Used by FFI when the required_len out-param
    /// from get() yields zero (e.g. key evicted between the seqlock validation
    /// and the error return).
    pub fn getRequiredLen(self: *Engine, key: []const u8) ?usize {
        const hash = common.hashKey(key);
        const shard_idx = hash & self.shard_mask;
        const shard = &self.shards[shard_idx];

        // A zero-length scratch buffer guarantees BufferTooSmall for any hit.
        // shard.get() will populate `required` with the exact metadata.len.
        var required: usize = 0;
        _ = shard.get(key, hash, &[_]u8{}, &required) catch |err| {
            if (err == error.BufferTooSmall) return if (required > 0) required else null;
        };
        // Cache miss.
        return null;
    }

    pub fn statsSnapshot(self: *const Engine) StatsSnapshot {
        var result = StatsSnapshot{};
        for (self.shards) |*shard| {
            const stats = shard.statsSnapshot();
            result.lookup_probes += stats.lookup_probes;
            result.index_deletes += stats.index_deletes;
            result.index_rebuilds += stats.index_rebuilds;
            result.slab_reclaim_blocks += stats.slab_reclaim_blocks;
            result.slab_wraps += stats.slab_wraps;
            result.seqlock_retries += stats.seqlock_retries;
            result.lock_fallbacks += stats.lock_fallbacks;
        }
        return result;
    }
};
