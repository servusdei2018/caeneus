import { Buffer } from "node:buffer";
import { existsSync } from "node:fs";
import * as path from "node:path";

/** Key type accepted by Cache methods. */
export type CacheKey = string | Buffer;

interface BunNativeBinding {
  createCache(
    numShards?: number,
    slotsPerShard?: number,
    slabSizePerShard?: number,
  ): unknown;
  closeCache(cache: unknown): void;
  set(cache: unknown, key: CacheKey, value: string | Buffer): void;
  get(cache: unknown, key: CacheKey): Buffer | null;
  getInto(cache: unknown, key: CacheKey, output: Buffer): number | null;
  scratch(cache: unknown): Buffer;
}

interface FastCacheBinding {
  set(key: CacheKey, value: string | Buffer): number | undefined;
  setSlow(key: CacheKey, value: string | Buffer): void;
  get(key: CacheKey): number;
  getSlow(key: CacheKey): number;
  getInto(key: CacheKey, output: Buffer): number | null;
  getIntoSlow(key: CacheKey, output: Buffer): number | null;
  scratch(): Buffer;
  close(): void;
}

interface FastNativeBinding {
  createCache(
    numShards?: number,
    slotsPerShard?: number,
    slabSizePerShard?: number,
  ): FastCacheBinding;
}

const runningOnBun = typeof process.versions.bun === "string";

function nativeBindingPath(): string {
  // Prefer the locally-compiled addon so that a source checkout always runs
  // the freshest build.  In a published npm install the local build does not
  // exist, so we fall through to the prebuilt.
  const local = path.join(
    __dirname,
    "..",
    "build",
    "Release",
    runningOnBun ? "caeneus_bun.node" : "caeneus.node",
  );
  if (existsSync(local)) {
    return local;
  }

  const prebuilt = path.join(
    __dirname,
    "..",
    "prebuilds",
    runningOnBun
      ? `bun-${process.platform}-${process.arch}`
      : `node-${process.versions.modules}-${process.platform}-${process.arch}`,
    "caeneus.node",
  );
  if (existsSync(prebuilt)) {
    return prebuilt;
  }

  throw new Error(
    `No Caeneus native addon for ${process.platform}-${process.arch}. ` +
      "Install a supported package target or build from source with " +
      "`CAENEUS_BUILD_FROM_SOURCE=1 npm install`.",
  );
}

// The compiled JavaScript lives in dist/, so this path resolves to the
// runtime-specific prebuild or the local node-gyp output.
// eslint-disable-next-line @typescript-eslint/no-var-requires
const native = require(nativeBindingPath()) as
  | BunNativeBinding
  | FastNativeBinding;
const bunNative = native as BunNativeBinding;
const fastNative = native as FastNativeBinding;

/**
 * CacheOptions configures a new Cache.
 *
 * `numShards` must be a power of two.
 * `slotsPerShard` must be at least 64.
 * `slabSizePerShard` must be at least the system page size.
 */
export interface CacheOptions {
  /** Number of independent shards used by the cache. */
  numShards?: number;

  /** Number of hash slots allocated in each shard. */
  slotsPerShard?: number;

  /** Number of bytes reserved for values in each shard. */
  slabSizePerShard?: number;
}

/**
 * Cache is a native Caeneus in-memory cache client.
 */
export class Cache {
  private readonly handle: unknown;
  private readonly scratchBuffer: Buffer | undefined;
  private scratchView: Buffer | undefined;
  private scratchLength = -1;
  private closed = false;

  /**
   * Creates and initializes a new Cache.
   *
   * Omitted options use 64 shards, 1024 slots per shard, and a 1 MiB slab per
   * shard.
   */
  public constructor(options: CacheOptions = {}) {
    const numShards = options.numShards ?? 64;
    const slotsPerShard = options.slotsPerShard ?? 1024;
    const slabSizePerShard = options.slabSizePerShard ?? 1024 * 1024;
    if (runningOnBun) {
      this.handle = bunNative.createCache(
        numShards,
        slotsPerShard,
        slabSizePerShard,
      );
    } else {
      const fastHandle = fastNative.createCache(
        numShards,
        slotsPerShard,
        slabSizePerShard,
      );
      this.handle = fastHandle;
      this.scratchBuffer = fastHandle.scratch();
    }
  }

  /**
   * `set` stores the value for key in the cache.
   *
   * The key and value may each be a string or a Buffer. The cache does not
   * retain the input Buffer.
   */
  public set(key: CacheKey, value: string | Buffer): void {
    this.assertOpen();
    if (runningOnBun) {
      bunNative.set(this.handle, key, value);
      return;
    }
    const fastHandle = this.handle as FastCacheBinding;
    const status = fastHandle.set(key, value);
    if (status === -2) {
      fastHandle.setSlow(key, value);
      return;
    }
    if (status !== undefined && status !== 0) {
      throw new Error("caeneus set failed");
    }
  }

  /**
   * `get` retrieves the value for key.
   *
   * It returns a Buffer view, or null if key is not in the cache. The returned
   * view uses reusable native scratch storage; consume it before the next get
   * call. Same-length hits may return the same Buffer object with overwritten
   * contents.
   *
   * The scratch buffer is capped at 4 MiB. If the stored value exceeds that
   * limit, `get` throws a `RangeError`. Use {@link getInto} with a
   * caller-managed Buffer for values larger than 4 MiB.
   */
  public get(key: CacheKey): Buffer | null {
    this.assertOpen();
    if (runningOnBun) {
      return bunNative.get(this.handle, key);
    }
    const fastHandle = this.handle as FastCacheBinding;
    let length = fastHandle.get(key);
    if (length === -2) {
      length = fastHandle.getSlow(key);
    }
    if (length < 0) {
      return null;
    }
    if (this.scratchBuffer === undefined) {
      throw new Error("caeneus scratch buffer is unavailable");
    }
    if (this.scratchView === undefined || this.scratchLength !== length) {
      this.scratchView = this.scratchBuffer.subarray(0, length);
      this.scratchLength = length;
    }
    return this.scratchView;
  }

  /**
   * `getInto` retrieves the value for key into output.
   *
   * It returns the number of bytes written, or null if key is not in the
   * cache. output must be large enough to hold the value.
   */
  public getInto(key: CacheKey, output: Buffer): number | null {
    this.assertOpen();
    if (runningOnBun) {
      return bunNative.getInto(this.handle, key, output);
    }
    const fastHandle = this.handle as FastCacheBinding;
    let length = fastHandle.getInto(key, output);
    if (length === -2) {
      length = fastHandle.getIntoSlow(key, output);
    }
    return length === -1 ? null : length;
  }

  /**
   * `close` closes the cache and releases its native resources.
   *
   * Calling `close` more than once is safe. After `close`, the Cache cannot be
   * used.
   */
  public close(): void {
    if (this.closed) {
      return;
    }
    if (runningOnBun) {
      bunNative.closeCache(this.handle);
    } else {
      (this.handle as FastCacheBinding).close();
    }
    this.closed = true;
  }

  /** usesV8FastApi reports whether this runtime uses the V8 Fast API addon. */
  public get usesV8FastApi(): boolean {
    return !runningOnBun;
  }

  private assertOpen(): void {
    if (this.closed) {
      throw new Error("caeneus cache is closed");
    }
  }
}

export default Cache;
