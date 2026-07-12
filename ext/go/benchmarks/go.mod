module github.com/servusdei2018/caeneus/benchmarks

go 1.26

replace github.com/servusdei2018/caeneus => ../

require (
	github.com/allegro/bigcache/v3 v3.1.0
	github.com/dgraph-io/ristretto v0.2.0
	github.com/maypok86/otter v1.2.4
	github.com/patrickmn/go-cache v2.1.0+incompatible
	github.com/servusdei2018/caeneus v0.0.0-20260712092358-69da34953e24
)

require (
	github.com/cespare/xxhash/v2 v2.1.1 // indirect
	github.com/dolthub/maphash v0.1.0 // indirect
	github.com/dustin/go-humanize v1.0.1 // indirect
	github.com/gammazero/deque v0.2.1 // indirect
	github.com/pkg/errors v0.9.1 // indirect
	golang.org/x/sys v0.11.0 // indirect
)
