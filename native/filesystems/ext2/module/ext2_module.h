/* SPDX-License-Identifier: GPL-2.0-or-later
 * EXT2.so standalone ABI. All offsets are partition-relative.
 * No Linux VFS, UEFI protocol or libc dependency. The host owns buffers,
 * callbacks and the volume object; a mounted volume must not be moved.
 */
#ifndef INFILTRATR_EXT2_DROPIN_H
#define INFILTRATR_EXT2_DROPIN_H
#include "../core/ext2_engine.h"

typedef struct IfsExt2DropInIo {
    IfsExt2Io device; /* host callbacks; read_at mandatory */
    ifs_ext2_u64 volume_bytes;
} IfsExt2DropInIo;

typedef struct IfsExt2DropInVolume {
    IfsExt2DropInIo host;
    IfsExt2Volume fs;
    unsigned int mounted;
    unsigned int writable;
} IfsExt2DropInVolume;

typedef struct IfsExt2NativeOps {
    IfsExt2Status (*mount)(IfsExt2DropInVolume *,
                           const IfsExt2DropInIo *, int allow_write);
    IfsExt2Status (*root)(const IfsExt2DropInVolume *,
                          IfsExt2Inode *, void *, ifs_ext2_u32);
    IfsExt2Status (*lookup)(const IfsExt2DropInVolume *,
                            ifs_ext2_u32 directory_inode,
                            const char *, ifs_ext2_u32 name_length,
                            IfsExt2Inode *, void *, ifs_ext2_u32);
    IfsExt2Status (*enumerate)(const IfsExt2DropInVolume *,
                               ifs_ext2_u32 directory_inode,
                               IfsExt2DirectoryCallback, void *,
                               void *, ifs_ext2_u32);
    IfsExt2Status (*read)(const IfsExt2DropInVolume *,
                          ifs_ext2_u32 inode_number, ifs_ext2_u64 offset,
                          void *, ifs_ext2_u32, void *, ifs_ext2_u32,
                          ifs_ext2_u32 *);
    IfsExt2Status (*write_existing)(const IfsExt2DropInVolume *,
                                    ifs_ext2_u32 inode_number,
                                    ifs_ext2_u64 offset, const void *,
                                    ifs_ext2_u32, void *, ifs_ext2_u32,
                                    ifs_ext2_u32 *);
} IfsExt2NativeOps;

/* Writes can only overwrite allocated bytes of an existing regular file.
 * No create, resize, allocation, sparse-hole writes, delete, journalling,
 * crash-safe transaction or formatting. A failed flush reports an I/O error
 * but does not roll back bytes already submitted by the host device.
 */
const IfsExt2NativeOps *ifs_ext2_native_operations(void);
#endif
