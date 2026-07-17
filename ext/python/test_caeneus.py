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

    def test_mapping_protocol(self) -> None:
        cache = self.make_cache()
        try:
            cache["hello"] = "world"
            self.assertEqual(cache["hello"], b"world")

            cache[b"binary"] = bytes([0, 1, 2, 255])
            self.assertEqual(cache[b"binary"], bytes([0, 1, 2, 255]))

            with self.assertRaises(KeyError):
                _ = cache["missing"]

            with self.assertRaises(TypeError):
                del cache["hello"]
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

            # 1. Direct bytearray (triggers direct bytearray fast-path)
            output = bytearray(64)
            output_length = cache.get_into("buffered", output)
            self.assertEqual(output_length, len(value))
            self.assertEqual(bytes(output[:output_length]), value)

            # 2. memoryview of bytearray (triggers generic buffer fallback-path)
            output_mv = memoryview(bytearray(64))
            output_length_mv = cache.get_into("buffered", output_mv)
            self.assertEqual(output_length_mv, len(value))
            self.assertEqual(bytes(output_mv[:output_length_mv]), value)

            self.assertIsNone(cache.get_into("missing", output))
            with self.assertRaises(BufferError):
                cache.get_into("buffered", bytearray(1))
        finally:
            cache.close()

    def test_buffer_protocol_support(self) -> None:
        cache = self.make_cache()
        try:
            # Test key/value as bytearray/memoryview in set/get
            key_ba = bytearray(b"bytearray-key")
            val_ba = bytearray(b"bytearray-value")
            
            cache.set(key_ba, val_ba)
            self.assertEqual(cache.get(key_ba), b"bytearray-value")
            
            key_mv = memoryview(key_ba)
            val_mv = memoryview(val_ba)
            
            cache.set(key_mv, val_mv)
            self.assertEqual(cache.get(key_mv), b"bytearray-value")
            
            # Check get_into works with buffer protocol keys
            output = bytearray(64)
            length = cache.get_into(key_mv, output)
            self.assertEqual(length, len(val_mv))
            self.assertEqual(bytes(output[:length]), b"bytearray-value")
            
            # Test that invalid types still raise TypeError
            with self.assertRaises(TypeError):
                cache.set(123, b"value")
            with self.assertRaises(TypeError):
                cache.set(b"key", 123)
            with self.assertRaises(TypeError):
                cache.get(123)
            with self.assertRaises(TypeError):
                cache.get_into(123, output)
        finally:
            cache.close()

    def test_numpy_array_support(self) -> None:
        try:
            import numpy as np
        except ImportError:
            return
            
        cache = self.make_cache()
        try:
            k = np.array([1, 2, 3], dtype=np.int64)
            v = np.array([4, 5, 6], dtype=np.int64)
            cache.set(k, v)
            self.assertEqual(cache.get(k), v.tobytes())
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

    def test_constructor_arguments(self) -> None:
        # Test default arguments
        c1 = caeneus.Cache()
        c1.close()

        # Test positional arguments
        c2 = caeneus.Cache(16, 1024, 4 * 1024 * 1024)
        c2.close()

        # Test keyword arguments
        c3 = caeneus.Cache(num_shards=8, slots_per_shard=512)
        c3.close()

        # Test mixed positional/keyword
        c4 = caeneus.Cache(16, slots_per_shard=256)
        c4.close()

        # Test invalid positional count
        with self.assertRaises(TypeError):
            caeneus.Cache(16, 1024, 4096, 0, 999, 111)

        # Test invalid keyword argument
        with self.assertRaises(TypeError):
            caeneus.Cache(num_shards=16, invalid_arg=True)

        # Test negative integer value
        with self.assertRaises(ValueError):
            caeneus.Cache(num_shards=-1)

        # Test negative gil_threshold
        with self.assertRaises(ValueError):
            caeneus.Cache(gil_threshold=-100)

        # Test valid gil_threshold
        c5 = caeneus.Cache(gil_threshold=2048)
        c5.close()


if __name__ == "__main__":
    unittest.main()
