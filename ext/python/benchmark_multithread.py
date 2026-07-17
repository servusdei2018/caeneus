from __future__ import annotations

import argparse
import concurrent.futures
import json
import random
import statistics
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Protocol

import caeneus

try:
    from cachetools import LRUCache
except ImportError:
    LRUCache = None

try:
    from lru import LRU
except ImportError:
    LRU = None


class CacheAdapter(Protocol):
    def set(self, key: str, value: bytes) -> None: ...

    def get(self, key: str) -> bytes | None: ...

    def get_into(self, key: str, output: bytearray) -> int | None: ...

    def close(self) -> None: ...


class DictCache:
    def __init__(self, _: int) -> None:
        self.cache: dict[str, bytes] = {}

    def set(self, key: str, value: bytes) -> None:
        self.cache[key] = value

    def get(self, key: str) -> bytes | None:
        return self.cache.get(key)

    def get_into(self, key: str, output: bytearray) -> int | None:
        value = self.get(key)
        if value is None:
            return None
        if len(value) > len(output):
            raise BufferError(f"value requires {len(value)} bytes")
        output[: len(value)] = value
        return len(value)

    def close(self) -> None:
        self.cache.clear()


class CacheToolsAdapter:
    def __init__(self, cache: Any) -> None:
        self.cache = cache

    def set(self, key: str, value: bytes) -> None:
        self.cache[key] = value

    def get(self, key: str) -> bytes | None:
        return self.cache.get(key)

    def get_into(self, key: str, output: bytearray) -> int | None:
        value = self.get(key)
        if value is None:
            return None
        if len(value) > len(output):
            raise BufferError(f"value requires {len(value)} bytes")
        output[: len(value)] = value
        return len(value)

    def close(self) -> None:
        self.cache.clear()


class LruDictAdapter(CacheToolsAdapter):
    pass


@dataclass(frozen=True)
class Profile:
    name: str
    write_ratio: float
    hot_key: bool


PROFILES = {
    "read_distributed": Profile("read_distributed", 0.0, False),
    "read_hot": Profile("read_hot", 0.0, True),
    "mixed_distributed": Profile("mixed_distributed", 0.20, False),
    "mixed_hot": Profile("mixed_hot", 0.20, True),
}


def percentile(values: list[int], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index] / 1_000.0


def native_shard_count(workers: int) -> int:
    shards = 1
    while shards < max(16, workers * 4):
        shards <<= 1
    return shards


def make_factory(
    implementation: str,
    workers: int,
    keys_per_worker: int,
    value_size: int,
    initial_value_capacity: int,
    gil_threshold: int | None = None,
) -> Callable[[], CacheAdapter]:
    capacity = max(1024, workers * keys_per_worker * 2)
    if implementation == "caeneus":
        num_shards = native_shard_count(workers)

        def create_native() -> CacheAdapter:
            kwargs = {
                "num_shards": num_shards,
                "slots_per_shard": 1024,
                "slab_size_per_shard": 4 * 1024 * 1024,
                "initial_value_capacity": initial_value_capacity,
            }
            if gil_threshold is not None:
                kwargs["gil_threshold"] = gil_threshold
            return caeneus.Cache(**kwargs)

        return create_native
    if implementation == "plain_dict":
        return lambda: DictCache(capacity)
    if implementation == "cachetools_lru":
        if LRUCache is None:
            raise RuntimeError("install the benchmarks extra to use cachetools")
        return lambda: CacheToolsAdapter(LRUCache(maxsize=capacity))
    if implementation == "lru_dict":
        if LRU is None:
            raise RuntimeError("install the benchmarks extra to use lru-dict")
        return lambda: LruDictAdapter(LRU(capacity))
    raise ValueError(f"unknown cache implementation: {implementation}")


def build_workloads(
    profile: Profile,
    workers: int,
    operations_per_worker: int,
    keys_per_worker: int,
) -> tuple[list[list[str]], list[list[tuple[str, bool]]]]:
    key_sets: list[list[str]] = []
    workloads: list[list[tuple[str, bool]]] = []
    for worker_index in range(workers):
        keys = (
            ["hot-key"]
            if profile.hot_key
            else [
                f"worker:{worker_index}:key:{key_index:06d}"
                for key_index in range(keys_per_worker)
            ]
        )
        rng = random.Random(42 + worker_index)
        operations: list[tuple[str, bool]] = []
        for _ in range(operations_per_worker):
            key = keys[rng.randrange(len(keys))]
            is_write = rng.random() < profile.write_ratio
            operations.append((key, is_write))
        key_sets.append(keys)
        workloads.append(operations)
    return key_sets, workloads


def run_one(
    implementation: str,
    profile: Profile,
    mode: str,
    api: str,
    workers: int,
    operations_per_worker: int,
    keys_per_worker: int,
    value: bytes,
    sample_rate: int,
    initial_value_capacity: int,
    gil_threshold: int | None = None,
) -> dict[str, object]:
    key_sets, workloads = build_workloads(
        profile,
        workers,
        operations_per_worker,
        keys_per_worker,
    )
    factory = make_factory(
        implementation,
        workers,
        keys_per_worker,
        len(value),
        initial_value_capacity,
        gil_threshold,
    )
    if mode == "shared":
        caches = [factory()]
        worker_caches = [caches[0]] * workers
        warmup_sets = [sorted({key for keys in key_sets for key in keys})]
    else:
        caches = [factory() for _ in range(workers)]
        worker_caches = caches
        warmup_sets = key_sets
    output_buffers = [bytearray(len(value)) for _ in range(workers)]

    try:
        for cache, keys in zip(
            worker_caches if mode == "local" else caches,
            warmup_sets,
        ):
            for key in keys:
                cache.set(key, value)
                if cache.get(key) is None:
                    raise RuntimeError(f"{implementation}: warmup failed")

        ready = threading.Barrier(workers + 1)
        start = threading.Barrier(workers + 1)

        def worker(worker_index: int) -> tuple[list[int], int, int]:
            cache = worker_caches[worker_index]
            samples: list[int] = []
            hits = 0
            misses = 0
            ready.wait()
            start.wait()
            for operation, (key, is_write) in enumerate(workloads[worker_index]):
                operation_start = (
                    time.perf_counter_ns()
                    if operation % sample_rate == 0
                    else 0
                )
                if is_write:
                    cache.set(key, value)
                else:
                    if api == "get_into":
                        output_length = cache.get_into(
                            key,
                            output_buffers[worker_index],
                        )
                        if output_length is None:
                            misses += 1
                        elif output_length != len(value):
                            raise RuntimeError(
                                f"{implementation}: value length changed"
                            )
                        else:
                            hits += 1
                    else:
                        result = cache.get(key)
                        if result is None:
                            misses += 1
                        else:
                            if len(result) != len(value):
                                raise RuntimeError(
                                    f"{implementation}: value length changed"
                                )
                            hits += 1
                if operation_start:
                    samples.append(time.perf_counter_ns() - operation_start)
            return samples, hits, misses

        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(worker, index) for index in range(workers)]
            ready.wait()
            started = time.perf_counter_ns()
            start.wait()
            results = [future.result() for future in futures]
        elapsed_ns = time.perf_counter_ns() - started
    finally:
        for cache in {id(cache): cache for cache in caches}.values():
            cache.close()

    samples = [sample for result in results for sample in result[0]]
    hits = sum(result[1] for result in results)
    misses = sum(result[2] for result in results)
    operations = workers * operations_per_worker
    return {
        "implementation": implementation,
        "profile": profile.name,
        "mode": mode,
        "api": api,
        "workers": workers,
        "operations": operations,
        "operations_per_worker": operations_per_worker,
        "value_bytes": len(value),
        "initial_value_capacity": initial_value_capacity,
        "hits": hits,
        "misses": misses,
        "elapsed_ms": round(elapsed_ns / 1_000_000, 3),
        "operations_per_second": round(operations * 1_000_000_000 / elapsed_ns),
        "sample_count": len(samples),
        "p50_us": round(percentile(samples, 0.50), 3),
        "p99_us": round(percentile(samples, 0.99), 3),
        "sample_mean_us": round(statistics.fmean(samples) / 1_000, 3)
        if samples
        else 0.0,
    }


def parse_workers(value: str) -> tuple[int, ...]:
    workers = tuple(sorted({int(item) for item in value.split(",")}))
    if not workers or any(worker < 1 for worker in workers):
        raise argparse.ArgumentTypeError("workers must be positive integers")
    return workers


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Measure Python cache throughput under thread contention"
    )
    parser.add_argument(
        "--profile",
        choices=(*PROFILES.keys(), "all"),
        default="all",
    )
    parser.add_argument(
        "--mode",
        choices=("shared", "local", "both"),
        default="both",
        help="share one cache across threads, use one per thread, or run both",
    )
    parser.add_argument(
        "--api",
        choices=("get", "get_into", "both"),
        default="get",
        help="allocate Python bytes, reuse a writable buffer, or run both",
    )
    parser.add_argument("--implementation", default="all")
    parser.add_argument(
        "--workers",
        type=parse_workers,
        default=(1, 2, 4, 8),
        help="comma-separated worker counts",
    )
    parser.add_argument("--operations-per-worker", type=int, default=50_000)
    parser.add_argument("--keys-per-worker", type=int, default=2_048)
    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--sample-rate", type=int, default=1_000)
    parser.add_argument(
        "--initial-value-capacity",
        type=int,
        help="initial native get buffer size; defaults to --value-size, use 0 to force probing",
    )
    parser.add_argument(
        "--gil-threshold",
        type=int,
        help="value size threshold in bytes below which GIL is not released",
    )
    args = parser.parse_args()

    if (
        args.operations_per_worker < 1
        or args.keys_per_worker < 1
        or args.value_size < 0
        or args.sample_rate < 1
    ):
        parser.error(
            "operations, keys, sample rate, and value size must be positive "
            "(value size may be zero)"
        )
    initial_value_capacity = (
        args.value_size
        if args.initial_value_capacity is None
        else args.initial_value_capacity
    )
    if initial_value_capacity < 0:
        parser.error("--initial-value-capacity cannot be negative")
    if args.gil_threshold is not None and args.gil_threshold < 0:
        parser.error("--gil-threshold cannot be negative")

    profiles = (
        tuple(PROFILES.values())
        if args.profile == "all"
        else (PROFILES[args.profile],)
    )
    implementations = (
        ("caeneus", "plain_dict", "cachetools_lru", "lru_dict")
        if args.implementation == "all"
        else tuple(args.implementation.split(","))
    )
    modes = ("shared", "local") if args.mode == "both" else (args.mode,)
    apis = ("get", "get_into") if args.api == "both" else (args.api,)
    value = b"a" * args.value_size

    for profile in profiles:
        for workers in args.workers:
            for mode in modes:
                for api in apis:
                    for implementation in implementations:
                        try:
                            result = run_one(
                                implementation,
                                profile,
                                mode,
                                api,
                                workers,
                                args.operations_per_worker,
                                args.keys_per_worker,
                                value,
                                args.sample_rate,
                                initial_value_capacity,
                                args.gil_threshold,
                            )
                        except RuntimeError as error:
                            print(f"skipping {implementation}: {error}")
                            continue
                        print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
