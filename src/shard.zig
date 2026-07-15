const std = @import("std");
const builtin = @import("builtin");
const common = @import("common.zig");
const TicketLock = common.TicketLock;
const BitField = common.BitField;
const SlabPool = @import("slab.zig").SlabPool;
const BlockHeader = @import("slab.zig").BlockHeader;
const invalid_slot_idx = @import("slab.zig").invalid_slot_idx;
const invalid_offset = @import("slab.zig").invalid_offset;
const tombstone_slot_idx = invalid_slot_idx - 1;

pub const StatsSnapshot = struct {
    lookup_probes: u64 = 0,
    index_deletes: u64 = 0,
    index_rebuilds: u64 = 0,
    slab_reclaim_blocks: u64 = 0,
    slab_wraps: u64 = 0,
    seqlock_retries: u64 = 0,
    lock_fallbacks: u64 = 0,
};

const DebugStats = struct {
    lookup_probes: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
    index_deletes: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
    index_rebuilds: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
    slab_reclaim_blocks: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
    slab_wraps: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
    seqlock_retries: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
    lock_fallbacks: std.atomic.Value(u64) = std.atomic.Value(u64).init(0),
};

pub const IndexEntry = extern struct {
    hash_low: u32 align(8),
    slot_idx: u32,
};

pub const SlotMetadata = extern struct {
    offset: u32 align(8),
    len: u32,
};

pub const Shard = struct {
    // --- Cache Line 1 ---
    lock: TicketLock align(std.atomic.cache_line) = .{},

    // --- Cache Line 2 ---
    seq: std.atomic.Value(u32) align(std.atomic.cache_line) = std.atomic.Value(u32).init(0),

    // --- Cache Line 3 ---
    probationary_head: u32 align(std.atomic.cache_line) = 0,
    hand: u32 = 0,

    // --- Cache Line 4 ---
    num_probationary: u32 align(std.atomic.cache_line),
    num_main: u32,
    total_slots: u32,

    index_mask: usize,
    table: []IndexEntry,
    index_positions: []u32,
    tombstones: u32,
    stats: if (builtin.mode == .Debug) DebugStats else void,

    visited: BitField,
    reclaim_offsets: []u32,
    hashes: []u64,
    metadata: []SlotMetadata,

    slab: SlabPool,

    pub fn init(
        allocator: std.mem.Allocator,
        total_slots: u32,
        slab_size: usize,
    ) !Shard {
        const total_slots_aligned = std.mem.alignForward(u32, total_slots, 64);
        const num_probationary = total_slots_aligned / 10;
        const num_main = total_slots_aligned - num_probationary;

        const min_table_size = @as(usize, @intCast(total_slots_aligned)) * 10 / 6;
        const table_size = try std.math.ceilPowerOfTwo(usize, min_table_size);
        const index_mask = table_size - 1;

        const table = try allocator.alloc(IndexEntry, table_size);
        @memset(table, .{ .hash_low = 0, .slot_idx = invalid_slot_idx });

        const index_positions = try allocator.alloc(u32, total_slots_aligned);
        @memset(index_positions, invalid_slot_idx);

        const visited = try BitField.init(allocator, total_slots_aligned);

        const reclaim_offsets = try allocator.alloc(u32, total_slots_aligned);
        @memset(reclaim_offsets, invalid_offset);

        const hashes = try allocator.alloc(u64, total_slots_aligned);
        @memset(hashes, 0);

        const metadata = try allocator.alloc(SlotMetadata, total_slots_aligned);
        @memset(metadata, .{ .offset = invalid_offset, .len = 0 });

        const slab_buf = if (slab_size >= 2 * 1024 * 1024)
            try allocator.allocWithOptions(u8, slab_size, std.mem.Alignment.fromByteUnits(2 * 1024 * 1024), null)
        else
            try allocator.allocWithOptions(u8, slab_size, std.mem.Alignment.fromByteUnits(std.heap.page_size_min), null);
        @memset(slab_buf, 0);

        const builtin_info = @import("builtin");
        if (builtin_info.os.tag == .linux) {
            // Only request THP for slabs that are already hugepage-sized
            // (>= 2 MiB) and below a per-shard ceiling (256 MiB).  Asking
            // for hugepages on dozens of large arenas simultaneously drives
            // khugepaged into expensive compact-zone walks under memory
            // pressure, which can stall or OOM adjacent processes.
            const thp_min = 2 * 1024 * 1024;
            const thp_max = 256 * 1024 * 1024;
            if (slab_buf.len >= thp_min and slab_buf.len <= thp_max) {
                std.posix.madvise(@alignCast(slab_buf.ptr), slab_buf.len, std.posix.MADV.HUGEPAGE) catch {};
            }
        }

        const shard = Shard{
            .lock = .{},
            .seq = std.atomic.Value(u32).init(0),
            .probationary_head = 0,
            .hand = num_probationary,
            .num_probationary = num_probationary,
            .num_main = num_main,
            .total_slots = total_slots_aligned,
            .index_mask = index_mask,
            .table = table,
            .index_positions = index_positions,
            .tombstones = 0,
            .stats = if (builtin.mode == .Debug) .{} else {},
            .visited = visited,
            .reclaim_offsets = reclaim_offsets,
            .hashes = hashes,
            .metadata = metadata,
            .slab = SlabPool.init(slab_buf),
        };

        return shard;
    }

    pub fn deinit(self: *Shard, allocator: std.mem.Allocator) void {
        allocator.free(self.table);
        allocator.free(self.index_positions);
        allocator.free(self.visited.words);
        allocator.free(self.reclaim_offsets);
        allocator.free(self.hashes);
        allocator.free(self.metadata);
        allocator.free(self.slab.buffer);
    }

    pub fn isSlotActive(self: *Shard, slot_idx: u32, offset: u32) bool {
        if (slot_idx >= self.total_slots) return false;
        const meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[slot_idx])), .monotonic);
        const metadata = @as(SlotMetadata, @bitCast(meta_u64));
        return metadata.offset == offset;
    }

    pub fn statsSnapshot(self: *const Shard) StatsSnapshot {
        if (comptime builtin.mode == .Debug) {
            return .{
                .lookup_probes = self.stats.lookup_probes.load(.monotonic),
                .index_deletes = self.stats.index_deletes.load(.monotonic),
                .index_rebuilds = self.stats.index_rebuilds.load(.monotonic),
                .slab_reclaim_blocks = self.stats.slab_reclaim_blocks.load(.monotonic),
                .slab_wraps = self.stats.slab_wraps.load(.monotonic),
                .seqlock_retries = self.stats.seqlock_retries.load(.monotonic),
                .lock_fallbacks = self.stats.lock_fallbacks.load(.monotonic),
            };
        }
        return .{};
    }

    pub inline fn noteSlabReclaimBlock(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.slab_reclaim_blocks.fetchAdd(1, .monotonic);
        }
    }

    pub inline fn noteSlabWrap(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.slab_wraps.fetchAdd(1, .monotonic);
        }
    }

    pub fn markSlabBlockPending(self: *Shard, slot_idx: u32, offset: u32) void {
        if (slot_idx >= self.total_slots) return;
        if (self.isSlotActive(slot_idx, offset)) {
            self.reclaim_offsets[slot_idx] = offset;
            self.noteSlabReclaimBlock();
        }
    }

    pub fn evictPendingSlabBlocks(self: *Shard) void {
        var slot_idx: u32 = 0;
        while (slot_idx < self.total_slots) : (slot_idx += 1) {
            const offset = self.reclaim_offsets[slot_idx];
            if (offset == invalid_offset) continue;
            self.reclaim_offsets[slot_idx] = invalid_offset;
            if (self.isSlotActive(slot_idx, offset)) {
                self.evictSlot(slot_idx);
            }
        }
    }

    pub fn evictSlot(self: *Shard, slot_idx: u32) void {
        std.debug.assert(slot_idx < self.total_slots);
        const meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[slot_idx])), .monotonic);
        const metadata = @as(SlotMetadata, @bitCast(meta_u64));
        if (metadata.offset == invalid_offset) return;

        self.deleteIndexMap(slot_idx);

        if (self.reclaim_offsets[slot_idx] == metadata.offset) {
            self.reclaim_offsets[slot_idx] = invalid_offset;
        }
        self.visited.unsetAtomic(slot_idx);
        const new_meta_u64 = @as(u64, @bitCast(SlotMetadata{ .offset = invalid_offset, .len = 0 }));
        @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.metadata[slot_idx])), new_meta_u64, .release);
        self.hashes[slot_idx] = 0;
        self.maybeRebuildIndexMap();
    }

    pub fn get(self: *Shard, key: []const u8, hash: u64, buf: []u8, required_len: ?*usize) !?[]const u8 {
        // --- Optimistic lock-free
        var attempts: u32 = 0;
        while (attempts < 10) : (attempts += 1) {
            var seq1 = self.seq.load(.acquire);
            while (seq1 & 1 != 0) {
                std.atomic.spinLoopHint();
                seq1 = self.seq.load(.acquire);
            }

            const hit = self.lookupHit(key, hash);
            if (hit == null) {
                const seq2 = self.seq.load(.acquire);
                if (seq1 == seq2) return null;
                self.noteSeqlockRetry();
                continue;
            }
            const slot_idx = hit.?.slot_idx;
            const hit_offset = hit.?.offset;

            const meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[slot_idx])), .acquire);
            const metadata = @as(SlotMetadata, @bitCast(meta_u64));
            if (metadata.len == 0 or metadata.offset >= self.slab.buffer.len) {
                const seq2 = self.seq.load(.acquire);
                if (seq1 == seq2) return null;
                self.noteSeqlockRetry();
                continue;
            }

            if (buf.len < metadata.len) {
                const seq2 = self.seq.load(.acquire);
                if (seq1 == seq2) {
                    if (required_len) |rl| rl.* = metadata.len;
                    return error.BufferTooSmall;
                }
                self.noteSeqlockRetry();
                continue;
            }

            // lookupHit already validated the key at hit_offset. Skip a second
            // slab key compare when the slot still points at that offset.
            if (metadata.offset != hit_offset) {
                self.noteSeqlockRetry();
                continue;
            }
            const offset = hit_offset;
            const needed_len = @as(usize, @sizeOf(BlockHeader)) + key.len + metadata.len;
            if (offset >= self.slab.buffer.len or self.slab.buffer.len - offset < needed_len) {
                self.noteSeqlockRetry();
                continue;
            }

            const val_offset = offset + @sizeOf(BlockHeader) + key.len;
            const stored_val = self.slab.buffer[val_offset..][0..metadata.len];
            @memcpy(buf[0..metadata.len], stored_val);

            asm volatile ("" ::: .{ .memory = true });
            const seq2 = self.seq.load(.acquire);
            if (seq1 == seq2) {
                self.visited.setAtomic(slot_idx);
                return buf[0..metadata.len];
            }
            self.noteSeqlockRetry();
        }

        // --- Locking
        self.noteLockFallback();
        self.lock.lock();
        defer self.lock.unlock();

        const slot_idx = self.lookup(key, hash);
        if (slot_idx == invalid_slot_idx) return null;

        const meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[slot_idx])), .acquire);
        const metadata = @as(SlotMetadata, @bitCast(meta_u64));
        @prefetch(&self.slab.buffer[metadata.offset], .{ .rw = .read, .locality = 3, .cache = .data });
        self.visited.setAtomic(slot_idx);

        const header = self.slab.readHeader(metadata.offset);
        const val_len = metadata.len;
        if (buf.len < val_len) {
            if (required_len) |rl| rl.* = val_len;
            return error.BufferTooSmall;
        }

        const val_offset = metadata.offset + @sizeOf(BlockHeader) + header.key_len;
        @memcpy(buf[0..val_len], self.slab.buffer[val_offset..][0..val_len]);

        return buf[0..val_len];
    }

    pub fn set(self: *Shard, key: []const u8, value: []const u8, hash: u64) !void {
        self.lock.lock();
        defer self.lock.unlock();

        // Increment sequence to odd (write starting)
        const seq1 = self.seq.load(.monotonic);
        self.seq.store(seq1 +% 1, .seq_cst);
        defer self.seq.store(seq1 +% 2, .release);

        const existing_slot = self.lookup(key, hash);
        if (existing_slot != invalid_slot_idx) {
            const new_offset = try self.slab.allocate(key, value, existing_slot);

            const meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[existing_slot])), .acquire);
            const metadata = @as(SlotMetadata, @bitCast(meta_u64));
            const was_evicted = (metadata.offset == invalid_offset);

            const new_meta_u64 = @as(u64, @bitCast(SlotMetadata{ .offset = new_offset, .len = @intCast(value.len) }));
            @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.metadata[existing_slot])), new_meta_u64, .release);
            self.visited.setAtomic(existing_slot);

            if (was_evicted) {
                self.hashes[existing_slot] = hash;
                self.insertIndexMap(hash, existing_slot);
            }
            return;
        }

        const target_slot = self.probationary_head;
        const new_offset = try self.slab.allocate(key, value, target_slot);

        const target_meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[target_slot])), .acquire);
        const target_metadata = @as(SlotMetadata, @bitCast(target_meta_u64));
        if (target_metadata.offset != invalid_offset) {
            @branchHint(.cold);
            if (self.visited.testBit(target_slot)) {
                const main_slot = self.sieveEvict();
                const main_meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[main_slot])), .acquire);
                const main_metadata = @as(SlotMetadata, @bitCast(main_meta_u64));
                if (main_metadata.offset != invalid_offset) {
                    self.evictSlot(main_slot);
                }

                @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.metadata[main_slot])), target_meta_u64, .release);
                self.hashes[main_slot] = self.hashes[target_slot];
                self.visited.unsetAtomic(main_slot);

                var header = self.slab.readHeader(target_metadata.offset);
                header.slot_idx = main_slot;
                self.slab.writeHeader(target_metadata.offset, header);

                self.updateSlotInIndexMap(self.hashes[main_slot], target_slot, main_slot);
            } else {
                self.evictSlot(target_slot);
            }
        }

        const new_meta_u64 = @as(u64, @bitCast(SlotMetadata{ .offset = new_offset, .len = @intCast(value.len) }));
        @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.metadata[target_slot])), new_meta_u64, .release);
        self.hashes[target_slot] = hash;
        self.visited.unsetAtomic(target_slot);

        self.insertIndexMap(hash, target_slot);
        self.probationary_head = (self.probationary_head + 1) % self.num_probationary;
    }

    const LookupHit = struct {
        slot_idx: u32,
        offset: u32,
    };

    fn lookup(self: *Shard, key: []const u8, hash: u64) u32 {
        const hit = self.lookupHit(key, hash) orelse return invalid_slot_idx;
        return hit.slot_idx;
    }

    fn lookupHit(self: *Shard, key: []const u8, hash: u64) ?LookupHit {
        const mask = self.index_mask;
        const hash_low = @as(u32, @intCast(hash & 0xFFFFFFFF));
        var idx = @as(usize, hash_low) & mask;
        while (true) {
            self.noteLookupProbe();
            const entry_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.table[idx])), .monotonic);
            const entry = @as(IndexEntry, @bitCast(entry_u64));
            if (entry.slot_idx == invalid_slot_idx) {
                return null;
            }
            if (entry.slot_idx == tombstone_slot_idx) {
                idx = (idx + 1) & mask;
                continue;
            }
            if (entry.hash_low == hash_low) {
                if (self.keyEqualsOffset(entry.slot_idx, key)) |offset| {
                    return .{ .slot_idx = entry.slot_idx, .offset = offset };
                }
            }
            idx = (idx + 1) & mask;
        }
    }

    inline fn keyEqualsOffset(self: *Shard, slot_idx: u32, key: []const u8) ?u32 {
        if (slot_idx >= self.total_slots) return null;
        const meta_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.metadata[slot_idx])), .acquire);
        const metadata = @as(SlotMetadata, @bitCast(meta_u64));
        const offset = metadata.offset;
        if (offset == invalid_offset) return null;

        if (offset >= self.slab.buffer.len or self.slab.buffer.len - offset < @sizeOf(BlockHeader)) return null;
        const header = self.slab.readHeader(offset);
        if (header.key_len != key.len) return null;

        const needed_len = @as(usize, @sizeOf(BlockHeader)) + header.key_len;
        if (self.slab.buffer.len - offset < needed_len) return null;

        const stored_key = self.slab.buffer[offset + @sizeOf(BlockHeader) ..][0..header.key_len];
        if (!std.mem.eql(u8, stored_key, key)) return null;
        return offset;
    }

    fn insertIndexMap(self: *Shard, hash: u64, slot_idx: u32) void {
        const mask = self.index_mask;
        const hash_low = @as(u32, @intCast(hash & 0xFFFFFFFF));
        var idx = @as(usize, hash_low) & mask;
        var first_tombstone: ?usize = null;
        while (true) {
            const entry_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.table[idx])), .monotonic);
            const entry = @as(IndexEntry, @bitCast(entry_u64));
            if (entry.slot_idx == invalid_slot_idx) {
                const insert_idx = first_tombstone orelse idx;
                const new_entry_u64 = @as(u64, @bitCast(IndexEntry{ .hash_low = hash_low, .slot_idx = slot_idx }));
                @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.table[insert_idx])), new_entry_u64, .monotonic);
                self.index_positions[slot_idx] = @intCast(insert_idx);
                if (first_tombstone != null) {
                    self.tombstones -= 1;
                }
                return;
            }
            if (entry.slot_idx == tombstone_slot_idx and first_tombstone == null) {
                first_tombstone = idx;
            }
            idx = (idx + 1) & mask;
        }
    }

    fn updateSlotInIndexMap(self: *Shard, hash: u64, old_slot_idx: u32, new_slot_idx: u32) void {
        const hash_low = @as(u32, @intCast(hash & 0xFFFFFFFF));
        const idx = self.index_positions[old_slot_idx];
        if (idx == invalid_slot_idx) {
            std.debug.panic("slot index not found in index map", .{});
        }
        const entry_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.table[idx])), .monotonic);
        const entry = @as(IndexEntry, @bitCast(entry_u64));
        if (entry.slot_idx != old_slot_idx) {
            std.debug.panic("slot index position is stale", .{});
        }
        const new_entry_u64 = @as(u64, @bitCast(IndexEntry{ .hash_low = hash_low, .slot_idx = new_slot_idx }));
        @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.table[idx])), new_entry_u64, .monotonic);
        self.index_positions[old_slot_idx] = invalid_slot_idx;
        self.index_positions[new_slot_idx] = idx;
    }

    fn deleteIndexMap(self: *Shard, slot_idx: u32) void {
        const idx = self.index_positions[slot_idx];
        if (idx == invalid_slot_idx) {
            return;
        }
        const entry_u64 = @atomicLoad(u64, @as(*align(8) const u64, @ptrCast(&self.table[idx])), .monotonic);
        const entry = @as(IndexEntry, @bitCast(entry_u64));
        if (entry.slot_idx != slot_idx) {
            std.debug.panic("slot index position is stale", .{});
        }
        const tombstone_u64 = @as(u64, @bitCast(IndexEntry{ .hash_low = 0, .slot_idx = tombstone_slot_idx }));
        @atomicStore(u64, @as(*align(8) u64, @ptrCast(&self.table[idx])), tombstone_u64, .monotonic);
        self.index_positions[slot_idx] = invalid_slot_idx;
        self.tombstones += 1;
        self.noteIndexDelete();
    }

    fn maybeRebuildIndexMap(self: *Shard) void {
        // Rebuild when tombstones reach 25% of the table. The prior 12.5%
        // threshold rebuilt too often under eviction storms (Profile B p99).
        const threshold: u32 = @intCast(@max(@as(usize, 1), self.table.len / 4));
        if (self.tombstones < threshold) return;

        const empty_u64 = @as(u64, @bitCast(IndexEntry{ .hash_low = 0, .slot_idx = invalid_slot_idx }));
        for (self.table) |*entry| {
            @atomicStore(u64, @as(*align(8) u64, @ptrCast(entry)), empty_u64, .monotonic);
        }
        @memset(self.index_positions, invalid_slot_idx);
        self.tombstones = 0;
        self.noteIndexRebuild();

        var slot_idx: u32 = 0;
        while (slot_idx < self.total_slots) : (slot_idx += 1) {
            const meta_u64 = @atomicLoad(
                u64,
                @as(*align(8) const u64, @ptrCast(&self.metadata[slot_idx])),
                .monotonic,
            );
            const metadata = @as(SlotMetadata, @bitCast(meta_u64));
            if (metadata.offset != invalid_offset) {
                self.insertIndexMap(self.hashes[slot_idx], slot_idx);
            }
        }
    }

    fn sieveEvict(self: *Shard) u32 {
        const P = self.num_probationary;
        const M = self.num_main;
        const total = P + M;

        // Snapshot the starting position so we can detect a full lap.
        // If every slot in the main pool is visited (e.g. all-hot read
        // workload), the old unbounded loop spun forever.  After one full
        // sweep we've cleared all visited bits along the way; force-evict
        // the current hand position rather than spinning again.
        const start = self.hand;

        while (true) {
            const slot = self.hand;
            self.hand += 1;
            if (self.hand >= total) {
                self.hand = P;
            }

            if (!self.visited.testAndUnsetAtomic(slot)) {
                return slot;
            }

            // Full lap: all slots were visited.  We've already cleared every
            // bit as we went, so just evict wherever the hand sits now.
            if (self.hand == start) {
                const force = self.hand;
                self.hand += 1;
                if (self.hand >= total) self.hand = P;
                return force;
            }
        }
    }

    inline fn noteLookupProbe(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.lookup_probes.fetchAdd(1, .monotonic);
        }
    }

    inline fn noteIndexDelete(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.index_deletes.fetchAdd(1, .monotonic);
        }
    }

    inline fn noteIndexRebuild(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.index_rebuilds.fetchAdd(1, .monotonic);
        }
    }

    inline fn noteSeqlockRetry(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.seqlock_retries.fetchAdd(1, .monotonic);
        }
    }

    inline fn noteLockFallback(self: *Shard) void {
        if (comptime builtin.mode == .Debug) {
            _ = self.stats.lock_fallbacks.fetchAdd(1, .monotonic);
        }
    }
};
