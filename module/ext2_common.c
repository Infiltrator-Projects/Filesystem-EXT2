/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2-only adapter to the canonical, freestanding EXT2 engine.
 * No legacy provider descriptor, alternate parser or POSIX device handles.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../native/filesystems/ext2/module/ext2_module.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    IfsFsHostRuntimeV2 host;
    IfsExt2DropInVolume media;
} CommonVolume;

static const IfsExt2NativeOps *engine(void)
{
    return ifs_ext2_native_operations();
}

static int io_usable(const IfsFsHostRuntimeV2 *h)
{
    return ifs_fs_host_v2_valid(h) && h->volume_bytes >= 2048u;
}

static IfsExt2DropInIo native_io(const IfsFsHostRuntimeV2 *h)
{
    IfsExt2DropInIo io = {0};
    io.device.read_at = h->read_at;
    io.device.write_at = h->write_at;
    io.device.flush = h->flush;
    io.device.user = h->context;
    io.volume_bytes = h->volume_bytes;
    return io;
}

static int status(IfsExt2Status s)
{
    return s == IFS_EXT2_OK ? IFS_FS_OK : IFS_FS_CORRUPT;
}

/* No format sniffing in a consumer: the real EXT2 engine validates media. */
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    IfsExt2DropInVolume media = {0};
    IfsExt2DropInIo io;
    if (!io_usable(h) || !variant) return IFS_FS_INVALID;
    io = native_io(h);
    if (engine()->mount(&media, &io, 0) != IFS_EXT2_OK)
        return IFS_FS_NOT_FOUND;
    media.mounted = 0u;
    *variant = 2u;
    return IFS_FS_OK;
}

static int mount(const IfsFsHostRuntimeV2 *h, void **out,
                 IfsFsVolumeInfoV2 *info)
{
    CommonVolume *v;
    IfsExt2DropInIo io;
    int allow_write;
    if (!out || !info || info->structure_bytes < sizeof(*info) ||
        !io_usable(h) || !h->allocate || !h->release)
        return IFS_FS_INVALID;
    *out = NULL;
    v = (CommonVolume *)h->allocate(h->context, sizeof(*v));
    if (!v) return IFS_FS_NO_MEMORY;
    memset(v, 0, sizeof(*v));
    v->host = *h;
    io = native_io(&v->host);
    allow_write = !(h->capabilities & IFS_FS_HOST_READ_ONLY) &&
        (h->capabilities & IFS_FS_HOST_EXCLUSIVE) && h->write_at && h->flush;
    if (engine()->mount(&v->media, &io, allow_write) != IFS_EXT2_OK) {
        h->release(h->context, v);
        return IFS_FS_CORRUPT;
    }
    info->variant = 2u;
    info->block_bytes = v->media.fs.block_size;
    info->max_name_bytes = IFS_EXT2_MAX_NAME_LENGTH;
    info->total_bytes = v->media.fs.byte_size;
    info->free_bytes = v->media.fs.free_blocks * v->media.fs.block_size;
    *out = v;
    return IFS_FS_OK;
}

static int valid(const CommonVolume *v)
{
    return v && v->media.mounted && v->host.read_at &&
        v->media.host.device.user == v->host.context;
}

static int unmount(void *volume)
{
    CommonVolume *v = (CommonVolume *)volume;
    IfsFsHostRuntimeV2 h;
    if (!valid(v)) return IFS_FS_INVALID;
    h = v->host;
    v->media.mounted = 0u;
    v->media.writable = 0u;
    h.release(h.context, v);
    return IFS_FS_OK;
}

static void *scratch(CommonVolume *v)
{
    return v->host.allocate(v->host.context, v->media.fs.block_size);
}

static void unscratch(CommonVolume *v, void *p)
{
    if (p) v->host.release(v->host.context, p);
}

/* Common paths are absolute within one mounted volume. Reject relative,
 * empty, dot and dot-dot components rather than normalising them silently. */
static int resolve(CommonVolume *v, const char *path,
                   IfsExt2Inode *inode, const char **basename,
                   uint32_t *basename_bytes, void *work)
{
    const char *p;
    IfsExt2Status result;
    if (!valid(v) || !path || path[0] != '/' || !inode || !work)
        return IFS_FS_INVALID;
    result = engine()->root(&v->media, inode, work, v->media.fs.block_size);
    if (result != IFS_EXT2_OK) return status(result);
    p = path + 1;
    if (!*p) {
        if (basename) *basename = "/";
        if (basename_bytes) *basename_bytes = 1u;
        return IFS_FS_OK;
    }
    while (*p) {
        const char *first = p;
        size_t count;
        while (*p && *p != '/') ++p;
        count = (size_t)(p - first);
        if (!count || count > IFS_EXT2_MAX_NAME_LENGTH ||
            (count == 1u && first[0] == '.') ||
            (count == 2u && first[0] == '.' && first[1] == '.'))
            return IFS_FS_INVALID;
        if (ifs_ext2_inode_type(inode) != IFS_EXT2_NODE_DIRECTORY)
            return IFS_FS_NOT_FOUND;
        result = engine()->lookup(&v->media, inode->number, first,
            (ifs_ext2_u32)count, inode, work, v->media.fs.block_size);
        if (result != IFS_EXT2_OK) return IFS_FS_NOT_FOUND;
        if (basename) *basename = first;
        if (basename_bytes) *basename_bytes = (uint32_t)count;
        if (*p == '/' && !*++p) return IFS_FS_INVALID;
    }
    return IFS_FS_OK;
}

static int fill(IfsFsEntryV2 *entry, const IfsExt2Inode *inode,
                const char *name, uint32_t name_bytes)
{
    IfsExt2NodeType t;
    if (!entry || entry->structure_bytes < sizeof(*entry) ||
        !entry->name || !name || name_bytes >= entry->name_capacity)
        return IFS_FS_INVALID;
    t = ifs_ext2_inode_type(inode);
    if (t != IFS_EXT2_NODE_REGULAR &&
        t != IFS_EXT2_NODE_DIRECTORY &&
        t != IFS_EXT2_NODE_SYMLINK)
        return IFS_FS_UNSUPPORTED;
    memcpy(entry->name, name, name_bytes);
    entry->name[name_bytes] = '\0';
    entry->name_bytes = name_bytes;
    entry->size_bytes = inode->size;
    entry->attributes = inode->mode;
    entry->type = t == IFS_EXT2_NODE_DIRECTORY ? IFS_FS_ENTRY_DIRECTORY :
        t == IFS_EXT2_NODE_SYMLINK ? IFS_FS_ENTRY_LINK : IFS_FS_ENTRY_FILE;
    return IFS_FS_OK;
}

static int stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    CommonVolume *v = (CommonVolume *)volume;
    IfsExt2Inode inode;
    const char *name = NULL;
    uint32_t len = 0u;
    int result;
    void *work;
    if (!valid(v) || !entry) return IFS_FS_INVALID;
    work = scratch(v);
    if (!work) return IFS_FS_NO_MEMORY;
    result = resolve(v, path, &inode, &name, &len, work);
    if (!result) result = fill(entry, &inode, name, len);
    unscratch(v, work);
    return result;
}

typedef struct {
    CommonVolume *volume;
    IfsFsEntryV2 *entry;
    uint32_t skip, current;
    void *lookup_scratch;
    int result;
    int found;
} EnumerateState;

static int enumerate_callback(void *opaque, ifs_ext2_u32 number,
                              IfsExt2NodeType type, const char *name,
                              ifs_ext2_u8 bytes)
{
    EnumerateState *s = (EnumerateState *)opaque;
    IfsExt2Inode inode;
    (void)type;
    /* POSIX dot entries are not volume children. */
    if ((bytes == 1u && name[0] == '.') ||
        (bytes == 2u && name[0] == '.' && name[1] == '.'))
        return 0;
    if (s->current++ != s->skip) return 0;
    s->found = 1;
    if (ifs_ext2_read_inode(&s->volume->media.fs, number, &inode,
            s->lookup_scratch, s->volume->media.fs.block_size) != IFS_EXT2_OK)
        s->result = IFS_FS_CORRUPT;
    else s->result = fill(s->entry, &inode, name, bytes);
    return 1;
}

static int directory_entry(void *volume, const char *path,
                           uint32_t index, IfsFsEntryV2 *entry)
{
    CommonVolume *v = (CommonVolume *)volume;
    IfsExt2Inode parent;
    void *work = NULL, *callback_work = NULL;
    EnumerateState state = {0};
    IfsExt2Status result;
    int rc;
    if (!valid(v) || !entry) return IFS_FS_INVALID;
    work = scratch(v);
    callback_work = scratch(v);
    if (!work || !callback_work) {
        rc = IFS_FS_NO_MEMORY; goto done;
    }
    rc = resolve(v, path, &parent, NULL, NULL, work);
    if (rc) goto done;
    if (ifs_ext2_inode_type(&parent) != IFS_EXT2_NODE_DIRECTORY) {
        rc = IFS_FS_INVALID; goto done;
    }
    state.volume = v;
    state.entry = entry;
    state.skip = index;
    state.lookup_scratch = callback_work;
    state.result = IFS_FS_END;
    result = ifs_ext2_iterate_directory(&v->media.fs, &parent,
        enumerate_callback, &state, work, v->media.fs.block_size);
    if (state.found && result == IFS_EXT2_STOP) rc = state.result;
    else if (result == IFS_EXT2_OK) rc = IFS_FS_END;
    else rc = IFS_FS_CORRUPT;
done:
    unscratch(v, callback_work);
    unscratch(v, work);
    return rc;
}

static int file_io(void *volume, const char *path, uint64_t offset,
                   void *buffer, uint32_t length, int write)
{
    CommonVolume *v = (CommonVolume *)volume;
    IfsExt2Inode inode;
    ifs_ext2_u32 done = 0u;
    void *work;
    int rc;
    IfsExt2Status result;
    if (!valid(v) || (!buffer && length)) return IFS_FS_INVALID;
    if (write && !v->media.writable) return IFS_FS_ACCESS;
    work = scratch(v);
    if (!work) return IFS_FS_NO_MEMORY;
    rc = resolve(v, path, &inode, NULL, NULL, work);
    if (rc) goto finish;
    if (ifs_ext2_inode_type(&inode) != IFS_EXT2_NODE_REGULAR) {
        rc = IFS_FS_INVALID; goto finish;
    }
    if (offset > inode.size || length > inode.size - offset) {
        rc = IFS_FS_UNSUPPORTED; goto finish;
    }
    if (write)
        result = engine()->write_existing(&v->media, inode.number, offset,
            buffer, length, work, v->media.fs.block_size, &done);
    else
        result = engine()->read(&v->media, inode.number, offset,
            buffer, length, work, v->media.fs.block_size, &done);
    rc = result == IFS_EXT2_OK && done == length ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
finish:
    unscratch(v, work);
    return rc;
}
static int read_file(void *v, const char *path, uint64_t pos, void *b, uint32_t n)
{
    return file_io(v, path, pos, b, n, 0);
}
static int write_file(void *v, const char *path, uint64_t pos,
                      const void *b, uint32_t n)
{
    return file_io(v, path, pos, (void *)b, n, 1);
}

/* Advertise no create/remove/resize/repair/format until the canonical
 * EXT2 implementation actually supplies independently tested operations. */
static int volume_query(void *mounted, IfsFsVolumeInfoV1 *out)
{
    CommonVolume *v = (CommonVolume *)mounted;
    if (!valid(v) || !out || out->structure_bytes < sizeof(*out))
        return IFS_FS_INVALID;
    out->variant = 2u;
    out->block_bytes = v->media.fs.block_size;
    out->max_name_bytes = IFS_EXT2_MAX_NAME_LENGTH;
    out->total_bytes = v->media.fs.byte_size;
    out->free_bytes = v->media.fs.free_blocks * v->media.fs.block_size;
    return IFS_FS_OK;
}
static int volume_sync(void *mounted)
{
    CommonVolume *v = (CommonVolume *)mounted;
    if (!valid(v)) return IFS_FS_INVALID;
    if (!v->media.writable) return IFS_FS_OK;
    return v->host.flush && v->host.flush(v->host.context) == 0 ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
static const IfsFsVolumeOpsV1 volume_group = {
    .header = { IFS_FS_GROUP_ABI_V1, sizeof(IfsFsVolumeOpsV1),
        IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_SYNC },
    .query = volume_query,
    .sync = volume_sync
};
/* V2 CORE directly owns the implemented production read/write and namespace
 * operations. Missing allocate/create/repair/format callbacks are deliberate
 * capability denials rather than successful placeholders. */
static const IfsFsCoreOpsV1 core_group = {
    .header = {IFS_FS_GROUP_ABI_V1, sizeof(IfsFsCoreOpsV1),
        IFS_FS_GROUP_CORE, 0u,
        IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
        IFS_FS_CORE_STAT | IFS_FS_CORE_LIST |
        IFS_FS_CORE_READ | IFS_FS_CORE_WRITE},
    .probe = probe,
    .mount = mount,
    .unmount = unmount,
    .stat = stat_path,
    .directory_entry = directory_entry,
    .read = read_file,
    .write = write_file
};
static const IfsFsGroupHeaderV2 *query_group(uint32_t group,
                                              uint32_t minimum,
                                              uint32_t maximum)
{
    if (minimum > IFS_FS_GROUP_ABI_V1 || maximum < IFS_FS_GROUP_ABI_V1)
        return NULL;
    if (group == IFS_FS_GROUP_CORE) return &core_group.header;
    if (group == IFS_FS_GROUP_VOLUME) return &volume_group.header;
    return NULL;
}
static const IfsFsModuleApiV2 common_v2 = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "ext2",
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .base_v1 = NULL,
    .query_group = query_group
};

__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &common_v2;
}
