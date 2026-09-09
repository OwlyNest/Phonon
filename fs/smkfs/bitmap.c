/*
 * fs/smkfs/bitmap.c - Regioned Bitmap Allocation (G1)
 * Author:   amity
 * Date:     Wed Jul 29 17:38:13 2026
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
static ULONG bitmap_region_size(_SMKFS_MOUNT *mnt, ULONG region);
static SMKFS_BLOCK bitmap_alloc_range_linear(_SMKFS_MOUNT *mnt, ULONG count);
static SMKFS_BLOCK bitmap_alloc_in_region(_SMKFS_MOUNT *mnt, ULONG region,
                                          ULONG count);

/* --- Functions ---*/

/* --- Region Cache Management --- */

static inline ULONG bitmap_region_size(_SMKFS_MOUNT *mnt, ULONG region) {
  ULONGLONG data_blocks = mnt->sb.total_blocks - mnt->sb.data_start;
  ULONGLONG start = (ULONGLONG)region * SMKFS_REGION_BLOCKS;
  ULONGLONG end = start + SMKFS_REGION_BLOCKS;
  if (end > data_blocks)
    end = data_blocks;
  return (end > start) ? (ULONG)(end - start) : 0;
}

SMKFS_STATUS bitmap_init_regions(_SMKFS_MOUNT *mnt) {
  PUCHAR buf;
  ULONGLONG data_blocks;
  ULONG i;

  if (!mnt)
    return SMKFS_ERR_INVAL;
  if (mnt->regions)
    return SMKFS_ERR_INVAL;

  data_blocks = mnt->sb.total_blocks - mnt->sb.data_start;
  if (data_blocks == 0) {
    mnt->region_count = 0;
    mnt->regions = NULL;
    mnt->alloc_hint_region = 0;
    return SMKFS_OK;
  }

  mnt->region_count =
      (ULONG)((data_blocks + SMKFS_REGION_BLOCKS - 1) / SMKFS_REGION_BLOCKS);
  mnt->regions =
      (_SMKFS_REGION *)malloc(sizeof(_SMKFS_REGION) * mnt->region_count);
  if (!mnt->regions)
    return SMKFS_ERR_NOMEM;

  for (i = 0; i < mnt->region_count; i++) {
    mnt->regions[i].free_count = bitmap_region_size(mnt, i);
    mnt->regions[i].alloc_hint = 0;
  }

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    free(mnt->regions);
    mnt->regions = NULL;
    return SMKFS_ERR_NOMEM;
  }

  for (ULONGLONG bb = 0; bb < mnt->sb.bitmap_length; bb++) {
    if (read_block(mnt, mnt->sb.bitmap_start + bb, buf) != SMKFS_OK) {
      free(buf);
      free(mnt->regions);
      mnt->regions = NULL;
      return SMKFS_ERR_IO;
    }

    for (LONG bo = 0; bo < SMKFS_BLOCK_SIZE; bo++) {
      ULONGLONG byte_idx = bb * SMKFS_BLOCK_SIZE + bo;
      ULONGLONG bit_base = byte_idx * 8;
      UCHAR byte = buf[bo];

      if (byte == 0x00)
        continue;
      if (bit_base >= data_blocks)
        break;

      if (byte == 0xFF) {
        ULONG bits_in_byte = 8;
        if (bit_base + 8 > data_blocks) {
          bits_in_byte = (ULONG)(data_blocks - bit_base);
        }

        for (ULONG bi = 0; bi < bits_in_byte; bi++) {
          ULONGLONG bit = bit_base + bi;
          ULONG region = (ULONG)(bit / SMKFS_REGION_BLOCKS);
          if (mnt->regions[region].free_count > 0) {
            mnt->regions[region].free_count--;
          }
        }

        continue;
      }

      for (LONG bi = 0; bi < 8; bi++) {
        ULONGLONG bit = bit_base + bi;
        if (bit >= data_blocks)
          break;
        if ((byte >> bi) & 1) {
          ULONG region = (ULONG)(bit / SMKFS_REGION_BLOCKS);
          if (mnt->regions[region].free_count > 0)
            mnt->regions[region].free_count--;
        }
      }
    }
  }

  mnt->alloc_hint_region = 0;
  free(buf);
  return SMKFS_OK;
}

VOID bitmap_shutdown(_SMKFS_MOUNT *mnt) {
  if (mnt && mnt->regions) {
    free(mnt->regions);
    mnt->regions = NULL;
  }
  mnt->region_count = 0;
  mnt->alloc_hint_region = 0;
}

/* --- Bitmap Primitives --- */

SMKFS_STATUS bitmap_set(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block) {
  PUCHAR buf;
  ULONGLONG idx;
  SMKFS_OFFSET byte_offset, bit_offset, block_offset;
  SMKFS_BLOCK bitmap_block;

  if (block < mnt->sb.data_start)
    return SMKFS_ERR_CORRUPT;
  idx = block - mnt->sb.data_start;
  byte_offset = idx / 8;
  bit_offset = idx % 8;
  bitmap_block = mnt->sb.bitmap_start + (byte_offset / SMKFS_BLOCK_SIZE);
  block_offset = byte_offset % SMKFS_BLOCK_SIZE;

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    return SMKFS_ERR_NOMEM;
  }
  if (read_block(mnt, bitmap_block, buf) != SMKFS_OK) {
    free(buf);
    return SMKFS_ERR_IO;
  }

  if (((buf[block_offset] >> bit_offset) & 1) == 0) {
    mnt->sb.free_blocks--;
    if (mnt->regions) {
      ULONG region = (ULONG)(idx / SMKFS_REGION_BLOCKS);
      if (region < mnt->region_count && mnt->regions[region].free_count > 0)
        mnt->regions[region].free_count--;
    }
  }

  buf[block_offset] |= (1 << bit_offset);
  if (write_block(mnt, bitmap_block, buf) != SMKFS_OK) {
    free(buf);
    return SMKFS_ERR_IO;
  }
  free(buf);
  return SMKFS_OK;
}

SMKFS_STATUS bitmap_clear(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block) {
  PUCHAR buf;
  ULONGLONG idx;
  SMKFS_OFFSET byte_offset, bit_offset, block_offset;
  SMKFS_BLOCK bitmap_block;

  if (block < mnt->sb.data_start)
    return SMKFS_ERR_CORRUPT;
  idx = block - mnt->sb.data_start;
  byte_offset = idx / 8;
  bit_offset = idx % 8;
  bitmap_block = mnt->sb.bitmap_start + (byte_offset / SMKFS_BLOCK_SIZE);
  block_offset = byte_offset % SMKFS_BLOCK_SIZE;

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    return SMKFS_ERR_NOMEM;
  }
  if (read_block(mnt, bitmap_block, buf) != SMKFS_OK) {
    free(buf);
    return SMKFS_ERR_IO;
  }

  if (((buf[block_offset] >> bit_offset) & 1) != 0) {
    mnt->sb.free_blocks++;
    if (mnt->regions) {
      ULONG region = (ULONG)(idx / SMKFS_REGION_BLOCKS);
      if (region < mnt->region_count) {
        ULONG rsize = bitmap_region_size(mnt, region);
        if (mnt->regions[region].free_count < rsize)
          mnt->regions[region].free_count++;
      }
    }
  }
  buf[block_offset] &= ~(1 << bit_offset);
  if (write_block(mnt, bitmap_block, buf) != SMKFS_OK) {
    free(buf);
    return SMKFS_ERR_IO;
  }
  free(buf);
  return SMKFS_OK;
}

/*
 * bitmap_test
 * Returns 1 if allocated, 0 if free, or a negative SMKFS_STATUS on error.
 */
LONG bitmap_test(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block) {
  PUCHAR buf;
  LONG ret;
  ULONGLONG idx;
  SMKFS_OFFSET byte_offset, bit_offset, block_offset;
  SMKFS_BLOCK bitmap_block;

  if (block < mnt->sb.data_start) {
    return SMKFS_ERR_CORRUPT;
  }
  idx = block - mnt->sb.data_start;
  byte_offset = idx / 8;
  bit_offset = idx % 8;
  bitmap_block = mnt->sb.bitmap_start + (byte_offset / SMKFS_BLOCK_SIZE);
  block_offset = byte_offset % SMKFS_BLOCK_SIZE;

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    return SMKFS_ERR_NOMEM;
  }
  if (read_block(mnt, bitmap_block, buf) != SMKFS_OK) {
    free(buf);
    return SMKFS_ERR_IO;
  }

  ret = (buf[block_offset] >> bit_offset) & 1;
  free(buf);
  return ret;
}

/* --- Regioned Allocation --- */

static SMKFS_BLOCK bitmap_alloc_in_region(_SMKFS_MOUNT *mnt, ULONG region,
                                          ULONG count) {
  ULONGLONG data_blocks = mnt->sb.total_blocks - mnt->sb.data_start;
  ULONGLONG region_start_bit = (ULONGLONG)region * SMKFS_REGION_BLOCKS;
  ULONGLONG region_end_bit = region_start_bit + SMKFS_REGION_BLOCKS;
  SMKFS_BLOCK bitmap_block;
  SMKFS_OFFSET block_offset;
  ULONGLONG start_byte;
  ULONG scan_len;
  PUCHAR buf;
  ULONGLONG run_start = 0;
  ULONG run_len = 0;
  SMKFS_BLOCK result = 0;
  ULONG i;
  LONG bi;

  if (region_end_bit > data_blocks)
    region_end_bit = data_blocks;
  if (region_end_bit <= region_start_bit)
    return 0;
  if (mnt->regions[region].free_count < count)
    return 0;

  start_byte = region_start_bit / 8;
  block_offset = (SMKFS_OFFSET)(start_byte % SMKFS_BLOCK_SIZE);
  scan_len = (ULONG)((region_end_bit + 7) / 8 - start_byte);
  bitmap_block = mnt->sb.bitmap_start + (start_byte / SMKFS_BLOCK_SIZE);

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf)
    return 0;
  if (read_block(mnt, bitmap_block, buf) != SMKFS_OK) {
    free(buf);
    return 0;
  }

  for (i = 0; i < scan_len; i++) {
    UCHAR byte = buf[block_offset + i];
    for (bi = 0; bi < 8; bi++) {
      ULONGLONG bit = region_start_bit + i * 8 + bi;
      if (bit >= region_end_bit)
        break;

      if ((byte >> bi) & 1) {
        run_len = 0;
      } else {
        if (run_len == 0)
          run_start = bit;
        run_len++;
        if (run_len >= count) {
          result = mnt->sb.data_start + run_start;
          goto found;
        }
      }
    }
  }

found:
  if (result != 0) {
    for (ULONG j = 0; j < count; j++) {
      ULONGLONG bit = run_start + j;
      ULONG byte_idx = (ULONG)(bit / 8 - start_byte);
      LONG bit_offset = (LONG)(bit % 8);
      buf[block_offset + byte_idx] |= (1 << bit_offset);
    }
    if (write_block(mnt, bitmap_block, buf) != SMKFS_OK) {
      result = 0;
    } else {
      mnt->sb.free_blocks -= count;
      if (journal_log_alloc(mnt, result, count) != SMKFS_OK) {
        /* undo the bits we just set */
        for (ULONG j = 0; j < count; j++) {
          bitmap_clear(mnt, result + j);
        }

        result = 0;
      }

      mnt->regions[region].free_count -= count;
      mnt->regions[region].alloc_hint =
          (ULONG)((run_start + count - region_start_bit) % SMKFS_REGION_BLOCKS);
    }
  }

  free(buf);
  return result;
}

/* --- Linear Fallback --- */

static SMKFS_BLOCK bitmap_alloc_range_linear(_SMKFS_MOUNT *mnt, ULONG count) {
  PUCHAR buf;
  PUCHAR j_buf;
  ULONGLONG total_bits;
  ULONGLONG run_start = 0;
  ULONG run_len = 0;
  ULONGLONG bb;
  LONG bo;
  LONG bi;

  if (count == 0)
    return 0;

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  j_buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf || !j_buf) {
    free(buf);
    free(j_buf);
    return 0;
  }

  total_bits = mnt->sb.total_blocks - mnt->sb.data_start;

  for (bb = 0; bb < mnt->sb.bitmap_length; bb++) {

    if (read_block(mnt, mnt->sb.bitmap_start + bb, buf) != SMKFS_OK) {
      free(buf);
      free(j_buf);
      return 0;
    }

    for (bo = 0; bo < SMKFS_BLOCK_SIZE; bo++) {
      uint8_t byte = buf[bo];
      for (bi = 0; bi < 8; bi++) {
        ULONGLONG global_bit = bb * SMKFS_BLOCK_SIZE * 8 + bo * 8 + bi;
        if (global_bit >= total_bits)
          break;

        if ((byte >> bi) & 1) {
          run_len = 0;
        } else {
          if (run_len == 0)
            run_start = global_bit;
          run_len++;
          if (run_len >= count) {
            SMKFS_BLOCK first_block = mnt->sb.data_start + run_start;
            ULONG j;

            for (j = 0; j < count; j++) {
              ULONGLONG idx = run_start + j;
              ULONGLONG j_bb = idx / (SMKFS_BLOCK_SIZE * 8);
              ULONGLONG j_bo = (idx % (SMKFS_BLOCK_SIZE * 8)) / 8;
              ULONGLONG j_bi = idx % 8;
              if (read_block(mnt, mnt->sb.bitmap_start + j_bb, j_buf) !=
                  SMKFS_OK) {
                free(buf);
                free(j_buf);
                return 0;
              }
              j_buf[j_bo] |= (1 << j_bi);
              if (write_block(mnt, mnt->sb.bitmap_start + j_bb, j_buf) !=
                  SMKFS_OK) {
                free(buf);
                free(j_buf);
                return 0;
              }
            }

            mnt->sb.free_blocks -= count;
            if (journal_log_alloc(mnt, first_block, count) != SMKFS_OK) {
              /* undo the bits we just set */
              for (ULONG j = 0; j < count; j++) {
                bitmap_clear(mnt, first_block + j);
              }

              first_block = 0;
            }

            if (mnt->regions) {
              for (j = 0; j < count; j++) {
                ULONGLONG bit = run_start + j;
                ULONG region = (ULONG)(bit / SMKFS_REGION_BLOCKS);
                if (region < mnt->region_count &&
                    mnt->regions[region].free_count > 0) {
                  mnt->regions[region].free_count--;
                }
              }
            }
            free(buf);
            free(j_buf);
            printk("first block = %llu\n", first_block);
            return first_block;
          }
        }
      }
    }
  }
  free(buf);
  free(j_buf);
  return 0;
}

/* --- Public Allocators --- */

SMKFS_BLOCK bitmap_alloc_range(_SMKFS_MOUNT *mnt, ULONG count) {
  if (count == 0)
    return 0;
  if (count == 1)
    return bitmap_alloc(mnt);

  if (mnt->regions && count <= SMKFS_REGION_BLOCKS) {
    ULONG start_region = mnt->alloc_hint_region;
    ULONG r;

    for (r = 0; r < mnt->region_count; r++) {
      ULONG region = (start_region + r) % mnt->region_count;
      if (mnt->regions[region].free_count >= count) {
        SMKFS_BLOCK block = bitmap_alloc_in_region(mnt, region, count);
        if (block != 0) {
          mnt->alloc_hint_region = (region + 1) % mnt->region_count;
          return block;
        }
      }
    }
  }

  return bitmap_alloc_range_linear(mnt, count);
}

SMKFS_BLOCK bitmap_alloc(_SMKFS_MOUNT *mnt) {
  ULONG start_region;
  ULONG r;

  if (!mnt->regions)
    return bitmap_alloc_range_linear(mnt, 1);

  start_region = mnt->alloc_hint_region;
  for (r = 0; r < mnt->region_count; r++) {
    ULONG region = (start_region + r) % mnt->region_count;
    if (mnt->regions[region].free_count > 0) {
      SMKFS_BLOCK block = bitmap_alloc_in_region(mnt, region, 1);
      if (block != 0) {
        mnt->alloc_hint_region = region;
        return block;
      }
    }
  }

  return bitmap_alloc_range_linear(mnt, 1);
}

SMKFS_STATUS bitmap_free_range(_SMKFS_MOUNT *mnt, SMKFS_BLOCK start,
                               ULONG count) {
  PUCHAR buf;
  ULONG actually_freed = 0;
  ULONG i;
  SMKFS_STATUS status = SMKFS_OK;

  if (!mnt || count == 0) {
    return SMKFS_ERR_INVAL;
  }

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    return SMKFS_ERR_NOMEM;
  }

  for (i = 0; i < count; i++) {
    SMKFS_BLOCK block = start + i;
    ULONGLONG idx;
    ULONGLONG bb, bo, bi;

    if (block < mnt->sb.data_start) {
      continue;
    }

    idx = block - mnt->sb.data_start;
    bb = idx / (SMKFS_BLOCK_SIZE * 8);
    bo = (idx % (SMKFS_BLOCK_SIZE * 8)) / 8;
    bi = idx % 8;

    if (read_block(mnt, mnt->sb.bitmap_start + bb, buf) != SMKFS_OK) {
      free(buf);
      return SMKFS_ERR_IO;
    }

    if ((buf[bo] >> bi) & 1) {
      buf[bo] &= ~(1 << bi);
      actually_freed++;

      if (mnt->regions) {
        ULONG region = (ULONG)(idx / SMKFS_REGION_BLOCKS);
        if (region < mnt->region_count) {
          ULONG rsize = bitmap_region_size(mnt, region);
          if (mnt->regions[region].free_count < rsize)
            mnt->regions[region].free_count++;
        }
      }
    }

    if (write_block(mnt, mnt->sb.bitmap_start + bb, buf) != SMKFS_OK) {
      free(buf);
      return SMKFS_ERR_IO;
    }
  }

  if (actually_freed > 0) {
    if (journal_log_free(mnt, start, actually_freed) != SMKFS_OK) {
      /* undo the bits we just cleared */
      for (ULONG j = 0; j < actually_freed; j++) {
        bitmap_set(mnt, start + j);
      }
      status = SMKFS_ERR_JOURNAL;
    } else {
      mnt->sb.free_blocks += actually_freed;
    }
  }
  free(buf);
  return status;
}