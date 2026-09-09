/*
 * fs/smkfs/block.c - Block I/O Primitives
 * Author:   amity
 * Date:     Wed Jul 29 17:37:58 2026
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
#include <fs/smkfs.h>
#include <fs/smkfs_internal.h>
#include <hw/ide.h>
#include <lib/string.h>
#include <screen/printk.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/

/*
 * SMKFS_STATUS disk_read_block
 * Convert block to sectors, read sectors into buffer
 * return status code
 */
SMKFS_STATUS
disk_read_block(_SMKFS_MOUNT *mnt, /* Needs drive number and sector size */
                SMKFS_BLOCK block, /* Block to read*/
                PVOID buf          /* Buffer to read into */
) {
  if (SMKFS_BLOCK_SIZE % mnt->sb.sector_size != 0) {
    return SMKFS_ERR_IO;
  }

  UCHAR sectors = SMKFS_BLOCK_SIZE / mnt->sb.sector_size;
  ULONG lba = (ULONG)(block * sectors);
  PUCHAR ptr = (PUCHAR)buf;

  for (UCHAR i = 0; i < sectors; i++) {
    if (ide_read_sectors(mnt->drive_num, lba + i, 1,
                         (uint16_t *)(ptr + i * mnt->sb.sector_size)) !=
        SMKFS_OK) {
      return SMKFS_ERR_IO;
    }
  }
  return SMKFS_OK;
}

SMKFS_STATUS read_block(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block, PVOID buf) {
  _SMKFS_BLOCK_BUF *slot;
  SMKFS_STATUS status = block_cache_read(mnt, block, &slot);
  if (status != SMKFS_OK)
    return status;
  memcpy(buf, slot->data, SMKFS_BLOCK_SIZE);
  return SMKFS_OK;
}

/*
 * SMKFS_STATUS disk_write_block
 * Convert block to sectors, write buffer to sectors on disk
 * return status code
 */
SMKFS_STATUS disk_write_block(
    _SMKFS_MOUNT *mnt, /* Mount context, needs drive number and sector size */
    SMKFS_BLOCK block, /* The physical block to write to*/
    PCVOID buf         /* (Pointer to Const VOID), data to write to block*/
) {
  if (SMKFS_BLOCK_SIZE % mnt->sb.sector_size != 0) {
    return SMKFS_ERR_IO;
  }

  UCHAR sectors = SMKFS_BLOCK_SIZE / mnt->sb.sector_size;
  ULONG lba = (ULONG)(block * sectors);
  PCUCHAR ptr = (PCUCHAR)buf;

  for (UCHAR i = 0; i < sectors; i++) {
    if (ide_write_sectors(mnt->drive_num, lba + i, 1,
                          (const uint16_t *)(ptr + i * mnt->sb.sector_size)) !=
        SMKFS_OK) {
      printk("[SmKFS] write_block FAILED: block=%llu LBA=%u\n", block, lba + i);
      return SMKFS_ERR_IO;
    }
  }
  return SMKFS_OK;
}

SMKFS_STATUS write_block(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block, PCVOID buf) {
  _SMKFS_BLOCK_BUF *slot;
  SMKFS_STATUS status = block_cache_read(mnt, block, &slot);
  if (status != SMKFS_OK)
    return status;
  memcpy(slot->data, buf, SMKFS_BLOCK_SIZE);
  return block_cache_write(mnt, slot);
}