const std = @import("std");
const Shard = @import("shard.zig").Shard;

pub const invalid_slot_idx = std.math.maxInt(u32);
pub const invalid_offset = std.math.maxInt(u32);

pub const BlockHeader = struct {
    slot_idx: u32,
    block_len: u32,
    key_len: u32,
};

pub const SlabPool = struct {
    buffer: []align(std.heap.page_size_min) u8,
    write_ptr: u32 align(std.atomic.cache_line) = 0,
    shard: *Shard = undefined, // Set after shard initialization

    pub fn init(buffer: []align(std.heap.page_size_min) u8) SlabPool {
        var self = SlabPool{
            .buffer = buffer,
            .write_ptr = 0,
        };
        if (buffer.len >= @sizeOf(BlockHeader)) {
            self.writeHeader(0, .{
                .slot_idx = invalid_slot_idx,
                .block_len = @intCast(buffer.len),
                .key_len = 0,
            });
        }
        return self;
    }

    pub fn getBlockLen(self: SlabPool, offset: u32) u32 {
        const header = self.readHeader(offset);
        return header.block_len;
    }

    pub fn readHeader(self: SlabPool, offset: u32) BlockHeader {
        const ptr_slot = @as(*align(4) const std.atomic.Value(u32), @ptrCast(@alignCast(&self.buffer[offset])));
        const ptr_block_len = @as(*align(4) const std.atomic.Value(u32), @ptrCast(@alignCast(&self.buffer[offset + 4])));
        const ptr_key_len = @as(*align(4) const std.atomic.Value(u32), @ptrCast(@alignCast(&self.buffer[offset + 8])));

        return BlockHeader{
            .slot_idx = ptr_slot.load(.monotonic),
            .block_len = ptr_block_len.load(.monotonic),
            .key_len = ptr_key_len.load(.monotonic),
        };
    }

    pub fn writeHeader(self: *SlabPool, offset: u32, header: BlockHeader) void {
        const ptr_slot = @as(*align(4) std.atomic.Value(u32), @ptrCast(@alignCast(&self.buffer[offset])));
        const ptr_block_len = @as(*align(4) std.atomic.Value(u32), @ptrCast(@alignCast(&self.buffer[offset + 4])));
        const ptr_key_len = @as(*align(4) std.atomic.Value(u32), @ptrCast(@alignCast(&self.buffer[offset + 8])));

        ptr_slot.store(header.slot_idx, .monotonic);
        ptr_block_len.store(header.block_len, .monotonic);
        ptr_key_len.store(header.key_len, .monotonic);
    }

    pub fn allocate(self: *SlabPool, key: []const u8, value: []const u8, slot_idx: u32) !u32 {
        const header_size = @sizeOf(BlockHeader);
        const needed_len = std.mem.alignForward(u32, @as(u32, @intCast(header_size + key.len + value.len)), 4);
        const S = @as(u32, @intCast(self.buffer.len));

        if (needed_len > S - header_size) {
            return error.ValueTooLarge;
        }

        // Check if we need to wrap around to offset 0
        if (self.write_ptr + needed_len > S) {
            var scan_offset = self.write_ptr;
            while (scan_offset < S) {
                const header = self.readHeader(scan_offset);
                if (header.slot_idx != invalid_slot_idx) {
                    if (self.shard.isSlotActive(header.slot_idx, scan_offset)) {
                        self.shard.evictSlot(header.slot_idx);
                    }
                }
                scan_offset += header.block_len;
            }
            const wrap_len = S - self.write_ptr;
            self.writeHeader(self.write_ptr, .{
                .slot_idx = invalid_slot_idx,
                .block_len = wrap_len,
                .key_len = 0,
            });
            self.write_ptr = 0;
        }

        const init_header = self.readHeader(self.write_ptr);
        if (init_header.slot_idx != invalid_slot_idx) {
            if (self.shard.isSlotActive(init_header.slot_idx, self.write_ptr)) {
                self.shard.evictSlot(init_header.slot_idx);
            }
        }

        var total_len = init_header.block_len;
        var scan_offset = self.write_ptr + total_len;
        while (total_len < needed_len) {
            const header = self.readHeader(scan_offset);
            if (header.slot_idx != invalid_slot_idx) {
                if (self.shard.isSlotActive(header.slot_idx, scan_offset)) {
                    self.shard.evictSlot(header.slot_idx);
                }
            }
            total_len += header.block_len;
            scan_offset += header.block_len;
        }

        const allocated_offset = self.write_ptr;
        const rem_len = total_len - needed_len;

        if (rem_len >= header_size) {
            self.writeHeader(allocated_offset, .{
                .slot_idx = slot_idx,
                .block_len = needed_len,
                .key_len = @intCast(key.len),
            });
            self.writeHeader(allocated_offset + needed_len, .{
                .slot_idx = invalid_slot_idx,
                .block_len = rem_len,
                .key_len = 0,
            });
            self.write_ptr += needed_len;
        } else {
            self.writeHeader(allocated_offset, .{
                .slot_idx = slot_idx,
                .block_len = total_len,
                .key_len = @intCast(key.len),
            });
            self.write_ptr += total_len;
        }

        @memcpy(self.buffer[allocated_offset + header_size ..][0..key.len], key);
        @memcpy(self.buffer[allocated_offset + header_size + key.len ..][0..value.len], value);

        if (self.write_ptr == S) {
            self.write_ptr = 0;
        }

        return allocated_offset;
    }
};
