import { Buffer } from "node:buffer";
import { existsSync } from "node:fs";
import * as path from "node:path";

interface NativeBinding {
  createCache(
    numShards?: number,
    slotsPerShard?: number,
    slabSizePerShard?: number,
  ): unknown;
  closeCache(cache: unknown): void;
  set(cache: unknown, key: string, value: string | Buffer): void;
  get(cache: unknown, key: string): number | null;
  getInto(cache: unknown, key: string, output: Buffer): number | null;
  scratch(cache: unknown): Buffer;
  fastApiAvailable(): boolean;
}

function nativeBindingPath(): string {
  const prebuilt = path.join(
    __dirname,
    "..",
    "prebuilds",
    `${process.platform}-${process.arch}`,
    "caeneus.node",
  );
  if (existsSync(prebuilt)) {
    return prebuilt;
  }

  const local = path.join(__dirname, "..", "build", "Release", "caeneus.node");
  if (existsSync(local)) {
    return local;
  }

  throw new Error(
    `No Caeneus native addon for ${process.platform}-${process.arch}. ` +
      "Install a supported package target or build from source with " +
      "`CAENEUS_BUILD_FROM_SOURCE=1 npm install`.",
  );
}

// The compiled JavaScript lives in dist/, so this path resolves to either the
// packaged N-API prebuild or the local node-gyp output.
// eslint-disable-next-line @typescript-eslint/no-var-requires
const native = require(nativeBindingPath()) as NativeBinding;

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
  private scratchBuffer: Buffer;
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
    this.handle = native.createCache(
      numShards,
      slotsPerShard,
      slabSizePerShard,
    );
    this.scratchBuffer = native.scratch(this.handle);
  }

  /**
   * `set` stores the value for key in the cache.
   *
   * The key must be a string. The value may be a string or a Buffer. The cache
   * does not retain the input Buffer.
   */
  public set(key: string, value: string | Buffer): void {
    this.assertOpen();
    native.set(this.handle, key, value);
  }

  /**
   * `get` retrieves the value for key.
   *
   * It returns a Buffer view, or null if key is not in the cache. The returned
   * view uses reusable native-owned storage; consume it before the next get
   * call, because a subsequent read may overwrite its contents.
   */
  public get(key: string): Buffer | null {
    this.assertOpen();
    const outputLength = native.get(this.handle, key);
    if (outputLength === null) {
      return null;
    }

    // The native scratch allocation only grows. Avoid a Node-API call on the
    // stable hot path; refresh the pinned Buffer only after a resize.
    if (outputLength > this.scratchBuffer.length) {
      this.scratchBuffer = native.scratch(this.handle);
    }
    return Buffer.from(this.scratchBuffer.buffer, 0, outputLength);
  }

  /**
   * `getInto` retrieves the value for key into output.
   *
   * It returns the number of bytes written, or null if key is not in the
   * cache. output must be large enough to hold the value.
   */
  public getInto(key: string, output: Buffer): number | null {
    this.assertOpen();
    return native.getInto(this.handle, key, output);
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
    native.closeCache(this.handle);
    this.closed = true;
  }

  /** usesV8FastApi reports whether the experimental V8 Fast API is available. */
  public get usesV8FastApi(): boolean {
    return native.fastApiAvailable();
  }

  private assertOpen(): void {
    if (this.closed) {
      throw new Error("caeneus cache is closed");
    }
  }
}

export default Cache;
