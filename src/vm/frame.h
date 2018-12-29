#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/synch.h"

/* A physical frame. */
struct frame
{
  struct lock lock;           /* Prevent simultaneous access. */
  void *base;                 /* Kernel virtual base address. */
  struct page *page;          /* Mapped process page, if any. */
};

void frame_init (void);
struct frame * frame_alloc_and_lock (struct page *p);
void frame_free (struct frame *f);
void frame_lock (struct frame *f);
void frame_unlock (struct frame *f);

#endif /* vm/frame.h */
