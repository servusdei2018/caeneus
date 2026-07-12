#ifndef CAENEUS_H
#define CAENEUS_H

/*
 * Public C ABI for Caeneus language bindings (Go, Python, Node, etc.).
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAENEUS_OK 0
#define CAENEUS_MISS (-1)
#define CAENEUS_ERR_SMALL_BUF (-2)
#define CAENEUS_ERR_PANIC (-3)

void *caeneus_init(unsigned int num_shards,
                   unsigned int slots_per_shard,
                   size_t slab_size_per_shard);
void caeneus_deinit(void *handle);
int caeneus_set(void *handle,
                const unsigned char *key_ptr,
                size_t key_len,
                const unsigned char *val_ptr,
                size_t val_len);

/*
 * The signed status is stored in the high 32 bits. The value length is stored
 * in the low 32 bits. Status values match CAENEUS_* above.
 */
uint64_t caeneus_get(void *handle,
                     const unsigned char *key_ptr,
                     size_t key_len,
                     unsigned char *buf_ptr,
                     size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif
