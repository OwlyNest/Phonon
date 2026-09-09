/*
 * include/fs/smkfs.h - SmKFS Public API Header (G1)
 * Author:   amity
 * Date:     Thu Jul 23 11:40:08 2026
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

/*
 * Two-hours-in-the-future-Amity:
 *     Aww, look who's been reading Linux source code
 */

/*
 *
 * This file defines the global constants, structures, on-disk layout and public
 * API of the SmKFS file system
 *
 * First is a set of macros used by the internal and public API.
 *
 * Secondly are structures, the on-disk structures are intentionally packed to
 * ensure predictable and deterministic disk layout The cannonical header that
 * precedes all on-disk structures is intentionally expaned to 32 Bytes to
 * ensure alignment with any type of data that follows it
 *
 * Thirdly are the prototypes for the public API
 * It is divided into four levels, level 4 being internal and not exposed in
 * this file
 *
 *  Level 3 is the Kernel-interface, these functions are called by the (future)
 * VFS. These functions operate on structures and record_id's, not paths.
 *
 *  Level 2 is the User-interface, these are functions that a user program would
 * call These functions operate on paths, using lookup functions to translate
 * paths into record_id (which are translated to physical blocks by the MRT)
 *
 *  Level 1 is the Administrative-interface, these functions, create, check,
 * repair and dump the file system
 *
 */

/*
 * Every "ground" (version) comes with it's own design rules:
 *
 * Golden Design Rules (G0)
 *
 * 1. Everything is a record.
 * 2. Records consist of typed attributes.
 * 3. Extents are the sole mechanism for non-resident data.
 * 4. Metadata is journaled.
 * 5. Small files remain resident whenever possible.
 * 6. Directories are indexed using B+ Trees.
 * 7. Allocation favors contiguous extents through delayed allocation.
 * 8. New functionality is added through attributes, not structural revisions.
 * 9. The on-disk format must remain understandable and debuggable.
 * 10. Reliability takes precedence over clever optimization.
 * 11. Everything on disk should be self-describing.
 *
 *
 * Golden Design Rules (G1)
 *
 * 1. Everything is a record with a **logical identity**.
 * 2. Physical location is managed by the **MRT**, never assumed by callers.
 * 3. Attributes are **multi-valued**; type + id uniquely identifies an
 * attribute instance.
 * 4. **Names live in directories**, not records. Records have link counts.
 * 5. Extents are **per-attribute instances**, not monolithic arrays.
 * 6. Metadata changes are **atomic via journaling**; user data is not
 * journaled.
 * 7. The journal is **redo-only**, circular, and checkpointed.
 * 8. The B+ tree is **generic**; directory entries are one use case.
 * 9. Allocation is **regioned**; contiguous extents are preferred but not
 * required.
 * 10. All state is **per-mount**; globals are forbidden.
 * 11. Every structure is **self-describing** with a canonical header and
 * CRC32C.
 * 12. **Corruption is detected early**; invalid structures panic the mount,
 * never propagate.
 *
 */

/* --- Macros --- */
#ifndef __FS_SMKFS_H__
#define __FS_SMKFS_H__

/* Magic And Version */
#define SMKFS_MAGIC "SmKF"
#define SMKFS_VERSION 2

/*
 * SMKFS Format Versions
 *
 * SMKFS versions correspond to development "Grounds":
 *
 *     Ground 0 → version 1
 *     Ground 1 → version 2
 *     Ground 2 → version 3
 *     ...
 *
 * A new Ground represents an (intentionally in)compatible filesystem
 * format revision. Compatibility with previous Grounds is not guaranteed.
 */

#define SMKFS_NAME_LEN 256
#define SMKFS_PATH_LEN 4096
/*
 * Same as Linux and Windows, with a distinction, this currently includes the
 * entire path, not just the file name Which is a problem for later.
 * Better[[[]]]
 */
#define SMKFS_REGION_BLOCKS 256

/* Block Size */
// #define SMKFS_SECTOR_SIZE       512
/*
 * Newer storage devices use 4 KiB sectors, CR-ROM's use 2 KiB, This should
 * honestly be set by the storage device Solved, sector_size gets queried from
 * the storage device and written to the superblock
 */

#define SMKFS_BLOCK_SIZE 4096
/*
 * The block.c I/O functions read/write a block, the storage device understands
 * sectors, so this number is used to convert a number of blocks into a number
 * of sectors. If block size and sector size are not divisible (powers of two)
 * then the read/write functions fail
 *
 */

/* Structure Types
 *
 * SmKFS knows a small set of on-disk structures.
 * This structure type is stored in the header.
 * Both sets of design rules hold "everything is a record" at the top. Which is
 * not fully correct (yet) What holds absolute is that everything on-disk is a
 * structure and starts with the same header A record is an on-disk structure
 * with a logical ID, ROT (Record Object Type), and a set of attributes. This is
 * fundamentally differrent from B+ tree nodes and the Superblock.
 *
 */
#define SMKFS_ST_SUPERBLOCK 0x0001
#define SMKFS_ST_RECORD 0x0002
#define SMKFS_ST_BTREE_NODE 0x0003
#define SMKFS_ST_JOURNAL_HDR 0x0004
#define SMKFS_ST_JOURNAL_ENT 0x0005
#define SMKFS_ST_BITMAP 0x0006
#define SMKFS_ST_ALLOC_META 0x0007
#define SMKFS_ST_MRT 0x0008
#define SMKFS_ST_MAT 0x0009

/* Record Object Types
 *
 *
 */
#define SMKFS_ROT_FILE 0x01
#define SMKFS_ROT_DIR 0x02
#define SMKFS_ROT_SYMLINK 0x03
#define SMKFS_ROT_DEVICE 0x04

/* Attribute Types
 *
 *
 */
#define SMKFS_ATTRT_END 0x0000
#define SMKFS_ATTRT_NAME 0x0001
#define SMKFS_ATTRT_DATA 0x0002
#define SMKFS_ATTRT_FSIZE 0x0003
#define SMKFS_ATTRT_PERMISSIONS 0x0004
#define SMKFS_ATTRT_OWNER 0x0005
#define SMKFS_ATTRT_TIMESTAMPS 0x0006
#define SMKFS_ATTRT_PARENT 0x0007
#define SMKFS_ATTRT_EXTENTS 0x0008
#define SMKFS_ATTRT_SYMLINK 0x0009
#define SMKFS_ATTRT_DEVICE 0x000A

/* Attribute Behavior Flags
 *
 *
 */
#define SMKFS_ATTRF_UNIQUE 0x0001
#define SMKFS_ATTRF_REQUIRED 0x0002
#define SMKFS_ATTRF_RESIDENT 0x0004

/* Flags
 *
 *
 */
#define SMKFS_FLA_RESIDENT 0x00000001
#define SMKFS_FLA_DELETED 0x00000002
#define SMKFS_FLA_COMPRESSED 0x00000004
#define SMKFS_FLA_ENCRYPTED 0x00000008

/* MRT entry Flags
 *
 *
 */
#define SMKFS_MRTF_FREE UINT64_MAX
#define SMKFS_MRTF_ALLOCATED 0x0001
#define SMKFS_MRTF_OVERFLOW 0x0002
#define SMKFS_MRTF_DELETED 0x0004

/* B+ Tree Nodes
 *
 *
 */
#define SMKFS_BTR_MAX_KEY 255
#define SMKFS_BTN_LEAF 0x1
#define SMKFS_BTN_ROOT 0x2

/* Directory ENntry Flags
 *
 *
 */
#define SMKFS_DENTF_NORMAL 0x0000
#define SMKFS_DENTF_DOT 0x0001
#define SMKFS_DENTF_DOTDOT 0x0002

/* Journal OPerations
 *
 *
 */
#define SMKFS_JOP_WRITE 1
#define SMKFS_JOP_ALLOC 2
#define SMKFS_JOP_FREE 3
#define SMKFS_JOP_COMMIT 4
#define SMKFS_JOP_MRT_UPDATE 5
#define SMKFS_JOP_CHECKPOINT 6

/* Superblock Flags
 *
 *
 */
#define SMKFS_SBF_CLEAN 0x00000001
#define SMKFS_SBF_ERRORS 0x00000002

/* File Descriptor
 *
 *
 */
#define SMKFS_FD_MAX 16

/* Open flags
 *
 *
 */
#define SMKFS_O_RDONLY 0x0000
#define SMKFS_O_WRONLY 0x0001
#define SMKFS_O_RDWR 0x0002
#define SMKFS_O_CREATE 0x0004
#define SMKFS_O_TRUNC 0x0008
#define SMKFS_O_APPEND 0x0010

/* SEEK flags
 *
 *
 */
#define SMKFS_SEEK_SET 0
#define SMKFS_SEEK_CUR 1
#define SMKFS_SEEK_END 2

/* Permissions
 *
 *
 */
#define SMKFS_PERM_WRITE 0x0080
#define SMKFS_PERM_EXEC 0x0040
#define SMKFS_PERM_READ 0x0100

/* Block buffer cache entry
 *
 *
 */
#define SMKFS_CACHE_SLOTS 16

/* Error Codes
 *
 *
 */
typedef enum {
  SMKFS_OK = 0,
  SMKFS_ERR_IO = -1,
  SMKFS_ERR_NOMEM = -2,
  SMKFS_ERR_NOTFOUND = -3,
  SMKFS_ERR_EXISTS = -4,
  SMKFS_ERR_NOSPC = -5,
  SMKFS_ERR_INVAL = -6,
  SMKFS_ERR_CORRUPT = -7,
  SMKFS_ERR_NOTEMPTY = -8,
  SMKFS_ERR_ROFS = -9,
  SMKFS_ERR_JOURNAL = -10,
  SMKFS_ERR_TOO_BIG = -11,
  SMKFS_ERR_NOT_YET_BOUND = -12,
} SMKFS_STATUS;

/* --- Includes --- */
#include <internal/phonon_macros.h>
#include <stddef.h>
#include <stdint.h>

/* --- Semantic Types --- */

/*
 * These aliases exist to prevent mixing values that share the same
 * underlying integer width but represent different domains.
 *
 * Using SMKFS_RECORD_ID instead of ULONGLONG makes it impossible to
 * accidentally pass a block number where a record ID is expected.
 */

typedef ULONGLONG SMKFS_RECORD_ID; /* Logical record identifier (MRT slot) */
typedef ULONGLONG SMKFS_BLOCK;     /* Physical block number on storage */
typedef ULONGLONG SMKFS_LBLOCK;    /* Logical block offset within a stream */
typedef ULONGLONG SMKFS_OFFSET;    /* Byte offset within a file */
typedef ULONG SMKFS_GENERATION;    /* MRT entry generation counter */
typedef USHORT SMKFS_STRUCT_TYPE;  /* On-disk structure type (SMKFS_ST_*) */
typedef USHORT SMKFS_OBJECT_TYPE;  /* Record object type (SMKFS_ROT_*) */
typedef USHORT SMKFS_ATTR_TYPE;    /* Attribute type (SMKFS_ATTRT_*) */
typedef ULONG SMKFS_ATTR_ID;       /* Attribute instance id */
typedef USHORT SMKFS_ATTR_FLAGS;   /* Attribute behavior flags */
typedef USHORT SMKFS_MRT_FLAGS;    /* MRT entry flags (SMKFS_MRTF_*) */
typedef USHORT SMKFS_DIRENT_FLAGS; /* Directory entry flags (SMKFS_DENTF_*) */
typedef ULONG SMKFS_JOP;           /* Journal operation code (SMKFS_JOP_*) */
typedef ULONG SMKFS_SBF;           /* Superblock flags (SMKFS_SBF_*) */
typedef SHORT SMKFS_PERM;          /* Permission bitmask (SMKFS_PERM_*) */
typedef const char *SMKFS_PATH;    /* Path */
typedef const char *SMKFS_NAME;    /* Path */

/* --- Typedefs - Structs - Enums --- */

/*
 * SMKFS canonical object header.
 *
 * Persistent on-disk structure; exactly 32 bytes.
 *
 * This structure is part of the on-disk format and MUST NOT change
 * without a filesystem format version change.
 *
 * All multi-byte integer fields are stored in little-endian byte order.
 * The structure is exactly 32 bytes.
 *
 * Layout:
 *   0x00  magic[4]      Object signature ("SmKF")
 *   0x04  version       SMKFS format version
 *   0x06  type          Object type (SMKFS_ST_*)
 *   0x08  length        Size of the object payload, including including
 * ~~excluding~~ this header 0x0C  flags         Object-specific flags 0x10
 * checksum      CRC32C checksum 0x14  reserved[3]   Reserved; MUST be written
 * as zero and ignored
 *
 * The total serialized object size is:
 *
 *     sizeof(_SMKFS_HEADER) + header.length
 *
 * `length` is the size of the data following this header. It is NOT
 * the size of the complete object and is unrelated to a file's logical
 * size (which is stored in the FSIZE attribute).
 *
 * Every persistent SMKFS structure begins with this header. The header
 * allows the filesystem to identify the structure type, validate its
 * version, determine its serialized size, and verify its contents.
 *
 * Currently defined structure types include:
 *   - Superblock
 *   - Record
 *   - B+ tree node
 *   - Journal header
 *   - Journal entry
 *   - Bitmap
 *   - Allocated metadata
 *   - Master Record Table
 *   - Master Attribute Table
 *
 *
 * Might be important to clarify the difference between "Structure" "Record" and
 * "File" Going NT-Style and declaring "everything is a Record" might be
 * easiest. Or I formalize what a Structure is... extra complexity. Yay!
 */
typedef struct {
  /*  0 */ CHAR magic[4]; /* SMKFS magic: "SmKF" (SMKFS_MAGIC) */
  /*  4 */ SHORT version; /* On-disk format version (SMKFS_VERSION) */
  /*  6 */ SHORT type;    /* Structure type (SMKFS_ST_*) */
  /*  8 */ ULONG
      length; /* Payload length in bytes; includes ~~excludes~~ header */
  /* 12 */ ULONG flags;       /* Structure-specific flags */
  /* 16 */ ULONG checksum;    /* CRC32C(header with checksum field zeroed) */
  /* 20 */ ULONG reserved[3]; /* Reserved; MUST be zero */
} __attribute__((__packed__)) _SMKFS_HEADER;

_Static_assert(sizeof(_SMKFS_HEADER) == 32, "SMKFS header size changed");

/*
 * Master Record Table entry.
 *
 * Persistent on-disk structure; exactly 16 bytes.
 *
 * Each entry describes the physical location and allocation state of
 * an MRT slot. An entry with physical_block == UINT64_MAX represents an
 * unallocated/free slot.
 * Physical block 0 is permanently reserved for the superblock and will never
 * appear as an MRT entry's physical_block.
 *
 * Using UINT64_MAX as unallocated can only accidentally write to the block at
 * 64 ZiB.
 *
 * `generation` is incremented when the MRT slot is reused. This allows
 * code using MRT references to distinguish a current occupant from a
 * previous occupant of the same slot.
 *
 * `reserved` is reserved for future use and MUST be written as zero.
 *
 * The physical block value 0 is reserved for the Superblock and
 * therefore MUST NOT identify an allocated MRT entry. Unallocated will be -1
 * (UINT64_MAX).
 */

typedef struct {
  /*  0 */ SMKFS_BLOCK
      physical_block;                   /* UINT64_MAX = no block assigned.
                                           Block 0 is the Superblock and is always
                                           a valid physical block reference. */
  /*  8 */ SMKFS_MRT_FLAGS flags;       /* SMKFS_MRTF_* */
  /* 10 */ SHORT reserved;              /* Reserved; MUST be zero */
  /* 12 */ SMKFS_GENERATION generation; /* Incremented on slot reuse */
} __attribute__((__packed__)) _SMKFS_MRT_ENTRY;
_Static_assert(sizeof(_SMKFS_MRT_ENTRY) == 16, "SMKFS MRT entry size changed");

/* Extent (20B) */
typedef struct {
  SMKFS_LBLOCK logical_offset;
  SMKFS_BLOCK physical_block;
  ULONG block_count;
} __attribute__((__packed__)) _SMKFS_EXTENT;
_Static_assert(sizeof(_SMKFS_EXTENT) == 20, "SmKFS Extent size changed");

/* Attribute header (12B) */
typedef struct {
  SMKFS_ATTR_TYPE type;
  SMKFS_ATTR_FLAGS flags;
  SMKFS_ATTR_ID id;
  ULONG length;
} __attribute__((__packed__)) _SMKFS_ATTR_HEADER;
_Static_assert(sizeof(_SMKFS_ATTR_HEADER) == 12,
               "SmKFS Attribute Header size changed");

/* Attribute behavior definition (in-memory only) */
typedef struct {
  SMKFS_ATTR_TYPE type;
  SMKFS_NAME name;
  SMKFS_ATTR_FLAGS flags;
  SIZE_T fixed_size;
  SMKFS_STATUS (*validate)(PCVOID data, SIZE_T len);
  VOID (*debug_print)(PCVOID data, SIZE_T len);
} _SMKFS_ATTR_DEF;

/* B+ tree node header (56B) */
typedef struct {
  _SMKFS_HEADER header;
  SMKFS_BLOCK parent_block;
  ULONG flags;
  ULONG key_count;
  SMKFS_BLOCK right_sibling;
} __attribute__((__packed__)) _SMKFS_BTREE_NODE;
_Static_assert(sizeof(_SMKFS_BTREE_NODE) == 56, "B+ tree node size changed");

/* On-disk leaf entry (264B): filename -> record ID */
typedef struct {
  SMKFS_RECORD_ID record_id;
  CHAR name[SMKFS_NAME_LEN];
} _SMKFS_BTREE_LEAF_ENTRY;
_Static_assert(sizeof(_SMKFS_BTREE_LEAF_ENTRY) == 264,
               "SmKFS B+ tree leaf entry size changed");

/*
 * On-disk internal (index) node entry (264B).
 *
 * Each entry pairs a child pointer with that child's fence key:
 * the lowest key reachable through child_block. An internal node
 * with key_count == n therefore has exactly n children, and entry
 * 0's key is the lowest key of the whole subtree.
 *
 * The full 256-byte key is stored (no prefix truncation) so that
 * names sharing more than a few leading bytes still separate
 * correctly; leaf and index entries are deliberately the same size.
 */
typedef struct {
  SMKFS_BLOCK child_block;
  CHAR key[SMKFS_NAME_LEN];
} _SMKFS_BTREE_INDEX_ENTRY;
_Static_assert(sizeof(_SMKFS_BTREE_INDEX_ENTRY) == 264,
               "SmKFS B+ tree index entry size changed");

/*
 * Node capacities are derived from the block and entry sizes, so they
 * stay correct if the layouts ever change.
 *
 * A full node holds MAX entries; after a delete a non-root node must
 * still hold at least MIN entries, otherwise it borrows an entry from
 * a sibling or merges with one.
 */

enum {
  SMKFS_BTR_LEAF_MAX = (SMKFS_BLOCK_SIZE - sizeof(_SMKFS_BTREE_NODE)) /
                       sizeof(_SMKFS_BTREE_LEAF_ENTRY),
  SMKFS_BTR_INDEX_MAX = (SMKFS_BLOCK_SIZE - sizeof(_SMKFS_BTREE_NODE)) /
                        sizeof(_SMKFS_BTREE_INDEX_ENTRY),
  SMKFS_BTR_LEAF_MIN = SMKFS_BTR_LEAF_MAX / 2,
  SMKFS_BTR_INDEX_MIN = SMKFS_BTR_INDEX_MAX / 2
};

/*
 * Result of a recursive insert: whether the node split, and if so the
 * freshly allocated right neighbor plus its low key, which the parent
 * stores as the separator (fence) for the new child.
 */

typedef struct {
  LONG split;
  SMKFS_BLOCK right_block;
  CHAR sep_key[SMKFS_NAME_LEN];
} _SMKFS_BTREE_SPLIT;

/* Record header v2 (56B) */
typedef struct {
  _SMKFS_HEADER header;
  SMKFS_RECORD_ID record_id; /* Logical ID (MRT index) */
  SMKFS_OBJECT_TYPE object_type;
  SHORT attr_count;
  ULONG link_count;            /* Directory entries pointing here */
  SMKFS_GENERATION generation; /* Matches MRT generation */
  ULONG reserved;              /* Reserved; MUST be zero */
} __attribute__((__packed__)) _SMKFS_RECORD;
_Static_assert(sizeof(_SMKFS_RECORD) == 56, "SmKFS Record size changed");

/* Journal entry v2 (64B) */
typedef struct {
  _SMKFS_HEADER header;
  ULONGLONG sequence;
  SMKFS_BLOCK target_block;
  SMKFS_JOP operation;
  ULONG data_length;
  SMKFS_RECORD_ID record_id; /* Logical record for fsck */
} __attribute__((__packed__)) _SMKFS_JOURNAL_ENTRY;
_Static_assert(sizeof(_SMKFS_JOURNAL_ENTRY) == 64,
               "SmKFS Journal entry size changed");

/* Superblock v2 (320 bytes) */
typedef struct {
  _SMKFS_HEADER header;
  ULONGLONG total_blocks;
  ULONGLONG free_blocks;
  ULONGLONG sector_size;
  ULONGLONG record_count;
  SMKFS_RECORD_ID next_record_id; /* Next free MRT slot */
  SMKFS_BLOCK mrt_start;
  ULONGLONG mrt_length;   /* Blocks reserved for MRT */
  ULONGLONG mrt_capacity; /* Max records (MRT entries) */
  ULONGLONG mrt_free_count;
  SMKFS_RECORD_ID root_record_id; /* Logical ID of root dir */
  SMKFS_BLOCK journal_start;
  ULONGLONG journal_length;
  ULONGLONG journal_head;     /* Next free journal slot */
  ULONGLONG journal_tail;     /* Oldest uncheckpointed entry */
  ULONGLONG journal_sequence; /* Monotonic transaction counter */
  SMKFS_BLOCK bitmap_start;
  ULONGLONG bitmap_length;
  SMKFS_BLOCK alloc_meta_start;
  ULONGLONG alloc_meta_length;
  SMKFS_BLOCK data_start;
  ULONG block_size;
  SMKFS_SBF flags; /* SMKFS_SBF_* */
  UCHAR uuid[16];
  CHAR volume_name[64];
  ULONGLONG creation_time;
  ULONGLONG last_mount_time;
  ULONG mount_count;
  ULONG max_mount_count;
  ULONG reserved[4];
} __attribute__((__packed__)) _SMKFS_SUPERBLOCK;
_Static_assert(sizeof(_SMKFS_SUPERBLOCK) == 320,
               "SmKFS Superblock size changed");

/* On-disk directory entry (B+ tree value) (16B) */
typedef struct {
  SMKFS_RECORD_ID record_id;
  ULONG name_hash;          /* Fast comparison filter */
  SMKFS_DIRENT_FLAGS flags; /* SMKFS_DENTF_* */
  SHORT name_len;           /* Actual name length */
} __attribute__((__packed__)) _SMKFS_DIRENT_DISK;
_Static_assert(sizeof(_SMKFS_DIRENT_DISK) == 16,
               "SmKFS Dirent Disk size changed");

/* Directory entry (in-memory, userspace-facing, 264B) */
typedef struct {
  SMKFS_RECORD_ID record_id;
  CHAR name[SMKFS_NAME_LEN];
} _SMKFS_DIRENT;

/* In-memory representation of a hard link.
 * On disk this is just a B+ tree entry in a directory:
 *   key   = name
 *   value = _SMKFS_DIRENT_DISK
 */
typedef struct {
  SMKFS_RECORD_ID record_id;  /* Target record (the "inode") */
  SMKFS_RECORD_ID parent_dir; /* Directory containing this link */
  CHAR name[SMKFS_NAME_LEN];
} _SMKFS_LINK;

/* File Descriptor */
typedef struct {
  LONG used;
  SMKFS_RECORD_ID record_id;
  SMKFS_OFFSET offset;
  LONG flags;
} _SMKFS_FD;

typedef struct {
  ULONG free_count;
  ULONG alloc_hint;
} _SMKFS_REGION;

typedef struct {
  SMKFS_BLOCK block;
  UCHAR data[SMKFS_BLOCK_SIZE];
  LONG dirty;
  LONG valid;
} _SMKFS_BLOCK_BUF;

/* Per-mount context (G1 multi-mount support) */
typedef struct {
  UCHAR drive_num;
  LONG mounted;
  _SMKFS_SUPERBLOCK sb;
  ULONGLONG journal_next_sequence;
  LONG journal_in_transaction;
  ULONGLONG journal_write_pos;
  _SMKFS_FD fd_table[SMKFS_FD_MAX];
  _SMKFS_REGION *regions;
  ULONG region_count;
  ULONG alloc_hint_region;
  _SMKFS_BLOCK_BUF *block_cache; // allocated in block_cache_init
} _SMKFS_MOUNT;

/* Read Directory Context */
typedef struct {
  _SMKFS_DIRENT *entries;
  SIZE_T max;
  SIZE_T count;
} _SMKFS_READDIR_CTX;

/* Itterate Attribute Context */
typedef struct {
  SMKFS_LBLOCK block;
  _SMKFS_EXTENT *out;
  UCHAR found;
} _SMKFS_ATTR_CTX;

typedef struct {
  _SMKFS_EXTENT *extents;
  ULONG count;
} _SMKFS_EXT_REMOVE_CTX;

typedef struct _SMKFS_EXT_MERGE_CTX {
  SMKFS_LBLOCK logical_block;
  SMKFS_BLOCK physical_block;
  ULONG count;
  SMKFS_ATTR_ID matched_id;
  _SMKFS_EXTENT merged;
  int found;
} _SMKFS_EXT_MERGE_CTX;

/* --- Prototypes --- */

/* ~~~ Master Attribute Table ~~~ */
const _SMKFS_ATTR_DEF *smkfs_attr_lookup(SMKFS_ATTR_TYPE type);
SMKFS_NAME smkfs_attr_name(SMKFS_ATTR_TYPE type);
VOID smkfs_attr_debug_print(SMKFS_ATTR_TYPE type, PCVOID data, SIZE_T len);

/* ~~~ Level 3: Kernel ~~~ */
SMKFS_STATUS smkfs_mount(UCHAR drive, _SMKFS_MOUNT *mnt);
SMKFS_STATUS smkfs_unmount(_SMKFS_MOUNT *mnt);
SMKFS_STATUS smkfs_sync(_SMKFS_MOUNT *mnt);
SMKFS_STATUS smkfs_lookup_by_name(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID dir_record,
                                  SMKFS_NAME name, SMKFS_RECORD_ID *out_record);
SMKFS_STATUS smkfs_create_record(_SMKFS_MOUNT *mnt,
                                 SMKFS_OBJECT_TYPE object_type,
                                 SMKFS_RECORD_ID parent_dir, SMKFS_NAME name,
                                 SMKFS_RECORD_ID *out_record);
SMKFS_STATUS smkfs_delete_record(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID parent_dir,
                                 SMKFS_NAME name, SMKFS_RECORD_ID record_id);
SMKFS_STATUS smkfs_rename(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID old_parent,
                          SMKFS_NAME old_name, SMKFS_RECORD_ID new_parent,
                          SMKFS_NAME new_name, SMKFS_RECORD_ID record_id);
LONG smkfs_read(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                SMKFS_OFFSET offset, SIZE_T len, PVOID buf);
LONG smkfs_write(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                 SMKFS_OFFSET offset, SIZE_T len, PCVOID buf);
SMKFS_STATUS smkfs_truncate(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                            ULONGLONG new_size);
SMKFS_STATUS smkfs_punc_hole(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                             SMKFS_OFFSET offset, SIZE_T len);
SMKFS_STATUS smkfs_getattr(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                           _SMKFS_RECORD *rec, PVOID attr_buf, SIZE_T buf_size);
SMKFS_STATUS smkfs_setattr(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id,
                           SMKFS_ATTR_TYPE attr_type, PCVOID data, SIZE_T len);

/* ~~~ Level 2: User ~~~ */
SMKFS_STATUS path_split(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                        SMKFS_RECORD_ID *out_parent, PCHAR out_name);
SMKFS_STATUS path_lookup(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                         SMKFS_RECORD_ID *out_record);
LONG smkfs_open(_SMKFS_MOUNT *mnt, SMKFS_PATH path, LONG flags);
SMKFS_STATUS smkfs_link(_SMKFS_MOUNT *mnt, SMKFS_PATH existing_path,
                        SMKFS_PATH new_path);
SMKFS_STATUS smkfs_unlink(_SMKFS_MOUNT *mnt, SMKFS_PATH path);
SMKFS_STATUS smkfs_close(_SMKFS_MOUNT *mnt, LONG fd);
LONG smkfs_read_file(_SMKFS_MOUNT *mnt, LONG fd, PVOID buf, SIZE_T len);
LONG smkfs_write_file(_SMKFS_MOUNT *mnt, LONG fd, PCVOID buf, SIZE_T len);
LONG smkfs_seek(_SMKFS_MOUNT *mnt, LONG fd, LONGLONG offset, LONG whence);
SMKFS_STATUS smkfs_ftruncate(_SMKFS_MOUNT *mnt, LONG fd, ULONGLONG new_size);
SMKFS_STATUS smkfs_create_file(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                               SMKFS_PERM permissions);
SMKFS_STATUS smkfs_delete_file(_SMKFS_MOUNT *mnt, SMKFS_PATH path);
SMKFS_STATUS smkfs_mkdir(_SMKFS_MOUNT *mnt, SMKFS_PATH path);
SMKFS_STATUS smkfs_rmdir(_SMKFS_MOUNT *mnt, SMKFS_PATH path);
SMKFS_STATUS smkfs_readdir(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                           _SMKFS_DIRENT *entries, SIZE_T max_entries,
                           SIZE_T *out_count);
SMKFS_STATUS smkfs_stat(_SMKFS_MOUNT *mnt, SMKFS_PATH path, _SMKFS_RECORD *rec,
                        PVOID attr_buf, SIZE_T buf_size);
SMKFS_STATUS smkfs_chmod(_SMKFS_MOUNT *mnt, SMKFS_PATH path,
                         SMKFS_PERM permissions);
SMKFS_STATUS smkfs_chown(_SMKFS_MOUNT *mnt, SMKFS_PATH path, ULONG uid,
                         ULONG gid);

/* ~~~ Level 1: Admin ~~~ */
SMKFS_STATUS smkfs_mkfs(UCHAR drive, ULONGLONG total_blocks,
                        ULONGLONG sector_size);
SMKFS_STATUS smkfs_fsck(UCHAR drive);
SMKFS_STATUS smkfs_dump_superblock(_SMKFS_MOUNT *mnt);
SMKFS_STATUS smkfs_dump_record(_SMKFS_MOUNT *mnt, SMKFS_RECORD_ID record_id);
SMKFS_STATUS smkfs_dump_journal(_SMKFS_MOUNT *mnt);
SMKFS_STATUS smkfs_dump_btree(_SMKFS_MOUNT *mnt, SMKFS_BLOCK root_block);
PCCHAR smkfs_strerror(SMKFS_STATUS err);
VOID smkfs_fserror(SMKFS_STATUS err);

#endif