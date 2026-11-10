/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
int main(void)
{
    const IfsFsModuleApiV2 *m = infiltrator_filesystem_api_v2();
    if (!ifs_fs_api_v2_valid(m, "ext2") ||
        m->group_mask != (IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                          IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME))) {
        fputs("EXT2 is not a genuine native V2-only provider\n", stderr);
        return 1;
    }
    const IfsFsCoreOpsV2 *core =
        (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(m, IFS_FS_GROUP_CORE);
    if (!core || !core->probe || !core->mount || !core->unmount ||
        !core->stat || !core->directory_entry || !core->read ||
        !core->write || core->create || core->truncate || core->remove)
        return 1;
    return 0;
}
