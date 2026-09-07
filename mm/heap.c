/*
 * mm/heap.c - Kernel heap (sbrk + first-fit)
 * Author:   amity
 * Date:     Tue Sep  1 23:01:04 2026
 * Copyright © 2026 OwlyNest
 */

/* --- Styling Instructions ---
 * Encoding:                      UTF-8, Unix line endings
 * Text font:                     Monospace
 * Line width:                    Max 80 characters
 * Indentation:                   Use 4 spaces
 * Brace style:                   Same line as control statement
 * Inline comments:               Column 40, wherever possible, else, whole
 * multiple of 20 Section headers:               Use 3 '-' characters before and
 * after Pointer notation:              Next to variable name, not type Binary
 * operations:             Space around operator Empty parameter list: Use
 * (void) instead of () Statements and declarations:   Max one per line
 */

/*
 * Architecture-agnostic. The virtual arena comes from mmap.h
 * (HEAP_VIRT_BASE / HEAP_VIRT_SIZE), so 32-bit and 64-bit kernels
 * get different canonical addresses without this file changing.
 *
 * Frames backing the arena are physically non-contiguous; only the
 * virtual range is contiguous.
 *
 * Locking: malloc/free/realloc hold heap_lock and call sbrk_locked()
 * rather than sbrk(), which would re-acquire the same spinlock.
 */

/* --- Macros ---*/
#define ALIGN16(x) (((x) + 15) & ~(SIZE_T)15)
#define BLOCK_SIZE sizeof(Block)
#define HEAP_SIZE HEAP_VIRT_SIZE
#define HEAP_INITIAL_PAGES 8

/* --- Includes ---*/
#include <arch/x86/interrupts.h>
#include <internal/kscope.h>
#include <internal/kscope_nodes.h>
#include <internal/phonon_consts.h>
#include <internal/phonon_types.h>
#include <lib/string.h>
#include <mm/heap.h>
#include <mm/mmap.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <screen/printk.h>
#include <screen/screen.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>

/* --- Typedefs - Structs - Enums ---*/
typedef struct Block {
  SIZE_T size;
  struct Block *next;
  INT free;
} ALIGN(16) Block;

/* --- Globals ---*/
PUCHAR heap_base;
PUCHAR heap_end;
PUCHAR heap_break;
static Block *head = NULL;
static _SPINLOCK heap_lock;

/* --- Prototypes ---*/
static PVOID sbrk_locked(SSIZE_T increment);

/* --- Functions ---*/

/* Commits [heap_end, target_end) by allocating + mapping individual
 * physical frames. They do NOT need to be contiguous in physical
 * memory — only within this virtual arena. Caller holds heap_lock. */
static INT heap_commit_to(PUCHAR target_end) {
  PUCHAR page = heap_end;

  while (page < target_end) {
    PHYS_ADDR_T frame = pmm_alloc_frame();
    if (!frame) {
      printk("[heap] Out of physical memory while growing heap "
             "(committed %llu KB)\n",
             (unsigned long long)((heap_end - heap_base) / 1024));
      return -1;
    }

    /* NX on the heap: data, not code. Harmless on 32-bit
     * (PAGE_NX is 0 without PAE). */
    if (map_page(frame, (VIRT_ADDR_T)page, PAGE_WRITABLE | PAGE_NX) != 0) {
      printk("[heap] map_page failed while growing heap\n");
      pmm_free_frame(frame);
      return -1;
    }

    page += PAGE_SIZE;
    heap_end = page;
  }

  return 0;
}

static INT heap_init(VOID) {
  ULONG flags;
  int ok;

  heap_base = (PUCHAR)HEAP_VIRT_BASE;
  heap_end = heap_base;
  heap_break = heap_base;

  spinlock_init(&heap_lock);

  flags = spinlock_acquire(&heap_lock);
  ok = heap_commit_to(heap_base + HEAP_INITIAL_PAGES * PAGE_SIZE);
  spinlock_release(&heap_lock, flags);

  if (ok != 0) {
    panic("Heap allocation failed", 0xFFFF, 0xDEAD);
  }

  printk("[heap] Initialized at %p, initial %u pages, "
         "virtual arena %u MB (physically non-contiguous)\n",
         heap_base, HEAP_INITIAL_PAGES, HEAP_SIZE / (1024 * 1024));
  return 0;
}

kscope_node_t heap_node = {
    .name = "heap",
    .id = 0x0008,
    .class = KSCOPE_CLASS_MEMORY,
    .subclass = KSCOPE_SUBCLASS_MEMORY_HEAP,
    .requires = (kscope_node_t *[]){&pmm_node, &paging_node},
    .require_count = 2,
    .provides = (const char *[]){"mem.heap", "mem.kmalloc"},
    .provide_count = 2,
    .init = heap_init,
};

PVOID malloc(SIZE_T size) {
  ULONG flags;
  Block *current;
  PUCHAR next_addr;
  PVOID alloc;

  if (size == 0) {
    return NULL;
  }

  size = ALIGN16(size);

  flags = spinlock_acquire(&heap_lock);

  if (!head) {
    PVOID allocated = sbrk_locked((SSIZE_T)(BLOCK_SIZE + size));
    if (allocated == (PVOID)(LONG_PTR)-1) {
      spinlock_release(&heap_lock, flags);
      return NULL;
    }
    head = (Block *)heap_base;
    head->size = size;
    head->next = NULL;
    head->free = 0;
    spinlock_release(&heap_lock, flags);
    return (PVOID)(head + 1);
  }

  current = head;
  while (current) {
    if (current->free && current->size >= size) {
      SIZE_T leftover = current->size - size;
      if (leftover > BLOCK_SIZE + 8) {
        Block *new_block = (Block *)((PUCHAR)(current + 1) + size);
        new_block->size = leftover - BLOCK_SIZE;
        new_block->free = 1;
        new_block->next = current->next;

        current->size = size;
        current->free = 0;
        current->next = new_block;

        spinlock_release(&heap_lock, flags);
        return (PVOID)(current + 1);
      } else {
        current->free = 0;
        spinlock_release(&heap_lock, flags);
        return (PVOID)(current + 1);
      }
    }

    if (!current->next) {
      break;
    }
    current = current->next;
  }

  next_addr = (PUCHAR)current + BLOCK_SIZE + current->size;
  alloc = sbrk_locked((SSIZE_T)(BLOCK_SIZE + size));
  if (alloc == (PVOID)(LONG_PTR)-1 || alloc != next_addr) {
    spinlock_release(&heap_lock, flags);
    return NULL;
  }

  {
    Block *new_block = (Block *)next_addr;
    new_block->size = size;
    new_block->free = 0;
    new_block->next = NULL;
    current->next = new_block;

    spinlock_release(&heap_lock, flags);
    return (PVOID)(new_block + 1);
  }
}

VOID free(PVOID ptr) {
  ULONG flags;
  Block *block;
  Block *next;
  Block *prev;

  if (!ptr) {
    return;
  }

  if ((VIRT_ADDR_T)ptr & 0xF) {
    return;
  }

  flags = spinlock_acquire(&heap_lock);

  block = (Block *)ptr - 1;
  if ((PUCHAR)block < heap_base || (PUCHAR)block >= heap_end) {
    spinlock_release(&heap_lock, flags);
    return;
  }

  if (block->free) {
    spinlock_release(&heap_lock, flags);
    return;
  }
  block->free = 1;

  next = block->next;
  if (next && next->free) {
    block->size += BLOCK_SIZE + next->size;
    block->next = next->next;
  }

  prev = head;
  while (prev && prev->next != block) {
    prev = prev->next;
  }
  if (prev && prev->free) {
    prev->size += BLOCK_SIZE + block->size;
    prev->next = block->next;
  }

  spinlock_release(&heap_lock, flags);
}

PVOID calloc(SIZE_T num, SIZE_T size) {
  SIZE_T total;
  PVOID ptr;

  if (num == 0 || size == 0) {
    return NULL;
  }
  if (SIZE_MAX / num < size) {
    return NULL;
  }
  total = num * size;
  ptr = malloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

PVOID realloc(PVOID ptr, SIZE_T new_size) {
  ULONG flags;
  Block *block;
  SIZE_T old_size;
  PVOID new_ptr;

  if (!ptr) {
    return malloc(new_size);
  }

  if (new_size == 0) {
    free(ptr);
    return NULL;
  }

  new_size = ALIGN16(new_size);

  flags = spinlock_acquire(&heap_lock);

  block = (Block *)ptr - 1;
  if ((PUCHAR)block < heap_base || (PUCHAR)block >= heap_end) {
    spinlock_release(&heap_lock, flags);
    return NULL;
  }

  if (block->size >= new_size) {
    SIZE_T leftover = block->size - new_size;
    if (leftover > BLOCK_SIZE + 16) {
      Block *new_block = (Block *)((PUCHAR)(block + 1) + new_size);
      new_block->size = leftover - BLOCK_SIZE;
      new_block->free = 1;
      new_block->next = block->next;

      block->size = new_size;
      block->next = new_block;

      if (new_block->next && new_block->next->free) {
        new_block->size += BLOCK_SIZE + new_block->next->size;
        new_block->next = new_block->next->next;
      }
    }
    spinlock_release(&heap_lock, flags);
    return ptr;
  }

  if (block->next && block->next->free) {
    SIZE_T combined = block->size + BLOCK_SIZE + block->next->size;
    if (combined >= new_size) {
      block->size = combined;
      block->next = block->next->next;
      spinlock_release(&heap_lock, flags);
      return ptr;
    }
  }

  old_size = block->size;
  spinlock_release(&heap_lock, flags);

  /* Falls outside the lock: malloc/free below re-acquire on their
   * own, and copying doesn't touch shared heap metadata. */
  new_ptr = malloc(new_size);
  if (!new_ptr)
    return NULL;

  memcpy(new_ptr, ptr, old_size);
  free(ptr);
  return new_ptr;
}

VOID print_heap_state(VOID) {
  Block *current;
  ULONG flags;

  flags = spinlock_acquire(&heap_lock);
  current = head;
  printk("Heap blocks:\n");
  while (current) {
    printk("Block @ %p, size=%llu, free=%s\n", current,
           (unsigned long long)current->size, current->free ? "yes" : "no");
    current = current->next;
  }
  spinlock_release(&heap_lock, flags);
}

static PVOID sbrk_locked(SSIZE_T increment) {
  PUCHAR prev_break = heap_break;
  PUCHAR new_break;

  if (increment == 0) {
    return prev_break;
  }

  new_break = heap_break + increment;

  if (new_break < heap_base) {
    printk("[heap] sbrk: cannot shrink below heap base\n");
    return (PVOID)(LONG_PTR)-1;
  }

  if (new_break > heap_base + HEAP_SIZE) {
    printk("[heap] sbrk: heap arena exhausted (need %p, limit %p)\n", new_break,
           heap_base + HEAP_SIZE);
    return (PVOID)(LONG_PTR)-1;
  }

  if (new_break > heap_end) {
    PUCHAR target = (PUCHAR)(((VIRT_ADDR_T)new_break + PAGE_SIZE - 1) &
                             ~(VIRT_ADDR_T)(PAGE_SIZE - 1));

    printk("[heap] sbrk: growing committed region %p -> %p\n", heap_end,
           target);

    if (heap_commit_to(target) != 0)
      return (PVOID)(LONG_PTR)-1;
  }

  heap_break = new_break;
  return prev_break;
}

PVOID sbrk(SSIZE_T increment) {
  ULONG flags = spinlock_acquire(&heap_lock);
  PVOID r = sbrk_locked(increment);
  spinlock_release(&heap_lock, flags);
  return r;
}