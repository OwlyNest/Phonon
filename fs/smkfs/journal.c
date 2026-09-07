/*
 * fs/smkfs/journal.c - Metadata Journaling (G1)
 * Author:   amity
 * Date:     Wed Jul 29 17:38:18 2026
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
static ULONGLONG journal_next_pos(_SMKFS_MOUNT *mnt, ULONGLONG pos);
static LONG journal_is_full(_SMKFS_MOUNT *mnt);
static SMKFS_STATUS journal_write_entry(_SMKFS_MOUNT *mnt, SMKFS_JOP operation,
                                        SMKFS_BLOCK target_block,
                                        ULONG data_length,
                                        SMKFS_RECORD_ID record_id,
                                        PCVOID payload);
static SMKFS_STATUS journal_redo_entry(_SMKFS_MOUNT *mnt,
                                       const _SMKFS_JOURNAL_ENTRY *ent,
                                       PCVOID payload);

/* --- Functions ---*/

/* Advance a journal position with wrap-around */
static ULONGLONG journal_next_pos(_SMKFS_MOUNT *mnt, ULONGLONG pos) {
  pos++;
  if (pos >= mnt->sb.journal_length) {
    pos = 0;
  }

  return pos;
}

/* Journal is full when the next write would overwrite the tail */
static LONG journal_is_full(_SMKFS_MOUNT *mnt) {
  return journal_next_pos(mnt, mnt->sb.journal_head) == mnt->sb.journal_tail;
}

/*
 * Common path used by every log_* function.
 * Allocates a full block, fills a journal entry + optional payload,
 * writes it at sb.journal_head and advances the head.
 */

static SMKFS_STATUS journal_write_entry(_SMKFS_MOUNT *mnt, SMKFS_JOP operation,
                                        SMKFS_BLOCK target_block,
                                        ULONG data_length,
                                        SMKFS_RECORD_ID record_id,
                                        PCVOID payload) {
  PUCHAR buf;
  _SMKFS_JOURNAL_ENTRY *ent;
  SIZE_T total_len;
  SMKFS_STATUS ret;

  if (!mnt->journal_in_transaction) {
    return SMKFS_ERR_JOURNAL;
  }

  if (journal_is_full(mnt)) {
    return SMKFS_ERR_NOSPC;
  }

  if (data_length > SMKFS_BLOCK_SIZE - sizeof(_SMKFS_JOURNAL_ENTRY)) {
    return SMKFS_ERR_TOO_BIG;
  }

  total_len = sizeof(_SMKFS_JOURNAL_ENTRY) + data_length;

  buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    return SMKFS_ERR_NOMEM;
  }

  memset(buf, 0, SMKFS_BLOCK_SIZE);

  ent = (_SMKFS_JOURNAL_ENTRY *)buf;
  header_init(&ent->header, SMKFS_ST_JOURNAL_ENT, (ULONG)total_len, 0);
  ent->sequence = mnt->journal_next_sequence++;
  ent->target_block = target_block;
  ent->operation = operation;
  ent->data_length = data_length;
  ent->record_id = record_id;

  if (data_length > 0 && payload) {
    memcpy(buf + sizeof(_SMKFS_JOURNAL_ENTRY), payload, data_length);
  }

  header_checksum_update(&ent->header, buf, ent->header.length);

  ret = write_block(mnt, mnt->sb.journal_start + mnt->sb.journal_head, buf);
  free(buf);

  if (ret != SMKFS_OK) {
    printk("[SmKFS] journal_write_entry FAILED at journal block %llu\n",
           mnt->sb.journal_start + mnt->sb.journal_head);
    return SMKFS_ERR_IO;
  }

  mnt->sb.journal_head = journal_next_pos(mnt, mnt->sb.journal_head);
  mnt->journal_write_pos = mnt->sb.journal_head;

  return SMKFS_OK;
}

SMKFS_STATUS journal_start_transaction(_SMKFS_MOUNT *mnt) {
  if (mnt->journal_in_transaction) {
    return SMKFS_ERR_JOURNAL;
  }

  /* Journal should be empty after every commit/checkpoint.
     If it is not, reclaim it first. */
  if (mnt->sb.journal_head != mnt->sb.journal_tail) {
    if (journal_checkpoint(mnt) != SMKFS_OK) {
      return SMKFS_ERR_JOURNAL;
    }
  }

  /* Also refuse if a single free slot is all that remains */
  if (journal_is_full(mnt)) {
    if (journal_checkpoint(mnt) != SMKFS_OK) {
      return SMKFS_ERR_NOSPC;
    }
    if (journal_is_full(mnt)) {
      return SMKFS_ERR_NOSPC;
    }
  }

  mnt->journal_in_transaction = 1;
  return SMKFS_OK;
}

/*
 * Pure redo: only the *new* data is stored.
 * old_data is accepted for API compatibility but ignored.
 */

SMKFS_STATUS journal_log_write(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block,
                               PCVOID old_data, PCVOID new_data, SIZE_T len) {
  (void)old_data;

  if (len > 0 && !new_data)
    return SMKFS_ERR_INVAL;

  return journal_write_entry(mnt, SMKFS_JOP_WRITE, block, (ULONG)len, 0,
                             new_data);
}

SMKFS_STATUS journal_log_alloc(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block,
                               ULONG count) {
  return journal_write_entry(mnt, SMKFS_JOP_ALLOC, block, count, 0, NULL);
}

SMKFS_STATUS journal_log_free(_SMKFS_MOUNT *mnt, SMKFS_BLOCK block,
                              ULONG count) {
  return journal_write_entry(mnt, SMKFS_JOP_FREE, block, count, 0, NULL);
}

static SMKFS_STATUS journal_persist_superblock(_SMKFS_MOUNT *mnt) {
  PUCHAR block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!block) {
    return SMKFS_ERR_NOMEM;
  }
  SMKFS_STATUS ret;

  memset(block, 0, SMKFS_BLOCK_SIZE);
  memcpy(block, &mnt->sb, sizeof(mnt->sb));
  header_checksum_update(&((_SMKFS_SUPERBLOCK *)block)->header, block,
                         sizeof(_SMKFS_SUPERBLOCK));

  ret = write_block(mnt, 0, block);
  if (ret != SMKFS_OK) {
    free(block);
    return SMKFS_ERR_IO;
  }

  /* Make sure the superblock itself is on stable storage */
  free(block);
  return block_cache_flush(mnt);
}

SMKFS_STATUS journal_commit(_SMKFS_MOUNT *mnt) {
  SMKFS_STATUS ret;

  if (!mnt->journal_in_transaction) {
    return SMKFS_ERR_JOURNAL;
  }

  ret = journal_write_entry(mnt, SMKFS_JOP_COMMIT, 0, 0, 0, NULL);
  if (ret != SMKFS_OK) {
    return ret;
  }

  mnt->sb.journal_sequence = mnt->journal_next_sequence;
  mnt->journal_in_transaction = 0;

  /* Journal blocks must be durable before we publish the new head */
  ret = block_cache_flush(mnt);
  if (ret != SMKFS_OK) {
    return ret;
  }

  /* Publish the new head/sequence so recovery can see this transaction */
  ret = journal_persist_superblock(mnt);
  if (ret != SMKFS_OK) {
    return ret;
  }

  /*
   * Everything is now durable on the final locations.
   * There is nothing left to redo, so the journal can be considered empty.
   * Advancing the tail here is what prevents the circular buffer from
   * filling up during a long-running mount.
   */

  mnt->sb.journal_tail = mnt->sb.journal_head;
  mnt->journal_write_pos = mnt->sb.journal_head;

  /* Persist the new tail as well */
  return journal_persist_superblock(mnt);
}

/*
 * Apply a single committed journal entry (redo).
 * Called only for entries that belong to a transaction that reached COMMIT.
 */

static SMKFS_STATUS journal_redo_entry(_SMKFS_MOUNT *mnt,
                                       const _SMKFS_JOURNAL_ENTRY *ent,
                                       PCVOID payload) {
  ULONG i;
  SMKFS_STATUS ret;

  switch (ent->operation) {
  case SMKFS_JOP_WRITE:
    if (ent->data_length == 0) {
      return SMKFS_OK;
    }

    /* Write the logged new data to its final location */
    ret = write_block(mnt, ent->target_block, payload);
    if (ret != SMKFS_OK) {
      return SMKFS_ERR_IO;
    }

    break;

  case SMKFS_JOP_ALLOC:
    /* data_length holds the block count */
    for (i = 0; i < ent->data_length; i++) {
      bitmap_set(mnt, ent->target_block + i);
    }

    break;

  case SMKFS_JOP_FREE:
    for (i = 0; i < ent->data_length; i++) {
      bitmap_clear(mnt, ent->target_block + i);
    }

    break;

  case SMKFS_JOP_COMMIT:
  case SMKFS_JOP_CHECKPOINT:
    /* Nothing to redo for these markers */
    break;

  case SMKFS_JOP_MRT_UPDATE: {
    PUCHAR mrt_block = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
    if (!mrt_block) {
      free(mrt_block);
      return SMKFS_ERR_NOMEM;
    }
    _SMKFS_MRT_ENTRY *entries;
    ULONG entries_per_block;
    SMKFS_BLOCK block_id;
    ULONG entry_id;

    if (ent->data_length != sizeof(_SMKFS_MRT_ENTRY) || !payload) {
      free(mrt_block);
      return SMKFS_ERR_CORRUPT;
    }

    if (ent->record_id >= mnt->sb.mrt_capacity) {
      free(mrt_block);
      return SMKFS_ERR_CORRUPT;
    }

    entries_per_block = SMKFS_BLOCK_SIZE / sizeof(_SMKFS_MRT_ENTRY);
    block_id = mnt->sb.mrt_start + ent->record_id / entries_per_block;
    entry_id = (ULONG)(ent->record_id % entries_per_block);

    if (read_block(mnt, block_id, mrt_block) != SMKFS_OK) {
      free(mrt_block);
      return SMKFS_ERR_IO;
    }

    entries = (_SMKFS_MRT_ENTRY *)mrt_block;
    entries[entry_id] = *(_SMKFS_MRT_ENTRY *)payload;

    if (write_block(mnt, block_id, mrt_block) != SMKFS_OK) {
      free(mrt_block);
      return SMKFS_ERR_IO;
    }
    free(mrt_block);
    break;
  }

  default:
    printk("[SmKFS] journal_redo: unknown op %u seq %llu\n", ent->operation,
           ent->sequence);
    return SMKFS_ERR_CORRUPT;
  }

  return SMKFS_OK;
}

/*
 * Redo-only journal recovery.
 *
 * 1. Scan the circular buffer from tail → head.
 * 2. Discover the highest sequence number that has a COMMIT.
 * 3. Re-apply every entry whose sequence ≤ that value (in order).
 * 4. Discard everything after the last commit.
 * 5. Advance tail = head so the journal is clean.
 *
 * Called from smkfs_mount before any other metadata work.
 */

SMKFS_STATUS journal_replay(_SMKFS_MOUNT *mnt) {
  PUCHAR buf = (PUCHAR)malloc(SMKFS_BLOCK_SIZE);
  if (!buf) {
    return SMKFS_ERR_NOMEM;
  }
  ULONGLONG pos;
  ULONGLONG last_commit_seq;
  LONG found_commit;
  SMKFS_STATUS ret;
  _SMKFS_JOURNAL_ENTRY *ent;
  PCVOID payload;

  /* Empty journal? */
  if (mnt->sb.journal_head == mnt->sb.journal_tail) {
    mnt->journal_next_sequence = mnt->sb.journal_sequence;
    mnt->journal_write_pos = mnt->sb.journal_head;

    free(buf);
    return SMKFS_OK;
  }

  /* ---------- Pass 1: find the last committed sequence ---------- */
  last_commit_seq = 0;
  found_commit = 0;
  pos = mnt->sb.journal_tail;

  while (pos != mnt->sb.journal_head) {
    memset(buf, 0, SMKFS_BLOCK_SIZE);
    ret = read_block(mnt, mnt->sb.journal_start + pos, buf);
    if (ret != SMKFS_OK) {
      printk("[SmKFS] journal_replay: read failed at slot %llu\n", pos);

      free(buf);
      return SMKFS_ERR_IO;
    }

    ent = (_SMKFS_JOURNAL_ENTRY *)buf;

    if (header_validate(&ent->header, SMKFS_ST_JOURNAL_ENT) != SMKFS_OK ||
        header_checksum_verify(&ent->header, buf, ent->header.length) !=
            SMKFS_OK) {
      printk("[SmKFS] journal_replay: corrupt entry at slot %llu\n", pos);

      free(buf);
      return SMKFS_ERR_CORRUPT;
    }

    if (ent->operation == SMKFS_JOP_COMMIT) {
      if (ent->sequence >= last_commit_seq) {
        last_commit_seq = ent->sequence;
        found_commit = 1;
      }
    }

    pos = journal_next_pos(mnt, pos);
  }

  if (!found_commit) {
    /* Nothing ever reached COMMIT – discard the whole journal */
    printk("[SmKFS] journal_replay: no COMMIT found, discarding journal\n");
    goto clean;
  }

  /* ---------- Pass 2: redo every entry up to last_commit_seq ---------- */
  pos = mnt->sb.journal_tail;

  while (pos != mnt->sb.journal_head) {
    memset(buf, 0, SMKFS_BLOCK_SIZE);
    ret = read_block(mnt, mnt->sb.journal_start + pos, buf);
    if (ret != SMKFS_OK) {

      free(buf);
      return SMKFS_ERR_IO;
    }

    ent = (_SMKFS_JOURNAL_ENTRY *)buf;

    /* Re-validate (cheap and safe) */
    if (header_validate(&ent->header, SMKFS_ST_JOURNAL_ENT) != SMKFS_OK ||
        header_checksum_verify(&ent->header, buf, ent->header.length) !=
            SMKFS_OK) {

      free(buf);
      return SMKFS_ERR_CORRUPT;
    }

    if (ent->sequence <= last_commit_seq) {
      payload = (ent->data_length > 0)
                    ? (PCVOID)(buf + sizeof(_SMKFS_JOURNAL_ENTRY))
                    : NULL;

      ret = journal_redo_entry(mnt, ent, payload);
      if (ret != SMKFS_OK) {
        printk("[SmKFS] journal_replay: redo failed seq %llu op %u\n",
               ent->sequence, ent->operation);

        free(buf);
        return ret;
      }
    }

    pos = journal_next_pos(mnt, pos);
  }

  printk("[SmKFS] journal_replay: redid up to sequence %llu\n",
         last_commit_seq);

clean:
  /* Journal is now clean – advance tail to head */
  mnt->sb.journal_tail = mnt->sb.journal_head;
  mnt->journal_write_pos = mnt->sb.journal_head;
  mnt->journal_next_sequence = mnt->sb.journal_sequence;

  /* Make sure any blocks we just rewrote are durable */
  block_cache_flush(mnt);

  free(buf);
  return SMKFS_OK;
}

SMKFS_STATUS journal_abort(_SMKFS_MOUNT *mnt) {
  if (!mnt->journal_in_transaction) {
    return SMKFS_OK; /* already clean */
  }

  mnt->journal_in_transaction = 0;
  /* Do NOT write a COMMIT and do NOT advance head.
     The next mount’s replay will see no COMMIT and discard
     everything after the previous tail. */
  return SMKFS_OK;
}

SMKFS_STATUS journal_log_mrt_update(_SMKFS_MOUNT *mnt,
                                    SMKFS_RECORD_ID record_id,
                                    const _SMKFS_MRT_ENTRY *entry) {
  if (!entry) {
    return SMKFS_ERR_INVAL;
  }
  return journal_write_entry(mnt, SMKFS_JOP_MRT_UPDATE, 0,
                             sizeof(_SMKFS_MRT_ENTRY), record_id, entry);
}

/*
 * journal_checkpoint
 *
 * Make the filesystem consistent on stable storage.
 * Must not be called while a transaction is open.
 *
 * Steps:
 *   1. Refuse if a transaction is in progress.
 *   2. Flush the block cache (all dirty metadata + data).
 *   3. If the journal is non-empty (should not happen after a
 *      normal commit), reclaim it by advancing the tail.
 *   4. Optionally write a CHECKPOINT marker for the audit trail.
 *   5. Persist the superblock.
 */
SMKFS_STATUS journal_checkpoint(_SMKFS_MOUNT *mnt) {
  SMKFS_STATUS ret;

  if (!mnt) {
    return SMKFS_ERR_INVAL;
  }

  if (mnt->journal_in_transaction) {
    return SMKFS_ERR_JOURNAL;
  }

  /* 1. Everything that is still only in the cache must get on disk */
  ret = block_cache_flush(mnt);
  if (ret != SMKFS_OK) {
    return ret;
  }

  /* 2. Reclaim any leftover journal entries (belt-and-suspenders) */
  if (mnt->sb.journal_head != mnt->sb.journal_tail) {
    mnt->sb.journal_tail = mnt->sb.journal_head;
    mnt->journal_write_pos = mnt->sb.journal_head;
  }

  /*
   * 3. Write a CHECKPOINT marker so the journal history shows
   *    a clear consistency point.  We temporarily open a
   *    one-entry "transaction" just for the marker.
   */

  mnt->journal_in_transaction = 1;
  ret = journal_write_entry(mnt, SMKFS_JOP_CHECKPOINT, 0, 0, 0, NULL);
  mnt->journal_in_transaction = 0;

  if (ret != SMKFS_OK) {
    /* Marker is best-effort; still try to persist the SB */
    printk("[SmKFS] checkpoint: could not write marker (%d)\n", ret);
  } else {
    /* The marker itself is durable; reclaim again */
    mnt->sb.journal_tail = mnt->sb.journal_head;
    mnt->journal_write_pos = mnt->sb.journal_head;
    mnt->sb.journal_sequence = mnt->journal_next_sequence;
  }

  /* 4. Publish the new tail / sequence */
  ret = journal_persist_superblock(mnt);
  if (ret != SMKFS_OK) {
    return ret;
  }

  return SMKFS_OK;
}