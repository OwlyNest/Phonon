/*
 * fs/smkfs/mrt.c - Master Record Table (G1)
 * Author:   amity
 * Date:     Thu Jul 30 13:18:15 2026
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

/* --- Macros ---*/

/* --- Includes ---*/
#include "internal/phonon_types.h"
#include <fs/smkfs.h>
#include <fs/smkfs_internal.h>
#include <internal/phonon_macros.h>
#include <mm/heap.h>
#include <screen/printk.h>
#include <stdint.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/
inline LONG power_of_two(LONG n) { return (n > 0) && ((n & (n - 1)) == 0); }

SMKFS_STATUS mrt_format(_SMKFS_MOUNT *mnt, SMKFS_BLOCK start_block,
                        ULONGLONG length) {

  /* 0 length MRT is invalid*/
  ASSERT(length != 0);

  /* MRT entry size must be power of two for capacity to be calculated nicely */
  /* SMKFS_BLOCK_SIZE = 4096 = 2^12, so only powers of two are divisors*/
  ASSERT(power_of_two(sizeof(_SMKFS_MRT_ENTRY)) != 0);

  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }
  ULONG entries_per_block = SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY);
  _SMKFS_MRT_ENTRY *entries = (_SMKFS_MRT_ENTRY *)block;

  /* Write j entries to the i'th block */
  for (ULONGLONG i = 0; i < length; i++) {
    for (ULONG j = 0; j < entries_per_block; j++) {
      entries[j].physical_block = UINT64_MAX;
      entries[j].flags = 0;
      entries[j].reserved = 0;
      entries[j].generation = 0;
    }

    if (write_block(mnt, start_block + i, block) != SMKFS_OK) {
      free(block);
      return SMKFS_ERR_IO;
    }
  }

  free(block);
  return SMKFS_OK;
}

SMKFS_STATUS mrt_init(_SMKFS_MOUNT *mnt, SMKFS_BLOCK start_block,
                      ULONGLONG length) {

  /* 0 length MRT is invalid*/
  ASSERT(length != 0);

  /* MRT entry size must be power of two for capacity to be calculated nicely */
  /* SMKFS_BLOCK_SIZE = 4096 = 2^12, so only powers of two are divisors*/
  ASSERT(power_of_two(sizeof(_SMKFS_MRT_ENTRY)) != 0);

  mnt->sb.mrt_start = start_block;
  mnt->sb.mrt_length = length;
  mnt->sb.mrt_capacity = length * (SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY));
  return SMKFS_OK;
}

SMKFS_STATUS mrt_alloc_entry(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID *out_record_id,
                             SMKFS_GENERATION *out_generation) {

  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }

  /* No check necessary, if it would fail, the mrt couldn't be initialized
   * or kernel code is being modified while running.
   * This would mean a bug or attack, best to check anyway.
   */
  ASSERT(power_of_two(sizeof(_SMKFS_MRT_ENTRY)) != 0);

  ULONG entries_per_block = SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY);

  for (ULONGLONG i = 0; i < mnt->sb.mrt_length; i++) {
    if (read_block(mnt, mnt->sb.mrt_start + i, block) != SMKFS_OK) {
      free(block);
      return SMKFS_ERR_IO;
    }

    _SMKFS_MRT_ENTRY *entries = (_SMKFS_MRT_ENTRY *)block;
    for (ULONG j = 0; j < entries_per_block; j++) {

      /* MEOW flashback */
      SMKFS_RECORD_ID candidate = i * entries_per_block + j;
      if (candidate == 0) {
        continue; /* reserved for Superblock */
      }
      if (entries[j].flags & SMKFS_MRTF_ALLOCATED) {
        continue; /* Already in use */
      }

      _SMKFS_MRT_ENTRY old = entries[j];

      entries[j].flags |= SMKFS_MRTF_ALLOCATED;
      entries[j].physical_block = UINT64_MAX;
      entries[j].generation++;

      if (write_block(mnt, mnt->sb.mrt_start + i, block) != SMKFS_OK) {
        free(block);
        return SMKFS_ERR_IO;
      }

      if (mnt->journal_in_transaction) {
        if (journal_log_mrt_update(mnt, candidate, &entries[j]) != SMKFS_OK) {
          entries[j] = old;
          write_block(mnt, mnt->sb.mrt_start + i, block);
          free(block);
          return SMKFS_ERR_JOURNAL;
        }
      }

      mnt->sb.mrt_free_count--;
      *out_record_id = candidate;
      *out_generation = entries[j].generation;
      free(block);
      return SMKFS_OK;
    }
  }
  free(block);
  return SMKFS_ERR_NOSPC;
}

SMKFS_STATUS mrt_update_entry(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                              SMKFS_BLOCK new_physical_block,
                              SMKFS_MRT_FLAGS flags) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }

  ULONG entries_per_block;
  SMKFS_BLOCK block_id;
  ULONG entry_id;
  _SMKFS_MRT_ENTRY old;

  /* I'm not gonna reason this one again */
  ASSERT(power_of_two(sizeof(_SMKFS_MRT_ENTRY)) != 0);

  if (record_id >= mnt->sb.mrt_capacity) {
    free(block);
    return SMKFS_ERR_INVAL;
  }

  entries_per_block = SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY);
  block_id = mnt->sb.mrt_start + record_id / entries_per_block;
  entry_id = record_id % entries_per_block;

  if (read_block(mnt, block_id, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  _SMKFS_MRT_ENTRY *entries = (_SMKFS_MRT_ENTRY *)block;
  old = entries[entry_id];

  entries[entry_id].physical_block = new_physical_block;
  entries[entry_id].flags |= flags;

  if (write_block(mnt, block_id, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  if (mnt->journal_in_transaction) {
    if (journal_log_mrt_update(mnt, record_id, &entries[entry_id]) !=
        SMKFS_OK) {
      entries[entry_id] = old;
      write_block(mnt, block_id, block);
      free(block);
      return SMKFS_ERR_JOURNAL;
    }
  }
  free(block);
  return SMKFS_OK;
}

SMKFS_STATUS mrt_free_entry(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id) {

  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }

  ULONG entries_per_block;
  SMKFS_BLOCK block_id;
  ULONG entry_id;
  _SMKFS_MRT_ENTRY old;

  /* I'm not gonna reason this one again */
  ASSERT(power_of_two(sizeof(_SMKFS_MRT_ENTRY)) != 0);

  if (record_id >= mnt->sb.mrt_capacity) {
    free(block);
    return SMKFS_ERR_INVAL;
  }

  entries_per_block = SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY);
  block_id = mnt->sb.mrt_start + record_id / entries_per_block;
  entry_id = record_id % entries_per_block;

  if (read_block(mnt, block_id, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  _SMKFS_MRT_ENTRY *entries = (_SMKFS_MRT_ENTRY *)block;
  old = entries[entry_id];

  entries[entry_id].physical_block = UINT64_MAX; /* 64 ZiB, safe for now */
  entries[entry_id].flags = 0; /* Zero flags, also means unallocated */
  /*
   * alloc_entry increases generation when record is allocated, no need to do it
   * here. entry has to be allocated again for reuse.
   */

  if (write_block(mnt, block_id, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  if (mnt->journal_in_transaction) {
    if (journal_log_mrt_update(mnt, record_id, &entries[entry_id]) !=
        SMKFS_OK) {
      entries[entry_id] = old;
      write_block(mnt, block_id, block);
      free(block);
      return SMKFS_ERR_JOURNAL;
    }
  }

  mnt->sb.mrt_free_count++;
  free(block);
  return SMKFS_OK;
}

SMKFS_STATUS mrt_resolve(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                         SMKFS_BLOCK *out_physical_block,
                         SMKFS_MRT_FLAGS *out_flags,
                         SMKFS_GENERATION *out_generation) {

  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }

  /* I'm not gonna reason this one again */
  ASSERT(power_of_two(sizeof(_SMKFS_MRT_ENTRY)) != 0);

  if (record_id >= mnt->sb.mrt_capacity) {
    free(block);
    return SMKFS_ERR_INVAL;
  }

  ULONG entries_per_block = SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY);

  SMKFS_BLOCK block_id = mnt->sb.mrt_start + record_id / entries_per_block;
  if (read_block(mnt, block_id, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  _SMKFS_MRT_ENTRY *entries = (_SMKFS_MRT_ENTRY *)block;
  ULONG entry_id = record_id % entries_per_block;
  _SMKFS_MRT_ENTRY entry = entries[entry_id];

  if (!(entry.flags & SMKFS_MRTF_ALLOCATED)) {
    free(block);
    return SMKFS_ERR_NOTFOUND;
  }

  if (entry.physical_block == 0 || entry.physical_block == UINT64_MAX) {
    free(block);
    return SMKFS_ERR_NOT_YET_BOUND;
  }

  if (out_physical_block) {
    *out_physical_block = entry.physical_block;
  }

  if (out_flags) {
    *out_flags = entry.flags;
  }

  if (out_generation) {
    *out_generation = entry.generation;
  }

  free(block);
  return SMKFS_OK;
}