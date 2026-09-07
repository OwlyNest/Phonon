/*
 * fs/smkfs/debug.c - Debug and Diagnostic Output (G1)
 * Author:   amity
 * Date:     Wed Jul 29 17:39:11 2026
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
#include <mm/heap.h>
#include <screen/printk.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/
static VOID printk_size(PCCHAR label, QWORD bytes) {
  static PCCHAR units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};

  QWORD whole = bytes;
  QWORD remainder = 0;
  ULONG unit = 0;

  while (whole >= 1024 && unit < 5) {
    remainder = whole % 1024;
    whole /= 1024;
    unit++;
  }

  if (unit == 0) {
    printk("%-16s %llu %s\n", label, whole, units[unit]);
    return;
  }

  /*
   * One decimal place, rounded rather than truncated.
   *
   * remainder / 1024 gives the fractional part.
   * Multiply by 10 before dividing to get one decimal digit.
   */
  QWORD decimal = (remainder * 10 + 512) / 1024;

  /*
   * Rounding can turn e.g. 1023.96 MiB into 1024.0 MiB.
   * Promote to the next unit in that case.
   */
  if (decimal == 10) {
    whole++;
    decimal = 0;

    if (whole >= 1024 && unit < 5) {
      whole /= 1024;
      unit++;
    }
  }

  printk("%-16s %llu.%llu %s, (%lluB)\n", label, whole, decimal, units[unit],
         bytes);
}

SMKFS_STATUS smkfs_dump_superblock(_SMKFS_MOUNT *mnt) {
  if (!mnt->mounted) {
    printk("[SmKFS] Not mounted\n");
    return SMKFS_ERR_INVAL;
  }

  _SMKFS_SUPERBLOCK sb = mnt->sb; // Not gonna rewrite all that

  printk("\n=== SmKFS Superblock (G1) ===\n");
  printk("Magic:           %.4s\n", sb.header.magic);
  printk("Version:         %u\n", sb.header.version);
  printk("Type:            0x%04X\n", sb.header.type);
  printk("Length:          %u\n", sb.header.length);
  printk("Flags:           0x%08X\n", sb.header.flags);
  printk("Checksum:        0x%08X\n", sb.header.checksum);
  printk("Total blocks:    %llu\n", sb.total_blocks);
  printk_size("Total Size:", sb.total_blocks * SMKFS_BLOCK_SIZE);
  printk("Free blocks:     %llu\n", sb.free_blocks);
  printk_size("Free Size:", sb.free_blocks * SMKFS_BLOCK_SIZE);
  printk("Sector size      %llu\n", sb.sector_size);
  printk("Records:         %llu\n", sb.record_count);
  printk("Next ID:         %llu\n", sb.next_record_id);
  printk("Root record:     %llu\n", sb.root_record_id);
  printk("Journal:         %llu + %llu (head %llu, tail %llu, seq %llu)\n",
         sb.journal_start, sb.journal_length, sb.journal_head, sb.journal_tail,
         sb.journal_sequence);
  printk("Bitmap:          %llu + %llu\n", sb.bitmap_start, sb.bitmap_length);
  printk("MRT:             %llu + %llu (cap %llu, free %llu)\n", sb.mrt_start,
         sb.mrt_length, sb.mrt_capacity, sb.mrt_free_count);
  printk("Data start:      %llu\n", sb.data_start);
  printk("Block size:      %u\n", sb.block_size);
  printk("SB flags:        0x%08X\n", sb.flags);
  printk("Mount count:     %u / %u\n", sb.mount_count, sb.max_mount_count);
  printk("Volume:          %.64s\n", sb.volume_name);
  printk("===============================\n\n");
  return SMKFS_OK;
}

SMKFS_STATUS smkfs_dump_record(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD *rec;
  PCUCHAR ptr;

  if (!mnt->mounted) {
    free(block);
    return SMKFS_ERR_INVAL;
  }

  SMKFS_BLOCK phys_block;
  SMKFS_STATUS mrt_ret = mrt_resolve(mnt, record_id, &phys_block, NULL, NULL);
  if (mrt_ret != SMKFS_OK) {
    free(block);
    return mrt_ret;
  }

  if (read_block(mnt, phys_block, block) != 0) {
    printk("[SmKFS] Cannot read record %llu, (phys: %llu)\n", record_id,
           phys_block);
    free(block);
    return SMKFS_ERR_IO;
  }

  rec = (_SMKFS_RECORD *)block;
  if (header_validate(&rec->header, SMKFS_ST_RECORD) != 0) {
    printk("[SmKFS] Record %llu (phys: %llu): invalid header\n", record_id,
           phys_block);
    free(block);
    return SMKFS_ERR_CORRUPT;
  }

  printk("\n=== Record %llu ===\n", record_id);
  printk("Magic:      %.4s\n", rec->header.magic);
  printk("Version:    %u\n", rec->header.version);
  printk("Type:       0x%04X\n", rec->header.type);
  printk("Length:     %u\n", rec->header.length);
  printk("Flags:      0x%08X\n", rec->header.flags);
  printk("Checksum:   0x%08X\n", rec->header.checksum);
  printk("Record ID:  %llu\n", rec->record_id);
  printk("Obj Type:   %u\n", rec->object_type);
  printk("Attr Count: %u\n", rec->attr_count);
  printk("Link Count: %u\n", rec->link_count);
  printk("Generation: %u\n", rec->generation);

  ptr = block + sizeof(_SMKFS_RECORD);
  while (1) {
    _SMKFS_ATTR_HEADER *ah = (_SMKFS_ATTR_HEADER *)ptr;
    if (ah->type == SMKFS_ATTRT_END) {
      printk("  [END]\n");
      break;
    }

    printk("  Attr %s (0x%04X, id=%u), len %u: ", smkfs_attr_name(ah->type),
           ah->type, ah->id, ah->length);
    smkfs_attr_debug_print(ah->type, ptr + sizeof(_SMKFS_ATTR_HEADER),
                           ah->length);
    printk("\n");
    ptr += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
  }

  printk("==================\n\n");
  free(block);
  return SMKFS_OK;
}

SMKFS_STATUS smkfs_dump_journal(_SMKFS_MOUNT *mnt) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_JOURNAL_ENTRY *ent;

  if (!mnt->mounted) {
    printk("[SmKFS] Not mounted\n");
    free(block);
    return SMKFS_ERR_INVAL;
  }

  printk("\n=== Journal ===\n");
  for (ULONGLONG i = 0; i < mnt->sb.journal_length; i++) {
    if (read_block(mnt, mnt->sb.journal_start + i, block) != 0)
      continue;

    ent = (_SMKFS_JOURNAL_ENTRY *)block;
    if (header_validate(&ent->header, SMKFS_ST_JOURNAL_ENT) != 0) {
      continue;
    }

    printk("Entry %llu: seq=%llu, op=%u, block=%llu, len=%u, rec=%llu\n", i,
           ent->sequence, ent->operation, ent->target_block, ent->data_length,
           ent->record_id);

    /*
     * Sinatra and C should be mandatory!
     */

    if (ent->operation == SMKFS_JOP_COMMIT) {
      printk("  [COMMIT]\n");
    } else if (ent->operation == SMKFS_JOP_WRITE) {
      printk("  [WRITE %llu bytes]\n", ent->data_length);
    } else if (ent->operation == SMKFS_JOP_ALLOC) {
      printk("  [ALLOC %u blocks]\n", ent->data_length);
    } else if (ent->operation == SMKFS_JOP_FREE) {
      printk("  [FREE %u blocks]\n", ent->data_length);
    } else if (ent->operation == SMKFS_JOP_MRT_UPDATE) {
      printk("  [MRT UPDATE]\n");
    } else if (ent->operation == SMKFS_JOP_CHECKPOINT) {
      printk("  [CHECKPOINT]\n");
    }
  }

  printk("===============\n\n");
  free(block);
  return SMKFS_OK;
}

SMKFS_STATUS smkfs_dump_btree(_SMKFS_MOUNT *mnt, SMKFS_BLOCK root_block) {
  if (!mnt->mounted) {
    printk("[SmKFS] Not mounted\n");
    return SMKFS_ERR_INVAL;
  }

  printk("\n=== B+ Tree ===\n");
  btree_dump_recursive(mnt, root_block, 0);
  printk("===============\n\n");
  return SMKFS_OK;
}

PCCHAR smkfs_strerror(SMKFS_STATUS err) {
  switch (err) {
  case SMKFS_OK:
    return "OK";
  case SMKFS_ERR_IO:
    return "I/O error";
  case SMKFS_ERR_NOMEM:
    return "out of memory";
  case SMKFS_ERR_NOTFOUND:
    return "not found";
  case SMKFS_ERR_EXISTS:
    return "already exists";
  case SMKFS_ERR_NOSPC:
    return "no space left";
  case SMKFS_ERR_INVAL:
    return "invalid argument";
  case SMKFS_ERR_CORRUPT:
    return "filesystem corrupt";
  case SMKFS_ERR_NOTEMPTY:
    return "directory not empty";
  case SMKFS_ERR_ROFS:
    return "read-only filesystem";
  case SMKFS_ERR_JOURNAL:
    return "journal error";
  case SMKFS_ERR_TOO_BIG:
    return "too big";
  case SMKFS_ERR_NOT_YET_BOUND:
    return "MRT entry not yet bound";
  default:
    return "unknown error";
  }
}

VOID smkfs_fserror(SMKFS_STATUS err) {
  printk("[SmKFS] File System Error: %s (%d)\n", smkfs_strerror(err), err);
}