/* SPDX-License-Identifier: GPL-2.0-or-later
 * The portable kernel/firmware-ready EXT2 engine has no dependency on libc,
 * Common or the historical V1 module-entry contract.
 */
#include "ext2_module.h"
int main(void)
{
    const IfsExt2NativeOps *ops = ifs_ext2_native_operations();
    return (!ops || !ops->mount || !ops->root || !ops->lookup ||
            !ops->enumerate || !ops->read || !ops->write_existing);
}
