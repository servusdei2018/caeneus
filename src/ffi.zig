//! C-ABI bridge.
//!
//! Exposes entry points that can be called from any C-compatible host runtime
//! (C, compiled CPython/Node extensions, Go/cgo, Rust/bindgen, JNI…).
//!
//! caeneus_get returns a packed u64: the signed c_int status occupies the
//! high 32 bits and the value length occupies the low 32 bits.
//!
//!   0  CAENEUS_OK           — Operation succeeded.
//! -1  CAENEUS_MISS           — Key not present in the cache.
//! -2  CAENEUS_ERR_SMALL_BUF  — Destination buffer too small; the low 32 bits
//!                              contain the exact number of bytes required.
//! -3  CAENEUS_ERR_PANIC      — Unexpected internal error or allocation failure.

const std = @import("std");
const caeneus = @import("caeneus");
const Engine = caeneus.Engine;

// ---------------------------------------------------------------------------
// Return codes
// ---------------------------------------------------------------------------

const CAENEUS_OK: c_int = 0;
const CAENEUS_MISS: c_int = -1;
const CAENEUS_ERR_SMALL_BUF: c_int = -2;
const CAENEUS_ERR_PANIC: c_int = -3;

fn packResult(code: c_int, length: usize) u64 {
    const code_bits: u32 = @bitCast(code);
    const length_bits: u32 = @intCast(length);
    return (@as(u64, code_bits) << 32) | @as(u64, length_bits);
}

// ---------------------------------------------------------------------------
// Internal wrapper
//
// Engine contains Zig fat-pointer slices and therefore cannot be placed in an
// extern struct or returned by value across the C boundary.  We heap-allocate
// a wrapper and hand the host an opaque pointer instead.
// ---------------------------------------------------------------------------

const EngineWrapper = struct {
    engine: Engine,
};

// ---------------------------------------------------------------------------
// caeneus_init
//
// Allocates and initialises the engine.  Returns an opaque handle on success,
// or NULL on any allocation / validation failure.
// ---------------------------------------------------------------------------

pub export fn caeneus_init(
    num_shards: u32,
    slots_per_shard: u32,
    slab_size_per_shard: usize,
) ?*anyopaque {
    const wrapper = std.heap.c_allocator.create(EngineWrapper) catch return null;
    wrapper.engine = Engine.init(
        std.heap.c_allocator,
        num_shards,
        slots_per_shard,
        slab_size_per_shard,
    ) catch {
        std.heap.c_allocator.destroy(wrapper);
        return null;
    };
    return @ptrCast(wrapper);
}

// ---------------------------------------------------------------------------
// caeneus_deinit
//
// Shuts down the engine and frees the wrapper memory.  Safe to call with a
// NULL handle (no-op).
// ---------------------------------------------------------------------------

pub export fn caeneus_deinit(handle: ?*anyopaque) void {
    const wrapper: *EngineWrapper = @ptrCast(@alignCast(handle orelse return));
    wrapper.engine.deinit();
    std.heap.c_allocator.destroy(wrapper);
}

// ---------------------------------------------------------------------------
// caeneus_set
//
// Inserts or updates the value for the given key.  Both key and value are
// described by a (pointer, length) pair; no null-terminator is required.
//
// Returns:
//   CAENEUS_OK       on success.
//   CAENEUS_ERR_PANIC on any internal failure (OOM, slab full, etc.).
// ---------------------------------------------------------------------------

pub export fn caeneus_set(
    handle: ?*anyopaque,
    key_ptr: [*]const u8,
    key_len: usize,
    val_ptr: [*]const u8,
    val_len: usize,
) c_int {
    const wrapper: *EngineWrapper = @ptrCast(@alignCast(handle orelse return CAENEUS_ERR_PANIC));
    wrapper.engine.set(key_ptr[0..key_len], val_ptr[0..val_len]) catch return CAENEUS_ERR_PANIC;
    return CAENEUS_OK;
}

// ---------------------------------------------------------------------------
// caeneus_get
//
// Looks up `key` and copies the stored value into `buf_ptr[0..buf_len]`.
//
// On a successful hit:
//   - The value bytes are written to buf_ptr.
//   - The low 32 bits contain the number of bytes written.
//   - Returns a packed CAENEUS_OK result.
//
// On a buffer-too-small hit:
//   - No value bytes are copied.
//   - The low 32 bits contain the exact number of bytes required.
//   - Returns a packed CAENEUS_ERR_SMALL_BUF result.
//   The caller should resize buf and retry.
//
//   Passing buf_len == 0 is a valid "probe-only" call: the engine will always
//   report CAENEUS_ERR_SMALL_BUF and pack the required size in the result,
//   allowing callers to allocate the right buffer before a second call.
//
// On a cache miss:
//   - The low 32 bits are zero.
//   - Returns a packed CAENEUS_MISS result.
//
// On any unexpected error:
//   - Returns a packed CAENEUS_ERR_PANIC result.
// ---------------------------------------------------------------------------

pub export fn caeneus_get(
    handle: ?*anyopaque,
    key_ptr: [*]const u8,
    key_len: usize,
    buf_ptr: [*]u8,
    buf_len: usize,
) u64 {
    const wrapper: *EngineWrapper = @ptrCast(@alignCast(handle orelse return packResult(CAENEUS_ERR_PANIC, 0)));
    const key = key_ptr[0..key_len];
    const buf = buf_ptr[0..buf_len];

    var required_len: usize = 0;
    const result = wrapper.engine.get(key, buf, &required_len) catch |err| blk: {
        if (err == error.BufferTooSmall) {
            // Otherwise: if the key was evicted between the seqlock
            // validation and the error return, required_len may still be 0.
            // In which case, getRequiredLen() performs a fresh shard lookup.
            if (required_len == 0) {
                required_len = wrapper.engine.getRequiredLen(key) orelse 0;
            }
            return packResult(CAENEUS_ERR_SMALL_BUF, required_len);
        }
        break :blk null;
    };

    if (result) |slice| {
        return packResult(CAENEUS_OK, slice.len);
    }

    return packResult(CAENEUS_MISS, 0);
}
