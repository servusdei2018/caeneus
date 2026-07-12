from __future__ import annotations

import argparse
import concurrent.futures
import json
import random
import statistics
import sys
import time
from dataclasses import dataclass, replace
from typing import Any, Callable, Protocol

import caeneus

try:
    from cachetools import LRUCache, TTLCache
except ImportError:
    LRUCache = TTLCache = None

try:
    from lru import LRU
except ImportError:
    LRU = None


class CacheAdapter(Protocol):
    def set(self, key: str, value: bytes) -> None: ...

    def get(self, key: str) -> bytes | None: ...

    def close(self) -> None: ...


class DictCache:
    def __init__(self, _: int) -> None:
        self.cache: dict[str, bytes] = {}

    def set(self, key: str, value: bytes) -> None:
        self.cache[key] = value

    def get(self, key: str) -> bytes | None:
        return self.cache.get(key)

    def close(self) -> None:
        self.cache.clear()


class CacheToolsAdapter:
    def __init__(self, cache: Any) -> None:
        self.cache = cache

    def set(self, key: str, value: bytes) -> None:
        self.cache[key] = value

    def get(self, key: str) -> bytes | None:
        return self.cache.get(key)

    def close(self) -> None:
        self.cache.clear()


class LruDictAdapter(CacheToolsAdapter):
    pass


@dataclass(frozen=True)
class Profile:
    name: str
    operations: int
    keys: int
    capacity: int
    read_ratio: float
    skew_power: float


PROFILES = {
    "A": Profile("A_read_heavy", 200_000, 30_000, 65_536, 0.95, 3.0),
    "B": Profile("B_eviction_storm", 200_000, 50_000, 8_192, 0.50, 0.0),
}


def percentile(values: list[int], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index] / 1_000.0


def make_factory(
    name: str,
    profile: Profile,
    initial_value_capacity: int,
) -> Callable[[], CacheAdapter]:
    if name == "caeneus":
        slots = max(64, (profile.capacity + 63) // 64)

        def create() -> CacheAdapter:
            return caeneus.Cache(
                num_shards=64,
                slots_per_shard=slots,
                slab_size_per_shard=16 * 1024 * 1024,
                initial_value_capacity=initial_value_capacity,
            )

        return create
    if name == "plain_dict":
        return lambda: DictCache(profile.capacity)
    if name == "cachetools_lru":
        if LRUCache is None:
            raise RuntimeError("install the benchmarks extra to use cachetools")
        return lambda: CacheToolsAdapter(LRUCache(maxsize=profile.capacity))
    if name == "cachetools_ttl":
        if TTLCache is None:
            raise RuntimeError("install the benchmarks extra to use cachetools")
        return lambda: CacheToolsAdapter(
            TTLCache(maxsize=profile.capacity, ttl=600)
        )
    if name == "lru_dict":
        if LRU is None:
            raise RuntimeError("install the benchmarks extra to use lru-dict")
        return lambda: LruDictAdapter(LRU(profile.capacity))
    raise ValueError(f"unknown cache implementation: {name}")


def build_workload(profile: Profile, keys: list[str]) -> list[tuple[str, bool]]:
    rng = random.Random(42)
    workload: list[tuple[str, bool]] = []
    write_ratio = 1.0 - profile.read_ratio
    write_index = 0
    for operation in range(profile.operations):
        is_write = rng.random() < write_ratio
        if profile.skew_power:
            key_index = int((rng.random() ** profile.skew_power) * profile.keys)
        else:
            write_index += 1
            key_index = write_index % profile.keys
        if profile.name.startswith("B") and not is_write:
            offset = rng.randrange(min(profile.keys, profile.capacity * 4))
            key_index = (write_index - offset) % profile.keys
        workload.append((keys[key_index], is_write))
    return workload


def run_implementation(
    name: str,
    profile: Profile,
    keys: list[str],
    values: bytes,
    workload: list[tuple[str, bool]],
    workers: int,
    sample_rate: int,
) -> dict[str, object]:
    factory = make_factory(name, profile, len(values))
    caches = [factory() for _ in range(workers)]
    try:
        for cache in caches:
            for key in keys:
                cache.set(key, values)
                if cache.get(key) is None:
                    raise RuntimeError(f"{name}: warmup verification failed")

        # Verify the native size-oracle path before timing the common workload.
        large = b"b" * (128 * 1024)
        for cache in caches:
            cache.set("__large__", large)
            if cache.get("__large__") != large:
                raise RuntimeError(f"{name}: large-value verification failed")

        def worker(worker_index: int) -> tuple[list[int], int, int]:
            cache = caches[worker_index]
            samples: list[int] = []
            hits = 0
            misses = 0
            for operation, (key, is_write) in enumerate(
                workload[worker_index::workers]
            ):
                started = (
                    time.perf_counter_ns()
                    if operation % sample_rate == 0
                    else 0
                )
                if is_write:
                    cache.set(key, values)
                elif cache.get(key) is None:
                    misses += 1
                else:
                    hits += 1
                if started:
                    samples.append(time.perf_counter_ns() - started)
            return samples, hits, misses

        started = time.perf_counter_ns()
        if workers == 1:
            results = [worker(0)]
        else:
            with concurrent.futures.ThreadPoolExecutor(
                max_workers=workers
            ) as pool:
                results = list(pool.map(worker, range(workers)))
        elapsed_ns = time.perf_counter_ns() - started
    finally:
        for cache in caches:
            cache.close()

    samples = [sample for result in results for sample in result[0]]
    hits = sum(result[1] for result in results)
    misses = sum(result[2] for result in results)
    return {
        "implementation": name,
        "profile": profile.name,
        "operations": len(workload),
        "workers": workers,
        "value_bytes": len(values),
        "hits": hits,
        "misses": misses,
        "elapsed_ms": round(elapsed_ns / 1_000_000, 3),
        "operations_per_second": round(len(workload) * 1_000_000_000 / elapsed_ns),
        "sample_count": len(samples),
        "p50_us": round(percentile(samples, 0.50), 3),
        "p99_us": round(percentile(samples, 0.99), 3),
        "sample_mean_us": round(statistics.fmean(samples) / 1_000, 3)
        if samples
        else 0.0,
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare Caeneus with common Python in-memory caches"
    )
    parser.add_argument("--profile", choices=("A", "B", "all"), default="all")
    parser.add_argument("--cache", default="all")
    parser.add_argument("--operations", type=int)
    parser.add_argument("--keys", type=int)
    parser.add_argument("--value-size", type=int, default=128)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--sample-rate", type=int, default=1_000)
    args = parser.parse_args()

    if args.workers < 1 or args.sample_rate < 1:
        parser.error("--workers and --sample-rate must be positive")

    profile_names = ("A", "B") if args.profile == "all" else (args.profile,)
    implementation_names = (
        ("caeneus", "plain_dict", "cachetools_lru", "cachetools_ttl", "lru_dict")
        if args.cache == "all"
        else tuple(args.cache.split(","))
    )

    for profile_key in profile_names:
        profile = PROFILES[profile_key]
        if args.operations is not None:
            profile = replace(profile, operations=args.operations)
        if args.keys is not None:
            profile = replace(profile, keys=args.keys)
        keys = [f"key:{index:08d}" for index in range(profile.keys)]
        workload = build_workload(profile, keys)
        values = b"a" * args.value_size

        for implementation in implementation_names:
            try:
                result = run_implementation(
                    implementation,
                    profile,
                    keys,
                    values,
                    workload,
                    args.workers,
                    args.sample_rate,
                )
            except RuntimeError as error:
                print(f"skipping {implementation}: {error}", file=sys.stderr)
                continue
            print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
