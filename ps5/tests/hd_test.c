#include "hd_textures.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifndef NFS_HD_ROOT
#error Test root required
#endif
static void write_file(const char *name,const void *data,size_t size)
{
    char path[256];snprintf(path,sizeof path,NFS_HD_ROOT "/%s",name);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,size,f)==size);fclose(f);
}
static uint64_t fnv(const unsigned char *p,size_t n)
{
    uint64_t hash=UINT64_C(0xcbf29ce484222325);
    for (size_t i=0;i<n;i++) hash=(hash^p[i])*UINT64_C(0x100000001b3);
    return hash;
}
/* "patch": tools/test.py wrote an hdpatch (Python hashlib) beside NFSU1_HD_DIR; records 0x1111 and
 * 0x2222 share one encrypted NHD1 file, 0x3333 comes from the release folder, 0x4444 is damaged. */
static int patch_mode(void)
{
    unsigned char s[4][8];
    for (int r=0;r<4;r++) for (int k=0;k<8;k++) s[r][k]=(unsigned char)(10*r+k+1);
    NfsHdPixels info;
    const NfsHdTexture *t=nfsu1_hd_find(s[1],12,4,4,8,0x2222);     /* the record the file was not made with */
    assert(t);
    void *p=nfsu1_hd_load(t,&info);
    assert(p && info.color==0 && info.bytes==256);
    for (unsigned k=0;k<256;k++) assert(((unsigned char *)p)[k]==(unsigned char)(k*7));
    free(p);
    assert(nfsu1_hd_find(s[0],12,4,4,8,0x1111));
    assert((t=nfsu1_hd_find(s[2],12,4,4,8,0x3333)) && (p=nfsu1_hd_load(t,&info)) && info.bytes==256);
    free(p);
    assert(!nfsu1_hd_find(s[3],12,4,4,8,0x4444));
    unsigned char other[8]={0};
    assert(!nfsu1_hd_find(other,12,4,4,8,0x1111));
    puts("HD patch: opened by the original in hand, shared file, release copy, damaged file refused");
    return 0;
}
/* argv[1]: "invalid" (bad manifest), "preload" (NFSU1_HD_PRELOAD=1), "p8" (NFSU1_HD_P8=1), "patch", or none. */
int main(int argc,char **argv)
{
    if (argc>1 && !strcmp(argv[1],"patch")) return patch_mode();
    const unsigned char source[8]={1,2,3,4,5,6,7,8};
    const unsigned char source2[16]={9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9};
    /* P8 4x4: 16 indices, then a 2-entry palette (the record's key covers both). */
    const unsigned char p8[16+8]={0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1, 10,20,30,255, 40,50,60,255};
    uint64_t hash=fnv(source,8),hash2=fnv(source2,16),hash3=fnv(p8,24);
    NfsHdTexture entries[3]={
        {.color=12,.source_w=4,.source_h=4,.source_bytes=8,
         .sample_hash=0x1234,.width=8,.height=8,.hash_lo=(uint32_t)hash,
         .hash_hi=(uint32_t)(hash>>32),.file="0123456789abcdef0123456789abcdef.rgba"},
        {.color=15,.source_w=4,.source_h=4,.source_bytes=16,
         .sample_hash=0x5678,.width=8,.height=8,.hash_lo=(uint32_t)hash2,
         .hash_hi=(uint32_t)(hash2>>32),.file="fedcba9876543210fedcba9876543210.dxtc"},
        {.color=0x0B,.source_w=4,.source_h=4,.source_bytes=24,
         .sample_hash=0x9abc,.width=8,.height=8,.hash_lo=(uint32_t)hash3,
         .hash_hi=(uint32_t)(hash3>>32),.file="0123456789abcdef0123456789abcdef.rgba"}};
    unsigned char manifest[16+sizeof entries];
    uint32_t header[4]={0,1,3,sizeof entries[0]};memcpy(header,"NHDX",4);
    int invalid=argc>1 && !strcmp(argv[1],"invalid");
    int preload=argc>1 && !strcmp(argv[1],"preload");
    int with_p8=argc>1 && !strcmp(argv[1],"p8");
    if (invalid) strcpy(entries[0].file,"../outside.rgba");
    memcpy(manifest,header,16);memcpy(manifest+16,entries,sizeof entries);
    write_file("manifest.bin",manifest,sizeof manifest);
    /* NHD2: 8x8 BC1 with 2 mips = 32 + 8 bytes. */
    unsigned char bc[24+40];uint32_t bh[6]={0,8,8,40,0x0C,2};memcpy(bh,"NHD2",4);
    memcpy(bc,bh,24);for (unsigned i=0;i<40;i++) bc[24+i]=(unsigned char)i;
    write_file(entries[1].file,bc,sizeof bc);
    unsigned char image[16+256];uint32_t ih[4]={0,8,8,256};memcpy(ih,"NHD1",4);
    memcpy(image,ih,16);memset(image+16,0x5a,256);
    write_file(entries[0].file,image,sizeof image);
    if (preload) setenv("NFSU1_HD_PRELOAD","1",1);
    if (with_p8) {
        setenv("NFSU1_HD_P8","1",1);
        const NfsHdTexture *t=nfsu1_hd_find_p8(p8,4,4,p8+16,8,0x9abc);
        assert(t && t==nfsu1_hd_find_p8(p8,4,4,p8+16,8,0x9abc) && t->width==8);
        unsigned char other[8]={10,20,30,255, 40,50,61,255};  /* one palette byte differs */
        assert(!nfsu1_hd_find_p8(p8,4,4,other,8,0x9abc));
        assert(!nfsu1_hd_find_p8(p8,4,4,p8+16,4,0x9abc));    /* shorter palette */
        unsigned char idx[16];memcpy(idx,p8,16);idx[15]^=1;
        assert(!nfsu1_hd_find_p8(idx,4,4,p8+16,8,0x9abc));
        NfsHdPixels info;void *pixels=nfsu1_hd_load(t,&info);
        assert(pixels && info.color==0 && info.bytes==256);free(pixels);
        puts("HD P8: indices+palette key, palette/index change rejected");
        return 0;
    }
    assert(!nfsu1_hd_find_p8(p8,4,4,p8+16,8,0x9abc));          /* NFSU1_HD_P8 unset: off */
    const NfsHdTexture *found=nfsu1_hd_find(source,12,4,4,8,0x1234);
    if (invalid) { assert(!found);puts("HD invalid manifest rejected");return 0; }
    const NfsHdTexture *found2=nfsu1_hd_find(source2,15,4,4,16,0x5678);
    assert(found && found->width==8 && found2 && found2->width==8);
    NfsHdPixels info;
    if (preload) {
        for (int i=0;i<2000 && nfsu1_hd_preloaded()<1;i++) nanosleep(&(struct timespec){0,1000000},NULL);
        assert(nfsu1_hd_preloaded()==1);         /* the NHD2 file only */
        char path[256];snprintf(path,sizeof path,NFS_HD_ROOT "/%s",entries[1].file);remove(path);
        void *ram=nfsu1_hd_load(found2,&info);
        assert(ram && info.color==0x0C && info.levels==2 && info.bytes==40 && !memcmp(ram,bc+24,40));free(ram);
        puts("HD preload: NHD2 served from RAM after its file was removed");
        return 0;
    }
    assert(!nfsu1_hd_find(source,12,8,4,8,0x1234));
    assert(!nfsu1_hd_find(source,12,4,4,8,0x1235));
    unsigned char different[8]={0};assert(!nfsu1_hd_find(different,12,4,4,8,0x1234));
    void *pixels=nfsu1_hd_load(found,&info);
    assert(pixels && info.color==0 && info.levels==1 && info.bytes==256 && !memcmp(pixels,image+16,256));free(pixels);
    pixels=nfsu1_hd_load(found2,&info);
    assert(pixels && info.color==0x0C && info.levels==2 && info.bytes==40 && !memcmp(pixels,bc+24,40));free(pixels);
    write_file(entries[1].file,bc,sizeof bc-1);assert(!nfsu1_hd_load(found2,&info));
    bh[5]=5;memcpy(bc,bh,24);write_file(entries[1].file,bc,sizeof bc);assert(!nfsu1_hd_load(found2,&info));
    bh[5]=2;bh[4]=0x0D;memcpy(bc,bh,24);write_file(entries[1].file,bc,sizeof bc);assert(!nfsu1_hd_load(found2,&info));
    write_file(entries[0].file,image,sizeof image-1);assert(!nfsu1_hd_load(found,&info));
    image[4]=16;write_file(entries[0].file,image,sizeof image);assert(!nfsu1_hd_load(found,&info));
    memset(image,0,16);write_file(entries[0].file,image,sizeof image);assert(!nfsu1_hd_load(found,&info));
    puts("HD valid lookup, strong hash, dimensions, NHD1/NHD2, malformed/truncated file checks passed");
    return 0;
}
