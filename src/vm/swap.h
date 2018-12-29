#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include "vm/page.h"

void swap_init (void);
bool swap_in (struct page *p);
bool swap_out (struct page *p);

#endif
