import assert from "node:assert/strict";
import test from "node:test";

import { Cache } from "../index";

test("set/get supports strings and binary values", () => {
  const cache = new Cache();
  try {
    cache.set("hello", "world");
    assert.deepEqual(cache.get("hello"), Buffer.from("world"));

    const binary = Buffer.from([0, 1, 2, 255]);
    cache.set("binary", binary);
    assert.deepEqual(cache.get("binary"), binary);

    const key = Buffer.from("buf-key");
    cache.set(key, Buffer.from("buf-value"));
    assert.deepEqual(cache.get(key), Buffer.from("buf-value"));
  } finally {
    cache.close();
  }
});

test("get returns null for a miss", () => {
  const cache = new Cache();
  try {
    assert.equal(cache.get("missing"), null);
  } finally {
    cache.close();
  }
});

test("getInto writes into a caller-owned Buffer", () => {
  const cache = new Cache();
  try {
    const value = Buffer.from("reusable output");
    const output = Buffer.alloc(64);
    cache.set("buffered", value);
    const length = cache.getInto("buffered", output);
    assert.equal(length, value.length);
    assert.deepEqual(output.subarray(0, length ?? 0), value);
    assert.equal(cache.getInto("missing", output), null);
    assert.throws(
      () => cache.getInto("buffered", Buffer.alloc(1)),
      /too small/,
    );
  } finally {
    cache.close();
  }
});

test("get returns a Buffer view over reusable scratch storage", () => {
  const cache = new Cache();
  try {
    cache.set("a", "same-size-value!");
    const first = cache.get("a");
    assert.ok(Buffer.isBuffer(first));
    assert.equal(first?.toString(), "same-size-value!");

    cache.set("b", "same-size-value?");
    const second = cache.get("b");
    assert.ok(Buffer.isBuffer(second));
    assert.equal(second?.toString(), "same-size-value?");
    // Same-length hits reuse the cached view object; contents are overwritten.
    assert.equal(first, second);
    assert.equal(first?.toString(), "same-size-value?");
  } finally {
    cache.close();
  }
});

test("scratch storage resizes for large values", () => {
  const cache = new Cache();
  try {
    const value = Buffer.alloc(128 * 1024, 0x5a);
    cache.set("large", value);
    assert.deepEqual(cache.get("large"), value);
  } finally {
    cache.close();
  }
});

test("cache close is idempotent and rejects later operations", () => {
  const cache = new Cache();
  cache.close();
  cache.close();
  assert.throws(() => cache.get("closed"), /closed/);
});

