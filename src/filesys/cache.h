#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include <stdbool.h>

struct cache_block
  {
    block_sector_t sector;
    bool used;
    bool dirty;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };


void cache_init (void);
void cache_write (struct cache_block *);
struct cache_block *cache_read (block_sector_t);
void *cache_zero (struct cache_block *);
void cache_dirty (struct cache_block *);
void cache_free (block_sector_t);

#endif /* filesys/cache.h */
