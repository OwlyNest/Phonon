/*
 * fs/smkfs/dir.c - Directory Operations (G1)
 * Author:   amity
 * Date:     Wed Jul 29 17:38:51 2026
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
#include <lib/string.h>
#include <mm/heap.h>
#include <screen/printk.h>

/* --- Typedefs - Structs - Enums ---*/

/* --- Globals ---*/

/* --- Prototypes ---*/

/* --- Functions ---*/

SMKFS_STATUS smkfs_lookup_by_name(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID dir_record,
                                  SMKFS_NAME name,
                                  SMKFS_RECORD_ID *out_record) {
  PUCHAR attr_buf;
  _SMKFS_RECORD rec;
  SMKFS_BLOCK *root_block;
  SMKFS_STATUS status;
  SIZE_T attr_sz = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);

  if (!mnt->mounted || !name || !out_record) {
    return SMKFS_ERR_INVAL;
  }
  attr_buf = (PUCHAR)malloc(attr_sz);
  if (!attr_buf) {
    return SMKFS_ERR_NOMEM;
  }

  if (record_read(mnt, dir_record, &rec, attr_buf, attr_sz) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type != SMKFS_ROT_DIR) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (record_find_attr(attr_buf, SMKFS_ATTRT_DATA, (PVOID *)&root_block,
                       NULL) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_NOTFOUND;
  }

  status = btree_search(mnt, *root_block, name, out_record);
  free(attr_buf);
  return status;
}

SMKFS_STATUS smkfs_create_record(_SMKFS_MOUNT *mnt,
                                 SMKFS_OBJECT_TYPE object_type,
                                 SMKFS_RECORD_ID parent_dir, SMKFS_NAME name,
                                 SMKFS_RECORD_ID *out_record) {
  SMKFS_STATUS ret = SMKFS_ERR_IO;
  SIZE_T attr_buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR parent_attr;
  PUCHAR new_attr;
  _SMKFS_RECORD parent_rec;
  SMKFS_RECORD_ID new_id;
  SMKFS_BLOCK btree_root = 0;
  SMKFS_BLOCK *parent_btree;

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    return SMKFS_ERR_JOURNAL;
  }

  parent_attr = (PUCHAR)malloc(attr_buf_size);
  new_attr = (PUCHAR)malloc(attr_buf_size);
  if (!parent_attr || !new_attr) {
    free(parent_attr);
    free(new_attr);
    journal_commit(mnt);
    return SMKFS_ERR_NOMEM;
  }

  if (!mnt->mounted || !name || !out_record) {
    goto cleanup;
  }

  new_id = record_alloc(mnt, object_type);
  if (new_id == 0) {
    printk("[SmKFS] create_record: record_alloc failed\n");
    goto cleanup;
  }

  SMKFS_GENERATION new_gen;
  if (mrt_resolve(mnt, new_id, NULL, NULL, &new_gen) != SMKFS_OK)
    goto cleanup;

  if (object_type == SMKFS_ROT_DIR) {
    PUCHAR empty_leaf;
    _SMKFS_BTREE_NODE *node;

    empty_leaf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
    if (!empty_leaf) {
      record_free(mnt, new_id);
      goto cleanup;
    }

    memset(empty_leaf, 0, SMKFS_BLOCK_SIZE);
    node = (_SMKFS_BTREE_NODE *)empty_leaf;
    header_init(&node->header, SMKFS_ST_BTREE_NODE, sizeof(_SMKFS_BTREE_NODE),
                SMKFS_BTN_LEAF | SMKFS_BTN_ROOT);
    node->parent_block = 0;
    node->flags = SMKFS_BTN_LEAF;
    node->key_count = 0;
    node->right_sibling = 0;
    header_checksum_update(&node->header, empty_leaf,
                           sizeof(_SMKFS_BTREE_NODE));

    btree_root = bitmap_alloc(mnt);
    if (btree_root == 0) {
      record_free(mnt, new_id);
      free(empty_leaf);
      goto cleanup;
    }

    if (write_block(mnt, btree_root, empty_leaf) != SMKFS_OK) {
      bitmap_clear(mnt, btree_root);
      record_free(mnt, new_id);
      free(empty_leaf);
      goto cleanup;
    }

    free(empty_leaf);
  }

  if (record_read(mnt, parent_dir, &parent_rec, parent_attr, attr_buf_size) !=
      SMKFS_OK) {
    if (btree_root) {
      bitmap_clear(mnt, btree_root);
    }
    record_free(mnt, new_id);
    goto cleanup;
  }

  if (record_find_attr(parent_attr, SMKFS_ATTRT_DATA, (PVOID *)&parent_btree,
                       NULL) != SMKFS_OK) {
    printk("[SmKFS] create_record: DATA attr missing on parent %llu\n",
           parent_dir);
    if (btree_root)
      bitmap_clear(mnt, btree_root);
    record_free(mnt, new_id);
    goto cleanup;
  }

  SMKFS_BLOCK new_root = *parent_btree;
  SMKFS_STATUS ret2 = btree_insert(mnt, *parent_btree, name, new_id, &new_root);
  if (ret2 != SMKFS_OK) {
    printk("[SmKFS] create_record: btree_insert failed, root=%llu, %d\n",
           *parent_btree, ret2);
    if (btree_root)
      bitmap_clear(mnt, btree_root);
    record_free(mnt, new_id);
    goto cleanup;
  }

  memset(new_attr, 0, attr_buf_size);
  _SMKFS_ATTR_HEADER term;
  term.type = SMKFS_ATTRT_END;
  term.flags = 0;
  term.id = 0;
  term.length = 0;
  memcpy(new_attr, &term, sizeof(term));

  record_add_attr(new_attr, attr_buf_size, SMKFS_ATTRT_PARENT, &parent_dir,
                  sizeof(parent_dir));
  if (btree_root) {
    record_add_attr(new_attr, attr_buf_size, SMKFS_ATTRT_DATA, &btree_root,
                    sizeof(btree_root));
  }

  _SMKFS_RECORD new_rec;
  header_init(&new_rec.header, SMKFS_ST_RECORD,
              sizeof(_SMKFS_RECORD) + sizeof(_SMKFS_ATTR_HEADER), 0);
  new_rec.record_id = new_id;
  new_rec.object_type = object_type;
  new_rec.attr_count = btree_root ? 2 : 1;
  new_rec.link_count = 1;
  new_rec.generation = new_gen; /* What's the point of tracking generation if
                                   alloctating resets it? */
  new_rec.header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(new_attr);

  if (record_write(mnt, new_id, &new_rec, new_attr) != SMKFS_OK) {
    printk("[SmKFS] create_record: record_write(new_id=%llu) failed\n", new_id);
    if (btree_root)
      bitmap_clear(mnt, btree_root);
    record_free(mnt, new_id);
    goto cleanup;
  }

  record_add_attr(parent_attr, attr_buf_size, SMKFS_ATTRT_DATA, &new_root,
                  sizeof(new_root));
  parent_rec.header.length =
      sizeof(_SMKFS_RECORD) + attr_buf_total_len(parent_attr);
  if (record_write(mnt, parent_dir, &parent_rec, parent_attr) != SMKFS_OK) {
    printk("[SmKFS] create_record: record_write(parent_dir=%llu) failed\n",
           parent_dir);
    record_free(mnt, new_id);
    goto cleanup;
  }

  *out_record = new_id;
  ret = SMKFS_OK;

cleanup:
  free(parent_attr);
  free(new_attr);
  if (ret == SMKFS_OK) {
    journal_commit(mnt);
  } else {
    journal_abort(mnt);
  }

  return ret;
}

SMKFS_STATUS smkfs_delete_record(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID parent_dir,
                                 SMKFS_NAME name, SMKFS_RECORD_ID record_id) {
  PUCHAR attr_buf = NULL;
  PUCHAR parent_attr = NULL;
  _SMKFS_RECORD rec;
  _SMKFS_RECORD parent_rec;
  SMKFS_BLOCK *parent_btree;
  SMKFS_BLOCK new_root;
  SIZE_T attr_sz = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);

  if (!mnt->mounted || record_id == 0 || !name) {
    return SMKFS_ERR_INVAL;
  }

  attr_buf = (PUCHAR)malloc(attr_sz);
  parent_attr = (PUCHAR)malloc(attr_sz);
  if (!attr_buf || !parent_attr) {
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_NOMEM;
  }

  if (record_read(mnt, record_id, &rec, attr_buf, attr_sz) != SMKFS_OK) {
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type == SMKFS_ROT_DIR) {
    SMKFS_BLOCK *dir_btree;
    if (record_find_attr(attr_buf, SMKFS_ATTRT_DATA, (PVOID *)&dir_btree,
                         NULL) == SMKFS_OK) {
      _SMKFS_BTREE_NODE node;
      if (btree_node_read(mnt, *dir_btree, &node, NULL, 0) == SMKFS_OK) {
        if (node.key_count > 0) {
          free(attr_buf);
          free(parent_attr);
          return SMKFS_ERR_NOTEMPTY;
        }
      }
    }
  }

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_JOURNAL;
  }

  if (record_read(mnt, parent_dir, &parent_rec, parent_attr, attr_sz) !=
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

  new_root = *parent_btree;
  {
    SMKFS_STATUS del_ret = btree_delete(mnt, *parent_btree, name, &new_root);
    if (del_ret != SMKFS_OK) {
      journal_abort(mnt);
      free(attr_buf);
      free(parent_attr);
      return del_ret;
    }
  }

  if (record_add_attr(parent_attr, attr_sz, SMKFS_ATTRT_DATA, &new_root,
                      sizeof(new_root)) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_NOSPC;
  }

  parent_rec.header.length =
      sizeof(_SMKFS_RECORD) + attr_buf_total_len(parent_attr);
  if (record_write(mnt, parent_dir, &parent_rec, parent_attr) != SMKFS_OK) {
    journal_abort(mnt);
    free(attr_buf);
    free(parent_attr);
    return SMKFS_ERR_IO;
  }

  if (rec.link_count == 0) {
    rec.link_count = 1;
  }
  rec.link_count--;

  if (rec.link_count == 0) {
    record_free(mnt, record_id);
  } else {
    if (record_write(mnt, record_id, &rec, attr_buf) != SMKFS_OK) {
      journal_abort(mnt);
      free(attr_buf);
      free(parent_attr);
      return SMKFS_ERR_IO;
    }
  }

  free(attr_buf);
  free(parent_attr);

  if (journal_commit(mnt) != SMKFS_OK) {
    return SMKFS_ERR_JOURNAL;
  }

  return SMKFS_OK;
}

SMKFS_STATUS smkfs_rename(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID old_parent,
                          SMKFS_NAME old_name, SMKFS_RECORD_ID new_parent,
                          SMKFS_NAME new_name, SMKFS_RECORD_ID record_id) {
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR rec_attr_buf = (PUCHAR)malloc(buf_size);
  PUCHAR old_parent_attr = (PUCHAR)malloc(buf_size);
  PUCHAR new_parent_attr = (PUCHAR)malloc(buf_size);
  if (!rec_attr_buf || !old_parent_attr || !new_parent_attr) {
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD rec, old_parent_rec, new_parent_rec;
  SMKFS_BLOCK *old_btree_ptr;
  SMKFS_BLOCK *new_btree_ptr;
  SMKFS_BLOCK new_root;

  if (!mnt->mounted || !old_name || !new_name || record_id == 0) {
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_INVAL;
  }

  /* Pure reads – keep them outside the transaction */
  if (record_read(mnt, record_id, &rec, rec_attr_buf, buf_size) != SMKFS_OK) {
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_IO;
  }

  /*
   * Cycle check: Renaming a directory under one of it's own descendants
   * would make the tree unreachable and leave rmdir permanently stuck.
   * Walk the PARENT chain from new_parent; if we meet record_id, reject.
   * Only directories can form cycles (hard links to dirs are forbidden).
   */
  if (rec.object_type == SMKFS_ROT_DIR) {
    SMKFS_RECORD_ID walk = new_parent;
    _SMKFS_RECORD walk_rec;
    ULONG depth = 0;

    if (walk == record_id) {
      free(rec_attr_buf);
      free(old_parent_attr);
      free(new_parent_attr);
      return SMKFS_ERR_INVAL;
    }

    while (walk != 0 && walk != mnt->sb.root_record_id && depth < 256) {
      PUCHAR walk_attr =
          (PUCHAR)malloc(SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD));
      SMKFS_RECORD_ID *parent_ptr;

      if (!walk_attr)
        break;

      if (record_read(mnt, walk, &walk_rec, walk_attr, buf_size) != SMKFS_OK) {
        free(walk_attr);
        break;
      }

      if (record_find_attr(walk_attr, SMKFS_ATTRT_PARENT, (PVOID *)&parent_ptr,
                           NULL) != SMKFS_OK) {
        free(walk_attr);
        break;
      }

      walk = *parent_ptr;
      free(walk_attr); /* always free before next iteration */

      if (walk == record_id) {
        free(rec_attr_buf);
        free(old_parent_attr);
        free(new_parent_attr);
        return SMKFS_ERR_INVAL;
      }
      depth++;
    }
  }

  if (journal_start_transaction(mnt) != SMKFS_OK) {
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_JOURNAL;
  }

  /* --- Remove old directory entry --- */
  if (record_read(mnt, old_parent, &old_parent_rec, old_parent_attr,
                  buf_size) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_IO;
  }

  if (record_find_attr(old_parent_attr, SMKFS_ATTRT_DATA,
                       (PVOID *)&old_btree_ptr, NULL) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOTFOUND;
  }

  new_root = *old_btree_ptr;
  {
    SMKFS_STATUS del_ret =
        btree_delete(mnt, *old_btree_ptr, old_name, &new_root);
    if (del_ret != SMKFS_OK) {
      journal_abort(mnt);
      free(rec_attr_buf);
      free(old_parent_attr);
      free(new_parent_attr);
      return del_ret;
    }
  }

  if (record_add_attr(old_parent_attr, buf_size, SMKFS_ATTRT_DATA, &new_root,
                      sizeof(new_root)) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOSPC;
  }

  old_parent_rec.header.length =
      sizeof(_SMKFS_RECORD) + attr_buf_total_len(old_parent_attr);
  if (record_write(mnt, old_parent, &old_parent_rec, old_parent_attr) !=
      SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_IO;
  }

  /* --- Insert new directory entry --- */
  if (record_read(mnt, new_parent, &new_parent_rec, new_parent_attr,
                  buf_size) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_IO;
  }

  if (record_find_attr(new_parent_attr, SMKFS_ATTRT_DATA,
                       (PVOID *)&new_btree_ptr, NULL) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOTFOUND;
  }

  new_root = *new_btree_ptr;
  if (btree_insert(mnt, *new_btree_ptr, new_name, record_id, &new_root) !=
      SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOSPC;
  }

  if (record_add_attr(new_parent_attr, buf_size, SMKFS_ATTRT_DATA, &new_root,
                      sizeof(new_root)) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOSPC;
  }

  new_parent_rec.header.length =
      sizeof(_SMKFS_RECORD) + attr_buf_total_len(new_parent_attr);
  if (record_write(mnt, new_parent, &new_parent_rec, new_parent_attr) !=
      SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_IO;
  }

  /* --- Update PARENT attribute on the record itself --- */
  if (record_add_attr(rec_attr_buf, buf_size, SMKFS_ATTRT_PARENT, &new_parent,
                      sizeof(new_parent)) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_NOSPC;
  }

  rec.header.length = sizeof(_SMKFS_RECORD) + attr_buf_total_len(rec_attr_buf);
  if (record_write(mnt, record_id, &rec, rec_attr_buf) != SMKFS_OK) {
    journal_abort(mnt);
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_IO;
  }

  if (journal_commit(mnt) != SMKFS_OK) {
    free(rec_attr_buf);
    free(old_parent_attr);
    free(new_parent_attr);
    return SMKFS_ERR_JOURNAL;
  }

  free(rec_attr_buf);
  free(old_parent_attr);
  free(new_parent_attr);
  return SMKFS_OK;
}

static LONG readdir_cb(PCCHAR key, ULONGLONG value, PVOID ctx) {
  _SMKFS_READDIR_CTX *c = (_SMKFS_READDIR_CTX *)ctx;
  if (c->count >= c->max) {
    return 1;
  }
  strncpy(c->entries[c->count].name, key, SMKFS_NAME_LEN - 1);
  c->entries[c->count].name[SMKFS_NAME_LEN - 1] = '\0';
  c->entries[c->count].record_id = value;
  c->count++;
  return 0;
}

SMKFS_STATUS smkfs_readdir(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                           _SMKFS_DIRENT *entries, SIZE_T max_entries,
                           SIZE_T *out_count) {
  SMKFS_RECORD_ID dir_record;
  SIZE_T buf_size = SMKFS_BLOCK_SIZE - sizeof(_SMKFS_RECORD);
  PUCHAR attr_buf = (PUCHAR)malloc(buf_size);
  if (!attr_buf) {
    free(attr_buf);
    return SMKFS_ERR_NOMEM;
  }
  _SMKFS_RECORD rec;
  SMKFS_BLOCK *btree_root;
  _SMKFS_READDIR_CTX ctx;

  if (!mnt->mounted || !path || !entries || !out_count) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (path_validate(path) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }

  if (path_lookup(mnt, path, &dir_record) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_NOTFOUND;
  }
  if (record_read(mnt, dir_record, &rec, attr_buf, buf_size) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_IO;
  }

  if (rec.object_type != SMKFS_ROT_DIR) {
    free(attr_buf);
    return SMKFS_ERR_INVAL;
  }
  if (record_find_attr(attr_buf, SMKFS_ATTRT_DATA, (PVOID *)&btree_root,
                       NULL) != SMKFS_OK) {
    free(attr_buf);
    return SMKFS_ERR_NOTFOUND;
  }

  ctx.entries = entries;
  ctx.max = max_entries;
  ctx.count = 0;

  btree_iterate(mnt, *btree_root, readdir_cb, &ctx);

  *out_count = ctx.count;
  free(attr_buf);
  return SMKFS_OK;
}