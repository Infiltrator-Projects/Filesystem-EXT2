/* SPDX-License-Identifier: GPL-3.0-or-later
 * Prove that the production freestanding V2 adapter cannot negotiate an
 * EXT2 write-capable native volume, regardless of the host's lease flags.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);

static unsigned reads, writes;
static int read_failure(void *context, uint64_t offset, void *data, uint32_t length)
{
    (void)context; (void)offset; (void)data; (void)length;
    ++reads;
    return -1;
}
static int write_failure(void *context, uint64_t offset,
                         const void *data, uint32_t length)
{
    (void)context; (void)offset; (void)data; (void)length;
    ++writes;
    return -1;
}
static int flush_ok(void *context)
{
    (void)context;
    return 0;
}
static void *allocate(void *context, size_t length)
{
    (void)context;
    return malloc(length);
}
static void release(void *context, void *pointer)
{
    (void)context;
    free(pointer);
}
static int check(int condition, const char *message)
{
    if (!condition) fprintf(stderr, "EXT2 firmware V2: %s\n", message);
    return condition ? 0 : 1;
}

int main(void)
{
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core;
    IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2,
        .structure_bytes = sizeof(IfsFsHostRuntimeV2),
        .capabilities = IFS_FS_HOST_EXCLUSIVE | IFS_FS_HOST_DURABLE,
        .volume_bytes = 32768u,
        .logical_sector_bytes = 512u,
        .physical_sector_bytes = 512u,
        .maximum_transfer_bytes = 512u,
        .read_at = read_failure,
        .write_at = write_failure,
        .flush = flush_ok,
        .allocate = allocate,
        .release = release
    };
    IfsFsVolumeInfoV2 info = { .structure_bytes = sizeof(IfsFsVolumeInfoV2) };
    void *mounted = (void *)(uintptr_t)1u;
    int result;
    if (check(ifs_fs_api_v2_valid(api, "ext2"),
              "invalid freestanding descriptor") ||
        check(api->runtime_profiles ==
                  (IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE),
              "wrong firmware profile"))
        return 1;
    core = (const IfsFsCoreOpsV2 *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    if (check(core != NULL && core->mount != NULL, "CORE.MOUNT missing"))
        return 1;
    if (check((core->header.capabilities & IFS_FS_CORE_WRITE) == 0u &&
              core->write == NULL, "firmware must not advertise writes") ||
        check(ifs_fs_host_v2_valid(&host), "invalid exclusive V2 fixture"))
        return 1;
    result = core->mount(&host, &mounted, &info);
    if (check(result == IFS_FS_ACCESS && mounted == NULL,
              "writable host accepted by firmware mount") ||
        check(reads == 0u && writes == 0u,
              "firmware accessed media before rejecting writable lease"))
        return 1;

    /* A valid read-only V2 host must get as far as the native disk probe.
     * Deliberate I/O failure must not become an accepted or fake mount. */
    host.capabilities = IFS_FS_HOST_READ_ONLY;
    host.write_at = NULL;
    host.flush = NULL;
    info.structure_bytes = sizeof(info);
    mounted = (void *)(uintptr_t)1u;
    if (check(ifs_fs_host_v2_valid(&host), "invalid read-only V2 fixture"))
        return 1;
    result = core->mount(&host, &mounted, &info);
    if (check(result == IFS_FS_IO_ERROR && mounted == NULL,
              "read failure was not reported through native V2 mount") ||
        check(reads > 0u && writes == 0u,
              "read-only fixture bypassed disk I/O or attempted a write"))
        return 1;
    return 0;
}
