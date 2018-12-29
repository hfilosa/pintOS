#include "vm/frame.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
/*
Managing the frame table

The main job is to obtain a free frame to map a page to. To do so:

1. Easy situation is there is a free frame in frame table and it can be
obtained. If there is no free frame, you need to choose a frame to evict
using your page replacement algorithm based on setting accessed and dirty
bits for each page. See section 4.1.5.1 and A.7.3 to know details of
replacement algorithm(accessed and dirty bits) If no frame can be evicted
without allocating a swap slot and swap is full, you should panic the
kernel.

2. remove references from any page table that refers to.

3.write the page to file system or swap.

*/




// we just provide frame_init() for swap.c
// the rest is your responsibility


static struct frame *frames;
static size_t frame_cnt;

static struct lock scan_lock;
static size_t hand;


void
frame_init (void)
{
  void *base;

  lock_init (&scan_lock);

  frames = malloc (sizeof *frames * init_ram_pages);
  if (frames == NULL)
    PANIC ("out of memory allocating page frames");

  while ((base = palloc_get_page (PAL_USER)) != NULL)
    {
      struct frame *f = &frames[frame_cnt++];
      lock_init (&f->lock);
      f->base = base;
      f->page = NULL;
    }
  hand = 0;
}

static void
inc_hand(void)
{
  hand++;
  if (hand >= frame_cnt) hand = 0;
}

/* attempts to acquire a free frame and lock it, returns null if all frames are taken */
static struct frame *
try_frame_alloc_and_lock (struct page *p)
{
  struct frame *f;
  for (size_t i = 0; i < frame_cnt; i++)
  {
    f = &frames[i];
    if (f->page == NULL)
    {
      //free page since no page is mapped to it lock the frame and return it
      frame_lock(f);
      p->frame = f;
    	f->page = p;
      return f;
    }
  }
  return NULL;
}

static struct frame *
evict(void)
{
  struct frame *f;
  struct page *p;
  struct thread *owner;
  while(true)
  {
    f = &frames[hand];
    frame_lock(f);
    //p will not be null since we just checked all values in frames
    p = f->page;
    if (p == NULL){
      inc_hand();
      return f;
    }
    //printf("check if it was accessed\n");
    if (page_accessed_recently(p))
    {
      //printf("you are still wanted\n");
      //page has been recently used so set it to cold
      owner = p->thread;
      pagedir_set_accessed(owner->pagedir, p->addr, false);
      //printf("i hav eset ytou to no longer wanr ro be\n");
    }
    else //the page has not been recently accessed and can be kicked out
    {
      if (!page_out(p)) //remove the page and make sure it was removed
        return NULL; //couldn't write to file
      inc_hand();
      return f; //return the freed frame
    }
    //page was recently accessed so couldn't be kicked out
    frame_unlock(f);
    inc_hand();
  }
}

/* acquires a free frame that is locked and then returns it */
/* caller must release the frame lock when ready */
struct frame *
frame_alloc_and_lock (struct page *page)
{
  lock_acquire(&scan_lock);
  //printf("getiing a frame\n");
  struct frame *f = try_frame_alloc_and_lock(page);
  if (f != NULL) //found free frame
  {
    //printf("got a frame on first loop\n");
    lock_release(&scan_lock);
    return f;
  }
  //couldn't find a free frame so must evict someone
  f = evict();
  page->frame = f;
  f->page = page;
  //printf("eviciterd\n");
  if (lock_held_by_current_thread(&scan_lock))
    lock_release(&scan_lock);
  return f;
}

/* acquire lock of frame within page */
void
frame_lock (struct frame *f)
{
  if (!lock_held_by_current_thread(&f->lock))
    lock_acquire(&f->lock);
}

/* free the page that the frame is mapped to in memory */
void
frame_free (struct frame *f)
{
  if (!lock_held_by_current_thread(&f->lock))
    lock_acquire(&f->lock);
  if (!lock_held_by_current_thread(&scan_lock)){
    lock_acquire(&scan_lock);
  }

  f->page = NULL;

  lock_release(&scan_lock);
  lock_release(&f->lock);
}

/* free lock within frame f */
void
frame_unlock (struct frame *f)
{
  if (lock_held_by_current_thread(&f->lock))
    lock_release(&f->lock);
}
