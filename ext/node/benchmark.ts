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
  operations?: number;
  keys?: number;
  valueSize: number;
  workers: number;
  sampleRate: number;
}

interface WorkItem {
  key: string;
  write: boolean;
}

interface CacheAdapter {
  set(key: string, value: Buffer): void;
  get(key: string): Buffer | null | undefined;
  getInto?(key: string, output: Buffer): number | null;
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
  private readonly cache = new Map<string, Buffer>();

  public set(key: string, value: Buffer): void {
    this.cache.set(key, value);
  }

  public get(key: string): Buffer | undefined {
    return this.cache.get(key);
  }

  public close(): void {
    this.cache.clear();
  }
}

class LruCacheAdapter implements CacheAdapter {
  private readonly cache: LRUCache<string, Buffer>;

  public constructor(capacity: number) {
    this.cache = new LRUCache({ max: capacity });
  }

  public set(key: string, value: Buffer): void {
    this.cache.set(key, value);
  }

  public get(key: string): Buffer | undefined {
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

  public set(key: string, value: Buffer): void {
    this.cache.set(key, value);
  }

  public get(key: string): Buffer | undefined {
    return this.cache.get<Buffer>(key);
  }

  public close(): void {
    this.cache.flushAll();
    this.cache.close();
  }
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
    operations: values.has("--operations")
      ? Number(values.get("--operations"))
      : undefined,
    keys: values.has("--keys") ? Number(values.get("--keys")) : undefined,
    valueSize: Number(values.get("--value-size") ?? 128),
    workers: Number(values.get("--workers") ?? 1),
    sampleRate: Number(values.get("--sample-rate") ?? 1_000),
  };
}

function createAdapter(name: string, profile: Profile): CacheAdapter {
  if (name === "caeneus") {
    const slots = Math.max(64, Math.ceil(profile.capacity / 64));
    return new Cache({
      numShards: 64,
      slotsPerShard: slots,
      slabSizePerShard: 16 * 1024 * 1024,
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

function buildWorkload(profile: Profile, keys: string[]): WorkItem[] {
  const state = { value: 42 };
  const workload: WorkItem[] = [];
  let writeIndex = 0;
  for (let operation = 0; operation < profile.operations; operation += 1) {
    const write = nextRandom(state) >= profile.readRatio;
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
): Record<string, string | number> {
  const keys = Array.from(
    { length: profile.keys },
    (_, index) => `key:${index.toString().padStart(8, "0")}`,
  );
  const workload = buildWorkload(profile, keys);
  const value = Buffer.alloc(valueSize, 0x61);
  const output = Buffer.alloc(valueSize);
  const cache = createAdapter(implementation, profile);
  try {
    for (const key of keys) {
      cache.set(key, value);
    }

    const large = Buffer.alloc(128 * 1024, 0x62);
    cache.set("__large__", large);
    const largeResult = cache.get("__large__");
    if (largeResult == null || largeResult.length !== large.length) {
      throw new Error(`${implementation}: large-value verification failed`);
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
      profile: profile.name,
      operations: workload.length,
      workers: 1,
      value_bytes: valueSize,
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
  };
  parentPort?.postMessage(
    runSingle(
      request.implementation,
      request.profile,
      request.valueSize,
      request.sampleRate,
      request.api,
    ),
  );
}

async function runFromMain(): Promise<void> {
  const options = parseOptions();
  if (options.workers < 1 || options.sampleRate < 1) {
    throw new Error("--workers and --sample-rate must be positive");
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
          implementation,
          api: options.api,
          profile: profile.name,
          operations,
          workers: options.workers,
          value_bytes: options.valueSize,
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

