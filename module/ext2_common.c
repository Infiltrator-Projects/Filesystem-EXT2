/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2-only adapter to the canonical, freestanding EXT2 engine.
 * No legacy provider descriptor, alternate parser or POSIX device handles.
 */
#include <infiltratr/filesystem_module_v2.h>
#include "../native/filesystems/ext2/module/ext2_module.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

/* Serialize access to the native engine and the session table. The V2
 * host must never be able to retire an active volume from an I/O callback.
 * Return BUSY rather than waiting inside a reentrant host callback.
 * Per-volume parallel I/O requires separate qualification. */
static atomic_flag v2_operation_gate=ATOMIC_FLAG_INIT;
static int v2_enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&v2_operation_gate,memory_order_acquire);
}
static int v2_leave_operation(int rc)
{
    atomic_flag_clear_explicit(&v2_operation_gate,memory_order_release);
    return rc;
}


/* Static Common V2 module identity is discoverable without executing ELF. */
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
__attribute__((used, retain, section(".ifsmeta")))
static const char ext2_v2_identity[] = "IFS-MODULE:ext2:common2";
#endif

typedef struct {
    IfsFsHostRuntimeV2 host;
    IfsExt2DropInVolume media;
} CommonVolume;

/* A V2 caller holds an opaque lifetime token, never an allocator address.
 * Reusing the same allocation after unmount must not revive stale handles.
 * The table is deliberately bounded for both hosted and freestanding BOOT. */
#define IFS_EXT_V2_MAX_SESSIONS 32u
static struct {
    uintptr_t token;
    CommonVolume *volume;
} sessions[IFS_EXT_V2_MAX_SESSIONS];
static uintptr_t session_generation;
static CommonVolume *session_lookup(void *handle)
{
    const uintptr_t token=(uintptr_t)handle;
    if (!token) return NULL;
    for (size_t i=0;i<IFS_EXT_V2_MAX_SESSIONS;++i)
        if (sessions[i].token==token)
            return sessions[i].volume;
    return NULL;
}
static size_t session_free_slot(void)
{
    for (size_t i=0;i<IFS_EXT_V2_MAX_SESSIONS;++i)
        if (!sessions[i].token)
            return i;
    return IFS_EXT_V2_MAX_SESSIONS;
}

/* Snapshot only the V2 fields supplied by the host, including older
 * callers that end before later optional extensions. */
static void retain_host(IfsFsHostRuntimeV2 *dst, const IfsFsHostRuntimeV2 *src)
{
    const size_t bytes = src->structure_bytes < sizeof(*dst) ?
        src->structure_bytes : sizeof(*dst);
    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, bytes);
    dst->structure_bytes = (uint32_t)bytes;
}

static const IfsExt2NativeOps *engine(void)
{
    return ifs_ext2_native_operations();
}

static int io_usable(const IfsFsHostRuntimeV2 *h)
{
    return ifs_fs_host_v2_valid(h) && h->volume_bytes >= 2048u;
}

/* Host callbacks are raw block transfers, not byte streams. The native
 * EXT2 engine sometimes requests only one bitmap byte or part of a file.
 * Convert those requests to aligned, sector-sized transfers, preserving
 * the verified Common V2 exclusive and durable write policy. */
static int host_transfer(CommonVolume *v, uint64_t offset, void *buffer,
                         uint32_t bytes, int write)
{
    const IfsFsHostRuntimeV2 *h = &v->host;
    const uint32_t sector = h->logical_sector_bytes;
    const uint32_t max_aligned =
        h->maximum_transfer_bytes - h->maximum_transfer_bytes % sector;
    uint8_t *cursor = (uint8_t *)buffer;
    uint8_t *bounce = NULL;
    int status = -1;
    if ((!cursor && bytes) || !h->read_at || !sector || !max_aligned ||
        offset > h->volume_bytes || bytes > h->volume_bytes - offset ||
        (write && (!h->write_at || !h->flush ||
         !(h->capabilities & IFS_FS_HOST_EXCLUSIVE) ||
         !(h->capabilities & IFS_FS_HOST_DURABLE))))
        return -1;
    while (bytes) {
        const uint32_t in_sector = (uint32_t)(offset % sector);
        if (h->cancelled && h->cancelled(h->context)) goto done;
        if (in_sector || bytes < sector) {
            const uint64_t aligned = offset - in_sector;
            const uint32_t count =
                bytes < sector - in_sector ? bytes : sector - in_sector;
            if (aligned > h->volume_bytes ||
                sector > h->volume_bytes - aligned)
                goto done;
            if (!bounce) {
                bounce = h->allocate(h->context, sector);
                if (!bounce) goto done;
            }
            if (h->read_at(h->context, aligned, bounce, sector) != 0)
                goto done;
            if (write) {
                memcpy(bounce + in_sector, cursor, count);
                if (h->write_at(h->context, aligned, bounce, sector) != 0)
                    goto done;
            } else {
                memcpy(cursor, bounce + in_sector, count);
            }
            cursor += count;
            offset += count;
            bytes -= count;
        } else {
            uint32_t count = bytes < max_aligned ? bytes : max_aligned;
            count -= count % sector;
            if (!count || (write ?
                h->write_at(h->context, offset, cursor, count) :
                h->read_at(h->context, offset, cursor, count)) != 0)
                goto done;
            cursor += count;
            offset += count;
            bytes -= count;
        }
    }
    status = 0;
done:
    if (bounce) h->release(h->context, bounce);
    return status;
}
static int host_read(void *context, uint64_t offset, void *destination,
                     uint32_t bytes)
{
    return host_transfer(context, offset, destination, bytes, 0);
}
static int host_write(void *context, uint64_t offset, const void *source,
                      uint32_t bytes)
{
    return host_transfer(context, offset, (void *)source, bytes, 1);
}

static int host_flush(void *context)
{
    CommonVolume *v = context;
    return v->host.flush ? v->host.flush(v->host.context) : -1;
}

static IfsExt2DropInIo native_io(CommonVolume *v)
{
    IfsExt2DropInIo io = {0};
    io.device.read_at = host_read;
    io.device.write_at =
        (v->host.capabilities & IFS_FS_HOST_READ_ONLY) ? NULL : host_write;
    io.device.flush =
        (v->host.capabilities & IFS_FS_HOST_READ_ONLY) ? NULL : host_flush;
    io.device.user = v;
    io.volume_bytes = v->host.volume_bytes;
    return io;
}

/* Keep native failures distinct.  In particular, a disk read error is
 * neither a missing filesystem nor an absent path in Common V2. */
static int status(IfsExt2Status s)
{
    switch (s) {
    case IFS_EXT2_OK: return IFS_FS_OK;
    case IFS_EXT2_ERROR_MAGIC:
    case IFS_EXT2_ERROR_NOT_FOUND: return IFS_FS_NOT_FOUND;
    case IFS_EXT2_ERROR_IO: return IFS_FS_IO_ERROR;
    case IFS_EXT2_ERROR_ARGUMENT:
    case IFS_EXT2_ERROR_NOT_DIRECTORY:
    case IFS_EXT2_ERROR_IS_DIRECTORY: return IFS_FS_INVALID;
    case IFS_EXT2_ERROR_FEATURES:
    case IFS_EXT2_ERROR_REVISION:
    case IFS_EXT2_ERROR_JOURNALLED:
    case IFS_EXT2_ERROR_UNSUPPORTED: return IFS_FS_UNSUPPORTED;
    case IFS_EXT2_ERROR_NO_SPACE:
    case IFS_EXT2_ERROR_BUFFER_TOO_SMALL: return IFS_FS_NO_SPACE;
    case IFS_EXT2_STOP: return IFS_FS_END;
    default: return IFS_FS_CORRUPT;
    }
}

/* No format sniffing in a consumer: the real EXT2 engine validates media. */
static int probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    CommonVolume v = {0};
    IfsExt2DropInIo io;
    IfsExt2Status result;
    if (!variant) return IFS_FS_INVALID;
    *variant = 0u;
    if (!io_usable(h)) return IFS_FS_INVALID;
    retain_host(&v.host, h);
    io = native_io(&v);
    result = engine()->mount(&v.media, &io, 0);
    if (result != IFS_EXT2_OK) return status(result);
    v.media.mounted = 0u;
    *variant = 2u;
    return IFS_FS_OK;
}
static int guarded_probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant)
{
    if (variant) *variant=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(probe(h,variant));
}


static int mount(const IfsFsHostRuntimeV2 *h, void **out,
                 IfsFsVolumeInfoV2 *info)
{
    CommonVolume *v;
    IfsExt2DropInIo io;
    int allow_write;
    IfsExt2Status result;
    if (!out || !info || info->structure_bytes < sizeof(*info) ||
        !io_usable(h) || !h->allocate || !h->release)
        return IFS_FS_INVALID;
    *out = NULL;
#if defined(IFS_EXT2_FIRMWARE_BUILD)
    /* The firmware descriptor exposes only a read-only namespace. A
     * writable host must never result in a writable native mount, even
     * when it supplies a valid exclusive and durable Common V2 lease. */
    if (!(h->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
#endif
    const size_t slot=session_free_slot();
    if (slot==IFS_EXT_V2_MAX_SESSIONS ||
        session_generation==UINTPTR_MAX) return IFS_FS_BUSY;
    v = (CommonVolume *)h->allocate(h->context, sizeof(*v));
    if (!v) return IFS_FS_NO_MEMORY;
    memset(v, 0, sizeof(*v));
    retain_host(&v->host, h);
    io = native_io(v);
    allow_write = !(h->capabilities & IFS_FS_HOST_READ_ONLY) &&
        (h->capabilities & IFS_FS_HOST_EXCLUSIVE) && h->write_at && h->flush;
    result = engine()->mount(&v->media, &io, allow_write);
    if (result != IFS_EXT2_OK) {
        h->release(h->context, v);
        return status(result);
    }
    info->variant = 2u;
    info->block_bytes = v->media.fs.block_size;
    info->max_name_bytes = IFS_EXT2_MAX_NAME_LENGTH;
    info->total_bytes = v->media.fs.byte_size;
    info->free_bytes = v->media.fs.free_blocks * v->media.fs.block_size;
    const uintptr_t token=++session_generation;
    sessions[slot].token=token;
    sessions[slot].volume=v;
    *out = (void *)token;
    return IFS_FS_OK;
}
static int guarded_mount(const IfsFsHostRuntimeV2 *h, void **out,
                 IfsFsVolumeInfoV2 *info)
{
    if (out) *out=NULL;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(mount(h,out,info));
}


static int valid(const CommonVolume *v)
{
    return v && v->media.mounted && v->host.read_at &&
        v->media.host.device.user == v;
}

static int unmount(void *volume)
{
    CommonVolume *v=session_lookup(volume);
    IfsFsHostRuntimeV2 h;
    if (!valid(v)) return IFS_FS_INVALID;
    for(size_t i=0;i<IFS_EXT_V2_MAX_SESSIONS;++i) {
        if(sessions[i].token==(uintptr_t)volume) {
            sessions[i].token=0u;
            sessions[i].volume=NULL;
            break;
        }
    }
    h = v->host;
    v->media.mounted = 0u;
    v->media.writable = 0u;
    h.release(h.context, v);
    return IFS_FS_OK;
}
static int guarded_unmount(void *volume)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(unmount(volume));
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
        if (result != IFS_EXT2_OK) return status(result);
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
        !entry->name || !name)
        return IFS_FS_INVALID;
    /* The structure is valid; only the caller-owned output buffer is small.
     * No entry fields or name bytes are modified on this failure. */
    if (name_bytes >= entry->name_capacity)
        return IFS_FS_NO_SPACE;
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
    CommonVolume *v = session_lookup(volume);
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
static int guarded_stat_path(void *volume, const char *path, IfsFsEntryV2 *entry)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(stat_path(volume,path,entry));
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
    {
        IfsExt2Status result = ifs_ext2_read_inode(
            &s->volume->media.fs, number, &inode, s->lookup_scratch,
            s->volume->media.fs.block_size);
        s->result = result == IFS_EXT2_OK ?
            fill(s->entry, &inode, name, bytes) : status(result);
    }
    return 1;
}

static int directory_entry(void *volume, const char *path,
                           uint32_t index, IfsFsEntryV2 *entry)
{
    CommonVolume *v = session_lookup(volume);
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
    else rc = status(result);
done:
    unscratch(v, callback_work);
    unscratch(v, work);
    return rc;
}
static int guarded_directory_entry(void *volume, const char *path,
                           uint32_t index, IfsFsEntryV2 *entry)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(directory_entry(volume,path,index,entry));
}


static int file_io(void *volume, const char *path, uint64_t offset,
                   void *buffer, uint32_t length, int write)
{
    CommonVolume *v = session_lookup(volume);
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
        /* Reads outside the file are invalid; extending writes remain
         * unsupported until a qualified allocation transaction exists. */
        rc = write ? IFS_FS_UNSUPPORTED : IFS_FS_INVALID;
        goto finish;
    }
    if (write)
        result = engine()->write_existing(&v->media, inode.number, offset,
            buffer, length, work, v->media.fs.block_size, &done);
    else
        result = engine()->read(&v->media, inode.number, offset,
            buffer, length, work, v->media.fs.block_size, &done);
    rc = result != IFS_EXT2_OK ? status(result) :
        done == length ? IFS_FS_OK : IFS_FS_IO_ERROR;
finish:
    unscratch(v, work);
    return rc;
}
static int read_file(void *v, const char *path, uint64_t pos, void *b, uint32_t n)
{
    return file_io(v, path, pos, b, n, 0);
}
static int guarded_read_file(void *v, const char *path, uint64_t pos, void *b, uint32_t n)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(read_file(v,path,pos,b,n));
}

#if !defined(IFS_EXT2_FIRMWARE_BUILD)
static int write_file(void *v, const char *path, uint64_t pos,
                      const void *b, uint32_t n)
{
    return file_io(v, path, pos, (void *)b, n, 1);
}
static int guarded_write_file(void *v, const char *path, uint64_t pos,
                      const void *b, uint32_t n)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(write_file(v,path,pos,b,n));
}

#endif

/* Expose the canonical EXT2 block map through Common V2. This is an
 * actual verified extent enumerator, not a relocation capability. No planner
 * or execute callback is advertised until their transaction engine exists. */
static int extent_map(void *volume, const char *path, uint64_t first_extent,
                      IfsFsExtentV2 *extents, uint32_t capacity,
                      uint32_t *written)
{
    CommonVolume *v = session_lookup(volume);
    IfsExt2Inode inode;
    uint64_t logical, index = 0u;
    uint64_t blocks;
    uint32_t count = 0u;
    void *work;
    int rc;
    if (!valid(v) || !path || !written || !capacity || !extents)
        return IFS_FS_INVALID;
    *written = 0u;
    work = scratch(v);
    if (!work) return IFS_FS_NO_MEMORY;
    rc = resolve(v, path, &inode, NULL, NULL, work);
    if (rc) goto done;
    if (ifs_ext2_inode_type(&inode) != IFS_EXT2_NODE_REGULAR) {
        rc = IFS_FS_UNSUPPORTED;
        goto done;
    }
    blocks = inode.size / v->media.fs.block_size +
        (inode.size % v->media.fs.block_size != 0u);
    if (blocks > UINT32_MAX) {
        rc = IFS_FS_UNSUPPORTED;
        goto done;
    }
    for (logical = 0u; logical < blocks; ++logical) {
        ifs_ext2_u64 physical = 0u;
        int hole = 0;
        IfsExt2Status native_status;
        if (v->host.cancelled && (logical & 255u) == 0u &&
            v->host.cancelled(v->host.context)) {
            rc = IFS_FS_BUSY;
            goto done;
        }
        native_status = ifs_ext2_map_file_block(&v->media.fs, &inode,
            (ifs_ext2_u32)logical, &physical, &hole,
            work, v->media.fs.block_size);
        if (native_status != IFS_EXT2_OK) {
            rc = status(native_status);
            goto done;
        }
        if (hole) continue;
        native_status = ifs_ext2_validate_allocated_data_block(
            &v->media.fs, physical, work, v->media.fs.block_size);
        if (native_status != IFS_EXT2_OK) {
            rc = status(native_status);
            goto done;
        }
        if (index++ < first_extent) continue;
        extents[count].logical_offset = logical * v->media.fs.block_size;
        extents[count].physical_offset = physical * v->media.fs.block_size;
        extents[count].length = v->media.fs.block_size;
        extents[count].flags = 0u;
        extents[count].reserved = 0u;
        if (++count == capacity) break;
    }
    *written = count;
    rc = count ? IFS_FS_OK : IFS_FS_END;
done:
    unscratch(v, work);
    return rc;
}
static int guarded_extent_map(void *volume, const char *path, uint64_t first_extent,
                      IfsFsExtentV2 *extents, uint32_t capacity,
                      uint32_t *written)
{
    if (written) *written=0u;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(extent_map(volume,path,first_extent,extents,capacity,written));
}


static const IfsFsDefragOpsV2 defrag_group = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsDefragOpsV2),
        IFS_FS_GROUP_DEFRAG, 0u, IFS_FS_DEFRAG_MAP },
    .map = guarded_extent_map
};

/* Advertise no create/remove/resize/repair until the canonical
 * EXT2 implementation actually supplies independently tested operations. */
static int volume_query(void *mounted, IfsFsVolumeInfoV2 *out)
{
    CommonVolume *v = session_lookup(mounted);
    if (!valid(v) || !out || out->structure_bytes < sizeof(*out))
        return IFS_FS_INVALID;
    out->variant = 2u;
    out->block_bytes = v->media.fs.block_size;
    out->max_name_bytes = IFS_EXT2_MAX_NAME_LENGTH;
    out->total_bytes = v->media.fs.byte_size;
    out->free_bytes = v->media.fs.free_blocks * v->media.fs.block_size;
    return IFS_FS_OK;
}
static int guarded_volume_query(void *mounted, IfsFsVolumeInfoV2 *out)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_query(mounted,out));
}

static int volume_sync(void *mounted)
{
    CommonVolume *v = session_lookup(mounted);
    if (!valid(v)) return IFS_FS_INVALID;
    if (!v->media.writable) return IFS_FS_OK;
    return v->host.flush && v->host.flush(v->host.context) == 0 ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
static int guarded_volume_sync(void *mounted)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(volume_sync(mounted));
}

#if !defined(IFS_EXT2_FIRMWARE_BUILD)
/* Native EXT2 empty-volume formatting. The filesystem owns every on-disk
 * record; the caller provides only the exclusive, durable Common V2 lease.
 * This initial profile uses 4 KiB blocks, 256-byte inodes and complete
 * superblock/descriptor backups (no sparse-super, journal or extents).
 * No pathname, external formatter or kernel mount is used. */
enum { FMT_BLOCK=4096u, FMT_BPG=32768u, FMT_IPG=1024u,
       FMT_INODE_BYTES=256u, FMT_ITABLE=64u, FMT_META=68u };
static void fmt16(uint8_t *p,uint32_t x)
{ p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8); }
static void fmt32(uint8_t *p,uint32_t x)
{ fmt16(p,x);fmt16(p+2,x>>16); }
static int fmt_write(const IfsFsHostRuntimeV2 *h,uint64_t at,
                     const void *data,uint32_t n)
{
    if(h->cancelled && h->cancelled(h->context))return IFS_FS_BUSY;
    return ifs_fs_host_write_bytes_v2(h,at,data,n);
}
static void fmt_bit(uint8_t *bitmap,uint32_t bit)
{ bitmap[bit/8u]|=(uint8_t)(1u<<(bit%8u)); }
static int format_ext2(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                        const char *label,uint32_t flags)
{
    if(!io_usable(h))return IFS_FS_INVALID;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    /* Without stable device identities, conservatively require all local
     * sessions closed before modifying any volume. */
    for(size_t i=0;i<IFS_EXT_V2_MAX_SESSIONS;++i)
        if(sessions[i].token)return IFS_FS_BUSY;
    if(variant!=2u || flags || h->logical_sector_bytes>FMT_BLOCK ||
       h->volume_bytes%FMT_BLOCK)return IFS_FS_UNSUPPORTED;
    const uint64_t count=h->volume_bytes/FMT_BLOCK;
    /* A one-block group descriptor table holds up to 128 entries.
     * Refuse a short final group instead of rounding off its capacity. */
    if(count<4096u || count>128ull*FMT_BPG)return IFS_FS_UNSUPPORTED;
    const uint32_t blocks=(uint32_t)count;
    const uint32_t groups=(blocks+FMT_BPG-1u)/FMT_BPG;
    const uint32_t tail=blocks-(groups-1u)*FMT_BPG;
    if(tail<=FMT_META+1u)return IFS_FS_UNSUPPORTED;
    const size_t label_bytes=label?strlen(label):0u;
    if(label_bytes>16u)return IFS_FS_INVALID;

    uint8_t uuid[16]={0};
    if(h->structure_bytes>=sizeof(*h) && h->random_bytes) {
        if(ifs_fs_host_random_bytes_v2(h,uuid,sizeof(uuid))!=IFS_FS_OK)
            return IFS_FS_IO_ERROR;
        uuid[6]=(uint8_t)((uuid[6]&15u)|64u);
        uuid[8]=(uint8_t)((uuid[8]&63u)|128u);
    }
    uint32_t now=0u;
    if(h->realtime_ns) {
        const uint64_t seconds=h->realtime_ns(h->context)/1000000000ull;
        if(seconds<=UINT32_MAX)now=(uint32_t)seconds;
    }

    uint8_t block[FMT_BLOCK]={0},desc[FMT_BLOCK]={0},sb[1024]={0};
    uint32_t free_blocks=0u;
    int rc;
    /* Clear the primary signature first. No new valid superblock is
     * published until all allocation metadata and root data are durable. */
    for(uint32_t g=0u;g<groups;++g) {
        rc=fmt_write(h,(uint64_t)g*FMT_BPG*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    for(uint32_t g=0u;g<groups;++g) {
        const uint32_t base=g*FMT_BPG;
        const uint32_t span=g+1u==groups?tail:FMT_BPG;
        const uint32_t used=FMT_META+(g==0u?1u:0u);
        const uint32_t available=span-used;
        uint8_t *d=desc+g*32u;
        fmt32(d,base+2u);fmt32(d+4,base+3u);fmt32(d+8,base+4u);
        fmt16(d+12,available);fmt16(d+14,FMT_IPG-(g==0u?10u:0u));
        fmt16(d+16,g==0u?1u:0u);
        free_blocks+=available;
        /* Mark metadata and nonexistent tail blocks allocated. */
        memset(block,0,sizeof(block));
        for(uint32_t bit=0u;bit<used;++bit)fmt_bit(block,bit);
        for(uint32_t bit=span;bit<FMT_BPG;++bit)fmt_bit(block,bit);
        rc=fmt_write(h,(uint64_t)(base+2u)*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
        /* Inode bits 0..9 are reserved in group zero (root is inode 2).
         * Bits beyond the declared inode count are unavailable. */
        memset(block,0xff,sizeof(block));
        memset(block,0,FMT_IPG/8u);
        if(!g){block[0]=0xffu;block[1]=3u;}
        rc=fmt_write(h,(uint64_t)(base+3u)*FMT_BLOCK,block,sizeof(block));
        if(rc!=IFS_FS_OK)return rc;
        for(uint32_t i=0u;i<FMT_ITABLE;++i) {
            memset(block,0,sizeof(block));
            if(!g && !i) {
                uint8_t *inode=block+FMT_INODE_BYTES;
                fmt16(inode,0040755u);
                fmt32(inode+4,FMT_BLOCK);
                fmt32(inode+8,now);fmt32(inode+12,now);fmt32(inode+16,now);
                fmt16(inode+26,2u);fmt32(inode+28,FMT_BLOCK/512u);
                fmt32(inode+40,FMT_META);
            }
            rc=fmt_write(h,(uint64_t)(base+4u+i)*FMT_BLOCK,block,sizeof(block));
            if(rc!=IFS_FS_OK)return rc;
        }
    }
    memset(block,0,sizeof(block));
    fmt32(block,2u);fmt16(block+4,12u);block[6]=1u;block[7]=2u;block[8]='.';
    fmt32(block+12,2u);fmt16(block+16,FMT_BLOCK-12u);
    block[18]=2u;block[19]=2u;block[20]='.';block[21]='.';
    rc=fmt_write(h,(uint64_t)FMT_META*FMT_BLOCK,block,sizeof(block));
    if(rc!=IFS_FS_OK)return rc;
    for(uint32_t g=0u;g<groups;++g) {
        rc=fmt_write(h,((uint64_t)g*FMT_BPG+1u)*FMT_BLOCK,desc,sizeof(desc));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;

    fmt32(sb,groups*FMT_IPG);fmt32(sb+4,blocks);
    fmt32(sb+12,free_blocks);fmt32(sb+16,groups*FMT_IPG-10u);
    fmt32(sb+24,2u);fmt32(sb+28,2u);
    fmt32(sb+32,FMT_BPG);fmt32(sb+36,FMT_BPG);fmt32(sb+40,FMT_IPG);
    fmt32(sb+48,now);fmt16(sb+54,0xffffu);
    fmt16(sb+56,0xef53u);fmt16(sb+58,1u);fmt16(sb+60,1u);
    fmt32(sb+64,now);fmt32(sb+76,1u);
    fmt32(sb+84,11u);fmt16(sb+88,FMT_INODE_BYTES);
    fmt32(sb+96,2u); /* directory entry file-type feature */
    fmt32(sb+100,2u); /* large-file read-only compatibility feature */
    memcpy(sb+104,uuid,sizeof(uuid));
    if(label_bytes)memcpy(sb+120,label,label_bytes);
    fmt16(sb+254,32u);
    for(uint32_t g=1u;g<groups;++g) {
        fmt16(sb+90,g);
        rc=fmt_write(h,(uint64_t)g*FMT_BPG*FMT_BLOCK,sb,sizeof(sb));
        if(rc!=IFS_FS_OK)return rc;
    }
    if(h->flush(h->context)!=0)return IFS_FS_IO_ERROR;
    fmt16(sb+90,0u);
    rc=fmt_write(h,1024u,sb,sizeof(sb));
    if(rc!=IFS_FS_OK)return rc;
    if(h->flush(h->context)!=0) {
        memset(sb,0,sizeof(sb));
        (void)ifs_fs_host_write_bytes_v2(h,1024u,sb,sizeof(sb));
        (void)h->flush(h->context);
        return IFS_FS_IO_ERROR;
    }
    uint32_t observed=0u;
    rc=probe(h,&observed);
    return rc!=IFS_FS_OK?rc:observed==2u?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int guarded_format_ext2(const IfsFsHostRuntimeV2 *h,uint32_t variant,
                        const char *label,uint32_t flags)
{
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    return v2_leave_operation(format_ext2(h,variant,label,flags));
}

#endif /* hosted format */

static const IfsFsVolumeOpsV2 volume_group = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
        IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_SYNC
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
        | IFS_FS_VOLUME_FORMAT
#endif
    },
    .query = guarded_volume_query,
    .sync = guarded_volume_sync
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
    , .format = guarded_format_ext2
#endif
};
/* V2 CORE directly owns the implemented production read/write and namespace
 * operations. Missing allocate/create/repair callbacks are deliberate
 * capability denials rather than successful placeholders. */
static const IfsFsCoreOpsV2 core_group = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
        IFS_FS_GROUP_CORE, 0u,
        IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
        IFS_FS_CORE_STAT | IFS_FS_CORE_LIST |
#if defined(IFS_EXT2_FIRMWARE_BUILD)
        IFS_FS_CORE_READ},
#else
        IFS_FS_CORE_READ | IFS_FS_CORE_WRITE},
#endif
    .probe = guarded_probe,
    .mount = guarded_mount,
    .unmount = guarded_unmount,
    .stat = guarded_stat_path,
    .directory_entry = guarded_directory_entry,
    .read = guarded_read_file,
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
    .write = guarded_write_file
#endif
};
/* Expose ownership and permissions from the same verified inode used
 * by STAT. No second parser and no metadata mutation are required.
 * GET_TIMES is not advertised: classic inodes lack a creation time. */
static int metadata_get(void *volume,const char *path,IfsFsMetadataV2 *out)
{
    if (!out || out->structure_bytes<sizeof(*out)) return IFS_FS_INVALID;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    CommonVolume *v=session_lookup(volume);
    if (!valid(v)) return v2_leave_operation(IFS_FS_INVALID);
    void *work=scratch(v);
    if (!work) return v2_leave_operation(IFS_FS_NO_MEMORY);
    IfsExt2Inode inode;
    const char *name=NULL;
    uint32_t length=0;
    int rc=resolve(v,path,&inode,&name,&length,work);
    if (rc==IFS_FS_OK) {
        *out=(IfsFsMetadataV2){.structure_bytes=sizeof(*out),
            .mask=IFS_FS_META_GET_MODE,.mode=inode.mode,
            .owner=inode.uid,.group=inode.gid};
    }
    unscratch(v,work);
    return v2_leave_operation(rc);
}
static const IfsFsMetadataOpsV2 metadata_group={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(metadata_group),
             IFS_FS_GROUP_METADATA,0u,IFS_FS_META_GET_MODE},
    .get=metadata_get
};

static const IfsFsGroupHeaderV2 *query_group(uint32_t group,
                                              uint32_t minimum,
                                              uint32_t maximum)
{
    if (minimum > IFS_FS_GROUP_ABI_V2 || maximum < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (group == IFS_FS_GROUP_CORE) return &core_group.header;
    if (group == IFS_FS_GROUP_VOLUME) return &volume_group.header;
    if (group == IFS_FS_GROUP_DEFRAG) return &defrag_group.header;
    if (group == IFS_FS_GROUP_METADATA) return &metadata_group.header;
    return NULL;
}
static const IfsFsModuleApiV2 common_v2 = {
    .abi_version = IFS_FS_COMMON_ABI_V2,
    .structure_bytes = sizeof(IfsFsModuleApiV2),
    .format_id = "ext2",
#if defined(IFS_EXT2_FIRMWARE_BUILD)
    .runtime_profiles = IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE,
#else
    .runtime_profiles = IFS_FS_PROFILE_HOSTED,
#endif
    .group_mask = IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG) |
                  IFS_FS_GROUP_BIT(IFS_FS_GROUP_METADATA),
    .query_group = query_group
};

__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void)
{
    return &common_v2;
}
