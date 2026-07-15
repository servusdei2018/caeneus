const std = @import("std");

/// TicketLock represents a fair userspace ticket lock using std.atomic.Value(u32).
/// This prevents starvation by serving threads in the order they requested the lock.
/// It spins for a short period and then yields to the OS scheduler to avoid CPU burn.
pub const TicketLock = struct {
    next_ticket: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),
    now_serving: std.atomic.Value(u32) = std.atomic.Value(u32).init(0),

    pub fn lock(self: *TicketLock) void {
        const my_ticket = self.next_ticket.fetchAdd(1, .monotonic);
        var spins: usize = 0;
        while (self.now_serving.load(.acquire) != my_ticket) {
            spins += 1;
            if (spins < 1000) {
                std.atomic.spinLoopHint();
            } else {
                std.Thread.yield() catch {};
            }
        }
    }

    pub fn unlock(self: *TicketLock) void {
        _ = self.now_serving.fetchAdd(1, .release);
    }
};

/// BitField represents a tightly packed array of bits using []u64.
/// It provides both normal and thread-safe atomic access methods.
pub const BitField = struct {
    words: []u64,

    pub fn init(allocator: std.mem.Allocator, num_bits: usize) !BitField {
        const num_words = (num_bits + 63) / 64;
        const words = try allocator.alloc(u64, num_words);
        @memset(words, 0);
        return BitField{ .words = words };
    }

    pub fn testBit(self: BitField, i: usize) bool {
        const word_idx = i >> 6;
        const bit_idx = i & 63;
        const mask = @as(u64, 1) << @intCast(bit_idx);
        const word = @atomicLoad(u64, &self.words[word_idx], .monotonic);
        return (word & mask) != 0;
    }

    pub fn setAtomic(self: BitField, i: usize) void {
        const word_idx = i >> 6;
        const bit_idx = i & 63;
        const mask = @as(u64, 1) << @intCast(bit_idx);
        const word = @atomicLoad(u64, &self.words[word_idx], .monotonic);
        if ((word & mask) == 0) {
            _ = @atomicRmw(u64, &self.words[word_idx], .Or, mask, .monotonic);
        }
    }

    pub fn unsetAtomic(self: BitField, i: usize) void {
        const word_idx = i >> 6;
        const bit_idx = i & 63;
        const mask = @as(u64, 1) << @intCast(bit_idx);
        const word = @atomicLoad(u64, &self.words[word_idx], .monotonic);
        if ((word & mask) != 0) {
            _ = @atomicRmw(u64, &self.words[word_idx], .And, ~mask, .monotonic);
        }
    }

    pub fn testAndUnsetAtomic(self: BitField, i: usize) bool {
        const word_idx = i >> 6;
        const bit_idx = i & 63;
        const mask = @as(u64, 1) << @intCast(bit_idx);
        const old_word = @atomicRmw(u64, &self.words[word_idx], .And, ~mask, .monotonic);
        return (old_word & mask) != 0;
    }
};

pub fn hashKey(key: []const u8) u64 {
    return std.hash.Wyhash.hash(0, key);
}
