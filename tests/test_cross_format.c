/* SPDX-License-Identifier: GPL-3.0-or-later
 * Cross-format data must never be accepted as EXT2 by the native V2 probe.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static int read_media(void *context, uint64_t offset, void *dest, uint32_t n)
{
    const uint8_t *bytes = context;
    if (offset > 4096u || n > 4096u-offset) return -1;
    memcpy(dest, bytes+offset, n);
    return 0;
}
static void *allocate(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p) { (void)ctx; free(p); }
int main(void)
{
    uint8_t bytes[4096] = {0};
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    if (!ifs_fs_api_v2_native_valid(api,"ext2")) return 1;
    const IfsFsCoreOpsV1 *core =
        (const IfsFsCoreOpsV1 *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    if (!core) return 1;
    IfsFsHostRuntimeV2 host = {
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=sizeof bytes,
        .logical_sector_bytes=512,
        .physical_sector_bytes=512,
        .maximum_transfer_bytes=1024,
        .context=bytes, .read_at=read_media,
        .allocate=allocate, .release=release
    };
    uint32_t variant=999u;
    if (core->probe(&host,&variant)==IFS_FS_OK) return 1;
    /* Genuine NTFS boot sector is not EXT2. */
    memcpy(bytes+3, "NTFS    ", 8);
    if (core->probe(&host,&variant)==IFS_FS_OK) return 1;
    memset(bytes,0,sizeof bytes);
    /* Genuine APFS container signature is not EXT2. */
    memcpy(bytes+32, "NXSB", 4);
    if (core->probe(&host,&variant)==IFS_FS_OK) return 1;
    host.abi_version=IFS_FS_COMMON_ABI_V1;
    if (core->probe(&host,&variant)==IFS_FS_OK) return 1;
    puts("EXT2 V2 native probe rejected unrelated media and invalid host");
    return 0;
}
