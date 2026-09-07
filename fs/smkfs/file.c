/*
 * fs/smkfs/file.c - File I/O and File Descriptor Management (G1)
 * Author:   amity
 * Date:     Wed Jul 29 17:38:56 2026
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
#include <screen/printk.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/

/* --- Byte-aligned Block I/O Helpers --- */

/*
 * block_read_byte
 * Read `len` bytes from `offset` within logical block `lblock`.
 * Holes read as zero.  Returns bytes read or negative error.
 */
static LONG block_read_byte(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                            SMKFS_LBLOCK lblock, SIZE_T offset, SIZE_T len,
                            PVOID buf) {
  _SMKFS_EXTENT ext;
  UCHAR *block = (UCHAR *)malloc(sizeof(UCHAR) * SMKFS_BLOCK_SIZE);
  if (!block) {
    free(block);
    return SMKFS_ERR_NOMEM;
  }

  if (offset + len > SMKFS_BLOCK_SIZE) {
    free(block);
    return SMKFS_ERR_INVAL;
  }
  if (len == 0) {
    free(block);
    return 0;
  }

  if (extent_resolve(mnt, record_id, lblock, &ext) == SMKFS_OK) {
    SMKFS_BLOCK phys = ext.physical_block + (lblock - ext.logical_offset);
    if (read_block(mnt, phys, block) != SMKFS_OK) {
      free(block);
      return SMKFS_ERR_IO;
    }
    memcpy(buf, block + offset, len);
  } else {
    /* Sparse hole */
    memset(buf, 0, len);
  }

  free(block);
  return (LONG)len;
}

/*
 * block_write_byte
 * Write `len` bytes to `offset` within logical block `lblock`.
 * RMW on existing blocks; allocates and zero-fills new blocks.
 * Returns bytes written or negative error.
 */
static LONG block_write_byte(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                             SMKFS_LBLOCK lblock, SIZE_T offset, SIZE_T len,
                             PCVOID src) {
  _SMKFS_EXTENT ext;
  PUCHAR block = (PUCHAR)malloc(sizeof(UCHAR) * SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }

  SMKFS_BLOCK phys;

  if (offset + len > SMKFS_BLOCK_SIZE) {
    free(block);
    return SMKFS_ERR_INVAL;
  }

  if (len == 0) {
    free(block);
    return 0;
  }

  if (extent_resolve(mnt, record_id, lblock, &ext) == SMKFS_OK) {
    phys = ext.physical_block + (lblock - ext.logical_offset);
    if (read_block(mnt, phys, block) != SMKFS_OK) {
      free(block);
      return SMKFS_ERR_IO;
    }
  } else {
    phys = bitmap_alloc(mnt);
    if (phys == 0) {
      free(block);
      return SMKFS_ERR_NOSPC;
    }

    if (extent_add(mnt, record_id, lblock, phys, 1) != SMKFS_OK) {
      bitmap_clear(mnt, phys);
      free(block);
      return SMKFS_ERR_NOSPC;
      ;
    }

    memset(block, 0, SMKFS_BLOCK_SIZE);
  }

  memcpy(block + offset, src, len);

  if (write_block(mnt, phys, block) != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  free(block);
  return (LONG)len;
}

LONG smkfs_read(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                SMKFS_OFFSET offset, SIZE_T len, PVOID buf) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf;
  _SMKFS_RECORD rec;
  ULONGLONG *fsize_ptr;
  ULONGLONG file_size;
  SIZE_T to_read;
  PUCHAR out = (PUCHAR)buf;
  LONG ret;

  if (!mnt->mounted || !buf || record_id == 0) {
    return SMKFS_ERR_INVAL;
  }

  attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }

  if (record_read(mnt, record_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type != SMKFS_ROT_FILE) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (record_find_attr(attr_buf, SMKFS_ATTRT_FSIZE, (PVOID *)&fsize_ptr,
                       NULL) != SMKFS_OK) {
    file_size = 0;
  } else {
    file_size = *fsize_ptr;
  }

  if (offset >= file_size) {
    free(attr_buf);
    return 0;
  }

  to_read = len;
  if (offset + to_read > file_size)
    to_read = (SIZE_T)(file_size - offset);

  for (SIZE_T done = 0; done < to_read;) {
    SMKFS_LBLOCK logical_block =
        (SMKFS_LBLOCK)((offset + done) / SMKFS_BLOCK_SIZE);
    SIZE_T block_offset = (SIZE_T)((offset + done) % SMKFS_BLOCK_SIZE);
    SIZE_T chunk = to_read - done;
    if (chunk > SMKFS_BLOCK_SIZE - block_offset)
      chunk = SMKFS_BLOCK_SIZE - block_offset;

    ret = block_read_byte(mnt, record_id, logical_block, block_offset, chunk,
                          out + done);
    if (ret < 0) {
      free(attr_buf);
      return ret;
    }
    done += (SIZE_T)ret;
  }

  free(attr_buf);
  return (LONG)to_read;
}

LONG smkfs_write(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                 SMKFS_OFFSET offset, SIZE_T len, PCVOID buf) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }

  _SMKFS_RECORD rec;
  ULONGLONG *fsize_ptr;
  ULONGLONG file_size;
  ULONGLONG new_size;
  PCUCHAR in = (PCUCHAR)buf;

  if (!mnt->mounted || !buf || record_id == 0) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }
  if (record_read(mnt, record_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type != SMKFS_ROT_FILE) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }
  if (record_find_attr(attr_buf, SMKFS_ATTRT_FSIZE, (PVOID *)&fsize_ptr,
                       NULL) != SMKFS_OK) {
    file_size = 0;
  } else {
    file_size = *fsize_ptr;
  }

  new_size = offset + len;
  if (new_size < file_size) {
    new_size = file_size;
  }

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  for (SIZE_T done = 0; done < len;) {
    SMKFS_LBLOCK logical_block =
        (SMKFS_LBLOCK)((offset + done) / SMKFS_BLOCK_SIZE);
    SIZE_T block_offset = (SIZE_T)((offset + done) % SMKFS_BLOCK_SIZE);
    SIZE_T chunk = len - done;
    if (chunk > SMKFS_BLOCK_SIZE - block_offset) {
      chunk = SMKFS_BLOCK_SIZE - block_offset;
    }

    LONG r = block_write_byte(mnt, record_id, logical_block, block_offset,
                              chunk, in + done);
    if (r < 0) {
      journal_abort(mnt);
      free(attr_buf);
      return r;
    }
    done += (SIZE_T)r;
  }

  if (new_size != file_size) {
    SIZE_T fbuf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
    PUCHAR final_attr = (PUCHAR)malloc(fbuf_size);
    _SMKFS_RECORD final_rec;

    if (!final_attr) {
      journal_abort(mnt);
      free(attr_buf);
      return SMKFS_ERR_NOMEM;
    }

    if (record_read(mnt, record_id, &final_rec, final_attr, fbuf_size) !=
        SMKFS_OK) {
      journal_abort(mnt);
      free(attr_buf);
      free(final_attr);
      return SMKFS_ERR_IO;
    }

    if (record_add_attr(final_attr, fbuf_size, SMKFS_ATTRT_FSIZE, &new_size,
                        sizeof(new_size)) == SMKFS_OK) {
      final_rec.attr_count++;
      if (record_write(mnt, record_id, &final_rec, final_attr) != SMKFS_OK) {
        journal_abort(mnt);
        free(attr_buf);
        free(final_attr);
        return SMKFS_ERR_IO;
      }
    }
    free(final_attr);
  }

  if (journal_commit(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  free(attr_buf);
  return (LONG)len;
}

static LONG truncate_collect_cb(SMKFS_ATTR_ID attr_id, PVOID data, SIZE_T len,
                                PVOID ctx) {
  _SMKFS_EXT_REMOVE_CTX *c = (_SMKFS_EXT_REMOVE_CTX *)ctx;

  (VOID) attr_id;
  if (len != sizeof(_SMKFS_EXTENT)) {
    return 0;
  }

  if (c->count >= 32) {
    return 0;
  }

  c->extents[c->count++] = *(_SMKFS_EXTENT *)data;
  return 0;
}

SMKFS_STATUS smkfs_truncate(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                            ULONGLONG new_size) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD rec;
  ULONGLONG *fsize_ptr;
  ULONGLONG old_size;
  ULONGLONG old_blocks;
  ULONGLONG new_blocks;
  SMKFS_STATUS ret;

  if (!mnt->mounted || record_id == 0) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }
  if (record_read(mnt, record_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type != SMKFS_ROT_FILE) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }
  if (record_find_attr(attr_buf, SMKFS_ATTRT_FSIZE, (PVOID *)&fsize_ptr,
                       NULL) != SMKFS_OK) {
    old_size = 0;
  } else {
    old_size = *fsize_ptr;
  }

  if (new_size == old_size) {
    free(attr_buf);
    return SMKFS_OK;
  }

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  if (new_size < old_size) {
    old_blocks = (old_size + SMKFS_BLOCK_SIZE - 1) / SMKFS_BLOCK_SIZE;
    new_blocks = (new_size + SMKFS_BLOCK_SIZE - 1) / SMKFS_BLOCK_SIZE;

    if (new_blocks < old_blocks) {
      _SMKFS_EXTENT all_ext[32];
      _SMKFS_EXT_REMOVE_CTX collect;
      PVOID ext_data;
      SIZE_T ext_len;

      collect.extents = all_ext;
      collect.count = 0;

      /* Gather every extent attribute instance */
      record_iterate_attr(attr_buf, SMKFS_ATTRT_EXTENTS, truncate_collect_cb,
                          &collect);

      /* Strip every extent attribute from the buffer */
      while (record_find_attr(attr_buf, SMKFS_ATTRT_EXTENTS, &ext_data,
                              &ext_len) == SMKFS_OK) {
        record_remove_attr(attr_buf, SMKFS_ATTRT_EXTENTS);
      }

      /* Rewrite only the extents that survive truncation */
      for (ULONG i = 0; i < collect.count; i++) {
        ULONGLONG ext_start = all_ext[i].logical_offset;
        ULONGLONG ext_end = ext_start + all_ext[i].block_count;

        if (ext_start >= new_blocks) {
          /* Entire extent is past the new EOF: free all blocks */
          bitmap_free_range(mnt, all_ext[i].physical_block,
                            all_ext[i].block_count);
        } else if (ext_end > new_blocks) {
          /* Extent straddles the truncation point: trim tail */
          ULONGLONG keep = new_blocks - ext_start;
          bitmap_free_range(mnt, all_ext[i].physical_block + keep,
                            all_ext[i].block_count - (ULONG)keep);
          all_ext[i].block_count = (ULONG)keep;
          if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS,
                              &all_ext[i], sizeof(_SMKFS_EXTENT)) != SMKFS_OK) {
            journal_abort(mnt);
            free(attr_buf);
            return SMKFS_ERR_NOSPC;
          }
        } else {
          /* Entire extent is before the new EOF: keep as-is */
          if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS,
                              &all_ext[i], sizeof(_SMKFS_EXTENT)) != SMKFS_OK) {
            journal_abort(mnt);
            free(attr_buf);
            return SMKFS_ERR_NOSPC;
          }
        }
      }
    }
  }
  /* Growing (new_size > old_size): sparse — just update FSIZE,
   * no block allocation.  Holes read as zero via block_read_byte.
   */

  /* Update FSIZE */
  record_remove_attr(attr_buf, SMKFS_ATTRT_FSIZE);
  if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_FSIZE, &new_size,
                      sizeof(new_size)) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return SMKFS_ERR_NOSPC;
  }

  rec.header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(attr_buf);

  ret = record_write(mnt, record_id, &rec, attr_buf);
  if (ret != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return ret;
  }

  if (journal_commit(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  free(attr_buf);
  return SMKFS_OK;
}

LONG smkfs_open(_SMKFS_MOUNT *mnt, SMKFS_PATH path, LONG flags) {
  SMKFS_RECORD_ID record_id;
  LONG fd;

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    if (flags & SMKFS_O_CREATE) {
      if (smkfs_create_file(mnt, path, SMKFS_PERM_WRITE | SMKFS_PERM_READ) !=
          SMKFS_OK) {
        return SMKFS_ERR_NOSPC;
      }

      if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
        return SMKFS_ERR_NOTFOUND;
      }
    } else {
      return SMKFS_ERR_NOTFOUND;
    }
  }

  smkfs_dump_record(mnt, record_id);

  for (fd = 0; fd < SMKFS_FD_MAX; fd++) {
    if (!mnt->fd_table[fd].used)
      break;
  }

  if (fd >= SMKFS_FD_MAX)
    return SMKFS_ERR_NOSPC;

  mnt->fd_table[fd].used = 1;
  mnt->fd_table[fd].record_id = record_id;
  mnt->fd_table[fd].offset = 0;
  mnt->fd_table[fd].flags = flags;

  if (flags & SMKFS_O_APPEND) {
    SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
    PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
    if (!attr_buf) {
      free(attr_buf);
      return SMKFS_ERR_NOMEM;
    }

    _SMKFS_RECORD rec;
    ULONGLONG *fsize_ptr;
    if (record_read(mnt, record_id, &rec, attr_buf, buf_size) == SMKFS_OK) {
      if (record_find_attr(attr_buf, SMKFS_ATTRT_FSIZE, (PVOID *)&fsize_ptr,
                           NULL) == SMKFS_OK) {
        mnt->fd_table[fd].offset = *fsize_ptr;
      }
    }
    free(attr_buf);
  }

  printk("[OPEN] fd: %d\n", fd);
  return fd;
}

SMKFS_STATUS smkfs_close(_SMKFS_MOUNT *mnt, LONG fd) {
  if (fd < 0 || fd >= SMKFS_FD_MAX)
    return SMKFS_ERR_INVAL;
  if (!mnt->fd_table[fd].used)
    return SMKFS_ERR_INVAL;

  mnt->fd_table[fd].used = 0;
  mnt->fd_table[fd].record_id = 0;
  mnt->fd_table[fd].offset = 0;
  mnt->fd_table[fd].flags = 0;
  return SMKFS_OK;
}

LONG smkfs_read_file(_SMKFS_MOUNT *mnt, LONG fd, PVOID buf, SIZE_T len) {
  LONG ret;

  if (fd < 0 || fd >= SMKFS_FD_MAX)
    return SMKFS_ERR_INVAL;
  if (!mnt->fd_table[fd].used)
    return SMKFS_ERR_INVAL;
  if (!buf)
    return SMKFS_ERR_INVAL;

  ret = smkfs_read(mnt, mnt->fd_table[fd].record_id, mnt->fd_table[fd].offset,
                   len, buf);
  if (ret > 0)
    mnt->fd_table[fd].offset += ret;
  return ret;
}

LONG smkfs_write_file(_SMKFS_MOUNT *mnt, LONG fd, PCVOID buf, SIZE_T len) {
  LONG ret;

  if (fd < 0 || fd >= SMKFS_FD_MAX)
    return SMKFS_ERR_INVAL;
  if (!mnt->fd_table[fd].used)
    return SMKFS_ERR_INVAL;
  if (!buf)
    return SMKFS_ERR_INVAL;

  ret = smkfs_write(mnt, mnt->fd_table[fd].record_id, mnt->fd_table[fd].offset,
                    len, buf);
  if (ret > 0)
    mnt->fd_table[fd].offset += ret;
  return ret;
}

LONG smkfs_seek(_SMKFS_MOUNT *mnt, LONG fd, LONGLONG offset, LONG whence) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf;
  _SMKFS_RECORD rec;
  ULONGLONG *fsize_ptr;
  ULONGLONG file_size = 0;
  LONGLONG new_offset;

  if (fd < 0 || fd >= SMKFS_FD_MAX) {
    return SMKFS_ERR_INVAL;
  }

  if (!mnt->fd_table[fd].used) {
    return SMKFS_ERR_INVAL;
  }

  switch (whence) {
  case SMKFS_SEEK_END:
    attr_buf = (PUCHAR)malloc(buf_size);
    if (!attr_buf) {
      free(attr_buf);
      return SMKFS_ERR_NOMEM;
    }

    if (record_read(mnt, mnt->fd_table[fd].record_id, &rec, attr_buf,
                    buf_size) == SMKFS_OK) {
      if (record_find_attr(attr_buf, SMKFS_ATTRT_FSIZE, (PVOID *)&fsize_ptr,
                           NULL) == SMKFS_OK) {
        file_size = *fsize_ptr;
      }
    }
    free(attr_buf);
    new_offset = (LONGLONG)file_size + offset;
    break;
  case SMKFS_SEEK_CUR:
    new_offset = (LONGLONG)mnt->fd_table[fd].offset + offset;
    break;
  case SMKFS_SEEK_SET:
    new_offset = offset;
    break;
  default:
    return SMKFS_ERR_INVAL;
  }

  if (new_offset < 0) {
    return SMKFS_ERR_INVAL;
  }
  mnt->fd_table[fd].offset = (ULONGLONG)new_offset;
  return (LONG)mnt->fd_table[fd].offset;
}

SMKFS_STATUS smkfs_ftruncate(_SMKFS_MOUNT *mnt, LONG fd, ULONGLONG new_size) {
  if (fd < 0 || fd >= SMKFS_FD_MAX)
    return SMKFS_ERR_INVAL;
  if (!mnt->fd_table[fd].used)
    return SMKFS_ERR_INVAL;

  return smkfs_truncate(mnt, mnt->fd_table[fd].record_id, new_size);
}

static LONG punc_collect_cb(SMKFS_ATTR_ID attr_id, PVOID data, SIZE_T len,
                            PVOID ctx) {
  _SMKFS_EXT_REMOVE_CTX *c = (_SMKFS_EXT_REMOVE_CTX *)ctx;

  (VOID) attr_id;

  if (len != sizeof(_SMKFS_EXTENT)) {
    return 0;
  }

  if (c->count >= 32) {
    return 0;
  }

  c->extents[c->count++] = *(_SMKFS_EXTENT *)data;
  return 0;
}

SMKFS_STATUS smkfs_punc_hole(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                             SMKFS_OFFSET offset, SIZE_T len) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf;
  _SMKFS_RECORD rec;
  ULONGLONG *fsize_ptr;
  ULONGLONG file_size;
  SMKFS_LBLOCK hole_start, hole_end;
  _SMKFS_EXTENT all_ext[32];
  _SMKFS_EXT_REMOVE_CTX collect;
  PVOID ext_data;
  SIZE_T ext_len;
  SMKFS_STATUS ret;

  if (!mnt->mounted || record_id == 0 || len == 0) {
    return SMKFS_ERR_INVAL;
  }

  attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }

  if (record_read(mnt, record_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type != SMKFS_ROT_FILE) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (record_find_attr(attr_buf, SMKFS_ATTRT_FSIZE, (PVOID *)&fsize_ptr,
                       NULL) != SMKFS_OK) {
    file_size = 0;
  } else {
    file_size = *fsize_ptr;
  }

  if (offset >= file_size) {
    free(attr_buf);
    return SMKFS_OK;
  }

  if (offset + len > file_size) {
    len = (SIZE_T)(file_size - offset);
  }

  hole_start = (SMKFS_LBLOCK)(offset / SMKFS_BLOCK_SIZE);
  hole_end =
      (SMKFS_LBLOCK)((offset + len + SMKFS_BLOCK_SIZE - 1) / SMKFS_BLOCK_SIZE);

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  collect.extents = all_ext;
  collect.count = 0;
  record_iterate_attr(attr_buf, SMKFS_ATTRT_EXTENTS, punc_collect_cb, &collect);

  while (record_find_attr(attr_buf, SMKFS_ATTRT_EXTENTS, &ext_data, &ext_len) ==
         SMKFS_OK) {
    record_remove_attr(attr_buf, SMKFS_ATTRT_EXTENTS);
  }

  for (ULONG i = 0; i < collect.count; i++) {
    _SMKFS_EXTENT *e = &all_ext[i];
    SMKFS_LBLOCK ext_start = e->logical_offset;
    SMKFS_LBLOCK ext_end = ext_start + e->block_count;

    if (ext_end <= hole_start || ext_start >= hole_end) {
      if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS, e,
                          sizeof(*e)) != SMKFS_OK) {
        journal_abort(mnt);
        free(attr_buf);
        return SMKFS_ERR_NOSPC;
      }
      continue;
    }

    if (ext_start >= hole_start && ext_end <= hole_end) {
      bitmap_free_range(mnt, e->physical_block, e->block_count);
      continue;
    }

    if (ext_start < hole_start && ext_end > hole_end) {
      ULONGLONG left_cnt = hole_start - ext_start;
      ULONGLONG hole_cnt = hole_end - hole_start;
      ULONGLONG right_cnt = ext_end - hole_end;
      _SMKFS_EXTENT left = *e;
      _SMKFS_EXTENT right = *e;

      left.block_count = (ULONG)left_cnt;
      if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS, &left,
                          sizeof(left)) != SMKFS_OK) {
        journal_abort(mnt);
        free(attr_buf);
        return SMKFS_ERR_NOSPC;
      }

      bitmap_free_range(mnt, e->physical_block + left_cnt, (ULONG)hole_cnt);

      right.logical_offset = hole_end;
      right.physical_block = e->physical_block + left_cnt + hole_cnt;
      right.block_count = (ULONG)right_cnt;
      if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS, &right,
                          sizeof(right)) != SMKFS_OK) {
        journal_abort(mnt);
        free(attr_buf);
        return SMKFS_ERR_NOSPC;
      }
      continue;
    }

    if (ext_start < hole_start && ext_end <= hole_end) {
      ULONGLONG keep_cnt = hole_start - ext_start;
      ULONGLONG free_cnt = ext_end - hole_start;

      e->block_count = (ULONG)keep_cnt;
      if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS, e,
                          sizeof(*e)) != SMKFS_OK) {
        journal_abort(mnt);
        free(attr_buf);
        return SMKFS_ERR_NOSPC;
      }
      bitmap_free_range(mnt, e->physical_block + keep_cnt, (ULONG)free_cnt);
      continue;
    }

    if (ext_start >= hole_start && ext_end > hole_end) {
      ULONGLONG free_cnt = hole_end - ext_start;
      ULONGLONG keep_cnt = ext_end - hole_end;

      bitmap_free_range(mnt, e->physical_block, (ULONG)free_cnt);
      e->logical_offset = hole_end;
      e->physical_block = e->physical_block + free_cnt;
      e->block_count = (ULONG)keep_cnt;
      if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_EXTENTS, e,
                          sizeof(*e)) != SMKFS_OK) {
        journal_abort(mnt);
        free(attr_buf);
        return SMKFS_ERR_NOSPC;
      }
      continue;
    }
  }

  rec.header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(attr_buf);
  ret = record_write(mnt, record_id, &rec, attr_buf);
  if (ret != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return ret;
  }

  if (journal_commit(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  free(attr_buf);
  return SMKFS_OK;
}

SMKFS_STATUS smkfs_create_file(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                               SMKFS_PERM permissions) {
  SMKFS_RECORD_ID parent;
  CHAR name[SMKFS_NAME_LEN];
  SMKFS_RECORD_ID new_record;
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf;
  _SMKFS_RECORD rec;
  SMKFS_STATUS ret;

  if (!mnt->mounted || path_validate(path) != SMKFS_OK)
    return SMKFS_ERR_INVAL;

  if (path_split(mnt, path, &parent, name) != SMKFS_OK)
    return SMKFS_ERR_NOTFOUND;

  if (smkfs_create_record(mnt, SMKFS_ROT_FILE, parent, name, &new_record) !=
      SMKFS_OK)
    return SMKFS_ERR_NOSPC;

  if (journal_start_transaction(mnt) != SMKFS_OK)
    return SMKFS_ERR_JOURNAL;

  attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    journal_abort(mnt);
    return SMKFS_ERR_NOMEM;
  }

  if (record_read(mnt, new_record, &rec, attr_buf, buf_size) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (record_add_attr(attr_buf, buf_size, SMKFS_ATTRT_PERMISSIONS, &permissions,
                      sizeof(permissions)) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return SMKFS_ERR_NOSPC;
  }

  rec.attr_count++;
  rec.header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(attr_buf);

  ret = record_write(mnt, new_record, &rec, attr_buf);
  if (ret != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return ret;
  }

  if (journal_commit(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  free(attr_buf);
  return SMKFS_OK;
}
SMKFS_STATUS smkfs_delete_file(_SMKFS_MOUNT *mnt, SMKFS_PATH path) {
  SMKFS_RECORD_ID record_id;
  SMKFS_RECORD_ID parent_id;
  CHAR name[SMKFS_NAME_LEN];

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    return SMKFS_ERR_NOTFOUND;
  }

  if (path_split(mnt, path, &parent_id, name) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  return smkfs_delete_record(mnt, parent_id, name, record_id);
}

SMKFS_STATUS smkfs_mkdir(_SMKFS_MOUNT *mnt, SMKFS_PATH path) {
  SMKFS_RECORD_ID parent;
  CHAR name[SMKFS_NAME_LEN];
  SMKFS_RECORD_ID new_record;

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_split(mnt, path, &parent, name) != SMKFS_OK) {
    return SMKFS_ERR_NOTFOUND;
  }

  return smkfs_create_record(mnt, SMKFS_ROT_DIR, parent, name, &new_record);
}

SMKFS_STATUS smkfs_rmdir(_SMKFS_MOUNT *mnt, SMKFS_PATH path) {
  SMKFS_RECORD_ID record_id;
  SMKFS_RECORD_ID parent_id;
  CHAR name[SMKFS_NAME_LEN];

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    return SMKFS_ERR_NOTFOUND;
  }

  if (path_split(mnt, path, &parent_id, name) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  return smkfs_delete_record(mnt, parent_id, name, record_id);
}