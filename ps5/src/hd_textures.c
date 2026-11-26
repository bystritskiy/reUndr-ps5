/* Host-side texture replacements: lossless UI art (NHD1, BGRA8) and BC
 * texture packs (NHD2). Guest texture geometry stays unchanged. */
#include "hd_textures.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

_Static_assert(sizeof(NfsHdTexture) == 84, "HD manifest record ABI");
#ifndef NFS_HD_ROOT
#define NFS_HD_ROOT "/app0/hd"
#endif
#define DUMP_MIN_TEXELS (128*128)
static NfsHdTexture *entries;
static uint32_t count;
static int initialized;
static char root[256];
/* NFSU1_HD_PRELOAD=1: a background thread reads every NHD2 file into RAM
 * once, so a texture first seen in a race costs a memcpy, not a disk read. */
typedef struct { void *pixels; NfsHdPixels info; } Resident;
static Resident *resident;
static uint32_t resident_count;
static void *read_pixels(const NfsHdTexture *t,NfsHdPixels *info);

static void *preload(void *unused)
{
    uint64_t bytes=0;
    (void)unused;
    for (uint32_t i=0;i<count;i++) {
        NfsHdPixels info;
        void *p;
        if (strcmp(entries[i].file+32,".dxtc") || !(p=read_pixels(&entries[i],&info))) continue;
        resident[i].info=info;
        __atomic_store_n(&resident[i].pixels,p,__ATOMIC_RELEASE);
        __atomic_add_fetch(&resident_count,1,__ATOMIC_RELAXED);
        bytes+=info.bytes;
    }
    fprintf(stderr,"[HD] Preloaded %u files, %llu MB\n",resident_count,(unsigned long long)(bytes>>20));
    return NULL;
}

uint32_t nfsu1_hd_preloaded(void) { return __atomic_load_n(&resident_count,__ATOMIC_RELAXED); }

static FILE *open_manifest(const char *dir)
{
    char path[320];
    snprintf(root,sizeof root,"%s",dir);
    snprintf(path,sizeof path,"%s/manifest.bin",root);
    return fopen(path,"rb");
}

static void init(void)
{
    uint32_t header[4];
    FILE *f=NULL;
    initialized = 1;
    const char *enabled = getenv("NFSU1_HD");
    if (enabled && !strcmp(enabled,"0")) return;
    /* NFSU1_HD_DIR holds a pack kept outside the app; it comes first. */
    const char *outside=getenv("NFSU1_HD_DIR");
    if (outside && *outside) f=open_manifest(outside);
    if (!f) f=open_manifest(NFS_HD_ROOT);
    if (!f) return;
    if (fread(header,sizeof header,1,f)!=1 || memcmp(header,"NHDX",4)
        || header[1]!=1 || !header[2] || header[2]>4096 || header[3]!=sizeof(NfsHdTexture)) {
        fclose(f); return;
    }
    NfsHdTexture *data = calloc(header[2],sizeof *data);
    if (!data) { fclose(f); return; }
    if (fread(data,sizeof *data,header[2],f)!=header[2] || fgetc(f)!=EOF) {
        free(data); fclose(f); return;
    }
    fclose(f);
    for (uint32_t i=0;i<header[2];i++) {
        NfsHdTexture *t=&data[i];
        if (!t->width || !t->height || t->width>4096 || t->height>4096
            || (uint64_t)t->width*t->height>8u*1024u*1024u
            || !memchr(t->file,0,sizeof t->file) || !t->file[0]
            || strlen(t->file)!=37 || (strcmp(t->file+32,".rgba") && strcmp(t->file+32,".dxtc"))
            || (i && t->sample_hash<data[i-1].sample_hash)) {
            free(data); return;
        }
        for (unsigned k=0;k<32;k++) {
            unsigned char c=(unsigned char)t->file[k];
            if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) { free(data); return; }
        }
    }
    entries=data; count=header[2];
    fprintf(stderr,"[HD] Loaded %u texture fingerprints\n",count);
    const char *pre=getenv("NFSU1_HD_PRELOAD");
    if (pre && !strcmp(pre,"1") && (resident=calloc(count,sizeof *resident))!=NULL) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,preload,NULL)) { free(resident); resident=NULL; }
        else pthread_detach(thread);
    }
}

/* NFSU1_HD_DUMP=<folder>: writes each large texture without a replacement once,
 * to find out what a screen is made of. */
static void dump(const void *source,uint32_t bytes,const void *tail,uint32_t tail_bytes,
                 uint32_t color,uint32_t w,uint32_t h,uint32_t sample_hash)
{
    static const char *folder;
    static int checked;
    if (!checked) { folder=getenv("NFSU1_HD_DUMP"); checked=1; }
    if (!folder || !*folder || w*h<DUMP_MIN_TEXELS) return;
    char path[320];
    snprintf(path,sizeof path,"%s/%08x_%ux%u_%02x.dxt",folder,sample_hash,w,h,color);
    FILE *f=fopen(path,"rb");
    if (f) { fclose(f); return; }
    f=fopen(path,"wb");
    if (!f) return;
    fwrite(source,1,bytes-tail_bytes,f);
    if (tail_bytes) fwrite(tail,1,tail_bytes,f);
    fclose(f);
}

/* The strong key is FNV-1a over source then tail (bytes counts both). */
static const NfsHdTexture *find(const void *source,const void *tail,uint32_t tail_bytes,uint32_t color,
                                uint32_t w,uint32_t h,uint32_t bytes,uint32_t sample_hash)
{
    if (!initialized) init();
    uint32_t lo=0,hi=count;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        if (entries[mid].sample_hash<sample_hash) lo=mid+1; else hi=mid;
    }
    uint64_t hash=0;
    int hashed=0;
    for (uint32_t i=lo;i<count && entries[i].sample_hash==sample_hash;i++) {
        const NfsHdTexture *t=&entries[i];
        if (t->color!=color || t->source_w!=w || t->source_h!=h || t->source_bytes!=bytes) continue;
        if (!hashed) {
            hash=UINT64_C(0xcbf29ce484222325);
            const unsigned char *p=source,*q=tail;
            for (uint32_t k=0;k<bytes-tail_bytes;k++) hash=(hash^p[k])*UINT64_C(0x100000001b3);
            for (uint32_t k=0;k<tail_bytes;k++) hash=(hash^q[k])*UINT64_C(0x100000001b3);
            hashed=1;
        }
        if (hash==((uint64_t)t->hash_hi<<32 | t->hash_lo)) return t;
    }
    dump(source,bytes,tail,tail_bytes,color,w,h,sample_hash);
    return NULL;
}

const NfsHdTexture *nfsu1_hd_find(const void *source,uint32_t color,
                                uint32_t w,uint32_t h,uint32_t bytes,uint32_t sample_hash)
{
    return find(source,NULL,0,color,w,h,bytes,sample_hash);
}

const NfsHdTexture *nfsu1_hd_find_p8(const void *indices,uint32_t w,uint32_t h,
                                     const void *palette,uint32_t palette_bytes,uint32_t sample_hash)
{
    static int enabled=-1;
    if (enabled<0) { const char *e=getenv("NFSU1_HD_P8"); enabled=e && !strcmp(e,"1"); }
    if (!enabled) return NULL;
    return find(indices,palette,palette_bytes,0x0B,w,h,w*h+palette_bytes,sample_hash);
}

static uint32_t levels_bytes(uint32_t color,uint32_t w,uint32_t h,uint32_t levels)
{
    uint32_t n=0;
    for (uint32_t l=0;l<levels;l++) {
        n+=((w+3)/4)*((h+3)/4)*(color==0x0C ? 8 : 16);
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    return n;
}

static void *read_pixels(const NfsHdTexture *t,NfsHdPixels *info)
{
    char path[320];
    uint32_t header[6],full=1;
    void *pixels=NULL;
    snprintf(path,sizeof path,"%s/%s",root,t->file);
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    while ((t->width>t->height ? t->width : t->height)>>full) full++;
    if (fread(header,16,1,f)!=1 || header[1]!=t->width || header[2]!=t->height) goto out;
    if (!memcmp(header,"NHD1",4)) {
        if (header[3]!=t->width*t->height*4) goto out;
        info->color=0; info->levels=1;
    } else if (!memcmp(header,"NHD2",4)) {
        if (fread(header+4,8,1,f)!=1 || (header[4]!=0x0C && header[4]!=0x0E && header[4]!=0x0F)
            || !header[5] || header[5]>full
            || header[3]!=levels_bytes(header[4],t->width,t->height,header[5])) goto out;
        info->color=header[4]; info->levels=header[5];
    } else goto out;
    info->bytes=header[3];
    pixels=malloc(info->bytes);
    if (pixels && (fread(pixels,1,info->bytes,f)!=info->bytes || fgetc(f)!=EOF)) {
        free(pixels); pixels=NULL;
    }
out:
    fclose(f);
    return pixels;
}

void *nfsu1_hd_load(const NfsHdTexture *t,NfsHdPixels *info)
{
    void *pixels=NULL;
    if (!t) return NULL;
    if (resident) {
        const Resident *r=&resident[t-entries];
        void *ram=__atomic_load_n(&r->pixels,__ATOMIC_ACQUIRE);
        if (ram && (pixels=malloc(r->info.bytes))!=NULL) {
            memcpy(pixels,ram,r->info.bytes);
            *info=r->info;
        }
    }
    if (!pixels) pixels=read_pixels(t,info);
    if (pixels)
        fprintf(stderr,"[HD] %ux%u -> %ux%u %s\n",t->source_w,t->source_h,t->width,t->height,t->file);
    return pixels;
}
