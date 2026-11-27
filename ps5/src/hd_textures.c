/* Host-side texture replacements: lossless UI art (NHD1, BGRA8) and BC
 * texture packs (NHD2). Guest texture geometry stays unchanged. */
#include "hd_textures.h"
#include "sha256.h"
#include <dirent.h>
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
 * with every file XORed with SHAKE-256 keyed by the original texture it replaces. All files open
 * at the start from the game's texture files (nfsu1_hd_patch_prepare); one that did not opens the
 * first time the game shows that texture, as the bytes in hand are the key. Each is checked
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
static char patch_id[65];                       /* sha256 of index.bin: hd/hdpatch.id, hd/hdpatch.done */
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

static void *read_whole_max(const char *path,size_t *size,size_t max)
{
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    long n=fseek(f,0,SEEK_END) ? -1 : ftell(f);
    void *data=n>=0 && (size_t)n<=max ? malloc(n ? (size_t)n : 1) : NULL;
    if (data && (fseek(f,0,SEEK_SET) || fread(data,1,(size_t)n,f)!=(size_t)n)) { free(data); data=NULL; }
    fclose(f);
    if (data) *size=(size_t)n;
    return data;
}
static void *read_whole(const char *path,size_t *size) { return read_whole_max(path,size,PATCH_FILE_MAX); }

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
    snprintf(patch_id,sizeof patch_id,"%s",id);
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
    /* at the start, or (a fallback) on the renderer thread the first time a texture shows: its cost is logged */
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

static int preparing;                            /* nfsu1_hd_patch_prepare: preload waits for it */
static int walking;                              /* ... and reads the game files: a miss there is no texture on screen */
static void start_preload(void)
{
    static int started;
    const char *pre=getenv("NFSU1_HD_PRELOAD");
    if (started || !entries || !pre || strcmp(pre,"1")) return;
    started=1;
    if ((resident=calloc(count,sizeof *resident))!=NULL) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,preload,NULL)) { free(resident); resident=NULL; }
        else pthread_detach(thread);
    }
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
    if (!preparing) start_preload();
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
    if (!walking) dump(source,bytes,tail,tail_bytes,color,w,h,sample_hash);
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

/* ---- The whole patch at the start (nfsu1_hd_patch_prepare) ----
 * The texture packs (TPK) of the game files are read as tools/nfs_assets.py textures() and
 * tools/hdpack/extract.py p8_source() read them, and each texture goes through find() as the game
 * would show it: the keys are the same bytes, so the patch opens with nothing left for a race. */
static uint32_t sampled(const uint8_t *p,uint32_t n)  /* nv2a_vk.c bytes_hash(), the manifest's sample_hash */
{
    uint32_t v=2166136261u^n,w,step=(n/256>4 ? n/256 : 4)&~3u;
    for (uint32_t i=0;i+3<n;i+=step) { memcpy(&w,p+i,4); v=(v^w)*16777619u; }
    memcpy(&w,p+n-4,4);
    return (v^w)*16777619u;
}

static uint8_t *jdlz(const uint8_t *d,size_t n,uint32_t *size)  /* nfs_assets.py jdlz_decode() */
{
    uint32_t want,total,len=0,flags=1,matches=1;
    if (n<16 || memcmp(d,"JDLZ\x02\x10\0\0",8)) return NULL;
    memcpy(&want,d+8,4); memcpy(&total,d+12,4);
    if (total!=n || !want || want>PATCH_FILE_MAX) return NULL;
    uint8_t *o=malloc(want);
    size_t pos=16;
    if (!o) return NULL;
    while (len<want) {
        if (flags==1) { if (pos>=n) goto bad; flags=d[pos++]|256u; }
        if (matches==1) { if (pos>=n) goto bad; matches=d[pos++]|256u; }
        if (flags&1) {
            if (n-pos<2) goto bad;
            uint32_t a=d[pos],b=d[pos+1],length,distance;
            pos+=2;
            if (matches&1) { length=(b|((a&240)<<4))+3; distance=(a&15)+1; }
            else { length=(a&31)+3; distance=(b|((a&224)<<3))+17; }
            if (distance>len || length>want-len) goto bad;
            for (uint32_t k=0;k<length;k++,len++) o[len]=o[len-distance];
            matches>>=1;
        } else {
            if (pos>=n) goto bad;
            o[len++]=d[pos++];
        }
        flags>>=1;
    }
    *size=want;
    return o;
bad:
    free(o);
    return NULL;
}

typedef struct { size_t at,end; } Part;
/* the last 0x33310004 (records), 0x33310005 (formats), 0x33310003 (compressed), 0x33320001 (blob) */
static int tpk_parts(const uint8_t *d,size_t pos,size_t end,Part part[4],int depth)
{
    static const uint32_t ids[4]={0x33310004,0x33310005,0x33310003,0x33320001};
    while (pos<end) {
        uint32_t id,size;
        if (end-pos<8) return 0;
        memcpy(&id,d+pos,4); memcpy(&size,d+pos+4,4);
        if (size>end-pos-8) return 0;
        for (int k=0;k<4;k++) if (id==ids[k]) part[k]=(Part){pos+8,pos+8+size};
        if ((id&0x80000000u) && (depth>8 || !tpk_parts(d,pos+8,pos+8+size,part,depth+1))) return 0;
        pos+=8+(size_t)size;
    }
    return 1;
}

static void prepare_texture(const uint8_t *src,uint32_t n,const uint8_t *tail,uint32_t tail_bytes,
                            uint32_t color,uint32_t w,uint32_t h)
{
    for (uint32_t i=0;i<count;i++) {
        const NfsHdTexture *t=&entries[i];
        if (t->color!=color || t->source_w!=w || t->source_h!=h || t->source_bytes<4) continue;
        if (tail ? t->source_bytes!=n+tail_bytes : t->source_bytes>n) continue;
        find(src,tail,tail_bytes,color,w,h,t->source_bytes,sampled(src,tail ? n : t->source_bytes));
    }
}

static void prepare_tpk(const uint8_t *d,size_t n,size_t start,size_t stop)
{
    Part part[4]={{0,0},{0,0},{0,0},{0,0}};
    if (!tpk_parts(d,start+8,stop,part,0) || !part[0].end || !part[1].end || !part[3].end) return;
    size_t first=part[0].at,records=(part[0].end-first)/124;
    if (first+records*124!=part[0].end) return;
    for (size_t i=0;i<records;i++) {
        const uint8_t *r=d+first+i*124;
        uint32_t hash,format,offset,palette,size,palette_size,at,length=0,compressed=0;
        uint16_t w,h;
        if (part[1].at+i*60+16>n) return;
        memcpy(&hash,r+36,4); memcpy(&format,d+part[1].at+i*60+12,4);
        memcpy(&offset,r+48,4); memcpy(&palette,r+52,4); memcpy(&size,r+56,4); memcpy(&palette_size,r+60,4);
        memcpy(&w,r+68,2); memcpy(&h,r+70,2);
        if (!w || !h) continue;
        at=(uint32_t)(part[3].at+0x78+offset);
        if (part[2].end) {
            uint32_t key;
            if (part[2].at+i*20+12>n) return;
            memcpy(&key,d+part[2].at+i*20,4); memcpy(&at,d+part[2].at+i*20+4,4); memcpy(&length,d+part[2].at+i*20+8,4);
            if (key!=hash) continue;
            compressed=1;
        }
        uint8_t *payload=NULL;
        const uint8_t *p;
        uint32_t bytes;
        if (compressed) {
            if (at>n || length>n-at || !(payload=jdlz(d+at,length,&bytes))) continue;
            p=payload;
        } else {
            if (at>n || size>n-at) continue;
            p=d+at; bytes=size;
        }
        if (format==0x0B) {                          /* P8: the indices, then the palette in use */
            uint32_t texels=(uint32_t)w*h;
            const uint8_t *pal=NULL;
            if (palette_size==128 || palette_size==256 || palette_size==512 || palette_size==1024) {
                int64_t q=(int64_t)at+palette-offset;   /* the palette may sit before the indices (extract.py) */
                if (compressed) {
                    if (palette>=offset && palette-offset>=texels && palette-offset<=bytes && palette_size<=bytes-(palette-offset))
                        pal=payload+(palette-offset);
                } else if (q>=0 && (uint64_t)q+palette_size<=n) pal=d+q;
            }
            if (pal && texels>=4 && ((compressed && texels<=bytes) || (!compressed && texels<=n-at)))
                prepare_texture(p,texels,pal,palette_size,0x0B,w,h);
        } else prepare_texture(p,bytes,NULL,0,format,w,h);
        free(payload);
    }
}

static void prepare_file(const char *path)
{
    size_t n=0;
    uint8_t *d=read_whole_max(path,&n,1u<<30);    /* TRACKS/STREAML1RA.BUN is 130 MB */
    if (!d) return;
    for (size_t pos=0;n-pos>=8;) {
        uint32_t id,size;
        memcpy(&id,d+pos,4); memcpy(&size,d+pos+4,4);
        if (size>n-pos-8) break;                     /* not (or no longer) a chunk file */
        if (id==0xB3300000u) prepare_tpk(d,n,pos,pos+8+size);
        pos+=8+(size_t)size;
    }
    free(d);
}

/* files whose first 8 bytes can start a chunk; fills paths when given, adds their sizes */
static int prepare_walk(const char *dir,int depth,char (*paths)[400],int *count_paths,int max,uint64_t *bytes)
{
    DIR *d=opendir(dir);
    if (!d) return 0;
    for (struct dirent *e;(e=readdir(d));) {
        char path[400];
        struct stat st;
        if (e->d_name[0]=='.') continue;
        if (snprintf(path,sizeof path,"%s/%s",dir,e->d_name)>=(int)sizeof path || stat(path,&st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth<8) prepare_walk(path,depth+1,paths,count_paths,max,bytes); continue; }
        if (!S_ISREG(st.st_mode) || st.st_size<8) continue;
        uint32_t head[2];
        FILE *f=fopen(path,"rb");
        if (!f) continue;
        int chunk=fread(head,8,1,f)==1 && (uint64_t)head[1]+8<=(uint64_t)st.st_size;
        fclose(f);
        if (!chunk || *count_paths>=max) continue;
        if (paths) snprintf(paths[*count_paths],sizeof paths[0],"%s",path);
        (*count_paths)++;
        *bytes+=(uint64_t)st.st_size;
    }
    closedir(d);
    return 1;
}

void nfsu1_hd_patch_prepare(const char *game,void (*progress)(unsigned percent))
{
    preparing=1;
    if (!initialized) init();
    preparing=0;
    if (patch && game) {
        char path[400],done[65]={0};
        snprintf(path,sizeof path,"%s/hdpatch.done",root);
        FILE *f=fopen(path,"rb");
        if (f) { if (fread(done,1,64,f)!=64) done[0]=0; fclose(f); }
        if (strcmp(done,patch_id)) {
            enum { MAX_FILES=8192 };
            struct timespec t0,t1;
            clock_gettime(CLOCK_MONOTONIC,&t0);
            uint64_t total=0,sofar=0;
            int files=0,step=0;
            char (*paths)[400]=malloc(MAX_FILES*sizeof *paths);
            int walked=paths && prepare_walk(game,0,paths,&files,MAX_FILES,&total);
            walking=1;
            if (progress) progress(0);
            for (int i=0;i<files;i++) {
                struct stat st;
                prepare_file(paths[i]);
                if (!stat(paths[i],&st)) sofar+=(uint64_t)st.st_size;
                unsigned percent=total && sofar<total ? (unsigned)(sofar*100/total) : 100;
                if (progress && (int)percent!=step) progress((unsigned)(step=(int)percent));
            }
            free(paths);
            for (uint32_t i=0;i<count;i++)       /* release copies need no key: /app0/hd */
                if (patch[i].from==PATCH_BASE) patch_ready(i,NULL,NULL,0,0);
            uint32_t opened=0,need=0;
            for (uint32_t i=0;i<count;i++) {
                if (patch[i].from==PATCH_NONE) continue;
                need++;
                opened+=__atomic_load_n(&patch_state[i],__ATOMIC_ACQUIRE)==1;
            }
            clock_gettime(CLOCK_MONOTONIC,&t1);
            fprintf(stderr,"[HD] patch: %u of %u records opened at the start from %d game files, %.1f s\n",
                    opened,need,files,(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9);
            /* a record still closed (another disc revision) is tried again when the game shows it */
            walking=0;
            /* done only once the game files were read: a folder not there yet (USB) tries again next start */
            if (walked && files && opened && !write_whole(path,patch_id,64)) fprintf(stderr,"[HD] patch: cannot write %s\n",path);
        }
    }
    start_preload();
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
