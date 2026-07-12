//! C-ABI bridge.
//!
//! Exposes entry points that can be called from any C-compatible host runtime
//! (C, Python/ctypes, Go/cgo, Rust/bindgen, JNI…).
//!
//! Errors are returned as explicit c_int codes:
//!
//!   0  CAENEUS_OK           — Operation succeeded.
//!  -1  CAENEUS_MISS         — Key not present in the cache.
//!  -2  CAENEUS_ERR_SMALL_BUF — Destination buffer too small; *out_val_len
//!                              contains the exact number of bytes required.
//!  -3  CAENEUS_ERR_PANIC    — Unexpected internal error or allocation failure.

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
//   - *out_val_len is set to the number of bytes written.
//   - Returns CAENEUS_OK.
//
// On a buffer-too-small hit:
//   - No value bytes are copied.
//   - *out_val_len is set to the exact number of bytes required.
//   - Returns CAENEUS_ERR_SMALL_BUF.
//   The caller should resize buf and retry.
//
//   Passing buf_len == 0 is a valid "probe-only" call: the engine will always
//   report CAENEUS_ERR_SMALL_BUF and populate *out_val_len with the required
//   size, allowing callers to allocate the right buffer before a second call.
//
// On a cache miss:
//   - *out_val_len is unmodified.
//   - Returns CAENEUS_MISS.
//
// On any unexpected error:
//   - Returns CAENEUS_ERR_PANIC.
// ---------------------------------------------------------------------------

pub export fn caeneus_get(
    handle: ?*anyopaque,
    key_ptr: [*]const u8,
    key_len: usize,
    buf_ptr: [*]u8,
    buf_len: usize,
    out_val_len: *usize,
) c_int {
    const wrapper: *EngineWrapper = @ptrCast(@alignCast(handle orelse return CAENEUS_ERR_PANIC));
    const key = key_ptr[0..key_len];
    const buf = buf_ptr[0..buf_len];

    // Pass out_val_len directly into engine.get().
    // On a successful hit the returned slice length is written below.
    // On error.BufferTooSmall, shard.get() writes metadata.len to
    // out_val_len before returning the error.
    const result = wrapper.engine.get(key, buf, out_val_len) catch |err| blk: {
        if (err == error.BufferTooSmall) {
            // out_val_len was already populated by shard.get().
            // Otherwise: if the key was evicted between the seqlock
            // validation and the error return, out_val_len may still be 0.
            // In which case, getRequiredLen() performs a fresh shard lookup.
            if (out_val_len.* == 0) {
                out_val_len.* = wrapper.engine.getRequiredLen(key) orelse 0;
            }
            return CAENEUS_ERR_SMALL_BUF;
        }
        break :blk null;
    };

    if (result) |slice| {
        out_val_len.* = slice.len;
        return CAENEUS_OK;
    }

    return CAENEUS_MISS;
}
