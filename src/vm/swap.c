
/*

Managing the swap table

You should handle picking an unused swap slot for evicting a page from its
frame to the swap partition. And handle freeing a swap slot which its page
is read back.

You can use the BLOCK_SWAP block device for swapping, obtaining the struct
block that represents it by calling block_get_role(). Also to attach a swap
disk, please see the documentation.

and to attach a swap disk for a single run, use this option ‘--swap-size=n’

*/
#include "vm/swap.h"
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/interrupt.h"
#include "threads/vaddr.h"
#include <debug.h>
#include <stdio.h>
#include <stdbool.h>
#include "vm/frame.h"
#include "vm/page.h"


// we just provide swap_init() for swap.c
// the rest is your responsibility

/* The swap device. */
static struct block *swap_device;

/* Used swap pages. */
static struct bitmap *swap_bitmap;

/* Protects swap_bitmap. */
static struct lock swap_lock;

/* Number of sectors per page. */
#define PAGE_SECTORS (PGSIZE / BLOCK_SECTOR_SIZE)

void swap_init (void);
bool swap_in (struct page *p);
bool swap_out (struct page *p);

void
swap_init (void)
{
  swap_device = block_get_role (BLOCK_SWAP);
  if (swap_device == NULL)
    {
      printf ("no swap device--swap disabled\n");
      swap_bitmap = bitmap_create (0);
    }
  else
    swap_bitmap = bitmap_create (block_size (swap_device)
                                 / PAGE_SECTORS);
  if (swap_bitmap == NULL)
    PANIC ("couldn't create swap bitmap");
  lock_init (&swap_lock);
}

bool
swap_in (struct page *p)
{

  struct frame *f = p->frame;
  void *base = f->base;
  lock_acquire(&swap_lock);

  //get sector from page and flip the corresponding page sector in bitmap
  block_sector_t block_idx = p->sector;
  bitmap_reset(swap_bitmap, p->sector / PAGE_SECTORS);

  //properly protect and update page, only release lock if not being called in C/S
  bool gained_lock = false;
  if (!lock_held_by_current_thread(&f->lock))
  {
    gained_lock = true;
    frame_lock(f);
  }
  //no longer in swap device
  p->sector = -1;
  //printf("block idx is %d\n", block_idx);

  for (size_t i = 0; i < PAGE_SECTORS; i++)
  {
    block_read(swap_device, block_idx + i, base + (i * BLOCK_SECTOR_SIZE));
  }
  if (gained_lock)
    frame_unlock(f);
  lock_release(&swap_lock);
  return true;
}

bool
swap_out (struct page *p)
{
  struct frame *f = p->frame;
  void *base = f->base;
  lock_acquire(&swap_lock);

  size_t block_idx = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
  if (block_idx == BITMAP_ERROR)
    PANIC("Out of swap space");

  //properly protect and update page, only release lock if not being called in C/S
  bool gained_lock = false;
  if (!lock_held_by_current_thread(&f->lock))
  {
    gained_lock = true;
    frame_lock(f);
  }
  p->sector = (block_sector_t) block_idx * PAGE_SECTORS;


  for (size_t i = 0; i < PAGE_SECTORS; i++)
  {
    block_write(swap_device, p->sector + i, base + (i * BLOCK_SECTOR_SIZE));
  }
  if (gained_lock)
    frame_unlock(f);
  lock_release(&swap_lock);
  return true;
}
