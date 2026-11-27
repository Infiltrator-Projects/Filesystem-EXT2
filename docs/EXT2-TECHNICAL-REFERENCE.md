# EXT2: technical reference

## Original format and scope

Second Extended Filesystem was developed for Linux by Remy Card, Theodore Ts'o and Stephen Tweedie and appeared in 1993. Its block groups combine local allocation bitmaps and inode tables with shared superblock geometry. This reference uses the historical Linux v2.6.32 ext2 definitions rather than silently adopting contemporary ext4 fields. Ext2 has no native transaction journal. A filesystem carrying a journal or pending recovery must be classified by actual features, not by the label supplied to the probe. Ordinary allocation, file mapping and directory semantics are related to ext3 but their interruption guarantees differ.

Linux initially used MINIX and the first extended filesystem. Ext2's January 1993 release and block-group locality improved allocation and Unix-style metadata at the cost of full consistency scans after unclean shutdown. Low write amplification is not crash resilience.

## Superblock geometry and field inventory

All ordinary ext2/ext3 metadata integers are little-endian. The primary superblock is 1024 bytes long at partition-relative byte 1024, regardless of block size. Relative 32-bit fields: inode count 0, block count 4, reserved blocks 8, free blocks 12, free inodes 16, first data block 20, log block size 24, log fragment size 28, blocks per group 32, fragments per group 36, inodes per group 40, mount time 44, write time 48. Mount counts at 52/54 and magic 0xEF53 at 56 are 16-bit. State 58, errors policy 60, minor revision 62, last check 64, check interval 68, creator OS 72, revision 76, reserve UID/GID 80/82. Block bytes B=1024 shifted left by the validated log block size. First data block is ordinarily 1 for 1024-byte blocks and 0 for larger blocks. Check total geometry against the supplied device.

## Revision and feature admission

For dynamic revision, first ordinary inode at 84, 16-bit inode size 88, superblock group number 90, compatible/incompatible/read-only-compatible flags at 92/96/100, UUID 104, label 120 and last mounted path 136. Revision zero instead uses 128-byte inodes and first ordinary inode 11. Validate inode size and alignment, group counts and bitmap capacity (one bitmap block covers at most 8B allocation units). Unknown incompatible features prohibit mounting; unknown read-only-compatible flags prohibit writing. A repair tool needs a stronger understanding of all features than a read-only parser. FILETYPE incompatibility bit 0x2 changes the directory entry interpretation; sparse-super RO bit 0x1 changes backup placement; LARGE_FILE RO bit 0x2 permits large regular files. Do not admit ext4 extent, 64-bit or bigalloc features under a legacy format name.

## Group descriptors and redundant geometry

Let F be first data block, G blocks per group, T total blocks. Group count is ceil((T-F)/G); group g starts at F+g*G and ends at min(T,F+(g+1)*G). In the conventional non-META_BG layout, the primary descriptor table begins at block F+1. Each 32-byte descriptor contains bitmap block 0, inode bitmap block 4, inode table block 8, free block count 12, free inode count 14, used directory count 16, reserved/padding 18..31. The descriptor for group g begins g*32 bytes into the table. The inode-table span is ceil(inodes_per_group*inode_size/B). Validate all its blocks and metadata reservations before using a group. With sparse-super, backup superblocks and descriptor tables are in groups 0,1 and positive powers of 3,5 or 7. META_BG and resize reservations require their own placement rules; never apply the simple table formula to those without validation.

## Bitmaps and worked allocation address

A set bit means allocated in the legacy ext block and inode bitmaps, least-significant bit first in each byte. For block p>=F, group=floor((p-F)/G), local=(p-F) mod G. The byte within that group's block bitmap is floor(local/8), mask=1 shifted left by local mod 8. For B=4096, F=0, G=32768, block 33000 maps to group 1, local 232, byte 29, bit 0. For inode n>=1, group=floor((n-1)/inodes_per_group), local=(n-1) mod inodes_per_group. Inode 0 is not valid. Check last partial groups, reserved inodes, superblock replicas, descriptors and inode tables; metadata must not be offered as free storage. Free counters are summaries and must reconcile with bitmap state and actual ownership.

## Inode location and base fields

The inode byte position is inode_table_block*B+local_inode_index*inode_size. For B=4096, inode size 256, 8192 inodes per group and inode 8200, group 1 and local index 7 give offset 1792 into that group's inode table. The root is inode 2. Base 128-byte inode fields: mode 0, UID low 2, size low 4, atime 8, ctime 12, mtime 16, deletion time 20, GID low 24, links 26, i_blocks 28, flags 32, OS-specific 36. Fifteen 32-bit block pointers start at 40; generation 100, external EA block 104, directory ACL or regular-file size high 108, fragment address 112, OS-specific bytes 116..127. Linux UID/GID high halves at 120/122. ctime means inode-status change, not creation. i_blocks counts 512-byte units, including indirect and applicable EA blocks, not logical file blocks. Decode large regular-file size only under the admitted format.

## Direct and indirect file mapping

Let N=B/4. Inode pointers 0..11 map logical blocks 0..11; pointer 12 addresses N single-indirect entries, 13 addresses N*N double-indirect entries, and 14 addresses N*N*N triple-indirect entries. For logical k>=12, subtract 12, then subtract each preceding level's coverage until the correct level is selected. Divide by the appropriate power of N to select each successive pointer index. Example with B=4096 and N=1024: logical block 1040 is the fifth block of the double-indirect region (zero-based index 4), so use inode pointer 13, first index 0, then index 4. A zero pointer represents a hole over its subtree coverage; an invalid pointer is corruption, not a hole. Verify ownership, range and cycles for every indirect block; clip reads to EOF and never expose allocation slack.

## Directory records and indexed namespaces

Old directory records contain inode 0 (32 bits), record length 4 (16 bits), 16-bit name length 6 and name at 8. With FILETYPE, name length is byte 6 and file type byte 7. In either case minimum record length is align_up(8+name_length,4). Inode zero marks an unused entry; never advance by zero or cross a block boundary. Filetype values 1..7 denote regular, directory, character, block, FIFO, socket and symlink. An indexed directory uses HTree root and interior records hidden behind directory-compatible layouts; use the hash version and seed, validate sorted hash intervals and collisions, and still compare exact names in leaf directory entries. Preserve dot/dot-dot and link counts. A fast symlink can store up to 60 target bytes in i_block; determine eligibility from native allocation/type rules, not length alone.

## Extended attributes and special objects

An external EA block uses magic 0xEA020000, reference count, block count and hash in its header, followed by variable entries. Each entry has one-byte name length and namespace index, 16-bit value offset, 32-bit value-block field, value length and hash. Entries and values have four-byte alignment; verify separate bounds and shared-block reference counts. Do not treat an external EA block as ordinary file payload or reclaim it while shared. On supported larger inode variants the inode tail may contain inline extended attributes; validate the extra-inode-size field before decoding that area. Preserve ACL namespaces and unknown attributes. Device nodes encode device identity in i_block rather than data pointers. Distinguish special files, sparse files and symlinks when reconstructing ownership.

## Structural checking and formatting

Reconstruct all metadata reservations, allocated inode identities, direct and indirect trees, EA ownership, namespace reachability and hard-link counts. Check duplicate ownership against native sharing rules, compare bitmaps and counters, and distinguish damaged references from genuinely free space. A formatter must establish valid superblock copies, descriptors, bitmaps, reserved inode records, root inode and root directory, and all required features before publishing a usable state. An independent formatter oracle can create fixtures; it must not be a production fallback. Test block sizes 1,2,4 KiB, different inode sizes, sparse-super groups, partial final groups, direct/indirect transitions, sparse subtrees, large file sizes, invalid record lengths and metadata overlap.

## Non-journalled update ordering

Ext2 has no native commit log. A conservative allocation sequence reserves blocks before making live pointers refer to them, initializes content and metadata before publishing namespace reachability, and flushes according to the storage contract. Removal clears live references before returning blocks to free space; link counts and directory entries must be reconciled. Interrupted operations can still leak blocks or leave incomplete directory relationships; ordering alone cannot provide all-or-nothing transactions. Never advertise crash-atomic relocation solely on the basis of successful ordinary writes. A proprietary recovery extension must not be presented as native ext2 metadata.

## Relocation and filesystem checking

To move an allocated data block, establish exclusive ownership and a clean image, reserve a destination, copy and verify the payload, durably publish the exact inode or indirect pointer, and retire the source only after the new reference is safe. Moving indirect blocks requires updating their owning parent pointer. Reconcile counters and preserve timestamps, permissions and hard-link identities as appropriate. A crash between these steps may require a full consistency check; test every boundary on disposable copies. The native checker can resolve some disagreements by reconnecting or clearing damaged objects; those decisions are not a blanket authorization to modify the original evidence.

## Implementation and qualification boundary

At historical source revision 09beddd958e7 the hosted descriptor included read, write, CREATE and format, guarded by exclusive host access. A CREATE callback does not establish crash-atomic repair or relocation. Historical workflow 45534075017 records a specific tested scope.

Common V2 is the independent provider interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate real operation masks and media profiles. Use bounded host I/O, exclusive unmounted ownership and durable flush for mutation; distinguish unsupported, corrupt, I/O and stale-handle results. Test Media and Defragmenter use the same engine. Hosted, freestanding, BOOT, kernel, recovery and package qualification are independent. Current capability and release status belongs to the repository README and exact-source qualification records; historical evidence is not a current status claim.

Historical source or test reference: https://github.com/Infiltrator-Projects/Filesystem-EXT2/actions/runs/45534075017 .

## Sources and outstanding qualification

Pinned source: https://github.com/torvalds/linux/blob/v2.6.32/include/linux/ext2_fs.h ; algorithms in https://github.com/torvalds/linux/tree/v2.6.32/fs/ext2 . Independent utilities: https://github.com/tytso/e2fsprogs . Cross-check authentic revision-zero and dynamic images, EA variants, legacy indexed directories and sparse pointers. Exact META_BG layout, optional OS-specific fields and HTree algorithms need dedicated tests.  This reference documents native format semantics, not the current Common V2 implementation or release qualification.
