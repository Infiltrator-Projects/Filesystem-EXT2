/* SPDX-License-Identifier: GPL-3.0-or-later
 * EXT2 has no standard journal. Native format reserves 98 blocks outside
 * the standard superblock's allocation domain: two mirrored headers and
 * up to 96 redo blocks. Foreign media retain standard read/write access
 * but relocation is refused without this UUID-bound recovery area.
 * Included only in the hosted Common V2 engine. No sidecar or OS parser.
 */
#ifndef IFS_EXT2_RELOC_JOURNAL_H
#define IFS_EXT2_RELOC_JOURNAL_H
enum { EJ_BS=4096u,EJ_MAX=96u,EJ_BLOCKS=98u,EJ_RECORD=40u,EJ_HASH=32u };
static const uint8_t ej_magic[8]={'I','F','S','2','R','E','D','O'};
static uint32_t ej_u16(const uint8_t *p)
{return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t ej_u32(const uint8_t *p)
{return ej_u16(p)|(ej_u16(p+2u)<<16);}
static void ej_p32(uint8_t *p,uint32_t n)
{p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static int ej_hash(const void *p,size_t n,uint8_t out[EJ_HASH])
{
    unsigned length=0u;
    return EVP_Digest(p,n,out,&length,EVP_sha256(),NULL)==1 && length==EJ_HASH ?
           IFS_FS_OK:IFS_FS_IO_ERROR;
}
static int jread(const IfsFsHostRuntimeV2 *h,uint32_t block,uint8_t *p)
{return ifs_fs_host_read_bytes_v2(h,(uint64_t)block*EJ_BS,p,EJ_BS);}
static int jwrite(const IfsFsHostRuntimeV2 *h,uint32_t block,const uint8_t *p)
{return ifs_fs_host_write_bytes_v2(h,(uint64_t)block*EJ_BS,p,EJ_BS);}
static int jflush(const IfsFsHostRuntimeV2 *h)
{return h->flush && h->flush(h->context)==0?IFS_FS_OK:IFS_FS_IO_ERROR;}
static int ej_geometry(const IfsFsHostRuntimeV2 *h,uint8_t sb[EJ_BS],
                       uint32_t *total)
{
    if(h->volume_bytes%EJ_BS || h->volume_bytes/EJ_BS<4096u ||
       h->volume_bytes/EJ_BS>128ull*32768u ||
       h->logical_sector_bytes>EJ_BS || h->physical_sector_bytes>EJ_BS)
        return IFS_FS_NOT_FOUND;
    int rc=jread(h,0u,sb);if(rc!=IFS_FS_OK)return rc;
    const uint8_t *b=sb+1024u;
    const uint32_t n=ej_u32(b+4u);
    if(ej_u16(b+56u)!=0xef53u || (uint64_t)n+EJ_BLOCKS!=h->volume_bytes/EJ_BS ||
       ej_u32(b+20u) || ej_u32(b+24u)!=2u || ej_u32(b+32u)!=32768u ||
       ej_u32(b+40u)!=1024u || ej_u32(b+84u)!=11u || ej_u16(b+88u)!=256u ||
       ej_u32(b+92u) || ej_u32(b+96u)!=2u || ej_u32(b+100u)!=2u)
        return IFS_FS_NOT_FOUND;
    *total=n;return IFS_FS_OK;
}
static int ej_header(uint8_t header[EJ_BS],uint32_t total,const uint8_t uuid[16],
                      uint32_t count)
{
    if(count>EJ_MAX)return IFS_FS_INVALID;
    memset(header,0,EJ_BS);
    memcpy(header,ej_magic,sizeof(ej_magic));
    ej_p32(header+8u,1u);ej_p32(header+12u,count);ej_p32(header+16u,total);
    memcpy(header+20u,uuid,16u);
    return IFS_FS_OK;
}
static int ej_seal(uint8_t header[EJ_BS])
{return ej_hash(header,EJ_BS-EJ_HASH,header+EJ_BS-EJ_HASH);}
static int ej_valid(const uint8_t header[EJ_BS],uint32_t total,const uint8_t uuid[16])
{
    uint8_t hash[EJ_HASH];
    return !memcmp(header,ej_magic,sizeof(ej_magic)) && ej_u32(header+8u)==1u &&
           ej_u32(header+12u)<=EJ_MAX && ej_u32(header+16u)==total &&
           !memcmp(header+20u,uuid,16u) &&
           ej_hash(header,EJ_BS-EJ_HASH,hash)==IFS_FS_OK &&
           !memcmp(hash,header+EJ_BS-EJ_HASH,EJ_HASH);
}
/* Committed copy wins over clean copy. Replay is idempotent, and the
 * payload remains intact until both headers are durably clean. */
static int ej_load(const IfsFsHostRuntimeV2 *h,uint8_t sb[EJ_BS],
                    uint8_t header[EJ_BS],uint32_t *total)
{
    int rc=ej_geometry(h,sb,total);if(rc!=IFS_FS_OK)return rc;
    uint8_t other[EJ_BS];
    rc=jread(h,*total,header);if(rc!=IFS_FS_OK)return rc;
    rc=jread(h,*total+1u,other);if(rc!=IFS_FS_OK)return rc;
    const uint8_t *uuid=sb+1024u+104u;
    const int a=ej_valid(header,*total,uuid),b=ej_valid(other,*total,uuid);
    if(!a && !b) {
        if(!memcmp(header,ej_magic,sizeof(ej_magic)) ||
           !memcmp(other,ej_magic,sizeof(ej_magic)))return IFS_FS_CORRUPT;
        return IFS_FS_NOT_FOUND;
    }
    if(a && b && ej_u32(header+12u) && ej_u32(other+12u) &&
       memcmp(header,other,EJ_BS))return IFS_FS_CORRUPT;
    if(!a || (b && !ej_u32(header+12u) && ej_u32(other+12u)))
        memcpy(header,other,EJ_BS);
    const uint32_t state=ej_u16(sb+1024u+58u);
    if(state!=1u && (state!=2u || !ej_u32(header+12u)))return IFS_FS_CORRUPT;
    return IFS_FS_OK;
}
static int ej_publish(const IfsFsHostRuntimeV2 *h,uint32_t total,
                      const uint8_t header[EJ_BS])
{
    int rc=jwrite(h,total,header);if(rc!=IFS_FS_OK)return rc;
    rc=jflush(h);if(rc!=IFS_FS_OK)return rc;
    rc=jwrite(h,total+1u,header);if(rc!=IFS_FS_OK)return rc;
    return jflush(h);
}
static int ej_clear(const IfsFsHostRuntimeV2 *h,uint32_t total,const uint8_t uuid[16])
{
    uint8_t header[EJ_BS];
    int rc=ej_header(header,total,uuid,0u);if(rc!=IFS_FS_OK)return rc;
    rc=ej_seal(header);if(rc!=IFS_FS_OK)return rc;
    return ej_publish(h,total,header);
}
/* Relocation may update primary group descriptors, block bitmaps, inodes,
 * indirect pointers and payload. No backup superblocks, inode-allocation
 * bitmaps or anything outside the declared ext2 volume are targets. */
static int ej_targets(const uint8_t header[EJ_BS],uint32_t total)
{
    const uint32_t count=ej_u32(header+12u);
    unsigned gdt=0u,bitmap=0u;
    if(!count || count>EJ_MAX)return IFS_FS_CORRUPT;
    for(uint32_t i=0u;i<count;++i){
        const uint32_t n=ej_u32(header+EJ_RECORD+i*36u),k=n%32768u;
        if(n>=total || !n || (n!=1u && (k<2u || k==3u)))
            return IFS_FS_CORRUPT;
        for(uint32_t j=0u;j<i;++j)
            if(n==ej_u32(header+EJ_RECORD+j*36u))return IFS_FS_CORRUPT;
        if(n==1u)gdt=1u;
        if(k==2u)bitmap=1u;
    }
    return gdt && bitmap?IFS_FS_OK:IFS_FS_CORRUPT;
}
static int ej_checkpoint(const IfsFsHostRuntimeV2 *h,uint32_t total,
                         uint8_t sb[EJ_BS],const uint8_t header[EJ_BS])
{
    int rc=ej_targets(header,total);if(rc!=IFS_FS_OK)return rc;
    const uint32_t count=ej_u32(header+12u);
    uint8_t *payload=h->allocate(h->context,(size_t)count*EJ_BS);
    if(!payload)return IFS_FS_NO_MEMORY;
    /* Validate ALL records before any metadata write. */
    for(uint32_t i=0u;i<count;++i) {
        uint8_t hash[EJ_HASH],*p=payload+(size_t)i*EJ_BS;
        rc=jread(h,total+2u+i,p);if(rc!=IFS_FS_OK)goto done;
        rc=ej_hash(p,EJ_BS,hash);if(rc!=IFS_FS_OK)goto done;
        if(memcmp(hash,header+EJ_RECORD+i*36u+4u,EJ_HASH)){
            rc=IFS_FS_CORRUPT;goto done;
        }
    }
    /* Standard ext2's ERROR state makes an incomplete checkpoint
     * visibly unclean to ordinary ext2 readers and firmware. */
    sb[1024u+58u]=2u;sb[1024u+59u]=0u;
    rc=jwrite(h,0u,sb);if(rc!=IFS_FS_OK)goto done;
    rc=jflush(h);if(rc!=IFS_FS_OK)goto done;
    for(uint32_t i=0u;i<count;++i) {
        rc=jwrite(h,ej_u32(header+EJ_RECORD+i*36u),payload+(size_t)i*EJ_BS);
        if(rc!=IFS_FS_OK)goto done;
    }
    rc=jflush(h);if(rc!=IFS_FS_OK)goto done;
    sb[1024u+58u]=1u;
    rc=jwrite(h,0u,sb);if(rc!=IFS_FS_OK)goto done;
    rc=jflush(h);if(rc!=IFS_FS_OK)goto done;
    rc=ej_clear(h,total,sb+1024u+104u);
done:
    h->release(h->context,payload);return rc;
}
static int ej_mount(const IfsFsHostRuntimeV2 *h,uint64_t *recovered)
{
    uint8_t sb[EJ_BS],header[EJ_BS];uint32_t total=0u;
    *recovered=0u;
    int rc=ej_load(h,sb,header,&total);
    if(rc==IFS_FS_NOT_FOUND)return IFS_FS_OK;
    if(rc!=IFS_FS_OK)return rc;
    if(!ej_u32(header+12u))return IFS_FS_OK;
    if((h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       !(h->capabilities&IFS_FS_HOST_EXCLUSIVE) ||
       !(h->capabilities&IFS_FS_HOST_DURABLE))return IFS_FS_ACCESS;
    rc=ej_checkpoint(h,total,sb,header);
    if(rc==IFS_FS_OK)*recovered=1u;
    return rc;
}
#endif
