/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reject overlapping metadata at the canonical format boundary.
 * This test applies to both the shared-object engine and the Linux VFS
 * adapter, which compile the same ext2_core.c implementation.
 */
#include "ext2_core.h"
#include <stdio.h>
#include <string.h>

static int expect(const char *name, const IfsExt2Superblock *super,
                  const IfsExt2GroupDescriptor *group,
                  IfsExt2Status expected)
{
    const IfsExt2Status actual =
        ifs_ext2_validate_group_descriptor(super, 0U, group);
    if (actual != expected) {
        fprintf(stderr, "%s: returned %d, expected %d\n",
                name, (int)actual, (int)expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    IfsExt2Superblock super;
    IfsExt2GroupDescriptor group;
    int failures = 0;

    memset(&super, 0, sizeof(super));
    memset(&group, 0, sizeof(group));
    super.blocks_count = 33U;
    super.first_data_block = 1U;
    super.blocks_per_group = 32U;
    super.inode_table_blocks_per_group = 3U;
    super.group_count = 1U;

    group.block_bitmap = 3U;
    group.inode_bitmap = 4U;
    group.inode_table = 5U; /* Occupies blocks 5, 6 and 7. */
    failures += expect("nonoverlapping metadata", &super, &group, IFS_EXT2_OK);

    group.inode_bitmap = group.block_bitmap;
    failures += expect("identical bitmaps", &super, &group, IFS_EXT2_ERROR_CORRUPT);
    group.inode_bitmap = 4U;

    group.block_bitmap = 6U;
    failures += expect("block bitmap inside inode table", &super, &group,
                       IFS_EXT2_ERROR_CORRUPT);
    group.block_bitmap = 3U;

    group.inode_bitmap = 7U;
    failures += expect("inode bitmap at inode table end", &super, &group,
                       IFS_EXT2_ERROR_CORRUPT);
    group.inode_bitmap = 4U;

    group.inode_table = 31U; /* Blocks 31-33 exceed last valid block 32. */
    failures += expect("inode table outside group", &super, &group,
                       IFS_EXT2_ERROR_CORRUPT);

    return failures != 0;
}
