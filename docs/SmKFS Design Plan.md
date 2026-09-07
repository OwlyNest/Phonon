# SmKFS Ground Zero
## Shadow-mode Kernel File System
### Architecture Specification v0.1

## 1. Design Philosophy

SmKFS is the native filesystem of the Shadow kernel and the default filesystem for Phonon.

It is designed around five principles:

- simplicity of implementation
- long-term extensibility
- predictable performance
- strong metadata integrity
- efficient storage utilization

Rather than inheriting historical constraints from existing filesystems, SmKFS adopts proven concepts from ext4 and NTFS while exposing a clean, modern on-disk format.

The filesystem is record-oriented, extent-based, and attribute-driven.

---

# 2. Core Goals

The filesystem shall:

- support files of arbitrary size
- support directories containing millions of entries
- minimize fragmentation
- minimize metadata overhead
- recover cleanly after unexpected shutdown
- remain extensible without incompatible format revisions

Everything else is secondary.

---

# 3. Filesystem Philosophy

Every filesystem object is a **record**.

Records describe objects.

Objects do not directly describe storage.

Storage is allocated independently by the allocator.

This creates a clear separation between:

```
Filesystem Object

↓

Metadata

↓

Logical Data Layout

↓

Allocator

↓

Physical Storage
```

Each layer has a single responsibility.

---

# 4. Records

Every object begins life as a record.

Objects include:

- files
- directories
- symbolic links
- device nodes
- future object types

A record contains a collection of typed attributes.

A record never has fixed metadata fields beyond its mandatory header.

---

# 5. Attributes

Attributes are the fundamental building blocks of SmKFS.

Examples include:

- Name
- Data
- Security
- Permissions
- Timestamps
- Owner
- Extended metadata
- Integrity information

The filesystem never assumes a fixed collection beyond those required for basic operation.

New functionality is introduced by defining additional attribute types rather than modifying record layouts.

---

# 6. Variable Sized Records

Records are variable-sized.

Small objects consume little metadata.

Large metadata structures may expand naturally without forcing overflow records until necessary.

Benefits:

- improved space efficiency
- fewer metadata extensions
- better cache utilization
- easier future expansion

---

# 7. Resident Data

Very small files may store their contents directly inside the record.

Example:

```
notes.txt

Header
Attributes
File Data (37 bytes)
```

No separate storage allocation is required.

When data exceeds available record space, it automatically becomes extent-backed.

---

# 8. Extents

SmKFS is extent-native.

All non-resident data is described using extents.

An extent describes:

```
Logical Offset

↓

Physical Block

↓

Length
```

No indirect blocks exist.

No legacy pointer trees exist.

Every large file is simply a list of extents.

---

# 9. Logical vs Physical Layout

File metadata describes logical ordering only.

The allocator determines physical placement.

```
Logical File

Extent A
Extent B
Extent C

↓

Allocator

↓

Disk Locations
```

This separation allows future allocators to optimize independently for HDDs, SSDs, NVMe, or other storage media without changing the file format.

---

# 10. Allocation

Storage allocation is handled by a dedicated allocator.

Primary goals:

- contiguous allocation
- delayed allocation
- fragmentation avoidance
- locality preservation

Allocation decisions should occur as late as practical.

---

# 11. Free Space Management

Free storage is tracked using allocation bitmaps.

Each allocation unit has one corresponding bit.

```
0 = Free

1 = Allocated
```

Bitmap scanning provides efficient allocation and simplifies integrity checking.

---

# 12. Directories

Directories are indexed structures.

Linear directory scans are never the primary lookup mechanism.

Directory contents are stored using B+ Trees.

Keys:

```
Filename
```

Values:

```
Record Identifier
```

This provides near logarithmic lookup regardless of directory size.

---

# 13. Journaling

SmKFS journals metadata.

The journal guarantees filesystem consistency following crashes or unexpected power loss.

User file data is not required to be journaled.

Metadata integrity has priority over complete write ordering of user data.

---

# 14. Locality

Objects that are likely to be accessed together should be stored near one another whenever practical.

Examples:

- directory and children
- metadata and extents
- neighboring files

This concept is inspired by ext4 block groups but is not tied to a specific physical layout.

---

# 15. Extensibility

SmKFS shall never require structural redesign for new features.

Future additions should be implemented through:

- new attribute types
- new record types
- optional metadata structures

Backward compatibility is maintained by allowing unknown attributes to be safely ignored when appropriate.

---

# 16. Scalability

SmKFS is designed to scale from small removable media to multi-terabyte storage.

The architecture assumes:

- millions of files
- extremely large directories
- large contiguous allocations
- future storage technologies

No architectural limits should depend on assumptions common in legacy filesystems.

---

# 17. Golden Design Rules (G0)

1. Everything is a record.
2. Records consist of typed attributes.
3. Extents are the sole mechanism for non-resident data.
4. Metadata is journaled.
5. Small files remain resident whenever possible.
6. Directories are indexed using B+ Trees.
7. Allocation favors contiguous extents through delayed allocation.
8. New functionality is added through attributes, not structural revisions.
9. The on-disk format must remain understandable and debuggable.
10. Reliability takes precedence over clever optimization.
11. Everything on disk should be self-describing.

18. Self-Describing Structures

In accordance with Design Rule 11, every major on-disk structure shall begin with a common header that identifies the structure and provides sufficient information for validation and parsing. Every structure in the file system starts with exactly the same cannonical header:
struct SmKFS_Header{
    char     Magic[4];
    uint16_t Version;
    uint16_t Type;
    uint32_t Length;
    uint32_t Flags;
    uint32_t Checksum;
}

This leaves no special cases, no guessing, after reading the first ~20 bytes the parser know all information about the the object that it needs.

Additional fields may be included where required by the structure.

This requirement applies to all primary filesystem structures, including but not limited to:
- Records
- B+ Tree nodes
- Journal entries
- Allocation bitmaps
- Allocator metadata
- Superblocks
- Future structure types

The purpose of this design is to allow the filesystem driver, recovery utilities, and diagnostic tools to identify and validate any structure directly from its on-disk representation without relying on external context.

Self-describing structures improve:
- corruption detection
- filesystem recovery
- offline consistency checking
- forward compatibility
- version migration
- debugging and forensic analysis

# Checksum
The checksum covers the entire structure; header + payload. The checksum field itself is treated as zero during computation.
Process for writing:
1. Fill in all fields of the structure
2. Set checksum = 0
3. Compute checksum over length bytes starting from the header
4. Store result in checksum
5. Write to disk

Process for reading/verifying:
1. Read structure from disk
2. Save checksum value
3. Set checksum field to zero in memory
4. Compute checksum over length bytes
5. Compare with saved value

**Important**: length is the total on-disk size of the structure (header + payload). Not just the header. Not just the payload. The checksum validates the entire self-describing unit.  
**Algorithm**: A simple CRC-like approach: accumulate with a polynomial, rotate left by one, XOR with byte. It's robust enough for corruption detection and trivial to implement.

# Records
**Record storage semantics**: Records live in the data area. record_alloc finds free blocks via the bitmap, writes a record there, and returns the record_id (which is also the block number where the record lives). Simple, self-describing, no separate inode table. record_count and next_record_id in the superblock track metadata but not physical location.

**Record layout on disk**: A record is one or more blocks. The first block contains smkfs_record_t header followed by packed attributes. If attributes exceed one block, the record has an SMKFS_ATTRT_EXTENTS attribute describing where the overflow blocks are. But for now, records are single-block (4KB). The header's length field tells us the total size.

**Attribute layout**: Attributes are packed sequentially after the record header. Each starts with smkfs_attr_header_t (type, flags, id, length), followed by length bytes of data. SMKFS_ATTRT_END (0x0000) terminates the list. The id field can be used for multi-valued attributes (e.g., multiple extents).

**record_id**: Since records are stored in data blocks, record_id == block_number. This is elegant; no indirection. The superblock's root_record is the block number of the root directory record.

# API
The SmKFS API is divided into four levels

## Level 4: Static Internal Interface
These are the primitives everything else builds on. They don't know about paths, directories, or user concepts. They know about blocks, records, attributes, extents, trees, and the journal.

### Block I/O
`static int read_block(uint64_t block, void *buf);`  
**Purpose**: Read a single 4096-byte block from disk into a buffer. This is the fundamental read primitive, every other "read" in SmKFS eventually calls this.  
**Design note**: The IDE driver works in 512-byte sectors. One SmKFS block is 8 sectors. We translate the block number to LBA, then read 8 consecutive sectors. We use the drive number stored in the global drive_num.  
**Parameters**:
- block: logical block number within the filesystem (0 = superblock)
- buf: destination buffer, must be at least SMKFS_BLOCK_SIZE bytes  

**Returns**: 0 on success, -1 on error (IDE failure)  

---

`static int write_block(uint64_t block, const void *buf);`  
**Purpose**: Write a single 4096-byte block from a buffer to disk. The fundamental write primitive. Same sector-by-sector translation as read_block.  
**Parameters**:
- block: logical block number within the filesystem
- buf: source buffer, must contain at least SMKFS_BLOCK_SIZE bytes

**Returns**: 0 on success, -1 on error  

---

### Header
`static void header_init(smkfs_header_t *h, uint16_t type, uint32_t length, uint32_t flags);`  
**Purpose**: opulate a canonical header with magic, version, type, length, and flags. Does NOT compute checksum; caller fills payload then calls header_checksum_update.  
**Parameters**:
- h: pointer to header to initialize
- type: structure type (SMKFS_ST_*)
- length: total size of structure in bytes
- flags: structure-specific flags

---

`static int header_validate(const smkfs_header_t *h, uint16_t expected_type);`  
**Purpose**: Check magic, version, and type match expectations. Does NOT verify checksum, caller does that separately if needed.  
**Parameters**:
- h: pointer to header read from disk
- expected_type: what structure type we expect here  

**Returns**: 0 if valid, -1 if magic/version/type mismatch

---

### Checksum
`static uint32_t checksum_compute(const void *data, size_t len);`  
**Purpose**: Compute checksum over arbitrary data. The checksum field is assumed zero if it's part of the data.
**Parameters**:
- data: pointer to data
- len: number of bytes  

**Returns**: 32-bit checksum value

---

`static void header_checksum_update(smkfs_header_t *h, const void *data, size_t len);`  
**Purpose**: Compute checksum over entire structure and store it in header. Caller must have already called header_init and filled payload.  
**Parameters**:
- h: pointer to header (at start of structure)
- data: same pointer as h, for clarity; the structure to checksum
- len: same as h->length, total structure size

---

`static int header_checksum_verify(const smkfs_header_t *h, const void *data, size_t len);`  
**Purpose**: Verify checksum of a structure read from disk.
**Parameters**:
- h: pointer to header
- data: pointer to entire structure
- len: total structure size

**Returns**: 0 if checksum valid, -1 if corrupt

---


### Bitmap
`static void bitmap_set(uint64_t block);`  
**Purpose**: Mark a single block as allocated in the bitmap.  
**Parameters**:
- block: absolute block number to mark allocated

---

`static void bitmap_clear(uint64_t block);`  
**Purpose**: Mark a single block as free in the bitmap.  
**Parameters**:
- block: absolute block number to mark free

---

`static int bitmap_test(uint64_t block);`  
**Purpose**: Check if a block is allocated.  
**Parameters**:
- block: absolute block number to test

**Returns**: 1 if allocated, 0 if free, -1 if invalid block number

---

`static uint64_t bitmap_alloc_range(uint32_t count);`  
**Purpose**: Find and allocate a contiguous range of count free blocks. This is the core of extent-based allocation; we want contiguous runs.  
**Parameters**:
- count: number of contiguous blocks needed

**Returns**: first absolute block number of allocated range, or -1 on fail  
**Algorithm**: Scan bitmap linearly, looking for count consecutive free bits. First-fit. Not optimal, but simple and predictable.

---

`static uint64_t bitmap_alloc(void);`  
**Purpose**: Allocate a single block. Wrapper around bitmap_alloc_range(1).  
**Returns**: absolute block number, or -1 on fail

---

`static void bitmap_free_range(uint64_t start, uint32_t count);`  
**Purpose**: Mark a contiguous range of blocks as free.  
**Parameters**:
- start: absolute block number of first block to free
- count: number of blocks to free

---


### Record
`static int record_read(uint64_t record_id, smkfs_record_t *rec, void *attr_buf, size_t buf_size);`  
**Purpose**: Read a record header and its attributes from disk.  
**Parameters**:
- record_id: block number where record lives
- rec: output buffer for record header
- attr_buf: output buffer for attributes
- buf_size: size of attr_buf

**Returns**: bytes of attributes read, or -1 on error

---

`static int record_write(uint64_t record_id, const smkfs_record_t *rec, const void *attr_buf);`  
**Purpose**: Write a record header and attributes to disk.  
**Parameters**:
- record_id: block number where record lives
- rec: record header (must have valid header.length)
- attr_buf: attribute data

**Returns**: 0 on success, -1 on error

---

`static uint64_t record_alloc(uint16_t object_type);`  
**Purpose**: Allocate a new block, initialize it as a record, write to disk.  
**Parameters**:
- object_type: SMKFS_ROT_FILE, DIR, etc.

**Returns**: record_id (block number), or -1 on fail

---

`static void record_free(uint64_t record_id);`  
**Purpose**: Free all resources owned by a record (extents, then the record block itself).  
**Parameters**:
- record_id: block number of record to free

---

`static int record_find_attr(const void *attr_buf, uint16_t attr_type, void **out_attr, size_t out_len);`  
**Purpose**: Scan attribute list for first matching type.  
**Parameters**:
- attr_buf: pointer to attribute data
- attr_type: attribute type to find
- out_attr: output pointer to attribute data (after header)
- out_len: output length of attribute data

**Returns**: 0 if found, -1 if not found

---

`static int record_add_attr(void *attr_buf, size_t buf_size, uint16_t attr_type, const void *data, size_t data_len);`  
**Purpose**: Append a new attribute to the attribute list. Replaces existing attribute of same type if present.  
**Parameters**:
- attr_buf: attribute buffer to modify
- buf_size: total size of attr_buf
- attr_type: type to add
- data: attribute data
- data_len: length of data

**Returns**: 0 on success, -1 if no space

---

`static int record_remove_attr(void *attr_buf, uint16_t attr_type);`  
**Purpose**: Remove an attribute from the list by compacting subsequent attributes.  
**Parameters**:
- attr_buf: attribute buffer to modify
- attr_type: type to remove

**Returns**: 0 if removed or not found, -1 on error

---


### Extent
`static int extent_resolve(uint64_t record_id, uint64_t logical_block, smkfs_extent_t *out);`  
**Purpose**: Find which extent covers a given logical block.  
**Parameters**:
- record_id: record to query
- logical_block: block offset within the file
- out: output extent

**Returns**: 0 if found, -1 if not found

---

`static int extent_add(uint64_t record_id, uint64_t logical_block, uint64_t physical_block, uint32_t out);`  
**Purpose**: Add a new extent to a record's extent list  
**Parameters**:
- record_id: record to modify
- logical_block: starting logical offset
- physical_block: starting physical block
- count: number of blocks

**Returns**: 0 on success, -1 on error

---

`static void extent_remove_all(uint64_t record_id);`  
**Purpose**: Free all blocks described by all extents in a record, then remove the extents attribute.  
**Parameters**:
- record_id: record to clean

---


### B+ tree
`static int btree_search(uint64_t root_block, const char *key, uint64_t *out_value);`  
**Purpose**: Search a B+ tree for a key and return the associated value  
**Paramters**:
- root_block: starting block of the tree
- key: lookup key
- out_value: destination for the matched record id.

**Returns**: 0 on success, -1 when the key is not found or input is invalid  
**Algorithm**: Descends through the tree by following child pointers or, for leaf nodes, scans the sorted names until the match is found and then follows sibling links

---

`static int btree_insert(uint64_t root_block, const char *key, uint64_t value, uint64_t *new_root);`  
**Purpose**: Insert a key/value pair into a B+ tree.  
**Parameters**:
- root_block: root block of the tree
- key: key to insert
- value: record id to store
- new_root: output root block

**Returns**: 0 on success, -1 on fail.  
**Algorithm**: Creates a new root leaf when the tree is empty; otherwise delegates to the recursive insertion routine.


---

`static int btree_delete(uint64_t root_block, const char *key, uint64_t *new_root);`  
**purpose**: Remove a key/value pair from a B+ tree.  
**parameters**:
- root_block: root block of the tree
- key: key to remove
- new_root: output root block after the operation.

**Returns**: 0 on success, -1 when the key is absent or input is invalid.  
**Algorithm**: Reads the leaf node, removes the entry from the sorted payload, rewrites the node, and returns the current root block.

---

`static int btree_iterate(uint64_t root_block, int (*cb)(uint64_t key, uint64_t value, void *ctx), void *ctx);`  
**Purpose**: Visit every key/value pair in a B+ tree in key order.  
**Parameters**:
- root_block: root block of the tree
- cb: callback invoked for each entry
- ctx: caller context passed to the callback

**Returns**: 0 when the walk completes, -1 on traversal or callback errors  
**Algorithm**: Reads the current leaf node, invokes the callback for each entry, and then follows the right sibling link until the tree is exhausted.

---


## Journal
`static int journal_start_transaction(void);`  
**Purpose**: Begin a new transaction. Sets the in-transaction flag. Returns a transaction ID (sequence number of first entry)  
**Returns**: -1 if already in a transaction, otherwise the next sequence number

---

`static int journal_log_write(uint64_t block, const void *old_data, const void *new_data, size_t len);`  
**Purpose**: Log a block write for undo-redo. Stores both old and new data so the journal can restore the old state (undo) or reapply the new state (redo) 
**Parameters**:
- block: absolute block number being modified
- old_data: original block contents
- new_data: new block contents
- len: data length (up to SMKFS_BLOCK_SIZE)

**Returns**: 0 on success, -1 on fail (not in transaction, length too large, or write error)

---

`static int journal_log_alloc(uint64_t block, uint32_t count);`  
**Purpose**: Log a block allocation. For redo: mark blocks allocated. For undo: mark blocks free  
**Parameters**:
- block: first absolute block number being allocated
- count: number of contiguous blocks allocated

**Returns**: 0 on success, -1 on fail


---

`static int journal_log_free(uint64_t block, uint32_t count);`  
**Purpose**: Log a block free. For redo: mark blocks free. For undo: mark blocks allocated  
**Parameters**:
- block: first absolute block number beeing freed
- count: number of contiguous blocks freed

**Returns**: 0 on success, -1 on fail

---

`static int journal_commit(void);`  
**Purpose**: Write a commit record, clear in-transaction flag. The commit record marks all previous entries in this transaction as durable  
**Returns**: 0 on success, -1 on fail (not in transaction or write error)

---

`static int journal_replay(void);`  
**Purpose**: Scan journal, find uncommitted transactions, replay them. Called during smkfs_mount if journal is non-empty  
**Returns**: 0 on success, -1 on fail  
**Algorithm**: Scan all journal blocks. Group entries by transaction (entries between commits). If last group has no commit, replay: for WRITE, restore old_data; for ALLOC, free blocks; for FREE, allocate blocks. Then clear journal

---

`static size_t attr_buf_total_len(const void *attr_buf);`  
**Purpose**: Find total length of attribute  
**Paramters**:
- attr_buf: attribute buffer to find size of

**Returns**: Size of attribute.

---


## Level 3: Kernel Interface
These are what the VFS layer calls. They operate on record IDs and raw structures, not paths. The kernel translates paths to record IDs before calling these.

`int smkfs_mount(uint8_t drive);`  
**Purpose**: Initialize the filesystem, read and validate the superblock, replay the journal if needed, and mark the filesystem as mounted. This is the entry point for making a SmKFS volume accessible  
**Parameters**:
- drive: IDE drive number to mount

**Returns**: 0 on success, -1 on fail

---

`int smkfs_unmount(void);`  
**Purpose**: Safely dismount the filesystem. Flushes the journal, writes the superblock to disk, and clears the mounted flag  
**Returns**: 0 on success, -1 on fail

---

`int smkfs_sync(void);`  
**Purpose**: Force all filesystem state to disk without unmounting. Writes the superblock and flushes any pending journal entries.  
**Returns**: 0 on success, -1 on fail

---

`int smkfs_lookup_by_name(uint64_t dir_record, const char *name, uint64_t *out_record);`  
**Purpose**: Search a directory's B+ tree for a filename and return the associated record ID. This is the core name-to-record resolution used by path traversal  
**Parameters**:
- dir_record: block number of the directory record
- name: filename to look up
- out_record: output pointer for the found record ID

**Returns**: 0 on success, -1 if not found or invalid input

---

`int smkfs_create_record(uint16_t object_type, uint64_t parent_dir, const char *name, uint64_t *out_record);` 
**Purpose**: Create a new filesystem object, store its name in its own record, add to parent directory's B+ tree  
**Parameters**:
- object_type: SMKFS_ROT_FILE, SMKFS_ROT_DIR, etc.
- parent_dir: block number of parent directory record
- name: name for the new object
- out_record: output pointer for new record ID

**Returns**: 0 on success, -1 on fail

---
 
`int smkfs_delete_record(uint64_t record_id);`  
**Purpose**: Remove a record from its parent directory and free all resources  
**Parameters**:
- record_id: block number of record to delete

**Returns**: 0 on success, -1 on fail

---

`int smkfs_rename(uint64_t record_id, uint64_t new_parent, const char *new_name);`  
**Purpose**: Move a record to a new parent directory and/or rename it. Updates the record's stored name, removes from old parent's B+ tree, inserts into new parent's B+ tree  
**Parameters**:
- record_id: block number of record to rename
- new_parent: block number of new parent directory
- new_name: new name for the object

**Returns**: 0 on success, -1 on fail

---

`int smkfs_read(uint64_t record_id, uint64_t offset, size_t len, void *buf);`  
**Purpose**: Read data from a file record at a given offset  
**Parameters**:
- record_id: block number of file record
- offset: byte offset within file
- len: number of bytes to read
- buf: destination buffer

**Returns**: bytes read, or -1 on error

---

`int smkfs_write(uint64_t record_id, uint64_t offset, size_t len, const void *buf);`  
**Purpose**: Write data to a file record at a given offset. Allocates blocks as needed  
**Parameters**:
- record_id: block number of file record
- offset: byte offset within file
- len: number of bytes to write
- buf: source buffer

**Returns**: bytes written, or -1 on error

---

`int smkfs_truncate(uint64_t record_id, uint64_t new_size);`  
**Purpose**: Resize a file. If shrinking, free extents beyond new size. If growing, update size (blocks allocated on demand by write)  
**Parameters**:
- record_id: block number of file record
- new_size: new file size in bytes

**Returns**: 0 on success, -1 on error

---

`int smkfs_getattr(uint64_t record_id, smkfs_record_t *rec, void *attr_buf, size_t buf_size);`  
**Purpose**: Read a record's header and all attributes.  
**Parameters**:
- record_id: block number of record
- rec: output buffer for record header
- attr_buf: output buffer for attributes
- buf_size: size of attr_buf

**Returns**: bytes of attributes read, or -1 on error

---

`int smkfs_setattr(uint64_t record_id, uint16_t attr_type, const void *data, size_t len);` 
**Purpose**: Add or replace an attribute on a record  
**Parameters**:
- record_id: block number of record
- attr_type: attribute type to set
- data: attribute data
- len: data length

**Returns**: 0 on success, -1 on error

---
 

## Level 2: User Interface
These are the POSIX-like calls a userspace program makes. Phonon's transparency principle means these should explain themselves; good error messages, clear behavior, no hidden magic.

`int path_lookup(const char *path, uint64_t *out_record)`  
**Purpose**: resolves a path string to a record_id  
**Parameters**:
- path: path to resolve
- out_record: record_id corrensponding to path

**Returns**: 0 on success, -1 on fail

---

`int smkfs_open(const char *path, int flags);`  
**Purpose**: Open a file by path, returning a file descriptor handle. The fd tracks current record_id and file offset for subsequent read/write/seek operations   
**Parameters**:
- path: absolute path to file
- flags: open flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, etc.)

&&: fd on success, -1 on fail

---

`int smkfs_close(int fd);`  
**Purpose**: Release a file descriptor  
**Parameters**:
- fd: file descriptor to close

**Returns**: 0 on success, -1 on fail

---

`int smkfs_read_file(int fd, void *buf, size_t len);`  
**Purpose**: Read from an open file at current offset  
**Parameters**:
- fd: file descriptor
- buf: destination buffer
- len: bytes to read

**Returns**: bytes read, or -1 on error

---

`int smkfs_write_file(int fd, const void *buf, size_t len);`  
**Purpose**: Write to an open file at current offset  
**Parameters**:
- fd: file descriptor
- buf: source buffer
- len: bytes to write

**Returns**: bytes written, or -1 on error

---

`int smkfs_seek(int fd, int64_t offset, int whence);`  
**Purpose**: Adjust file offset in fd table  
**Parameters**:
- fd: file descriptor
- offset: offset value
- whence: SEEK_SET (0), SEEK_CUR (1), SEEK_END (2)

**Returns**: new offset, or -1 on error

---

`int smkfs_create_file(const char *path, uint16_t permissions);`  
**Purpose**: Create a new file at the given path  
**Parameters**:
- path: drive-prefixed path
- permissions: file permissions (stored as attribute)

**Returns**: 0 on success, -1 on fail

---

`int smkfs_delete_file(const char *path);`  
**Purpose**: Delete a file by path  
**Parameters**:
- path: path of file to delete

**Returns**: 0 on success, -1 on fail

---

`int smkfs_mkdir(const char *path);`  
**Purpse**: Create a directory
**Parameters**:
- path: path of directory to create

**Returns**: 0 on success, -1 on fail

---

`int smkfs_rmdir(const char *path);`  
**Purpse**: Delete a directory
**Parameters**:
- path: path of directory to delete

**Returns**: 0 on success, -1 on fail

---

`int readdir_cb(const char *key, uint64_t value, void *ctx);`  
**Purpose**: btree_iterate callback used by smkfs_readdir. Copies one B+ tree key/value pair (filename, record_id) into the caller's dirent array via the readdir_ctx_t passed as ctx  
**Parameters**:
- key: filename string from the B+ tree entry
- value: record_id associated with that filename
- ctx: pointer to a readdir_ctx_t; holds the destination entries array, its capacity (max), and the running count

**Returns**: 0 on success, -1 if ctx->count has reached ctx->max (signals btree_iterate to stop early)

---

`int smkfs_readdir(const char *path, smkfs_dirent_t *entries, size_t max_entries, size_t *out_count);`  
**Purpose**: Read directory entries. Resolves path to its record, confirms it's a directory, finds its B+ tree root via the SMKFS_ATTRT_DATA attribute, then iterates the tree collecting name/record_id pairs via readdir_cb  
**Parameters**:
- path: drive-prefixed path of the directory to read
- entries: caller-provided array to receive directory entries
- max_entries: capacity of the entries array; iteration stops once this many entries have been collected
- out_count: output pointer, set to the number of entries actually written into entries

**Returns**: 0 on success, -1 on error (not mounted, invalid path, not a directory, or missing data attribute)

---

`int smkfs_stat(const char *path, smkfs_record_t *rec, void *attr_buf, size_t buf_size);`  
**Purpose**: Get file/directory statistics by reading the record and its attributes  
**Parameters**:
- path: drive-prefixed path
- rec: output buffer for record header
- attr_buf: output buffer for attributes
- buf_size: size of attr_buf

**Returns**: 0 on success, -1 on error

---

`int smkfs_chmod(const char *path, uint16_t permissions);`  
**Purpose**: Change file permissions  
**Parameters**:
- path: path of file to modify
- permissions: new file permissions

**Returns**: 0 on success, -1 on error

---

`int smkfs_chown(const char *path, uint32_t uid, uint32_t gid);`  
**Purpose**: Change the owner and group of a file or directory. Packs uid and gid into a single 64-bit SMKFS_ATTRT_OWNER attribute (uid in the high 32 bits, gid in the low 32 bits) and writes it via smkfs_setattr  
**Parameters**:
- path: drive-prefixed path of the file or directory to modify
- uid: new User IDentifier, stored in the upper 32 bits of the owner attribute
- gid: new Group IDentifier, stored in the lower 32 bits of the owner attribute

**Returns**: 0 on success, -1 on error (not mounted, invalid path, lookup failure, or setattr failure)

---


## Level 1: Administrative Interface
These are the "does this even work" functions. The ones that create a filesystem from nothing, verify it's healthy, and repair it if not.

`int smkfs_mkfs(uint8_t drive, uint64_t total_blocks);`  
**Purpose**: Create a new SmKFS filesystem on the specified drive. Writes the superblock, bitmap, root directory record, empty journal, and initializes all metadata structures  
**Parameters**:
- drive: drive number to format
- total_blocks: total number of 4KB blocks available on the drive

**Returns**: 0 on success, -1 on fail  
**Algorithm**
Algorithm:
1. Validate total_blocks is sufficient (minimum ~16 blocks for superblock + bitmap + journal + root dir + data)
2. Calculate layout: superblock at 0, bitmap after, journal after bitmap, data area after journal
3. Write zeroed superblock with magic, version, layout parameters
4. Zero and write bitmap blocks
5. Zero and write journal blocks
6. Create root directory record with empty B+ tree
7. Write final superblock with checksum

---

`int smkfs_fsck(uint8_t drive);`  
**Purpose**: Verify filesystem consistency. Check superblock, bitmap, and basic record integrity. Report errors, optionally repair  
**Parameters**:
- drive: drive number to check
**Returns**: 0 if clean, 1 if errors found and fixed, -1 if unrecoverable

---

`int smkfs_dump_superblock(void);`  
**Purpose**: Print superblock information for debugging and transparency. Displays volume parameters, layout, and state  
**Returns**: 0 on success, -1 if not mounted

---

`int smkfs_dump_record(uint64_t record_id);`  
**Purpose**: Print a record's header and all attributes for debugging and transparency  
**Parameters**:
- record_id: block number of record to dump
**Returns**: 0 on success, -1 on fail

---

`int smkfs_dump_journal(void);`  
**Purpose**: Print all journal entries for debugging and crash analysis  
**Returns**: 0 on success, -1 if not mounted

---

`int smkfs_dump_btree(uint64_t root_block);`
**Purpose**: Recursively print B+ tree structure for debugging  
**Parameters**:
- root_block: root block of tree to dump

**Returns**: 0 on success, -1 on fail

---

# SmKFS Ground One
## Shadow-mode Kernel File System
### Architecture Specification v1.0 — Production Upgrade Plan

---

## 1. Executive Summary

SmKFS Ground Zero (G0) proved the architecture. Ground One (G1) makes it production-grade.

G1 is not an incremental patch. It is a deliberate architectural upgrade that fixes the fundamental constraints discovered during G0 implementation while preserving the core philosophy: **self-describing structures, record-oriented metadata, extent-based storage, and attribute-driven extensibility.**

The three pillars of G1 are:

1. **Master Record Table (MRT)** — Logical record identities decoupled from physical block numbers.
2. **Multi-Valued Attribute Model** — Full support for complex metadata without format hacks.
3. **Directory-Centric Naming** — Hard links, proper POSIX semantics, and a clean separation between identity and location.

Everything else — journal correctness, B+ tree completeness, byte-level I/O, allocator regions — is built atop these pillars.

---

## 2. Design Philosophy: What Changes and What Doesn't

### What Stays
- Self-describing canonical headers on every structure.
- Extents as the sole non-resident data mechanism.
- B+ trees for directory indexing.
- Metadata journaling for crash consistency.
- Attribute-driven extensibility.

### What Changes
| G0 Constraint | G1 Replacement | Rationale |
|---------------|----------------|-----------|
| `record_id == block_number` | Logical record IDs via MRT | Records can move, grow, shrink, and be defragmented |
| Single-valued attributes | Multi-valued attributes with `attr_id` | Multiple extents, ACLs, xattrs, streams |
| Name stored in record | Name stored only in directory B+ tree | Hard links, atomic rename, POSIX compliance |
| Linear bitmap scan | Regioned allocator with free-count summaries | Scalable to multi-terabyte storage |
| Custom checksum | CRC32C with hardware acceleration | Standardized, fast, well-tested |
| Redo+undo physical logging | Redo-only metadata journaling | Halves journal write amplification |
| Global single-mount state | Per-mount context | Multiple volumes, hot-swap, namespace composition |

---

## 3. The Master Record Table (MRT)

### 3.1 Rationale

G0's elegance — `record_id == block_number` — is also its cage. If a record needs to grow beyond one block, if metadata is repacked, if storage is migrated or defragmented, the identity of the object changes. Modern filesystems separate **object identity** from **physical location** because physical locations are temporary.

G1 introduces the **Master Record Table (MRT)**, inspired by NTFS's MFT but adapted to SmKFS's block-oriented, self-describing design.

### 3.2 MRT Structure

The MRT is a reserved array of **record descriptors** stored in a known location on disk. The superblock points to the MRT's starting block and defines its capacity.

```c
typedef struct {
    uint64_t physical_block;    // UINT64_MAX (64 ZiB) = unallocated / free slot
    uint16_t flags;             // SMKFS_MRTF_* flags
    uint16_t reserved;
    uint32_t generation;        // Incremented on reuse to detect stale refs
} smkfs_mrt_entry_t;
```

**MRT Flags:**
- `SMKFS_MRTF_ALLOCATED` — Slot is in use.
- `SMKFS_MRTF_OVERFLOW` — Record spans multiple blocks (see §3.4).
- `SMKFS_MRTF_DELETED` — Slot marked for reclamation.

### 3.3 Record Identity

In G1, `record_id` is a **logical index into the MRT**, not a block number.

```
record_id = 812
    |
    v
MRT[812] = { physical_block = 49152, flags = ALLOCATED, generation = 7 }
    |
    v
Read block 49152 → smkfs_record_t + attributes
```

This indirection enables:
- **Record relocation** without changing identity.
- **Defragmentation** of metadata.
- **Future resizing** of the MRT without reformatting.
- **RAID and volume managers** to remap physical locations independently.

### 3.4 MRT Bootstrap and Sizing

During `smkfs_mkfs`:
1. Reserve MRT blocks immediately after the superblock.
2. MRT size = `max_records * sizeof(smkfs_mrt_entry_t) / BLOCK_SIZE`, rounded up.
3. MRT block 0 is always reserved for the **root directory record**.
4. The superblock stores: `mrt_start`, `mrt_length`, `mrt_capacity`, `mrt_free_count`.

The MRT itself is journaled like any other metadata structure.

### 3.5 MRT Operations

```c
static uint64_t mrt_alloc_entry(uint64_t *out_record_id);
static int mrt_update_entry(uint64_t record_id, uint64_t new_physical_block, uint16_t flags);
static int mrt_free_entry(uint64_t record_id);
static int mrt_resolve(uint64_t record_id, uint64_t *out_physical_block, uint16_t *out_flags);
```

All MRT mutations occur inside journal transactions.

---

## 4. Multi-Valued Attribute Model (Option B)

### 4.1 Rationale

G0's attribute model was ambiguous: the `id` field existed but `record_add_attr()` replaced by type. G1 makes the `id` field first-class, enabling true multi-valued attributes.

This is essential for:
- Multiple extent ranges per file.
- Multiple ACL entries.
- Multiple xattrs.
- Alternate data streams (future).

### 4.2 Attribute Header v2

```c
typedef struct {
    uint16_t type;      // SMKFS_ATTRT_* 
    uint16_t flags;     // SMKFS_ATTRF_*
    uint32_t id;        // Instance ID within this type (0 = default)
    uint32_t length;    // Payload length in bytes
} smkfs_attr_header_t;
```

**Attribute Flags:**
- `SMKFS_ATTRF_RESIDENT` — Data lives in the record block.
- `SMKFS_ATTRF_NON_RESIDENT` — Data is extent-backed (payload = extent list).
- `SMKFS_ATTRF_ENCRYPTED` — Payload is encrypted.
- `SMKFS_ATTRF_COMPRESSED` — Payload is compressed.

### 4.3 Attribute API v2

```c
// Find first attribute of given type and id
static int record_find_attr(const void *attr_buf, uint16_t attr_type, 
                            uint32_t attr_id, void **out_attr, size_t *out_len);

// Add or replace specific (type, id) pair
static int record_add_attr(void *attr_buf, size_t buf_size, 
                           uint16_t attr_type, uint32_t attr_id,
                           const void *data, size_t data_len);

// Remove specific (type, id) pair
static int record_remove_attr(void *attr_buf, uint16_t attr_type, uint32_t attr_id);

// Iterate all attributes of a given type
static int record_find_attr_by_type(const void *attr_buf, uint16_t attr_type,
                                    int (*cb)(uint32_t id, void *data, size_t len, void *ctx),
                                    void *ctx);
```

### 4.4 Extents as Multi-Valued Attributes

Extents are no longer a single attribute containing an array. Each contiguous extent range is a separate `(SMKFS_ATTRT_EXTENTS, id)` attribute. The `id` is monotonically assigned per record.

```
Record 812:
  ATTR(EXTENTS, id=0) → { log=0, phys=1024, count=8 }
  ATTR(EXTENTS, id=1) → { log=100, phys=2048, count=4 }
  ATTR(EXTENTS, id=2) → { log=200, phys=4096, count=16 }
```

This eliminates the need to rewrite a large extent array on every append. Only new extents are appended; coalescing is a background optimization.

---

## 5. Directory-Centric Naming & Hard Links

### 5.1 Rationale

In G0, the filename was stored inside the record as `SMKFS_ATTRT_NAME`. This made hard links architecturally impossible because a record could only have one name.

G1 moves all naming information into the directory B+ tree. The record represents the **inode** (identity, metadata, extents). The directory entries represent the **links** (names pointing to identities).

### 5.2 Directory Entry Format

The B+ tree key is the filename. The value is a directory entry structure:

```c
typedef struct {
    uint64_t record_id;     // Target record (logical ID via MRT)
    uint32_t name_hash;     // Fast comparison filter
    uint16_t flags;         // SMKFS_DENTF_* 
    uint16_t name_len;      // Actual name length (key may be truncated in index nodes)
} smkfs_dirent_t;
```

**Directory Entry Flags:**
- `SMKFS_DENTF_NORMAL` — Regular entry.
- `SMKFS_DENTF_DOT` — "." entry (optional, can be synthesized).
- `SMKFS_DENTF_DOTDOT` — ".." entry (optional, can be synthesized).

### 5.3 Record Link Count

Records gain a `link_count` field in the record header:

```c
typedef struct {
    smkfs_header_t header;
    uint64_t record_id;         // Logical ID
    uint16_t object_type;
    uint16_t attr_count;
    uint32_t link_count;        // Number of directory entries pointing to this record
    uint64_t generation;        // Matches MRT generation
} smkfs_record_t;
```

Rules:
- `create_record` sets `link_count = 1`.
- `link()` increments `link_count` and adds a new directory entry.
- `unlink()` decrements `link_count`; if it reaches 0, the record and its extents are freed.
- `rename()` across directories updates two B+ trees but does not touch the record.

### 5.4 Path Resolution

Path lookup traverses directories using the B+ tree. Special cases:
- `"."` → current directory (synthesized, no lookup).
- `".."` → parent directory (stored as `SMKFS_ATTRT_PARENT` on directory records, or synthesized from path history).

### 5.5 Hard Link API

```c
int smkfs_link(const char *existing_path, const char *new_path);
int smkfs_unlink(const char *path);
```

`smkfs_delete_record()` is renamed to `smkfs_unlink()` to reflect POSIX semantics. Record reclamation happens only when `link_count == 0`.

---

## 6. Journal v2: Trustworthy Crash Recovery

### 6.1 Rationale

G0's journal replay was broken: it only undid uncommitted transactions, ignoring committed transactions whose data blocks never reached disk. G1 implements a correct redo-only metadata journal.

### 6.2 Journal Format

The journal is a **circular buffer** of fixed-size entries. The superblock tracks:
- `journal_head` — Next free slot for writing.
- `journal_tail` — Oldest uncheckpointed entry.
- `journal_sequence` — Monotonic transaction counter.

```c
typedef struct {
    smkfs_header_t header;
    uint64_t sequence;          // Transaction sequence number
    uint64_t target_block;      // Physical block being modified
    uint32_t operation;         // SMKFS_JOP_*
    uint32_t data_length;       // Payload length
    uint64_t record_id;         // Logical record affected (for fsck correlation)
    // Payload follows: new_data for WRITE, count for ALLOC/FREE
} smkfs_journal_entry_t;
```

**Operations:**
- `SMKFS_JOP_WRITE` — Log new block contents (redo).
- `SMKFS_JOP_ALLOC` — Log block allocation.
- `SMKFS_JOP_FREE` — Log block free.
- `SMKFS_JOP_MRT_UPDATE` — Log MRT entry change.
- `SMKFS_JOP_COMMIT` — Transaction boundary marker.
- `SMKFS_JOP_CHECKPOINT` — Journal truncation marker.

### 6.3 Redo-Only Logging

G1 stores **only new data** in WRITE entries. This halves journal space usage.

**Commit Protocol:**
1. `journal_start_transaction()` → acquire sequence number.
2. Log all metadata changes (WRITE, ALLOC, FREE, MRT_UPDATE).
3. `journal_commit()` → write COMMIT record, flush to disk.
4. Apply changes to their final on-disk locations.
5. `journal_checkpoint()` → advance tail, free journal space.

**Replay Algorithm (Mount):**
```
1. Scan journal from tail to head.
2. Find all transactions with a COMMIT record (committed).
3. For each committed transaction:
   a. For WRITE: write payload to target_block (if not already applied).
   b. For ALLOC: set bits in bitmap.
   c. For FREE: clear bits in bitmap.
   d. For MRT_UPDATE: apply MRT entry change.
4. Discard all uncommitted trailing entries (no undo needed).
5. Write CHECKPOINT to mark journal clean.
```

### 6.4 Transactional Boundaries

Every multi-step Level-3 operation MUST open a transaction:

| Operation | Steps Inside Transaction |
|-----------|-------------------------|
| `create_record` | Alloc MRT entry → alloc physical block → init record → insert into dir B+ tree → update parent link count |
| `unlink` | Remove from dir B+ tree → decrement link_count → if 0, free extents → free MRT entry |
| `rename` | Delete from old dir B+ tree → insert into new dir B+ tree → update parent attrs |
| `truncate` | Update size attr → free partial extents → update MRT if record moves |
| `write` (metadata path) | Alloc blocks → update extents → update MRT → update size |

If any step fails, the transaction is **not committed**. On next mount, replay sees no COMMIT and ignores the partial entries.

---

## 7. B+ Tree v2: Complete Specification

### 7.1 Node Types

**Leaf Node:**
- Keys: filenames (or generic byte strings).
- Values: `smkfs_dirent_t` structures.
- Sibling pointer: `right_sibling` for ordered iteration.

**Internal Node:**
- Keys: separator strings (prefixes).
- Values: child block pointers.
- No sibling pointer (tree is navigated from root).

### 7.2 Invariants

- **Order (m):** Derived from block size. For 4KB blocks with 256-byte names:
  - Leaf capacity: ~14 entries.
  - Internal capacity: ~120 entries.
- **Minimum fill:** All nodes except root must be ≥50% full.
- **Root exception:** Root may have as few as 1 key (if tree has ≥1 entry).
- **Key uniqueness:** No duplicate keys within a single leaf.
- **Sorted order:** Keys within a node are lexicographically sorted.

### 7.3 Insertion with Splitting

```
btree_insert(key, value):
  if root is leaf and full:
    split root leaf → create new root internal node
  else:
    descend to appropriate leaf
  if leaf is full:
    split leaf into two (50/50 or median split)
    promote median key to parent
    if parent is full, split recursively upward
  insert into leaf
```

**Leaf Split:**
1. Allocate new leaf block.
2. Move upper half of entries to new leaf.
3. Update sibling pointers: `old_leaf.right_sibling = new_leaf`.
4. Promote first key of new leaf to parent internal node.

**Internal Split:**
1. Allocate new internal block.
2. Move upper half of keys/children to new node.
3. Promote median key to parent (or create new root).

### 7.4 Deletion with Underflow Handling

```
btree_delete(key):
  descend to leaf containing key
  remove key from leaf
  if leaf is root or ≥50% full:
    done
  else:
    try redistribution from left sibling
    if left has > minimum:
      borrow entry from left
    else:
      try redistribution from right sibling
      if right has > minimum:
        borrow entry from right
      else:
        merge with a sibling
        if parent underflows, recurse upward
```

**Merge:**
1. Move all entries from underflow node into sibling.
2. Free underflow node's block.
3. Remove separator key from parent.
4. If parent underflows, recurse.

### 7.5 Generic Key Support

Internally, the B+ tree is generic:

```c
typedef struct {
    uint8_t  *key;
    uint16_t key_len;
    uint8_t  *value;
    uint16_t value_len;
} btree_kv_t;
```

Directory entries use `(name, name_len) → (dirent, sizeof(dirent))`.
Future uses (extent indexing, xattr names) reuse the same implementation with different key/value sizes.

---

## 8. Byte-Aligned I/O Layer

### 8.1 Block Buffer Cache

G1 introduces a minimal block cache for read-modify-write operations:

```c
typedef struct {
    uint64_t block;
    uint8_t  data[SMKFS_BLOCK_SIZE];
    int      dirty;
    int      valid;
} smkfs_block_buf_t;
```

**API:**
```c
static int block_cache_read(uint64_t block, smkfs_block_buf_t **out_buf);
static int block_cache_write(smkfs_block_buf_t *buf);
static int block_cache_flush(void);  // Write all dirty blocks
```

### 8.2 Byte I/O Implementation

```c
int smkfs_read(uint64_t record_id, uint64_t offset, size_t len, void *buf):
  for each block overlapping [offset, offset+len):
    block_cache_read(phys_block, &bbuf)
    copy relevant bytes from bbuf.data to user buf
    zero-fill if hole (no extent)

int smkfs_write(uint64_t record_id, uint64_t offset, size_t len, const void *buf):
  for each block overlapping [offset, offset+len):
    resolve extent or allocate new block
    block_cache_read(phys_block, &bbuf)  // RMW
    copy relevant bytes from user buf to bbuf.data
    bbuf.dirty = 1
  block_cache_flush() if sync write
```

---

## 9. Checksum: CRC32C

Replace the custom checksum with CRC32C (Castagnoli polynomial, `0x1EDC6F41`).

**Rationale:**
- Hardware acceleration on x86 (SSE 4.2 `CRC32` instruction).
- Standardized in iSCSI, SCTP, Btrfs, ext4.
- Excellent error detection for burst errors and bit flips.
- Existing tooling and verification libraries.

**Implementation:**
```c
static uint32_t checksum_compute(const void *data, size_t len) {
    // Use hardware CRC32C if available, software fallback otherwise
    return crc32c(0, data, len);
}
```

The header interface (`header_checksum_update`, `header_checksum_verify`) remains unchanged; only the algorithm changes.

---

## 10. Allocator v2: Regioned Bitmap

### 10.1 Region Structure

Divide the bitmap into **allocation regions** (default: 8192 blocks / region):

```c
typedef struct {
    uint64_t start_block;       // First data block in region
    uint32_t total_blocks;
    uint32_t free_count;        // Cached free block count
    uint32_t last_alloc;        // Hint for next allocation (roving allocator)
} smkfs_alloc_region_t;
```

### 10.2 Allocation Strategy

1. **Fast path:** Check region's `free_count`. If zero, skip entirely.
2. **Contiguous search:** Within a region, scan for `count` consecutive free bits.
3. **Extent coalescing:** Prefer regions near the last allocated extent for locality.
4. **Fallback:** If no region has enough contiguous space, allow non-contiguous multi-extent allocation.

### 10.3 Delayed Allocation (Design-Only for G1)

G1's allocator API supports delayed allocation at the interface level:

```c
uint64_t bitmap_alloc_delayed(uint32_t count, uint64_t *out_handle);
int bitmap_alloc_commit(uint64_t handle, uint64_t *out_block);
```

Full delayed allocation (write buffering, flush-time resolution) is a G2 feature, but the API is designed to accommodate it.

---

## 11. Multi-Mount Support

### 11.1 Per-Mount Context

Remove all global state. Every mounted volume has a context:

```c
typedef struct {
    uint8_t  drive_num;
    int      mounted;
    smkfs_superblock_t sb;
    uint64_t journal_next_sequence;
    int      journal_in_transaction;
    uint64_t journal_write_pos;
    smkfs_fd_t fd_table[SMKFS_FD_MAX];
    smkfs_block_buf_t block_cache[SMKFS_CACHE_SIZE];
} smkfs_mount_t;
```

### 11.2 API Changes

All Level-4 functions take `smkfs_mount_t *mnt` as the first parameter:

```c
static int read_block(smkfs_mount_t *mnt, uint64_t block, void *buf);
static uint64_t bitmap_alloc(smkfs_mount_t *mnt);
static int btree_search(smkfs_mount_t *mnt, uint64_t root_block, 
                        const char *key, uint64_t *out_value);
```

Level-3 functions take `smkfs_mount_t *mnt` or resolve it from a mount handle.

---

## 12. Error Handling & Return Codes

### 12.1 Standardized Error Constants

```c
#define SMKFS_OK            0
#define SMKFS_ERR_IO       -1   // Disk I/O failure
#define SMKFS_ERR_NOMEM    -2   // Memory allocation failure
#define SMKFS_ERR_NOTFOUND -3   // Record, attribute, or path not found
#define SMKFS_ERR_EXISTS   -4   // File or directory already exists
#define SMKFS_ERR_NOSPC    -5   // No space left on device
#define SMKFS_ERR_INVAL    -6   // Invalid parameter
#define SMKFS_ERR_CORRUPT  -7   // Checksum or structure validation failed
#define SMKFS_ERR_NOTEMPTY -8   // Directory not empty
#define SMKFS_ERR_ROFS     -9   // Read-only filesystem
#define SMKFS_ERR_JOURNAL  -10  // Journal replay or write failure
#define SMKFS_ERR_TOO_BIG  -11  // File too large or record overflow
```

### 12.2 Failure Propagation Rules

- Every `malloc()` failure must return `SMKFS_ERR_NOMEM`.
- Every `read_block()` / `write_block()` failure must return `SMKFS_ERR_IO`.
- Checksum mismatch must return `SMKFS_ERR_CORRUPT`.
- All multi-step operations must abort and return error on first failure (transaction not committed).

---

## 13. fsck v2: Comprehensive Checking

### 13.1 Checks Performed

1. **Superblock validation** — magic, version, checksum, layout sanity.
2. **MRT validation** — every allocated MRT entry points to a valid block.
3. **Bitmap reconstruction** — traverse all records and extents, rebuild expected bitmap, compare to on-disk bitmap.
4. **Orphan detection** — blocks marked allocated but unreachable from any record.
5. **Record integrity** — header validation, checksum, attribute chain walkability.
6. **Extent validation** — no overlapping extents within a record, physical blocks within volume bounds.
7. **Directory validation** — B+ tree structural invariants, no dangling record_ids, link_count consistency.
8. **Cycle detection** — directory graph must be a DAG (no loops via ".." or hard links to directories).
9. **Journal replay** — always replay journal before checking (ensures consistent state).

### 13.2 Repair Modes

- **Read-only (`-n`):** Report only, no modifications.
- **Automatic (`-y`):** Fix all safe issues automatically.
- **Interactive (`-a`):** Prompt for dangerous fixes (orphan reclamation, link_count repair).

---

## 14. On-Disk Format Summary (G1)

```
Block 0:              Superblock (smkfs_superblock_t)
Blocks 1..N:          Master Record Table (MRT entries)
Blocks N+1..M:        Journal (circular buffer)
Blocks M+1..P:        Allocation Bitmap
Blocks P+1..Q:        Allocation Region Metadata (optional)
Blocks Q+1..end:      Data Area (records, B+ tree nodes, file data)
```

### 14.1 Superblock v2

```c
typedef struct {
    smkfs_header_t header;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t record_count;
    uint64_t mrt_start;
    uint64_t mrt_length;
    uint64_t mrt_capacity;
    uint64_t mrt_free_count;
    uint64_t root_record_id;    // Logical ID, not block number
    uint64_t journal_start;
    uint64_t journal_length;
    uint64_t journal_head;
    uint64_t journal_tail;
    uint64_t journal_sequence;
    uint64_t bitmap_start;
    uint64_t bitmap_length;
    uint64_t alloc_meta_start;
    uint64_t alloc_meta_length;
    uint64_t data_start;
    uint32_t block_size;
    uint32_t flags;
    uint8_t  uuid[16];
    char     volume_name[64];
    uint64_t creation_time;
    uint64_t last_mount_time;
    uint32_t mount_count;
    uint32_t max_mount_count;   // Force fsck after N mounts
} smkfs_superblock_t;
```

---

## 15. Migration Path from G0 → G1

G0 and G1 are **not on-disk compatible**. Migration requires a reformat or an offline conversion tool.

### 15.1 Offline Conversion (`smkfs_convert`)

1. Read G0 superblock, validate.
2. Create G1 superblock with same volume size.
3. Allocate G1 MRT with capacity = G0 `total_blocks - data_start`.
4. Walk all G0 records (block-by-block scan, since record_id == block_number):
   - For each valid record header, allocate G1 MRT entry.
   - Copy attributes, converting single-valued extent array to multi-valued extents.
   - Remove `SMKFS_ATTRT_NAME` from record, insert into parent directory B+ tree.
   - Set `link_count = 1`.
5. Rebuild G1 bitmap from converted extents.
6. Initialize empty G1 journal.
7. Write G1 superblock.

### 15.2 Fresh Format

Simply call `smkfs_mkfs` with G1 code. The layout is written natively.

---

## 15.5 Code Organization: From Monolith to Modules

SmKFS G0 was implemented as a single `smkfs.c` source file. While this is acceptable for an initial proof-of-concept, it becomes unmaintainable as the codebase grows. G1 reorganizes the implementation into a dedicated `fs/smkfs/` directory, mirroring the modular structure of production filesystems such as Linux's `fs/ntfs`.

### File Layout

```
fs/smkfs/
├── smkfs_internal.h    Shared internal declarations and cross-file prototypes
├── block.c             Block I/O primitives (read_block, write_block)
├── checksum.c          Canonical header and checksum utilities
├── bitmap.c            Bitmap allocation and free-space management
├── journal.c           Transaction logging, commit, and replay
├── record.c            Record read/write, allocation, and freeing
├── attr.c              Central Attribute Organ
├── extent.c            Extent resolution, addition, and removal
├── btree.c             B+ tree search, insert, delete, iterate
├── super.c             Mount, unmount, sync, mkfs, fsck
├── dir.c               Directory lookup, creation, deletion, rename, readdir
├── file.c              File read/write, truncate, open/close/seek
├── inode.c             getattr, setattr, chmod, chown, stat
├── path.c              Path string resolution
└── debug.c             Diagnostic dumps (superblock, record, journal, btree)
```

The public header remains a single file at `include/fs/smkfs.h`, preserving the existing include convention.

### The Central Attribute Organ

A key architectural addition is the **Central Attribute Organ**, implemented in `attr.c`. Rather than scattering attribute-specific knowledge across the codebase, every attribute type is declared in a single registry table (`attr_registry[]`). Each entry specifies:

- **Human-readable name** — used by debug and diagnostic output.
- **Behavior flags** — whether the attribute is unique, required, or resident.
- **Fixed size** — expected payload length, or zero for variable-length attributes.
- **Validation callback** — runtime integrity checking of payload data.
- **Debug-print callback** — pretty-printing for `dump_record` and forensic tools.

Adding a new attribute to SmKFS now requires only a single row in this table. No other files need modification for the attribute to be validated, displayed, and recognized by the filesystem. This directly supports Golden Rule 8: new functionality is added through attributes, not structural rewrites.

## 16. Sparse File Semantics
| Scenario                      | Behavior                                                                   |
| ----------------------------- | -------------------------------------------------------------------------- |
| **Punch inside one extent**   | Splits it into two surviving extents; middle blocks freed                  |
| **Punch covers whole extent** | Extent dropped, all blocks freed                                           |
| **Punch covers tail**         | Extent shrunk, tail blocks freed                                           |
| **Punch covers head**         | Extent moved forward, head blocks freed                                    |
| **Punch past EOF**            | No-op                                                                      |
| **Partial-block punch**       | Operates on whole logical blocks (conservative; keeps partial edge blocks) |

## 17. Implementation Phases

### Phase 1: Foundation (Weeks 1–2)
- [x] Define all canonical structs (superblock v2, MRT, record v2, attr v2, dirent, journal v2).
- [x] Implement CRC32C checksum.
- [x] Implement per-mount context and remove global state.
- [x] Implement MRT allocation, resolution, and update.
- [x] Update block I/O to use mount context.

### Phase 2: Core Metadata (Weeks 3–4)
- [x] Remove 512-Byte sector assumption, store size in SB
- [x] Implement multi-valued attribute API.
- [x] Implement record v2 with link_count and generation.
- [x] Move names to directory B+ tree.
- [x] Implement hard link (`link`, `unlink`) semantics.
- [x] Update path resolution for new directory format.

### Phase 3: Storage & I/O (Weeks 5–6)
- [x] Implement regioned bitmap allocator.
- [x] Implement extent v2 (multi-valued, coalescing).
- [x] Implement block buffer cache.
- [x] Implement byte-aligned read/write with RMW.
- [x] Implement sparse file semantics (zero-fill holes).
- [x] Implement partial truncate with extent range removal.

### Phase 4: Tree & Journal (Weeks 7–8)
- [x] Implement complete B+ tree (internal nodes, split, merge, redistribution).
- [x] Implement redo-only journal with circular buffer.
- [x] Implement two-pass replay (redo committed, discard uncommitted).
- [x] Implement checkpointing.
- [x] Wrap all multi-step operations in transactions.

### Phase 5: Hardening (Weeks 9–10)
- [x] Heap-allocate all large (4KB) block buffers, switch back to 16 KB kernel stack.
- [ ] Chain merge extents.
- [ ] Write MAT to disk and use that for attribute implementation. (essentially $AttrDef)
- [ ] Implement comprehensive fsck v2.
- [ ] Implement standardized error codes and audit all failure paths.
- [ ] Add UUID, volume name, mount count to superblock (replace drive letter from drive number to drive letter by volume name).
- [ ] Stress testing: crash injection, bitmap reconstruction, large directory tests.
- [ ] Write G1 specification document (this document, finalized).
Bug fixes (they're small and independent) → 2. Point 1 stack purge (unblocks the 16 KB stack) → 3. Error-code standardization (makes everything after it easier to audit) → 4. Extent chain merge + cap fix → 5. MAT on disk → 6. fsck v2 (consumes MAT + clean error codes) → 7. UUID/volume-name + crash-injection harness in parallel → 8. Spec doc last, when the code has stopped moving.

---

## 17. Golden Design Rules (G1)

1. Everything is a record with a **logical identity**.
2. Physical location is managed by the **MRT**, never assumed by callers.
3. Attributes are **multi-valued**; type + id uniquely identifies an attribute instance.
4. **Names live in directories**, not records. Records have link counts.
5. Extents are **per-attribute instances**, not monolithic arrays.
6. Metadata changes are **atomic via journaling**; user data is not journaled.
7. The journal is **redo-only**, circular, and checkpointed.
8. The B+ tree is **generic**; directory entries are one use case.
9. Allocation is **regioned**; contiguous extents are preferred but not required.
10. All state is **per-mount**; globals are forbidden.
11. Every structure is **self-describing** with a canonical header and CRC32C.
12. **Corruption is detected early**; invalid structures panic the mount, never propagate.

---

*Document Version: 1.0-draft*
*Target: SmKFS G1 Implementation*
*Author: amity*
*Date: 2026-07-29*
