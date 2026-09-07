/*
 * fs/smkfs/inode.c - Inode Attribute Operations
 * Author:   amity
 * Date:     Wed Jul 29 17:39:00 2026
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
#include <mm/heap.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/

SMKFS_STATUS smkfs_getattr(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                           _SMKFS_RECORD *rec, PVOID attr_buf,
                           SIZE_T buf_size) {
  if (!mnt->mounted || !rec || !attr_buf || record_id == 0) {
    return SMKFS_ERR_INVAL;
  }

  return record_read(mnt, record_id, rec, attr_buf, buf_size);
}

SMKFS_STATUS smkfs_setattr(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                           SMKFS_ATTR_TYPE attr_type, PCVOID data, SIZE_T len) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD rec;
  SMKFS_STATUS ret;

  if (!mnt->mounted || !data || record_id == 0) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (record_read(mnt, record_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_JOURNAL;
  }

  if (record_add_attr(attr_buf, buf_size, attr_type, data, len) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    return SMKFS_ERR_NOSPC;
  }

  /* UNIQUE attributes replace the previous instance; non-unique add one.
     Recompute so the in-memory count stays accurate. */
  rec.attr_count = 0;
  {
    PUCHAR ptr = attr_buf;
    while (1) {
      _SMKFS_ATTR_HEADER *ah = (_SMKFS_ATTR_HEADER *)ptr;
      if (ah->type == SMKFS_ATTRT_END) {
        break;
      }

      rec.attr_count++;
      ptr += sizeof(_SMKFS_ATTR_HEADER) + ah->length;
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

SMKFS_STATUS smkfs_stat(_SMKFS_MOUNT *mnt, SMKFS_PATH path, _SMKFS_RECORD *rec,
                        PVOID attr_buf, SIZE_T buf_size) {
  SMKFS_RECORD_ID record_id;

  if (!mnt->mounted || !rec || !attr_buf || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    return SMKFS_ERR_NOTFOUND;
  }

  return smkfs_getattr(mnt, record_id, rec, attr_buf, buf_size);
}

SMKFS_STATUS smkfs_chmod(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                         SMKFS_PERM permissions) {
  SMKFS_RECORD_ID record_id;

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    return SMKFS_ERR_NOTFOUND;
  }

  return smkfs_setattr(mnt, record_id, SMKFS_ATTRT_PERMISSIONS, &permissions,
                       sizeof(permissions));
}

SMKFS_STATUS smkfs_chown(_SMKFS_MOUNT *mnt, SMKFS_PATH path, ULONG uid,
                         ULONG gid) {
  SMKFS_RECORD_ID record_id;
  ULONGLONG owner = ((ULONGLONG)uid << 32) | gid;

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    return SMKFS_ERR_NOTFOUND;
  }

  return smkfs_setattr(mnt, record_id, SMKFS_ATTRT_OWNER, &owner,
                       sizeof(owner));
}