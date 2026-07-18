import { execFileSync } from "node:child_process";
import * as path from "node:path";
import { performance } from "node:perf_hooks";
import { isMainThread, parentPort, Worker, workerData } from "node:worker_threads";
import { LRUCache } from "lru-cache";
import NodeCache from "node-cache";

import { Cache } from "./index";

interface Profile {
  name: string;
  operations: number;
  keys: number;
  capacity: number;
  readRatio: number;
  skewPower: number;
}

interface Options {
  profile: "A" | "B" | "all";
  cache: string;
  api: "get" | "get_into";
  keyType: "string" | "buffer";
  operation: "mixed" | "get" | "set";
  operations?: number;
  keys?: number;
  valueSize: number;
  workers: number;
  sampleRate: number;
  repeats: number;
  warmupOperations: number;
}

type BenchmarkKey = string | Buffer;

interface WorkItem {
  key: BenchmarkKey;
  write: boolean;
}

interface CacheAdapter {
  set(key: BenchmarkKey, value: Buffer): void;
  get(key: BenchmarkKey): Buffer | null | undefined;
  getInto?(key: BenchmarkKey, output: Buffer): number | null;
  close(): void;
}

const profiles: Record<"A" | "B", Profile> = {
  A: {
    name: "A_read_heavy",
    operations: 200_000,
    keys: 30_000,
    capacity: 65_536,
    readRatio: 0.95,
    skewPower: 3,
  },
  B: {
    name: "B_eviction_storm",
    operations: 200_000,
    keys: 50_000,
    capacity: 8_192,
    readRatio: 0.5,
    skewPower: 0,
  },
};

class MapAdapter implements CacheAdapter {
  private readonly cache = new Map<BenchmarkKey, Buffer>();

  public set(key: BenchmarkKey, value: Buffer): void {
    this.cache.set(key, value);
  }

  public get(key: BenchmarkKey): Buffer | undefined {
    return this.cache.get(key);
  }

  public close(): void {
    this.cache.clear();
  }
}

class LruCacheAdapter implements CacheAdapter {
  private readonly cache: LRUCache<BenchmarkKey, Buffer>;

  public constructor(capacity: number) {
    this.cache = new LRUCache({ max: capacity });
  }

  public set(key: BenchmarkKey, value: Buffer): void {
    this.cache.set(key, value);
  }

  public get(key: BenchmarkKey): Buffer | undefined {
    return this.cache.get(key);
  }

  public close(): void {
    this.cache.clear();
  }
}

class NodeCacheAdapter implements CacheAdapter {
  private readonly cache = new NodeCache({
    stdTTL: 600,
    checkperiod: 0,
    useClones: false,
  });

  public set(key: BenchmarkKey, value: Buffer): void {
    this.cache.set(benchmarkKeyString(key), value);
  }

  public get(key: BenchmarkKey): Buffer | undefined {
    return this.cache.get<Buffer>(benchmarkKeyString(key));
  }

  public close(): void {
    this.cache.flushAll();
    this.cache.close();
  }
}

function benchmarkKeyString(key: BenchmarkKey): string {
  return typeof key === "string" ? key : key.toString("latin1");
}

function parseOptions(): Options {
  const values = new Map<string, string>();
  for (let index = 2; index < process.argv.length; index += 1) {
    const argument = process.argv[index];
    if (!argument.startsWith("--")) {
      continue;
    }
    const equals = argument.indexOf("=");
    if (equals >= 0) {
      values.set(argument.slice(0, equals), argument.slice(equals + 1));
      continue;
    }
    const next = process.argv[index + 1];
    if (next !== undefined && !next.startsWith("--")) {
      values.set(argument, next);
      index += 1;
    } else {
      values.set(argument, "true");
    }
  }
  const profile = values.get("--profile") ?? "all";
  if (profile !== "A" && profile !== "B" && profile !== "all") {
    throw new Error("--profile must be A, B, or all");
  }
  const keyType = values.get("--key-type") ?? "string";
  if (keyType !== "string" && keyType !== "buffer") {
    throw new Error("--key-type must be string or buffer");
  }
  const operation = values.get("--operation") ?? "mixed";
  if (operation !== "mixed" && operation !== "get" && operation !== "set") {
    throw new Error("--operation must be mixed, get, or set");
  }
  return {
    profile,
    cache: values.get("--cache") ?? "all",
    api: (() => {
      const api = values.get("--api") ?? "get";
      if (api !== "get" && api !== "get_into") {
        throw new Error("--api must be get or get_into");
      }
      return api;
    })(),
    keyType,
    operation,
    operations: values.has("--operations")
      ? Number(values.get("--operations"))
      : undefined,
    keys: values.has("--keys") ? Number(values.get("--keys")) : undefined,
    valueSize: Number(values.get("--value-size") ?? 128),
    workers: Number(values.get("--workers") ?? 1),
    sampleRate: Number(values.get("--sample-rate") ?? 1_000),
    repeats: Number(values.get("--repeats") ?? 1),
    warmupOperations: Number(values.get("--warmup-operations") ?? 0),
  };
}

// Maximum total native slab bytes across all workers for a single caeneus
// Cache. Each worker gets an equal share so RSS stays bounded regardless of
// how many workers --workers specifies. 1 GiB is a generous but safe ceiling
// for a benchmark tool running alongside other system processes.
const kMaxTotalSlabBytes = 1 * 1024 * 1024 * 1024;

function createAdapter(
  name: string,
  profile: Profile,
  workers: number,
): CacheAdapter {
  if (name === "caeneus") {
    const slots = Math.max(64, Math.ceil(profile.capacity / 64));
    // Divide the total budget equally across workers so that spawning N
    // workers does not multiply native (c_allocator) heap by N.
    const slabSizePerShard = Math.max(
      64 * 1024,
      Math.floor(kMaxTotalSlabBytes / (64 * Math.max(1, workers))),
    );
    return new Cache({
      numShards: 64,
      slotsPerShard: slots,
      slabSizePerShard,
    });
  }
  if (name === "map") {
    return new MapAdapter();
  }
  if (name === "lru_cache") {
    return new LruCacheAdapter(profile.capacity);
  }
  if (name === "node_cache") {
    return new NodeCacheAdapter();
  }
  throw new Error(`unknown cache implementation: ${name}`);
}

function nextRandom(state: { value: number }): number {
  state.value |= 0;
  state.value = (state.value + 0x6d2b79f5) | 0;
  let result = Math.imul(state.value ^ (state.value >>> 15), 1 | state.value);
  result = (result + Math.imul(result ^ (result >>> 7), 61 | result)) ^ result;
  return ((result ^ (result >>> 14)) >>> 0) / 4_294_967_296;
}

function buildWorkload(
  profile: Profile,
  keys: BenchmarkKey[],
  operation: Options["operation"],
): WorkItem[] {
  const state = { value: 42 };
  const workload: WorkItem[] = [];
  let writeIndex = 0;
  for (
    let operationIndex = 0;
    operationIndex < profile.operations;
    operationIndex += 1
  ) {
    const write =
      operation === "set"
        ? true
        : operation === "get"
          ? false
          : nextRandom(state) >= profile.readRatio;
    let keyIndex: number;
    if (profile.skewPower > 0) {
      keyIndex = Math.floor(
        Math.pow(nextRandom(state), profile.skewPower) * profile.keys,
      );
    } else {
      writeIndex += 1;
      keyIndex = writeIndex % profile.keys;
    }
    if (profile.name.startsWith("B") && !write) {
      const window = Math.min(profile.keys, profile.capacity * 4);
      const offset = Math.floor(nextRandom(state) * window);
      keyIndex = (writeIndex - offset + profile.keys) % profile.keys;
    }
    workload.push({ key: keys[keyIndex], write });
  }
  return workload;
}

function runSingle(
  implementation: string,
  profile: Profile,
  valueSize: number,
  sampleRate: number,
  api: Options["api"],
  workers: number,
  keyType: Options["keyType"],
  operation: Options["operation"],
  warmupOperations: number,
): Record<string, string | number> {
  const keyStrings = Array.from(
    { length: profile.keys },
    (_, index) => `key:${index.toString().padStart(8, "0")}`,
  );
  const keys: BenchmarkKey[] =
    keyType === "buffer"
      ? keyStrings.map((key) => Buffer.from(key))
      : keyStrings;
  const workload = buildWorkload(profile, keys, operation);
  const value = Buffer.alloc(valueSize, 0x61);
  const output = Buffer.alloc(valueSize);
  const cache = createAdapter(implementation, profile, workers);
  try {
    for (const key of keys) {
      cache.set(key, value);
    }

    const large = Buffer.alloc(128 * 1024, 0x62);
    const largeKey: BenchmarkKey =
      keyType === "buffer" ? Buffer.from("__large__") : "__large__";
    cache.set(largeKey, large);
    const largeResult = cache.get(largeKey);
    if (largeResult == null || largeResult.length !== large.length) {
      throw new Error(`${implementation}: large-value verification failed`);
    }

    for (
      let operationIndex = 0;
      operationIndex < Math.min(warmupOperations, workload.length);
      operationIndex += 1
    ) {
      const item = workload[operationIndex];
      if (item.write) {
        cache.set(item.key, value);
      } else if (api === "get_into") {
        if (cache.getInto === undefined) {
          throw new Error(`${implementation}: get_into is not supported`);
        }
        cache.getInto(item.key, output);
      } else {
        cache.get(item.key);
      }
    }

    const samples: number[] = [];
    let hits = 0;
    let misses = 0;
    const started = performance.now();
    for (let operation = 0; operation < workload.length; operation += 1) {
      const item = workload[operation];
      const sampleStart =
        operation % sampleRate === 0 ? performance.now() : 0;
      if (item.write) {
        cache.set(item.key, value);
      } else {
        if (api === "get_into") {
          if (cache.getInto === undefined) {
            throw new Error(`${implementation}: get_into is not supported`);
          }
          if (cache.getInto(item.key, output) === null) {
            misses += 1;
          } else {
            hits += 1;
          }
        } else if (cache.get(item.key) == null) {
          misses += 1;
        } else {
          hits += 1;
        }
      }
      if (sampleStart !== 0) {
        samples.push((performance.now() - sampleStart) * 1_000);
      }
    }
    const elapsedMs = performance.now() - started;
    samples.sort((left, right) => left - right);
    return {
      implementation,
      api,
      key_type: keyType,
      operation,
      profile: profile.name,
      operations: workload.length,
      workers: 1,
      value_bytes: valueSize,
      warmup_operations: Math.min(warmupOperations, workload.length),
      hits,
      misses,
      elapsed_ms: Number(elapsedMs.toFixed(3)),
      operations_per_second: Math.round((workload.length / elapsedMs) * 1_000),
      sample_count: samples.length,
      p50_us: Number((samples[Math.floor(samples.length * 0.5)] ?? 0).toFixed(3)),
      p99_us: Number((samples[Math.floor(samples.length * 0.99)] ?? 0).toFixed(3)),
    };
  } finally {
    cache.close();
  }
}

function withOverrides(profile: Profile, options: Options): Profile {
  return {
    ...profile,
    operations: options.operations ?? profile.operations,
    keys: options.keys ?? profile.keys,
  };
}

async function runFromWorker(): Promise<void> {
  const request = workerData as {
    implementation: string;
    profile: Profile;
    valueSize: number;
    sampleRate: number;
    api: Options["api"];
    workers: number;
    keyType: Options["keyType"];
    operation: Options["operation"];
    warmupOperations: number;
  };
  parentPort?.postMessage(
    runSingle(
      request.implementation,
      request.profile,
      request.valueSize,
      request.sampleRate,
      request.api,
      request.workers,
      request.keyType,
      request.operation,
      request.warmupOperations,
    ),
  );
}

function benchmarkMetadata(): Record<string, string> {
  let revision = process.env.CAENEUS_GIT_REVISION ?? "unknown";
  if (revision === "unknown") {
    try {
      revision = execFileSync(
        "git",
        ["-C", path.resolve(__dirname, "..", ".."), "rev-parse", "--short", "HEAD"],
        { encoding: "utf8" },
      ).trim();
    } catch {
      // Packaged consumers may not have a Git checkout.
    }
  }
  return {
    node_version: process.version,
    v8_version: process.versions.v8,
    platform: process.platform,
    arch: process.arch,
    bun_version: process.versions.bun ?? "none",
    native_boundary:
      process.versions.bun === undefined ? "node-v8-fast-api" : "bun-node-api",
    native_build: process.env.CAENEUS_NATIVE_BUILD ?? "unknown",
    git_revision: revision,
  };
}

async function runFromMain(): Promise<void> {
  const options = parseOptions();
  if (
    options.workers < 1 ||
    options.sampleRate < 1 ||
    options.repeats < 1 ||
    options.warmupOperations < 0
  ) {
    throw new Error(
      "--workers, --sample-rate, and --repeats must be positive; " +
        "--warmup-operations cannot be negative",
    );
  }
  const profileNames: Array<"A" | "B"> =
    options.profile === "all" ? ["A", "B"] : [options.profile];
  const implementations =
    options.cache === "all"
      ? ["caeneus", "map", "lru_cache", "node_cache"]
      : options.cache.split(",");

  for (const profileName of profileNames) {
    const profile = withOverrides(profiles[profileName], options);
    for (const implementation of implementations) {
      for (let repeat = 1; repeat <= options.repeats; repeat += 1) {
        const started = performance.now();
        let results: Array<Record<string, string | number>>;
        if (options.workers === 1) {
          results = [
            runSingle(
              implementation,
              profile,
              options.valueSize,
              options.sampleRate,
              options.api,
              options.workers,
              options.keyType,
              options.operation,
              options.warmupOperations,
            ),
          ];
        } else {
          results = await Promise.all(
            Array.from({ length: options.workers }, () =>
              new Promise<Record<string, string | number>>((resolve, reject) => {
                const worker = new Worker(__filename, {
                  workerData: {
                    implementation,
                    profile,
                    valueSize: options.valueSize,
                    sampleRate: options.sampleRate,
                    api: options.api,
                    workers: options.workers,
                    keyType: options.keyType,
                    operation: options.operation,
                    warmupOperations: options.warmupOperations,
                  },
                });
                worker.once("message", resolve);
                worker.once("error", reject);
              }),
            ),
          );
        }
        const elapsedMs =
          options.workers === 1
            ? Number(results[0].elapsed_ms)
            : performance.now() - started;
        const operations = results.reduce(
          (sum, result) => sum + Number(result.operations),
          0,
        );
        const hits = results.reduce((sum, result) => sum + Number(result.hits), 0);
        const misses = results.reduce(
          (sum, result) => sum + Number(result.misses),
          0,
        );
        console.log(
          JSON.stringify({
            ...benchmarkMetadata(),
            repeat,
            implementation,
            api: options.api,
            key_type: options.keyType,
            operation: options.operation,
            profile: profile.name,
            operations,
            workers: options.workers,
            value_bytes: options.valueSize,
            warmup_operations: options.warmupOperations,
            hits,
            misses,
            elapsed_ms: Number(elapsedMs.toFixed(3)),
            operations_per_second: Math.round((operations / elapsedMs) * 1_000),
            worker_elapsed_ms: Number(
              results
                .reduce((sum, result) => sum + Number(result.elapsed_ms), 0)
                .toFixed(3),
            ),
            p50_us: Number(
              (results.reduce((sum, result) => sum + Number(result.p50_us), 0) /
                results.length).toFixed(3),
            ),
            p99_us: Number(
              (results.reduce((sum, result) => sum + Number(result.p99_us), 0) /
                results.length).toFixed(3),
            ),
          }),
        );
      }
    }
  }
}

if (isMainThread) {
  runFromMain().catch((error: unknown) => {
    console.error(error);
    process.exitCode = 1;
  });
} else {
  runFromWorker().catch((error: unknown) => {
    console.error(error);
    process.exitCode = 1;
  });
}

