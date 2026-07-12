package benchmarks

import (
	"context"
	"math/rand/v2"
	"os"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"

	"github.com/allegro/bigcache/v3"
	"github.com/dgraph-io/ristretto"
	"github.com/maypok86/otter"
	gocache "github.com/patrickmn/go-cache"
	"github.com/servusdei2018/caeneus"
)

const (
	keyLen  = 16
	valLen  = 128
	numVals = 1000
)

var (
	keysA    [][]byte
	keysB    [][]byte
	valsPool [][]byte
	onceA    sync.Once
	onceB    sync.Once
	onceVals sync.Once
)

// b2s converts a byte slice to a string without allocations.
//
//go:nosplit
func b2s(b []byte) string {
	return unsafe.String(unsafe.SliceData(b), len(b))
}

// fastrand returns a pseudo-random number using Xorshift64.
//
//go:nosplit
func fastrand(state *uint64) uint64 {
	x := *state
	x ^= x << 13
	x ^= x >> 7
	x ^= x << 17
	*state = x
	return x
}

func preallocateKeys(numKeys int) [][]byte {
	data := make([]byte, numKeys*keyLen)
	keys := make([][]byte, numKeys)
	for i := 0; i < numKeys; i++ {
		k := data[i*keyLen : (i+1)*keyLen]
		copy(k, "k:")
		val := uint64(i)
		for j := keyLen - 1; j >= 2; j-- {
			k[j] = '0' + byte(val%10)
			val /= 10
		}
		keys[i] = k
	}
	return keys
}

func preallocateValues(num int) [][]byte {
	data := make([]byte, num*valLen)
	vals := make([][]byte, num)
	for i := 0; i < num; i++ {
		v := data[i*valLen : (i+1)*valLen]
		for j := 0; j < valLen; j++ {
			v[j] = byte('A' + (i+j)%26)
		}
		vals[i] = v
	}
	return vals
}

func profileAKeyCount() int {
	if os.Getenv("CAENEUS_GO_QUICK") == "1" {
		return 2_048
	}
	return 150_000
}

func getKeysA() [][]byte {
	onceA.Do(func() {
		keysA = preallocateKeys(profileAKeyCount())
	})
	return keysA
}

func getKeysB() [][]byte {
	onceB.Do(func() {
		keysB = preallocateKeys(20_000_000)
	})
	return keysB
}

func getVals() [][]byte {
	onceVals.Do(func() {
		valsPool = preallocateValues(numVals)
	})
	return valsPool
}

func runProfileA(b *testing.B, cacheSetter func(key string, val []byte), cacheGetter func(key string)) {
	keys := getKeysA()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	b.ReportAllocs()
	b.ResetTimer()

	b.RunParallel(func(pb *testing.PB) {
		seed1 := uint64(rand.Uint64())
		seed2 := uint64(rand.Uint64())
		if seed1 == 0 {
			seed1 = 1
		}
		r := rand.New(rand.NewPCG(seed1, seed2))
		zipf := rand.NewZipf(r, 1.1, 2.0, uint64(numKeys-1))

		localSeed := seed1

		for pb.Next() {
			idx := zipf.Uint64()
			keyStr := b2s(keys[idx])
			pct := fastrand(&localSeed) % 100

			if pct < 95 {
				cacheGetter(keyStr)
			} else {
				cacheSetter(keyStr, vals[idx%uint64(numVals)])
			}
		}
	})
}

// BenchmarkProfileA_Caeneus benchmarks the zero-allocation hot path.
func BenchmarkProfileA_Caeneus(b *testing.B) {
	cache, err := caeneus.NewCache(4096, 1024, 256*1024)
	if err != nil {
		b.Fatalf("NewCache failed: %v", err)
	}
	defer cache.Close()

	keys := getKeysA()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		_ = cache.Set(b2s(keys[i]), b2s(vals[i%numVals]))
	}

	b.ReportAllocs()
	b.ResetTimer()

	b.RunParallel(func(pb *testing.PB) {
		seed1 := uint64(rand.Uint64())
		seed2 := uint64(rand.Uint64())
		if seed1 == 0 {
			seed1 = 1
		}
		r := rand.New(rand.NewPCG(seed1, seed2))
		zipf := rand.NewZipf(r, 1.1, 2.0, uint64(numKeys-1))

		localSeed := seed1
		var buf [valLen + 16]byte

		for pb.Next() {
			idx := zipf.Uint64()
			keyStr := b2s(keys[idx])
			pct := fastrand(&localSeed) % 100

			if pct < 95 {
				_, _ = cache.Get(keyStr, buf[:])
			} else {
				_ = cache.Set(keyStr, b2s(vals[idx%uint64(numVals)]))
			}
		}
	})
}

func BenchmarkProfileA_BigCache(b *testing.B) {
	cfg := bigcache.Config{
		Shards:             256,
		LifeWindow:         10 * time.Minute,
		CleanWindow:        0, // Disable background clean during benchmark run
		MaxEntriesInWindow: 150_000,
		MaxEntrySize:       valLen,
		Verbose:            false,
	}
	cache, err := bigcache.New(context.Background(), cfg)
	if err != nil {
		b.Fatalf("BigCache creation failed: %v", err)
	}

	keys := getKeysA()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		_ = cache.Set(b2s(keys[i]), vals[i%numVals])
	}

	runProfileA(b, func(key string, val []byte) {
		_ = cache.Set(key, val)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func BenchmarkProfileA_Otter(b *testing.B) {
	cache, err := otter.MustBuilder[string, []byte](150_000).Build()
	if err != nil {
		b.Fatalf("Otter creation failed: %v", err)
	}

	keys := getKeysA()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		cache.Set(b2s(keys[i]), vals[i%numVals])
	}

	runProfileA(b, func(key string, val []byte) {
		cache.Set(key, val)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func BenchmarkProfileA_Ristretto(b *testing.B) {
	cache, err := ristretto.NewCache(&ristretto.Config{
		NumCounters: 1_500_000, // 10x MaxCost
		MaxCost:     150_000,
		BufferItems: 64,
	})
	if err != nil {
		b.Fatalf("Ristretto creation failed: %v", err)
	}

	keys := getKeysA()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		cache.Set(b2s(keys[i]), vals[i%numVals], 1)
	}
	time.Sleep(100 * time.Millisecond) // wait for async set queues

	runProfileA(b, func(key string, val []byte) {
		cache.Set(key, val, 1)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func BenchmarkProfileA_GoCache(b *testing.B) {
	cache := gocache.New(10*time.Minute, 10*time.Minute)

	keys := getKeysA()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		cache.Set(b2s(keys[i]), vals[i%numVals], gocache.DefaultExpiration)
	}

	runProfileA(b, func(key string, val []byte) {
		cache.Set(key, val, gocache.DefaultExpiration)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func runProfileB(b *testing.B, cacheSetter func(key string, val []byte), cacheGetter func(key string)) {
	keys := getKeysB()
	vals := getVals()
	numKeys := uint64(len(keys))
	numVals := uint64(len(vals))
	windowSize := uint64(1_000_000)

	var opCounter uint64

	b.ReportAllocs()
	b.ResetTimer()

	b.RunParallel(func(pb *testing.PB) {
		seed := uint64(rand.Uint64())
		if seed == 0 {
			seed = 1
		}

		for pb.Next() {
			op := atomic.AddUint64(&opCounter, 1)
			writeIdx := op % numKeys

			if op%2 == 0 {
				// Write
				keyStr := b2s(keys[writeIdx])
				cacheSetter(keyStr, vals[writeIdx%numVals])
			} else {
				// Read
				offset := fastrand(&seed) % windowSize
				readIdx := (writeIdx - offset + numKeys) % numKeys
				keyStr := b2s(keys[readIdx])
				cacheGetter(keyStr)
			}
		}
	})
}

// BenchmarkProfileB_Caeneus benchmarks the zero-allocation hot path under eviction-storm conditions.
func BenchmarkProfileB_Caeneus(b *testing.B) {
	cache, err := caeneus.NewCache(1024, 2048, 2*1024*1024)
	if err != nil {
		b.Fatalf("NewCache failed: %v", err)
	}
	defer cache.Close()

	keys := getKeysB()
	vals := getVals()
	numKeys := uint64(len(keys))
	numVals := uint64(len(vals))
	windowSize := uint64(1_000_000)

	for i := 0; i < int(numKeys); i++ {
		_ = cache.Set(b2s(keys[i]), b2s(vals[i%int(numVals)]))
	}

	var opCounter uint64

	b.ReportAllocs()
	b.ResetTimer()

	b.RunParallel(func(pb *testing.PB) {
		seed := uint64(rand.Uint64())
		if seed == 0 {
			seed = 1
		}
		var buf [valLen + 16]byte

		for pb.Next() {
			op := atomic.AddUint64(&opCounter, 1)
			writeIdx := op % numKeys

			if op%2 == 0 {
				keyStr := b2s(keys[writeIdx])
				_ = cache.Set(keyStr, b2s(vals[writeIdx%numVals]))
			} else {
				offset := fastrand(&seed) % windowSize
				readIdx := (writeIdx - offset + numKeys) % numKeys
				keyStr := b2s(keys[readIdx])
				_, _ = cache.Get(keyStr, buf[:])
			}
		}
	})
}

func BenchmarkProfileB_BigCache(b *testing.B) {
	cfg := bigcache.Config{
		Shards:             1024,
		LifeWindow:         10 * time.Minute,
		CleanWindow:        0,
		MaxEntriesInWindow: 2_000_000,
		MaxEntrySize:       valLen,
		HardMaxCacheSize:   1024, // 1 GB
		Verbose:            false,
	}
	cache, err := bigcache.New(context.Background(), cfg)
	if err != nil {
		b.Fatalf("BigCache creation failed: %v", err)
	}

	keys := getKeysB()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		_ = cache.Set(b2s(keys[i]), vals[i%numVals])
	}

	runProfileB(b, func(key string, val []byte) {
		_ = cache.Set(key, val)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func BenchmarkProfileB_Otter(b *testing.B) {
	cache, err := otter.MustBuilder[string, []byte](2_000_000).Build()
	if err != nil {
		b.Fatalf("Otter creation failed: %v", err)
	}

	keys := getKeysB()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		cache.Set(b2s(keys[i]), vals[i%numVals])
	}

	runProfileB(b, func(key string, val []byte) {
		cache.Set(key, val)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func BenchmarkProfileB_Ristretto(b *testing.B) {
	cache, err := ristretto.NewCache(&ristretto.Config{
		NumCounters: 20_000_000,
		MaxCost:     2_000_000,
		BufferItems: 64,
	})
	if err != nil {
		b.Fatalf("Ristretto creation failed: %v", err)
	}

	keys := getKeysB()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		cache.Set(b2s(keys[i]), vals[i%numVals], 1)
	}
	time.Sleep(100 * time.Millisecond) // wait for async set queues

	runProfileB(b, func(key string, val []byte) {
		cache.Set(key, val, 1)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}

func BenchmarkProfileB_GoCache(b *testing.B) {
	cache := gocache.New(10*time.Minute, 10*time.Minute)

	keys := getKeysB()
	vals := getVals()
	numKeys := len(keys)
	numVals := len(vals)

	for i := 0; i < numKeys; i++ {
		cache.Set(b2s(keys[i]), vals[i%numVals], gocache.DefaultExpiration)
	}

	runProfileB(b, func(key string, val []byte) {
		cache.Set(key, val, gocache.DefaultExpiration)
	}, func(key string) {
		_, _ = cache.Get(key)
	})
}
