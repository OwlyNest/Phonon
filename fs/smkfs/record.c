/*
 * fs/smkfs/record.c - Record Operations (G1)
 * Author:   amity
 * Date:     Wed Jul 29 17:38:25 2026
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
#include <lib/string.h>
#include <mm/heap.h>
#include <stdint.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/

SMKFS_STATUS record_read(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                         _SMKFS_RECORD *rec, PVOID attr_buf, SIZE_T buf_size) {
  PUCHAR block;
  SIZE_T attr_len;
  SMKFS_BLOCK phys_block;
  SMKFS_STATUS mrt_ret;
  SMKFS_GENERATION generation;

  if (!rec || !attr_buf) {
    return SMKFS_ERR_INVAL;
  }

  block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block)
    return SMKFS_ERR_NOMEM;

  mrt_ret = mrt_resolve(mnt, record_id, &phys_block, NULL, &generation);
  if (mrt_ret != SMKFS_OK) {
    free(block);
    return mrt_ret;
  }

  if (read_block(mnt, phys_block, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  memcpy(rec, block, sizeof(_SMKFS_RECORD));

  if (header_validate(&rec->header, SMKFS_ST_RECORD) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_CORRUPT;
  }

  if (rec->header.length < sizeof(_SMKFS_RECORD)) {
    free(block);
    return SMKFS_ERR_CORRUPT;
  }

  if (header_checksum_verify(&rec->header, block, rec->header.length) !=
      SMKFS_OK) {
    free(block);
    return SMKFS_ERR_CORRUPT;
  }

  if (rec->generation != generation) {
    free(block);
    return SMKFS_ERR_CORRUPT;
  }

  attr_len = rec->header.length - sizeof(_SMKFS_RECORD);
  if (attr_len > buf_size)
    attr_len = buf_size;

  memcpy(attr_buf, block + sizeof(_SMKFS_RECORD), attr_len);
  free(block);
  return SMKFS_OK;
}

SMKFS_STATUS record_write(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                          const _SMKFS_RECORD *rec, PCVOID attr_buf) {
  PUCHAR block, old_block;
  SIZE_T total_len;
  SIZE_T attr_len;
  SMKFS_STATUS ret;
  PCUCHAR ptr;
  _SMKFS_RECORD *wrec;
  SMKFS_BLOCK phys_block;
  SMKFS_STATUS mrt_ret;

  if (!rec)
    return SMKFS_ERR_INVAL;
  if (sizeof(_SMKFS_RECORD) > SMKFS_BLOCK_SIZE) {
    return SMKFS_ERR_TOO_BIG;
  }

  if (!attr_buf)
    return SMKFS_ERR_INVAL;

  attr_len = 0;
  ptr = (PCUCHAR)attr_buf;
  while (1) {
    _SMKFS_ATTR_HEADER *ah = (_SMKFS_ATTR_HEADER *)((uintptr_t)ptr);
    if (ah->type == SMKFS_ATTRT_END) {
      attr_len = (SIZE_T)(ptr - (PCUCHAR)attr_buf) + sizeof(_SMKFS_ATTR_HEADER);
      break;
    }

    if (attr_len > SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD)) {
      return SMKFS_ERR_TOO_BIG;
    }

    ptr += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
    if ((SIZE_T)(ptr - (PCUCHAR)attr_buf) >
        SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD)) {
      return SMKFS_ERR_TOO_BIG;
    }
  }

  total_len = sizeof(_SMKFS_RECORD) + attr_len;
  if (total_len < sizeof(_SMKFS_RECORD) || total_len > SMKFS_BLOCK_SIZE) {
    return SMKFS_ERR_TOO_BIG;
  }

  block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  old_block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block || !old_block) {
    free(block);
    free(old_block);
    return SMKFS_ERR_NOMEM;
  }

  mrt_ret = mrt_resolve(mnt, record_id, &phys_block, NULL, NULL);
  if (mrt_ret != SMKFS_OK) {
    free(block);
    free(old_block);
    return mrt_ret;
  }

  if (read_block(mnt, phys_block, old_block) != SMKFS_OK) {
    memset(old_block, 0, SMKFS_BLOCK_SIZE);
  }

  memset(block, 0, SMKFS_BLOCK_SIZE);
  wrec = (_SMKFS_RECORD *)block;
  memcpy(wrec, rec, sizeof(_SMKFS_RECORD));
  wrec->header.length = total_len;
  memcpy(block + sizeof(_SMKFS_RECORD), attr_buf, attr_len);
  header_checksum_update(&wrec->header, block, total_len);

  ret = journal_log_write(mnt, phys_block, old_block, block, total_len);
  if (ret != SMKFS_OK) {
    free(block);
    free(old_block);
    return ret;
  }

  ret = write_block(mnt, phys_block, block);
  free(block);
  free(old_block);
  return (ret == SMKFS_OK) ? SMKFS_OK : SMKFS_ERR_IO;
}

SMKFS_RECORD_ID record_alloc(_SMKFS_MOUNT *mnt, SMKFS_OBJECT_TYPE object_type) {
  SMKFS_RECORD_ID logical_id;
  SMKFS_GENERATION generation;
  SMKFS_BLOCK block;

  if (mrt_alloc_entry(mnt, &logical_id, &generation) != SMKFS_OK) {
    return 0;
  }

  block = bitmap_alloc(mnt);
  if (block == 0) {
    mrt_free_entry(mnt, logical_id);
    return 0;
  }

  if (mrt_update_entry(mnt, logical_id, block, SMKFS_MRTF_ALLOCATED) !=
      SMKFS_OK) {
    bitmap_clear(mnt, block);
    mrt_free_entry(mnt, logical_id);
    return 0;
  }

  _SMKFS_RECORD rec;
  header_init(&rec.header, SMKFS_ST_RECORD,
              sizeof(_SMKFS_RECORD) + sizeof(_SMKFS_ATTR_HEADER), 0);
  rec.record_id = logical_id;
  rec.object_type = object_type;
  rec.attr_count = 0;
  rec.link_count = 1;
  rec.generation = generation;

  _SMKFS_ATTR_HEADER term;
  term.type = SMKFS_ATTRT_END;
  term.flags = 0;
  term.id = 0;
  term.length = 0;

  if (record_write(mnt, logical_id, &rec, &term) != SMKFS_OK) {
    bitmap_clear(mnt, block);
    mrt_free_entry(mnt, logical_id);
    return 0;
  }

  mnt->sb.record_count++;
  return logical_id;
}

VOID record_free(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    free(block);
    return;
  }
  _SMKFS_RECORD *rec;
  SMKFS_BLOCK phys_block;
  PUCHAR attr_buf;

  if (mrt_resolve(mnt, record_id, &phys_block, NULL, NULL) != SMKFS_OK) {
    free(block);
    return;
  }

  /* Free all data extents and journal the cleaned record image */
  extent_remove_all(mnt, record_id);

  /* Mark the record deleted (still under the caller's transaction) */
  if (read_block(mnt, phys_block, block) == SMKFS_OK) {
    rec = (_SMKFS_RECORD *)block;
    attr_buf = block + sizeof(_SMKFS_RECORD);

    rec->header.flags |= SMKFS_FLA_DELETED;
    rec->header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(attr_buf);
    /* record_write journals the final image */
    record_write(mnt, record_id, rec, attr_buf);
  }

  /* Free the record's own block – journalled */
  bitmap_free_range(mnt, phys_block, 1);

  /* Free the MRT slot – now journalled via JOP_MRT_UPDATE */
  mrt_free_entry(mnt, record_id);

  if (mnt->sb.record_count > 0) {
    mnt->sb.record_count--;
  }
}

SIZE_T attr_buf_total_len(PCVOID attr_buf) {
  PCUCHAR ptr = (PCUCHAR)attr_buf;
  SIZE_T total = 0;

  while (1) {
    const _SMKFS_ATTR_HEADER *ah = (const _SMKFS_ATTR_HEADER *)ptr;
    total += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
    if (ah->type == SMKFS_ATTRT_END)
      break;
    ptr += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
  }
  return total;
}