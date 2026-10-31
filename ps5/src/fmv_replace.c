/* Host-side FMV picture replacement. The guest keeps decoding its .mad, which
 * keeps timing, audio and skipping its own; a decoded frame recognised by its
 * texture hash is shown from the upscaled AV1 movie in a pack. Without a pack
 * the movie plays as on the disc. Built by tools/build-fmv.py. */
#include "fmv_replace.h"
#include "fmv_video.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#ifndef NFS_HD_ROOT
#define NFS_HD_ROOT "/app0/hd"
#endif
#define MAX_PACKS 64
#define CAPTURE_TEXELS 64
#define VERSION 2
#define MAX_JUMP 90

typedef struct {
    char magic[4];
    uint32_t version, source_w, source_h, width, height, frames, frame_bytes;
    uint64_t data_offset;
} NfsFmvHeader;
typedef struct { uint32_t hash, frame; } NfsFmvEntry;
typedef struct {
    int fd;
    char name[32];
    NfsFmvHeader header;
    NfsFmvEntry *entries;
} NfsFmvPack;
_Static_assert(sizeof(NfsFmvHeader)==40,"FMV pack header ABI");
_Static_assert(sizeof(NfsFmvEntry)==8,"FMV pack entry ABI");

static NfsFmvPack packs[MAX_PACKS];
static unsigned pack_count;
static int initialized;
static _Atomic(const NfsFmvPack *) movie;
static atomic_int reopened;
static const NfsFmvPack *playing;
static uint32_t shown;

static int valid(const NfsFmvHeader *h)
{
    return !memcmp(h->magic,"NFMV",4) && h->version==VERSION && h->source_w && h->source_h
        && h->frames && h->frames<=1u<<20
        && h->width && h->height && h->width<=4096 && h->height<=4096
        && !(h->width&1) && !(h->height&1)
        && h->frame_bytes==h->width*h->height*4
        && h->data_offset>=sizeof *h+h->frames*sizeof(NfsFmvEntry);
}

static void open_pack(const char *root,const char *name)
{
    char path[320];
    NfsFmvPack *p=&packs[pack_count];
    snprintf(path,sizeof path,"%s/fmv/%s",root,name);
    int fd=open(path,O_RDONLY);
    if (fd<0) return;
    if (read(fd,&p->header,sizeof p->header)!=(ssize_t)sizeof p->header || !valid(&p->header)) {
        close(fd); return;
    }
    const size_t bytes=p->header.frames*sizeof(NfsFmvEntry);
    p->entries=malloc(bytes);
    if (!p->entries || read(fd,p->entries,bytes)!=(ssize_t)bytes) {
        free(p->entries); p->entries=NULL; close(fd); return;
    }
    snprintf(p->name,sizeof p->name,"%.*s",(int)(strlen(name)-4),name);
    p->fd=fd;
    pack_count++;
    fprintf(stderr,"[FMV] %s: %u frames %ux%u -> %ux%u\n",name,p->header.frames,
            p->header.source_w,p->header.source_h,p->header.width,p->header.height);
}

static void scan(const char *root)
{
    char path[320];
    snprintf(path,sizeof path,"%s/fmv",root);
    DIR *dir=opendir(path);
    if (!dir) return;
    struct dirent *e;
    while ((e=readdir(dir))!=NULL && pack_count<MAX_PACKS) {
        const size_t n=strlen(e->d_name);
        if (n>4 && !strcmp(e->d_name+n-4,".fmv")) open_pack(root,e->d_name);
    }
    closedir(dir);
}

static void init(void)
{
    initialized=1;
    const char *enabled=getenv("NFSU1_FMV");
    if (enabled && !strcmp(enabled,"0")) return;
    /* NFSU1_HD_DIR holds packs kept outside the app; they come first. */
    const char *outside=getenv("NFSU1_HD_DIR");
    if (outside && *outside && strcmp(outside,NFS_HD_ROOT)) scan(outside);
    scan(NFS_HD_ROOT);
}

/* Called for each file the game opens. Dark frames of different movies can share a
 * hash, so a frame is only looked up in the pack named after the open movie. */
void nfsu1_fmv_movie(const char *xbox_path)
{
    const char *slash=strrchr(xbox_path,'\\');
    const char *name=slash ? slash+1 : xbox_path;
    const size_t n=strlen(name);
    if (n<=4 || strcasecmp(name+n-4,".mad")) return;
    if (!initialized) init();
    const NfsFmvPack *found=NULL;
    for (unsigned i=0;i<pack_count && !found;i++)
        if (strlen(packs[i].name)==n-4 && !strncasecmp(packs[i].name,name,n-4)) found=&packs[i];
    atomic_store(&movie,found);
    atomic_store(&reopened,1);
}

/* A picture that repeats has an entry for each of its frames, sorted, so the next
 * one after the frame on screen is the one playing. The movie only runs forward:
 * a texture uploaded again keeps the frame on screen. It opens on its first frames:
 * black that matches only the black at the far end stays the disc's picture, or
 * the decoder would race to the end and the movie stop there. */
static int pick(const NfsFmvPack *p,uint32_t first,uint32_t hash,uint32_t *frame,int *restart)
{
    *restart=atomic_load(&reopened) || p!=playing;
    const uint32_t from=*restart ? 0 : shown+1;
    for (uint32_t i=first;i<p->header.frames && p->entries[i].hash==hash;i++) {
        const uint32_t next=p->entries[i].frame;
        if (next>=from && next-from<MAX_JUMP) {
            atomic_store(&reopened,0);
            *frame=next;
            return 1;
        }
    }
    *frame=shown;
    return !*restart;
}

int nfsu1_fmv_find(uint32_t hash,uint32_t w,uint32_t h,NfsFmvFrame *out)
{
    const NfsFmvPack *p=atomic_load(&movie);
    if (!p || p->header.source_w!=w || p->header.source_h!=h) return 0;
    uint32_t lo=0,hi=p->header.frames;
    while (lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;
        if (p->entries[mid].hash<hash) lo=mid+1; else hi=mid;
    }
    if (lo==p->header.frames || p->entries[lo].hash!=hash) return 0;
    if (!pick(p,lo,hash,&out->frame,&out->restart)) return 0;
    out->pack=p;
    out->width=p->header.width;
    out->height=p->header.height;
    out->bytes=p->header.frame_bytes;
    return 1;
}

const void *nfsu1_fmv_load(const NfsFmvFrame *f)
{
    const NfsFmvPack *p=f->pack;
    const void *pixels=nfsu1_fmv_video_frame(p->fd,p->header.data_offset,f->width,f->height,
                                             f->frame,f->restart);
    if (pixels) { playing=p; shown=f->frame; }
    return pixels;
}

/* NFSU1_FMV_CAPTURE=<path>: lists the hash of every decoded frame, in order, with
 * a few of its texels so tools/fmv-align.py can tell which movie frame it was.
 * Frames themselves would cost too much to write. */
void nfsu1_fmv_capture(uint32_t hash,uint32_t w,uint32_t h,const void *pixels)
{
    static FILE *file;
    static int opened;
    if (!opened) {
        const char *path=getenv("NFSU1_FMV_CAPTURE");
        opened=1;
        if (path && *path) file=fopen(path,"w");
    }
    if (!file) return;
    const uint32_t *texels=pixels;
    fprintf(file,"%08X %u %u",hash,w,h);
    for (unsigned i=0;i<CAPTURE_TEXELS;i++) fprintf(file," %08X",texels[i*(w*h/CAPTURE_TEXELS)]);
    fputc('\n',file);
    fflush(file);
}
