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
    uint32_t allocation_hint;
    int mutation_fault;
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
/* Bounded native EXT2 allocation for the formatter's classic 4 KiB
 * profile. Stage full blocks before writing, including allocation maps,
 * inode records, directory records and indirect blocks. Ordinary I/O
 * failures restore the attempted blocks; power-loss atomicity is not
 * claimed for this non-journalled filesystem. */
enum { ALLOC_BS=4096, ALLOC_PATCHES=96 };
typedef struct { uint32_t number; uint8_t *before,*after; int dirty; } AllocPatch;
typedef struct {
    CommonVolume *v;
    AllocPatch patches[ALLOC_PATCHES];
    unsigned count;
    uint32_t blocks,inodes,hint;
} AllocTxn;
static uint32_t alloc_u16(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t alloc_u32(const uint8_t *p)
{return alloc_u16(p)|(alloc_u16(p+2)<<16);}
static void alloc_p16(uint8_t *p,uint32_t n)
{p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void alloc_p32(uint8_t *p,uint32_t n)
{alloc_p16(p,n);alloc_p16(p+2,n>>16);}
static int alloc_profile(CommonVolume *v)
{
    if(!valid(v))return IFS_FS_INVALID;
    if(!v->media.writable || (v->host.capabilities&IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    if(v->mutation_fault)return IFS_FS_IO_ERROR;
    const IfsExt2Volume *f=&v->media.fs;
    if(f->block_size!=ALLOC_BS || f->inode_size!=256u ||
       f->first_data_block || f->blocks_per_group!=32768u ||
       f->inodes_per_group!=1024u || f->group_count>128u ||
       f->descriptor_size!=32u || f->feature_compat ||
       f->feature_incompat!=2u || f->feature_ro_compat!=2u ||
       f->state!=1u || f->super.first_inode!=11u ||
       v->host.logical_sector_bytes>ALLOC_BS)return IFS_FS_UNSUPPORTED;
    return IFS_FS_OK;
}
static void alloc_release(AllocTxn *t)
{
    for(unsigned i=0;i<t->count;++i)
        t->v->host.release(t->v->host.context,t->patches[i].before);
}
static int alloc_patch(AllocTxn *t,uint32_t number,AllocPatch **out)
{
    if(number>=t->v->media.fs.total_blocks)return IFS_FS_CORRUPT;
    for(unsigned i=0;i<t->count;++i)
        if(t->patches[i].number==number){*out=&t->patches[i];return IFS_FS_OK;}
    if(t->count>=ALLOC_PATCHES)return IFS_FS_NO_MEMORY;
    const IfsFsHostRuntimeV2 *h=&t->v->host;
    uint8_t *p=h->allocate(h->context,2u*ALLOC_BS);
    if(!p)return IFS_FS_NO_MEMORY;
    int rc=ifs_fs_host_read_bytes_v2(h,(uint64_t)number*ALLOC_BS,p,ALLOC_BS);
    if(rc!=IFS_FS_OK){h->release(h->context,p);return rc;}
    memcpy(p+ALLOC_BS,p,ALLOC_BS);
    AllocPatch *a=&t->patches[t->count++];
    *a=(AllocPatch){.number=number,.before=p,.after=p+ALLOC_BS};
    *out=a;return IFS_FS_OK;
}
static int alloc_commit(AllocTxn *t)
{
    CommonVolume *v=t->v;
    const IfsFsHostRuntimeV2 *h=&v->host;
    unsigned attempted=0;int rc=IFS_FS_OK;
    if(t->blocks>v->media.fs.free_blocks ||
       t->inodes>v->media.fs.super.free_inodes_count)return IFS_FS_CORRUPT;
    for(unsigned i=0;i<t->count;++i) {
        AllocPatch *p=&t->patches[i];
        if(!p->dirty)continue;
        if(h->cancelled && h->cancelled(h->context)){rc=IFS_FS_BUSY;break;}
        attempted=i+1u;
        rc=ifs_fs_host_write_bytes_v2(h,(uint64_t)p->number*ALLOC_BS,p->after,ALLOC_BS);
        if(rc!=IFS_FS_OK)break;
    }
    if(rc==IFS_FS_OK && h->flush(h->context)!=0)rc=IFS_FS_IO_ERROR;
    if(rc!=IFS_FS_OK) {
        int bad=0;
        while(attempted) {
            AllocPatch *p=&t->patches[--attempted];
            if(p->dirty && ifs_fs_host_write_bytes_v2(h,(uint64_t)p->number*ALLOC_BS,
                                      p->before,ALLOC_BS)!=IFS_FS_OK)bad=1;
        }
        if(h->flush(h->context)!=0)bad=1;
        if(bad)v->mutation_fault=1;
        return rc;
    }
    v->media.fs.free_blocks-=t->blocks;
    v->media.fs.super.free_blocks_count-=t->blocks;
    v->media.fs.super.free_inodes_count-=t->inodes;
    v->allocation_hint=t->hint;
    return IFS_FS_OK;
}
/* Validate primary descriptors against the exact native formatter
 * layout, and require reserved metadata bits to remain allocated. */
static int alloc_group(AllocTxn *t,uint32_t group,AllocPatch **gdt,
                        uint8_t **d,uint32_t *span)
{
    const IfsExt2Volume *f=&t->v->media.fs;
    if(group>=f->group_count)return IFS_FS_CORRUPT;
    int rc=alloc_patch(t,1u,gdt);if(rc!=IFS_FS_OK)return rc;
    *d=(*gdt)->after+group*32u;
    const uint32_t base=group*32768u;
    uint64_t left=f->total_blocks-base;
    *span=left>32768u?32768u:(uint32_t)left;
    if(*span<=68u || alloc_u32(*d)!=base+2u ||
       alloc_u32(*d+4)!=base+3u || alloc_u32(*d+8)!=base+4u ||
       alloc_u16(*d+12)>*span-68u || alloc_u16(*d+14)>1024u)
        return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int alloc_new(AllocTxn *t,int inode,uint32_t *number)
{
    const IfsExt2Volume *f=&t->v->media.fs;
    const uint32_t groups=f->group_count;
    const uint32_t start=inode?0u:t->hint/32768u;
    for(uint32_t i=0;i<groups;++i) {
        const uint32_t g=(start+i)%groups;
        AllocPatch *gd,*map,*sb;uint8_t *d;uint32_t span;
        int rc=alloc_group(t,g,&gd,&d,&span);if(rc!=IFS_FS_OK)return rc;
        uint32_t count=alloc_u16(d+(inode?14u:12u));
        if(!count)continue;
        rc=alloc_patch(t,alloc_u32(d+(inode?4u:0u)),&map);
        if(rc!=IFS_FS_OK)return rc;
        uint32_t lo=inode?(g?0u:10u):68u,hi=inode?1024u:span;
        for(uint32_t k=0;k<lo;++k)
            if(!(map->after[k/8u]&(1u<<(k%8u))))return IFS_FS_CORRUPT;
        uint32_t first=!inode && g==start && t->hint%32768u>=lo &&
                       t->hint%32768u<hi?t->hint%32768u:lo;
        for(uint32_t j=0;j<hi-lo;++j) {
            const uint32_t k=lo+(first-lo+j)%(hi-lo);
            uint8_t mask=(uint8_t)(1u<<(k%8u));
            if(map->after[k/8u]&mask)continue;
            rc=alloc_patch(t,0,&sb);if(rc!=IFS_FS_OK)return rc;
            uint8_t *total=sb->after+1024u+(inode?16u:12u);
            if(!alloc_u32(total))return IFS_FS_CORRUPT;
            map->after[k/8u]|=mask;map->dirty=1;
            alloc_p16(d+(inode?14u:12u),count-1u);gd->dirty=1;
            alloc_p32(total,alloc_u32(total)-1u);sb->dirty=1;
            if(inode){*number=g*1024u+k+1u;++t->inodes;}
            else {
                *number=g*32768u+k;t->hint=*number+1u;++t->blocks;
                AllocPatch *p;rc=alloc_patch(t,*number,&p);
                if(rc!=IFS_FS_OK)return rc;
                memset(p->after,0,ALLOC_BS);p->dirty=1;
            }
            return IFS_FS_OK;
        }
        return IFS_FS_CORRUPT; /* descriptor free count disagrees */
    }
    return IFS_FS_NO_SPACE;
}
static int alloc_inode(AllocTxn *t,uint32_t n,AllocPatch **p,uint8_t **raw)
{
    if(!n || n>t->v->media.fs.total_inodes)return IFS_FS_CORRUPT;
    const uint32_t g=(n-1u)/1024u,k=(n-1u)%1024u;
    AllocPatch *gd;uint8_t *d;uint32_t span;
    int rc=alloc_group(t,g,&gd,&d,&span);if(rc!=IFS_FS_OK)return rc;
    rc=alloc_patch(t,alloc_u32(d+8)+k/16u,p);
    if(rc==IFS_FS_OK)*raw=(*p)->after+(k%16u)*256u;
    return rc;
}
static int alloc_checked(AllocTxn *t,uint32_t n,AllocPatch **out)
{
    const IfsExt2Volume *f=&t->v->media.fs;
    if(n>=f->total_blocks || n%32768u<68u)return IFS_FS_CORRUPT;
    AllocPatch *gd,*map;uint8_t *d;uint32_t span;
    int rc=alloc_group(t,n/32768u,&gd,&d,&span);if(rc!=IFS_FS_OK)return rc;
    rc=alloc_patch(t,alloc_u32(d),&map);if(rc!=IFS_FS_OK)return rc;
    uint32_t k=n%32768u;
    if(!(map->after[k/8u]&(1u<<(k%8u))))return IFS_FS_CORRUPT;
    return alloc_patch(t,n,out);
}
/* Allocate through the canonical direct/single/double/triple path.
 * i_blocks counts both data and indirect sectors, not logical length. */
static int alloc_data(AllocTxn *t,AllocPatch *ip,uint8_t *raw,
                     uint32_t logical,AllocPatch **out)
{
    IfsExt2BlockPath path;
    IfsExt2Status st=ifs_ext2_block_to_path(ALLOC_BS,logical,&path);
    if(st!=IFS_EXT2_OK || !path.depth || path.depth>4u)return IFS_FS_UNSUPPORTED;
    AllocPatch *owner=ip;
    uint8_t *ptr=raw+40u+4u*path.offsets[0];
    for(unsigned level=0;level<path.depth;++level) {
        uint32_t n=alloc_u32(ptr);
        if(!n) {
            int rc=alloc_new(t,0,&n);if(rc!=IFS_FS_OK)return rc;
            if(alloc_u32(raw+28)>UINT32_MAX-8u)return IFS_FS_NO_SPACE;
            alloc_p32(raw+28,alloc_u32(raw+28)+8u);ip->dirty=1;
            alloc_p32(ptr,n);owner->dirty=1;
        }
        AllocPatch *block;
        int rc=alloc_checked(t,n,&block);if(rc!=IFS_FS_OK)return rc;
        if(level+1u==path.depth){*out=block;return IFS_FS_OK;}
        owner=block;ptr=block->after+4u*path.offsets[level+1u];
    }
    return IFS_FS_CORRUPT;
}
static int alloc_create(void *volume,const char *path,uint32_t type)
{
    CommonVolume *v=session_lookup(volume);
    int rc=alloc_profile(v);if(rc!=IFS_FS_OK)return rc;
    if(type!=IFS_FS_ENTRY_FILE || !path || path[0]!='/' || !path[1] ||
       strchr(path+1,'/') || !strcmp(path,"/.") || !strcmp(path,"/.."))
        return IFS_FS_UNSUPPORTED;
    size_t len=strlen(path+1);if(len>255u)return IFS_FS_INVALID;
    void *work=scratch(v);if(!work)return IFS_FS_NO_MEMORY;
    IfsExt2Inode found,root;
    rc=resolve(v,path,&found,NULL,NULL,work);
    if(rc==IFS_FS_OK){rc=IFS_FS_EXISTS;goto done;}
    if(rc!=IFS_FS_NOT_FOUND)goto done;
    rc=status(engine()->root(&v->media,&root,work,ALLOC_BS));
    if(rc!=IFS_FS_OK)goto done;
    if(root.flags || !root.size || root.size%ALLOC_BS || root.size>UINT32_MAX) {
        rc=IFS_FS_UNSUPPORTED;goto done;
    }
    AllocTxn t={.v=v,.hint=v->allocation_hint};
    AllocPatch *rp,*fp,*dp=NULL;uint8_t *rr,*fr;uint32_t ino;
    rc=alloc_inode(&t,2u,&rp,&rr);if(rc!=IFS_FS_OK)goto release;
    rc=alloc_new(&t,1,&ino);if(rc!=IFS_FS_OK)goto release;
    rc=alloc_inode(&t,ino,&fp,&fr);if(rc!=IFS_FS_OK)goto release;
    memset(fr,0,256u);alloc_p16(fr,0100644u);alloc_p16(fr+26,1u);fp->dirty=1;
    uint32_t at=0,rec=0;
    /* A root-directory insertion reuses a validated record's slack.
     * If none fits, append one native directory block. */
    for(uint32_t logical=0;logical<root.size/ALLOC_BS && !rec;++logical) {
        uint64_t physical;int hole;
        rc=status(ifs_ext2_map_file_block(&v->media.fs,&root,logical,&physical,
                                        &hole,work,ALLOC_BS));
        if(rc!=IFS_FS_OK || hole || physical>UINT32_MAX) {
            if(rc==IFS_FS_OK)rc=IFS_FS_CORRUPT;
            goto release;
        }
        rc=alloc_checked(&t,(uint32_t)physical,&dp);if(rc!=IFS_FS_OK)goto release;
        for(uint32_t pos=0;pos<ALLOC_BS;) {
            uint8_t *d=dp->after+pos;
            uint32_t n=alloc_u32(d),r=alloc_u16(d+4),nl=d[6],occupied=0;
            if(ifs_ext2_validate_directory_record(pos,r,nl,n,ALLOC_BS,
                       v->media.fs.total_inodes)!=IFS_EXT2_DIRECTORY_RECORD_OK) {
                rc=IFS_FS_CORRUPT;goto release;
            }
            if(ifs_ext2_directory_record_can_insert(r,nl,n,(uint32_t)len,&occupied)) {
                if(n)alloc_p16(d+4,occupied);
                at=pos+occupied;rec=r-occupied;break;
            }
            pos+=r;
        }
    }
    if(!rec) {
        rc=alloc_data(&t,rp,rr,(uint32_t)(root.size/ALLOC_BS),&dp);
        if(rc!=IFS_FS_OK)goto release;
        alloc_p32(rr+4,(uint32_t)root.size+ALLOC_BS);rp->dirty=1;
        at=0;rec=ALLOC_BS;
    }
    uint8_t *d=dp->after+at;memset(d,0,rec);
    alloc_p32(d,ino);alloc_p16(d+4,rec);d[6]=(uint8_t)len;d[7]=1u;
    memcpy(d+8,path+1,len);dp->dirty=1;
    rc=alloc_commit(&t);
release:
    alloc_release(&t);
done:
    unscratch(v,work);return rc;
}
static int guarded_alloc_create(void *volume,const char *path,uint32_t type)
{
    if(!v2_enter_operation())return IFS_FS_BUSY;
    return v2_leave_operation(alloc_create(volume,path,type));
}
static int alloc_append(void *volume,const char *path,uint64_t pos,
                       const void *data,uint32_t length)
{
    CommonVolume *v=session_lookup(volume);
    int rc=alloc_profile(v);if(rc!=IFS_FS_OK)return rc;
    if(!data || !length || length>65536u)return IFS_FS_UNSUPPORTED;
    void *work=scratch(v);if(!work)return IFS_FS_NO_MEMORY;
    IfsExt2Inode inode;
    rc=resolve(v,path,&inode,NULL,NULL,work);if(rc!=IFS_FS_OK)goto done;
    if(ifs_ext2_inode_type(&inode)!=IFS_EXT2_NODE_REGULAR || inode.flags ||
       pos!=inode.size || pos>UINT32_MAX || length>UINT32_MAX-pos) {
        rc=IFS_FS_UNSUPPORTED;goto done;
    }
    AllocTxn t={.v=v,.hint=v->allocation_hint};
    AllocPatch *ip;uint8_t *raw;
    rc=alloc_inode(&t,inode.number,&ip,&raw);if(rc!=IFS_FS_OK)goto release;
    const uint8_t *src=data;uint32_t left=length;
    while(left) {
        uint32_t logical=(uint32_t)(pos/ALLOC_BS),skip=(uint32_t)(pos%ALLOC_BS);
        uint32_t n=ALLOC_BS-skip;if(n>left)n=left;
        AllocPatch *dp;
        rc=alloc_data(&t,ip,raw,logical,&dp);if(rc!=IFS_FS_OK)goto release;
        memcpy(dp->after+skip,src,n);dp->dirty=1;
        pos+=n;src+=n;left-=n;
    }
    alloc_p32(raw+4,(uint32_t)pos);ip->dirty=1;
    rc=alloc_commit(&t);
release:
    alloc_release(&t);
done:
    unscratch(v,work);return rc;
}

static int write_file(void *v, const char *path, uint64_t pos,
                      const void *b, uint32_t n)
{
    CommonVolume *mounted=session_lookup(v);
    if(valid(mounted) && mounted->mutation_fault)return IFS_FS_IO_ERROR;
    int rc=file_io(v,path,pos,(void *)b,n,1);
    return rc==IFS_FS_UNSUPPORTED?alloc_append(v,path,pos,b,n):rc;
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
    const IfsExt2NodeType type=ifs_ext2_inode_type(&inode);
    if (type != IFS_EXT2_NODE_REGULAR && type != IFS_EXT2_NODE_DIRECTORY) {
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
        if (hole) {
            if(type==IFS_EXT2_NODE_DIRECTORY){rc=IFS_FS_CORRUPT;goto done;}
            continue;
        }
        native_status = ifs_ext2_validate_allocated_data_block(
            &v->media.fs, physical, work, v->media.fs.block_size);
        if (native_status != IFS_EXT2_OK) {
            rc = status(native_status);
            goto done;
        }
        if (index++ < first_extent) continue;
        /* Return logical bytes, not allocation padding after EOF.
         * A one-byte file has a one-byte mapped extent, while the
         * provider's allocation accounting still owns the entire block. */
        const uint64_t unit=v->media.fs.block_size;
        const uint64_t off=logical*unit;
        const uint64_t length=inode.size-off<unit?inode.size-off:unit;
        if(physical>UINT64_MAX/unit){rc=IFS_FS_CORRUPT;goto done;}
        const uint64_t at=physical*unit;
        if(at>v->host.volume_bytes || length>v->host.volume_bytes-at){
            rc=IFS_FS_CORRUPT;goto done;
        }
        extents[count].logical_offset = off;
        extents[count].physical_offset = at;
        extents[count].length = length;
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

/* Volume accounting includes committed native allocation growth.
 * Remove, truncate and repair remain unadvertised. */
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
    if((variant!=0u && variant!=2u) || flags || h->logical_sector_bytes>FMT_BLOCK ||
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

    /* A new volume needs an independent identity; a missing entropy
     * callback must fail before the first destructive write, never
     * silently create another all-zero UUID. */
    uint8_t uuid[16]={0};
    const int entropy_status=ifs_fs_host_random_bytes_v2(h,uuid,sizeof(uuid));
    if(entropy_status!=IFS_FS_OK)return entropy_status;
    uuid[6]=(uint8_t)((uuid[6]&15u)|64u);
    uuid[8]=(uint8_t)((uuid[8]&63u)|128u);
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

/* The canonical mount parser retains the 16-byte superblock label.
 * Copy its bounded contents only when the entire name and terminator fit.
 * No on-disk label mutation is advertised. */
static int volume_label(void *mounted,char *out,uint32_t capacity,uint32_t *bytes)
{
    if (!bytes) return IFS_FS_INVALID;
    *bytes=0u;
    if (!out || !capacity) return IFS_FS_INVALID;
    if (!v2_enter_operation()) return IFS_FS_BUSY;
    CommonVolume *v=session_lookup(mounted);
    if (!valid(v)) return v2_leave_operation(IFS_FS_INVALID);
    uint32_t length=0u;
    while (length<16u && v->media.fs.label[length]) ++length;
    if (length>=capacity) return v2_leave_operation(IFS_FS_NO_SPACE);
    memcpy(out,v->media.fs.label,length);
    out[length]=0;
    *bytes=length;
    return v2_leave_operation(IFS_FS_OK);
}

static const IfsFsVolumeOpsV2 volume_group = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
        IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_SYNC | IFS_FS_VOLUME_LABEL_GET
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
        | IFS_FS_VOLUME_FORMAT
#endif
    },
    .query = guarded_volume_query,
    .label = volume_label,
    .sync = guarded_volume_sync
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
    , .format = guarded_format_ext2
#endif
};
/* Hosted CORE supports bounded file creation and append allocation;
 * the firmware descriptor retains its read-only contract. */
static const IfsFsCoreOpsV2 core_group = {
    .header = {IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
        IFS_FS_GROUP_CORE, 0u,
        IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
        IFS_FS_CORE_STAT | IFS_FS_CORE_LIST |
#if defined(IFS_EXT2_FIRMWARE_BUILD)
        IFS_FS_CORE_READ},
#else
        IFS_FS_CORE_READ | IFS_FS_CORE_WRITE | IFS_FS_CORE_CREATE},
#endif
    .probe = guarded_probe,
    .mount = guarded_mount,
    .unmount = guarded_unmount,
    .stat = guarded_stat_path,
    .directory_entry = guarded_directory_entry,
    .read = guarded_read_file,
#if !defined(IFS_EXT2_FIRMWARE_BUILD)
    .write = guarded_write_file,
    .create = guarded_alloc_create
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
