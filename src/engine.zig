const std = @import("std");
const common = @import("common.zig");
const Shard = @import("shard.zig").Shard;

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

    pub fn get(self: *Engine, key: []const u8, buf: []u8) !?[]const u8 {
        const hash = common.hashKey(key);
        const shard_idx = hash & self.shard_mask;
        const shard = &self.shards[shard_idx];

        // Prefetch the hash table entry in the shard before we do shard.get
        const hash_low = @as(u32, @intCast(hash & 0xFFFFFFFF));
        const table_idx = @as(usize, hash_low) & shard.index_mask;
        @prefetch(&shard.table[table_idx], .{ .rw = .read, .locality = 3, .cache = .data });

        return shard.get(key, hash, buf);
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
};
