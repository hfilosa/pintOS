#include "vm/page.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include <stdio.h>
#include <stdbool.h>
#include <debug.h>
#include <string.h>

/* removes the page from the threads supplementary page table and frees the page structure */
static void destroy_page (struct hash_elem *p_, void *aux UNUSED)
{
	struct page *p = hash_entry(p_, struct page, hash_elem);
	//get frame mapped to page
	struct frame *f = p->frame;
	if (f != NULL){
	  //set the page mapped to the frame to be NULL if the page has a mapping
	  frame_free(f);
	}
	//free the page stucture
  free(p);
}

/* frees the page table for exiting thread */
void page_exit (void)
{
	struct thread *cur = thread_current();
	//applies the destroy_page function to all elements in thread's hash table
	hash_destroy(&cur->pages, destroy_page);
}

/* takes the user address and returns the page assocaited to it */
static struct page *
page_for_addr (const void *address)
{
	struct page p;
	struct hash_elem *e;
	struct thread *cur = thread_current();

	p.addr = address;
	e = hash_find(&cur->pages, &p.hash_elem);
	return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

/* Load data from swap into frame */
static bool
load_via_swap_device(struct page *p, struct frame *f)
{
	bool worked;
	worked  = install_page(p->addr, f->base, !p->read_only);
	if (!worked) {
		page_deallocate(p);
		return false;
	}
	worked = swap_in(p);
	if (!worked) {
		page_deallocate(p);
		return false;
	}
	return true;
}

/* load data from file into frame */
static bool
load_via_file(struct page *p, struct frame *f)
{
	if (p->file == NULL){
		memset(f->base,0,PGSIZE);
	}
	else{
		lock_acquire(&file_lock);
		file_read_at(p->file, f->base, p->file_bytes, p->file_offset);
		lock_release(&file_lock);
		off_t zero_bytes = PGSIZE - p->file_bytes;
		if (zero_bytes > 0)
			memset(f->base + p->file_bytes, 0, zero_bytes);
	}
	bool worked = install_page(p->addr, f->base, !p->read_only);
	if (!worked) {
		//printf("it failed to install\n");
		page_deallocate(p);
		return false;
	}
	return true;
}

/* assigns a frame to the page and copies in the data from swap or frame*/
static bool
do_page_in (struct page *p)
{
	//printf("in do page in\n");
	struct frame *f = frame_alloc_and_lock(p);
	//printf("got frame\n");
	//printf("p->addr: %0x\n", p->addr);
  //printf("f->base: %0x\n", f->base);

	if (p->sector != (-1)) {
	  if (!load_via_swap_device(p, f)){
		  frame_unlock(f);
		  return false;
		}
	}
	else {
		//printf("loading via file\n");
		if (!load_via_file(p,f)){
			frame_unlock(f);
			return false;
		}
	}
	frame_unlock(f);
	return true;
}

void *
grow_stack(void *addr){
	struct page *p = page_allocate(addr, false);
	struct frame *f = frame_alloc_and_lock(p);
	bool worked = install_page(p->addr, f->base, !p->read_only);
	if (!worked) {
		free(p);
		frame_unlock(f);
		return NULL;
	}
	frame_unlock(f);
	return p;
}

/* determines whether to grow stack or retrieve a frame from memory */
bool page_in (void *fault_addr)
{
	bool worked;
	struct thread *cur = thread_current();
	if (!is_user_vaddr(fault_addr))
		return false;
	void *base = pg_round_down(fault_addr);
	//printf("base %0x\n", base);
	struct page *p = page_for_addr(base);
	if (p != NULL) { //virtually memory has been allocated for this location
		worked = do_page_in(p);
	}
	else if (fault_addr >= cur->user_esp - 32){
		//printf("growing stack\n");
		if (PHYS_BASE - base > STACK_MAX)
			return false; //kill the thread for attempting to grow too large
		//thread stack is not too large so allow it to grow
		if (grow_stack(base) == NULL) return false;
		else worked = true;
	}
	else {
		worked = false;
	}
	//printf("this is the page mapping %0x\n",pagedir_get_page(cur->pagedir, base));
	return worked;
}

/* gives up frame held by page and sets the present bit to empty*/
/* called within frame_lock context */
bool page_out (struct page *p)
{
	struct thread *owner = p->thread;
	bool page_dirty, in_stack, in_mmap;
	//determines if data has been altered since written in
	page_dirty = pagedir_is_dirty(owner->pagedir, p->addr);
	//checks if the page is used for the user's stack as it is the only type that does not have a file attached to it
	//if page has been altered or is for the stack must put it in swap_device or write to file
	//page is from opening a file so it must be written back in order to not crowd swap_device
	if (p->private && page_dirty) {
		lock_acquire(&file_lock);
		file_write_at(p->file, p->frame->base, p->file_bytes, p->file_offset);
		lock_release(&file_lock);
	} else { //then the page must be preserved but was not created via mmap system call
		//if swap device fails to put fram data into swap device return false
		if (!p->private){
		  if (!swap_out(p))
			  PANIC("couldn't write to swap device");
		}
	}
	//set the present bit of page entry to false and reset page and frame
	pagedir_clear_page(owner->pagedir, p->addr);
	frame_free(p->frame);
	p->frame = NULL;
	return true;
}

/* determines if the page p has been accessed recently */
bool page_accessed_recently (struct page *p)
{
	struct thread *owner = p->thread;
	return pagedir_is_accessed(owner->pagedir, p->addr);
}

/* sets up basic information and adds the page to the threads supplementary page table
	 returns NULL if addr already allocated a page*/
struct page * page_allocate (void *vaddr, bool read_only)
{
	struct page *p = (struct page *) malloc(sizeof(struct page));
	struct thread *cur = thread_current();
	p->addr      = vaddr;
	p->read_only = read_only;
	p->thread    = cur;
	p->sector    = -1;   //not in swap device
	p->frame     = NULL; //not mapped to any frame
	p->file      = NULL; //no file is assocaited to it yet
	p->private   = false;
	/* make sure it was added */
	if (hash_insert(&cur->pages, &p->hash_elem) != NULL)
		return NULL;
  return p;
}

/* deallocate the page associated with the addr giving up the associated frame */
void page_deallocate (void *vaddr)
{
	void *upage = pg_round_down(vaddr);
	struct page *p = page_for_addr(upage);
	if (p == NULL)
		return;
	struct frame *f = p->frame;
	struct thread *owner = p->thread;
	//free frame from page if it has a frame
	if (f != NULL){
		//check if dirty and then write it back
    page_out(p);
	}
	//remove page from thread's hash table
	hash_delete(&owner->pages, &p->hash_elem);
	free(p);
}

/* used to find the bucket (idx within the list array) to place the element */
unsigned page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct page *p = hash_entry(e, struct page, hash_elem);
  return hash_bytes(&p->addr, sizeof(p->addr));
}

/* used to find the place to put the item within the bucket */
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED)
{
	const struct page *a = hash_entry(a_, struct page, hash_elem);
	const struct page *b = hash_entry(b_, struct page, hash_elem);

	return a->addr < b->addr;
}

static void
print_page(struct hash_elem *p_, void *aux UNUSED){
	struct page *p = hash_entry(p_, struct page, hash_elem);
	//get frame mapped to page
	printf("uaddr base is %0x\n", p->addr);
}

/* locks the page that contains this addr */
/* ensures the page is in mem for kernel and grows stack if necessary */
/* may kill thread upon invalid procedure */
bool page_lock (void *addr, bool will_write)
{
  if (!is_user_vaddr(addr)) {
  	sys_exit(-1);
  }
  struct thread *cur = thread_current();
  void *base = pg_round_down(addr);
  struct page *p = page_for_addr(base);
  //printf("current pages\n");
  //hash_apply(&thread_current()->pages, print_page);
  //printf("finished printing pages\n");
  bool worked = false;
  if (p == NULL){
    if (addr >= cur->user_esp - 32){
      if (PHYS_BASE - addr > STACK_MAX) {
        sys_exit(-1); //kill the thread for attempting to grow too large
      }
      //thread stack is not too large so allow it to grow
      if ((p = grow_stack(base)) == NULL) {
        sys_exit(-1); //upon failure kill it
      } else {
      	worked = true;
      }
    }
    if (!worked) {
      sys_exit(-1);
    }
  }
  struct frame *f = p->frame;
  if (f == NULL){
  	if (!do_page_in(p)){
  	  sys_exit(-1);
  	  return false;
  	} else {
      //update f to be the newly acquired frame
      f = p->frame;
  	}
  }
  if (lock_held_by_current_thread(&f->lock)) return true;
  lock_acquire(&f->lock);
  return true;
}

/* releases the lock on the page that the address is associated to */
void page_unlock (const void *addr)
{
	struct page *p = page_for_addr(addr);
	struct frame *f = p->frame;
	if (f == NULL)
		return false;
	if (!lock_held_by_current_thread(&f->lock)) return true;
	lock_release(&f->lock);
	return true;
}
