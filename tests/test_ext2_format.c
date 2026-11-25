/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <infiltratr/filesystem_module_v2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <errno.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct {int fd;uint64_t bytes;uint32_t sector;unsigned writes,flushes,fail_flush,crash_flush;} Media;
static int read_at(void *ctx,uint64_t at,void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    return pread(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int write_at(void *ctx,uint64_t at,const void *p,uint32_t n)
{
    Media *m=ctx;
    if(at%m->sector || n%m->sector || n>m->sector || at>m->bytes || n>m->bytes-at)return -1;
    ++m->writes;
    return pwrite(m->fd,p,n,(off_t)at)==(ssize_t)n?0:-1;
}
static int flush(void *ctx)
{
    Media *m=ctx;
    if(++m->flushes==m->fail_flush)return -1;
    int rc=fsync(m->fd);
    if(!rc && m->crash_flush==m->flushes)_exit(86);
    return rc;
}
static void *allocate(void *ctx,size_t n){(void)ctx;return malloc(n);}
static void release(void *ctx,void *p){(void)ctx;free(p);}
static int entropy(void *ctx,void *p,uint32_t n)
{(void)ctx;return getrandom(p,n,0)==(ssize_t)n?0:-1;}
static int fail_entropy(void *ctx,void *p,uint32_t n)
{(void)ctx;(void)p;(void)n;return -1;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"EXT2 format line %d: %s\n",__LINE__,#x);goto done;}}while(0)
int main(int argc,char **argv)
{
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    if(!ifs_fs_api_v2_valid(api,"ext2"))return 1;
    const IfsFsCoreOpsV2 *core=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *vol=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    if(!core || !vol || !vol->format)return 1;
    char temp[]="/tmp/ifs-ext2-format.XXXXXX";
    if(argc!=1 && argc!=4)return 2;
    Media m={.bytes=64ull*1024u*1024u,.sector=512u};
    if(argc==4) {
        m.bytes=strtoull(argv[2],NULL,10)*1024u*1024u;
        m.sector=(uint32_t)strtoul(argv[3],NULL,10);
        m.fd=open(argv[1],O_CREAT|O_EXCL|O_RDWR|O_NOFOLLOW,0600);
    } else m.fd=mkstemp(temp);
    if(m.fd<0)return 2;
    if(argc==1)unlink(temp);
    int result=1;
    CHECK(ftruncate(m.fd,(off_t)m.bytes)==0);
    IfsFsHostRuntimeV2 h={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(h),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=m.bytes,.logical_sector_bytes=m.sector,
        .physical_sector_bytes=m.sector,.maximum_transfer_bytes=m.sector,
        .context=&m,.read_at=read_at,.write_at=write_at,.flush=flush,
        .allocate=allocate,.release=release,.random_bytes=entropy
    },ro=h;
    ro.capabilities=IFS_FS_HOST_READ_ONLY;ro.write_at=NULL;ro.flush=NULL;
    CHECK(vol->format(&ro,2u,"EXT2TEST",0u)==IFS_FS_ACCESS);
    CHECK(vol->format(&h,3u,"EXT2TEST",0u)==IFS_FS_UNSUPPORTED);
    CHECK(vol->format(&h,2u,"01234567890123456",0u)==IFS_FS_INVALID);
    CHECK(!m.writes && !m.flushes);
    /* Entropy failure is a preflight rejection, not partial format. */
    h.random_bytes=NULL;
    CHECK(vol->format(&h,2u,"EXT2TEST",0u)==IFS_FS_UNSUPPORTED);
    h.random_bytes=fail_entropy;
    CHECK(vol->format(&h,2u,"EXT2TEST",0u)==IFS_FS_IO_ERROR);
    CHECK(!m.writes && !m.flushes);
    h.random_bytes=entropy;
    if(argc==1) {
        m.fail_flush=2u;
        CHECK(vol->format(&h,2u,"EXT2TEST",0u)==IFS_FS_IO_ERROR);
        uint32_t rejected=0u;
        CHECK(core->probe(&ro,&rejected)!=IFS_FS_OK);
        m.fail_flush=0u;
    }
    CHECK(vol->format(&h,2u,"EXT2TEST",0u)==IFS_FS_OK);
    /* Read the actual superblock UUID and verify its version/variant.
     * A second explicit format of the disposable image must change it. */
    uint8_t uuid[16],second_uuid[16];
    CHECK(pread(m.fd,uuid,sizeof(uuid),1024u+104u)==(ssize_t)sizeof(uuid));
    CHECK((uuid[6]&0xf0u)==0x40u && (uuid[8]&0xc0u)==0x80u);
    if(argc==1) {
        CHECK(vol->format(&h,2u,"EXT2TEST",0u)==IFS_FS_OK);
        CHECK(pread(m.fd,second_uuid,sizeof(second_uuid),1024u+104u)==(ssize_t)sizeof(second_uuid));
        CHECK(memcmp(uuid,second_uuid,sizeof(uuid))!=0);
    }
    uint32_t v=0u;void *session=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    char name[256];
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,.name_capacity=sizeof(name)};
    CHECK(core->probe(&ro,&v)==IFS_FS_OK && v==2u);
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK && session);
    CHECK(info.variant==2u && info.block_bytes==4096u && info.total_bytes+98u*4096u==m.bytes);
    CHECK(info.free_bytes>0u && info.free_bytes<info.total_bytes);
    CHECK(core->stat(session,"/",&entry)==IFS_FS_OK && entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->directory_entry(session,"/",0u,&entry)==IFS_FS_END);
    CHECK(vol->format(&h,2u,"EXT2TEST",0u)==IFS_FS_BUSY);
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->mount(&h,&session,&info)==IFS_FS_OK && session);
    CHECK(core->create && core->write);
    CHECK(core->create(session,"/A.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(session,"/B.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(session,"/A.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_EXISTS);
    /* A one-shot flush error must roll back the directory and inode map. */
    m.fail_flush=m.flushes+1u;
    CHECK(core->create(session,"/ROLLBACK.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_IO_ERROR);
    m.fail_flush=0u;
    CHECK(core->stat(session,"/ROLLBACK.BIN",&entry)==IFS_FS_NOT_FOUND);
    uint8_t a[65536],bb[65536],copy[65536];
    memset(a,0x35,sizeof(a));memset(bb,0x78,sizeof(bb));
    /* 65 appends per file traverse both single and double indirection.
     * The other file's allocation separates successive appends. */
    for(unsigned i=0;i<65u;++i) {
        CHECK(core->write(session,"/A.BIN",(uint64_t)i*sizeof(a),a,sizeof(a))==IFS_FS_OK);
        CHECK(core->write(session,"/B.BIN",(uint64_t)i*sizeof(bb),bb,sizeof(bb))==IFS_FS_OK);
    }
    /* Exercise empty files and each boundary-sized native allocation. */
    const uint32_t sizes[]={0u,1u,511u,512u,513u,4095u,4096u,4097u};
    for(unsigned i=0u;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        char path[32];(void)snprintf(path,sizeof(path),"/EDGE%u.BIN",i);
        CHECK(core->create(session,path,IFS_FS_ENTRY_FILE)==IFS_FS_OK);
        if(sizes[i])CHECK(core->write(session,path,0u,a,sizes[i])==IFS_FS_OK);
    }
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK);
    CHECK(core->stat(session,"/A.BIN",&entry)==IFS_FS_OK && entry.size_bytes==65u*sizeof(a));
    for(unsigned i=0;i<65u;++i) {
        CHECK(core->read(session,"/A.BIN",(uint64_t)i*sizeof(a),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(copy,a,sizeof(a)));
        CHECK(core->read(session,"/B.BIN",(uint64_t)i*sizeof(bb),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(copy,bb,sizeof(bb)));
    }
    const IfsFsDefragOpsV2 *map=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    /* Walk actual physical runs through terminal END, proving no
     * adjacent extents were artificially split and no data was omitted.
     * Interleaved appends must produce a genuine physical discontinuity. */
    IfsFsExtentV2 ext[2];uint32_t written=0;
    CHECK(map && map->map);
    uint64_t logical=0u,previous_end=0u,runs=0u;
    unsigned gaps=0u;
    for(uint64_t page=0u;;){
        CHECK(page<1041u);
        int rc=map->map(session,"/A.BIN",page,ext,2u,&written);
        if(rc==IFS_FS_END){CHECK(!written);break;}
        CHECK(rc==IFS_FS_OK && written && written<=2u);
        for(uint32_t i=0u;i<written;++i){
            CHECK(ext[i].logical_offset==logical && ext[i].length &&
                  !ext[i].flags && !ext[i].reserved &&
                  ext[i].length<=65u*sizeof(a)-logical);
            if(runs){CHECK(ext[i].physical_offset!=previous_end);++gaps;}
            previous_end=ext[i].physical_offset+ext[i].length;
            logical+=ext[i].length;++runs;
        }
        page+=written;
    }
    CHECK(logical==65u*sizeof(a) && gaps>=64u && runs<1040u);
    for(unsigned i=0u;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        char path[32];(void)snprintf(path,sizeof(path),"/EDGE%u.BIN",i);
        uint64_t offset=0u;
        for(uint64_t page=0u;;++page) {
            int rc=map->map(session,path,page,ext,1u,&written);
            if(rc==IFS_FS_END){CHECK(!written && offset==sizes[i]);break;}
            CHECK(rc==IFS_FS_OK && written==1u && ext[0].logical_offset==offset);
            CHECK(ext[0].length && ext[0].length<=sizes[i]-offset);
            offset+=ext[0].length;
        }
        if(sizes[i]){
            CHECK(core->read(session,path,0u,copy,sizes[i])==IFS_FS_OK);
            CHECK(!memcmp(copy,a,sizes[i]));
        }
    }
    CHECK(map->map(session,"/",0u,ext,2u,&written)==IFS_FS_OK && written>0u &&
          ext[0].logical_offset==0u && ext[0].length==4096u);
    CHECK(map->header.capabilities==IFS_FS_DEFRAG_ALL);
    void *plan=NULL;
    CHECK(map->plan(session,"/",10u,&plan)==IFS_FS_OK && plan);
    uint64_t moved=0u;
    CHECK(map->execute(session,plan,&moved)==IFS_FS_ACCESS);
    map->release_plan(session,plan);plan=NULL;
    CHECK(core->unmount(session)==IFS_FS_OK);
    /* Crash after durable publication of both redo headers, before
     * checkpointing. The parent must observe a positive replay and every
     * original payload must survive. Never exercise an error-unwind path. */
    pid_t child=fork();
    CHECK(child>=0);
    if(child==0){
        void *cp=NULL,*cs=NULL;
        uint64_t cm=0u;
        if(core->mount(&h,&cs,&info)!=IFS_FS_OK ||
           map->plan(cs,"/",0u,&cp)!=IFS_FS_OK || !cp)_exit(87);
        m.crash_flush=m.flushes+3u;
        (void)map->execute(cs,cp,&cm);
        _exit(88);
    }
    int ws=0;
    CHECK(waitpid(child,&ws,0)==child && WIFEXITED(ws) && WEXITSTATUS(ws)==86);
    session=NULL;
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_ACCESS && !session);
    CHECK(core->mount(&h,&session,&info)==IFS_FS_OK && session);
    uint64_t replayed=0u;
    CHECK(map->recover(session,0u,&replayed)==IFS_FS_OK && replayed==1u);
    CHECK(map->recover(session,0u,&replayed)==IFS_FS_OK && !replayed);
    for(unsigned i=0u;i<65u;++i) {
        CHECK(core->read(session,"/A.BIN",(uint64_t)i*sizeof(a),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(copy,a,sizeof(a)));
        CHECK(core->read(session,"/B.BIN",(uint64_t)i*sizeof(bb),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(copy,bb,sizeof(bb)));
    }
    CHECK(map->plan(session,"/",10u,&plan)==IFS_FS_OK && plan);
    CHECK(core->create(session,"/BLOCKED.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_BUSY);
    CHECK(core->write(session,"/A.BIN",0u,a,1u)==IFS_FS_BUSY);
    CHECK(map->execute(session,plan,&moved)==IFS_FS_OK && moved>0u);
    CHECK(map->verify(session,plan)==IFS_FS_OK);
    map->release_plan(session,plan);
    uint64_t recovered=UINT64_MAX;
    CHECK(map->recover(session,0u,&recovered)==IFS_FS_OK && !recovered);
    CHECK(core->unmount(session)==IFS_FS_OK);
    CHECK(core->mount(&ro,&session,&info)==IFS_FS_OK);
    /* Independently validate the whole-volume allocation cursor after growth. */
    const IfsFsAllocationOpsV2 *allocation=(const void*)ifs_fs_group_v2(api,IFS_FS_GROUP_ALLOCATION);
    CHECK(allocation && allocation->map);
    uint64_t cursor=0u,unknown=0u,free_bytes=0u;
    for(unsigned page=0u;;++page){
        IfsFsAllocationRangeV2 ranges[64];uint32_t count=0u;uint64_t next=0u;
        CHECK(page<65536u);
        int status=allocation->map(session,cursor,ranges,64u,&count);
        CHECK(ifs_fs_allocation_page_v2_valid(h.volume_bytes,cursor,status,ranges,64u,count,&next,&unknown));
        for(uint32_t k=0u;k<count;++k)if(ranges[k].state==IFS_FS_SPACE_FREE)free_bytes+=ranges[k].length;
        cursor=next;
        if(status==IFS_FS_END)break;
    }
    CHECK(cursor==h.volume_bytes && !unknown && free_bytes==info.free_bytes);
    CHECK(map->map(session,"/A.BIN",0u,ext,2u,&written)==IFS_FS_OK &&
          written==1u && ext[0].length==65u*sizeof(a));
    for(unsigned i=0u;i<65u;++i) {
        CHECK(core->read(session,"/A.BIN",(uint64_t)i*sizeof(a),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(copy,a,sizeof(a)));
        CHECK(core->read(session,"/B.BIN",(uint64_t)i*sizeof(bb),copy,sizeof(copy))==IFS_FS_OK);
        CHECK(!memcmp(copy,bb,sizeof(bb)));
    }
    CHECK(core->unmount(session)==IFS_FS_OK);
    puts("EXT2 Common V2 native format, allocation, real map, journalled relocation, growth and payload verification PASS");
    result=0;
done:
    close(m.fd);
    if(result && argc==4)unlink(argv[1]);
    return result;
}
