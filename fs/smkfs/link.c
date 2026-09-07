/*
 * fs/smkfs/link.c - [Enter description]
 * Author:   amity
 * Date:     Thu Aug  6 13:38:34 2026
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

SMKFS_STATUS smkfs_link(_SMKFS_MOUNT *mnt, SMKFS_PATH existing_path,
                        SMKFS_PATH new_path) {
  SMKFS_RECORD_ID existing_id;
  SMKFS_RECORD_ID new_parent;
  SMKFS_RECORD_ID tmp_id;
  CHAR new_name[SMKFS_NAME_LEN];
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  PUCHAR parent_attr = (PUCHAR)malloc(buf_size);
  if (!attr_buf || !parent_attr) {
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD rec;
  _SMKFS_RECORD parent_rec;
  SMKFS_BLOCK *parent_btree;
  SMKFS_BLOCK new_root;

  if (!mnt->mounted || path_validate(existing_path) != SMKFS_OK ||
      path_validate(new_path) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_INVAL;
  }

  /* Resolve the existing record */
  if (path_lookup(mnt, existing_path, &existing_id) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_NOTFOUND;
  }

  if (record_read(mnt, existing_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_IO;
  }

  /* G1: hard links to directories are forbidden */
  if (rec.object_type == SMKFS_ROT_DIR) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_INVAL;
  }

  /* Target must not already exist */
  if (path_lookup(mnt, new_path, &tmp_id) == SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_EXISTS;
  }

  /* Split new path into parent directory and name */
  if (path_split(mnt, new_path, &new_parent, new_name) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_INVAL;
  }

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_JOURNAL;
  }

  /* Read parent directory and locate its B+ tree root */
  if (record_read(mnt, new_parent, &parent_rec, parent_attr, buf_size) !=
      SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_IO;
  }

  if (record_find_attr(parent_attr, SMKFS_ATTRT_DATA, (PVOID *)&parent_btree,
                       NULL) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_NOTFOUND;
  }

  /* Insert the new directory entry */
  new_root = *parent_btree;
  if (btree_insert(mnt, *parent_btree, new_name, existing_id, &new_root) !=
      SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_NOSPC;
  }

  record_add_attr(parent_attr, buf_size, SMKFS_ATTRT_DATA, &new_root,
                  sizeof(new_root));

  if (record_write(mnt, new_parent, &parent_rec, parent_attr) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_IO;
  }

  /* Increment link_count on the target record */
  rec.link_count++;
  if (record_write(mnt, existing_id, &rec, attr_buf) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_IO;
  }

  journal_commit(mnt);
  printk("[SmKFS] Linked %s -> %s (id %llu)", existing_path, new_path,
         existing_id);
  free(attr_buf);
  free(parent_attr);
  return SMKFS_OK;
}

SMKFS_STATUS smkfs_unlink(_SMKFS_MOUNT *mnt, SMKFS_PATH path) {
  SMKFS_RECORD_ID record_id;
  SMKFS_RECORD_ID parent_id;
  CHAR name[SMKFS_NAME_LEN];
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }

  _SMKFS_RECORD rec;

  if (!mnt->mounted || path_validate(path) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &record_id) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_NOTFOUND;
  }

  if (path_split(mnt, path, &parent_id, name) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (record_read(mnt, record_id, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  /* Directories must be removed via smkfs_rmdir() */
  if (rec.object_type == SMKFS_ROT_DIR) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }
  free(attr_buf);
  return smkfs_delete_record(mnt, parent_id, name, record_id);
}