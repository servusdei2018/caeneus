package caeneus_test

import (
	"fmt"
	"math"
	"math/rand/v2"
	"testing"
	"unsafe"

	caeneus "github.com/servusdei2018/caeneus"
)

// b2s converts a byte slice to a string without allocation.
//
//go:nosplit
func b2s(b []byte) string {
	return unsafe.String(unsafe.SliceData(b), len(b))
}

// defaultTestCache returns a Cache configured for testing.
func defaultTestCache(tb testing.TB) *caeneus.Cache {
	tb.Helper()
	c, err := caeneus.NewCache(16, 1024, 4*1024*1024)
	if err != nil {
		tb.Fatalf("NewCache: %v", err)
	}
	tb.Cleanup(c.Close)
	return c
}

func TestSetGet(t *testing.T) {
	c := defaultTestCache(t)

	if err := c.Set("hello", "world"); err != nil {
		t.Fatalf("Set: %v", err)
	}

	var buf [64]byte
	n, err := c.Get("hello", buf[:])
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if string(buf[:n]) != "world" {
		t.Fatalf("got %q, want %q", buf[:n], "world")
	}
}

func TestSetGetBytes(t *testing.T) {
	c := defaultTestCache(t)

	key := []byte("binary-key")
	value := []byte{0, 1, 2, 255}
	if err := c.SetBytes(key, value); err != nil {
		t.Fatalf("SetBytes: %v", err)
	}

	dest := make([]byte, len(value))
	n, err := c.GetBytes(key, dest)
	if err != nil {
		t.Fatalf("GetBytes: %v", err)
	}
	if string(dest[:n]) != string(value) {
		t.Fatalf("got %v, want %v", dest[:n], value)
	}
}

func TestGetMiss(t *testing.T) {
	c := defaultTestCache(t)

	var buf [64]byte
	_, err := c.Get("no_such_key", buf[:])
	if err != caeneus.ErrCacheMiss {
		t.Fatalf("expected ErrCacheMiss, got %v", err)
	}
}

func TestGetBufferTooSmall(t *testing.T) {
	c := defaultTestCache(t)

	if err := c.Set("k", "hello"); err != nil {
		t.Fatalf("Set: %v", err)
	}

	// Use a buffer too small to hold the value.
	var buf [1]byte
	required, err := c.Get("k", buf[:])
	if err != caeneus.ErrBufferTooSmall {
		t.Fatalf("expected ErrBufferTooSmall, got %v", err)
	}
	if required != len("hello") {
		t.Fatalf("required=%d, want %d", required, len("hello"))
	}

	// Retry with a buffer of the required size.
	dest := make([]byte, required)
	n, err := c.Get("k", dest)
	if err != nil {
		t.Fatalf("Get retry: %v", err)
	}
	if string(dest[:n]) != "hello" {
		t.Fatalf("got %q, want %q", dest[:n], "hello")
	}
}

func TestSetOverwrite(t *testing.T) {
	c := defaultTestCache(t)

	if err := c.Set("k", "v1"); err != nil {
		t.Fatalf("Set v1: %v", err)
	}
	if err := c.Set("k", "v2"); err != nil {
		t.Fatalf("Set v2: %v", err)
	}
	if err := c.Set("k", "v2"); err != nil {
		t.Fatalf("Set unchanged v2: %v", err)
	}

	var buf [64]byte
	n, err := c.Get("k", buf[:])
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if string(buf[:n]) != "v2" {
		t.Fatalf("got %q, want %q", buf[:n], "v2")
	}
}

func TestEmptyKeyValue(t *testing.T) {
	c := defaultTestCache(t)

	if err := c.Set("empty_val", ""); err != nil {
		t.Fatalf("Set empty value: %v", err)
	}
	// A zero-length value is treated as absent on read.
	var buf [64]byte
	_, err := c.Get("empty_val", buf[:])
	if err != caeneus.ErrCacheMiss {
		t.Fatalf("expected ErrCacheMiss for zero-length value, got %v", err)
	}
}

func prepareBenchmarkCache(
	b *testing.B,
) (*caeneus.Cache, []string, [][]byte) {
	const numKeys = 100_000

	c, err := caeneus.NewCache(64, 8192, 16*1024*1024)
	if err != nil {
		b.Fatalf("NewCache: %v", err)
	}
	defer c.Close()

	keys := make([]string, numKeys)
	byteKeys := make([][]byte, numKeys)
	for i := range keys {
		keys[i] = fmt.Sprintf("bench:key:%06d", i)
		byteKeys[i] = []byte(keys[i])
		valSize := max(1, int(1000.0/math.Pow(float64(i+1), 0.5)))
		val := make([]byte, valSize)
		for j := range val {
			val[j] = byte('a' + (i+j)%26)
		}
		if err := c.Set(keys[i], b2s(val)); err != nil {
			b.Fatalf("pre-populate Set key %d: %v", i, err)
		}
	}
	return c, keys, byteKeys
}

// BenchmarkGet benchmarks Cache.Get.
func BenchmarkGet(b *testing.B) {
	c, keys, _ := prepareBenchmarkCache(b)
	defer c.Close()
	zipf := rand.NewZipf(
		rand.New(rand.NewPCG(42, 0)),
		1.1,
		1.0,
		uint64(len(keys)-1),
	)

	b.ReportAllocs()
	b.ResetTimer()
	b.SetParallelism(256)

	b.RunParallel(func(pb *testing.PB) {
		var buf [1024]byte
		for pb.Next() {
			idx := int(zipf.Uint64())
			_, _ = c.Get(keys[idx], buf[:])
		}
	})
}

// BenchmarkGetBytes benchmarks Cache.GetBytes without a key conversion.
func BenchmarkGetBytes(b *testing.B) {
	c, _, byteKeys := prepareBenchmarkCache(b)
	defer c.Close()
	zipf := rand.NewZipf(rand.New(rand.NewPCG(42, 0)), 1.1, 1.0, uint64(len(byteKeys)-1))

	b.ReportAllocs()
	b.ResetTimer()
	b.SetParallelism(256)

	b.RunParallel(func(pb *testing.PB) {
		var buf [1024]byte
		for pb.Next() {
			idx := int(zipf.Uint64())
			_, _ = c.GetBytes(byteKeys[idx], buf[:])
		}
	})
}
