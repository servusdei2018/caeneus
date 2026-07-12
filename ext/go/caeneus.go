// Package caeneus provides a client for the Caeneus in-memory cache.
package caeneus

/*
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo linux LDFLAGS: -ldl -lpthread -lm
#cgo darwin LDFLAGS: -lpthread
#include "caeneus.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

// Cache is an in-memory cache client.
//
// A Cache is safe for concurrent use by multiple goroutines.
type Cache struct {
	handle unsafe.Pointer
}

// NewCache creates and initializes a new Cache.
//
// numShards must be a power of two.
// slotsPerShard must be at least 64.
// slabSizePerShard must be at least the system page size.
func NewCache(numShards, slotsPerShard uint32, slabSizePerShard uint64) (*Cache, error) {
	h := C.caeneus_init(
		C.uint(numShards),
		C.uint(slotsPerShard),
		C.size_t(slabSizePerShard),
	)
	if h == nil {
		return nil, fmt.Errorf("caeneus: engine initialisation failed (null handle);" +
			" verify numShards is a power of two, slotsPerShard >= 64, slabSizePerShard >= page size")
	}

	return &Cache{
		handle: unsafe.Pointer(h),
	}, nil
}

// Close closes the cache and releases its resources.
//
// Close must not be called concurrently with other Cache methods.
// After Close, the Cache cannot be used.
func (c *Cache) Close() {
	if c.handle != nil {
		C.caeneus_deinit(c.handle)
		c.handle = nil
	}
}

// Error values returned by Cache operations.
var (
	// ErrCacheMiss indicates that the requested key is not in the cache.
	ErrCacheMiss = errors.New("caeneus: key not found in cache")

	// ErrBufferTooSmall indicates that the destination buffer is too small
	// to hold the value.
	ErrBufferTooSmall = errors.New("caeneus: destination buffer too small")

	// ErrInternalPanic indicates an unexpected failure in the cache engine.
	ErrInternalPanic = errors.New("caeneus: unexpected internal engine panic")
)

// Set stores the value for key in the cache.
//
// It returns ErrInternalPanic if the cache engine fails.
func (c *Cache) Set(key, value string) error {
	var keyPtr, valPtr *C.uchar
	if len(key) > 0 {
		keyPtr = (*C.uchar)(unsafe.Pointer(unsafe.StringData(key)))
	}
	if len(value) > 0 {
		valPtr = (*C.uchar)(unsafe.Pointer(unsafe.StringData(value)))
	}

	rc := C.caeneus_set(
		c.handle,
		keyPtr, C.size_t(len(key)),
		valPtr, C.size_t(len(value)),
	)
	if rc == 0 {
		return nil
	}
	return ErrInternalPanic
}

// SetBytes stores a byte-slice value for a byte-slice key without converting
// either argument to a Go string. The engine does not retain either slice.
func (c *Cache) SetBytes(key, value []byte) error {
	var keyPtr, valPtr *C.uchar
	if len(key) > 0 {
		keyPtr = (*C.uchar)(unsafe.Pointer(unsafe.SliceData(key)))
	}
	if len(value) > 0 {
		valPtr = (*C.uchar)(unsafe.Pointer(unsafe.SliceData(value)))
	}

	rc := C.caeneus_set(
		c.handle,
		keyPtr, C.size_t(len(key)),
		valPtr, C.size_t(len(value)),
	)
	if rc == 0 {
		return nil
	}
	return ErrInternalPanic
}

// Get retrieves the value for key, writing it into dest.
//
// It returns the number of bytes written.
// If dest is too small, Get returns the required size and ErrBufferTooSmall.
// If key is not in the cache, Get returns 0 and ErrCacheMiss.
func (c *Cache) Get(key string, dest []byte) (int, error) {
	var keyPtr *C.uchar
	if len(key) > 0 {
		keyPtr = (*C.uchar)(unsafe.Pointer(unsafe.StringData(key)))
	}
	var bufPtr *C.uchar
	if len(dest) > 0 {
		bufPtr = (*C.uchar)(unsafe.Pointer(unsafe.SliceData(dest)))
	}

	packed := C.caeneus_get(
		c.handle,
		keyPtr, C.size_t(len(key)),
		bufPtr, C.size_t(len(dest)),
	)
	rc := int32(uint32(uint64(packed) >> 32))
	outValLen := int(uint32(uint64(packed)))

	switch rc {
	case 0: // CAENEUS_OK
		return outValLen, nil
	case -1: // CAENEUS_MISS
		return 0, ErrCacheMiss
	case -2: // CAENEUS_ERR_SMALL_BUF
		return outValLen, ErrBufferTooSmall
	default: // CAENEUS_ERR_PANIC
		return 0, ErrInternalPanic
	}
}

// GetBytes retrieves the value for a byte-slice key into dest without
// converting the key to a Go string. The caller owns and reuses dest.
func (c *Cache) GetBytes(key, dest []byte) (int, error) {
	var keyPtr *C.uchar
	if len(key) > 0 {
		keyPtr = (*C.uchar)(unsafe.Pointer(unsafe.SliceData(key)))
	}
	var bufPtr *C.uchar
	if len(dest) > 0 {
		bufPtr = (*C.uchar)(unsafe.Pointer(unsafe.SliceData(dest)))
	}

	packed := C.caeneus_get(
		c.handle,
		keyPtr, C.size_t(len(key)),
		bufPtr, C.size_t(len(dest)),
	)
	rc := int32(uint32(uint64(packed) >> 32))
	outValLen := int(uint32(uint64(packed)))

	switch rc {
	case 0: // CAENEUS_OK
		return outValLen, nil
	case -1: // CAENEUS_MISS
		return 0, ErrCacheMiss
	case -2: // CAENEUS_ERR_SMALL_BUF
		return outValLen, ErrBufferTooSmall
	default: // CAENEUS_ERR_PANIC
		return 0, ErrInternalPanic
	}
}
