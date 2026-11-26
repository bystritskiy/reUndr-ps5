/* Host-side texture replacements: lossless UI art (NHD1, BGRA8) and BC
 * texture packs (NHD2). Guest texture geometry stays unchanged. */
#include "hd_textures.h"
#include "sha256.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>

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

/* HD patch (tools/hdpatch): <hd>patch/ beside the hd folder holds the pack
 * with every file XORed with SHAKE-256 keyed by the original texture it replaces. A file opens
 * the first time the game shows that texture, as the bytes in hand are the key; it is checked
 * against its SHA-256 and written into the hd folder. An hd folder from anything else (an older
 * pack, prepare-disc) is kept as hd.bak-<time>, never deleted. */
#define PATCH_DOMAIN "reUndr hdpatch 1"        /* with its NUL: hdpatch.py DOMAIN */
#define PATCH_FILE_MAX (64u<<20)
enum { PATCH_NONE=0, PATCH_DISC=1, PATCH_BASE=2 };
typedef struct { uint8_t wrap[32],sha[32]; uint32_t size,from; } PatchRecord;
_Static_assert(sizeof(PatchRecord)==72,"hdpatch index record ABI");
static PatchRecord *patch;
static uint32_t patch_count;
static unsigned char *patch_state;              /* per record: 0 not tried, 1 file in place, 2 cannot */
static char patch_dir[256];
static pthread_mutex_t patch_lock=PTHREAD_MUTEX_INITIALIZER;

#define ROL64(v,n) ((n) ? ((v)<<(n) | (v)>>(64-(n))) : (v))
static void keccak(uint64_t s[25])
{
    static const uint64_t rc[24]={
        0x0000000000000001ull,0x0000000000008082ull,0x800000000000808Aull,0x8000000080008000ull,
        0x000000000000808Bull,0x0000000080000001ull,0x8000000080008081ull,0x8000000000008009ull,
        0x000000000000008Aull,0x0000000000000088ull,0x0000000080008009ull,0x000000008000000Aull,
        0x000000008000808Bull,0x800000000000008Bull,0x8000000000008089ull,0x8000000000008003ull,
        0x8000000000008002ull,0x8000000000000080ull,0x000000000000800Aull,0x800000008000000Aull,
        0x8000000080008081ull,0x8000000000008080ull,0x0000000080000001ull,0x8000000080008008ull};
    static const unsigned rot[25]={0,1,62,28,27, 36,44,6,55,20, 3,10,43,25,39, 41,45,15,21,8, 18,2,61,56,14};
    for (int r=0;r<24;r++) {
        uint64_t c[5],b[25];
        for (int x=0;x<5;x++) c[x]=s[x]^s[x+5]^s[x+10]^s[x+15]^s[x+20];
        for (int x=0;x<5;x++) {
            uint64_t d=c[(x+4)%5]^ROL64(c[(x+1)%5],1);
            for (int y=0;y<25;y+=5) s[y+x]^=d;
        }
        for (int x=0;x<5;x++) for (int y=0;y<5;y++) b[y+5*((2*x+3*y)%5)]=ROL64(s[x+5*y],rot[x+5*y]);
        for (int y=0;y<25;y+=5) for (int x=0;x<5;x++) s[y+x]=b[y+x]^(~b[y+(x+1)%5]&b[y+(x+2)%5]);
        s[0]^=rc[r];
    }
}

/* data ^= SHAKE-256(key || name), as hdpatch.py stream() */
static void shake_xor(const uint8_t key[32],const char *name,uint8_t *data,size_t n)
{
    uint64_t s[25]={0};
    uint8_t in[136]={0};
    size_t len=strlen(name);
    if (len>136-33) return;
    memcpy(in,key,32); memcpy(in+32,name,len);
    in[32+len]^=0x1F; in[135]^=0x80;
    for (int i=0;i<17;i++) for (int k=0;k<8;k++) s[i]^=(uint64_t)in[8*i+k]<<(8*k);
    for (size_t at=0;at<n;at+=136) {
        keccak(s);
        for (size_t k=0;k<136 && at+k<n;k++) data[at+k]^=(uint8_t)(s[k/8]>>(8*(k%8)));
    }
}

static int write_whole(const char *path,const void *data,size_t size)
{
    char part[400];
    snprintf(part,sizeof part,"%s.part",path);
    FILE *f=fopen(part,"wb");
    if (!f) return 0;
    int ok=fwrite(data,1,size,f)==size;
    ok&=fclose(f)==0;
    if (ok && rename(part,path)) ok=0;
    if (!ok) remove(part);
    else chmod(path,0666);                           /* PS5Upload/FTP run as another user (main_ps5.c open_up) */
    return ok;
}

static void *read_whole(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    long n=fseek(f,0,SEEK_END) ? -1 : ftell(f);
    void *data=n>=0 && (size_t)n<=PATCH_FILE_MAX ? malloc(n ? (size_t)n : 1) : NULL;
    if (data && (fseek(f,0,SEEK_SET) || fread(data,1,(size_t)n,f)!=(size_t)n)) { free(data); data=NULL; }
    fclose(f);
    if (data) *size=(size_t)n;
    return data;
}

static int exists(const char *dir,const char *name)
{
    char path[400];
    struct stat st;
    snprintf(path,sizeof path,"%s/%s",dir,name);
    return !stat(path,&st);
}

static void patch_setup(const char *hd)
{
    char path[400],id[65],old[65]={0};
    size_t n=strlen(hd);
    /* "<hd>/patch" would sit inside the folder it moves; root[] must hold hd whole */
    if (!n || hd[n-1]=='/' || n>=sizeof root-48) return;
    snprintf(patch_dir,sizeof patch_dir,"%spatch",hd);
    snprintf(path,sizeof path,"%s/index.bin",patch_dir);
    size_t size=0;
    uint32_t *index=read_whole(path,&size);
    if (!index) return;                              /* no patch: the hd folder as it is */
    if (size<16 || memcmp(index,"NHDP",4) || index[1]!=1 || index[3]!=sizeof(PatchRecord)
        || !index[2] || size!=16+(size_t)index[2]*sizeof(PatchRecord) || !nfsu1_sha256_file(path,id)) {
        fprintf(stderr,"[HD] patch: %s is not an hdpatch index\n",path); free(index); return;
    }
    snprintf(path,sizeof path,"%s/hdpatch.id",hd);
    FILE *f=fopen(path,"rb");
    if (f) { if (fread(old,1,64,f)!=64) old[0]=0; fclose(f); }
    if (strcmp(old,id)) {
        size_t msize=0;
        char from[400];
        snprintf(from,sizeof from,"%s/manifest.bin",patch_dir);
        void *manifest=read_whole(from,&msize);
        if (!manifest) { fprintf(stderr,"[HD] patch: %s missing, hd folder left as it is\n",from); free(index); return; }
        /* only a pack is moved aside: a folder with its manifest or an earlier patch's id */
        if (exists(hd,"manifest.bin") || exists(hd,"hdpatch.id")) {
            char bak[400],stamp[32];
            time_t now=time(NULL);
            strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%S",localtime(&now));
            snprintf(bak,sizeof bak,"%s.bak-%s",hd,stamp);
            if (rename(hd,bak)) {
                fprintf(stderr,"[HD] patch: cannot move %s aside (errno %d)\n",hd,errno);
                free(manifest); free(index); return;
            }
            fprintf(stderr,"[HD] patch: kept the previous %s as %s\n",hd,bak);
        }
        if (mkdir(hd,0777) && errno!=EEXIST) manifest=(free(manifest),NULL);
        chmod(hd,0777);
        snprintf(path,sizeof path,"%s/manifest.bin",hd);
        int ok=manifest && write_whole(path,manifest,msize);
        free(manifest);
        snprintf(path,sizeof path,"%s/hdpatch.id",hd);
        if (!ok || !write_whole(path,id,64)) { fprintf(stderr,"[HD] patch: cannot set up %s\n",hd); free(index); return; }
        fprintf(stderr,"[HD] patch %.16s set up in %s\n",id,hd);
    }
    patch_count=index[2];
    patch_state=calloc(patch_count,1);
    if (!patch_state) { free(index); return; }
    patch=malloc(patch_count*sizeof *patch);
    if (patch) memcpy(patch,index+4,patch_count*sizeof *patch);
    free(index);
}

static int patch_open(uint32_t i,const void *source,const void *tail,uint32_t tail_bytes,uint32_t bytes)
{
    const PatchRecord *r=&patch[i];
    const char *name=entries[i].file;
    char path[400],from[400];
    snprintf(path,sizeof path,"%s/%s",root,name);
    FILE *f=fopen(path,"rb");
    if (f) { fclose(f); return 1; }                  /* opened on an earlier start, or by another record */
    if ((r->from!=PATCH_DISC && r->from!=PATCH_BASE) || r->size>PATCH_FILE_MAX) return 0;
    if (r->from==PATCH_BASE) snprintf(from,sizeof from,"%s/%s",NFS_HD_ROOT,name);
    else snprintf(from,sizeof from,"%s/files/%s",patch_dir,name);
    struct timespec t0,t1;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    size_t size=0;
    uint8_t *data=read_whole(from,&size),sum[32];
    int ok=0;
    if (!data || size!=r->size) goto out;
    Nfsu1Sha256 c;
    if (r->from==PATCH_DISC) {
        uint8_t key[32];
        nfsu1_sha256_init(&c);
        nfsu1_sha256_update(&c,PATCH_DOMAIN,sizeof PATCH_DOMAIN);
        nfsu1_sha256_update(&c,source,bytes-tail_bytes);
        if (tail_bytes) nfsu1_sha256_update(&c,tail,tail_bytes);
        nfsu1_sha256_final(&c,key);
        for (int k=0;k<32;k++) key[k]^=r->wrap[k];  /* a file shared by records: keyed by one of them */
        shake_xor(key,name,data,size);
    }
    nfsu1_sha256_init(&c); nfsu1_sha256_update(&c,data,size); nfsu1_sha256_final(&c,sum);
    if (memcmp(sum,r->sha,32)) { fprintf(stderr,"[HD] patch: %s does not open with this disc\n",name); goto out; }
    if (!(ok=write_whole(path,data,size))) { fprintf(stderr,"[HD] patch: cannot write %s\n",path); goto out; }
    /* the open runs on the renderer thread the first time a texture shows: its cost is logged */
    clock_gettime(CLOCK_MONOTONIC,&t1);
    fprintf(stderr,"[HD] patch: opened %s %u KB %.1f ms\n",name,(unsigned)(size>>10),
            (t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6);
out:
    free(data);
    return ok;
}

static int patch_ready(uint32_t i,const void *source,const void *tail,uint32_t tail_bytes,uint32_t bytes)
{
    unsigned char s=__atomic_load_n(&patch_state[i],__ATOMIC_ACQUIRE);
    if (s) return s==1;
    pthread_mutex_lock(&patch_lock);
    if (!(s=patch_state[i])) {
        int ok=patch_open(i,source,tail,tail_bytes,bytes);
        /* a record that cannot open its file waits for one that can */
        if (ok || patch[i].from!=PATCH_NONE) __atomic_store_n(&patch_state[i],s=ok ? 1 : 2,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&patch_lock);
    return s==1;
}

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
    if (outside && *outside) { patch_setup(outside); f=open_manifest(outside); }
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
    if (patch && (patch_count!=count || !outside || strcmp(root,outside))) {
        fprintf(stderr,"[HD] patch: index does not match the manifest, not used\n");
        free(patch); patch=NULL;
    }
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
        if (hash==((uint64_t)t->hash_hi<<32 | t->hash_lo))
            return !patch || patch_ready(i,source,tail,tail_bytes,bytes) ? t : NULL;
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
