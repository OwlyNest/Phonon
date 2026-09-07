/*
 * fs/smkfs/extent.c - Extent Management
 * Author:   amity
 * Date:     Wed Jul 29 17:38:38 2026
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

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/
static LONG extent_resolve_cb(SMKFS_ATTR_ID attr_id, PVOID data, SIZE_T len,
                              PVOID ctx) {
  (VOID) attr_id;
  _SMKFS_ATTR_CTX *c = (_SMKFS_ATTR_CTX *)ctx;

  if (len != sizeof(_SMKFS_EXTENT))
    return 0;

  _SMKFS_EXTENT *ext = (_SMKFS_EXTENT *)data;
  if (c->block >= ext->logical_offset &&
      c->block < ext->logical_offset + ext->block_count) {
    if (c->out)
      *c->out = *ext;
    c->found = 1;
    return 1;
  }
  return 0;
}

SMKFS_STATUS extent_resolve(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                            SMKFS_LBLOCK logical_block, _SMKFS_EXTENT *out) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD rec;
  SMKFS_STATUS status;
  _SMKFS_ATTR_CTX ctx;

  status = record_read(mnt, record_id, &rec, attr_buf, buf_size);
  if (status != SMKFS_OK) {
    free(attr_buf);
    return status;
  }

  ctx.block = logical_block;
  ctx.out = out;
  ctx.found = 0;

  record_iterate_attr(attr_buf, SMKFS_ATTRT_EXTENTS, extent_resolve_cb, &ctx);

  free(attr_buf);
  return ctx.found ? SMKFS_OK : SMKFS_ERR_NOTFOUND;
}

static LONG extent_merge_cb(SMKFS_ATTR_ID attr_id, PVOID data, SIZE_T len,
                            PVOID ctx) {
  _SMKFS_EXT_MERGE_CTX *c = (_SMKFS_EXT_MERGE_CTX *)ctx;

  if (len != sizeof(_SMKFS_EXTENT))
    return 0;

  _SMKFS_EXTENT *ext = (_SMKFS_EXTENT *)data;

  /* New follows ext */
  if (ext->logical_offset + ext->block_count == c->logical_block &&
      ext->physical_block + ext->block_count == c->physical_block) {
    c->merged.logical_offset = ext->logical_offset;
    c->merged.physical_block = ext->physical_block;
    c->merged.block_count = ext->block_count + c->count;
    c->matched_id = attr_id;
    c->found = 1;
    return 1;
  }

  /* New precedes ext */
  if (c->logical_block + c->count == ext->logical_offset &&
      c->physical_block + c->count == ext->physical_block) {
    c->merged.logical_offset = c->logical_block;
    c->merged.physical_block = c->physical_block;
    c->merged.block_count = c->count + ext->block_count;
    c->matched_id = attr_id;
    c->found = 1;
    return 1;
  }

  return 0;
}

SMKFS_STATUS extent_add(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                        SMKFS_LBLOCK logical_block, SMKFS_BLOCK physical_block,
                        ULONG count) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }

  _SMKFS_RECORD *rec = (_SMKFS_RECORD *)block;

  SMKFS_BLOCK phys_block;
  SMKFS_STATUS mrt_ret = mrt_resolve(mnt, record_id, &phys_block, NULL, NULL);
  if (mrt_ret != SMKFS_OK) {
    free(block);
    return mrt_ret;
  }

  if (read_block(mnt, phys_block, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  PUCHAR attr_buf = block + sizeof(_SMKFS_RECORD);
  SIZE_T attr_space = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);

  _SMKFS_EXT_MERGE_CTX ctx;
  ctx.logical_block = logical_block;
  ctx.physical_block = physical_block;
  ctx.count = count;
  ctx.found = 0;

  record_iterate_attr(attr_buf, SMKFS_ATTRT_EXTENTS, extent_merge_cb, &ctx);

  if (ctx.found) {
    record_remove_attr_id(attr_buf, SMKFS_ATTRT_EXTENTS, ctx.matched_id);
    if (record_add_attr(attr_buf, attr_space, SMKFS_ATTRT_EXTENTS, &ctx.merged,
                        sizeof(ctx.merged)) != SMKFS_OK) {
      free(block);
      return SMKFS_ERR_NOSPC;
    }
  } else {
    _SMKFS_EXTENT new_ext;
    new_ext.logical_offset = logical_block;
    new_ext.physical_block = physical_block;
    new_ext.block_count = count;
    if (record_add_attr(attr_buf, attr_space, SMKFS_ATTRT_EXTENTS, &new_ext,
                        sizeof(new_ext)) != SMKFS_OK) {
      free(block);
      return SMKFS_ERR_NOSPC;
    }
  }

  /* Don't modify length header entry, record_write updates it */
  rec->attr_count = 0;
  PUCHAR ptr = attr_buf;
  while (1) {
    _SMKFS_ATTR_HEADER *ah = (_SMKFS_ATTR_HEADER *)ptr;
    rec->attr_count++;
    if (ah->type == SMKFS_ATTRT_END)
      break;
    ptr += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
  }
  rec->attr_count--;

  SMKFS_STATUS ret = record_write(mnt, record_id, rec, attr_buf);
  free(block);
  return ret;
}

static LONG extent_remove_cb(SMKFS_ATTR_ID attr_id, PVOID data, SIZE_T len,
                             PVOID ctx) {
  (VOID) attr_id;
  _SMKFS_EXT_REMOVE_CTX *c = (_SMKFS_EXT_REMOVE_CTX *)ctx;

  if (len != sizeof(_SMKFS_EXTENT)) {
    return 0;
  }

  if (c->count >= 32) {
    return 0;
  }

  _SMKFS_EXTENT *ext = (_SMKFS_EXTENT *)data;

  c->extents[c->count] = *ext;
  c->count++;

  return 0;
}

VOID extent_remove_all(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return;
  }
  _SMKFS_RECORD *rec = (_SMKFS_RECORD *)block;
  PUCHAR attr_buf;
  _SMKFS_EXTENT extents[32]; /* 32 * 20 = 640 bytes. Iffy, passes for now. */
  _SMKFS_EXT_REMOVE_CTX ctx;
  SMKFS_BLOCK phys_block;
  SMKFS_STATUS mrt_ret;

  mrt_ret = mrt_resolve(mnt, record_id, &phys_block, NULL, NULL);
  if (mrt_ret != SMKFS_OK) {
    free(block);
    return;
  }

  if (read_block(mnt, phys_block, block) != SMKFS_OK) {
    free(block);
    return;
  }

  attr_buf = block + sizeof(_SMKFS_RECORD);

  ctx.extents = extents;
  ctx.count = 0;
  record_iterate_attr(attr_buf, SMKFS_ATTRT_EXTENTS, extent_remove_cb, &ctx);

  for (ULONG i = 0; i < ctx.count; i++) {
    bitmap_free_range(mnt, extents[i].physical_block, extents[i].block_count);
  }

  record_remove_attr(attr_buf, SMKFS_ATTRT_EXTENTS);

  /* Recount attributes and recompute length */
  rec->attr_count = 0;
  {
    PUCHAR ptr = attr_buf;
    while (1) {
      _SMKFS_ATTR_HEADER *ah = (_SMKFS_ATTR_HEADER *)ptr;
      if (ah->type == SMKFS_ATTRT_END) {
        break;
      }
      rec->attr_count++;
      ptr += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
    }
  }

  rec->header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(attr_buf);

  /* Journalled write of the cleaned record */
  record_write(mnt, record_id, rec, attr_buf);
  free(block);
}