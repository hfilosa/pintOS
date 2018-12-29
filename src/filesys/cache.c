#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "filesys/filesys.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define INVALID_SECTOR ((block_sector_t) -1)

/* Cache. */
#define CACHE_CNT 64
struct cache_block cache[CACHE_CNT];
struct lock cache_sync;
static int hand = 0;


static void flushd_init (void);
static void readaheadd_init (void);
static void readaheadd_submit (block_sector_t sector);


static void
set_block(struct cache_block *b, block_sector_t sector)
{
  b->sector = sector;
  b->dirty = false;
}

/* Initializes cache. */
void
cache_init (void)
{
  for (int i = 0; i < CACHE_CNT; i++){
    cache[i].sector = -1;
    cache[i].used = false;
    cache[i].dirty = false;
  }
}

/* Flush cache to disk. */
void
cache_flush ()
{
  struct cache_block *b;
  for (int i = 0; i < CACHE_CNT; i++){
    b = &cache[i];
    if (b->dirty)
      block_write(fs_device, b->sector, b->data);
    }
}

/* write cache b to disk if dirty. */
void
cache_write (struct cache_block * b)
{
  //if (b->dirty)
    block_write(fs_device, b->sector, b->data);
}

/* On success returns the block in the cache that points to the desired sector
   on failure returns NULL */
static struct cache_block *
get_block_in_cache (block_sector_t sector)
{
  struct cache_block *b;
  for (int i = 0; i < CACHE_CNT; i++){
    b = &cache[i];
    if (b->sector == sector)
      return b;
  }
  return NULL;
}

/* On success returns a cache block set with the desired sector */
/* on failure returns NULL indicating that no block is free */
static struct cache_block *
get_free_block (block_sector_t sector)
{
  struct cache_block *b;
  for (int i = 0; i < CACHE_CNT; i++){
    b = &cache[i];
    //check to see if occupied by some sector
    if ((unsigned int)b->sector == -1){
      //then this block is free so reset its info and set its sector after locking it
      b->sector = sector;
      return b;
    }
  }
  return NULL;
}

static void
inc_hand(void){
  hand++;
  if (hand == CACHE_CNT)
    hand = 0;
}

/* Bring sector into memory and return pointer to cache_block. Data field
 can now be modified and writeback handled by eviction*/
struct cache_block *
cache_read (block_sector_t sector)
{
  //printf("in cache read\n");
  struct cache_block *b;

  /* Is the block already in-cache? */
  if ((b = get_block_in_cache(sector)) != NULL){
    goto done;
  }

  /* Not in cache.  Find empty slot. */
  if ((b = get_free_block(sector)) != NULL){
    block_read (fs_device, sector, b->data);
    b->dirty = false;
    goto done;
  }

  /* No empty slots.  Evict something. */
  while(1){
    b = &cache[hand];
    if (b->used == false){
      cache_write(b);
      block_read (fs_device, sector, b->data);
      b->sector = sector;
      b->dirty = false;
      goto done;
    }
    b->used = false;
    inc_hand();
  }

  done:
    b->used = true;
    return b;
}

/* Zero out block B, without reading it from disk, and return a
   pointer to the zeroed data.*/
void *
cache_zero (struct cache_block *b)
{
  memset (b->data, 0, BLOCK_SECTOR_SIZE);
  return b->data;
}

/* Marks block B as dirty, so that it will be written back to
   disk before eviction.
   B must be up-to-date. */
void
cache_dirty (struct cache_block *b)
{
  b->dirty = true;
}

/* If SECTOR is in the cache, evicts it immediately without
   writing it back to disk (even if dirty).
   The block must be entirely unused. */
void
cache_free (block_sector_t sector)
{
  struct cache_block *b = get_block_in_cache(sector);
  if (b != NULL)
    b->sector = -1;
}
