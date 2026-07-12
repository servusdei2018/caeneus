from __future__ import annotations

import concurrent.futures
import unittest

import caeneus


class CacheTests(unittest.TestCase):
    def make_cache(self) -> caeneus.Cache:
        return caeneus.Cache(
            num_shards=16,
            slots_per_shard=1024,
            slab_size_per_shard=4 * 1024 * 1024,
        )

    def test_set_get_and_binary_values(self) -> None:
        cache = self.make_cache()
        try:
            cache.set("hello", "world")
            self.assertEqual(cache.get("hello"), b"world")

            cache.set(b"binary", bytes([0, 1, 2, 255]))
            self.assertEqual(cache.get(b"binary"), bytes([0, 1, 2, 255]))
        finally:
            cache.close()

    def test_miss_returns_none(self) -> None:
        cache = self.make_cache()
        try:
            self.assertIsNone(cache.get("missing"))
        finally:
            cache.close()

    def test_size_oracle_allocates_exact_value(self) -> None:
        cache = self.make_cache()
        try:
            value = b"x" * (128 * 1024)
            cache.set("large", value)
            result = cache.get("large")
            self.assertEqual(result, value)
            self.assertEqual(len(result), len(value))
        finally:
            cache.close()

    def test_size_hint_handles_growth_and_shrink(self) -> None:
        cache = caeneus.Cache(
            num_shards=16,
            slots_per_shard=1024,
            slab_size_per_shard=4 * 1024 * 1024,
            initial_value_capacity=16,
        )
        try:
            cache.set("sized", b"a" * 16)
            self.assertEqual(cache.get("sized"), b"a" * 16)

            cache.set("sized", b"b" * 128)
            self.assertEqual(cache.get("sized"), b"b" * 128)

            cache.set("sized", b"c" * 3)
            self.assertEqual(cache.get("sized"), b"c" * 3)
        finally:
            cache.close()

    def test_get_into_reuses_writable_buffer(self) -> None:
        cache = self.make_cache()
        try:
            value = b"zero-copy destination"
            cache.set("buffered", value)
            output = bytearray(64)
            output_length = cache.get_into("buffered", output)
            self.assertEqual(output_length, len(value))
            self.assertEqual(bytes(output[:output_length]), value)
            self.assertIsNone(cache.get_into("missing", output))
            with self.assertRaises(BufferError):
                cache.get_into("buffered", bytearray(1))
        finally:
            cache.close()

    def test_concurrent_reads(self) -> None:
        cache = self.make_cache()
        try:
            cache.set("shared", b"value")

            def read(_: int) -> bytes | None:
                return cache.get("shared")

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(read, range(512)))
            self.assertTrue(all(result == b"value" for result in results))
        finally:
            cache.close()

    def test_concurrent_updates_and_reads(self) -> None:
        cache = self.make_cache()
        values = (b"short", b"v" * 256)
        try:
            cache.set("shared", values[0])

            def update_or_read(index: int) -> bytes | None:
                if index % 2 == 0:
                    cache.set("shared", values[(index // 2) % len(values)])
                    return None
                return cache.get("shared")

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(update_or_read, range(512)))
            self.assertTrue(
                all(result is None or result in values for result in results)
            )
        finally:
            cache.close()

    def test_close_is_idempotent(self) -> None:
        cache = self.make_cache()
        cache.close()
        cache.close()
        with self.assertRaises(RuntimeError):
            cache.get("closed")


if __name__ == "__main__":
    unittest.main()
